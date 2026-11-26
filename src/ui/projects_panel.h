#pragma once

#include "core/backend.h"
#include "core/operations.h"

#include <QPointer>
#include <QSet>
#include <QWidget>
#include <functional>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QStackedWidget;
class QTabBar;
class QTimer;

namespace bx {

class BoardWidget;
namespace ui { class AvatarBadge; class NameDialog; class ProjectToolsDialog; class StageManagerDialog; class TaskGanttView; }

class ProjectsPanel final : public QWidget {
    Q_OBJECT
public:
    struct Workspace {
        std::function<bool()> writable;
        std::function<bool()> clearTask;
        std::function<const QPixmap *(const QUrl &)> avatar;
    };
    ProjectsPanel(Backend *backend, Workspace workspace, QWidget *parent);
    const QString &currentProjectId() const { return m_currentProjectId; }
    const QVector<Stage> &stages() const { return m_stages; }
    const QVector<Project> &projectDirectory() const { return m_projectDirectory; }
    bool directoryLoaded() const { return m_projectDirectoryLoaded; }
    bool moreAvailable() const { return m_projectsMoreAvailable; }
    void resetAccount(bool previousAccount);
    void resetTaskChats();
    void updateColumnWidthContext(const QString &scope);
    void refreshBoardAvatars();
    void requestNextProjectsPage();

    void setSectionActive(bool active);
    void closeTools();
    void resumeSearch();
    void refresh(bool reloadProjectDirectory);
    bool handleFailure(const QString &operation, OperationKind kind, const QStringList &ids, const QString &reason, QString &footer);
    void setWriteAvailability(bool canWrite, bool creatingTask);
    bool boardEditPending() const { return !m_pendingBoardEdit.isEmpty(); }
    void cancelPendingMessageProject() { m_pendingMessageProject = {}; }
    Task cachedTask(const QString &taskId, const QString &projectId) const;
    QString currentProjectTitle() const;
    void refreshTasks(const QString &project, const QString &targetProject = {});
    void openLinkedProject(const NotificationTarget &target, const QUrl &messageUrl);
    void requestVisibleAvatars(const std::function<void(const QUrl &)> &requestAvatar, const std::function<void(QListWidget *)> &requestVisible) const;
    void updateAvatars(const std::function<void(QListWidget *)> &updateList);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

signals:
    void sectionRequested();
    void boardMoveFailed(const QString &taskId, const QString &reason);
    void directoryLoadFailed();
    void taskRequested(const QString &taskId);
    void projectTaskRequested(const QString &taskId, const QString &projectId);
    void taskCreationRequested();
    void refreshRequested();
    void avatarsRequested();
    void avatarRowsChanged();
    void writeStateChanged();
    void statusChanged(const QString &text, bool loading = false);
    void fileSaveRequested(const MessageFile &file, QWidget *source);
    void directoryChanged(const QVector<Project> &projects);
    void moreProjectsChanged(bool available);
    void formStagesLoaded(const QString &id, const QVector<Stage> &stages);
    void currentStagesLoaded(const QString &id, const QVector<Stage> &stages);
    void taskMoved(const QString &taskId);

private:
    void showProjectPinMenu(const QPoint &point);
    bool refreshProjectTaskChats(const QString &project);
    void requestCurrentProjectBoard();
    void connectBackend();
    void onProjectSelected();
    void onProjectCreated(const QString &projectId);
    void onBoardUpdated(const QString &projectId);
    bool canMoveBoardTask(const QString &taskId, const QString &sourceStage, const QString &targetStage) const;
    void runProjectTaskSearch();
    void onProjectTasksFound(const QString &projectId, const QString &query, const QVector<Task> &tasks);
    void displayProjectTaskResults();
    void onProjectScopeChanged(int scope);
    void ensureProjectTaskView();
    void openProjectTaskChats();
    void closeProjectTaskChats();
    void requestProjectTaskChatPage();
    void onProjectTaskChatsLoaded(const QString &project, int offset, const QVector<Chat> &rows, int nextOffset);
    void updateProjectTaskChatBadges(const QString &project, const QVector<Chat> &rows);
    void updateProjectTaskChatTab();
    void updateProjectTaskChatControls();
    void updateProjectUnreadRows();
    void scheduleProjectsPageCheck();
    void maybeLoadMoreProjects();
    void updateProjectsRailPagination();
    void showProjects();
    void openProjectTools();
    void showProjectTimeline(bool show);
    void createProject();
    void openStageManager();

    Backend *m_backend;
    Workspace m_workspace;
    bool m_active = false;
    QSet<QString> m_pendingPins;
    QListWidget *m_projects = nullptr;
    QVector<Project> m_projectDirectory;
    QHash<QString, qint64> m_projectUnreadCounts;
    bool m_projectUnreadCountsComplete = false;
    ui::AvatarBadge *m_projectAvatar = nullptr;
    QUrl m_projectAvatarUrl;
    QLabel *m_projectTitle = nullptr;
    QLabel *m_projectDirectoryTitle = nullptr;
    QTabBar *m_projectTabs = nullptr;
    QStackedWidget *m_projectViews = nullptr;
    QListWidget *m_projectTaskChats = nullptr;
    QLabel *m_projectTaskChatStatus = nullptr;
    QPushButton *m_moreProjectTaskChats = nullptr;
    bool m_projectTaskViewRequested = false;
    QString m_projectTaskChatScope;
    QString m_projectTaskChatError;
    QHash<QString, QListWidgetItem *> m_projectTaskChatRows;
    qint64 m_projectTaskChatUnread = 0;
    int m_projectTaskChatNext = 0;
    int m_projectTaskChatPending = -1;
    QLineEdit *m_filter = nullptr;
    QLabel *m_projectTaskSearchStatus = nullptr;
    QLineEdit *m_projectNameSearch = nullptr;
    QString m_projectNameQuery;
    bool m_projectDirectoryLoaded = false;
    bool m_projectsMoreAvailable = false;
    bool m_projectsPagePending = false;
    bool m_projectsPageFailed = false;
    bool m_projectsPageCheckQueued = false;
    QPushButton *m_kanbanButton = nullptr;
    QPushButton *m_listButton = nullptr;
    ui::TaskGanttView *m_projectTimeline = nullptr;
    bool m_ganttMode = false;
    QPushButton *m_newTaskButton = nullptr;
    QPushButton *m_newProjectButton = nullptr;
    QPushButton *m_moreProjectsButton = nullptr;
    QPushButton *m_projectToolsButton = nullptr;
    QPointer<ui::ProjectToolsDialog> m_projectTools;
    QPushButton *m_stagesButton = nullptr;
    QVector<Stage> m_stages;
    QSet<QString> m_loadedBoardTaskIds;
    ui::StageManagerDialog *m_stageManager = nullptr;
    QTimer *m_projectSearchTimer = nullptr;
    QHash<QString, QString> m_pendingProjectSearches;
    QVector<Task> m_projectSearchResults;
    bool m_projectSearchDisplayed = false;

    QString m_linkedProjectId;
    struct {
        NotificationTarget target;
        QUrl url;
    } m_pendingMessageProject;
    BoardWidget *m_board = nullptr;
    QString m_currentProjectId;
    QString m_pendingProjectId;
    ui::NameDialog *m_projectForm = nullptr;
    bool m_pendingCreateProject = false;
    QString m_pendingBoardEdit;
    QString m_pendingMoveTask;
    QString m_boardMoveRefreshProject;
};

} // namespace bx
