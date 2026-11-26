#pragma once

#include "cloud_service_context.h"
#include "core/models.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QQueue>

namespace bx {

class TaskChatHost {
public:
    virtual ~TaskChatHost() = default;
    virtual quint64 unreadBindingRevision() const = 0;
    virtual void rememberLoadedTask(const QString &taskId, const QJsonObject &task, quint64 bindingRevision) = 0;
    virtual void rememberTaskTitle(const QString &id, const QString &title) = 0;
    virtual void updateTaskChatProject(const QString &taskId, qint64 project) = 0;
    virtual void invalidateTaskChat(const QString &taskId) = 0;
    virtual void clearDeletedTaskHistory(const QString &taskId) = 0;
    virtual void forgetTaskChat(const QString &taskId) = 0;
};

class TaskFileHost {
public:
    virtual ~TaskFileHost() = default;
    virtual QUrl taskFileUrl(const QString &value, qint64 size, bool photo) const = 0;
    virtual QUrl taskImageUrl(const QString &value, const QString &attachmentId, const QString &objectId,
                              bool original = false) const = 0;
    virtual qsizetype taskImageMaxBytes() const = 0;
    virtual void uploadTaskFile(const QString &taskId, const QString &localPath, const QString &operation) = 0;
};

class TasksService final : public QObject {
    Q_OBJECT
public:
    TasksService(CloudServiceContext &session, TaskChatHost &taskChats, TaskFileHost &files,
                 PortalHttp &http, QObject *parent);
    void reset();
    void handleOperationFailure(const QString &operation);
    void attachUploadedFile(const QString &taskId, const QString &fileId, const QString &operation);
    void createTask(const QString &projectId, const TaskEdit &edit);
    void createSubtask(const QString &parentId, const QString &projectId, const TaskEdit &edit);
    void updateTask(const QString &taskId, const TaskEdit &edit);
    void deleteTask(const QString &taskId);
    void requestTask(const QString &taskId);
    void requestTaskChecklist(const QString &taskId);
    void addTaskChecklistItem(const QString &taskId, const QString &parentId, const QString &title);
    void setTaskChecklistComplete(const QString &taskId, const QString &itemId, bool complete);
    void requestSubtasks(const QString &taskId);
    void requestTaskDependencies(const QString &taskId);
    void requestTaskHistory(const QString &taskId);
    void attachTaskFile(const QString &taskId, const QString &localPath);
    void loadTask(const QString &taskId, const QString &operation,
                  std::function<void(QJsonObject)> onTask, const QString &requestKey = {}, bool remember = true);
    QUrl taskRoleAvatar(const QJsonObject &current, const QJsonObject &legacy, const QString &personId) const;

signals:
    void taskCreated(const QString &projectId, const QString &taskId);
    void subtaskCreated(const QString &parentId, const QString &projectId, const QString &taskId);
    void taskDeleted(const QString &taskId);
    void taskUpdated(const QString &taskId);
    void taskSummaryLoaded(const Task &task);
    void taskLoaded(const Task &task);
    void taskChecklistLoaded(const QString &taskId, const QVector<TaskChecklistItem> &items);
    void taskChecklistChanged(const QString &taskId);
    void subtasksLoaded(const QString &taskId, const QVector<Task> &tasks);
    void taskDependenciesLoaded(const QString &taskId, const QVector<TaskDependency> &dependencies);
    void taskHistoryLoaded(const QString &taskId, const QVector<TaskHistoryEntry> &history, bool limited);
    void taskLinkPreviewInvalidated(const QString &taskId);
    void taskFileAttached(const QString &taskId, const QString &fileId);
    void fileUploaded(const QString &scopeId, const QString &kind);
    void failed(const QString &operation, const QString &reason);

private:
    CloudServiceContext &m_session;
    TaskChatHost &m_taskChats;
    TaskFileHost &m_files;
    PortalHttp &m_portalHttp;
    using Fields = PortalHttp::Fields;
    using Handler = PortalHttp::Handler;
    void createTaskRequest(const QString &projectId, const QString &parentId, const TaskEdit &edit);
    void updateTaskRequisite(const QString &taskId, const TaskEdit &edit);
    void updateTaskObservers(const QString &taskId, const QJsonObject &current, const QStringList &original,
                             const QStringList &after, bool canManage);
    void updateTaskFields(const QString &taskId, QQueue<QPair<QString, QJsonObject>> writes,
                          const QString &requestKey = {});
    void finishTaskCardRead(const QString &taskId);
    void loadTaskChecklist(const QString &taskId, const QString &operation,
                           std::function<void(QJsonArray, QJsonValue)> handler);
    void updateTaskChecklist(const QString &taskId, const QString &action, QJsonObject payload);
    void loadTaskAttachments(Task task, const QJsonObject &row);

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
};

} // namespace bx
