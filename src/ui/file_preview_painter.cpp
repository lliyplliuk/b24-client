#include "ui/file_preview_painter.h"
#include "ui/row_paint_primitives.h"
#include "ui/attachment_icons.h"
#include "ui/loading_indicator.h"

#include <QLocale>
#include <QPainterPath>

namespace bx::workspace {

void FilePreviewPainter::paintMediaFrame(QPainter &p, const QRect &frame, const MessageFile &file, bool crop) const
{
    fill(p, frame, QColor("#e2ede8"), QColor("#bbd3ca"), 6);
    if (const auto *photo = rows.filePreview(file)) {
        const auto aspect = crop ? Qt::KeepAspectRatioByExpanding : Qt::KeepAspectRatio;
        QRect image(QPoint{}, photo->size().scaled(frame.size(), aspect));
        image.moveCenter(frame.center());
        p.save();
        QPainterPath clip;
        clip.addRoundedRect(frame, 6, 6);
        p.setClipPath(clip, Qt::IntersectClip);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawPixmap(image, *photo);
        p.restore();
    } else if (rows.previewPending(file)) {
        ui::paintLoadingRing(p, rows.mediaLoadingRing(frame, file, crop), Accent, rows.previewAngle());
    } else if (crop || !file.isVideo || !rows.downloadable(file)) {
        const auto &icon = ui::attachmentIcon(file.name);
        const QRect glyph(0, 0, 21, 21);
        ui::paintGlyph(p, icon.glyph, glyph.translated(frame.center() - glyph.center()), icon.ink);
    }
}

void FilePreviewPainter::paintMediaTile(QPainter &p, const QRect &thumbnail, const MessageFile &file) const
{
    paintMediaFrame(p, thumbnail, file);
    if (file.isVideo && rows.downloadable(file) && thumbnail.width() >= 40) {
        const auto center = thumbnail.center();
        p.save();
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(32, 57, 69, 218));
        p.drawEllipse(center, 19, 19);
        p.setBrush(Qt::white);
        p.drawPolygon(QPolygon{center + QPoint(-4, -8), center + QPoint(9, 0), center + QPoint(-4, 8)});
        p.restore();
    }
    const auto menu = rows.photoMenuRect(thumbnail, file);
    if (!menu.isEmpty()) {
        fill(p, menu, QColor("#203945"), Qt::transparent, 5);
        ui::paintGlyph(p, ui::Glyph::More, QRectF(menu).adjusted(3, 3, -3, -3), Qt::white);
    }
}

void FilePreviewPainter::paintFiles(QPainter &p, const QRect &start, const QVector<MessageFile> &entries, RowPainter::AttachmentSection section) const
{
    rows.layoutFiles(entries, start, section, [&](const MessageFile &file, const RowPainter::FileGeometry &geometry) {
        if (!geometry.thumbnail.isEmpty()) {
            paintMediaTile(p, geometry.thumbnail, file);
            return false;
        }
        const auto &card = geometry.control;
        fill(p, card, QColor("#f6faf8"), QColor("#bbd3ca"), 6);
        const QRect icon(card.left() + 6, card.top() + 7, 34, 28);
        const auto &typeIcon = ui::attachmentIcon(file.name);
        fill(p, icon, typeIcon.background, Qt::transparent, 5);
        if (rows.previewPending(file))
            ui::paintLoadingRing(p, rows.cardLoadingRing(card), typeIcon.ink, rows.previewAngle());
        else
            ui::paintGlyph(p, typeIcon.glyph, QRectF(icon).adjusted(6, 3, -6, -3), typeIcon.ink);
        const int textLeft = icon.right() + 7;
        const QRect textArea(textLeft, card.top() + RowPainter::fileTitleTop, qMax(1, card.right() - textLeft - 5),
                             RowPainter::fileTitleHeight);
        const bool opensMedia = (file.isImage || file.isVideo) && rows.downloadable(file);
        const auto title = !opensMedia ? file.name : (file.isVideo ? QStringLiteral("Воспроизвести %1")
                                                                  : QStringLiteral("Открыть %1")).arg(file.name);
        line(p, textArea, title, ui::bodyTextPixels, opensMedia ? Accent : Ink, true);
        QStringList metadata;
        if (file.size >= 0)
            metadata.append(QLocale().formattedDataSize(file.size, 1));
        metadata.append(rows.downloadable(file) ? QStringLiteral("Сохранить") : QStringLiteral("Скачивание недоступно"));
        line(p, textArea.translated(0, RowPainter::fileTitleHeight), metadata.join(QStringLiteral(" · ")),
             ui::secondaryTextPixels, Accent);
        return false;
    }, nullptr);
}

void FilePreviewPainter::paintQuote(QPainter &p, const QRect &quote, const QModelIndex &index, bool outgoing) const
{
    fill(p, quote, outgoing ? QColor("#b5d9c9") : QColor("#e8f1ee"), Qt::transparent, 5);
    p.setPen(QColor("#54877a"));
    p.drawLine(quote.left() + 4, quote.top() + 5, quote.left() + 4, quote.bottom() - 5);
    const auto attachments = rows.replyAttachments(index);
    const auto excerpt = index.data(ReplyPreview).toString();
    auto text = quote.adjusted(11, 2, -6, 0);
    text.setHeight(RowPainter::messageQuoteLineHeight);
    if (attachments.representative) {
        const auto thumbnail = rows.quoteMediaRect(quote);
        if (!thumbnail.isEmpty()) {
            paintMediaFrame(p, thumbnail, *attachments.representative, true);
            text.setLeft(thumbnail.right() + 8);
        }
    }
    line(p, text, index.data(ReplyByline).toString(), ui::secondaryTextPixels, Accent, true);
    text.translate(0, RowPainter::messageQuoteLineHeight);
    const auto attachmentsOnly = excerpt == QLatin1String("Вложение");
    if (attachments.representative) {
        auto media = attachments.media > 1 ? QStringLiteral("Галерея · %1 медиа").arg(attachments.media)
            : attachments.representative->isVideo ? QStringLiteral("Видео") : QStringLiteral("Фотография");
        if (attachments.documents)
            media += QStringLiteral(" · Файлы: %1").arg(attachments.documents);
        line(p, text, media, ui::secondaryTextPixels, Ink, true);
        if (!attachmentsOnly) {
            text.translate(0, RowPainter::messageQuoteLineHeight);
            messagePreviewLine(p, text, excerpt, ConversationMetaInk);
        }
    } else {
        auto caption = excerpt;
        if (attachments.documents) {
            const auto description = attachments.documents == 1 ? attachments.firstDocument
                : QStringLiteral("Файлы · %1").arg(attachments.documents);
            caption = attachmentsOnly ? description : caption + QStringLiteral(" · ") + description;
        }
        messagePreviewLine(p, text, caption, Ink);
    }
}

} // namespace bx::workspace
