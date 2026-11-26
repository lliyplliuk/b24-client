#include "ui/requisite_avatar.h"

#include "ui/workspace_style.h"

#include <QPainter>

namespace bx::ui {

bool hasRequisiteProjectMark(const QUrl &url, const QColor &color, const QString &variant, const QString &type)
{
    return !type.isEmpty() || (url.isEmpty() && (color.isValid() || !variant.isEmpty()));
}

QIcon requisiteAvatarIcon(const RequisiteAvatarSource &source, const QString &name,
                          const QUrl &url, workspace::AvatarKind kind, const QColor &color,
                          const QString &variant, const QString &type)
{
    const auto *photo = source && !url.isEmpty() ? source(url) : nullptr;
    if (kind == workspace::AvatarKind::Project && (!photo || photo->isNull())
        && !hasRequisiteProjectMark(url, color, variant, type))
        return {};
    QPixmap icon(22, 22);
    icon.fill(Qt::transparent);
    QPainter painter(&icon);
    painter.setRenderHint(QPainter::Antialiasing);
    workspace::paintAvatar(painter, QRect(0, 0, 21, 21), name, kind, photo ? *photo : QPixmap(),
                           color, variant, type);
    painter.end();
    return QIcon(icon);
}

} // namespace bx::ui
