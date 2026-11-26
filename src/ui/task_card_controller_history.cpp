#include "ui/task_card_controller.h"
#include "ui/task_dialogs.h"
#include "ui/chat_panel.h"
#include "ui/projects_panel.h"
#include "ui/chat_widgets.h"
#include "ui/history_surface.h"
#include "ui/workspace_message_viewers.h"
#include "ui/workspace_presentation.h"
#include <QMenu>
#include <QScrollBar>

namespace bx {

void TaskCardController::bindHistory(ui::TaskCardDialog *card)
{
    m_workspace.bindMessageFiles(card->messageList(), true);
    static_cast<workspace::RowPainter *>(card->messageList()->itemDelegate())->setMessageViewersHandler(
        [this, card](const QString &id, const QPoint &anchor) { openTaskMessageViewers(card->messageList(), id, anchor); });
    auto *history = static_cast<ui::HistoryList *>(card->messageList());
    history->setOnViewportChanged([this] { m_chatPanel->scheduleVisibleChatRead(); });
    history->setOnOlder([this, card](const QString &before) {
        if (m_card == card && m_cardSearchQuery.isEmpty())
            m_backend->requestOlderChatMessages(card->taskId(), before, true);
    });
    history->setReplyActions([card](const ui::ChatReply &reply) {
        card->composer()->setReply(reply);
        card->messageInput()->setFocus(Qt::OtherFocusReason);
    }, [this, card] { return m_card == card && m_cardDetailsReady && writable(); },
       [this](const QString &id) { openPinnedTaskMessage(id); });
    card->composer()->setTypingActions([this, card] {
        return m_card == card && m_cardDetailsReady && writable() && card->isActiveWindow()
            ? card->taskChatDialogId() : QString();
    }, [this](const QString &dialog) { m_backend->notifyChatTyping(dialog); });
    card->composer()->setEditLatest([this, card, history] {
        return m_card == card && m_cardDetailsReady && m_chatPanel->editLatestMessage(history, card->taskChatDialogId());
    });
    history->setMessageMenu([this, card](QMenu &menu, const QPersistentModelIndex &index, const QString &href) {
        if (m_card == card && m_cardDetailsReady)
            m_chatPanel->addMessageActions(menu, index, card->taskChatDialogId(), true, href);
    });
    history->setReactionActions([this, card](const QModelIndex &index, const QString &code, const QPoint &anchor) {
        if (m_card == card && m_cardDetailsReady)
            m_chatPanel->actOnMessageReaction(index, card->taskChatDialogId(), code, anchor, card);
    }, [this, card] {
        return m_card == card && m_cardDetailsReady && writable() && !m_backend->isDemo() && !card->taskChatDialogId().isEmpty();
    });
    connect(card->messageList()->verticalScrollBar(), &QScrollBar::valueChanged, this, &TaskCardController::avatarsRequested);
    ui::HistorySurface::bindMentions(card->composer(), m_backend, [card] { return card->taskChatDialogId(); },
                                     m_workspace.avatar, [this] { emit avatarsRequested(); });
    card->setOnSend([this, card] {
        m_workspace.sendFromComposer(m_taskSendController, card->composer(), card->taskId());
    });
    card->setOnSearch([this, card](const QString &query) {
        if (query.isEmpty()) {
            if (m_cardSearchQuery.isEmpty())
                return;
            m_cardSearchQuery.clear();
            card->setChatStatus(QStringLiteral("Загружаем сообщения…"), true);
            m_backend->requestTaskMessages(card->taskId());
        } else {
            m_cardContextMessageId.clear();
            card->setContextMode(false);
            m_cardSearchQuery = query;
            card->setChatStatus(QStringLiteral("Ищем сообщения…"), true);
            m_taskMessageSearch.request(card->taskId(), query);
        }
    });
    card->setOnPinned([this](const QString &id) { openPinnedTaskMessage(id); });
    card->setOnLatest([this, card] {
        if (m_card != card)
            return;
        m_cardContextMessageId.clear();
        card->setContextMode(false);
        m_backend->requestTaskMessages(card->taskId());
    });
    card->setOnFile([this, card](bool attachment) {
        m_workspace.pickFile(card->taskId(), attachment ? QStringLiteral("task") : QStringLiteral("task-chat"));
    });
}

QListWidget *TaskCardController::history() const
{
    return m_card ? m_card->messageList() : nullptr;
}

bool TaskCardController::readActiveTask(const QWidget *active)
{
    if (!m_card || active != m_card || !m_cardDetailsReady || !m_cardSearchQuery.isEmpty() || !m_cardContextMessageId.isEmpty())
        return false;
    const auto seen = static_cast<ui::HistoryList *>(m_card->messageList())->visibleMessageIds();
    if (!seen.all.isEmpty())
        m_backend->readTaskChatMessages(m_card->taskId(), seen.incoming, seen.all);
    return true;
}

void TaskCardController::updateTaskCardChatBinding(const QString &previousDialog)
{
    if (!m_card || m_card->taskChatDialogId() == previousDialog)
        return;
    const auto dialog = m_card->taskChatDialogId();
    m_card->composer()->dismissMentionSearch();
    m_card->setPinnedMessages({});
    m_cardContextMessageId.clear();
    m_card->setContextMode(false);
    if (!dialog.isEmpty())
        m_backend->requestPinnedMessages(dialog);
}

void TaskCardController::onMessagesFound(const QString &scopeId, const QString &query, bool task,
                                 const QVector<Message> &messages)
{
    if (!task || !m_taskMessageSearch.accept(scopeId, query) || !m_card ||
        m_card->taskId() != scopeId || m_cardSearchQuery.isEmpty())
        return;
    if (query != m_cardSearchQuery) {
        m_taskMessageSearch.request(scopeId, m_cardSearchQuery);
    } else if (query == m_card->searchQuery()) {
        m_card->showMessages(messages, ui::MessageView::Search);
        emit avatarRowsChanged();
        emit avatarsRequested();
    }
}

void TaskCardController::openPinnedTaskMessage(const QString &messageId)
{
    if (!m_card)
        return;
    const auto dialog = m_card->taskChatDialogId();
    const auto target = portalTaskIdentifier(messageId);
    if (dialog.isEmpty() || target.isEmpty())
        return;
    const bool searching = !m_cardSearchQuery.isEmpty();
    m_cardSearchQuery.clear();
    if (!searching && m_card->focusMessage(target))
        return;
    m_cardContextMessageId = target;
    m_card->setContextMode(true);
    m_card->setChatStatus(QStringLiteral("Загружаем сообщение…"), true);
    m_backend->requestMessageContext(dialog, target);
}

void TaskCardController::openTaskMessageViewers(QListWidget *history, const QString &messageId, const QPoint &anchor)
{
    if (!m_card || history != m_card->messageList())
        return;
    const auto scope = m_card->taskId();
    const auto sameHistory = [controller = QPointer<TaskCardController>(this), source = QPointer<QListWidget>(history), scope] {
        return controller && source && source->isVisible() && controller->m_card
            && source == controller->m_card->messageList() && scope == controller->m_card->taskId();
    };
    workspace::openMessageViewers(m_backend, history, messageId, anchor, scope, true, sameHistory, {
        [this](const QUrl &url) { return m_workspace.avatar(url); },
        [this](const QUrl &url) { m_workspace.requestAvatar(url); }
    });
}

} // namespace bx
