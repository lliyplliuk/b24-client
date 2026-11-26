#pragma once

#include <QStyledItemDelegate>

namespace bx::ui {

inline constexpr int TaskPickerDraft = Qt::UserRole + 150;

// The current value and the keyboard cursor are independent in picker lists.
class TaskPickerChoiceDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    static constexpr int RowHeight = 34;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

} // namespace bx::ui
