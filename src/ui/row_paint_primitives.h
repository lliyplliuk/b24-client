#pragma once

#include "ui/workspace_row_style.h"
#include <QStyleOptionViewItem>
#include <QPainter>

namespace bx::workspace {

int wrappedText(QString text, int width, const QFont &font,
                QPainter *painter = nullptr, const QPoint &position = {});
void fill(QPainter &p, const QRect &rect, const QColor &background,
          const QColor &outline = Qt::transparent, int radius = 8);
void line(QPainter &p, const QRect &rect, const QString &text, int pixels,
          const QColor &color = Ink, bool bold = false,
          Qt::Alignment alignment = Qt::AlignLeft | Qt::AlignVCenter);
void paragraph(QPainter &p, const QRect &rect, const QString &text, int pixels,
               const QColor &color = Ink, bool bold = false);
void messagePreviewLine(QPainter &painter, const QRect &rect, const QString &text, const QColor &color);
QColor itemOutline(const QStyleOptionViewItem &option, const QColor &normal = Qt::transparent);
void paintBadge(QPainter &p, const QRect &rect, const QString &text, int pixels,
                const QColor &background, const QColor &ink, int radius);
void paintRowTime(QPainter &p, const QRect &rect, const QString &text, const QColor &ink);

} // namespace bx::workspace
