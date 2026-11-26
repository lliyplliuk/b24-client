#include "cloud_chat_metadata.h"
#include "cloud_rows.h"

#include <algorithm>
#include <limits>

namespace bx {
using namespace cloud_rows;

QString validDialog(const QString &candidate)
{
    return candidate.startsWith(QLatin1String("chat")) ? chatDialog(candidate.mid(4)) : numericId(candidate);
}

bool matchesRequestedHistory(const QJsonObject &history, const QString &dialog, qint64 expectedChatId)
{
    const auto chat = history.value(QStringLiteral("chat")).toObject();
    const auto declaredId = chat.value(QStringLiteral("id"));
    const auto declaredDialog = chat.value(QStringLiteral("dialogId"));
    const auto matchesId = [expectedChatId](qint64 value) {
        return value >= 0 && (expectedChatId <= 0 || value == 0 || value == expectedChatId);
    };
    const auto declaredNumericId = numericId(string(declaredId)).toLongLong();
    return (declaredId.isUndefined() || (declaredNumericId > 0 && matchesId(declaredNumericId)))
        && (declaredDialog.isUndefined() || validDialog(string(declaredDialog)) == dialog)
        && matchesId(explicitMessageChatId(chat)) && matchesId(explicitMessageChatId(history));
}

qint64 recentMessageId(const QJsonObject &message)
{
    return numericId(field(message, {"id", "ID"})).toLongLong();
}

std::optional<qint64> explicitMessageAuthorId(const QJsonObject &message)
{
    std::optional<qint64> author;
    for (const auto *key : {"senderId", "authorId", "author_id", "AUTHOR_ID"}) {
        const auto value = message.value(QLatin1String(key));
        if (value.isUndefined())
            continue;
        bool valid = true;
        const auto id = value.isString() ? value.toString().toLongLong(&valid) : value.toInteger(-1);
        if (!valid || id < 0 || (value.isString() && value.toString() != QString::number(id)) || (author && *author != id))
            return std::nullopt;
        author = id;
    }
    return author;
}

qint64 groupDialogChatId(const QString &dialog)
{
    return dialog.startsWith(QLatin1String("chat")) ? dialog.sliced(4).toLongLong() : 0;
}

bool isTaskChatType(const QString &type)
{
    const auto normalized = type.toLower();
    return normalized == QLatin1String("taskstask") || normalized == QLatin1String("tasks")
        || normalized == QLatin1String("tasks_task");
}

bool isTaskChatMetadata(const QString &kind, const QJsonObject &entity)
{
    return isTaskChatType(kind) || isTaskChatType(field(entity, {"type"}));
}

bool isRecentTaskChat(bool metadataTaskChat, bool listedTask, const QJsonArray &sections)
{
    return metadataTaskChat || listedTask || sections.contains(QStringLiteral("tasksTask"));
}

bool mutedListContains(const QJsonValue &list, const QString &user)
{
    if (user.isEmpty())
        return false;
    if (list.isArray()) {
        const auto entries = list.toArray();
        return std::any_of(entries.cbegin(), entries.cend(), [&user](const QJsonValue &entry) { return string(entry) == user; });
    }
    return list.toObject().value(user).toBool();
}

std::optional<bool> dismissedPersonalUser(const QString &dialog, const QJsonObject &row, const QJsonObject &user)
{
    if (numericId(dialog).isEmpty())
        return false;
    const auto invited = row.value(QStringLiteral("invited"));
    if (invited.isObject() || invited == true)
        return false;
    const auto userId = numericId(field(user, {"id", "ID"}));
    if (!userId.isEmpty() && userId != dialog)
        return std::nullopt;
    const auto userType = field(user, {"type"});
    for (const auto *externalType : {"extranet", "collaber", "guest", "bot", "network", "connector"}) {
        if (userType == QLatin1String(externalType))
            return false;
    }
    for (const auto *flag : {"extranet", "network", "bot", "connector"}) {
        if (user.value(QLatin1String(flag)) == true)
            return false;
    }
    // The web messenger checks active independently of directory visibility and login method.
    const auto active = user.value(QStringLiteral("active"));
    return active.isBool() ? std::optional<bool>(!active.toBool()) : std::nullopt;
}

bool isPersonalMetadata(const QString &dialog, const QString &type)
{
    return !numericId(dialog).isEmpty() && (type.isEmpty() || type == QLatin1String("user")
        || type == QLatin1String("private") || type == QLatin1String("im"));
}

QString userName(const QJsonObject &user)
{
    if (const QString name = field(user, {"name", "NAME"}); !name.isEmpty())
        return name;
    return (field(user, {"firstName", "FIRST_NAME"}) + QLatin1Char(' ')
            + field(user, {"lastName", "LAST_NAME"})).trimmed();
}

std::optional<int> chatUnreadCount(const QJsonValue &value)
{
    bool parsed = true;
    const auto count = value.isString() ? value.toString().toLongLong(&parsed) : value.toInteger(-1);
    return parsed && count >= 0 && count <= std::numeric_limits<int>::max()
        ? std::optional<int>(int(count)) : std::nullopt;
}

} // namespace bx
