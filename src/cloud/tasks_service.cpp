#include "cloud/tasks_service.h"
#include "cloud/cloud_rows.h"
#include "core/origin_guard.h"
#include "core/tunables.h"

#include <QBuffer>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>
#include <utility>

namespace bx {
using namespace cloud_rows;
namespace {

QString taskActionRequestKey(quint64 scopeRevision, const QString &taskId)
{
    return operationKey(OperationKind::TaskCardAction, QString::number(scopeRevision), taskId);
}

std::optional<qint64> taskDeadline(const QString &value)
{
    if (value.isEmpty())
        return 0;
    const auto timestamp = QDateTime::fromString(value, Qt::ISODate);
    if (!timestamp.isValid() || timestamp.toSecsSinceEpoch() <= 0)
        return std::nullopt;
    return timestamp.toSecsSinceEpoch();
}

QString encodedTaskDescription(QString text)
{
    // The modern task service decodes BX.Text entities when reading its description DTO.
    return text.replace(QLatin1Char('&'), QLatin1String("&amp;"))
        .replace(QLatin1Char('<'), QLatin1String("&lt;"))
        .replace(QLatin1Char('>'), QLatin1String("&gt;"))
        .replace(QLatin1Char('\''), QLatin1String("&#39;"))
        .replace(QLatin1Char('"'), QLatin1String("&quot;"));
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

const QPair<TaskStatusAction, const char *> taskStatusActions[]{
    {TaskStatusAction::Start, "start"}, {TaskStatusAction::Pause, "pause"},
    {TaskStatusAction::Complete, "complete"}, {TaskStatusAction::Renew, "renew"},
    {TaskStatusAction::Defer, "defer"}, {TaskStatusAction::Approve, "approve"},
    {TaskStatusAction::Disapprove, "disapprove"}, {TaskStatusAction::Take, "take"}
};

QString taskStatusCommand(TaskStatusAction action)
{
    for (const auto &[candidate, command] : taskStatusActions) {
        if (candidate == action)
            return QLatin1String(command);
    }
    return {};
}

std::optional<QStringList> taskObserverIds(const QJsonValue &value)
{
    if (!value.isArray() || value.toArray().size() > tunables::kPageSizeChats)
        return std::nullopt;
    QStringList ids;
    for (const auto observer : value.toArray()) {
        const auto id = numericId(field(observer.toObject(), {"id"}));
        if (id.isEmpty() || ids.contains(id))
            return std::nullopt;
        ids.append(id);
    }
    return ids;
}

bool validTaskObserverIds(const QStringList &ids)
{
    if (ids.size() > tunables::kPageSizeChats)
        return false;
    QSet<QString> unique;
    for (const auto &id : ids) {
        if (id.isEmpty() || numericId(id) != id || unique.contains(id))
            return false;
        unique.insert(id);
    }
    return true;
}

bool singleTaskAssignee(const QJsonObject &card)
{
    return !numericId(field(card.value(QStringLiteral("responsible")).toObject(), {"id"})).isEmpty()
        && card.value(QStringLiteral("responsibleCollection")).toArray().size() <= 1
        && card.value(QStringLiteral("multiResponsibles")).toArray().isEmpty()
        && card.value(QStringLiteral("isMultitask")) != true;
}

void applyTaskCardDetails(Task &task, const QJsonObject &card)
{
    const auto creator = card.value(QStringLiteral("creator")).toObject();
    const auto responsible = card.value(QStringLiteral("responsible")).toObject();
    const auto group = card.value(QStringLiteral("group")).toObject();
    const auto stage = card.value(QStringLiteral("stage")).toObject();
    const auto applyText = [](QString &target, const QString &value) {
        if (!value.isEmpty())
            target = value;
    };
    applyText(task.createdBy, field(creator, {"name"}));
    applyText(task.creatorId, numericId(field(creator, {"id"})));
    applyText(task.assignee, field(responsible, {"name"}));
    applyText(task.assigneeId, numericId(field(responsible, {"id"})));
    if (card.value(QStringLiteral("tags")).isArray())
        task.tags = taskTags(card.value(QStringLiteral("tags")));
    const auto groupId = field(group, {"id"});
    if (!groupId.isEmpty() || card.value(QStringLiteral("group")).isNull()) {
        const auto projectId = numericId(groupId);
        if (task.projectId != projectId) {
            task.projectId = projectId;
            task.project.clear();
            task.stageId.clear();
            task.stageName.clear();
        }
    }
    applyText(task.project, field(group, {"name"}));
    applyText(task.stageId, field(stage, {"id"}));
    applyText(task.stageName, field(stage, {"title", "name"}));
    if (const auto created = date(card.value(QStringLiteral("createdTs"))); created.isValid())
        task.createdAt = created;
    const auto deadline = card.value(QStringLiteral("deadlineTs"));
    if (const auto due = date(deadline); due.isValid())
        task.due = due.toString(Qt::ISODate);
    else if (deadline.isNull() || (deadline.isDouble() && deadline.toInteger() == 0))
        task.due.clear();
    const auto rawStatus = field(card, {"status"});
    applyText(task.statusCode, rawStatus);
    static const QHash<QString, QString> statusLabels{
        {QStringLiteral("pending"), QStringLiteral("Ждёт выполнения")},
        {QStringLiteral("in_progress"), QStringLiteral("Выполняется")},
        {QStringLiteral("supposedly_completed"), QStringLiteral("Ждёт контроля")},
        {QStringLiteral("completed"), QStringLiteral("Завершена")},
        {QStringLiteral("deferred"), QStringLiteral("Отложена")},
        {QStringLiteral("declined"), QStringLiteral("Отклонена")}
    };
    applyText(task.status, statusLabels.value(rawStatus));
    const auto auditors = card.value(QStringLiteral("auditors"));
    if (auditors.isArray()) {
        task.observers.clear();
        task.observerPeople.clear();
        appendTaskObservers(task, auditors);
    }
    const auto description = card.value(QStringLiteral("description"));
    if (description.isString() && (description.toString() == task.description
        || description.toString() == encodedTaskDescription(task.description)))
        task.descriptionChecksum = field(card, {"descriptionChecksum"});
    task.deadlineReasonRequired = card.value(QStringLiteral("requireDeadlineChangeReason")) == true;
    if (const auto rating = taskRating(card))
        task.rating = *rating;
    const auto rights = card.value(QStringLiteral("rights")).toObject();
    const auto allowed = [&rights](const char *key) { return rights.value(QLatin1String(key)) == true; };
    const bool edit = allowed("edit");
    const bool flow = !numericId(field(card.value(QStringLiteral("flow")).toObject(), {"id"})).isEmpty();
    const bool scrum = field(group, {"type"}) == QLatin1String("scrum");
    task.editRights = {edit, edit, edit, allowed("changeDirector") && singleTaskAssignee(card),
        (allowed("changeResponsible") || allowed("delegate")) && singleTaskAssignee(card),
        allowed("addAuditors") && taskObserverIds(auditors).has_value(), allowed("deadline"),
        edit && !flow && !scrum, allowed("sort") && !task.projectId.isEmpty() && !scrum,
        allowed("mark"), allowed("remove"), allowed("delegate")};
    for (const auto &[action, command] : taskStatusActions) {
        if (allowed(command))
            task.statusActions.append(action);
    }
}

} // namespace

void TasksService::reset()
{
    m_taskCardReads.clear();
    m_taskAttachmentRevisions.clear();
}

void TasksService::handleOperationFailure(const QString &operation)
{
    const auto parsed = parseOperation(operation);
    if (parsed.kind == OperationKind::Task) {
        const auto id = parsed.ids.value(0);
        const auto current = m_taskCardReads.find(id);
        if (current != m_taskCardReads.end()) {
            current->baseFinished = true;
            finishTaskCardRead(id);
        }
    }
}

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
    const auto account = m_host.accountScopeRevision();
    m_host.withCurrentUser(operation, [this, projectId, parentId, task = std::move(task), deadline, operation, account,
                                creatorId, assigneeId](qint64 userId) mutable {
        if (account != m_host.accountScopeRevision())
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
                return m_host.failShape(operation);
            if (parentId.isEmpty())
                emit taskCreated(projectId, taskId);
            else
                emit subtaskCreated(parentId, projectId, taskId);
        });
    });
}

