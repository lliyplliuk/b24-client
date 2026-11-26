#pragma once

#include <QJsonValue>

namespace bx::badge_data {

inline constexpr qsizetype maxBadgeEntries = 4096;

inline qint64 positiveId(const QJsonValue &value)
{
    bool ok = false;
    const auto id = value.isString() ? value.toString().toLongLong(&ok) : value.toInteger();
    return id > 0 && (!value.isString() || ok) ? id : 0;
}

} // namespace bx::badge_data
