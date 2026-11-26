#include "cloud/cloud_backend.h"

#include "core/origin_guard.h"
#include "core/notification_targets.h"
#include "core/stage_color.h"
#include "core/tunables.h"
#include "signin/session_capture.h"
#include "signin/session_restore.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QLoggingCategory>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSettings>
#include <QUrlQuery>
#include <QUuid>
#include <algorithm>
#include <memory>
#include <limits>
#include <utility>

namespace bx {
namespace {

Q_LOGGING_CATEGORY(sessionLog, "bitrix24.session")

const QString recentOp = QStringLiteral("chats");
const QString taskRecentOp = QStringLiteral("task-chats");
const QString projectTaskChatsPrefix = QStringLiteral("project-task-chats:");
constexpr int projectTaskChatsPageSize = 50;
const QString projectsOp = QStringLiteral("projects");
const QString projectFallbackOp = QStringLiteral("project-page-fallback");
const QString projectPinIdentityOp = QStringLiteral("project-pin-identity");
const QString settingsIdentityOp = QStringLiteral("settings-identity");
const QString recentUserStatusOp = QStringLiteral("recent-user-status");
const QString pushOp = QStringLiteral("push");
const QString notificationsOp = QStringLiteral("notifications");
const QString olderNotificationsOp = QStringLiteral("notifications-older");
const QString notificationBatchPrefix = QStringLiteral("notification-batch:");
const QString taskActionRequestPrefix = QStringLiteral("task-card-action:");
const QString taskCardReadPrefix = QStringLiteral("task-card-read:");
const QString taskReadOperationPrefix = QStringLiteral("task:");
const QString soundsOp = QStringLiteral("chat-sound-settings");
const QString chatIdentityOp = QStringLiteral("chat-history-identity");
const QString currentPersonPrefix = QStringLiteral("current-person:");
const QString historySnapshotPrefix = QStringLiteral("chat-history-snapshot:");
const QString historyReadPrefix = QStringLiteral("chat-history-read:");
const QString sessionRecoveryOp = QStringLiteral("session-recovery");
const QString sessionKeepaliveOp = QStringLiteral("session-keepalive");
const QString csrfRefreshOp = QStringLiteral("csrf-refresh");
const QString soundMuted = QStringLiteral("soundMuted");
const QString volumeGeneral = QStringLiteral("soundVolumeGeneral");
const QString volumeMessages = QStringLiteral("soundVolumeMessages");
const QString notifyScheme = QStringLiteral("notifyScheme");
const QString notifyOnComputer = QStringLiteral("notifySchemeSendSite");
const QString simpleNotifyScheme = QStringLiteral("simple");
const QStringList simpleChannels{notifyOnComputer, QStringLiteral("notifySchemeSendEmail"),
                                 QStringLiteral("notifySchemeSendPush")};
constexpr qsizetype maxResponse = 2 * 1024 * 1024;
constexpr int maxRequests = 12;
constexpr int maxSeenEvents = 256;
constexpr int maxNotifications = 100;
constexpr int notificationPageSize = 50;
constexpr int maxPendingChatPopups = 12;
constexpr int freshPopupSeconds = 10;
constexpr int freshHistoryPopupSeconds = 30;
constexpr int heartbeatIntervalMs = 60000;
constexpr int handshakeTimeoutMs = 15000;

qsizetype notificationHistoryLimit(int olderPages)
{
    constexpr auto maxPages = (std::numeric_limits<qsizetype>::max() - maxNotifications) / notificationPageSize;
    return maxNotifications + qMin<qsizetype>(olderPages, maxPages) * notificationPageSize;
}

void trimNotificationHistory(QVector<Notification> &notices, int olderPages)
{
    if (notices.size() <= maxNotifications)
        return;
    const auto historyLimit = notificationHistoryLimit(olderPages);
    qsizetype ordinary = 0;
    qsizetype visible = 0;
    notices.removeIf([&](const Notification &notice) {
        return notice.ordinaryChatMessage ? ++ordinary > maxNotifications : ++visible > historyLimit;
    });
}

QString notificationRequestKey(quint64 scopeRevision, const QString &key)
{
    return notificationBatchPrefix + QString::number(scopeRevision) + QLatin1Char(':') + key;
}

QString taskActionRequestKey(quint64 scopeRevision, const QString &taskId)
{
    return taskActionRequestPrefix + QString::number(scopeRevision) + QLatin1Char(':') + taskId;
}

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

QString validDialog(const QString &candidate)
{
    return candidate.startsWith(QLatin1String("chat")) ? chatDialog(candidate.mid(4)) : numericId(candidate);
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

bool optionalLookup(const QString &operation)
{
    return operation == projectPinIdentityOp || operation == settingsIdentityOp || operation == recentUserStatusOp
        || operation == projectFallbackOp || operation == sessionKeepaliveOp
        || operation.startsWith(currentPersonPrefix)
        || operation.startsWith(QLatin1String("project-files:")) || operation.startsWith(QLatin1String("project-members:"))
        || operation.startsWith(QLatin1String("project-unread:"))
        || operation.startsWith(QLatin1String("task-history:"))
        || operation.startsWith(QLatin1String("task-details:"))
        || operation.startsWith(QLatin1String("link-preview:"))
        || operation.startsWith(QLatin1String("message-viewers:"))
        || operation == QLatin1String("chat-badges-snapshot")
        || operation == QLatin1String("portal-status-bootstrap")
        || operation.startsWith(QLatin1String("read-visible:"))
        || operation.startsWith(QLatin1String("chat-typing:"))
        || operation.startsWith(QLatin1String("search-people:"));
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

bool boardStageId(const QString &id)
{
    return id == QLatin1String("0") || !numericId(id).isEmpty();
}

QString boardReadKey(const QString &operation, quint64 generation)
{
    return operation + QLatin1Char('@') + QString::number(generation);
}

std::optional<qsizetype> stageTaskCount(const QJsonObject &column)
{
    // The web grid keeps the server total separately from the loaded cards.
    const auto total = column.value(QStringLiteral("total")).toInteger(-1);
    if (total < 0 || total > std::numeric_limits<qsizetype>::max())
        return std::nullopt;
    return static_cast<qsizetype>(total);
}

QJsonObject newProjectDto(const QString &name, qint64 ownerId)
{
    // Defaults from the cloud Project Wizard's Pinia store, mapped to Project.add field names.
    const QString participants = QStringLiteral("K");
    const QString moderators = QStringLiteral("E");
    const QString owner = QStringLiteral("A");
    const QString enabled = QStringLiteral("Y");
    const QJsonObject tasks{{QStringLiteral("view"), participants}, {QStringLiteral("view_all"), participants},
                            {QStringLiteral("sort"), participants}, {QStringLiteral("create_tasks"), participants},
                            {QStringLiteral("edit_tasks"), moderators}, {QStringLiteral("delete_tasks"), moderators}};
    const QJsonObject blog{{QStringLiteral("view_post"), participants}, {QStringLiteral("premoderate_post"), participants},
                           {QStringLiteral("write_post"), participants}, {QStringLiteral("moderate_post"), moderators},
                           {QStringLiteral("full_post"), owner}, {QStringLiteral("view_comment"), participants},
                           {QStringLiteral("premoderate_comment"), participants}, {QStringLiteral("write_comment"), participants},
                           {QStringLiteral("moderate_comment"), moderators}, {QStringLiteral("full_comment"), moderators}};
    const QJsonObject knowledge{{QStringLiteral("read"), participants}, {QStringLiteral("edit"), participants},
                                {QStringLiteral("sett"), participants}, {QStringLiteral("delete"), participants}};
    const QJsonObject options{{QStringLiteral("whoCanInvite"), participants}, {QStringLiteral("manageMessages"), participants},
                              {QStringLiteral("manageMessagesAutoDelete"), moderators}, {QStringLiteral("messagesAutoDeleteDelay"), QString()},
                              {QStringLiteral("showHistory"), enabled}, {QStringLiteral("canGuestCopyText"), enabled},
                              {QStringLiteral("canGuestScreenshot"), enabled}, {QStringLiteral("allowGuestsInvitation"), enabled}};
    const auto emptyDate = QJsonValue(QJsonValue::Null);
    return {{QStringLiteral("name"), name}, {QStringLiteral("description"), QString()}, {QStringLiteral("goal"), QString()},
            {QStringLiteral("privacyType"), QStringLiteral("closed")}, {QStringLiteral("ownerId"), ownerId},
            {QStringLiteral("members"), QJsonArray{}}, {QStringLiteral("moderatorMembers"), QJsonArray{}},
            {QStringLiteral("permissions"), QJsonObject{{QStringLiteral("tasks"), tasks}, {QStringLiteral("blog"), blog},
                                                       {QStringLiteral("landingKnowledge"), knowledge}}},
            {QStringLiteral("options"), options}, {QStringLiteral("publication"), false},
            {QStringLiteral("dates"), QJsonObject{{QStringLiteral("startTs"), emptyDate}, {QStringLiteral("finishTs"), emptyDate}}},
            {QStringLiteral("tags"), QJsonArray{}}, {QStringLiteral("baseFeatureId"), QStringLiteral("chat")}};
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

QString stageColorFrom(const QJsonObject &column)
{
    const auto color = normalizedStageColor(field(column, {"color"}));
    return color.isEmpty() ? normalizedStageColor(field(column, {"COLOR"})) : color;
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

QJsonValue legacyTaskMark(TaskRating rating)
{
    switch (rating) {
    case TaskRating::Unrated: return QJsonValue::Null;
    case TaskRating::Positive: return QStringLiteral("P");
    case TaskRating::Negative: return QStringLiteral("N");
    }
    return QJsonValue::Undefined;
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

QString boardItemId(const QJsonObject &item, const QString &projectId, const QString &stageId)
{
    const auto data = item.value(QStringLiteral("data")).toObject();
    if (data.isEmpty() || field(item, {"columnId"}) != stageId)
        return {};
    const auto rowProject = field(data, {"groupId", "GROUP_ID"});
    if (!rowProject.isEmpty() && numericId(rowProject) != projectId)
        return {};
    return numericId(field(item, {"id"}));
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

QString userName(const QJsonObject &user)
{
    if (const QString name = field(user, {"name", "NAME"}); !name.isEmpty())
        return name;
    return (field(user, {"firstName", "FIRST_NAME"}) + QLatin1Char(' ')
            + field(user, {"lastName", "LAST_NAME"})).trimmed();
}

QString noticeAuthorId(const QJsonObject &notice, const QJsonObject &outer = {})
{
    QString author;
    for (const auto *source : {&notice, &outer}) {
        for (const auto *key : {"author_id", "authorId", "AUTHOR_ID"}) {
            const auto value = source->value(QLatin1String(key));
            if (value.isUndefined())
                continue;
            const auto id = numericId(string(value));
            if (id.isEmpty() || (!author.isEmpty() && author != id))
                return {};
            author = id;
        }
    }
    return author;
}

QString noticeAuthorName(const QString &id, const QJsonValue &users)
{
    if (id.isEmpty())
        return {};
    const auto matchedName = [&id](const QJsonObject &user) {
        auto matches = false;
        for (const auto *key : {"id", "ID"}) {
            const auto value = user.value(QLatin1String(key));
            if (!value.isUndefined()) {
                if (numericId(string(value)) != id)
                    return QString();
                matches = true;
            }
        }
        return matches ? userName(user) : QString();
    };
    if (users.isArray()) {
        for (const auto &item : users.toArray())
            if (const auto name = matchedName(item.toObject()); !name.isEmpty())
                return name;
        return {};
    }
    const auto object = users.toObject();
    return matchedName(object.contains(id) ? object.value(id).toObject() : object);
}

bool safeHeader(const QByteArray &header)
{
    return !header.isEmpty() && header.size() < 16384
        && std::none_of(header.cbegin(), header.cend(), [](char character) {
            const auto byte = static_cast<unsigned char>(character);
            return byte < 32 || byte == 127;
        });
}

bool trustedSocket(const QUrl &url, const QUrl &portal, bool allowLoopback)
{
    if (!url.isValid() || !url.userInfo().isEmpty() || url.hasFragment() || url.hasQuery()
        || url.path().isEmpty())
        return false;
    if (allowLoopback && OriginGuard::isLoopback(portal) && url.host() == portal.host()
        && url.scheme() == QLatin1String("ws"))
        return true;
    if (url.scheme() != QLatin1String("wss") || url.port(443) != 443)
        return false;
    const auto host = url.host().toLower();
    return host == portal.host().toLower() || host.endsWith(QLatin1String(".bitrix24.ru"))
        || host.endsWith(QLatin1String(".bitrix24.com")) || host.endsWith(QLatin1String(".bitrix.info"));
}

bool sessionUsable(const Session &session, bool allowLoopback)
{
    if (!session.isValid() || !session.origin.userInfo().isEmpty()
        || !safeHeader(session.cookie) || !safeHeader(session.csrf))
        return false;
    return session.origin.scheme() == QLatin1String("https")
        || (allowLoopback && OriginGuard::isLoopback(session.origin)
            && session.origin.scheme() == QLatin1String("http"));
}

} // namespace

CloudBackend::CloudBackend(Session session, bool allowLoopback, QObject *parent)
    : Backend(parent), m_session(std::move(session)), m_allowLoopback(allowLoopback),
      m_socket(OriginGuard(m_session.origin).resolve(QStringLiteral("/")).toString(QUrl::RemovePath))
{
    m_http.setProxy(QNetworkProxy::NoProxy);
    m_socket.setProxy(QNetworkProxy::NoProxy);
    m_socket.setMaxAllowedIncomingFrameSize(maxResponse);
    m_socket.setMaxAllowedIncomingMessageSize(maxResponse);
    for (auto timer : {&m_retry, &m_watchdog, &m_heartbeat, &m_refresh, &m_configExpiry})
        timer->setSingleShot(true);

    connect(&m_retry, &QTimer::timeout, this, &CloudBackend::connectPull);
    connect(&m_watchdog, &QTimer::timeout, this, [this] { scheduleReconnect(); });
    connect(&m_heartbeat, &QTimer::timeout, this, [this] {
        if (m_socket.state() == QAbstractSocket::ConnectedState) {
            m_watchdog.start(handshakeTimeoutMs);
            m_socket.ping();
        }
    });
    connect(&m_socket, &QWebSocket::pong, this, [this] {
        m_watchdog.stop();
        m_heartbeat.start(heartbeatIntervalMs);
    });
    connect(&m_configExpiry, &QTimer::timeout, this, [this] { scheduleReconnect(true); });
    connect(&m_refresh, &QTimer::timeout, this, &CloudBackend::refreshDirtyViews);
    m_historyCatchup.setInterval(15 * 1000);
    m_historyCatchup.setTimerType(Qt::VeryCoarseTimer);
    m_historyCatchup.setObjectName(QStringLiteral("active-history-catchup"));
    m_historyCatchup.setParent(this);
    connect(&m_historyCatchup, &QTimer::timeout, this, [this] {
        if (m_activeSection == 0) {
            m_recentDirty = true;
            m_chatDirty = !m_openDialog.isEmpty();
        }
        m_taskMessagesDirty = !m_openTask.isEmpty();
        scheduleRefresh();
    });
    m_sessionKeepalive.setInterval(15 * 60 * 1000);
    m_sessionKeepalive.setTimerType(Qt::VeryCoarseTimer);
    connect(&m_sessionKeepalive, &QTimer::timeout, this, [this] {
        ajax(QStringLiteral("im.v2.Recent.load"),
             {{QStringLiteral("filter[recentSection]"), QStringLiteral("default")},
              {QStringLiteral("limit"), QStringLiteral("1")}},
             sessionKeepaliveOp, [](const QJsonValue &) {});
    });
    connect(this, &Backend::failed, this, [this](const QString &operation, const QString &) {
        if (operation == chatIdentityOp)
            resumeHistoriesWithIdentity();
        if (operation == olderNotificationsOp && m_loadingOlderNotifications) {
            m_loadingOlderNotifications = false;
            publishNotificationHistoryPageState();
        }
        if (operation.startsWith(taskReadOperationPrefix)) {
            const auto id = operation.mid(taskReadOperationPrefix.size());
            const auto current = m_taskCardReads.find(id);
            if (current != m_taskCardReads.end()) {
                current->baseFinished = true;
                finishTaskCardRead(id);
            }
        }
    });
    connect(&m_socket, &QWebSocket::connected, this, [this] {
        if (!m_started)
            return;
        m_attempt = 0;
        m_watchdog.stop();
        m_heartbeat.start(heartbeatIntervalMs);
        const bool refreshPopupSettings = m_resumed && (!m_chatPopupSettings.isEmpty() || m_inflightOperations.contains(soundsOp));
        if (m_resumed) {
            for (auto &state : m_dialogMessageStates)
                state.muteFromPull = false;
        }
        if (refreshPopupSettings) {
            ++m_chatPopupSettingsGeneration;
            m_chatPopupSettings = {};
            m_chatPopupSettingsKnown = false;
            m_pendingChatPopups.removeIf([](const ChatPopup &popup) { return !popup.fromHistory; });
            m_nextChatPopupSettingsRequest = 0;
        }
        setOnline(true);
        if (refreshPopupSettings)
            loadChatPopupSettings();
        if (m_resumed) {
            loadChatBadges();
            invalidateActiveView();
        }
        m_resumed = true;
    });
    connect(&m_socket, &QWebSocket::disconnected, this, [this] {
        scheduleReconnect(m_socket.closeCode() == static_cast<QWebSocketProtocol::CloseCode>(4010));
    });
    connect(&m_socket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, [this](QAbstractSocket::SocketError) { scheduleReconnect(); });
    connect(&m_socket, &QWebSocket::textMessageReceived, this, &CloudBackend::receivePull);
}

CloudBackend::~CloudBackend()
{
    stop();
}

QString CloudBackend::displayName() const
{
    return m_session.origin.host();
}

void CloudBackend::start()
{
    if (m_started)
        return;
    m_started = true;
    m_authExpired = false;
    m_resumed = false;
    m_attempt = 0;
    if (!sessionUsable(m_session, m_allowLoopback)) {
        emit failed(pushOp, QStringLiteral("Для подключения нужны HTTPS, Cookie и CSRF веб-сессии"));
        return;
    }
    m_initialPortalStatusPending = true;
    connectPull();
    loadChatBadges();
    updateHistoryCatchup();
    m_sessionKeepalive.start();
}

void CloudBackend::stop()
{
    m_started = false;
    m_userId.clear();
    resetAccountScopedState();
    m_retry.stop();
    m_watchdog.stop();
    m_heartbeat.stop();
    m_refresh.stop();
    m_sessionKeepalive.stop();
    m_configExpiry.stop();
    m_socket.abort();
    setOnline(false);
}

void CloudBackend::resetAccountScopedState()
{
    ++m_accountScopeRevision;
    resetWorktime();
    const auto pending = m_requests;

    cancelSessionConfirmation();
    m_portalStatus.clear();
    m_initialPortalStatusPending = m_started;
    m_unboundPortalStatuses.clear();
    ++m_portalStatusRevision;
    m_lastPortalStatusPushTime = 0;

    m_taskCardReads.clear();
    m_taskAttachmentRevisions.clear();
    m_loadingProjects = false;
    ++m_boardPageGeneration;
    m_boardColumnCursors.clear();
    m_linkStageSnapshots.clear();
    m_pendingLinkStages.clear();
    m_openBoard.clear();
    m_loadedProjects.clear();
    m_projectDisks.clear();
    m_nextProjectGridPage.clear();
    m_nextProjectOffset = -1;
    m_taskProjectScope = false;
    m_emptyProjectPages = 0;

    clearRasterAssets();
    m_personalChatStatuses.clear();
    m_chatAnchors.clear();
    m_chatAnchorCount = 0;
    m_pendingAnchorEvents.clear();
    m_recentPullsAwaitingIdentity.clear();
    m_chatContacts.clear();
    m_pinnedDialog.clear();
    ++m_pinnedGeneration;
    m_havePinnedSnapshot = false;
    m_pinnedMessages.clear();
    m_pinnedChanges.clear();
    m_registeredMessageFileUrls.clear();
    m_replyPreviews.clear();
    m_lastTypingRequest.clear();
    m_historiesWaitingForIdentity.clear();
    m_triedHistoryIdentity = false;
    m_dialogMessageStates.clear();
    m_messageViewerMembership.clear();
    m_messageViewerCounts.clear();
    m_taskDialogs.clear();
    m_dialogChatIds.clear();
    m_recentTitles.clear();
    m_taskChatTitles.clear();

    m_seenEvents.clear();
    m_seenEventIds.clear();
    m_sentTemplates.clear();
    m_mid.clear();
    m_tag.clear();
    m_time.clear();

    // MainWindow does not reopen the chat or task card on accountScopeChanged.
    // Resume history updates only when a view requests them for the new account.
    m_openDialog.clear();
    m_openTask.clear();
    m_chatHistoryRetry = {};
    m_taskHistoryRetry = {};
    m_recentDirty = false;
    m_chatDirty = false;
    m_taskMessagesDirty = false;
    m_boardDirty = false;
    updateHistoryCatchup();

    ++m_chatPopupSettingsGeneration;
    m_chatPopupSettings = {};
    m_chatPopupSettingsKnown = false;
    m_notificationSettings.clear();
    m_notificationSettingChanges.clear();
    m_notificationSettingsRevision = 0;
    m_nextChatPopupSettingsRequest = 0;
    resetChatCounters();
    resetNoticeState();

    // abort() can synchronously notify consumers. Only cancel requests from before the reset.
    for (const auto &[reply, key] : pending.asKeyValueRange()) {
        // The pending clipboard upload owns the staged file, which a new upload may still open.
        // Its account-specific key allows the new request, and its stale response cannot advance the upload.
        if (m_started && key.startsWith(QLatin1String(chatImageUploadRequestPrefix)))
            continue;
        if (m_requests.contains(reply))
            reply->abort();
    }
}

void CloudBackend::setOnline(bool online)
{
    if (m_online == online)
        return;
    m_online = online;
    emit connectionChanged(online);
}

void CloudBackend::setForeground(bool foreground)
{
    if (m_foreground == foreground)
        return;
    const bool wasWatching = canAutoRefreshChats();
    m_foreground = foreground;
    if (foreground && m_activeSection == 1)
        publishProjectUnreadCounts();
    if (foreground && m_started)
        invalidateActiveView();
    updateChatViewActivity(wasWatching);
}

void CloudBackend::setWorkspaceVisible(bool visible)
{
    if (m_workspaceVisible == visible)
        return;
    const bool wasWatching = canAutoRefreshChats();
    m_workspaceVisible = visible;
    updateChatViewActivity(wasWatching);
}

void CloudBackend::updateChatViewActivity(bool wasWatching)
{
    const bool watching = canAutoRefreshChats();
    if (wasWatching == watching)
        return;
    if (!watching) {
        m_refresh.stop();
        ++m_chatHistoryRetry.revision;
        ++m_taskHistoryRetry.revision;
    } else if (!m_foreground && m_started) {
        if (m_activeSection == 0) {
            m_recentDirty = true;
            m_chatDirty = !m_openDialog.isEmpty();
        }
        m_taskMessagesDirty = !m_openTask.isEmpty();
        scheduleRefresh();
    }
    updateHistoryCatchup();
}

void CloudBackend::setActiveSection(int section)
{
    if (m_activeSection == section)
        return;
    m_activeSection = section;
    if (section != 0)
        ++m_chatHistoryRetry.revision;
    if (section == 1) {
        m_projectUnreadParentAttempts.clear();
        m_projectUnreadTaskChatAttempts.clear();
        m_projectUnreadMoveAttempts.clear();
        publishProjectUnreadCounts(true);
    }
    scheduleRefresh();
    updateHistoryCatchup();
}

void CloudBackend::clearOpenHistory(bool task)
{
    if (task) {
        ++m_taskHistoryRetry.revision;
        m_openTask.clear();
        m_taskMessagesDirty = false;
        m_taskPopupHistory = {};
    } else {
        ++m_chatHistoryRetry.revision;
        m_openDialog.clear();
        m_chatDirty = false;
        m_chatPopupHistory = {};
    }
    updateHistoryCatchup();
}

void CloudBackend::updateHistoryCatchup(bool restart)
{
    const bool watchingChat = m_activeSection == 0 && !m_openDialog.isEmpty();
    // A task card can be open over any workspace section.
    const bool watchingTask = !m_openTask.isEmpty();
    const bool shouldRun = m_started && canAutoRefreshChats() && !m_authExpired && (watchingChat || watchingTask);
    // With two histories open, traffic in one must not postpone the safety refresh for the other.
    const bool restartSingleHistory = restart && !(watchingChat && watchingTask);
    if (shouldRun && (restartSingleHistory || !m_historyCatchup.isActive()))
        m_historyCatchup.start();
    else if (!shouldRun)
        m_historyCatchup.stop();
}

void CloudBackend::rejectSession()
{
    if (m_authExpired)
        return;
    m_authExpired = true;
    ++m_chatHistoryRetry.revision;
    ++m_taskHistoryRetry.revision;
    m_messageViewerMembership.clear();
    m_messageViewerCounts.clear();
    ++m_sessionRevision;
    auto completion = std::exchange(m_sessionConfirmationCallback, {});
    cancelSessionConfirmation();
    m_retry.stop();
    m_refresh.stop();
    m_sessionKeepalive.stop();
    updateHistoryCatchup();
    scheduleReconnect();
    qCWarning(sessionLog) << "Portal authorization expired; waiting for the user to renew it";
    emit messageLinkPreviewAccessChanged(false);
    emit sessionRejected();
    if (completion)
        completion(false);
}

void CloudBackend::cancelSessionConfirmation()
{
    m_sessionConfirmationCallback = {};
    if (auto *probe = std::exchange(m_sessionConfirmation, nullptr)) {
        probe->cancel();
        probe->deleteLater();
    }
}

void CloudBackend::confirmSession(quint64 revision, std::function<void(bool)> onConfirmed)
{
    if (!m_started || m_authExpired || revision != m_sessionRevision) {
        if (onConfirmed)
            onConfirmed(m_started && !m_authExpired);
        return;
    }
    if (m_sessionConfirmation && m_sessionConfirmationRevision == revision) {
        if (onConfirmed)
            m_sessionConfirmationCallback = std::move(onConfirmed);
        return;
    }
    auto previous = std::move(m_sessionConfirmationCallback);
    cancelSessionConfirmation();
    if (previous)
        previous(true);
    auto *probe = new signin::SessionRestore(this);
    m_sessionConfirmation = probe;
    m_sessionConfirmationRevision = revision;
    m_sessionConfirmationCallback = std::move(onConfirmed);
    connect(probe, &signin::SessionRestore::verified, this,
            [this, probe, revision](signin::RestoreStatus result, const Session &verified) {
        if (probe != m_sessionConfirmation)
            return;
        m_sessionConfirmation = nullptr;
        auto completion = std::exchange(m_sessionConfirmationCallback, {});
        probe->deleteLater();
        if (!m_started || m_authExpired)
            return;
        if (revision != m_sessionRevision) {
            if (completion)
                completion(true);
            return;
        }
        if (result == signin::RestoreStatus::Rejected)
            rejectSession();
        const bool accepted = result == signin::RestoreStatus::Authenticated
            && verified.origin == m_session.origin && sessionUsable(verified, m_allowLoopback);
        if (accepted && (verified.csrf != m_session.csrf || verified.cookie != m_session.cookie)) {
            m_session = verified;
            ++m_sessionRevision;
            emit sessionUpdated(m_session);
            invalidateActiveView();
        }
        if (completion)
            completion(accepted);
    });
    probe->verify(m_session, m_allowLoopback);
}

bool CloudBackend::refreshCsrf(Session candidate, quint64 revision)
{
    if (m_inflightOperations.contains(csrfRefreshOp))
        return true;
    auto verified = std::make_shared<Session>(std::move(candidate));
    webRequest(actionUrl(QStringLiteral("im.v2.Recent.load")),
               {{QStringLiteral("filter[recentSection]"), QStringLiteral("default")},
                {QStringLiteral("limit"), QStringLiteral("1")}},
               csrfRefreshOp, QStringLiteral("data"), [this, verified, revision](const QJsonValue &data) {
        if (revision != m_sessionRevision || m_authExpired)
            return;
        const auto response = data.toObject();
        const auto user = numericId(field(response, {"userId", "USER_ID"}));
        if ((!user.isEmpty() && !m_userId.isEmpty() && user != m_userId) || !verified->isValid()) {
            rejectSession();
            return;
        }
        if (!response.value(QStringLiteral("recentItems")).isArray())
            return;
        m_session = *verified;
        ++m_sessionRevision;
        qCInfo(sessionLog) << "Portal CSRF token renewed";
        emit sessionUpdated(m_session);
        invalidateActiveView();
    }, {}, verified);
    return m_inflightOperations.contains(csrfRefreshOp);
}

void CloudBackend::invalidateActiveView()
{
    switch (m_activeSection) {
    case 0:
        m_recentDirty = true;
        m_chatDirty = !m_openDialog.isEmpty();
        break;
    case 1:
        m_boardDirty = !m_openBoard.isEmpty();
        break;
    case 2:
        m_notificationsDirty = true;
        break;
    }
    m_taskMessagesDirty = !m_openTask.isEmpty();
    scheduleRefresh();
}

void CloudBackend::scheduleRefresh()
{
    if (!m_started || !canAutoRefreshChats() || m_authExpired || m_refresh.isActive())
        return;
    const bool sectionDirty = m_activeSection == 0 ? m_recentDirty || m_chatDirty
        : m_foreground && (m_activeSection == 1 ? m_boardDirty : m_activeSection == 2 && m_notificationsDirty);
    if (sectionDirty || m_taskMessagesDirty)
        m_refresh.start(250);
}

void CloudBackend::refreshDirtyViews()
{
    if (!m_started || !canAutoRefreshChats() || m_authExpired)
        return;
    const auto takeDirty = [this](bool &dirty, const QString &operation) {
        if (!dirty || m_requests.size() >= maxRequests || m_inflightOperations.contains(operation))
            return false;
        dirty = false;
        return true;
    };
    if (m_activeSection == 0) {
        if (takeDirty(m_recentDirty, m_showTaskChats ? taskRecentOp : recentOp))
            loadRecentChats(m_showTaskChats);
        if (!m_openDialog.isEmpty() && takeDirty(m_chatDirty, QStringLiteral("chat:") + m_openDialog))
            requestChatMessages(m_openDialog);
    } else if (m_foreground && m_activeSection == 1) {
        if (!m_openBoard.isEmpty() && takeDirty(m_boardDirty,
            boardReadKey(QStringLiteral("board:") + m_openBoard, m_boardPageGeneration)))
            requestProjectBoard(m_openBoard);
    } else if (m_foreground && m_activeSection == 2 && takeDirty(m_notificationsDirty,
                                                notificationRequestKey(m_accountScopeRevision, notificationsOp))) {
        requestNotifications();
    }
    if (!m_openTask.isEmpty() && takeDirty(m_taskMessagesDirty, QStringLiteral("task-messages:") + m_openTask))
        requestTaskMessages(m_openTask);
}

void CloudBackend::invalidateTaskChat(const QString &taskId)
{
    if (!taskId.isEmpty()) {
        if (taskId == m_openTask)
            m_taskMessagesDirty = true;
        const auto listed = std::find_if(m_recentTaskChatItems.cbegin(), m_recentTaskChatItems.cend(),
            [&taskId](const Chat &chat) { return chat.taskId == taskId; });
        if (listed != m_recentTaskChatItems.cend() && m_showTaskChats)
            m_recentDirty = true;
        if (!m_openDialog.isEmpty() && (m_taskDialogs.value(taskId) == m_openDialog
            || (listed != m_recentTaskChatItems.cend() && listed->id == m_openDialog)))
            m_chatDirty = true;
    }
    scheduleRefresh();
}

void CloudBackend::webRequest(QUrl url, const Fields &fields, const QString &operation,
                              const QString &envelope, Handler onData, const QString &requestKey,
                              const std::shared_ptr<Session> &candidate, bool includeResponseMeta)
{
    QByteArray encoded;
    const auto appendField = [&encoded](const QString &key, const QString &value) {
        if (!encoded.isEmpty())
            encoded += '&';
        encoded += QUrl::toPercentEncoding(key);
        encoded += '=';
        encoded += QUrl::toPercentEncoding(value);
    };
    appendField(QStringLiteral("sessid"), QString::fromLatin1(candidate ? candidate->csrf : m_session.csrf));
    for (const auto &field : fields)
        appendField(field.first, field.second);
    auto *buffer = new QBuffer;
    buffer->setData(std::move(encoded));
    buffer->open(QIODevice::ReadOnly);
    webRequestRaw(std::move(url), buffer,
                  {{"Content-Type", "application/x-www-form-urlencoded; charset=UTF-8"}},
                  operation, envelope, std::move(onData), requestKey, candidate, includeResponseMeta);
}

QString CloudBackend::taskFormMutationKey(const QString &operation) const
{
    if (operation.startsWith(QLatin1String("create-task:")) || operation.startsWith(QLatin1String("create-subtask:"))
        || operation.startsWith(QLatin1String("attach-task:")))
        return operation + QLatin1Char(':') + QString::number(m_accountScopeRevision);
    return {};
}

void CloudBackend::webRequestRaw(QUrl url, QIODevice *body, const Headers &headers,
                                 const QString &operation, const QString &envelope, Handler onData,
                                 const QString &requestKey, const std::shared_ptr<Session> &candidate,
                                 bool includeResponseMeta)
{
    std::unique_ptr<QIODevice> ownedBody(body);
    const auto accountScope = m_accountScopeRevision;
    const bool personLookup = operation.startsWith(QLatin1String("person:"));
    const auto mutationKey = taskFormMutationKey(operation);
    const auto key = personLookup ? operation + QLatin1Char(':') + QString::number(accountScope)
        : !mutationKey.isEmpty() ? mutationKey : requestKey.isEmpty() ? operation : requestKey;
    const bool historySnapshot = operation.startsWith(historySnapshotPrefix);
    const bool latestHistory = historySnapshot || operation.startsWith(historyReadPrefix);
    const auto historyRevision = (key.startsWith(QLatin1String("task-messages:"))
        ? m_taskHistoryRetry : m_chatHistoryRetry).revision;
    const auto failedOperation = latestHistory ? key : operation;
    const auto rejectRequest = [&](const QString &reason) {
        if (optionalLookup(operation))
            onData({});
        else
            emit failed(failedOperation, reason);
    };
    if (m_inflightOperations.contains(key))
        return;
    const auto &credentials = candidate ? *candidate : m_session;
    if (m_authExpired && !candidate) {
        rejectRequest(QStringLiteral("Вход в портал истёк. Обновите подключение."));
        return;
    }
    if (!sessionUsable(credentials, m_allowLoopback) || credentials.origin != m_session.origin
        || !url.userInfo().isEmpty() || url.hasFragment() || !OriginGuard(m_session.origin).isSameOrigin(url)) {
        rejectRequest(QStringLiteral("Импортируйте действующую веб-сессию портала"));
        return;
    }
    if (m_requests.size() >= maxRequests) {
        rejectRequest(QStringLiteral("Дождитесь завершения текущих запросов"));
        if (operation == pushOp)
            scheduleReconnect();
        return;
    }
    if (!body || !body->isReadable()) {
        rejectRequest(QStringLiteral("Не удалось прочитать данные запроса"));
        return;
    }
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    bool upload = false;
    for (const auto &[name, value] : headers) {
        const auto normalized = name.toLower();
        if ((normalized != "content-type" && normalized != "content-range"
             && !normalized.startsWith("x-upload-")) || !safeHeader(value)) {
            rejectRequest(QStringLiteral("Некорректные параметры запроса"));
            return;
        }
        upload = upload || normalized == "x-upload-content-name";
        request.setRawHeader(name, value);
    }
    request.setTransferTimeout(upload ? 60000 : 12000);
    request.setRawHeader("Cookie", credentials.cookie);
    request.setRawHeader("X-Bitrix-Csrf-Token", credentials.csrf);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("X-Requested-With", "XMLHttpRequest");
    request.setRawHeader("BX-Ajax", "true");
    const auto online = OriginGuard(m_session.origin).resolve(QStringLiteral("/online/"));
    request.setRawHeader("Origin", online.toEncoded(QUrl::RemovePath));
    request.setRawHeader("Referer", online.toEncoded());

    auto *reply = m_http.post(request, body);
    ownedBody.release()->setParent(reply);
    m_requests.insert(reply, key);
    m_inflightOperations.insert(key);
    if (operation == sessionRecoveryOp)
        m_sessionRecoveryReply = reply;
    reply->setReadBufferSize(maxResponse + 1);
    auto *deadline = new QTimer(reply);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, reply, &QNetworkReply::abort);
    deadline->start(upload ? 300000 : 15000);
    connect(reply, &QIODevice::readyRead, this, [reply] {
        if (reply->bytesAvailable() > maxResponse)
            reply->abort();
    });
    const auto revision = m_sessionRevision;
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, operation, key, envelope, revision, candidate, historySnapshot, latestHistory, historyRevision, failedOperation,
             accountScope, includeResponseMeta,
             handler = std::move(onData)] {
        m_requests.remove(reply);
        reply->deleteLater();
        if (operation == sessionRecoveryOp) {
            if (m_sessionRecoveryReply != reply)
                return;
            m_sessionRecoveryReply = nullptr;
        }
        m_inflightOperations.remove(key);
        if (!m_started)
            return;
        if (accountScope != m_accountScopeRevision) {
            // These operations leave controls busy until they receive a terminal signal.
            if (key == operation && (key.startsWith(QLatin1String("send:")) || key.startsWith(QLatin1String("upload-"))
                || key.startsWith(QLatin1String("board-edit:")) || key == QLatin1String("create-project")
                || key.startsWith(QLatin1String("chat-pin:")) || key.startsWith(QLatin1String("project-pin:"))
                || key.startsWith(QLatin1String("search-project-tasks:"))))
                emit failed(operation, QStringLiteral("Аккаунт изменился. Результат запроса не подтверждён."));
            return;
        }
        scheduleRefresh();
        if (operation.startsWith(projectTaskChatsPrefix)
            && key != m_projectTaskChatsRequest)
            return;
        if (key != operation
            && (operation.startsWith(QLatin1String("board:")) || operation.startsWith(QLatin1String("board-column:")))
            && key != boardReadKey(operation, m_boardPageGeneration))
            return;

        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool redirect = status >= 300 && status < 400;
        const bool complete = reply->isOpen() && reply->bytesAvailable() <= maxResponse && !redirect;
        const auto document = complete ? QJsonDocument::fromJson(reply->readAll()) : QJsonDocument();
        const auto object = document.object();
        const auto location = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        const bool denied = signin::isAuthorizationRejection(status, object, reply->url(), location);
        const auto result = object.value(envelope);
        const auto errors = object.value(QStringLiteral("errors"));
        const bool ajaxRejected = envelope == QLatin1String("data")
            && (object.value(QStringLiteral("status")) != QLatin1String("success")
                || (!errors.isUndefined() && !errors.isNull() && (!errors.isArray() || !errors.toArray().isEmpty())));
        const bool failed = reply->error() != QNetworkReply::NoError || redirect || !document.isObject()
            || object.contains(QStringLiteral("error")) || ajaxRejected || result.isUndefined();

        if (failed) {
            const auto error = reply->error();
            const bool transientConnection = status == 0
                && (error == QNetworkReply::TimeoutError || error == QNetworkReply::OperationCanceledError
                    || error == QNetworkReply::RemoteHostClosedError || error == QNetworkReply::TemporaryNetworkFailureError
                    || error == QNetworkReply::ConnectionRefusedError || error == QNetworkReply::NetworkSessionFailedError);
            const bool temporary = transientConnection || (status >= 500 && status < 600);
            if (historySnapshot && !denied && !m_authExpired
                && (temporary || (status >= 200 && status < 300))) {
                // The separate message list can still be usable if the full chat snapshot fails.
                handler({});
                return;
            }
            if (latestHistory && !denied && temporary && scheduleHistoryRetry(key, historyRevision))
                return;
            if (operation == csrfRefreshOp) {
                if (denied && revision == m_sessionRevision)
                    rejectSession();
                return;
            }
            // Some optional endpoints may deny access while the main routes are still usable.
            const bool lookup = optionalLookup(operation);
            bool refreshing = false;
            if (denied && (!lookup || operation == sessionKeepaliveOp)
                && !candidate && revision == m_sessionRevision) {
                auto retry = signin::csrfRetrySession(m_session, *reply, object);
                refreshing = retry.isValid() && refreshCsrf(std::move(retry), revision);
                if (!refreshing) {
                    if (operation == sessionKeepaliveOp) {
                        rejectSession();
                    } else {
                        confirmSession(revision);
                        refreshing = true;
                    }
                }
            }
            if (lookup) {
                handler({});
                return;
            }
            QString detail = refreshing ? QStringLiteral("Клиент проверяет вход в портал. Повторите действие после проверки")
                : denied && revision != m_sessionRevision && !m_authExpired
                    ? QStringLiteral("Параметры входа обновлены. Повторите действие")
                : denied ? QStringLiteral("Действие требует новой веб-сессии портала")
                : status == 403 ? QStringLiteral("Портал запретил действие")
                : status > 0 ? QStringLiteral("Портал отклонил действие или вернул другой формат (HTTP %1)").arg(status)
                : QStringLiteral("Ошибка сети или истекло время ожидания");
            if (operation.startsWith(QLatin1String("send"))
                || operation.startsWith(QLatin1String("upload-")))
                detail += QStringLiteral(". Проверьте историю перед повторной отправкой");
            if (operation.startsWith(QLatin1String("attach-task:")))
                detail += QStringLiteral(". Проверьте файлы задачи перед повторным прикреплением");
            if (operation.startsWith(QLatin1String("update-task:"))
                || operation.startsWith(QLatin1String("board-edit:")))
                detail += QStringLiteral(". Проверьте сохранённые изменения перед повтором");
            emit this->failed(failedOperation, detail);
            if (operation == pushOp && !m_authExpired)
                scheduleReconnect();
            return;
        }
        if (candidate) {
            *candidate = signin::updatedSession(*candidate, *reply);
        } else if (revision == m_sessionRevision) {
            const auto renewed = signin::updatedSession(m_session, *reply);
            if (!renewed.isValid()) {
                rejectSession();
            } else if (renewed.cookie != m_session.cookie || renewed.csrf != m_session.csrf) {
                m_session = renewed;
                ++m_sessionRevision;
                emit sessionUpdated(m_session);
            }
        }
        handler(includeResponseMeta ? QJsonValue(object) : result);
    });
}

void CloudBackend::ajax(const QString &action, Fields fields, const QString &operation, Handler handler,
                        const QString &requestKey)
{
    webRequest(actionUrl(action), fields, operation, QStringLiteral("data"), std::move(handler), requestKey);
}

QUrl CloudBackend::actionUrl(const QString &action) const
{
    QUrl url = OriginGuard(m_session.origin).resolve(QStringLiteral("/bitrix/services/main/ajax.php"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("action"), action);
    url.setQuery(query);
    return url;
}

void CloudBackend::ajaxJson(const QString &action, const QJsonObject &body, const QString &operation,
                           Handler handler, const QString &requestKey)
{
    ajaxJson(actionUrl(action), body, operation, std::move(handler), requestKey);
}

void CloudBackend::ajaxJson(QUrl url, const QJsonObject &body, const QString &operation,
                           Handler handler, const QString &requestKey)
{
    auto *buffer = new QBuffer;
    buffer->setData(QJsonDocument(body).toJson(QJsonDocument::Compact));
    buffer->open(QIODevice::ReadOnly);
    webRequestRaw(std::move(url), buffer, {{"Content-Type", "application/json"}}, operation,
                  QStringLiteral("data"), std::move(handler), requestKey);
}

void CloudBackend::componentAction(const QString &action, const Fields &fields,
                                   const QString &operation, Handler handler, const QString &component,
                                   const QString &requestKey)
{
    QUrl url = OriginGuard(m_session.origin).resolve(QStringLiteral("/bitrix/services/main/ajax.php"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("mode"), QStringLiteral("class"));
    query.addQueryItem(QStringLiteral("c"), component);
    query.addQueryItem(QStringLiteral("action"), action);
    url.setQuery(query);
    webRequest(url, fields, operation, QStringLiteral("data"), std::move(handler), requestKey);
}

void CloudBackend::failShape(const QString &operation)
{
    emit failed(operation, QStringLiteral("Портал вернул неподдерживаемый формат данных"));
}

void CloudBackend::withCurrentUser(const QString &operation, std::function<void(qint64)> onUser)
{
    if (!numericId(m_userId).isEmpty()) {
        onUser(m_userId.toLongLong());
        return;
    }
    requestCurrentUser(operation, [this, onUser = std::move(onUser)](const QString &id) {
        if (!id.isEmpty())
            rememberCurrentUser(id);
        onUser(id.toLongLong());
    });
}

void CloudBackend::requestCurrentPerson(PersonHandler complete)
{
    const auto operation = currentPersonPrefix + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto account = m_accountScopeRevision;
    withCurrentUser(operation, [this, operation, account, complete = std::move(complete)](qint64 userId) mutable {
        if (account != m_accountScopeRevision || userId <= 0) {
            complete({});
            return;
        }
        loadPerson(QString::number(userId), operation, std::move(complete), true);
    });
}

void CloudBackend::requestCurrentUser(const QString &operation,
                                      std::function<void(const QString &)> onUser,
                                      const std::shared_ptr<Session> &candidate)
{
    const auto extension = QStringLiteral("socialnetwork.v2.current-user");
    auto url = actionUrl(QStringLiteral("main.bitrix.main.controller.loadext.resolveextensions"));
    QUrlQuery query(url);
    query.addQueryItem(QStringLiteral("e"), extension);
    url.setQuery(query);
    webRequest(url, {{QStringLiteral("extension[0]"), extension}, {QStringLiteral("coreLoaded"), QStringLiteral("Y")}},
               operation, QStringLiteral("data"), [this, extension, operation, onUser = std::move(onUser)](QJsonValue data) {
        if (!data.isArray() && !optionalLookup(operation))
            return failShape(operation);
        for (const auto &entry : data.toArray()) {
            const auto item = entry.toObject();
            if (item.value(QStringLiteral("extension")) != extension)
                continue;
            const auto settings = item.value(QStringLiteral("settings")).toObject();
            const auto id = numericId(field(settings, {"id"}));
            if (!id.isEmpty()) {
                onUser(id);
                return;
            }
            break;
        }
        if (optionalLookup(operation))
            onUser({});
        else
            emit failed(operation, QStringLiteral("Портал не подтвердил текущего пользователя"));
    }, {}, candidate);
}

void CloudBackend::cancelSessionRecovery()
{
    if (auto *reply = std::exchange(m_sessionRecoveryReply, nullptr)) {
        m_requests.remove(reply);
        m_inflightOperations.remove(sessionRecoveryOp);
        reply->abort();
    }
}

void CloudBackend::recoverSession(const Session &candidate, std::function<bool()> stillAllowed)
{
    if (!m_started || !m_authExpired || m_inflightOperations.contains(sessionRecoveryOp))
        return;
    if (candidate.origin != m_session.origin || numericId(m_userId).isEmpty()) {
        emit failed(sessionRecoveryOp, QStringLiteral("Чтобы открыть другой портал или аккаунт, выйдите из текущего рабочего окна."));
        return;
    }
    const auto expectedUser = m_userId;
    auto verified = std::make_shared<Session>(candidate);
    requestCurrentUser(sessionRecoveryOp, [this, verified, expectedUser, stillAllowed = std::move(stillAllowed)](const QString &id) {
        if (stillAllowed && !stillAllowed())
            return;
        if (!sessionUsable(*verified, m_allowLoopback)) {
            emit failed(sessionRecoveryOp, QStringLiteral("Портал отклонил новый вход. Обновите подключение ещё раз."));
            return;
        }
        if (id != expectedUser || m_userId != expectedUser) {
            emit failed(sessionRecoveryOp, QStringLiteral("Подключитесь под тем же аккаунтом, чтобы сохранить рабочее окно и черновики."));
            return;
        }
        const auto pending = m_requests;
        for (auto *reply : pending.keys())
            reply->abort();
        resetWorktime();
        m_session = *verified;
        ++m_sessionRevision;
        m_authExpired = false;
        m_attempt = 0;
        m_sessionKeepalive.start();
        qCInfo(sessionLog) << "Portal authorization renewed";
        emit sessionUpdated(m_session);
        emit messageLinkPreviewAccessChanged(true);
        emit sessionRecovered();
        connectPull();
        loadChatBadges();
        invalidateActiveView();
        updateHistoryCatchup();
    }, verified);
}

void CloudBackend::rememberCurrentUser(const QString &id)
{
    if (m_userId == id || numericId(id).isEmpty())
        return;
    const bool accountChanged = !m_userId.isEmpty();
    m_userId = id;
    if (accountChanged) {
        ++m_sessionRevision;
        resetAccountScopedState();
        scheduleReconnect(true);
    }
    bindPortalStatusIdentity(accountChanged);
    emit accountScopeChanged();
    const auto waiting = std::exchange(m_recentPullsAwaitingIdentity, {});
    for (const auto &event : waiting)
        applyRecentPull(event.command, event.dialog, event.chatId, event.params, event.extra);
}

void CloudBackend::rememberDismissalStatus(const QString &id, std::optional<bool> dismissed)
{
    if (!m_personalChatStatuses.contains(id) && m_personalChatStatuses.size() >= 2 * tunables::kPageSizeChats)
        m_personalChatStatuses.erase(m_personalChatStatuses.begin());
    const auto ttl = dismissed.has_value() ? 300 : 5;
    m_personalChatStatuses.insert(id, {dismissed, QDateTime::currentSecsSinceEpoch() + ttl});
}

std::optional<bool> CloudBackend::cachedDismissalStatus(const QString &id) const
{
    const auto cached = m_personalChatStatuses.constFind(id);
    if (cached != m_personalChatStatuses.cend() && cached->expiresAt > QDateTime::currentSecsSinceEpoch())
        return cached->dismissed;
    return std::nullopt;
}

Chat CloudBackend::chatFromMetadata(const QString &dialog, const QJsonObject &row, const QJsonObject &chat,
                                    const QJsonObject &user)
{
    Chat result;
    result.id = dialog;
    auto type = field(chat, {"type", "kind"});
    if (type.isEmpty())
        type = field(row, {"type", "kind"});
    // The search selector calls every result a chat, including direct user dialogs.
    result.kind = type.isEmpty() ? field(row, {"entityType"}) : type;
    auto entity = chat.value(QStringLiteral("entityLink")).toObject();
    if (entity.isEmpty())
        entity = row.value(QStringLiteral("entityLink")).toObject();
    result.taskChat = isTaskChatMetadata(result.kind, entity);
    if (result.taskChat)
        result.taskId = numericId(field(entity, {"id"}));
    QString avatar;
    if (!dialog.startsWith(QLatin1String("chat"))) {
        result.title = userName(user);
        avatar = field(user, {"avatar", "avatarUrl"});
    }
    if (result.title.isEmpty())
        result.title = field(chat, {"name", "title"});
    if (result.title.isEmpty())
        result.title = field(row, {"title", "name"});
    if (avatar.isEmpty())
        avatar = field(chat, {"avatar", "avatarUrl"});
    if (avatar.isEmpty())
        avatar = field(row, {"avatar", "avatarUrl"});
    result.avatarUrl = avatarUrlFromPortal(avatar);
    result.pinned = notificationFlag(row.value(QStringLiteral("pinned"))).value_or(false);
    if (!result.taskChat && isPersonalMetadata(dialog, type)) {
        result.kind = QStringLiteral("im");
        const auto dismissed = dismissedPersonalUser(dialog, row, user);
        result.dismissed = dismissed.value_or(cachedDismissalStatus(dialog).value_or(false));
        if (dismissed)
            rememberDismissalStatus(dialog, dismissed);
        rememberChatContact(dialog, user, false);
        applyChatContact(result);
    }
    return result;
}

void CloudBackend::requestRecentChats()
{
    m_showTaskChats = false;
    loadRecentChats(false);
    loadChatBadges();
}

void CloudBackend::requestTaskChats()
{
    m_showTaskChats = true;
    loadRecentChats(true);
    loadChatBadges();
}

void CloudBackend::requestTaskChatBadges()
{
    loadRecentChats(true, true);
}

void CloudBackend::requestProjectTaskChats(const QString &projectId, int offset)
{
    if (projectId.isEmpty()) {
        resetProjectTaskChats();
        return;
    }
    const auto operation = projectTaskChatsPrefix + projectId;
    if (!m_started || numericId(projectId) != projectId || offset < 0
        || offset > std::numeric_limits<int>::max() - projectTaskChatsPageSize
        || (offset > 0 && (projectId != m_projectTaskChatsProject || offset != m_projectTaskChatsNext)))
        return failShape(operation);
    if (offset == 0) {
        resetProjectTaskChats();
        m_projectTaskChatsProject = projectId;
    } else if (m_inflightOperations.contains(m_projectTaskChatsRequest)) {
        return;
    }
    const auto request = operation + QLatin1Char(':') + QString::number(++m_projectTaskChatsSerial);
    const auto bindingRevision = m_projectUnreadBindingRevision;
    m_projectTaskChatsRequest = request;
    Fields fields{{QStringLiteral("filter[GROUP_ID]"), projectId},
                  {QStringLiteral("order[ID]"), QStringLiteral("DESC")},
                  {QStringLiteral("start"), QString::number(offset)}};
    for (const auto *name : {"ID", "TITLE", "GROUP_ID", "CHAT_ID", "ACTIVITY_DATE"})
        fields.append({QStringLiteral("select[]"), QLatin1String(name)});
    const auto url = OriginGuard(m_session.origin).resolve(QStringLiteral("/rest/tasks.task.list.json"));
    webRequest(url, fields, operation, QStringLiteral("result"),
        [this, projectId, offset, request, operation, bindingRevision](const QJsonValue &response) {
            if (m_projectTaskChatsRequest != request || m_authExpired)
                return;
            const auto result = response.toObject();
            const auto value = result.value(QStringLiteral("result")).toObject().value(QStringLiteral("tasks"));
            const auto rows = value.toArray();
            const auto cursor = result.value(QStringLiteral("next"));
            const auto next = cursor.isUndefined() || cursor.isNull() ? -1 : cursor.toInteger(-2);
            if (!value.isArray() || rows.size() > projectTaskChatsPageSize
                || (next != -1 && (next != offset + projectTaskChatsPageSize || rows.size() != projectTaskChatsPageSize)))
                return failShape(operation);
            QVector<Chat> chats;
            for (const auto &entry : rows) {
                const auto row = entry.toObject();
                const auto taskId = numericId(field(row, {"id", "ID"}));
                const auto chatId = explicitMessageChatId(row);
                if (!matchesTaskIdentity(row, taskId) || chatId <= 0
                    || m_projectTaskIds.contains(taskId) || m_projectTaskChatIds.contains(chatId))
                    continue;
                const auto group = row.value(QStringLiteral("group")).toObject();
                bool matched = false;
                for (const auto &candidate : {row.value("groupId"), row.value("GROUP_ID"), group.value("id"), group.value("ID")}) {
                    if (candidate.isUndefined())
                        continue;
                    matched = numericId(string(candidate)) == projectId;
                    if (!matched)
                        break;
                }
                if (!matched)
                    continue;
                m_projectTaskIds.insert(taskId);
                m_projectTaskChatIds.insert(chatId);
                if (bindingRevision == m_projectUnreadBindingRevision)
                    rememberProjectUnreadTask(chatId, taskId.toLongLong(), projectId.toLongLong());
                Chat chat;
                chat.id = chatDialog(QString::number(chatId));
                chat.serverChatId = chatId;
                chat.taskId = taskId;
                chat.taskChat = true;
                chat.kind = QStringLiteral("chat");
                chat.title = field(row, {"title", "TITLE"}).left(500);
                if (chat.title.isEmpty())
                    chat.title = QStringLiteral("Задача №%1").arg(taskId);
                chat.updated = date(oneOf(row, {"activityDate", "ACTIVITY_DATE"}));
                decorateProjectTaskChat(chat);
                chats.append(std::move(chat));
            }
            m_projectTaskChatsNext = static_cast<int>(next);
            publishProjectUnreadCounts();
            emit projectTaskChatsLoaded(projectId, offset, chats, m_projectTaskChatsNext);
        }, request, {}, true);
    loadChatBadges();
}

void CloudBackend::loadRecentChats(bool task, bool badgesOnly)
{
    const auto operation = task ? taskRecentOp : recentOp;
    ajax(QStringLiteral("im.v2.Recent.load"),
         {{QStringLiteral("filter[recentSection]"), task ? QStringLiteral("tasksTask") : QStringLiteral("default")},
          {QStringLiteral("limit"), QString::number(tunables::kPageSizeChats)}},
         operation, [this, task, badgesOnly, operation, scope = m_accountScopeRevision,
                     counterRevision = m_chatBadgeRevision](QJsonValue data) {
        if (scope != m_accountScopeRevision)
            return;
        const auto rows = list(data, {"recentItems", "items", "recent"});
        if (!rows.isArray())
            return failShape(operation);

        const auto root = data.toObject();
        const auto userId = numericId(field(root, {"userId", "currentUserId"}));
        rememberCurrentUser(userId);
        const auto currentUser = numericId(m_userId).toLongLong();
        QHash<qint64, QJsonObject> messages, latestMessagesByChat;
        QHash<QString, QJsonObject> users, chatById, chatByDialog, counters;
        for (const auto value : root.value(QStringLiteral("messages")).toArray()) {
            const auto message = value.toObject();
            const auto id = recentMessageId(message);
            if (id <= 0)
                continue;
            messages.insert(id, message);
            const auto messageChatId = explicitMessageChatId(message);
            if (messageChatId > 0 && id > recentMessageId(latestMessagesByChat.value(messageChatId)))
                latestMessagesByChat.insert(messageChatId, message);
        }
        for (const auto *key : {"usersShort", "users"}) {
            for (const auto value : root.value(QLatin1String(key)).toArray()) {
                const auto user = value.toObject();
                const auto id = numericId(field(user, {"id", "ID"}));
                if (id.isEmpty())
                    continue;
                auto merged = users.value(id);
                for (auto it = user.constBegin(); it != user.constEnd(); ++it) {
                    if (!it.value().isNull() && (!it.value().isString() || !it.value().toString().isEmpty()))
                        merged.insert(it.key(), it.value());
                }
                users.insert(id, merged);
            }
        }
        for (const auto value : root.value(QStringLiteral("chats")).toArray()) {
            const auto chat = value.toObject();
            chatById.insert(field(chat, {"id", "chatId"}), chat);
            chatByDialog.insert(validDialog(field(chat, {"dialogId"})), chat);
        }
        const auto rawCounters = root.value(QStringLiteral("counters"));
        for (const auto value : rawCounters.toArray()) {
            const auto count = value.toObject();
            counters.insert(field(count, {"chatId"}), count);
        }
        const auto countersObject = rawCounters.toObject();
        for (auto it = countersObject.constBegin(); it != countersObject.constEnd(); ++it)
            counters.insert(it.key(), it.value().isObject() ? it.value().toObject()
                : QJsonObject{{QStringLiteral("counter"), it.value()}});

        QVector<Chat> chats;
        QVector<Chat> legacyTasks;
        QSet<QString> taskDialogs;
        QStringList usersWithoutStatus;
        for (const auto row : rows.toArray()) {
            const auto object = row.toObject();
            const auto recentChatId = field(object, {"chatId", "CHAT_ID"});
            auto dialogId = validDialog(field(object, {"dialogId", "DIALOG_ID", "id"}));
            auto metadata = object.value(QStringLiteral("chat")).toObject();
            if (metadata.isEmpty() && !recentChatId.isEmpty())
                metadata = chatById.value(recentChatId);
            if (metadata.isEmpty() && !dialogId.isEmpty())
                metadata = chatByDialog.value(dialogId);
            if (dialogId.isEmpty())
                dialogId = validDialog(field(metadata, {"dialogId"}));
            if (dialogId.isEmpty())
                dialogId = chatDialog(recentChatId);
            if (dialogId.isEmpty())
                continue;
            auto user = users.value(dialogId);
            if (user.isEmpty())
                user = object.value(QStringLiteral("user")).toObject();
            auto chat = chatFromMetadata(dialogId, object, metadata, user);
            if (chat.title.isEmpty())
                chat.title = QStringLiteral("Диалог %1").arg(dialogId);
            if (task && chat.kind.isEmpty())
                chat.taskChat = true; // A row in the dedicated server section may omit its type.

            auto rowChatId = recentChatId;
            if (rowChatId.isEmpty())
                rowChatId = field(metadata, {"id", "chatId"});
            chat.serverChatId = numericId(rowChatId).toLongLong();
            rememberProjectUnreadTaskChat(chat);
            const auto counter = counters.value(rowChatId);
            bool hasCounterSnapshot = false;
            const auto snapshotCount = counter.value(QStringLiteral("counter")).toVariant().toInt(&hasCounterSnapshot);
            chat.unread = hasCounterSnapshot ? std::max(0, snapshotCount)
                : std::max({0, oneOf(object, {"counter", "unreadCount"}).toVariant().toInt(),
                             metadata.value(QStringLiteral("counter")).toVariant().toInt()});
            const auto snapshotMarked = notificationFlag(counter.value(QStringLiteral("isMarkedAsUnread")));
            chat.markedUnread = snapshotMarked.value_or(notificationFlag(object.value(QStringLiteral("unread"))).value_or(false)
                || notificationFlag(metadata.value(QStringLiteral("isMarkedAsUnread"))).value_or(false));
            auto messageId = numericId(field(object, {"messageId"})).toLongLong();
            auto preview = object.value(QStringLiteral("message")).toObject();
            if (preview.isEmpty())
                preview = messages.value(messageId);
            const auto groupChatId = groupDialogChatId(dialogId);
            const auto expectedChatId = groupChatId > 0 ? groupChatId : chat.serverChatId;
            const auto previewChatId = explicitMessageChatId(preview);
            const bool foreignPreview = previewChatId != 0 && previewChatId != expectedChatId;
            // Recent rows can point into another chat's message in the shared list.
            const bool useChatMessage = foreignPreview || preview.isEmpty();
            if (useChatMessage) {
                preview = latestMessagesByChat.value(expectedChatId);
                if (foreignPreview || !preview.isEmpty())
                    messageId = recentMessageId(preview);
            }
            chat.lastMessage = field(preview, {"text"});
            const auto updated = oneOf(object, {"dateLastActivity", "dateUpdate", "updated"});
            const auto previewDate = date(preview.value(QStringLiteral("date")));
            chat.updated = (useChatMessage && previewDate.isValid()) || updated.isUndefined()
                ? previewDate : date(updated);
            bool hasReadId = false;
            const auto readId = field(metadata, {"lastId"}).toLongLong(&hasReadId);
            auto &live = recentOverlay(dialogId);
            const auto state = m_dialogMessageStates.constFind(dialogId);
            const auto unreadId = numericId(field(metadata, {"unreadId"})).toLongLong();
            const bool alreadyRead = chat.unread == 1 && unreadId > 0 && state != m_dialogMessageStates.cend()
                && (unreadId <= state->confirmedReadThrough || state->exactlyRead.contains(unreadId));
            const bool olderCursor = hasReadId && readId >= 0 && readId < live.confirmedCounterCursor;
            // A newer own preview alone does not supersede a confirmed read.
            const bool staleOwnCounter = live.counter && (alreadyRead || olderCursor) && currentUser > 0
                && expectedChatId > 0 && explicitMessageChatId(preview) == expectedChatId
                && explicitMessageAuthorId(preview) == currentUser;
            const bool counterCurrent = m_chatCounterSnapshotRevision <= counterRevision
                && live.counterRevision <= counterRevision && !staleOwnCounter;
            if (!metadata.isEmpty() || !recentChatId.isEmpty())
                rememberDialog(dialogId, {{QStringLiteral("chat"), metadata}, {QStringLiteral("chatId"), recentChatId}},
                               counterCurrent);
            auto &rejectedPreviewChatId = chat.taskChat ? live.rejectedTaskPreviewChatId : live.rejectedRecentPreviewChatId;
            rejectedPreviewChatId = foreignPreview && preview.isEmpty() ? expectedChatId : 0;
            const auto lastRead = m_dialogMessageStates.value(dialogId).lastReadId;
            if (messageId > live.messageId && messageId > lastRead) {
                live.messageId = messageId;
                // A newer preview can arrive in a response carrying an older unread count.
                if (counterCurrent) {
                    live.incomingId = 0;
                    if (!m_haveCounterSnapshot)
                        live.counter.reset();
                }
                live.preview = {};
            }
            if (counterCurrent && !m_haveCounterSnapshot && live.incomingId > 0 && messageId >= live.messageId
                && readId >= live.incomingId && readId >= lastRead
                && (hasCounterSnapshot || unreadCount(oneOf(object, {"counter", "unreadCount"})))) {
                live.counter = chat.unread;
            }

            if (!m_recentTitles.contains(dialogId) && m_recentTitles.size() >= 2 * tunables::kPageSizeChats)
                m_recentTitles.erase(m_recentTitles.begin());
            m_recentTitles.insert(dialogId, chat.title);
            if (!task && chat.taskChat && legacyTasks.size() < tunables::kPageSizeChats)
                legacyTasks.append(chat);
            if (chat.taskChat == task && chats.size() < tunables::kPageSizeChats) {
                const auto status = m_personalChatStatuses.constFind(dialogId);
                if (!task && chat.kind == QLatin1String("im")
                    && (status == m_personalChatStatuses.cend() || status->expiresAt <= QDateTime::currentSecsSinceEpoch())
                    && !usersWithoutStatus.contains(dialogId)) {
                    usersWithoutStatus.append(dialogId);
                }
                taskDialogs.insert(dialogId);
                chats.append(std::move(chat));
            }
        }
        publishProjectUnreadCounts();
        if (!task) {
            m_legacyTaskChats = std::move(legacyTasks);
            const auto generation = ++m_recentStatusGeneration;
            publishRecentChats(chats, false);
            if (!usersWithoutStatus.isEmpty())
                hydrateRecentUserStatuses(chats, usersWithoutStatus, generation);
            if (m_userId.isEmpty() && std::any_of(chats.cbegin(), chats.cend(), [](const Chat &chat) { return chat.dismissed; }))
                withCurrentUser(settingsIdentityOp, [](qint64) {});
            return;
        }
        for (const auto &legacy : std::as_const(m_legacyTaskChats)) {
            if (chats.size() >= tunables::kPageSizeChats)
                break;
            if (!taskDialogs.contains(legacy.id))
                chats.append(legacy);
        }
        if (badgesOnly && !m_showTaskChats) {
            publishRecentChats(std::move(chats), true);
            return;
        }
        resolveTaskChatTitles(std::move(chats), taskRecentOp, [this, scope = m_accountScopeRevision](QVector<Chat> result) {
            if (scope != m_accountScopeRevision)
                return;
            for (const auto &chat : result)
                m_recentTitles.insert(chat.id, chat.title);
            publishRecentChats(std::move(result), true);
        });
    });
}

void CloudBackend::hydrateRecentUserStatuses(QVector<Chat> chats, const QStringList &ids, quint64 generation)
{
    Fields fields;
    QSet<QString> pending;
    for (const auto &id : ids) {
        fields.append({QStringLiteral("ID[%1]").arg(fields.size()), id});
        pending.insert(id);
        rememberDismissalStatus(id, std::nullopt);
    }
    const auto url = OriginGuard(m_session.origin).resolve(QStringLiteral("/rest/im.user.list.get.json"));
    webRequest(url, fields, recentUserStatusOp, QStringLiteral("result"),
               [this, pending = std::move(pending), chats = std::move(chats), generation](QJsonValue data) mutable {
        if (data.isNull() || generation != m_recentStatusGeneration)
            return;
        const auto record = [this, &pending](const QJsonObject &user, const QString &id) {
            if (pending.contains(id))
                rememberDismissalStatus(id, dismissedPersonalUser(id, {}, user));
        };
        if (data.isArray()) {
            for (const auto item : data.toArray()) {
                const auto user = item.toObject();
                record(user, numericId(field(user, {"id", "ID"})));
            }
        } else {
            const auto keyed = data.toObject();
            for (auto it = keyed.constBegin(); it != keyed.constEnd(); ++it) {
                const auto id = numericId(it.key());
                const auto user = it.value().toObject();
                const auto embedded = oneOf(user, {"id", "ID"});
                if (embedded.isUndefined() || numericId(string(embedded)) == id)
                    record(user, id);
            }
        }
        bool changed = false;
        for (auto &chat : chats) {
            if (pending.contains(chat.id) && cachedDismissalStatus(chat.id) == true && !chat.dismissed) {
                chat.dismissed = true;
                changed = true;
            }
        }
        if (changed) {
            publishRecentChats(std::move(chats), false);
            if (m_userId.isEmpty())
                withCurrentUser(settingsIdentityOp, [](qint64) {});
        }
    }, recentUserStatusOp + QLatin1Char(':') + QString::number(generation));
}

CloudBackend::RecentOverlay &CloudBackend::recentOverlay(const QString &dialog)
{
    if (!m_recentOverlays.contains(dialog) && m_recentOverlays.size() >= 3 * tunables::kPageSizeChats)
        m_recentOverlays.erase(m_recentOverlays.begin());
    return m_recentOverlays[dialog];
}

void CloudBackend::publishRecentChats(QVector<Chat> chats, bool task)
{
    QSet<QString> listed;
    bool hasLivePreview = false;
    for (auto &chat : chats) {
        listed.insert(chat.id);
        decorateChatBadges(chat);
        applyChatContact(chat);
        const auto live = m_recentOverlays.constFind(chat.id);
        if (live == m_recentOverlays.cend())
            continue;
        if (live->counter)
            chat.unread = *live->counter;
        if (live->markedUnread)
            chat.markedUnread = *live->markedUnread;
        const auto rejectedChatId = task ? live->rejectedTaskPreviewChatId : live->rejectedRecentPreviewChatId;
        const auto &history = live->historyPreview;
        const bool recoveredPreview = rejectedChatId > 0 && rejectedChatId == history.chatId;
        if (recoveredPreview) {
            chat.lastMessage = history.text;
            if (history.updated.isValid())
                chat.updated = history.updated;
            hasLivePreview = true;
        }
        if (live->preview.updated.isValid() && (!chat.updated.isValid() || live->preview.updated >= chat.updated)
            && (!recoveredPreview || live->messageId >= history.messageId)) {
            chat.lastMessage = live->preview.lastMessage;
            chat.updated = live->preview.updated;
            hasLivePreview = true;
        }
    }
    for (const auto &live : std::as_const(m_recentOverlays)) {
        const auto &preview = live.preview;
        if (preview.id.isEmpty() || preview.taskChat != task || listed.contains(preview.id)
            || (live.counter.value_or(0) == 0 && !live.markedUnread.value_or(false)))
            continue;
        auto chat = preview;
        decorateChatBadges(chat);
        applyChatContact(chat);
        chat.unread = live.counter.value_or(0);
        chat.markedUnread = live.markedUnread.value_or(false);
        chats.append(std::move(chat));
        hasLivePreview = true;
    }
    if (hasLivePreview) {
        std::stable_sort(chats.begin(), chats.end(), [this](const Chat &left, const Chat &right) {
            if (left.pinned != right.pinned)
                return left.pinned;
            if (left.pinned)
                return false;
            if (left.updated != right.updated)
                return left.updated > right.updated;
            return m_recentOverlays.value(left.id).messageId > m_recentOverlays.value(right.id).messageId;
        });
    }
    if (chats.size() > tunables::kPageSizeChats)
        chats.resize(tunables::kPageSizeChats);
    if (task) {
        m_recentTaskChatItems = std::move(chats);
        emit taskChatsLoaded(m_recentTaskChatItems);
    } else {
        m_recentChatItems = std::move(chats);
        emit recentChatsLoaded(m_recentChatItems);
    }
    if (!m_haveCounterSnapshot)
        publishUnreadChatCount();
}

void CloudBackend::applyRecentPull(const QString &command, const QString &dialog, const QString &chatId,
                                  const QJsonObject &params, const QJsonObject &extra)
{
    if (dialog.isEmpty())
        return;
    const bool messageEvent = command == QLatin1String("message") || command == QLatin1String("messageChat");
    const bool manualRead = command == QLatin1String("chatUnread");
    const bool readEvent = command == QLatin1String("readMessage") || command == QLatin1String("readMessageChat");
    if (!messageEvent && !manualRead && !readEvent)
        return;

    const auto counter = unreadCount(params.value(QStringLiteral("counter")));
    const auto marked = manualRead ? std::optional<bool>(params.value(QStringLiteral("active")).toBool())
        : readEvent ? notificationFlag(params.value(QStringLiteral("unread"))) : std::nullopt;
    if (!counter && !marked)
        return;
    const auto message = params.value(QStringLiteral("message")).toObject();
    const auto messageId = numericId(field(message, {"id"})).toLongLong();
    const auto author = numericId(field(message, {"senderId", "authorId"}));
    const auto state = m_dialogMessageStates.constFind(dialog);
    if (messageEvent) {
        const auto groupChatId = groupDialogChatId(dialog);
        const auto knownChatId = groupChatId > 0 ? groupChatId : badgeChatId(dialog);
        const auto eventChatId = chatId.toLongLong();
        const auto expectedChatId = knownChatId > 0 ? knownChatId : eventChatId;
        const auto messageChatId = explicitMessageChatId(message);
        if ((knownChatId > 0 && eventChatId > 0 && eventChatId != knownChatId)
            || (messageChatId != 0 && messageChatId != expectedChatId))
            return;
        if (!counter || messageId <= 0 || author.isEmpty() || author == m_userId
            || m_sentTemplates.contains(field(message, {"templateId", "uuid"})))
            return;
        if (messageId <= m_recentOverlays.value(dialog).messageId
            || (state != m_dialogMessageStates.cend()
                && (messageId <= state->lastReadId || state->exactlyRead.contains(messageId))))
            return;
        const auto sent = date(extra.value(QStringLiteral("server_time_unix")));
        if (extra.value(QStringLiteral("server_time_ago")).toInteger() > freshPopupSeconds
            || (sent.isValid() && sent.secsTo(QDateTime::currentDateTimeUtc()) > freshPopupSeconds))
            return;
        if (m_userId.isEmpty()) {
            if (m_recentPullsAwaitingIdentity.size() >= 8)
                m_recentPullsAwaitingIdentity.removeFirst();
            m_recentPullsAwaitingIdentity.append({command, dialog, chatId, params, extra});
            withCurrentUser(settingsIdentityOp, [this](qint64 id) {
                if (id == 0)
                    m_recentPullsAwaitingIdentity.clear();
            });
            return;
        }
        const auto members = params.value(QStringLiteral("userInChat")).toObject().value(chatId);
        if (members.isArray()) {
            const auto users = members.toArray();
            if (std::none_of(users.cbegin(), users.cend(), [this](const QJsonValue &user) { return string(user) == m_userId; }))
                return;
        }
    }

    const auto sections = params.value(QStringLiteral("recentConfig")).toObject().value(QStringLiteral("sections")).toArray();
    const auto lookup = [&dialog](const QVector<Chat> &chats) {
        return std::find_if(chats.cbegin(), chats.cend(), [&dialog](const Chat &chat) { return chat.id == dialog; });
    };
    const auto ordinary = lookup(m_recentChatItems);
    const auto task = lookup(m_recentTaskChatItems);
    const bool listedOrdinary = ordinary != m_recentChatItems.cend();
    const bool listedTask = task != m_recentTaskChatItems.cend();
    const auto chat = params.value(QStringLiteral("chat")).toObject().value(chatId).toObject();
    const auto user = params.value(QStringLiteral("users")).toObject().value(dialog).toObject();
    auto candidate = chatFromMetadata(dialog, {}, chat, user);
    candidate.serverChatId = chatId.toLongLong();
    candidate.taskChat = isRecentTaskChat(candidate.taskChat, listedTask, sections);
    const bool listed = candidate.taskChat ? listedTask : listedOrdinary;
    const auto previous = m_recentOverlays.constFind(dialog);
    const bool knownRow = listed || (previous != m_recentOverlays.cend() && !previous->preview.id.isEmpty());
    if (candidate.taskChat && listedTask)
        candidate = *task;
    else if (!candidate.taskChat && listedOrdinary)
        candidate = *ordinary;

    const auto eventChatId = chatId.toLongLong();
    const auto canonicalChatId = eventChatId > 0 ? eventChatId
        : candidate.serverChatId > 0 ? candidate.serverChatId : badgeChatId(dialog);
    rememberProjectUnreadTaskChat(candidate);
    const bool inRecentScope = knownRow || (candidate.taskChat ? !sections.isEmpty()
                                                               : sections.contains(QStringLiteral("default")));
    const bool inProjectScope = (m_projectTaskChatIds.contains(canonicalChatId)
        || m_projectUnreadParentsByChat.contains(canonicalChatId) || m_projectUnreadTasksByChat.contains(canonicalChatId))
        && dialog == chatDialog(QString::number(canonicalChatId));
    // Reads, manual marks and updates for snapshot chats also affect the global count off page.
    if (!inRecentScope && !inProjectScope
        && (canonicalChatId <= 0 || (messageEvent && !m_chatCounterBadges.contains(canonicalChatId))))
        return;
    auto &live = recentOverlay(dialog);
    live.chatId = canonicalChatId;
    live.counterRevision = ++m_chatBadgeRevision;
    if (counter)
        live.counter = counter;
    if (marked)
        live.markedUnread = marked;
    live.eventTime = std::max(live.eventTime, field(extra, {"server_time_unix"}).toLongLong());
    if (messageEvent) {
        live.messageId = messageId;
        live.incomingId = messageId;
        if (inRecentScope && (!params.value(QStringLiteral("skipRecentUpdate")).toBool() || !knownRow)) {
            if (candidate.title.isEmpty())
                candidate.title = m_recentTitles.value(dialog, QStringLiteral("Диалог %1").arg(dialog));
            candidate.lastMessage = field(message, {"text", "message"}).left(2000);
            candidate.updated = date(message.value(QStringLiteral("date")));
            if (!candidate.updated.isValid())
                candidate.updated = date(extra.value(QStringLiteral("server_time_unix")));
            if (!candidate.updated.isValid())
                candidate.updated = QDateTime::currentDateTimeUtc();
            live.preview = candidate;
        }
    }
    if (inRecentScope && (knownRow || live.counter.value_or(0) > 0 || live.markedUnread.value_or(false)))
        publishRecentChats(candidate.taskChat ? m_recentTaskChatItems : m_recentChatItems, candidate.taskChat);
    if (inProjectScope)
        publishProjectTaskChatBadges(canonicalChatId);
    publishUnreadChatCount();
}

bool CloudBackend::acceptsRecentRead(const QString &dialog, const QJsonObject &params, const QJsonObject &extra) const
{
    const auto live = m_recentOverlays.constFind(dialog);
    const auto state = m_dialogMessageStates.constFind(dialog);
    const auto currentRead = state == m_dialogMessageStates.cend() ? -1 : state->lastReadId;
    const auto eventTime = field(extra, {"server_time_unix"}).toLongLong();
    bool hasRead = false;
    const auto lastRead = field(params, {"lastId"}).toLongLong(&hasRead);
    const bool exact = notificationFlag(params.value(QStringLiteral("exact"))).value_or(false);
    if (hasRead && !exact && lastRead < currentRead)
        return false;
    if (live == m_recentOverlays.cend())
        return true;
    if (eventTime > 0 && eventTime < live->eventTime)
        return false;
    if (!hasRead || live->incomingId <= lastRead || live->counter.value_or(0) <= 0)
        return true;

    const auto viewed = params.value(QStringLiteral("viewedMessages")).toArray();
    if (std::any_of(viewed.cbegin(), viewed.cend(), [id = live->incomingId](const QJsonValue &value) {
            return string(value).toLongLong() == id;
        }))
        return true;
    // A read below the latest unread message can clear earlier messages. It cannot clear the
    // whole chat. Require a current server event or advancing cursor before adopting a partial count.
    const auto counter = unreadCount(params.value(QStringLiteral("counter")));
    return counter != 0 && (lastRead > currentRead || (eventTime > 0 && eventTime > live->eventTime));
}

void CloudBackend::rememberTaskTitle(const QString &id, const QString &title)
{
    if (id.isEmpty() || title.isEmpty())
        return;
    if (!m_taskChatTitles.contains(id) && m_taskChatTitles.size() >= 2 * tunables::kPageSizeChats)
        m_taskChatTitles.erase(m_taskChatTitles.begin());
    m_taskChatTitles.insert(id, title);
}

void CloudBackend::resolveTaskChatTitles(QVector<Chat> chats, const QString &key, std::function<void(QVector<Chat>)> deliver)
{
    Fields fields{{QStringLiteral("select[]"), QStringLiteral("ID")},
                  {QStringLiteral("select[]"), QStringLiteral("TITLE")},
                  {QStringLiteral("start"), QStringLiteral("-1")}};
    QSet<QString> missing;
    for (auto &chat : chats) {
        if (chat.taskId.isEmpty())
            continue;
        const auto title = m_taskChatTitles.value(chat.taskId);
        if (!title.isEmpty()) {
            chat.title = title;
        } else if (!missing.contains(chat.taskId)) {
            fields.append({QStringLiteral("filter[ID][%1]").arg(missing.size()), chat.taskId});
            missing.insert(chat.taskId);
        }
    }
    // Show available rows even if the optional title batch is unavailable.
    deliver(chats);
    if (missing.isEmpty())
        return;
    ajax(QStringLiteral("tasks.task.list"), fields, QStringLiteral("task-chat-titles"),
         [this, chats = std::move(chats), missing = std::move(missing), deliver = std::move(deliver)](QJsonValue data) mutable {
        for (const auto row : data.toObject().value(QStringLiteral("tasks")).toArray()) {
            const auto item = row.toObject();
            const auto id = numericId(field(item, {"id", "ID"}));
            if (missing.contains(id))
                rememberTaskTitle(id, field(item, {"title", "TITLE"}));
        }
        bool changed = false;
        for (auto &chat : chats) {
            const auto title = m_taskChatTitles.value(chat.taskId);
            if (!title.isEmpty() && chat.title != title) {
                chat.title = title;
                changed = true;
            }
        }
        if (changed)
            deliver(std::move(chats));
    }, QStringLiteral("task-chat-titles:") + key);
}

bool CloudBackend::scheduleHistoryRetry(const QString &operation, quint64 revision)
{
    const bool task = operation.startsWith(QLatin1String("task-messages:"));
    const auto key = operation.section(QLatin1Char(':'), 1);
    auto &attempt = task ? m_taskHistoryRetry : m_chatHistoryRetry;
    const auto dialog = attempt.dialog;
    const auto scope = m_accountScopeRevision;
    const auto current = [this, task, key, dialog, revision, scope] {
        return m_started && canAutoRefreshChats() && !m_authExpired && scope == m_accountScopeRevision
            && !dialog.isEmpty() && (task ? m_openTask == key && m_taskDialogs.value(key) == dialog
                                         : m_activeSection == 0 && m_openDialog == dialog)
            && (task ? m_taskHistoryRetry : m_chatHistoryRetry).revision == revision;
    };
    if (attempt.used || !current())
        return false;
    attempt.used = true;
    QTimer::singleShot(350, this, [this, key, dialog, task, current] {
        if (current())
            loadMessages(key, dialog, task, true);
    });
    return true;
}

void CloudBackend::loadMessages(const QString &key, const QString &dialogId, bool task, bool retry)
{
    if (m_userId.isEmpty() && (!m_triedHistoryIdentity || m_inflightOperations.contains(chatIdentityOp))) {
        auto &pending = m_historiesWaitingForIdentity;
        pending.erase(std::remove_if(pending.begin(), pending.end(), [task](const auto &request) {
            return request.task == task;
        }), pending.end());
        pending.append({key, dialogId, task});
        if (!m_triedHistoryIdentity) {
            m_triedHistoryIdentity = true;
            withCurrentUser(chatIdentityOp, [this](qint64) { resumeHistoriesWithIdentity(); });
        }
        return;
    }
    const auto operation = (task ? QStringLiteral("task-messages:") : QStringLiteral("chat:")) + key;
    if (m_inflightOperations.contains(operation))
        return;
    auto &attempt = task ? m_taskHistoryRetry : m_chatHistoryRetry;
    attempt.dialog = dialogId;
    ++attempt.revision;
    attempt.used = retry;
    const auto generation = m_dialogMessageStates.value(dialogId).historyGeneration;
    const auto superseded = [this, key, dialogId, task, generation, scope = m_accountScopeRevision] {
        if (scope != m_accountScopeRevision)
            return true;
        if (m_dialogMessageStates.value(dialogId).historyGeneration == generation)
            return false;
        if (task ? m_openTask == key : m_openDialog == dialogId) {
            (task ? m_taskMessagesDirty : m_chatDirty) = true;
            scheduleRefresh();
        }
        return true;
    };
    const auto expectedChatId = badgeChatId(dialogId);
    const auto groupChatId = groupDialogChatId(dialogId);
    const auto popupChatId = groupChatId > 0 ? groupChatId : expectedChatId;
    const auto snapshotOperation = (expectedChatId > 0 ? historySnapshotPrefix : historyReadPrefix) + operation;
    const auto deliver = [this, key, dialogId, popupChatId, task](const QJsonObject &history, bool trustedMetadata) {
        const auto messages = decodeMessages(history, true, dialogId);
        if (trustedMetadata && (task ? m_openTask == key : m_openDialog == dialogId)) {
            rememberRecentHistoryPreview(dialogId, history);
            reconcileHistoryPopups(dialogId, popupChatId, task, history, messages);
        }
        if (task)
            emit taskMessagesLoaded(key, messages);
        else
            emit chatMessagesLoaded(key, messages);
    };
    ajax(QStringLiteral("im.v2.Chat.load"),
         {{QStringLiteral("dialogId"), dialogId}, {QStringLiteral("messageLimit"), QStringLiteral("25")}},
         snapshotOperation, [this, key, dialogId, task, operation, superseded, deliver, popupChatId,
                             counterRevision = ++m_chatBadgeRevision](QJsonValue data) {
        if (superseded())
            return;
        const auto object = data.toObject();
        if (!matchesRequestedHistory(object, dialogId, popupChatId)) {
            // Retain direct history rows for existing callers without accepting the conflicting chat metadata.
            if (object.value(QStringLiteral("messages")).isArray())
                deliver(object, false);
            else
                failShape(operation);
            return;
        }
        const auto chat = object.value(QStringLiteral("chat")).toObject();
        const auto publishHistory = [this, key, dialogId, task, chat, popupChatId, counterRevision, deliver]
            (const QJsonObject &history) {
            if (task ? m_openTask == key : m_openDialog == dialogId) {
                const auto rows = history.value(QStringLiteral("messages")).toArray();
                rememberUnreadHistoryRows(dialogId, popupChatId > 0 ? popupChatId : badgeChatId(dialogId),
                                          rows, counterRevision);
                reconcileHistoryUnread(dialogId, chat, rows, popupChatId, counterRevision);
            }
            deliver(history, true);
        };
        if (data.isObject()) {
            const auto live = m_recentOverlays.constFind(dialogId);
            const auto counterCurrent = m_chatCounterSnapshotRevision <= counterRevision
                && (live == m_recentOverlays.cend() || live->counterRevision <= counterRevision);
            rememberDialog(dialogId, object, counterCurrent);
            rememberDialogContact(dialogId, object);
            acceptPinnedSnapshot(dialogId, object);
        } else if (task ? m_openTask != key : m_openDialog != dialogId) {
            return;
        }
        if (object.value(QStringLiteral("messages")).isArray()) {
            publishHistory(object);
            return;
        }
        auto chatId = field(object, {"chatId", "CHAT_ID"});
        if (chatId.isEmpty())
            chatId = QString::number(popupChatId > 0 ? popupChatId : badgeChatId(dialogId));
        const auto pagingChatId = numericId(chatId).toLongLong();
        if (pagingChatId <= 0 || (popupChatId > 0 && pagingChatId != popupChatId))
            return failShape(operation);

        ajax(QStringLiteral("im.v2.Chat.Message.list"),
             {{QStringLiteral("chatId"), chatId}, {QStringLiteral("limit"), QStringLiteral("25")}},
             historyReadPrefix + operation, [this, operation, dialogId, pagingChatId, superseded, publishHistory](QJsonValue page) {
            if (superseded())
                return;
            const auto result = page.toObject();
            if (!result.value(QStringLiteral("messages")).isArray() || !matchesRequestedHistory(result, dialogId, pagingChatId))
                return failShape(operation);
            publishHistory(result);
        }, operation);
    }, operation);
}

void CloudBackend::rememberUnreadHistoryRows(const QString &dialog, qint64 chatId, const QJsonArray &rows, quint64 revision)
{
    if (chatId <= 0 || badgeChatId(dialog) != chatId)
        return;
    auto &state = dialogMessageState(dialog);
    if (state.readReceiptRevision > revision)
        return;
    const auto currentUser = numericId(m_userId).toLongLong();
    QSet<qint64> pageUnread;
    for (const auto &value : rows) {
        const auto row = value.toObject();
        const auto id = recentMessageId(row);
        const auto unread = row.value(QStringLiteral("unread"));
        if (id <= 0 || !unread.isBool() || explicitMessageChatId(row) != chatId)
            continue;
        if (!unread.toBool() || id <= state.confirmedReadThrough || state.exactlyRead.contains(id)) {
            state.historyUnread.remove(id);
            pageUnread.remove(id);
            continue;
        }
        const auto author = explicitMessageAuthorId(row);
        if (author && (*author == 0 || (currentUser > 0 && *author != currentUser))
            && pageUnread.size() < tunables::kPageSizeMessages)
            pageUnread.insert(id);
    }
    state.historyUnread.subtract(pageUnread);
    const auto excess = state.historyUnread.size() + pageUnread.size() - tunables::kRetainedChatMessages;
    if (excess > 0) {
        auto oldest = state.historyUnread.values();
        std::sort(oldest.begin(), oldest.end());
        for (qsizetype index = 0; index < excess; ++index)
            state.historyUnread.remove(oldest.at(index));
    }
    state.historyUnread.unite(pageUnread);
}

void CloudBackend::reconcileHistoryUnread(const QString &dialog, const QJsonObject &chat, const QJsonArray &messages,
                                         qint64 expectedChatId, quint64 revision)
{
    const auto chatId = numericId(field(chat, {"id", "chatId"})).toLongLong();
    const auto groupId = groupDialogChatId(dialog);
    const auto chatIdAlias = chat.value(QStringLiteral("chatId"));
    const auto firstUnreadId = unreadCount(chat.value(QStringLiteral("unreadId")));
    const auto readIdValue = chat.value(QStringLiteral("lastId"));
    bool hasReadId = true;
    const auto readId = readIdValue.isString() ? readIdValue.toString().toLongLong(&hasReadId)
        : readIdValue.toInteger(-1);
    if (chatId <= 0 || (expectedChatId > 0 && chatId != expectedChatId)
        || (groupId > 0 && chatId != groupId)
        || (!chatIdAlias.isUndefined() && numericId(string(chatIdAlias)).toLongLong() != chatId)
        || validDialog(field(chat, {"dialogId"})) != dialog || firstUnreadId != 0
        || !hasReadId || readId < 0
        || m_chatCounterSnapshotRevision > revision)
        return;
    const auto state = m_dialogMessageStates.constFind(dialog);
    if (state != m_dialogMessageStates.cend() && readId < state->lastReadId)
        return;
    auto &live = recentOverlay(dialog);
    if (live.counterRevision > revision || live.incomingId > readId)
        return;

    // Chat metadata carries a participant count. Only its explicit no-unread cursor can
    // repair badges, and only if the exact scoped history does not contradict it.
    for (const auto value : messages) {
        const auto row = value.toObject();
        if (recentMessageId(row) > 0 && explicitMessageChatId(row) == chatId
            && notificationFlag(row.value(QStringLiteral("unread"))).value_or(false))
            return;
    }

    const bool changed = live.chatId != chatId || live.counter != 0;
    live.chatId = chatId;
    live.counterRevision = revision;
    live.counter = 0;
    if (changed) {
        publishUnreadChatCount();
        republishChatBadges(chatId);
    }
}

void CloudBackend::rememberRecentHistoryPreview(const QString &dialog, const QJsonObject &history)
{
    const auto groupChatId = groupDialogChatId(dialog);
    const auto expectedChatId = groupChatId > 0 ? groupChatId : badgeChatId(dialog);
    if (expectedChatId <= 0)
        return;
    QJsonObject latest;
    qint64 latestId = 0;
    for (const auto &value : history.value(QStringLiteral("messages")).toArray()) {
        const auto message = value.toObject();
        const auto id = recentMessageId(message);
        if (id > latestId && explicitMessageChatId(message) == expectedChatId) {
            latest = message;
            latestId = id;
        }
    }
    if (latestId <= 0)
        return;
    auto &live = recentOverlay(dialog);
    const auto text = field(latest, {"text", "message"}).left(2000);
    const auto updated = date(latest.value(QStringLiteral("date")));
    const auto &previous = live.historyPreview;
    if (previous.chatId == expectedChatId
        && (latestId < previous.messageId
            || (latestId == previous.messageId && text == previous.text && updated == previous.updated)))
        return;
    live.historyPreview = {expectedChatId, latestId, text, updated};
    const bool recoverRecent = live.rejectedRecentPreviewChatId == expectedChatId;
    const bool recoverTask = live.rejectedTaskPreviewChatId == expectedChatId;
    if (recoverRecent)
        publishRecentChats(m_recentChatItems, false);
    if (recoverTask)
        publishRecentChats(m_recentTaskChatItems, true);
}

void CloudBackend::resumeHistoriesWithIdentity()
{
    const auto histories = std::exchange(m_historiesWaitingForIdentity, {});
    for (const auto &request : histories)
        loadMessages(request.key, request.dialog, request.task);
}

void CloudBackend::requestChatMessages(const QString &dialogId)
{
    const auto valid = validDialog(dialogId);
    if (valid.isEmpty()) {
        emit failed(QStringLiteral("chat:") + dialogId, QStringLiteral("Некорректный номер диалога"));
        return;
    }
    if (m_openDialog != valid) {
        ++m_chatHistoryRetry.revision;
        m_openDialog = valid;
        m_chatDirty = false;
        m_chatPopupHistory = {};
    }
    if (m_activeSection == 0)
        updateHistoryCatchup(true);
    if (m_activeSection == 0 && canAutoRefreshChats() && (m_requests.size() >= maxRequests
        || m_inflightOperations.contains(QStringLiteral("chat:") + dialogId))) {
        m_chatDirty = true;
        scheduleRefresh();
        return;
    }
    loadMessages(dialogId, valid, false);
}

QString CloudBackend::settingsScope() const
{
    const auto identity = numericId(m_userId);
    if (identity.isEmpty())
        return {};
    const auto origin = m_session.origin.toEncoded(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment);
    const auto digest = QCryptographicHash::hash(origin + '\n' + identity.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QString::fromLatin1(digest);
}

QString CloudBackend::projectPinSettingsKey() const
{
    const auto scope = settingsScope();
    return scope.isEmpty() ? QString() : (m_taskProjectScope ? QStringLiteral("task-project-pins/")
                                                             : QStringLiteral("project-pins/")) + scope;
}

namespace {

QString projectHtmlText(QString text)
{
    return text.replace(QLatin1String("&quot;"), QLatin1String("\""))
        .replace(QLatin1String("&#39;"), QLatin1String("'"))
        .replace(QLatin1String("&lt;"), QLatin1String("<"))
        .replace(QLatin1String("&gt;"), QLatin1String(">"))
        .replace(QLatin1String("&nbsp;"), QLatin1String(" "))
        .replace(QLatin1String("&amp;"), QLatin1String("&"));
}

QHash<QString, QString> projectHtmlAttributes(const QString &attrs)
{
    static const QRegularExpression attribute(QString::fromLatin1(
        R"attribute(([a-zA-Z][\w:-]*)\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+)))attribute"));
    QHash<QString, QString> values;
    for (auto matches = attribute.globalMatch(attrs); matches.hasNext();) {
        const auto match = matches.next();
        values.insert(match.captured(1).toLower(), projectHtmlText(match.captured(match.lastCapturedIndex())));
    }
    return values;
}

bool hasHtmlClass(const QHash<QString, QString> &attrs, const QString &name)
{
    static const QRegularExpression spacing(QStringLiteral("\\s+"));
    return attrs.value(QStringLiteral("class")).split(spacing, Qt::SkipEmptyParts).contains(name);
}

QUrl safeProjectHref(const QUrl &base, const QString &href)
{
    if (href.isEmpty() || href.size() > 2048)
        return {};
    auto url = base.resolved(QUrl(href, QUrl::StrictMode));
    if (!url.isValid() || !url.userInfo().isEmpty() || url.hasFragment()
        || !OriginGuard(base).isSameOrigin(url))
        return {};
    QUrlQuery query(url);
    for (const auto &item : query.queryItems(QUrl::FullyDecoded)) {
        const auto key = item.first.toLower();
        if (key.contains(QLatin1String("token")) || key == QLatin1String("sessid")
            || key == QLatin1String("auth") || key == QLatin1String("logout") || key == QLatin1String("action"))
            return {};
    }
    // Composite cache selection belongs to the request, not to the grid scope or cursor.
    if (query.hasQueryItem(QStringLiteral("ncc"))) {
        query.removeAllQueryItems(QStringLiteral("ncc"));
        if (query.isEmpty())
            url.setQuery(QString());
        else
            url.setQuery(query);
    }
    return url;
}

const QRegularExpression projectAnchors(QString::fromLatin1(R"(<a\b([^<>]*)>([\s\S]*?)</a\s*>)"),
                                        QRegularExpression::CaseInsensitiveOption);

QString cleanProjectPath(const QUrl &url)
{
    auto path = url.path(QUrl::FullyDecoded);
    if (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return path;
}

QUrl projectGridHref(const QString &html, const QUrl &tasksPage)
{
    static const QRegularExpression projectTab(QStringLiteral("^/(project|projects|group|groups)$"));
    const auto personalPath = cleanProjectPath(tasksPage);
    for (auto it = projectAnchors.globalMatch(html); it.hasNext();) {
        const auto match = it.next();
        const auto attrs = projectHtmlAttributes(match.captured(1));
        const auto url = safeProjectHref(tasksPage, attrs.value(QStringLiteral("href")));
        const auto path = cleanProjectPath(url);
        for (const auto &prefix : {personalPath, QStringLiteral("/tasks")}) {
            if (path.startsWith(prefix) && projectTab.match(path.mid(prefix.size())).hasMatch())
                return url;
        }
    }
    return {};
}

struct ProjectPageCursor
{
    QString gridPath;
    QString name;
    qint64 number = 0;
    bool pathBased = false;
};

ProjectPageCursor projectPageCursor(const QUrl &url)
{
    static const QRegularExpression namedPage(QStringLiteral("^page-([1-9][0-9]{0,8})(?:-size-[1-9][0-9]{0,8})?$"));
    static const QRegularExpression classicPage(QStringLiteral("^PAGEN_[1-9][0-9]*$"));
    static const QRegularExpression pathPage(
        QStringLiteral("^(.+)/([A-Za-z0-9_-]+)/page-([1-9][0-9]{0,8})(?:/size-[1-9][0-9]{0,8})?$"));
    ProjectPageCursor cursor{cleanProjectPath(url), {}};
    const auto pathMatch = pathPage.match(cursor.gridPath);
    if (pathMatch.hasMatch())
        cursor = {pathMatch.captured(1), pathMatch.captured(2), pathMatch.captured(3).toLongLong(), true};

    const auto queryItems = QUrlQuery(url).queryItems(QUrl::FullyDecoded);
    for (const auto &[key, value] : queryItems) {
        const auto named = namedPage.match(value).captured(1);
        const auto number = !named.isEmpty() ? named.toLongLong()
            : classicPage.match(key).hasMatch() ? numericId(value).toLongLong() : 0;
        if (number <= 0)
            continue;
        if (cursor.number != 0)
            return {};
        cursor.name = key;
        cursor.number = number;
    }
    if (cursor.number && !cursor.pathBased
        && std::count_if(queryItems.cbegin(), queryItems.cend(), [&](const auto &item) {
            return item.first == cursor.name;
        }) != 1)
        return {};
    return cursor;
}

QMap<QString, QStringList> projectPageScope(const QUrl &url, const ProjectPageCursor &cursor)
{
    QMap<QString, QStringList> scope;
    for (const auto &[key, value] : QUrlQuery(url).queryItems(QUrl::FullyDecoded)) {
        if (cursor.pathBased || !cursor.number || key != cursor.name)
            scope[key].append(value);
    }
    return scope;
}

QUrl nextProjectGridHref(const QString &html, const QUrl &current)
{
    static const QRegularExpression pagerAnchors(QString::fromLatin1(
        R"((?:<li\b([^<>]*)>\s*)?<a\b([^<>]*)>[\s\S]*?</a\s*>)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression nextRelation(QStringLiteral("(?:^|\\s)next(?:\\s|$)"),
        QRegularExpression::CaseInsensitiveOption);
    const auto cursor = projectPageCursor(current);
    if (cursor.gridPath.isEmpty() || cursor.number == std::numeric_limits<qint64>::max())
        return {};
    const auto nextNumber = std::max<qint64>(cursor.number, 1) + 1;
    const auto scope = projectPageScope(current, cursor);
    QUrl marked;
    QUrl bare;
    bool conflictingMarked = false;
    bool conflictingBare = false;
    for (auto it = pagerAnchors.globalMatch(html); it.hasNext();) {
        const auto match = it.next();
        const auto attrs = projectHtmlAttributes(match.captured(2));
        const auto candidate = safeProjectHref(current, attrs.value(QStringLiteral("href")));
        const auto next = projectPageCursor(candidate);
        if (next.gridPath != cursor.gridPath || next.number != nextNumber)
            continue;
        const auto nextScope = projectPageScope(candidate, next);
        if (cursor.number) {
            if (next.name != cursor.name || next.pathBased != cursor.pathBased || nextScope != scope)
                continue;
        } else {
            const auto scopeItems = scope.asKeyValueRange();
            if (!std::all_of(scopeItems.begin(), scopeItems.end(), [&](const auto &item) {
                return nextScope.value(item.first) == item.second;
            }))
                continue;
        }

        const bool explicitNext = nextRelation.match(attrs.value(QStringLiteral("rel"))).hasMatch()
            || hasHtmlClass(attrs, QStringLiteral("bx-pag-next"))
            || hasHtmlClass(projectHtmlAttributes(match.captured(1)), QStringLiteral("bx-pag-next"));
        auto &selected = explicitNext ? marked : bare;
        auto &conflicting = explicitNext ? conflictingMarked : conflictingBare;
        if (selected.isEmpty())
            selected = candidate;
        else if (selected != candidate)
            conflicting = true;
    }
    if (!marked.isEmpty())
        return conflictingMarked ? QUrl{} : marked;
    return conflictingBare ? QUrl{} : bare;
}

QString projectAvatarColor(const QString &value)
{
    static const QRegularExpression hex(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
    return hex.match(value).hasMatch() ? value : QString();
}

} // namespace

void CloudBackend::fetchPortalHtml(const QUrl &url, std::function<void(QString)> handler)
{
    if (!m_started || m_authExpired || !sessionUsable(m_session, m_allowLoopback)
        || !OriginGuard(m_session.origin).isSameOrigin(url) || !url.userInfo().isEmpty() || url.hasFragment()
        || m_requests.size() >= maxRequests) {
        handler({});
        return;
    }
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setRawHeader("Cookie", m_session.cookie);
    request.setRawHeader("Accept", "text/html");
    request.setTransferTimeout(8000);
    auto *reply = m_http.get(request);
    m_requests.insert(reply, {});
    reply->setReadBufferSize(maxResponse + 1);
    QTimer::singleShot(8000, reply, [reply] {
        if (!reply->isFinished())
            reply->abort();
    });
    connect(reply, &QIODevice::readyRead, this, [reply] {
        if (reply->bytesAvailable() > maxResponse)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, scope = m_accountScopeRevision, handler = std::move(handler)] {
        m_requests.remove(reply);
        reply->deleteLater();
        if (!m_started || scope != m_accountScopeRevision)
            return;
        scheduleRefresh();
        const auto contentType = reply->rawHeader("Content-Type").split(';').first().trimmed().toLower();
        if (reply->error() != QNetworkReply::NoError
            || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200
            || contentType != "text/html" || reply->bytesAvailable() > maxResponse) {
            handler({});
            return;
        }
        handler(QString::fromUtf8(reply->readAll()));
    });
}

void CloudBackend::requestProjects()
{
    if (m_loadingProjects)
        return;
    m_loadingProjects = true;
    m_loadedProjects.clear();
    m_nextProjectGridPage.clear();
    m_nextProjectOffset = -1;
    m_emptyProjectPages = 0;
    withCurrentUser(projectPinIdentityOp, [this](qint64 userId) {
        if (userId <= 0) {
            loadProjectFallback(0);
            return;
        }
        const auto taskPage = OriginGuard(m_session.origin).resolve(
            QStringLiteral("/company/personal/user/%1/tasks/").arg(userId));
        fetchPortalHtml(taskPage, [this, taskPage](const QString &html) {
            const auto grid = projectGridHref(html, taskPage);
            if (grid.isEmpty())
                loadProjectFallback(0);
            else
                loadProjectGrid(grid, true);
        });
    });
}

void CloudBackend::requestMoreProjects()
{
    if (m_loadingProjects || (m_nextProjectGridPage.isEmpty() && m_nextProjectOffset < 0))
        return;
    m_loadingProjects = true;
    if (!m_nextProjectGridPage.isEmpty())
        loadProjectGrid(m_nextProjectGridPage, false);
    else
        loadProjectFallback(m_nextProjectOffset);
}

void CloudBackend::loadProjectGrid(const QUrl &url, bool discovery)
{
    auto requestUrl = url;
    QUrlQuery query(requestUrl);
    // The cached page can contain only the Composite frame scaffold without project rows.
    query.addQueryItem(QStringLiteral("ncc"), QStringLiteral("1"));
    requestUrl.setQuery(query);
    fetchPortalHtml(requestUrl, [this, url, discovery](const QString &html) {
        static const QRegularExpression taskScope(QStringLiteral(
            "[\"']?pinMode[\"']?\\s*:\\s*[\"']tasks_project[\"']"));
        if (!html.contains(QLatin1String("BX.Socialnetwork.Project.List.Controller.init")) || !taskScope.match(html).hasMatch()) {
            if (discovery) {
                loadProjectFallback(0);
                return;
            }
            m_loadingProjects = false;
            emit projectsMoreAvailable(true);
            failShape(projectsOp);
            return;
        }
        m_taskProjectScope = true;
        static const QRegularExpression gridRows(QString::fromLatin1(R"(<tr\b([^<>]*)>([\s\S]*?)</tr\s*>)"),
                                                  QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression rowElements(QString::fromLatin1(R"(<(?:span|div|button|a)\b([^<>]*)>)"),
                                                    QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression tags(QStringLiteral("<[^>]*>"));
        QVector<Project> projects;
        for (auto it = gridRows.globalMatch(html); it.hasNext();) {
            const auto match = it.next();
            const auto attrs = projectHtmlAttributes(match.captured(1));
            if (!hasHtmlClass(attrs, QStringLiteral("main-grid-row")))
                continue;
            Project project;
            project.id = numericId(attrs.value(QStringLiteral("data-id")));
            if (project.id.isEmpty())
                continue;
            project.pinned = hasHtmlClass(attrs, QStringLiteral("sonet-ui-grid-row-pinned"));
            const auto row = match.captured(2);
            for (auto links = projectAnchors.globalMatch(row); links.hasNext();) {
                const auto link = links.next();
                const auto linkAttrs = projectHtmlAttributes(link.captured(1));
                if (hasHtmlClass(linkAttrs, QStringLiteral("socialnetwork-project-list-name-link")))
                    project.name = projectHtmlText(link.captured(2).remove(tags)).trimmed();
            }
            for (auto elements = rowElements.globalMatch(row); elements.hasNext();) {
                const auto avatar = projectHtmlAttributes(elements.next().captured(1));
                if (hasHtmlClass(avatar, QStringLiteral("main-grid-cell-content-action-pin"))
                    && (hasHtmlClass(avatar, QStringLiteral("main-grid-cell-content-action-active"))
                        || avatar.value(QStringLiteral("aria-pressed")) == QLatin1String("true")))
                    project.pinned = true;
                if (!hasHtmlClass(avatar, QStringLiteral("socialnetwork-project-list-avatar")))
                    continue;
                if (project.name.isEmpty())
                    project.name = avatar.value(QStringLiteral("data-title")).trimmed();
                project.avatarVariant = avatar.value(QStringLiteral("data-border-variant")) == QLatin1String("guest")
                    ? QStringLiteral("guest") : QStringLiteral("project");
                project.avatarColor = projectAvatarColor(avatar.value(QStringLiteral("data-color")));
                project.avatarUrl = avatarUrlFromPortal(avatar.value(QStringLiteral("data-pic")));
            }
            if (!project.name.isEmpty())
                projects.append(std::move(project));
        }
        m_nextProjectGridPage = nextProjectGridHref(html, url);
        publishProjectPage(std::move(projects));
    });
}

void CloudBackend::loadProjectFallback(qint64 offset)
{
    const auto offsetField = offset > 0 ? QString::number(offset) : QStringLiteral("0");
    ajax(QStringLiteral("socialnetwork.api.workgroup.list"),
         {{QStringLiteral("filter[PROJECT]"), QStringLiteral("Y")}, {QStringLiteral("filter[ACTIVE]"), QStringLiteral("Y")},
          {QStringLiteral("filter[CLOSED]"), QStringLiteral("N")}, {QStringLiteral("order[DATE_ACTIVITY]"), QStringLiteral("DESC")},
          {QStringLiteral("order[ID]"), QStringLiteral("ASC")}, {QStringLiteral("params[mode]"), QStringLiteral("mobile")},
          {QStringLiteral("start"), offsetField}, {QStringLiteral("select[0]"), QStringLiteral("ID")},
          {QStringLiteral("select[1]"), QStringLiteral("NAME")}, {QStringLiteral("select[2]"), QStringLiteral("AVATAR")},
          {QStringLiteral("select[3]"), QStringLiteral("TYPE")}, {QStringLiteral("select[4]"), QStringLiteral("AVATAR_TYPE")}},
         projectFallbackOp, [this, offset](QJsonValue data) {
            const auto rows = list(data, {"items", "groups", "workgroups", "result"});
            if (!rows.isArray()) {
                m_loadingProjects = false;
                emit projectsMoreAvailable(offset > 0);
                failShape(projectsOp);
                return;
            }
            const auto items = rows.toArray();
            if (offset == 0) {
                m_taskProjectScope = std::any_of(items.cbegin(), items.cend(), [](const QJsonValue &item) {
                    return item.toObject().value(QStringLiteral("additionalData")).toObject()
                        .value(QStringLiteral("role")).isString();
                });
            }
            const auto cacheKey = projectPinSettingsKey();
            QStringList cached;
            if (!cacheKey.isEmpty()) {
                QSettings settings(QStringLiteral("bitrix24-client"), QStringLiteral("bitrix24-client"));
                cached = settings.value(cacheKey).toStringList();
            }
            QVector<Project> projects;
            for (const auto row : items) {
                const auto item = row.toObject();
                const auto id = numericId(field(item, {"ID", "id"}));
                const auto name = field(item, {"NAME", "name"});
                if (id.isEmpty() || name.isEmpty())
                    continue;
                const auto extra = item.value(QStringLiteral("additionalData")).toObject();
                const auto role = field(extra, {"role"});
                if (m_taskProjectScope && role != QLatin1String("A") && role != QLatin1String("E") && role != QLatin1String("K"))
                    continue;
                const auto remotePin = notificationFlag(oneOf(item, {"pinned", "isPinned", "IS_PINNED"}));
                auto project = projectFromApiRow(item);
                project.pinned = remotePin.value_or(cached.contains(id));
                projects.append(std::move(project));
            }
            const auto explicitNext = numericId(field(data.toObject(), {"next"})).toLongLong();
            constexpr auto lastOffset = std::numeric_limits<int>::max() - tunables::kPageSizeProjects;
            m_nextProjectOffset = explicitNext > offset && explicitNext <= lastOffset ? explicitNext
                : items.size() == tunables::kPageSizeProjects && offset <= lastOffset
                ? offset + tunables::kPageSizeProjects : -1;
            publishProjectPage(std::move(projects));
    });
}

void CloudBackend::publishProjectPage(QVector<Project> rows)
{
    QSet<QString> ids;
    for (const auto &project : std::as_const(m_loadedProjects))
        ids.insert(project.id);
    for (auto &project : rows) {
        if (!ids.contains(project.id)) {
            ids.insert(project.id);
            m_loadedProjects.append(std::move(project));
        }
    }
    m_loadingProjects = false;
    const bool more = !m_nextProjectGridPage.isEmpty() || m_nextProjectOffset >= 0;
    if (m_loadedProjects.isEmpty() && more && ++m_emptyProjectPages <= 3) {
        requestMoreProjects();
        return;
    }
    emit projectsScopeLoaded(m_taskProjectScope);
    emit projectsMoreAvailable(more);
    emit projectsLoaded(m_loadedProjects);
}

void CloudBackend::requestProjectMembers(const QString &projectId)
{
    const auto operation = QStringLiteral("project-members:") + projectId;
    if (numericId(projectId).isEmpty()) {
        emit failed(operation, QStringLiteral("Некорректный номер проекта"));
        return;
    }
    const QJsonArray fields{QStringLiteral("OWNER_DATA"), QStringLiteral("LIST_OF_MEMBERS"),
        QStringLiteral("LIST_OF_MEMBERS_AWAITING_INVITE"), QStringLiteral("GROUP_MEMBERS_LIST")};
    ajaxJson(QStringLiteral("socialnetwork.api.workgroup.get"),
        {{QStringLiteral("params"), QJsonObject{{QStringLiteral("groupId"), projectId.toLongLong()},
                                                {QStringLiteral("select"), fields}}}},
        operation, [this, projectId, operation](QJsonValue data) {
        const auto payload = data.toObject();
        const auto memberList = payload.value(QStringLiteral("LIST_OF_MEMBERS"));
        if (field(payload, {"ID", "id"}) != projectId
            || (!memberList.isObject() && !memberList.isArray() && !payload.value(QStringLiteral("MEMBERS")).isArray())) {
            emit failed(operation, QStringLiteral("Не удалось получить участников проекта"));
            return;
        }
        QHash<QString, ProjectMember> entries;
        const auto append = [&entries](const QString &id, ProjectMemberRole role, const QJsonObject &row = {}) {
            if (id.isEmpty() || (entries.size() >= 200 && !entries.contains(id)))
                return;
            auto *entry = &entries[id];
            if (entry->id.isEmpty() || role < entry->role)
                entry->role = role;
            entry->id = id;
            auto name = field(row, {"FORMATTED_NAME"});
            if (name.isEmpty())
                name = (field(row, {"name"}) + QLatin1Char(' ') + field(row, {"lastName"})).trimmed();
            if (!name.isEmpty())
                entry->name = name.left(256);
            const auto position = field(row, {"position"});
            if (!position.isEmpty())
                entry->position = position.left(256);
        };
        const auto owner = numericId(field(payload, {"OWNER_ID"}));
        const auto ownerInfo = payload.value(QStringLiteral("OWNER_DATA")).toObject();
        append(owner, ProjectMemberRole::Owner, field(ownerInfo, {"ID", "id"}) == owner ? ownerInfo : QJsonObject());
        const QPair<const char *, ProjectMemberRole> roleLists[] = {
            {"MODERATOR_MEMBERS", ProjectMemberRole::Moderator}, {"ORDINARY_MEMBERS", ProjectMemberRole::Member},
            {"MEMBERS", ProjectMemberRole::Member}, {"INVITED_MEMBERS", ProjectMemberRole::Invited}};
        for (const auto &[key, role] : roleLists) {
            for (const auto value : payload.value(QLatin1String(key)).toArray())
                append(numericId(string(value)), role);
        }
        const auto visit = [&payload](const QString &key, const auto &consumer) {
            const auto values = payload.value(key);
            for (const auto value : values.toArray())
                consumer(value.toObject());
            for (const auto value : values.toObject())
                consumer(value.toObject());
        };
        visit(QStringLiteral("LIST_OF_MEMBERS"), [&append](const QJsonObject &row) {
            const auto role = row.value(QStringLiteral("isOwner")) == true ? ProjectMemberRole::Owner
                : row.value(QStringLiteral("isModerator")) == true ? ProjectMemberRole::Moderator : ProjectMemberRole::Member;
            append(numericId(field(row, {"id"})), role, row);
        });
        visit(QStringLiteral("LIST_OF_MEMBERS_AWAITING_INVITE"), [&append](const QJsonObject &row) {
            append(numericId(field(row, {"id"})), ProjectMemberRole::Awaiting, row);
        });
        visit(QStringLiteral("GROUP_MEMBERS_LIST"), [&append](const QJsonObject &row) {
            const auto id = numericId(field(row, {"id"}));
            if (row.value(QStringLiteral("isMember")) == true)
                append(id, ProjectMemberRole::Member);
            else if (row.value(QStringLiteral("invited")) == true)
                append(id, ProjectMemberRole::Invited);
            else if (row.value(QStringLiteral("isAwaiting")) == true)
                append(id, ProjectMemberRole::Awaiting);
        });
        auto roster = entries.values();
        for (auto &entry : roster) {
            if (entry.name.isEmpty())
                entry.name = QStringLiteral("Пользователь №%1").arg(entry.id);
        }
        std::sort(roster.begin(), roster.end(), [](const ProjectMember &a, const ProjectMember &b) {
            return a.role != b.role ? a.role < b.role : QString::localeAwareCompare(a.name, b.name) < 0;
        });
        emit projectMembersLoaded(projectId, roster);
    });
}

void CloudBackend::requestProjectFiles(const QString &projectId, const QString &folderId, int offset)
{
    const auto operation = QStringLiteral("project-files:%1:%2").arg(projectId, folderId);
    if (numericId(projectId).isEmpty() || offset < 0 || offset > std::numeric_limits<int>::max() - 50) {
        emit failed(operation, QStringLiteral("Некорректная папка проекта"));
        return;
    }
    if (!folderId.isEmpty() || offset > 0) {
        const auto disk = m_projectDisks.constFind(projectId);
        if (disk == m_projectDisks.cend() || (!folderId.isEmpty() && !disk->folders.contains(folderId))) {
            emit failed(operation, QStringLiteral("Папка недоступна в этом проекте"));
            return;
        }
        loadProjectDiskItems(projectId, folderId, offset);
        return;
    }
    const auto url = OriginGuard(m_session.origin).resolve(QStringLiteral("/rest/disk.storage.getList.json"));
    webRequest(url, {{QStringLiteral("filter[ENTITY_TYPE]"), QStringLiteral("group")},
                     {QStringLiteral("filter[ENTITY_ID]"), projectId}}, operation,
               QStringLiteral("result"), [this, projectId, operation](QJsonValue data) {
        if (!data.isArray()) {
            emit failed(operation, QStringLiteral("Диск проекта недоступен"));
            return;
        }
        for (const auto value : data.toArray()) {
            const auto entry = value.toObject();
            if (field(entry, {"ENTITY_TYPE"}) != QLatin1String("group") || field(entry, {"ENTITY_ID"}) != projectId)
                continue;
            const auto id = numericId(field(entry, {"ID"}));
            const auto root = numericId(field(entry, {"ROOT_OBJECT_ID"}));
            if (id.isEmpty() || root.isEmpty())
                continue;
            if (m_projectDisks.size() >= 8 && !m_projectDisks.contains(projectId))
                m_projectDisks.erase(m_projectDisks.begin());
            m_projectDisks.insert(projectId, {id, root, {}});
            loadProjectDiskItems(projectId, {}, 0);
            return;
        }
        emit failed(operation, QStringLiteral("Диск проекта недоступен"));
    });
}

void CloudBackend::loadProjectDiskItems(const QString &projectId, const QString &folderId, int offset)
{
    const auto disk = m_projectDisks.value(projectId);
    const auto operation = QStringLiteral("project-files:%1:%2").arg(projectId, folderId);
    const auto endpoint = folderId.isEmpty() ? QStringLiteral("/rest/disk.storage.getChildren.json")
                                            : QStringLiteral("/rest/disk.folder.getChildren.json");
    const auto url = OriginGuard(m_session.origin).resolve(endpoint);
    webRequest(url, {{QStringLiteral("id"), folderId.isEmpty() ? disk.id : folderId},
                     {QStringLiteral("start"), QString::number(offset)}}, operation, QStringLiteral("result"),
               [this, projectId, folderId, operation, storage = disk.id, root = disk.root, offset](QJsonValue data) {
        auto current = m_projectDisks.find(projectId);
        if (current == m_projectDisks.end() || current->id != storage || current->root != root) {
            emit failed(operation, QStringLiteral("Папка больше недоступна"));
            return;
        }
        if (!data.isArray()) {
            emit failed(operation, QStringLiteral("Не удалось получить файлы проекта"));
            return;
        }
        const auto expectedParent = folderId.isEmpty() ? root : folderId;
        QVector<ProjectDiskItem> items;
        const auto rows = data.toArray();
        QSet<QString> seen;
        for (const auto value : rows) {
            const auto entry = value.toObject();
            if (field(entry, {"STORAGE_ID"}) != storage || field(entry, {"PARENT_ID"}) != expectedParent)
                continue;
            const auto id = numericId(field(entry, {"ID"}));
            const auto name = field(entry, {"NAME"}).left(255);
            const auto type = field(entry, {"TYPE"});
            if (id.isEmpty() || name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")
                || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) || seen.contains(id)
                || (type != QLatin1String("folder") && type != QLatin1String("file")))
                continue;
            seen.insert(id);
            ProjectDiskItem item{{id, name, -1, {}}, type == QLatin1String("folder")};
            if (item.folder) {
                if (current->folders.size() >= 500)
                    current->folders.erase(current->folders.begin());
                current->folders.insert(id);
            } else {
                bool ok = false;
                const auto size = field(entry, {"SIZE"}).toLongLong(&ok);
                item.file.size = ok && size >= 0 ? size : -1;
                item.file.downloadUrl = registerMessageFileUrl(field(entry, {"DOWNLOAD_URL"}), item.file.size, false);
            }
            items.append(std::move(item));
            if (items.size() == 50)
                break;
        }
        const auto next = rows.size() >= 50 ? offset + 50 : -1;
        emit projectFilesLoaded(projectId, folderId, items, next);
    });
}

void CloudBackend::setChatPinned(const QString &dialogId, bool pinned)
{
    const auto operation = QStringLiteral("chat-pin:") + dialogId;
    const auto dialog = validDialog(dialogId);
    if (dialog.isEmpty()) {
        emit failed(operation, QStringLiteral("Некорректный номер диалога"));
        return;
    }
    const auto endpoint = pinned ? QStringLiteral("im.v2.Chat.pin") : QStringLiteral("im.v2.Chat.unpin");
    ajax(endpoint, {{QStringLiteral("dialogId"), dialog}}, operation, [this, dialogId, pinned, operation](QJsonValue result) {
        if (result == false)
            return failShape(operation);
        emit chatPinChanged(dialogId, pinned);
    });
}

void CloudBackend::setProjectPinned(const QString &projectId, bool pinned)
{
    Q_UNUSED(pinned); // The official project controller changes the current pin and returns its new state.
    const auto operation = QStringLiteral("project-pin:") + projectId;
    const auto id = numericId(projectId);
    if (id.isEmpty()) {
        emit failed(operation, QStringLiteral("Некорректный номер проекта"));
        return;
    }
    withCurrentUser(operation, [this, id, projectId, operation](qint64) {
        ajaxJson(QStringLiteral("socialnetwork.v2.Project.changePin"),
                 {{QStringLiteral("projectId"), id.toLongLong()},
                  {QStringLiteral("mode"), m_taskProjectScope ? QStringLiteral("tasks_project") : QString()}},
                 operation, [this, id, projectId, operation, key = projectPinSettingsKey(), taskScope = m_taskProjectScope](QJsonValue result) {
            const auto actual = notificationFlag(result.toObject().value(QStringLiteral("pinned")));
            if (!actual)
                return failShape(operation);
            if (!key.isEmpty()) {
                QSettings settings(QStringLiteral("bitrix24-client"), QStringLiteral("bitrix24-client"));
                auto ids = settings.value(key).toStringList();
                ids.removeAll(id);
                if (*actual)
                    ids.prepend(id);
                if (ids.size() > tunables::kPageSizeBoardTasks)
                    ids = ids.first(tunables::kPageSizeBoardTasks);
                settings.setValue(key, ids);
            }
            if (m_taskProjectScope == taskScope) {
                const auto project = std::find_if(m_loadedProjects.begin(), m_loadedProjects.end(),
                                                  [id](const Project &row) { return row.id == id; });
                if (project != m_loadedProjects.end())
                    project->pinned = *actual;
            }
            emit projectPinChanged(projectId, *actual);
        });
    });
}

void CloudBackend::createProject(const QString &name)
{
    const auto op = QStringLiteral("create-project");
    const auto title = name.trimmed();
    if (title.isEmpty()) {
        emit failed(op, QStringLiteral("Введите название проекта"));
        return;
    }
    withCurrentUser(op, [this, title, op](qint64 userId) {
        ajaxJson(QStringLiteral("socialnetwork.V2.Project.add"),
                 {{QStringLiteral("project"), newProjectDto(title, userId)}},
                 op, [this, op](QJsonValue result) {
            const auto id = numericId(field(result.toObject(), {"id"}));
            if (id.isEmpty())
                return failShape(op);
            emit projectCreated(id);
        });
    });
}

void CloudBackend::searchProjectTasks(const QString &projectId, const QString &query)
{
    const auto operation = QStringLiteral("search-project-tasks:") + projectId;
    if (numericId(projectId).isEmpty()) {
        emit failed(operation, QStringLiteral("Некорректный номер проекта"));
        return;
    }
    const auto needle = query.trimmed();
    if (needle.isEmpty()) {
        emit projectTasksFound(projectId, query, {});
        return;
    }
    Fields fields{{QStringLiteral("filter[GROUP_ID]"), projectId},
                  {QStringLiteral("order[ID]"), QStringLiteral("DESC")},
                  {QStringLiteral("start"), QStringLiteral("0")}};
    const bool numberPrefix = needle.startsWith(QLatin1Char('#')) || needle.startsWith(QChar(u'№'));
    const auto matchId = numericId(numberPrefix ? needle.mid(1).trimmed() : needle);
    if (matchId.isEmpty()) {
        QString pattern = needle;
        pattern.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        pattern.replace(QLatin1Char('%'), QStringLiteral("\\%"));
        pattern.replace(QLatin1Char('_'), QStringLiteral("\\_"));
        fields.append({QStringLiteral("filter[TITLE]"), QStringLiteral("%") + pattern + QLatin1Char('%')});
    } else {
        fields.append({QStringLiteral("filter[ID]"), matchId});
    }
    for (const auto *name : {"ID", "TITLE", "GROUP_ID", "STAGE_ID", "DEADLINE", "PRIORITY", "RESPONSIBLE_ID"})
        fields.append({QStringLiteral("select[]"), QLatin1String(name)});
    ajax(QStringLiteral("tasks.task.list"), fields, operation,
         [this, projectId, query, matchId, operation](QJsonValue data) {
        const auto rows = data.toObject().value(QStringLiteral("tasks"));
        if (!rows.isArray())
            return failShape(operation);
        QVector<Task> matches;
        for (const auto &row : rows.toArray()) {
            const auto item = row.toObject();
            auto groupId = field(item, {"groupId", "GROUP_ID"});
            if (groupId.isEmpty())
                groupId = field(item.value(QStringLiteral("group")).toObject(), {"id", "ID"});
            if (groupId != projectId)
                continue;
            auto task = taskFromRow(item);
            if (numericId(task.id).isEmpty() || (!matchId.isEmpty() && task.id != matchId))
                continue;
            if (task.stageName.isEmpty())
                task.stageName = stageLabel(task.stageId);
            matches.append(std::move(task));
            if (matches.size() == tunables::kPageSizeBoardTasks)
                break;
        }
        emit projectTasksFound(projectId, query, matches);
    }, operation + QLatin1Char(':') + query);
}

Task CloudBackend::boardTask(const QJsonObject &item, const QString &id, const QString &projectId,
                            const QString &stageId, const QString &stageName) const
{
    const auto data = item.value(QStringLiteral("data")).toObject();
    auto task = taskFromRow(data);
    task.id = id;
    task.projectId = projectId;
    task.stageId = stageId;
    task.stageName = stageName;
    task.name = field(data, {"name", "title", "TITLE"});
    const auto fillPerson = [this](const QJsonObject &person, QString &personId, QString &name, QUrl &avatar) {
        const auto id = numericId(field(person, {"id"}));
        if (id.isEmpty())
            return;
        personId = id;
        name = field(person, {"name"});
        avatar = avatarUrlFromPortal(field(person.value(QStringLiteral("photo")).toObject(), {"src"}));
    };
    fillPerson(data.value(QStringLiteral("author")).toObject(), task.creatorId, task.createdBy, task.creatorAvatarUrl);
    fillPerson(data.value(QStringLiteral("responsible")).toObject(), task.assigneeId, task.assignee, task.assigneeAvatarUrl);
    return task;
}

void CloudBackend::requestProjectBoard(const QString &projectId)
{
    const auto op = QStringLiteral("board:") + projectId;
    if (numericId(projectId).isEmpty()) {
        emit failed(op, QStringLiteral("Некорректный номер проекта"));
        return;
    }
    if (m_openBoard == projectId && m_inflightOperations.contains(boardReadKey(op, m_boardPageGeneration)))
        return;
    if (m_openBoard != projectId) {
        m_openBoard = projectId;
        m_boardColumnCursors.clear();
        m_boardDirty = false;
    }
    const auto component = QStringLiteral("bitrix:tasks.kanban");
    const auto generation = ++m_boardPageGeneration;
    componentAction(QStringLiteral("applyFilter"), boardFields(projectId), op, [this, projectId, op, generation](QJsonValue data) {
        if (m_openBoard != projectId || m_boardPageGeneration != generation)
            return;
        const auto board = data.toObject();
        const auto columns = board.value(QStringLiteral("columns"));
        const auto items = board.value(QStringLiteral("items"));
        if (!columns.isArray() || !items.isArray())
            return failShape(op);
        QVector<Stage> stages;
        QHash<QString, qsizetype> byId;
        QHash<QString, BoardColumnCursor> cursors;
        for (const auto value : columns.toArray()) {
            const auto column = value.toObject();
            const auto id = field(column, {"id", "ID"});
            if (id.isEmpty() || byId.contains(id))
                continue;
            auto label = field(column, {"name", "TITLE"});
            if (label.isEmpty())
                label = stageLabel(id);
            byId.insert(id, stages.size());
            stages.append({id, label, {}, stageTaskCount(column), false, stageColorFrom(column)});
            auto &cursor = cursors[id];
            cursor.stageName = label;
            cursor.stageColor = stages.last().color;
            cursor.total = stages.last().taskCount;
            if (stages.size() == tunables::kPageSizeBoardTasks)
                break;
        }
        int tasks = 0;
        for (const auto value : items.toArray()) {
            const auto item = value.toObject();
            const auto stageId = field(item, {"columnId"});
            if (!byId.contains(stageId))
                continue;
            auto &cursor = cursors[stageId];
            const auto id = boardItemId(item, projectId, stageId);
            if (id.isEmpty() || cursor.seen.contains(id))
                continue;
            cursor.seen.insert(id);
            if (tasks < tunables::kPageSizeBoardTasks) {
                stages[byId.value(stageId)].tasks.append(boardTask(item, id, projectId, stageId, cursor.stageName));
                ++tasks;
            } else {
                // Do not skip unpainted page-one cards in later columns.
                cursor.deferred.append(item);
            }
        }
        for (auto &stage : stages) {
            auto &cursor = cursors[stage.id];
            if (cursor.total && *cursor.total < cursor.seen.size()) {
                cursor.total.reset();
                stage.taskCount.reset();
            }
            stage.hasMoreTasks = cursor.hasMore();
        }
        m_boardColumnCursors = std::move(cursors);
        emit boardLoaded(projectId, stages);
    }, component, boardReadKey(op, generation));
}

void CloudBackend::requestMoreBoardTasks(const QString &projectId, const QString &stageId)
{
    const auto operation = QStringLiteral("board-column:") + projectId + QLatin1Char(':') + stageId;
    auto cursor = m_boardColumnCursors.find(stageId);
    if (projectId != m_openBoard || cursor == m_boardColumnCursors.end() || !boardStageId(stageId)) {
        emit failed(operation, QStringLiteral("Эта колонка сейчас недоступна"));
        return;
    }
    const auto generation = m_boardPageGeneration;
    if (m_inflightOperations.contains(boardReadKey(QStringLiteral("board:") + projectId, generation))) {
        emit failed(operation, QStringLiteral("Дождитесь обновления доски"));
        return;
    }
    const auto key = boardReadKey(operation, generation);
    if (m_inflightOperations.contains(key))
        return;
    if (!cursor->deferred.isEmpty()) {
        publishDeferredBoardTasks(projectId, stageId, *cursor);
        return;
    }
    if (!cursor->hasMore()) {
        emit boardTasksPageLoaded(projectId, stageId, {}, false);
        return;
    }

    auto fields = boardFields(projectId);
    fields.append({QStringLiteral("pageId"), QString::number(cursor->nextPage)});
    fields.append({QStringLiteral("columnId"), stageId});
    componentAction(QStringLiteral("getColumnItems"), fields, operation,
        [this, projectId, stageId, operation, generation](QJsonValue data) {
        auto current = m_boardColumnCursors.find(stageId);
        if (generation != m_boardPageGeneration || projectId != m_openBoard || current == m_boardColumnCursors.end())
            return;
        const auto rows = data.isArray() ? data : data.toObject().value(QStringLiteral("items"));
        if (!rows.isArray())
            return failShape(operation);
        const auto items = rows.toArray();
        if (items.isEmpty()) {
            current->serverExhausted = true;
            emit boardTasksPageLoaded(projectId, stageId, {}, false);
            return;
        }
        bool scoped = false;
        for (const auto value : items) {
            const auto item = value.toObject();
            const auto id = boardItemId(item, projectId, stageId);
            if (id.isEmpty())
                continue;
            scoped = true;
            if (current->seen.contains(id))
                continue;
            current->seen.insert(id);
            current->deferred.append(item);
        }
        if (!scoped)
            return failShape(operation);
        if (current->nextPage < std::numeric_limits<int>::max())
            ++current->nextPage;
        else
            current->serverExhausted = true;
        publishDeferredBoardTasks(projectId, stageId, *current);
    }, QStringLiteral("bitrix:tasks.kanban"), key);
}

void CloudBackend::publishDeferredBoardTasks(const QString &projectId, const QString &stageId, BoardColumnCursor &cursor)
{
    QVector<Task> tasks;
    const auto end = std::min(cursor.deferredOffset + tunables::kPageSizeBoardTasks, cursor.deferred.size());
    tasks.reserve(end - cursor.deferredOffset);
    for (; cursor.deferredOffset < end; ++cursor.deferredOffset) {
        const auto item = cursor.deferred.at(cursor.deferredOffset).toObject();
        tasks.append(boardTask(item, numericId(field(item, {"id"})), projectId, stageId, cursor.stageName));
    }
    if (cursor.deferredOffset == cursor.deferred.size()) {
        cursor.deferred = {};
        cursor.deferredOffset = 0;
    }
    emit boardTasksPageLoaded(projectId, stageId, tasks, cursor.hasMore());
}

void CloudBackend::requestProjectStages(const QString &projectId)
{
    const auto operation = QStringLiteral("project-stages:") + projectId;
    if (numericId(projectId).isEmpty()) {
        emit failed(operation, QStringLiteral("Некорректный номер проекта"));
        return;
    }
    loadProjectStages(projectId, operation, [this, operation, projectId](std::optional<QVector<Stage>> stages) {
        if (!stages)
            return failShape(operation);
        emit projectStagesLoaded(projectId, *stages);
    });
}

void CloudBackend::loadProjectStages(const QString &projectId, const QString &operation, ProjectStagesHandler complete)
{
    const QJsonObject group{{QStringLiteral("id"), projectId.toLongLong()}};
    ajaxJson(QStringLiteral("tasks.v2.Group.Stage.list"), {{QStringLiteral("group"), group}},
             operation, [projectId, complete = std::move(complete)](QJsonValue data) {
        if (!data.isArray())
            return complete(std::nullopt);
        QSet<QString> seen;
        QVector<QPair<qint64, Stage>> ordered;
        for (const auto value : data.toArray()) {
            const auto row = value.toObject();
            const auto id = numericId(field(row, {"id"}));
            const auto title = field(row, {"title", "name"});
            const auto groupId = field(row, {"groupId", "GROUP_ID"});
            if (id.isEmpty() || title.isEmpty() || seen.contains(id)
                || (!groupId.isEmpty() && numericId(groupId) != projectId))
                continue;
            seen.insert(id);
            ordered.append({row.value(QStringLiteral("sort")).toInteger(),
                            {id, title, {}, std::nullopt, false, stageColorFrom(row)}});
            if (ordered.size() >= tunables::kPageSizeBoardTasks)
                break;
        }
        std::stable_sort(ordered.begin(), ordered.end(), [](const auto &left, const auto &right) {
            return left.first < right.first;
        });
        QVector<Stage> stages;
        stages.reserve(ordered.size());
        for (auto &entry : ordered)
            stages.append(std::move(entry.second));
        complete(std::move(stages));
    });
}

void CloudBackend::loadLinkStage(const QString &projectId, const QString &stageId, std::function<void(Stage)> complete)
{
    if (numericId(stageId).isEmpty() || m_authExpired) {
        complete({});
        return;
    }
    if (projectId == m_openBoard) {
        const auto stage = m_boardColumnCursors.constFind(stageId);
        if (stage != m_boardColumnCursors.cend() && !stage->stageColor.isEmpty()) {
            complete({stageId, stage->stageName, {}, std::nullopt, false, stage->stageColor});
            return;
        }
    }
    const auto scope = settingsScope();
    const auto key = QString::number(m_sessionRevision) + QLatin1Char(':') + scope + QLatin1Char(':') + projectId;
    auto deliver = [stageId, complete = std::move(complete)](const QVector<Stage> &stages) {
        const auto stage = std::find_if(stages.cbegin(), stages.cend(), [&stageId](const Stage &entry) {
            return entry.id == stageId;
        });
        complete(stage == stages.cend() ? Stage{} : *stage);
    };
    if (const auto *snapshot = m_linkStageSnapshots.object(key);
        snapshot && snapshot->expiresAt > QDateTime::currentMSecsSinceEpoch()) {
        deliver(snapshot->stages);
        return;
    }
    auto &pending = m_pendingLinkStages[key];
    if (pending.size() >= tunables::kPageSizeProjects) {
        deliver({});
        return;
    }
    pending.append(std::move(deliver));
    if (pending.size() > 1)
        return;
    loadProjectStages(projectId, QStringLiteral("link-preview:stage:") + key,
        [this, key, scope](std::optional<QVector<Stage>> stages) {
            const auto pending = m_pendingLinkStages.take(key);
            if (m_authExpired || settingsScope() != scope)
                stages.reset();
            const auto entries = stages.value_or(QVector<Stage>{});
            constexpr int stageSnapshotTtlMs = 60 * 1000;
            if (stages)
                m_linkStageSnapshots.insert(key, new LinkStageSnapshot{
                    entries, QDateTime::currentMSecsSinceEpoch() + stageSnapshotTtlMs});
            for (const auto &complete : pending)
                complete(entries);
        });
}

CloudBackend::Fields CloudBackend::boardFields(const QString &projectId) const
{
    Fields fields{{QStringLiteral("params[GROUP_ID]"), projectId},
                  {QStringLiteral("params[GROUP_ID_FORCED]"), projectId},
                  {QStringLiteral("params[PERSONAL]"), QStringLiteral("N")},
                  {QStringLiteral("personal"), QStringLiteral("N")}};
    if (!m_userId.isEmpty())
        fields.append({QStringLiteral("params[USER_ID]"), m_userId});
    return fields;
}

void CloudBackend::boardMutation(const QString &action, const QString &projectId, Fields fields,
                                 Handler onData)
{
    fields.append(boardFields(projectId));
    if (!onData)
        onData = [this, projectId](QJsonValue) { emit boardUpdated(projectId); };
    componentAction(action, fields, QStringLiteral("board-edit:") + projectId, std::move(onData));
}

void CloudBackend::moveTask(const QString &projectId, const QString &taskId, const QString &stageId)
{
    const auto operation = QStringLiteral("board-edit:") + projectId;
    if (numericId(projectId).isEmpty() || numericId(taskId).isEmpty() || numericId(stageId).isEmpty()) {
        emit failed(operation, QStringLiteral("Проверьте проект, задачу и стадию"));
        return;
    }
    boardMutation(QStringLiteral("moveTask"), projectId,
                  {{QStringLiteral("itemId"), taskId}, {QStringLiteral("columnId"), stageId},
                   {QStringLiteral("beforeItemId"), QStringLiteral("0")},
                   {QStringLiteral("afterItemId"), QStringLiteral("0")}},
                  [this, projectId, operation](const QJsonValue &result) {
        const bool moved = result.toBool() || result.isObject() || result.isArray()
            || result.toDouble() != 0 || !result.toString().isEmpty();
        if (!moved) {
            emit failed(operation, QStringLiteral("Портал отклонил перенос задачи"));
            return;
        }
        if (projectId == m_openBoard) {
            // The board refresh requested on confirmation needs a new snapshot and deduplication key.
            ++m_boardPageGeneration;
            m_boardColumnCursors.clear();
        }
        emit boardUpdated(projectId);
    });
}

void CloudBackend::saveStage(const QString &projectId, const QString &stageId, const QString &title,
                             const QString &afterStageId, const QString &color)
{
    const auto operation = QStringLiteral("board-edit:") + projectId;
    const auto cleanTitle = title.trimmed();
    const auto requestedColor = normalizedStageColor(color);
    if (!color.isEmpty() && requestedColor.isEmpty()) {
        emit failed(operation, QStringLiteral("Укажите цвет стадии в формате RRGGBB"));
        return;
    }
    const bool colorOnly = title.isEmpty() && !stageId.isEmpty() && !requestedColor.isEmpty();
    if (numericId(projectId).isEmpty() || (cleanTitle.isEmpty() && !colorOnly)
        || (!stageId.isEmpty() && numericId(stageId).isEmpty())
        || (!afterStageId.isEmpty() && (!boardStageId(afterStageId) || afterStageId == stageId))) {
        emit failed(operation, QStringLiteral("Проверьте название и положение стадии"));
        return;
    }
    componentAction(QStringLiteral("applyFilter"), boardFields(projectId), operation,
                    [this, projectId, stageId, cleanTitle, afterStageId, requestedColor, colorOnly, operation](QJsonValue data) {
        const auto columns = data.toObject().value(QStringLiteral("columns"));
        if (!columns.isArray())
            return failShape(operation);
        QString previous = QStringLiteral("0");
        QString currentPrevious;
        QJsonObject current;
        bool predecessorExists = afterStageId.isEmpty() || afterStageId == QLatin1String("0");
        for (const auto &value : columns.toArray()) {
            const auto column = value.toObject();
            const auto id = field(column, {"id", "ID"});
            if (!boardStageId(id))
                continue;
            if (id == stageId) {
                current = column;
                currentPrevious = previous;
            }
            predecessorExists = predecessorExists || id == afterStageId;
            previous = id;
        }
        if ((!stageId.isEmpty() && current.isEmpty()) || !predecessorExists) {
            emit failed(operation, QStringLiteral("Стадия больше не доступна на этой доске"));
            return;
        }
        const auto currentTitle = field(current, {"name", "TITLE"});
        const auto nextTitle = colorOnly ? currentTitle : cleanTitle;
        if (nextTitle.isEmpty())
            return failShape(operation);
        const auto currentColor = stageColorFrom(current);
        const auto nextColor = requestedColor.isEmpty() ? currentColor : requestedColor;
        Fields fields{{QStringLiteral("fields[columnName]"), nextTitle},
                      {QStringLiteral("fields[columnColor]"), nextColor.isEmpty() ? QStringLiteral("ace9fb") : nextColor}};
        if (stageId.isEmpty()) {
            fields.append({QStringLiteral("fields[afterColumnId]"), afterStageId.isEmpty() ? previous : afterStageId});
            boardMutation(QStringLiteral("modifyColumn"), projectId, fields);
            return;
        }
        fields.append({QStringLiteral("fields[id]"), stageId});
        auto finish = [this, projectId, stageId, afterStageId, currentPrevious](QJsonValue) {
            if (!afterStageId.isEmpty() && afterStageId != currentPrevious) {
                boardMutation(QStringLiteral("moveColumn"), projectId,
                              {{QStringLiteral("columnId"), stageId},
                               {QStringLiteral("afterColumnId"), afterStageId}});
            } else {
                emit boardUpdated(projectId);
            }
        };
        if (nextTitle == currentTitle && nextColor == currentColor)
            finish({});
        else
            boardMutation(QStringLiteral("modifyColumn"), projectId, fields, std::move(finish));
    });
}

void CloudBackend::deleteStage(const QString &projectId, const QString &stageId)
{
    const auto operation = QStringLiteral("board-edit:") + projectId;
    if (numericId(projectId).isEmpty() || numericId(stageId).isEmpty()) {
        emit failed(operation, QStringLiteral("Проверьте проект и стадию"));
        return;
    }
    componentAction(QStringLiteral("applyFilter"), boardFields(projectId), operation,
                    [this, projectId, stageId, operation](QJsonValue data) {
        const auto board = data.toObject();
        const auto columns = board.value(QStringLiteral("columns"));
        const auto items = board.value(QStringLiteral("items"));
        if (!columns.isArray() || !items.isArray())
            return failShape(operation);
        const auto stageRows = columns.toArray();
        const auto stage = std::find_if(stageRows.cbegin(), stageRows.cend(), [&stageId](const QJsonValue &value) {
            return field(value.toObject(), {"id", "ID"}) == stageId;
        });
        const auto taskRows = items.toArray();
        const bool listedTask = std::any_of(taskRows.cbegin(), taskRows.cend(), [&stageId](const QJsonValue &value) {
            return field(value.toObject(), {"columnId"}) == stageId;
        });
        if (stage == stageRows.cend() || stageTaskCount((*stage).toObject()) != 0 || listedTask
            || !(*stage).toObject().value(QStringLiteral("canRemove")).toBool(true)) {
            emit failed(operation, QStringLiteral("Портал не подтвердил, что стадию можно удалить без переноса задач"));
            return;
        }

        // The portal can retain a personal board filter. Check the stage again by project and stage alone.
        ajax(QStringLiteral("tasks.task.list"),
             {{QStringLiteral("filter[GROUP_ID]"), projectId}, {QStringLiteral("filter[STAGE_ID]"), stageId},
              {QStringLiteral("select[]"), QStringLiteral("ID")}, {QStringLiteral("start"), QStringLiteral("0")}},
             operation, [this, projectId, stageId, operation](QJsonValue result) {
            const auto tasks = result.toObject().value(QStringLiteral("tasks"));
            if (!tasks.isArray())
                return failShape(operation);
            if (!tasks.toArray().isEmpty()) {
                emit failed(operation, QStringLiteral("На этой стадии ещё есть задачи"));
                return;
            }
            boardMutation(QStringLiteral("modifyColumn"), projectId,
                          {{QStringLiteral("fields[id]"), stageId}, {QStringLiteral("fields[delete]"), QStringLiteral("1")}});
        });
    });
}

void CloudBackend::createTask(const QString &projectId, const TaskEdit &edit)
{
    createTaskRequest(projectId, {}, edit);
}

void CloudBackend::createSubtask(const QString &parentId, const QString &projectId, const TaskEdit &edit)
{
    if (numericId(parentId).isEmpty()) {
        emit failed(QStringLiteral("create-subtask:") + parentId, QStringLiteral("Номер родительской задачи недействителен"));
        return;
    }
    createTaskRequest(projectId, parentId, edit);
}

void CloudBackend::createTaskRequest(const QString &projectId, const QString &parentId, const TaskEdit &edit)
{
    const bool child = !parentId.isEmpty();
    const auto operation = child ? QStringLiteral("create-subtask:") + parentId : QStringLiteral("create-task:") + projectId;
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
    const auto account = m_accountScopeRevision;
    withCurrentUser(operation, [this, projectId, parentId, task = std::move(task), deadline, operation, account,
                                creatorId, assigneeId](qint64 userId) mutable {
        if (account != m_accountScopeRevision)
            return;
        const QJsonObject responsible{{QStringLiteral("id"), assigneeId > 0 ? assigneeId : userId}};
        task.insert(QStringLiteral("creator"), QJsonObject{{QStringLiteral("id"), creatorId > 0 ? creatorId : userId}});
        task.insert(QStringLiteral("responsible"), responsible);
        task.insert(QStringLiteral("responsibleCollection"), QJsonArray{responsible});
        task.insert(QStringLiteral("isMultitask"), false);
        ajaxJson(QStringLiteral("tasks.v2.Task.add"),
                 {{QStringLiteral("task"), task}, {QStringLiteral("view"), false},
                  {QStringLiteral("deadlineManuallySet"), *deadline != 0}},
                 operation, [this, projectId, parentId, operation](QJsonValue result) {
            const auto taskId = numericId(field(result.toObject(), {"id"}));
            if (taskId.isEmpty())
                return failShape(operation);
            if (parentId.isEmpty())
                emit taskCreated(projectId, taskId);
            else
                emit subtaskCreated(parentId, projectId, taskId);
        });
    });
}

void CloudBackend::updateTask(const QString &taskId, const TaskEdit &edit)
{
    const auto operation = QStringLiteral("update-task:") + taskId;
    const auto deadline = taskDeadline(edit.deadline.value_or(QString()));
    if (numericId(taskId).isEmpty() || (edit.title && edit.title->trimmed().isEmpty()) || !deadline
        || (edit.deadlineReason && edit.deadlineReason->size() > 2000)) {
        emit failed(operation, QStringLiteral("Проверьте название и срок задачи"));
        return;
    }
    if (m_inflightOperations.contains(operation) || m_inflightOperations.contains(taskActionRequestKey(m_accountScopeRevision, taskId)))
        return;
    if (edit.rating && (!m_started || numericId(taskId) != taskId || legacyTaskMark(*edit.rating).isUndefined()))
        return failShape(operation);
    const int requisites = static_cast<int>(edit.creatorId.has_value()) + edit.assigneeId.has_value()
        + edit.observerIds.has_value() + edit.projectId.has_value() + edit.stageId.has_value()
        + edit.statusAction.has_value() + edit.rating.has_value();
    if (edit.requireDelegate && !edit.assigneeId)
        return failShape(operation);
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
    ajaxJson(QStringLiteral("tasks.v2.Task.get"), {{QStringLiteral("task"), id}}, operation,
             [this, taskId, id, edit, writes = std::move(writes), operation]
             (QJsonValue result) mutable {
        const auto current = result.toObject();
        if (field(current, {"id"}) != taskId)
            return failShape(operation);
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

void CloudBackend::deleteTask(const QString &taskId)
{
    const auto operation = QStringLiteral("delete-task:") + taskId;
    if (!m_started || taskId.isEmpty() || numericId(taskId) != taskId)
        return failShape(operation);
    const auto account = m_accountScopeRevision;
    const auto key = taskActionRequestKey(account, taskId);
    if (m_inflightOperations.contains(key) || m_inflightOperations.contains(QStringLiteral("update-task:") + taskId))
        return;
    const QJsonObject id{{QStringLiteral("id"), taskId.toLongLong()}};
    ajaxJson(QStringLiteral("tasks.v2.Task.get"), {{QStringLiteral("task"), id}}, operation,
             [this, taskId, operation, account, key](QJsonValue response) {
        if (account != m_accountScopeRevision)
            return;
        const auto current = response.toObject();
        if (!matchesTaskIdentity(current, taskId))
            return failShape(operation);
        if (current.value(QStringLiteral("rights")).toObject().value(QStringLiteral("remove")) != true) {
            emit failed(operation, QStringLiteral("Портал не разрешает удалить эту задачу"));
            return;
        }
        const auto url = OriginGuard(m_session.origin).resolve(QStringLiteral("/rest/tasks.task.delete.json"));
        webRequest(url, {{QStringLiteral("taskId"), taskId}}, operation, QStringLiteral("result"),
                   [this, taskId, operation, account](QJsonValue result) {
            if (account != m_accountScopeRevision)
                return;
            if (result.toObject().value(QStringLiteral("task")) != true)
                return failShape(operation);
            if (m_openTask == taskId)
                clearOpenHistory(true);
            invalidateTaskChat(taskId);
            m_taskAttachmentRevisions.remove(taskId);
            m_taskDialogs.remove(taskId);
            m_taskChatTitles.remove(taskId);
            emit taskLinkPreviewInvalidated(taskId);
            emit taskDeleted(taskId);
        }, key);
    }, key);
}

void CloudBackend::updateTaskRequisite(const QString &taskId, const TaskEdit &edit)
{
    const auto operation = QStringLiteral("update-task:") + taskId;
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
    const auto account = m_accountScopeRevision;
    const auto requestKey = (edit.rating || edit.statusAction || edit.assigneeId || edit.projectId || edit.stageId)
        ? taskActionRequestKey(account, taskId) : QString();
    ajaxJson(QStringLiteral("tasks.v2.Task.get"), {{QStringLiteral("task"), id}}, operation,
             [this, taskId, operation, id, edit, account, requestKey](QJsonValue data) {
        const auto current = data.toObject();
        if (!requestKey.isEmpty() && account != m_accountScopeRevision)
            return;
        if (!matchesTaskIdentity(current, taskId))
            return failShape(operation);
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
            const auto url = OriginGuard(m_session.origin).resolve(QStringLiteral("/rest/tasks.task.update.json"));
            const QJsonObject body{{QStringLiteral("taskId"), taskId.toLongLong()},
                {QStringLiteral("fields"), QJsonObject{{QStringLiteral("MARK"), legacyTaskMark(rating)}}},
                {QStringLiteral("sessid"), QString::fromLatin1(m_session.csrf)}};
            auto *buffer = new QBuffer;
            buffer->setData(QJsonDocument(body).toJson(QJsonDocument::Compact));
            buffer->open(QIODevice::ReadOnly);
            webRequestRaw(url, buffer, {{"Content-Type", "application/json"}}, operation, QStringLiteral("result"),
                          [this, taskId, operation, rating, account](QJsonValue result) {
                if (account != m_accountScopeRevision)
                    return;
                const auto task = result.toObject().value(QStringLiteral("task")).toObject();
                if (!matchesTaskIdentity(task, taskId) || taskRating(task) != rating)
                    return failShape(operation);
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
            componentAction(QStringLiteral("moveStage"),
                {{QStringLiteral("taskId"), taskId}, {QStringLiteral("stageId"), *edit.stageId}}, operation,
                [this, taskId, operation, account](QJsonValue result) {
                    if (account != m_accountScopeRevision)
                        return;
                    if (result != true)
                        return failShape(operation);
                    emit taskUpdated(taskId);
                }, QStringLiteral("bitrix:tasks.task"), requestKey);
            return;
        }
        const auto postAction = [this, taskId, operation, requestKey](const QString &action, const QJsonObject &body) {
            ajaxJson(action, body, operation, [this, taskId, operation](QJsonValue result) {
                if (result == false)
                    return failShape(operation);
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
                return failShape(operation);
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
            withCurrentUser(operation, [this, taskId, operation, id, changed, adding, canWatch,
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

void CloudBackend::updateTaskFields(const QString &taskId, QQueue<QPair<QString, QJsonObject>> writes,
                                    const QString &requestKey)
{
    const auto scope = m_accountScopeRevision;
    if (!requestKey.isEmpty() && requestKey != taskActionRequestKey(scope, taskId))
        return;
    if (writes.isEmpty()) {
        emit taskUpdated(taskId);
        return;
    }
    const auto [action, task] = writes.dequeue();
    const auto operation = QStringLiteral("update-task:") + taskId;
    ajaxJson(action, {{QStringLiteral("task"), task}}, operation,
             [this, taskId, operation, title = task.value(QStringLiteral("title")).toString(),
              changedProject = task.contains(QStringLiteral("group")),
              project = task.value(QStringLiteral("group")).toObject().value(QStringLiteral("id")).toInteger(-1),
              scope, requestKey,
              writes = std::move(writes)](QJsonValue result) mutable {
        if (!requestKey.isEmpty() && scope != m_accountScopeRevision)
            return;
        if (result == false)
            return failShape(operation);
        rememberTaskTitle(taskId, title);
        if (changedProject) {
            if (scope == m_accountScopeRevision)
                updateProjectUnreadTask(taskId.toLongLong(), groupDialogChatId(m_taskDialogs.value(taskId)), project);
            invalidateTaskChat(taskId);
        }
        updateTaskFields(taskId, std::move(writes), requestKey);
    }, requestKey);
}

Project CloudBackend::projectFromApiRow(const QJsonObject &row) const
{
    Project project{numericId(field(row, {"ID", "id"})), field(row, {"NAME", "name"}), {}};
    project.avatarType = field(row, {"AVATAR_TYPE", "avatarType"});
    auto avatar = field(row, {"AVATAR", "avatar"});
    const auto data = row.value(QStringLiteral("AVATAR_DATA")).toObject();
    const auto kind = field(data, {"type"});
    const auto icon = field(data, {"id"});
    if (kind == QLatin1String("icon") && !icon.isEmpty())
        project.avatarType = icon;
    if (kind == QLatin1String("image") && avatar.isEmpty())
        avatar = icon;
    project.avatarUrl = avatarUrlFromPortal(avatar);
    return project;
}

void CloudBackend::loadLinkProject(const QString &id, const QString &operation,
                                  std::function<void(Project, QString)> complete)
{
    const auto scope = settingsScope();
    ajax(QStringLiteral("socialnetwork.api.workgroup.list"),
         {{QStringLiteral("filter[ID]"), id}, {QStringLiteral("params[mode]"), QStringLiteral("mobile")},
          {QStringLiteral("select[0]"), QStringLiteral("ID")}, {QStringLiteral("select[1]"), QStringLiteral("NAME")},
          {QStringLiteral("select[2]"), QStringLiteral("DESCRIPTION")}, {QStringLiteral("select[3]"), QStringLiteral("AVATAR")},
          {QStringLiteral("select[4]"), QStringLiteral("AVATAR_TYPE")}},
         operation, [this, id, scope, complete = std::move(complete)](QJsonValue value) {
        if (m_authExpired || settingsScope() != scope) {
            complete({}, {});
            return;
        }
        const auto rows = list(value, {"workgroups", "items", "groups", "result"}).toArray();
        const auto match = std::find_if(rows.cbegin(), rows.cend(), [&id](const QJsonValue &entry) {
            const auto row = entry.toObject();
            const auto upper = row.value(QStringLiteral("ID"));
            const auto lower = row.value(QStringLiteral("id"));
            return (!upper.isUndefined() || !lower.isUndefined())
                && (upper.isUndefined() || numericId(string(upper)) == id)
                && (lower.isUndefined() || numericId(string(lower)) == id);
        });
        if (match == rows.cend()) {
            complete({}, {});
            return;
        }
        const auto row = (*match).toObject();
        auto project = projectFromApiRow(row);
        const auto known = std::find_if(m_loadedProjects.cbegin(), m_loadedProjects.cend(), [&project](const Project &entry) {
            return entry.id == project.id && entry.name == project.name
                && (entry.avatarType.isEmpty() || entry.avatarType == project.avatarType);
        });
        if (known != m_loadedProjects.cend())
            project.avatarColor = known->avatarColor;
        complete(std::move(project), field(row, {"DESCRIPTION", "description"}));
    });
}

void CloudBackend::requestMessageLinkPreview(const QUrl &url, LinkPreviewHandler complete)
{
    const auto target = portalLinkPreviewTarget(m_session.origin, url);
    if (m_authExpired || !target) {
        complete({});
        return;
    }
    const auto operation = QStringLiteral("link-preview:") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto scope = settingsScope();
    const auto finish = [this, scope, complete = std::move(complete)](MessageLinkPreview preview) {
        if (m_authExpired || settingsScope() != scope)
            preview = {};
        complete(std::move(preview));
    };
    switch (target->kind) {
    case NotificationTargetKind::Task:
        loadTask(target->id, operation, [this, finish, scope, operation](const QJsonObject &row) {
            if (m_authExpired || settingsScope() != scope) {
                finish({});
                return;
            }
            const auto task = taskFromRow(row);
            MessageLinkPreview preview{notificationDisplayText(task.name).left(160), {},
                                       MessageLinkPreviewKind::Task};
            const auto creator = row.value(QStringLiteral("creator")).toObject();
            const auto responsible = row.value(QStringLiteral("responsible")).toObject();
            preview.creatorAvatarUrl = taskRoleAvatar({}, creator, task.creatorId);
            preview.assigneeAvatarUrl = taskRoleAvatar({}, responsible, task.assigneeId);
            if (!preview.title.isEmpty()) {
                const auto append = [&preview](const QString &label, const QString &value) {
                    const auto display = notificationDisplayText(value).left(180);
                    if (!display.isEmpty())
                        preview.details.append(label.arg(display));
                };
                append(QStringLiteral("Статус · %1"), task.status);
                append(QStringLiteral("Постановщик · %1"), task.createdBy);
                append(QStringLiteral("Исполнитель · %1"), task.assignee);
                append(QStringLiteral("Описание · %1"), task.description);
            }
            const auto explicitProject = field(row, {"groupId", "GROUP_ID"});
            const auto projectId = explicitProject.isEmpty() ? task.projectId : numericId(explicitProject);
            const auto group = row.value(QStringLiteral("group")).toObject();
            const auto nestedProject = numericId(field(group, {"id", "ID"}));
            const auto fallbackName = explicitProject.isEmpty()
                || (!projectId.isEmpty() && (nestedProject.isEmpty() || nestedProject == projectId))
                ? task.project : QString{};
            const bool canLoadProject = !preview.title.isEmpty() && !projectId.isEmpty()
                && !m_authExpired && settingsScope() == scope;
            auto applyProject = [this, finish, fallbackName, stageId = task.stageId,
                                 stageName = task.stageName, hasStatus = !task.status.isEmpty(),
                                 preview = std::move(preview)](Project project, QString) mutable {
                if (!preview.title.isEmpty()) {
                    const auto name = notificationDisplayText(project.name.isEmpty() ? fallbackName : project.name).left(180);
                    if (!name.isEmpty())
                        preview.details.append(QStringLiteral("Проект · %1").arg(name));
                    if (!project.name.isEmpty())
                        preview.project = std::move(project);
                }
                const auto group = preview.project ? preview.project->id : QString{};
                auto applyStage = [finish, stageName, hasStatus, preview = std::move(preview)](Stage stage) mutable {
                    const auto name = notificationDisplayText(stage.name.isEmpty() ? stageName : stage.name).left(180);
                    if (!preview.title.isEmpty() && !name.isEmpty()) {
                        preview.details.insert(hasStatus ? 1 : 0, QStringLiteral("Стадия · %1").arg(name));
                        preview.stageColor = stage.color;
                    }
                    finish(std::move(preview));
                };
                if (group.isEmpty() || numericId(stageId).isEmpty())
                    applyStage({});
                else
                    loadLinkStage(group, stageId, std::move(applyStage));
            };
            if (canLoadProject)
                loadLinkProject(projectId, operation, std::move(applyProject));
            else
                applyProject({}, {});
        }, {}, false);
        break;
    case NotificationTargetKind::Project:
        loadLinkProject(target->id, operation, [finish](Project project, const QString &text) {
            MessageLinkPreview preview{notificationDisplayText(project.name).left(160), {}, MessageLinkPreviewKind::Project};
            const auto description = notificationDisplayText(text).left(180);
            if (!description.isEmpty())
                preview.details.append(description);
            if (!preview.title.isEmpty())
                preview.project = std::move(project);
            finish(std::move(preview));
        });
        break;
    case NotificationTargetKind::Person:
        loadPerson(target->id, operation, [finish](const Person &person) {
            finish({person.name, person.position.isEmpty() ? QStringList{} : QStringList{person.position},
                    MessageLinkPreviewKind::Person, person.avatarUrl});
        });
        break;
    case NotificationTargetKind::Chat:
        ajax(QStringLiteral("im.v2.Chat.load"),
             {{QStringLiteral("dialogId"), target->id}, {QStringLiteral("messageLimit"), QStringLiteral("1")}},
             operation, [this, dialogId = target->id, finish](QJsonValue value) {
            const auto chat = value.toObject().value(QStringLiteral("chat")).toObject();
            const auto supplied = field(chat, {"dialogId", "DIALOG_ID"});
            if (chatDialog(field(chat, {"id", "chatId", "ID"})) != dialogId
                || (!supplied.isEmpty() && supplied != dialogId)) {
                finish({});
                return;
            }
            finish({notificationDisplayText(field(chat, {"name", "title", "TITLE"})).left(160), {},
                    MessageLinkPreviewKind::Chat, avatarUrlFromPortal(field(chat, {"avatar", "avatarUrl"}))});
        });
        break;
    }
}

void CloudBackend::loadTask(const QString &taskId, const QString &operation,
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
    ajax(QStringLiteral("tasks.task.get"), fields, operation,
         [this, taskId, operation, onTask = std::move(onTask), remember, scope = m_accountScopeRevision,
          bindingRevision = m_projectUnreadBindingRevision](QJsonValue data) {
        if (remember && scope != m_accountScopeRevision)
            return;
        const auto task = data.toObject().value(QStringLiteral("task")).toObject();
        if (!remember) {
            onTask(matchesTaskIdentity(task, taskId) ? task : QJsonObject{});
            return;
        }
        if (!matchesTaskIdentity(task, taskId))
            return failShape(operation);
        rememberTaskTitle(taskId, field(task, {"title", "TITLE"}));
        const auto chatId = explicitMessageChatId(task);
        const auto dialog = chatDialog(QString::number(chatId));
        if (!dialog.isEmpty()) {
            bool groupValid = false;
            const auto group = field(task, {"groupId", "GROUP_ID"}).toLongLong(&groupValid);
            if (groupValid && group >= 0 && bindingRevision == m_projectUnreadBindingRevision) {
                rememberProjectUnreadTask(chatId, taskId.toLongLong(), group);
                publishProjectUnreadCounts();
            }
            if (m_taskDialogs.size() >= tunables::kPageSizeChats)
                m_taskDialogs.erase(m_taskDialogs.begin());
            m_taskDialogs.insert(taskId, dialog);
        } else {
            m_taskDialogs.remove(taskId);
        }
        onTask(task);
    }, requestKey);
}

QUrl CloudBackend::taskRoleAvatar(const QJsonObject &current, const QJsonObject &legacy, const QString &personId) const
{
    if (personId.isEmpty())
        return {};
    if (numericId(field(current, {"id"})) == personId && current.contains(QStringLiteral("image")))
        return avatarUrlFromPortal(field(current.value(QStringLiteral("image")).toObject(), {"src"}));
    if (numericId(field(legacy, {"id"})) == personId) {
        const auto icon = field(legacy, {"icon"});
        return avatarUrlFromPortal(icon.isEmpty() ? field(legacy.value(QStringLiteral("image")).toObject(), {"src"}) : icon);
    }
    return {};
}

void CloudBackend::requestTask(const QString &taskId)
{
    const auto operation = taskReadOperationPrefix + taskId;
    if (numericId(taskId) != taskId || taskId.isEmpty()) {
        emit failed(operation, QStringLiteral("Некорректный номер задачи"));
        return;
    }
    if (!m_started)
        return;
    if (const auto existing = m_taskCardReads.find(taskId); existing != m_taskCardReads.end()) {
        // An initial duplicate shares both requests; a refresh after the base reply needs a fresh pair.
        existing->refresh |= existing->baseFinished;
        return;
    }
    m_taskCardReads.insert(taskId, TaskCardRead{});
    m_taskAttachmentRevisions.remove(taskId);
    const auto account = m_accountScopeRevision;
    const auto key = taskCardReadPrefix + QString::number(account) + QLatin1Char(':') + taskId;
    loadTask(taskId, operation, [this, taskId, account](const QJsonObject &data) {
        if (account != m_accountScopeRevision)
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
        if (account == m_accountScopeRevision)
            finishTaskCardRead(taskId);
    }, key + QStringLiteral(":base"));

    const auto current = m_taskCardReads.find(taskId);
    if (account != m_accountScopeRevision || current == m_taskCardReads.end())
        return;
    if (current->baseFinished && current->base.isEmpty()) {
        // A synchronous rejection of the mandatory read (for example expired authentication).
        current->detailsFinished = true;
        finishTaskCardRead(taskId);
        return;
    }
    const QJsonObject id{{QStringLiteral("id"), taskId.toLongLong()}};
    ajaxJson(QStringLiteral("tasks.v2.Task.get"), {{QStringLiteral("task"), id}},
             QStringLiteral("task-details:") + taskId, [this, taskId, account](QJsonValue element) {
        if (account != m_accountScopeRevision)
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

void CloudBackend::finishTaskCardRead(const QString &taskId)
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

void CloudBackend::loadTaskChecklist(const QString &taskId, const QString &operation,
                                    std::function<void(QJsonArray, QJsonValue)> handler)
{
    if (numericId(taskId).isEmpty()) {
        emit failed(operation, QStringLiteral("Номер задачи недействителен"));
        return;
    }
    ajaxJson(QStringLiteral("tasks.v2.CheckList.get"), {{QStringLiteral("taskId"), taskId.toLongLong()}},
             operation, [this, operation, handler = std::move(handler)](QJsonValue data) {
        const auto object = data.toObject();
        const auto status = object.value(QStringLiteral("status"));
        const auto rows = data.isArray() ? data : object.value(QStringLiteral("checklist"));
        if (!rows.isArray() || (!status.isUndefined() && status != QLatin1String("success")))
            return failShape(operation);
        handler(rows.toArray(), object.value(QStringLiteral("revision")));
    });
}

void CloudBackend::requestTaskChecklist(const QString &taskId)
{
    loadTaskChecklist(taskId, QStringLiteral("checklist:") + taskId, [this, taskId](QJsonArray rows, QJsonValue) {
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

void CloudBackend::updateTaskChecklist(const QString &taskId, const QString &action, QJsonObject payload)
{
    const auto operation = QStringLiteral("checklist-write:") + taskId;
    ajaxJson(action, payload, operation, [this, taskId, operation](QJsonValue data) {
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

void CloudBackend::addTaskChecklistItem(const QString &taskId, const QString &parentId, const QString &title)
{
    const auto operation = QStringLiteral("checklist-write:") + taskId;
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

void CloudBackend::setTaskChecklistComplete(const QString &taskId, const QString &itemId, bool complete)
{
    const auto operation = QStringLiteral("checklist-write:") + taskId;
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

void CloudBackend::requestSubtasks(const QString &taskId)
{
    const auto operation = QStringLiteral("subtasks:") + taskId;
    if (numericId(taskId).isEmpty()) {
        emit failed(operation, QStringLiteral("Номер задачи недействителен"));
        return;
    }
    ajaxJson(QStringLiteral("tasks.v2.Task.Relation.Child.list"),
        {{QStringLiteral("taskId"), taskId.toLongLong()}, {QStringLiteral("withIds"), false},
         {QStringLiteral("withCompleted"), true}, {QStringLiteral("withSubTasks"), false},
         {QStringLiteral("navigation"), QJsonObject{{QStringLiteral("size"), tunables::kPageSizeChats}}}},
        operation, [this, taskId, operation](QJsonValue data) {
        const auto tasks = data.toObject().value(QStringLiteral("tasks"));
        if (!tasks.isArray())
            return failShape(operation);
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

void CloudBackend::requestTaskDependencies(const QString &taskId)
{
    const auto operation = QStringLiteral("gantt-links:") + taskId;
    if (numericId(taskId).isEmpty()) {
        emit failed(operation, QStringLiteral("Номер задачи недействителен"));
        return;
    }
    ajaxJson(QStringLiteral("tasks.v2.Task.Relation.Gantt.Dependence.list"),
        {{QStringLiteral("taskId"), taskId.toLongLong()}, {QStringLiteral("withIds"), false},
         {QStringLiteral("withCompleted"), true}, {QStringLiteral("withSubTasks"), false},
         {QStringLiteral("navigation"), QJsonObject{{QStringLiteral("size"), tunables::kPageSizeChats}}}},
        operation, [this, taskId, operation](QJsonValue data) {
        const auto tasks = data.toObject().value(QStringLiteral("tasks"));
        if (!tasks.isArray())
            return failShape(operation);
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

void CloudBackend::requestTaskHistory(const QString &taskId)
{
    const auto operation = QStringLiteral("task-history:") + taskId;
    if (numericId(taskId).isEmpty()) {
        emit failed(operation, QStringLiteral("Номер задачи недействителен"));
        return;
    }
    const auto url = OriginGuard(m_session.origin).resolve(QStringLiteral("/rest/tasks.task.history.list.json"));
    webRequest(url, {{QStringLiteral("taskId"), taskId}}, operation, QStringLiteral("result"),
               [this, taskId, operation](QJsonValue data) {
        const auto list = data.toObject().value(QStringLiteral("list"));
        if (!list.isArray()) {
            emit failed(operation, QStringLiteral("История задачи недоступна"));
            return;
        }
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
    });
}

void CloudBackend::requestProjectTimeline(const QString &projectId)
{
    if (numericId(projectId).isEmpty()) {
        emit failed(QStringLiteral("timeline:") + projectId, QStringLiteral("Некорректный номер проекта"));
        return;
    }
    loadProjectTimeline(projectId, 0, {});
}

void CloudBackend::loadProjectTimeline(const QString &projectId, int offset, QVector<Task> tasks)
{
    const auto operation = QStringLiteral("timeline:") + projectId;
    Fields fields{{QStringLiteral("filter[GROUP_ID]"), projectId},
                  {QStringLiteral("order[ID]"), QStringLiteral("DESC")},
                  {QStringLiteral("start"), QString::number(offset)}};
    for (const auto *name : {"ID", "TITLE", "GROUP_ID", "START_DATE_PLAN", "END_DATE_PLAN"})
        fields.append({QStringLiteral("select[]"), QLatin1String(name)});
    ajax(QStringLiteral("tasks.task.list"), fields, operation,
         [this, projectId, offset, operation, tasks = std::move(tasks)](QJsonValue data) mutable {
        const auto rows = data.toObject().value(QStringLiteral("tasks"));
        if (!rows.isArray())
            return failShape(operation);
        const auto page = rows.toArray();
        const auto previousLength = tasks.size();
        for (const auto value : page) {
            auto task = taskFromRow(value.toObject());
            if (!task.id.isEmpty() && task.projectId == projectId &&
                std::none_of(tasks.cbegin(), tasks.cend(), [&task](const Task &other) { return other.id == task.id; }))
                tasks.append(std::move(task));
            if (tasks.size() == tunables::kPageSizeBoardTasks)
                break;
        }
        constexpr int pageSize = 50;
        const bool full = page.size() >= pageSize;
        if (full && tasks.size() > previousLength && offset + pageSize < tunables::kPageSizeBoardTasks) {
            loadProjectTimeline(projectId, offset + pageSize, std::move(tasks));
            return;
        }
        emit projectTimelineLoaded(projectId, tasks, full);
    });
}

void CloudBackend::requestTaskMessages(const QString &taskId)
{
    if (m_openTask != taskId) {
        ++m_taskHistoryRetry.revision;
        m_openTask = taskId;
        m_taskMessagesDirty = false;
        m_taskPopupHistory = {};
    }
    updateHistoryCatchup(true);
    if (canAutoRefreshChats() && (m_requests.size() >= maxRequests
        || m_inflightOperations.contains(QStringLiteral("task-messages:") + taskId))) {
        m_taskMessagesDirty = true;
        scheduleRefresh();
        return;
    }
    if (const auto id = m_taskDialogs.value(taskId); !id.isEmpty()) {
        loadMessages(taskId, id, true);
        return;
    }
    const auto op = QStringLiteral("task-messages:") + taskId;
    loadTask(taskId, op, [this, taskId, op](const QJsonObject &) {
        const auto dialog = m_taskDialogs.value(taskId);
        if (dialog.isEmpty()) {
            emit failed(op, QStringLiteral("У задачи нет чата. Старые обсуждения могут быть в комментариях портала"));
            return;
        }
        loadMessages(taskId, dialog, true);
    });
}

void CloudBackend::sendMessage(const QString &key, const QString &dialogId, const QString &text, bool task,
                               const QString &replyId)
{
    const auto scope = (task ? QStringLiteral("task:") : QStringLiteral("chat:")) + key;
    const auto op = QStringLiteral("send:") + scope;
    if (m_inflightOperations.contains(op))
        return;
    if (validDialog(dialogId).isEmpty() || text.trimmed().isEmpty() || text.size() > 20000) {
        emit failed(op, QStringLiteral("Введите сообщение длиной до 20 000 символов"));
        return;
    }
    const auto reply = numericId(replyId);
    if (!replyId.isEmpty() && reply.isEmpty()) {
        emit failed(op, QStringLiteral("Некорректный идентификатор цитируемого сообщения"));
        return;
    }
    const auto templateId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_sentTemplates.enqueue(templateId);
    if (m_sentTemplates.size() > 64)
        m_sentTemplates.dequeue();
    Fields fields{{QStringLiteral("dialogId"), dialogId},
                  {QStringLiteral("fields[message]"), text},
                  {QStringLiteral("fields[templateId]"), templateId}};
    if (!reply.isEmpty())
        fields.append({QStringLiteral("fields[replyId]"), reply});
    ajax(QStringLiteral("im.v2.Chat.Message.send"), std::move(fields),
         op, [this, key, scope, op, dialogId, task](QJsonValue data) {
        const auto id = field(data.toObject(), {"id", "messageId"});
        if (id.isEmpty()) {
            emit failed(op, QStringLiteral("Портал не подтвердил отправку. Проверьте историю перед повтором"));
            return;
        }
        emit messageSent(scope, id);
        if (task && m_openTask == key)
            m_taskMessagesDirty = true;
        else if (!task && m_openDialog == dialogId)
            m_chatDirty = true;
        scheduleRefresh();
    });
}

void CloudBackend::sendChatMessage(const QString &dialogId, const QString &text)
{
    sendMessage(dialogId, dialogId, text, false);
}

void CloudBackend::sendChatReply(const QString &dialog, const QString &text, const QString &replyId)
{
    if (replyId.isEmpty()) {
        emit failed(QStringLiteral("send:chat:") + dialog, QStringLiteral("Выберите сообщение для ответа"));
        return;
    }
    sendMessage(dialog, dialog, text, false, replyId);
}

void CloudBackend::sendTaskMessage(const QString &taskId, const QString &text)
{
    sendTaskMessage(taskId, text, {});
}

void CloudBackend::sendTaskReply(const QString &taskId, const QString &text, const QString &replyId)
{
    if (replyId.isEmpty()) {
        emit failed(QStringLiteral("send:task:") + taskId, QStringLiteral("Выберите сообщение для ответа"));
        return;
    }
    sendTaskMessage(taskId, text, replyId);
}

void CloudBackend::sendTaskMessage(const QString &taskId, const QString &text, const QString &replyId)
{
    const auto dialog = m_taskDialogs.value(taskId);
    if (dialog.isEmpty()) {
        emit failed(QStringLiteral("send:task:") + taskId, QStringLiteral("Сначала откройте чат задачи"));
        return;
    }
    sendMessage(taskId, dialog, text, true, replyId);
}

QString CloudBackend::replyPreviewKey(const QString &dialog, const QString &message)
{
    return dialog + QLatin1Char('\n') + message;
}

void CloudBackend::resetNoticeState()
{
    const bool pagingVisible = m_loadingOlderNotifications || m_notificationOlderCursor.id > 0;
    m_notifications.clear();
    m_notificationSearchRows.clear();
    m_notificationSearchQuery.clear();
    m_notificationMutations.clear();
    m_notificationRevision = 0;
    m_notificationOlderCursor = {};
    m_notificationOlderPages = 0;
    m_loadingOlderNotifications = false;
    m_notificationsDirty = false;
    m_pendingInitialAlerts.clear();
    m_pendingChatPopups.clear();
    m_seenChatPopups.clear();
    m_chatPopupHistory = {};
    m_taskPopupHistory = {};
    if (pagingVisible)
        publishNotificationHistoryPageState();
}

void CloudBackend::notificationBatch(const QString &method, const QUrlQuery &parameters,
                                     const QString &operation, std::function<void(QJsonValue)> onResult,
                                     const QString &requestKey)
{
    auto command = method;
    if (!parameters.isEmpty())
        command += QLatin1Char('?') + parameters.toString(QUrl::FullyEncoded);
    const auto url = OriginGuard(m_session.origin).resolve(QStringLiteral("/rest/batch.json"));
    webRequest(url, {{QStringLiteral("halt"), QStringLiteral("0")},
                     {QStringLiteral("cmd[notify]"), command}},
               operation, QStringLiteral("result"),
               [this, operation, onResult = std::move(onResult)](QJsonValue batch) {
        const auto result = batch.toObject().value(QStringLiteral("result")).toObject().value(QStringLiteral("notify"));
        if (result.isUndefined() || result.isNull())
            return failShape(operation);
        onResult(result);
    }, notificationRequestKey(m_accountScopeRevision, requestKey.isEmpty() ? operation : requestKey));
}

QVector<Notification> CloudBackend::decodeNotifications(const QJsonArray &rows, const QJsonValue &users,
                                                         quint64 requestedAt) const
{
    QVector<Notification> items;
    QSet<QString> ids;
    for (const auto row : rows) {
        if (items.size() == maxNotifications)
            break;
        const auto item = row.toObject();
        const auto id = numericId(field(item, {"id"}));
        if (id.isEmpty() || ids.contains(id))
            continue;
        const auto version = m_notificationMutations.constFind(id);
        const bool newer = version != m_notificationMutations.cend() && version->revision > requestedAt;
        if (newer && version->removed)
            continue;
        ids.insert(id);
        const auto title = notificationDisplayText(field(item, {"notify_title"}));
        Notification notice{id, field(item, {"notify_module"}),
                            title.isEmpty() ? QStringLiteral("Уведомление Битрикс24") : title, {},
                            date(item.value(QStringLiteral("date"))),
                            newer ? version->read : field(item, {"notify_read"}) == QLatin1String("Y")};
        const auto authorId = noticeAuthorId(item);
        decodeNotificationContent(notice, m_session.origin, field(item, {"text"}),
                                  item.value(QStringLiteral("params")).toObject(),
                                  authorId, noticeAuthorName(authorId, users));
        items.append(std::move(notice));
    }
    return items;
}

CloudBackend::NotificationCursor CloudBackend::notificationCursor(const QJsonArray &rows)
{
    if (rows.size() != notificationPageSize)
        return {};
    const auto tail = rows.last().toObject();
    const auto id = numericId(field(tail, {"id"})).toLongLong();
    const int type = field(tail, {"notify_type"}).toInt();
    if (id <= 0 || (type != 1 && type != 2 && type != 4))
        return {};
    return {id, type == 1 ? 1 : 3};
}

void CloudBackend::publishNotificationHistoryPageState()
{
    const bool more = m_notificationOlderCursor.id > 0;
    emit notificationHistoryPageStateChanged(more, m_loadingOlderNotifications);
}

void CloudBackend::publishNotificationPage(QVector<Notification> items)
{
    QSet<QString> ids;
    for (const auto &item : std::as_const(items))
        ids.insert(item.id);
    for (const auto &live : std::as_const(m_notifications)) {
        if (!ids.contains(live.id)) {
            ids.insert(live.id);
            items.append(live);
        }
    }
    std::stable_sort(items.begin(), items.end(), [](const Notification &left, const Notification &right) {
        return left.createdAt > right.createdAt;
    });
    trimNotificationHistory(items, m_notificationOlderPages);
    m_notifications = std::move(items);
    emit notificationsLoaded(m_notifications);
    publishNotificationHistoryPageState();
}

void CloudBackend::requestNotifications()
{
    QUrlQuery parameters;
    parameters.addQueryItem(QStringLiteral("LIMIT"), QString::number(notificationPageSize));
    parameters.addQueryItem(QStringLiteral("CONVERT_TEXT"), QStringLiteral("Y"));
    notificationBatch(QStringLiteral("im.notify.get"), parameters, notificationsOp,
                      [this, revision = m_notificationRevision](QJsonValue result) {
        const auto response = result.toObject();
        const auto rows = response.value(QStringLiteral("notifications"));
        if (!rows.isArray())
            return failShape(notificationsOp);

        const auto page = rows.toArray();
        if (m_notificationOlderPages == 0 && !m_loadingOlderNotifications)
            m_notificationOlderCursor = notificationCursor(page);
        publishNotificationPage(decodeNotifications(page, response.value(QStringLiteral("users")), revision));
    });
}

void CloudBackend::requestOlderNotifications()
{
    const auto cursor = m_notificationOlderCursor;
    if (!m_started || m_loadingOlderNotifications || cursor.id <= 0)
        return;
    m_loadingOlderNotifications = true;
    publishNotificationHistoryPageState();
    QUrlQuery parameters;
    parameters.addQueryItem(QStringLiteral("LIMIT"), QString::number(notificationPageSize));
    parameters.addQueryItem(QStringLiteral("CONVERT_TEXT"), QStringLiteral("Y"));
    parameters.addQueryItem(QStringLiteral("LAST_ID"), QString::number(cursor.id));
    parameters.addQueryItem(QStringLiteral("LAST_TYPE"), QString::number(cursor.type));
    notificationBatch(QStringLiteral("im.notify.get"), parameters, olderNotificationsOp,
                      [this, cursor, revision = m_notificationRevision](QJsonValue result) {
        const auto response = result.toObject();
        const auto rows = response.value(QStringLiteral("notifications"));
        if (!rows.isArray())
            return failShape(olderNotificationsOp);
        const auto page = rows.toArray();
        const auto next = notificationCursor(page);
        const bool advances = next.id > 0 && ((next.type == cursor.type && next.id < cursor.id)
            || (cursor.type == 1 && next.type == 3));
        m_notificationOlderCursor = advances ? next : NotificationCursor{};
        if (m_notificationOlderPages < std::numeric_limits<int>::max())
            ++m_notificationOlderPages;
        m_loadingOlderNotifications = false;
        publishNotificationPage(decodeNotifications(page, response.value(QStringLiteral("users")), revision));
    });
}

void CloudBackend::searchNotifications(const QString &query)
{
    const auto operation = QStringLiteral("search-notifications:") + query;
    if (query.size() < 3 || query.size() > 200)
        return failShape(operation);
    if (m_notificationSearchQuery != query)
        m_notificationSearchRows.clear();
    m_notificationSearchQuery = query;
    QUrlQuery parameters;
    // The batch command is decoded again to build REST query parameters. Encode
    // separators in the literal search term before constructing the command.
    parameters.setQuery(QStringLiteral("SEARCH_TEXT=") + QString::fromLatin1(QUrl::toPercentEncoding(query)));
    notificationBatch(QStringLiteral("im.notify.history.search"), parameters, operation,
                      [this, query, operation, revision = m_notificationRevision](QJsonValue result) {
        if (query != m_notificationSearchQuery)
            return;
        const auto response = result.toObject();
        const auto rows = response.value(QStringLiteral("notifications"));
        if (!rows.isArray())
            return failShape(operation);
        m_notificationSearchRows = decodeNotifications(rows.toArray(), response.value(QStringLiteral("users")), revision);
        emit notificationsFound(query, m_notificationSearchRows);
    });
}

void CloudBackend::updateNotificationReadState(const QStringList &ids, bool read, std::optional<quint64> started)
{
    if (ids.isEmpty())
        return;
    const auto revision = ++m_notificationRevision;
    QStringList changed;
    for (const auto &id : ids) {
        const auto previous = m_notificationMutations.constFind(id);
        if (previous != m_notificationMutations.cend()
            && (previous->removed || (started && previous->revision > *started)))
            continue;
        if (m_notificationMutations.size() >= 256 && !m_notificationMutations.contains(id))
            m_notificationMutations.erase(m_notificationMutations.begin());
        m_notificationMutations.insert(id, {revision, read, false});
        changed.append(id);
    }
    if (changed.isEmpty())
        return;
    for (auto *source : {&m_notifications, &m_notificationSearchRows})
        for (auto &item : *source)
            if (changed.contains(item.id))
                item.read = read;
    emit notificationReadStateChanged(changed, read);
}

void CloudBackend::removeNotification(const QString &id)
{
    if (id.isEmpty())
        return;
    const auto previous = m_notificationMutations.constFind(id);
    if (previous != m_notificationMutations.cend() && previous->removed)
        return;
    if (m_notificationMutations.size() >= 256 && !m_notificationMutations.contains(id))
        m_notificationMutations.erase(m_notificationMutations.begin());
    m_notificationMutations.insert(id, {++m_notificationRevision, false, true});
    for (auto *source : {&m_notifications, &m_notificationSearchRows})
        source->erase(std::remove_if(source->begin(), source->end(), [&id](const Notification &item) {
            return item.id == id;
        }), source->end());
    emit notificationRemoved(id);
}

void CloudBackend::markNotificationRead(const QString &id)
{
    const auto operation = QStringLiteral("read-notification");
    if (id.isEmpty() || numericId(id) != id)
        return failShape(operation);
    QUrlQuery parameters;
    parameters.addQueryItem(QStringLiteral("ID"), id);
    parameters.addQueryItem(QStringLiteral("ACTION"), QStringLiteral("Y"));
    parameters.addQueryItem(QStringLiteral("ONLY_CURRENT"), QStringLiteral("Y"));
    notificationBatch(QStringLiteral("im.notify.read"), parameters, operation,
                      [this, id, operation, started = m_notificationRevision](QJsonValue result) {
        if (result != true && result.toObject().value(QStringLiteral("result")) != true)
            return failShape(operation);
        updateNotificationReadState({id}, true, started);
        emit notificationActionFinished(operation);
    });
}

void CloudBackend::markNotificationsRead(const QStringList &ids)
{
    if (ids.isEmpty())
        return;
    const auto operation = QStringLiteral("read-notifications");
    QStringList selected;
    QStringList local;
    QSet<QString> seen;
    QUrlQuery parameters;
    for (const auto &id : ids) {
        if (id.isEmpty() || seen.contains(id))
            continue;
        seen.insert(id);
        if (isLocalNotificationId(id)) {
            const auto known = std::any_of(m_notifications.cbegin(), m_notifications.cend(), [&id](const Notification &item) {
                return item.id == id;
            });
            if (known)
                local.append(id);
            continue;
        }
        if (numericId(id) != id)
            continue;
        selected.append(id);
        parameters.addQueryItem(QStringLiteral("IDS[]"), id);
        if (selected.size() == maxNotifications)
            break;
    }
    if (selected.isEmpty() && local.isEmpty())
        return failShape(operation);
    updateNotificationReadState(local, true);
    if (selected.isEmpty()) {
        emit notificationActionFinished(operation);
        return;
    }
    parameters.addQueryItem(QStringLiteral("ACTION"), QStringLiteral("Y"));
    auto keyIds = selected;
    keyIds.sort();
    const auto requestKey = operation + QLatin1Char(':') + keyIds.join(QLatin1Char(','));
    notificationBatch(QStringLiteral("im.notify.read.list"), parameters, operation,
                      [this, operation, selected = std::move(selected), started = m_notificationRevision](QJsonValue result) {
        if (result != true)
            return failShape(operation);
        updateNotificationReadState(selected, true, started);
        emit notificationActionFinished(operation);
    }, requestKey);
}

void CloudBackend::markAllNotificationsRead()
{
    const auto operation = QStringLiteral("read-all-notifications");
    QStringList snapshot;
    for (const auto *source : {&m_notifications, &m_notificationSearchRows})
        for (const auto &item : *source)
            if (!item.read && !snapshot.contains(item.id))
                snapshot.append(item.id);
    notificationBatch(QStringLiteral("im.notify.read.all"), {}, operation,
                      [this, operation, snapshot, started = m_notificationRevision](QJsonValue result) {
        if (result.toObject().value(QStringLiteral("result")) != true)
            return failShape(operation);
        updateNotificationReadState(snapshot, true, started);
        emit notificationActionFinished(operation);
    });
}

QString CloudBackend::notificationTaskId(const QString &dialog, const QJsonObject &eventChat) const
{
    QSet<QString> matching;
    const auto add = [&matching](const QString &id) {
        if (const auto valid = numericId(id); !valid.isEmpty())
            matching.insert(valid);
    };
    const auto taskChat = isTaskChatType(field(eventChat, {"type", "kind"}));
    const auto entity = eventChat.value(QStringLiteral("entityLink")).toObject();
    const auto linkType = field(entity, {"type"});
    if (isTaskChatType(linkType) || (linkType.isEmpty() && taskChat))
        add(field(entity, {"id"}));
    const auto entityType = field(eventChat, {"entityType", "entity_type"});
    if (isTaskChatType(entityType) || (entityType.isEmpty() && taskChat))
        add(field(eventChat, {"entityId", "entity_id"}));
    for (auto it = m_taskDialogs.cbegin(); it != m_taskDialogs.cend(); ++it)
        if (it.value() == dialog)
            add(it.key());
    for (const auto &known : m_recentTaskChatItems)
        if (known.id == dialog && known.taskChat)
            add(known.taskId);
    return matching.size() == 1 ? *matching.cbegin() : QString();
}

void CloudBackend::deleteNotification(const QString &id)
{
    const auto operation = QStringLiteral("delete-notification");
    if (id.isEmpty() || numericId(id) != id)
        return failShape(operation);
    QUrlQuery parameters;
    parameters.addQueryItem(QStringLiteral("ID"), id);
    notificationBatch(QStringLiteral("im.notify.delete"), parameters, operation, [this, id, operation](QJsonValue result) {
        if (result != true)
            return failShape(operation);
        removeNotification(id);
        emit notificationActionFinished(operation);
    });
}

void CloudBackend::connectPull()
{
    if (!m_started || m_authExpired || m_inflightOperations.contains(pushOp))
        return;
    auto url = OriginGuard(m_session.origin).resolve(QStringLiteral("/rest/pull.config.get.json"));
    QUrlQuery configQuery;
    configQuery.addQueryItem(QStringLiteral("logTag"), QStringLiteral("client_start"));
    url.setQuery(configQuery);
    webRequest(url, {{QStringLiteral("CACHE"), QStringLiteral("N")}}, pushOp, QStringLiteral("result"),
               [this](QJsonValue response) {
        const auto config = response.toObject();
        const auto server = config.value(QStringLiteral("server")).toObject();
        QUrl socketUrl(field(server, {"websocket_secure"}));
        const auto channels = config.value(QStringLiteral("channels")).toObject();

        QUrlQuery query;
        const auto jwt = field(config, {"jwt"});
        if (!jwt.isEmpty()) {
            query.addQueryItem(QStringLiteral("token"), jwt);
        } else {
            QStringList ids;
            for (const auto *kind : {"private", "shared"}) {
                const auto id = field(channels.value(QLatin1String(kind)).toObject(), {"id"});
                if (!id.isEmpty())
                    ids.append(id);
            }
            if (!ids.isEmpty())
                query.addQueryItem(QStringLiteral("CHANNEL_ID"), ids.join(QLatin1Char('/')));
        }

        if (query.isEmpty() || !trustedSocket(socketUrl, m_session.origin, m_allowLoopback)) {
            emit failed(pushOp, QStringLiteral("Портал не предоставил допустимый защищённый канал уведомлений"));
            scheduleReconnect();
            return;
        }
        if (const auto clientId = field(config, {"clientId"}); !clientId.isEmpty())
            query.addQueryItem(QStringLiteral("clientId"), clientId);
        if (const auto hostname = field(server, {"hostname"}); !hostname.isEmpty())
            query.addQueryItem(QStringLiteral("hostname"), hostname);
        query.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
        query.addQueryItem(QStringLiteral("revision"), QStringLiteral("19"));
        if (!m_mid.isEmpty()) {
            query.addQueryItem(QStringLiteral("mid"), m_mid);
        } else if (!m_tag.isEmpty() && !m_time.isEmpty()) {
            query.addQueryItem(QStringLiteral("tag"), m_tag);
            query.addQueryItem(QStringLiteral("time"), m_time);
        }
        socketUrl.setQuery(query);
        // Signed Pull channels authenticate the socket. The portal's Cookie and CSRF stay on the portal.
        m_socket.open(socketUrl);
        m_watchdog.start(handshakeTimeoutMs);

        qint64 earliest = 0;
        const auto addExpiry = [&earliest](qint64 expiry) {
            if (expiry > 0 && (earliest == 0 || expiry < earliest))
                earliest = expiry;
        };
        addExpiry(field(config, {"exp"}).toLongLong());
        for (const auto *kind : {"private", "shared"})
            addExpiry(field(channels.value(QLatin1String(kind)).toObject(), {"end"}).toLongLong());
        if (earliest > 0) {
            const auto seconds = std::clamp<qint64>(earliest - QDateTime::currentSecsSinceEpoch() - 10, 5, 86400);
            m_configExpiry.start(static_cast<int>(seconds * 1000));
        }
    });
}

void CloudBackend::scheduleReconnect(bool immediately)
{
    m_watchdog.stop();
    m_heartbeat.stop();
    m_configExpiry.stop();
    setOnline(false);
    if (!m_started || m_authExpired) {
        if (m_socket.state() != QAbstractSocket::UnconnectedState)
            m_socket.abort();
        return;
    }
    if (m_retry.isActive())
        return;
    const int ms = immediately ? 100
        : std::min(tunables::kReconnectMaxDelayMs, tunables::kReconnectBaseDelayMs << std::min(m_attempt++, 5));
    m_attempt = std::min(m_attempt, 6);
    m_retry.start(ms + QRandomGenerator::global()->bounded(std::max(1, ms / 4)));
    if (m_socket.state() != QAbstractSocket::UnconnectedState)
        m_socket.abort();
}

void CloudBackend::receivePull(const QString &frame)
{
    if (!m_started)
        return;
    m_watchdog.stop();
    m_heartbeat.start(heartbeatIntervalMs);
    if (frame == QLatin1String("ping")) {
        m_socket.sendTextMessage(QStringLiteral("pong"));
        return;
    }
    const auto receiveJson = [this](const QByteArray &payload) {
        const auto document = QJsonDocument::fromJson(payload);
        if (document.isArray()) {
            for (const auto event : document.array())
                handlePullEvent(event.toObject());
        } else if (document.isObject()) {
            const auto object = document.object();
            if (object.value(QStringLiteral("messages")).isArray()) {
                for (const auto event : object.value(QStringLiteral("messages")).toArray())
                    handlePullEvent(event.toObject());
            } else {
                handlePullEvent(object);
            }
        }
    };
    const QLatin1String prefix("#!NGINXNMS!#");
    const QLatin1String suffix("#!NGINXNME!#");
    if (!frame.startsWith(prefix))
        return receiveJson(frame.toUtf8());
    for (qsizetype offset = 0; (offset = frame.indexOf(prefix, offset)) >= 0;) {
        const qsizetype start = offset + prefix.size();
        const qsizetype end = frame.indexOf(suffix, start);
        if (end < 0)
            break;
        receiveJson(frame.mid(start, end - start).toUtf8());
        offset = end + suffix.size();
    }
}

void CloudBackend::handlePullEvent(const QJsonObject &event)
{
    const auto mid = field(event, {"mid"});
    const auto tag = field(event, {"tag"});
    const auto time = field(event, {"time"});
    if (!mid.isEmpty() && m_seenEventIds.contains(mid))
        return;
    if (!mid.isEmpty()) {
        m_mid = mid;
        m_seenEventIds.insert(mid);
        m_seenEvents.enqueue(mid);
        if (m_seenEvents.size() > maxSeenEvents)
            m_seenEventIds.remove(m_seenEvents.dequeue());
    }
    if (!tag.isEmpty() && !time.isEmpty()) {
        m_tag = tag;
        m_time = time;
    }

    const auto value = event.value(QStringLiteral("text"));
    const auto body = value.isObject() ? value.toObject()
        : value.isString() ? QJsonDocument::fromJson(value.toString().toUtf8()).object() : event;
    const auto module = field(body, {"module_id"});
    const auto command = field(body, {"command"});
    const auto params = body.value(QStringLiteral("params")).toObject();
    const auto recentExtra = body.contains(QStringLiteral("extra")) ? body.value(QStringLiteral("extra")).toObject()
                                                                    : event.value(QStringLiteral("extra")).toObject();
    if (module == QLatin1String("pull")) {
        if (command == QLatin1String("channel_expire") || command == QLatin1String("config_expire")
            || command == QLatin1String("server_restart"))
            scheduleReconnect(true);
        return;
    }
    if (module == QLatin1String("tasks")) {
        const auto fields = params.value(QStringLiteral("fields")).toObject();
        auto taskId = numericId(field(params, {"taskId", "TASK_ID", "id", "ID"}));
        if (taskId.isEmpty())
            taskId = numericId(field(params.value(QStringLiteral("task")).toObject(), {"id", "ID"}));
        if (taskId.isEmpty())
            taskId = numericId(field(fields, {"id", "ID"}));
        auto groupId = field(params, {"groupId", "GROUP_ID"});
        if (groupId.isEmpty())
            groupId = field(fields, {"GROUP_ID", "groupId"});
        const auto changedProject = field(fields, {"GROUP_ID", "groupId"});
        bool hasProject = false;
        const auto project = changedProject.toLongLong(&hasProject);
        if (hasProject && !taskId.isEmpty() && project >= 0) {
            const auto chat = explicitMessageChatId(fields);
            const auto remembered = groupDialogChatId(m_taskDialogs.value(taskId));
            updateProjectUnreadTask(taskId.toLongLong(), remembered > 0 ? remembered
                : m_projectUnreadParentsByChat.contains(chat) ? chat : 0, project);
        }
        // After moving away, the event may identify only the destination board.
        const bool shownOnBoard = !taskId.isEmpty()
            && std::any_of(m_boardColumnCursors.cbegin(), m_boardColumnCursors.cend(),
                [&taskId](const BoardColumnCursor &cursor) { return cursor.seen.contains(taskId); });
        if (!m_openBoard.isEmpty() && (groupId.isEmpty() || groupId == m_openBoard || shownOnBoard))
            m_boardDirty = true;
        if (!taskId.isEmpty() && (!groupId.isEmpty() || command == QLatin1String("task_update")
                                 || command == QLatin1String("taskUpdate")))
            emit taskLinkPreviewInvalidated(taskId);
        invalidateTaskChat(taskId);
        return;
    }
    if (module == QLatin1String("online") && command == QLatin1String("userStatus")) {
        const auto age = recentExtra.value(QStringLiteral("server_time_ago")).toInteger();
        if (age >= 0 && age < 240) {
            handlePortalStatusPush(params.value(QStringLiteral("users")).toObject(),
                                   field(recentExtra, {"server_time_unix"}).toLongLong());
            handleOnlinePull(params);
        }
        return;
    }
    if (module != QLatin1String("im"))
        return;

    if (command == QLatin1String("pinAdd") || command == QLatin1String("pinDelete")) {
        handlePinnedPull(command, params);
        return;
    }
    if (handleAnchorPull(command, params))
        return;
    if (command == QLatin1String("settingsUpdate")) {
        const auto revision = ++m_notificationSettingsRevision;
        const auto remember = [this, &params, revision](const QString &key) {
            if (params.contains(key))
                m_notificationSettingChanges.insert(key, {revision, params.value(key)});
        };
        for (const auto &key : {soundMuted, volumeGeneral, volumeMessages, notifyScheme})
            remember(key);
        for (const auto &key : simpleChannels)
            remember(key);
        if (params.contains(notifyScheme))
            m_notificationSettings.clear();
        applyChatPopupSettings(params, ChatPopupSettingsSource::Push);
        return;
    }
    auto dialog = validDialog(field(params, {"dialogId"}));
    const auto chatId = numericId(field(params, {"chatId"}));
    if (dialog.isEmpty() && !chatId.isEmpty()) {
        const auto id = chatId.toLongLong();
        const auto personal = std::find_if(m_recentChatItems.cbegin(), m_recentChatItems.cend(), [id](const Chat &chat) {
            return chat.serverChatId == id;
        });
        dialog = personal != m_recentChatItems.cend() ? personal->id
            : m_dialogChatIds.key(id, chatDialog(chatId));
    }
    if (command == QLatin1String("readMessageOpponent") || command == QLatin1String("readMessageChatOpponent")) {
        const auto user = numericId(field(params, {"userId"}));
        if (dialog.isEmpty() || user.isEmpty() || m_userId.isEmpty() || user == m_userId)
            return;
        auto &seen = dialogMessageState(dialog).seenByOthers;
        QStringList confirmed;
        const auto rows = params.value(QStringLiteral("viewedMessages")).toArray();
        for (qsizetype i = 0; i < std::min<qsizetype>(rows.size(), tunables::kPageSizeMessages); ++i) {
            const auto id = numericId(string(rows.at(i)));
            const auto number = id.toLongLong();
            if (number <= 0 || seen.contains(number))
                continue;
            if (seen.size() >= tunables::kPageSizeMessages)
                seen.erase(std::min_element(seen.cbegin(), seen.cend()));
            seen.insert(number);
            confirmed.append(id);
        }
        if (!confirmed.isEmpty())
            emit chatMessagesSeen(dialog, confirmed);
        return;
    }
    if (command == QLatin1String("inputActionNotify")) {
        const auto user = numericId(field(params, {"userId"}));
        if (m_foreground && !dialog.isEmpty() && !user.isEmpty() && !m_userId.isEmpty() && user != m_userId
            && params.value(QStringLiteral("type")) == QLatin1String("writing")) {
            const auto duration = params.value(QStringLiteral("duration")).toInt();
            emit chatTypingChanged(dialog, user, duration > 0 ? std::min(duration, 25000) : 25000);
        }
        return;
    }
    if (command == QLatin1String("message") || command == QLatin1String("messageChat")) {
        const auto user = numericId(field(params.value(QStringLiteral("message")).toObject(), {"senderId", "authorId"}));
        if (!dialog.isEmpty() && !user.isEmpty() && user != m_userId)
            emit chatTypingChanged(dialog, user, 0);
    }
    if (command == QLatin1String("addReaction") || command == QLatin1String("deleteReaction")) {
        const auto reaction = params.value(QStringLiteral("actualReactions")).toObject().value(QStringLiteral("reaction")).toObject();
        const auto id = numericId(field(reaction, {"messageId"}));
        const auto matches = [&](const QString &shown) {
            return !shown.isEmpty() && (shown == dialog || (dialog.isEmpty() && !id.isEmpty()
                && m_replyPreviews.contains(replyPreviewKey(shown, id))));
        };
        if (matches(m_openDialog))
            m_chatDirty = true;
        if (matches(m_taskDialogs.value(m_openTask)))
            m_taskMessagesDirty = true;
        scheduleRefresh();
        return;
    }
    const auto mappedChat = badgeChatId(dialog);
    if (!dialog.isEmpty() && (chatId.isEmpty() || mappedChat == 0 || mappedChat == chatId.toLongLong())) {
        QStringList deleted;
        if (command == QLatin1String("messageUpdate") || command == QLatin1String("messageDelete")
            || command == QLatin1String("messageDeleteComplete")) {
            const auto message = numericId(field(params, {"id"}));
            if (!message.isEmpty()) {
                m_replyPreviews.remove(replyPreviewKey(dialog, message));
                if (command != QLatin1String("messageUpdate"))
                    deleted.append(message);
            }
        } else if (command == QLatin1String("messageDeleteV2")) {
            for (const auto &value : params.value(QStringLiteral("messages")).toArray()) {
                const auto row = value.toObject();
                const auto message = numericId(field(row, {"id"}));
                const auto rowChat = field(row, {"chatId"});
                const auto rowDialog = field(row, {"dialogId"});
                if (message.isEmpty() || (!rowDialog.isEmpty() && rowDialog != dialog)
                    || (!rowChat.isEmpty() && mappedChat > 0 && rowChat.toLongLong() != mappedChat))
                    continue;
                m_replyPreviews.remove(replyPreviewKey(dialog, message));
                if (!deleted.contains(message))
                    deleted.append(message);
                if (deleted.size() >= tunables::kPageSizeMessages)
                    break;
            }
        } else if (command == QLatin1String("chatHistoryDeletedForSelf") || command == QLatin1String("chatHistoryDeletedForAll")) {
            bool validBoundary = false;
            const auto through = field(params, {"lastId"}).toLongLong(&validBoundary);
            if (!params.contains(QStringLiteral("lastId")) || (validBoundary && through >= 0)) {
                if (through == 0)
                    ++dialogMessageState(dialog).historyGeneration;
                emit chatHistoryRemoved(dialog, {}, through);
                if (m_openDialog == dialog)
                    requestChatMessages(dialog);
                if (!m_openTask.isEmpty() && m_taskDialogs.value(m_openTask) == dialog)
                    requestTaskMessages(m_openTask);
            }
        }
        if (!deleted.isEmpty())
            emit chatHistoryRemoved(dialog, deleted);
    }
    if (command == QLatin1String("chatMuteNotify")) {
        const auto muted = notificationFlag(params.value(QStringLiteral("muted")));
        if (!dialog.isEmpty() && muted) {
            auto &state = dialogMessageState(dialog);
            state.muted = muted;
            state.muteList = {};
            state.muteFromPull = true;
            deliverChatPopups();
        }
        return;
    }
    const bool readEvent = command == QLatin1String("readMessage") || command == QLatin1String("readMessageChat");
    if (readEvent && !dialog.isEmpty()) {
        if (!acceptsRecentRead(dialog, params, recentExtra))
            return;
        if (const auto counter = unreadCount(params.value(QStringLiteral("counter"))))
            dialogMessageState(dialog).unreadCounter = counter;
        applyReadHistory(dialog, params);
    }

    if (command == QLatin1String("notifyRead") || command == QLatin1String("notifyUnread")
        || command == QLatin1String("notifyConfirm") || command == QLatin1String("notifyDelete")) {
        const bool remove = command == QLatin1String("notifyConfirm") || command == QLatin1String("notifyDelete");
        if (remove) {
            removeNotification(numericId(field(params, {"id"})));
        } else {
            QStringList ids;
            for (const auto value : params.value(QStringLiteral("list")).toArray()) {
                const auto id = numericId(string(value));
                if (!id.isEmpty())
                    ids.append(id);
            }
            updateNotificationReadState(ids, command == QLatin1String("notifyRead"));
        }
        emit notificationsLoaded(m_notifications);
        return;
    }

    if (readEvent || command.startsWith(QLatin1String("message"))) {
        m_recentDirty = true;
        const auto eventChat = chatDialog(chatId);
        const auto matches = [&](const QString &dialogId) {
            return !dialogId.isEmpty() && (dialog == dialogId || eventChat == dialogId);
        };
        if (matches(m_openDialog))
            m_chatDirty = true;
        if (matches(m_taskDialogs.value(m_openTask)))
            m_taskMessagesDirty = true;
        scheduleRefresh();
    }
    applyRecentPull(command, dialog, chatId, params, recentExtra);
    if (command != QLatin1String("message") && command != QLatin1String("messageChat")
        && command != QLatin1String("notify") && command != QLatin1String("notifyAdd"))
        return;

    const auto message = params.value(QStringLiteral("message")).toObject();
    auto notification = params.value(QStringLiteral("notify")).toObject();
    const auto author = numericId(field(message, {"senderId", "authorId"}));
    const auto templateId = field(message, {"templateId", "uuid"});
    if ((!m_userId.isEmpty() && m_userId == author)
        || (!templateId.isEmpty() && m_sentTemplates.contains(templateId)))
        return;
    const bool isMessage = command.startsWith(QLatin1String("message"));
    if (!isMessage && params.value(QStringLiteral("onlyFlash")).toBool())
        return;
    const auto outerId = field(params, {"id"});
    const auto nestedId = field(notification, {"id"});
    if (!outerId.isEmpty() && !nestedId.isEmpty() && outerId != nestedId)
        notification = {};
    auto notifyTitle = notificationDisplayText(field(params, {"notify_title", "title"}));
    if (notifyTitle.isEmpty())
        notifyTitle = notificationDisplayText(field(notification, {"notify_title", "title"}));
    const auto title = isMessage ? m_recentTitles.value(dialog, QStringLiteral("Новое сообщение"))
        : notifyTitle.isEmpty() ? QStringLiteral("Уведомление Битрикс24") : notifyTitle;
    QString content = field(message, {"text", "message"});
    if (content.isEmpty())
        content = field(notification, {"text", "message"});
    if (content.isEmpty())
        content = field(params, {"text", "message", "notify"});
    if (content.isEmpty() && isMessage)
        content = title;
    auto id = numericId(isMessage ? field(message, {"id"}) : outerId.isEmpty() ? nestedId : outerId);
    if (id.isEmpty())
        id = QStringLiteral("push:") + (mid.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : mid);
    else if (isMessage)
        id.prepend(QStringLiteral("message:"));
    const auto notify = notificationFlag(params.value(QStringLiteral("notify")));
    const auto messageNotify = notificationFlag(message.value(QStringLiteral("params")).toObject().value(QStringLiteral("NOTIFY")));
    const auto blockedUsers = params.value(QStringLiteral("userBlockChat")).toObject().value(chatId);
    if (isMessage && (notify == false || messageNotify == false || mutedListContains(blockedUsers, m_userId)
                      || m_portalStatus == QLatin1String("dnd"))) {
        rememberChatPopup(dialog, id);
        m_pendingChatPopups.removeIf([&](const ChatPopup &popup) {
            return popup.dialog == dialog && popup.entry.id == id;
        });
    }
    if (std::any_of(m_notifications.cbegin(), m_notifications.cend(), [&id](const auto &item) { return item.id == id; }))
        return;
    const auto extra = body.contains(QStringLiteral("extra")) ? body.value(QStringLiteral("extra")).toObject()
                                                              : event.value(QStringLiteral("extra")).toObject();
    const auto serverTime = extra.value(QStringLiteral("server_time_unix"));
    auto published = date(serverTime);
    if (!published.isValid()) {
        const auto seconds = numericId(string(serverTime)).toLongLong();
        if (seconds > 0)
            published = QDateTime::fromSecsSinceEpoch(seconds);
    }
    const auto now = QDateTime::currentDateTime();
    bool hasAge = false;
    const auto age = extra.value(QStringLiteral("server_time_ago")).toVariant().toInt(&hasAge);
    auto noticeModule = field(notification, {"notify_module", "module"});
    if (noticeModule.isEmpty())
        noticeModule = field(params, {"notify_module", "module"});
    Notification entry{id, isMessage || noticeModule.isEmpty() ? module : noticeModule, title, {},
                       published.isValid() ? published : now.addSecs(hasAge ? -age : 0)};
    if (isLocalNotificationId(id)) {
        const auto previous = m_notificationMutations.constFind(id);
        if (previous != m_notificationMutations.cend())
            entry.read = previous->read;
    }
    if (isMessage) {
        entry.text = notificationDisplayText(content);
        auto &targets = entry.targets;
        targets = notificationTargets(m_session.origin, content);
        auto target = incomingMessageTarget(params, command == QLatin1String("messageChat"), true);
        QString taskId;
        if (target) {
            const auto chatMap = params.value(QStringLiteral("chat")).toObject();
            const auto targetChatId = QString::number(groupDialogChatId(target->id));
            auto eventChat = chatMap.value(targetChatId).toObject();
            if (eventChat.isEmpty() && numericId(field(chatMap, {"id", "chatId", "CHAT_ID"})) == targetChatId)
                eventChat = chatMap;
            const auto knownDialog = oneOf(eventChat, {"dialogId"});
            if (!knownDialog.isUndefined() && validDialog(string(knownDialog)) != target->id)
                eventChat = {};
            for (const auto *key : {"id", "chatId", "CHAT_ID"}) {
                const auto reportedId = eventChat.value(QLatin1String(key));
                if (!reportedId.isUndefined() && numericId(string(reportedId)) != targetChatId) {
                    eventChat = {};
                    break;
                }
            }
            taskId = target->id.startsWith(QLatin1String("chat"))
                ? notificationTaskId(target->id, eventChat) : QString();
            const bool listedTask = std::any_of(m_recentTaskChatItems.cbegin(), m_recentTaskChatItems.cend(),
                [&target](const Chat &chat) { return chat.id == target->id; });
            const auto sections = params.value(QStringLiteral("recentConfig")).toObject().value(QStringLiteral("sections")).toArray();
            const bool metadataTaskChat = isTaskChatMetadata(field(eventChat, {"type", "kind"}),
                eventChat.value(QStringLiteral("entityLink")).toObject())
                || isTaskChatType(field(eventChat, {"entityType", "entity_type"}));
            target->taskChat = !taskId.isEmpty() || isRecentTaskChat(metadataTaskChat, listedTask, sections)
                || std::any_of(m_taskDialogs.cbegin(), m_taskDialogs.cend(), [&target](const auto &known) {
                    return known == target->id;
                });
            entry.ordinaryChatMessage = !target->taskChat;
        }
        if (target && !target->messageId.isEmpty()) {
            if (!taskId.isEmpty()) {
                const auto taskLink = QStringLiteral("[URL=/tasks/task/view/%1/]Задача #%1[/URL]").arg(taskId);
                const auto plainMessage = entry.text;
                decodeNotificationContent(entry, m_session.origin, taskLink + QLatin1Char('\n') + content, {});
                entry.text = plainMessage;
            }
            const auto duplicate = std::find_if(targets.begin(), targets.end(), [&target](const auto &link) {
                return link.kind == NotificationTargetKind::Chat && link.id == target->id
                    && link.messageId == target->messageId;
            });
            if (duplicate == targets.end()) {
                if (targets.size() >= 8)
                    targets.removeLast();
                targets.prepend(std::move(*target));
            } else if (target->taskChat) {
                duplicate->taskChat = true;
            }
        }
    } else {
        auto noticeParams = notification.value(QStringLiteral("params")).toObject();
        if (noticeParams.isEmpty())
            noticeParams = params.value(QStringLiteral("params")).toObject();
        if (noticeParams.isEmpty())
            noticeParams = notification.isEmpty() ? params : notification;
        const auto authorId = noticeAuthorId(notification, params);
        QString authorName;
        for (const auto *source : {&params, &std::as_const(notification)}) {
            for (const auto *key : {"users", "user", "author"}) {
                authorName = noticeAuthorName(authorId, source->value(QLatin1String(key)));
                if (!authorName.isEmpty())
                    break;
            }
            if (!authorName.isEmpty())
                break;
        }
        decodeNotificationContent(entry, m_session.origin, content, noticeParams, authorId, authorName);
        if (entry.text.isEmpty())
            entry.text = entry.title;
    }
    m_notifications.prepend(entry);
    trimNotificationHistory(m_notifications, m_notificationOlderPages);
    emit notificationsLoaded(m_notifications);
    if (m_portalStatus == QLatin1String("dnd"))
        return;
    const bool fresh = (!hasAge || age <= freshPopupSeconds)
        && (!published.isValid() || published.secsTo(now) <= freshPopupSeconds);
    if (!isMessage) {
        if (fresh && field(params, {"silent"}) != QLatin1String("Y")) {
            if (m_initialPortalStatusPending) {
                if (m_pendingInitialAlerts.size() == maxPendingChatPopups)
                    m_pendingInitialAlerts.dequeue();
                m_pendingInitialAlerts.enqueue(entry);
            } else {
                emit notificationReceived(entry);
            }
        }
        return;
    }
    const bool important = message.value(QStringLiteral("isImportant")).toBool();
    if (!fresh || (!published.isValid() && !hasAge) || author.isEmpty() || dialog.isEmpty()
        || notify == false || messageNotify == false || (!important && notify != true))
        return;
    const auto pullChats = params.value(QStringLiteral("chat")).toObject();
    auto eventChat = pullChats.value(chatId).toObject();
    if (eventChat.isEmpty() && !field(pullChats, {"id", "chatId"}).isEmpty())
        eventChat = pullChats;
    if (!eventChat.isEmpty())
        rememberDialog(dialog, {{QStringLiteral("chat"), eventChat}});
    queueChatPopup({entry, dialog, author, blockedUsers});
}

void CloudBackend::reconcileHistoryPopups(const QString &dialog, qint64 chatId, bool task, const QJsonObject &history,
                                         const QVector<Message> &messages)
{
    if (chatId <= 0)
        return;

    const auto snapshot = history.value(QStringLiteral("messages")).toArray();
    QHash<qint64, QJsonObject> rows;
    qint64 newest = 0;
    for (qsizetype i = 0; i < qMin<qsizetype>(snapshot.size(), tunables::kPageSizeMessages); ++i) {
        const auto row = snapshot.at(i).toObject();
        const auto id = recentMessageId(row);
        const auto rowChat = explicitMessageChatId(row);
        if (id > 0 && (rowChat == 0 || rowChat == chatId)) {
            rows.insert(id, row);
            newest = qMax(newest, id);
        }
    }
    if (!snapshot.isEmpty() && rows.isEmpty())
        return;
    auto &scope = task ? m_taskPopupHistory : m_chatPopupHistory;
    const auto previous = scope.dialog == dialog ? scope.through.value_or(-1) : -1;
    scope = {dialog, qMax(newest, previous)};
    const auto state = m_dialogMessageStates.constFind(dialog);
    m_pendingChatPopups.removeIf([&](const ChatPopup &popup) {
        if (!popup.fromHistory || popup.dialog != dialog)
            return false;
        const auto id = popup.entry.id.section(QLatin1Char(':'), -1).toLongLong();
        const auto unread = rows.value(id).value(QStringLiteral("unread"));
        if (unread.isBool())
            return !unread.toBool();
        return state != m_dialogMessageStates.cend() && id <= state->lastReadId && !state->historyUnread.contains(id);
    });
    if (previous < 0 || m_foreground || !m_workspaceVisible || (!task && m_activeSection != 0)
        || m_authExpired || m_userId.isEmpty() || m_portalStatus == QLatin1String("dnd"))
        return;
    if (!m_online && m_portalStatus.isEmpty())
        return;

    const auto now = QDateTime::currentDateTime();
    const auto title = m_recentTitles.value(dialog, QStringLiteral("Новое сообщение"));
    const auto chat = history.value(QStringLiteral("chat")).toObject();
    const bool taskChat = task || !notificationTaskId(dialog, chat).isEmpty();
    for (const auto &message : messages) {
        const auto id = numericId(message.id).toLongLong();
        if (id <= previous || !message.unread || message.outgoing || message.system || !message.createdAt.isValid())
            continue;
        const auto age = message.createdAt.secsTo(now);
        if (age < -5 || age > freshHistoryPopupSeconds)
            continue;
        const auto row = rows.value(id);
        const auto author = explicitMessageAuthorId(row);
        const auto params = oneOf(row, {"params", "PARAMS"}).toObject();
        if (!author || *author <= 0 || QString::number(*author) == m_userId
            || notificationFlag(oneOf(row, {"notify", "NOTIFY"})) == false
            || notificationFlag(oneOf(params, {"NOTIFY", "notify"})) == false
            || notificationFlag(row.value(QStringLiteral("silent"))) == true
            || notificationFlag(row.value(QStringLiteral("isDeleted"))) == true
            || notificationFlag(params.value(QStringLiteral("IS_DELETED"))) == true)
            continue;
        auto text = notificationDisplayText(message.text);
        if (text.isEmpty())
            text = message.attachments.isEmpty() ? QStringLiteral("Новое сообщение") : QStringLiteral("Вложение");
        Notification entry{QStringLiteral("message:%1").arg(id), QStringLiteral("im"), title, text, message.createdAt};
        entry.ordinaryChatMessage = !taskChat;
        entry.targets.append({NotificationTargetKind::Chat, dialog, QString::number(id), {}, title, taskChat});
        queueChatPopup({std::move(entry), dialog, QString::number(*author), {}, true});
    }
}

bool CloudBackend::rememberChatPopup(const QString &dialog, const QString &noticeId)
{
    if (dialog.isEmpty() || noticeId.isEmpty())
        return false;
    const auto key = replyPreviewKey(dialog, noticeId);
    if (m_seenChatPopups.contains(key))
        return false;
    m_seenChatPopups.insert(key, new bool(true));
    return true;
}

void CloudBackend::queueChatPopup(ChatPopup popup)
{
    if (!rememberChatPopup(popup.dialog, popup.entry.id) || chatMuted(popup) || chatPopupsEnabled() == false)
        return;
    if (m_pendingChatPopups.size() == maxPendingChatPopups)
        m_pendingChatPopups.dequeue();
    m_pendingChatPopups.enqueue(std::move(popup));
    deliverChatPopups();
    if (!m_pendingChatPopups.isEmpty())
        loadChatPopupSettings();
}

CloudBackend::DialogMessageState &CloudBackend::dialogMessageState(const QString &dialog)
{
    if (!m_dialogMessageStates.contains(dialog) && m_dialogMessageStates.size() >= tunables::kPageSizeChats) {
        const auto taskDialog = m_taskDialogs.value(m_openTask);
        auto candidate = m_dialogMessageStates.begin();
        while (candidate != m_dialogMessageStates.end()
               && (candidate.key() == m_openDialog || candidate.key() == taskDialog))
            ++candidate;
        if (candidate != m_dialogMessageStates.end())
            m_dialogMessageStates.erase(candidate);
    }
    return m_dialogMessageStates[dialog];
}

bool CloudBackend::chatMuted(const ChatPopup &popup) const
{
    const auto state = m_dialogMessageStates.constFind(popup.dialog);
    return mutedListContains(popup.muteList, m_userId)
        || (state != m_dialogMessageStates.cend() && (state->muted.value_or(false) || mutedListContains(state->muteList, m_userId)));
}

std::optional<bool> CloudBackend::chatPopupsEnabled() const
{
    const auto scheme = m_chatPopupSettings.value(notifyScheme);
    if (scheme == simpleNotifyScheme) {
        if (const auto desktop = notificationFlag(m_chatPopupSettings.value(notifyOnComputer)))
            return desktop;
    }
    if (!m_chatPopupSettingsKnown && scheme != QLatin1String("expert"))
        return std::nullopt;

    const auto isMuted = notificationFlag(m_chatPopupSettings.value(soundMuted));
    bool generalKnown = false;
    bool messagesKnown = false;
    const auto general = m_chatPopupSettings.value(volumeGeneral).toVariant().toDouble(&generalKnown);
    const auto messages = m_chatPopupSettings.value(volumeMessages).toVariant().toDouble(&messagesKnown);
    if (isMuted == true || (generalKnown && general <= 0) || (messagesKnown && messages <= 0))
        return false;
    if (isMuted == false && generalKnown && messagesKnown)
        return true;
    return std::nullopt;
}

void CloudBackend::applyChatPopupSettings(const QJsonObject &settings, ChatPopupSettingsSource source)
{
    const bool push = source == ChatPopupSettingsSource::Push;
    const bool initial = source == ChatPopupSettingsSource::InitialSnapshot;
    if (settings.value(notifyScheme) == simpleNotifyScheme && !notificationFlag(settings.value(notifyOnComputer))) {
        if (push && m_chatPopupSettings.value(notifyScheme) != simpleNotifyScheme) {
            // A scheme change can omit the computer setting. Refresh it before displaying a banner.
            m_chatPopupSettings.remove(notifyOnComputer);
            m_chatPopupSettingsKnown = false;
            ++m_chatPopupSettingsGeneration;
            m_nextChatPopupSettingsRequest = 0;
        } else if (source == ChatPopupSettingsSource::RefreshSnapshot) {
            m_chatPopupSettings.remove(notifyOnComputer);
        }
    }
    if (!push && !settings.isEmpty())
        m_chatPopupSettingsKnown = true;
    for (const auto *key : {&notifyScheme, &notifyOnComputer, &soundMuted, &volumeGeneral, &volumeMessages}) {
        const auto setting = settings.value(*key);
        if (!setting.isUndefined() && (!initial || !m_chatPopupSettings.contains(*key)))
            m_chatPopupSettings.insert(*key, setting);
    }
    deliverChatPopups();
}

void CloudBackend::settleInitialPortalStatus()
{
    if (!std::exchange(m_initialPortalStatusPending, false))
        return;
    const auto pending = std::exchange(m_pendingInitialAlerts, {});
    const auto now = QDateTime::currentDateTime();
    for (const auto &entry : pending) {
        if (!m_started || m_authExpired || m_portalStatus == QLatin1String("dnd"))
            return;
        const auto current = std::find_if(m_notifications.cbegin(), m_notifications.cend(), [&entry](const auto &item) {
            return item.id == entry.id;
        });
        if (current != m_notifications.cend() && !current->read
            && entry.createdAt.secsTo(now) <= freshPopupSeconds)
            emit notificationReceived(entry);
    }
    deliverChatPopups();
}

void CloudBackend::deliverChatPopups()
{
    if (m_initialPortalStatusPending)
        return;
    if (m_authExpired || m_portalStatus == QLatin1String("dnd")) {
        m_pendingChatPopups.clear();
        return;
    }
    const auto enabled = chatPopupsEnabled();
    if (enabled == false) {
        m_pendingChatPopups.clear();
        return;
    }
    const auto now = QDateTime::currentDateTime();
    auto waiting = std::exchange(m_pendingChatPopups, {});
    for (const auto &popup : waiting) {
        if (!m_started)
            return;
        const bool watching = (m_activeSection == 0 && popup.dialog == m_openDialog)
            || (!m_openTask.isEmpty() && popup.dialog == m_taskDialogs.value(m_openTask));
        if ((!m_online && (!popup.fromHistory || m_portalStatus.isEmpty()))
            || (popup.fromHistory && (m_foreground || !m_workspaceVisible || !watching)))
            continue;
        const auto maxAge = popup.fromHistory ? freshHistoryPopupSeconds : freshPopupSeconds;
        const auto state = m_dialogMessageStates.constFind(popup.dialog);
        const auto id = popup.entry.id.section(QLatin1Char(':'), -1).toLongLong();
        const bool read = popup.fromHistory && state != m_dialogMessageStates.cend()
            && (id <= state->confirmedReadThrough || state->exactlyRead.contains(id));
        if (read || popup.author == m_userId || popup.entry.createdAt.secsTo(now) > maxAge || chatMuted(popup))
            continue;
        if (enabled == true && !m_userId.isEmpty())
            emit notificationReceived(popup.entry);
        else
            m_pendingChatPopups.enqueue(popup);
    }
}

void CloudBackend::requestNotificationSettings()
{
    requestNotificationSettings(QStringLiteral("notification-settings"));
}

void CloudBackend::requestNotificationSettings(const QString &operation, const QString &updated, int expected,
                                               std::optional<quint64> started)
{
    const auto before = started.value_or(m_notificationSettingsRevision);
    const auto popupGeneration = m_chatPopupSettingsGeneration;
    const auto accountScope = m_accountScopeRevision;
    withCurrentUser(operation, [this, operation, updated, expected, before, popupGeneration, accountScope](qint64 userId) {
        if (accountScope != m_accountScopeRevision)
            return;
        ajax(QStringLiteral("im.v2.Settings.General.list"), {{QStringLiteral("userId"), QString::number(userId)}},
             operation, [this, operation, updated, expected, before, popupGeneration, accountScope](QJsonValue result) {
            if (accountScope != m_accountScopeRevision)
                return;
            auto data = result.toObject();
            for (auto entry = m_notificationSettingChanges.cbegin(); entry != m_notificationSettingChanges.cend(); ++entry)
                if (entry.value().first > before)
                    data.insert(entry.key(), entry.value().second);
            QVariantMap settings;
            if (const auto muted = notificationFlag(data.value(soundMuted)))
                settings.insert(soundMuted, *muted);
            for (const auto &name : {volumeGeneral, volumeMessages}) {
                bool ok = false;
                const auto value = data.value(name).toVariant().toInt(&ok);
                if (ok && value >= 0 && value <= 100)
                    settings.insert(name, value);
            }
            if (data.value(notifyScheme) == simpleNotifyScheme) {
                for (const auto &channel : simpleChannels)
                    if (const auto enabled = notificationFlag(data.value(channel)))
                        settings.insert(channel, *enabled);
            }
            m_notificationSettings = settings;
            if (popupGeneration == m_chatPopupSettingsGeneration)
                applyChatPopupSettings(data, ChatPopupSettingsSource::RefreshSnapshot);
            if (settings.isEmpty())
                return failShape(operation);
            if (!updated.isEmpty() && settings.contains(updated) && data.contains(updated))
                m_notificationSettingChanges.insert(updated, {++m_notificationSettingsRevision, data.value(updated)});
            emit notificationSettingsLoaded(settings, updated);
            if (!updated.isEmpty() && (!settings.contains(updated) || settings.value(updated).toInt() != expected)) {
                emit failed(operation, QStringLiteral("Портал не подтвердил изменение настройки"));
                return;
            }
        });
    });
}

void CloudBackend::updateNotificationSetting(const QString &name, int value)
{
    const auto operation = QStringLiteral("notification-settings-update");
    const bool toggle = name == soundMuted || simpleChannels.contains(name);
    if (!m_notificationSettings.contains(name) || (!toggle && name != volumeGeneral && name != volumeMessages))
        return failShape(operation);
    if (value < 0 || value > (toggle ? 1 : 100))
        return failShape(operation);
    withCurrentUser(operation, [this, name, value, toggle, operation, started = m_notificationSettingsRevision](qint64 userId) {
        const auto setting = toggle ? QJsonValue(value != 0) : QJsonValue(value);
        ajaxJson(QStringLiteral("im.v2.Settings.General.update"),
                 {{QStringLiteral("userId"), userId}, {QStringLiteral("name"), name}, {QStringLiteral("value"), setting},
                  {QStringLiteral("actionUuid"), QUuid::createUuid().toString(QUuid::WithoutBraces)}},
                 operation, [this, name, value, operation, started](QJsonValue response) {
            if (response == false)
                return failShape(operation);
            requestNotificationSettings(operation, name, value, started);
        });
    });
}

void CloudBackend::loadChatPopupSettings()
{
    const auto now = QDateTime::currentMSecsSinceEpoch();
    if (m_inflightOperations.contains(soundsOp) || now < m_nextChatPopupSettingsRequest)
        return;
    m_nextChatPopupSettingsRequest = now + 3000;
    const bool hadUser = !m_userId.isEmpty();
    const auto generation = m_chatPopupSettingsGeneration;
    withCurrentUser(soundsOp, [this, hadUser, generation](qint64 userId) {
        if (generation != m_chatPopupSettingsGeneration) {
            loadChatPopupSettings();
            return;
        }
        if (!hadUser) {
            if (!m_openDialog.isEmpty())
                m_chatDirty = true;
            if (!m_openTask.isEmpty())
                m_taskMessagesDirty = true;
            scheduleRefresh();
        }
        ajax(QStringLiteral("im.v2.Settings.General.list"), {{QStringLiteral("userId"), QString::number(userId)}},
             soundsOp, [this, generation](QJsonValue data) {
            if (generation != m_chatPopupSettingsGeneration) {
                loadChatPopupSettings();
                return;
            }
            applyChatPopupSettings(data.toObject(), ChatPopupSettingsSource::InitialSnapshot);
        });
    });
}

} // namespace bx
