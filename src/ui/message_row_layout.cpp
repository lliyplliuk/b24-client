#include "ui/row_painter.h"
#include "ui/row_paint_primitives.h"

#include <QFontMetrics>

#include <algorithm>

namespace bx::workspace {

int RowPainter::messageWidth(int width)
{
    const int desktopCap = qBound(compactMessageCap, width * 2 / 3, 780);
    return qBound(1, qMax(96, qMin(desktopCap, width * 80 / 100)), qMax(1, width - 12));
}

int RowPainter::bubbleWidth(int available, const QString &text, int headingWidth, bool block)
{
    const QFontMetrics metrics(rowFont(ui::bodyTextPixels));
    const int preferred = qMax(metrics.horizontalAdvance(text), headingWidth) + 28;
    if (block)
        return qMin(available, qMax(compactMessageCap, preferred));
    return qBound(qMin(available, 120), preferred, available);
}

QRect RowPainter::messageBubble(const QRect &row, const QModelIndex &index) const
{
    const int width = messageBubbleWidth(row.width(), index);
    const int x = row.left() + 4 + (m_style == Row::TaskContent ? 0 : messageAvatarSpace(row.width()));
    const int dividers = messageDividers(index);
    return QRect(x, row.top() + dividers + messageRowTop, width,
                 row.height() - dividers - messageRowTop - messageRowBottom);
}

int RowPainter::messageAvatarSpace(int width)
{
    return width >= 250 ? 33 : 0;
}

int RowPainter::messageBubbleWidth(int outerWidth, const QModelIndex &index) const
{
    if (m_style == Row::TaskContent)
        return qMax(1, outerWidth - 8);
    const int contentWidth = outerWidth - messageAvatarSpace(outerWidth);
    const int actionWidth = m_reaction ? messageActionGroupWidth : messageActionSize;
    const int available = qMin(messageWidth(contentWidth), qMax(1, contentWidth - actionWidth - messageActionGap - 6));
    const QFontMetrics measure(rowFont(ui::secondaryTextPixels, true));
    int headingWidth = measure.horizontalAdvance(index.data(Title).toString());
    for (const auto role : {ForwardByline, ReplyByline}) {
        const auto content = measure.elidedText(index.data(role).toString(), Qt::ElideRight, 260);
        headingWidth = qMax(headingWidth, measure.horizontalAdvance(content));
    }
    const auto reply = index.data(ReplyPreview).toString();
    if (!reply.isEmpty()) {
        const int replyWidth = available > compactMessageCap ? available - 44 : 260;
        headingWidth = qMax(headingWidth, measure.horizontalAdvance(measure.elidedText(reply, Qt::ElideRight, replyWidth)));
    }
    const bool mediaQuote = !reply.isEmpty() && replyAttachments(index).representative.has_value();
    if (mediaQuote)
        headingWidth += 55; // Thumbnail and its spacing sit inside the quotation's text allowance.
    const auto detail = index.data(Detail).toString();
    const int contentLimit = detail.isEmpty() && reply.isEmpty() ? qMin(available, compactMessageCap) : available;
    int result = qMax(bubbleWidth(contentLimit, detail, headingWidth, index.data(MarkupBlock).toBool()),
                      qMin(available, minimumReactionWidth(index)));
    if (mediaQuote)
        result = qMax(result, qMin(available, 285));
    for (const auto &card : linkPreviews(index))
        result = qMax(result, minimumLinkCardWidth(card.second, available));

    const auto entries = files(index, MessageFiles);
    const auto adjacentMedia = std::adjacent_find(entries.cbegin(), entries.cend(), [](const auto &first, const auto &second) {
        return isMediaFile(first) && isMediaFile(second);
    });
    if (adjacentMedia != entries.cend())
        return qMax(result, qMin(galleryWidthLimit + 20, available));
    if (std::any_of(entries.cbegin(), entries.cend(), isMediaFile))
        result = qMax(result, qMin(linkCardMinimum, available));
    return entries.isEmpty() ? result : qMax(result, qMin(280, available));
}

int RowPainter::messageContext(const QModelIndex &index) const
{
    return (index.data(ForwardByline).toString().isEmpty() ? 0 : messageForwardHeight) +
           (index.data(ReplyByline).toString().isEmpty() ? 0 : quoteHeight(index) + 5);
}

int RowPainter::messageHeader(const QModelIndex &index)
{
    return index.data(Title).toString().isEmpty() ? 0 : messageHeaderHeight;
}

QRect RowPainter::replyHeaderRect(const QRect &row, const QModelIndex &index) const
{
    const auto bubble = messageBubble(row, index);
    const int afterForward = index.data(ForwardByline).toString().isEmpty() ? 0 : messageForwardHeight;
    return QRect(bubble.left() + 10, bubble.top() + 5 + messageHeader(index) + afterForward,
                 bubble.width() - 20, quoteHeight(index));
}

int RowPainter::messageTime(const QModelIndex &index) const
{
    if (m_style != Row::Message)
        return index.data(Meta).toString().isEmpty() ? 0 : messageTimeHeight;
    if (!index.data(SystemMessage).toBool() && !index.data(MessageReactionCounts).toMap().isEmpty())
        return reactionHeight + 2;
    return deliveryText(index).isEmpty() ? 0 : messageTimeHeight;
}

bool RowPainter::reactionRow(const QModelIndex &index) const
{
    return m_style == Row::Message && m_reaction && index.isValid()
        && !index.data(SystemMessage).toBool() && !index.data(ReceiptMessageId).toString().isEmpty();
}

QString RowPainter::deliveryText(const QModelIndex &index)
{
    auto caption = index.data(Meta).toString();
    if (index.data(Outgoing).toBool()
        && (!caption.isEmpty() || !index.data(ReceiptMessageId).toString().isEmpty()))
        caption += index.data(MessageSeen).toBool() ? QStringLiteral("  ✓✓") : QStringLiteral("  ✓");
    return caption;
}

QRect RowPainter::deliveryRect(const QRect &bubble)
{
    return {bubble.left() + 8, bubble.bottom() - messageTimeHeight, bubble.width() - 19, messageTimeHeight - 2};
}

QRect RowPainter::viewersReceipt(const QModelIndex &index, const QRect &row) const
{
    if (m_style != Row::Message || !m_messageViewers || !index.data(Outgoing).toBool()
        || !index.data(MessageSeen).toBool() || index.data(ReceiptMessageId).toString().isEmpty())
        return {};
    auto rect = deliveryRect(messageBubble(row, index));
    const auto marks = QFontMetrics(rowFont(messageMetaPixels)).horizontalAdvance(QStringLiteral("✓✓"));
    rect.setLeft(qMax(rect.left(), rect.right() - marks - 5));
    return rect;
}

int RowPainter::reactionBadgeWidth(const QString &count)
{
    const int countWidth = QFontMetrics(rowFont(ui::secondaryTextPixels, true)).horizontalAdvance(count) + 2;
    return reactionCountOffset + countWidth + reactionCountPadding;
}

int RowPainter::reactionDeliverySpace(const QModelIndex &index)
{
    const auto delivery = deliveryText(index);
    return delivery.isEmpty() ? 10
        : QFontMetrics(rowFont(messageMetaPixels)).horizontalAdvance(delivery) + 16;
}

int RowPainter::minimumReactionWidth(const QModelIndex &index)
{
    if (index.data(SystemMessage).toBool())
        return 0;
    int widest = 0;
    const auto counts = index.data(MessageReactionCounts).toMap();
    for (const auto &count : counts) {
        if (count.toInt() > 0)
            widest = qMax(widest, reactionBadgeWidth(count.toString()));
    }
    return widest > 0 ? reactionStart + widest + reactionDeliverySpace(index) + 1 : 0;
}

QVector<RowPainter::ReactionTarget> RowPainter::reactionTargets(const QModelIndex &index, const QRect &bubble)
{
    QVector<ReactionTarget> result;
    const auto counts = index.data(MessageReactionCounts).toMap();
    if (counts.isEmpty())
        return result;
    const auto own = index.data(OwnMessageReactions).toStringList();
    const int right = bubble.right() - reactionDeliverySpace(index);
    const int top = bubble.bottom() - reactionHeight - 2;
    int left = bubble.left() + reactionStart;
    for (bool marked : {true, false}) {
        for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
            if (it.value().toInt() <= 0 || own.contains(it.key()) != marked)
                continue;
            const auto count = it.value().toString();
            const int width = reactionBadgeWidth(count);
            if (left + width > right)
                continue;
            result.append({QRect(left, top, width, reactionHeight), it.key(), count, marked});
            left += width + 3;
        }
    }
    return result;
}

int RowPainter::messageDay(const QModelIndex &index)
{
    return index.data(DayDivider).toString().isEmpty() ? 0 : 27;
}

int RowPainter::messageDividers(const QModelIndex &index)
{
    return messageDay(index) + (index.data(UnreadDivider).toBool() ? 30 : 0);
}

QRect RowPainter::messageContentRect(const QRect &row, const QModelIndex &index) const
{
    if (index.data(Id).toString().isEmpty())
        return row.adjusted(8, 3, -8, 0);
    const auto bubble = messageBubble(row, index);
    const auto entries = files(index, MessageFiles);
    const auto width = bubble.width() - 20;
    return bubble.adjusted(12, messageBodyTop + messageHeader(index) + messageContext(index)
                              + fileHeight(entries, width, AttachmentSection::Media),
                          -12, -messageBodyBottom - messageTime(index)
                              - fileHeight(entries, width, AttachmentSection::Documents) - linkPreviewHeight(index));
}

} // namespace bx::workspace
