#include "ui/task_card_controller.h"
#include "ui/task_dialogs.h"
#include "ui/chat_panel.h"
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QTemporaryDir>

namespace bx {

void TaskCardController::onFileUploaded(const QString &scope, const QString &kind)
{
    if (kind != QStringLiteral("task") && kind != QStringLiteral("task-chat"))
        return;
    const auto operation = operationKey(kind == QStringLiteral("task") ? OperationKind::AttachTask : OperationKind::UploadTaskChat, scope);
    if (m_workspace.upload.operation != operation)
        return;
    const auto sent = std::exchange(m_workspace.upload.pastedImage, {});
    if (sent.image && kind == QStringLiteral("task-chat"))
        clearSentDraft(m_card ? m_card->composer() : nullptr, m_taskDrafts,
                       m_card ? m_card->taskId() : QString{}, scope, sent);
    if (kind == QStringLiteral("task") && m_taskForm && m_taskForm->createdTask() == scope) {
        m_workspace.upload.operation.clear();
        m_workspace.upload.name.clear();
        m_taskForm->confirmFirstFile();
        if (!m_taskForm->queuedFiles().isEmpty()) {
            if (!uploadNextTaskFormFile())
                m_taskForm->setStatus(QStringLiteral("Задача №%1 создана. Повторите загрузку оставшихся файлов.").arg(scope));
        } else {
            const auto project = m_taskForm->projectToOpen();
            m_taskForm->accept();
            emit statusChanged(QStringLiteral("Файлы прикреплены к задаче №%1").arg(scope));
            if (!project.isEmpty() && m_workspace.projectsActive())
                openTaskCardForProject(scope, project);
        }
        m_chatPanel->updateComposers();
        emit writeStateChanged();
        return;
    }
    const bool descriptionUpload = kind == QStringLiteral("task") && scope == m_descriptionUploadTask;
    if (descriptionUpload) {
        m_descriptionUploadConfirmed = true;
        m_descriptionPaste.reset();
        if (m_card && m_descriptionUploadCard == m_card)
            m_card->descriptionEditor()->setUploadStatus(m_descriptionUploadLinked
                ? QStringLiteral("Файл вставлен в описание")
                : QStringLiteral("Файл прикреплён к задаче. Ссылка для вставки недоступна."));
    }
    m_workspace.upload.operation.clear();
    const auto status = QStringLiteral("Файл загружен: %1").arg(m_workspace.upload.name);
    m_workspace.upload.name.clear();
    if (descriptionUpload && uploadNextDescriptionFile())
        return;
    if (m_card && m_card->taskId() == scope) {
        m_card->setStatus(status);
        if (kind == QStringLiteral("task"))
            m_backend->requestTask(scope);
        else if (m_cardSearchQuery.isEmpty())
            m_backend->requestTaskMessages(scope);
    } else {
        emit statusChanged(status);
    }
    m_chatPanel->updateComposers();
    emit writeStateChanged();
}

void TaskCardController::queueDescriptionFiles(const QString &taskId, const QStringList &paths)
{
    if (!writable() || !m_card || m_card->taskId() != taskId || paths.isEmpty() ||
        !m_workspace.upload.operation.isEmpty() || !m_descriptionUploadTask.isEmpty())
        return;
    m_descriptionUploadTask = taskId;
    m_descriptionUploadCard = m_card;
    m_descriptionUploadPaths = paths;
    uploadNextDescriptionFile();
}

bool TaskCardController::uploadNextDescriptionFile()
{
    if (!writable() || !m_card || m_descriptionUploadCard != m_card ||
        m_card->taskId() != m_descriptionUploadTask ||
        m_descriptionUploadPaths.isEmpty()) {
        clearDescriptionUpload();
        return false;
    }
    const auto taskId = m_descriptionUploadTask;
    const auto path = m_descriptionUploadPaths.takeFirst();
    m_workspace.upload.operation = operationKey(OperationKind::AttachTask, taskId);
    m_workspace.upload.name = QFileInfo(path).fileName();
    m_descriptionUploadLinked = false;
    m_descriptionUploadSize = QImageReader(path).size();
    const auto status = QStringLiteral("Загружаем %1…").arg(m_workspace.upload.name);
    m_card->descriptionEditor()->setUploadStatus(status, true);
    m_chatPanel->updateComposers();
    emit writeStateChanged();
    m_backend->attachTaskFile(taskId, path);
    return true;
}

void TaskCardController::clearDescriptionUpload()
{
    m_descriptionUploadTask.clear();
    m_descriptionUploadCard.clear();
    m_descriptionUploadPaths.clear();
    m_descriptionUploadSize = {};
    m_descriptionUploadLinked = false;
    m_descriptionUploadConfirmed = false;
    m_descriptionPaste.reset();
}

void TaskCardController::pasteDescriptionPhoto(const QString &taskId, const QImage &image)
{
    if (!writable() || !m_card || m_card->taskId() != taskId || !m_workspace.upload.operation.isEmpty() ||
        !m_descriptionUploadTask.isEmpty())
        return;
    if (image.isNull() || qint64(image.width()) * image.height() > 12 * 1024 * 1024) {
        m_card->descriptionEditor()->setUploadStatus(QStringLiteral("Изображение слишком большое для вставки"));
        return;
    }
    auto directory = std::make_unique<QTemporaryDir>(QDir::temp().filePath(QStringLiteral("bitrix24-task-XXXXXX")));
    const auto path = directory->filePath(QStringLiteral("Из буфера.png"));
    if (!directory->isValid() || !image.save(path, "PNG")) {
        m_card->descriptionEditor()->setUploadStatus(QStringLiteral("Не удалось подготовить изображение для загрузки"));
        return;
    }
    m_descriptionPaste = std::move(directory);
    queueDescriptionFiles(taskId, {path});
}

bool TaskCardController::beginTaskFormFiles(const QString &taskId, const QString &projectToOpen)
{
    if (!m_taskForm)
        return false;
    if (m_taskForm->queuedFiles().isEmpty()) {
        m_taskForm->accept();
        return false;
    }
    m_taskForm->setCreatedTask(taskId, projectToOpen);
    if (!uploadNextTaskFormFile())
        m_taskForm->setStatus(QStringLiteral("Задача №%1 создана. Повторите загрузку оставшихся файлов.").arg(taskId));
    return true;
}

bool TaskCardController::uploadNextTaskFormFile()
{
    if (!writable() || !m_workspace.upload.operation.isEmpty() || !m_taskForm
        || m_taskForm->createdTask().isEmpty() || m_taskForm->queuedFiles().isEmpty())
        return false;
    const auto task = m_taskForm->createdTask();
    const auto path = m_taskForm->queuedFiles().first();
    m_workspace.upload.operation = operationKey(OperationKind::AttachTask, task);
    m_workspace.upload.name = QFileInfo(path).fileName();
    m_taskForm->setStatus(QStringLiteral("Загружаем «%1»…").arg(m_workspace.upload.name), true);
    m_chatPanel->updateComposers();
    emit writeStateChanged();
    m_backend->attachTaskFile(task, path);
    return true;
}

} // namespace bx
