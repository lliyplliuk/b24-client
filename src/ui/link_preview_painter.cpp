#include "ui/link_preview_painter.h"
#include "ui/row_paint_primitives.h"
#include "ui/avatar_painter.h"
#include "ui/glyphs.h"

#include <QPainterPath>

#include <utility>

namespace bx::workspace {

QVector<RowPainter::LinkCard> RowPainter::linkPreviews(const QModelIndex &index) const
{
    QVector<LinkCard> cards;
    if (m_style == Row::Notice && !index.data(NoticePresentation).value<NoticeRow>().expanded)
        return cards;
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

int RowPainter::linkCardHeight(const MessageLinkPreview &preview)
{
    const int textHeight = 38 + linkDetailLineHeight(preview) * preview.details.size();
    return preview.thumbnailUrl.isEmpty() ? textHeight : qMax(76, textHeight);
}

const QString &RowPainter::taskLinkDetailSeparator()
{
    static const auto separator = QStringLiteral(" · ");
    return separator;
}

int RowPainter::taskLinkLabelExtent(const QFontMetrics &labels, const QString &label)
{
    return qMax(labels.horizontalAdvance(label), labels.boundingRect(label).width()) + 4;
}

int RowPainter::minimumLinkCardWidth(const MessageLinkPreview &preview, int available)
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

int RowPainter::linkDetailLineHeight(const MessageLinkPreview &preview)
{
    return preview.kind == MessageLinkPreviewKind::Task ? 21 : 19;
}

int RowPainter::linkPreviewHeight(const QModelIndex &index) const
{
    int height = 0;
    for (const auto &card : linkPreviews(index))
        height += linkCardHeight(card.second) + linkCardGap;
    return height;
}

QRect RowPainter::firstLinkCard(const QRect &row, const QModelIndex &index) const
{
    if (m_style == Row::Notice) {
        const auto card = noticeRect(row, index);
        const auto detail = noticeBodyRect(row, index);
        return QRect(detail.left(), detail.bottom() + 6, qMax(1, card.width() - 40), 1);
    }
    auto rect = firstFileRect(row, index, AttachmentSection::Documents);
    rect.translate(0, fileHeight(files(index, MessageFiles), rect.width(), AttachmentSection::Documents));
    return rect;
}

void LinkPreviewPainter::paintLinkProjectAvatar(QPainter &p, const QRect &box, const Project &project) const
{
    const auto *photo = rows.m_linkAvatar && !project.avatarUrl.isEmpty() ? rows.m_linkAvatar(project.avatarUrl) : nullptr;
    paintAvatar(p, box, project.name, AvatarKind::Project, photo ? *photo : QPixmap{},
                QColor(project.avatarColor), project.avatarVariant, project.avatarType);
}

void LinkPreviewPainter::paintLinkIcon(QPainter &p, const QRect &box, const MessageLinkPreview &preview) const
{
    if (preview.kind == MessageLinkPreviewKind::Task && preview.project) {
        paintLinkProjectAvatar(p, box, *preview.project);
        return;
    }
    const auto &url = preview.imageUrl();
    const auto *photo = rows.m_linkAvatar && !url.isEmpty() ? rows.m_linkAvatar(url) : nullptr;
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

void LinkPreviewPainter::paintLinkCards(QPainter &p, const QRect &row, const QModelIndex &index) const
{
    auto rect = rows.firstLinkCard(row, index);
    for (const auto &[href, preview] : rows.linkPreviews(index)) {
        rect.setHeight(rows.linkCardHeight(preview));
        fill(p, rect, QColor("#f6faf8"), QColor("#aac6ba"), 6);
        paintLinkIcon(p, QRect(rect.left() + 8, rect.top() + 6, 24, 24), preview);
        const int textLeft = rect.left() + 40;
        const int thumbnailWidth = preview.thumbnailUrl.isEmpty() ? 0 : qBound(0, rect.width() - 145, 80);
        const int textWidth = qMax(0, rect.width() - 48 - (thumbnailWidth > 0 ? thumbnailWidth + 8 : 0));
        line(p, QRect(textLeft, rect.top() + 7, textWidth, 22), preview.title, ui::bodyTextPixels, Accent, true);
        if (thumbnailWidth > 0) {
            const auto *photo = rows.m_messagePreview ? rows.m_messagePreview(preview.thumbnailUrl) : nullptr;
            const auto *thumbnail = !photo && rows.m_linkThumbnail ? rows.m_linkThumbnail(preview.thumbnailUrl) : nullptr;
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
        rect.translate(0, rect.height() + RowPainter::linkCardGap);
    }
}

int LinkPreviewPainter::paintTaskLinkDetailIcon(QPainter &p, const QRect &row, const QString &label,
                                        const MessageLinkPreview &preview) const
{
    constexpr int iconSize = 15;
    constexpr int gap = 5;
    if (row.width() < iconSize + gap + 24)
        return 0;
    const QRect box(row.left(), row.top() + (row.height() - iconSize) / 2, iconSize, iconSize);
    if (label == QStringLiteral("Постановщик") || label == QStringLiteral("Исполнитель")) {
        const auto &url = label == QStringLiteral("Постановщик") ? preview.creatorAvatarUrl : preview.assigneeAvatarUrl;
        const auto *photo = rows.m_linkAvatar && !url.isEmpty() ? rows.m_linkAvatar(url) : nullptr;
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

void LinkPreviewPainter::paintLinkDetails(QPainter &p, const QRect &area, const MessageLinkPreview &preview) const
{
    const int lineHeight = rows.linkDetailLineHeight(preview);
    const auto &separator = rows.taskLinkDetailSeparator();
    const bool task = preview.kind == MessageLinkPreviewKind::Task;
    const QFontMetrics labels(rowFont(ui::secondaryTextPixels, true));
    int labelWidth = 0;
    if (task && area.width() >= 225) {
        for (const auto &detail : preview.details) {
            const auto split = detail.indexOf(separator);
            if (split > 0)
                labelWidth = qMax(labelWidth, rows.taskLinkLabelExtent(labels, detail.left(split)));
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
                : qMin(rows.taskLinkLabelExtent(labels, label), qMax(0, area.width() - 54));
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

} // namespace bx::workspace
