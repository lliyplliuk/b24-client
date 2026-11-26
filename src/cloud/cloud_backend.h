#pragma once

#include "cloud/portal_http.h"
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
#include <QWebSocket>
#include <functional>
#include <memory>

class QNetworkReply;
class QUrlQuery;
class TestCloudBackend;

namespace bx {

namespace signin { class SessionRestore; }

class CloudBackend final : public Backend, private PortalSession {
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
    friend class ::TestCloudBackend;

    const Session &currentSession() const override { return m_session; }
    quint64 sessionRevision() const override { return m_sessionRevision; }
    quint64 accountScopeRevision() const override { return m_accountScopeRevision; }
    bool started() const override { return m_started; }
    bool authExpired() const override { return m_authExpired; }
    bool allowLoopback() const override { return m_allowLoopback; }
    void confirmPortalSession(quint64 revision) override { confirmSession(revision); }
    void reconnectPortal() override { scheduleReconnect(); }
    void retryHistory(const QString &key, const QString &dialog, bool task) override { loadMessages(key, dialog, task, true); }
    void applyRenewedSession(const Session &renewed) override;
    void portalRequestFailed(const QString &operation, const QString &detail) override { emit failed(operation, detail); }
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
    void withCurrentUser(const QString &operation, std::function<void(qint64)> onUser);
    void requestCurrentUser(const QString &operation, std::function<void(const QString &)> onUser,
                            const std::shared_ptr<Session> &candidate = {});
    void resetAccountScopedState();
    void rememberCurrentUser(const QString &id);
    Fields boardFields(const QString &projectId) const;
    Task boardTask(const QJsonObject &item, const QString &id, const QString &projectId,
                   const QString &stageId, const QString &stageName) const;
    void boardMutation(const QString &action, const QString &projectId, Fields fields,
                       Handler onData = {});
    void updateTaskFields(const QString &taskId, QQueue<QPair<QString, QJsonObject>> writes,
                          const QString &requestKey = {});
    void updateTaskRequisite(const QString &taskId, const TaskEdit &edit);
    void loadTask(const QString &taskId, const QString &operation,
                  std::function<void(QJsonObject)> onTask, const QString &requestKey = {}, bool remember = true);
    void finishTaskCardRead(const QString &taskId);
    void loadPerson(const QString &id, const QString &operation, PersonHandler onPerson, bool requireName = false);
    Project projectFromApiRow(const QJsonObject &row) const;
    void loadLinkProject(const QString &id, const QString &operation,
                         std::function<void(Project, QString)> complete);
    using ProjectStagesHandler = std::function<void(std::optional<QVector<Stage>>)>;
    void loadProjectStages(const QString &projectId, const QString &operation, ProjectStagesHandler complete);
    void loadLinkStage(const QString &projectId, const QString &stageId, std::function<void(Stage)> complete);
    QUrl taskRoleAvatar(const QJsonObject &current, const QJsonObject &legacy, const QString &personId) const;
    struct LinkStageSnapshot {
        QVector<Stage> stages;
        qint64 expiresAt = 0;
    };
    QCache<QString, LinkStageSnapshot> m_linkStageSnapshots{tunables::kPageSizeProjects};
    QHash<QString, QVector<std::function<void(const QVector<Stage> &)>>> m_pendingLinkStages;
    void createTaskRequest(const QString &projectId, const QString &parentId, const TaskEdit &edit);
    void loadTaskChecklist(const QString &taskId, const QString &operation,
                           std::function<void(QJsonArray, QJsonValue)> handler);
    void updateTaskChecklist(const QString &taskId, const QString &action, QJsonObject payload);
    void loadProjectTimeline(const QString &projectId, int offset, QVector<Task> tasks);
    void loadMessages(const QString &key, const QString &dialogId, bool task, bool retry = false);
    void resumeHistoriesWithIdentity();
    void loadRecentChats(bool task, bool badgesOnly = false);
    void publishRecentChats(QVector<Chat> chats, bool task);
    void rememberRecentHistoryPreview(const QString &dialog, const QJsonObject &history);
    void reconcileHistoryUnread(const QString &dialog, const QJsonObject &chat, const QJsonArray &messages,
                                qint64 expectedChatId, quint64 revision);
    void applyRecentPull(const QString &command, const QString &dialog, const QString &chatId,
                         const QJsonObject &params, const QJsonObject &extra);
    bool acceptsRecentRead(const QString &dialog, const QJsonObject &params, const QJsonObject &extra) const;
    void hydrateRecentUserStatuses(QVector<Chat> chats, const QStringList &ids, quint64 generation);
    void rememberDismissalStatus(const QString &id, std::optional<bool> dismissed);
    std::optional<bool> cachedDismissalStatus(const QString &id) const;
    void resolveTaskChatTitles(QVector<Chat> chats, const QString &key, std::function<void(QVector<Chat>)> deliver);
    void rememberTaskTitle(const QString &id, const QString &title);
    QString recentTitle(const QString &dialog, const QString &fallback) const;
    Chat chatFromMetadata(const QString &dialog, const QJsonObject &row, const QJsonObject &chat,
                          const QJsonObject &user);
    QString projectPinSettingsKey() const;
    void fetchPortalHtml(const QUrl &url, std::function<void(QString)> handler);
    void loadProjectGrid(const QUrl &url, bool discovery);
    void loadProjectFallback(qint64 offset);
    void publishProjectPage(QVector<Project> rows);
    void loadProjectDiskItems(const QString &projectId, const QString &folderId, int offset);
    struct ProjectDisk {
        QString id;
        QString root;
        QSet<QString> folders;
    };
    QHash<QString, ProjectDisk> m_projectDisks;
    QVector<Project> m_loadedProjects;
    QUrl m_nextProjectGridPage;
    qint64 m_nextProjectOffset = -1;
    bool m_loadingProjects = false;
    bool m_taskProjectScope = false;
    int m_emptyProjectPages = 0;
    void sendMessage(const QString &key, const QString &dialogId, const QString &text, bool task,
                     const QString &replyId = {});
    void sendTaskMessage(const QString &taskId, const QString &text, const QString &replyId);
    void refreshChangedChat(const QString &dialog, bool pins = false);
    QHash<QString, qint64> m_lastTypingRequest;
    void connectPull();
    void receivePull(const QString &frame);
    void handlePullEvent(const QJsonObject &event);
    void loadChatPopupSettings();
    enum class ChatPopupSettingsSource { Push, InitialSnapshot, RefreshSnapshot };
    void applyChatPopupSettings(const QJsonObject &settings, ChatPopupSettingsSource source);
    void requestNotificationSettings(const QString &operation, const QString &updated = {}, int expected = -1,
                                     std::optional<quint64> started = {});
    void readPortalStatus(const QString &operation, std::function<void(const QString &, bool)> onStatus);
    void loadInitialPortalStatus();
    void settleInitialPortalStatus();
    void handlePortalStatusPush(const QJsonObject &users, qint64 stamp);
    void bindPortalStatusIdentity(bool accountChanged);
    struct PendingPortalStatus { QString status; qint64 stamp = 0; };
    QHash<QString, PendingPortalStatus> m_unboundPortalStatuses;
    bool m_initialPortalStatusPending = false;
    QQueue<Notification> m_pendingInitialAlerts;
    void rememberPortalStatus(const QString &status, bool notifyUnchanged = false);
    QString m_portalStatus;
    quint64 m_portalStatusRevision = 0;
    qint64 m_lastPortalStatusPushTime = 0;
    std::optional<bool> chatPopupsEnabled() const;
    void deliverChatPopups();
    void scheduleReconnect(bool immediately = false);
    void invalidateActiveView();
    void updateHistoryCatchup(bool restart = false);
    bool canAutoRefreshChats() const { return m_foreground || m_workspaceVisible; }
    void updateChatViewActivity(bool wasWatching);
    void rejectSession() override;
    void confirmSession(quint64 revision, std::function<void(bool)> onConfirmed = {});
    void cancelSessionConfirmation();
    bool refreshCsrf(Session candidate, quint64 revision) override;
    void scheduleRefresh() override;
    void refreshDirtyViews();
    void invalidateTaskChat(const QString &taskId);
    void setOnline(bool online);
    void failShape(const QString &operation);

