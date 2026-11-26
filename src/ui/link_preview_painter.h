#pragma once

#include "ui/row_painter.h"

namespace bx::workspace {

struct LinkPreviewPainter {
    const RowPainter &rows;

    void paintLinkProjectAvatar(QPainter &p, const QRect &box, const Project &project) const;
    void paintLinkIcon(QPainter &p, const QRect &box, const MessageLinkPreview &preview) const;
    void paintLinkCards(QPainter &p, const QRect &row, const QModelIndex &index) const;
    int paintTaskLinkDetailIcon(QPainter &p, const QRect &row, const QString &label,
                                const MessageLinkPreview &preview) const;
    void paintLinkDetails(QPainter &p, const QRect &area, const MessageLinkPreview &preview) const;
};

} // namespace bx::workspace
