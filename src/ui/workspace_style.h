#pragma once

#include "core/models.h"
#include "ui/attachment_icons.h"
#include "ui/chat_image_previews.h"
#include "ui/glyphs.h"
#include "ui/loading_indicator.h"
#include "ui/message_emoji.h"
#include "ui/message_markup.h"
#include "ui/reaction_catalog.h"
#include "ui/typography.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QCache>
#include <QColor>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QImage>
#include <QLabel>
#include <QLocale>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyledItemDelegate>
#include <QTextDocument>
#include <QTextLayout>
#include <QToolTip>
#include <QtMath>

#include <algorithm>
#include <functional>
#include <optional>

namespace bx::workspace {

inline const QColor Ink("#182d3b");
inline const QColor Muted("#516674");
inline const QColor Accent("#215c70");
inline const QColor ConversationMetaInk("#3e5663");
inline const QColor Surface("#fffefa");
inline const QColor Selected("#dcecf0");
inline const QColor Border("#dce4e4");

constexpr int Id = Qt::UserRole;
constexpr int Title = Qt::UserRole + 1;
constexpr int Detail = Qt::UserRole + 2;
constexpr int Meta = Qt::UserRole + 3;
constexpr int Count = Qt::UserRole + 4;
constexpr int Outgoing = Qt::UserRole + 5;
constexpr int MarkupHtml = Qt::UserRole + 6;
constexpr int MarkupBlock = Qt::UserRole + 7;
constexpr int DayDivider = Qt::UserRole + 8;
constexpr int UnreadDivider = Qt::UserRole + 9;
constexpr int AvatarUrl = Qt::UserRole + 10;
constexpr int AvatarImage = Qt::UserRole + 11;
constexpr int AvatarType = Qt::UserRole + 12;
constexpr int Pinned = Qt::UserRole + 13;
constexpr int TaskId = Qt::UserRole + 14;
constexpr int MarkedUnread = Qt::UserRole + 15;
constexpr int ForwardByline = Qt::UserRole + 16;
constexpr int ReplyByline = Qt::UserRole + 17;
constexpr int ReplyPreview = Qt::UserRole + 18;
constexpr int NoticePresentation = Qt::UserRole + 19;
constexpr int MessageFiles = Qt::UserRole + 20;
constexpr int ReplyFiles = Qt::UserRole + 21;
constexpr int SystemMessage = Qt::UserRole + 22;
constexpr int HasMention = Qt::UserRole + 23;
constexpr int HasReaction = Qt::UserRole + 24;
constexpr int ProjectAvatarColor = Qt::UserRole + 25;
constexpr int ProjectAvatarVariant = Qt::UserRole + 26;
constexpr int ProjectAvatarType = Qt::UserRole + 27;
constexpr int ReceiptMessageId = Qt::UserRole + 28;
constexpr int ContactOnlineUntil = Qt::UserRole + 29;
constexpr int ContactPosition = Qt::UserRole + 30;
constexpr int ReplyTargetId = Qt::UserRole + 31;
constexpr int RawMessageBody = Qt::UserRole + 37;
constexpr int MessageReactionCounts = Qt::UserRole + 38;
constexpr int OwnMessageReactions = Qt::UserRole + 39;
constexpr int MessageSeen = Qt::UserRole + 40;
constexpr int MessageUnread = Qt::UserRole + 41;
constexpr int MessageCalendarDay = Qt::UserRole + 42;
constexpr int MessageLinks = Qt::UserRole + 43;
constexpr int CountPartial = Qt::UserRole + 44;
constexpr int ChatMessagePreview = Qt::UserRole + 45;
constexpr int NoticeTargets = Qt::UserRole + 46;

struct NoticeRow {
    QString day;
    QString excerpt;
    QString primaryLabel;
    int primaryTarget = -1;
    bool expanded = false;
};
} // namespace bx::workspace

Q_DECLARE_METATYPE(bx::workspace::NoticeRow)

