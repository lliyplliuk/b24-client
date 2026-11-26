#include "cloud/tasks_service.h"
#include "cloud/cloud_rows.h"
#include "core/tunables.h"

#include <QSet>
#include <utility>

namespace bx {
using namespace cloud_rows;

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
            return m_session.failShape(operation);
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
            return m_session.failShape(operation);
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

} // namespace bx