    enum class RasterRegistration { Primary, Standby };
    QUrl avatarUrlFromPortal(const QString &value) const;
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
                       const QString &requestKey, ResolvedDialog onDialog);
    void rememberDialog(const QString &dialogId, const QJsonObject &payload, bool rememberCounter = true);
    void rememberChatContact(const QString &dialogId, const QJsonObject &user, bool publish);
    void rememberDialogContact(const QString &dialogId, const QJsonObject &payload);
    void applyChatContact(Chat &chat) const;
    void handleOnlinePull(const QJsonObject &params);
    struct ChatContact {
        QDateTime onlineUntil;
        QString position;
    };
    QHash<QString, ChatContact> m_chatContacts;
    QVector<Message> decodePinnedMessages(const QJsonObject &payload) const;
    void acceptPinnedSnapshot(const QString &dialog, const QJsonObject &payload);
    void handlePinnedPull(const QString &command, const QJsonObject &params);
    void publishPinnedMessages();
    QString m_pinnedDialog;
    quint64 m_pinnedGeneration = 0;
    bool m_havePinnedSnapshot = false;
    QVector<Message> m_pinnedMessages;
    QHash<QString, std::optional<Message>> m_pinnedChanges;
    MessageFile decodeChatFile(const QString &id, const QJsonObject &file) const;
    QVector<Message> decodeMessages(const QJsonObject &data, bool sortChronologically, const QString &dialog = {}) const;
    mutable QCache<QString, bool> m_messageViewerMembership{tunables::kPageSizeChats};
    mutable QCache<QString, qint64> m_messageViewerCounts{4 * tunables::kPageSizeMessages};
    struct ReplyPreview { QString author; QString text; QVector<MessageFile> files; };
    static QString replyPreviewKey(const QString &dialog, const QString &message);
    mutable QCache<QString, ReplyPreview> m_replyPreviews{128};
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
    void loadTaskAttachments(Task task, const QJsonObject &row);
    void uploadChatAttachment(const QString &scopeId, const QString &localPath, bool task, bool image,
                              const QString &caption, std::shared_ptr<QTemporaryDir> temporaryDirectory = {});
    std::shared_ptr<Upload> openUpload(const QString &scopeId, const QString &localPath,
                                       const QString &kind, const QString &operation);
    void uploadChunk(const std::shared_ptr<Upload> &upload, qint64 offset);
    void commitUpload(const std::shared_ptr<Upload> &upload);
    QHash<QString, qint64> m_dialogChatIds;
    struct TaskCardRead {
        QJsonObject base;
        QJsonObject details;
        bool baseFinished = false;
        bool detailsFinished = false;
        bool refresh = false;
    };
    QHash<QString, TaskCardRead> m_taskCardReads;
    QHash<QString, quint64> m_taskAttachmentRevisions;
    quint64 m_taskAttachmentRevision = 0;

