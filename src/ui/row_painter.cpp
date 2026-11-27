#include "ui/row_painter.h"
#include "ui/row_paint_primitives.h"
#include "ui/directory_row_painter.h"
#include "ui/card_row_painter.h"
#include "ui/message_row_painter.h"
#include "ui/notice_row_painter.h"

#include <QListWidget>

#include <utility>

namespace bx::workspace {

RowPainter::RowPainter(QListWidget *view, Row style) : QStyledItemDelegate(view), m_view(view), m_style(style)
{
    if (m_style == Row::Message || m_style == Row::TaskContent || m_style == Row::Notice) {
        m_view->viewport()->setMouseTracking(true);
        m_view->viewport()->installEventFilter(this);
    }
    if (m_style == Row::Message || m_style == Row::TaskContent) {
        m_previewAnimation.emplace(m_view->viewport());
        // Model changes already repaint the view and can also occur during its destruction.
        const auto forgetSelection = [this] { m_textIndex = QPersistentModelIndex{}; };
        connect(m_view->model(), &QAbstractItemModel::modelAboutToBeReset, this, forgetSelection);
        connect(m_view->model(), &QAbstractItemModel::rowsAboutToBeRemoved, this,
                [this, forgetSelection](const QModelIndex &parent, int first, int last) {
            if (m_textIndex.isValid() && m_textIndex.parent() == parent &&
                m_textIndex.row() >= first && m_textIndex.row() <= last)
                forgetSelection();
        });
        connect(m_view->model(), &QAbstractItemModel::dataChanged, this,
                [this, forgetSelection](const QModelIndex &first, const QModelIndex &last, const QList<int> &roles) {
            if (m_textIndex.isValid() && m_textIndex.row() >= first.row() && m_textIndex.row() <= last.row() &&
                (roles.isEmpty() || roles.contains(Id) || roles.contains(Detail) || roles.contains(MarkupHtml)))
                forgetSelection();
        });
    }
}

RowPainter::~RowPainter() = default;

void RowPainter::setPreviewAnimation(bool active)
{
    if (m_previewAnimation)
        m_previewAnimation->setActive(active);
}

int RowPainter::previewAngle() const
{
    return m_previewAnimation ? m_previewAnimation->angle() : 0;
}

void RowPainter::setNoticeHandlers(std::function<void(const QModelIndex &, int)> open,
                                   std::function<void(const QModelIndex &)> toggle,
                                   std::function<void(const QModelIndex &, const QPoint &)> menu)
{
    m_noticeAction = std::move(open);
    m_noticeToggle = std::move(toggle);
    m_noticeMenu = std::move(menu);
}

void RowPainter::setMessageDownloadHandler(std::function<void(const MessageFile &)> handler)
{
    m_messageDownload = std::move(handler);
}

void RowPainter::setMessageLinkHandler(std::function<void(const QString &)> handler)
{
    m_messageLink = std::move(handler);
}

void RowPainter::setMessagePreviewSource(std::function<const QPixmap *(const QUrl &)> source)
{
    m_messagePreview = std::move(source);
}

void RowPainter::setPreviewPendingSource(std::function<bool(const MessageFile &)> source)
{
    m_previewPending = std::move(source);
}

void RowPainter::setVideoPreviewSource(std::function<const QPixmap *(const MessageFile &)> source)
{
    m_videoPreview = std::move(source);
}

const QPixmap *RowPainter::filePreview(const MessageFile &file) const
{
    if (file.isVideo)
        return m_videoPreview ? m_videoPreview(file) : nullptr;
    if (!m_messagePreview)
        return nullptr;
    const auto *photo = file.previewUrl.isEmpty() ? nullptr : m_messagePreview(file.previewUrl);
    if ((!photo || photo->isNull()) && !file.originalPreviewUrl.isEmpty())
        photo = m_messagePreview(file.originalPreviewUrl);
    return photo && !photo->isNull() ? photo : nullptr;
}

void RowPainter::visitFileGeometry(const QModelIndex &index, const QRect &row, const FileVisitor &visit) const
{
    const auto entries = files(index, MessageFiles);
    for (const auto section : {AttachmentSection::Media, AttachmentSection::Documents}) {
        if (layoutFiles(entries, firstFileRect(row, index, section), section, visit, nullptr))
            return;
    }
}

bool RowPainter::layoutFiles(const QVector<MessageFile> &entries, const QRect &area, AttachmentSection section,
                             const FileVisitor &visit, int *height) const
{
    int y = area.top();
    for (qsizetype i = 0; i < entries.size();) {
        const auto &file = entries.at(i);
        const bool media = isMediaFile(file);
        if (media != (section == AttachmentSection::Media)) {
            ++i;
            continue;
        }
        qsizetype end = i + 1;
        while (media && end < entries.size() && isMediaFile(entries.at(end)))
            ++end;
        if (end == i + 1) {
            const auto geometry = fileGeometry(QRect(area.left(), y, area.width(), fileControlHeight), file);
            if (visit(file, geometry))
                return true;
            y += geometry.bounds().height() + galleryGap;
        } else {
            const auto width = qMin(area.width(), galleryWidthLimit);
            const int columns = width >= 204 ? 2 : 1;
            const int tileWidth = (width - (columns - 1) * galleryGap) / columns;
            const int tileHeight = qBound(52, tileWidth * 9 / 16, 128);
            for (qsizetype tile = 0; tile < end - i; ++tile) {
                const int x = area.left() + int(tile % columns) * (tileWidth + galleryGap);
                const int top = y + int(tile / columns) * (tileHeight + galleryGap);
                if (visit(entries.at(i + tile), FileGeometry{QRect(x, top, tileWidth, tileHeight), {}}))
                    return true;
            }
            y += int((end - i + columns - 1) / columns) * (tileHeight + galleryGap);
        }
        i = end;
    }
    if (height)
        *height = y - area.top();
    return false;
}

void RowPainter::setLinkThumbnailSource(std::function<const QImage *(const QUrl &)> source)
{
    m_linkThumbnail = std::move(source);
}

void RowPainter::setLinkPreviewSource(std::function<MessageLinkPreview(const QModelIndex &, const QString &)> source)
{
    m_linkPreview = std::move(source);
}

void RowPainter::setLinkAvatarSource(std::function<const QPixmap *(const QUrl &)> source)
{
    m_linkAvatar = std::move(source);
}

void RowPainter::setMessagePhotoHandler(std::function<void(const MessageFile &)> handler)
{
    m_messagePhoto = std::move(handler);
}

void RowPainter::setMessageVideoHandler(std::function<void(const QModelIndex &, const MessageFile &)> handler)
{
    m_messageVideo = std::move(handler);
}

void RowPainter::setMessagePhotoMenuHandler(std::function<void(const QModelIndex &, const MessageFile &, const QRect &)> handler)
{
    m_messagePhotoMenu = std::move(handler);
}

void RowPainter::setMessageViewersHandler(std::function<void(const QString &, const QPoint &)> handler)
{
    m_messageViewers = std::move(handler);
}

void RowPainter::setReplyNavigation(std::function<void(const QString &)> handler)
{
    m_replyNavigation = std::move(handler);
}

void RowPainter::setReactionActions(std::function<void(const QModelIndex &, const QString &, const QPoint &)> action,
                                    std::function<bool()> allowed)
{
    m_reaction = std::move(action);
    m_reactionAllowed = std::move(allowed);
}

bool RowPainter::canReact(const QModelIndex &index) const
{
    return reactionRow(index) && m_reactionAllowed && m_reactionAllowed();
}

void RowPainter::openReactionPalette(const QModelIndex &index, const QPoint &anchor) const
{
    if (canReact(index))
        m_reaction(index, {}, anchor);
}

QSize RowPainter::sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const
{
    const int width = m_view->viewport()->width();
    if (m_style == Row::Directory)
        return QSize(0, 56);
    if (m_style == Row::Message || m_style == Row::TaskContent)
        return MessageRowPainter{*this}.sizeHint(width, index);
    if (m_style == Row::Notice)
        return NoticeRowPainter{*this}.sizeHint(width, index);
    return cardSizeHint(width, index);
}

void RowPainter::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    painter->save();
    painter->setClipRect(option.rect);
    painter->setRenderHint(QPainter::Antialiasing);
    const auto title = index.data(Title).toString();
    const auto detail = index.data(Detail).toString();
    const auto meta = index.data(Meta).toString();
    if (m_style == Row::Directory)
        paintDirectory(*painter, option, index, title, detail, meta);
    else if (m_style == Row::Message || m_style == Row::TaskContent)
        MessageRowPainter{*this}.paintMessage(*painter, option, index, title, detail);
    else if (m_style == Row::Notice)
        NoticeRowPainter{*this}.paintNotice(*painter, option, index, title, detail, meta);
    else
        paintCard(*painter, option, title, detail);
    painter->restore();
}

} // namespace bx::workspace
