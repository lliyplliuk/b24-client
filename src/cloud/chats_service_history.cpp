#include "chats_service.h"
#include "cloud_chat_metadata.h"
#include "cloud_rows.h"
#include "core/origin_guard.h"

#include <QUuid>
#include <algorithm>
#include <utility>

namespace bx {
using namespace cloud_rows;

namespace {
const QString chatIdentityOp = operationKey(OperationKind::ChatHistoryIdentity);

} // namespace

void ChatsService::loadMessages(const QString &key, const QString &dialogId, bool task, bool retry)
{
    if (m_directory.currentUserId().isEmpty() && (!m_triedHistoryIdentity || m_portalHttp.contains(chatIdentityOp))) {
        auto &pending = m_historiesWaitingForIdentity;
        pending.erase(std::remove_if(pending.begin(), pending.end(), [task](const auto &request) {
            return request.task == task;
        }), pending.end());
        pending.append({key, dialogId, task});
        if (!m_triedHistoryIdentity) {
            m_triedHistoryIdentity = true;
            m_session.withCurrentUser(chatIdentityOp, [this](qint64) { resumeHistoriesWithIdentity(); });
        }
        return;
    }
    const auto operation = operationKey(task ? OperationKind::TaskMessages : OperationKind::Chat, key);
    if (m_portalHttp.contains(operation))
        return;
    m_portalHttp.beginHistory(dialogId, task, retry);
    const auto generation = m_timeline.historyGeneration(dialogId);
    const auto superseded = [this, key, dialogId, task, generation, scope = m_session.accountScopeRevision()] {
        if (scope != m_session.accountScopeRevision())
            return true;
        if (m_timeline.historyGeneration(dialogId) == generation)
            return false;
        if (task ? m_timeline.activeTask() == key : m_timeline.activeDialog() == dialogId) {
            m_timeline.invalidateHistory(task);
            m_activity.scheduleRefresh();
        }
        return true;
    };
    const auto expectedChatId = m_badges.badgeChatId(dialogId);
    const auto groupChatId = groupDialogChatId(dialogId);
    const auto popupChatId = groupChatId > 0 ? groupChatId : expectedChatId;
    const bool snapshot = expectedChatId > 0;
    const auto snapshotOperation = operationKey(snapshot ? OperationKind::ChatHistorySnapshot : OperationKind::ChatHistoryRead, operation);
    const auto deliver = [this, key, dialogId, popupChatId, task](const QJsonObject &history, bool trustedMetadata) {
        const auto messages = m_history.decodeMessages(history, true, dialogId);
        if (trustedMetadata && (task ? m_timeline.activeTask() == key : m_timeline.activeDialog() == dialogId)) {
            rememberRecentHistoryPreview(dialogId, history);
            m_history.reconcileHistoryPopups(dialogId, popupChatId, task, history, messages);
        }
        if (task)
            emit taskMessagesLoaded(key, messages);
        else
            emit chatMessagesLoaded(key, messages);
    };
    m_portalHttp.ajax(QStringLiteral("im.v2.Chat.load"),
         {{QStringLiteral("dialogId"), dialogId}, {QStringLiteral("messageLimit"), QStringLiteral("25")}},
         snapshotOperation,
         [this, key, dialogId, task, operation, superseded, deliver, popupChatId,
                             counterRevision = m_timeline.nextBadgeRevision()](QJsonValue data) {
        if (superseded())
            return;
        const auto object = data.toObject();
        if (!matchesRequestedHistory(object, dialogId, popupChatId)) {
            // Retain direct history rows for existing callers without accepting the conflicting chat metadata.
            if (object.value(QStringLiteral("messages")).isArray())
                deliver(object, false);
            else
                m_session.failShape(operation);
            return;
        }
        const auto chat = object.value(QStringLiteral("chat")).toObject();
        const auto publishHistory = [this, key, dialogId, task, chat, popupChatId, counterRevision, deliver]
            (const QJsonObject &history) {
            if (task ? m_timeline.activeTask() == key : m_timeline.activeDialog() == dialogId) {
                const auto rows = history.value(QStringLiteral("messages")).toArray();
                rememberUnreadHistoryRows(dialogId, popupChatId > 0 ? popupChatId : m_badges.badgeChatId(dialogId),
                                          rows, counterRevision);
                reconcileHistoryUnread(dialogId, chat, rows, popupChatId, counterRevision);
            }
            deliver(history, true);
        };
        if (data.isObject()) {
            const auto live = m_timeline.overlays().constFind(dialogId);
            const auto counterCurrent = m_timeline.snapshotRevision() <= counterRevision
                && (live == m_timeline.overlays().cend() || live->counterRevision <= counterRevision);
            m_directory.rememberDialog(dialogId, object, counterCurrent);
            m_directory.rememberDialogContact(dialogId, object);
            acceptPinnedSnapshot(dialogId, object);
        } else if (task ? m_timeline.activeTask() != key : m_timeline.activeDialog() != dialogId) {
            return;
        }
        if (object.value(QStringLiteral("messages")).isArray()) {
            publishHistory(object);
            return;
        }
        auto chatId = field(object, {"chatId", "CHAT_ID"});
        if (chatId.isEmpty())
            chatId = QString::number(popupChatId > 0 ? popupChatId : m_badges.badgeChatId(dialogId));
        const auto pagingChatId = numericId(chatId).toLongLong();
        if (pagingChatId <= 0 || (popupChatId > 0 && pagingChatId != popupChatId))
            return m_session.failShape(operation);

        m_portalHttp.ajax(QStringLiteral("im.v2.Chat.Message.list"),
             {{QStringLiteral("chatId"), chatId}, {QStringLiteral("limit"), QStringLiteral("25")}},
             operationKey(OperationKind::ChatHistoryRead, operation), [this, operation, dialogId, pagingChatId, superseded, publishHistory](QJsonValue page) {
            if (superseded())
                return;
            const auto result = page.toObject();
            if (!result.value(QStringLiteral("messages")).isArray() || !matchesRequestedHistory(result, dialogId, pagingChatId))
                return m_session.failShape(operation);
            publishHistory(result);
        }, operation);
    }, operation);
}

void ChatsService::rememberRecentHistoryPreview(const QString &dialog, const QJsonObject &history)
{
    const auto groupChatId = groupDialogChatId(dialog);
    const auto expectedChatId = groupChatId > 0 ? groupChatId : m_badges.badgeChatId(dialog);
    if (expectedChatId <= 0)
        return;
    QJsonObject latest;
    qint64 latestId = 0;
    for (const auto &value : history.value(QStringLiteral("messages")).toArray()) {
        const auto message = value.toObject();
        const auto id = recentMessageId(message);
        if (id > latestId && explicitMessageChatId(message) == expectedChatId) {
            latest = message;
            latestId = id;
        }
    }
    if (latestId <= 0)
        return;
    auto &live = m_timeline.recentOverlay(dialog);
    const auto text = field(latest, {"text", "message"}).left(2000);
    const auto updated = date(latest.value(QStringLiteral("date")));
    const auto &previous = live.historyPreview;
    if (previous.chatId == expectedChatId
        && (latestId < previous.messageId
            || (latestId == previous.messageId && text == previous.text && updated == previous.updated)))
        return;
    live.historyPreview = {expectedChatId, latestId, text, updated};
    const bool recoverRecent = live.rejectedRecentPreviewChatId == expectedChatId;
    const bool recoverTask = live.rejectedTaskPreviewChatId == expectedChatId;
    if (recoverRecent)
        publishRecentChats(m_timeline.recentChats(false), false);
    if (recoverTask)
        publishRecentChats(m_timeline.recentChats(true), true);
}

void ChatsService::resumeHistoriesWithIdentity()
{
    const auto histories = std::exchange(m_historiesWaitingForIdentity, {});
    for (const auto &request : histories)
        loadMessages(request.key, request.dialog, request.task);
}

void ChatsService::setChatPinned(const QString &dialogId, bool pinned)
{
    const auto operation = operationKey(OperationKind::ChatPin, dialogId);
    const auto dialog = validDialog(dialogId);
    if (dialog.isEmpty()) {
        emit failed(operation, QStringLiteral("Некорректный номер диалога"));
        return;
    }
    const auto endpoint = pinned ? QStringLiteral("im.v2.Chat.pin") : QStringLiteral("im.v2.Chat.unpin");
    m_portalHttp.ajax(endpoint, {{QStringLiteral("dialogId"), dialog}}, operation, [this, dialogId, pinned, operation](QJsonValue result) {
        if (result == false)
            return m_session.failShape(operation);
        emit chatPinChanged(dialogId, pinned);
    });
}

void ChatsService::sendMessage(const QString &key, const QString &dialogId, const QString &text, bool task,
                               const QString &replyId)
{
    const auto scope = operationKey(task ? OperationKind::Task : OperationKind::Chat, key);
    const auto op = operationKey(task ? OperationKind::SendTask : OperationKind::SendChat, key);
    if (m_portalHttp.contains(op))
        return;
    if (validDialog(dialogId).isEmpty() || text.trimmed().isEmpty() || text.size() > 20000) {
        emit failed(op, QStringLiteral("Введите сообщение длиной до 20 000 символов"));
        return;
    }
    const auto reply = numericId(replyId);
    if (!replyId.isEmpty() && reply.isEmpty()) {
        emit failed(op, QStringLiteral("Некорректный идентификатор цитируемого сообщения"));
        return;
    }
    const auto templateId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_timeline.rememberSentTemplate(templateId);
    Fields fields{{QStringLiteral("dialogId"), dialogId},
                  {QStringLiteral("fields[message]"), text},
                  {QStringLiteral("fields[templateId]"), templateId}};
    if (!reply.isEmpty())
        fields.append({QStringLiteral("fields[replyId]"), reply});
    m_portalHttp.ajax(QStringLiteral("im.v2.Chat.Message.send"), std::move(fields),
         op, [this, key, scope, op, dialogId, task](QJsonValue data) {
        const auto id = field(data.toObject(), {"id", "messageId"});
        if (id.isEmpty()) {
            emit failed(op, QStringLiteral("Портал не подтвердил отправку. Проверьте историю перед повтором"));
            return;
        }
        emit messageSent(scope, id);
        if (task && m_timeline.activeTask() == key)
            m_timeline.invalidateHistory(true);
        else if (!task && m_timeline.activeDialog() == dialogId)
            m_timeline.invalidateHistory(false);
        m_activity.scheduleRefresh();
    });
}

void ChatsService::sendChatMessage(const QString &dialogId, const QString &text)
{
    sendMessage(dialogId, dialogId, text, false);
}

void ChatsService::sendChatReply(const QString &dialog, const QString &text, const QString &replyId)
{
    if (replyId.isEmpty()) {
        emit failed(operationKey(OperationKind::SendChat, dialog), QStringLiteral("Выберите сообщение для ответа"));
        return;
    }
    sendMessage(dialog, dialog, text, false, replyId);
}

void ChatsService::sendTaskMessage(const QString &taskId, const QString &text)
{
    sendTaskMessage(taskId, text, {});
}

void ChatsService::sendTaskReply(const QString &taskId, const QString &text, const QString &replyId)
{
    if (replyId.isEmpty()) {
        emit failed(operationKey(OperationKind::SendTask, taskId), QStringLiteral("Выберите сообщение для ответа"));
        return;
    }
    sendTaskMessage(taskId, text, replyId);
}

void ChatsService::sendTaskMessage(const QString &taskId, const QString &text, const QString &replyId)
{
    const auto dialog = m_timeline.taskDialog(taskId);
    if (dialog.isEmpty()) {
        emit failed(operationKey(OperationKind::SendTask, taskId), QStringLiteral("Сначала откройте чат задачи"));
        return;
    }
    sendMessage(taskId, dialog, text, true, replyId);
}

void ChatsService::rememberUnreadHistoryRows(const QString &dialog, qint64 chatId, const QJsonArray &rows, quint64 revision)
{
    if (chatId <= 0 || m_badges.badgeChatId(dialog) != chatId)
        return;
    auto &state = m_timeline.dialogMessageState(dialog);
    if (state.readReceiptRevision > revision)
        return;
    const auto currentUser = numericId(m_directory.currentUserId()).toLongLong();
    QSet<qint64> pageUnread;
    for (const auto &value : rows) {
        const auto row = value.toObject();
        const auto id = recentMessageId(row);
        const auto unread = row.value(QStringLiteral("unread"));
        if (id <= 0 || !unread.isBool() || explicitMessageChatId(row) != chatId)
            continue;
        if (!unread.toBool() || id <= state.confirmedReadThrough || state.exactlyRead.contains(id)) {
            state.historyUnread.remove(id);
            pageUnread.remove(id);
            continue;
        }
        const auto author = explicitMessageAuthorId(row);
        if (author && (*author == 0 || (currentUser > 0 && *author != currentUser))
            && pageUnread.size() < tunables::kPageSizeMessages)
            pageUnread.insert(id);
    }
    state.historyUnread.subtract(pageUnread);
    const auto excess = state.historyUnread.size() + pageUnread.size() - tunables::kRetainedChatMessages;
    if (excess > 0) {
        auto oldest = state.historyUnread.values();
        std::sort(oldest.begin(), oldest.end());
        for (qsizetype index = 0; index < excess; ++index)
            state.historyUnread.remove(oldest.at(index));
    }
    state.historyUnread.unite(pageUnread);
}

void ChatsService::reconcileHistoryUnread(const QString &dialog, const QJsonObject &chat, const QJsonArray &messages,
                                         qint64 expectedChatId, quint64 revision)
{
    const auto chatId = numericId(field(chat, {"id", "chatId"})).toLongLong();
    const auto groupId = groupDialogChatId(dialog);
    const auto chatIdAlias = chat.value(QStringLiteral("chatId"));
    const auto firstUnreadId = chatUnreadCount(chat.value(QStringLiteral("unreadId")));
    const auto readIdValue = chat.value(QStringLiteral("lastId"));
    bool hasReadId = true;
    const auto readId = readIdValue.isString() ? readIdValue.toString().toLongLong(&hasReadId)
        : readIdValue.toInteger(-1);
    if (chatId <= 0 || (expectedChatId > 0 && chatId != expectedChatId)
        || (groupId > 0 && chatId != groupId)
        || (!chatIdAlias.isUndefined() && numericId(string(chatIdAlias)).toLongLong() != chatId)
        || validDialog(field(chat, {"dialogId"})) != dialog || firstUnreadId != 0
        || !hasReadId || readId < 0
        || m_timeline.snapshotRevision() > revision)
        return;
    const auto *state = m_timeline.findMessageState(dialog);
    if (state && readId < state->lastReadId)
        return;
    auto &live = m_timeline.recentOverlay(dialog);
    if (live.counterRevision > revision || live.incomingId > readId)
        return;

    // Chat metadata carries a participant count. Only its explicit no-unread cursor can
    // repair badges, and only if the exact scoped history does not contradict it.
    for (const auto value : messages) {
        const auto row = value.toObject();
        if (recentMessageId(row) > 0 && explicitMessageChatId(row) == chatId
            && notificationFlag(row.value(QStringLiteral("unread"))).value_or(false))
            return;
    }

    const bool changed = live.chatId != chatId || live.counter != 0;
    live.chatId = chatId;
    live.counterRevision = revision;
    live.counter = 0;
    if (changed) {
        m_badges.publishUnreadChatCount();
        m_badges.republishChatBadges(chatId);
    }
}

void ChatsService::requestChatMessages(const QString &dialogId)
{
    const auto valid = validDialog(dialogId);
    if (valid.isEmpty()) {
        emit failed(operationKey(OperationKind::Chat, dialogId), QStringLiteral("Некорректный номер диалога"));
        return;
    }
    if (m_timeline.activeDialog() != valid) {
        m_portalHttp.invalidateHistory(false);
        m_timeline.openHistory(valid, false);
        m_history.clearPopupHistory(false);
    }
    if (m_activity.chatsSectionActive())
        m_activity.updateHistoryCatchup(true);
    if (m_activity.chatsSectionActive() && m_activity.canAutoRefreshChats() && (m_portalHttp.atCapacity()
        || m_portalHttp.contains(operationKey(OperationKind::Chat, dialogId)))) {
        m_timeline.invalidateHistory(false);
        m_activity.scheduleRefresh();
        return;
    }
    loadMessages(dialogId, valid, false);
}

void ChatsService::requestTaskMessages(const QString &taskId)
{
    if (m_timeline.activeTask() != taskId) {
        m_portalHttp.invalidateHistory(true);
        m_timeline.openHistory(taskId, true);
        m_history.clearPopupHistory(true);
    }
    m_activity.updateHistoryCatchup(true);
    if (m_activity.canAutoRefreshChats() && (m_portalHttp.atCapacity()
        || m_portalHttp.contains(operationKey(OperationKind::TaskMessages, taskId)))) {
        m_timeline.invalidateHistory(true);
        m_activity.scheduleRefresh();
        return;
    }
    if (const auto id = m_timeline.taskDialog(taskId); !id.isEmpty()) {
        loadMessages(taskId, id, true);
        return;
    }
    const auto op = operationKey(OperationKind::TaskMessages, taskId);
    m_history.loadChatTask(taskId, op, [this, taskId, op](const QJsonObject &) {
        const auto dialog = m_timeline.taskDialog(taskId);
        if (dialog.isEmpty()) {
            emit failed(op, QStringLiteral("У задачи нет чата. Старые обсуждения могут быть в комментариях портала"));
            return;
        }
        loadMessages(taskId, dialog, true);
    });
}

} // namespace bx