    struct DialogMessageState {
        qint64 lastReadId = -1;
        qint64 confirmedReadThrough = 0;
        quint64 historyGeneration = 0;
        quint64 readReceiptRevision = 0;
        std::optional<int> unreadCounter;
        std::optional<bool> muted;
        bool muteFromPull = false;
        QJsonValue muteList;
        QSet<qint64> exactlyRead;
        QSet<qint64> historyUnread;
        QSet<qint64> seenByOthers;
        QStringList pendingReadIds;
        QString pendingReadTask;
    };
    struct ChatPopup {
        Notification entry;
        QString dialog;
        QString author;
        QJsonValue muteList;
        bool fromHistory = false;
    };
    struct ChatPopupHistory {
        QString dialog;
        std::optional<qint64> through;
    };
    void reconcileHistoryPopups(const QString &dialog, qint64 chatId, bool task, const QJsonObject &history,
                                const QVector<Message> &messages);
    bool rememberChatPopup(const QString &dialog, const QString &noticeId);
    void queueChatPopup(ChatPopup popup);
    DialogMessageState &dialogMessageState(const QString &dialog);
    bool chatMuted(const ChatPopup &popup) const;
    QHash<QString, DialogMessageState> m_dialogMessageStates;
    QJsonObject m_chatPopupSettings;
    QVariantMap m_notificationSettings;
    QHash<QString, QPair<quint64, QJsonValue>> m_notificationSettingChanges;
    quint64 m_notificationSettingsRevision = 0;
    QQueue<ChatPopup> m_pendingChatPopups;
    QCache<QString, bool> m_seenChatPopups{256};
    ChatPopupHistory m_chatPopupHistory;
    ChatPopupHistory m_taskPopupHistory;
    qint64 m_nextChatPopupSettingsRequest = 0;
    quint64 m_chatPopupSettingsGeneration = 0;
    bool m_chatPopupSettingsKnown = false;

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
    bool m_recentDirty = false;
    bool m_chatDirty = false;
    bool m_taskMessagesDirty = false;
    bool m_boardDirty = false;
    bool m_notificationsDirty = false;
    int m_attempt = 0;
    QNetworkAccessManager m_http;
    PortalHttp m_portalHttp;
    QWebSocket m_socket;
    QTimer m_retry;
    QTimer m_watchdog;
    QTimer m_heartbeat;
    QTimer m_refresh;
    QTimer m_configExpiry;
    QTimer m_historyCatchup;
    QTimer m_sessionKeepalive;
    QHash<QString, QString> m_taskDialogs;
    QCache<QString, QString> m_recentTitles{2 * tunables::kPageSizeChats};
    QCache<QString, QString> m_taskChatTitles{2 * tunables::kPageSizeChats};
    QVector<Chat> m_legacyTaskChats;
    QVector<Chat> m_recentTaskChatItems;
    QVector<Chat> m_recentChatItems;
    struct RecentOverlay {
        Chat preview;
        struct HistoryPreview {
            qint64 chatId = 0;
            qint64 messageId = 0;
            QString text;
            QDateTime updated;
        } historyPreview;
        qint64 rejectedRecentPreviewChatId = 0;
        qint64 rejectedTaskPreviewChatId = 0;
        qint64 chatId = 0;
        qint64 messageId = 0;
        qint64 incomingId = 0;
        qint64 eventTime = 0;
        qint64 confirmedCounterCursor = 0;
        quint64 counterRevision = 0;
        std::optional<int> counter;
        std::optional<bool> markedUnread;
    };
    RecentOverlay &recentOverlay(const QString &dialog);
    QHash<QString, RecentOverlay> m_recentOverlays;
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
    void loadChatBadges();
    void applyChatCounterSnapshot(const QJsonArray &rows, quint64 revision);
    void publishUnreadChatCount();
    QHash<qint64, int> projectUnreadCandidates() const;
    void publishProjectUnreadCounts(bool force = false);
    void resolveProjectUnreadChats(const QHash<qint64, int> &candidates);
    void resolveProjectUnreadTasks(const QHash<qint64, qint64> &tasks);
    void rememberProjectUnreadTask(qint64 chat, qint64 task, qint64 project);
    void rememberProjectUnreadTaskChat(const Chat &chat);
    void updateProjectUnreadTask(qint64 task, qint64 chat, qint64 project);
    void resetChatCounters();
    void resetProjectTaskChats();
    void decorateProjectTaskChat(Chat &chat) const;
    void publishProjectTaskChatBadges(qint64 chatId = 0);
    qint64 badgeChatId(const QString &dialog) const;
    void decorateChatBadges(Chat &chat) const;
    bool updateAnchors(const QString &command, const QJsonObject &params, quint64 revision);
    void notifyAnchorAvailable(qint64 chatId = 0);
    bool handleAnchorPull(const QString &command, const QJsonObject &params);
    void republishChatBadges(qint64 changedChatId = 0);
    void readVisibleChat(const QString &dialog, const QStringList &incoming, const QStringList &visible,
                         const QString &task = {});
    static std::optional<int> unreadCount(const QJsonValue &value);
    void applyReadHistory(const QString &dialog, const QJsonObject &receipt, const QStringList &submitted = {});
    void rememberUnreadHistoryRows(const QString &dialog, qint64 chatId, const QJsonArray &rows, quint64 revision);
    void readVisibleAnchors(qint64 chatId, const QStringList &visible);
    quint64 m_anchorRevision = 0;
    bool m_badgePagePending = false;
    QElapsedTimer m_chatBadgeRefresh;
    bool m_badgePageOvertaken = false;
    qsizetype m_chatAnchorCount = 0;
    bool m_haveCounterSnapshot = false;
    quint64 m_chatBadgeRevision = 0;
    quint64 m_chatCounterSnapshotRevision = 0;
    quint64 m_accountScopeRevision = 0;
    qint64 m_chatCounterSnapshotTotal = 0;
    std::optional<qint64> m_unreadChatCount;
    QHash<qint64, ChatCounterBadge> m_chatCounterBadges;
    QHash<qint64, qint64> m_projectUnreadParentsByChat;
    QSet<qint64> m_projectUnreadNonTaskChats;
    struct ProjectUnreadTask { qint64 task = 0; qint64 project = -1; };
    QHash<qint64, ProjectUnreadTask> m_projectUnreadTasksByChat;
    QHash<qint64, qint64> m_projectUnreadGroupsByParent;
    QSet<qint64> m_projectUnreadParentAttempts;
    QSet<qint64> m_projectUnreadTaskChatAttempts;
    QHash<qint64, qint64> m_projectUnreadMovedTasks;
    QSet<qint64> m_projectUnreadMoveAttempts;
    QHash<QString, qint64> m_projectUnreadCounts;
    bool m_projectUnreadComplete = false;
    bool m_projectUnreadPending = false;
    quint64 m_projectUnreadBindingRevision = 0;
    QString m_projectTaskChatsProject;
    QString m_projectTaskChatsRequest;
    quint64 m_projectTaskChatsSerial = 0;
    int m_projectTaskChatsNext = -1;
    QSet<QString> m_projectTaskIds;
    QSet<qint64> m_projectTaskChatIds;
    QHash<qint64, QVector<ChatBadgeAnchor>> m_chatAnchors;
    QVector<PendingAnchorEvent> m_pendingAnchorEvents;
    struct PendingRecentPull { QString command; QString dialog; QString chatId; QJsonObject params; QJsonObject extra; };
    QVector<PendingRecentPull> m_recentPullsAwaitingIdentity;
    bool m_showTaskChats = false;
    struct PersonalChatStatus { std::optional<bool> dismissed; qint64 expiresAt; };
    QCache<QString, PersonalChatStatus> m_personalChatStatuses{2 * tunables::kPageSizeChats};
    quint64 m_recentStatusGeneration = 0;
    quint64 m_taskTitleGeneration = 0;
    struct HistoryRequest { QString key; QString dialog; bool task; };
    QVector<HistoryRequest> m_historiesWaitingForIdentity;
    bool m_triedHistoryIdentity = false;
    void notificationBatch(const QString &method, const QUrlQuery &parameters, const QString &operation,
                           std::function<void(QJsonValue)> onResult, const QString &requestKey = {});
    void resetNoticeState();
    struct NotificationCursor {
        qint64 id = 0;
        int type = 0;
    };
    static NotificationCursor notificationCursor(const QJsonArray &rows);
    void publishNotificationPage(QVector<Notification> items);
    void publishNotificationHistoryPageState();
    QVector<Notification> decodeNotifications(const QJsonArray &rows, const QJsonValue &users, quint64 requestedAt) const;
    QString notificationTaskId(const QString &dialog, const QJsonObject &eventChat) const;
    void updateNotificationReadState(const QStringList &ids, bool read, std::optional<quint64> started = {});
    void removeNotification(const QString &id);
    struct NotificationMutation {
        quint64 revision;
        bool read;
        bool removed;
    };
    QVector<Notification> m_notifications;
    QVector<Notification> m_notificationSearchRows;
    QString m_notificationSearchQuery;
    QHash<QString, NotificationMutation> m_notificationMutations;
    quint64 m_notificationRevision = 0;
    NotificationCursor m_notificationOlderCursor;
    int m_notificationOlderPages = 0;
    bool m_loadingOlderNotifications = false;
    QQueue<QString> m_seenEvents;
    QSet<QString> m_seenEventIds;
    QQueue<QString> m_sentTemplates;
    QString m_mid;
    QString m_tag;
    QString m_time;
    QString m_openDialog;
    QString m_openTask;
    QString m_openBoard;
    struct BoardColumnCursor {
        QString stageName;
        QString stageColor;
        std::optional<qsizetype> total;
        QSet<QString> seen;
        QJsonArray deferred;
        qsizetype deferredOffset = 0;
        int nextPage = 2;
        bool serverExhausted = false;

        bool hasMore() const { return !deferred.isEmpty() || (!serverExhausted && total && *total > seen.size()); }
    };
    void publishDeferredBoardTasks(const QString &projectId, const QString &stageId, BoardColumnCursor &cursor);
    QHash<QString, BoardColumnCursor> m_boardColumnCursors;
    quint64 m_boardPageGeneration = 0;
    QString m_userId;

};

} // namespace bx