void TasksService::updateTask(const QString &taskId, const TaskEdit &edit)
{
    const auto operation = operationKey(OperationKind::UpdateTask, taskId);
    const auto deadline = taskDeadline(edit.deadline.value_or(QString()));
    if (numericId(taskId).isEmpty() || (edit.title && edit.title->trimmed().isEmpty()) || !deadline
        || (edit.deadlineReason && edit.deadlineReason->size() > 2000)) {
        emit failed(operation, QStringLiteral("Проверьте название и срок задачи"));
        return;
    }
    if (m_portalHttp.contains(operation) || m_portalHttp.contains(taskActionRequestKey(m_host.accountScopeRevision(), taskId)))
        return;
    if (edit.rating && (!m_host.started() || numericId(taskId) != taskId || legacyTaskMark(*edit.rating).isUndefined()))
        return m_host.failShape(operation);
    const int requisites = static_cast<int>(edit.creatorId.has_value()) + edit.assigneeId.has_value()
        + edit.observerIds.has_value() + edit.projectId.has_value() + edit.stageId.has_value()
        + edit.statusAction.has_value() + edit.rating.has_value();
    if (edit.requireDelegate && !edit.assigneeId)
        return m_host.failShape(operation);
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
            return m_host.failShape(operation);
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
    if (!m_host.started() || taskId.isEmpty() || numericId(taskId) != taskId)
        return m_host.failShape(operation);
    const auto account = m_host.accountScopeRevision();
    const auto key = taskActionRequestKey(account, taskId);
    if (m_portalHttp.contains(key) || m_portalHttp.contains(operationKey(OperationKind::UpdateTask, taskId)))
        return;
    const QJsonObject id{{QStringLiteral("id"), taskId.toLongLong()}};
    m_portalHttp.ajaxJson(QStringLiteral("tasks.v2.Task.get"), {{QStringLiteral("task"), id}}, operation,
             [this, taskId, operation, account, key](QJsonValue response) {
        if (account != m_host.accountScopeRevision())
            return;
        const auto current = response.toObject();
        if (!matchesTaskIdentity(current, taskId))
            return m_host.failShape(operation);
        if (current.value(QStringLiteral("rights")).toObject().value(QStringLiteral("remove")) != true) {
            emit failed(operation, QStringLiteral("Портал не разрешает удалить эту задачу"));
            return;
        }
        const auto url = OriginGuard(m_host.portalOrigin()).resolve(QStringLiteral("/rest/tasks.task.delete.json"));
        m_portalHttp.webRequest(url, {{QStringLiteral("taskId"), taskId}}, operation, QStringLiteral("result"),
                   [this, taskId, operation, account](QJsonValue result) {
            if (account != m_host.accountScopeRevision())
                return;
            if (result.toObject().value(QStringLiteral("task")) != true)
                return m_host.failShape(operation);
            m_host.clearDeletedTaskHistory(taskId);
            m_taskAttachmentRevisions.remove(taskId);
            m_host.forgetTaskChat(taskId);
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
    const auto account = m_host.accountScopeRevision();
    const auto requestKey = (edit.rating || edit.statusAction || edit.assigneeId || edit.projectId || edit.stageId)
        ? taskActionRequestKey(account, taskId) : QString();
    m_portalHttp.ajaxJson(QStringLiteral("tasks.v2.Task.get"), {{QStringLiteral("task"), id}}, operation,
             [this, taskId, operation, id, edit, account, requestKey](QJsonValue data) {
        const auto current = data.toObject();
        if (!requestKey.isEmpty() && account != m_host.accountScopeRevision())
            return;
        if (!matchesTaskIdentity(current, taskId))
            return m_host.failShape(operation);
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
            const auto url = OriginGuard(m_host.portalOrigin()).resolve(QStringLiteral("/rest/tasks.task.update.json"));
            const QJsonObject body{{QStringLiteral("taskId"), taskId.toLongLong()},
                {QStringLiteral("fields"), QJsonObject{{QStringLiteral("MARK"), legacyTaskMark(rating)}}},
                {QStringLiteral("sessid"), QString::fromLatin1(m_host.csrfToken())}};
            auto *buffer = new QBuffer;
            buffer->setData(QJsonDocument(body).toJson(QJsonDocument::Compact));
            buffer->open(QIODevice::ReadOnly);
            m_portalHttp.webRequestRaw(url, buffer, {{"Content-Type", "application/json"}}, operation, QStringLiteral("result"),
                          [this, taskId, operation, rating, account](QJsonValue result) {
                if (account != m_host.accountScopeRevision())
                    return;
                const auto task = result.toObject().value(QStringLiteral("task")).toObject();
                if (!matchesTaskIdentity(task, taskId) || taskRating(task) != rating)
                    return m_host.failShape(operation);
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
                    if (account != m_host.accountScopeRevision())
                        return;
                    if (result != true)
                        return m_host.failShape(operation);
                    emit taskUpdated(taskId);
                }, QStringLiteral("bitrix:tasks.task"), requestKey);
            return;
        }
        const auto postAction = [this, taskId, operation, requestKey](const QString &action, const QJsonObject &body) {
            m_portalHttp.ajaxJson(action, body, operation, [this, taskId, operation](QJsonValue result) {
                if (result == false)
                    return m_host.failShape(operation);
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
            const auto loaded = taskObserverIds(current.value(QStringLiteral("auditors")));
            if (!loaded)
                return m_host.failShape(operation);
            const auto before = *loaded;
            const auto beforeSet = QSet<QString>(before.cbegin(), before.cend());
            const auto baseline = QSet<QString>(edit.originalObserverIds->cbegin(), edit.originalObserverIds->cend());
            if (beforeSet != baseline) {
                emit failed(operation, QStringLiteral("Наблюдатели изменились на портале. Обновите задачу"));
                return;
            }
            const auto after = *edit.observerIds;
            const auto difference = [](const QStringList &source, const QStringList &other) {
                QStringList result;
                for (const auto &userId : source) {
                    if (!other.contains(userId))
                        result.append(userId);
                }
                return result;
            };
            const auto added = difference(after, before);
            const auto removed = difference(before, after);
            if (!added.isEmpty() && !removed.isEmpty()) {
                emit failed(operation, QStringLiteral("Добавление и удаление наблюдателей сохраните отдельно"));
                return;
            }
            if (added.isEmpty() && removed.isEmpty()) {
                emit taskUpdated(taskId);
                return;
            }
            const bool adding = !added.isEmpty();
            const auto changed = adding ? added : removed;
            const bool canWatch = current.value(QStringLiteral("rights")).toObject().value(QStringLiteral("watch")) == true;
            m_host.withCurrentUser(operation, [this, taskId, operation, id, changed, adding, canWatch,
                                        canManage = rights.observers](qint64 currentUser) {
                const bool self = changed.size() == 1 && changed.first().toLongLong() == currentUser;
                if (self ? !canWatch : !canManage) {
                    emit failed(operation, QStringLiteral("Портал не разрешает изменить состав наблюдателей"));
                    return;
                }
                QJsonArray people;
                for (const auto &userId : changed)
                    people.append(QJsonObject{{QStringLiteral("id"), userId.toLongLong()}});
                auto payload = id;
                payload.insert(QStringLiteral("auditors"), people);
                const auto action = self ? (adding ? "Task.Audit.watch" : "Task.Audit.unwatch")
                    : (adding ? "Task.Stakeholder.Auditor.add" : "Task.Stakeholder.Auditor.delete");
                QQueue<QPair<QString, QJsonObject>> changes;
                changes.enqueue({QStringLiteral("tasks.v2.") + QLatin1String(action), payload});
                updateTaskFields(taskId, std::move(changes));
            });
            return;
        }
        updateTaskFields(taskId, std::move(writes), requestKey);
    }, requestKey);
}

void TasksService::updateTaskFields(const QString &taskId, QQueue<QPair<QString, QJsonObject>> writes,
                                    const QString &requestKey)
{
    const auto scope = m_host.accountScopeRevision();
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
        if (!requestKey.isEmpty() && scope != m_host.accountScopeRevision())
            return;
        if (result == false)
            return m_host.failShape(operation);
        m_host.rememberTaskTitle(taskId, title);
        if (changedProject) {
            if (scope == m_host.accountScopeRevision())
                m_host.updateTaskChatProject(taskId, project);
            m_host.invalidateTaskChat(taskId);
        }
        updateTaskFields(taskId, std::move(writes), requestKey);
    }, requestKey);
}

void TasksService::loadTask(const QString &taskId, const QString &operation,
                            std::function<void(QJsonObject)> onTask, const QString &requestKey, bool remember)
{
    if (numericId(taskId).isEmpty()) {
        emit failed(operation, QStringLiteral("Некорректный номер задачи"));
        return;
    }
    Fields fields{{QStringLiteral("taskId"), taskId}};
    for (const auto *name : {"ID", "TITLE", "STAGE_ID", "DESCRIPTION", "RESPONSIBLE_ID", "DEADLINE", "PRIORITY",
                            "CHAT_ID", "UF_TASK_WEBDAV_FILES", "CREATED_BY", "CREATED_DATE", "GROUP_ID", "STATUS", "AUDITORS", "TAGS", "MARK"})
        fields.append({QStringLiteral("select[]"), QLatin1String(name)});
    m_portalHttp.ajax(QStringLiteral("tasks.task.get"), fields, operation,
         [this, taskId, operation, onTask = std::move(onTask), remember, scope = m_host.accountScopeRevision(),
          bindingRevision = m_host.unreadBindingRevision()](QJsonValue data) {
        if (remember && scope != m_host.accountScopeRevision())
            return;
        const auto task = data.toObject().value(QStringLiteral("task")).toObject();
        if (!remember) {
            onTask(matchesTaskIdentity(task, taskId) ? task : QJsonObject{});
            return;
        }
        if (!matchesTaskIdentity(task, taskId))
            return m_host.failShape(operation);
        m_host.rememberLoadedTask(taskId, task, bindingRevision);
        onTask(task);
    }, requestKey);
}

QUrl TasksService::taskRoleAvatar(const QJsonObject &current, const QJsonObject &legacy, const QString &personId) const
{
    if (personId.isEmpty())
        return {};
    if (numericId(field(current, {"id"})) == personId && current.contains(QStringLiteral("image")))
        return m_host.avatarUrlFromPortal(field(current.value(QStringLiteral("image")).toObject(), {"src"}));
    if (numericId(field(legacy, {"id"})) == personId) {
        const auto icon = field(legacy, {"icon"});
        return m_host.avatarUrlFromPortal(icon.isEmpty() ? field(legacy.value(QStringLiteral("image")).toObject(), {"src"}) : icon);
    }
    return {};
}

void TasksService::requestTask(const QString &taskId)
{
    const auto operation = operationKey(OperationKind::Task, taskId);
    if (numericId(taskId) != taskId || taskId.isEmpty()) {
        emit failed(operation, QStringLiteral("Некорректный номер задачи"));
        return;
    }
    if (!m_host.started())
        return;
    if (const auto existing = m_taskCardReads.find(taskId); existing != m_taskCardReads.end()) {
        // An initial duplicate shares both requests; a refresh after the base reply needs a fresh pair.
        existing->refresh |= existing->baseFinished;
        return;
    }
    m_taskCardReads.insert(taskId, TaskCardRead{});
    m_taskAttachmentRevisions.remove(taskId);
    const auto account = m_host.accountScopeRevision();
    const auto key = operationKey(OperationKind::TaskCardRead, QString::number(account), taskId);
    loadTask(taskId, operation, [this, taskId, account](const QJsonObject &data) {
        if (account != m_host.accountScopeRevision())
            return;
        const auto current = m_taskCardReads.find(taskId);
        if (current == m_taskCardReads.end())
            return;
        current->base = data;
        current->baseFinished = true;
        if (!current->detailsFinished) {
            auto summary = taskFromRow(data);
            summary.id = taskId;
            if (summary.stageName.isEmpty())
                summary.stageName = stageLabel(summary.stageId);
            emit taskSummaryLoaded(summary);
        }
        if (account == m_host.accountScopeRevision())
            finishTaskCardRead(taskId);
    }, key + QStringLiteral(":base"));

    const auto current = m_taskCardReads.find(taskId);
    if (account != m_host.accountScopeRevision() || current == m_taskCardReads.end())
        return;
    if (current->baseFinished && current->base.isEmpty()) {
        // A synchronous rejection of the mandatory read (for example expired authentication).
        current->detailsFinished = true;
        finishTaskCardRead(taskId);
        return;
    }
    const QJsonObject id{{QStringLiteral("id"), taskId.toLongLong()}};
    m_portalHttp.ajaxJson(QStringLiteral("tasks.v2.Task.get"), {{QStringLiteral("task"), id}},
             operationKey(OperationKind::TaskDetails, taskId), [this, taskId, account](QJsonValue element) {
        if (account != m_host.accountScopeRevision())
            return;
        const auto read = m_taskCardReads.find(taskId);
        if (read == m_taskCardReads.end())
            return;
        const auto card = element.toObject();
        if (matchesTaskIdentity(card, taskId))
            read->details = card;
        read->detailsFinished = true;
        finishTaskCardRead(taskId);
    }, key + QStringLiteral(":details"));
}

void TasksService::finishTaskCardRead(const QString &taskId)
{
    const auto it = m_taskCardReads.find(taskId);
    if (it == m_taskCardReads.end() || !it->baseFinished || !it->detailsFinished)
        return;
    const auto read = std::move(it.value());
    m_taskCardReads.erase(it);
    if (read.refresh) {
        requestTask(taskId);
        return;
    }
    if (read.base.isEmpty())
        return;
    auto task = taskFromRow(read.base);
    task.id = taskId;
    if (!read.details.isEmpty())
        applyTaskCardDetails(task, read.details);
    const auto creator = QStringLiteral("creator");
    const auto responsible = QStringLiteral("responsible");
    task.creatorAvatarUrl = taskRoleAvatar(read.details.value(creator).toObject(),
                                         read.base.value(creator).toObject(), task.creatorId);
    task.assigneeAvatarUrl = taskRoleAvatar(read.details.value(responsible).toObject(),
                                          read.base.value(responsible).toObject(), task.assigneeId);
    const auto legacyObservers = taskObserversById(read.base.value(QStringLiteral("auditorsData")));
    const auto currentObservers = taskObserversById(read.details.value(QStringLiteral("auditors")).toArray());
    for (auto &observer : task.observerPeople)
        observer.avatarUrl = taskRoleAvatar(currentObservers.value(observer.id), legacyObservers.value(observer.id), observer.id);
    if (task.stageName.isEmpty())
        task.stageName = stageLabel(task.stageId);
    loadTaskAttachments(std::move(task), read.base);
}

void TasksService::loadTaskChecklist(const QString &taskId, const QString &operation,
                                    std::function<void(QJsonArray, QJsonValue)> handler)
{
    if (numericId(taskId).isEmpty()) {
        emit failed(operation, QStringLiteral("Номер задачи недействителен"));
        return;
    }
    m_portalHttp.ajaxJson(QStringLiteral("tasks.v2.CheckList.get"), {{QStringLiteral("taskId"), taskId.toLongLong()}},
             operation, [this, operation, handler = std::move(handler)](QJsonValue data) {
        const auto object = data.toObject();
        const auto status = object.value(QStringLiteral("status"));
        const auto rows = data.isArray() ? data : object.value(QStringLiteral("checklist"));
        if (!rows.isArray() || (!status.isUndefined() && status != QLatin1String("success")))
            return m_host.failShape(operation);
        handler(rows.toArray(), object.value(QStringLiteral("revision")));
    });
}

void TasksService::requestTaskChecklist(const QString &taskId)
{
    loadTaskChecklist(taskId, operationKey(OperationKind::Checklist, taskId), [this, taskId](QJsonArray rows, QJsonValue) {
        QVector<TaskChecklistItem> items;
        QSet<QString> seen;
        for (const auto value : rows) {
            const auto row = value.toObject();
            const auto id = numericId(field(row, {"id"}));
            if (id.isEmpty() || seen.contains(id))
                continue;
            seen.insert(id);
            items.append({id, numericId(field(row, {"parentId"})), field(row, {"title"}).left(1000),
                          row.value(QStringLiteral("isComplete")).toBool(),
                          row.value(QStringLiteral("actions")).toObject().value(QStringLiteral("toggle")).toBool()});
            if (items.size() == 500)
                break;
        }
        emit taskChecklistLoaded(taskId, items);
    });
}

void TasksService::updateTaskChecklist(const QString &taskId, const QString &action, QJsonObject payload)
{
    const auto operation = operationKey(OperationKind::ChecklistWrite, taskId);
    m_portalHttp.ajaxJson(action, payload, operation, [this, taskId, operation](QJsonValue data) {
        const auto status = data.toObject().value(QStringLiteral("status"));
        if (status == QLatin1String("conflict")) {
            emit failed(operation, QStringLiteral("Чек-лист изменился на портале. Повторите действие"));
            return;
        }
        if (data != true && !data.isArray() && status != QLatin1String("success")) {
            emit failed(operation, QStringLiteral("Портал не сохранил изменение чек-листа"));
            return;
        }
        emit taskChecklistChanged(taskId);
    });
}

void TasksService::addTaskChecklistItem(const QString &taskId, const QString &parentId, const QString &title)
{
    const auto operation = operationKey(OperationKind::ChecklistWrite, taskId);
    const auto name = title.trimmed();
    if (name.isEmpty() || name.size() > 300 || (!parentId.isEmpty() && numericId(parentId).isEmpty())) {
        emit failed(operation, QStringLiteral("Проверьте название пункта и номер чек-листа"));
        return;
    }
    loadTaskChecklist(taskId, operation, [this, taskId, parentId, name, operation](QJsonArray rows, QJsonValue revision) {
        if ((!revision.isDouble() && !revision.isString()) || rows.size() >= 499) {
            emit failed(operation, QStringLiteral("Портал не предоставил версию чек-листа или достигнут предел пунктов"));
            return;
        }
        QJsonValue parent = parentId.toLongLong();
        QString parentNode;
        int siblings = 0;
        int groups = 0;
        for (const auto value : std::as_const(rows)) {
            const auto row = value.toObject();
            const auto rowParent = numericId(field(row, {"parentId"}));
            if (rowParent.isEmpty())
                ++groups;
            if (!parentId.isEmpty() && rowParent == parentId)
                ++siblings;
            if (!parentId.isEmpty() && rowParent.isEmpty() && numericId(field(row, {"id"})) == parentId)
                parentNode = field(row, {"nodeId"});
        }
        const auto newNode = [] { return QUuid::createUuid().toString(QUuid::Id128); };
        if (parentId.isEmpty()) {
            parentNode = newNode();
            parent = parentNode;
            rows.append(QJsonObject{{QStringLiteral("id"), parent}, {QStringLiteral("nodeId"), parent},
                {QStringLiteral("parentId"), 0}, {QStringLiteral("parentNodeId"), 0},
                {QStringLiteral("sortIndex"), groups}, {QStringLiteral("title"), QStringLiteral("Чек-лист %1").arg(groups + 1)}});
        } else if (parentNode.isEmpty()) {
            emit failed(operation, QStringLiteral("Выбранный чек-лист больше недоступен"));
            return;
        }
        const auto node = newNode();
        rows.append(QJsonObject{{QStringLiteral("id"), node}, {QStringLiteral("nodeId"), node},
            {QStringLiteral("parentId"), parent}, {QStringLiteral("parentNodeId"), parentNode},
            {QStringLiteral("title"), name}, {QStringLiteral("sortIndex"), siblings}, {QStringLiteral("isComplete"), false}});
        updateTaskChecklist(taskId, QStringLiteral("tasks.v2.CheckList.save"),
            {{QStringLiteral("task"), QJsonObject{{QStringLiteral("id"), taskId.toLongLong()}, {QStringLiteral("checklist"), rows}}},
             {QStringLiteral("skipNotification"), false}, {QStringLiteral("baseRevision"), revision}});
    });
}

void TasksService::setTaskChecklistComplete(const QString &taskId, const QString &itemId, bool complete)
{
    const auto operation = operationKey(OperationKind::ChecklistWrite, taskId);
    loadTaskChecklist(taskId, operation, [this, taskId, itemId, complete, operation](QJsonArray rows, QJsonValue) {
        for (const auto value : rows) {
            auto row = value.toObject();
            if (itemId.isEmpty() || numericId(field(row, {"id"})) != itemId)
                continue;
            if (!row.value(QStringLiteral("actions")).toObject().value(QStringLiteral("toggle")).toBool())
                break;
            row.insert(QStringLiteral("isComplete"), complete);
            updateTaskChecklist(taskId, complete ? QStringLiteral("tasks.v2.CheckList.complete") : QStringLiteral("tasks.v2.CheckList.renew"),
                {{QStringLiteral("task"), QJsonObject{{QStringLiteral("id"), taskId.toLongLong()},
                                                     {QStringLiteral("checklist"), QJsonArray{row}}}}});
            return;
        }
        emit failed(operation, QStringLiteral("Пункт недоступен для изменения"));
    });
}

void TasksService::requestSubtasks(const QString &taskId)
{
    const auto operation = operationKey(OperationKind::Subtasks, taskId);
    if (numericId(taskId).isEmpty()) {
        emit failed(operation, QStringLiteral("Номер задачи недействителен"));
        return;
    }
    m_portalHttp.ajaxJson(QStringLiteral("tasks.v2.Task.Relation.Child.list"),
        {{QStringLiteral("taskId"), taskId.toLongLong()}, {QStringLiteral("withIds"), false},
         {QStringLiteral("withCompleted"), true}, {QStringLiteral("withSubTasks"), false},
         {QStringLiteral("navigation"), QJsonObject{{QStringLiteral("size"), tunables::kPageSizeChats}}}},
        operation, [this, taskId, operation](QJsonValue data) {
        const auto tasks = data.toObject().value(QStringLiteral("tasks"));
        if (!tasks.isArray())
            return m_host.failShape(operation);
        QVector<Task> result;
        for (const auto row : tasks.toArray()) {
            const auto object = row.toObject();
            const auto parent = field(object.value(QStringLiteral("parent")).toObject(), {"id"});
            auto task = taskFromRow(object);
            if (numericId(task.id).isEmpty() || task.id == taskId || (!parent.isEmpty() && parent != taskId))
                continue;
            if (!task.name.isEmpty())
                result.append(std::move(task));
            if (result.size() == tunables::kPageSizeChats)
                break;
        }
        emit subtasksLoaded(taskId, result);
    });
}

void TasksService::requestTaskDependencies(const QString &taskId)
{
    const auto operation = operationKey(OperationKind::GanttLinks, taskId);
    if (numericId(taskId).isEmpty()) {
        emit failed(operation, QStringLiteral("Номер задачи недействителен"));
        return;
    }
    m_portalHttp.ajaxJson(QStringLiteral("tasks.v2.Task.Relation.Gantt.Dependence.list"),
        {{QStringLiteral("taskId"), taskId.toLongLong()}, {QStringLiteral("withIds"), false},
         {QStringLiteral("withCompleted"), true}, {QStringLiteral("withSubTasks"), false},
         {QStringLiteral("navigation"), QJsonObject{{QStringLiteral("size"), tunables::kPageSizeChats}}}},
        operation, [this, taskId, operation](QJsonValue data) {
        const auto tasks = data.toObject().value(QStringLiteral("tasks"));
        if (!tasks.isArray())
            return m_host.failShape(operation);
        QHash<QString, QString> names;
        for (const auto entry : tasks.toArray()) {
            const auto row = entry.toObject();
            names.insert(field(row, {"id"}), field(row, {"title"}));
        }
        QVector<TaskDependency> links;
        QSet<QString> seen;
        for (const auto entry : tasks.toArray()) {
            const auto row = entry.toObject();
            const auto dependent = numericId(field(row, {"id"}));
            if (dependent.isEmpty())
                continue;
            const auto relations = row.value(QStringLiteral("ganttLinks")).toObject();
            for (auto it = relations.constBegin(); it != relations.constEnd(); ++it) {
                const auto source = numericId(it.key());
                if (source.isEmpty() || source == dependent || (source != taskId && dependent != taskId))
                    continue;
                const bool prerequisite = dependent == taskId;
                const auto related = prerequisite ? source : dependent;
                const auto key = source + QLatin1Char(':') + dependent;
                if (!seen.contains(key) && links.size() < tunables::kPageSizeChats) {
                    seen.insert(key);
                    links.append({related, names.value(related), string(it.value()), prerequisite});
                }
            }
        }
        emit taskDependenciesLoaded(taskId, links);
    });
}

void TasksService::requestTaskHistory(const QString &taskId)
{
    const auto operation = operationKey(OperationKind::TaskHistory, taskId);
    if (numericId(taskId).isEmpty()) {
        emit failed(operation, QStringLiteral("Номер задачи недействителен"));
        return;
    }
    const auto url = OriginGuard(m_host.portalOrigin()).resolve(QStringLiteral("/rest/tasks.task.history.list.json"));
    m_portalHttp.webRequest(url, {{QStringLiteral("taskId"), taskId}}, operation, QStringLiteral("result"),
               Handler::checked(QStringLiteral("История задачи недоступна"),
               [](const QJsonValue &data) { return data.toObject().value(QStringLiteral("list")).isArray(); },
               [this, taskId](QJsonValue data) {
        const auto list = data.toObject().value(QStringLiteral("list"));
        static const QHash<QString, QString> eventNames{
            {QStringLiteral("NEW"), QStringLiteral("Задача создана")},
            {QStringLiteral("TITLE"), QStringLiteral("Изменилось название")},
            {QStringLiteral("DESCRIPTION"), QStringLiteral("Изменилось описание")},
            {QStringLiteral("COMMENT"), QStringLiteral("Добавлен комментарий")},
            {QStringLiteral("STATUS"), QStringLiteral("Изменился статус")},
            {QStringLiteral("PRIORITY"), QStringLiteral("Изменился приоритет")},
            {QStringLiteral("DEADLINE"), QStringLiteral("Изменился срок")},
            {QStringLiteral("STAGE_ID"), QStringLiteral("Изменилась стадия")},
            {QStringLiteral("RESPONSIBLE_ID"), QStringLiteral("Изменился исполнитель")},
            {QStringLiteral("GROUP_ID"), QStringLiteral("Изменился проект")},
            {QStringLiteral("AUDITORS"), QStringLiteral("Изменились наблюдатели")},
            {QStringLiteral("CHECKLIST_ITEM_CREATE"), QStringLiteral("Добавлен пункт чек-листа")}
        };
        const auto rows = list.toArray();
        QVector<TaskHistoryEntry> history;
        QSet<QString> seen;
        for (const auto value : rows) {
            const auto row = value.toObject();
            const auto id = numericId(field(row, {"id"}));
            const auto event = field(row, {"field"});
            if (id.isEmpty() || event.isEmpty() || seen.contains(id))
                continue;
            seen.insert(id);
            const auto user = row.value(QStringLiteral("user")).toObject();
            const auto author = (field(user, {"name"}) + QLatin1Char(' ') + field(user, {"lastName"})).trimmed().left(160);
            QString detail;
            if (event == QLatin1String("TITLE") || event == QLatin1String("DEADLINE")) {
                const auto change = row.value(QStringLiteral("value")).toObject();
                const auto from = change.value(QStringLiteral("from")).toString().simplified().left(120);
                const auto to = change.value(QStringLiteral("to")).toString().simplified().left(120);
                if (!from.isEmpty() || !to.isEmpty())
                    detail = QStringLiteral("%1 → %2").arg(from.isEmpty() ? QStringLiteral("—") : from,
                                                          to.isEmpty() ? QStringLiteral("—") : to);
            }
            history.append({id, eventNames.value(event, QStringLiteral("Задача изменена")),
                            author, date(row.value(QStringLiteral("createdDate"))), detail});
            if (history.size() == tunables::kPageSizeChats)
                break;
        }
        emit taskHistoryLoaded(taskId, history, rows.size() >= tunables::kPageSizeChats);
    }));
}

void TasksService::loadTaskAttachments(Task task, const QJsonObject &row)
{
    if (!m_taskAttachmentRevisions.contains(task.id) && m_taskAttachmentRevisions.size() >= tunables::kPageSizeChats)
        m_taskAttachmentRevisions.erase(m_taskAttachmentRevisions.begin());
    const auto revision = ++m_taskAttachmentRevision;
    m_taskAttachmentRevisions.insert(task.id, revision);
    auto source = row.value(QStringLiteral("ufTaskWebdavFiles"));
    if (!source.isArray())
        source = row.value(QStringLiteral("UF_TASK_WEBDAV_FILES"));
    if (!source.isArray())
        source = row.value(QStringLiteral("fileIds"));
    QJsonArray ids;
    QSet<QString> requested;
    for (const auto value : source.toArray()) {
        const auto id = taskFileId(string(value));
        if (id.isEmpty() || requested.contains(id))
            continue;
        requested.insert(id);
        ids.append(id.startsWith(QLatin1Char('n')) ? QJsonValue(id) : QJsonValue(id.toLongLong()));
        if (ids.size() == maxAttachmentNames)
            break;
    }

    emit taskLoaded(task);
    if (ids.isEmpty())
        return;
    const auto operation = operationKey(OperationKind::TaskFiles, task.id);
    const auto key = operation + QLatin1Char(':') + QString::number(revision);
    m_portalHttp.ajaxJson(QStringLiteral("tasks.v2.File.listObjects"), {{QStringLiteral("ids"), ids}},
             operation, [this, task = std::move(task), ids, operation, revision](QJsonValue data) mutable {
        if (m_taskAttachmentRevisions.value(task.id) != revision)
            return;
        if (!data.isArray()) {
            m_host.failShape(operation);
            return;
        }
        const auto files = responseFiles(data);
        for (const auto id : ids) {
            const auto fileId = string(id);
            const auto metadata = files.value(fileId);
            const auto name = plainFileName(field(metadata, {"name"}));
            if (name.isEmpty())
                continue;
            bool sizeKnown = false;
            const auto size = metadata.value(QStringLiteral("size")).toVariant().toLongLong(&sizeKnown);
            const auto exactSize = sizeKnown && size >= 0 ? size : -1;
            const bool photo = metadata.value(QStringLiteral("isImage")) == true;
            const auto original = m_host.taskFileUrl(field(metadata, {"downloadUrl"}), exactSize, photo);
            MessageFile file{fileId, name, exactSize, original};
            file.isImage = photo;
            auto diskObject = taskFileId(field(metadata.value(QStringLiteral("customData")).toObject(), {"objectId"}));
            if (!diskObject.isEmpty() && !diskObject.startsWith(QLatin1Char('n')))
                file.diskObjectId = QLatin1Char('n') + diskObject;
            if (fileId.startsWith(QLatin1Char('n')))
                diskObject = fileId.mid(1);
            if (photo) {
                file.previewUrl = m_host.taskImageUrl(field(metadata, {"serverPreviewUrl"}), fileId, diskObject);
                const auto width = field(metadata, {"serverPreviewWidth", "width"}).toLongLong();
                const auto height = field(metadata, {"serverPreviewHeight", "height"}).toLongLong();
                if (!file.previewUrl.isEmpty() && width > 0 && height > 0 && width <= 1048576 && height <= 1048576)
                    file.previewSize = QSize(static_cast<int>(width), static_cast<int>(height));
                if (file.previewUrl.isEmpty() && !original.isEmpty() && exactSize > 0
                    && exactSize <= m_host.taskImageMaxBytes())
                    file.previewUrl = m_host.taskImageUrl(original.toString(QUrl::FullyEncoded), fileId, diskObject, true);
            }
            task.attachments.append(file.name);
            task.files.append(std::move(file));
        }
        if (!task.attachments.isEmpty())
            emit taskLoaded(task);
    }, key);
}

void TasksService::attachTaskFile(const QString &taskId, const QString &localPath)
{
    const auto operation = operationKey(OperationKind::AttachTask, taskId);
    if (numericId(taskId).isEmpty()) {
        emit failed(operation, QStringLiteral("Некорректный номер задачи"));
        return;
    }
    m_host.uploadTaskFile(taskId, localPath, operation);
}

void TasksService::attachUploadedFile(const QString &taskId, const QString &fileId, const QString &operation)
{
    const QJsonObject task{{QStringLiteral("id"), taskId.toLongLong()}};
    m_portalHttp.ajaxJson(QStringLiteral("tasks.v2.File.attach"),
        {{QStringLiteral("task"), task}, {QStringLiteral("ids"), QJsonArray{fileId}}},
        operation, [this, taskId, fileId, operation](QJsonValue result) {
            if (result.isBool() && !result.toBool())
                return m_host.failShape(operation);
            emit taskFileAttached(taskId, fileId);
            emit fileUploaded(taskId, QStringLiteral("task"));
        });
}

} // namespace bx
