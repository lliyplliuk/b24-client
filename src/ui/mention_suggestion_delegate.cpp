#include "ui/mention_suggestion_delegate.h"

#include <QPainter>

namespace bx::ui {

QSize MentionSuggestionDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const
{
    return {160, qMax(42, 2 * option.fontMetrics.height() + 10)};
}

void MentionSuggestionDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QStyleOptionViewItem row(option);
    initStyleOption(&row, index);
    const auto box = row.rect.adjusted(1, 1, -1, -1);
    painter->save();
    painter->setClipRect(row.rect);
    painter->setRenderHint(QPainter::Antialiasing);
    if (row.state.testFlag(QStyle::State_Selected) || row.state.testFlag(QStyle::State_MouseOver)) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(row.state.testFlag(QStyle::State_Selected) ? QColor("#dcecf0") : QColor("#f0f6f3"));
        painter->drawRoundedRect(box, 6, 6);
    }

    constexpr int iconSize = 22;
    row.icon.paint(painter, QRect(box.left() + 8, box.center().y() - iconSize / 2, iconSize, iconSize));
    const auto text = box.adjusted(38, 0, -9, 0);
    const auto position = index.data(PositionRole).toString();
    const int titleHeight = row.fontMetrics.height();
    const int positionHeight = option.fontMetrics.height();
    const int top = text.top() + (text.height() - titleHeight - (position.isEmpty() ? 0 : positionHeight)) / 2;
    painter->setFont(row.font);
    painter->setPen(QColor("#182d3b"));
    painter->drawText(QRect(text.left(), top, text.width(), titleHeight), Qt::AlignVCenter | Qt::AlignLeft,
                      row.fontMetrics.elidedText(row.text, Qt::ElideRight, text.width()));
    if (!position.isEmpty()) {
        painter->setFont(option.font);
        painter->setPen(QColor("#455c67"));
        painter->drawText(QRect(text.left(), top + titleHeight, text.width(), positionHeight),
                          Qt::AlignVCenter | Qt::AlignLeft,
                          option.fontMetrics.elidedText(position, Qt::ElideRight, text.width()));
    }
    painter->restore();
}

} // namespace bx::ui
