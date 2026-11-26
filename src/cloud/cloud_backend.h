#pragma once

#include "cloud/notifications_service.h"
#include "cloud/projects_service.h"
#include "cloud/pull_channel.h"
#include "cloud/tasks_service.h"
#include "cloud/chats_service.h"
#include "core/backend.h"
#include "core/session.h"
#include "core/tunables.h"

#include <QCache>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <functional>
#include <memory>

class QNetworkReply;
class QUrlQuery;
class CloudTestAccess;

namespace bx {

namespace signin { class SessionRestore; }

class CloudBackend final : public Backend, private NotificationsHost, private PullHost, private PortalSession, private ProjectsHost,
                           private CloudServiceContext, private ChatActivityHost, private ChatHistoryHost,
                           private ChatDirectoryHost, private ChatBadgeHost, private TaskChatHost, private TaskFileHost {
    Q_OBJECT

public:
    explicit CloudBackend(Session session, bool allowLoopback, QObject *parent = nullptr);
    ~CloudBackend() override;

    QString displayName() const override;
    bool isDemo() const override { return false; }
    QUrl portalOrigin() const override { return m_session.origin; }
    QString settingsScope() const override;
    void start() override;
    void stop() override;

    void requestRecentChats() override;
    void requestTaskChats() override;
    void requestTaskChatBadges() override;
    void requestProjectTaskChats(const QString &projectId, int offset = 0) override;
    std::optional<qint64> unreadChatCount() const override { return m_unreadChatCount; }
    void downloadMessageFile(const QUrl &url, const QString &targetPath) override;
    void downloadMessagePhoto(const QUrl &url, const QString &targetPath) override;
    void cancelMessagePhoto(const QString &targetPath) override;
    void downloadMessageVideo(const QUrl &url, const QString &targetPath) override;
    void cancelMessageVideo(const QString &targetPath) override;
    void requestAvatar(const QUrl &url) override;
    void requestMessageViewers(const QString &scopeId, const QString &messageId, bool task,
                               MessageViewersHandler complete) override;
    void requestMessageImage(const QUrl &url) override;
    void requestMessageLinkPreview(const QUrl &url, LinkPreviewHandler complete) override;
    void requestChatMessages(const QString &dialogId) override;
    void requestOlderChatMessages(const QString &scopeId, const QString &beforeId, bool task) override;
    void requestPinnedMessages(const QString &dialogId) override;
    void readChatMessages(const QString &dialog, const QStringList &incoming, const QStringList &visible = {}) override;
    void readTaskChatMessages(const QString &task, const QStringList &incoming, const QStringList &visible = {}) override;
    void requestProjects() override;
    void requestProjectMembers(const QString &projectId) override;
    void requestProjectFiles(const QString &projectId, const QString &folderId, int offset) override;
    void requestMoreProjects() override;
    void requestProjectBoard(const QString &projectId) override;
    void requestMoreBoardTasks(const QString &projectId, const QString &stageId) override;
    void requestProjectStages(const QString &projectId) override;
    void requestTask(const QString &taskId) override;
    void requestTaskChecklist(const QString &taskId) override;
    void addTaskChecklistItem(const QString &taskId, const QString &parentId, const QString &title) override;
    void setTaskChecklistComplete(const QString &taskId, const QString &itemId, bool complete) override;
    void requestSubtasks(const QString &taskId) override;
    void createSubtask(const QString &parentId, const QString &projectId, const TaskEdit &edit) override;
    void requestTaskDependencies(const QString &taskId) override;
    void requestTaskHistory(const QString &taskId) override;
    void requestProjectTimeline(const QString &projectId) override;
    void requestTaskMessages(const QString &taskId) override;
    void notifyChatTyping(const QString &dialog) override;
    void requestChatAssets(const QString &dialog, ChatAssetKind kind, const QString &cursor = {}) override;
    void requestChatLinkPreviews(const QString &scopeId, bool task, ChatLinkPreviewHandler complete) override;
    void changeMessage(const QString &dialog, const QString &id, MessageAction action, const QString &value = {}) override;
    void forwardMessage(const QString &source, const QString &id, const QString &target) override;
    void createGroupChat(const QString &title, const QStringList &members) override;
    void changeChatMember(const QString &dialog, const QString &user, bool add) override;
    void requestNotifications() override;
    void requestOlderNotifications() override;
    void searchNotifications(const QString &query) override;
    void markNotificationRead(const QString &id) override;
    void markNotificationsRead(const QStringList &ids) override;
    void markAllNotificationsRead() override;
    void deleteNotification(const QString &id) override;
    void requestPortalStatus() override;
    void updatePortalStatus(const QString &expected, const QString &status) override;
    void requestNotificationSettings() override;
    void updateNotificationSetting(const QString &name, int value) override;
    void requestMessageContext(const QString &dialogId, const QString &messageId) override;
    void requestPerson(const QString &id) override;
    void requestCurrentPerson(PersonHandler complete) override;
    void searchPeople(const QString &requestId, const QString &query, const QString &dialogId = {}) override;
    void sendChatMessage(const QString &dialogId, const QString &text) override;
    void sendTaskMessage(const QString &taskId, const QString &text) override;
    void sendChatReply(const QString &dialog, const QString &text, const QString &replyId) override;
    void sendTaskReply(const QString &task, const QString &text, const QString &replyId) override;
    void updateTask(const QString &taskId, const TaskEdit &edit) override;
    void deleteTask(const QString &taskId) override;
    void createTask(const QString &projectId, const TaskEdit &edit) override;
    void createProject(const QString &name) override;
    void moveTask(const QString &projectId, const QString &taskId, const QString &stageId) override;
    void saveStage(const QString &projectId, const QString &stageId, const QString &title,
                   const QString &afterStageId, const QString &color = {}) override;
    void deleteStage(const QString &projectId, const QString &stageId) override;
    void searchChats(const QString &query) override;
    void searchTaskChats(const QString &query) override;
    void setChatPinned(const QString &dialogId, bool pinned) override;
    void setProjectPinned(const QString &projectId, bool pinned) override;
    void searchProjectTasks(const QString &projectId, const QString &query) override;
    void searchMessages(const QString &scopeId, const QString &query, bool task) override;
    void uploadChatFile(const QString &scopeId, const QString &localPath, bool task) override;
    void uploadChatImage(const QString &scopeId, const QString &localPath, bool task, const QString &caption,
                         std::shared_ptr<QTemporaryDir> temporaryDirectory = {}) override;
    void attachTaskFile(const QString &taskId, const QString &localPath) override;

    void setForeground(bool foreground);
    void setWorkspaceVisible(bool visible);
    void setActiveSection(int section);
    void clearOpenHistory(bool task);
    void recoverSession(const Session &candidate, std::function<bool()> stillAllowed = {});
    void cancelSessionRecovery();
    void requestWorkday() override;
    void changeWorkday(WorkdayAction action, const QString &report = {}) override;
    void changeWorkdayAt(WorkdayAction action, const QDate &date, const QTime &time, const QString &reason,
                         const QString &report = {}) override;
    void requestWorktimeSheet(const QDate &month) override;
    void exportWorktimeSheet(const QDate &month, const QString &targetPath) override;

signals:
    void notificationReceived(const Notification &notification);
    void sessionRejected();
    void sessionUpdated(const Session &session);
    void sessionRecovered();

private:
    bool popupWorkspaceVisible() const override;
    QString popupPortalStatus() const override;
    bool initialStatusPending() const override;
    void popupIdentityResolved() override;
    friend class ::CloudTestAccess;
    void clearPopupHistory(bool task) override { m_notices.clearHistory(task); }
    void loadChatTask(const QString &taskId, const QString &operation, std::function<void(QJsonObject)> onTask) override;
    bool chatForeground() const override { return m_foreground; }
    bool chatOnline() const override { return m_online; }
    bool chatsSectionActive() const override { return m_activeSection == 0; }
    bool hasProjectChat(qint64 chatId) const override { return m_projects.containsChat(chatId); }
    bool hasChatCounter(qint64 chatId) const override { return m_chatCounterBadges.contains(chatId); }
    void publishProjectChatBadges(qint64 chatId) override { m_projects.publishProjectTaskChatBadges(chatId); }
    void trackProjectChat(const Chat &chat) override { m_projects.rememberProjectUnreadTaskChat(chat); }
    void publishProjectChatCounts() override { m_projects.publishProjectUnreadCounts(); }


    const Session &currentSession() const override { return m_session; }
    quint64 sessionRevision() const override { return m_sessionRevision; }
    quint64 accountScopeRevision() const override { return m_accountScopeRevision; }
    bool started() const override { return m_started; }
    bool authExpired() const override { return m_authExpired; }
    bool allowLoopback() const override { return m_allowLoopback; }
    void confirmPortalSession(quint64 revision) override { confirmSession(revision); }
    void reconnectPortal() override { m_pull.scheduleReconnect(); }
    void retryHistory(const QString &key, const QString &dialog, bool task) override { m_chats.loadMessages(key, dialog, task, true); }
    void applyRenewedSession(const Session &renewed) override;
    void portalRequestFailed(const QString &operation, const QString &detail) override { emit failed(operation, detail); }
    QString currentUserId() const override { return m_userId; }
    QUrl projectFileUrl(const QString &value, qint64 size) const override { return registerMessageFileUrl(value, size, false); }
    bool projectsVisible() const override { return m_activeSection == 1 && m_foreground; }
    bool hasCounterSnapshot() const override { return m_haveCounterSnapshot; }
    QHash<qint64, int> chatMessageCounters() const override;

    using Fields = PortalHttp::Fields;
    using Headers = PortalHttp::Headers;
    using Handler = PortalHttp::Handler;

    bool isCurrentResponse(OperationKind kind, const QString &key, const QString &operation) const override;
    bool isCurrentHistory(bool task, const QString &key, const QString &dialog) const override;

    struct WorktimeExport;
    void resetWorktime();
    void queryWorkday(const QString &action, Fields fields, bool allowSessionRetry = true);
    void loadWorktimeSheet(const QDate &month);
    void pollWorktimeExport(const std::shared_ptr<WorktimeExport> &job);
    void downloadWorktimeExport(const std::shared_ptr<WorktimeExport> &job, const QUrl &url);
    QNetworkReply *startWorktimeRequest(const QUrl &url, const QByteArray &form, qsizetype limit);
    QJsonObject m_workday;
    QNetworkReply *m_workdayReply = nullptr;
    QDate m_worktimeSheetMonth;
    bool m_loadingWorktimeSheet = false;
    QDate m_worktimeExportMonth;
    Fields m_worktimeExportFields;
    std::weak_ptr<WorktimeExport> m_worktimeExport;
    quint64 m_worktimeRevision = 0;

    void ajax(const QString &action, Fields fields, const QString &operation, Handler onData,
              const QString &requestKey = {});
    void ajaxJson(const QString &action, const QJsonObject &body, const QString &operation, Handler onData,
                  const QString &requestKey = {});
    void ajaxJson(QUrl url, const QJsonObject &body, const QString &operation, Handler onData,
                  const QString &requestKey = {});
    void componentAction(const QString &action, const Fields &fields, const QString &operation, Handler onData,
                         const QString &component = QStringLiteral("bitrix:tasks.kanban"), const QString &requestKey = {});
    QUrl actionUrl(const QString &action) const;
    void withCurrentUser(const QString &operation, std::function<void(qint64)> onUser) override;
    void requestCurrentUser(const QString &operation, std::function<void(const QString &)> onUser,
                            const std::shared_ptr<Session> &candidate = {});
    quint64 unreadBindingRevision() const override { return m_projects.unreadBindingRevision(); }
    void rememberLoadedTask(const QString &taskId, const QJsonObject &task, quint64 bindingRevision) override;
    void updateTaskChatProject(const QString &taskId, qint64 project) override;
    void clearDeletedTaskHistory(const QString &taskId) override;
    void forgetTaskChat(const QString &taskId) override;
    QUrl taskFileUrl(const QString &value, qint64 size, bool photo) const override { return registerMessageFileUrl(value, size, photo); }
    QUrl taskImageUrl(const QString &value, const QString &attachmentId, const QString &objectId,
                      bool original) const override { return taskImageUrlFromPortal(value, attachmentId, objectId, original); }
    qsizetype taskImageMaxBytes() const override { return m_messageImageAssets.maxBytes; }
    void uploadTaskFile(const QString &taskId, const QString &localPath, const QString &operation) override;
    void resetAccountScopedState();
    void rememberCurrentUser(const QString &id) override;
    void loadPerson(const QString &id, const QString &operation, PersonHandler onPerson, bool requireName = false);
    void rememberTaskTitle(const QString &id, const QString &title) override;
    QString recentTitle(const QString &dialog, const QString &fallback) const;
    void fetchPortalHtml(const QUrl &url, std::function<void(QString)> handler);
    void handleTaskPull(const QJsonObject &fields, const QString &groupId, const QString &taskId, bool invalidatePreview);
    void handlePresencePull(const QJsonObject &params, qint64 timestamp);
    void loadChatPopupSettings();
    void readPortalStatus(const QString &operation, std::function<void(const QString &, bool)> onStatus);
    void loadInitialPortalStatus();
    void settleInitialPortalStatus();
    void handlePortalStatusPush(const QJsonObject &users, qint64 stamp);
    void bindPortalStatusIdentity(bool accountChanged);
    struct PendingPortalStatus { QString status; qint64 stamp = 0; };
    QHash<QString, PendingPortalStatus> m_unboundPortalStatuses;
    bool m_initialPortalStatusPending = false;
    void rememberPortalStatus(const QString &status, bool notifyUnchanged = false);
    QString m_portalStatus;
    quint64 m_portalStatusRevision = 0;
    qint64 m_lastPortalStatusPushTime = 0;
    void deliverChatPopups();
    void invalidateActiveView();
    void updateHistoryCatchup(bool restart = false) override;
    bool canAutoRefreshChats() const override { return m_foreground || m_workspaceVisible; }
    void updateChatViewActivity(bool wasWatching);
    void rejectSession() override;
    void confirmSession(quint64 revision, std::function<void(bool)> onConfirmed = {});
    void cancelSessionConfirmation();
    bool refreshCsrf(Session candidate, quint64 revision) override;
    void scheduleRefresh() override;
    void refreshDirtyViews();
    void invalidateTaskChat(const QString &taskId) override;
    void setOnline(bool online);
    void failShape(const QString &operation) override;

    enum class RasterRegistration { Primary, Standby };
    QUrl avatarUrlFromPortal(const QString &value) const override;
    QUrl messageImageUrlFromPortal(const QString &value, const QString &fileId, bool previewField,
                                  RasterRegistration registration = RasterRegistration::Primary) const;
    QUrl taskImageUrlFromPortal(const QString &value, const QString &attachmentId, const QString &objectId,
                               bool original = false, RasterRegistration registration = RasterRegistration::Primary) const;
    struct RasterRegistrations {
        QSet<QUrl> urls;
        QQueue<QUrl> order;
    };
    struct RasterAssets {
        RasterAssets(qsizetype maxBytes, qsizetype concurrentRequests, qsizetype cacheBytes)
            : maxBytes(maxBytes), concurrentRequests(concurrentRequests), cache(cacheBytes) {}

        qsizetype maxBytes;
        qsizetype concurrentRequests;
        mutable RasterRegistrations registrations;
        QQueue<QUrl> queue;
        QSet<QUrl> pending;
        QHash<QUrl, QNetworkReply *> replies;
        QCache<QUrl, QByteArray> cache;
    };
    using RasterLoaded = void (Backend::*)(const QUrl &, const QByteArray &);
    QUrl registerRasterAsset(const QUrl &url, const RasterAssets &assets,
                             RasterRegistration registration = RasterRegistration::Primary) const;
    void requestRasterAsset(const QUrl &url, RasterAssets &assets, RasterLoaded loaded);
    void startRasterAssets(RasterAssets &assets, RasterLoaded loaded);
    void startRasterAsset(const QUrl &key, const QUrl &url, RasterAssets &assets, RasterLoaded loaded);
    void clearRasterAssets();
    RasterAssets m_avatarAssets{128 * 1024, 4, 1024 * 1024};
    RasterAssets m_messageImageAssets{512 * 1024, 2, 2 * 1024 * 1024};
    mutable RasterRegistrations m_standbyMessageImageRegistrations;

    // Chat lookup, message attachments and the portal uploader.
    struct Upload;
    using ResolvedDialog = std::function<void(const QString &dialogId, qint64 chatId)>;
    void loadChatAssets(qint64 chatId, ChatAssetKind kind, qint64 boundary, const QString &operation,
                        std::function<void(QVector<ChatAsset>, QString)> complete);
    void resolveDialog(const QString &scopeId, bool task, const QString &operation,
                       const QString &requestKey, ResolvedDialog onDialog) override;
    void rememberDialog(const QString &dialogId, const QJsonObject &payload, bool rememberCounter = true) override;
    void rememberChatContact(const QString &dialogId, const QJsonObject &user, bool publish) override;
    void rememberDialogContact(const QString &dialogId, const QJsonObject &payload) override;
    void applyChatContact(Chat &chat) const override;
    void handleOnlinePull(const QJsonObject &params);
    struct ChatContact {
        QDateTime onlineUntil;
        QString position;
    };
    QHash<QString, ChatContact> m_chatContacts;
    MessageFile decodeChatFile(const QString &id, const QJsonObject &file) const;
    QVector<Message> decodeMessages(const QJsonObject &data, bool sortChronologically, const QString &dialog = {}) const override;
    mutable QCache<QString, bool> m_messageViewerMembership{tunables::kPageSizeChats};
    mutable QCache<QString, qint64> m_messageViewerCounts{4 * tunables::kPageSizeMessages};
    struct MessageFileSource {
        qint64 size;
        bool photo;
        QString videoObjectId;
        QString videoAttachmentId;
        quint64 accountRevision;
    };
    QUrl registerMessageFileUrl(const QString &value, qint64 size, bool photo,
                                const QString &videoObjectId = {}, const QString &videoAttachmentId = {}) const;
    enum class MessageDownloadMode { File, Photo, Video };
    struct MessageDownload;
    using MediaDownloads = QHash<QString, std::shared_ptr<MessageDownload>>;
    MediaDownloads &mediaDownloads(MessageDownloadMode mode);
    void cancelMessageMedia(MessageDownloadMode mode, const QString &targetPath);
    void beginMessageDownload(const QUrl &url, const QString &targetPath, MessageDownloadMode mode);
    void failMessageDownload(const QString &targetPath, const QString &reason, MessageDownloadMode mode);
    void streamMessageFile(const QUrl &url, const std::shared_ptr<MessageDownload> &download);
    mutable QHash<QUrl, MessageFileSource> m_registeredMessageFileUrls;
    QSet<QString> m_downloadTargets;
    MediaDownloads m_photoDownloads;
    MediaDownloads m_videoDownloads;
    void uploadChatAttachment(const QString &scopeId, const QString &localPath, bool task, bool image,
                              const QString &caption, std::shared_ptr<QTemporaryDir> temporaryDirectory = {});
    std::shared_ptr<Upload> openUpload(const QString &scopeId, const QString &localPath,
                                       const QString &kind, const QString &operation);
    void uploadChunk(const std::shared_ptr<Upload> &upload, qint64 offset);
    void commitUpload(const std::shared_ptr<Upload> &upload);
    void reconcileHistoryPopups(const QString &dialog, qint64 chatId, bool task, const QJsonObject &history,
                                const QVector<Message> &messages) override;
    Session m_session;
    bool m_allowLoopback;
    bool m_started = false;
    bool m_online = false;
    bool m_resumed = false;
    bool m_authExpired = false;
    quint64 m_sessionRevision = 0;
    signin::SessionRestore *m_sessionConfirmation = nullptr;
    quint64 m_sessionConfirmationRevision = 0;
    std::function<void(bool)> m_sessionConfirmationCallback;
    bool m_foreground = true;
    bool m_workspaceVisible = false;
    int m_activeSection = 0;
    bool m_notificationsDirty = false;
    QNetworkAccessManager m_http;
    PortalHttp m_portalHttp;
    ProjectsService m_projects;
    TasksService m_tasks;
    ChatTimeline m_timeline;
    ChatsService m_chats;
    NotificationsService m_notices;
    PullChannel m_pull;
    QTimer m_refresh;
    QTimer m_historyCatchup;
    QTimer m_sessionKeepalive;
    struct ChatCounterBadge { int unread = 0; bool marked = false; int messageCount = 0; };
    struct ChatBadgeAnchor {
        qint64 message = 0;
        qint64 actor = 0;
        bool reaction = false;
        quint64 revision = 0;
        bool operator==(const ChatBadgeAnchor &other) const {
            return message == other.message && actor == other.actor && reaction == other.reaction;
        }
    };
    struct PendingAnchorEvent { QString command; QJsonObject params; quint64 revision; };
    void loadChatBadges() override;
    void applyChatCounterSnapshot(const QJsonArray &rows, quint64 revision);
    void publishUnreadChatCount() override;
    void resetChatCounters();
    void decorateProjectTaskChat(Chat &chat) const override;
    qint64 badgeChatId(const QString &dialog) const override;
    void decorateChatBadges(Chat &chat) const override;
    bool updateAnchors(const QString &command, const QJsonObject &params, quint64 revision);
    void notifyAnchorAvailable(qint64 chatId = 0);
    bool handleAnchorPull(const QString &command, const QJsonObject &params);
    void republishChatBadges(qint64 changedChatId = 0) override;
    void readVisibleChat(const QString &dialog, const QStringList &incoming, const QStringList &visible,
                         const QString &task = {});
    void applyReadHistory(const QString &dialog, const QJsonObject &receipt, const QStringList &submitted = {});
    void readVisibleAnchors(qint64 chatId, const QStringList &visible);
    quint64 m_anchorRevision = 0;
    bool m_badgePagePending = false;
    QElapsedTimer m_chatBadgeRefresh;
    bool m_badgePageOvertaken = false;
    qsizetype m_chatAnchorCount = 0;
    bool m_haveCounterSnapshot = false;
    quint64 m_accountScopeRevision = 0;
    qint64 m_chatCounterSnapshotTotal = 0;
    std::optional<qint64> m_unreadChatCount;
    QHash<qint64, ChatCounterBadge> m_chatCounterBadges;
    QHash<qint64, QVector<ChatBadgeAnchor>> m_chatAnchors;
    QVector<PendingAnchorEvent> m_pendingAnchorEvents;
    QString m_userId;

};

} // namespace bx
