#include "cloud/tasks_service.h"
#include "cloud/cloud_rows.h"
#include "cloud/task_card_fields.h"

#include <utility>

namespace bx {
using namespace cloud_rows;
using namespace task_card_fields;

TasksService::TasksService(CloudServiceContext &session, TaskChatHost &taskChats, TaskFileHost &files,
                           PortalHttp &http, QObject *parent)
    : QObject(parent), m_session(session), m_taskChats(taskChats), m_files(files), m_portalHttp(http)
{
}

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
         [this, taskId, operation, onTask = std::move(onTask), remember, scope = m_session.accountScopeRevision(),
          bindingRevision = m_taskChats.unreadBindingRevision()](QJsonValue data) {
        if (remember && scope != m_session.accountScopeRevision())
            return;
        const auto task = data.toObject().value(QStringLiteral("task")).toObject();
        if (!remember) {
            onTask(matchesTaskIdentity(task, taskId) ? task : QJsonObject{});
            return;
        }
        if (!matchesTaskIdentity(task, taskId))
            return m_session.failShape(operation);
        m_taskChats.rememberLoadedTask(taskId, task, bindingRevision);
        onTask(task);
    }, requestKey);
}

QUrl TasksService::taskRoleAvatar(const QJsonObject &current, const QJsonObject &legacy, const QString &personId) const
{
    if (personId.isEmpty())
        return {};
    if (numericId(field(current, {"id"})) == personId && current.contains(QStringLiteral("image")))
        return m_session.avatarUrlFromPortal(field(current.value(QStringLiteral("image")).toObject(), {"src"}));
    if (numericId(field(legacy, {"id"})) == personId) {
        const auto icon = field(legacy, {"icon"});
        return m_session.avatarUrlFromPortal(icon.isEmpty() ? field(legacy.value(QStringLiteral("image")).toObject(), {"src"}) : icon);
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
    if (!m_session.started())
        return;
    if (const auto existing = m_taskCardReads.find(taskId); existing != m_taskCardReads.end()) {
        // An initial duplicate shares both requests; a refresh after the base reply needs a fresh pair.
        existing->refresh |= existing->baseFinished;
        return;
    }
    m_taskCardReads.insert(taskId, TaskCardRead{});
    m_taskAttachmentRevisions.remove(taskId);
    const auto account = m_session.accountScopeRevision();
    const auto key = operationKey(OperationKind::TaskCardRead, QString::number(account), taskId);
    loadTask(taskId, operation, [this, taskId, account](const QJsonObject &data) {
        if (account != m_session.accountScopeRevision())
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
        if (account == m_session.accountScopeRevision())
            finishTaskCardRead(taskId);
    }, key + QStringLiteral(":base"));

    const auto current = m_taskCardReads.find(taskId);
    if (account != m_session.accountScopeRevision() || current == m_taskCardReads.end())
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
        if (account != m_session.accountScopeRevision())
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

} // namespace bx
