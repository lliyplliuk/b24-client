#include "cloud/tasks_service.h"
#include "cloud/cloud_rows.h"
#include "cloud/task_card_fields.h"

#include <utility>

namespace bx {
using namespace cloud_rows;
using namespace task_card_fields;

void TasksService::createTask(const QString &projectId, const TaskEdit &edit)
{
    createTaskRequest(projectId, {}, edit);
}

void TasksService::createSubtask(const QString &parentId, const QString &projectId, const TaskEdit &edit)
{
    if (numericId(parentId).isEmpty()) {
        emit failed(operationKey(OperationKind::CreateSubtask, parentId), QStringLiteral("Номер родительской задачи недействителен"));
        return;
    }
    createTaskRequest(projectId, parentId, edit);
}

void TasksService::createTaskRequest(const QString &projectId, const QString &parentId, const TaskEdit &edit)
{
    const bool child = !parentId.isEmpty();
    const auto operation = child ? operationKey(OperationKind::CreateSubtask, parentId) : operationKey(OperationKind::CreateTask, projectId);
    const auto title = edit.title.value_or(QString()).trimmed();
    const auto deadline = taskDeadline(edit.deadline.value_or(QString()));
    if (((!child || !projectId.isEmpty()) && numericId(projectId).isEmpty()) || title.isEmpty() || !deadline) {
        emit failed(operation, QStringLiteral("Проверьте проект, название и срок задачи"));
        return;
    }
    const auto creatorId = edit.creatorId.value_or(QString()).toLongLong();
    const auto assigneeId = edit.assigneeId.value_or(QString()).toLongLong();
    const auto stageId = edit.stageId.value_or(QString()).toLongLong();
    if ((edit.creatorId && creatorId <= 0) || (edit.assigneeId && assigneeId <= 0)
        || (edit.stageId && (projectId.isEmpty() || stageId <= 0))
        || (edit.observerIds && !validTaskObserverIds(*edit.observerIds))) {
        emit failed(operation, QStringLiteral("Проверьте значения реквизитов задачи"));
        return;
    }
    QJsonObject task{{QStringLiteral("title"), title},
                     {QStringLiteral("priority"), edit.highPriority.value_or(false) ? QStringLiteral("high") : QStringLiteral("average")}};
    task.insert(QStringLiteral("group"), QJsonObject{{QStringLiteral("id"), projectId.toLongLong()}});
    if (edit.stageId)
        task.insert(QStringLiteral("stage"), QJsonObject{{QStringLiteral("id"), stageId}});
    if (edit.observerIds) {
        QJsonArray observers;
        for (const auto &id : *edit.observerIds)
            observers.append(QJsonObject{{QStringLiteral("id"), id.toLongLong()}});
        task.insert(QStringLiteral("auditors"), observers);
    }
    if (child)
        task.insert(QStringLiteral("parent"), QJsonObject{{QStringLiteral("id"), parentId.toLongLong()}});
    if (edit.description)
        task.insert(QStringLiteral("description"), *edit.description);
    if (edit.deadline)
        task.insert(QStringLiteral("deadlineTs"), *deadline);
    const auto account = m_session.accountScopeRevision();
    m_session.withCurrentUser(operation, [this, projectId, parentId, task = std::move(task), deadline, operation, account,
                                creatorId, assigneeId](qint64 userId) mutable {
        if (account != m_session.accountScopeRevision())
            return;
        const QJsonObject responsible{{QStringLiteral("id"), assigneeId > 0 ? assigneeId : userId}};
        task.insert(QStringLiteral("creator"), QJsonObject{{QStringLiteral("id"), creatorId > 0 ? creatorId : userId}});
        task.insert(QStringLiteral("responsible"), responsible);
        task.insert(QStringLiteral("responsibleCollection"), QJsonArray{responsible});
        task.insert(QStringLiteral("isMultitask"), false);
        m_portalHttp.ajaxJson(QStringLiteral("tasks.v2.Task.add"),
                 {{QStringLiteral("task"), task}, {QStringLiteral("view"), false},
                  {QStringLiteral("deadlineManuallySet"), *deadline != 0}},
                 operation, [this, projectId, parentId, operation](QJsonValue result) {
            const auto taskId = numericId(field(result.toObject(), {"id"}));
            if (taskId.isEmpty())
                return m_session.failShape(operation);
            if (parentId.isEmpty())
                emit taskCreated(projectId, taskId);
            else
                emit subtaskCreated(parentId, projectId, taskId);
        });
    });
}

} // namespace bx
