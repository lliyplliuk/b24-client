#include "cloud/tasks_service.h"
#include "cloud/cloud_rows.h"
#include "cloud/task_card_fields.h"
#include "core/origin_guard.h"

#include <QBuffer>
#include <QJsonDocument>
#include <utility>

namespace bx {
using namespace cloud_rows;
using namespace task_card_fields;

namespace {

QString taskActionRequestKey(quint64 scopeRevision, const QString &taskId)
{
    return operationKey(OperationKind::TaskCardAction, QString::number(scopeRevision), taskId);
}

QJsonValue legacyTaskMark(TaskRating rating)
{
    switch (rating) {
    case TaskRating::Unrated: return QJsonValue::Null;
    case TaskRating::Positive: return QStringLiteral("P");
    case TaskRating::Negative: return QStringLiteral("N");
    }
    return QJsonValue::Undefined;
}

} // namespace

void TasksService::updateTask(const QString &taskId, const TaskEdit &edit)
{
    const auto operation = operationKey(OperationKind::UpdateTask, taskId);
    const auto deadline = taskDeadline(edit.deadline.value_or(QString()));
    if (numericId(taskId).isEmpty() || (edit.title && edit.title->trimmed().isEmpty()) || !deadline
        || (edit.deadlineReason && edit.deadlineReason->size() > 2000)) {
        emit failed(operation, QStringLiteral("Проверьте название и срок задачи"));
        return;
    }
    if (m_portalHttp.contains(operation) || m_portalHttp.contains(taskActionRequestKey(m_session.accountScopeRevision(), taskId)))
        return;
    if (edit.rating && (!m_session.started() || numericId(taskId) != taskId || legacyTaskMark(*edit.rating).isUndefined()))
        return m_session.failShape(operation);
    const int requisites = static_cast<int>(edit.creatorId.has_value()) + edit.assigneeId.has_value()
        + edit.observerIds.has_value() + edit.projectId.has_value() + edit.stageId.has_value()
        + edit.statusAction.has_value() + edit.rating.has_value();
    if (edit.requireDelegate && !edit.assigneeId)
        return m_session.failShape(operation);
    if (requisites > 1 || (requisites && (edit.title || edit.description || edit.highPriority || edit.deadline))) {
        emit failed(operation, QStringLiteral("Меняйте по одному реквизиту задачи за раз"));
        return;
    }
    if (requisites || (edit.deadline && !edit.title && !edit.description && !edit.highPriority)) {
        updateTaskRequisite(taskId, edit);
        return;
    }

    const QJsonObject id{{QStringLiteral("id"), taskId.toLongLong()}};
    QJsonObject fields = id;
    if (edit.title)
        fields.insert(QStringLiteral("title"), edit.title->trimmed());
    if (edit.highPriority)
        fields.insert(QStringLiteral("priority"), *edit.highPriority ? QStringLiteral("high") : QStringLiteral("average"));
    QQueue<QPair<QString, QJsonObject>> writes;
    if (fields.size() > id.size())
        writes.enqueue({QStringLiteral("tasks.v2.Task.update"), fields});
    if (edit.deadline) {
        QJsonObject deadlineFields = id;
        deadlineFields.insert(QStringLiteral("deadlineTs"), *deadline);
        if (edit.deadlineReason && !edit.deadlineReason->trimmed().isEmpty())
            deadlineFields.insert(QStringLiteral("deadlineChangeReason"), edit.deadlineReason->trimmed());
        writes.enqueue({QStringLiteral("tasks.v2.Task.Deadline.update"), deadlineFields});
    }
    if (!edit.hasChanges())
        return;
    m_portalHttp.ajaxJson(QStringLiteral("tasks.v2.Task.get"), {{QStringLiteral("task"), id}}, operation,
             [this, taskId, id, edit, writes = std::move(writes), operation]
             (QJsonValue result) mutable {
        const auto current = result.toObject();
        if (field(current, {"id"}) != taskId)
            return m_session.failShape(operation);
        const auto rights = current.value(QStringLiteral("rights")).toObject();
        if (((edit.title || edit.description || edit.highPriority) && rights.value(QStringLiteral("edit")) != true)
            || (edit.deadline && rights.value(QStringLiteral("deadline")) != true)) {
            emit failed(operation, QStringLiteral("Портал не разрешает изменить этот реквизит задачи"));
            return;
        }
        if (edit.deadline && current.value(QStringLiteral("requireDeadlineChangeReason")) == true
            && edit.deadlineReason.value_or(QString()).trimmed().isEmpty()) {
            emit failed(operation, QStringLiteral("Укажите причину изменения срока"));
            return;
        }
        if (!edit.description) {
            updateTaskFields(taskId, std::move(writes));
            return;
        }
        const auto currentChecksum = current.value(QStringLiteral("descriptionChecksum")).toString();
        const auto currentDescription = current.value(QStringLiteral("description"));
        const auto &baseline = edit.originalDescription;
        const auto &checksum = edit.descriptionChecksum;
        const bool descriptionChanged = baseline && (!currentDescription.isString()
            || (*baseline != currentDescription.toString() && encodedTaskDescription(*baseline) != currentDescription.toString()));
        if ((checksum && *checksum != currentChecksum)
            || (!checksum && descriptionChanged)) {
            emit failed(operation, QStringLiteral("Описание уже изменилось на портале. Скопируйте свои правки и обновите задачу"));
            return;
        }
        QJsonObject descriptionFields = id;
        descriptionFields.insert(QStringLiteral("description"), *edit.description);
        descriptionFields.insert(QStringLiteral("descriptionChecksum"), currentChecksum);
        writes.enqueue({QStringLiteral("tasks.v2.Task.Description.update"), descriptionFields});
        updateTaskFields(taskId, std::move(writes));
    });
}

void TasksService::deleteTask(const QString &taskId)
{
    const auto operation = operationKey(OperationKind::DeleteTask, taskId);
    if (!m_session.started() || taskId.isEmpty() || numericId(taskId) != taskId)
        return m_session.failShape(operation);
    const auto account = m_session.accountScopeRevision();
    const auto key = taskActionRequestKey(account, taskId);
    if (m_portalHttp.contains(key) || m_portalHttp.contains(operationKey(OperationKind::UpdateTask, taskId)))
        return;
    const QJsonObject id{{QStringLiteral("id"), taskId.toLongLong()}};
    m_portalHttp.ajaxJson(QStringLiteral("tasks.v2.Task.get"), {{QStringLiteral("task"), id}}, operation,
             [this, taskId, operation, account, key](QJsonValue response) {
        if (account != m_session.accountScopeRevision())
            return;
        const auto current = response.toObject();
        if (!matchesTaskIdentity(current, taskId))
            return m_session.failShape(operation);
        if (current.value(QStringLiteral("rights")).toObject().value(QStringLiteral("remove")) != true) {
            emit failed(operation, QStringLiteral("Портал не разрешает удалить эту задачу"));
            return;
        }
        const auto url = OriginGuard(m_session.portalOrigin()).resolve(QStringLiteral("/rest/tasks.task.delete.json"));
        m_portalHttp.webRequest(url, {{QStringLiteral("taskId"), taskId}}, operation, QStringLiteral("result"),
                   [this, taskId, operation, account](QJsonValue result) {
            if (account != m_session.accountScopeRevision())
                return;
            if (result.toObject().value(QStringLiteral("task")) != true)
                return m_session.failShape(operation);
            m_taskChats.clearDeletedTaskHistory(taskId);
            m_taskAttachmentRevisions.remove(taskId);
            m_taskChats.forgetTaskChat(taskId);
            emit taskLinkPreviewInvalidated(taskId);
            emit taskDeleted(taskId);
        }, key);
    }, key);
}

void TasksService::updateTaskRequisite(const QString &taskId, const TaskEdit &edit)
{
    const auto operation = operationKey(OperationKind::UpdateTask, taskId);
    if ((edit.creatorId && numericId(*edit.creatorId).isEmpty())
        || (edit.assigneeId && numericId(*edit.assigneeId).isEmpty())
        || (edit.projectId && !boardStageId(*edit.projectId))
        || (edit.stageId && numericId(*edit.stageId).isEmpty())
        || (edit.observerIds && (!edit.originalObserverIds || !validTaskObserverIds(*edit.originalObserverIds)
                                || !validTaskObserverIds(*edit.observerIds)))) {
        emit failed(operation, QStringLiteral("Проверьте значения реквизита задачи"));
        return;
    }
    const QJsonObject id{{QStringLiteral("id"), taskId.toLongLong()}};
    const auto account = m_session.accountScopeRevision();
    const auto requestKey = (edit.rating || edit.statusAction || edit.assigneeId || edit.projectId || edit.stageId)
        ? taskActionRequestKey(account, taskId) : QString();
    m_portalHttp.ajaxJson(QStringLiteral("tasks.v2.Task.get"), {{QStringLiteral("task"), id}}, operation,
             [this, taskId, operation, id, edit, account, requestKey](QJsonValue data) {
        const auto current = data.toObject();
        if (!requestKey.isEmpty() && account != m_session.accountScopeRevision())
            return;
        if (!matchesTaskIdentity(current, taskId))
            return m_session.failShape(operation);
        Task task;
        applyTaskCardDetails(task, current);
        const auto rights = task.editRights;
        if ((edit.creatorId && !rights.creator)
            || (edit.assigneeId && (!rights.assignee || (edit.requireDelegate && !rights.delegate)))
            || (edit.deadline && !rights.deadline)
            || (edit.projectId && !rights.project) || (edit.stageId && !rights.stage)
            || (edit.statusAction && !task.statusActions.contains(*edit.statusAction))
            || (edit.rating && !rights.rate)) {
            emit failed(operation, QStringLiteral("Портал не разрешает изменить этот реквизит задачи"));
            return;
        }
        if (edit.rating) {
            const auto rating = *edit.rating;
            const auto url = OriginGuard(m_session.portalOrigin()).resolve(QStringLiteral("/rest/tasks.task.update.json"));
            const QJsonObject body{{QStringLiteral("taskId"), taskId.toLongLong()},
                {QStringLiteral("fields"), QJsonObject{{QStringLiteral("MARK"), legacyTaskMark(rating)}}},
                {QStringLiteral("sessid"), QString::fromLatin1(m_portalHttp.csrfToken())}};
            auto *buffer = new QBuffer;
            buffer->setData(QJsonDocument(body).toJson(QJsonDocument::Compact));
            buffer->open(QIODevice::ReadOnly);
            m_portalHttp.webRequestRaw(url, buffer, {{"Content-Type", "application/json"}}, operation, QStringLiteral("result"),
                          [this, taskId, operation, rating, account](QJsonValue result) {
                if (account != m_session.accountScopeRevision())
                    return;
                const auto task = result.toObject().value(QStringLiteral("task")).toObject();
                if (!matchesTaskIdentity(task, taskId) || taskRating(task) != rating)
                    return m_session.failShape(operation);
                emit taskUpdated(taskId);
            }, requestKey);
            return;
        }
        if ((edit.projectId && numericId(*edit.projectId) == task.projectId)
            || (edit.stageId && numericId(*edit.stageId) == task.stageId)) {
            emit taskUpdated(taskId);
            return;
        }
        if (edit.stageId) {
            m_portalHttp.componentAction(QStringLiteral("moveStage"),
                {{QStringLiteral("taskId"), taskId}, {QStringLiteral("stageId"), *edit.stageId}}, operation,
                [this, taskId, operation, account](QJsonValue result) {
                    if (account != m_session.accountScopeRevision())
                        return;
                    if (result != true)
                        return m_session.failShape(operation);
                    emit taskUpdated(taskId);
                }, QStringLiteral("bitrix:tasks.task"), requestKey);
            return;
        }
        const auto postAction = [this, taskId, operation, requestKey](const QString &action, const QJsonObject &body) {
            m_portalHttp.ajaxJson(action, body, operation, [this, taskId, operation](QJsonValue result) {
                if (result == false)
                    return m_session.failShape(operation);
                emit taskUpdated(taskId);
            }, requestKey);
        };
        if (edit.statusAction) {
            const auto command = taskStatusCommand(*edit.statusAction);
            QJsonObject body{{QStringLiteral("task"), id}};
            if (*edit.statusAction == TaskStatusAction::Complete)
                body.insert(QStringLiteral("completeWithChecklist"), false);
            postAction(QStringLiteral("tasks.v2.Task.Status.") + command, body);
            return;
        }
        QJsonObject fields = id;
        const auto person = [](const QString &userId) { return QJsonObject{{QStringLiteral("id"), userId.toLongLong()}}; };
        const auto responsible = [&](const QString &userId) {
            fields.insert(QStringLiteral("responsible"), person(userId));
            fields.insert(QStringLiteral("responsibleCollection"), QJsonArray{person(userId)});
            fields.insert(QStringLiteral("isMultitask"), false);
        };
        QQueue<QPair<QString, QJsonObject>> writes;
        if (edit.creatorId) {
            fields.insert(QStringLiteral("creator"), person(*edit.creatorId));
            responsible(task.assigneeId);
            writes.enqueue({QStringLiteral("tasks.v2.Task.Stakeholder.Creator.update"), fields});
        } else if (edit.assigneeId) {
            responsible(*edit.assigneeId);
            postAction(QStringLiteral("tasks.v2.Task.Stakeholder.Responsible.delegate"), {{QStringLiteral("task"), fields}});
            return;
        } else if (edit.projectId) {
            fields.insert(QStringLiteral("group"), person(*edit.projectId));
            fields.insert(QStringLiteral("stage"), QJsonObject{{QStringLiteral("id"), 0}});
            writes.enqueue({QStringLiteral("tasks.v2.Task.update"), fields});
        } else if (edit.deadline) {
            const auto reason = edit.deadlineReason.value_or(QString()).trimmed();
            if (task.deadlineReasonRequired && reason.isEmpty()) {
                emit failed(operation, QStringLiteral("Укажите причину изменения срока"));
                return;
            }
            fields.insert(QStringLiteral("deadlineTs"), *taskDeadline(*edit.deadline));
            if (!reason.isEmpty())
                fields.insert(QStringLiteral("deadlineChangeReason"), reason);
            writes.enqueue({QStringLiteral("tasks.v2.Task.Deadline.update"), fields});
        } else if (edit.observerIds) {
            updateTaskObservers(taskId, current, *edit.originalObserverIds, *edit.observerIds, rights.observers);
            return;
        }
        updateTaskFields(taskId, std::move(writes), requestKey);
    }, requestKey);
}

void TasksService::updateTaskFields(const QString &taskId, QQueue<QPair<QString, QJsonObject>> writes,
                                    const QString &requestKey)
{
    const auto scope = m_session.accountScopeRevision();
    if (!requestKey.isEmpty() && requestKey != taskActionRequestKey(scope, taskId))
        return;
    if (writes.isEmpty()) {
        emit taskUpdated(taskId);
        return;
    }
    const auto [action, task] = writes.dequeue();
    const auto operation = operationKey(OperationKind::UpdateTask, taskId);
    m_portalHttp.ajaxJson(action, {{QStringLiteral("task"), task}}, operation,
             [this, taskId, operation, title = task.value(QStringLiteral("title")).toString(),
              changedProject = task.contains(QStringLiteral("group")),
              project = task.value(QStringLiteral("group")).toObject().value(QStringLiteral("id")).toInteger(-1),
              scope, requestKey,
              writes = std::move(writes)](QJsonValue result) mutable {
        if (!requestKey.isEmpty() && scope != m_session.accountScopeRevision())
            return;
        if (result == false)
            return m_session.failShape(operation);
        m_taskChats.rememberTaskTitle(taskId, title);
        if (changedProject) {
            if (scope == m_session.accountScopeRevision())
                m_taskChats.updateTaskChatProject(taskId, project);
            m_taskChats.invalidateTaskChat(taskId);
        }
        updateTaskFields(taskId, std::move(writes), requestKey);
    }, requestKey);
}

} // namespace bx
