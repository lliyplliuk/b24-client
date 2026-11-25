#pragma once

#include "core/backend.h"
#include "ui/chat_image_previews.h"
#include "ui/chat_reply.h"
#include "ui/public_link_preview.h"

#include <QHash>
#include <QCache>
#include <QMainWindow>
#include <QPixmap>
#include <QPointer>
#include <QSet>
#include <QThreadPool>
#include <QVector>

#include <memory>

class QLabel;
class QCheckBox;
class QAbstractItemModel;
class QDialog;
class QCheckBox;
class QComboBox;
class QFileDialog;
class QLineEdit;
class QMenu;
class QPersistentModelIndex;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QTabBar;
class QPoint;
class QResizeEvent;
class QStackedWidget;
class QTimer;
class QTemporaryDir;

namespace bx {

class BoardWidget;
namespace ui { class TaskGanttView; }
namespace ui { class WorktimePanel; class ChatVideoPreviews; }
namespace ui { class ProjectToolsDialog; }
namespace ui { class AvatarBadge; class ChatComposer; class HistoryList; class PinnedMessageBar; class TaskCardDialog; class TaskEditForm; class NameDialog; class StageManagerDialog; class GlyphButton; }

class ClientUpdateFeed final : public QObject {
    Q_OBJECT
public:
    void publish(const QString &tag, const QUrl &downloadUrl);
    const Notification &notice() const { return m_notice; }
    void markRead();

signals:
    void changed();

private:
    Notification m_notice;
};

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    using PublicLinkRequest = std::function<bool(const QUrl &, ui::PublicLinkPreviewFetcher::Completion)>;
    explicit MainWindow(Backend *backend, bool readOnly = false, QWidget *parent = nullptr,
                        PublicLinkRequest publicLinkRequest = {});
    ~MainWindow() override;

