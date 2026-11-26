#include "cloud/cloud_backend.h"

#include "cloud/cloud_avatars.h"
#include "cloud/cloud_chat_fields.h"
#include "cloud/cloud_rows.h"
#include "core/download_target.h"
#include "core/origin_guard.h"
#include "core/tunables.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QNetworkReply>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrlQuery>
#include <QtEndian>

namespace bx {
using chat_fields::positiveId;
using cloud_rows::field;
using cloud_rows::plainFileName;

namespace {

constexpr qint64 maxMessageFileBytes = 2LL * 1024 * 1024 * 1024;
constexpr qint64 maxMessagePhotoBytes = 24LL * 1024 * 1024;
constexpr int videoDownloadTimeoutMs = 30000;
constexpr qsizetype maxMessageFileUrlChars = 4096;
constexpr qint64 downloadBlockBytes = 64 * 1024;
constexpr qsizetype maxRegisteredMessageFiles = 256;

const QString downloadOperation = operationKey(OperationKind::MessageFileDownload);

bool videoDiskDownloadAllowed(const QUrl &url, const QString &attachmentId, const QString &objectId)
{
    if (!cloud::isPortalDiskDownload(url, attachmentId, objectId))
        return false;
    // Keep normal Bitrix parameters, but exclude alternate dispatch keys and PHP array/alias forms.
    static const QRegularExpression safeKey(QStringLiteral("^[A-Za-z][A-Za-z0-9_-]*$"));
    QSet<QString> seen;
    const bool directDiskPath = url.path(QUrl::FullyDecoded).startsWith(QLatin1String("/disk/downloadFile/"));
    const QUrlQuery query(url);
    for (const auto &[key, value] : query.queryItems(QUrl::FullyDecoded)) {
        const auto folded = key.toLower();
        if ((key != QLatin1String("_esd") && !safeKey.match(key).hasMatch()) || seen.contains(folded)
            || folded == QLatin1String("c") || folded == QLatin1String("controller")
            || folded == QLatin1String("method") || (folded == QLatin1String("mode") && value != QLatin1String("ajax"))
            || (directDiskPath && folded == QLatin1String("action"))) {
            return false;
        }
        seen.insert(folded);
    }
    return true;
}

} // namespace

struct CloudBackend::MessageDownload {
    MessageDownload(QUrl url, const QString &target, const QString &requestedTarget, qint64 size,
                    MessageDownloadMode mode, quint64 accountRevision, QString videoObjectId, QString videoAttachmentId)
        : source(std::move(url)), file(target), requestedTarget(requestedTarget), expectedSize(size),
          mode(mode), accountRevision(accountRevision), videoObjectId(std::move(videoObjectId)),
          videoAttachmentId(std::move(videoAttachmentId))
    {
        lifetime.start();
    }

    qint64 limit() const
    {
        const auto sizeLimit = mode == MessageDownloadMode::Photo ? maxMessagePhotoBytes
            : mode == MessageDownloadMode::Video ? tunables::kChatVideoPlaybackBytes : maxMessageFileBytes;
        return expectedSize >= 0 ? std::min(expectedSize, sizeLimit) : sizeLimit;
    }

    bool validMp4(const QByteArray &mime) const
    {
        const bool videoMime = mime.isEmpty() || mime == "application/octet-stream" ||
            mime == "video/mp4" || mime == "application/mp4" || mime == "video/x-m4v";
        if (!videoMime || header.size() < 16 || header.mid(4, 4) != "ftyp")
            return false;
        const auto typeBoxSize = qFromBigEndian<quint32>(header.constData());
        return typeBoxSize >= 16 && typeBoxSize <= written;
    }

