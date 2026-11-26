#include "ui/projects_panel.h"

#include "ui/board_widget.h"
#include "ui/chat_widgets.h"
#include "ui/project_tools.h"
#include "ui/task_dialogs.h"
#include "ui/task_gantt.h"
#include "ui/workspace_directory.h"
#include "ui/workspace_presentation.h"

#include <QDesktopServices>
#include <QKeyEvent>
#include <QLayout>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>

namespace bx {

void ProjectsPanel::onProjectSelected()
{
    auto *item = m_projects->currentItem();
    if (!item)
        return;
    const auto projectId = item->data(workspace::Id).toString();
    if (projectId.isEmpty())
        return;
    if (projectId != m_currentProjectId && !m_workspace.clearTask()) {
        const QSignalBlocker blocker(m_projects);
        for (int i = 0; i < m_projects->count(); ++i) {
            if (m_projects->item(i)->data(workspace::Id).toString() == m_currentProjectId) {
                m_projects->setCurrentRow(i);
                return;
            }
        }
        m_projects->setCurrentItem(nullptr);
        return;
    }
    const auto name = item->data(workspace::Title).toString();
    m_projectTitle->setText(name);
    m_projectTitle->setToolTip(ui::plainTooltip(name));
    m_projectAvatar->setTitle(name, workspace::AvatarKind::Project,
                             item->data(workspace::ProjectAvatarColor).value<QColor>(),
                             item->data(workspace::ProjectAvatarVariant).toString(),
                             item->data(workspace::ProjectAvatarType).toString());
    m_projectAvatarUrl = item->data(workspace::AvatarUrl).toUrl();
    if (const auto *photo = m_workspace.avatar(m_projectAvatarUrl))
        m_projectAvatar->setPhoto(*photo);
    emit avatarsRequested();
    if (projectId == m_currentProjectId)
        return;
    if (m_projectTools)
        m_projectTools->reject();
    closeProjectTaskChats();
    m_currentProjectId = projectId;
    updateProjectTaskChatTab();
    m_projectTaskViewRequested = false;
    m_stages.clear();
    m_loadedBoardTaskIds.clear();
    m_projectSearchDisplayed = false;
    m_projectSearchResults.clear();
    m_filter->clear();
    emit writeStateChanged();
    emit statusChanged(QString());
    m_board->clearBoard(QStringLiteral("Загружаем доску…"), !m_ganttMode);
    updateColumnWidthContext(m_backend->settingsScope());
    if (m_projectTabs->currentIndex() == 1)
        openProjectTaskChats();
    else
        ensureProjectTaskView();
}

void ProjectsPanel::onProjectCreated(const QString &projectId)
{
    if (m_pendingCreateProject) {
        m_pendingCreateProject = false;
        if (m_projectForm)
            m_projectForm->accept();
        m_pendingProjectId = projectId;
    }
    m_backend->requestProjects();
    emit statusChanged(QStringLiteral("Проект создан"));
    emit writeStateChanged();
}

void ProjectsPanel::updateProjectUnreadRows()
{
    if (!m_projects || !m_projectTabs)
        return;
    for (int row = 0; row < m_projects->count(); ++row) {
        auto *item = m_projects->item(row);
        const auto id = item->data(workspace::Id).toString();
        if (!id.isEmpty())
            ui::setProjectRowUnreadCount(item, m_projectUnreadCounts.value(id), !m_projectUnreadCountsComplete);
    }
    updateProjectTaskChatTab();
}

void ProjectsPanel::scheduleProjectsPageCheck()
{
    if (m_projectsPageCheckQueued || !m_projectsMoreAvailable || m_projectsPagePending || m_projectsPageFailed)
        return;
    m_projectsPageCheckQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_projectsPageCheckQueued = false;
        maybeLoadMoreProjects();
    });
}

void ProjectsPanel::maybeLoadMoreProjects()
{
    if (!m_active || !m_projects->isVisible() || m_projectsPageFailed)
        return;
    const auto *bar = m_projects->verticalScrollBar();
    const auto span = qint64(bar->maximum()) - bar->minimum();
    if (span > 0 && 3 * (qint64(bar->value()) - bar->minimum()) >= 2 * span)
        requestNextProjectsPage();
}

void ProjectsPanel::requestNextProjectsPage()
{
    if (!m_projectDirectoryLoaded || !m_projectsMoreAvailable || m_projectsPagePending)
        return;
    m_projectsPagePending = true;
    updateProjectsRailPagination();
    m_backend->requestMoreProjects();
}

