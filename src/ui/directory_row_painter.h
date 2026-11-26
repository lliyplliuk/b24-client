#pragma once

#include <QStyleOptionViewItem>
#include <QModelIndex>

namespace bx::workspace {

void paintDirectory(QPainter &p, const QStyleOptionViewItem &option, const QModelIndex &index,
                    const QString &title, const QString &detail, const QString &meta);

} // namespace bx::workspace
