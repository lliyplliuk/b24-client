#pragma once

#include <QApplication>
#include <QFont>
#include <QString>

namespace bx::ui {

inline constexpr int bodyTextPixels = 16;
inline constexpr int secondaryTextPixels = 14;

inline QString plainTooltip(const QString &text)
{
    return QStringLiteral("<qt>%1</qt>").arg(text.toHtmlEscaped());
}

inline void installApplicationTypography(QApplication &app)
{
    auto font = app.font();
    font.setPixelSize(bodyTextPixels);
    app.setFont(font);
}

} // namespace bx::ui
