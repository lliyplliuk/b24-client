#pragma once

#include "cloud/portal_http.h"
#include "core/models.h"
#include "core/tunables.h"

#include <QCache>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

namespace bx {

class ProjectsHost : public virtual SessionStateHost {
public:
    virtual QUrl portalOrigin() const = 0;
    virtual QString settingsScope() const = 0;
    virtual QString currentUserId() const = 0;
    virtual void withCurrentUser(const QString &operation, std::function<void(qint64)> onUser) = 0;
    virtual void failShape(const QString &operation) = 0;
    virtual QUrl avatarUrlFromPortal(const QString &value) const = 0;
    virtual QUrl projectFileUrl(const QString &value, qint64 size) const = 0;
    virtual bool projectsVisible() const = 0;
    virtual bool hasCounterSnapshot() const = 0;
    virtual QHash<qint64, int> chatMessageCounters() const = 0;
    virtual void decorateProjectTaskChat(Chat &chat) const = 0;
    virtual void loadChatBadges() = 0;
};

class ProjectsService final : public QObject {
    Q_OBJECT

public:
    ProjectsService(ProjectsHost &host, PortalHttp &http, QObject *parent) : QObject(parent), m_host(host), m_portalHttp(http) {}
    // The backend publishes cleared unread counts after resetting its chat counter snapshot.
    [[nodiscard]] bool reset();
    void invalidateOpenBoard() { m_boardDirty = !m_openBoard.isEmpty(); }
    void handleTaskPull(const QJsonObject &fields, const QString &groupId, const QString &taskId, qint64 remembered);
    bool boardDirty() const { return m_boardDirty; }
    void refreshDirtyBoard();
    bool isCurrentResponse(OperationKind kind, const QString &key, const QString &operation) const;

    void requestProjects();
    void requestMoreProjects();
    void requestProjectMembers(const QString &projectId);
    void requestProjectFiles(const QString &projectId, const QString &folderId, int offset);
    void setProjectPinned(const QString &projectId, bool pinned);
    void createProject(const QString &name);
    void searchProjectTasks(const QString &projectId, const QString &query);
    void requestProjectBoard(const QString &projectId);
    void requestMoreBoardTasks(const QString &projectId, const QString &stageId);
    void requestProjectStages(const QString &projectId);
    void loadLinkStage(const QString &projectId, const QString &stageId, std::function<void(Stage)> complete);
    void moveTask(const QString &projectId, const QString &taskId, const QString &stageId);
    void saveStage(const QString &projectId, const QString &stageId, const QString &title,
                   const QString &afterStageId, const QString &color = {});
    void deleteStage(const QString &projectId, const QString &stageId);
    void loadLinkProject(const QString &id, const QString &operation,
                         std::function<void(Project, QString)> complete);
    void requestProjectTimeline(const QString &projectId);
    void requestProjectTaskChats(const QString &projectId, int offset = 0);

    void setChatClassification(QHash<qint64, qint64> taskParents, QSet<qint64> nonTaskChats);
    void refreshUnreadCounts();
    void publishProjectUnreadCounts(bool force = false);
    void rememberProjectUnreadTask(qint64 chat, qint64 task, qint64 project);
    void rememberProjectUnreadTaskChat(const Chat &chat);
    void updateProjectUnreadTask(qint64 task, qint64 chat, qint64 project);
    void publishProjectTaskChatBadges(qint64 chatId = 0);
    bool containsChat(qint64 chat) const;
    quint64 unreadBindingRevision() const { return m_projectUnreadBindingRevision; }

signals:
    void boardLoaded(const QString &projectId, const QVector<Stage> &stages);
    void boardTasksPageLoaded(const QString &projectId, const QString &stageId,
                              const QVector<Task> &tasks, bool hasMore);
    void boardUpdated(const QString &projectId);
    void failed(const QString &operation, const QString &reason);
    void projectCreated(const QString &projectId);
    void projectFilesLoaded(const QString &projectId, const QString &folderId, const QVector<ProjectDiskItem> &files,
                            int nextOffset);
    void projectMembersLoaded(const QString &projectId, const QVector<ProjectMember> &members);
    void projectPinChanged(const QString &projectId, bool pinned);
    void projectStagesLoaded(const QString &projectId, const QVector<Stage> &stages);
    void projectTaskChatBadgesChanged(const QString &projectId, const QVector<Chat> &chats);
    void projectTaskChatsLoaded(const QString &projectId, int offset, const QVector<Chat> &chats, int nextOffset);
    void projectTasksFound(const QString &projectId, const QString &query, const QVector<Task> &tasks);
    void projectTimelineLoaded(const QString &projectId, const QVector<Task> &tasks, bool limited);
    void projectUnreadCountsChanged(const QHash<QString, qint64> &counts, bool complete);
    void projectsLoaded(const QVector<Project> &projects);
    void projectsMoreAvailable(bool available);
    void projectsScopeLoaded(bool membershipVerified);

private:
    void invalidateBoardForTask(const QString &groupId, const QString &taskId);
    bool hasTaskParent(qint64 chat) const { return m_projectUnreadParentsByChat.contains(chat); }
    ProjectsHost &m_host;
    PortalHttp &m_portalHttp;
    using Fields = PortalHttp::Fields;
    using Handler = PortalHttp::Handler;
    using ProjectStagesHandler = std::function<void(std::optional<QVector<Stage>>)>;

    struct LinkStageSnapshot {
        QVector<Stage> stages;
        qint64 expiresAt = 0;
    };
    QCache<QString, LinkStageSnapshot> m_linkStageSnapshots{tunables::kPageSizeProjects};
    QHash<QString, QVector<std::function<void(const QVector<Stage> &)>>> m_pendingLinkStages;
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
    bool m_boardDirty = false;
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
    QHash<QString, BoardColumnCursor> m_boardColumnCursors;
    quint64 m_boardPageGeneration = 0;

    QString projectPinSettingsKey() const;
    void loadProjectGrid(const QUrl &url, bool discovery);
    void loadProjectFallback(qint64 offset);
    void publishProjectPage(QVector<Project> rows);
    void loadProjectDiskItems(const QString &projectId, const QString &folderId, int offset);
    Task boardTask(const QJsonObject &item, const QString &id, const QString &projectId,
                   const QString &stageId, const QString &stageName) const;
    void publishDeferredBoardTasks(const QString &projectId, const QString &stageId, BoardColumnCursor &cursor);
    void loadProjectStages(const QString &projectId, const QString &operation, ProjectStagesHandler complete);
    Fields boardFields(const QString &projectId) const;
    void boardMutation(const QString &action, const QString &projectId, Fields fields,
                       Handler onData = {});
    Project projectFromApiRow(const QJsonObject &row) const;
    void loadProjectTimeline(const QString &projectId, int offset, QVector<Task> tasks);
    void resetProjectTaskChats();
    void resolveProjectUnreadChats(const QHash<qint64, int> &candidates);
    void resolveProjectUnreadTasks(const QHash<qint64, qint64> &tasks);
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
};

} // namespace bx
