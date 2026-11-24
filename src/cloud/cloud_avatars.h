#pragma once

#include <QByteArrayView>

namespace bx::cloud {

inline bool isRasterImageMime(QByteArrayView mime)
{
    return mime == "image/png" || mime == "image/jpeg" || mime == "image/webp" || mime == "image/gif";
}

} // namespace bx::cloud
