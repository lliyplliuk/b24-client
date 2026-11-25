#pragma once

#include <QByteArrayView>
#include <QString>
#include <QUrl>

namespace bx::cloud {

inline bool isRasterImageMime(QByteArrayView mime)
{
    return mime == "image/png" || mime == "image/jpeg" || mime == "image/webp" || mime == "image/gif";
}

bool isPortalDiskDownload(const QUrl &url, const QString &attachmentId, const QString &objectId);

} // namespace bx::cloud