    // Открытие карточки задачи (окно-модальный диалог).
    void openTaskCard(const QString &taskId);
    QString taskLinkFor(const QString &taskId) const;
    // Opens a clicked message link: own-portal task links open the native task card,
    // other safe http(s) links keep the browser behaviour.
    void openMessageLink(const QString &href, QWidget *source = nullptr);
    void setClientUpdateFeed(ClientUpdateFeed *feed);
    void setLogoutAvailable(bool available);
    void setSessionRecoveryAvailable(bool available);
    bool confirmWorkspaceClose();
    bool closeWorkspace();

signals:
    void activeSectionChanged(int section);
    void logoutRequested();
    void sessionRecoveryRequested();
    void historyClosed(bool task);

public slots:
    void setStatus(const QString &text, bool loading = false);
    void onConnectionChanged(bool online);

protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onSectionChanged(int index);
    void onChatSelected();
    void onSendChat();
    void onProjectSelected();
    void onRefresh();
    void onFailed(const QString &operation, const QString &reason);
    void onTaskUpdated(const QString &taskId);
    void confirmTaskDeletion(ui::TaskCardDialog *card);
    void onTaskDeleted(const QString &taskId);
    void onTaskCreated(const QString &projectId, const QString &taskId);
    void onProjectCreated(const QString &projectId);
    void onBoardUpdated(const QString &projectId);
    void onMessagesFound(const QString &scopeId, const QString &query, bool task,
                         const QVector<Message> &messages);
    void onFileUploaded(const QString &scopeId, const QString &kind);
    void onProjectTasksFound(const QString &projectId, const QString &query, const QVector<Task> &tasks);

private:
    QWidget *buildChatPanel();
    QWidget *buildProjectsPanel();
    QWidget *buildNotificationsPanel();
    void refreshCurrentSection(bool reloadProjectDirectory = true);
    void updateComposers();
    bool clearTask();
    void connectWorkspaceActions();
    void showProfileSettings();
    void reloadChatVisibilityPreference();
    void setShowDismissedChats(bool show);
    void runSearchChats();
    void runMessageSearch();
    void clearChatSearch();
    void clearMessageSearch();
    void showChatResults(const QVector<Chat> &chats, bool useSearchMode = true, bool backgroundRefresh = false);
    void openChatRow(const QListWidgetItem &item);
    void addMessageActions(QMenu &menu, const QPersistentModelIndex &message, const QString &dialog, bool taskChat,
                           const QString &href);
    bool beginMessageEdit(const QPersistentModelIndex &message, const QString &dialog);
    bool editLatestMessage(const ui::HistoryList *history, const QString &dialog);
    QWidget *messageActionOwner(const QAbstractItemModel *model, const QString &dialog);
    std::function<QPersistentModelIndex()> messageActionTarget(const QPersistentModelIndex &message,
                                                              const QString &dialog);
    QUrl messageLinkDestination(const QString &href) const;
    void actOnMessageReaction(const QPersistentModelIndex &message, const QString &dialog, const QString &code,
                              const QPoint &anchor, QWidget *parent);
    QVariantMap reactionFrequency() const;
    void incrementReactionFrequency(const QString &code);
    void recordConfirmedReaction(const QString &dialog, const QString &id, const QString &code);
    void showChatAssets(const QString &dialog, QWidget *parent);
    QPointer<QDialog> m_chatAssetsDialog;
    void promptGroupChat();
    void promptChatMember(const QString &dialog, bool add);
    void focusChatComposer();
    void scheduleVisibleChatRead();
    void reportVisibleChatRead();
    void updateChatContactPresentation();
    void openChatMessageContext(const QString &dialogId, const QString &messageId);
    void openPinnedTaskMessage(const QString &messageId);
    void onChatScopeChanged(int scope);
    void onChatListLoaded(int scope, const QVector<Chat> &chats);
    void onChatsFound(int scope, const QString &query, const QVector<Chat> &chats);
    void requestCurrentChatList();
    void updateChatUnreadBadges();
    void handleChatTyping(const QString &dialog, const QString &user, int duration);
    void rememberTypingName(const QString &user, const QString &name);
    void setChatTabUnreadCount(int scope, qint64 count);
    void updateNotificationRailBadge();
    void setRailUnreadCount(int row, qint64 count);
    void setRailStatus(int row, const QString &detail);
    void requestNotificationHistory();
    void updateNotificationPager();
    void requestRailWorkday();
    void updateRailWorkday();
    void showProjects();
    void scheduleProjectsPageCheck();
    void maybeLoadMoreProjects();
    void updateProjectsRailPagination();
    void requestNextProjectsPage();
    void updateProjectUnreadRows();
    void onProjectScopeChanged(int scope);
    void ensureProjectTaskView();
    void openProjectTaskChats();
    void closeProjectTaskChats();
    bool refreshProjectTaskChats(const QString &project);
    void requestProjectTaskChatPage();
    void updateProjectTaskChatControls();
    void updateProjectTaskChatTab();
    void onProjectTaskChatsLoaded(const QString &project, int offset, const QVector<Chat> &rows, int nextOffset);
    void updateProjectTaskChatBadges(const QString &project, const QVector<Chat> &rows);
    void showPinMenu(QListWidget *list, const QPoint &point, bool project);
    void scheduleAvatars();
    void loadVisibleAvatars();
    void requestAvatar(const QUrl &url);
    void loadVisibleChatPreviews();
    void refreshChatMediaRows();
    bool isCurrentTaskLink(const QListWidget *history, const QUrl &url) const;
    void requestExternalLinkPreviews(QListWidget *history, qint64 newest);
    bool needsPublicLinkPreview(QListWidget *history, const QString &id, const QUrl &url) const;
    void requestPublicLinkPreview(QListWidget *history, const QString &id, const QUrl &url);
    void addPublicLinkPreviewActions(QMenu &menu, const QPersistentModelIndex &message,
                                     const std::function<QPersistentModelIndex()> &resolve);
    void scheduleLinkPreviewLayout();
    void refreshLinkPreviewRows();
    void invalidateTaskLinkPreview(const QString &taskId);
    void resetLinkPreviews();
    void loadMessageLinkPreview(const QString &href);
    void updateAvatarRows();
    void openNotificationTarget(int row, int target);
    void runNotificationSearch();
    void resetNotificationAccount();
    void showNotifications(bool markVisible = true, bool preserveScroll = true);
    QVector<int> visibleNoticeRows() const;
    void scheduleVisibleNoticeRead();
    void readVisibleNotices();
    void finishVisibleNoticeRead(bool success);
    bool canReadVisibleNotices() const;
    void updateNotificationActions();
    void openNotificationChat(const NotificationTarget &target);
    void openTaskCardForProject(const QString &taskId, const QString &projectId);
    void openPerson(const QString &personId, QWidget *source = nullptr);
    void resetMessageContext();
    bool focusChatMessage(const QString &id);
    void bindMessageFiles(QListWidget *list, bool linkCards = false);
    void openMessageViewers(QListWidget *history, const QString &messageId, const QPoint &anchor);
    void openMessagePhoto(const MessageFile &file, QWidget *parent, const QVector<MessageFile> &loaded = {});
    void playMessageVideo(const MessageFile &file, QWidget *parent, std::function<bool()> stillVisible);
    void saveMessageFile(const MessageFile &file, QWidget *parent, const QImage &preview = {});
    void showMessageResults(const QVector<Message> &messages);
    void runProjectTaskSearch();
    void displayProjectTaskResults();
    void updateWriteAvailability();
    void onTaskSummaryLoaded(const Task &task);
    void updateTaskCardChatBinding(const QString &previousDialog);
    void bindTaskForm(ui::TaskEditForm *form, const QString &projectId, bool canDetachProject);
    void createTaskInProject();
    void createSubtask(ui::TaskCardDialog *card);
    bool beginTaskFormFiles(const QString &taskId, const QString &projectToOpen = {});
    bool uploadNextTaskFormFile();
    void showProjectTimeline(bool show);
    void openProjectTools();
    void createProject();
    void openStageManager();
    void pickFile(const QString &scope, const QString &kind);
    void uploadPastedChatImage(ui::ChatComposer *composer, const QString &scope, bool task, const QString &caption);
    bool pastedChatImagePending(const QString &scope, bool task) const;
    void clearPastedChatImages();
    void queueDescriptionFiles(const QString &taskId, const QStringList &paths);
    bool uploadNextDescriptionFile();
    void clearDescriptionUpload();
    void pasteDescriptionPhoto(const QString &taskId, const QImage &image);
    void requestMessageSearch(const QString &scope, const QString &query, bool task);
    bool canMoveBoardTask(const QString &taskId, const QString &sourceStage, const QString &targetStage) const;
    void requestCurrentProjectBoard();
    bool writable() const { return !m_readOnly && m_online; }
    void onPeopleFound(const QString &requestId, const QVector<Person> &people);

