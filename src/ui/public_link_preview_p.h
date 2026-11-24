#pragma once

#include "ui/public_link_preview.h"

#include <QHostAddress>
#include <QNetworkRequest>

namespace bx::ui::public_link_preview {

struct HtmlMetadata {
    QString title;
    QString description;
    QUrl thumbnailUrl;
};

QByteArray publicHttpsHost(const QUrl &url);
std::optional<QHostAddress> publicAddress(const QList<QHostAddress> &addresses);
QNetworkRequest pinnedRequest(const QUrl &url, const QByteArray &host, const QHostAddress &address, bool image = false);
QUrl thumbnailTarget(const QUrl &base, const QByteArray &reference);
std::optional<HtmlMetadata> htmlMetadata(const QByteArray &bytes, const QUrl &page = {});
QImage decodeThumbnail(const QByteArray &bytes, const QByteArray &contentType);

} // namespace bx::ui::public_link_preview
