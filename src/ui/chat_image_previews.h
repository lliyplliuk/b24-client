#pragma once

#include <QCache>
#include <QPixmap>
#include <QSet>
#include <QUrl>

namespace bx::ui {

QImage decodeChatPreviewRaster(const QByteArray &bytes, const QSize &bounds, const QByteArray &contentType = {});

QSize chatPreviewDisplaySize(const QSize &source, int availableWidth);

class ChatImagePreviews final {
public:
    bool begin(const QUrl &url);
    bool complete(const QUrl &url, const QByteArray &bytes);
    QUrl requestUrl(const QUrl &preferred, const QUrl &original) const;
    bool pending(const QUrl &url) const { return m_pending.contains(url); }
    const QPixmap *photo(const QUrl &url) const { return m_images.object(url); }
    bool failed(const QUrl &url) const { return m_failures.contains(url); }
    void clear();

private:
    QSet<QUrl> m_pending;
    QCache<QUrl, QPixmap> m_images{6 * 1024};
    QCache<QUrl, bool> m_failures{256};
};

} // namespace bx::ui
