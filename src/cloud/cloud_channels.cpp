#include "cloud/cloud_backend.h"
#include "cloud/cloud_avatars.h"
#include "cloud/reaction_code.h"

#include "core/download_target.h"
#include "core/origin_guard.h"
#include "core/notification_targets.h"
#include "core/tunables.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeDatabase>
#include <QNetworkReply>
#include <QPointer>
#include <QSaveFile>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QUuid>
#include <QtEndian>
#include <algorithm>
#include <utility>

namespace bx {
namespace {

constexpr qsizetype maxQueryLength = 512;
constexpr qsizetype maxAttachmentNames = 100;
constexpr qsizetype chatAssetPageLimit = 50;
constexpr qint64 chatLinkMaxOffset = 10000;
constexpr qint64 uploadChunkSize = 10 * 1024 * 1024;
constexpr qint64 maxMessageFileBytes = 2LL * 1024 * 1024 * 1024;
constexpr qint64 maxMessagePhotoBytes = 24LL * 1024 * 1024;
constexpr int videoDownloadTimeoutMs = 30000;
constexpr qsizetype maxMessageFileUrlChars = 4096;
constexpr qint64 downloadBlockBytes = 64 * 1024;
constexpr qsizetype maxRegisteredMessageFiles = 256;
constexpr qsizetype maxPinnedMessages = 64;
constexpr qsizetype maxMessageReactionKinds = 64;
constexpr qsizetype initialPinBlockSize = 50;
constexpr int onlineLimitSeconds = 15 * 60;
const QString downloadOperation = QStringLiteral("message-file-download");
const QString dataEnvelope = QStringLiteral("data");
const QString resultEnvelope = QStringLiteral("result");

QString text(const QJsonValue &value)
{
    return value.isDouble() ? QString::number(value.toInteger()) : value.toString();
}

QString field(const QJsonObject &object, std::initializer_list<const char *> keys)
{
    for (const char *key : keys) {
        const auto value = text(object.value(QLatin1String(key)));
        if (!value.isEmpty())
            return value;
    }
    return {};
}

qint64 positiveId(const QString &candidate)
{
    const auto id = candidate.toLongLong();
    return id > 0 ? id : 0;
}

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

std::optional<qint64> taskSearchId(const QJsonObject &row, std::initializer_list<const char *> aliases,
                                   bool allowZero = false)
{
    std::optional<qint64> result;
    for (const auto *alias : aliases) {
        const auto value = row.value(QLatin1String(alias));
        if (value.isUndefined())
            continue;
        const auto raw = value.isDouble() ? QString::number(value.toInteger(-1)) : value.toString();
        const auto id = portalTaskIdentifier(raw);
        if (id.isEmpty() && !(allowZero && raw == QLatin1String("0")))
            return std::nullopt;
        const auto number = id.toLongLong();
        if (result && *result != number)
            return std::nullopt;
        result = number;
    }
    return result;
}

QString dialogKey(const QString &candidate)
{
    const bool isGroup = candidate.startsWith(QLatin1String("chat"));
    const auto id = positiveId(isGroup ? candidate.mid(4) : candidate);
    return id > 0 ? (isGroup ? QStringLiteral("chat") : QString()) + QString::number(id) : QString();
}

qint64 groupChatId(const QString &dialog)
{
    return dialog.startsWith(QLatin1String("chat")) ? positiveId(dialog.mid(4)) : 0;
}

QString plainFileName(QString path)
{
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    path = path.section(QLatin1Char('/'), -1);
    static const QRegularExpression controls(QStringLiteral("[\\p{Cc}\\p{Cf}]"));
    path.remove(controls);
    return path.left(255).trimmed();
}

QString authorName(const QJsonObject &user)
{
    if (const auto name = field(user, {"name", "NAME"}); !name.isEmpty())
        return name;
    return (field(user, {"firstName", "FIRST_NAME"}) + QLatin1Char(' ')
            + field(user, {"lastName", "LAST_NAME"})).trimmed();
}

QDateTime date(const QJsonValue &value)
{
    if (value.isDouble() && value.toInteger() > 0)
        return QDateTime::fromSecsSinceEpoch(value.toInteger());
    return QDateTime::fromString(value.toString(), Qt::ISODate);
}

QHash<QString, QJsonObject> responseFiles(const QJsonValue &value)
{
    QHash<QString, QJsonObject> files;
    auto insert = [&files](const QJsonValue &row, const QString &fallbackId = QString()) {
        const auto file = row.toObject();
        auto id = field(file, {"id", "ID"});
        if (id.isEmpty())
            id = fallbackId;
        if (!id.isEmpty())
            files.insert(id, file);
    };
    if (value.isArray()) {
        for (const auto row : value.toArray())
            insert(row);
    } else {
        const auto fileMap = value.toObject();
        for (auto it = fileMap.constBegin(); it != fileMap.constEnd(); ++it)
            insert(it.value(), it.key());
    }
    return files;
}

QString taskFileId(const QString &raw)
{
    static const QRegularExpression syntax(QStringLiteral("^n?[0-9]{1,19}$"));
    if (!syntax.match(raw).hasMatch())
        return {};
    const bool diskObject = raw.startsWith(QLatin1Char('n'));
    const auto number = positiveId(diskObject ? raw.mid(1) : raw);
    return number == 0 ? QString() : (diskObject ? QStringLiteral("n") : QString()) + QString::number(number);
}

QJsonObject messageParams(const QJsonObject &message)
{
    const auto params = message.value(QStringLiteral("params")).toObject();
    return params.isEmpty() ? message.value(QStringLiteral("PARAMS")).toObject() : params;
}

QString replyMessageId(const QJsonObject &message)
{
    const auto direct = positiveId(field(message, {"replyId"}));
    const auto id = direct > 0 ? direct : positiveId(field(messageParams(message), {"replyId", "REPLY_ID"}));
    return id > 0 ? QString::number(id) : QString();
}

bool deletedMessage(const QJsonObject &message)
{
    const auto params = messageParams(message);
    return message.value(QStringLiteral("isDeleted")).toBool()
        || params.value(QStringLiteral("IS_DELETED")) == QLatin1String("Y");
}

QJsonArray messageFileIds(const QJsonObject &message)
{
    auto ids = message.value(QStringLiteral("files")).toArray();
    if (ids.isEmpty())
        ids = message.value(QStringLiteral("fileIds")).toArray();
    if (ids.isEmpty()) {
        const auto params = messageParams(message);
        auto value = params.value(QStringLiteral("FILE_ID"));
        if (value.isUndefined())
            value = params.value(QStringLiteral("fileId"));
        ids = value.isArray() ? value.toArray() : QJsonArray{value};
    }
    return ids;
}

// QNetworkAccessManager reads this bounded range directly from the selected file.
class FileChunk final : public QIODevice {
public:
    FileChunk(std::shared_ptr<QFile> file, qint64 offset, qint64 length)
        : m_file(std::move(file)), m_offset(offset), m_length(length)
    {
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }
    qint64 size() const override { return m_length; }

protected:
    qint64 readData(char *destination, qint64 maxSize) override
    {
        const auto remaining = m_length - pos();
        if (remaining <= 0)
            return 0;
        const auto position = m_offset + pos();
        if (m_file->pos() != position && !m_file->seek(position))
            return -1;
        return m_file->read(destination, std::min(remaining, maxSize));
    }
    qint64 writeData(const char *, qint64) override { return -1; }

private:
    std::shared_ptr<QFile> m_file;
    qint64 m_offset;
    qint64 m_length;
};

void addQueryParameter(QUrl &url, const QString &key, const QString &value)
{
    auto query = url.query(QUrl::FullyEncoded);
    if (!query.isEmpty())
        query += QLatin1Char('&');
    query += QString::fromLatin1(QUrl::toPercentEncoding(key)) + QLatin1Char('=')
        + QString::fromLatin1(QUrl::toPercentEncoding(value));
    url.setQuery(query);
}

} // namespace

struct CloudBackend::Upload {
    std::shared_ptr<QTemporaryDir> temporaryDirectory;
    std::shared_ptr<QFile> file;
    qint64 size = 0;
    QByteArray nameHeader;
    QByteArray mime;
    QString scopeId;
    QString kind;
    QString operation;
    QString requestKey;
    QString caption;
    bool image = false;
    quint64 accountScope = 0;
    qint64 chatId = 0;
    QJsonObject controllerOptions;
    QString token;
    QString serverFileId;

