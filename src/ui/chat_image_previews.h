#pragma once

#include <QBuffer>
#include <QCache>
#include <QImageReader>
#include <QPixmap>
#include <QSet>
#include <QUrl>

namespace bx::ui {

inline QImage decodeChatPreviewRaster(const QByteArray &bytes, const QSize &bounds, const QByteArray &contentType = {})
{
    if (bytes.isEmpty())
        return {};
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly))
        return {};
    QImageReader reader(&buffer);
    reader.setDecideFormatFromContent(true);
    const auto format = reader.format();
    if (format != "jpeg" && format != "png" && format != "webp" && format != "gif")
        return {};
    if (!contentType.isEmpty() && contentType != "image/" + format)
        return {};
    const auto source = reader.size();
    if (source.isEmpty() || source.width() > 4096 || source.height() > 4096
        || qint64(source.width()) * source.height() > 4 * 1024 * 1024)
        return {};
    if (source.width() > bounds.width() || source.height() > bounds.height()) {
        const auto scaled = source.scaled(bounds, Qt::KeepAspectRatio);
        reader.setScaledSize(QSize(qMax(1, scaled.width()), qMax(1, scaled.height())));
    }
    auto image = reader.read();
    if (image.width() > bounds.width() || image.height() > bounds.height())
        image = image.scaled(bounds, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

class ChatImagePreviews final {
public:
    bool begin(const QUrl &url)
    {
        if (m_pending.size() >= 2 || !url.isValid() || url.scheme() != QStringLiteral("https") ||
            url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasFragment() ||
            m_pending.contains(url) || m_failures.contains(url) || m_images.contains(url))
            return false;
        m_pending.insert(url);
        return true;
    }

    bool complete(const QUrl &url, const QByteArray &bytes)
    {
        if (!m_pending.remove(url))
            return false;
        const auto image = bytes.size() <= 512 * 1024 ? decodeChatPreviewRaster(bytes, QSize(640, 400)) : QImage{};
        if (image.isNull()) {
            m_failures.insert(url, new bool(true));
        } else {
            auto *photo = new QPixmap(QPixmap::fromImage(image));
            if (photo->isNull()) {
                delete photo;
                m_failures.insert(url, new bool(true));
            } else {
                const auto bytes = qsizetype(photo->width()) * photo->height() * 4;
                const auto cost = qMax<qsizetype>(1, (bytes + 1023) / 1024);
                m_images.insert(url, photo, cost);
            }
        }
        return true;
    }

    const QPixmap *photo(const QUrl &url) const { return m_images.object(url); }
    bool failed(const QUrl &url) const { return m_failures.contains(url); }
    void clear()
    {
        m_pending.clear();
        m_images.clear();
        m_failures.clear();
    }

private:
    QSet<QUrl> m_pending;
    QCache<QUrl, QPixmap> m_images{6 * 1024};
    QCache<QUrl, bool> m_failures{256};
};

inline QSize chatPreviewDisplaySize(const QSize &source, int availableWidth)
{
    const int width = qBound(1, availableWidth, 320);
    const auto aspect = source.isEmpty() ? QSize(4, 3) : source;
    const auto fitted = aspect.scaled(width, 190, Qt::KeepAspectRatio);
    return {qBound(qMin(width, 68), fitted.width(), width), qBound(54, fitted.height(), 190)};
}

} // namespace bx::ui
