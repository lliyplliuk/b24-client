#include "ui/directory_row_painter.h"
#include "ui/row_paint_primitives.h"
#include "ui/avatar_painter.h"
#include "ui/typography.h"
#include <QDateTime>

namespace bx::workspace {

void paintDirectory(QPainter &p, const QStyleOptionViewItem &option, const QModelIndex &index,
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
    paintRowTime(p, QRect(right - metaWidth, r.top() + 5, metaWidth, 21), meta, Muted);
    const int badgesWidth = unreadWidth + anchorWidth + (pinned && !projectPin ? 15 : 0);
    const QRect preview(textLeft, r.top() + 26, qMax(1, right - textLeft - badgesWidth), 20);
    if (index.data(ChatMessagePreview).toBool())
        messagePreviewLine(p, preview, detail.simplified(), Muted);
    else
        line(p, preview, detail.simplified(), ui::secondaryTextPixels, Muted);
    int anchorRight = right - unreadWidth;
    const auto paintAnchor = [&](const QString &symbol, const QColor &color) {
        const QRect badge(anchorRight - 16, r.top() + 27, 16, 18);
        paintBadge(p, badge, symbol, 11, color, Qt::white, 8);
        anchorRight -= 19;
    };
    if (mention)
        paintAnchor(QStringLiteral("@"), Accent);
    if (reaction)
        paintAnchor(QStringLiteral("♥"), QColor("#a64b62"));
    if (pillWidth > 0) {
        const QRect badge(right - pillWidth, r.top() + 26, pillWidth, 20);
        paintBadge(p, badge, unreadCaption, ui::secondaryTextPixels, Accent, Qt::white, 8);
    } else if (markedUnread) {
        p.setPen(Qt::NoPen);
        p.setBrush(Accent);
        p.drawEllipse(QRect(right - 9, r.top() + 32, 7, 7));
    }
}

} // namespace bx::workspace
