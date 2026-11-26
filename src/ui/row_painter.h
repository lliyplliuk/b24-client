#pragma once

#include "core/models.h"
#include "ui/preview_animation.h"
#include "ui/typography.h"
#include "ui/workspace_row_style.h"

#include <QCache>
#include <QPersistentModelIndex>
#include <QStyledItemDelegate>
#include <QTextCursor>
#include <QTextDocument>

#include <functional>
#include <optional>

class QListWidget;

namespace bx::workspace {

class RowPainter final : public QStyledItemDelegate {
public:
    static constexpr int messageActionSize = 26;
    static constexpr int messageActionGap = 5;
    static constexpr int messageActionSpacing = 4;
    static constexpr int messageActionGroupWidth = messageActionSize * 2 + messageActionSpacing;
    RowPainter(QListWidget *view, Row style);
    ~RowPainter() override;
    void setNoticeHandlers(std::function<void(const QModelIndex &, int)> open,
                           std::function<void(const QModelIndex &)> toggle,
                           std::function<void(const QModelIndex &, const QPoint &)> menu);
    void setMessageDownloadHandler(std::function<void(const MessageFile &)> handler);
    void setMessageLinkHandler(std::function<void(const QString &)> handler);
    void setMessagePreviewSource(std::function<const QPixmap *(const QUrl &)> source);
    void setPreviewPendingSource(std::function<bool(const MessageFile &)> source);
    void setPreviewAnimation(bool active);
    bool hasVisibleLoadingPreview(const QModelIndex &index, const QRect &row, const QRect &visible) const;
    void setVideoPreviewSource(std::function<const QPixmap *(const MessageFile &)> source);
    const QPixmap *filePreview(const MessageFile &file) const;
    void setLinkThumbnailSource(std::function<const QImage *(const QUrl &)> source);
    void setLinkPreviewSource(std::function<MessageLinkPreview(const QModelIndex &, const QString &)> source);
    void setLinkAvatarSource(std::function<const QPixmap *(const QUrl &)> source);
    void setMessagePhotoHandler(std::function<void(const MessageFile &)> handler);
    void setMessageVideoHandler(std::function<void(const QModelIndex &, const MessageFile &)> handler);
    void setMessagePhotoMenuHandler(std::function<void(const QModelIndex &, const MessageFile &, const QRect &)> handler);
    QVector<MessageFile> visibleVideos(const QModelIndex &index, const QRect &row, const QRect &visible) const;
    std::optional<MessageFile> visibleQuoteMedia(const QModelIndex &index, const QRect &row, const QRect &visible) const;
    bool showPhotoMenuAt(const QModelIndex &index, const QPoint &point) const;
    QVector<MessageFile> photoOriginals(const QModelIndex &index) const;
    static bool hasOriginal(const QModelIndex &index, const MessageFile &file);
    static bool hasPhotoPreview(const QModelIndex &index, const MessageFile &file);
    static bool downloadable(const MessageFile &file);
    void setMessageViewersHandler(std::function<void(const QString &, const QPoint &)> handler);
    // Hit test of the drawn message body: returns the href under the viewport point.
    QString anchorAt(const QModelIndex &index, const QRect &row, const QPoint &viewportPos) const;
    QString linkAt(const QModelIndex &index, const QRect &row, const QPoint &point) const;
    QRect receiptArea(const QModelIndex &index, const QRect &row) const;
    void setReplyNavigation(std::function<void(const QString &)> handler);
    void setReactionActions(std::function<void(const QModelIndex &, const QString &, const QPoint &)> action,
                            std::function<bool()> allowed);
    bool canReact(const QModelIndex &index) const;
    void openReactionPalette(const QModelIndex &index, const QPoint &anchor) const;

    enum class TextPick { Position, Extend, Word };

    struct TextSelection {
        QString messageId;
        QString detail;
        QString markup;
        int anchor;
        int position;
        int verticalX;
    };
    std::optional<TextSelection> snapshotTextSelection() const;
    void restoreTextSelection(const QModelIndex &index, const TextSelection &selection);
    bool pickMessageText(const QModelIndex &index, const QPoint &point, TextPick pick);
    void extendTextSelection(const QPoint &point);
    bool hasTextCursor() const;
    void clearTextSelection();
    QString selectedText(const QModelIndex &index = {}) const;
    void selectMessageText();
    void moveTextSelection(QTextCursor::MoveOperation operation);
    bool activateTextLink(const QString &href, const QPoint &point) const;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const override;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
                     const QModelIndex &index) override;
    bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
                   const QModelIndex &index) override;

    // Enter opens the same media as a click on its preview.
    bool activateMedia(const QModelIndex &index);

