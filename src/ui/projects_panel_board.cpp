#include "ui/projects_panel.h"

#include "ui/board_widget.h"
#include "ui/chat_widgets.h"
#include "ui/project_tools.h"
#include "ui/task_dialogs.h"
#include "ui/task_gantt.h"
#include "ui/workspace_directory.h"
#include "ui/workspace_presentation.h"

#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
#include <algorithm>

namespace bx {

void ProjectsPanel::onBoardUpdated(const QString &projectId)
{
    bool refreshSearch = false;
    if (!m_pendingBoardEdit.isEmpty() && projectId == m_pendingBoardEdit) {
        if (m_stageManager && m_stageManager->projectId() == projectId)
            m_stageManager->setStatus(QStringLiteral("Этапы обновлены"));
        const auto movedTask = m_pendingMoveTask;
        m_pendingBoardEdit.clear();
        m_pendingMoveTask.clear();
        if (!movedTask.isEmpty() && projectId == m_currentProjectId) {
            m_boardMoveRefreshProject = projectId;
            refreshSearch = m_projectSearchDisplayed;
            emit statusChanged(QStringLiteral("Задача перенесена"));
        }
        if (!movedTask.isEmpty())
            emit taskMoved(movedTask);
    }
    if (projectId == m_currentProjectId)
        requestCurrentProjectBoard();
    if (refreshSearch)
        runProjectTaskSearch();
    emit writeStateChanged();
}

bool ProjectsPanel::canMoveBoardTask(const QString &taskId, const QString &sourceStage, const QString &targetStage) const
{
    if (!m_workspace.writable() || !m_active || m_ganttMode || m_board->view() != BoardWidget::View::Kanban
        || m_currentProjectId.isEmpty() || m_currentProjectId == m_boardMoveRefreshProject
        || !m_pendingBoardEdit.isEmpty() || taskId.isEmpty() || sourceStage == targetStage
        || targetStage == QLatin1String("0"))
        return false;

    const auto boardStage = [this](const QString &id) {
        const bool demoId = m_backend->isDemo() && id.startsWith(QLatin1String("s-"));
        const auto number = demoId ? id.mid(2) : id;
        bool parsed = false;
        const auto value = number.toLongLong(&parsed);
        return parsed && value >= 0 && number == QString::number(value)
            ? std::find_if(m_stages.cbegin(), m_stages.cend(), [&id](const Stage &column) { return column.id == id; })
            : m_stages.cend();
    };
    const auto source = boardStage(sourceStage);
    if (source == m_stages.cend() || (!targetStage.isEmpty() && boardStage(targetStage) == m_stages.cend()))
        return false;
    return std::any_of(source->tasks.cbegin(), source->tasks.cend(), [this, &taskId](const Task &task) {
        return task.id == taskId && (task.projectId.isEmpty() || task.projectId == m_currentProjectId);
    });
}

void ProjectsPanel::openProjectTools()
{
    if (m_currentProjectId.isEmpty())
        return;
    if (m_projectTools) {
        m_projectTools->activateWindow();
        return;
    }
    const auto project = m_currentProjectId;
    auto *tools = new ui::ProjectToolsDialog(project, m_projectTitle->text(), this);
    m_projectTools = tools;
    tools->onMembers = [this, project] { m_backend->requestProjectMembers(project); };
    tools->onFiles = [this, project](const QString &folder, int offset) {
        m_backend->requestProjectFiles(project, folder, offset);
    };
    tools->onDownload = [this, tools](const MessageFile &file) { emit fileSaveRequested(file, tools); };
    connect(tools, &QDialog::finished, this, [this, tools] {
        if (m_projectTools == tools)
            m_projectTools.clear();
    });
    tools->open();
    tools->reload();
}

void ProjectsPanel::showProjectTimeline(bool show)
{
    if (m_ganttMode == show)
        return;
    m_ganttMode = show;
    m_projectTimeline->setVisible(show);
    m_board->setVisible(!show);
    m_stagesButton->setVisible(!show);
    m_filter->setToolTip(show ? QStringLiteral("Поиск по загруженным задачам диаграммы") : QString());
    if (show) {
        m_projectSearchTimer->stop();
        ui::setStatusLabel(m_projectTaskSearchStatus, {});
        m_projectTimeline->clear(m_currentProjectId.isEmpty() ? QStringLiteral("Выберите проект")
                                                             : QStringLiteral("Загружаем плановые даты…"),
                                 !m_currentProjectId.isEmpty());
        if (!m_currentProjectId.isEmpty())
            m_backend->requestProjectTimeline(m_currentProjectId);
    } else if (!m_currentProjectId.isEmpty()) {
        m_board->setFilter(m_filter->text());
        if (!m_filter->text().trimmed().isEmpty() && !m_projectSearchDisplayed) {
            m_board->setStages(m_stages);
            ui::setStatusLabel(m_projectTaskSearchStatus, QStringLiteral("Ищем задачи в проекте…"), true);
            m_projectSearchTimer->start();
        }
        requestCurrentProjectBoard();
    }
}

void ProjectsPanel::requestCurrentProjectBoard()
{
    m_board->resetStagePageLoads(false);
    m_backend->requestProjectBoard(m_currentProjectId);
}

void ProjectsPanel::openStageManager()
{
    if (!m_workspace.writable() || m_currentProjectId.isEmpty() || m_stageManager || !m_pendingBoardEdit.isEmpty())
        return;
    auto *manager = new ui::StageManagerDialog(m_currentProjectId, this);
    manager->setAttribute(Qt::WA_DeleteOnClose);
    m_stageManager = manager;
    manager->setStages(m_stages);
    manager->setOnSave([this, manager](const QString &stage, const QString &title, const QString &after,
                                      const QString &color) {
        if (!m_workspace.writable() || !m_pendingBoardEdit.isEmpty())
            return;
        m_pendingBoardEdit = manager->projectId();
        manager->setStatus(QStringLiteral("Сохраняем этап…"), true);
        emit writeStateChanged();
        m_backend->saveStage(manager->projectId(), stage, title, after, color);
    });
    manager->setOnDelete([this, manager](const QString &stage) {
        if (!m_workspace.writable() || !m_pendingBoardEdit.isEmpty())
            return;
        const auto answer = QMessageBox::question(manager, QStringLiteral("Удалить этап"),
            QStringLiteral("Удалить пустой этап?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes || !m_workspace.writable())
            return;
        m_pendingBoardEdit = manager->projectId();
        manager->setStatus(QStringLiteral("Удаляем этап…"), true);
        emit writeStateChanged();
        m_backend->deleteStage(manager->projectId(), stage);
    });
    connect(manager, &QDialog::finished, this, [this] { m_stageManager = nullptr; });
    manager->open();
}

void ProjectsPanel::updateColumnWidthContext(const QString &scope)
{
    m_board->setColumnWidthContext(m_currentProjectId, chatPreferenceKey(scope, QLatin1String("kanbanColumnWidths")));
}

void ProjectsPanel::refreshBoardAvatars()
{
    m_board->refreshAvatars();
}

void ProjectsPanel::closeTools()
{
    if (m_projectTools)
        m_projectTools->reject();
}

void ProjectsPanel::setWriteAvailability(bool canWrite, bool creatingTask)
{
    m_newTaskButton->setEnabled(canWrite && !m_currentProjectId.isEmpty() && !creatingTask);
    m_projectToolsButton->setEnabled(!m_currentProjectId.isEmpty());
    m_newProjectButton->setEnabled(canWrite && !m_pendingCreateProject);
    m_stagesButton->setEnabled(canWrite && !m_currentProjectId.isEmpty() && m_pendingBoardEdit.isEmpty());
    if (m_stageManager)
        m_stageManager->setWritable(canWrite && m_pendingBoardEdit != m_stageManager->projectId());
    if (m_projectForm)
        m_projectForm->setWritable(canWrite && !m_pendingCreateProject);
}

Task ProjectsPanel::cachedTask(const QString &taskId, const QString &projectId) const
{
    const bool knownProject = !projectId.isEmpty() && projectId == m_currentProjectId;
    Task task;
    task.id = taskId;
    for (const auto &stage : knownProject ? m_stages : QVector<Stage>()) {
        for (const auto &candidate : stage.tasks) {
            if (candidate.id == taskId) {
                task = candidate;
                break;
            }
        }
    }
    if (task.name.isEmpty() && knownProject) {
        for (const auto &candidate : m_projectSearchResults) {
            if (candidate.id == taskId) {
                task = candidate;
                break;
            }
        }
    }
    return task;
}

void ProjectsPanel::refreshTasks(const QString &project, const QString &targetProject)
{
    if ((project != m_currentProjectId && (targetProject.isEmpty() || targetProject != m_currentProjectId))
        || refreshProjectTaskChats(m_currentProjectId))
        return;
    if (m_ganttMode)
        m_backend->requestProjectTimeline(m_currentProjectId);
    else
        requestCurrentProjectBoard();
}

void ProjectsPanel::requestVisibleAvatars(const std::function<void(const QUrl &)> &requestAvatar, const std::function<void(QListWidget *)> &requestVisible) const
{
    requestAvatar(m_projectAvatarUrl);
    requestVisible(m_projects);
    if (m_projectTabs->currentIndex() == 1) {
        requestVisible(m_projectTaskChats);
    } else {
        for (const auto &url : m_board->visibleAvatarUrls())
            requestAvatar(url);
    }
}

void ProjectsPanel::updateAvatars(const std::function<void(QListWidget *)> &updateList)
{
    updateList(m_projects);
    updateList(m_projectTaskChats);
    if (const auto *photo = m_workspace.avatar(m_projectAvatarUrl))
        m_projectAvatar->setPhoto(*photo);
}

} // namespace bx
