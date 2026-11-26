#include "ui/task_card_controller.h"
#include "ui/task_dialogs.h"
#include "ui/chat_panel.h"
#include "ui/projects_panel.h"
#include "ui/history_surface.h"
#include "ui/chat_widgets.h"
#include "ui/workspace_message_previews.h"
#include "ui/workspace_presentation.h"

namespace bx {

void TaskCardController::connectBackend()
{
    connect(m_backend, &Backend::taskChecklistLoaded, this, [this](const QString &id, const auto &items) {
        if (m_card && m_card->taskId() == id)
            m_card->relations()->setChecklist(items);
    });
    connect(m_backend, &Backend::taskChecklistChanged, this, [this](const QString &id) {
        if (m_card && m_card->taskId() == id) {
            m_card->relations()->confirmChecklistChange();
            m_card->setStatus(QString());
            m_backend->requestTaskChecklist(id);
        }
    });
    connect(m_backend, &Backend::subtasksLoaded, this, [this](const QString &id, const auto &tasks) {
        if (m_card && m_card->taskId() == id)
            m_card->relations()->setSubtasks(tasks);
    });
    connect(m_backend, &Backend::taskDependenciesLoaded, this, [this](const QString &id, const auto &dependencies) {
        if (m_card && m_card->taskId() == id)
            m_card->relations()->setDependencies(dependencies);
    });
    connect(m_backend, &Backend::taskHistoryLoaded, this, [this](const QString &id, const auto &history, bool limited) {
        if (m_card && m_card->taskId() == id)
            m_card->relations()->setHistory(history, limited);
    });
    connect(m_backend, &Backend::subtaskCreated, this, [this](const QString &parent, const QString &project, const QString &task) {
        if (!task.isEmpty() && !m_pendingCreateSubtask.isEmpty() && parent == m_pendingCreateSubtask) {
            m_pendingCreateSubtask.clear();
            beginTaskFormFiles(task);
        }
        if (m_card && m_card->taskId() == parent) {
            m_card->setStatus(QStringLiteral("Подзадача создана"));
            m_backend->requestSubtasks(parent);
        }
        if (!project.isEmpty())
            m_projectsPanel->refreshTasks(project);
        emit writeStateChanged();
    });
    connect(m_backend, &Backend::taskLinkPreviewInvalidated, m_messagePreviews, &WorkspaceMessagePreviews::invalidateTaskLinkPreview);
    connect(m_backend, &Backend::taskUpdated, m_messagePreviews, &WorkspaceMessagePreviews::invalidateTaskLinkPreview);
    connect(m_backend, &Backend::taskUpdated, this, &TaskCardController::onTaskUpdated);
    connect(m_backend, &Backend::taskDeleted, this, &TaskCardController::onTaskDeleted);
    connect(m_backend, &Backend::taskCreated, this, &TaskCardController::onTaskCreated);
    connect(m_backend, &Backend::messagesFound, this, &TaskCardController::onMessagesFound);
    connect(m_backend, &Backend::fileUploaded, this, &TaskCardController::onFileUploaded);
    connect(m_backend, &Backend::taskFileAttached, this, [this](const QString &taskId, const QString &fileId) {
        if (taskId != m_descriptionUploadTask || m_workspace.upload.operation != operationKey(OperationKind::AttachTask, taskId))
            return;
        if (m_card && m_descriptionUploadCard == m_card) {
            m_card->descriptionEditor()->insertAttachment(fileId, m_workspace.upload.name, m_descriptionUploadSize);
            m_descriptionUploadLinked = true;
        }
    });
    auto *surface = new ui::HistorySurface(m_backend, true, [this]() -> ui::HistorySurface::View {
        if (!m_card)
            return {};
        return {static_cast<ui::HistoryList *>(m_card->messageList()), m_card->taskChatDialogId(), m_card->taskId(),
                true, !m_cardSearchQuery.isEmpty(), !m_cardContextMessageId.isEmpty()};
    }, this);
    connect(surface, &ui::HistorySurface::pinsChanged, this, [this](const QVector<Message> &messages) {
        m_card->setPinnedMessages(messages);
    });
    connect(surface, &ui::HistorySurface::olderPageAdded, this, [this] {
        emit avatarRowsChanged();
        emit avatarsRequested();
    });
    connect(surface, &ui::HistorySurface::historyCleared, this, [this] {
        m_cardContextMessageId.clear();
        m_card->setContextMode(false);
        m_card->setChatStatus(QStringLiteral("Сообщений пока нет"));
    });

    connect(m_backend, &Backend::messageContextLoaded, this,
            [this](const QString &dialogId, const QString &messageId, const QVector<Message> &messages) {
        if (m_card && !m_cardContextMessageId.isEmpty() && dialogId == m_card->taskChatDialogId() &&
            messageId == m_cardContextMessageId) {
            m_card->showMessages(messages, ui::MessageView::Context);
            emit avatarsRequested();
            if (!m_card->focusMessage(messageId)) {
                m_cardContextMessageId.clear();
                m_card->setContextMode(false);
                m_backend->requestTaskMessages(m_card->taskId());
                m_card->setStatus(QStringLiteral("Сообщение не найдено в истории чата задачи"));
            }
            return;
        }
        m_chatPanel->showMessageContext(dialogId, messageId, messages);
    });
    connect(m_backend, &Backend::peopleFound, this, &TaskCardController::onPeopleFound);
    connect(m_backend, &Backend::projectStagesLoaded, this,
            [this](const QString &projectId, const QVector<Stage> &stages) {
        if (m_taskForm && m_taskForm->projectId() == projectId)
            m_taskForm->setStages(stages);
        if (!m_card || m_card->projectId() != projectId || m_cardRequestedStages != projectId)
            return;
        if (projectId != m_projectsPanel->currentProjectId() || m_projectsPanel->stages().isEmpty())
            m_card->setStages(stages);
    });

    connect(m_backend, &Backend::taskSummaryLoaded, this, &TaskCardController::onTaskSummaryLoaded);
    connect(m_backend, &Backend::taskLoaded, this, [this](const Task &task) {
        if (!m_card || task.id != m_card->taskId())
            return;
        auto *const card = m_card;
        m_cardSummaryPending = false;
        const auto previousDialog = m_card->taskChatDialogId();
        if (!m_cardDetailsReady)
            card->setStatus({});
        m_card->setTask(task);
        const auto projectId = m_card->projectId();
        if (!m_projectsPanel->directoryLoaded() && !m_cardRequestedProjectDirectory
            && (task.editRights.project || !projectId.isEmpty())) {
            m_cardRequestedProjectDirectory = true;
            m_backend->requestProjects();
            if (m_card != card)
                return;
        }
        if (projectId == m_projectsPanel->currentProjectId() && !m_projectsPanel->stages().isEmpty()) {
            m_card->setStages(m_projectsPanel->stages());
        } else if (task.editRights.stage && !projectId.isEmpty() && m_cardRequestedStages != projectId) {
            m_cardRequestedStages = projectId;
            m_backend->requestProjectStages(projectId);
        }
        updateTaskCardChatBinding(previousDialog);
        if (!task.projectId.isEmpty())
            m_card->setLink(ui::taskLink(m_backend->portalOrigin(), task.projectId, task.id));
        emit avatarsRequested();
        m_cardDetailsReady = true;
        emit writeStateChanged();
        if (m_loadCardChat) {
            m_loadCardChat = false;
            m_backend->requestTaskMessages(task.id);
        }
        m_chatPanel->scheduleVisibleChatRead();
    });
    connect(m_backend, &Backend::taskMessagesLoaded, this,
            [this](const QString &taskId, const QVector<Message> &messages) {
        if (m_card && taskId == m_card->taskId() && m_cardSearchQuery.isEmpty() && m_cardContextMessageId.isEmpty()) {
            m_card->showMessages(messages);
            emit avatarRowsChanged();
            emit avatarsRequested();
        }
    });

    connect(m_backend, &Backend::messageSent, this, [this](const QString &scope, const QString &) {
        m_taskSendController.confirm(scope, m_card ? m_card->composer() : nullptr,
                                     m_card ? m_card->taskId() : QString{}, m_taskDrafts);
    });
}

void TaskCardController::onPeopleFound(const QString &requestId, const QVector<Person> &people)
{
    if (m_taskForm)
        m_taskForm->setPeopleResults(requestId, people);
    if (m_card) {
        m_card->composer()->setMentionResults(requestId, people);
        m_card->setPeopleResults(requestId, people);
    }
}

} // namespace bx
