#pragma once

#include <QApplication>
#include <QFont>

namespace bx::ui {

inline constexpr int bodyTextPixels = 16;
inline constexpr int secondaryTextPixels = 14;

inline void installApplicationTypography(QApplication &app)
{
    auto font = app.font();
    font.setPixelSize(bodyTextPixels);
    app.setFont(font);
}

} // namespace bx::ui
