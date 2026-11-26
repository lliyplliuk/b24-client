#include "cloud/tasks_service.h"
#include "cloud/cloud_rows.h"
#include "core/tunables.h"

#include <QSet>
#include <utility>

namespace bx {
using namespace cloud_rows;

void TasksService::loadTaskAttachments(Task task, const QJsonObject &row)
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
        const auto id = taskFileId(string(value));
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
    const auto operation = operationKey(OperationKind::TaskFiles, task.id);
    const auto key = operation + QLatin1Char(':') + QString::number(revision);
    m_portalHttp.ajaxJson(QStringLiteral("tasks.v2.File.listObjects"), {{QStringLiteral("ids"), ids}},
             operation, [this, task = std::move(task), ids, operation, revision](QJsonValue data) mutable {
        if (m_taskAttachmentRevisions.value(task.id) != revision)
            return;
        if (!data.isArray()) {
            m_session.failShape(operation);
            return;
        }
        const auto files = responseFiles(data);
        for (const auto id : ids) {
            const auto fileId = string(id);
            const auto metadata = files.value(fileId);
            const auto name = plainFileName(field(metadata, {"name"}));
            if (name.isEmpty())
                continue;
            bool sizeKnown = false;
            const auto size = metadata.value(QStringLiteral("size")).toVariant().toLongLong(&sizeKnown);
            const auto exactSize = sizeKnown && size >= 0 ? size : -1;
            const bool photo = metadata.value(QStringLiteral("isImage")) == true;
            const auto original = m_files.taskFileUrl(field(metadata, {"downloadUrl"}), exactSize, photo);
            MessageFile file{fileId, name, exactSize, original};
            file.isImage = photo;
            auto diskObject = taskFileId(field(metadata.value(QStringLiteral("customData")).toObject(), {"objectId"}));
            if (!diskObject.isEmpty() && !diskObject.startsWith(QLatin1Char('n')))
                file.diskObjectId = QLatin1Char('n') + diskObject;
            if (fileId.startsWith(QLatin1Char('n')))
                diskObject = fileId.mid(1);
            if (photo) {
                file.previewUrl = m_files.taskImageUrl(field(metadata, {"serverPreviewUrl"}), fileId, diskObject);
                const auto width = field(metadata, {"serverPreviewWidth", "width"}).toLongLong();
                const auto height = field(metadata, {"serverPreviewHeight", "height"}).toLongLong();
                if (!file.previewUrl.isEmpty() && width > 0 && height > 0 && width <= 1048576 && height <= 1048576)
                    file.previewSize = QSize(static_cast<int>(width), static_cast<int>(height));
                if (file.previewUrl.isEmpty() && !original.isEmpty() && exactSize > 0
                    && exactSize <= m_files.taskImageMaxBytes())
                    file.previewUrl = m_files.taskImageUrl(original.toString(QUrl::FullyEncoded), fileId, diskObject, true);
            }
            task.attachments.append(file.name);
            task.files.append(std::move(file));
        }
        if (!task.attachments.isEmpty())
            emit taskLoaded(task);
    }, key);
}

void TasksService::attachTaskFile(const QString &taskId, const QString &localPath)
{
    const auto operation = operationKey(OperationKind::AttachTask, taskId);
    if (numericId(taskId).isEmpty()) {
        emit failed(operation, QStringLiteral("Некорректный номер задачи"));
        return;
    }
    m_files.uploadTaskFile(taskId, localPath, operation);
}

void TasksService::attachUploadedFile(const QString &taskId, const QString &fileId, const QString &operation)
{
    const QJsonObject task{{QStringLiteral("id"), taskId.toLongLong()}};
    m_portalHttp.ajaxJson(QStringLiteral("tasks.v2.File.attach"),
        {{QStringLiteral("task"), task}, {QStringLiteral("ids"), QJsonArray{fileId}}},
        operation, [this, taskId, fileId, operation](QJsonValue result) {
            if (result.isBool() && !result.toBool())
                return m_session.failShape(operation);
            emit taskFileAttached(taskId, fileId);
            emit fileUploaded(taskId, QStringLiteral("task"));
        });
}

} // namespace bx
