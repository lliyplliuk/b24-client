#pragma once

#include "ui/workspace_message_search.h"
#include "ui/notifications_panel.h"
#include "ui/composer_send_controller.h"
#include "ui/public_link_preview.h"

#include <QHash>
#include <QCache>
#include <QMainWindow>
#include <QPixmap>
#include <QPointer>
#include <QSet>
#include <QVector>

#include <memory>

class QLabel;
class QCheckBox;
class QDialog;
class QListWidget;
class QPushButton;
class QPoint;
class QResizeEvent;
class QStackedWidget;
class QTimer;
class QTemporaryDir;

namespace bx {

class ChatPanel;
class ProjectsPanel;
class WorkspaceMessagePreviews;
namespace ui { class WorktimePanel; class MessageFiles; }
namespace ui { class AvatarBadge; class ChatComposer; class HistoryList; class PinnedMessageBar; class TaskCardDialog; class TaskEditForm; class GlyphButton; }

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
    // Opens supported portal links in the workspace and other web links in the browser.
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
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onSectionChanged(int index);
    void onRefresh();
    void onFailed(const QString &operation, const QString &reason);
    void onTaskUpdated(const QString &taskId);
    void confirmTaskDeletion(ui::TaskCardDialog *card);
    void onTaskDeleted(const QString &taskId);
    void onTaskCreated(const QString &projectId, const QString &taskId);
    void onMessagesFound(const QString &scopeId, const QString &query, bool task,
                         const QVector<Message> &messages);
    void onFileUploaded(const QString &scopeId, const QString &kind);

private:
    void refreshCurrentSection(bool reloadProjectDirectory = true);
    bool clearTask();
    void connectWorkspaceActions();
    void showProfileSettings();
    void reloadChatVisibilityPreference();
    QUrl messageLinkDestination(const QString &href) const;
    void openPinnedTaskMessage(const QString &messageId);
    void setRailUnreadCount(int row, qint64 count);
    void setRailStatus(int row, const QString &detail);
    void requestRailWorkday();
    void updateRailWorkday();
    void scheduleAvatars();
    void loadVisibleAvatars();
    void requestAvatar(const QUrl &url);
    void updateAvatarRows();
    void openPortalTarget(NotificationTarget target, QWidget *source = nullptr, const QUrl &messageUrl = {});
    void openTaskCardForProject(const QString &taskId, const QString &projectId);
    void openPerson(const QString &personId, QWidget *source = nullptr);
    void bindMessageFiles(QListWidget *list, bool linkCards = false);
    void openTaskMessageViewers(QListWidget *history, const QString &messageId, const QPoint &anchor);
    void updateWriteAvailability();
    void onTaskSummaryLoaded(const Task &task);
    void updateTaskCardChatBinding(const QString &previousDialog);
    void bindTaskForm(ui::TaskEditForm *form, const QString &projectId, bool canDetachProject);
    void createTaskInProject();
    void createSubtask(ui::TaskCardDialog *card);
    bool beginTaskFormFiles(const QString &taskId, const QString &projectToOpen = {});
    bool uploadNextTaskFormFile();
    void pickFile(const QString &scope, const QString &kind);
    void sendFromComposer(ui::ComposerSendController &controller, ui::ChatComposer *composer, const QString &id);
    void uploadPastedChatImage(ui::ChatComposer *composer, const QString &scope, bool task, const QString &caption);
    bool pastedChatImagePending(const QString &scope, bool task) const;
    void queueDescriptionFiles(const QString &taskId, const QStringList &paths);
    bool uploadNextDescriptionFile();
    void clearDescriptionUpload();
    void pasteDescriptionPhoto(const QString &taskId, const QImage &image);
    bool writable() const { return !m_readOnly && m_online; }
    void onPeopleFound(const QString &requestId, const QVector<Person> &people);

    bool readActiveTask(const QWidget *active);
    ChatPanel *m_chatPanel = nullptr;
    Backend *m_backend;
    bool m_readOnly;
    bool m_online = false;
    bool m_closingWorkspace = false;

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
    WorkspaceMessageSearch m_taskMessageSearch;

    QCache<QUrl, QPixmap> m_avatars{128};
    std::unique_ptr<ui::MessageFiles> m_messageFiles;
    std::unique_ptr<WorkspaceMessagePreviews> m_messagePreviews;
    QSet<QUrl> m_requestedAvatars;
    bool m_avatarUpdateQueued = false;
    ui::TaskCardDialog *m_card = nullptr;
    bool m_loadCardChat = false;
    bool m_cardDetailsReady = false;
    bool m_cardSummaryPending = false;
    bool m_cardRequestedProjectDirectory = false;
    QString m_cardRequestedStages;
    ProjectsPanel *m_projectsPanel = nullptr;
    NotificationsPanel *m_events = nullptr;
    QPointer<QDialog> m_personDialog;
    QLabel *m_personName = nullptr;
    QLabel *m_personPosition = nullptr;
    ui::AvatarBadge *m_personAvatar = nullptr;
    QString m_personId;
    QUrl m_personAvatarUrl;

    QString m_accountScope;
    QHash<QString, ui::ChatDraft> m_taskDrafts;
    QPointer<ui::TaskEditForm> m_taskForm;
    QString m_pendingCreateTask;
    QString m_pendingCreateSubtask;
    QString m_pendingTaskEdit;
    QPointer<ui::TaskCardDialog> m_pendingTaskEditCard;
    QString m_pendingTaskEditProject;
    QString m_pendingTaskEditTargetProject;
    QString m_pendingTaskDelete;
    QPointer<ui::TaskCardDialog> m_pendingTaskDeleteCard;
    QString m_pendingTaskDeleteProject;
    ui::ComposerSendController m_taskSendController;
    int m_lastSection = 0;
};

} // namespace bx
