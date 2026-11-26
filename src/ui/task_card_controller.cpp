#include "ui/task_card_controller.h"
#include "ui/task_dialogs.h"
#include "ui/chat_panel.h"
#include "ui/projects_panel.h"
#include "ui/chat_widgets.h"
#include "ui/workspace_message_previews.h"
#include "ui/workspace_presentation.h"
#include <QClipboard>
#include <QGuiApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QTemporaryDir>

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
    bindHistory(card);
    m_workspace.bindMessageFiles(card->descriptionList(), false);
    m_workspace.bindMessageFiles(card->attachmentList(), false);
    card->descriptionList()->setOnLayout([this] { emit avatarsRequested(); });
    card->attachmentList()->setOnLayout([this] { emit avatarsRequested(); });
    connect(card->detailsScrollBar(), &QScrollBar::valueChanged, this, &TaskCardController::avatarsRequested);
    card->setAttribute(Qt::WA_DeleteOnClose);
    m_card = card;
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

} // namespace bx