    Backend *m_backend;
    bool m_readOnly;
    bool m_online = false;
    bool m_closingWorkspace = false;
    BoardWidget *m_board = nullptr;

    QListWidget *m_sections = nullptr;
    QTimer *m_railWorkdayTimer = nullptr;
    bool m_railWorkdayCurrent = false;
    QStackedWidget *m_stack = nullptr;
    ui::WorktimePanel *m_worktime = nullptr;
    QLabel *m_mode = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_logout = nullptr;
    QWidget *m_sessionRecovery = nullptr;
    QDialog *m_profileSettings = nullptr;
    QCheckBox *m_dismissedChatsCheck = nullptr;
    QLabel *m_chatSettingsAccountHint = nullptr;
    bool m_showDismissedChats = false;

    QListWidget *m_chats = nullptr;
    QTabBar *m_chatTabs = nullptr;
    QListWidget *m_chatMessages = nullptr;
    QLabel *m_chatTitle = nullptr;
    QLabel *m_chatContactInfo = nullptr;
    QDateTime m_chatContactUntil;
    QString m_chatContactPosition;
    bool m_chatContactIsPerson = false;
    QCache<QString, QString> m_typingNames{64};
    QSet<QString> m_typingNamesPending;
    QTimer *m_chatContactTimer = nullptr;
    ui::PinnedMessageBar *m_chatPinned = nullptr;
    QLabel *m_chatEmpty = nullptr;
    ui::AvatarBadge *m_chatAvatar = nullptr;
    QPushButton *m_openChatTask = nullptr;
    QPushButton *m_latestChat = nullptr;
    QString m_openChatTaskId;
    QString m_contextDialogId;
    QString m_contextMessageId;
    QUrl m_chatAvatarUrl;
    ui::ChatComposer *m_chatComposer = nullptr;
    QPlainTextEdit *m_chatInput = nullptr;
    QPushButton *m_chatSend = nullptr;
    QLineEdit *m_chatSearch = nullptr;
    QLabel *m_chatSearchProgress = nullptr;
    QPushButton *m_chatSearchClear = nullptr;
    QPushButton *m_createGroupChat = nullptr;
    ui::GlyphButton *m_unreadOnlyButton = nullptr;
    bool m_unreadOnlyChats = false;
    QVector<Chat> m_chatSource;
    bool m_chatSourceSearching = false;
    Chat m_retainedTaskChat;
    int m_retainedTaskChatRow = -1;
    bool m_taskChatPressActive = false;
    bool m_taskChatRefreshDeferred = false;
    QTimer *m_taskChatPressEnd = nullptr;
    QLineEdit *m_messageSearch = nullptr;
    QPushButton *m_messageSearchClear = nullptr;
    QPushButton *m_messageSearchToggle = nullptr;
    QPushButton *m_chatEmoji = nullptr;
    QPushButton *m_chatFile = nullptr;
    struct ChatDirectory {
        QVector<Chat> recent;
        QVector<Chat> found;
        Chat linked;
        QString selected;
        QString query;
        QString input;
        bool loaded = false;
    };
    ChatDirectory m_chatDirectories[2];
    bool m_taskChatBadgesRequested = false;
    int m_chatScope = 0;
    bool m_messageSearchActive = false;
    QString m_messageSearchQuery;
    QString m_messageSearchScope;
    QString m_pendingUpload;
    ui::ChatDraft m_pendingPastedChatImage;
    QString m_uploadName;
    QString m_descriptionUploadTask;
    QPointer<ui::TaskCardDialog> m_descriptionUploadCard;
    QStringList m_descriptionUploadPaths;
    QSize m_descriptionUploadSize;
    bool m_descriptionUploadLinked = false;
    bool m_descriptionUploadConfirmed = false;
    std::unique_ptr<QTemporaryDir> m_descriptionPaste;
    QString m_cardSearchQuery;
    QString m_cardContextMessageId;
    QHash<QString, QString> m_messageSearches;
    QTimer *m_chatSearchTimer = nullptr;
    QTimer *m_messageSearchTimer = nullptr;
    QTimer *m_visibleChatReadTimer = nullptr;

