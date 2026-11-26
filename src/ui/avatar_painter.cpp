#include "ui/avatar_painter.h"

#include "ui/workspace_row_style.h"

#include <QPainter>
#include <QPainterPath>

#include <iterator>

namespace bx::workspace {

void paintOnlineDot(QPainter &painter, const QRect &avatar)
{
    const int size = avatar.width() > 28 ? 11 : 9;
    painter.save();
    painter.setPen(QPen(Surface, 1.5));
    painter.setBrush(QColor("#24875b"));
    painter.drawEllipse(QRect(avatar.right() - size + 1, avatar.bottom() - size + 1, size, size));
    painter.restore();
}

bool paintProjectAvatarIcon(QPainter &painter, const QRectF &box, const QString &type, const QColor &color)
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

void paintAvatar(QPainter &painter, const QRect &box, const QString &title,
                 AvatarKind kind, const QPixmap &photo,
                 const QColor &projectColor, const QString &projectVariant, const QString &projectType)
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

} // namespace bx::workspace
