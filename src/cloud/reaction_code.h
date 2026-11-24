#pragma once

#include <QStringView>

namespace bx {

inline bool isReactionCode(QStringView code)
{
    if (code.isEmpty() || code.size() > 48 || code.front() < QLatin1Char('a') || code.front() > QLatin1Char('z'))
        return false;

    for (const auto letter : code) {
        if ((letter < QLatin1Char('a') || letter > QLatin1Char('z'))
            && (letter < QLatin1Char('A') || letter > QLatin1Char('Z')))
            return false;
    }
    return true;
}

} // namespace bx