private:
    friend struct NoticeRowPainter;
    friend struct MessageRowPainter;
    friend struct LinkPreviewPainter;
    friend struct FilePreviewPainter;
    QListWidget *m_view;
    Row m_style;
    std::function<void(const QModelIndex &, int)> m_noticeAction;
    std::function<void(const QModelIndex &)> m_noticeToggle;
    std::function<void(const QModelIndex &, const QPoint &)> m_noticeMenu;
    std::function<void(const MessageFile &)> m_messageDownload;
    std::function<void(const QString &)> m_messageLink;
    std::function<const QPixmap *(const QUrl &)> m_messagePreview;
    std::function<bool(const MessageFile &)> m_previewPending;
    std::optional<PreviewAnimation> m_previewAnimation;
    std::function<const QPixmap *(const MessageFile &)> m_videoPreview;
    std::function<const QImage *(const QUrl &)> m_linkThumbnail;
    std::function<MessageLinkPreview(const QModelIndex &, const QString &)> m_linkPreview;
    std::function<const QPixmap *(const QUrl &)> m_linkAvatar;
    std::function<void(const MessageFile &)> m_messagePhoto;
    std::function<void(const QModelIndex &, const MessageFile &)> m_messageVideo;
    std::function<void(const QModelIndex &, const MessageFile &, const QRect &)> m_messagePhotoMenu;
    std::function<void(const QString &, const QPoint &)> m_messageViewers;
    std::function<void(const QString &)> m_replyNavigation;
    std::function<void(const QModelIndex &, const QString &, const QPoint &)> m_reaction;
    std::function<bool()> m_reactionAllowed;
    mutable QCache<QString, QTextDocument> m_documents{16};
    QPersistentModelIndex m_textIndex;
    int m_textAnchor = 0;
    int m_textPosition = 0;
    int m_textVerticalX = -1;

    static constexpr int compactMessageCap = 570;
    static constexpr int linkCardMinimum = 344;
    static constexpr int linkCardGap = 6;
    static constexpr int messageRowTop = 3;
    static constexpr int messageRowBottom = 4;
    static constexpr int messageBodyTop = 6;
    static constexpr int messageBodyBottom = 5;
    static constexpr int messageMetaPixels = ui::secondaryTextPixels;
    static constexpr int messageHeaderHeight = 20;
    static constexpr int messageForwardHeight = 21;
    static constexpr int messageQuoteLineHeight = 19;
    static constexpr int messageQuoteHeight = 2 * messageQuoteLineHeight + 4;
    static constexpr int messageMediaQuoteHeight = 62;
    static constexpr int galleryWidthLimit = 432;
    static constexpr int galleryGap = 5;
    static constexpr int messageTimeHeight = 19;
    static int messageWidth(int width);
    static int bubbleWidth(int available, const QString &text, int headingWidth, bool block);
    QRect messageBubble(const QRect &row, const QModelIndex &index) const;
    static int messageAvatarSpace(int width);
    int messageBubbleWidth(int outerWidth, const QModelIndex &index) const;
    int messageContext(const QModelIndex &index) const;
    static int messageHeader(const QModelIndex &index);
    QRect replyHeaderRect(const QRect &row, const QModelIndex &index) const;
    int messageTime(const QModelIndex &index) const;
    bool reactionRow(const QModelIndex &index) const;
    static QString deliveryText(const QModelIndex &index);
    static QRect deliveryRect(const QRect &bubble);
    QRect viewersReceipt(const QModelIndex &index, const QRect &row) const;

    struct ReactionTarget {
        QRect rect;
        QString code;
        QString count;
        bool own = false;
    };

    static constexpr int reactionHeight = 23;
    static constexpr int reactionGlyphBox = 19;
    static constexpr int reactionIconPadding = 4;
    static constexpr int reactionCountOffset = reactionIconPadding + reactionGlyphBox + 3;
    static constexpr int reactionCountPadding = 5;
    static constexpr int reactionStart = 9;
    static int reactionBadgeWidth(const QString &count);
    static int reactionDeliverySpace(const QModelIndex &index);
    static int minimumReactionWidth(const QModelIndex &index);
    static QVector<ReactionTarget> reactionTargets(const QModelIndex &index, const QRect &bubble);
    static int messageDay(const QModelIndex &index);
    static int messageDividers(const QModelIndex &index);
    QRect messageContentRect(const QRect &row, const QModelIndex &index) const;

    using LinkCard = QPair<QString, MessageLinkPreview>;
    QVector<LinkCard> linkPreviews(const QModelIndex &index) const;
    static int linkCardHeight(const MessageLinkPreview &preview);
    static const QString &taskLinkDetailSeparator();
    static int taskLinkLabelExtent(const QFontMetrics &labels, const QString &label);
    static int minimumLinkCardWidth(const MessageLinkPreview &preview, int available);
    static int linkDetailLineHeight(const MessageLinkPreview &preview);
    int linkPreviewHeight(const QModelIndex &index) const;
    QRect firstLinkCard(const QRect &row, const QModelIndex &index) const;
    bool eventFilter(QObject *object, QEvent *event) override;
    std::function<void()> rowActionAt(const QModelIndex &index, const QRect &row, const QPoint &point) const;

    std::function<void()> linkActionAt(const QModelIndex &index, const QRect &row, const QPoint &point) const;

    static constexpr int fileTitleTop = 3;
    static constexpr int fileTitleHeight = 20;
    static constexpr int fileControlHeight = 46;
    static QVector<MessageFile> files(const QModelIndex &index, int role);
    bool previewPending(const MessageFile &file) const;
    int previewAngle() const;
    static QRect cardLoadingRing(const QRect &card);
    static QRect mediaLoadingRing(const QRect &frame, const MessageFile &file, bool crop);

    enum class AttachmentSection { Media, Documents };
    static bool isMediaFile(const MessageFile &file);

    struct QuoteAttachments {
        std::optional<MessageFile> representative;
        int media = 0;
        int documents = 0;
        QString firstDocument;
    };
    QuoteAttachments replyAttachments(const QModelIndex &index) const;
    int quoteHeight(const QModelIndex &index) const;
    static QRect quoteMediaRect(const QRect &quote);

    struct FileGeometry {
        QRect thumbnail;
        QRect control;
        QRect bounds() const { return thumbnail.isEmpty() ? control : thumbnail; }
    };

    struct FileTarget { MessageFile file; FileGeometry geometry; };

    template<typename Visit>
    void visitFileGeometry(const QModelIndex &index, const QRect &row, Visit visit) const
    {
        const auto entries = files(index, MessageFiles);
        for (const auto section : {AttachmentSection::Media, AttachmentSection::Documents}) {
            if (layoutFiles(entries, firstFileRect(row, index, section), section, visit, nullptr))
                return;
        }
    }

    template<typename Visit>
    bool layoutFiles(const QVector<MessageFile> &entries, const QRect &area, AttachmentSection section,
                     Visit visit, int *height) const
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

    std::optional<FileTarget> fileAt(const QModelIndex &index, const QRect &row, const QPoint &point) const;
    FileGeometry fileGeometry(const QRect &card, const MessageFile &file) const;
    int fileHeight(const QVector<MessageFile> &entries, int width, AttachmentSection section) const;
    static bool matchesOriginal(const QVector<MessageFile> &entries, const MessageFile &file, bool preview = false);
    QRect photoMenuRect(const QRect &thumbnail, const MessageFile &file) const;
    QRect firstFileRect(const QRect &row, const QModelIndex &index, AttachmentSection section) const;

    static constexpr int noticeGap = 10;
    static constexpr int noticeDayHeight = 30;
    static QRect noticeRect(const QRect &row, const QModelIndex &index);
    static QRect noticeMenuRect(const QRect &row, const QModelIndex &index);
    static int noticeTextWidth(const QRect &card, bool heading);
    static QStringList noticeExcerptLines(const QString &text, int width, bool singleLine);
    static QRect noticeHeaderRect(const QRect &row, const QModelIndex &index, const NoticeRow &notice);
    QRect noticeBodyRect(const QRect &row, const QModelIndex &index) const;
    QRect noticePrimaryRect(const QRect &row, const QModelIndex &index, const NoticeRow &notice) const;
    QTextDocument *bodyDocument(const QModelIndex &index, int width) const;
    QRect selectableTextRect(const QModelIndex &index, const QRect &row) const;
    static int textPosition(const QTextDocument &document, const QRect &rect, const QPoint &point);
    QTextCursor selectionCursor() const;
    void saveTextSelection(const QModelIndex &index, const QTextCursor &cursor);
    int bodyHeight(const QModelIndex &index, const QString &text, int width) const;
    void body(QPainter &p, const QRect &rect, const QModelIndex &index, const QString &text,
              const QColor &color) const;
};

} // namespace bx::workspace
