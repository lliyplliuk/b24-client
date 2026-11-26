#include "ui/avatar_badge.h"

namespace bx::ui {

AvatarBadge::AvatarBadge(QWidget *parent)
    : QWidget(parent)
{
    setFixedSize(35, 35);
}

void AvatarBadge::setTitle(const QString &title, workspace::AvatarKind kind,
                           const QColor &projectColor, const QString &projectVariant, const QString &projectType)
{
    m_title = title;
    m_kind = kind;
    m_projectColor = projectColor;
    m_projectVariant = projectVariant;
    m_projectType = projectType;
    m_photo = {};
    setVisible(!title.isEmpty());
    update();
}

void AvatarBadge::setPhoto(const QPixmap &photo)
{
    m_photo = photo;
    update();
}

void AvatarBadge::setOnline(bool online)
{
    m_online = online;
    update();
}

void AvatarBadge::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    workspace::paintAvatar(painter, rect().adjusted(0, 0, -1, -1), m_title, m_kind, m_photo,
                           m_projectColor, m_projectVariant, m_projectType);
    if (m_online && m_kind == workspace::AvatarKind::Person)
        workspace::paintOnlineDot(painter, rect().adjusted(0, 0, -1, -1));
}

} // namespace bx::ui