    QListWidget *m_projects = nullptr;
    QVector<Project> m_projectDirectory;
    QHash<QString, qint64> m_projectUnreadCounts;
    bool m_projectUnreadCountsComplete = false;
    ui::AvatarBadge *m_projectAvatar = nullptr;
    QUrl m_projectAvatarUrl;
    QCache<QUrl, QPixmap> m_avatars{128};
    ui::ChatImagePreviews m_chatImagePreviews;
    std::unique_ptr<ui::ChatVideoPreviews> m_chatVideoPreviews;
    QCache<QUrl, MessageLinkPreview> m_chatLinkPreviews{300};
    QCache<QPair<QString, QUrl>, ChatAsset> m_externalLinkPreviews{150};
    PublicLinkRequest m_publicLinkRequest;
    ui::PublicLinkPreviewFetcher *m_publicLinkFetcher = nullptr;
    QCache<QUrl, ui::PublicLinkMetadata> m_publicLinkPreviews{300};
    quint64 m_publicLinkAccountGeneration = 0;
    bool m_publicLinkPending = false;
    QPointer<QListWidget> m_externalLinkHistory;
    quint64 m_externalLinkGeneration = 0;
    qint64 m_latestExternalLinkMessage = 0;
    bool m_externalLinkPending = false;
    QHash<QUrl, quint64> m_pendingLinkPreviews;
    int m_linkPreviewRequests = 0;
    quint64 m_lastLinkPreviewRequest = 0;
    bool m_linkPreviewLayoutQueued = false;
    bool m_linkPreviewAccess = true;
    QSet<QUrl> m_requestedAvatars;
    bool m_avatarUpdateQueued = false;
    QSet<QString> m_pendingPins;
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
    ui::TaskCardDialog *m_card = nullptr;
    bool m_loadCardChat = false;
    bool m_cardDetailsReady = false;
    bool m_cardSummaryPending = false;
    bool m_cardRequestedProjectDirectory = false;
    QString m_cardRequestedStages;
    ui::StageManagerDialog *m_stageManager = nullptr;
    QTimer *m_projectSearchTimer = nullptr;
    QHash<QString, QString> m_pendingProjectSearches;
    QVector<Task> m_projectSearchResults;
    bool m_projectSearchDisplayed = false;