namespace bx::workspace {

inline QString reactionGlyph(const QString &code)
{
    const auto *reaction = ui::findReaction(code);
    return reaction ? QString::fromUtf8(reaction->emoji) : QStringLiteral("•");
}

inline QFont reactionFont(int pixels)
{
    static const auto preferred = [] {
        auto font = QApplication::font();
        font.setFamilies(emojiFontFamilies() + QStringList{font.family()});
        return font;
    }();
    auto font = preferred;
    font.setPixelSize(pixels);
    return font;
}

constexpr int MessageAuthor = Qt::UserRole + 32;
enum class AvatarKind { Person, Chat, Project };

inline void paintOnlineDot(QPainter &painter, const QRect &avatar)
{
    const int size = avatar.width() > 28 ? 11 : 9;
    painter.save();
    painter.setPen(QPen(Surface, 1.5));
    painter.setBrush(QColor("#24875b"));
    painter.drawEllipse(QRect(avatar.right() - size + 1, avatar.bottom() - size + 1, size, size));
    painter.restore();
}

inline const QString UnreadMessages = QStringLiteral("Непрочитанные сообщения");

enum class Row { Directory, Message, Notice, Card, TaskContent };

inline QLabel *label(const QString &text, const char *name, QWidget *parent)
{
    auto *result = new QLabel(text, parent);
    result->setObjectName(QString::fromLatin1(name));
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    return result;
}

inline QFont rowFont(int pixels, bool bold = false)
{
    auto result = QApplication::font();
    result.setPixelSize(pixels);
    result.setWeight(bold ? QFont::DemiBold : QFont::Normal);
    return result;
}

inline bool paintProjectAvatarIcon(QPainter &painter, const QRectF &box, const QString &type, const QColor &color)
{
    QPainterPath icon;
    if (type == QLatin1String("folder")) {
        icon.moveTo(2.5, 14.5);
        icon.lineTo(2.5, 5);
        icon.quadTo(2.5, 3.5, 4, 3.5);
        icon.lineTo(8, 3.5);
        icon.lineTo(10, 6);
        icon.lineTo(16, 6);
        icon.quadTo(17.5, 6, 17.5, 7.5);
        icon.lineTo(17.5, 14.5);
        icon.quadTo(17.5, 16, 16, 16);
        icon.lineTo(4, 16);
        icon.quadTo(2.5, 16, 2.5, 14.5);
    } else if (type == QLatin1String("checks") || type == QLatin1String("tasks")) {
        icon.addRoundedRect(QRectF(3, 3, 14, 14), 2, 2);
        icon.moveTo(6.5, 10);
        icon.lineTo(9, 12.5);
        icon.lineTo(14, 7.5);
    } else if (type == QLatin1String("pie") || type == QLatin1String("chart")) {
        icon.addEllipse(QRectF(2.5, 2.5, 15, 15));
        icon.moveTo(10, 2.5);
        icon.lineTo(10, 10);
        icon.lineTo(17.5, 10);
    } else if (type == QLatin1String("bag") || type == QLatin1String("briefcase")) {
        icon.addRoundedRect(QRectF(2.5, 6.5, 15, 10), 2, 2);
        icon.moveTo(7, 6.5);
        icon.lineTo(7, 4.5);
        icon.quadTo(7, 3.5, 8, 3.5);
        icon.lineTo(12, 3.5);
        icon.quadTo(13, 3.5, 13, 4.5);
        icon.lineTo(13, 6.5);
        icon.moveTo(3, 11);
        icon.lineTo(17, 11);
    } else if (type == QLatin1String("members") || type == QLatin1String("group")) {
        icon.addEllipse(QRectF(4, 4, 5.5, 5.5));
        icon.addEllipse(QRectF(11.5, 4.5, 5, 5));
        icon.moveTo(1.5, 16);
        icon.quadTo(1.5, 11.5, 6.75, 11.5);
        icon.quadTo(12, 11.5, 12, 16);
        icon.moveTo(13, 11.7);
        icon.quadTo(18.5, 11, 18.5, 16);
    } else {
        return false;
    }

    const auto side = qMin(qreal(22), box.width() * 0.62);
    const auto scale = side / 20;
    painter.save();
    painter.translate(box.center() - QPointF(side / 2, side / 2));
    painter.scale(scale, scale);
    painter.setPen(QPen(color, 1.55, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(icon);
    painter.restore();
    return true;
}

inline void paintAvatar(QPainter &painter, const QRect &box, const QString &title,
                        AvatarKind kind = AvatarKind::Person, const QPixmap &photo = {},
                        const QColor &projectColor = {}, const QString &projectVariant = {}, const QString &projectType = {})
{
    const auto words = title.simplified();
    const auto second = words.indexOf(QLatin1Char(' '));
    const auto initials = (words.left(1) + (second >= 0 ? words.mid(second + 1, 1) : QString())).toUpper();
    if (kind == AvatarKind::Project) {
        static const QColor palette[]{QColor("#B15EF5"), QColor("#1F86FF"), QColor("#FAA72C"),
                                      QColor("#F85E9E"), QColor("#02BB9A")};
        quint32 hash = 0;
        for (const auto letter : title)
            hash = hash * 31u + letter.unicode();
        const auto signedHash = static_cast<qint64>(static_cast<qint32>(hash));
        const auto color = projectColor.isValid() ? projectColor : palette[qAbs(signedHash) % std::size(palette)];
        const auto inside = QRectF(box).adjusted(1, 1, -1, -1);
        const auto center = inside.center();
        QPainterPath hexagon;
        hexagon.moveTo(center.x(), inside.top());
        hexagon.lineTo(inside.right(), inside.top() + inside.height() * 0.23);
        hexagon.lineTo(inside.right(), inside.bottom() - inside.height() * 0.23);
        hexagon.lineTo(center.x(), inside.bottom());
        hexagon.lineTo(inside.left(), inside.bottom() - inside.height() * 0.23);
        hexagon.lineTo(inside.left(), inside.top() + inside.height() * 0.23);
        hexagon.closeSubpath();
        painter.save();
        if (photo.isNull()) {
            painter.fillPath(hexagon, color);
            const QColor foreground = qGray(color.rgb()) >= 145 ? QColor("#142427") : Qt::white;
            if (!paintProjectAvatarIcon(painter, inside, projectType, foreground)) {
                painter.setFont(rowFont(box.width() >= 35 ? 13 : 10, true));
                painter.setPen(foreground);
                painter.drawText(box, Qt::AlignCenter, initials);
            }
        } else {
            painter.setClipPath(hexagon);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawPixmap(box, photo);
            painter.setClipping(false);
        }
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(projectVariant == QLatin1String("guest") ? QColor("#28645c") : color.darker(130), 1));
        painter.drawPath(hexagon);
        painter.restore();
        return;
    }
    if (!photo.isNull()) {
        painter.save();
        QPainterPath clip;
        clip.addEllipse(QRectF(box));
        painter.setClipPath(clip);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawPixmap(box, photo);
        painter.restore();
        return;
    }
    struct Tone { const char *back; const char *ink; };
    static constexpr Tone tones[] = {
        {"#d9ebe5", "#245f57"}, {"#dbe8ef", "#275672"}, {"#efe4d7", "#754d32"},
        {"#e9e1ef", "#634b76"}, {"#ebead4", "#575a32"},
    };
    uint index = 0;
    for (auto letter : title)
        index = index * 31 + letter.unicode();
    const auto tone = tones[index % (sizeof(tones) / sizeof(Tone))];
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(tone.back));
    painter.drawEllipse(box);
    if (kind != AvatarKind::Person) {
        painter.save();
        painter.translate(QRectF(box).center());
        painter.scale(box.width() / 32.0, box.height() / 32.0);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(tone.ink), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPainterPath shape;
        shape.moveTo(-6, -5); shape.lineTo(6, -5); shape.lineTo(6, 3); shape.lineTo(-1, 3);
        shape.lineTo(-5, 6); shape.lineTo(-5, 3); shape.lineTo(-6, 3); shape.closeSubpath();
        painter.drawPath(shape);
        painter.restore();
        return;
    }
    painter.setPen(QColor(tone.ink));
    painter.setFont(rowFont(box.width() >= 35 ? 13 : 12, true));
    painter.drawText(box, Qt::AlignCenter, initials);
}

inline QListWidgetItem *addRow(QListWidget *list, const QString &title, const QString &detail = {},
                              const QString &meta = {}, const QString &id = {}, bool showTextToolTip = true)
{
    auto *item = new QListWidgetItem(list);
    QStringList accessible;
    for (const auto &part : {title, detail, meta}) {
        if (!part.isEmpty())
            accessible.append(part);
    }
    item->setText(accessible.join(QStringLiteral("\n")));
    item->setData(Id, id);
    item->setData(Title, title);
    item->setData(Detail, detail);
    item->setData(Meta, meta);
    if (showTextToolTip)
        item->setToolTip(ui::plainTooltip(item->text()).replace(QLatin1Char('\n'), QStringLiteral("<br>")));
    return item;
}

inline QListWidgetItem *addMarkupRow(QListWidget *list, const QString &title, const QString &raw,
                                    const QString &meta = {}, const QString &id = {})
{
    const auto markup = parseMessageMarkup(raw);
    auto *item = addRow(list, title, markup.plain, meta, id, false);
    if (!markup.html.isEmpty()) {
        item->setData(MarkupHtml, markup.html);
        item->setData(MarkupBlock, markup.block);
    }
    if (!markup.links.isEmpty())
        item->setData(MessageLinks, markup.links);
    return item;
}

inline void emptyRow(QListWidget *list, const QString &message)
{
    auto *item = addRow(list, QString(), message);
    item->setFlags(Qt::ItemIsEnabled);
}

inline void loadingRow(QListWidget *list, const QString &message)
{
    auto *item = new QListWidgetItem(list);
    item->setFlags(Qt::ItemIsEnabled);
    item->setData(Qt::AccessibleTextRole, message);
    auto *caption = label(message, "quiet", list);
    caption->setContentsMargins(10, 2, 8, 2);
    ui::setLoadingIndicator(caption, true);
    item->setSizeHint(caption->sizeHint().expandedTo(QSize(0, 36)));
    list->setItemWidget(item, caption);
}

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
    QListWidget *m_view;
    Row m_style;
    std::function<void(const QModelIndex &, int)> m_noticeAction;
    std::function<void(const QModelIndex &)> m_noticeToggle;
    std::function<void(const QModelIndex &, const QPoint &)> m_noticeMenu;
    std::function<void(const MessageFile &)> m_messageDownload;
    std::function<void(const QString &)> m_messageLink;
    std::function<const QPixmap *(const QUrl &)> m_messagePreview;
    std::function<bool(const MessageFile &)> m_previewPending;
    QWidget *m_previewViewport = nullptr;
    QPointer<QWidget> m_previewWindow;
    QPointer<ui::LoadingClock> m_previewClock;
    bool m_previewAnimation = false;
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
    void paintLinkProjectAvatar(QPainter &p, const QRect &box, const Project &project) const;
    void paintLinkIcon(QPainter &p, const QRect &box, const MessageLinkPreview &preview) const;
    void paintLinkCards(QPainter &p, const QRect &row, const QModelIndex &index) const;
    int paintTaskLinkDetailIcon(QPainter &p, const QRect &row, const QString &label,
                                const MessageLinkPreview &preview) const;
    void paintLinkDetails(QPainter &p, const QRect &area, const MessageLinkPreview &preview) const;
    bool eventFilter(QObject *object, QEvent *event) override;
    std::function<void()> rowActionAt(const QModelIndex &index, const QRect &row, const QPoint &point) const
    {
        if (m_style == Row::Notice) {
            const auto notice = index.data(NoticePresentation).value<NoticeRow>();
            const auto menu = noticeMenuRect(row, index);
            if (m_noticeMenu && menu.contains(point))
                return [this, index, menu] { m_noticeMenu(index, menu.bottomLeft()); };
            if (m_noticeToggle && noticeHeaderRect(row, index, notice).contains(point))
                return [this, index] { m_noticeToggle(index); };
            if (m_noticeAction && notice.expanded && noticePrimaryRect(row, index, notice).contains(point))
                return [this, index, target = notice.primaryTarget] { m_noticeAction(index, target); };
            return linkActionAt(index, row, point);
        }
        const auto receipt = viewersReceipt(index, row);
        if (receipt.contains(point))
            return [this, id = index.data(ReceiptMessageId).toString(), receipt] {
                m_messageViewers(id, m_view->viewport()->mapToGlobal(receipt.bottomLeft()));
            };
        if (canReact(index)) {
            for (const auto &target : reactionTargets(index, messageBubble(row, index))) {
                if (target.rect.contains(point))
                    return [this, index, target] {
                        m_reaction(index, target.code, m_view->viewport()->mapToGlobal(target.rect.bottomLeft()));
                    };
            }
        }
        if (m_messageDownload || m_messagePhoto || m_messagePhotoMenu || m_messageVideo) {
            if (const auto target = fileAt(index, row, point)) {
                const auto &[file, geometry] = *target;
                const auto menu = photoMenuRect(geometry.thumbnail, file);
                if (menu.contains(point)) {
                    const QRect anchor(m_view->viewport()->mapToGlobal(menu.topLeft()), menu.size());
                        return [this, index, file = file, anchor] { m_messagePhotoMenu(index, file, anchor); };
                }
                const bool titleControl = geometry.thumbnail.isEmpty()
                    && point.y() < geometry.control.top() + fileTitleTop + fileTitleHeight;
                if (file.isVideo && m_messageVideo && downloadable(file)
                    && (geometry.thumbnail.contains(point) || titleControl)) {
                    return [this, index, file = file] { m_messageVideo(index, file); };
                }
                const bool originalControl = file.isImage && titleControl && downloadable(file);
                const bool opensThumbnail = geometry.thumbnail.contains(point) && (filePreview(file) || downloadable(file));
                if (!file.isVideo && m_messagePhoto && (opensThumbnail || originalControl))
                    return [this, file = file] { m_messagePhoto(file); };
                if (m_messageDownload && geometry.control.contains(point) && downloadable(file))
                    return [this, file = file] { m_messageDownload(file); };
            }
        }
        const auto id = index.data(ReplyTargetId).toString();
        if (m_replyNavigation && !id.isEmpty() && replyHeaderRect(row, index).contains(point))
            return [this, id] { m_replyNavigation(id); };
        return linkActionAt(index, row, point);
    }

