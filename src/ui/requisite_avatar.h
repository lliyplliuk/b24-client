#pragma once

#include <QColor>
#include <QIcon>
#include <QUrl>

#include <functional>

namespace bx::workspace { enum class AvatarKind; }

namespace bx::ui {

using RequisiteAvatarSource = std::function<const QPixmap *(const QUrl &)>;

bool hasRequisiteProjectMark(const QUrl &url, const QColor &color, const QString &variant, const QString &type);
QIcon requisiteAvatarIcon(const RequisiteAvatarSource &source, const QString &name,
                          const QUrl &url, workspace::AvatarKind kind, const QColor &color = {},
                          const QString &variant = {}, const QString &type = {});

} // namespace bx::ui
