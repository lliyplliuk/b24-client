#include "ui/message_row_painter.h"
#include "ui/row_paint_primitives.h"
#include "ui/link_preview_painter.h"
#include "ui/file_preview_painter.h"
#include "ui/avatar_painter.h"

#include <QPainterPath>

namespace bx::workspace {

QSize MessageRowPainter::sizeHint(int width, const QModelIndex &index) const
{
    const auto detail = index.data(Detail).toString();
    const int bubbleWidth = rows.messageBubbleWidth(width, index);
    const int bodyWidth = index.data(Id).toString().isEmpty() ? width - 16 : bubbleWidth - 24;
    const auto entries = rows.files(index, MessageFiles);
    const int textHeight = detail.isEmpty() && !entries.isEmpty() ? 0 : rows.bodyHeight(index, detail, bodyWidth);
    return QSize(0, RowPainter::messageRowTop + RowPainter::messageRowBottom + RowPainter::messageBodyTop + RowPainter::messageBodyBottom
                     + textHeight + rows.messageHeader(index) +
                     rows.messageContext(index) + rows.fileHeight(entries, bubbleWidth - 20, RowPainter::AttachmentSection::Media)
                     + rows.fileHeight(entries, bubbleWidth - 20, RowPainter::AttachmentSection::Documents) +
                     rows.linkPreviewHeight(index) +
                     rows.messageTime(index) + rows.messageDividers(index));
}

void MessageRowPainter::paintMessage(QPainter &p, const QStyleOptionViewItem &option, const QModelIndex &index,
                              const QString &title, const QString &detail) const
{
    const QRect r = option.rect;
    if (index.data(Id).toString().isEmpty()) {
        rows.body(p, r.adjusted(8, 3, -8, 0), index, detail, Muted);
        return;
    }
    const auto day = index.data(DayDivider).toString();
    if (!day.isEmpty()) {
        const int width = QFontMetrics(rowFont(ui::secondaryTextPixels, true)).horizontalAdvance(day) + 20;
        const QRect stamp(r.center().x() - width / 2, r.top() + 3, width, 19);
        paintBadge(p, stamp, day, ui::secondaryTextPixels, QColor("#dce9e2"), QColor("#486961"), 9);
    }
    if (index.data(UnreadDivider).toBool()) {
        const auto band = QRect(r.left() + 8, r.top() + rows.messageDay(index) + 3, r.width() - 16, 24);
        const int width = qMin(band.width(),
            QFontMetrics(rowFont(ui::secondaryTextPixels, true)).horizontalAdvance(UnreadMessages) + 18);
        const auto caption = QRect(band.center().x() - width / 2, band.top(), width, band.height());
        p.setPen(QColor("#9fbdb9"));
        if (caption.left() > band.left() + 6) {
            const int y = band.center().y();
            p.drawLine(band.left(), y, caption.left() - 6, y);
            p.drawLine(caption.right() + 6, y, band.right(), y);
        }
        paintBadge(p, caption, UnreadMessages, ui::secondaryTextPixels, QColor("#d6e9e5"), Accent, 7);
    }
    const bool outgoing = index.data(Outgoing).toBool();
    const QRect bubble = rows.messageBubble(r, index);
    const bool system = index.data(SystemMessage).toBool();
    if (!system && !outgoing && !title.isEmpty() && rows.messageAvatarSpace(r.width())) {
        const QRect avatar(r.left() + 4, bubble.top() + 4, 26, 26);
        paintAvatar(p, avatar, title, AvatarKind::Person, index.data(AvatarImage).value<QPixmap>());
    }
    const auto outline = option.state & QStyle::State_Selected ? Accent
        : system ? QColor("#bfcec5") : outgoing ? QColor("#9fc6b3") : QColor("#c2d2c7");
    if (rows.m_style != Row::TaskContent)
        fill(p, bubble, system ? QColor("#e2ebe6") : outgoing ? QColor("#c8e7d8") : Surface,
             itemOutline(option, outline), system ? 7 : 12);
    if (!title.isEmpty())
        line(p, QRect(bubble.left() + 12, bubble.top() + 4, bubble.width() - 23, RowPainter::messageHeaderHeight),
             title, ui::secondaryTextPixels, Accent, true);
    int contextTop = bubble.top() + 5 + rows.messageHeader(index);
    const auto forward = index.data(ForwardByline).toString();
    if (!forward.isEmpty()) {
        const QPoint mark(bubble.left() + 12, contextTop + 8);
        QPainterPath arrow;
        arrow.moveTo(mark + QPoint(0, 4));
        arrow.quadTo(mark + QPoint(1, -2), mark + QPoint(10, 0));
        arrow.moveTo(mark + QPoint(7, -3));
        arrow.lineTo(mark + QPoint(10, 0));
        arrow.lineTo(mark + QPoint(7, 3));
        p.setPen(QPen(Accent, 1.35, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(arrow);
        line(p, QRect(bubble.left() + 28, contextTop, bubble.width() - 39, RowPainter::messageForwardHeight),
             forward, ui::secondaryTextPixels, Accent, true);
        contextTop += RowPainter::messageForwardHeight;
    }
    const FilePreviewPainter attachments{rows};
    using Section = RowPainter::AttachmentSection;
    if (!index.data(ReplyByline).toString().isEmpty())
        attachments.paintQuote(p, rows.replyHeaderRect(r, index), index, outgoing);
    const auto entries = rows.files(index, MessageFiles);
    attachments.paintFiles(p, rows.firstFileRect(r, index, Section::Media), entries, Section::Media);
    if (!detail.isEmpty() || entries.isEmpty())
        rows.body(p, rows.messageContentRect(r, index), index, detail, Ink);
    attachments.paintFiles(p, rows.firstFileRect(r, index, Section::Documents), entries, Section::Documents);
    if (rows.m_linkPreview)
        LinkPreviewPainter{rows}.paintLinkCards(p, r, index);
    const auto delivery = rows.deliveryText(index);
    if (rows.m_style == Row::Message && !system) {
        for (const auto &target : rows.reactionTargets(index, bubble)) {
            fill(p, target.rect, target.own ? QColor("#b7bcff") : QColor("#e2ebfb"),
                 target.own ? QColor("#5553a7") : QColor("#9cacc8"), 6);
            const QRect emojiRect(target.rect.left() + RowPainter::reactionIconPadding, target.rect.top(),
                                  RowPainter::reactionGlyphBox, target.rect.height());
            p.setFont(reactionFont(17));
            p.setPen(Ink);
            p.drawText(emojiRect, Qt::AlignCenter, reactionGlyph(target.code));
            p.setFont(rowFont(ui::secondaryTextPixels, target.own));
            p.setPen(target.own ? QColor("#24225b") : QColor("#23375e"));
            p.drawText(target.rect.adjusted(RowPainter::reactionCountOffset, 0, -RowPainter::reactionCountPadding, 0),
                       Qt::AlignCenter | Qt::TextSingleLine, target.count);
        }
    }
    if (!delivery.isEmpty())
        paintRowTime(p, rows.deliveryRect(bubble), delivery, ConversationMetaInk);
}

} // namespace bx::workspace
