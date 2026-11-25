#include "cloud/cloud_backend.h"
#include "cloud/avatar_redirect.h"
#include "cloud/cloud_avatars.h"

#include "core/origin_guard.h"

#include <QDir>
#include <QHostAddress>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrlQuery>
#include <algorithm>
#include <utility>

namespace bx {
namespace {

constexpr qsizetype maxMetadataUrls = 256;
constexpr qsizetype maxPendingImages = 64;
constexpr int rasterTimeoutMs = 8000;

bool isRasterCdn(const QUrl &url)
{
    // https://apidocs.bitrix24.ru/api-reference/chat-bots/outdated/chats/imbot-dialog-get.html
    return url.host().compare(QLatin1String("cdn-ru.bitrix24.ru"), Qt::CaseInsensitive) == 0;
}

bool rasterAddressAllowed(const QUrl &url, const QUrl &portal, bool allowLoopback)
{
    if (!url.isValid() || url.isRelative() || !url.userInfo().isEmpty() || url.hasFragment())
        return false;
    const auto path = url.path(QUrl::FullyDecoded);
    const auto cleanPath = QDir::cleanPath(path);
    static const QRegularExpression unsafePath(QStringLiteral("[\\x00-\\x1f\\x7f]|%[0-9a-fA-F]{2}"));
    if (path.size() < 2 || path.contains(QLatin1Char('\\'))
        || (cleanPath != path && cleanPath + QLatin1Char('/') != path)
        || unsafePath.match(path).hasMatch())
        return false;
    for (const auto &entry : QUrlQuery(url).queryItems(QUrl::FullyDecoded)) {
        const auto key = entry.first.toLower();
        if (key == QLatin1String("auth") || key == QLatin1String("sessid") || key == QLatin1String("csrf")
            || key == QLatin1String("access_token") || key == QLatin1String("refresh_token"))
            return false;
    }

    const bool sameOrigin = OriginGuard(portal).isSameOrigin(url);
    if (allowLoopback && sameOrigin && OriginGuard::isLoopback(url) && url.scheme() == QLatin1String("http"))
        return true;
    if (url.scheme() != QLatin1String("https"))
        return false;

    const auto host = url.host().toLower();
    QHostAddress literalIp;
    if (host.isEmpty() || literalIp.setAddress(host) || host == QLatin1String("localhost")
        || host.endsWith(QLatin1String(".localhost")) || host.endsWith(QLatin1String(".local")))
        return false;
    return isRasterCdn(url) ? url.port(443) == 443 : sameOrigin;
}

bool portalResizePath(const QUrl &url)
{
    static const QRegularExpression path(QStringLiteral("^/b[0-9]+/resize_cache/"));
    return path.match(url.path(QUrl::FullyDecoded)).hasMatch();
}

bool staticRasterPathAllowed(const QUrl &url)
{
    const auto path = url.path(QUrl::FullyDecoded);
    // https://apidocs.bitrix24.ru/api-reference/sonet-group/socialnetwork-api-workgroup-list.html
    static const QRegularExpression cdnPath(QStringLiteral("^/b[0-9]+/"));
    if (isRasterCdn(url))
        return cdnPath.match(path).hasMatch();
    return path.startsWith(QLatin1String("/upload/")) || portalResizePath(url);
}

bool exactFileId(const QString &candidate, const QString &fileId)
{
    static const QRegularExpression digits(QStringLiteral("^[0-9]{1,19}$"));
    const auto expected = fileId.toLongLong();
    return expected > 0 && digits.match(candidate).hasMatch() && candidate.toLongLong() == expected;
}

bool diskImagePathAllowed(const QUrl &url, const QString &fileId, bool previewField)
{
    if (isRasterCdn(url))
        return false;
    const auto path = url.path(QUrl::FullyDecoded);
    // im.dialog.messages.get and im.disk.file.commit document these image display routes.
    // The download action is only accepted when it is explicitly named by urlPreview.
    if (path == QLatin1String("/bitrix/services/main/ajax.php")) {
        const QUrlQuery query(url);
        const auto actions = query.allQueryItemValues(QStringLiteral("action"), QUrl::FullyDecoded);
        const auto ids = query.allQueryItemValues(QStringLiteral("fileId"), QUrl::FullyDecoded);
        return actions.size() == 1 && ids.size() == 1 && exactFileId(ids.first(), fileId)
            && (actions.first() == QLatin1String("disk.api.file.showImage")
                || (previewField && actions.first() == QLatin1String("disk.api.file.download")));
    }
    static const QRegularExpression showFile(QStringLiteral("^/disk/showFile/([0-9]+)/?$"));
    return exactFileId(showFile.match(path).captured(1), fileId);
}

bool rasterTypeAllowed(const QByteArray &mime, bool allowBinary)
{
    return cloud::isRasterImageMime(mime) || (allowBinary && mime == "application/octet-stream");
}

QByteArray mimeType(const QNetworkReply &reply)
{
    return reply.rawHeader("Content-Type").split(';').first().trimmed().toLower();
}

bool matchesRasterType(const QByteArray &mime, const QByteArray &bytes)
{
    if (mime == "image/png")
        return bytes.startsWith(QByteArrayView("\x89PNG\r\n\x1a\n", 8)) && bytes.mid(12, 4) == "IHDR";
    if (mime == "image/jpeg")
        return bytes.startsWith(QByteArrayView("\xff\xd8\xff", 3));
    if (mime == "image/webp")
        return bytes.startsWith("RIFF") && bytes.mid(8, 4) == "WEBP";
    return mime == "image/gif" && (bytes.startsWith("GIF87a") || bytes.startsWith("GIF89a"));
}

bool exactQueryId(const QUrlQuery &query, const QString &parameter, const QString &expected)
{
    const auto ids = query.allQueryItemValues(parameter, QUrl::FullyDecoded);
    return ids.size() == 1 && exactFileId(ids.first(), expected);
}

} // namespace

bool cloud::isPortalDiskDownload(const QUrl &url, const QString &attachmentId, const QString &objectId)
{
    const auto path = url.path(QUrl::FullyDecoded);
    if (path == QLatin1String("/bitrix/tools/disk/uf.php") || path == QLatin1String("/bitrix/services/main/ajax.php")) {
        const QUrlQuery query(url);
        const auto actions = query.allQueryItemValues(QStringLiteral("action"), QUrl::FullyDecoded);
        if (actions.size() != 1)
            return false;
        if (path == QLatin1String("/bitrix/tools/disk/uf.php"))
            return actions.first() == QLatin1String("download")
                && exactQueryId(query, QStringLiteral("attachedId"), attachmentId);
        return (actions.first() == QLatin1String("disk.attachedObject.download")
                && exactQueryId(query, QStringLiteral("attachedObjectId"), attachmentId))
            || (actions.first() == QLatin1String("disk.api.file.download")
                && exactQueryId(query, QStringLiteral("fileId"), objectId));
    }
    static const QRegularExpression downloadFile(QStringLiteral("^/disk/downloadFile/([0-9]+)/?$"));
    return exactFileId(downloadFile.match(path).captured(1), objectId);
}

QUrl cloud::avatarCdnRedirect(const QUrl &portal, const QUrl &source, int status, const QUrl &target)
{
    // The portal's resize-cache responds with a permanent redirect to the same
    // asset on the Bitrix CDN. Do not send portal credentials to the CDN.
    if (status != 301 || isRasterCdn(source) || !OriginGuard(portal).isSameOrigin(source)
        || !portalResizePath(source) || !rasterAddressAllowed(source, portal, false)
        || !isRasterCdn(target) || !rasterAddressAllowed(target, portal, false)
        || source.path(QUrl::FullyEncoded) != target.path(QUrl::FullyEncoded)
        || source.hasQuery() != target.hasQuery()
        || source.query(QUrl::FullyEncoded) != target.query(QUrl::FullyEncoded))
        return {};
    return target;
}

QUrl CloudBackend::avatarUrlFromPortal(const QString &value) const
{
    if (value.isEmpty() || value.size() > 2048)
        return {};
    QUrl supplied(value, QUrl::StrictMode);
    const auto base = OriginGuard(m_session.origin).resolve(QStringLiteral("/"));
    if (!supplied.isValid() && value.contains(QLatin1Char(' '))) {
        // The Kanban renderer applies encodeURI to photos supplied with literal spaces.
        const auto queryStart = value.indexOf(QLatin1Char('?'));
        auto path = value.left(queryStart);
        path.replace(QLatin1Char(' '), QStringLiteral("%20"));
        if (queryStart >= 0)
            path += value.mid(queryStart);
        supplied = QUrl(path, QUrl::StrictMode);
        if (!portalResizePath(base.resolved(supplied)))
            return {};
    }
    if (!supplied.isValid())
        return {};
    const auto url = base.resolved(supplied);
    if (!rasterAddressAllowed(url, m_session.origin, m_allowLoopback) || !staticRasterPathAllowed(url))
        return {};
    return registerRasterAsset(url, m_avatarAssets);
}

QUrl CloudBackend::messageImageUrlFromPortal(const QString &value, const QString &fileId, bool previewField,
                                            RasterRegistration registration) const
{
    if (value.isEmpty() || value.size() > 4096)
        return {};
    const QUrl supplied(value, QUrl::StrictMode);
    if (!supplied.isValid())
        return {};
    const auto url = OriginGuard(m_session.origin).resolve(QStringLiteral("/")).resolved(supplied);
    if (!rasterAddressAllowed(url, m_session.origin, m_allowLoopback)
        || (!staticRasterPathAllowed(url) && !diskImagePathAllowed(url, fileId, previewField)))
        return {};
    return registerRasterAsset(url, m_messageImageAssets, registration);
}

QUrl CloudBackend::taskImageUrlFromPortal(const QString &value, const QString &attachmentId, const QString &objectId,
                                         bool original, RasterRegistration registration) const
{
    if (!original) {
        if (const auto image = messageImageUrlFromPortal(value, objectId, true, registration); !image.isEmpty())
            return image;
    }
    if (value.isEmpty() || value.size() > 4096)
        return {};
    const auto url = m_session.origin.resolved(QUrl(value, QUrl::StrictMode));
    if (!rasterAddressAllowed(url, m_session.origin, m_allowLoopback) || isRasterCdn(url))
        return {};
    const auto path = url.path(QUrl::FullyDecoded);
    const QUrlQuery query(url);
    const bool attached = path == QLatin1String("/bitrix/tools/disk/uf.php");
    const auto actions = query.allQueryItemValues(QStringLiteral("action"), QUrl::FullyDecoded);
    if (!original && attached && actions.size() == 1 && actions.first() == QLatin1String("show")
        && exactQueryId(query, QStringLiteral("attachedId"), attachmentId))
        return registerRasterAsset(url, m_messageImageAssets, registration);
    if ((original || attached) && cloud::isPortalDiskDownload(url, attachmentId, objectId))
        return registerRasterAsset(url, m_messageImageAssets, registration);
    return {};
}

QUrl CloudBackend::registerRasterAsset(const QUrl &url, const RasterAssets &assets,
                                      RasterRegistration registration) const
{
    auto &registered = registration == RasterRegistration::Standby
        ? m_standbyMessageImageRegistrations : assets.registrations;
    if (!registered.urls.contains(url)) {
        registered.urls.insert(url);
        registered.order.enqueue(url);
        if (registered.order.size() > maxMetadataUrls)
            registered.urls.remove(registered.order.dequeue());
    }
    return url;
}

void CloudBackend::requestAvatar(const QUrl &url)
{
    requestRasterAsset(url, m_avatarAssets, &Backend::avatarLoaded);
}

void CloudBackend::requestMessageImage(const QUrl &url)
{
    requestRasterAsset(url, m_messageImageAssets, &Backend::messageImageLoaded);
}

void CloudBackend::requestRasterAsset(const QUrl &url, RasterAssets &assets, RasterLoaded loaded)
{
    const bool registered = assets.registrations.urls.contains(url)
        || (&assets == &m_messageImageAssets && m_standbyMessageImageRegistrations.urls.contains(url));
    if (!m_started || m_authExpired || !registered) {
        (this->*loaded)(url, {});
        return;
    }
    if (const auto *entry = assets.cache.object(url)) {
        const auto bytes = *entry;
        (this->*loaded)(url, bytes);
        return;
    }
    if (assets.pending.contains(url))
        return;
    if (assets.pending.size() >= maxPendingImages) {
        (this->*loaded)(url, {});
        return;
    }
    assets.pending.insert(url);
    assets.queue.enqueue(url);
    startRasterAssets(assets, loaded);
}

void CloudBackend::startRasterAssets(RasterAssets &assets, RasterLoaded loaded)
{
    while (m_started && !m_authExpired
           && assets.replies.size() < assets.concurrentRequests && !assets.queue.isEmpty()) {
        const auto url = assets.queue.dequeue();
        startRasterAsset(url, url, assets, loaded);
    }
}

void CloudBackend::startRasterAsset(const QUrl &key, const QUrl &url, RasterAssets &assets, RasterLoaded loaded)
{
    QNetworkRequest request(url);
    request.setPriority(QNetworkRequest::LowPriority);
    request.setTransferTimeout(rasterTimeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    const auto original = m_registeredMessageFileUrls.constFind(url);
    const auto maxBytes = assets.maxBytes;
    const bool allowBinary = &assets == &m_messageImageAssets && original != m_registeredMessageFileUrls.cend()
        && original->photo && original->size > 0 && original->size <= maxBytes;
    request.setRawHeader("Accept", allowBinary ? "image/png,image/jpeg,image/webp,image/gif,application/octet-stream"
                                              : "image/png,image/jpeg,image/webp,image/gif");
    request.setRawHeader("Accept-Encoding", "identity");
    if (!isRasterCdn(url) && OriginGuard(m_session.origin).isSameOrigin(url))
        request.setRawHeader("Cookie", m_session.cookie);

    auto *reply = m_http.get(request);
    assets.replies.insert(key, reply);
    reply->setReadBufferSize(maxBytes + 1);
    QTimer::singleShot(rasterTimeoutMs, reply, [reply] {
        if (!reply->isFinished())
            reply->abort();
    });
    connect(reply, &QNetworkReply::metaDataChanged, this, [reply, maxBytes, allowBinary] {
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto advertisedLength = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        const auto encoding = reply->rawHeader("Content-Encoding").trimmed().toLower();
        if (status != 200 || advertisedLength > maxBytes || !rasterTypeAllowed(mimeType(*reply), allowBinary)
            || (!encoding.isEmpty() && encoding != "identity"))
            reply->abort();
    });
    connect(reply, &QIODevice::readyRead, this, [reply, maxBytes] {
        if (reply->bytesAvailable() > maxBytes)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, key, url, reply, &assets, maxBytes, allowBinary, loaded] {
        assets.replies.remove(key);
        reply->deleteLater();
        if (!m_started)
            return;
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (!m_authExpired && &assets == &m_avatarAssets && key == url && status == 301) {
            const auto location = reply->rawHeader("Location");
            if (location.size() <= 2048) {
                const auto target = cloud::avatarCdnRedirect(
                    m_session.origin, url, status, QUrl::fromEncoded(location, QUrl::StrictMode));
                if (!target.isEmpty()) {
                    startRasterAsset(key, target, assets, loaded);
                    return;
                }
            }
        }
        assets.pending.remove(key);

        QByteArray bytes;
        if (!m_authExpired && reply->error() == QNetworkReply::NoError
            && status == 200) {
            const auto candidate = reply->read(maxBytes + 1);
            const auto type = mimeType(*reply);
            const bool raster = matchesRasterType(type, candidate)
                || (allowBinary && type == "application/octet-stream"
                    && (matchesRasterType("image/jpeg", candidate) || matchesRasterType("image/png", candidate)
                        || matchesRasterType("image/webp", candidate) || matchesRasterType("image/gif", candidate)));
            if (candidate.size() <= maxBytes && raster) {
                bytes = candidate;
                assets.cache.insert(key, new QByteArray(bytes), std::max<qsizetype>(1024, bytes.size()));
            }
        }
        startRasterAssets(assets, loaded);
        (this->*loaded)(key, bytes);
    });
}

void CloudBackend::clearRasterAssets()
{
    m_standbyMessageImageRegistrations = {};
    for (auto *assets : {&m_avatarAssets, &m_messageImageAssets}) {
        assets->registrations = {};
        assets->queue.clear();
        assets->pending.clear();
        assets->cache.clear();
        const auto replies = std::exchange(assets->replies, {});
        for (auto *reply : replies) {
            QObject::disconnect(reply, nullptr, this, nullptr);
            reply->abort();
            reply->deleteLater();
        }
    }
}

} // namespace bx
