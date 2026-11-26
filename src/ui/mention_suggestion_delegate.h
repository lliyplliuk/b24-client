#pragma once

#include <QStyledItemDelegate>

namespace bx::ui {

class MentionSuggestionDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    static constexpr int PositionRole = Qt::UserRole;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

} // namespace bx::ui
