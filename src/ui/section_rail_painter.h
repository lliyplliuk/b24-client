#pragma once

#include <QStyledItemDelegate>

namespace bx::ui {

constexpr int SectionUnreadCount = Qt::UserRole + 1;
constexpr int SectionWorkdayStatus = Qt::UserRole + 2;
constexpr int SectionWorkdayElapsed = Qt::UserRole + 3;

class SectionRailPainter final : public QStyledItemDelegate {
public:
    explicit SectionRailPainter(QObject *parent);
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override;
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

} // namespace bx::ui
