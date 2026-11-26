#include "ui/main_window.h"
#include "ui/chat_panel.h"
#include "ui/workspace_message_previews.h"
#include "ui/message_files.h"
#include "ui/chat_composer.h"
#include "ui/task_card_dialog.h"
#include "ui/row_painter.h"
#include "core/operations.h"

#include <QFileDialog>
#include <QFileInfo>

namespace bx {

void MainWindow::sendFromComposer(ui::ComposerSendController &controller, ui::ChatComposer *composer, const QString &id)
{
    const bool task = controller.isTask();
    controller.send(*composer, id, writable(), m_upload.pastedImagePending(id, task),
                    [this, composer, id, task](const QString &caption) {
                        uploadPastedChatImage(composer, id, task, caption);
                    });
}

void MainWindow::bindMessageFiles(QListWidget *list, bool linkCards)
{
    auto *delegate = static_cast<workspace::RowPainter *>(list->itemDelegate());
    m_messageFiles->bindMessageFiles(list);
    delegate->setMessageLinkHandler([this, list](const QString &href) { openMessageLink(href, list); });
    if (linkCards)
        m_messagePreviews->bindLinkCards(list, delegate);
}

void MainWindow::uploadPastedChatImage(ui::ChatComposer *composer, const QString &scope, bool task, const QString &caption)
{
    if (!m_upload.operation.isEmpty())
        return;
    const auto draft = composer->draft();
    if (!draft.image)
        return;
    const auto feedback = [this, task, &scope](const QString &status, bool loading = false) {
        if (!task || !m_tasks->showTaskStatus(scope, status, loading))
            setStatus(status, loading);
    };
    if (!draft.reply.id.isEmpty()) {
        feedback(QStringLiteral("Чтобы отправить изображение, отмените ответ или удалите изображение из черновика"));
        return;
    }
    m_upload.operation = operationKey(task ? OperationKind::UploadTaskChat : OperationKind::UploadChat, scope);
    m_upload.pastedImage = draft;
    m_upload.name = QFileInfo(draft.image->path).fileName();
    feedback(QStringLiteral("Отправляем изображение…"), true);
    m_chatPanel->updateComposers();
    updateWriteAvailability();
    m_backend->uploadChatImage(scope, draft.image->path, task, caption, draft.image->directory);
}

void MainWindow::pickFile(const QString &scope, const QString &kind)
{
    if (!writable() || !m_upload.operation.isEmpty() || scope.isEmpty())
        return;
    const bool attachment = kind == QStringLiteral("task");
    const bool taskChat = kind == QStringLiteral("task-chat");
    QWidget *parent = this;
    if ((attachment || taskChat) && m_tasks->card())
        parent = m_tasks->card();
    const auto path = QFileDialog::getOpenFileName(parent, QStringLiteral("Выберите файл"));
    if (path.isEmpty() || !writable())
        return;
    m_upload.operation = operationKey(attachment ? OperationKind::AttachTask
        : taskChat ? OperationKind::UploadTaskChat : OperationKind::UploadChat, scope);
    m_upload.name = QFileInfo(path).fileName();
    const auto status = QStringLiteral("Загружаем %1…").arg(m_upload.name);
    if (!(attachment || taskChat) || !m_tasks->showTaskStatus(scope, status, true))
        setStatus(status, true);
    m_chatPanel->updateComposers();
    updateWriteAvailability();
    if (attachment)
        m_backend->attachTaskFile(scope, path);
    else
        m_backend->uploadChatFile(scope, path, taskChat);
}

} // namespace bx
