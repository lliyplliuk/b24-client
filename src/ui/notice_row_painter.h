#pragma once

#include "ui/row_painter.h"

namespace bx::workspace {

struct NoticeRowPainter {
    const RowPainter &rows;

    QSize sizeHint(int width, const QModelIndex &index) const;
    void paintNotice(QPainter &p, const QStyleOptionViewItem &option, const QModelIndex &index,
                     const QString &title, const QString &detail, const QString &meta) const;
};

} // namespace bx::workspace
