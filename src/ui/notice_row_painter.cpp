#include "ui/notice_row_painter.h"
#include "ui/row_paint_primitives.h"
#include "ui/link_preview_painter.h"
#include "ui/glyphs.h"

#include <QTextLayout>

namespace bx::workspace {

QRect RowPainter::noticeRect(const QRect &row, const QModelIndex &index)
{
    const auto notice = index.data(NoticePresentation).value<NoticeRow>();
    auto rect = row.adjusted(0, notice.day.isEmpty() ? 2 : noticeDayHeight, -6, -noticeGap);
    rect.setWidth(qMin(rect.width(), 760));
    return rect;
}

QRect RowPainter::noticeMenuRect(const QRect &row, const QModelIndex &index)
{
    const auto r = noticeRect(row, index);
    return QRect(r.right() - 31, r.top() + 5, 27, 28);
}

int RowPainter::noticeTextWidth(const QRect &card, bool heading)
{
    return qMax(1, card.width() - (heading ? 135 : 44));
}

QStringList RowPainter::noticeExcerptLines(const QString &text, int width, bool singleLine)
{
    if (text.isEmpty())
        return {};
    if (singleLine)
        return {text};
    QTextLayout layout(text, rowFont(ui::secondaryTextPixels));
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.beginLayout();
    auto first = layout.createLine();
    first.setLineWidth(width);
    const int next = first.textLength();
    layout.endLayout();
    if (next >= text.size())
        return {text};
    return {text.left(next).trimmed(), text.mid(next).trimmed()};
}

QRect RowPainter::noticeHeaderRect(const QRect &row, const QModelIndex &index, const NoticeRow &notice)
{
    const auto r = noticeRect(row, index);
    const bool titled = !index.data(Title).toString().isEmpty();
    int height = 40;
    if (!notice.expanded) {
        const auto lines = noticeExcerptLines(notice.excerpt, noticeTextWidth(r, !titled), false);
        height += 19 * qMax(0, int(lines.size()) - (titled ? 0 : 1));
    }
    return QRect(r.topLeft(), QSize(r.width(), height));
}

QRect RowPainter::noticeBodyRect(const QRect &row, const QModelIndex &index) const
{
    const auto notice = index.data(NoticePresentation).value<NoticeRow>();
    const auto header = noticeHeaderRect(row, index, notice);
    const int width = noticeTextWidth(header, false);
    const auto detail = index.data(Detail).toString();
    const int height = detail.isEmpty() ? 0 : bodyHeight(index, detail, width);
    const auto action = noticePrimaryRect(row, index, notice);
    const int top = action.isEmpty() ? header.bottom() + 5 : action.bottom() + 9;
    return QRect(header.left() + 25, top, width, height);
}

QRect RowPainter::noticePrimaryRect(const QRect &row, const QModelIndex &index, const NoticeRow &notice) const
{
    if (notice.primaryTarget < 0)
        return {};
    const auto header = noticeHeaderRect(row, index, notice);
    const QFontMetrics font(rowFont(ui::secondaryTextPixels, true));
    const int width = qMin(noticeTextWidth(header, false), font.horizontalAdvance(notice.primaryLabel) + 22);
    return QRect(header.left() + 25, header.bottom() + 5, width, 28);
}

QSize NoticeRowPainter::sizeHint(int width, const QModelIndex &index) const
{
    const QRect row(0, 0, width, 0);
    const auto notice = index.data(NoticePresentation).value<NoticeRow>();
    const auto header = rows.noticeHeaderRect(row, index, notice);
    if (!notice.expanded)
        return QSize(0, header.bottom() + RowPainter::noticeGap + 1);
    const auto action = rows.noticePrimaryRect(row, index, notice);
    const int bodyBottom = rows.noticeBodyRect(row, index).bottom() + rows.linkPreviewHeight(index);
    const int bottom = qMax(qMax(header.bottom(), bodyBottom), action.bottom());
    return QSize(0, bottom + RowPainter::noticeGap + 9);
}

void NoticeRowPainter::paintNotice(QPainter &p, const QStyleOptionViewItem &option, const QModelIndex &index,
                             const QString &title, const QString &detail, const QString &meta) const
{
    const auto notice = index.data(NoticePresentation).value<NoticeRow>();
    const auto r = rows.noticeRect(option.rect, index);
    if (!notice.day.isEmpty()) {
        const QRect day(r.left() + 25, option.rect.top(), r.width() - 25, RowPainter::noticeDayHeight - 3);
        line(p, day, notice.day, ui::secondaryTextPixels, Ink, true);
    }

    const bool unread = index.data(Count).toBool();
    const bool selected = option.state & QStyle::State_Selected;
    fill(p, r, selected ? Selected : unread ? QColor("#e7f0f0") : Surface, itemOutline(option), 6);
    p.setPen(Border);
    p.drawLine(r.left() + 8, r.bottom(), r.right() - 8, r.bottom());
    if (unread) {
        p.setPen(Qt::NoPen);
        p.setBrush(Accent);
        p.drawEllipse(QPoint(r.left() + 13, r.top() + 20), 3, 3);
    }
    const int top = r.top() + 9;
    const int left = r.left() + 25;
    const QRect time(r.right() - 112, top, 53, 22);
    paintRowTime(p, time, meta, ConversationMetaInk);
    const int chevronX = time.right() + 13;
    const int chevronY = time.center().y();
    const int direction = notice.expanded ? -1 : 1;
    p.setPen(QPen(ConversationMetaInk, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const QPoint chevron[]{QPoint(chevronX - 4, chevronY - direction * 2),
                           QPoint(chevronX, chevronY + direction * 2),
                           QPoint(chevronX + 4, chevronY - direction * 2)};
    p.drawPolyline(chevron, 3);
    const auto menu = rows.noticeMenuRect(option.rect, index);
    ui::paintGlyph(p, ui::Glyph::More, QRectF(menu.center() - QPoint(8, 8), QSize(16, 16)), ConversationMetaInk);

    const bool titled = !title.isEmpty();
    if (titled)
        line(p, QRect(left, top, rows.noticeTextWidth(r, true), 22), title, ui::bodyTextPixels, Ink, true);
    if (!notice.expanded || !titled) {
        const int width = rows.noticeTextWidth(r, !titled);
        int y = top + (titled ? 22 : 0);
        for (const auto &excerpt : rows.noticeExcerptLines(notice.excerpt, width, notice.expanded)) {
            line(p, QRect(left, y, width, 20), excerpt, ui::secondaryTextPixels,
                 titled ? ConversationMetaInk : Ink);
            y += 19;
        }
    }
    if (!notice.expanded)
        return;
    rows.body(p, rows.noticeBodyRect(option.rect, index), index, detail, Ink);
    LinkPreviewPainter{rows}.paintLinkCards(p, option.rect, index);
    const auto action = rows.noticePrimaryRect(option.rect, index, notice);
    if (!action.isEmpty()) {
        fill(p, action, QColor("#f5faf7"), QColor("#b4cdc6"), 6);
        line(p, action.adjusted(8, 0, -8, 0), notice.primaryLabel, ui::secondaryTextPixels,
             Accent, true, Qt::AlignCenter);
    }
}

} // namespace bx::workspace