void ProjectsPanel::updateProjectsRailPagination()
{
    const auto *bar = m_projects->verticalScrollBar();
    // Allow for the button's own height when deciding whether the list can scroll without it.
    const auto buttonSpace = m_moreProjectsButton->isHidden() ? 0
        : m_moreProjectsButton->height() + m_projects->parentWidget()->layout()->spacing();
    const bool shortList = bar->maximum() - bar->minimum() <= buttonSpace;
    m_moreProjectsButton->setVisible(m_projectsMoreAvailable
        && (m_projectsPageFailed || shortList || !m_projectNameQuery.isEmpty() || !m_linkedProjectId.isEmpty()));
    m_moreProjectsButton->setEnabled(m_projectsMoreAvailable && !m_projectsPagePending);
    m_moreProjectsButton->setText(m_projectsPagePending ? QStringLiteral("Загружаем…")
        : m_projectsPageFailed ? QStringLiteral("Повторить загрузку проектов") : QStringLiteral("Ещё проекты"));
}

void ProjectsPanel::showProjects()
{
    auto ordered = m_projectDirectory;
    if (!m_projectNameQuery.isEmpty()) {
        // Поиск по названию идёт по загруженному каталогу: пагинация и подгрузка сохраняются.
        ordered.erase(std::remove_if(ordered.begin(), ordered.end(), [this](const Project &project) {
            return !project.name.contains(m_projectNameQuery, Qt::CaseInsensitive);
        }), ordered.end());
    }
    std::stable_partition(ordered.begin(), ordered.end(), [](const Project &project) { return project.pinned; });
    {
        const QSignalBlocker blocker(m_projects);
        m_projects->clear();
        QListWidgetItem *selected = nullptr;
        QListWidgetItem *linked = nullptr;
        for (const auto &project : ordered) {
            auto *item = workspace::addRow(m_projects, project.name, project.group, {}, project.id);
            item->setData(workspace::AvatarType, int(workspace::AvatarKind::Project));
            item->setData(workspace::AvatarUrl, project.avatarUrl);
            item->setData(workspace::ProjectAvatarColor, QColor(project.avatarColor));
            item->setData(workspace::ProjectAvatarVariant, project.avatarVariant);
            item->setData(workspace::ProjectAvatarType, project.avatarType);
            item->setData(workspace::Pinned, project.pinned);
            ui::setProjectRowUnreadCount(item, m_projectUnreadCounts.value(project.id), !m_projectUnreadCountsComplete);
            if (project.id == m_currentProjectId)
                selected = item;
            if (project.id == m_linkedProjectId)
                linked = item;
        }
        if (linked) {
            selected = linked;
            m_linkedProjectId.clear();
        }
        for (int i = 0; i < m_projects->count() && !m_pendingProjectId.isEmpty(); ++i) {
            auto *item = m_projects->item(i);
            if (item->data(workspace::Id).toString() == m_pendingProjectId) {
                selected = item;
                m_pendingProjectId.clear();
            }
        }
        if (m_projectNameQuery.isEmpty())
            selectAvailable(m_projects, selected);
        else if (selected)
            m_projects->setCurrentItem(selected);
        if (ordered.isEmpty()) {
            const auto noMatches = !m_projectsMoreAvailable ? QStringLiteral("Проектов по запросу нет")
                : QStringLiteral("В загруженных проектах совпадений нет");
            workspace::emptyRow(m_projects, m_projectNameQuery.isEmpty()
                ? QStringLiteral("Проектов пока нет") : noMatches);
        }
    }
    updateProjectsRailPagination();
    if (ordered.isEmpty() && m_projectNameQuery.isEmpty()) {
        if (!m_workspace.clearTask())
            return;
        closeProjectTaskChats();
        m_currentProjectId.clear();
        updateProjectTaskChatTab();
        m_projectTaskViewRequested = false;
        m_projectTitle->setText(QStringLiteral("Проекты"));
        m_projectAvatar->setTitle({});
        m_projectAvatarUrl.clear();
        m_board->clearBoard(QStringLiteral("Доски появятся здесь, когда у вас будут проекты"));
        m_projectTimeline->clear(QStringLiteral("Выберите проект"));
        emit writeStateChanged();
    } else {
        onProjectSelected();
    }
    emit avatarRowsChanged();
    emit avatarsRequested();
}

