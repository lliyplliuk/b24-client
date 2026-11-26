#include "cloud/cloud_rows.h"
#include "core/tunables.h"

#include <QJsonArray>
#include <QSet>

namespace bx::cloud_rows {

QString string(const QJsonValue &value)
{
    if (value.isDouble())
        return QString::number(value.toInteger());
    return value.toString();
}

QString field(const QJsonObject &object, std::initializer_list<const char *> keys)
{
    for (const char *key : keys) {
        const auto result = string(object.value(QLatin1String(key)));
        if (!result.isEmpty())
            return result;
    }
    return {};
}

QString numericId(const QString &candidate)
{
    bool ok = false;
    const auto number = candidate.toLongLong(&ok);
    return ok && number > 0 ? QString::number(number) : QString();
}

QString chatDialog(const QString &chatId)
{
    const QString id = numericId(chatId);
    return id.isEmpty() ? QString() : QStringLiteral("chat") + id;
}

qint64 explicitMessageChatId(const QJsonObject &message)
{
    qint64 chatId = 0;
    for (const auto *key : {"chatId", "chat_id", "CHAT_ID"}) {
        const auto value = message.value(QLatin1String(key));
        if (value.isUndefined())
            continue;
        const auto id = numericId(string(value)).toLongLong();
        if (id <= 0 || (chatId > 0 && id != chatId))
            return -1;
        chatId = id;
    }
    return chatId;
}

std::optional<bool> notificationFlag(const QJsonValue &value)
{
    if (value.isBool())
        return value.toBool();
    if (value == QLatin1String("Y"))
        return true;
    if (value == QLatin1String("N"))
        return false;
    return std::nullopt;
}

bool boardStageId(const QString &id)
{
    return id == QLatin1String("0") || !numericId(id).isEmpty();
}

QJsonValue oneOf(const QJsonObject &object, std::initializer_list<const char *> keys)
{
    for (const char *key : keys) {
        const auto value = object.value(QLatin1String(key));
        if (!value.isUndefined() && !value.isNull())
            return value;
    }
    return QJsonValue(QJsonValue::Undefined);
}

QJsonValue list(const QJsonValue &data, std::initializer_list<const char *> keys)
{
    return data.isArray() ? data : oneOf(data.toObject(), keys);
}

QDateTime date(const QJsonValue &value)
{
    if (value.isDouble() && value.toInteger() > 0)
        return QDateTime::fromSecsSinceEpoch(value.toInteger());
    return QDateTime::fromString(value.toString(), Qt::ISODate);
}

QString stageLabel(const QString &id)
{
    return id.isEmpty() || id == QLatin1String("0")
        ? QStringLiteral("Без стадии")
        : QStringLiteral("Стадия №%1 · название недоступно").arg(id);
}

QStringList taskTags(const QJsonValue &value)
{
    QStringList tags;
    const auto add = [&tags](QString tag) {
        tag = tag.trimmed();
        if (!tag.isEmpty() && tag.size() <= 256 && !tags.contains(tag))
            tags.append(std::move(tag));
    };
    if (value.isArray()) {
        const auto rows = value.toArray();
        for (qsizetype i = 0; i < qMin<qsizetype>(rows.size(), 128) && tags.size() < 32; ++i) {
            const auto item = rows.at(i);
            add(item.isString() ? item.toString() : field(item.toObject(), {"name"}));
        }
    } else if (value.isObject()) {
        const auto rows = value.toObject();
        int scanned = 0;
        for (auto item = rows.constBegin(); item != rows.constEnd() && scanned < 128 && tags.size() < 32; ++item, ++scanned) {
            if (!numericId(item.key()).isEmpty())
                add(field(item.value().toObject(), {"title"}));
        }
    }
    return tags;
}

template<typename Visit>
void visitTaskObservers(const QJsonValue &rows, Visit visit)
{
    const auto limit = tunables::kPageSizeChats;
    if (rows.isArray()) {
        const auto people = rows.toArray();
        for (qsizetype i = 0; i < people.size() && i < limit; ++i)
            visit(people.at(i).toObject());
    } else if (rows.isObject()) {
        const auto people = rows.toObject();
        auto it = people.begin();
        for (int i = 0; it != people.end() && i < limit; ++it, ++i)
            visit(it.value().toObject());
    }
}

void appendTaskObservers(Task &task, const QJsonValue &rows)
{
    QSet<QString> seen;
    visitTaskObservers(rows, [&task, &seen](const QJsonObject &person) {
        const auto id = numericId(field(person, {"id"}));
        const auto name = field(person, {"name", "NAME"});
        if (!id.isEmpty() && !seen.contains(id)) {
            seen.insert(id);
            task.observerPeople.append({id, name});
        }
        if (!name.isEmpty() && !task.observers.contains(name))
            task.observers.append(name);
    });
}

QHash<QString, QJsonObject> taskObserversById(const QJsonValue &rows)
{
    QHash<QString, QJsonObject> people;
    visitTaskObservers(rows, [&people](const QJsonObject &person) {
        const auto id = numericId(field(person, {"id"}));
        if (!id.isEmpty() && !people.contains(id))
            people.insert(id, person);
    });
    return people;
}

bool matchesTaskIdentity(const QJsonObject &row, const QString &taskId)
{
    bool supplied = false;
    for (const auto *key : {"id", "ID"}) {
        const auto value = row.value(QLatin1String(key));
        if (value.isUndefined())
            continue;
        supplied = true;
        if (numericId(string(value)) != taskId)
            return false;
    }
    return supplied && !taskId.isEmpty();
}

std::optional<TaskRating> taskRating(const QJsonObject &row)
{
    std::optional<TaskRating> result;
    for (const auto *key : {"mark", "MARK"}) {
        const auto value = row.value(QLatin1String(key));
        if (value.isUndefined())
            continue;
        const auto mark = value.toString().toLower();
        auto rating = TaskRating::Unrated;
        if (mark == QLatin1String("p") || mark == QLatin1String("positive"))
            rating = TaskRating::Positive;
        else if (mark == QLatin1String("n") || mark == QLatin1String("negative"))
            rating = TaskRating::Negative;
        else if (!value.isNull() && mark != QLatin1String("none"))
            return std::nullopt;
        if (result && *result != rating)
            return std::nullopt;
        result = rating;
    }
    return result;
}

Task taskFromRow(const QJsonObject &row)
{
    Task task;
    task.id = field(row, {"id", "ID"});
    task.name = field(row, {"title", "TITLE"});
    task.tags = taskTags(row.value(QStringLiteral("tags")));
    task.stageId = field(row, {"stageId", "STAGE_ID"});
    task.stageName = field(row, {"stageName", "STAGE_NAME"});
    task.description = field(row, {"description", "DESCRIPTION"});
    task.due = field(row, {"deadline", "DEADLINE"});
    task.priority = field(row, {"priority", "PRIORITY"});
    task.rating = taskRating(row).value_or(TaskRating::Unrated);
    const auto responsible = row.value(QStringLiteral("responsible")).toObject();
    const auto creator = row.value(QStringLiteral("creator")).toObject();
    task.assignee = field(responsible, {"name", "NAME"});
    task.createdBy = field(creator, {"name", "NAME"});
    task.assigneeId = numericId(field(row, {"responsibleId", "RESPONSIBLE_ID"}));
    task.creatorId = numericId(field(row, {"createdBy", "CREATED_BY"}));
    if (task.assigneeId.isEmpty())
        task.assigneeId = numericId(field(responsible, {"id"}));
    if (task.creatorId.isEmpty())
        task.creatorId = numericId(field(creator, {"id"}));
    const auto group = row.value(QStringLiteral("group")).toObject();
    task.project = field(group, {"name", "NAME"});
    task.projectId = numericId(field(row, {"groupId", "GROUP_ID"}));
    if (task.projectId.isEmpty())
        task.projectId = numericId(field(group, {"id", "ID"}));
    task.createdAt = QDateTime::fromString(field(row, {"createdDate", "CREATED_DATE"}), Qt::ISODate);
    task.descriptionChecksum = field(row, {"descriptionChecksum"});
    task.chatDialogId = chatDialog(QString::number(explicitMessageChatId(row)));
    task.planStart = date(oneOf(row, {"startDatePlan", "START_DATE_PLAN", "startPlanTs"}));
    task.planEnd = date(oneOf(row, {"endDatePlan", "END_DATE_PLAN", "endPlanTs"}));
    const auto status = field(row, {"status", "STATUS"});
    static const QHash<QString, QString> statusLabels{
        {QStringLiteral("1"), QStringLiteral("Новая")},
        {QStringLiteral("2"), QStringLiteral("Ждёт выполнения")},
        {QStringLiteral("3"), QStringLiteral("Выполняется")},
        {QStringLiteral("4"), QStringLiteral("Ждёт контроля")},
        {QStringLiteral("5"), QStringLiteral("Завершена")},
        {QStringLiteral("6"), QStringLiteral("Отложена")},
        {QStringLiteral("7"), QStringLiteral("Отклонена")}
    };
    task.status = statusLabels.value(status);
    task.statusCode = status;
    appendTaskObservers(task, row.value(QStringLiteral("auditorsData")));
    return task;
}

} // namespace bx::cloud_rows
