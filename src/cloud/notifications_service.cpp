#include "cloud/notifications_service.h"
#include "cloud/cloud_chat_metadata.h"
#include "cloud/cloud_rows.h"
#include "core/notification_targets.h"
#include "core/origin_guard.h"

#include <QSet>
#include <algorithm>
#include <limits>
#include <utility>

namespace bx {
using namespace cloud_rows;

const QString notificationsOp = operationKey(OperationKind::Notifications);
const QString olderNotificationsOp = operationKey(OperationKind::NotificationsOlder);
constexpr int maxNotifications = 100;
constexpr int notificationPageSize = 50;

static qsizetype notificationHistoryLimit(int olderPages)
{
    constexpr auto maxPages = (std::numeric_limits<qsizetype>::max() - maxNotifications) / notificationPageSize;
    return maxNotifications + qMin<qsizetype>(olderPages, maxPages) * notificationPageSize;
}

void NotificationsService::trimNotificationHistory(QVector<Notification> &notices, int olderPages)
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

static QString notificationRequestKey(quint64 scopeRevision, const QString &key)
{
    return operationKey(OperationKind::NotificationBatch, QString::number(scopeRevision), key);
}

QString NotificationsService::noticeAuthorId(const QJsonObject &notice, const QJsonObject &outer)
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

QString NotificationsService::noticeAuthorName(const QString &id, const QJsonValue &users)
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

NotificationsService::NotificationsService(NotificationsHost &host, const ChatTimeline &timeline, PortalHttp &http, QObject *parent)
    : QObject(parent), m_host(host), m_timeline(timeline), m_portalHttp(http)
{
}

QString NotificationsService::historyRequestKey() const
{
    return notificationRequestKey(m_host.accountScopeRevision(), notificationsOp);
}

void NotificationsService::reset()
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
    m_pendingInitialAlerts.clear();
    m_pendingChatPopups.clear();
    m_seenChatPopups.clear();
    m_chatPopupHistory = {};
    m_taskPopupHistory = {};
    if (pagingVisible)
        publishNotificationHistoryPageState();
}

void NotificationsService::requestNotifications()
{
    QUrlQuery parameters;
    parameters.addQueryItem(QStringLiteral("LIMIT"), QString::number(notificationPageSize));
    parameters.addQueryItem(QStringLiteral("CONVERT_TEXT"), QStringLiteral("Y"));
    notificationBatch(QStringLiteral("im.notify.get"), parameters, notificationsOp,
                      [this, revision = m_notificationRevision](QJsonValue result) {
        const auto response = result.toObject();
        const auto rows = response.value(QStringLiteral("notifications"));
        if (!rows.isArray())
            return m_host.failShape(notificationsOp);

        const auto page = rows.toArray();
        if (m_notificationOlderPages == 0 && !m_loadingOlderNotifications)
            m_notificationOlderCursor = notificationCursor(page);
        publishNotificationPage(decodeNotifications(page, response.value(QStringLiteral("users")), revision));
    });
}

void NotificationsService::requestOlderNotifications()
{
    const auto cursor = m_notificationOlderCursor;
    if (!m_host.started() || m_loadingOlderNotifications || cursor.id <= 0)
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
            return m_host.failShape(olderNotificationsOp);
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

void NotificationsService::searchNotifications(const QString &query)
{
    const auto operation = operationKey(OperationKind::SearchNotifications, query);
    if (query.size() < 3 || query.size() > 200)
        return m_host.failShape(operation);
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
            return m_host.failShape(operation);
        m_notificationSearchRows = decodeNotifications(rows.toArray(), response.value(QStringLiteral("users")), revision);
        emit notificationsFound(query, m_notificationSearchRows);
    });
}

void NotificationsService::markNotificationRead(const QString &id)
{
    const auto operation = operationKey(OperationKind::ReadNotification);
    if (id.isEmpty() || numericId(id) != id)
        return m_host.failShape(operation);
    QUrlQuery parameters;
    parameters.addQueryItem(QStringLiteral("ID"), id);
    parameters.addQueryItem(QStringLiteral("ACTION"), QStringLiteral("Y"));
    parameters.addQueryItem(QStringLiteral("ONLY_CURRENT"), QStringLiteral("Y"));
    notificationBatch(QStringLiteral("im.notify.read"), parameters, operation,
                      [this, id, operation, started = m_notificationRevision](QJsonValue result) {
        if (result != true && result.toObject().value(QStringLiteral("result")) != true)
            return m_host.failShape(operation);
        updateNotificationReadState({id}, true, started);
        emit notificationActionFinished(operation);
    });
}

