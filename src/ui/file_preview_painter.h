#pragma once

#include "ui/row_painter.h"

namespace bx::workspace {

struct FilePreviewPainter {
    const RowPainter &rows;

    void paintMediaFrame(QPainter &p, const QRect &frame, const MessageFile &file, bool crop = false) const;
    void paintMediaTile(QPainter &p, const QRect &thumbnail, const MessageFile &file) const;
    void paintFiles(QPainter &p, const QRect &start, const QVector<MessageFile> &entries, RowPainter::AttachmentSection section) const;
    void paintQuote(QPainter &p, const QRect &quote, const QModelIndex &index, bool outgoing) const;
};

} // namespace bx::workspace
