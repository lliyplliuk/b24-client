#include "cloud/pull_channel.h"

#include "cloud/cloud_rows.h"
#include "core/origin_guard.h"
#include "core/tunables.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QRandomGenerator>
#include <QUrlQuery>
#include <algorithm>

namespace bx {

using cloud_rows::field;
using cloud_rows::numericId;

namespace {

const QString pushOp = operationKey(OperationKind::Push);
constexpr int maxSeenEvents = 256;
constexpr int heartbeatIntervalMs = 60000;
constexpr int handshakeTimeoutMs = 15000;

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

} // namespace

PullChannel::PullChannel(PullHost &host, PortalHttp &http, QObject *parent)
    : QObject(parent), m_host(host), m_portalHttp(http),
      m_socket(OriginGuard(host.portalOrigin()).resolve(QStringLiteral("/")).toString(QUrl::RemovePath))
{
    m_socket.setProxy(QNetworkProxy::NoProxy);
    m_socket.setMaxAllowedIncomingFrameSize(PortalHttp::maxResponse);
    m_socket.setMaxAllowedIncomingMessageSize(PortalHttp::maxResponse);
    for (auto timer : {&m_retry, &m_watchdog, &m_heartbeat, &m_configExpiry})
        timer->setSingleShot(true);

    connect(&m_retry, &QTimer::timeout, this, &PullChannel::connectPull);
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
    connect(&m_socket, &QWebSocket::connected, this, [this] {
        if (!m_host.started())
            return;
        m_attempt = 0;
        m_watchdog.stop();
        m_heartbeat.start(heartbeatIntervalMs);
        emit connected();
    });
    connect(&m_socket, &QWebSocket::disconnected, this, [this] {
        scheduleReconnect(m_socket.closeCode() == static_cast<QWebSocketProtocol::CloseCode>(4010));
    });
    connect(&m_socket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, [this](QAbstractSocket::SocketError) { scheduleReconnect(); });
    connect(&m_socket, &QWebSocket::textMessageReceived, this, &PullChannel::receivePull);
}

void PullChannel::reset()
{
    m_seenEvents.clear();
    m_seenEventIds.clear();
    m_mid.clear();
    m_tag.clear();
    m_time.clear();
}

void PullChannel::resetBackoff()
{
    m_attempt = 0;
}

void PullChannel::cancelRetry()
{
    m_retry.stop();
}

void PullChannel::stop()
{
    m_retry.stop();
    m_watchdog.stop();
    m_heartbeat.stop();
    m_configExpiry.stop();
    m_socket.abort();
}

void PullChannel::connectPull()
{
    if (!m_host.started() || m_host.authExpired() || m_portalHttp.contains(pushOp))
        return;
    auto url = OriginGuard(m_host.portalOrigin()).resolve(QStringLiteral("/rest/pull.config.get.json"));
    QUrlQuery configQuery;
    configQuery.addQueryItem(QStringLiteral("logTag"), QStringLiteral("client_start"));
    url.setQuery(configQuery);
    m_portalHttp.webRequest(url, {{QStringLiteral("CACHE"), QStringLiteral("N")}}, pushOp, QStringLiteral("result"),
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

        if (query.isEmpty() || !trustedSocket(socketUrl, m_host.portalOrigin(), m_host.allowLoopback())) {
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

void PullChannel::scheduleReconnect(bool immediately)
{
    m_watchdog.stop();
    m_heartbeat.stop();
    m_configExpiry.stop();
    emit offline();
    if (!m_host.started() || m_host.authExpired()) {
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

void PullChannel::receivePull(const QString &frame)
{
    if (!m_host.started())
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

void PullChannel::handlePullEvent(const QJsonObject &event)
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
        handleTaskPull(command, params);
        return;
    }
    if (module == QLatin1String("online") && command == QLatin1String("userStatus")) {
        const auto age = recentExtra.value(QStringLiteral("server_time_ago")).toInteger();
        if (age >= 0 && age < 240)
            emit presenceEvent(params, field(recentExtra, {"server_time_unix"}).toLongLong());
        return;
    }
    if (module == QLatin1String("im"))
        emit imEvent(command, params, recentExtra, mid);
}

void PullChannel::handleTaskPull(const QString &command, const QJsonObject &params)
{
    const auto fields = params.value(QStringLiteral("fields")).toObject();
    auto taskId = numericId(field(params, {"taskId", "TASK_ID", "id", "ID"}));
    if (taskId.isEmpty())
        taskId = numericId(field(params.value(QStringLiteral("task")).toObject(), {"id", "ID"}));
    if (taskId.isEmpty())
        taskId = numericId(field(fields, {"id", "ID"}));
    auto groupId = field(params, {"groupId", "GROUP_ID"});
    if (groupId.isEmpty())
        groupId = field(fields, {"GROUP_ID", "groupId"});
    const bool invalidatePreview = !taskId.isEmpty() && (!groupId.isEmpty() || command == QLatin1String("task_update")
                                                           || command == QLatin1String("taskUpdate"));
    emit taskEvent(fields, groupId, taskId, invalidatePreview);
}

} // namespace bx
