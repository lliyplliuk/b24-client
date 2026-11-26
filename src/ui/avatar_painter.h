#pragma once

#include <QColor>
#include <QPixmap>

class QPainter;

namespace bx::workspace {

enum class AvatarKind { Person, Chat, Project };

void paintOnlineDot(QPainter &painter, const QRect &avatar);

bool paintProjectAvatarIcon(QPainter &painter, const QRectF &box, const QString &type, const QColor &color);

void paintAvatar(QPainter &painter, const QRect &box, const QString &title,
                 AvatarKind kind = AvatarKind::Person, const QPixmap &photo = {},
                 const QColor &projectColor = {}, const QString &projectVariant = {}, const QString &projectType = {});

} // namespace bx::workspace