    std::function<void()> linkActionAt(const QModelIndex &index, const QRect &row, const QPoint &point) const
    {
        const auto target = linkAt(index, row, point);
        if (!target.isEmpty()) {
            if (m_messageLink)
                return [this, target] { m_messageLink(target); };
            const auto url = messageWebLink(target);
            if (!url.isEmpty())
                return [url] { QDesktopServices::openUrl(url); };
        }
        return {};
    }

    static constexpr int fileTitleTop = 3;
    static constexpr int fileTitleHeight = 20;
    static constexpr int fileControlHeight = 46;
    static constexpr int FileRowStep = fileControlHeight + 5;
    static QVector<MessageFile> files(const QModelIndex &index, int role);
    void updatePreviewClock();
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
    void paintMediaFrame(QPainter &p, const QRect &frame, const MessageFile &file, bool crop = false) const;
    void paintMediaTile(QPainter &p, const QRect &thumbnail, const MessageFile &file) const;
    void paintFiles(QPainter &p, const QRect &start, const QVector<MessageFile> &entries, AttachmentSection section) const;
    void paintQuote(QPainter &p, const QRect &quote, const QModelIndex &index, bool outgoing) const;

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
    static int wrappedText(QString text, int width, const QFont &font,
                           QPainter *painter = nullptr, const QPoint &position = {});
    static void fill(QPainter &p, const QRect &rect, const QColor &background,
                     const QColor &outline = Qt::transparent, int radius = 8);
    static void line(QPainter &p, const QRect &rect, const QString &text, int pixels,
                     const QColor &color = Ink, bool bold = false,
                     Qt::Alignment alignment = Qt::AlignLeft | Qt::AlignVCenter);
    static void paragraph(QPainter &p, const QRect &rect, const QString &text, int pixels,
                          const QColor &color = Ink, bool bold = false);
    static void messagePreviewLine(QPainter &painter, const QRect &rect, const QString &text, const QColor &color);
    void body(QPainter &p, const QRect &rect, const QModelIndex &index, const QString &text,
              const QColor &color) const;
    static QColor itemOutline(const QStyleOptionViewItem &option, const QColor &normal = Qt::transparent);
    static void paintDirectory(QPainter &p, const QStyleOptionViewItem &option, const QModelIndex &index,
                               const QString &title, const QString &detail, const QString &meta);
    void paintMessage(QPainter &p, const QStyleOptionViewItem &option, const QModelIndex &index,
                      const QString &title, const QString &detail) const;
    void paintNotice(QPainter &p, const QStyleOptionViewItem &option, const QModelIndex &index,
                     const QString &title, const QString &detail, const QString &meta) const;
    static void paintCard(QPainter &p, const QStyleOptionViewItem &option, const QString &title,
                          const QString &detail);
};

