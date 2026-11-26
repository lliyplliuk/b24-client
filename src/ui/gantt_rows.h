#pragma once

#include <QStyledItemDelegate>
#include <QDate>

namespace bx::ui {

class GanttRows final : public QStyledItemDelegate {
public:
    explicit GanttRows(QObject *parent);
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override;
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QDate m_first;
    QDate m_last;
};

} // namespace bx::ui
