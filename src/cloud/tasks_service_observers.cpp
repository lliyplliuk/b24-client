#include "cloud/tasks_service.h"
#include "cloud/task_card_fields.h"

#include <QSet>
#include <utility>

namespace bx {
using namespace task_card_fields;

void TasksService::updateTaskObservers(const QString &taskId, const QJsonObject &current, const QStringList &original,
                                       const QStringList &after, bool canManage)
{
    const auto operation = operationKey(OperationKind::UpdateTask, taskId);
    const auto loaded = taskObserverIds(current.value(QStringLiteral("auditors")));
    if (!loaded)
        return m_session.failShape(operation);
    const auto before = *loaded;
    const auto beforeSet = QSet<QString>(before.cbegin(), before.cend());
    const auto baseline = QSet<QString>(original.cbegin(), original.cend());
    if (beforeSet != baseline) {
        emit failed(operation, QStringLiteral("Наблюдатели изменились на портале. Обновите задачу"));
        return;
    }
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
    m_session.withCurrentUser(operation, [this, taskId, operation, changed, adding, canWatch, canManage](qint64 currentUser) {
        const bool self = changed.size() == 1 && changed.first().toLongLong() == currentUser;
        if (self ? !canWatch : !canManage) {
            emit failed(operation, QStringLiteral("Портал не разрешает изменить состав наблюдателей"));
            return;
        }
        QJsonArray people;
        for (const auto &userId : changed)
            people.append(QJsonObject{{QStringLiteral("id"), userId.toLongLong()}});
        QJsonObject payload{{QStringLiteral("id"), taskId.toLongLong()}};
        payload.insert(QStringLiteral("auditors"), people);
        const auto action = self ? (adding ? "Task.Audit.watch" : "Task.Audit.unwatch")
            : (adding ? "Task.Stakeholder.Auditor.add" : "Task.Stakeholder.Auditor.delete");
        QQueue<QPair<QString, QJsonObject>> changes;
        changes.enqueue({QStringLiteral("tasks.v2.") + QLatin1String(action), payload});
        updateTaskFields(taskId, std::move(changes));
    });
}

} // namespace bx