void NotificationsService::markNotificationsRead(const QStringList &ids)
{
    if (ids.isEmpty())
        return;
    const auto operation = operationKey(OperationKind::ReadNotifications);
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
        return m_host.failShape(operation);
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
            return m_host.failShape(operation);
        updateNotificationReadState(selected, true, started);
        emit notificationActionFinished(operation);
    }, requestKey);
}

void NotificationsService::markAllNotificationsRead()
{
    const auto operation = operationKey(OperationKind::ReadAllNotifications);
    QStringList snapshot;
    for (const auto *source : {&m_notifications, &m_notificationSearchRows})
        for (const auto &item : *source)
            if (!item.read && !snapshot.contains(item.id))
                snapshot.append(item.id);
    notificationBatch(QStringLiteral("im.notify.read.all"), {}, operation,
                      [this, operation, snapshot, started = m_notificationRevision](QJsonValue result) {
        if (result.toObject().value(QStringLiteral("result")) != true)
            return m_host.failShape(operation);
        updateNotificationReadState(snapshot, true, started);
        emit notificationActionFinished(operation);
    });
}

void NotificationsService::deleteNotification(const QString &id)
{
    const auto operation = operationKey(OperationKind::DeleteNotification);
    if (id.isEmpty() || numericId(id) != id)
        return m_host.failShape(operation);
    QUrlQuery parameters;
    parameters.addQueryItem(QStringLiteral("ID"), id);
    notificationBatch(QStringLiteral("im.notify.delete"), parameters, operation, [this, id, operation](QJsonValue result) {
        if (result != true)
            return m_host.failShape(operation);
        removeNotification(id);
        emit notificationActionFinished(operation);
    });
}

void NotificationsService::handleNotificationStatePull(const QString &command, const QJsonObject &params)
{
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
}

void NotificationsService::notificationBatch(const QString &method, const QUrlQuery &parameters,
                                     const QString &operation, std::function<void(QJsonValue)> onResult,
                                     const QString &requestKey)
{
    auto command = method;
    if (!parameters.isEmpty())
        command += QLatin1Char('?') + parameters.toString(QUrl::FullyEncoded);
    const auto url = OriginGuard(m_host.portalOrigin()).resolve(QStringLiteral("/rest/batch.json"));
    m_portalHttp.webRequest(url, {{QStringLiteral("halt"), QStringLiteral("0")},
                     {QStringLiteral("cmd[notify]"), command}},
               operation, QStringLiteral("result"),
               [this, operation, onResult = std::move(onResult)](QJsonValue batch) {
        const auto result = batch.toObject().value(QStringLiteral("result")).toObject().value(QStringLiteral("notify"));
        if (result.isUndefined() || result.isNull())
            return m_host.failShape(operation);
        onResult(result);
    }, notificationRequestKey(m_host.accountScopeRevision(), requestKey.isEmpty() ? operation : requestKey));
}

QVector<Notification> NotificationsService::decodeNotifications(const QJsonArray &rows, const QJsonValue &users,
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
        decodeNotificationContent(notice, m_host.portalOrigin(), field(item, {"text"}),
                                  item.value(QStringLiteral("params")).toObject(),
                                  authorId, noticeAuthorName(authorId, users));
        items.append(std::move(notice));
    }
    return items;
}

NotificationsService::NotificationCursor NotificationsService::notificationCursor(const QJsonArray &rows)
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

void NotificationsService::publishNotificationHistoryPageState()
{
    const bool more = m_notificationOlderCursor.id > 0;
    emit notificationHistoryPageStateChanged(more, m_loadingOlderNotifications);
}

void NotificationsService::publishNotificationPage(QVector<Notification> items)
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

void NotificationsService::updateNotificationReadState(const QStringList &ids, bool read, std::optional<quint64> started)
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

void NotificationsService::removeNotification(const QString &id)
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

void NotificationsService::handleOperationFailure(const QString &operation)
{
    if (operation == olderNotificationsOp && m_loadingOlderNotifications) {
        m_loadingOlderNotifications = false;
        publishNotificationHistoryPageState();
    }
}

} // namespace bx
