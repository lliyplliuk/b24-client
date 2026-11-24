#include "signin/session_restore.h"

#include "core/origin_guard.h"
#include "signin/session_capture.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QUrlQuery>

namespace bx::signin {
namespace {
constexpr int probeDeadlineMs = 5000;
constexpr qsizetype responseLimit = 512 * 1024;
}

RestoreStatus restoreStatus(int status, const QByteArray &body, const QUrl &portal, const QUrl &redirect)
{
    if (authenticatedResponse(status, body))
        return RestoreStatus::Authenticated;
    const auto response = status == 200 || status == 403
        ? QJsonDocument::fromJson(body).object() : QJsonObject();
    return isAuthorizationRejection(status, response, portal, redirect)
        ? RestoreStatus::Rejected : RestoreStatus::Unavailable;
}

bool isAuthorizationRejection(int status, const QJsonObject &response, const QUrl &portal, const QUrl &redirect)
{
    if (status == 401)
        return true;
    if (status == 200 || status == 403) {
        if (response.value(QStringLiteral("status")) == QLatin1String("error")) {
            const auto errors = response.value(QStringLiteral("errors")).toArray();
            for (const auto &error : errors) {
                const auto code = error.toObject().value(QStringLiteral("code")).toString();
                if (code == QLatin1String("invalid_authentication") || code == QLatin1String("invalid_csrf"))
                    return true;
            }
        }
    }

    if (status >= 300 && status < 400 && !redirect.isEmpty()) {
        const auto destination = portal.resolved(redirect);
        if (mayNavigate(portal, destination)) {
            const auto path = destination.path();
            const auto query = QUrlQuery(destination);
            if (!OriginGuard(portal).isSameOrigin(destination)
                || path == QLatin1String("/auth") || path.startsWith(QLatin1String("/auth/"))
                || path == QLatin1String("/login") || path.startsWith(QLatin1String("/login/"))
                || query.queryItemValue(QStringLiteral("login")) == QLatin1String("yes")) {
                return true;
            }
        }
    }
    return false;
}

Session csrfRetrySession(const Session &current, const QNetworkReply &reply, const QJsonObject &response)
{
    const auto status = reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if ((status != 200 && status != 403) || response.value(QStringLiteral("status")) != QLatin1String("error")
        || !OriginGuard(current.origin).isSameOrigin(reply.url()))
        return {};
    QByteArray replacement;
    for (const auto &entry : response.value(QStringLiteral("errors")).toArray()) {
        const auto error = entry.toObject();
        const auto code = error.value(QStringLiteral("code")).toString();
        if (code == QLatin1String("invalid_authentication"))
            return {};
        if (code != QLatin1String("invalid_csrf"))
            continue;
        const auto csrf = error.value(QStringLiteral("customData")).toObject()
            .value(QStringLiteral("csrf")).toString().toLatin1();
        if (!validCsrf(csrf) || csrf == current.csrf || (!replacement.isEmpty() && csrf != replacement))
            return {};
        replacement = csrf;
    }
    if (replacement.isEmpty())
        return {};
    // The replacement token can belong to cookies rotated in this same response.
    auto retry = updatedSession(current, reply);
    retry.csrf = std::move(replacement);
    return retry;
}

SessionRestore::SessionRestore(QObject *parent) : QObject(parent)
{
    m_network.setProxy(QNetworkProxy::NoProxy);
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        if (m_reply) {
            m_aborted = true;
            m_reply->abort();
        }
    });
}

SessionRestore::~SessionRestore()
{
    cancel();
}

void SessionRestore::verify(Session session, bool allowLoopback)
{
    cancel();
    const auto &origin = session.origin;
    const bool loopback = allowLoopback && OriginGuard::isLoopback(origin)
        && origin.scheme() == QLatin1String("http") && origin.userInfo().isEmpty()
        && origin.path().isEmpty() && !origin.hasQuery() && !origin.hasFragment() && origin.port() > 0;
    if (origin.isEmpty() || (portalOrigin(origin) != origin && !loopback)) {
        emit verified(RestoreStatus::Unavailable, {});
        return;
    }
    m_session = std::move(session);
    probe();
}

void SessionRestore::probe()
{
    const auto request = authorizationProbe(m_session.origin, m_session.cookie, m_session.csrf);
    m_reply = m_network.post(request, authorizationProbeBody(m_session.csrf));
    auto *reply = m_reply.data();
    reply->setReadBufferSize(responseLimit + 1);
    connect(reply, &QIODevice::readyRead, this, &SessionRestore::drain);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        // Read any final bytes without calling abort from inside the finished handler.
        if (reply->isOpen())
            m_body += reply->read(responseLimit + 1 - m_body.size());
        m_deadline.stop();
        const auto httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        const bool incomplete = m_aborted || m_body.size() > responseLimit
            || (httpStatus == 200 && reply->error() != QNetworkReply::NoError);
        auto status = incomplete ? RestoreStatus::Unavailable
            : restoreStatus(httpStatus, m_body, m_session.origin, redirect);
        const Session verifiedSession = status == RestoreStatus::Authenticated
            ? updatedSession(m_session, *reply) : Session{};
        if (status == RestoreStatus::Authenticated && !verifiedSession.isValid())
            status = RestoreStatus::Rejected;
        auto retry = status == RestoreStatus::Rejected && !m_retriedCsrf
            ? csrfRetrySession(m_session, *reply, QJsonDocument::fromJson(m_body).object()) : Session{};
        m_reply.clear();
        reply->deleteLater();
        if (retry.isValid()) {
            m_retriedCsrf = true;
            m_session = std::move(retry);
            m_body.clear();
            probe();
            return;
        }
        m_session = {};
        m_body.clear();
        emit verified(status, verifiedSession);
    });
    m_deadline.start(probeDeadlineMs);
}

void SessionRestore::cancel()
{
    m_deadline.stop();
    if (auto *reply = m_reply.data()) {
        disconnect(reply, nullptr, this, nullptr);
        m_reply.clear();
        reply->abort();
        reply->deleteLater();
    }
    m_session = {};
    m_body.clear();
    m_aborted = false;
    m_retriedCsrf = false;
}

void SessionRestore::drain()
{
    m_body += m_reply->read(responseLimit + 1 - m_body.size());
    if (m_body.size() > responseLimit) {
        m_aborted = true;
        m_reply->abort();
    }
}

} // namespace bx::signin
