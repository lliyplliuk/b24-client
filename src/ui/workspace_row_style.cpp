#include "ui/workspace_row_style.h"

#include "ui/message_emoji.h"
#include "ui/reaction_catalog.h"

#include <QApplication>

namespace bx::workspace {

QString reactionGlyph(const QString &code)
{
    const auto *reaction = ui::findReaction(code);
    return reaction ? QString::fromUtf8(reaction->emoji) : QStringLiteral("•");
}

QFont reactionFont(int pixels)
{
    static const auto preferred = [] {
        auto font = QApplication::font();
        font.setFamilies(emojiFontFamilies() + QStringList{font.family()});
        return font;
    }();
    auto font = preferred;
    font.setPixelSize(pixels);
    return font;
}

QFont rowFont(int pixels, bool bold)
{
    auto result = QApplication::font();
    result.setPixelSize(pixels);
    result.setWeight(bold ? QFont::DemiBold : QFont::Normal);
    return result;
}

} // namespace bx::workspace
