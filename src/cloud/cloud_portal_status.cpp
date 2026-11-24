#include "cloud_backend.h"

#include "core/origin_guard.h"

namespace bx {
namespace {

const QString statusLoad = QStringLiteral("portal-status");
const QString statusChange = QStringLiteral("portal-status-update");

bool supportedStatus(const QString &status)
{
    return status == QLatin1String("online") || status == QLatin1String("away")
        || status == QLatin1String("break") || status == QLatin1String("dnd");
}

} // namespace

void CloudBackend::rememberPortalStatus(const QString &status, bool notifyUnchanged)
{
    if (!supportedStatus(status))
        return;
    ++m_portalStatusRevision;
    const bool changed = m_portalStatus != status;
    m_portalStatus = status;
    if (status == QLatin1String("dnd"))
        m_pendingChatPopups.clear();
    if (changed || notifyUnchanged)
        emit portalStatusLoaded(status);
    if (m_unboundPortalStatuses.isEmpty())
        settleInitialPortalStatus();
}

void CloudBackend::handlePortalStatusPush(const QJsonObject &users, qint64 stamp)
{
    if (!m_userId.isEmpty()) {
        const auto status = users.value(m_userId).toObject().value(QStringLiteral("status")).toString();
        if (supportedStatus(status) && (stamp <= 0 || stamp >= m_lastPortalStatusPushTime)) {
            m_lastPortalStatusPushTime = std::max(m_lastPortalStatusPushTime, stamp);
            rememberPortalStatus(status);
        }
        return;
    }
    if (!m_initialPortalStatusPending)
        return;
    constexpr qsizetype limit = 64;
    for (auto it = users.constBegin(); it != users.constEnd(); ++it) {
        const auto status = it.value().toObject().value(QStringLiteral("status")).toString();
        const auto id = it.key().toLongLong();
        if (id <= 0 || QString::number(id) != it.key() || !supportedStatus(status))
            continue;
        const auto previous = m_unboundPortalStatuses.constFind(it.key());
        if (previous != m_unboundPortalStatuses.cend() && previous->stamp > 0
            && (stamp <= 0 || stamp < previous->stamp))
            continue;
        if (previous == m_unboundPortalStatuses.cend() && m_unboundPortalStatuses.size() >= limit)
            m_unboundPortalStatuses.erase(m_unboundPortalStatuses.begin());
        m_unboundPortalStatuses.insert(it.key(), {status, stamp});
    }
}

void CloudBackend::bindPortalStatusIdentity(bool accountChanged)
{
    const auto pending = m_unboundPortalStatuses.value(m_userId);
    m_unboundPortalStatuses.clear();
    if (accountChanged) {
        m_portalStatus.clear();
        ++m_portalStatusRevision;
        m_lastPortalStatusPushTime = 0;
        m_pendingInitialAlerts.clear();
        m_pendingChatPopups.clear();
        m_initialPortalStatusPending = m_started;
        loadInitialPortalStatus();
    } else if (!pending.status.isEmpty()) {
        m_lastPortalStatusPushTime = pending.stamp;
        rememberPortalStatus(pending.status);
    }
}

void CloudBackend::loadInitialPortalStatus()
{
    if (!m_initialPortalStatusPending)
        return;
    const auto url = OriginGuard(m_session.origin).resolve(QStringLiteral("/rest/im.user.status.get.json"));
    const auto revision = m_portalStatusRevision;
    const auto user = m_userId;
    const auto operation = QStringLiteral("portal-status-bootstrap");
    webRequest(url, {}, operation, QStringLiteral("result"),
               [this, revision, user, operation](const QJsonValue &data) {
        if (!user.isEmpty() && user != m_userId) {
            loadInitialPortalStatus();
            return;
        }
        const auto finish = [this, revision, status = data.toString()](qint64) {
            if (m_initialPortalStatusPending && revision == m_portalStatusRevision)
                rememberPortalStatus(status);
            m_unboundPortalStatuses.clear();
            settleInitialPortalStatus();
        };
        if (m_initialPortalStatusPending && m_userId.isEmpty() && !m_unboundPortalStatuses.isEmpty())
            withCurrentUser(operation, finish);
        else
            finish(0);
    });
}

void CloudBackend::readPortalStatus(const QString &operation, std::function<void(const QString &, bool)> onStatus)
{
    const auto url = OriginGuard(m_session.origin).resolve(QStringLiteral("/rest/im.user.status.get.json"));
    const auto revision = m_portalStatusRevision;
    webRequest(url, {}, operation, QStringLiteral("result"),
               [this, operation, revision, onStatus = std::move(onStatus)](const QJsonValue &data) {
        const auto status = data.toString();
        if (!supportedStatus(status)) {
            emit failed(operation, QStringLiteral("Портал не подтвердил текущий статус"));
            return;
        }
        const bool fresh = revision == m_portalStatusRevision;
        if (fresh)
            rememberPortalStatus(status, true);
        onStatus(m_portalStatus, fresh);
    });
}

void CloudBackend::requestPortalStatus()
{
    readPortalStatus(statusLoad, [this](const QString &status, bool fresh) {
        if (!fresh)
            emit portalStatusLoaded(status);
    });
}

void CloudBackend::updatePortalStatus(const QString &expected, const QString &status)
{
    if (m_inflightOperations.contains(statusChange))
        return;
    if (!supportedStatus(expected) || !supportedStatus(status) || expected == status
        || (expected != QLatin1String("dnd") && status != QLatin1String("dnd"))) {
        emit failed(statusChange, QStringLiteral("Выберите новый статус для режима «Не беспокоить»"));
        return;
    }
    readPortalStatus(statusChange, [this, expected, status](const QString &current, bool fresh) {
        if (!fresh || current != expected) {
            emit failed(statusChange, QStringLiteral("Статус на портале изменился. Проверьте его и повторите действие"));
            return;
        }
        const auto url = OriginGuard(m_session.origin).resolve(QStringLiteral("/rest/im.user.status.set.json"));
        webRequest(url, {{QStringLiteral("STATUS"), status}}, statusChange, QStringLiteral("result"),
                   [this, status](const QJsonValue &data) {
            if (data != QJsonValue(true)) {
                emit failed(statusChange, QStringLiteral("Портал отклонил смену статуса"));
                return;
            }
            readPortalStatus(statusChange, [this, status](const QString &confirmed, bool) {
                if (confirmed == status)
                    emit portalStatusUpdated(confirmed);
                else
                    emit failed(statusChange, QStringLiteral("Портал не подтвердил новый статус"));
            });
        });
    });
}

} // namespace bx
