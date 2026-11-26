#include "ui/task_picker_choice_delegate.h"

#include <QPainter>
#include <QPainterPath>

namespace bx::ui {

QSize TaskPickerChoiceDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const
{
    return {160, RowHeight};
}

void TaskPickerChoiceDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QStyleOptionViewItem row(option);
    initStyleOption(&row, index);
    const bool confirmed = index.data(Qt::CheckStateRole).toInt() == Qt::Checked;
    const bool draft = index.data(TaskPickerDraft).toBool();
    const bool cursor = row.state.testFlag(QStyle::State_Selected);
    const auto box = row.rect.adjusted(1, 1, -2, -1);
    painter->save();
    painter->setClipRect(row.rect);
    painter->setRenderHint(QPainter::Antialiasing);
    if (confirmed || draft || cursor) {
        painter->setPen(cursor ? QPen(QColor("#54877e")) : Qt::NoPen);
        painter->setBrush(confirmed ? QColor("#e1f0ec") : draft ? QColor("#e5eff4") : QColor("#f3f7f5"));
        painter->drawRoundedRect(box, 6, 6);
    }

    int textLeft = box.left() + 10;
    if (!row.icon.isNull()) {
        constexpr int iconSize = 22;
        row.icon.paint(painter, QRect(textLeft, box.center().y() - iconSize / 2, iconSize, iconSize));
        textLeft += iconSize + 8;
    }
    const int rightPadding = confirmed || draft ? 31 : 10;
    const QRect textRect(textLeft, box.top(), qMax(0, box.right() - rightPadding - textLeft), box.height());
    painter->setFont(row.font);
    painter->setPen(QColor("#182d3b"));
    painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                      row.fontMetrics.elidedText(row.text, Qt::ElideRight, textRect.width()));
    const QPointF mark(box.right() - 16, box.center().y());
    if (confirmed) {
        painter->setPen(QPen(QColor("#26685d"), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->setBrush(Qt::NoBrush);
        QPainterPath check(mark + QPointF(-5, 0));
        check.lineTo(mark + QPointF(-1.5, 3.5));
        check.lineTo(mark + QPointF(5, -4));
        painter->drawPath(check);
    } else if (draft) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor("#28657b"));
        painter->drawEllipse(mark, 3.5, 3.5);
    }
    painter->restore();
}

} // namespace bx::ui
