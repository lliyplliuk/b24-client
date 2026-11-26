#pragma once

#include "ui/row_painter.h"

namespace bx::workspace {

struct MessageRowPainter {
    const RowPainter &rows;

    QSize sizeHint(int width, const QModelIndex &index) const;
    void paintMessage(QPainter &p, const QStyleOptionViewItem &option, const QModelIndex &index,
                      const QString &title, const QString &detail) const;
};

} // namespace bx::workspace
