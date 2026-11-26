#include "ui/row_paint_primitives.h"
#include "ui/message_emoji.h"
#include "ui/typography.h"
#include <QTextLayout>
#include <QtMath>

namespace bx::workspace {

int wrappedText(QString text, int width, const QFont &font,
                QPainter *painter, const QPoint &position)
{
    text.replace(QLatin1Char('\n'), QChar::LineSeparator);
    QTextLayout layout(text, font);
    layout.setFormats(messageEmojiFormats(text));
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.beginLayout();
    qreal y = 0;
    for (auto line = layout.createLine(); line.isValid(); line = layout.createLine()) {
        line.setLineWidth(qMax(1, width));
        line.setPosition(QPointF(0, y));
        y += line.height();
    }
    layout.endLayout();
    if (painter)
        layout.draw(painter, position);
    return qMax(QFontMetrics(font).height(), qCeil(y));
}

void fill(QPainter &p, const QRect &rect, const QColor &background,
          const QColor &outline, int radius)
{
    p.setPen(outline == Qt::transparent ? QPen(Qt::NoPen) : QPen(outline));
    p.setBrush(background);
    p.drawRoundedRect(rect, radius, radius);
}

void line(QPainter &p, const QRect &rect, const QString &text, int pixels,
          const QColor &color, bool bold,
          Qt::Alignment alignment)
{
    p.setFont(rowFont(pixels, bold));
    p.setPen(color);
    const QFontMetrics metrics(p.font());
    p.drawText(rect, int(alignment) | Qt::TextSingleLine, metrics.elidedText(text, Qt::ElideRight, rect.width()));
}

void paragraph(QPainter &p, const QRect &rect, const QString &text, int pixels,
               const QColor &color, bool bold)
{
    p.save();
    p.setClipRect(rect, Qt::IntersectClip);
    p.setPen(color);
    wrappedText(text, rect.width(), rowFont(pixels, bold), &p, rect.topLeft());
    p.restore();
}

void messagePreviewLine(QPainter &painter, const QRect &rect, const QString &text, const QColor &color)
{
    const auto formats = messageEmojiFormats(text);
    if (formats.isEmpty()) {
        line(painter, rect, text, ui::secondaryTextPixels, color);
        return;
    }

    const auto font = rowFont(ui::secondaryTextPixels);
    auto measuring = font;
    measuring.setFamilies(font.families() + emojiFontFamilies());
    const auto visible = QFontMetrics(measuring).elidedText(text, Qt::ElideRight, rect.width());
    QTextLayout layout(visible, font);
    layout.setFormats(visible == text ? formats : messageEmojiFormats(visible));
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    layout.setTextOption(option);
    layout.beginLayout();
    const auto row = layout.createLine();
    layout.endLayout();
    if (!row.isValid())
        return;

    painter.save();
    painter.setClipRect(rect, Qt::IntersectClip);
    painter.setPen(color);
    layout.draw(&painter, QPointF(rect.left(), rect.top() + (rect.height() - row.height()) / 2));
    painter.restore();
}
QColor itemOutline(const QStyleOptionViewItem &option, const QColor &normal)
{
    return option.state & QStyle::State_HasFocus ? Accent : normal;
}

void paintBadge(QPainter &p, const QRect &rect, const QString &text, int pixels,
                const QColor &background, const QColor &ink, int radius)
{
    fill(p, rect, background, Qt::transparent, radius);
    line(p, rect, text, pixels, ink, true, Qt::AlignCenter);
}

void paintRowTime(QPainter &p, const QRect &rect, const QString &text, const QColor &ink)
{
    line(p, rect, text, ui::secondaryTextPixels, ink, false, Qt::AlignRight | Qt::AlignVCenter);
}

} // namespace bx::workspace
