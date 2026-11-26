#include "ui/row_painter.h"
#include "ui/chat_image_previews.h"

#include <algorithm>

namespace bx::workspace {

QVector<MessageFile> RowPainter::visibleVideos(const QModelIndex &index, const QRect &row, const QRect &visible) const
{
    QVector<MessageFile> result;
    visitFileGeometry(index, row, [&](const MessageFile &file, const FileGeometry &geometry) {
        if (file.isVideo && (geometry.control.intersects(visible) || geometry.thumbnail.intersects(visible)))
            result.append(file);
        return false;
    });
    if (const auto representative = visibleQuoteMedia(index, row, visible); representative && representative->isVideo)
        result.append(*representative);
    return result;
}

std::optional<MessageFile> RowPainter::visibleQuoteMedia(const QModelIndex &index, const QRect &row, const QRect &visible) const
{
    if (index.data(ReplyByline).toString().isEmpty() ||
        !quoteMediaRect(replyHeaderRect(row, index)).intersects(visible))
        return {};
    return replyAttachments(index).representative;
}

QVector<MessageFile> RowPainter::photoOriginals(const QModelIndex &index) const
{
    QVector<MessageFile> result;
    for (const auto &file : files(index, MessageFiles)) {
        if (!file.isVideo && filePreview(file) && downloadable(file) && !matchesOriginal(result, file))
            result.append(file);
    }
    return result;
}

bool RowPainter::hasOriginal(const QModelIndex &index, const MessageFile &file)
{
    return matchesOriginal(files(index, MessageFiles), file) || matchesOriginal(files(index, ReplyFiles), file);
}

bool RowPainter::hasPhotoPreview(const QModelIndex &index, const MessageFile &file)
{
    return matchesOriginal(files(index, MessageFiles), file, true)
        || matchesOriginal(files(index, ReplyFiles), file, true);
}

bool RowPainter::downloadable(const MessageFile &file)
{
    return file.downloadUrl.isValid() && file.downloadUrl.scheme() == QStringLiteral("https") &&
           !file.downloadUrl.host().isEmpty() && file.downloadUrl.userInfo().isEmpty();
}

QVector<MessageFile> RowPainter::files(const QModelIndex &index, int role)
{
    return index.data(role).value<QVector<MessageFile>>();
}

bool RowPainter::isMediaFile(const MessageFile &file)
{
    return file.isImage || file.isVideo || !file.previewUrl.isEmpty() || !file.originalPreviewUrl.isEmpty();
}

RowPainter::QuoteAttachments RowPainter::replyAttachments(const QModelIndex &index) const
{
    QuoteAttachments result;
    for (const auto &entry : files(index, ReplyFiles)) {
        if (isMediaFile(entry)) {
            ++result.media;
            if (!result.representative)
                result.representative = entry;
        } else {
            ++result.documents;
            if (result.firstDocument.isEmpty())
                result.firstDocument = entry.name;
        }
    }
    return result;
}

int RowPainter::quoteHeight(const QModelIndex &index) const
{
    return replyAttachments(index).media > 0 ? messageMediaQuoteHeight : messageQuoteHeight;
}

QRect RowPainter::quoteMediaRect(const QRect &quote)
{
    if (quote.width() < 108)
        return {};
    return QRect(quote.left() + 11, quote.top() + (quote.height() - 40) / 2, 40, 40);
}

std::optional<RowPainter::FileTarget> RowPainter::fileAt(const QModelIndex &index, const QRect &row, const QPoint &point) const
{
    std::optional<FileTarget> result;
    visitFileGeometry(index, row, [&](const MessageFile &file, const FileGeometry &geometry) {
        if (!geometry.thumbnail.contains(point) && !geometry.control.contains(point))
            return false;
        result = FileTarget{file, geometry};
        return true;
    });
    return result;
}

RowPainter::FileGeometry RowPainter::fileGeometry(const QRect &card, const MessageFile &file) const
{
    const auto *photo = filePreview(file);
    if (!photo)
        return {{}, card};
    const auto source = file.previewSize.isEmpty() ? photo->size() : file.previewSize;
    const QRect thumbnail(card.topLeft(), ui::chatPreviewDisplaySize(source, card.width()));
    return {thumbnail, {}};
}

int RowPainter::fileHeight(const QVector<MessageFile> &entries, int width, AttachmentSection section) const
{
    int height = 0;
    layoutFiles(entries, QRect(0, 0, qMax(1, width), fileControlHeight), section,
                [](const auto &, const auto &) { return false; }, &height);
    return height;
}

bool RowPainter::matchesOriginal(const QVector<MessageFile> &entries, const MessageFile &file, bool preview)
{
    return std::any_of(entries.cbegin(), entries.cend(), [&file, preview](const MessageFile &current) {
        return current.downloadUrl == file.downloadUrl && current.id == file.id
            && current.diskObjectId == file.diskObjectId && current.name == file.name
            && (!preview || (current.previewUrl == file.previewUrl
                && current.originalPreviewUrl == file.originalPreviewUrl));
    });
}

QRect RowPainter::photoMenuRect(const QRect &thumbnail, const MessageFile &file) const
{
    if (thumbnail.width() < 32 || !m_messagePhotoMenu || (!downloadable(file) && !filePreview(file)))
        return {};
    return QRect(thumbnail.right() - 28, thumbnail.top() + 5, 24, 24);
}

QRect RowPainter::firstFileRect(const QRect &row, const QModelIndex &index, AttachmentSection section) const
{
    const auto bubble = messageBubble(row, index);
    const int top = section == AttachmentSection::Media
        ? bubble.top() + messageBodyTop + messageHeader(index) + messageContext(index)
        : messageContentRect(row, index).bottom() + 3;
    return QRect(bubble.left() + 10, top, qMax(1, bubble.width() - 20), fileControlHeight);
}

bool RowPainter::hasVisibleLoadingPreview(const QModelIndex &index, const QRect &row, const QRect &visible) const
{
    bool pending = false;
    visitFileGeometry(index, row, [&](const MessageFile &file, const FileGeometry &geometry) {
        if (!previewPending(file) || (!geometry.thumbnail.isEmpty() && filePreview(file)))
            return false;
        const auto ring = geometry.thumbnail.isEmpty() ? cardLoadingRing(geometry.control)
            : mediaLoadingRing(geometry.thumbnail, file, false);
        pending = ring.intersects(visible);
        return pending;
    });
    if (pending)
        return true;
    const auto representative = visibleQuoteMedia(index, row, visible);
    return representative && previewPending(*representative) && !filePreview(*representative)
        && mediaLoadingRing(quoteMediaRect(replyHeaderRect(row, index)), *representative, true).intersects(visible);
}

bool RowPainter::previewPending(const MessageFile &file) const
{
    return m_previewPending && m_previewPending(file);
}

QRect RowPainter::cardLoadingRing(const QRect &card)
{
    return QRect(card.left() + 14, card.top() + 12, 18, 18);
}

QRect RowPainter::mediaLoadingRing(const QRect &frame, const MessageFile &file, bool crop)
{
    QRect ring(0, 0, 18, 18);
    ring.moveCenter(frame.center());
    if (!crop && file.isVideo && downloadable(file) && frame.width() >= 40)
        ring.moveTopLeft(frame.topLeft() + QPoint(4, 4));
    return ring;
}

} // namespace bx::workspace
