#pragma once

#include "portal_http.h"

namespace bx {

class CloudServiceContext : public virtual SessionStateHost {
public:
    virtual QUrl portalOrigin() const = 0;
    virtual void withCurrentUser(const QString &operation, std::function<void(qint64)> onUser) = 0;
    virtual void failShape(const QString &operation) = 0;
    virtual QUrl avatarUrlFromPortal(const QString &value) const = 0;
};

} // namespace bx
