#include "cloud/cloud_backend.h"

#include "cloud/cloud_chat_fields.h"
#include "cloud/cloud_rows.h"
#include "core/origin_guard.h"
#include "core/tunables.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMimeDatabase>
#include <QUuid>

namespace bx {
using chat_fields::positiveId;
using cloud_rows::field;
using cloud_rows::taskFileId;

namespace {

constexpr qint64 uploadChunkSize = 10 * 1024 * 1024;

const QString dataEnvelope = QStringLiteral("data");
const QString resultEnvelope = QStringLiteral("result");

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

std::shared_ptr<CloudBackend::Upload> CloudBackend::openUpload(const QString &scopeId,
    const QString &localPath, const QString &kind, const QString &operation)
{
    const auto policy = defaultRequestPolicy(parseOperation(operation).kind);
    if (m_portalHttp.contains(m_portalHttp.scopedRequestKey(operation, policy)))
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
    const auto operation = operationKey(task ? OperationKind::UploadTaskChat : OperationKind::UploadChat, scopeId);
    if (caption.size() > tunables::kChatImageCaptionCharacters) {
        emit failed(operation, QStringLiteral("Подпись к изображению может содержать не больше 20 000 символов"));
        return;
    }
    const auto accountScope = m_accountScopeRevision;
    const auto imageKey = operationKey(OperationKind::ChatImageUpload, QString::number(accountScope), operation);
    if (m_portalHttp.contains(imageKey))
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
        m_portalHttp.webRequest(url, {{QStringLiteral("chat_id"), QString::number(chatId)}}, upload->operation, resultEnvelope,
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
    m_tasks.attachTaskFile(taskId, localPath);
}

void CloudBackend::uploadTaskFile(const QString &taskId, const QString &localPath, const QString &operation)
{
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
    m_portalHttp.webRequestRaw(url, new FileChunk(std::move(chunkFile), offset, length), headers, upload->operation, dataEnvelope,
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
    if (upload->chatId == 0) {
        m_tasks.attachUploadedFile(upload->scopeId, fileId, upload->operation);
        return;
    }
    const auto complete = [this, scopeId = upload->scopeId, kind = upload->kind,
                           operation = upload->operation, image = upload->image,
                           accountScope = upload->accountScope](QJsonValue result) {
        if (image && accountScope != m_accountScopeRevision)
            return;
        if (result.isBool() && !result.toBool()) {
            failShape(operation);
            return;
        }
        emit fileUploaded(scopeId, kind);
    };
    const auto url = OriginGuard(m_session.origin).resolve(QStringLiteral("/rest/im.disk.file.commit.json"));
    m_portalHttp.webRequest(url, {{QStringLiteral("chat_id"), QString::number(upload->chatId)},
                     {QStringLiteral("message"), upload->caption},
                     {QStringLiteral("template_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                     {QStringLiteral("file_template_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                     {QStringLiteral("as_file"), upload->image ? QStringLiteral("N") : QStringLiteral("Y")},
                     {QStringLiteral("upload_id"), fileId.mid(1)}}, upload->operation, resultEnvelope, complete,
                     upload->requestKey);
}

} // namespace bx
