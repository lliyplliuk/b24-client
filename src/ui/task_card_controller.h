#pragma once

#include "core/operations.h"
#include "ui/composer_send_controller.h"
#include "ui/workspace_message_search.h"

#include <QPointer>

class QListWidget;
class QTemporaryDir;

namespace bx {

class ChatPanel;
class ProjectsPanel;
class WorkspaceMessagePreviews;
namespace ui { class TaskCardDialog; class TaskEditForm; }

// Chat and task composers share one in-flight file upload.
struct WorkspaceUpload {
    QString operation;
    QString name;
    ui::ChatDraft pastedImage;

    bool pastedImagePending(const QString &scope, bool task) const
    {
        return pastedImage.image && operation == operationKey(task ? OperationKind::UploadTaskChat : OperationKind::UploadChat, scope);
    }
};

class TaskCardController final : public QObject {
    Q_OBJECT
public:
    struct Workspace {
        std::function<bool()> writable;
        std::function<bool()> projectsActive;
        std::function<const QPixmap *(const QUrl &)> avatar;
        std::function<void(const QUrl &)> requestAvatar;
        std::function<void(QListWidget *, bool)> bindMessageFiles;
        std::function<void(const QString &, const QString &)> pickFile;
        std::function<void(ui::ComposerSendController &, ui::ChatComposer *, const QString &)> sendFromComposer;
        WorkspaceUpload &upload;
    };

    TaskCardController(Backend *backend, ChatPanel *chat, ProjectsPanel *projects,
                       WorkspaceMessagePreviews *previews, Workspace workspace, QWidget *window);
    ~TaskCardController() override;
    ui::TaskCardDialog *card() const { return m_card; }
    bool sendPending() const { return m_taskSendController.pending(); }
    bool creationPending() const { return !m_pendingCreateTask.isEmpty(); }
    bool clearTask();
    bool confirmDiscard();
    void resetAccount();
    void refresh();
    void openTaskCardForProject(const QString &taskId, const QString &projectId);
    bool activateChat(const NotificationTarget &target);
    bool readActiveTask(const QWidget *active);
    void updateWriteState();
    void refreshAvatars();
    void requestVisibleAvatars() const;
    QListWidget *history() const;
    bool showFileStatus(QWidget *parent, const QString &status, bool loading = false);
    bool showTaskStatus(const QString &id, const QString &status, bool loading);
    bool handleFailure(const QString &operation, OperationKind kind, const QStringList &ids,
                       const QString &reason, bool networkFailure, bool &statusPresented);

signals:
    void statusChanged(const QString &text, bool loading = false);
    void writeStateChanged();
    void avatarsRequested();
    void avatarRowsChanged();
    void historyClosed(bool task);

private:
    bool writable() const { return m_workspace.writable(); }
    void connectBackend();
    void bindHistory(ui::TaskCardDialog *card);
    void onTaskUpdated(const QString &taskId);
    void confirmTaskDeletion(ui::TaskCardDialog *card);
    void onTaskDeleted(const QString &taskId);
    void onTaskCreated(const QString &projectId, const QString &taskId);
    void onMessagesFound(const QString &scopeId, const QString &query, bool task,
                         const QVector<Message> &messages);
    void onFileUploaded(const QString &scopeId, const QString &kind);
    void openPinnedTaskMessage(const QString &messageId);
    void openTaskMessageViewers(QListWidget *history, const QString &messageId, const QPoint &anchor);
    void onTaskSummaryLoaded(const Task &task);
    void updateTaskCardChatBinding(const QString &previousDialog);
    void bindTaskForm(ui::TaskEditForm *form, const QString &projectId, bool canDetachProject);
    void createTaskInProject();
    void createSubtask(ui::TaskCardDialog *card);
    bool beginTaskFormFiles(const QString &taskId, const QString &projectToOpen = {});
    bool uploadNextTaskFormFile();
    void queueDescriptionFiles(const QString &taskId, const QStringList &paths);
    bool uploadNextDescriptionFile();
    void clearDescriptionUpload();
    void pasteDescriptionPhoto(const QString &taskId, const QImage &image);
    void onPeopleFound(const QString &requestId, const QVector<Person> &people);

    Backend *m_backend;
    ChatPanel *m_chatPanel;
    ProjectsPanel *m_projectsPanel;
    WorkspaceMessagePreviews *m_messagePreviews;
    Workspace m_workspace;
    QWidget *m_window;
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
    ui::TaskCardDialog *m_card = nullptr;
    bool m_loadCardChat = false;
    bool m_cardDetailsReady = false;
    bool m_cardSummaryPending = false;
    bool m_cardRequestedProjectDirectory = false;
    QString m_cardRequestedStages;
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
};

} // namespace bx
