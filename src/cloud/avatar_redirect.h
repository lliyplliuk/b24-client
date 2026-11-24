#pragma once

#include <QUrl>

namespace bx::cloud {

QUrl avatarCdnRedirect(const QUrl &portal, const QUrl &source, int status, const QUrl &target);

}
