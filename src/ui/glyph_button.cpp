#include "ui/glyph_button.h"

#include "ui/workspace_style.h"

namespace bx::ui {

GlyphButton::GlyphButton(Glyph glyph, const QString &name, QWidget *parent, bool primary)
    : QPushButton(parent), m_glyph(glyph), m_primary(primary)
{
    setAccessibleName(name);
    setToolTip(name);
    setAutoDefault(false);
    setFixedSize(primary ? 40 : 36, 36);
}

void GlyphButton::setGlyph(Glyph glyph)
{
    m_glyph = glyph;
    update();
}

void GlyphButton::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool active = isEnabled() && (isChecked() || isDown() || underMouse());
    const auto surface = m_primary
        ? (isEnabled() ? (active ? QColor("#174658") : workspace::Accent) : QColor("#e6efeb"))
        : (active ? QColor("#e2efe9") : QColor(Qt::transparent));
    painter.setPen(hasFocus() ? QPen(workspace::Accent, 1.4) : Qt::NoPen);
    painter.setBrush(surface);
    painter.drawRoundedRect(rect().adjusted(2, 2, -2, -2), 8, 8);
    const auto color = isEnabled()
        ? (m_primary ? QColor(Qt::white) : workspace::Accent) : QColor("#687e84");
    paintGlyph(painter, m_glyph, QRectF((width() - 19) / 2.0, (height() - 19) / 2.0, 19, 19), color);
}

} // namespace bx::ui
