#pragma once

#include <QString>
#include <QStringView>

namespace bx {

inline QString normalizedStageColor(QStringView value)
{
    auto color = value.trimmed();
    if (color.startsWith(u'#'))
        color = color.mid(1);
    if (color.size() != 6)
        return {};
    for (const auto ch : color) {
        const auto c = ch.unicode();
        if (!(c >= u'0' && c <= u'9') && !(c >= u'A' && c <= u'F') && !(c >= u'a' && c <= u'f'))
            return {};
    }
    return color.toString().toLower();
}

} // namespace bx
