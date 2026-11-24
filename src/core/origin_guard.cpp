#include "core/origin_guard.h"

namespace bx {

namespace {
int effectivePort(const QUrl &url) {
    if (url.port() > 0)
        return url.port();
    return url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0 ? 443 : 80;
}
} // namespace

bool OriginGuard::isLoopback(const QUrl &url) {
    const QString host = url.host();
    return host == QStringLiteral("127.0.0.1") || host == QStringLiteral("::1")
           || host == QStringLiteral("localhost");
}

bool OriginGuard::isSameOrigin(const QUrl &url) const {
    if (!portal_.isValid() || !url.isValid())
        return false;
    return url.scheme().compare(portal_.scheme(), Qt::CaseInsensitive) == 0
           && url.host().compare(portal_.host(), Qt::CaseInsensitive) == 0
           && effectivePort(url) == effectivePort(portal_);
}

QString OriginGuard::rejectionReason(const QUrl &url, bool allowLoopback) const {
    if (!url.isValid())
        return QStringLiteral("Некорректный URL");
    if (isSameOrigin(url))
        return QString();

    if (allowLoopback && isLoopback(url)) {
        const QString scheme = url.scheme().toLower();
        if (scheme != QLatin1String("http") && scheme != QLatin1String("https")
            && scheme != QLatin1String("ws") && scheme != QLatin1String("wss")) {
            return QStringLiteral("Недопустимая схема для loopback: %1").arg(url.scheme());
        }
        return QString();
    }

    if (!isLoopback(url) && url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0)
        return QStringLiteral("Требуется HTTPS для внешнего портала");

    return QStringLiteral("Переход за пределы origin портала запрещён: %1").arg(url.host());
}

bool OriginGuard::isAllowed(const QUrl &url, bool allowLoopback) const {
    return rejectionReason(url, allowLoopback).isEmpty();
}

QUrl OriginGuard::resolve(const QString &path) const {
    QUrl url = portal_;
    QString p = path;
    if (!p.startsWith(QStringLiteral("/")))
        p.prepend(QStringLiteral("/"));
    url.setPath(p);
    url.setQuery(QString());
    url.setFragment(QString());
    return url;
}

} // namespace bx
