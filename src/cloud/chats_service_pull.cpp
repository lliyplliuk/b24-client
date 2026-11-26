#include "cloud/chats_service.h"
#include "cloud/cloud_chat_metadata.h"
#include "cloud/cloud_rows.h"

#include <algorithm>
#include <utility>

namespace bx {
using namespace cloud_rows;

void ChatsService::handleImPull(const QString &command, const QJsonObject &params, const QJsonObject &recentExtra, const QString &mid)
{
    if (command == QLatin1String("pinAdd") || command == QLatin1String("pinDelete")) {
        handlePinnedPull(command, params);
        return;
    }
    if (m_history.handleAnchorPull(command, params))
        return;
    if (command == QLatin1String("settingsUpdate")) {
        emit popupSettingsChanged(params);
        return;
    }
    const auto chatId = numericId(field(params, {"chatId"}));
    const auto dialog = pullDialog(params, chatId);
    if (command == QLatin1String("readMessageOpponent") || command == QLatin1String("readMessageChatOpponent")) {
        handleOpponentReadPull(dialog, params);
        return;
    }
    if (command == QLatin1String("inputActionNotify")) {
        handleTypingPull(dialog, params);
        return;
    }
    if (command == QLatin1String("message") || command == QLatin1String("messageChat")) {
        const auto user = numericId(field(params.value(QStringLiteral("message")).toObject(), {"senderId", "authorId"}));
        if (!dialog.isEmpty() && !user.isEmpty() && user != m_directory.currentUserId())
            emit chatTypingChanged(dialog, user, 0);
    }
    if (command == QLatin1String("addReaction") || command == QLatin1String("deleteReaction")) {
        handleReactionPull(dialog, params);
        return;
    }
    handleHistoryRemovalPull(command, dialog, chatId, params);
    if (command == QLatin1String("chatMuteNotify")) {
        handleMutePull(dialog, params);
        return;
    }
    const bool readEvent = command == QLatin1String("readMessage") || command == QLatin1String("readMessageChat");
    if (readEvent && !dialog.isEmpty()) {
        if (!acceptsRecentRead(dialog, params, recentExtra))
            return;
        if (const auto counter = chatUnreadCount(params.value(QStringLiteral("counter"))))
            m_timeline.dialogMessageState(dialog).unreadCounter = counter;
        m_history.applyReadHistory(dialog, params, {});
    }

    if (command == QLatin1String("notifyRead") || command == QLatin1String("notifyUnread")
        || command == QLatin1String("notifyConfirm") || command == QLatin1String("notifyDelete")) {
        emit notificationStateEvent(command, params);
        return;
    }
    if (readEvent || command.startsWith(QLatin1String("message"))) {
        m_timeline.invalidateRecent();
        const auto eventChat = chatDialog(chatId);
        const auto matches = [&](const QString &dialogId) {
            return !dialogId.isEmpty() && (dialog == dialogId || eventChat == dialogId);
        };
        if (matches(m_timeline.activeDialog()))
            m_timeline.invalidateHistory(false);
        if (matches(m_timeline.activeTaskDialog()))
            m_timeline.invalidateHistory(true);
        m_activity.scheduleRefresh();
    }
    applyRecentPull(command, dialog, chatId, params, recentExtra);
    if (command != QLatin1String("message") && command != QLatin1String("messageChat")
        && command != QLatin1String("notify") && command != QLatin1String("notifyAdd"))
        return;

    emit incomingNoticeEvent(command, dialog, chatId, params, recentExtra, mid);
}

QString ChatsService::pullDialog(const QJsonObject &params, const QString &chatId) const
{
    auto dialog = validDialog(field(params, {"dialogId"}));
    if (dialog.isEmpty() && !chatId.isEmpty()) {
        const auto id = chatId.toLongLong();
        const auto personal = std::find_if(m_timeline.recentChats(false).cbegin(), m_timeline.recentChats(false).cend(), [id](const Chat &chat) {
            return chat.serverChatId == id;
        });
        dialog = personal != m_timeline.recentChats(false).cend() ? personal->id
            : m_timeline.dialogForChat(id, chatDialog(chatId));
    }
    return dialog;
}

void ChatsService::handleOpponentReadPull(const QString &dialog, const QJsonObject &params)
{
    const auto user = numericId(field(params, {"userId"}));
    if (dialog.isEmpty() || user.isEmpty() || m_directory.currentUserId().isEmpty() || user == m_directory.currentUserId())
        return;
    auto &seen = m_timeline.dialogMessageState(dialog).seenByOthers;
    QStringList confirmed;
    const auto rows = params.value(QStringLiteral("viewedMessages")).toArray();
    for (qsizetype i = 0; i < std::min<qsizetype>(rows.size(), tunables::kPageSizeMessages); ++i) {
        const auto id = numericId(string(rows.at(i)));
        const auto number = id.toLongLong();
        if (number <= 0 || seen.contains(number))
            continue;
        if (seen.size() >= tunables::kPageSizeMessages)
            seen.erase(std::min_element(seen.cbegin(), seen.cend()));
        seen.insert(number);
        confirmed.append(id);
    }
    if (!confirmed.isEmpty())
        emit chatMessagesSeen(dialog, confirmed);
}

void ChatsService::handleTypingPull(const QString &dialog, const QJsonObject &params)
{
    const auto user = numericId(field(params, {"userId"}));
    if (m_activity.chatForeground() && !dialog.isEmpty() && !user.isEmpty() && !m_directory.currentUserId().isEmpty() && user != m_directory.currentUserId()
        && params.value(QStringLiteral("type")) == QLatin1String("writing")) {
        const auto duration = params.value(QStringLiteral("duration")).toInt();
        emit chatTypingChanged(dialog, user, duration > 0 ? std::min(duration, 25000) : 25000);
    }
}

void ChatsService::handleReactionPull(const QString &dialog, const QJsonObject &params)
{
    const auto reaction = params.value(QStringLiteral("actualReactions")).toObject().value(QStringLiteral("reaction")).toObject();
    const auto id = numericId(field(reaction, {"messageId"}));
    const auto matches = [&](const QString &shown) {
        return !shown.isEmpty() && (shown == dialog || (dialog.isEmpty() && !id.isEmpty()
            && m_timeline.hasReplyPreview(shown, id)));
    };
    if (matches(m_timeline.activeDialog()))
        m_timeline.invalidateHistory(false);
    if (matches(m_timeline.activeTaskDialog()))
        m_timeline.invalidateHistory(true);
    m_activity.scheduleRefresh();
}

void ChatsService::handleHistoryRemovalPull(const QString &command, const QString &dialog, const QString &chatId, const QJsonObject &params)
{
    const auto mappedChat = m_badges.badgeChatId(dialog);
    if (!dialog.isEmpty() && (chatId.isEmpty() || mappedChat == 0 || mappedChat == chatId.toLongLong())) {
        QStringList deleted;
        if (command == QLatin1String("messageUpdate") || command == QLatin1String("messageDelete")
            || command == QLatin1String("messageDeleteComplete")) {
            const auto message = numericId(field(params, {"id"}));
            if (!message.isEmpty()) {
                m_timeline.forgetReplyPreview(dialog, message);
                if (command != QLatin1String("messageUpdate"))
                    deleted.append(message);
            }
        } else if (command == QLatin1String("messageDeleteV2")) {
            for (const auto &value : params.value(QStringLiteral("messages")).toArray()) {
                const auto row = value.toObject();
                const auto message = numericId(field(row, {"id"}));
                const auto rowChat = field(row, {"chatId"});
                const auto rowDialog = field(row, {"dialogId"});
                if (message.isEmpty() || (!rowDialog.isEmpty() && rowDialog != dialog)
                    || (!rowChat.isEmpty() && mappedChat > 0 && rowChat.toLongLong() != mappedChat))
                    continue;
                m_timeline.forgetReplyPreview(dialog, message);
                if (!deleted.contains(message))
                    deleted.append(message);
                if (deleted.size() >= tunables::kPageSizeMessages)
                    break;
            }
        } else if (command == QLatin1String("chatHistoryDeletedForSelf") || command == QLatin1String("chatHistoryDeletedForAll")) {
            bool validBoundary = false;
            const auto through = field(params, {"lastId"}).toLongLong(&validBoundary);
            if (!params.contains(QStringLiteral("lastId")) || (validBoundary && through >= 0)) {
                if (through == 0)
                    ++m_timeline.dialogMessageState(dialog).historyGeneration;
                emit chatHistoryRemoved(dialog, {}, through);
                if (m_timeline.activeDialog() == dialog)
                    requestChatMessages(dialog);
                if (!m_timeline.activeTask().isEmpty() && m_timeline.activeTaskDialog() == dialog)
                    requestTaskMessages(m_timeline.activeTask());
            }
        }
        if (!deleted.isEmpty())
            emit chatHistoryRemoved(dialog, deleted);
    }
}

void ChatsService::handleMutePull(const QString &dialog, const QJsonObject &params)
{
    const auto muted = notificationFlag(params.value(QStringLiteral("muted")));
    if (!dialog.isEmpty() && muted) {
        auto &state = m_timeline.dialogMessageState(dialog);
        state.muted = muted;
        state.muteList = {};
        state.muteFromPull = true;
        m_history.deliverChatPopups();
    }
}

} // namespace bx
