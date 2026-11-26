#include "ui/emoji_tile.h"

#include "ui/workspace_style.h"

namespace bx::ui {

EmojiTile::EmojiTile(const QString &glyph, const QString &name, QWidget *parent)
    : QPushButton(parent), m_glyph(glyph)
{
    setAccessibleName(name);
    setToolTip(name);
    setAutoDefault(false);
    setFixedSize(43, 42);
}

void EmojiTile::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (underMouse() || hasFocus() || isDown()) {
        painter.setPen(hasFocus() ? QPen(workspace::Accent, 1.1) : Qt::NoPen);
        painter.setBrush(QColor("#e8f2ed"));
        painter.drawRoundedRect(rect().adjusted(2, 2, -2, -2), 7, 7);
    }
    auto font = workspace::rowFont(28);
    font.setFamilies({QStringLiteral("Noto Color Emoji"), QStringLiteral("Segoe UI Emoji"),
                      QStringLiteral("Apple Color Emoji"), font.family()});
    painter.setFont(font);
    painter.setPen(workspace::Ink);
    painter.drawText(rect(), Qt::AlignCenter, m_glyph);
}

} // namespace bx::ui