    QUrl source;
    QSaveFile file;
    QString requestedTarget;
    qint64 expectedSize;
    MessageDownloadMode mode;
    quint64 accountRevision;
    QString videoObjectId;
    QString videoAttachmentId;
    QPointer<QNetworkReply> reply;
    QElapsedTimer lifetime;
    QByteArray header;
    qint64 written = 0;
    int redirects = 0;
    bool bodyFailed = false;
    bool cancelled = false;
};

QUrl CloudBackend::registerMessageFileUrl(const QString &value, qint64 size, bool photo,
                                         const QString &videoObjectId, const QString &videoAttachmentId) const
{
    if (value.isEmpty() || value.size() > maxMessageFileUrlChars || size > maxMessageFileBytes)
        return {};
    const auto url = m_session.origin.resolved(QUrl(value, QUrl::StrictMode));
    if (!url.isValid() || url.path().isEmpty() || !url.userInfo().isEmpty() || url.hasFragment()
        || !OriginGuard(m_session.origin).isSameOrigin(url))
        return {};
    if (!m_registeredMessageFileUrls.contains(url) && m_registeredMessageFileUrls.size() >= maxRegisteredMessageFiles)
        m_registeredMessageFileUrls.erase(m_registeredMessageFileUrls.begin());
    m_registeredMessageFileUrls.insert(url, {size, photo, videoObjectId, videoAttachmentId, m_accountScopeRevision});
    return url;
}

void CloudBackend::downloadMessageFile(const QUrl &url, const QString &targetPath)
{
    beginMessageDownload(url, targetPath, MessageDownloadMode::File);
}

void CloudBackend::downloadMessagePhoto(const QUrl &url, const QString &targetPath)
{
    beginMessageDownload(url, targetPath, MessageDownloadMode::Photo);
}

void CloudBackend::cancelMessagePhoto(const QString &targetPath)
{
    cancelMessageMedia(MessageDownloadMode::Photo, targetPath);
}

void CloudBackend::downloadMessageVideo(const QUrl &url, const QString &targetPath)
{
    beginMessageDownload(url, targetPath, MessageDownloadMode::Video);
}

void CloudBackend::cancelMessageVideo(const QString &targetPath)
{
    cancelMessageMedia(MessageDownloadMode::Video, targetPath);
}

CloudBackend::MediaDownloads &CloudBackend::mediaDownloads(MessageDownloadMode mode)
{
    return mode == MessageDownloadMode::Photo ? m_photoDownloads : m_videoDownloads;
}

void CloudBackend::cancelMessageMedia(MessageDownloadMode mode, const QString &targetPath)
{
    const auto download = mediaDownloads(mode).take(QDir::cleanPath(targetPath));
    if (!download)
        return;
    download->cancelled = true;
    download->file.cancelWriting();
    if (download->reply)
        download->reply->abort();
}

void CloudBackend::failMessageDownload(const QString &targetPath, const QString &reason, MessageDownloadMode mode)
{
    if (mode == MessageDownloadMode::Photo)
        emit messagePhotoDownloadFailed(targetPath, reason);
    else if (mode == MessageDownloadMode::Video)
        emit messageVideoDownloadFailed(targetPath, reason);
    else
        emit failed(downloadOperation, reason);
}

void CloudBackend::beginMessageDownload(const QUrl &url, const QString &targetPath, MessageDownloadMode mode)
{
    const auto source = m_registeredMessageFileUrls.constFind(url);
    if (!m_started || m_authExpired || source == m_registeredMessageFileUrls.cend() || !QFileInfo(targetPath).isAbsolute()) {
        failMessageDownload(targetPath, QStringLiteral("Ссылка на файл или место сохранения недоступны"), mode);
        return;
    }
    if (mode == MessageDownloadMode::Photo && (!source->photo || source->size > maxMessagePhotoBytes)) {
        failMessageDownload(targetPath, source->photo ? QStringLiteral("Оригинал изображения превышает 24 МиБ")
                                                      : QStringLiteral("Ссылка не относится к изображению"), mode);
        return;
    }
    if (mode == MessageDownloadMode::Video &&
        (source->accountRevision != m_accountScopeRevision || source->size > tunables::kChatVideoPlaybackBytes ||
         !videoDiskDownloadAllowed(url, source->videoAttachmentId, source->videoObjectId))) {
        failMessageDownload(targetPath, source->size > tunables::kChatVideoPlaybackBytes
            ? QStringLiteral("Видео превышает 64 МиБ. Сохраните файл и откройте его вручную")
            : QStringLiteral("Ссылка не относится к доступному видео"), mode);
        return;
    }
    const auto target = QDir::cleanPath(targetPath);
    if (!safeDownloadTarget(target)) {
        failMessageDownload(targetPath, QStringLiteral("Выберите обычный файл или новое имя для сохранения"), mode);
        return;
    }
    const bool separateMedia = mode != MessageDownloadMode::File;
    const bool busy = separateMedia ? !mediaDownloads(mode).isEmpty() : m_downloadTargets.size() >= 2;
    if (busy || m_downloadTargets.contains(target) || m_photoDownloads.contains(target) || m_videoDownloads.contains(target)) {
        failMessageDownload(targetPath, QStringLiteral("Дождитесь завершения текущих скачиваний"), mode);
        return;
    }
    auto download = std::make_shared<MessageDownload>(url, target, targetPath, source->size, mode,
        m_accountScopeRevision, source->videoObjectId, source->videoAttachmentId);
    download->file.setDirectWriteFallback(false);
    if (!download->file.open(QIODevice::WriteOnly)) {
        failMessageDownload(targetPath, QStringLiteral("Не удалось открыть файл для сохранения"), mode);
        return;
    }
    if (!safeDownloadTarget(target)) {
        download->file.cancelWriting();
        failMessageDownload(targetPath, QStringLiteral("Место сохранения изменилось"), mode);
        return;
    }
    if (separateMedia)
        mediaDownloads(mode).insert(target, download);
    else
        m_downloadTargets.insert(target);
    streamMessageFile(url, download);
}

void CloudBackend::streamMessageFile(const QUrl &url, const std::shared_ptr<MessageDownload> &download)
{
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setTransferTimeout(60000);
    request.setRawHeader("Cookie", m_session.cookie);
    if (download->mode != MessageDownloadMode::Video)
        request.setRawHeader("X-Bitrix-Csrf-Token", m_session.csrf);
    if (download->mode != MessageDownloadMode::File)
        request.setRawHeader("Accept-Encoding", "identity");
    auto *reply = m_http.get(request);
    download->reply = reply;
    m_portalHttp.track(reply);
    reply->setReadBufferSize(downloadBlockBytes);
    if (download->mode == MessageDownloadMode::Video) {
        const int remaining = qMax(0, videoDownloadTimeoutMs - int(download->lifetime.elapsed()));
        QTimer::singleShot(remaining, reply, [reply] { if (!reply->isFinished()) reply->abort(); });
    }
    const auto pump = [reply, download] {
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
            reply->readAll();
            return;
        }
        const auto limit = download->limit();
        while (reply->bytesAvailable() > 0) {
            const auto bytes = reply->read(downloadBlockBytes);
            if (bytes.isEmpty())
                break;
            if (bytes.size() > limit - download->written || download->file.write(bytes) != bytes.size()) {
                download->bodyFailed = true;
                reply->abort();
                break;
            }
            if (download->mode == MessageDownloadMode::Video && download->header.size() < 16)
                download->header.append(bytes.first(qMin(bytes.size(), 16 - download->header.size())));
            download->written += bytes.size();
        }
    };
    if (download->mode != MessageDownloadMode::File) {
        connect(reply, &QNetworkReply::metaDataChanged, this, [reply, download] {
            if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200)
                return;
            const auto encoding = reply->rawHeader("Content-Encoding").trimmed().toLower();
            if (reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > download->limit()
                || (!encoding.isEmpty() && encoding != "identity")) {
                download->bodyFailed = true;
                reply->abort();
            }
        });
    }
    connect(reply, &QIODevice::readyRead, this, pump);
    connect(reply, &QNetworkReply::finished, this, [this, reply, download, pump] {
        m_portalHttp.untrack(reply);
        reply->deleteLater();
        if (download->cancelled)
            return;
        const auto target = download->file.fileName();
        const auto releaseTarget = [this, &download, &target] {
            if (download->mode != MessageDownloadMode::File)
                mediaDownloads(download->mode).remove(target);
            else
                m_downloadTargets.remove(target);
        };
        const auto mode = download->mode;
        if (!m_started || m_authExpired || download->accountRevision != m_accountScopeRevision) {
            releaseTarget();
            download->file.cancelWriting();
            if (m_started && (mode != MessageDownloadMode::File || download->accountRevision != m_accountScopeRevision))
                failMessageDownload(download->requestedTarget, QStringLiteral("Сеанс портала завершён"), mode);
            return;
        }
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        if (status >= 300 && status < 400 && !redirect.isEmpty() && download->redirects < 2) {
            const auto next = reply->url().resolved(redirect);
            if (OriginGuard(m_session.origin).isSameOrigin(next) && next.userInfo().isEmpty() && !next.hasFragment()
                && (mode != MessageDownloadMode::Video ||
                    videoDiskDownloadAllowed(next, download->videoAttachmentId, download->videoObjectId))) {
                ++download->redirects;
                streamMessageFile(next, download);
                return;
            }
        }
        if (!download->bodyFailed)
            pump();
        releaseTarget();
        const auto mime = reply->rawHeader("Content-Type").toLower();
        const bool isHtml = mime.startsWith("text/html")
            && !reply->rawHeader("Content-Disposition").toLower().startsWith("attachment");
        const auto mediaMime = mime.split(';').first().trimmed();
        const bool unexpectedPhoto = mode == MessageDownloadMode::Photo && (download->written == 0
            || (!mediaMime.isEmpty() && mediaMime != "application/octet-stream"
                && (!mediaMime.startsWith("image/") || mediaMime == "image/svg+xml")));
        const bool complete = download->expectedSize < 0 || download->written == download->expectedSize;
        if (status != 200 || reply->error() != QNetworkReply::NoError || download->bodyFailed || !complete || isHtml
            || unexpectedPhoto || (mode == MessageDownloadMode::Video && !download->validMp4(mediaMime))) {
            download->file.cancelWriting();
            failMessageDownload(download->requestedTarget, QStringLiteral("Портал не передал полный файл"), mode);
        } else if (!safeDownloadTarget(target)) {
            download->file.cancelWriting();
            failMessageDownload(download->requestedTarget, QStringLiteral("Место сохранения изменилось"), mode);
        } else if (!download->file.commit()) {
            failMessageDownload(download->requestedTarget, QStringLiteral("Не удалось сохранить файл"), mode);
        } else if (mode == MessageDownloadMode::Video) {
            emit messageVideoDownloaded(download->source, download->requestedTarget);
        } else {
            emit messageFileDownloaded(download->source, mode == MessageDownloadMode::Photo ? download->requestedTarget : target);
        }
    });
}

