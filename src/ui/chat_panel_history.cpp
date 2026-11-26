#include "ui/chat_panel.h"
#include "ui/chat_widgets.h"
#include "ui/workspace_presentation.h"
#include "ui/workspace_message_previews.h"
#include "ui/workspace_message_viewers.h"
#include "ui/task_dialogs.h"
#include <QApplication>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QTabBar>
#include <QTimer>
#include <QSignalBlocker>

namespace bx {

using ui::showEmpty;

void ChatPanel::scheduleVisibleChatRead()
{
    if (m_visibleChatReadTimer)
        m_visibleChatReadTimer->start();
}

void ChatPanel::reportVisibleChatRead()
{
    if (!writable())
        return;
    const auto *active = QApplication::activeWindow();
    const auto *modal = QApplication::activeModalWidget();
    if (!active || active->isMinimized() || QApplication::activePopupWidget() || (modal && modal != active))
        return;
    if (m_workspace.readActiveTask(active))
        return;
    if (active != window() || !m_active || m_currentChatId.isEmpty() ||
        m_messageSearchActive || !m_contextDialogId.isEmpty())
        return;
    const auto seen = static_cast<ui::HistoryList *>(m_chatMessages)->visibleMessageIds();
    if (!seen.all.isEmpty())
        m_backend->readChatMessages(m_currentChatId, seen.incoming, seen.all);
}

void ChatPanel::updateChatContactPresentation()
{
    const auto now = QDateTime::currentDateTimeUtc();
    const bool known = m_chatContactIsPerson && m_chatContactUntil.isValid();
    const bool online = known && m_chatContactUntil > now;
    QStringList details;
    if (known)
        details.append(online ? QStringLiteral("В сети") : QStringLiteral("Не в сети"));
    if (m_chatContactIsPerson && !m_chatContactPosition.trimmed().isEmpty())
        details.append(m_chatContactPosition.simplified());
    const auto text = details.join(QStringLiteral(" · "));
    ui::setStatusLabel(m_chatContactInfo, text);
    m_chatContactInfo->setToolTip(ui::plainTooltip(text));
    m_chatAvatar->setOnline(online);
    m_chats->viewport()->update();
    m_chatContactTimer->stop();
    QDateTime next = online ? m_chatContactUntil : QDateTime();
    for (int i = 0; i < m_chats->count(); ++i) {
        const auto expiry = m_chats->item(i)->data(workspace::ContactOnlineUntil).toDateTime();
        if (expiry.isValid() && expiry > now && (!next.isValid() || expiry < next))
            next = expiry;
    }
    if (next.isValid())
        m_chatContactTimer->start(int(qBound(qint64(1), now.msecsTo(next) + 10, qint64(900000))));
}

void ChatPanel::openChatRow(const QListWidgetItem &item)
{
    const auto chatId = item.data(workspace::Id).toString();
    if (chatId.isEmpty())
        return;
    const auto name = item.data(workspace::Title).toString();
    m_chatTitle->setText(name);
    m_chatTitle->setToolTip(ui::plainTooltip(name));
    const auto avatar = workspace::AvatarKind(item.data(workspace::AvatarType).toInt());
    m_chatAvatar->setTitle(name, avatar);
    m_chatContactIsPerson = m_chatScope == 0 && avatar == workspace::AvatarKind::Person;
    m_chatContactUntil = item.data(workspace::ContactOnlineUntil).toDateTime();
    m_chatContactPosition = item.data(workspace::ContactPosition).toString();
    updateChatContactPresentation();
    m_chatAvatarUrl = item.data(workspace::AvatarUrl).toUrl();
    if (const auto *photo = m_workspace.avatar(m_chatAvatarUrl))
        m_chatAvatar->setPhoto(*photo);
    const auto taskId = item.data(workspace::TaskId).toString();
    const bool taskChanged = m_openChatTaskId != taskId;
    m_openChatTaskId = taskId;
    m_openChatTask->setVisible(!m_openChatTaskId.isEmpty());
    m_chatDirectories[m_chatScope].selected = chatId;
    emit avatarsRequested();
    if (chatId == m_currentChatId) {
        if (taskChanged)
            m_workspace.previews->scheduleLinkPreviewLayout();
        return;
    }
    emit dialogChanged();
    m_workspace.previews->resetLinkPreviews();
    m_chatComposer->dismissEmoji();
    switchDraft(m_chatComposer, m_chatDrafts, m_currentChatId, chatId);
    m_messageSearchTimer->stop();
    {
        const QSignalBlocker blocker(m_messageSearch);
        m_messageSearch->clear();
    }
    m_messageSearchActive = false;
    m_messageSearchQuery.clear();
    m_messageSearchScope.clear();
    m_currentChatId = chatId;
    static_cast<ui::HistoryList *>(m_chatMessages)->resetHistory(chatId);
    m_chatPinned->setMessages({});
    if (m_contextDialogId != chatId)
        resetMessageContext();
    m_chatComposer->show();
    showEmpty(m_chatMessages, m_chatEmpty, QStringLiteral("Загружаем сообщения…"), true);
    emit statusChanged(QString());
    updateComposers();
    if (m_contextDialogId != chatId)
        m_backend->requestChatMessages(chatId);
    m_backend->requestPinnedMessages(chatId);
}

void ChatPanel::handleChatTyping(const QString &dialog, const QString &user, int duration)
{
    auto *card = m_workspace.card();
    auto *task = card && card->isVisible() && card->taskChatDialogId() == dialog ? card->composer() : nullptr;
    auto *chat = m_active && m_currentChatId == dialog && m_chatComposer->isVisible()
        ? m_chatComposer : nullptr;
    if (!task && !chat)
        return;

    const auto id = portalTaskIdentifier(user);
    const auto &writer = id.isEmpty() ? user : id;
    QString name;
    const QString *cached = nullptr;
    if (duration > 0 && !id.isEmpty()) {
        const auto &recent = m_chatDirectories[0].recent;
        const auto contact = std::find_if(recent.cbegin(), recent.cend(), [&id](const Chat &entry) {
            return entry.id == id && !entry.taskChat && !entry.title.trimmed().isEmpty();
        });
        if (contact != recent.cend())
            name = contact->title.simplified().left(96);
        if (name.isEmpty()) {
            cached = m_typingNames.object(id);
            if (cached)
                name = *cached;
        }
    }
    for (auto *composer : {task, chat})
        if (composer)
            composer->setTypingIndicator(writer, duration, name);

    if (duration > 0 && !id.isEmpty() && name.isEmpty() && !cached && !window()->isMinimized()
        && m_typingNamesPending.size() < 8 && !m_typingNamesPending.contains(id)) {
        m_typingNamesPending.insert(id);
        m_backend->requestPerson(id);
    }
}

void ChatPanel::rememberTypingName(const QString &user, const QString &name)
{
    if (!m_typingNamesPending.remove(user))
        return;
    const auto display = name.simplified().left(96);
    m_typingNames.insert(user, new QString(display));
    if (auto *card = m_workspace.card(); card && card->isVisible())
        card->composer()->setTypingName(user, display);
    if (m_active && m_chatComposer->isVisible())
        m_chatComposer->setTypingName(user, display);
}

void ChatPanel::closeCurrentChat(bool scopeChanged)
{
    m_chatComposer->dismissEmoji();
    switchDraft(m_chatComposer, m_chatDrafts, m_currentChatId, {});
    if (scopeChanged || !m_currentChatId.isEmpty())
        emit historyClosed(false);
    m_currentChatId.clear();
    static_cast<ui::HistoryList *>(m_chatMessages)->resetHistory();
    resetMessageContext();
    clearMessageSearch();
    m_chatTitle->setText(m_chatTabs->tabText(m_chatScope));
    m_chatTitle->setToolTip({});
    m_chatAvatar->setTitle({});
    m_chatContactIsPerson = false;
    updateChatContactPresentation();
    m_chatPinned->setMessages({});
    m_chatAvatarUrl.clear();
    m_openChatTaskId.clear();
    m_openChatTask->hide();
    m_chatComposer->hide();
    showEmpty(m_chatMessages, m_chatEmpty, QStringLiteral("Выберите чат"));
    updateComposers();
}

void ChatPanel::openLinkedChat(const NotificationTarget &target)
{
    int scope = target.taskChat ? 1 : 0;
    Chat linkedChat;
    for (int candidate = 0; candidate < 2; ++candidate) {
        const auto &directory = m_chatDirectories[candidate];
        for (const auto *list : {&directory.recent, &directory.found}) {
            const auto match = std::find_if(list->cbegin(), list->cend(), [&target](const Chat &chat) {
                return chat.id == target.id;
            });
            if (match != list->cend()) {
                scope = candidate;
                linkedChat = *match;
            }
        }
    }
    const bool sectionWillReloadChat = !m_active && m_chatScope == scope &&
        m_currentChatId == target.id && !m_messageSearchActive && m_contextDialogId.isEmpty();
    emit sectionRequested();
    m_chatTabs->setCurrentIndex(scope);
    auto &directory = m_chatDirectories[scope];
    m_chatSearchTimer->stop();
    m_messageSearchTimer->stop();
    if (m_messageSearchActive)
        showEmpty(m_chatMessages, m_chatEmpty, QStringLiteral("Загружаем сообщение…"), true);
    {
        const QSignalBlocker blocker(m_messageSearch);
        m_messageSearch->clear();
    }
    m_messageSearchActive = false;
    m_messageSearchQuery.clear();
    m_messageSearchScope.clear();
    {
        const QSignalBlocker blocker(m_chatSearch);
        m_chatSearch->clear();
    }
    directory.input.clear();
    directory.query.clear();
    directory.found.clear();
    ui::setStatusLabel(m_chatSearchProgress, {});
    if (linkedChat.id.isEmpty()) {
        linkedChat.id = target.id;
        linkedChat.title = QStringLiteral("Диалог %1").arg(target.id);
        linkedChat.taskChat = scope == 1;
    }
    directory.linked = linkedChat;
    if (target.messageId.isEmpty()) {
        resetMessageContext();
    } else if (m_currentChatId != target.id) {
        m_contextDialogId = target.id;
        m_contextMessageId = target.messageId;
    }
    const auto oldChat = m_currentChatId;
    if (oldChat != target.id) {
        switchDraft(m_chatComposer, m_chatDrafts, oldChat, {});
        m_currentChatId.clear();
        static_cast<ui::HistoryList *>(m_chatMessages)->resetHistory();
    }
    directory.selected = target.id;
    showChatResults(directory.recent);
    if (target.messageId.isEmpty()) {
        if (oldChat == target.id && !sectionWillReloadChat)
            m_backend->requestChatMessages(target.id);
        focusChatComposer();
        return;
    }
    openChatMessageContext(target.id, target.messageId);
}

void ChatPanel::openChatMessageContext(const QString &dialogId, const QString &messageId)
{
    const auto target = portalTaskIdentifier(messageId);
    if (!m_active || dialogId.isEmpty() || dialogId != m_currentChatId || target.isEmpty())
        return;
    const bool searching = m_messageSearchActive;
    m_messageSearchTimer->stop();
    {
        const QSignalBlocker search(m_messageSearch);
        const QSignalBlocker toggle(m_messageSearchToggle);
        m_messageSearch->clear();
        m_messageSearchToggle->setChecked(false);
        m_messageSearch->parentWidget()->hide();
    }
    m_messageSearchActive = false;
    m_messageSearchQuery.clear();
    m_messageSearchScope.clear();
    if (!searching && focusChatMessage(target))
        return;
    m_contextDialogId = dialogId;
    m_contextMessageId = target;
    m_latestChat->show();
    static_cast<ui::HistoryList *>(m_chatMessages)->setLiveReadHistory(false);
    showEmpty(m_chatMessages, m_chatEmpty, QStringLiteral("Загружаем сообщение…"), true);
    m_backend->requestMessageContext(dialogId, target);
}

void ChatPanel::resetMessageContext()
{
    m_contextDialogId.clear();
    m_contextMessageId.clear();
    if (m_chatMessages)
        static_cast<ui::HistoryList *>(m_chatMessages)->returnToLatest();
    if (m_latestChat)
        m_latestChat->hide();
}

bool ChatPanel::focusChatMessage(const QString &id)
{
    return static_cast<ui::HistoryList *>(m_chatMessages)->focusOnMessage(id);
}

void ChatPanel::openMessageViewers(const QString &messageId, const QPoint &anchor)
{
    const auto scope = m_currentChatId;
    const auto sameHistory = [panel = QPointer<ChatPanel>(this), directory = m_chatScope, scope] {
        return panel && panel->m_active && panel->m_chatMessages->isVisible()
            && panel->m_chatScope == directory && panel->m_currentChatId == scope;
    };
    auto *menu = workspace::openMessageViewers(m_backend, m_chatMessages, messageId, anchor, scope, false,
                                               sameHistory, {m_workspace.avatar, m_workspace.requestAvatar});
    if (!menu)
        return;
    const auto closeIfScopeChanged = [menu, sameHistory] {
        if (!sameHistory())
            menu->close();
    };
    connect(m_chats, &QListWidget::currentItemChanged, menu, closeIfScopeChanged, Qt::QueuedConnection);
    connect(m_chatTabs, &QTabBar::currentChanged, menu, closeIfScopeChanged, Qt::QueuedConnection);
    connect(this, &ChatPanel::sectionActiveChanged, menu, closeIfScopeChanged, Qt::QueuedConnection);
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

} // namespace bx