    bool activeFor(quint64 scope) const { return !image || accountScope == scope; }
};

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
    m_requests.insert(reply, {});
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
        m_requests.remove(reply);
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

QVector<Message> CloudBackend::decodeMessages(const QJsonObject &data, bool sortChronologically, const QString &dialog) const
{
    QHash<QString, QString> authors;
    QHash<QString, QString> authorPictures;
    for (const auto *key : {"usersShort", "users"}) {
        for (const auto row : data.value(QLatin1String(key)).toArray()) {
            const auto user = row.toObject();
            const auto userId = field(user, {"id", "ID"});
            if (const auto name = authorName(user); !name.isEmpty())
                authors.insert(userId, name);
            if (const auto picture = field(user, {"avatar", "avatarUrl"}); !picture.isEmpty())
                authorPictures.insert(userId, picture);
        }
    }
    const auto authorOf = [](const QJsonObject &message) { return field(message, {"senderId", "authorId", "author_id", "AUTHOR_ID"}); };
    const auto authorLabel = [&authors](const QString &id) {
        const auto fallback = positiveId(id) == 0 ? QStringLiteral("Системное сообщение")
                                                 : QStringLiteral("Пользователь №%1").arg(id);
        return authors.value(id, fallback);
    };
    const auto rows = data.value(QStringLiteral("messages")).toArray();
    const auto explicitChat = positiveId(field(data, {"chatId", "CHAT_ID"}));
    const auto resolvedChat = badgeChatId(dialog);
    const auto expectedChat = resolvedChat > 0 ? resolvedChat : explicitChat;
    const auto sameChat = [expectedChat](const QJsonObject &message) {
        for (const auto *name : {"chatId", "chat_id", "CHAT_ID"}) {
            const auto value = message.value(QLatin1String(name));
            if (!value.isUndefined() && (expectedChat <= 0 || positiveId(text(value)) != expectedChat))
                return false;
        }
        return true;
    };
    const auto viewerScope = settingsScope();
    const auto viewerDialog = viewerScope + QLatin1Char(':') + dialog;
    const auto chat = data.value(QStringLiteral("chat")).toObject();
    const auto role = field(chat, {"role"});
    const auto dialogId = chat.value(QStringLiteral("dialogId"));
    const auto componentId = chat.value(QStringLiteral("id"));
    const auto group = groupChatId(dialog);
    const bool matchingViewerHistory = sameChat(data) && sameChat(chat)
        && (componentId.isUndefined() || positiveId(text(componentId)) == expectedChat)
        && (dialogId.isUndefined() || text(dialogId) == dialog)
        && (group == 0 || group == expectedChat);
    if (group > 0 && !matchingViewerHistory) {
        m_messageViewerMembership.remove(viewerDialog);
    } else if (!viewerScope.isEmpty() && group > 0
               && positiveId(field(chat, {"id", "chatId"})) == group && !role.isEmpty()) {
        m_messageViewerMembership.insert(viewerDialog, new bool(role == QLatin1String("member")
            || role == QLatin1String("manager") || role == QLatin1String("owner")));
    }
    const auto *groupMember = m_messageViewerMembership.object(viewerDialog);
    const bool canRequestViewers = matchingViewerHistory && !viewerScope.isEmpty() && expectedChat > 0
        && (group > 0 ? (groupMember && *groupMember) : positiveId(dialog) > 0);
    const auto lastViews = chat.value(QStringLiteral("lastMessageViews")).toObject();
    const auto lastViewedMessage = positiveId(field(lastViews, {"messageId"}));
    const auto lastViewerCount = lastViews.value(QStringLiteral("countOfViewers")).toInteger(-1);
    QHash<QString, QJsonObject> replySources;
    QSet<QString> replies;
    for (const auto &row : rows) {
        const auto id = replyMessageId(row.toObject());
        if (!id.isEmpty())
            replies.insert(id);
        if (replies.size() >= tunables::kPageSizeMessages)
            break;
    }
    const auto rememberSource = [&replies, &replySources, &sameChat](const QJsonValue &row, const QString &key = {}) {
        const auto source = row.toObject();
        const auto id = field(source, {"id", "ID"});
        const auto sourceId = positiveId(id.isEmpty() ? key : id);
        const auto canonicalId = QString::number(sourceId);
        if (sourceId > 0 && (key.isEmpty() || positiveId(key) == sourceId)
            && replies.contains(canonicalId) && sameChat(source)) {
            replySources.insert(canonicalId, source);
        }
    };
    const auto additional = data.value(QStringLiteral("additionalMessages"));
    if (additional.isArray()) {
        for (const auto &row : additional.toArray())
            rememberSource(row);
    } else {
        const auto indexed = additional.toObject();
        for (auto it = indexed.constBegin(); it != indexed.constEnd(); ++it)
            rememberSource(it.value(), it.key());
    }
    for (const auto &row : rows)
        rememberSource(row);
    QHash<QString, QVector<MessageReaction>> reactions;
    for (const auto &value : data.value(QStringLiteral("reactions")).toArray()) {
        const auto row = value.toObject();
        const auto id = positiveId(field(row, {"messageId"}));
        if (id <= 0 || reactions.size() >= tunables::kPageSizeMessages)
            continue;
        QSet<QString> own;
        for (const auto &item : row.value(QStringLiteral("ownReactions")).toArray()) {
            const auto code = item.toString();
            if (isReactionCode(code))
                own.insert(code);
        }
        QVector<MessageReaction> decoded;
        const auto counters = row.value(QStringLiteral("reactionCounters")).toObject();
        for (auto it = counters.constBegin(); it != counters.constEnd() && decoded.size() < maxMessageReactionKinds; ++it) {
            const auto count = it.value().toInt();
            if (count > 0 && isReactionCode(it.key()))
                decoded.append({it.key(), count, own.remove(it.key())});
        }
        for (const auto &code : std::as_const(own)) {
            if (decoded.size() >= maxMessageReactionKinds)
                break;
            decoded.append({code, 0, true});
        }
        reactions.insert(QString::number(id), std::move(decoded));
    }
    const auto files = responseFiles(data.value(QStringLiteral("files")));
    const auto filesFor = [this, &files](const QJsonObject &message) {
        QVector<MessageFile> result;
        for (const auto &id : messageFileIds(message)) {
            const auto fileId = text(id);
            const auto file = files.value(fileId);
            auto entry = decodeChatFile(fileId, file);
            if (entry.name.isEmpty())
                continue;
            result.append(std::move(entry));
            if (result.size() >= maxAttachmentNames)
                break;
        }
        return result;
    };
    const auto state = m_dialogMessageStates.constFind(dialog);
    QVector<Message> messages;
    for (const auto row : rows) {
        if (messages.size() >= tunables::kPageSizeMessages)
            break;
        const auto message = row.toObject();
        const auto id = field(message, {"id", "ID"});
        if (id.isEmpty())
            continue;
        const auto authorId = authorOf(message);
        const auto timestamp = message.value(QStringLiteral("date"));
        const bool system = authorId == QLatin1String("0");
        const bool outgoing = !system && !m_userId.isEmpty() && m_userId == authorId;
        const auto numericId = positiveId(id);
        const auto explicitUnread = message.value(QStringLiteral("unread"));
        const bool readByCursor = state != m_dialogMessageStates.cend() && numericId > 0
            && (numericId <= state->confirmedReadThrough || state->exactlyRead.contains(numericId)
                || (numericId <= state->lastReadId
                    && !(explicitUnread.toBool() && state->historyUnread.contains(numericId))));
        const bool unread = !dialog.isEmpty() && !outgoing && !readByCursor && (explicitUnread.isBool()
            ? explicitUnread.toBool()
            : state != m_dialogMessageStates.cend() && !m_userId.isEmpty() && positiveId(authorId) > 0
                && state->lastReadId >= 0 && numericId > state->lastReadId && state->unreadCounter != 0);
        Message entry;
        entry.id = id;
        entry.author = authorLabel(authorId);
        entry.text = field(message, {"text", "TEXT"});
        entry.createdAt = date(timestamp.isUndefined() ? message.value(QStringLiteral("DATE_CREATE")) : timestamp);
        entry.outgoing = outgoing;
        entry.seenByOthers = outgoing && (message.value(QStringLiteral("viewedByOthers")).toBool()
            || (state != m_dialogMessageStates.cend() && state->seenByOthers.contains(numericId)));
        if (canRequestViewers && outgoing && numericId > 0 && sameChat(message)) {
            const auto key = viewerDialog + QLatin1Char(':') + QString::number(numericId);
            if (deletedMessage(message)) {
                m_messageViewerCounts.remove(key);
            } else if (lastViewedMessage == numericId || !m_messageViewerCounts.contains(key)) {
                m_messageViewerCounts.insert(key,
                    new qint64(lastViewedMessage == numericId ? lastViewerCount : -1));
            }
        }
        entry.system = system;
        entry.reactions = reactions.value(QString::number(numericId));
        entry.files = filesFor(message);
        for (const auto &file : entry.files)
            entry.attachments.append(file.name);
        entry.unread = unread;
        entry.authorAvatarUrl = avatarUrlFromPortal(authorPictures.value(authorId));
        const auto forward = message.value(QStringLiteral("forward")).toObject();
        const auto originalAuthor = field(forward, {"userId"});
        const auto originalMessage = field(forward, {"id"});
        entry.forwarded = (!originalMessage.isEmpty() && originalMessage != QLatin1String("0")) || positiveId(originalAuthor) > 0;
        if (entry.forwarded)
            entry.forwardedAuthor = authors.value(originalAuthor);
        entry.replyId = replyMessageId(message);
        if (!entry.replyId.isEmpty()) {
            const auto reply = replySources.value(entry.replyId);
            const auto previewKey = replyPreviewKey(dialog, entry.replyId);
            if (!reply.isEmpty() && deletedMessage(reply)) {
                m_replyPreviews.remove(previewKey);
            } else if (!reply.isEmpty()) {
                entry.replyAuthor = authorLabel(authorOf(reply)).left(512);
                entry.replyText = field(reply, {"text", "TEXT"}).left(2000);
                entry.replyFiles = filesFor(reply);
                if (!dialog.isEmpty()) {
                    m_replyPreviews.insert(previewKey,
                        new ReplyPreview{entry.replyAuthor, entry.replyText, entry.replyFiles},
                        1 + entry.replyFiles.size());
                }
            } else if (!dialog.isEmpty()) {
                if (const auto *cached = m_replyPreviews.object(previewKey)) {
                    entry.replyAuthor = cached->author;
                    entry.replyText = cached->text;
                    entry.replyFiles = cached->files;
                }
            }
        }
        if (!dialog.isEmpty() && numericId > 0 && sameChat(message)) {
            const auto key = replyPreviewKey(dialog, QString::number(numericId));
            if (deletedMessage(message))
                m_replyPreviews.remove(key);
            else
                m_replyPreviews.insert(key, new ReplyPreview{entry.author.left(512), entry.text.left(2000), entry.files},
                                       1 + entry.files.size());
        }
        messages.append(std::move(entry));
    }
    if (sortChronologically) {
        std::stable_sort(messages.begin(), messages.end(), [](const Message &left, const Message &right) {
            if (left.createdAt != right.createdAt)
                return left.createdAt < right.createdAt;
            return left.id.toLongLong() < right.id.toLongLong();
        });
    }
    return messages;
}

void CloudBackend::rememberChatContact(const QString &dialog, const QJsonObject &user, bool publish)
{
    if (positiveId(dialog) == 0 || user.isEmpty())
        return;
    const auto userId = field(user, {"id", "ID"});
    if (!userId.isEmpty() && userId != dialog)
        return;
    auto activity = user.value(QStringLiteral("lastActivityDate"));
    if (activity.isUndefined())
        activity = user.value(QStringLiteral("last_activity_date"));
    const auto lastActivity = date(activity);
    const auto position = user.value(QStringLiteral("workPosition"));
    if (!lastActivity.isValid() && !position.isString())
        return;
    if (!m_chatContacts.contains(dialog) && m_chatContacts.size() >= 2 * tunables::kPageSizeChats)
        m_chatContacts.erase(m_chatContacts.begin());
    auto &contact = m_chatContacts[dialog];
    const auto previous = contact;
    if (lastActivity.isValid() && lastActivity <= QDateTime::currentDateTimeUtc().addSecs(300)) {
        const auto until = lastActivity.addSecs(onlineLimitSeconds);
        if (!contact.onlineUntil.isValid() || until > contact.onlineUntil)
            contact.onlineUntil = until;
    }
    if (position.isString())
        contact.position = position.toString().trimmed().left(256);
    if (!publish || (contact.position == previous.position && contact.onlineUntil == previous.onlineUntil))
        return;
    for (auto &chat : m_recentChatItems) {
        if (chat.id == dialog)
            applyChatContact(chat);
    }
    const auto current = contact;
    emit chatContactChanged(dialog, current.onlineUntil, current.position);
}

void CloudBackend::applyChatContact(Chat &chat) const
{
    if (chat.taskChat || positiveId(chat.id) == 0)
        return;
    const auto contact = m_chatContacts.constFind(chat.id);
    if (contact != m_chatContacts.cend()) {
        chat.position = contact->position;
        chat.onlineUntil = contact->onlineUntil;
    }
}

void CloudBackend::rememberDialogContact(const QString &dialog, const QJsonObject &payload)
{
    if (positiveId(dialog) == 0)
        return;
    for (const auto *fieldName : {"usersShort", "users", "additionalUsers"}) {
        const auto users = payload.value(QLatin1String(fieldName));
        const auto update = [this, &dialog](const QJsonObject &user) {
            if (field(user, {"id", "ID"}) == dialog)
                rememberChatContact(dialog, user, true);
        };
        if (users.isObject())
            update(users.toObject());
        else {
            for (const auto user : users.toArray())
                update(user.toObject());
        }
    }
}

void CloudBackend::handleOnlinePull(const QJsonObject &params)
{
    const auto users = params.value(QStringLiteral("users"));
    const auto update = [this](const QJsonObject &user) {
        const auto id = field(user, {"id"});
        if (positiveId(id) == 0 || (!m_chatContacts.contains(id) && id != m_openDialog
            && std::none_of(m_recentChatItems.cbegin(), m_recentChatItems.cend(), [&id](const Chat &chat) { return chat.id == id; })))
            return;
        rememberChatContact(id, user, true);
    };
    if (users.isArray()) {
        for (const auto user : users.toArray())
            update(user.toObject());
    } else {
        for (const auto user : users.toObject())
            update(user.toObject());
    }
}

QVector<Message> CloudBackend::decodePinnedMessages(const QJsonObject &payload) const
{
    QStringList keys;
    QSet<QString> seen;
    for (const auto record : payload.value(QStringLiteral("pins")).toArray()) {
        const auto key = field(record.toObject(), {"messageId"});
        if (positiveId(key) <= 0 || seen.contains(key))
            continue;
        keys.append(key);
        seen.insert(key);
        if (keys.size() == maxPinnedMessages)
            break;
    }
    QHash<QString, QJsonObject> rows;
    for (const auto *fieldName : {"additionalMessages", "messages"}) {
        for (const auto record : payload.value(QLatin1String(fieldName)).toArray()) {
            const auto row = record.toObject();
            const auto key = field(row, {"id", "ID"});
            if (seen.contains(key))
                rows.insert(key, row);
        }
    }
    QJsonArray messages;
    for (const auto &key : keys) {
        if (rows.contains(key))
            messages.append(rows.value(key));
        else
            messages.append(QJsonObject{{QStringLiteral("id"), key},
                                        {QStringLiteral("text"), QStringLiteral("Сообщение недоступно")}});
    }
    auto data = payload;
    data.insert(QStringLiteral("messages"), messages);
    return decodeMessages(data, false, m_pinnedDialog);
}

void CloudBackend::publishPinnedMessages()
{
    auto rows = m_pinnedMessages;
    for (auto it = m_pinnedChanges.cbegin(); it != m_pinnedChanges.cend(); ++it) {
        rows.erase(std::remove_if(rows.begin(), rows.end(), [&it](const Message &row) {
            return row.id == it.key();
        }), rows.end());
        if (it.value())
            rows.append(*it.value());
    }
    std::sort(rows.begin(), rows.end(), [](const Message &a, const Message &b) {
        return positiveId(a.id) > positiveId(b.id);
    });
    if (rows.size() > maxPinnedMessages)
        rows.resize(maxPinnedMessages);
    const auto dialog = m_pinnedDialog;
    emit pinnedMessagesLoaded(dialog, rows);
}

void CloudBackend::acceptPinnedSnapshot(const QString &dialog, const QJsonObject &payload)
{
    if (dialog.isEmpty() || dialog != m_pinnedDialog)
        return;
    const auto generation = m_pinnedGeneration;
    m_havePinnedSnapshot = true;
    m_pinnedMessages = decodePinnedMessages(payload);
    publishPinnedMessages();
    if (m_pinnedGeneration != generation)
        return;
    const auto pins = payload.value(QStringLiteral("pins")).toArray();
    if (pins.size() < initialPinBlockSize || m_pinnedMessages.isEmpty())
        return;
    const auto last = field(pins.last().toObject(), {"messageId"});
    if (positiveId(last) == 0)
        return;
    const auto operation = QStringLiteral("pins:") + dialog;
    ajax(QStringLiteral("im.v2.Chat.Pin.tail"),
         {{QStringLiteral("dialogId"), dialog}, {QStringLiteral("filter[lastId]"), last},
          {QStringLiteral("order[id]"), QStringLiteral("DESC")}, {QStringLiteral("limit"), QStringLiteral("25")}},
         operation, [this, generation, dialog, last](QJsonValue data) {
        if (m_pinnedGeneration != generation || m_pinnedDialog != dialog)
            return;
        auto next = decodePinnedMessages(data.toObject());
        for (auto &row : next) {
            if (m_pinnedMessages.size() == maxPinnedMessages)
                break;
            if (positiveId(row.id) < positiveId(last)
                && std::none_of(m_pinnedMessages.cbegin(), m_pinnedMessages.cend(), [&row](const Message &old) { return old.id == row.id; }))
                m_pinnedMessages.append(std::move(row));
        }
        publishPinnedMessages();
    }, operation + QLatin1Char(':') + QString::number(generation) + QStringLiteral(":tail"));
}

void CloudBackend::requestPinnedMessages(const QString &dialogId)
{
    const auto dialog = dialogKey(dialogId);
    if (dialog == m_pinnedDialog) {
        if (m_havePinnedSnapshot)
            publishPinnedMessages();
        return;
    }
    m_pinnedDialog = dialog;
    m_havePinnedSnapshot = false;
    m_pinnedMessages.clear();
    m_pinnedChanges.clear();
    const auto generation = ++m_pinnedGeneration;
    if (dialog.isEmpty())
        return;
    QTimer::singleShot(0, this, [this, dialog, generation] {
        if (!m_started || m_pinnedGeneration != generation || m_havePinnedSnapshot)
            return;
        const auto historyOperation = QStringLiteral("chat:") + dialog;
        const auto taskOperation = QStringLiteral("task-messages:") + m_openTask;
        const bool pendingHistory = m_inflightOperations.contains(historyOperation)
            || (m_taskDialogs.value(m_openTask) == dialog && m_inflightOperations.contains(taskOperation))
            || std::any_of(m_historiesWaitingForIdentity.cbegin(), m_historiesWaitingForIdentity.cend(),
                           [&dialog](const auto &history) { return history.dialog == dialog; });
        if (pendingHistory)
            return;
        const auto operation = QStringLiteral("pins:") + dialog;
        ajax(QStringLiteral("im.v2.Chat.load"),
             {{QStringLiteral("dialogId"), dialog}, {QStringLiteral("messageLimit"), QStringLiteral("1")}},
             operation, [this, dialog, generation](QJsonValue data) {
            if (m_pinnedGeneration != generation)
                return;
            const auto payload = data.toObject();
            rememberDialog(dialog, payload);
            rememberDialogContact(dialog, payload);
            acceptPinnedSnapshot(dialog, payload);
        }, operation + QLatin1Char(':') + QString::number(generation));
    });
}

void CloudBackend::handlePinnedPull(const QString &command, const QJsonObject &params)
{
    if (m_pinnedDialog.isEmpty())
        return;
    const bool add = command == QLatin1String("pinAdd");
    const auto record = add ? params.value(QStringLiteral("pin")).toObject() : params;
    const auto id = field(record, {"messageId"});
    const auto chat = positiveId(field(record, {"chatId"}));
    if (positiveId(id) == 0 || chat <= 0
        || (chat != m_dialogChatIds.value(m_pinnedDialog) && chat != groupChatId(m_pinnedDialog)))
        return;
    std::optional<Message> refreshed;
    if (add) {
        auto payload = params;
        payload.insert(QStringLiteral("pins"), QJsonArray{record});
        auto rows = decodePinnedMessages(payload);
        if (!rows.isEmpty())
            refreshed = std::move(rows.first());
    }
    if (!m_pinnedChanges.contains(id) && m_pinnedChanges.size() >= 2 * maxPinnedMessages)
        m_pinnedChanges.erase(m_pinnedChanges.begin());
    m_pinnedChanges.insert(id, std::move(refreshed));
    publishPinnedMessages();
}

void CloudBackend::rememberDialog(const QString &dialog, const QJsonObject &object, bool rememberCounter)
{
    auto chat = object.value(QStringLiteral("chat")).toObject();
    if (chat.isEmpty()) {
        for (const auto &item : object.value(QStringLiteral("chats")).toArray()) {
            const auto candidate = item.toObject();
            if (field(candidate, {"dialogId", "DIALOG_ID"}) == dialog) {
                chat = candidate;
                break;
            }
        }
    }
    if (!chat.isEmpty()) {
        auto &state = dialogMessageState(dialog);
        const auto readId = chat.value(QStringLiteral("lastId")).toInteger(-1);
        if (readId >= state.lastReadId) {
            state.lastReadId = readId;
            const auto unreadIdValue = chat.value(QStringLiteral("unreadId"));
            bool hasUnreadId = true;
            const auto unreadId = unreadIdValue.isString() ? unreadIdValue.toString().toLongLong(&hasUnreadId)
                : unreadIdValue.toInteger(-1);
            if (rememberCounter && hasUnreadId && unreadId >= 0)
                state.unreadCounter = unreadId == 0 ? std::optional<int>(0) : std::nullopt;
        }
        const auto muted = chat.value(QStringLiteral("isMuted"));
        const auto muteList = chat.value(QStringLiteral("muteList"));
        if (!state.muteFromPull && muted.isBool()) {
            state.muted = muted.toBool();
            state.muteList = {};
        } else if (!state.muteFromPull && (muteList.isArray() || muteList.isObject())) {
            state.muteList = muteList;
            state.muted.reset();
        }
    }
    auto id = positiveId(field(object, {"chatId", "CHAT_ID"}));
    if (id == 0)
        id = positiveId(field(chat, {"id", "chatId"}));
    if (id == 0)
        return;
    if (!m_dialogChatIds.contains(dialog) && m_dialogChatIds.size() >= tunables::kPageSizeChats)
        m_dialogChatIds.erase(m_dialogChatIds.begin());
    m_dialogChatIds.insert(dialog, id);
}

void CloudBackend::resolveDialog(const QString &scopeId, bool task, const QString &operation,
                                 const QString &requestKey, ResolvedDialog onDialog)
{
    if (task) {
        const auto known = m_taskDialogs.value(scopeId);
        if (const auto id = groupChatId(known); id > 0) {
            onDialog(known, id);
            return;
        }
        loadTask(scopeId, operation, [this, scopeId, operation, onDialog](const QJsonObject &) {
            const auto dialog = m_taskDialogs.value(scopeId);
            const auto chatId = groupChatId(dialog);
            if (chatId == 0) {
                emit failed(operation, QStringLiteral("У задачи нет чата"));
                return;
            }
            onDialog(dialog, chatId);
        }, requestKey);
        return;
    }

    const auto dialog = dialogKey(scopeId);
    if (dialog.isEmpty()) {
        emit failed(operation, QStringLiteral("Некорректный номер диалога"));
        return;
    }
    if (auto chatId = groupChatId(dialog); chatId > 0 || (chatId = m_dialogChatIds.value(dialog)) > 0) {
        onDialog(dialog, chatId);
        return;
    }
    ajax(QStringLiteral("im.v2.Chat.load"),
         {{QStringLiteral("dialogId"), dialog}, {QStringLiteral("messageLimit"), QStringLiteral("1")}},
         operation, [this, operation, dialog, onDialog, scope = m_accountScopeRevision](QJsonValue data) {
        if (scope != m_accountScopeRevision)
            return;
        rememberDialog(dialog, data.toObject());
        const auto chatId = m_dialogChatIds.value(dialog);
        if (chatId == 0) {
            failShape(operation);
            return;
        }
        onDialog(dialog, chatId);
    }, requestKey);
}

void CloudBackend::searchTaskChats(const QString &query)
{
    const auto operation = QStringLiteral("search-task-chats:") + query;
    const auto needle = query.trimmed();
    if (needle.isEmpty()) {
        emit taskChatsFound(query, {});
        return;
    }
    if (needle.size() > maxQueryLength) {
        emit failed(operation, QStringLiteral("Слишком длинный запрос поиска"));
        return;
    }
    const auto number = needle.startsWith(QLatin1Char('#')) || needle.startsWith(QChar(u'№'))
        ? needle.mid(1).trimmed() : needle;
    const auto id = portalTaskIdentifier(number);
    Fields fields{{id.isEmpty() ? QStringLiteral("filter[%TITLE]") : QStringLiteral("filter[ID]"),
                   id.isEmpty() ? needle : id}, {QStringLiteral("start"), QStringLiteral("0")}};
    for (const auto *name : {"ID", "TITLE", "CHAT_ID", "GROUP_ID"})
        fields.append({QStringLiteral("select[]"), QLatin1String(name)});
    const auto scope = m_accountScopeRevision;
    const auto url = OriginGuard(m_session.origin).resolve(QStringLiteral("/rest/tasks.task.list.json"));
    webRequest(url, std::move(fields), operation, resultEnvelope,
               [this, scope, query, needle, id, operation](QJsonValue data) {
        if (scope != m_accountScopeRevision || m_authExpired)
            return;
        const auto rows = data.toObject().value(QStringLiteral("tasks"));
        if (!rows.isArray())
            return failShape(operation);
        QSet<qint64> seenTasks, seenChats;
        QVector<Chat> matches;
        const auto page = rows.toArray();
        const auto limit = qMin(qsizetype(tunables::kPageSizeChats), page.size());
        for (qsizetype index = 0; index < limit; ++index) {
            const auto row = page.at(index).toObject();
            const auto taskId = taskSearchId(row, {"id", "ID"});
            const auto chatId = taskSearchId(row, {"chatId", "CHAT_ID", "chat_id"});
            const auto group = taskSearchId(row, {"groupId", "GROUP_ID"}, true);
            if (!taskId || !chatId || !group || seenTasks.contains(*taskId) || seenChats.contains(*chatId))
                continue;
            const auto groupRow = row.value(QStringLiteral("group")).toObject();
            if (!groupRow.isEmpty() && taskSearchId(groupRow, {"id", "ID"}, true) != group)
                continue;
            const auto title = row.value(QStringLiteral("title"));
            const auto titleAlias = row.value(QStringLiteral("TITLE"));
            if (!title.isUndefined() && (!title.isString() || (!titleAlias.isUndefined() && title != titleAlias)))
                continue;
            const auto name = (title.isUndefined() ? titleAlias : title).toString();
            const auto task = QString::number(*taskId);
            if (name.trimmed().isEmpty() || (id.isEmpty() ? !name.contains(needle, Qt::CaseInsensitive) : task != id))
                continue;
            const auto dialog = QStringLiteral("chat") + QString::number(*chatId);
            const auto recent = std::find_if(m_recentTaskChatItems.cbegin(), m_recentTaskChatItems.cend(),
                [&dialog, &task](const Chat &chat) { return chat.id == dialog && chat.taskId == task; });
            Chat chat = recent == m_recentTaskChatItems.cend() ? Chat{} : *recent;
            chat.id = dialog;
            chat.taskId = task;
            chat.serverChatId = *chatId;
            chat.taskChat = true;
            chat.kind = QStringLiteral("tasksTask");
            chat.title = name.simplified().left(500);
            decorateProjectTaskChat(chat);
            seenTasks.insert(*taskId);
            seenChats.insert(*chatId);
            matches.append(std::move(chat));
        }
        emit taskChatsFound(query, matches);
    }, operation + QLatin1Char(':') + QString::number(scope));
}

void CloudBackend::searchChats(const QString &query)
{
    const auto operation = QStringLiteral("search-chats:") + query;
    const auto trimmed = query.trimmed();
    if (trimmed.isEmpty()) {
        emit chatsFound(query, {});
        return;
    }
    if (trimmed.size() > maxQueryLength) {
        emit failed(operation, QStringLiteral("Слишком длинный запрос поиска"));
        return;
    }
    static const QRegularExpression separator(QStringLiteral("[\\s()\\[\\]{}<>#\\-\"']+"));
    QJsonArray words;
    for (const auto &word : trimmed.split(separator, Qt::SkipEmptyParts))
        words.append(word);
    const QJsonObject entity{{QStringLiteral("id"), QStringLiteral("im-recent-v2")},
                             {QStringLiteral("dynamicLoad"), true}, {QStringLiteral("dynamicSearch"), true},
                             {QStringLiteral("options"), QJsonObject{}}};
    const QJsonObject dialog{{QStringLiteral("entities"), QJsonArray{entity}},
                             {QStringLiteral("preselectedItems"), QJsonArray{}},
                             {QStringLiteral("clearUnavailableItems"), false},
                             {QStringLiteral("context"), QStringLiteral("IM_CHAT_SEARCH")},
                             {QStringLiteral("id"), QStringLiteral("im-chat-search")}};
    const QJsonObject search{{QStringLiteral("queryWords"), words}, {QStringLiteral("query"), trimmed}};
    ajaxJson(QStringLiteral("ui.entityselector.doSearch"),
             {{QStringLiteral("dialog"), dialog}, {QStringLiteral("searchQuery"), search}},
             operation, [this, query, operation](QJsonValue data) {
        const auto items = data.toObject().value(QStringLiteral("dialog")).toObject().value(QStringLiteral("items"));
        if (!items.isArray())
            return failShape(operation);
        QVector<Chat> matches;
        QSet<QString> seen;
        for (const auto row : items.toArray()) {
            const auto item = row.toObject();
            const auto id = dialogKey(field(item, {"id"}));
            if (id.isEmpty() || seen.contains(id))
                continue;
            const auto custom = item.value(QStringLiteral("customData")).toObject();
            const auto chat = custom.value(QStringLiteral("chat")).toObject();
            auto result = chatFromMetadata(id, item, chat, custom.value(QStringLiteral("user")).toObject());
            if (result.title.isEmpty() || result.taskChat)
                continue;
            seen.insert(id);
            rememberDialog(id, QJsonObject{{QStringLiteral("chat"), chat}});
            result.updated = date(custom.value(QStringLiteral("dateMessage")));
            matches.append(std::move(result));
            if (matches.size() >= tunables::kPageSizeChats)
                break;
        }
        emit chatsFound(query, matches);
    });
}

void CloudBackend::requestChatAssets(const QString &dialog, ChatAssetKind kind, const QString &cursor)
{
    const auto operation = QStringLiteral("chat-assets:%1:%2:%3").arg(dialog).arg(int(kind)).arg(cursor);
    const bool links = kind == ChatAssetKind::Links;
    if (kind != ChatAssetKind::Media && kind != ChatAssetKind::Files && !links)
        return;
    const auto boundary = positiveId(cursor);
    if ((!cursor.isEmpty() && boundary <= 0) || (links && boundary > chatLinkMaxOffset)) {
        emit failed(operation, QStringLiteral("Некорректная граница списка вложений"));
        return;
    }
    resolveDialog(dialog, false, operation, operation,
                  [this, dialog, kind, cursor, operation, boundary](const QString &, qint64 chatId) {
        loadChatAssets(chatId, kind, boundary, operation, [this, dialog, kind, cursor](QVector<ChatAsset> items, QString next) {
            emit chatAssetsLoaded(dialog, kind, cursor, items, next);
        });
    });
}

void CloudBackend::requestChatLinkPreviews(const QString &scopeId, bool task, ChatLinkPreviewHandler complete)
{
    const auto dialog = task ? m_taskDialogs.value(scopeId) : dialogKey(scopeId);
    const auto chatId = badgeChatId(dialog);
    if (chatId <= 0 || m_authExpired) {
        complete({});
        return;
    }
    const auto operation = QStringLiteral("link-preview:chat-assets:") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    loadChatAssets(chatId, ChatAssetKind::Links, 0, operation,
                   [complete = std::move(complete)](QVector<ChatAsset> items, QString) {
        complete(std::move(items));
    });
}

void CloudBackend::loadChatAssets(qint64 chatId, ChatAssetKind kind, qint64 boundary, const QString &operation,
                                  std::function<void(QVector<ChatAsset>, QString)> complete)
{
    const bool links = kind == ChatAssetKind::Links;
    const bool preview = operation.startsWith(QLatin1String("link-preview:"));
    const auto scope = preview ? settingsScope() : QString{};
    Fields fields{{QStringLiteral("CHAT_ID"), QString::number(chatId)}, {QStringLiteral("LIMIT"), QString::number(chatAssetPageLimit)}};
    if (!links)
        fields.append({QStringLiteral("GROUP"), kind == ChatAssetKind::Media ? QStringLiteral("media") : QStringLiteral("file")});
    if (boundary > 0)
        fields.append({links ? QStringLiteral("OFFSET") : QStringLiteral("LAST_ID"), QString::number(boundary)});
    const auto url = OriginGuard(m_session.origin).resolve(links ? QStringLiteral("/rest/im.chat.url.get.json")
                                                                 : QStringLiteral("/rest/im.chat.file.get.json"));
    webRequest(url, fields, operation, resultEnvelope,
               [this, operation, links, boundary, chatId, preview, scope,
                complete = std::move(complete)](const QJsonValue &value) {
        if (preview && (m_authExpired || settingsScope() != scope)) {
            complete({}, {});
            return;
        }
        const auto result = value.toObject();
        const auto rawRows = result.value(QStringLiteral("list"));
        if (!rawRows.isArray() && preview) {
            complete({}, {});
            return;
        }
        if (!rawRows.isArray())
            return failShape(operation);
        const auto rows = rawRows.toArray();
        const auto files = responseFiles(result.value(QStringLiteral("files")));
        const auto count = std::min(chatAssetPageLimit, rows.size());
        QVector<ChatAsset> decoded;
        QSet<QString> seen;
        qint64 last = 0;
        for (qsizetype i = 0; i < count; ++i) {
            const auto row = rows.at(i).toObject();
            const auto numeric = positiveId(field(row, {"id"}));
            const auto rawChat = row.value(QStringLiteral("chatId"));
            if (numeric <= 0 || (!rawChat.isUndefined() && positiveId(text(rawChat)) != chatId))
                continue;
            last = last > 0 ? std::min(last, numeric) : numeric;
            ChatAsset asset;
            asset.id = QString::number(numeric);
            const auto messageId = positiveId(field(row, {"messageId"}));
            if (messageId > 0)
                asset.messageId = QString::number(messageId);
            if (seen.contains(asset.id))
                continue;
            seen.insert(asset.id);
            if (links) {
                const auto link = row.value(QStringLiteral("url")).toObject();
                const auto raw = field(link, {"source"});
                if (raw.isEmpty() || raw.size() > 4096)
                    continue;
                const QUrl parsed(raw, QUrl::StrictMode);
                if (!parsed.isValid() || parsed.host().isEmpty() || !parsed.userInfo().isEmpty()
                    || (parsed.scheme() != QLatin1String("https") && parsed.scheme() != QLatin1String("http")))
                    continue;
                asset.link = parsed;
                const auto rich = link.value(QStringLiteral("richData")).toObject();
                asset.name = notificationDisplayText(field(rich, {"name"})).left(250);
                if (preview && messageId > 0 && !OriginGuard(m_session.origin).isSameOrigin(parsed)) {
                    asset.description = notificationDisplayText(field(rich, {"description"})).simplified().left(350);
                    asset.thumbnailUrl = messageImageUrlFromPortal(field(rich, {"previewUrl"}), {}, false);
                }
                if (asset.name.isEmpty())
                    asset.name = parsed.toDisplayString();
            } else {
                const auto fileId = field(row, {"fileId"});
                asset.file = decodeChatFile(fileId, files.value(fileId));
                if (asset.file.name.isEmpty())
                    continue;
                asset.name = asset.file.name;
            }
            decoded.append(std::move(asset));
        }
        QString next;
        if (count == chatAssetPageLimit) {
            if (links && boundary + count <= chatLinkMaxOffset)
                next = QString::number(boundary + count);
            else if (!links && last > 0 && (boundary == 0 || last < boundary))
                next = QString::number(last);
        }
        complete(std::move(decoded), std::move(next));
    });
}

void CloudBackend::requestMessageViewers(const QString &scopeId, const QString &messageId, bool task,
                                        MessageViewersHandler complete)
{
    const auto scope = settingsScope();
    const auto dialog = task ? m_taskDialogs.value(scopeId) : dialogKey(scopeId);
    const auto id = positiveId(messageId);
    const auto chatId = badgeChatId(dialog);
    const auto group = groupChatId(dialog);
    const auto viewerDialog = scope + QLatin1Char(':') + dialog;
    const auto key = viewerDialog + QLatin1Char(':') + QString::number(id);
    const auto *knownCount = m_messageViewerCounts.object(key);
    const auto *member = m_messageViewerMembership.object(viewerDialog);
    if (m_authExpired || scope.isEmpty() || id <= 0 || chatId <= 0 || !knownCount
        || (group > 0 && (group != chatId || !member || !*member))
        || (task ? m_openTask != scopeId : m_openDialog != dialog)) {
        complete(std::nullopt);
        return;
    }
    const auto operation = QStringLiteral("message-viewers:") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto url = OriginGuard(m_session.origin).resolve(QStringLiteral("/rest/im.v2.Chat.Message.tailViewers.json"));
    webRequest(url, {{QStringLiteral("id"), QString::number(id)},
                     {QStringLiteral("limit"), QString::number(tunables::kPageSizeChats)}},
               operation, resultEnvelope,
               [this, scope, dialog, task, scopeId, id, chatId, count = *knownCount, group, viewerDialog, key,
                complete = std::move(complete)](QJsonValue response) {
        const auto result = response.toObject();
        const auto views = result.value(QStringLiteral("views"));
        const auto *memberNow = m_messageViewerMembership.object(viewerDialog);
        if (m_authExpired || settingsScope() != scope || !views.isArray()
            || !m_messageViewerCounts.contains(key) || badgeChatId(dialog) != chatId
            || (group > 0 && (!memberNow || !*memberNow))
            || (task ? m_openTask != scopeId : m_openDialog != dialog)) {
            complete(std::nullopt);
            return;
        }
        QHash<qint64, QJsonObject> users;
        const auto profiles = result.value(QStringLiteral("users")).toArray();
        for (qsizetype index = 0; index < qMin<qsizetype>(profiles.size(), tunables::kPageSizeChats); ++index) {
            const auto user = profiles[index].toObject();
            const auto uid = positiveId(field(user, {"id", "ID"}));
            if (uid > 0)
                users.insert(uid, user);
        }
        MessageViewers decoded;
        const auto array = views.toArray();
        const auto length = qMin<qsizetype>(array.size(), tunables::kPageSizeChats);
        QSet<qint64> seen;
        for (qsizetype index = 0; index < length; ++index) {
            const auto view = array[index].toObject();
            const auto explicitChat = view.value(QStringLiteral("chatId"));
            if (positiveId(field(view, {"messageId"})) != id
                || (!explicitChat.isUndefined() && positiveId(text(explicitChat)) != chatId)) {
                complete(std::nullopt);
                return;
            }
            const auto uid = positiveId(field(view, {"userId"}));
            if (uid <= 0 || QString::number(uid) == m_userId || seen.contains(uid))
                continue;
            seen.insert(uid);
            const auto user = users.value(uid);
            auto name = authorName(user);
            if (name.isEmpty())
                name = QStringLiteral("Пользователь №%1").arg(uid);
            decoded.people.append({QString::number(uid), name.left(160), {},
                                   avatarUrlFromPortal(field(user, {"avatar", "avatarUrl"}))});
        }
        const auto reported = result.value(QStringLiteral("viewedCount")).toInteger(count);
        const auto more = result.value(QStringLiteral("hasNextPage"));
        decoded.hasMore = reported > decoded.people.size()
            || (more.isBool() ? more.toBool() : array.size() >= tunables::kPageSizeChats);
        decoded.count = reported >= decoded.people.size() ? reported : decoded.hasMore ? -1 : decoded.people.size();
        complete(std::move(decoded));
    });
}

void CloudBackend::requestOlderChatMessages(const QString &scopeId, const QString &beforeId, bool task)
{
    const auto operation = QStringLiteral("older-chat:%1:%2:%3").arg(
        task ? QStringLiteral("task") : QStringLiteral("chat"), scopeId, beforeId);
    const auto before = positiveId(beforeId);
    if (before == 0)
        return failShape(operation);
    const auto scope = m_accountScopeRevision;
    resolveDialog(scopeId, task, operation, operation,
                  [this, scopeId, beforeId, before, task, operation, scope](const QString &dialog, qint64 chatId) {
        if (scope != m_accountScopeRevision)
            return;
        constexpr int limit = 25;
        const auto generation = m_dialogMessageStates.value(dialog).historyGeneration;
        const auto revision = m_chatBadgeRevision;
        ajax(QStringLiteral("im.v2.Chat.Message.tail"),
             {{QStringLiteral("chatId"), QString::number(chatId)}, {QStringLiteral("filter[lastId]"), QString::number(before)},
              {QStringLiteral("order[id]"), QStringLiteral("DESC")}, {QStringLiteral("limit"), QString::number(limit)}},
             operation, [this, scopeId, beforeId, before, task, operation, dialog, chatId, generation, scope, revision](QJsonValue data) {
            if (scope != m_accountScopeRevision || m_dialogMessageStates.value(dialog).historyGeneration != generation)
                return;
            auto page = data.toObject();
            const auto rawRows = page.value(QStringLiteral("messages"));
            const auto responseChat = field(page, {"chatId", "CHAT_ID"});
            if (!rawRows.isArray() || (!responseChat.isEmpty() && positiveId(responseChat) != chatId))
                return failShape(operation);
            QSet<qint64> seen;
            QJsonArray rows;
            for (const auto &value : rawRows.toArray()) {
                const auto row = value.toObject();
                const auto id = positiveId(field(row, {"id", "ID"}));
                const auto rowChat = field(row, {"chatId", "chat_id", "CHAT_ID"});
                if (id <= 0 || id >= before || (!rowChat.isEmpty() && positiveId(rowChat) != chatId) || seen.contains(id))
                    continue;
                seen.insert(id);
                rows.append(row);
                if (rows.size() >= limit)
                    break;
            }
            page.insert(QStringLiteral("chatId"), chatId);
            page.insert(QStringLiteral("messages"), rows);
            if (task ? m_openTask == scopeId : m_openDialog == dialog)
                rememberUnreadHistoryRows(dialog, chatId, rows, revision);
            const auto messages = decodeMessages(page, true, dialog);
            const auto hasMore = !messages.isEmpty() &&
                page.value(QStringLiteral("hasNextPage")).toBool(rows.size() == limit);
            emit olderChatMessagesLoaded(scopeId, beforeId, task, messages, hasMore);
        });
    });
}

void CloudBackend::searchMessages(const QString &scopeId, const QString &query, bool task)
{
    const auto operation = (task ? QStringLiteral("search-task-chat:") : QStringLiteral("search-chat:")) + scopeId;
    const auto trimmed = query.trimmed();
    if (trimmed.isEmpty()) {
        emit messagesFound(scopeId, query, task, {});
        return;
    }
    if (trimmed.size() > maxQueryLength) {
        emit failed(operation, QStringLiteral("Слишком длинный запрос поиска"));
        return;
    }
    const auto key = operation + QLatin1Char('\n') + trimmed;
    resolveDialog(scopeId, task, operation, key,
                  [this, scopeId, query, trimmed, task, operation, key](const QString &dialog, qint64 chatId) {
        const auto url = OriginGuard(m_session.origin).resolve(QStringLiteral("/rest/im.dialog.messages.search.json"));
        webRequest(url, {{QStringLiteral("CHAT_ID"), QString::number(chatId)},
                         {QStringLiteral("SEARCH_MESSAGE"), trimmed}}, operation, resultEnvelope,
                   [this, scopeId, query, task, operation, dialog](QJsonValue result) {
            const auto obj = result.toObject();
            if (!obj.value(QStringLiteral("messages")).isArray())
                return failShape(operation);
            auto messages = decodeMessages(obj, false, dialog);
            for (auto &message : messages)
                message.unread = false;
            emit messagesFound(scopeId, query, task, messages);
        }, key);
    });
}

void CloudBackend::loadTaskAttachments(Task task, const QJsonObject &row)
{
    if (!m_taskAttachmentRevisions.contains(task.id) && m_taskAttachmentRevisions.size() >= tunables::kPageSizeChats)
        m_taskAttachmentRevisions.erase(m_taskAttachmentRevisions.begin());
    const auto revision = ++m_taskAttachmentRevision;
    m_taskAttachmentRevisions.insert(task.id, revision);
    auto source = row.value(QStringLiteral("ufTaskWebdavFiles"));
    if (!source.isArray())
        source = row.value(QStringLiteral("UF_TASK_WEBDAV_FILES"));
    if (!source.isArray())
        source = row.value(QStringLiteral("fileIds"));
    QJsonArray ids;
    QSet<QString> requested;
    for (const auto value : source.toArray()) {
        const auto id = taskFileId(text(value));
        if (id.isEmpty() || requested.contains(id))
            continue;
        requested.insert(id);
        ids.append(id.startsWith(QLatin1Char('n')) ? QJsonValue(id) : QJsonValue(id.toLongLong()));
        if (ids.size() == maxAttachmentNames)
            break;
    }

    emit taskLoaded(task);
    if (ids.isEmpty())
        return;
    const auto operation = QStringLiteral("task-files:") + task.id;
    const auto key = operation + QLatin1Char(':') + QString::number(revision);
    ajaxJson(QStringLiteral("tasks.v2.File.listObjects"), {{QStringLiteral("ids"), ids}},
             operation, [this, task = std::move(task), ids, operation, revision](QJsonValue data) mutable {
        if (m_taskAttachmentRevisions.value(task.id) != revision)
            return;
        if (!data.isArray()) {
            failShape(operation);
            return;
        }
        const auto files = responseFiles(data);
        for (const auto id : ids) {
            const auto fileId = text(id);
            const auto metadata = files.value(fileId);
            const auto name = plainFileName(field(metadata, {"name"}));
            if (name.isEmpty())
                continue;
            bool sizeKnown = false;
            const auto size = metadata.value(QStringLiteral("size")).toVariant().toLongLong(&sizeKnown);
            const auto exactSize = sizeKnown && size >= 0 ? size : -1;
            const bool photo = metadata.value(QStringLiteral("isImage")) == true;
            const auto original = registerMessageFileUrl(field(metadata, {"downloadUrl"}), exactSize, photo);
            MessageFile file{fileId, name, exactSize, original};
            file.isImage = photo;
            auto diskObject = taskFileId(field(metadata.value(QStringLiteral("customData")).toObject(), {"objectId"}));
            if (!diskObject.isEmpty() && !diskObject.startsWith(QLatin1Char('n')))
                file.diskObjectId = QLatin1Char('n') + diskObject;
            if (fileId.startsWith(QLatin1Char('n')))
                diskObject = fileId.mid(1);
            if (photo) {
                file.previewUrl = taskImageUrlFromPortal(field(metadata, {"serverPreviewUrl"}), fileId, diskObject);
                auto width = positiveId(field(metadata, {"serverPreviewWidth", "width"}));
                auto height = positiveId(field(metadata, {"serverPreviewHeight", "height"}));
                if (!file.previewUrl.isEmpty() && width > 0 && height > 0 && width <= 1048576 && height <= 1048576)
                    file.previewSize = QSize(static_cast<int>(width), static_cast<int>(height));
                if (file.previewUrl.isEmpty() && !original.isEmpty() && exactSize > 0
                    && exactSize <= m_messageImageAssets.maxBytes)
                    file.previewUrl = taskImageUrlFromPortal(original.toString(QUrl::FullyEncoded), fileId, diskObject, true);
            }
            task.attachments.append(file.name);
            task.files.append(std::move(file));
        }
        if (!task.attachments.isEmpty())
            emit taskLoaded(task);
    }, key);
}

std::shared_ptr<CloudBackend::Upload> CloudBackend::openUpload(const QString &scopeId,
    const QString &localPath, const QString &kind, const QString &operation)
{
    const auto key = taskFormMutationKey(operation);
    if (m_inflightOperations.contains(key.isEmpty() ? operation : key))
        return {};
    const QFileInfo info(localPath);
    const auto fileName = info.fileName().normalized(QString::NormalizationForm_C);
    auto file = std::make_shared<QFile>(localPath);
    if (!info.isFile() || fileName.isEmpty() || !file->open(QIODevice::ReadOnly)) {
        emit failed(operation, QStringLiteral("Не удалось открыть выбранный файл"));
        return {};
    }
    const auto size = file->size();
    if (size <= 0) {
        emit failed(operation, QStringLiteral("Выбранный файл пуст"));
        return {};
    }
    auto upload = std::make_shared<Upload>();
    upload->file = std::move(file);
    upload->size = size;
    upload->nameHeader = QUrl::toPercentEncoding(fileName);
    upload->mime = QMimeDatabase().mimeTypeForFile(info, QMimeDatabase::MatchExtension).name().toLatin1();
    if (upload->mime.isEmpty())
        upload->mime = "application/octet-stream";
    upload->scopeId = scopeId;
    upload->kind = kind;
    upload->operation = operation;
    return upload;
}

void CloudBackend::uploadChatFile(const QString &scopeId, const QString &localPath, bool task)
{
    uploadChatAttachment(scopeId, localPath, task, false, {});
}

void CloudBackend::uploadChatImage(const QString &scopeId, const QString &localPath, bool task, const QString &caption,
                                   std::shared_ptr<QTemporaryDir> temporaryDirectory)
{
    uploadChatAttachment(scopeId, localPath, task, true, caption, std::move(temporaryDirectory));
}

void CloudBackend::uploadChatAttachment(const QString &scopeId, const QString &localPath, bool task, bool image,
                                        const QString &caption, std::shared_ptr<QTemporaryDir> temporaryDirectory)
{
    const auto operation = (task ? QStringLiteral("upload-task-chat:") : QStringLiteral("upload-chat:")) + scopeId;
    if (caption.size() > tunables::kChatImageCaptionCharacters) {
        emit failed(operation, QStringLiteral("Подпись к изображению может содержать не больше 20 000 символов"));
        return;
    }
    const auto accountScope = m_accountScopeRevision;
    const auto imageKey = QLatin1String(chatImageUploadRequestPrefix) + QString::number(accountScope) +
        QLatin1Char(':') + operation;
    if (m_inflightOperations.contains(imageKey))
        return;
    const auto kind = task ? QStringLiteral("task-chat") : QStringLiteral("chat");
    const auto upload = openUpload(scopeId, localPath, kind, operation);
    if (!upload)
        return;
    if (image && upload->size > tunables::kChatClipboardPngBytes) {
        emit failed(operation, QStringLiteral("Изображение превышает 16 МиБ"));
        return;
    }
    upload->requestKey = image ? imageKey : operation;
    upload->caption = caption;
    upload->temporaryDirectory = std::move(temporaryDirectory);
    upload->image = image;
    upload->accountScope = accountScope;
    resolveDialog(scopeId, task, operation, upload->requestKey,
                  [this, upload](const QString &dialogId, qint64 chatId) {
        if (!upload->activeFor(m_accountScopeRevision))
            return;
        const auto url = OriginGuard(m_session.origin).resolve(QStringLiteral("/rest/im.disk.folder.get.json"));
        webRequest(url, {{QStringLiteral("chat_id"), QString::number(chatId)}}, upload->operation, resultEnvelope,
                   [this, upload, chatId, dialogId](QJsonValue result) {
            if (!upload->activeFor(m_accountScopeRevision))
                return;
            const auto folderId = positiveId(field(result.toObject(), {"ID"}));
            if (folderId == 0)
                return failShape(upload->operation);
            upload->chatId = chatId;
            upload->controllerOptions = {
                {QStringLiteral("folderId"), folderId},
                {QStringLiteral("chat"), QJsonObject{{QStringLiteral("chatId"), chatId},
                                                      {QStringLiteral("dialogId"), dialogId}}}
            };
            uploadChunk(upload, 0);
        }, upload->requestKey);
    });
}

void CloudBackend::attachTaskFile(const QString &taskId, const QString &localPath)
{
    const auto operation = QStringLiteral("attach-task:") + taskId;
    if (positiveId(taskId) == 0) {
        emit failed(operation, QStringLiteral("Некорректный номер задачи"));
        return;
    }
    if (const auto upload = openUpload(taskId, localPath, QStringLiteral("task"), operation))
        uploadChunk(upload, 0);
}

void CloudBackend::uploadChunk(const std::shared_ptr<Upload> &upload, qint64 offset)
{
    if (!upload->activeFor(m_accountScopeRevision))
        return;
    if (upload->file->size() != upload->size) {
        emit failed(upload->operation, QStringLiteral("Размер файла изменился во время загрузки"));
        return;
    }
    const auto length = std::min(uploadChunkSize, upload->size - offset);
    const auto end = offset + length;
    auto url = actionUrl(QStringLiteral("ui.fileuploader.upload"));
    addQueryParameter(url, QStringLiteral("controller"), QStringLiteral("disk.uf.integration.diskUploaderController"));
    if (!upload->controllerOptions.isEmpty()) {
        const auto options = QJsonDocument(upload->controllerOptions).toJson(QJsonDocument::Compact);
        addQueryParameter(url, QStringLiteral("controllerOptions"), QString::fromUtf8(options));
    }
    addQueryParameter(url, QStringLiteral("token"), upload->token);
    Headers headers{{"Content-Type", upload->mime}, {"X-Upload-Content-Name", upload->nameHeader}};
    if (length != upload->size) {
        const auto range = QByteArray("bytes ") + QByteArray::number(offset) + '-' + QByteArray::number(end - 1)
            + '/' + QByteArray::number(upload->size);
        headers.append({"Content-Range", range});
    }
    // The request body must keep the staged directory alive until its file is closed.
    auto chunkFile = std::shared_ptr<QFile>(upload, upload->file.get());
    webRequestRaw(url, new FileChunk(std::move(chunkFile), offset, length), headers, upload->operation, dataEnvelope,
                  [this, upload, end](QJsonValue response) {
        if (!upload->activeFor(m_accountScopeRevision))
            return;
        const auto data = response.toObject();
        const auto token = field(data, {"token"});
        if (token.isEmpty() || token.size() > 1024)
            return failShape(upload->operation);
        upload->token = token;
        if (upload->serverFileId.isEmpty())
            upload->serverFileId = token; // Older replies use the first token as the Disk ID.
        if (end < upload->size) {
            uploadChunk(upload, end);
            return;
        }
        const auto file = data.value(QStringLiteral("file"));
        if (!file.isObject())
            return failShape(upload->operation);
        // The final file metadata can replace the upload token with the permanent Disk ID.
        const auto finalId = file.toObject().value(QStringLiteral("serverFileId"));
        if (!finalId.isUndefined())
            upload->serverFileId = finalId.toString();
        if (upload->image)
            upload->file->close();
        commitUpload(upload);
    }, upload->requestKey);
}

void CloudBackend::commitUpload(const std::shared_ptr<Upload> &upload)
{
    const auto fileId = taskFileId(upload->serverFileId);
    if (!fileId.startsWith(QLatin1Char('n')) || fileId != upload->serverFileId)
        return failShape(upload->operation);
    const auto taskObjectId = upload->chatId == 0 ? fileId : QString();
    const auto complete = [this, scopeId = upload->scopeId, kind = upload->kind,
                           operation = upload->operation, taskObjectId, image = upload->image,
                           accountScope = upload->accountScope](QJsonValue result) {
        if (image && accountScope != m_accountScopeRevision)
            return;
        if (result.isBool() && !result.toBool()) {
            failShape(operation);
            return;
        }
        if (!taskObjectId.isEmpty())
            emit taskFileAttached(scopeId, taskObjectId);
        emit fileUploaded(scopeId, kind);
    };
    if (!taskObjectId.isEmpty()) {
        const QJsonObject task{{QStringLiteral("id"), positiveId(upload->scopeId)}};
        ajaxJson(QStringLiteral("tasks.v2.File.attach"),
                 {{QStringLiteral("task"), task}, {QStringLiteral("ids"), QJsonArray{taskObjectId}}},
                 upload->operation, complete);
        return;
    }
    const auto url = OriginGuard(m_session.origin).resolve(QStringLiteral("/rest/im.disk.file.commit.json"));
    webRequest(url, {{QStringLiteral("chat_id"), QString::number(upload->chatId)},
                     {QStringLiteral("message"), upload->caption},
                     {QStringLiteral("template_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                     {QStringLiteral("file_template_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                     {QStringLiteral("as_file"), upload->image ? QStringLiteral("N") : QStringLiteral("Y")},
                     {QStringLiteral("upload_id"), fileId.mid(1)}}, upload->operation, resultEnvelope, complete,
                     upload->requestKey);
}

} // namespace bx
