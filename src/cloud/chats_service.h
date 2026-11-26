#pragma once

#include "chat_timeline.h"
#include "portal_http.h"
#include "core/models.h"
#include "core/tunables.h"

#include <QCache>
#include <QJsonArray>
#include <QJsonObject>
#include <QQueue>
#include <QSet>
#include <QTimer>

namespace bx {

class ChatsHost : public virtual SessionStateHost {
public:
    using ResolvedDialog = std::function<void(const QString &, qint64)>;
    virtual void applyReadHistory(const QString &dialog, const QJsonObject &receipt, const QStringList &submitted) = 0;
    virtual void clearPopupHistory(bool task) = 0;
    virtual void loadChatTask(const QString &taskId, const QString &operation, std::function<void(QJsonObject)> onTask) = 0;
    virtual bool canAutoRefreshChats() const = 0;
    virtual void updateHistoryCatchup(bool restart) = 0;
    virtual bool handleAnchorPull(const QString &command, const QJsonObject &params) = 0;
    virtual void deliverChatPopups() = 0;
    virtual bool chatForeground() const = 0;
    virtual bool chatOnline() const = 0;
    virtual bool chatsSectionActive() const = 0;
    virtual bool hasProjectChat(qint64 chatId) const = 0;
    virtual bool hasChatCounter(qint64 chatId) const = 0;
    virtual void publishProjectChatBadges(qint64 chatId) = 0;
    virtual void republishChatBadges(qint64 changedChatId = 0) = 0;
    virtual void trackProjectChat(const Chat &chat) = 0;
    virtual void publishProjectChatCounts() = 0;
    virtual void applyChatContact(Chat &chat) const = 0;
    virtual QUrl avatarUrlFromPortal(const QString &value) const = 0;
    virtual qint64 badgeChatId(const QString &dialog) const = 0;
    virtual QVector<Message> decodeMessages(const QJsonObject &data, bool sortChronologically, const QString &dialog = {}) const = 0;
    virtual void decorateChatBadges(Chat &chat) const = 0;
    virtual void decorateProjectTaskChat(Chat &chat) const = 0;
    virtual void failShape(const QString &operation) = 0;
    virtual void loadChatBadges() = 0;
    virtual void publishUnreadChatCount() = 0;
    virtual void reconcileHistoryPopups(const QString &dialog, qint64 chatId, bool task, const QJsonObject &history,
                                const QVector<Message> &messages) = 0;
    virtual void rememberChatContact(const QString &dialogId, const QJsonObject &user, bool publish) = 0;
    virtual void rememberCurrentUser(const QString &id) = 0;
    virtual void rememberDialog(const QString &dialogId, const QJsonObject &payload, bool rememberCounter = true) = 0;
    virtual void rememberDialogContact(const QString &dialogId, const QJsonObject &payload) = 0;
    virtual void resolveDialog(const QString &scopeId, bool task, const QString &operation,
                       const QString &requestKey, ResolvedDialog onDialog) = 0;
    virtual void scheduleRefresh() = 0;
    virtual void withCurrentUser(const QString &operation, std::function<void(qint64)> onUser) = 0;
    virtual QUrl portalOrigin() const = 0;
    virtual QString currentUserId() const = 0;
    virtual bool hasCounterSnapshot() const = 0;
};

class ChatsService final : public QObject {
    Q_OBJECT
public:
    ChatsService(ChatsHost &host, ChatTimeline &timeline, PortalHttp &http, QObject *parent);
    void handleImPull(const QString &command, const QJsonObject &params, const QJsonObject &recentExtra, const QString &mid);
    void reset();
    void invalidateRecentStatuses();
    void resumeRecentWithIdentity();
    void forgetTaskTitle(const QString &task);
    void requestChatMessages(const QString &dialogId);
    void requestTaskMessages(const QString &taskId);
    void changeChatMember(const QString &dialog, const QString &user, bool add);
    void changeMessage(const QString &dialog, const QString &id, MessageAction action, const QString &value = {});
    void createGroupChat(const QString &title, const QStringList &members);
    void forwardMessage(const QString &source, const QString &id, const QString &target);
    void loadMessages(const QString &key, const QString &dialogId, bool task, bool retry = false);
    void loadRecentChats(bool task, bool badgesOnly = false);
    void notifyChatTyping(const QString &dialog);
    void publishRecentChats(QVector<Chat> chats, bool task);
    QString recentTitle(const QString &dialog, const QString &fallback) const;
    void rememberTaskTitle(const QString &id, const QString &title);
    void requestOlderChatMessages(const QString &scopeId, const QString &beforeId, bool task);
    void requestPinnedMessages(const QString &dialogId);
    void requestRecentChats();
    void requestTaskChatBadges();
    void requestTaskChats();
    void resumeHistoriesWithIdentity();
    void searchChats(const QString &query);
    void searchMessages(const QString &scopeId, const QString &query, bool task);
    void searchTaskChats(const QString &query);
    void sendChatMessage(const QString &dialogId, const QString &text);
    void sendChatReply(const QString &dialog, const QString &text, const QString &replyId);
    void sendTaskMessage(const QString &taskId, const QString &text);
    void sendTaskReply(const QString &task, const QString &text, const QString &replyId);
    void setChatPinned(const QString &dialogId, bool pinned);

signals:
    void popupSettingsChanged(const QJsonObject &params);
    void notificationStateEvent(const QString &command, const QJsonObject &params);
    void incomingNoticeEvent(const QString &command, const QString &dialog, const QString &chatId, const QJsonObject &params, const QJsonObject &extra, const QString &mid);
    void chatMessagesSeen(const QString &dialogId, const QStringList &messageIds);
    void chatTypingChanged(const QString &dialogId, const QString &userId, int durationMs);
    void chatActionSucceeded(const QString &dialogId, const QString &message);
    void chatHistoryRemoved(const QString &dialogId, const QStringList &messageIds, qint64 through = 0);
    void chatMessagesLoaded(const QString &chatId, const QVector<Message> &messages);
    void chatOwnReactionChanged(const QString &dialogId, const QString &messageId, const QString &code, bool present);
    void chatPinChanged(const QString &dialogId, bool pinned);
    void chatsFound(const QString &query, const QVector<Chat> &chats);
    void failed(const QString &operation, const QString &reason);
    void groupChatCreated(const QString &dialogId);
    void messageEditAccepted(const QString &dialogId, const QString &messageId, const QString &text);
    void messageSent(const QString &scopeId, const QString &messageId);
    void messagesFound(const QString &scopeId, const QString &query, bool task,
                       const QVector<Message> &messages);
    void olderChatMessagesLoaded(const QString &scopeId, const QString &beforeId, bool task,
                                const QVector<Message> &messages, bool hasMore);
    void pinnedMessagesLoaded(const QString &dialogId, const QVector<Message> &messages);
    void recentChatsLoaded(const QVector<Chat> &chats);
    void taskChatsFound(const QString &query, const QVector<Chat> &chats);
    void taskChatsLoaded(const QVector<Chat> &chats);
    void taskMessagesLoaded(const QString &taskId, const QVector<Message> &messages);

private:
    void handlePinnedPull(const QString &command, const QJsonObject &params);
    bool acceptsRecentRead(const QString &dialog, const QJsonObject &params, const QJsonObject &extra) const;
    void applyRecentPull(const QString &command, const QString &dialog, const QString &chatId,
                         const QJsonObject &params, const QJsonObject &extra);
    Chat chatFromMetadata(const QString &dialog, const QJsonObject &row, const QJsonObject &chat,
                          const QJsonObject &user);
    void sendTaskMessage(const QString &taskId, const QString &text, const QString &replyId);
    void refreshChangedChat(const QString &dialog, bool pins = false);
    void rememberUnreadHistoryRows(const QString &dialog, qint64 chatId, const QJsonArray &rows, quint64 revision);
    void reconcileHistoryUnread(const QString &dialog, const QJsonObject &chat, const QJsonArray &messages,
                                qint64 expectedChatId, quint64 revision);
    using Fields = PortalHttp::Fields;
    ChatsHost &m_host;
    ChatTimeline &m_timeline;
    PortalHttp &m_portalHttp;
    QString pullDialog(const QJsonObject &params, const QString &chatId) const;
    void handleOpponentReadPull(const QString &dialog, const QJsonObject &params);
    void handleTypingPull(const QString &dialog, const QJsonObject &params);
    void handleReactionPull(const QString &dialog, const QJsonObject &params);
    void handleHistoryRemovalPull(const QString &command, const QString &dialog, const QString &chatId, const QJsonObject &params);
    void handleMutePull(const QString &dialog, const QJsonObject &params);
    void acceptPinnedSnapshot(const QString &dialog, const QJsonObject &payload);
    std::optional<bool> cachedDismissalStatus(const QString &id) const;
    QVector<Message> decodePinnedMessages(const QJsonObject &payload) const;
    void hydrateRecentUserStatuses(QVector<Chat> chats, const QStringList &ids, quint64 generation);
    void publishPinnedMessages();
    void rememberDismissalStatus(const QString &id, std::optional<bool> dismissed);
    void rememberRecentHistoryPreview(const QString &dialog, const QJsonObject &history);
    void resolveTaskChatTitles(QVector<Chat> chats, const QString &key, std::function<void(QVector<Chat>)> deliver);
    void sendMessage(const QString &key, const QString &dialogId, const QString &text, bool task,
                     const QString &replyId = {});
    struct PendingRecentPull { QString command; QString dialog; QString chatId; QJsonObject params; QJsonObject extra; };
    QVector<PendingRecentPull> m_recentPullsAwaitingIdentity;
    struct PersonalChatStatus { std::optional<bool> dismissed; qint64 expiresAt; };
    QCache<QString, PersonalChatStatus> m_personalChatStatuses{2 * tunables::kPageSizeChats};
    quint64 m_recentStatusGeneration = 0;
    quint64 m_taskTitleGeneration = 0;
    struct HistoryRequest { QString key; QString dialog; bool task; };
    QVector<HistoryRequest> m_historiesWaitingForIdentity;
    bool m_triedHistoryIdentity = false;
    QCache<QString, QString> m_recentTitles{2 * tunables::kPageSizeChats};
    QCache<QString, QString> m_taskChatTitles{2 * tunables::kPageSizeChats};
    QHash<QString, qint64> m_lastTypingRequest;
    QString m_pinnedDialog;
    quint64 m_pinnedGeneration = 0;
    bool m_havePinnedSnapshot = false;
    QVector<Message> m_pinnedMessages;
    QHash<QString, std::optional<Message>> m_pinnedChanges;
};

} // namespace bx
