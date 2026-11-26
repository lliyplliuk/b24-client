#include "ui/task_card_controller.h"
#include "ui/task_dialogs.h"
#include "ui/chat_panel.h"
#include "ui/projects_panel.h"
#include "ui/chat_widgets.h"

namespace bx {

bool TaskCardController::handleFailure(const QString &operation, OperationKind kind, const QStringList &ids,
                                       const QString &reason, bool networkFailure, bool &statusPresented)
{
    if (networkFailure && kind == OperationKind::TaskMessages &&
        (!m_card || !m_cardSearchQuery.isEmpty() || ids.value(0) != m_card->taskId()))
        return true;
    switch (kind) {
    case OperationKind::SearchPeople: {
        const auto request = ids.value(0);
        if (m_taskForm)
            m_taskForm->setPeopleSearchError(request);
        if (m_card) {
            m_card->composer()->setMentionSearchError(request);
            m_card->setPeopleSearchError(request);
        }
        return true;
    }
    case OperationKind::OlderChat: {
        if (m_card)
            static_cast<ui::HistoryList *>(m_card->messageList())->olderFailed(ids.value(1), ids.value(2), reason);
        return true;
    }
    case OperationKind::ProjectStages:
        if (m_taskForm && ids.value(0) == m_taskForm->projectId())
            m_taskForm->setStagesError(reason);
        if (m_card && ids.value(0) == m_cardRequestedStages) {
            m_cardRequestedStages.clear();
            m_card->setStatus(reason);
        }
        return true;
    case OperationKind::TaskHistory:
        if (m_card && ids.value(0) == m_card->taskId())
            m_card->relations()->historyFailed();
        return true;
    case OperationKind::MessageEdit: {
        const auto dialog = ids.value(0);
        const auto id = ids.value(1);
        if (m_card && m_card->composer()->matchesEdit(dialog, id)) {
            m_card->composer()->failMessageEdit();
            m_card->setStatus(reason);
        }
        return true;
    }
    case OperationKind::Checklist:
    case OperationKind::ChecklistWrite:
    case OperationKind::Subtasks:
    case OperationKind::GanttLinks:
        if (!m_card || ids.value(0) != m_card->taskId())
            break;
        if (kind == OperationKind::Checklist)
            m_card->relations()->checklistReadFailed();
        else if (kind == OperationKind::ChecklistWrite)
            m_card->relations()->setChecklistPending(false);
        else if (kind == OperationKind::Subtasks)
            m_card->relations()->subtasksReadFailed();
        else
            m_card->relations()->dependenciesReadFailed();
        m_card->setStatus(reason);
        return true;
    case OperationKind::CreateSubtask:
        if (m_pendingCreateSubtask.isEmpty() || ids.value(0) != m_pendingCreateSubtask)
            break;
        m_pendingCreateSubtask.clear();
        if (m_taskForm)
            m_taskForm->setStatus(reason);
        emit writeStateChanged();
        return true;
    case OperationKind::MessageContext:
        if (m_card && !m_cardContextMessageId.isEmpty() && ids.value(0) == m_card->taskChatDialogId()
            && ids.value(1) == m_cardContextMessageId) {
            m_cardContextMessageId.clear();
            m_card->setContextMode(false);
            m_card->setStatus(reason);
            m_backend->requestTaskMessages(m_card->taskId());
            return true;
        }
        if (m_chatPanel->failMessageContext(ids, reason))
            return true;
        break;
    case OperationKind::SearchTaskChat: {
        const auto query = m_taskMessageSearch.takeFailed(ids.value(0));
        if (query.isEmpty())
            return true;
        if (m_card && ids.value(0) == m_card->taskId() &&
            !m_cardSearchQuery.isEmpty()) {
            if (query != m_cardSearchQuery)
                m_taskMessageSearch.request(m_card->taskId(), m_cardSearchQuery);
            else if (query == m_card->searchQuery())
                m_card->setChatStatus(reason);
        }
        return true;
    }
    case OperationKind::UploadTaskChat:
    case OperationKind::AttachTask: {
        if (operation != m_workspace.upload.operation)
            return true;
        const auto scope = ids.value(0);
        const auto name = m_workspace.upload.name;
        m_workspace.upload = {};
        if (m_taskForm && m_taskForm->createdTask() == scope && kind == OperationKind::AttachTask) {
            m_taskForm->setStatus(QStringLiteral("Задача №%1 создана. Не удалось загрузить файл «%2». %3")
                                  .arg(scope, name, reason));
            m_chatPanel->updateComposers();
            emit writeStateChanged();
            return true;
        }
        bool reconcileTask = false;
        if (m_descriptionUploadTask == scope && kind == OperationKind::AttachTask) {
            reconcileTask = m_descriptionUploadConfirmed;
            if (m_card && m_descriptionUploadCard == m_card)
                m_card->descriptionEditor()->setUploadStatus(reason);
            clearDescriptionUpload();
        }
        if (m_card && m_card->taskId() == scope) {
            m_card->setStatus(reason);
            if (reconcileTask)
                m_backend->requestTask(scope);
        }
        break;
    }
    case OperationKind::CreateTask:
        if (m_pendingCreateTask.isEmpty() || ids.value(0) != m_pendingCreateTask)
            break;
        m_pendingCreateTask.clear();
        if (m_taskForm)
            m_taskForm->setStatus(reason);
        break;
    case OperationKind::UpdateTask:
        if (m_pendingTaskEdit.isEmpty() || ids.value(0) != m_pendingTaskEdit)
            break;
        if (m_card && m_card->taskId() == m_pendingTaskEdit) {
            m_card->setStatus(networkFailure
                ? QStringLiteral("Не удалось подтвердить сохранение задачи. Проверьте изменения на портале перед повтором")
                : reason);
            statusPresented = networkFailure;
        }
        m_pendingTaskEdit.clear();
        m_pendingTaskEditCard.clear();
        m_pendingTaskEditProject.clear();
        m_pendingTaskEditTargetProject.clear();
        break;
    case OperationKind::DeleteTask:
        if (m_pendingTaskDelete.isEmpty() || ids.value(0) != m_pendingTaskDelete)
            break;
        if (m_card && m_card == m_pendingTaskDeleteCard)
            m_card->setStatus(networkFailure
                ? QStringLiteral("Не удалось подтвердить удаление задачи. Проверьте её на портале перед повтором")
                : reason);
        m_pendingTaskDelete.clear();
        m_pendingTaskDeleteCard.clear();
        m_pendingTaskDeleteProject.clear();
        emit writeStateChanged();
        return true;
    case OperationKind::SendTask: {
        const auto id = ids.value(0);
        if (m_taskSendController.fail(id, reason, networkFailure))
            statusPresented = networkFailure || !m_card || m_card->taskId() != id;
        break;
    }
    case OperationKind::Task:
    case OperationKind::TaskMessages: {
        if (!m_card || ids.value(0) != m_card->taskId())
            break;
        const bool detailsFailed = kind == OperationKind::Task;
        if (detailsFailed)
            m_cardSummaryPending = false;
        const auto retryCard = detailsFailed
            ? QStringLiteral("Не удалось загрузить задачу. Откройте карточку заново")
            : QStringLiteral("Не удалось загрузить чат задачи. Откройте карточку заново");
        m_card->setStatus(networkFailure ? retryCard : reason);
        statusPresented = networkFailure;
        if (m_cardSearchQuery.isEmpty() && (!detailsFailed || m_loadCardChat)
            && (!networkFailure || m_card->messageList()->count() == 0))
            m_card->setChatStatus(QStringLiteral("Не удалось загрузить чат задачи"));
        break;
    }
    default:
        break;
    }
    return false;
}

} // namespace bx
