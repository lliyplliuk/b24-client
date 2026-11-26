#include "cloud/tasks_service.h"
#include "cloud/cloud_rows.h"
#include "core/origin_guard.h"
#include "core/tunables.h"

#include <QSet>

namespace bx {
using namespace cloud_rows;

void TasksService::requestTaskHistory(const QString &taskId)
{
    const auto operation = operationKey(OperationKind::TaskHistory, taskId);
    if (numericId(taskId).isEmpty()) {
        emit failed(operation, QStringLiteral("Номер задачи недействителен"));
        return;
    }
    const auto url = OriginGuard(m_session.portalOrigin()).resolve(QStringLiteral("/rest/tasks.task.history.list.json"));
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

} // namespace bx
