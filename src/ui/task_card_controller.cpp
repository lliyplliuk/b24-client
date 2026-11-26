#include "ui/task_card_controller.h"

#include "core/notification_targets.h"
#include "ui/chat_panel.h"
#include "ui/chat_widgets.h"
#include "ui/projects_panel.h"
#include "ui/task_dialogs.h"
#include "ui/requisite_avatar.h"
#include "ui/workspace_message_previews.h"
#include "ui/workspace_message_viewers.h"
#include "ui/workspace_presentation.h"

#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTemporaryDir>

#include <utility>

namespace bx {

TaskCardController::TaskCardController(Backend *backend, ChatPanel *chat, ProjectsPanel *projects,
                                       WorkspaceMessagePreviews *previews, Workspace workspace, QWidget *window)
    : QObject(window), m_backend(backend), m_chatPanel(chat), m_projectsPanel(projects),
      m_messagePreviews(previews), m_workspace(std::move(workspace)), m_window(window),
      m_taskMessageSearch(backend, true),
      m_taskSendController(ui::ComposerSendController::Surface::Task, backend,
          [this](const QString &id, const QString &status, bool loading) {
              if (m_card && m_card->taskId() == id)
                  m_card->setStatus(status, loading);
              else if (!status.isEmpty())
                  emit statusChanged(status, loading);
              emit writeStateChanged();
          })
{
    connect(m_projectsPanel, &ProjectsPanel::projectTaskRequested, this, &TaskCardController::openTaskCardForProject);
    connect(m_projectsPanel, &ProjectsPanel::taskCreationRequested, this, &TaskCardController::createTaskInProject);
    connect(m_projectsPanel, &ProjectsPanel::directoryChanged, this, [this](const QVector<Project> &projects) {
        if (m_card) {
            m_card->setProjectChoices(projects);
            emit avatarsRequested();
        }
        if (m_taskForm)
            m_taskForm->setProjectChoices(projects);
    });
    connect(m_projectsPanel, &ProjectsPanel::moreProjectsChanged, this, [this](bool available) {
        if (m_card)
            m_card->setMoreProjectsAvailable(available);
        if (m_taskForm)
            m_taskForm->setMoreProjectsAvailable(available);
    });
    connect(m_projectsPanel, &ProjectsPanel::formStagesLoaded, this, [this](const QString &id, const QVector<Stage> &stages) {
        if (m_taskForm && m_taskForm->projectId() == id)
            m_taskForm->setStages(stages);
    });
    connect(m_projectsPanel, &ProjectsPanel::currentStagesLoaded, this, [this](const QString &id, const QVector<Stage> &stages) {
        if (m_card && m_card->projectId() == id)
            m_card->setStages(stages);
    });
    connect(m_projectsPanel, &ProjectsPanel::taskMoved, this, [this](const QString &id) {
        if (m_card && m_card->taskId() == id) {
            m_card->setStatus(QStringLiteral("Задача перенесена"));
            m_backend->requestTask(id);
        }
    });
    connect(m_projectsPanel, &ProjectsPanel::boardMoveFailed, this, [this](const QString &id, const QString &reason) {
        if (m_card && m_card->taskId() == id)
            m_card->setStatus(reason);
    });
    connect(m_projectsPanel, &ProjectsPanel::directoryLoadFailed, this, [this] { m_cardRequestedProjectDirectory = false; });
    connectBackend();
}

TaskCardController::~TaskCardController() = default;

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
    connect(m_backend, &Backend::pinnedMessagesLoaded, this,
            [this](const QString &dialog, const QVector<Message> &messages) {
        if (dialog.isEmpty())
            return;
        if (m_card && dialog == m_card->taskChatDialogId())
            m_card->setPinnedMessages(messages);
    });
    connect(m_backend, &Backend::olderChatMessagesLoaded, this,
            [this](const QString &scope, const QString &before, bool task, const QVector<Message> &messages, bool more) {
        if (!task || !m_card || scope != m_card->taskId() || !m_cardSearchQuery.isEmpty())
            return;
        auto *history = static_cast<ui::HistoryList *>(m_card->messageList());
        if (history->addOlder(scope, before, messages, more)) {
            emit avatarRowsChanged();
            emit avatarsRequested();
        }
    });
    connect(m_backend, &Backend::chatReadConfirmed, this,
            [this](const QString &dialog, const QStringList &ids, qint64 readThrough) {
        if (dialog.isEmpty())
            return;
        if (m_card && m_card->taskChatDialogId() == dialog && m_cardSearchQuery.isEmpty() && m_cardContextMessageId.isEmpty())
            static_cast<ui::HistoryList *>(m_card->messageList())->confirmRead(ids, readThrough);
    });
    connect(m_backend, &Backend::chatHistoryRemoved, this,
            [this](const QString &dialog, const QStringList &ids, qint64 through) {
        if (dialog.isEmpty())
            return;
        const bool cleared = ids.isEmpty() && through == 0;
        if (m_card && dialog == m_card->taskChatDialogId()) {
            static_cast<ui::HistoryList *>(m_card->messageList())->removeHistory(ids, through);
            if (cleared && m_cardSearchQuery.isEmpty()) {
                m_cardContextMessageId.clear();
                m_card->setContextMode(false);
                m_card->setChatStatus(QStringLiteral("Сообщений пока нет"));
            }
        }
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

void TaskCardController::resetAccount()
{
    m_pendingCreateTask.clear();
    m_pendingCreateSubtask.clear();
    m_taskSendController.reset();
    if (m_descriptionUploadCard)
        m_descriptionUploadCard->descriptionEditor()->setUploadStatus({});
    clearDescriptionUpload();
    if (m_taskForm)
        m_taskForm->done(QDialog::Rejected);
    m_pendingTaskEdit.clear();
    m_pendingTaskEditCard.clear();
    m_pendingTaskEditProject.clear();
    m_pendingTaskEditTargetProject.clear();
    m_pendingTaskDelete.clear();
    m_pendingTaskDeleteCard.clear();
    m_pendingTaskDeleteProject.clear();
    if (m_card)
        m_card->done(QDialog::Rejected);
    m_taskMessageSearch.reset();
    m_taskDrafts.clear();
}

void TaskCardController::refresh()
{
    if (m_card) {
        m_backend->requestTask(m_card->taskId());
        if (!m_card->relations()->checklistPending())
            m_backend->requestTaskChecklist(m_card->taskId());
        m_backend->requestSubtasks(m_card->taskId());
        m_backend->requestTaskDependencies(m_card->taskId());
    }
}

bool TaskCardController::clearTask()
{
    return !m_card || m_card->close();
}

bool TaskCardController::confirmDiscard()
{
    return !m_card || m_card->confirmDiscardIfNeeded(false);
}

void TaskCardController::refreshAvatars()
{
    if (m_card) {
        m_card->refreshRequisiteAvatars();
        m_card->composer()->refreshMentionAvatars();
    }
    if (m_taskForm)
        m_taskForm->refreshRequisiteAvatars();
}

void TaskCardController::requestVisibleAvatars() const
{
    const auto *composer = m_card && m_card->isVisible() ? m_card->composer() : m_chatPanel->composer();
    for (const auto &url : composer->visibleMentionAvatarUrls())
        m_workspace.requestAvatar(url);
    if (m_card && m_card->isVisible()) {
        for (const auto &url : m_card->visibleRequisiteAvatarUrls())
            m_workspace.requestAvatar(url);
    }
    if (m_taskForm && m_taskForm->isVisible()) {
        for (const auto &url : m_taskForm->visibleRequisiteAvatarUrls())
            m_workspace.requestAvatar(url);
    }
}

QListWidget *TaskCardController::history() const
{
    return m_card ? m_card->messageList() : nullptr;
}

bool TaskCardController::activateChat(const NotificationTarget &target)
{
    if (!m_card || m_card->taskChatDialogId() != target.id)
        return false;
    m_card->activateWindow();
    if (!target.messageId.isEmpty())
        openPinnedTaskMessage(target.messageId);
    return true;
}

bool TaskCardController::showFileStatus(QWidget *parent, const QString &status, bool loading)
{
    if (!m_card || parent != m_card)
        return false;
    m_card->setStatus(status, loading);
    return true;
}

bool TaskCardController::showTaskStatus(const QString &id, const QString &status, bool loading)
{
    if (!m_card || m_card->taskId() != id)
        return false;
    m_card->setStatus(status, loading);
    return true;
}

bool TaskCardController::readActiveTask(const QWidget *active)
{
    if (!m_card || active != m_card || !m_cardDetailsReady || !m_cardSearchQuery.isEmpty() || !m_cardContextMessageId.isEmpty())
        return false;
    const auto seen = static_cast<ui::HistoryList *>(m_card->messageList())->visibleMessageIds();
    if (!seen.all.isEmpty())
        m_backend->readTaskChatMessages(m_card->taskId(), seen.incoming, seen.all);
    return true;
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

void TaskCardController::updateWriteState()
{
    if (m_card) {
        const bool deleting = !m_pendingTaskDelete.isEmpty();
        m_card->setWriteState(writable() && m_cardDetailsReady && !deleting, !m_pendingTaskEdit.isEmpty() || deleting,
                              m_projectsPanel->boardEditPending(),
                              m_taskSendController.pending() || m_workspace.upload.pastedImagePending(m_card->taskId(), true),
                              !m_workspace.upload.operation.isEmpty());
    }
    if (m_taskForm) {
        const auto task = m_taskForm->createdTask();
        const bool canWrite = writable() && m_pendingCreateTask.isEmpty() && m_pendingCreateSubtask.isEmpty()
            && (task.isEmpty() || m_workspace.upload.operation.isEmpty());
        m_taskForm->setEnabledForWrite(canWrite, !task.isEmpty()
            && m_workspace.upload.operation == operationKey(OperationKind::AttachTask, task));
    }
}

void TaskCardController::onTaskSummaryLoaded(const Task &task)
{
    if (!m_card || !m_cardSummaryPending || m_cardDetailsReady || task.id != m_card->taskId())
        return;
    m_cardSummaryPending = false;
    auto summary = task;
    summary.editRights = {};
    summary.statusActions.clear();
    const auto previousDialog = m_card->taskChatDialogId();
    m_card->setTask(summary, false);
    m_card->setStatus({});
    if (!summary.projectId.isEmpty())
        m_card->setLink(ui::taskLink(m_backend->portalOrigin(), summary.projectId, summary.id));
    updateTaskCardChatBinding(previousDialog);
    emit avatarsRequested();
    const auto &dialog = summary.chatDialogId;
    if (m_loadCardChat && dialog.startsWith(QLatin1String("chat"))
        && !portalTaskIdentifier(dialog.mid(4)).isEmpty()) {
        m_loadCardChat = false;
        m_backend->requestTaskMessages(summary.id);
    }
}

void TaskCardController::updateTaskCardChatBinding(const QString &previousDialog)
{
    if (!m_card || m_card->taskChatDialogId() == previousDialog)
        return;
    const auto dialog = m_card->taskChatDialogId();
    m_card->composer()->dismissMentionSearch();
    m_card->setPinnedMessages({});
    m_cardContextMessageId.clear();
    m_card->setContextMode(false);
    if (!dialog.isEmpty())
        m_backend->requestPinnedMessages(dialog);
}

void TaskCardController::openTaskCardForProject(const QString &taskId, const QString &projectId)
{
    if (taskId.isEmpty())
        return;
    const bool knownProject = !projectId.isEmpty() && projectId == m_projectsPanel->currentProjectId();
    if (m_card) {
        if (m_card->taskId() == taskId && m_card->projectId() == projectId) {
            m_card->activateWindow();
            return;
        }
        if (!clearTask())
            return;
    }

    const Task task = m_projectsPanel->cachedTask(taskId, projectId);
    auto *card = new ui::TaskCardDialog(task, projectId, m_window, false);
    card->setRequisiteAvatars([this](const QUrl &url) { return m_workspace.avatar(url); });
    card->setOnRequisiteAvatarChoicesChanged([this] { emit avatarsRequested(); });
    m_messagePreviews->resetLinkPreviews();
    m_workspace.bindMessageFiles(card->messageList(), true);
    static_cast<workspace::RowPainter *>(card->messageList()->itemDelegate())->setMessageViewersHandler(
        [this, card](const QString &id, const QPoint &anchor) { openTaskMessageViewers(card->messageList(), id, anchor); });
    auto *history = static_cast<ui::HistoryList *>(card->messageList());
    history->setOnViewportChanged([this] { m_chatPanel->scheduleVisibleChatRead(); });
    history->setOnOlder([this, card](const QString &before) {
        if (m_card == card && m_cardSearchQuery.isEmpty())
            m_backend->requestOlderChatMessages(card->taskId(), before, true);
    });
    history->setReplyActions([card](const ui::ChatReply &reply) {
        card->composer()->setReply(reply);
        card->messageInput()->setFocus(Qt::OtherFocusReason);
    }, [this, card] { return m_card == card && m_cardDetailsReady && writable(); },
       [this](const QString &id) { openPinnedTaskMessage(id); });
    card->composer()->setTypingActions([this, card] {
        return m_card == card && m_cardDetailsReady && writable() && card->isActiveWindow()
            ? card->taskChatDialogId() : QString();
    }, [this](const QString &dialog) { m_backend->notifyChatTyping(dialog); });
    card->composer()->setEditLatest([this, card, history] {
        return m_card == card && m_cardDetailsReady && m_chatPanel->editLatestMessage(history, card->taskChatDialogId());
    });
    history->setMessageMenu([this, card](QMenu &menu, const QPersistentModelIndex &index, const QString &href) {
        if (m_card == card && m_cardDetailsReady)
            m_chatPanel->addMessageActions(menu, index, card->taskChatDialogId(), true, href);
    });
    history->setReactionActions([this, card](const QModelIndex &index, const QString &code, const QPoint &anchor) {
        if (m_card == card && m_cardDetailsReady)
            m_chatPanel->actOnMessageReaction(index, card->taskChatDialogId(), code, anchor, card);
    }, [this, card] {
        return m_card == card && m_cardDetailsReady && writable() && !m_backend->isDemo() && !card->taskChatDialogId().isEmpty();
    });
    m_workspace.bindMessageFiles(card->descriptionList(), false);
    m_workspace.bindMessageFiles(card->attachmentList(), false);
    card->descriptionList()->setOnLayout([this] { emit avatarsRequested(); });
    card->attachmentList()->setOnLayout([this] { emit avatarsRequested(); });
    connect(card->detailsScrollBar(), &QScrollBar::valueChanged, this, &TaskCardController::avatarsRequested);
    connect(card->messageList()->verticalScrollBar(), &QScrollBar::valueChanged, this, &TaskCardController::avatarsRequested);
    card->setAttribute(Qt::WA_DeleteOnClose);
    m_card = card;
    card->composer()->setMentionSearchHandler([this, card](const QString &id, const QString &query) {
        const auto scope = card->taskChatDialogId();
        if (!scope.isEmpty())
            m_backend->searchPeople(id, query, scope);
        else
            card->composer()->setMentionResults(id, {});
    });
    card->composer()->setMentionAvatars([this](const Person &person) {
        return ui::requisiteAvatarIcon([this](const QUrl &url) { return m_workspace.avatar(url); },
                                       person.name, person.avatarUrl, workspace::AvatarKind::Person);
    }, [this] { emit avatarsRequested(); });
    card->setPeopleSearch([this](const QString &id, const QString &query) {
        m_backend->searchPeople(id, query, {});
    });
    card->setProjectChoices(m_projectsPanel->projectDirectory());
    card->setCanDetachProject(!m_backend->isDemo());
    card->setMoreProjectsAvailable(m_projectsPanel->moreAvailable());
    card->setOnMoreProjects([this] { m_projectsPanel->requestNextProjectsPage(); });
    m_loadCardChat = true;
    m_cardDetailsReady = false;
    m_cardSummaryPending = true;
    m_cardRequestedProjectDirectory = false;
    m_cardRequestedStages.clear();
    m_cardSearchQuery.clear();
    m_cardContextMessageId.clear();
    switchDraft(card->composer(), m_taskDrafts, {}, taskId);
    if (knownProject && card->projectId() == m_projectsPanel->currentProjectId()) {
        card->setStages(m_projectsPanel->stages());
        card->setProjectCaption(m_projectsPanel->currentProjectTitle());
    }
    card->setLink(ui::taskLink(m_backend->portalOrigin(), projectId, taskId));
    card->setOnCopy([](const QString &text) {
        QGuiApplication::clipboard()->setText(text);
    });
    const auto saveTaskEdit = [this, card](const TaskEdit &edit) {
        if (m_card != card || !m_cardDetailsReady || !writable()
            || !m_pendingTaskEdit.isEmpty() || !m_pendingTaskDelete.isEmpty())
            return;
        if (!edit.hasChanges()) {
            card->setStatus(QStringLiteral("Нет изменений"));
            return;
        }
        m_pendingTaskEdit = card->taskId();
        m_pendingTaskEditCard = card;
        m_pendingTaskEditProject = card->projectId();
        m_pendingTaskEditTargetProject = edit.projectId.value_or(QString());
        card->setStatus(QStringLiteral("Сохраняем…"), true);
        emit writeStateChanged();
        m_backend->updateTask(card->taskId(), edit);
    };
    card->setOnSave(saveTaskEdit);
    card->setOnDelete([this, card] { confirmTaskDeletion(card); });
    card->setOnMove([saveTaskEdit](const QString &stage) {
        TaskEdit edit;
        edit.stageId = stage;
        saveTaskEdit(edit);
    });
    card->setOnSend([this, card] {
        m_workspace.sendFromComposer(m_taskSendController, card->composer(), card->taskId());
    });
    card->setOnSearch([this, card](const QString &query) {
        if (query.isEmpty()) {
            if (m_cardSearchQuery.isEmpty())
                return;
            m_cardSearchQuery.clear();
            card->setChatStatus(QStringLiteral("Загружаем сообщения…"), true);
            m_backend->requestTaskMessages(card->taskId());
        } else {
            m_cardContextMessageId.clear();
            card->setContextMode(false);
            m_cardSearchQuery = query;
            card->setChatStatus(QStringLiteral("Ищем сообщения…"), true);
            m_taskMessageSearch.request(card->taskId(), query);
        }
    });
    card->relations()->onAdd = [this, card](const QString &group, const QString &title) {
        m_backend->addTaskChecklistItem(card->taskId(), group, title);
    };
    card->relations()->onToggle = [this, card](const QString &item, bool complete) {
        m_backend->setTaskChecklistComplete(card->taskId(), item, complete);
    };
    card->relations()->onOpenChild = [this](const QString &id) {
        openTaskCardForProject(id, {});
    };
    card->relations()->onCreateChild = [this, card] { createSubtask(card); };
    card->relations()->onHistory = [this, card] { m_backend->requestTaskHistory(card->taskId()); };
    card->setOnPinned([this](const QString &id) { openPinnedTaskMessage(id); });
    card->setOnLatest([this, card] {
        if (m_card != card)
            return;
        m_cardContextMessageId.clear();
        card->setContextMode(false);
        m_backend->requestTaskMessages(card->taskId());
    });
    card->setOnFile([this, card](bool attachment) {
        m_workspace.pickFile(card->taskId(), attachment ? QStringLiteral("task") : QStringLiteral("task-chat"));
    });
    card->descriptionEditor()->setAttachHandler([this, taskId](const QStringList &paths) {
        queueDescriptionFiles(taskId, paths);
    });
    card->descriptionEditor()->setImagePasteHandler([this, taskId](const QImage &image) {
        pasteDescriptionPhoto(taskId, image);
    });
    connect(card, &QDialog::finished, this, [this, card] {
        if (m_card != card)
            return;
        m_projectsPanel->cancelPendingMessageProject();
        switchDraft(card->composer(), m_taskDrafts, card->taskId(), {});
        m_card = nullptr;
        m_messagePreviews->resetLinkPreviews();
        emit historyClosed(true);
        m_cardSearchQuery.clear();
        m_cardContextMessageId.clear();
        m_loadCardChat = false;
        m_cardDetailsReady = false;
        m_cardSummaryPending = false;
    });
    emit writeStateChanged();
    if (writable())
        card->messageInput()->setFocus(Qt::OtherFocusReason);
    card->setStatus(QStringLiteral("Загружаем задачу…"), true);
    card->open();
    emit avatarsRequested();
    if (!card->taskChatDialogId().isEmpty())
        m_backend->requestPinnedMessages(card->taskChatDialogId());
    m_backend->requestTask(taskId);
    m_backend->requestTaskChecklist(taskId);
    m_backend->requestSubtasks(taskId);
    m_backend->requestTaskDependencies(taskId);
}

void TaskCardController::confirmTaskDeletion(ui::TaskCardDialog *card)
{
    if (!card || m_card != card || !writable() || !card->canDeleteTask()
        || !m_pendingTaskEdit.isEmpty() || !m_pendingTaskDelete.isEmpty())
        return;
    const auto taskId = portalTaskIdentifier(card->taskId());
    if (taskId.isEmpty())
        return;
    auto *confirmation = new QMessageBox(QMessageBox::Warning, QStringLiteral("Удаление задачи"),
        QStringLiteral("Удалить задачу №%1?").arg(taskId), QMessageBox::Yes | QMessageBox::Cancel, card);
    confirmation->setObjectName(QStringLiteral("taskDeleteConfirmation"));
    confirmation->setAttribute(Qt::WA_DeleteOnClose);
    confirmation->setDefaultButton(QMessageBox::Cancel);
    confirmation->setEscapeButton(QMessageBox::Cancel);
    confirmation->button(QMessageBox::Yes)->setText(QStringLiteral("Удалить"));
    const auto scope = m_backend->settingsScope();
    connect(confirmation, &QMessageBox::finished, this,
            [this, target = QPointer<ui::TaskCardDialog>(card), taskId, scope](int result) {
        if (result != QMessageBox::Yes || !target || m_card != target || target->taskId() != taskId
            || scope != m_backend->settingsScope() || !writable() || !target->canDeleteTask()
            || !m_pendingTaskEdit.isEmpty() || !m_pendingTaskDelete.isEmpty())
            return;
        m_pendingTaskDelete = taskId;
        m_pendingTaskDeleteCard = target;
        m_pendingTaskDeleteProject = target->projectId();
        target->setStatus(QStringLiteral("Удаляем задачу…"), true);
        emit writeStateChanged();
        m_backend->deleteTask(taskId);
    });
    confirmation->open();
}

void TaskCardController::onTaskDeleted(const QString &taskId)
{
    if (m_pendingTaskDelete.isEmpty() || m_pendingTaskDelete != taskId)
        return;
    m_pendingTaskDelete.clear();
    const auto card = std::exchange(m_pendingTaskDeleteCard, {});
    const auto project = std::exchange(m_pendingTaskDeleteProject, {});
    m_messagePreviews->invalidateTaskLinkPreview(taskId);
    emit writeStateChanged();
    if (card && card == m_card && card->taskId() == taskId)
        card->close();
    if (!project.isEmpty())
        m_projectsPanel->refreshTasks(project);
    emit statusChanged(QStringLiteral("Задача удалена"));
}

void TaskCardController::onTaskUpdated(const QString &taskId)
{
    const bool matches = !m_pendingTaskEdit.isEmpty() && taskId == m_pendingTaskEdit;
    const auto editorCard = m_pendingTaskEditCard;
    const auto project = matches ? m_pendingTaskEditProject : QString();
    const auto targetProject = matches ? m_pendingTaskEditTargetProject : QString();
    if (matches) {
        m_pendingTaskEdit.clear();
        m_pendingTaskEditCard.clear();
        m_pendingTaskEditProject.clear();
        m_pendingTaskEditTargetProject.clear();
    }
    if (m_card && m_card->taskId() == taskId) {
        if (matches && editorCard == m_card) {
            m_cardDetailsReady = false;
            m_card->acceptEdit();
        }
        m_card->setStatus(QStringLiteral("Задача обновлена"));
        m_backend->requestTask(taskId);
    }
    if (!m_projectsPanel->currentProjectId().isEmpty())
        m_projectsPanel->refreshTasks(project, targetProject);
    emit writeStateChanged();
}

void TaskCardController::onTaskCreated(const QString &projectId, const QString &taskId)
{
    if (taskId.isEmpty())
        return;
    const bool requested = !m_pendingCreateTask.isEmpty() && projectId == m_pendingCreateTask;
    bool attaching = false;
    if (requested) {
        m_pendingCreateTask.clear();
        attaching = beginTaskFormFiles(taskId, projectId);
    }
    m_projectsPanel->refreshTasks(projectId);
    if (requested && !attaching && m_workspace.projectsActive())
        openTaskCardForProject(taskId, projectId);
    emit statusChanged(QStringLiteral("Задача создана"));
    emit writeStateChanged();
}

void TaskCardController::onMessagesFound(const QString &scopeId, const QString &query, bool task,
                                 const QVector<Message> &messages)
{
    if (!task || !m_taskMessageSearch.accept(scopeId, query) || !m_card ||
        m_card->taskId() != scopeId || m_cardSearchQuery.isEmpty())
        return;
    if (query != m_cardSearchQuery) {
        m_taskMessageSearch.request(scopeId, m_cardSearchQuery);
    } else if (query == m_card->searchQuery()) {
        m_card->showMessages(messages, ui::MessageView::Search);
        emit avatarRowsChanged();
        emit avatarsRequested();
    }
}

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

void TaskCardController::openPinnedTaskMessage(const QString &messageId)
{
    if (!m_card)
        return;
    const auto dialog = m_card->taskChatDialogId();
    const auto target = portalTaskIdentifier(messageId);
    if (dialog.isEmpty() || target.isEmpty())
        return;
    const bool searching = !m_cardSearchQuery.isEmpty();
    m_cardSearchQuery.clear();
    if (!searching && m_card->focusMessage(target))
        return;
    m_cardContextMessageId = target;
    m_card->setContextMode(true);
    m_card->setChatStatus(QStringLiteral("Загружаем сообщение…"), true);
    m_backend->requestMessageContext(dialog, target);
}

void TaskCardController::openTaskMessageViewers(QListWidget *history, const QString &messageId, const QPoint &anchor)
{
    if (!m_card || history != m_card->messageList())
        return;
    const auto scope = m_card->taskId();
    const auto sameHistory = [controller = QPointer<TaskCardController>(this), source = QPointer<QListWidget>(history), scope] {
        return controller && source && source->isVisible() && controller->m_card
            && source == controller->m_card->messageList() && scope == controller->m_card->taskId();
    };
    workspace::openMessageViewers(m_backend, history, messageId, anchor, scope, true, sameHistory, {
        [this](const QUrl &url) { return m_workspace.avatar(url); },
        [this](const QUrl &url) { m_workspace.requestAvatar(url); }
    });
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

void TaskCardController::bindTaskForm(ui::TaskEditForm *form, const QString &projectId, bool canDetachProject)
{
    form->setAttribute(Qt::WA_DeleteOnClose);
    m_taskForm = form;
    connect(form, &QDialog::finished, this, [this] { m_taskForm = nullptr; });
    form->setPeopleSearch([this](const QString &id, const QString &query) {
        m_backend->searchPeople(id, query, {});
    });
    form->setRequisiteAvatars([this](const QUrl &url) { return m_workspace.avatar(url); },
                              [this] { emit avatarsRequested(); });
    form->setOnMoreProjects([this] { m_projectsPanel->requestNextProjectsPage(); });
    form->setOnStages([this, form](const QString &project) {
        if (project == m_projectsPanel->currentProjectId() && !m_projectsPanel->stages().isEmpty())
            form->setStages(m_projectsPanel->stages());
        else
            m_backend->requestProjectStages(project);
    });
    form->configure(projectId, canDetachProject);
    form->setProjectChoices(m_projectsPanel->projectDirectory());
    form->setMoreProjectsAvailable(m_projectsPanel->moreAvailable());
    m_backend->requestCurrentPerson([controller = QPointer<TaskCardController>(this), guard = QPointer<ui::TaskEditForm>(form)](Person person) {
        if (controller && guard && controller->m_taskForm == guard)
            guard->setCurrentPerson(std::move(person));
    });
    if (!m_projectsPanel->directoryLoaded())
        m_backend->requestProjects();
}

void TaskCardController::createTaskInProject()
{
    if (!writable() || m_projectsPanel->currentProjectId().isEmpty() || m_taskForm || !m_pendingCreateTask.isEmpty() || !m_pendingCreateSubtask.isEmpty())
        return;
    auto *form = new ui::TaskEditForm(m_window);
    bindTaskForm(form, m_projectsPanel->currentProjectId(), false);
    form->setOnSave([this, form](const TaskEdit &edit) {
        if (!writable() || !m_pendingCreateTask.isEmpty())
            return;
        if (!form->createdTask().isEmpty()) {
            uploadNextTaskFormFile();
            return;
        }
        const auto project = form->projectId();
        m_pendingCreateTask = project;
        form->setStatus(QStringLiteral("Создаём задачу…"), true);
        emit writeStateChanged();
        m_backend->createTask(project, edit);
    });
    form->open();
}

void TaskCardController::createSubtask(ui::TaskCardDialog *card)
{
    if (card != m_card || !writable() || m_taskForm || !m_pendingCreateTask.isEmpty() || !m_pendingCreateSubtask.isEmpty())
        return;
    auto *form = new ui::TaskEditForm(card);
    form->setWindowTitle(QStringLiteral("Новая подзадача"));
    form->setAccessibleName(QStringLiteral("Форма новой подзадачи"));
    const auto parent = card->taskId();
    bindTaskForm(form, card->projectId(), true);
    form->setOnSave([this, form, parent](const TaskEdit &edit) {
        if (!writable() || !m_pendingCreateSubtask.isEmpty())
            return;
        if (!form->createdTask().isEmpty()) {
            uploadNextTaskFormFile();
            return;
        }
        const auto project = form->projectId();
        m_pendingCreateSubtask = parent;
        form->setStatus(QStringLiteral("Создаём подзадачу…"), true);
        emit writeStateChanged();
        m_backend->createSubtask(parent, project, edit);
    });
    form->open();
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
