#pragma once

#include "core/session.h"

#include <QByteArray>
#include <QList>
#include <QNetworkCookie>
#include <QNetworkRequest>
#include <QUrl>

class QNetworkReply;

namespace bx::signin {

QUrl portalOrigin(const QUrl &url);
bool mayNavigate(const QUrl &portal, const QUrl &destination);

class CookieCapture {
public:
    explicit CookieCapture(QUrl portal);
    void add(const QNetworkCookie &cookie);
    void remove(const QNetworkCookie &cookie);
    QByteArray header() const;

private:
    QUrl m_portal;
    QList<QNetworkCookie> m_cookies;
};

bool validCsrf(const QByteArray &csrf);
Session updatedSession(Session session, const QNetworkReply &reply);
QNetworkRequest authorizationProbe(const QUrl &origin, const QByteArray &cookie, const QByteArray &csrf);
QByteArray authorizationProbeBody(const QByteArray &csrf);
bool authenticatedResponse(int status, const QByteArray &body);
QByteArray sessionLine(const QUrl &portal, const QByteArray &cookie, const QByteArray &csrf);

} // namespace bx::signin
