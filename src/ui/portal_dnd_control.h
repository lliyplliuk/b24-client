#pragma once

#include "core/backend.h"
#include <QVBoxLayout>
#include <functional>

namespace bx::ui {

QString portalStatusLabel(const QString &status);

void addPortalDndControl(QVBoxLayout *content, Backend *backend, std::function<bool()> writable);

} // namespace bx::ui
