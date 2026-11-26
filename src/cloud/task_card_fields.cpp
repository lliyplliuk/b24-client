#include "cloud/task_card_fields.h"
#include "cloud/cloud_rows.h"
#include "core/tunables.h"

#include <QJsonArray>
#include <QSet>

namespace bx::task_card_fields {
using namespace cloud_rows;
namespace {

const QPair<TaskStatusAction, const char *> taskStatusActions[]{
    {TaskStatusAction::Start, "start"}, {TaskStatusAction::Pause, "pause"},
    {TaskStatusAction::Complete, "complete"}, {TaskStatusAction::Renew, "renew"},
    {TaskStatusAction::Defer, "defer"}, {TaskStatusAction::Approve, "approve"},
    {TaskStatusAction::Disapprove, "disapprove"}, {TaskStatusAction::Take, "take"}
};

bool singleTaskAssignee(const QJsonObject &card)
{
    return !numericId(field(card.value(QStringLiteral("responsible")).toObject(), {"id"})).isEmpty()
        && card.value(QStringLiteral("responsibleCollection")).toArray().size() <= 1
        && card.value(QStringLiteral("multiResponsibles")).toArray().isEmpty()
        && card.value(QStringLiteral("isMultitask")) != true;
}

} // namespace

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

} // namespace bx::task_card_fields
