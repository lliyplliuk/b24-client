#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>

namespace bx {

// A portal session obtained after the user signs in with the temporary
// browser helper, or supplied on a private pipe by a diagnostic caller.
// The desktop client keeps this data in memory and confines it to the portal.
struct Session {
    QUrl origin;          // e.g. https://acme.bitrix24.ru
    QByteArray cookie;    // raw Cookie header value, in memory only
    QByteArray csrf;      // CSRF token, in memory only

    bool isValid() const {
        return origin.isValid() && !origin.host().isEmpty() && !cookie.isEmpty();
    }

    // Redacted description, safe for logs and error reports.
    QString safeDescription() const {
        return QStringLiteral("portal=%1 cookie=[hidden] csrf=%2")
            .arg(origin.host(), csrf.isEmpty() ? QStringLiteral("absent")
                                               : QStringLiteral("present"));
    }
};

} // namespace bx