MessageFile CloudBackend::decodeChatFile(const QString &id, const QJsonObject &file) const
{
    const auto name = plainFileName(field(file, {"name", "NAME", "fileName"}));
    if (name.isEmpty())
        return {};
    bool sizeKnown = false;
    const auto size = field(file, {"size", "SIZE"}).toLongLong(&sizeKnown);
    const auto exactSize = sizeKnown && size >= 0 ? size : -1;
    const auto type = field(file, {"type", "TYPE"}).trimmed().toLower();
    const auto mime = field(file, {"mimeType", "contentType", "MIME_TYPE"})
        .section(QLatin1Char(';'), 0, 0).trimmed().toLower().toLatin1();
    auto extension = QFileInfo(name).suffix();
    if (extension.isEmpty())
        extension = field(file, {"extension", "EXTENSION"}).trimmed();
    extension = extension.toLower();
    const bool rasterExtension = extension == QLatin1String("png") || extension == QLatin1String("jpg")
        || extension == QLatin1String("jpeg") || extension == QLatin1String("webp")
        || extension == QLatin1String("gif");
    const bool declaredImage = type == QLatin1String("image") || file.value(QStringLiteral("isImage")).toBool();
    const bool raster = cloud::isRasterImageMime(mime)
        || ((mime.isEmpty() || mime == "application/octet-stream")
            && (declaredImage || rasterExtension || cloud::isRasterImageMime(type.toLatin1())));
    const bool photo = declaredImage || (raster && (type.isEmpty() || type == QLatin1String("file")
        || cloud::isRasterImageMime(type.toLatin1())));
    const bool mp4Mime = mime == "video/mp4" || mime == "application/mp4" || mime == "video/x-m4v";
    const bool mp4Extension = extension == QLatin1String("mp4") || extension == QLatin1String("m4v");
    const bool video = !photo && (mp4Mime || (mp4Extension && (mime.isEmpty() || mime == "application/octet-stream")
        && (type.isEmpty() || type == QLatin1String("file") || type == QLatin1String("video")
            || type == QLatin1String("video/mp4"))));
    const auto download = field(file, {"urlDownload", "URL_DOWNLOAD"});
    const auto attached = video ? field(file, {"attachedId", "attachedObjectId", "ATTACHED_ID"}) : QString{};
    const auto url = registerMessageFileUrl(download, exactSize, photo, video ? id : QString{}, attached);
    MessageFile entry{id, name, exactSize, url};
    entry.isImage = photo;
    entry.isVideo = video;
    if (photo || video) {
        const auto usesOriginal = [this, &download, &url](const QString &candidate) {
            return !candidate.isEmpty() && (candidate == download
                || (!url.isEmpty() && candidate.size() <= maxMessageFileUrlChars
                    && m_session.origin.resolved(QUrl(candidate, QUrl::StrictMode)) == url));
        };
        const auto preview = field(file, {"urlPreview", "URL_PREVIEW"});
        const auto media = file.value(QStringLiteral("mediaUrl")).toObject().value(QStringLiteral("preview")).toObject();
        const auto mediaPreview = field(media, {"250"});
        const auto show = field(file, {"urlShow", "URL_SHOW"});
        // Bitrix can repeat urlDownload in both preview fields and supply the resize in urlShow.
        const bool previewIsOriginal = usesOriginal(preview);
        const bool mediaIsOriginal = usesOriginal(mediaPreview);
        if (!previewIsOriginal)
            entry.previewUrl = messageImageUrlFromPortal(preview, id, true);
        if (entry.previewUrl.isEmpty() && !mediaIsOriginal)
            entry.previewUrl = messageImageUrlFromPortal(mediaPreview, id, true);
        if (entry.previewUrl.isEmpty() && !usesOriginal(show))
            entry.previewUrl = messageImageUrlFromPortal(show, id, false);
        if (photo && raster && !url.isEmpty() && entry.previewUrl != url && exactSize > 0
            && exactSize <= m_messageImageAssets.maxBytes) {
            const auto registration = entry.previewUrl.isEmpty()
                ? RasterRegistration::Primary : RasterRegistration::Standby;
            QUrl originalPreview;
            if (previewIsOriginal || mediaIsOriginal)
                originalPreview = messageImageUrlFromPortal(download, id, true, registration);
            if (originalPreview.isEmpty())
                originalPreview = taskImageUrlFromPortal(url.toString(QUrl::FullyEncoded), {}, id, true, registration);
            if (entry.previewUrl.isEmpty())
                entry.previewUrl = originalPreview;
            else
                entry.originalPreviewUrl = originalPreview;
        }
        const auto image = file.value(QStringLiteral("image")).toObject();
        const auto width = positiveId(field(image, {"width"}));
        const auto height = positiveId(field(image, {"height"}));
        if (width > 0 && height > 0 && width <= 1048576 && height <= 1048576)
            entry.previewSize = QSize(static_cast<int>(width), static_cast<int>(height));
    }
    return entry;
}

} // namespace bx
