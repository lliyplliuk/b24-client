#pragma once

#include "ui/workspace_style.h"

namespace bx::ui {

class AvatarBadge final : public QWidget {
public:
    explicit AvatarBadge(QWidget *parent);
    void setTitle(const QString &title, workspace::AvatarKind kind = workspace::AvatarKind::Person,
                  const QColor &projectColor = {}, const QString &projectVariant = {}, const QString &projectType = {});
    void setPhoto(const QPixmap &photo);
    void setOnline(bool online);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QString m_title;
    workspace::AvatarKind m_kind = workspace::AvatarKind::Person;
    QPixmap m_photo;
    QColor m_projectColor;
    QString m_projectVariant;
    QString m_projectType;
    bool m_online = false;
};

} // namespace bx::ui