inline RowPainter *configureRows(QListWidget *list, Row style)
{
    list->setFrameShape(QFrame::NoFrame);
    list->setMouseTracking(true);
    list->setResizeMode(QListView::Adjust);
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *delegate = new RowPainter(list, style);
    list->setItemDelegate(delegate);
    return delegate;
}

inline QString styleSheet()
{
    return QStringLiteral(R"(
        QMainWindow, QWidget#workspace { background: #f4f4ef; color: #182d3b; }
        QWidget#rail { background: #193742; }
        QLabel#brand { color: #e8f6f0; background: #31525e; border-radius: 9px;
                       font-size: 14px; font-weight: 700; }
        QLabel#mode { color: #b2c8ce; font-size: 14px; }
        QLabel#mode[online="true"] { color: #b8ded2; }
        QListWidget, QPlainTextEdit { background: transparent; color: #182d3b; border: 0; }
        QListWidget#sections { color: #dce8ec; outline: 0; }
        QWidget#directory { background: #f6f7f1; border-right: 1px solid #dbe6e0; }
        QTabBar#chatScopeTabs::tab, QTabBar#projectScopeTabs::tab {
            color: #42606a; background: transparent; border: 1px solid transparent;
            min-width: 38px; padding: 6px 5px; margin: 0 2px 2px 0; border-radius: 6px;
            font-weight: 600;
        }
        QTabBar#chatScopeTabs::tab:hover, QTabBar#projectScopeTabs::tab:hover { background: #ebf1ee; }
        QTabBar#chatScopeTabs::tab:selected, QTabBar#projectScopeTabs::tab:selected {
            color: #215c70; background: #e1efec; border-color: #d3e4df;
        }
        QTabBar#chatScopeTabs::tab:focus, QTabBar#projectScopeTabs::tab:focus { border-color: #215c70; }
        QTabBar#chatScopeTabs QLabel#chatScopeUnreadBadge {
            color: white; background: #aa4c58; border-radius: 8px; font-size: 14px; font-weight: 700;
        }
        QSplitter#chatDirectorySplitter::handle:hover { background: #83aba5; }
        QWidget#conversationHeader { background: #fffefa; border-bottom: 1px solid #d9e5df; }
        QLabel#conversationTitle { color: #182d3b; font-size: 18px; font-weight: 650; }
        QWidget#conversationSearch { background: #f6f9f4; border-bottom: 1px solid #d9e5df; }
        QFrame#chatComposer { background: #fffefa; border-top: 1px solid #d2e2da; }
        QPlainTextEdit#chatEditor { background: #fffefa; padding: 4px 5px;
                                   border: 1px solid transparent; border-radius: 7px;
                                   selection-background-color: #215c70;
                                   selection-color: #fffefa; }
        QPlainTextEdit#chatEditor:focus { border-color: #8fb6aa; }
        QWidget#reading, QWidget#taskPane, QWidget#taskDetail { background: #fffefa; }
        QWidget#taskPane { border: 1px solid #dce4e4; border-radius: 10px; }
        QWidget#column { background: #e8edeb; border-radius: 10px; }
        QLabel { color: #182d3b; }
        QLabel#pageTitle { font-size: 22px; font-weight: 650; }
        QLabel#sectionTitle { font-size: 18px; font-weight: 650; }
        QLabel#columnTitle { font-weight: 650; }
        QLabel#quiet, QLabel#status { color: #516674; font-size: 14px; }
        QLabel#empty { color: #516674; }
        QLabel#stageCount { color: #355568; background: #dce5e4; border-radius: 8px; font-size: 14px; }
        QPushButton { color: #294b59; background: #f5f8f5; border: 1px solid #c8d5d4; border-radius: 7px; padding: 6px 12px; font-weight: 600; }
        QPushButton:hover { background: #e7f0ef; }
        QPushButton:focus { border-color: #215c70; }
        QPushButton#primary { color: white; background: #215c70; border-color: #215c70; }
        QPushButton#primary:hover { background: #194d5f; }
        QPushButton#primary:focus { border-color: #a8d9e5; }
        QPushButton:disabled, QPushButton#primary:disabled { color: #50626a; background: #e6edec; border-color: #d3dedc; }
        QScrollArea { background: transparent; border: 0; }
        QSplitter::handle { background: #dce4e4; }
        QScrollBar:vertical { width: 9px; background: transparent; margin: 2px; }
        QScrollBar:horizontal { height: 9px; background: transparent; margin: 2px; }
        QScrollBar::handle { background: #9fb3b8; border-radius: 3px; min-height: 24px; min-width: 24px; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
        QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
    )");
}

} // namespace bx::workspace