    QListWidget *m_notifications = nullptr;
    QLabel *m_notificationsEmpty = nullptr;
    QVector<QVector<NotificationTarget>> m_noticeTargets;
    QPointer<ClientUpdateFeed> m_clientUpdates;
    QVector<Notification> m_noticeHistory;
    QVector<Notification> m_noticeSearchResults;
    QString m_noticeSelectedId;
    QString m_noticeQuery;
    QString m_noticeAction;
    QTimer *m_noticeSearchTimer = nullptr;
    QTimer *m_noticeReadTimer = nullptr;
    QStringList m_pendingNoticeRead;
    QSet<QString> m_failedNoticeRead;
    bool m_readNoticesAfterPending = false;
    QLineEdit *m_noticeSearch = nullptr;
    QCheckBox *m_noticeUnreadOnly = nullptr;
    QComboBox *m_noticeSort = nullptr;
    QPushButton *m_noticeRead = nullptr;
    QPushButton *m_noticeReadAll = nullptr;
    QPushButton *m_noticeOlder = nullptr;
    QPushButton *m_noticeDelete = nullptr;
    enum class NoticeHistoryState { Ready, Loading, Failed };
    NoticeHistoryState m_noticeHistoryState = NoticeHistoryState::Ready;
    bool m_noticeHistoryRequested = false;
    bool m_noticeHasOlder = false;
    bool m_noticeOlderLoading = false;
    bool m_noticeSearchPending = false;
    QString m_noticeProjectId;
    QPointer<QDialog> m_personDialog;
    QLabel *m_personName = nullptr;
    QLabel *m_personPosition = nullptr;
    ui::AvatarBadge *m_personAvatar = nullptr;
    QString m_personId;
    QUrl m_personAvatarUrl;
    QFileDialog *m_messageSaveDialog = nullptr;
    QUrl m_messageDownloadUrl;
    QString m_messageDownloadPath;
    QPointer<QWidget> m_messageDownloadParent;
    QPointer<QDialog> m_chatPhoto;
    QThreadPool m_chatPhotoDecodePool;

    QHash<QString, ui::ChatDraft> m_chatDrafts;
    QSet<QString> m_pendingReactionAdds;
    QString m_reactionAccountScope;
    QHash<QString, ui::ChatDraft> m_taskDrafts;
    QString m_currentChatId;
    QString m_currentProjectId;
    QString m_pendingProjectId;
    QPointer<ui::TaskEditForm> m_taskForm;
    ui::NameDialog *m_projectForm = nullptr;
    QString m_pendingCreateTask;
    QString m_pendingCreateSubtask;
    bool m_pendingCreateProject = false;
    QString m_pendingTaskEdit;
    QPointer<ui::TaskCardDialog> m_pendingTaskEditCard;
    QString m_pendingTaskEditProject;
    QString m_pendingTaskEditTargetProject;
    QString m_pendingTaskDelete;
    QPointer<ui::TaskCardDialog> m_pendingTaskDeleteCard;
    QString m_pendingTaskDeleteProject;
    QString m_pendingBoardEdit;
    QString m_pendingMoveTask;
    QString m_boardMoveRefreshProject;
    QString m_pendingChatId;
    QString m_pendingTaskId;
    ui::ChatDraft m_pendingChatDraft;
    ui::ChatDraft m_pendingTaskDraft;
    int m_lastSection = 0;
};

} // namespace bx
