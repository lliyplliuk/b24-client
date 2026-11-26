#include "ui/projects_panel.h"

#include "ui/board_widget.h"
#include "ui/chat_widgets.h"
#include "ui/task_dialog_controls.h"
#include "ui/task_gantt.h"
#include "ui/workspace_directory.h"
#include "ui/workspace_presentation.h"

#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
#include <algorithm>
#include <iterator>

namespace bx {

void ProjectsPanel::runProjectTaskSearch()
{
    m_projectSearchTimer->stop();
    const auto query = m_filter->text().trimmed();
    if (!m_active || m_projectTabs->currentIndex() != 0 || m_ganttMode
        || m_currentProjectId.isEmpty() || query.isEmpty())
        return;
    ui::setStatusLabel(m_projectTaskSearchStatus, QStringLiteral("Ищем задачи в проекте…"), true);
    if (m_pendingProjectSearches.contains(m_currentProjectId))
        return;
    m_pendingProjectSearches.insert(m_currentProjectId, query);
    m_backend->searchProjectTasks(m_currentProjectId, query);
}

void ProjectsPanel::onProjectTasksFound(const QString &projectId, const QString &query, const QVector<Task> &tasks)
{
    if (m_pendingProjectSearches.value(projectId) != query)
        return;
    m_pendingProjectSearches.remove(projectId);
    if (!m_active || m_ganttMode || projectId != m_currentProjectId)
        return;
    if (query != m_filter->text().trimmed()) {
        runProjectTaskSearch();
        return;
    }
    m_projectSearchResults = tasks;
    m_projectSearchDisplayed = true;
    ui::setStatusLabel(m_projectTaskSearchStatus, {});
    displayProjectTaskResults();
}

void ProjectsPanel::displayProjectTaskResults()
{
    auto displayed = m_stages;
    for (auto &stage : displayed)
        stage.tasks.clear();
    for (const auto &task : m_projectSearchResults) {
        auto stage = std::find_if(displayed.begin(), displayed.end(), [&task](const Stage &candidate) {
            return candidate.id == task.stageId;
        });
        if (stage == displayed.end()) {
            displayed.append({task.stageId,
                task.stageName.isEmpty() ? QStringLiteral("Без этапа") : task.stageName, {}, {}});
            stage = std::prev(displayed.end());
        }
        stage->tasks.append(task);
    }
    m_board->setStages(displayed, true);
}

void ProjectsPanel::onProjectScopeChanged(int scope)
{
    m_projectViews->setCurrentIndex(scope);
    if (scope == 1) {
        m_projectSearchTimer->stop();
        ui::setStatusLabel(m_projectTaskSearchStatus, {});
        openProjectTaskChats();
    } else {
        closeProjectTaskChats();
        ensureProjectTaskView();
        if (!m_projectSearchDisplayed)
            runProjectTaskSearch();
    }
    emit avatarsRequested();
}

void ProjectsPanel::ensureProjectTaskView()
{
    if (m_projectTaskViewRequested || m_currentProjectId.isEmpty() || !m_active)
        return;
    m_projectTaskViewRequested = true;
    if (m_ganttMode) {
        m_projectTimeline->clear(QStringLiteral("Загружаем плановые даты…"), true);
        m_backend->requestProjectTimeline(m_currentProjectId);
    } else {
        requestCurrentProjectBoard();
    }
}

void ProjectsPanel::openProjectTaskChats()
{
    if (!m_active || m_currentProjectId.isEmpty() || m_projectTaskChatScope == m_currentProjectId)
        return;
    closeProjectTaskChats();
    m_projectTaskChatScope = m_currentProjectId;
    requestProjectTaskChatPage();
}

void ProjectsPanel::closeProjectTaskChats()
{
    if (m_projectTaskChatScope.isEmpty())
        return;
    m_projectTaskChatScope.clear();
    m_projectTaskChatError.clear();
    m_projectTaskChatNext = 0;
    m_projectTaskChatPending = -1;
    m_projectTaskChatUnread = 0;
    m_projectTaskChatRows.clear();
    m_projectTaskChats->clear();
    updateProjectTaskChatControls();
    m_backend->requestProjectTaskChats({}, 0);
}

bool ProjectsPanel::refreshProjectTaskChats(const QString &project)
{
    if (project.isEmpty() || project != m_currentProjectId || !m_active || m_projectTabs->currentIndex() != 1)
        return false;
    m_projectTaskViewRequested = false;
    closeProjectTaskChats();
    openProjectTaskChats();
    return true;
}

void ProjectsPanel::requestProjectTaskChatPage()
{
    if (!m_active || m_projectTabs->currentIndex() != 1
        || m_projectTaskChatScope.isEmpty() || m_projectTaskChatScope != m_currentProjectId
        || m_projectTaskChatPending >= 0 || m_projectTaskChatNext < 0)
        return;
    m_projectTaskChatError.clear();
    m_projectTaskChatPending = m_projectTaskChatNext;
    updateProjectTaskChatControls();
    m_backend->requestProjectTaskChats(m_projectTaskChatScope, m_projectTaskChatPending);
}

void ProjectsPanel::onProjectTaskChatsLoaded(const QString &project, int offset, const QVector<Chat> &rows, int nextOffset)
{
    if (project.isEmpty() || project != m_currentProjectId || project != m_projectTaskChatScope
        || m_projectTaskChatPending < 0 || offset != m_projectTaskChatPending)
        return;
    m_projectTaskChatPending = -1;
    m_projectTaskChatNext = nextOffset > offset ? nextOffset : -1;
    for (const auto &chat : rows) {
        if (!chat.taskChat || chat.taskId.isEmpty() || chat.id.isEmpty())
            continue;
        const auto found = m_projectTaskChatRows.constFind(chat.id);
        if (found == m_projectTaskChatRows.cend()) {
            m_projectTaskChatRows.insert(chat.id, ui::addChatRow(m_projectTaskChats, chat));
            m_projectTaskChatUnread += qMax(0, chat.unread);
        }
    }
    updateProjectTaskChatBadges(project, rows);
    updateProjectTaskChatControls();
    emit avatarsRequested();
}

void ProjectsPanel::updateProjectTaskChatBadges(const QString &project, const QVector<Chat> &rows)
{
    if (project.isEmpty() || project != m_projectTaskChatScope || project != m_currentProjectId)
        return;
    const auto previous = m_projectTaskChatUnread;
    for (const auto &chat : rows) {
        auto *item = m_projectTaskChatRows.value(chat.id);
        if (item) {
            m_projectTaskChatUnread += qMax(0, chat.unread) - qMax(0, item->data(workspace::Count).toInt());
            ui::setChatRowBadges(item, chat);
        }
    }
    if (m_projectTaskChatUnread != previous)
        updateProjectTaskChatTab();
}

void ProjectsPanel::updateProjectTaskChatTab()
{
    const bool confirmed = m_projectUnreadCountsComplete;
    const auto aggregate = m_projectUnreadCounts.value(m_currentProjectId);
    const bool hasScopedPage = !m_currentProjectId.isEmpty() && m_projectTaskChatScope == m_currentProjectId;
    const auto scoped = hasScopedPage ? m_projectTaskChatUnread : qint64(0);
    const auto unread = confirmed ? aggregate : qMax(aggregate, scoped);
    const bool partial = !confirmed && (!hasScopedPage || m_projectTaskChatNext >= 0 || aggregate > scoped);
    const auto caption = unread <= 0 ? QStringLiteral("Чаты")
        : partial ? QStringLiteral("Чаты (%1+)").arg(unread) : QStringLiteral("Чаты (%1)").arg(unread);
    m_projectTabs->setTabText(1, caption);
    const auto scope = unread <= 0 ? QString()
        : confirmed ? QStringLiteral("Непрочитанных сообщений в чатах проекта: %1").arg(unread)
        : aggregate > scoped ? QStringLiteral("Непрочитанных сообщений в чатах проекта: не менее %1").arg(unread)
                             : QStringLiteral("Непрочитанных сообщений в загруженных чатах: %1").arg(unread);
    m_projectTabs->setTabToolTip(1, scope);
    m_projectTabs->setAccessibleTabName(1, scope.isEmpty() ? QStringLiteral("Чаты")
                                                        : QStringLiteral("Чаты. ") + scope);
}

void ProjectsPanel::updateProjectTaskChatControls()
{
    const bool loading = m_projectTaskChatPending >= 0;
    const bool empty = m_projectTaskChatRows.isEmpty();
    QString status = m_projectTaskChatError;
    if (empty && status.isEmpty()) {
        if (m_projectTaskChatScope.isEmpty())
            status = QStringLiteral("Выберите проект");
        else if (loading)
            status = QStringLiteral("Загружаем чаты задач…");
        else if (m_projectTaskChatNext > 0)
            status = QStringLiteral("В загруженных задачах нет чатов");
        else
            status = QStringLiteral("В этом проекте пока нет чатов задач");
    }
    ui::setStatusLabel(m_projectTaskChatStatus, status, empty && loading);
    m_projectTaskChats->setVisible(!empty);
    m_moreProjectTaskChats->setVisible(!m_projectTaskChatError.isEmpty() || m_projectTaskChatNext > 0);
    m_moreProjectTaskChats->setEnabled(!loading);
    m_moreProjectTaskChats->setText(loading ? QStringLiteral("Загружаем…")
        : m_projectTaskChatError.isEmpty() ? QStringLiteral("Ещё чаты") : QStringLiteral("Повторить"));
    updateProjectTaskChatTab();
}

void ProjectsPanel::resetTaskChats()
{
    closeProjectTaskChats();
    if (m_projectUnreadCountsComplete || !m_projectUnreadCounts.isEmpty()) {
        m_projectUnreadCounts.clear();
        m_projectUnreadCountsComplete = false;
        updateProjectUnreadRows();
    }
}

void ProjectsPanel::resumeSearch()
{
    if (!m_projectSearchDisplayed)
        runProjectTaskSearch();
}

} // namespace bx
