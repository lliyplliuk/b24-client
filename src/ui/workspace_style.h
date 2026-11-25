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
constexpr int NoticeActions = Qt::UserRole + 19;
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
        item->setToolTip(QStringLiteral("<qt>%1</qt>").arg(item->text().toHtmlEscaped().replace(
            QLatin1Char('\n'), QStringLiteral("<br>"))));
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
    RowPainter(QListWidget *view, Row style) : QStyledItemDelegate(view), m_view(view), m_style(style)
    {
        if (m_style == Row::Message || m_style == Row::TaskContent || m_style == Row::Notice) {
            m_view->viewport()->setMouseTracking(true);
            m_view->viewport()->installEventFilter(this);
        }
        if (m_style == Row::Message || m_style == Row::TaskContent) {
            m_previewViewport = m_view->viewport();
            connect(m_previewViewport, &QObject::destroyed, this, [this] {
                if (m_previewClock)
                    m_previewClock->unwatch(m_previewViewport);
                m_previewClock.clear();
                m_previewViewport = nullptr;
            });
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
    ~RowPainter() override
    {
        if (m_previewClock)
            m_previewClock->unwatch(m_previewViewport);
    }

    void setNoticeActionHandler(std::function<void(const QModelIndex &, int)> handler)
    {
        m_noticeAction = std::move(handler);
    }
    void setMessageDownloadHandler(std::function<void(const MessageFile &)> handler)
    {
        m_messageDownload = std::move(handler);
    }
    void setMessageLinkHandler(std::function<void(const QString &)> handler)
    {
        m_messageLink = std::move(handler);
    }
    void setMessagePreviewSource(std::function<const QPixmap *(const QUrl &)> source)
    {
        m_messagePreview = std::move(source);
    }

    void setPreviewPendingSource(std::function<bool(const MessageFile &)> source)
    {
        m_previewPending = std::move(source);
    }

    const QPixmap *preview(const MessageFile &file) const
    {
        if (file.isVideo)
            return m_videoPreview ? m_videoPreview(file) : nullptr;
        if (!m_messagePreview)
            return nullptr;
        const auto *photo = m_messagePreview(file.previewUrl);
        if (!photo || photo->isNull())
            photo = m_messagePreview(file.originalPreviewUrl);
        return photo && !photo->isNull() ? photo : nullptr;
    }

    void setPreviewAnimation(bool active)
    {
        if (m_previewViewport && (m_previewAnimation || active)) {
            m_previewAnimation = active;
            updatePreviewClock();
            m_previewViewport->update();
        }
    }

    bool hasVisibleLoadingPreview(const QModelIndex &index, const QRect &row, const QRect &visible) const
    {
        bool pending = false;
        visitFileGeometry(index, row, [&](const MessageFile &file, const FileGeometry &geometry) {
            if (!previewPending(file) || (!geometry.thumbnail.isEmpty() && preview(file)))
                return false;
            const auto ring = geometry.thumbnail.isEmpty() ? cardLoadingRing(geometry.control)
                : mediaLoadingRing(geometry.thumbnail, file, false);
            pending = ring.intersects(visible);
            return pending;
        });
        if (pending)
            return true;
        const auto representative = visibleQuoteMedia(index, row, visible);
        return representative && previewPending(*representative) && !preview(*representative)
            && mediaLoadingRing(quoteMediaRect(replyHeaderRect(row, index)), *representative, true).intersects(visible);
    }

    void setVideoPreviewSource(std::function<const QPixmap *(const MessageFile &)> source)
    {
        m_videoPreview = std::move(source);
    }

    void setLinkThumbnailSource(std::function<const QImage *(const QUrl &)> source)
    {
        m_linkThumbnail = std::move(source);
    }
    void setLinkPreviewSource(std::function<MessageLinkPreview(const QModelIndex &, const QString &)> source)
    {
        m_linkPreview = std::move(source);
    }
    void setLinkAvatarSource(std::function<const QPixmap *(const QUrl &)> source)
    {
        m_linkAvatar = std::move(source);
    }
    void setMessagePhotoHandler(std::function<void(const MessageFile &)> handler)
    {
        m_messagePhoto = std::move(handler);
    }
    void setMessageVideoHandler(std::function<void(const QModelIndex &, const MessageFile &)> handler)
    {
        m_messageVideo = std::move(handler);
    }
    void setMessagePhotoMenuHandler(std::function<void(const QModelIndex &, const MessageFile &, const QRect &)> handler)
    {
        m_messagePhotoMenu = std::move(handler);
    }
    QVector<MessageFile> visibleVideos(const QModelIndex &index, const QRect &row, const QRect &visible) const
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

    std::optional<MessageFile> visibleQuoteMedia(const QModelIndex &index, const QRect &row, const QRect &visible) const
    {
        if (index.data(ReplyByline).toString().isEmpty() ||
            !quoteMediaRect(replyHeaderRect(row, index)).intersects(visible))
            return {};
        return replyAttachments(index).representative;
    }
    bool showPhotoMenuAt(const QModelIndex &index, const QPoint &point) const
    {
        if (!m_messagePhotoMenu || !index.isValid())
            return false;
        const auto target = fileAt(index, m_view->visualRect(index), point);
        if (!target || !target->geometry.thumbnail.contains(point)
            || photoMenuRect(target->geometry.thumbnail, target->file).isEmpty())
            return false;
        const auto global = m_view->viewport()->mapToGlobal(point);
        m_messagePhotoMenu(index, target->file, QRect(global, QSize(1, 1)));
        return true;
    }
    QVector<MessageFile> photoOriginals(const QModelIndex &index) const
    {
        QVector<MessageFile> result;
        for (const auto &file : files(index, MessageFiles)) {
            if (!file.isVideo && preview(file) && downloadable(file) && !matchesOriginal(result, file))
                result.append(file);
        }
        return result;
    }
    static bool hasOriginal(const QModelIndex &index, const MessageFile &file)
    {
        return matchesOriginal(files(index, MessageFiles), file) || matchesOriginal(files(index, ReplyFiles), file);
    }
    static bool hasPhotoPreview(const QModelIndex &index, const MessageFile &file)
    {
        return matchesOriginal(files(index, MessageFiles), file, true)
            || matchesOriginal(files(index, ReplyFiles), file, true);
    }
    static bool downloadable(const MessageFile &file)
    {
        return file.downloadUrl.isValid() && file.downloadUrl.scheme() == QStringLiteral("https") &&
               !file.downloadUrl.host().isEmpty() && file.downloadUrl.userInfo().isEmpty();
    }
    void setMessageViewersHandler(std::function<void(const QString &, const QPoint &)> handler)
    {
        m_messageViewers = std::move(handler);
    }
    // Hit test of the drawn message body: returns the href under the viewport point.
    QString anchorAt(const QModelIndex &index, const QRect &row, const QPoint &viewportPos) const
    {
        const auto rect = m_style == Row::Notice ? noticeBodyRect(row, index) : messageContentRect(row, index);
        if (!rect.contains(viewportPos))
            return {};
        const auto *document = bodyDocument(index, rect.width());
        if (!document)
            return {};
        return document->documentLayout()->anchorAt(viewportPos - rect.topLeft());
    }

    QString linkAt(const QModelIndex &index, const QRect &row, const QPoint &point) const
    {
        if (const auto href = anchorAt(index, row, point); !href.isEmpty())
            return href;
        auto card = firstLinkCard(row, index);
        for (const auto &[href, preview] : linkPreviews(index)) {
            card.setHeight(linkCardHeight(preview));
            if (card.contains(point))
                return href;
            card.translate(0, card.height() + linkCardGap);
        }
        return {};
    }

    QRect receiptArea(const QModelIndex &index, const QRect &row) const
    {
        return m_style == Row::Notice ? noticeRect(row) : messageBubble(row, index);
    }

    void setReplyNavigation(std::function<void(const QString &)> handler)
    {
        m_replyNavigation = std::move(handler);
    }

    void setReactionActions(std::function<void(const QModelIndex &, const QString &, const QPoint &)> action,
                            std::function<bool()> allowed)
    {
        m_reaction = std::move(action);
        m_reactionAllowed = std::move(allowed);
    }

    enum class TextPick { Position, Extend, Word };

    struct TextSelection {
        QString messageId;
        QString detail;
        QString markup;
        int anchor;
        int position;
        int verticalX;
    };

    std::optional<TextSelection> snapshotTextSelection() const
    {
        const auto id = m_textIndex.data(ReceiptMessageId).toString();
        if (id.isEmpty())
            return std::nullopt;
        return TextSelection{id, m_textIndex.data(Detail).toString(), m_textIndex.data(MarkupHtml).toString(),
                             m_textAnchor, m_textPosition, m_textVerticalX};
    }

    void restoreTextSelection(const QModelIndex &index, const TextSelection &selection)
    {
        if (index.data(Detail).toString() != selection.detail || index.data(MarkupHtml).toString() != selection.markup)
            return;
        m_textIndex = index;
        m_textAnchor = selection.anchor;
        m_textPosition = selection.position;
        m_textVerticalX = selection.verticalX;
        m_view->viewport()->update(m_view->visualRect(index));
    }

    bool pickMessageText(const QModelIndex &index, const QPoint &point, TextPick pick)
    {
        const auto rect = selectableTextRect(index, m_view->visualRect(index));
        if (!rect.contains(point))
            return false;
        auto *document = bodyDocument(index, rect.width());
        if (!document)
            return false;
        QTextCursor cursor(document);
        if (pick == TextPick::Extend && m_textIndex == index) {
            cursor.setPosition(m_textAnchor);
            cursor.setPosition(textPosition(*document, rect, point), QTextCursor::KeepAnchor);
        } else {
            cursor.setPosition(textPosition(*document, rect, point));
            if (pick == TextPick::Word)
                cursor.select(QTextCursor::WordUnderCursor);
        }
        saveTextSelection(index, cursor);
        return true;
    }

    void extendTextSelection(const QPoint &point)
    {
        auto cursor = selectionCursor();
        if (cursor.isNull())
            return;
        const auto rect = selectableTextRect(m_textIndex, m_view->visualRect(m_textIndex));
        cursor.setPosition(textPosition(*cursor.document(), rect, point), QTextCursor::KeepAnchor);
        saveTextSelection(m_textIndex, cursor);
    }

    bool hasTextCursor() const { return m_textIndex.isValid(); }

    void clearTextSelection()
    {
        const auto old = m_textIndex;
        m_textIndex = QPersistentModelIndex{};
        if (old.isValid())
            m_view->viewport()->update(m_view->visualRect(old));
    }

    QString selectedText(const QModelIndex &index = {}) const
    {
        if (index.isValid() && m_textIndex != index)
            return {};
        auto text = selectionCursor().selectedText();
        return text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'))
                   .replace(QChar::LineSeparator, QLatin1Char('\n'));
    }

    void selectMessageText()
    {
        auto cursor = selectionCursor();
        if (!cursor.isNull()) {
            cursor.select(QTextCursor::Document);
            saveTextSelection(m_textIndex, cursor);
        }
    }

    void moveTextSelection(QTextCursor::MoveOperation operation)
    {
        auto cursor = selectionCursor();
        if (!cursor.isNull()) {
            cursor.setVerticalMovementX(m_textVerticalX);
            cursor.movePosition(operation, QTextCursor::KeepAnchor);
            saveTextSelection(m_textIndex, cursor);
        }
    }

    bool activateTextLink(const QString &href, const QPoint &point) const
    {
        if (href.isEmpty() || !m_textIndex.isValid())
            return false;
        const auto rect = m_view->visualRect(m_textIndex);
        if (anchorAt(m_textIndex, rect, point) == href) {
            if (auto action = linkActionAt(m_textIndex, rect, point)) {
                action();
                return true;
            }
        }
        return false;
    }

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const override
    {
        const int width = m_view->viewport()->width();
        if (m_style == Row::Directory)
            return QSize(0, 56);
        const auto detail = index.data(Detail).toString();
        const auto title = index.data(Title).toString();
        if (m_style == Row::Message || m_style == Row::TaskContent) {
            const int bubbleWidth = messageBubbleWidth(width, index);
            const int bodyWidth = index.data(Id).toString().isEmpty() ? width - 16 : bubbleWidth - 24;
            const auto entries = files(index, MessageFiles);
            const int textHeight = detail.isEmpty() && !entries.isEmpty() ? 0 : bodyHeight(index, detail, bodyWidth);
            return QSize(0, messageRowTop + messageRowBottom + messageBodyTop + messageBodyBottom
                             + textHeight + messageHeader(index) +
                             messageContext(index) + fileHeight(entries, bubbleWidth - 20, AttachmentSection::Media)
                             + fileHeight(entries, bubbleWidth - 20, AttachmentSection::Documents) +
                             linkPreviewHeight(index) +
                             messageTime(index) + messageDividers(index));
        }
        if (m_style == Row::Notice) {
            const QRect row(0, 0, width, 0);
            const auto buttons = noticeActionRects(row, index);
            const int previews = linkPreviewHeight(index);
            const int bottom = buttons.isEmpty()
                ? noticeBodyRect(row, index).bottom() + (previews > 0 ? previews + 5 : 0)
                : buttons.back().bottom();
            return QSize(0, qMax(35, bottom + 8));
        }
        const int height = wrappedText(title, qMax(75, width - 27), rowFont(ui::bodyTextPixels, true));
        return QSize(0, 25 + qMin(45, height) + (detail.isEmpty() ? 0 : 19));
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
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
            paintMessage(*painter, option, index, title, detail);
        else if (m_style == Row::Notice)
            paintNotice(*painter, option, index, title, detail, meta);
        else
            paintCard(*painter, option, title, detail);
        painter->restore();
    }

    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
                     const QModelIndex &index) override
    {
        if ((m_style == Row::Message || m_style == Row::TaskContent || m_style == Row::Notice) &&
            event->type() == QEvent::MouseButtonRelease) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton) {
                if (auto action = rowActionAt(index, option.rect, mouse->position().toPoint())) {
                    action();
                    return true;
                }
            }
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }

    bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
                   const QModelIndex &index) override
    {
        if (event && event->type() == QEvent::ToolTip) {
            if (const auto target = fileAt(index, option.rect, event->pos())) {
                const auto &geometry = target->geometry;
                auto bounds = geometry.thumbnail.isEmpty() ? geometry.control : geometry.thumbnail;
                auto hint = QStringLiteral("<span style=\"white-space: pre-wrap\">%1</span>")
                                .arg(target->file.name.toHtmlEscaped());
                const auto menu = photoMenuRect(geometry.thumbnail, target->file);
                if (menu.contains(event->pos())) {
                    bounds = menu;
                    hint.prepend(target->file.isVideo ? QStringLiteral("Сохранить видео<br>")
                                                      : QStringLiteral("Действия с изображением<br>"));
                }
                QToolTip::showText(event->globalPos(), QStringLiteral("<qt>%1</qt>").arg(hint), view->viewport(), bounds);
                return true;
            }
            const auto quote = replyHeaderRect(option.rect, index);
            if (!index.data(ReplyByline).toString().isEmpty() && quote.contains(event->pos())) {
                const auto attachments = replyAttachments(index);
                if (!attachments.representative && attachments.documents == 1) {
                    QToolTip::showText(event->globalPos(), QStringLiteral("<qt><span style=\"white-space: pre-wrap\">%1</span></qt>")
                        .arg(attachments.firstDocument.toHtmlEscaped()), view->viewport(), quote);
                    return true;
                }
            }
            const auto receipt = viewersReceipt(index, option.rect);
            if (receipt.contains(event->pos())) {
                QToolTip::showText(event->globalPos(), QStringLiteral("Кто прочитал сообщение"),
                                   view->viewport(), receipt);
                return true;
            }
        }
        if (event && event->type() == QEvent::ToolTip && canReact(index)) {
            for (const auto &target : reactionTargets(index, messageBubble(option.rect, index))) {
                if (!target.rect.contains(event->pos()))
                    continue;
                const auto *reaction = ui::findReaction(target.code);
                const auto title = reaction ? QString::fromUtf8(reaction->caption) : target.code;
                const auto hint = (target.own ? QStringLiteral("Снять реакцию «%1»")
                                               : QStringLiteral("Поставить реакцию «%1»")).arg(title);
                QToolTip::showText(event->globalPos(), QStringLiteral("<qt>%1</qt>").arg(hint.toHtmlEscaped()),
                                   view->viewport(), target.rect);
                return true;
            }
        }
        return QStyledItemDelegate::helpEvent(event, view, option, index);
    }

    // Enter opens the same media as a click on its preview.
    bool activateMedia(const QModelIndex &index)
    {
        if (!m_view)
            return false;
        if (m_style != Row::Message && m_style != Row::TaskContent)
            return false;
        for (const auto &file : files(index, MessageFiles)) {
            if (file.isVideo && m_messageVideo && downloadable(file)) {
                m_messageVideo(index, file);
                return true;
            }
            if (!file.isVideo && m_messagePhoto && (preview(file) || (file.isImage && downloadable(file)))) {
                m_messagePhoto(file);
                return true;
            }
        }
        return false;
    }

