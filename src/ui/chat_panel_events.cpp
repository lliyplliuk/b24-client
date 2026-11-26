#include "ui/chat_panel.h"
#include "ui/chat_widgets.h"
#include "ui/task_dialogs.h"
#include "ui/reaction_catalog.h"
#include "ui/workspace_directory.h"
#include "ui/workspace_presentation.h"
#include "ui/workspace_message_previews.h"
#include <QApplication>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTabBar>
#include <QTimer>
#include <QWindow>

namespace bx {
using ui::showEmpty;

void ChatPanel::resetAccount()
{
    m_chatSendController.reset();
    m_historySearch.reset();
    for (auto &directory : m_chatDirectories)
        directory = {};
    m_chatSearchTimer->stop();
    {
        const QSignalBlocker input(m_chatSearch);
        m_chatSearch->clear();
    }
    m_chats->clear();
    ui::setStatusLabel(m_chatSearchProgress, {});
    closeCurrentChat();
    m_messageSearchToggle->setChecked(false);
    m_chatDrafts.clear();
    m_typingNames.clear();
    m_typingNamesPending.clear();
    m_chatSource.clear();
    m_taskChatPressActive = false;
    m_taskChatRefreshDeferred = false;
    m_retainedTaskChat = {};
    m_retainedTaskChatRow = -1;
    m_taskChatBadgesRequested = false;
}

void ChatPanel::connectBackend()
{
    m_chatSearchTimer = new QTimer(this);
    m_chatSearchTimer->setSingleShot(true);
    m_chatSearchTimer->setInterval(300);
    connect(m_chatSearchTimer, &QTimer::timeout, this, &ChatPanel::runSearchChats);
    m_messageSearchTimer = new QTimer(this);
    m_messageSearchTimer->setSingleShot(true);
    m_messageSearchTimer->setInterval(300);
    connect(m_messageSearchTimer, &QTimer::timeout, this, &ChatPanel::runMessageSearch);
    m_visibleChatReadTimer = new QTimer(this);
    m_visibleChatReadTimer->setObjectName(QStringLiteral("visibleChatReadTimer"));
    m_visibleChatReadTimer->setSingleShot(true);
    m_visibleChatReadTimer->setInterval(160);
    connect(m_visibleChatReadTimer, &QTimer::timeout, this, &ChatPanel::reportVisibleChatRead);
    connect(qApp, &QGuiApplication::focusWindowChanged, this, [this](QWindow *) {
        scheduleVisibleChatRead();
    });
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *current) {
        if (current && (current->window() == window() || current->window() == m_workspace.card()))
            scheduleVisibleChatRead();
    });
    m_chatContactTimer = new QTimer(this);
    m_chatContactTimer->setSingleShot(true);
    connect(m_chatContactTimer, &QTimer::timeout, this, &ChatPanel::updateChatContactPresentation);
    connect(m_backend, &Backend::recentChatsLoaded, this,
            [this](const auto &chats) { onChatListLoaded(0, chats); });
    connect(m_backend, &Backend::taskChatsLoaded, this,
            [this](const auto &chats) { onChatListLoaded(1, chats); });
    connect(m_backend, &Backend::chatPinChanged, this, [this](const QString &id, bool pinned) {
        m_pendingPins.remove(operationKey(OperationKind::ChatPin, id));
        for (auto &directory : m_chatDirectories) {
            for (auto *chats : {&directory.recent, &directory.found})
                for (auto &chat : *chats)
                    if (chat.id == id)
                        chat.pinned = pinned;
        }
        const auto &directory = m_chatDirectories[m_chatScope];
        if (m_active)
            showChatResults(directory.query.isEmpty() ? directory.recent : directory.found, true, true);
    });
    connect(m_backend, &Backend::chatContactChanged, this,
            [this](const QString &id, const QDateTime &onlineUntil, const QString &position) {
        const auto apply = [&](Chat &chat) {
            if (chat.id == id && !chat.taskChat && chat.kind == QStringLiteral("im")) {
                chat.onlineUntil = onlineUntil;
                chat.position = position;
            }
        };
        for (auto &directory : m_chatDirectories) {
            for (auto *chats : {&directory.recent, &directory.found})
                for (auto &chat : *chats)
                    apply(chat);
            apply(directory.linked);
        }
        for (int row = 0; row < m_chats->count(); ++row) {
            auto *item = m_chats->item(row);
            if (item->data(workspace::Id).toString() == id &&
                item->data(workspace::AvatarType).toInt() == int(workspace::AvatarKind::Person)) {
                item->setData(workspace::ContactOnlineUntil, onlineUntil);
                item->setData(workspace::ContactPosition, position);
            }
        }
        if (m_currentChatId == id && m_chatContactIsPerson) {
            m_chatContactUntil = onlineUntil;
            m_chatContactPosition = position;
        }
        updateChatContactPresentation();
    });
    connect(m_backend, &Backend::pinnedMessagesLoaded, this,
            [this](const QString &dialog, const QVector<Message> &messages) {
        if (dialog.isEmpty())
            return;
        if (dialog == m_currentChatId)
            m_chatPinned->setMessages(messages);
    });
    connect(m_backend, &Backend::chatMessagesLoaded, this,
            [this](const QString &chatId, const QVector<Message> &messages) {
        if (m_active && chatId == m_currentChatId && !m_messageSearchActive &&
            m_contextDialogId != chatId) {
            static_cast<ui::HistoryList *>(m_chatMessages)->showHistory(chatId, m_chatEmpty, messages);
            emit avatarRowsChanged();
            emit avatarsRequested();
        }
    });
    connect(m_backend, &Backend::olderChatMessagesLoaded, this,
            [this](const QString &scope, const QString &before, bool task, const QVector<Message> &messages, bool more) {
        if (task || !m_active || scope != m_currentChatId || m_messageSearchActive)
            return;
        auto *history = static_cast<ui::HistoryList *>(m_chatMessages);
        if (history->addOlder(scope, before, messages, more)) {
            emit avatarRowsChanged();
            emit avatarsRequested();
        }
    });
    connect(m_backend, &Backend::chatReadConfirmed, this,
            [this](const QString &dialog, const QStringList &ids, qint64 readThrough) {
        if (dialog.isEmpty())
            return;
        if (m_currentChatId == dialog && !m_messageSearchActive && m_contextDialogId.isEmpty())
            static_cast<ui::HistoryList *>(m_chatMessages)->confirmRead(ids, readThrough);
    });
    connect(m_backend, &Backend::chatHistoryRemoved, this,
            [this](const QString &dialog, const QStringList &ids, qint64 through) {
        if (dialog.isEmpty())
            return;
        const bool cleared = ids.isEmpty() && through == 0;
        if (dialog == m_currentChatId) {
            static_cast<ui::HistoryList *>(m_chatMessages)->removeHistory(ids, through);
            if (cleared && !m_messageSearchActive) {
                resetMessageContext();
                showEmpty(m_chatMessages, m_chatEmpty, QStringLiteral("Сообщений пока нет"));
            }
        }
    });
    connect(m_backend, &Backend::chatBadgeAnchorAvailable, this, [this](const QString &dialog) {
        if (dialog.isEmpty())
            return;
        const auto *active = QApplication::activeWindow();
        auto *card = m_workspace.card();
        if ((active == window() && m_active && m_currentChatId == dialog)
            || (card && active == card && card->taskChatDialogId() == dialog))
            scheduleVisibleChatRead();
    });
    connect(m_backend, &Backend::personLoaded, this, [this](const Person &person) {
        rememberTypingName(person.id, person.name);
    });
    connect(m_backend, &Backend::messageSent, this, [this](const QString &scope, const QString &) {
        m_chatSendController.confirm(scope, m_chatComposer, m_currentChatId, m_chatDrafts);
    });
    connect(m_backend, &Backend::chatMessagesSeen, this, [this](const QString &dialog, const QStringList &ids) {
        const QSet<QString> seen(ids.cbegin(), ids.cend());
        const auto mark = [&seen](QListWidget *history) {
            for (int i = 0; i < history->count(); ++i) {
                auto *item = history->item(i);
                if (item->data(workspace::Outgoing).toBool() && !item->data(workspace::MessageSeen).toBool()
                    && seen.contains(item->data(workspace::ReceiptMessageId).toString())) {
                    item->setData(workspace::MessageSeen, true);
                    item->setData(Qt::AccessibleTextRole,
                                  item->data(Qt::AccessibleTextRole).toString() + QStringLiteral(". Просмотрено"));
                }
            }
        };
        if (auto *card = m_workspace.card(); card && card->taskChatDialogId() == dialog)
            mark(card->messageList());
        if (m_currentChatId == dialog)
            mark(m_chatMessages);
    });
    connect(m_backend, &Backend::chatTypingChanged, this, &ChatPanel::handleChatTyping);
    connect(m_backend, &Backend::chatActionSucceeded, this, [this](const QString &dialog, const QString &message) {
        if (message.isEmpty())
            return;
        if (auto *card = m_workspace.card(); card && card->taskChatDialogId() == dialog)
            card->setStatus(message);
        emit statusChanged(message);
    });
    connect(m_backend, &Backend::messageEditAccepted, this,
            [this](const QString &dialog, const QString &id, const QString &text) {
        if (m_chatComposer->matchesEdit(dialog, id))
            m_chatComposer->acceptMessageEdit(text);
        if (auto *card = m_workspace.card(); card && card->composer()->matchesEdit(dialog, id))
            card->composer()->acceptMessageEdit(text);
        if (m_currentChatId == dialog)
            static_cast<ui::HistoryList *>(m_chatMessages)->confirmMessageEdit(id, text);
        if (auto *card = m_workspace.card(); card && card->taskChatDialogId() == dialog)
            static_cast<ui::HistoryList *>(card->messageList())->confirmMessageEdit(id, text);
    });
    connect(m_backend, &Backend::chatOwnReactionChanged, this,
            [this](const QString &dialog, const QString &id, const QString &code, bool present) {
        if (present)
            recordConfirmedReaction(dialog, id, code);
        if (m_currentChatId == dialog)
            static_cast<ui::HistoryList *>(m_chatMessages)->confirmMessageReaction(id, code, present);
        if (auto *card = m_workspace.card(); card && card->taskChatDialogId() == dialog)
            static_cast<ui::HistoryList *>(card->messageList())->confirmMessageReaction(id, code, present);
    });
    connect(m_backend, &Backend::groupChatCreated, this, [this](const QString &) {
        if (m_active && m_chatScope == 0)
            m_backend->requestRecentChats();
        emit statusChanged(QStringLiteral("Групповой чат создан"));
    });
    connect(m_backend, &Backend::peopleFound, m_chatComposer, &ui::ChatComposer::setMentionResults);
    connect(m_backend, &Backend::messagesFound, this,
            [this](const QString &scope, const QString &query, bool task, const QVector<Message> &messages) {
        if (!task && m_historySearch.accept(scope, query))
            showSearchResponse(scope, query, messages);
    });
    connect(m_backend, &Backend::chatsFound, this,
            [this](const auto &query, const auto &chats) { onChatsFound(0, query, chats); });
    connect(m_backend, &Backend::taskChatsFound, this,
            [this](const auto &query, const auto &chats) { onChatsFound(1, query, chats); });
}

