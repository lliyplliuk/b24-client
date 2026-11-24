#pragma once

#include <QString>
#include <QUrl>

namespace bx {

// Origin confinement rules.
//
// A session bound to portal P may only talk to P. Redirects are inspected
// manually (QNetworkRequest::ManualRedirectPolicy) so that a portal answer
// cannot silently move the client to a foreign origin. Plain HTTP is allowed
// only for loopback fixtures used by the automated tests.
class OriginGuard {
public:
    explicit OriginGuard(QUrl portalOrigin) : portal_(std::move(portalOrigin)) {}

    const QUrl &portal() const { return portal_; }

    // True when `url` has the same scheme+host+port as the portal origin.
    bool isSameOrigin(const QUrl &url) const;

    // True when `url` may be requested: same origin, or loopback for fixtures.
    bool isAllowed(const QUrl &url, bool allowLoopback = false) const;

    // Reason why a URL was rejected (empty when allowed).
    QString rejectionReason(const QUrl &url, bool allowLoopback = false) const;

    // Builds an absolute URL from the portal origin + path.
    QUrl resolve(const QString &path) const;

    static bool isLoopback(const QUrl &url);

private:
    QUrl portal_;
};

} // namespace bx
