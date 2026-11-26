#include "ui/card_row_painter.h"
#include "ui/row_paint_primitives.h"
#include "ui/typography.h"

namespace bx::workspace {

QSize cardSizeHint(int width, const QModelIndex &index)
{
    const auto title = index.data(Title).toString();
    const auto detail = index.data(Detail).toString();
    const int height = wrappedText(title, qMax(75, width - 27), rowFont(ui::bodyTextPixels, true));
    return QSize(0, 25 + qMin(45, height) + (detail.isEmpty() ? 0 : 19));
}

void paintCard(QPainter &p, const QStyleOptionViewItem &option, const QString &title,
               const QString &detail)
{
    const QRect r = option.rect.adjusted(1, 2, -2, -3);
    if (title.isEmpty()) {
        paragraph(p, r.adjusted(7, 3, -7, 0), detail, ui::bodyTextPixels, Muted);
        return;
    }
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;
    fill(p, r, selected ? Selected : (hovered ? QColor("#f5fbfa") : Surface),
         itemOutline(option, selected ? Accent : Border));
    paragraph(p, r.adjusted(10, 8, -10, detail.isEmpty() ? -6 : -25), title, ui::bodyTextPixels, Ink, true);
    if (!detail.isEmpty())
        line(p, QRect(r.left() + 10, r.bottom() - 22, r.width() - 20, 19), detail, ui::secondaryTextPixels, Muted);
}

} // namespace bx::workspace