void ChatPanel::showMessageContext(const QString &dialogId, const QString &messageId, const QVector<Message> &messages)
{
    if (!m_active || m_currentChatId != dialogId ||
        m_contextDialogId != dialogId || m_contextMessageId != messageId)
        return;
    static_cast<ui::HistoryList *>(m_chatMessages)->showContext(dialogId, m_chatEmpty, messages);
    emit avatarRowsChanged();
    emit avatarsRequested();
    if (!focusChatMessage(messageId)) {
        resetMessageContext();
        m_backend->requestChatMessages(dialogId);
        emit statusChanged(QStringLiteral("Сообщение не найдено в истории чата"));
    }
}

void ChatPanel::showSearchResponse(const QString &scopeId, const QString &query, const QVector<Message> &messages)
{
    if (!m_active || scopeId != m_messageSearchScope || !m_messageSearchActive)
        return;
    if (query != m_messageSearchQuery)
        m_historySearch.request(scopeId, m_messageSearchQuery);
    else if (query == m_messageSearch->text().trimmed())
        showMessageResults(messages);
}

bool ChatPanel::handleFailure(const QString &operation, OperationKind kind, const QStringList &ids,
                              const QString &reason, bool networkFailure, QString &footer, bool &statusPresented)
{
    const bool activeChatHistory = m_active && !m_currentChatId.isEmpty() && !m_messageSearchActive
        && kind == OperationKind::Chat && ids.value(0) == m_currentChatId;
    if (networkFailure && ((kind == OperationKind::Chat && !activeChatHistory)
        || (kind == OperationKind::Chats && (!m_active || m_chatScope != 0))))
        return true;
    switch (kind) {
    case OperationKind::SearchPeople:
        m_chatComposer->setMentionSearchError(ids.value(0));
        break;
    case OperationKind::OlderChat:
        if (ids.value(0) != QLatin1String("task")) {
            static_cast<ui::HistoryList *>(m_chatMessages)->olderFailed(ids.value(1), ids.value(2), reason);
            return true;
        }
        break;
    case OperationKind::MessageEdit:
        if (m_chatComposer->matchesEdit(ids.value(0), ids.value(1))) {
            m_chatComposer->failMessageEdit();
            emit statusChanged(reason);
        }
        break;
    case OperationKind::MessageReaction:
        m_pendingReactionAdds.remove(operation);
        [[fallthrough]];
    case OperationKind::MessageAction:
    case OperationKind::ForwardMessage:
    case OperationKind::CreateTaskFromMessage:
    case OperationKind::ChatMember: {
        const auto dialog = ids.value(0);
        if (auto *card = m_workspace.card(); card && card->taskChatDialogId() == dialog)
            card->setStatus(reason);
        emit statusChanged(reason);
        return true;
    }
    case OperationKind::Person:
        rememberTypingName(ids.value(0), {});
        break;
    case OperationKind::ChatPin:
        if (m_pendingPins.remove(operation))
            emit statusChanged(reason);
        return true;
    case OperationKind::SearchChat: {
        const auto query = m_historySearch.takeFailed(ids.value(0));
        if (!query.isEmpty())
            chatSearchFailed(ids.value(0), query, reason);
        return true;
    }
    case OperationKind::SearchChats:
    case OperationKind::SearchTaskChats: {
        const auto expectedKind = m_chatScope == 0 ? OperationKind::SearchChats : OperationKind::SearchTaskChats;
        if (m_active && !m_chatDirectories[m_chatScope].query.isEmpty() &&
            kind == expectedKind && ids.value(0) == m_chatSearch->text().trimmed()) {
            ui::setStatusLabel(m_chatSearchProgress, {});
            showChatResults({});
            emit statusChanged(reason);
        }
        return true;
    }
    case OperationKind::SendChat:
        statusPresented = m_chatSendController.fail(ids.value(0), reason, networkFailure);
        break;
    case OperationKind::Chat: {
        if (!activeChatHistory)
            break;
        const auto recovery = QStringLiteral("Не удалось загрузить сообщения. Нажмите «Обновить» или F5");
        if (m_chatMessages->count() == 0)
            showEmpty(m_chatMessages, m_chatEmpty, recovery);
        if (networkFailure)
            footer = recovery;
        break;
    }
    case OperationKind::TaskChats:
        if (!m_active || m_chatScope != 1)
            return true;
        [[fallthrough]];
    case OperationKind::Chats:
        if (!m_active || (kind == OperationKind::Chats && m_chatScope != 0)
            || m_chatDirectories[m_chatScope].loaded)
            break;
        m_chats->clear();
        workspace::emptyRow(m_chats, QStringLiteral("Не удалось загрузить чаты"));
        if (m_currentChatId.isEmpty())
            showEmpty(m_chatMessages, m_chatEmpty, QStringLiteral("Выберите чат"));
        break;
    default:
        break;
    }
    return false;
}

bool ChatPanel::failMessageContext(const QStringList &ids, const QString &reason)
{
    if (ids.value(0) == m_contextDialogId && ids.value(1) == m_contextMessageId) {
        const auto dialogId = m_contextDialogId;
        resetMessageContext();
        m_backend->requestChatMessages(dialogId);
        emit statusChanged(reason);
        return true;
    }
    return false;
}

void ChatPanel::chatSearchFailed(const QString &scope, const QString &query, const QString &reason)
{
    if (!m_active || !m_messageSearchActive || scope != m_messageSearchScope)
        return;
    if (query != m_messageSearchQuery)
        m_historySearch.request(m_messageSearchScope, m_messageSearchQuery);
    else if (query == m_messageSearch->text().trimmed())
        showEmpty(m_chatMessages, m_chatEmpty, reason);
}

void ChatPanel::refreshMentionAvatars()
{
    m_chatComposer->refreshMentionAvatars();
}

void ChatPanel::clearPendingReactions()
{
    m_pendingReactionAdds.clear();
}

} // namespace bx
