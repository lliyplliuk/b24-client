#include "cloud/tasks_service.h"
#include "cloud/cloud_rows.h"

#include <QSet>
#include <QUuid>
#include <utility>

namespace bx {
using namespace cloud_rows;

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
            return m_session.failShape(operation);
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

} // namespace bx