private:
    QListWidget *m_view;
    Row m_style;
    std::function<void(const QModelIndex &, int)> m_noticeAction;
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

    static int messageWidth(int width)
    {
        const int desktopCap = qBound(compactMessageCap, width * 2 / 3, 780);
        return qBound(1, qMax(96, qMin(desktopCap, width * 80 / 100)), qMax(1, width - 12));
    }

    static int bubbleWidth(int available, const QString &text, int headingWidth, bool block)
    {
        const QFontMetrics metrics(rowFont(ui::bodyTextPixels));
        const int preferred = qMax(metrics.horizontalAdvance(text), headingWidth) + 28;
        if (block)
            return qMin(available, qMax(compactMessageCap, preferred));
        return qBound(qMin(available, 120), preferred, available);
    }

    QRect messageBubble(const QRect &row, const QModelIndex &index) const
    {
        const int width = messageBubbleWidth(row.width(), index);
        const int x = row.left() + 4 + (m_style == Row::TaskContent ? 0 : messageAvatarSpace(row.width()));
        const int dividers = messageDividers(index);
        return QRect(x, row.top() + dividers + messageRowTop, width,
                     row.height() - dividers - messageRowTop - messageRowBottom);
    }

    static int messageAvatarSpace(int width)
    {
        return width >= 250 ? 33 : 0;
    }

    int messageBubbleWidth(int outerWidth, const QModelIndex &index) const
    {
        if (m_style == Row::TaskContent)
            return qMax(1, outerWidth - 8);
        const int contentWidth = outerWidth - messageAvatarSpace(outerWidth);
        const int available = qMin(messageWidth(contentWidth), qMax(1, contentWidth - messageActionSize - messageActionGap - 6));
        const QFontMetrics measure(rowFont(ui::secondaryTextPixels, true));
        int headingWidth = measure.horizontalAdvance(index.data(Title).toString());
        for (const auto role : {ForwardByline, ReplyByline}) {
            const auto content = measure.elidedText(index.data(role).toString(), Qt::ElideRight, 260);
            headingWidth = qMax(headingWidth, measure.horizontalAdvance(content));
        }
        const auto reply = index.data(ReplyPreview).toString();
        if (!reply.isEmpty()) {
            const int replyWidth = available > compactMessageCap ? available - 44 : 260;
            headingWidth = qMax(headingWidth, measure.horizontalAdvance(measure.elidedText(reply, Qt::ElideRight, replyWidth)));
        }
        const bool mediaQuote = !reply.isEmpty() && replyAttachments(index).representative.has_value();
        if (mediaQuote)
            headingWidth += 55; // Thumbnail and its spacing sit inside the quotation's text allowance.
        const auto detail = index.data(Detail).toString();
        const int contentLimit = detail.isEmpty() && reply.isEmpty() ? qMin(available, compactMessageCap) : available;
        int result = qMax(bubbleWidth(contentLimit, detail, headingWidth, index.data(MarkupBlock).toBool()),
                          qMin(available, minimumReactionWidth(index)));
        if (mediaQuote)
            result = qMax(result, qMin(available, 285));
        for (const auto &card : linkPreviews(index))
            result = qMax(result, minimumLinkCardWidth(card.second, available));

        const auto entries = files(index, MessageFiles);
        const auto adjacentMedia = std::adjacent_find(entries.cbegin(), entries.cend(), [](const auto &first, const auto &second) {
            return isMediaFile(first) && isMediaFile(second);
        });
        if (adjacentMedia != entries.cend())
            return qMax(result, qMin(galleryWidthLimit + 20, available));
        if (std::any_of(entries.cbegin(), entries.cend(), isMediaFile))
            result = qMax(result, qMin(linkCardMinimum, available));
        return entries.isEmpty() ? result : qMax(result, qMin(280, available));
    }

    int messageContext(const QModelIndex &index) const
    {
        return (index.data(ForwardByline).toString().isEmpty() ? 0 : messageForwardHeight) +
               (index.data(ReplyByline).toString().isEmpty() ? 0 : quoteHeight(index) + 5);
    }

    static int messageHeader(const QModelIndex &index)
    {
        return index.data(Title).toString().isEmpty() ? 0 : messageHeaderHeight;
    }

    QRect replyHeaderRect(const QRect &row, const QModelIndex &index) const
    {
        const auto bubble = messageBubble(row, index);
        const int afterForward = index.data(ForwardByline).toString().isEmpty() ? 0 : messageForwardHeight;
        return QRect(bubble.left() + 10, bubble.top() + 5 + messageHeader(index) + afterForward,
                     bubble.width() - 20, quoteHeight(index));
    }

    int messageTime(const QModelIndex &index) const
    {
        if (m_style != Row::Message)
            return index.data(Meta).toString().isEmpty() ? 0 : messageTimeHeight;
        if (!index.data(SystemMessage).toBool() && !index.data(MessageReactionCounts).toMap().isEmpty())
            return reactionHeight + 2;
        return deliveryText(index).isEmpty() ? 0 : messageTimeHeight;
    }

    bool reactionRow(const QModelIndex &index) const
    {
        return m_style == Row::Message && m_reaction && index.isValid()
            && !index.data(SystemMessage).toBool() && !index.data(ReceiptMessageId).toString().isEmpty();
    }

    bool canReact(const QModelIndex &index) const { return reactionRow(index) && m_reactionAllowed && m_reactionAllowed(); }

    static QString deliveryText(const QModelIndex &index)
    {
        auto caption = index.data(Meta).toString();
        if (index.data(Outgoing).toBool()
            && (!caption.isEmpty() || !index.data(ReceiptMessageId).toString().isEmpty()))
            caption += index.data(MessageSeen).toBool() ? QStringLiteral("  ✓✓") : QStringLiteral("  ✓");
        return caption;
    }

    static QRect deliveryRect(const QRect &bubble)
    {
        return {bubble.left() + 8, bubble.bottom() - messageTimeHeight, bubble.width() - 19, messageTimeHeight - 2};
    }

    QRect viewersReceipt(const QModelIndex &index, const QRect &row) const
    {
        if (m_style != Row::Message || !m_messageViewers || !index.data(Outgoing).toBool()
            || !index.data(MessageSeen).toBool() || index.data(ReceiptMessageId).toString().isEmpty())
            return {};
        auto rect = deliveryRect(messageBubble(row, index));
        const auto marks = QFontMetrics(rowFont(messageMetaPixels)).horizontalAdvance(QStringLiteral("✓✓"));
        rect.setLeft(qMax(rect.left(), rect.right() - marks - 5));
        return rect;
    }

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

    static int reactionBadgeWidth(const QString &count)
    {
        const int countWidth = QFontMetrics(rowFont(ui::secondaryTextPixels, true)).horizontalAdvance(count) + 2;
        return reactionCountOffset + countWidth + reactionCountPadding;
    }

    static int reactionDeliverySpace(const QModelIndex &index)
    {
        const auto delivery = deliveryText(index);
        return delivery.isEmpty() ? 10
            : QFontMetrics(rowFont(messageMetaPixels)).horizontalAdvance(delivery) + 16;
    }

    static int minimumReactionWidth(const QModelIndex &index)
    {
        if (index.data(SystemMessage).toBool())
            return 0;
        int widest = 0;
        const auto counts = index.data(MessageReactionCounts).toMap();
        for (const auto &count : counts) {
            if (count.toInt() > 0)
                widest = qMax(widest, reactionBadgeWidth(count.toString()));
        }
        return widest > 0 ? reactionStart + widest + reactionDeliverySpace(index) + 1 : 0;
    }

    static QVector<ReactionTarget> reactionTargets(const QModelIndex &index, const QRect &bubble)
    {
        QVector<ReactionTarget> result;
        const auto counts = index.data(MessageReactionCounts).toMap();
        if (counts.isEmpty())
            return result;
        const auto own = index.data(OwnMessageReactions).toStringList();
        const int right = bubble.right() - reactionDeliverySpace(index);
        const int top = bubble.bottom() - reactionHeight - 2;
        int left = bubble.left() + reactionStart;
        for (bool marked : {true, false}) {
            for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
                if (it.value().toInt() <= 0 || own.contains(it.key()) != marked)
                    continue;
                const auto count = it.value().toString();
                const int width = reactionBadgeWidth(count);
                if (left + width > right)
                    continue;
                result.append({QRect(left, top, width, reactionHeight), it.key(), count, marked});
                left += width + 3;
            }
        }
        return result;
    }

    static int messageDay(const QModelIndex &index)
    {
        return index.data(DayDivider).toString().isEmpty() ? 0 : 27;
    }

    static int messageDividers(const QModelIndex &index)
    {
        return messageDay(index) + (index.data(UnreadDivider).toBool() ? 30 : 0);
    }

    QRect messageContentRect(const QRect &row, const QModelIndex &index) const
    {
        if (index.data(Id).toString().isEmpty())
            return row.adjusted(8, 3, -8, 0);
        const auto bubble = messageBubble(row, index);
        const auto entries = files(index, MessageFiles);
        const auto width = bubble.width() - 20;
        return bubble.adjusted(12, messageBodyTop + messageHeader(index) + messageContext(index)
                                  + fileHeight(entries, width, AttachmentSection::Media),
                              -12, -messageBodyBottom - messageTime(index)
                                  - fileHeight(entries, width, AttachmentSection::Documents) - linkPreviewHeight(index));
    }

    using LinkCard = QPair<QString, MessageLinkPreview>;

    QVector<LinkCard> linkPreviews(const QModelIndex &index) const
    {
        QVector<LinkCard> cards;
        if (m_linkPreview) {
            const auto links = index.data(MessageLinks).toStringList();
            for (const auto &link : links) {
                auto preview = m_linkPreview(index, link);
                if (m_style == Row::Notice && preview.kind == MessageLinkPreviewKind::Task)
                    continue;
                if (!preview.title.isEmpty())
                    cards.append({link, std::move(preview)});
            }
        }
        return cards;
    }

    static int linkCardHeight(const MessageLinkPreview &preview)
    {
        const int textHeight = 38 + linkDetailLineHeight(preview) * preview.details.size();
        return preview.thumbnailUrl.isEmpty() ? textHeight : qMax(76, textHeight);
    }

    static const QString &taskLinkDetailSeparator()
    {
        static const auto separator = QStringLiteral(" · ");
        return separator;
    }

    static int taskLinkLabelExtent(const QFontMetrics &labels, const QString &label)
    {
        return qMax(labels.horizontalAdvance(label), labels.boundingRect(label).width()) + 4;
    }

    static int minimumLinkCardWidth(const MessageLinkPreview &preview, int available)
    {
        const int maxTaskWidth = qMin(620, available * 4 / 5);
        if (preview.kind != MessageLinkPreviewKind::Task || preview.details.isEmpty() || maxTaskWidth <= linkCardMinimum)
            return qMin(linkCardMinimum, available);
        const auto &separator = taskLinkDetailSeparator();
        const QFontMetrics labels(rowFont(ui::secondaryTextPixels, true));
        const QFontMetrics values(rowFont(ui::secondaryTextPixels));
        int labelWidth = 0;
        int valueWidth = 0;
        int lineWidth = QFontMetrics(rowFont(ui::bodyTextPixels, true)).horizontalAdvance(preview.title);
        for (const auto &detail : preview.details) {
            const int split = detail.indexOf(separator);
            if (split > 0) {
                labelWidth = qMax(labelWidth, taskLinkLabelExtent(labels, detail.left(split)));
                valueWidth = qMax(valueWidth, values.horizontalAdvance(detail.mid(split + separator.size())));
            } else {
                lineWidth = qMax(lineWidth, values.horizontalAdvance(detail));
            }
        }
        const int cardInsets = 68 + (preview.thumbnailUrl.isEmpty() ? 0 : 88);
        const int preferred = cardInsets + qMax(lineWidth, labelWidth + 28 + valueWidth);
        return qBound(linkCardMinimum, preferred, maxTaskWidth);
    }

    static int linkDetailLineHeight(const MessageLinkPreview &preview)
    {
        return preview.kind == MessageLinkPreviewKind::Task ? 21 : 19;
    }

    int linkPreviewHeight(const QModelIndex &index) const
    {
        int height = 0;
        for (const auto &card : linkPreviews(index))
            height += linkCardHeight(card.second) + linkCardGap;
        return height;
    }

    QRect firstLinkCard(const QRect &row, const QModelIndex &index) const
    {
        if (m_style == Row::Notice) {
            const auto card = noticeRect(row);
            const auto detail = noticeBodyRect(row, index);
            return QRect(detail.left(), detail.bottom() + 6, qMax(1, card.width() - 40), 1);
        }
        auto rect = firstFileRect(row, index, AttachmentSection::Documents);
        rect.translate(0, fileHeight(files(index, MessageFiles), rect.width(), AttachmentSection::Documents));
        return rect;
    }

    void paintLinkProjectAvatar(QPainter &p, const QRect &box, const Project &project) const
    {
        const auto *photo = m_linkAvatar && !project.avatarUrl.isEmpty() ? m_linkAvatar(project.avatarUrl) : nullptr;
        paintAvatar(p, box, project.name, AvatarKind::Project, photo ? *photo : QPixmap{},
                    QColor(project.avatarColor), project.avatarVariant, project.avatarType);
    }

    void paintLinkIcon(QPainter &p, const QRect &box, const MessageLinkPreview &preview) const
    {
        if (preview.kind == MessageLinkPreviewKind::Task && preview.project) {
            paintLinkProjectAvatar(p, box, *preview.project);
            return;
        }
        const auto &url = preview.imageUrl();
        const auto *photo = m_linkAvatar && !url.isEmpty() ? m_linkAvatar(url) : nullptr;
        if (photo && !photo->isNull()) {
            paintAvatar(p, box, preview.title, AvatarKind::Person, *photo);
            return;
        }
        auto glyph = ui::Glyph::Link;
        QColor background("#d9ebe5");
        switch (preview.kind) {
        case MessageLinkPreviewKind::External: break;
        case MessageLinkPreviewKind::Task:
            glyph = ui::Glyph::Task;
            background = QColor("#dbe8ef");
            break;
        case MessageLinkPreviewKind::Project:
            glyph = ui::Glyph::Projects;
            background = QColor("#e9e1ef");
            break;
        case MessageLinkPreviewKind::Person:
            glyph = ui::Glyph::Person;
            background = QColor("#efe4d7");
            break;
        case MessageLinkPreviewKind::Chat:
            glyph = ui::Glyph::Chat;
            background = QColor("#d1e9e3");
            break;
        }
        p.setPen(Qt::NoPen);
        p.setBrush(background);
        p.drawEllipse(box);
        ui::paintGlyph(p, glyph, QRectF(box).adjusted(4, 4, -4, -4), Accent);
    }

    void paintLinkCards(QPainter &p, const QRect &row, const QModelIndex &index) const
    {
        auto rect = firstLinkCard(row, index);
        for (const auto &[href, preview] : linkPreviews(index)) {
            rect.setHeight(linkCardHeight(preview));
            fill(p, rect, QColor("#f6faf8"), QColor("#aac6ba"), 6);
            paintLinkIcon(p, QRect(rect.left() + 8, rect.top() + 6, 24, 24), preview);
            const int textLeft = rect.left() + 40;
            const int thumbnailWidth = preview.thumbnailUrl.isEmpty() ? 0 : qBound(0, rect.width() - 145, 80);
            const int textWidth = qMax(0, rect.width() - 48 - (thumbnailWidth > 0 ? thumbnailWidth + 8 : 0));
            line(p, QRect(textLeft, rect.top() + 7, textWidth, 22), preview.title, ui::bodyTextPixels, Accent, true);
            if (thumbnailWidth > 0) {
                const auto *photo = m_messagePreview ? m_messagePreview(preview.thumbnailUrl) : nullptr;
                const auto *thumbnail = !photo && m_linkThumbnail ? m_linkThumbnail(preview.thumbnailUrl) : nullptr;
                const auto size = photo ? photo->size() : thumbnail ? thumbnail->size() : QSize{};
                if (!size.isEmpty()) {
                    const QRect box(rect.right() - thumbnailWidth - 7, rect.top() + 8, thumbnailWidth, 60);
                    QRect image(QPoint{}, size.scaled(box.size(), Qt::KeepAspectRatioByExpanding));
                    image.moveCenter(box.center());
                    p.save();
                    QPainterPath clip;
                    clip.addRoundedRect(box, 5, 5);
                    p.setClipPath(clip, Qt::IntersectClip);
                    p.setRenderHint(QPainter::SmoothPixmapTransform);
                    if (photo)
                        p.drawPixmap(image, *photo);
                    else
                        p.drawImage(image, *thumbnail);
                    p.restore();
                }
            }
            paintLinkDetails(p, QRect(textLeft, rect.top() + 30, textWidth, rect.height() - 30), preview);
            rect.translate(0, rect.height() + linkCardGap);
        }
    }

    int paintTaskLinkDetailIcon(QPainter &p, const QRect &row, const QString &label,
                                const MessageLinkPreview &preview) const
    {
        constexpr int iconSize = 15;
        constexpr int gap = 5;
        if (row.width() < iconSize + gap + 24)
            return 0;
        const QRect box(row.left(), row.top() + (row.height() - iconSize) / 2, iconSize, iconSize);
        if (label == QStringLiteral("Постановщик") || label == QStringLiteral("Исполнитель")) {
            const auto &url = label == QStringLiteral("Постановщик") ? preview.creatorAvatarUrl : preview.assigneeAvatarUrl;
            const auto *photo = m_linkAvatar && !url.isEmpty() ? m_linkAvatar(url) : nullptr;
            if (photo && !photo->isNull()) {
                paintAvatar(p, box, {}, AvatarKind::Person, *photo);
            } else {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor("#d9ebe5"));
                p.drawEllipse(box);
                ui::paintGlyph(p, ui::Glyph::Person, QRectF(box).adjusted(2, 2, -2, -2), Accent);
            }
        } else if (label == QStringLiteral("Стадия")) {
            const QColor color(QLatin1Char('#') + preview.stageColor);
            if (!color.isValid())
                return 0;
            p.setPen(QPen(color.darker(135), 1));
            p.setBrush(color);
            p.drawEllipse(box.adjusted(2, 2, -2, -2));
        } else if (label == QStringLiteral("Проект") && preview.project) {
            paintLinkProjectAvatar(p, box, *preview.project);
        } else {
            return 0;
        }
        return iconSize + gap;
    }

    void paintLinkDetails(QPainter &p, const QRect &area, const MessageLinkPreview &preview) const
    {
        const int lineHeight = linkDetailLineHeight(preview);
        const auto &separator = taskLinkDetailSeparator();
        const bool task = preview.kind == MessageLinkPreviewKind::Task;
        const QFontMetrics labels(rowFont(ui::secondaryTextPixels, true));
        int labelWidth = 0;
        if (task && area.width() >= 225) {
            for (const auto &detail : preview.details) {
                const auto split = detail.indexOf(separator);
                if (split > 0)
                    labelWidth = qMax(labelWidth, taskLinkLabelExtent(labels, detail.left(split)));
            }
            if (labelWidth > area.width() / 3)
                labelWidth = 0;
        }
        int top = area.top();
        for (const auto &detail : preview.details) {
            const auto split = task ? detail.indexOf(separator) : -1;
            const QRect row(area.left(), top, area.width(), lineHeight);
            if (split > 0) {
                const auto label = detail.left(split);
                const auto value = detail.mid(split + separator.size());
                const int width = labelWidth > 0 ? labelWidth
                    : qMin(taskLinkLabelExtent(labels, label), qMax(0, area.width() - 54));
                const int valueOffset = width + 8;
                line(p, QRect(row.left(), top, width, lineHeight), label, ui::secondaryTextPixels, Ink, true);
                if (valueOffset < area.width()) {
                    const auto valueRow = row.adjusted(valueOffset, 0, 0, 0);
                    const auto iconWidth = paintTaskLinkDetailIcon(p, valueRow, label, preview);
                    line(p, valueRow.adjusted(iconWidth, 0, 0, 0), value, ui::secondaryTextPixels, ConversationMetaInk);
                }
            } else {
                line(p, row, detail, ui::secondaryTextPixels, ConversationMetaInk);
            }
            top += lineHeight;
        }
    }

    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (object == m_previewWindow && event->type() == QEvent::WindowStateChange)
            updatePreviewClock();
        if (object == m_previewViewport && (event->type() == QEvent::Show || event->type() == QEvent::Hide
                                            || event->type() == QEvent::StyleChange))
            updatePreviewClock();
        if (object == m_view->viewport()) {
            if (event->type() == QEvent::ContextMenu && m_style == Row::TaskContent) {
                auto *context = static_cast<QContextMenuEvent *>(event);
                if (context->reason() == QContextMenuEvent::Mouse
                    && showPhotoMenuAt(m_view->indexAt(context->pos()), context->pos())) {
                    context->accept();
                    return true;
                }
            } else if (event->type() == QEvent::MouseMove) {
                const auto *mouse = static_cast<QMouseEvent *>(event);
                const auto point = mouse->position().toPoint();
                const auto index = m_view->indexAt(point);
                if (mouse->buttons() == Qt::NoButton) {
                    const auto row = m_view->visualRect(index);
                    if (index.isValid() && rowActionAt(index, row, point))
                        m_view->viewport()->setCursor(Qt::PointingHandCursor);
                    else if (selectableTextRect(index, row).contains(point))
                        m_view->viewport()->setCursor(Qt::IBeamCursor);
                    else
                        m_view->viewport()->unsetCursor();
                }
            } else if (event->type() == QEvent::MouseButtonDblClick) {
                const auto *mouse = static_cast<QMouseEvent *>(event);
                const auto point = mouse->position().toPoint();
                const auto index = m_view->indexAt(point);
                if (mouse->button() == Qt::LeftButton && index.isValid()) {
                    const auto target = fileAt(index, m_view->visualRect(index), point);
                    if (target && target->geometry.thumbnail.contains(point))
                        return true;
                }
            } else if (event->type() == QEvent::Leave) {
                m_view->viewport()->unsetCursor();
            }
        }
        return QStyledItemDelegate::eventFilter(object, event);
    }

    std::function<void()> rowActionAt(const QModelIndex &index, const QRect &row, const QPoint &point) const
    {
        if (m_style == Row::Notice) {
            if (m_noticeAction) {
                const auto buttons = noticeActionRects(row, index);
                for (int i = 0; i < buttons.size(); ++i) {
                    if (buttons[i].contains(point))
                        return [this, index, i] { m_noticeAction(index, i); };
                }
            }
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
                const bool opensThumbnail = geometry.thumbnail.contains(point) && (preview(file) || downloadable(file));
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

    static QVector<MessageFile> files(const QModelIndex &index, int role)
    {
        return index.data(role).value<QVector<MessageFile>>();
    }

    void updatePreviewClock()
    {
        if (!m_previewViewport)
            return;
        auto *window = m_previewViewport->window();
        if (m_previewAnimation && m_previewWindow != window) {
            if (m_previewWindow)
                m_previewWindow->removeEventFilter(this);
            m_previewWindow = window;
            window->installEventFilter(this);
        }
        if (m_previewAnimation && m_previewViewport->isVisible() && !window->isMinimized()
            && m_previewViewport->style()->styleHint(QStyle::SH_Widget_Animation_Duration, nullptr, m_previewViewport) > 0) {
            if (!m_previewClock) {
                m_previewClock = ui::LoadingClock::shared();
                m_previewClock->watch(m_previewViewport);
            }
        } else if (m_previewClock) {
            m_previewClock->unwatch(m_previewViewport);
            m_previewClock.clear();
        }
    }

    bool previewPending(const MessageFile &file) const
    {
        return m_previewPending && m_previewPending(file);
    }

    int previewAngle() const { return m_previewClock ? m_previewClock->angle() : 0; }

    static QRect cardLoadingRing(const QRect &card)
    {
        return QRect(card.left() + 14, card.top() + 12, 18, 18);
    }

    static QRect mediaLoadingRing(const QRect &frame, const MessageFile &file, bool crop)
    {
        QRect ring(0, 0, 18, 18);
        ring.moveCenter(frame.center());
        if (!crop && file.isVideo && downloadable(file) && frame.width() >= 40)
            ring.moveTopLeft(frame.topLeft() + QPoint(4, 4));
        return ring;
    }

    enum class AttachmentSection { Media, Documents };

    static bool isMediaFile(const MessageFile &file)
    {
        return file.isImage || file.isVideo || !file.previewUrl.isEmpty() || !file.originalPreviewUrl.isEmpty();
    }

    struct QuoteAttachments {
        std::optional<MessageFile> representative;
        int media = 0;
        int documents = 0;
        QString firstDocument;
    };

    QuoteAttachments replyAttachments(const QModelIndex &index) const
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

    int quoteHeight(const QModelIndex &index) const
    {
        return replyAttachments(index).media > 0 ? messageMediaQuoteHeight : messageQuoteHeight;
    }

    static QRect quoteMediaRect(const QRect &quote)
    {
        if (quote.width() < 108)
            return {};
        return QRect(quote.left() + 11, quote.top() + (quote.height() - 40) / 2, 40, 40);
    }

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

    std::optional<FileTarget> fileAt(const QModelIndex &index, const QRect &row, const QPoint &point) const
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

    FileGeometry fileGeometry(const QRect &card, const MessageFile &file) const
    {
        const auto *photo = preview(file);
        if (!photo)
            return {{}, card};
        const auto source = file.previewSize.isEmpty() ? photo->size() : file.previewSize;
        const QRect thumbnail(card.topLeft(), ui::chatPreviewDisplaySize(source, card.width()));
        return {thumbnail, {}};
    }

    int fileHeight(const QVector<MessageFile> &entries, int width, AttachmentSection section) const
    {
        int height = 0;
        layoutFiles(entries, QRect(0, 0, qMax(1, width), fileControlHeight), section,
                    [](const auto &, const auto &) { return false; }, &height);
        return height;
    }

    static bool matchesOriginal(const QVector<MessageFile> &entries, const MessageFile &file, bool preview = false)
    {
        return std::any_of(entries.cbegin(), entries.cend(), [&file, preview](const MessageFile &current) {
            return current.downloadUrl == file.downloadUrl && current.id == file.id
                && current.diskObjectId == file.diskObjectId && current.name == file.name
                && (!preview || (current.previewUrl == file.previewUrl
                                 && current.originalPreviewUrl == file.originalPreviewUrl));
        });
    }

    QRect photoMenuRect(const QRect &thumbnail, const MessageFile &file) const
    {
        if (thumbnail.width() < 32 || !m_messagePhotoMenu || (!downloadable(file) && !preview(file)))
            return {};
        return QRect(thumbnail.right() - 28, thumbnail.top() + 5, 24, 24);
    }

    QRect firstFileRect(const QRect &row, const QModelIndex &index, AttachmentSection section) const
    {
        const auto bubble = messageBubble(row, index);
        const int top = section == AttachmentSection::Media
            ? bubble.top() + messageBodyTop + messageHeader(index) + messageContext(index)
            : messageContentRect(row, index).bottom() + 3;
        return QRect(bubble.left() + 10, top, qMax(1, bubble.width() - 20), fileControlHeight);
    }

    void paintMediaFrame(QPainter &p, const QRect &frame, const MessageFile &file, bool crop = false) const
    {
        fill(p, frame, QColor("#e2ede8"), QColor("#bbd3ca"), 6);
        if (const auto *photo = preview(file)) {
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
        } else if (previewPending(file)) {
            ui::paintLoadingRing(p, mediaLoadingRing(frame, file, crop), Accent, previewAngle());
        } else if (crop || !file.isVideo || !downloadable(file)) {
            const auto &icon = ui::attachmentIcon(file.name);
            const QRect glyph(0, 0, 21, 21);
            ui::paintGlyph(p, icon.glyph, glyph.translated(frame.center() - glyph.center()), icon.ink);
        }
    }

    void paintMediaTile(QPainter &p, const QRect &thumbnail, const MessageFile &file) const
    {
        paintMediaFrame(p, thumbnail, file);
        if (file.isVideo && downloadable(file) && thumbnail.width() >= 40) {
            const auto center = thumbnail.center();
            p.save();
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(32, 57, 69, 218));
            p.drawEllipse(center, 19, 19);
            p.setBrush(Qt::white);
            p.drawPolygon(QPolygon{center + QPoint(-4, -8), center + QPoint(9, 0), center + QPoint(-4, 8)});
            p.restore();
        }
        const auto menu = photoMenuRect(thumbnail, file);
        if (!menu.isEmpty()) {
            fill(p, menu, QColor("#203945"), Qt::transparent, 5);
            ui::paintGlyph(p, ui::Glyph::More, QRectF(menu).adjusted(3, 3, -3, -3), Qt::white);
        }
    }

    void paintFiles(QPainter &p, const QRect &start, const QVector<MessageFile> &entries, AttachmentSection section) const
    {
        layoutFiles(entries, start, section, [&](const MessageFile &file, const FileGeometry &geometry) {
            if (!geometry.thumbnail.isEmpty()) {
                paintMediaTile(p, geometry.thumbnail, file);
                return false;
            }
            const auto &card = geometry.control;
            fill(p, card, QColor("#f6faf8"), QColor("#bbd3ca"), 6);
            const QRect icon(card.left() + 6, card.top() + 7, 34, 28);
            const auto &typeIcon = ui::attachmentIcon(file.name);
            fill(p, icon, typeIcon.background, Qt::transparent, 5);
            if (previewPending(file))
                ui::paintLoadingRing(p, cardLoadingRing(card), typeIcon.ink, previewAngle());
            else
                ui::paintGlyph(p, typeIcon.glyph, QRectF(icon).adjusted(6, 3, -6, -3), typeIcon.ink);
            const int textLeft = icon.right() + 7;
            const QRect textArea(textLeft, card.top() + fileTitleTop, qMax(1, card.right() - textLeft - 5),
                                 fileTitleHeight);
            const bool opensMedia = (file.isImage || file.isVideo) && downloadable(file);
            const auto title = !opensMedia ? file.name : (file.isVideo ? QStringLiteral("Воспроизвести %1")
                                                                      : QStringLiteral("Открыть %1")).arg(file.name);
            line(p, textArea, title, ui::bodyTextPixels, opensMedia ? Accent : Ink, true);
            QStringList metadata;
            if (file.size >= 0)
                metadata.append(QLocale().formattedDataSize(file.size, 1));
            metadata.append(downloadable(file) ? QStringLiteral("Сохранить") : QStringLiteral("Скачивание недоступно"));
            line(p, textArea.translated(0, fileTitleHeight), metadata.join(QStringLiteral(" · ")),
                 ui::secondaryTextPixels, Accent);
            return false;
        }, nullptr);
    }

    void paintQuote(QPainter &p, const QRect &quote, const QModelIndex &index, bool outgoing) const
    {
        fill(p, quote, outgoing ? QColor("#b5d9c9") : QColor("#e8f1ee"), Qt::transparent, 5);
        p.setPen(QColor("#54877a"));
        p.drawLine(quote.left() + 4, quote.top() + 5, quote.left() + 4, quote.bottom() - 5);
        const auto attachments = replyAttachments(index);
        const auto excerpt = index.data(ReplyPreview).toString();
        auto text = quote.adjusted(11, 2, -6, 0);
        text.setHeight(messageQuoteLineHeight);
        if (attachments.representative) {
            const auto thumbnail = quoteMediaRect(quote);
            if (!thumbnail.isEmpty()) {
                paintMediaFrame(p, thumbnail, *attachments.representative, true);
                text.setLeft(thumbnail.right() + 8);
            }
        }
        line(p, text, index.data(ReplyByline).toString(), ui::secondaryTextPixels, Accent, true);
        text.translate(0, messageQuoteLineHeight);
        const auto attachmentsOnly = excerpt == QLatin1String("Вложение");
        if (attachments.representative) {
            auto media = attachments.media > 1 ? QStringLiteral("Галерея · %1 медиа").arg(attachments.media)
                : attachments.representative->isVideo ? QStringLiteral("Видео") : QStringLiteral("Фотография");
            if (attachments.documents)
                media += QStringLiteral(" · Файлы: %1").arg(attachments.documents);
            line(p, text, media, ui::secondaryTextPixels, Ink, true);
            if (!attachmentsOnly) {
                text.translate(0, messageQuoteLineHeight);
                quotePreview(p, text, excerpt, ConversationMetaInk);
            }
        } else {
            auto caption = excerpt;
            if (attachments.documents) {
                const auto description = attachments.documents == 1 ? attachments.firstDocument
                    : QStringLiteral("Файлы · %1").arg(attachments.documents);
                caption = attachmentsOnly ? description : caption + QStringLiteral(" · ") + description;
            }
            quotePreview(p, text, caption, Ink);
        }
    }

    static QRect noticeRect(const QRect &row)
    {
        auto rect = row.adjusted(0, 2, -6, -2);
        rect.setWidth(qMin(rect.width(), 660));
        return rect;
    }

    static QVector<QRect> noticeButtons(const QRect &card, const QStringList &labels)
    {
        QVector<QRect> buttons;
        const int left = card.left() + 25;
        const int right = card.right() - 14;
        int x = left;
        int y = card.top();
        const QFontMetrics measure(rowFont(ui::bodyTextPixels, true));
        for (const auto &label : labels) {
            const int width = qBound(1, measure.horizontalAdvance(label) + 16, qMax(1, right - left));
            if (x > left && x + width > right) {
                x = left;
                y += 30;
            }
            buttons.append(QRect(x, y, width, 24));
            x += width + 6;
        }
        return buttons;
    }

    QVector<QRect> noticeActionRects(const QRect &row, const QModelIndex &index) const
    {
        auto card = noticeRect(row);
        card.moveTop(noticeBodyRect(row, index).bottom() + 6 + linkPreviewHeight(index));
        return noticeButtons(card, index.data(NoticeActions).toStringList());
    }

    QRect noticeBodyRect(const QRect &row, const QModelIndex &index) const
    {
        const auto card = noticeRect(row);
        const bool hasTitle = !index.data(Title).toString().isEmpty();
        const bool sharesTimestamp = !hasTitle && !index.data(Meta).toString().isEmpty();
        const int width = qMax(1, card.width() - (sharesTimestamp ? 125 : 40));
        return QRect(card.left() + 25, card.top() + (hasTitle ? 33 : 9), width,
                     bodyHeight(index, index.data(Detail).toString(), width));
    }
    QTextDocument *bodyDocument(const QModelIndex &index, int width) const
    {
        const auto html = index.data(MarkupHtml).toString();
        const auto plain = html.isEmpty() ? index.data(Detail).toString() : QString();
        if (html.isEmpty() && (plain.isEmpty() || m_style == Row::Notice))
            return nullptr;
        const auto key = QString::number(width) + (html.isEmpty() ? QStringLiteral(":plain:") + plain
                                                                  : QStringLiteral(":html:") + html);
        if (auto *existing = m_documents.object(key))
            return existing;
        auto *document = new QTextDocument;
        document->setUndoRedoEnabled(false);
        document->setDocumentMargin(0);
        document->setDefaultFont(rowFont(ui::bodyTextPixels));
        QTextOption options;
        options.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        document->setDefaultTextOption(options);
        if (html.isEmpty())
            document->setPlainText(plain);
        else
            document->setHtml(html);
        applyMessageEmojiFonts(*document);
        document->setTextWidth(qMax(1, width));
        m_documents.insert(key, document);
        return document;
    }

    QRect selectableTextRect(const QModelIndex &index, const QRect &row) const
    {
        if ((m_style != Row::Message && m_style != Row::TaskContent) || !index.isValid() ||
            index.data(Id).toString().isEmpty() || index.data(Detail).toString().isEmpty())
            return {};
        return messageContentRect(row, index);
    }

    static int textPosition(const QTextDocument &document, const QRect &rect, const QPoint &point)
    {
        const auto offset = point - rect.topLeft();
        const int end = document.characterCount() - 1;
        if (offset.y() < 0)
            return 0;
        if (offset.y() >= document.size().height())
            return end;
        return qBound(0, document.documentLayout()->hitTest(offset, Qt::FuzzyHit), end);
    }

    QTextCursor selectionCursor() const
    {
        if (!m_textIndex.isValid())
            return {};
        const auto rect = selectableTextRect(m_textIndex, m_view->visualRect(m_textIndex));
        auto *document = bodyDocument(m_textIndex, rect.width());
        if (!document)
            return {};
        QTextCursor cursor(document);
        const int end = document->characterCount() - 1;
        cursor.setPosition(qBound(0, m_textAnchor, end));
        cursor.setPosition(qBound(0, m_textPosition, end), QTextCursor::KeepAnchor);
        return cursor;
    }

    void saveTextSelection(const QModelIndex &index, const QTextCursor &cursor)
    {
        const auto old = m_textIndex;
        m_textIndex = index;
        m_textAnchor = cursor.anchor();
        m_textPosition = cursor.position();
        m_textVerticalX = cursor.verticalMovementX();
        if (old.isValid() && old != index)
            m_view->viewport()->update(m_view->visualRect(old));
        m_view->viewport()->update(m_view->visualRect(index));
    }

    int bodyHeight(const QModelIndex &index, const QString &text, int width) const
    {
        if (const auto *document = bodyDocument(index, width))
            return qMax(QFontMetrics(rowFont(ui::bodyTextPixels)).height(), qCeil(document->size().height()));
        return wrappedText(text, width, rowFont(ui::bodyTextPixels));
    }

    static int wrappedText(QString text, int width, const QFont &font,
                           QPainter *painter = nullptr, const QPoint &position = {})
    {
        text.replace(QLatin1Char('\n'), QChar::LineSeparator);
        QTextLayout layout(text, font);
        layout.setFormats(messageEmojiFormats(text));
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(option);
        layout.beginLayout();
        qreal y = 0;
        for (auto line = layout.createLine(); line.isValid(); line = layout.createLine()) {
            line.setLineWidth(qMax(1, width));
            line.setPosition(QPointF(0, y));
            y += line.height();
        }
        layout.endLayout();
        if (painter)
            layout.draw(painter, position);
        return qMax(QFontMetrics(font).height(), qCeil(y));
    }

    static void fill(QPainter &p, const QRect &rect, const QColor &background,
                     const QColor &outline = Qt::transparent, int radius = 8)
    {
        p.setPen(outline == Qt::transparent ? QPen(Qt::NoPen) : QPen(outline));
        p.setBrush(background);
        p.drawRoundedRect(rect, radius, radius);
    }

    static void line(QPainter &p, const QRect &rect, const QString &text, int pixels,
                     const QColor &color = Ink, bool bold = false,
                     Qt::Alignment alignment = Qt::AlignLeft | Qt::AlignVCenter)
    {
        p.setFont(rowFont(pixels, bold));
        p.setPen(color);
        const QFontMetrics metrics(p.font());
        p.drawText(rect, int(alignment) | Qt::TextSingleLine, metrics.elidedText(text, Qt::ElideRight, rect.width()));
    }

    static void paragraph(QPainter &p, const QRect &rect, const QString &text, int pixels,
                          const QColor &color = Ink, bool bold = false)
    {
        p.save();
        p.setClipRect(rect, Qt::IntersectClip);
        p.setPen(color);
        wrappedText(text, rect.width(), rowFont(pixels, bold), &p, rect.topLeft());
        p.restore();
    }

    static void quotePreview(QPainter &painter, const QRect &rect, const QString &text, const QColor &color)
    {
        const auto formats = messageEmojiFormats(text);
        if (formats.isEmpty()) {
            line(painter, rect, text, ui::secondaryTextPixels, color);
            return;
        }

        const auto font = rowFont(ui::secondaryTextPixels);
        auto measuring = font;
        measuring.setFamilies(font.families() + emojiFontFamilies());
        const auto visible = QFontMetrics(measuring).elidedText(text, Qt::ElideRight, rect.width());
        QTextLayout layout(visible, font);
        layout.setFormats(visible == text ? formats : messageEmojiFormats(visible));
        QTextOption option;
        option.setWrapMode(QTextOption::NoWrap);
        layout.setTextOption(option);
        layout.beginLayout();
        const auto row = layout.createLine();
        layout.endLayout();
        if (!row.isValid())
            return;

        painter.save();
        painter.setClipRect(rect, Qt::IntersectClip);
        painter.setPen(color);
        layout.draw(&painter, QPointF(rect.left(), rect.top() + (rect.height() - row.height()) / 2));
        painter.restore();
    }

    void body(QPainter &p, const QRect &rect, const QModelIndex &index, const QString &text,
              const QColor &color) const
    {
        auto *document = bodyDocument(index, rect.width());
        if (!document) {
            paragraph(p, rect, text, ui::bodyTextPixels, color);
            return;
        }
        p.save();
        p.setClipRect(rect, Qt::IntersectClip);
        p.translate(rect.topLeft());
        QAbstractTextDocumentLayout::PaintContext context;
        context.palette.setColor(QPalette::Text, color);
        if (m_textIndex == index && m_textAnchor != m_textPosition) {
            QTextCursor cursor(document);
            const int end = document->characterCount() - 1;
            cursor.setPosition(qBound(0, m_textAnchor, end));
            cursor.setPosition(qBound(0, m_textPosition, end), QTextCursor::KeepAnchor);
            QTextCharFormat format;
            format.setBackground(m_view->palette().brush(QPalette::Highlight));
            format.setForeground(m_view->palette().brush(QPalette::HighlightedText));
            context.selections.append({cursor, format});
        }
        document->documentLayout()->draw(&p, context);
        p.restore();
    }

    static QColor itemOutline(const QStyleOptionViewItem &option, const QColor &normal = Qt::transparent)
    {
        return option.state & QStyle::State_HasFocus ? Accent : normal;
    }

    static void paintDirectory(QPainter &p, const QStyleOptionViewItem &option, const QModelIndex &index,
                               const QString &title, const QString &detail, const QString &meta)
    {
        const QRect r = option.rect.adjusted(3, 2, -3, -2);
        if (title.isEmpty()) {
            paragraph(p, r.adjusted(8, 10, -8, 0), detail, ui::bodyTextPixels, Muted);
            return;
        }
        if (option.state & QStyle::State_Selected)
            fill(p, r, Selected, itemOutline(option));
        else if (option.state & (QStyle::State_MouseOver | QStyle::State_HasFocus))
            fill(p, r, QColor("#edf3f2"), itemOutline(option));
        const auto avatarKind = AvatarKind(index.data(AvatarType).toInt());
        const bool project = avatarKind == AvatarKind::Project;
        int textLeft = r.left() + 8;
        if (r.width() >= 92) {
            const int size = r.width() < 120 ? 24 : r.width() < 148 ? 28 : 36;
            const QRect mark(textLeft, r.center().y() - size / 2, size, size);
            paintAvatar(p, mark, title, avatarKind,
                        index.data(AvatarImage).value<QPixmap>(), index.data(ProjectAvatarColor).value<QColor>(),
                        index.data(ProjectAvatarVariant).toString(), index.data(ProjectAvatarType).toString());
            const auto onlineUntil = index.data(ContactOnlineUntil).toDateTime();
            if (onlineUntil.isValid() && onlineUntil > QDateTime::currentDateTimeUtc())
                paintOnlineDot(p, mark);
            textLeft = mark.right() + 9;
        }
        const int right = r.right() - 8;
        const int metaWidth = meta.isEmpty() ? 0 : QFontMetrics(rowFont(ui::secondaryTextPixels)).horizontalAdvance(meta) + 8;
        const auto unread = index.data(Count).toLongLong();
        const bool markedUnread = index.data(MarkedUnread).toBool();
        QString unreadCaption;
        int pillWidth = 0;
        if (unread > 0) {
            unreadCaption = !project && unread > 99 ? QStringLiteral("99+")
                : index.data(CountPartial).toBool() ? QStringLiteral("%1+").arg(unread) : QString::number(unread);
            pillWidth = qMax(24, QFontMetrics(rowFont(ui::secondaryTextPixels, true)).horizontalAdvance(unreadCaption) + 10);
        }
        const int unreadWidth = pillWidth > 0 ? pillWidth + 5 : markedUnread ? 14 : 0;
        const bool mention = index.data(HasMention).toBool();
        const bool reaction = index.data(HasReaction).toBool();
        const int anchorWidth = 19 * (int(mention) + int(reaction));
        const bool pinned = index.data(Pinned).toBool();
        const bool projectPin = pinned && project;
        if (pinned) {
            const QPoint center = projectPin ? QPoint(right - 8, r.top() + 13)
                : QPoint(right - unreadWidth - anchorWidth - 7, r.top() + 36);
            p.save();
            p.translate(center);
            if (projectPin)
                p.scale(1.35, 1.35);
            p.setPen(QPen(Accent, 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const QPolygon pin{QPoint(-4, -3), QPoint(3, -3), QPoint(2, 1),
                QPoint(3, 2), QPoint(-4, 2), QPoint(-3, 1)};
            p.setBrush(Qt::NoBrush);
            p.drawPolygon(pin);
            p.drawLine(0, 3, 0, 6);
            p.restore();
        }
        const int pinTitleWidth = projectPin ? 20 : 0;
        line(p, QRect(textLeft, r.top() + 5, qMax(1, right - textLeft - metaWidth - pinTitleWidth), 21),
             title, ui::bodyTextPixels, Ink, true);
        line(p, QRect(right - metaWidth, r.top() + 5, metaWidth, 21), meta, ui::secondaryTextPixels, Muted, false,
             Qt::AlignRight | Qt::AlignVCenter);
        const int badgesWidth = unreadWidth + anchorWidth + (pinned && !projectPin ? 15 : 0);
        line(p, QRect(textLeft, r.top() + 26, qMax(1, right - textLeft - badgesWidth), 20),
             detail.simplified(), ui::secondaryTextPixels, Muted);
        int anchorRight = right - unreadWidth;
        const auto paintAnchor = [&](const QString &symbol, const QColor &color) {
            const QRect badge(anchorRight - 16, r.top() + 27, 16, 18);
            fill(p, badge, color, Qt::transparent, 8);
            line(p, badge, symbol, 11, Qt::white, true, Qt::AlignCenter);
            anchorRight -= 19;
        };
        if (mention)
            paintAnchor(QStringLiteral("@"), Accent);
        if (reaction)
            paintAnchor(QStringLiteral("♥"), QColor("#a64b62"));
        if (pillWidth > 0) {
            const QRect badge(right - pillWidth, r.top() + 26, pillWidth, 20);
            fill(p, badge, Accent, Qt::transparent, 8);
            line(p, badge, unreadCaption, ui::secondaryTextPixels, Qt::white, true, Qt::AlignCenter);
        } else if (markedUnread) {
            p.setPen(Qt::NoPen);
            p.setBrush(Accent);
            p.drawEllipse(QRect(right - 9, r.top() + 32, 7, 7));
        }
    }

    void paintMessage(QPainter &p, const QStyleOptionViewItem &option, const QModelIndex &index,
                      const QString &title, const QString &detail) const
    {
        const QRect r = option.rect;
        if (index.data(Id).toString().isEmpty()) {
            body(p, r.adjusted(8, 3, -8, 0), index, detail, Muted);
            return;
        }
        const auto day = index.data(DayDivider).toString();
        if (!day.isEmpty()) {
            const int width = QFontMetrics(rowFont(ui::secondaryTextPixels, true)).horizontalAdvance(day) + 20;
            const QRect stamp(r.center().x() - width / 2, r.top() + 3, width, 19);
            fill(p, stamp, QColor("#dce9e2"), Qt::transparent, 9);
            line(p, stamp, day, ui::secondaryTextPixels, QColor("#486961"), true, Qt::AlignCenter);
        }
        if (index.data(UnreadDivider).toBool()) {
            const auto band = QRect(r.left() + 8, r.top() + messageDay(index) + 3, r.width() - 16, 24);
            const int width = qMin(band.width(),
                QFontMetrics(rowFont(ui::secondaryTextPixels, true)).horizontalAdvance(UnreadMessages) + 18);
            const auto caption = QRect(band.center().x() - width / 2, band.top(), width, band.height());
            p.setPen(QColor("#9fbdb9"));
            if (caption.left() > band.left() + 6) {
                const int y = band.center().y();
                p.drawLine(band.left(), y, caption.left() - 6, y);
                p.drawLine(caption.right() + 6, y, band.right(), y);
            }
            fill(p, caption, QColor("#d6e9e5"), Qt::transparent, 7);
            line(p, caption, UnreadMessages, ui::secondaryTextPixels, Accent, true, Qt::AlignCenter);
        }
        const bool outgoing = index.data(Outgoing).toBool();
        const QRect bubble = messageBubble(r, index);
        const bool system = index.data(SystemMessage).toBool();
        if (!system && !outgoing && !title.isEmpty() && messageAvatarSpace(r.width())) {
            const QRect avatar(r.left() + 4, bubble.top() + 4, 26, 26);
            paintAvatar(p, avatar, title, AvatarKind::Person, index.data(AvatarImage).value<QPixmap>());
        }
        const auto outline = option.state & QStyle::State_Selected ? Accent
            : system ? QColor("#bfcec5") : outgoing ? QColor("#9fc6b3") : QColor("#c2d2c7");
        if (m_style != Row::TaskContent)
            fill(p, bubble, system ? QColor("#e2ebe6") : outgoing ? QColor("#c8e7d8") : Surface,
                 itemOutline(option, outline), system ? 7 : 12);
        if (!title.isEmpty())
            line(p, QRect(bubble.left() + 12, bubble.top() + 4, bubble.width() - 23, messageHeaderHeight),
                 title, ui::secondaryTextPixels, Accent, true);
        int contextTop = bubble.top() + 5 + messageHeader(index);
        const auto forward = index.data(ForwardByline).toString();
        if (!forward.isEmpty()) {
            const QPoint mark(bubble.left() + 12, contextTop + 8);
            QPainterPath arrow;
            arrow.moveTo(mark + QPoint(0, 4));
            arrow.quadTo(mark + QPoint(1, -2), mark + QPoint(10, 0));
            arrow.moveTo(mark + QPoint(7, -3));
            arrow.lineTo(mark + QPoint(10, 0));
            arrow.lineTo(mark + QPoint(7, 3));
            p.setPen(QPen(Accent, 1.35, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPath(arrow);
            line(p, QRect(bubble.left() + 28, contextTop, bubble.width() - 39, messageForwardHeight),
                 forward, ui::secondaryTextPixels, Accent, true);
            contextTop += messageForwardHeight;
        }
        if (!index.data(ReplyByline).toString().isEmpty())
            paintQuote(p, replyHeaderRect(r, index), index, outgoing);
        const auto entries = files(index, MessageFiles);
        paintFiles(p, firstFileRect(r, index, AttachmentSection::Media), entries, AttachmentSection::Media);
        if (!detail.isEmpty() || entries.isEmpty())
            body(p, messageContentRect(r, index), index, detail, Ink);
        paintFiles(p, firstFileRect(r, index, AttachmentSection::Documents), entries, AttachmentSection::Documents);
        if (m_linkPreview)
            paintLinkCards(p, r, index);
        const auto delivery = deliveryText(index);
        if (m_style == Row::Message && !system) {
            for (const auto &target : reactionTargets(index, bubble)) {
                fill(p, target.rect, target.own ? QColor("#b7bcff") : QColor("#e2ebfb"),
                     target.own ? QColor("#5553a7") : QColor("#9cacc8"), 6);
                const QRect emojiRect(target.rect.left() + reactionIconPadding, target.rect.top(),
                                      reactionGlyphBox, target.rect.height());
                p.setFont(reactionFont(17));
                p.setPen(Ink);
                p.drawText(emojiRect, Qt::AlignCenter, reactionGlyph(target.code));
                p.setFont(rowFont(ui::secondaryTextPixels, target.own));
                p.setPen(target.own ? QColor("#24225b") : QColor("#23375e"));
                p.drawText(target.rect.adjusted(reactionCountOffset, 0, -reactionCountPadding, 0),
                           Qt::AlignCenter | Qt::TextSingleLine, target.count);
            }
        }
        if (!delivery.isEmpty())
            line(p, deliveryRect(bubble),
                 delivery, messageMetaPixels, ConversationMetaInk, false, Qt::AlignRight | Qt::AlignVCenter);
    }

    void paintNotice(QPainter &p, const QStyleOptionViewItem &option, const QModelIndex &index,
                     const QString &title, const QString &detail, const QString &meta) const
    {
        const QRect r = noticeRect(option.rect);
        const bool unread = index.data(Count).toBool();
        fill(p, r, unread ? QColor("#e7f0f0") : Surface, itemOutline(option, Border));
        if (unread) {
            p.setPen(Qt::NoPen);
            p.setBrush(Accent);
            p.drawEllipse(QPoint(r.left() + 13, r.top() + 18), 3, 3);
        }
        const QRect heading(r.left() + 25, r.top() + 6, qMax(20, r.width() - 129), 23);
        line(p, heading, title, ui::bodyTextPixels, Ink, true);
        line(p, QRect(r.right() - 97, r.top() + 6, 85, 23), meta, messageMetaPixels, ConversationMetaInk, false,
             Qt::AlignRight | Qt::AlignVCenter);
        const auto labels = index.data(NoticeActions).toStringList();
        const auto buttons = noticeActionRects(option.rect, index);
        body(p, noticeBodyRect(option.rect, index), index, detail, Ink);
        paintLinkCards(p, option.rect, index);
        for (int i = 0; i < buttons.size(); ++i) {
            fill(p, buttons[i], QColor("#f5faf7"), QColor("#b4cdc6"), 6);
            line(p, buttons[i].adjusted(5, 0, -5, 0), labels[i], ui::bodyTextPixels, Accent, true, Qt::AlignCenter);
        }
    }

    static void paintCard(QPainter &p, const QStyleOptionViewItem &option, const QString &title,
                          const QString &detail)
    {
        const QRect r = option.rect.adjusted(1, 2, -2, -3);
        if (title.isEmpty()) {
            paragraph(p, r.adjusted(7, 3, -7, 0), detail, ui::bodyTextPixels, Muted);
            return;
        }
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        fill(p, r, selected ? Selected : (hovered ? QColor("#f5fbfa") : Surface),
             itemOutline(option, selected ? Accent : Border));
        paragraph(p, r.adjusted(10, 8, -10, detail.isEmpty() ? -6 : -25), title, ui::bodyTextPixels, Ink, true);
        if (!detail.isEmpty())
            line(p, QRect(r.left() + 10, r.bottom() - 22, r.width() - 20, 19), detail, ui::secondaryTextPixels, Muted);
    }
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