void ProjectsPanel::createProject()
{
    if (!m_workspace.writable() || m_projectForm || m_pendingCreateProject)
        return;
    auto *form = new ui::NameDialog(this);
    form->setAttribute(Qt::WA_DeleteOnClose);
    m_projectForm = form;
    form->setOnCreate([this, form](const QString &name) {
        if (!m_workspace.writable() || m_pendingCreateProject)
            return;
        m_pendingCreateProject = true;
        form->setStatus(QStringLiteral("Создаём проект…"), true);
        emit writeStateChanged();
        m_backend->createProject(name);
    });
    connect(form, &QDialog::finished, this, [this] { m_projectForm = nullptr; });
    form->open();
}

void ProjectsPanel::refresh(bool reloadProjectDirectory)
{
    const auto previousProject = m_currentProjectId;
    const bool lookingUpNotice = !m_linkedProjectId.isEmpty();
    const bool noticeNeedsRefresh = lookingUpNotice && !m_projectsMoreAvailable &&
        std::none_of(m_projectDirectory.cbegin(), m_projectDirectory.cend(),
                     [this](const Project &project) { return project.id == m_linkedProjectId; });
    if (reloadProjectDirectory || !m_projectDirectoryLoaded || noticeNeedsRefresh) {
        if (!m_projectDirectoryLoaded) {
            m_projects->clear();
            workspace::loadingRow(m_projects, QStringLiteral("Загружаем проекты…"));
        }
        m_backend->requestProjects();
    } else {
        const auto scroll = m_projects->verticalScrollBar()->value();
        showProjects();
        if (!lookingUpNotice)
            m_projects->verticalScrollBar()->setValue(scroll);
    }
    if (!m_currentProjectId.isEmpty() && m_currentProjectId == previousProject) {
        if (m_projectTabs->currentIndex() == 1) {
            if (reloadProjectDirectory)
                refreshProjectTaskChats(m_currentProjectId);
            else
                openProjectTaskChats();
        } else if (m_ganttMode) {
            m_projectTaskViewRequested = true;
            m_backend->requestProjectTimeline(m_currentProjectId);
        } else {
            m_projectTaskViewRequested = true;
            requestCurrentProjectBoard();
        }
    }
}

bool ProjectsPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (m_projects && watched == m_projects->viewport() && event->type() == QEvent::Wheel) {
        const auto *wheel = static_cast<QWheelEvent *>(event);
        const auto delta = wheel->pixelDelta().isNull() ? wheel->angleDelta() : wheel->pixelDelta();
        if (delta.y() < 0 && -qint64(delta.y()) >= qAbs(qint64(delta.x())))
            scheduleProjectsPageCheck();
    } else if (watched == m_projects && event->type() == QEvent::KeyPress) {
        const auto key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Down || key == Qt::Key_PageDown || key == Qt::Key_End)
            scheduleProjectsPageCheck();
    }
    return QWidget::eventFilter(watched, event);
}

QString ProjectsPanel::currentProjectTitle() const
{
    return m_projectTitle->text();
}

void ProjectsPanel::openLinkedProject(const NotificationTarget &target, const QUrl &messageUrl)
{
    if (!messageUrl.isEmpty() && std::none_of(m_projectDirectory.cbegin(), m_projectDirectory.cend(),
            [&target](const Project &project) { return project.id == target.id; })) {
        if (m_projectDirectoryLoaded && (!m_projectsMoreAvailable || m_projectsPageFailed)) {
            QDesktopServices::openUrl(messageUrl);
        } else {
            m_pendingMessageProject = {target, messageUrl};
            if (!m_projectDirectoryLoaded)
                m_backend->requestProjects();
            else
                requestNextProjectsPage();
        }
        return;
    }
    if (!m_workspace.clearTask())
        return;
    const QSignalBlocker blocker(m_projectNameSearch);
    m_projectNameSearch->clear();
    m_projectNameQuery.clear();
    m_pendingProjectId.clear();
    m_linkedProjectId = target.id;
    emit sectionRequested();
}

void ProjectsPanel::showProjectPinMenu(const QPoint &point)
{
    const auto *item = m_projects->itemAt(point);
    if (!item)
        return;
    const auto id = item->data(workspace::Id).toString();
    if (id.isEmpty())
        return;
    const auto op = operationKey(OperationKind::ProjectPin, id);
    const bool pinned = item->data(workspace::Pinned).toBool();
    auto *menu = ui::directoryPinMenu(this, pinned, [this, op] {
        return m_workspace.writable() && !m_pendingPins.contains(op);
    }, [this, op, id, pinned] {
        emit statusChanged(QString());
        m_pendingPins.insert(op);
        m_backend->setProjectPinned(id, !pinned);
    });
    menu->popup(m_projects->viewport()->mapToGlobal(point));
}

} // namespace bx
