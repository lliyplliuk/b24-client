#pragma once

#include <QFileInfo>

namespace bx {

inline bool safeDownloadTarget(const QString &path)
{
    const QFileInfo target(path);
    return !target.isSymLink() && (!target.exists() || target.isFile());
}

} // namespace bx
