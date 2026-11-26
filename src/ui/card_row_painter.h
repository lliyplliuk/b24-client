#pragma once

#include <QStyleOptionViewItem>
#include <QModelIndex>

namespace bx::workspace {

QSize cardSizeHint(int width, const QModelIndex &index);
void paintCard(QPainter &p, const QStyleOptionViewItem &option, const QString &title,
               const QString &detail);

} // namespace bx::workspace
