#include "ui/projects_panel.h"

#include "ui/board_widget.h"
#include "ui/chat_widgets.h"
#include "ui/project_tools.h"
#include "ui/task_dialogs.h"
#include "ui/task_gantt.h"
#include "ui/workspace_directory.h"
#include "ui/workspace_presentation.h"

#include <QButtonGroup>
#include <QDesktopServices>
#include <QFormLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <utility>

namespace bx {

using workspace::label;

ProjectsPanel::ProjectsPanel(Backend *backend, Workspace workspace, QWidget *parent)
    : QWidget(parent), m_backend(backend), m_workspace(std::move(workspace))
{
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto *splitter = new DirectorySplitter(QStringLiteral("ui/projectDirectoryWidth"), 205, 151, this);
    splitter->setObjectName(QStringLiteral("projectDirectorySplitter"));
    splitter->setAccessibleName(QStringLiteral("Ширина списка проектов"));
    root->addWidget(splitter);
    auto *side = directory(QStringLiteral("Проекты"), &m_projects, splitter, &m_projectDirectoryTitle);
    side->setMinimumWidth(132);
    connect(m_projects, &QListWidget::currentItemChanged, this, [this] {
        m_pendingMessageProject = {};
        onProjectSelected();
    });
    m_projects->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_projects, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &point) { showProjectPinMenu(point); });
    connect(m_projects->verticalScrollBar(), &QScrollBar::valueChanged, this, &ProjectsPanel::avatarsRequested);
    auto *projectSearchRow = new QHBoxLayout;
    projectSearchRow->setSpacing(2);
    m_projectNameSearch = new QLineEdit(side);
    ui::editor(m_projectNameSearch);
    m_projectNameSearch->setAccessibleName(QStringLiteral("Поиск по названию проектов"));
    m_projectNameSearch->setPlaceholderText(QStringLiteral("Название проекта"));
    m_projectNameSearch->setClearButtonEnabled(true);
    projectSearchRow->addWidget(m_projectNameSearch, 1);
    auto *projectSideLayout = qobject_cast<QVBoxLayout *>(side->layout());
    if (projectSideLayout)
        projectSideLayout->insertLayout(1, projectSearchRow);
    connect(m_projectNameSearch, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (m_projectNameQuery == text.trimmed())
            return;
        m_projectNameQuery = text.trimmed();
        showProjects();
    });
    m_moreProjectsButton = new QPushButton(QStringLiteral("Ещё проекты"), side);
    m_moreProjectsButton->setObjectName(QStringLiteral("moreProjects"));
    m_moreProjectsButton->hide();
    connect(m_moreProjectsButton, &QPushButton::clicked, this, &ProjectsPanel::requestNextProjectsPage);
    side->layout()->addWidget(m_moreProjectsButton);
    m_projects->installEventFilter(this);
    m_projects->viewport()->installEventFilter(this);
    auto *projectScroll = m_projects->verticalScrollBar();
    connect(projectScroll, &QScrollBar::actionTriggered, this, [this, projectScroll](int action) {
        if (action == QAbstractSlider::SliderSingleStepAdd || action == QAbstractSlider::SliderPageStepAdd
            || action == QAbstractSlider::SliderToMaximum
            || (action == QAbstractSlider::SliderMove && projectScroll->isSliderDown()
                && projectScroll->sliderPosition() > projectScroll->value()))
            scheduleProjectsPageCheck();
    });
    connect(projectScroll, &QScrollBar::rangeChanged, this, &ProjectsPanel::updateProjectsRailPagination);
    m_newProjectButton = new QPushButton(QStringLiteral("Новый проект"), side);
    m_newProjectButton->setAccessibleName(QStringLiteral("Создать проект"));
    connect(m_newProjectButton, &QPushButton::clicked, this, &ProjectsPanel::createProject);
    side->layout()->addWidget(m_newProjectButton);
    splitter->addWidget(side);

    auto *body = new QWidget(splitter);
    body->setMinimumWidth(430);
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(16, 17, 16, 14);
    layout->setSpacing(12);
    auto *header = new QFormLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setHorizontalSpacing(10);
    header->setVerticalSpacing(6);
    header->setRowWrapPolicy(QFormLayout::WrapLongRows);
    header->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto *heading = new QWidget(body);
    auto *headingLayout = new QHBoxLayout(heading);
    headingLayout->setContentsMargins(0, 0, 0, 0);
    m_projectAvatar = new ui::AvatarBadge(body);
    m_projectAvatar->hide();
    headingLayout->addWidget(m_projectAvatar);
    m_projectTitle = label(QStringLiteral("Проекты"), "pageTitle", body);
    m_projectTitle->setAccessibleName(QStringLiteral("Открытый проект"));
    headingLayout->addWidget(m_projectTitle, 1);
    auto *actions = new QWidget(body);
    auto *actionsLayout = new QHBoxLayout(actions);
    actionsLayout->setContentsMargins(0, 0, 0, 0);
    actionsLayout->addStretch(1);
    m_projectToolsButton = new QPushButton(QStringLiteral("О проекте"), body);
    m_projectToolsButton->setAccessibleName(QStringLiteral("Участники и файлы проекта"));
    connect(m_projectToolsButton, &QPushButton::clicked, this, &ProjectsPanel::openProjectTools);
    actionsLayout->addWidget(m_projectToolsButton);
    m_newTaskButton = new QPushButton(QStringLiteral("Новая задача"), body);
    m_newTaskButton->setObjectName(QStringLiteral("primary"));
    m_newTaskButton->setAccessibleName(QStringLiteral("Создать задачу"));
    connect(m_newTaskButton, &QPushButton::clicked, this, &ProjectsPanel::taskCreationRequested);
    actionsLayout->addWidget(m_newTaskButton);
    auto *refresh = new QPushButton(QStringLiteral("Обновить"), body);
    refresh->setToolTip(QStringLiteral("Обновить проект (F5)"));
    connect(refresh, &QPushButton::clicked, this, &ProjectsPanel::refreshRequested);
    actionsLayout->addWidget(refresh);
    header->addRow(heading, actions);
    layout->addLayout(header);

    m_projectTabs = new QTabBar(body);
    m_projectTabs->setObjectName(QStringLiteral("projectScopeTabs"));
    m_projectTabs->setAccessibleName(QStringLiteral("Раздел проекта"));
    m_projectTabs->setExpanding(false);
    m_projectTabs->addTab(QStringLiteral("Задачи"));
    m_projectTabs->addTab(QStringLiteral("Чаты"));
    layout->addWidget(m_projectTabs);
    m_projectViews = new QStackedWidget(body);
    layout->addWidget(m_projectViews, 1);
    auto *tasks = new QWidget(m_projectViews);
    m_projectViews->addWidget(tasks);
    auto *taskLayout = new QVBoxLayout(tasks);
    taskLayout->setContentsMargins(0, 0, 0, 0);
    taskLayout->setSpacing(12);

    auto *tools = new QHBoxLayout;
    tools->setSpacing(5);
    auto *viewOptions = new QButtonGroup(body);
    m_kanbanButton = new QPushButton(QStringLiteral("Канбан"), body);
    m_kanbanButton->setObjectName(QStringLiteral("viewToggle"));
    m_kanbanButton->setCheckable(true);
    m_kanbanButton->setChecked(true);
    viewOptions->addButton(m_kanbanButton);
    m_kanbanButton->setAccessibleName(QStringLiteral("Вид Канбан"));
    connect(m_kanbanButton, &QPushButton::clicked, this, [this] {
        showProjectTimeline(false);
        m_board->setView(BoardWidget::View::Kanban);
    });
    tools->addWidget(m_kanbanButton, 0);
    m_listButton = new QPushButton(QStringLiteral("Список"), body);
    m_listButton->setObjectName(QStringLiteral("viewToggle"));
    m_listButton->setCheckable(true);
    viewOptions->addButton(m_listButton);
    m_listButton->setAccessibleName(QStringLiteral("Вид Список"));
    connect(m_listButton, &QPushButton::clicked, this, [this] {
        showProjectTimeline(false);
        m_board->setView(BoardWidget::View::List);
    });
    tools->addWidget(m_listButton, 0);
    auto *gantt = new QPushButton(QStringLiteral("Гант"), body);
    gantt->setObjectName(QStringLiteral("viewToggle"));
    gantt->setCheckable(true);
    gantt->setAccessibleName(QStringLiteral("Вид Гант"));
    viewOptions->addButton(gantt);
    connect(gantt, &QPushButton::clicked, this, [this] { showProjectTimeline(true); });
    tools->addWidget(gantt, 0);
    m_filter = new QLineEdit(body);
    ui::editor(m_filter);
    m_filter->setAccessibleName(QStringLiteral("Фильтр задач"));
    m_filter->setPlaceholderText(QStringLiteral("Название или №"));
    m_filter->setClearButtonEnabled(true);
    connect(m_filter, &QLineEdit::textChanged, this, [this](const QString &text) {
        const bool hadResults = m_projectSearchDisplayed;
        m_projectSearchDisplayed = false;
        m_projectSearchResults.clear();
        m_projectTimeline->setFilter(text);
        if (m_ganttMode)
            return;
        if (hadResults)
            m_board->setStages(m_stages);
        m_board->setFilter(text);
        const bool searching = m_active && m_projectTabs->currentIndex() == 0
            && !m_currentProjectId.isEmpty() && !text.trimmed().isEmpty();
        ui::setStatusLabel(m_projectTaskSearchStatus, searching ? QStringLiteral("Ищем задачи в проекте…") : QString(), searching);
        if (searching)
            m_projectSearchTimer->start();
        else
            m_projectSearchTimer->stop();
    });
    connect(m_filter, &QLineEdit::returnPressed, this, &ProjectsPanel::runProjectTaskSearch);
    tools->addWidget(m_filter, 1);
    m_stagesButton = new QPushButton(QStringLiteral("Этапы"), body);
    m_stagesButton->setAccessibleName(QStringLiteral("Управление этапами"));
    connect(m_stagesButton, &QPushButton::clicked, this, [this] { openStageManager(); });
    tools->addWidget(m_stagesButton, 0);
    taskLayout->addLayout(tools);
    m_projectTaskSearchStatus = label({}, "quiet", body);
    m_projectTaskSearchStatus->setAccessibleName(QStringLiteral("Состояние поиска задач"));
    m_projectTaskSearchStatus->setWordWrap(true);
    m_projectTaskSearchStatus->hide();
    taskLayout->addWidget(m_projectTaskSearchStatus);

    m_board = new BoardWidget(body);
    m_board->setAvatarSource([this](const QUrl &url) { return m_workspace.avatar(url); });
    connect(m_board, &BoardWidget::visibleCardsChanged, this, &ProjectsPanel::avatarsRequested);
    taskLayout->addWidget(m_board, 1);
    connect(m_board, &BoardWidget::taskClicked, this, &ProjectsPanel::taskRequested);
    m_board->setTaskMoveGuard([this](const QString &task, const QString &from, const QString &to) {
        return canMoveBoardTask(task, from, to);
    });
    connect(m_board, &BoardWidget::taskMoveRequested, this,
            [this](const QString &task, const QString &from, const QString &to) {
        if (to.isEmpty() || !canMoveBoardTask(task, from, to))
            return;
        const auto project = m_currentProjectId;
        m_pendingBoardEdit = project;
        m_pendingMoveTask = task;
        emit statusChanged(QStringLiteral("Переносим задачу…"));
        emit writeStateChanged();
        m_backend->moveTask(project, task, to);
    });
    connect(m_board, &BoardWidget::loadMoreRequested, this, [this](const QString &stage) {
        if (!m_active || m_projectTabs->currentIndex() != 0 || m_ganttMode
            || m_currentProjectId.isEmpty() || !m_filter->text().trimmed().isEmpty()
            || m_board->stagePageLoading(stage))
            return;
        m_board->setStagePageLoading(stage, true);
        m_backend->requestMoreBoardTasks(m_currentProjectId, stage);
    });
    m_projectTimeline = new ui::TaskGanttView(body);
    m_projectTimeline->onOpen = [this](const QString &id) { emit taskRequested(id); };
    m_projectTimeline->hide();
    taskLayout->addWidget(m_projectTimeline, 1);

    auto *chats = new QWidget(m_projectViews);
    m_projectViews->addWidget(chats);
    auto *chatLayout = new QVBoxLayout(chats);
    chatLayout->setContentsMargins(0, 0, 0, 0);
    chatLayout->setSpacing(6);
    m_projectTaskChatStatus = label({}, "quiet", chats);
    m_projectTaskChatStatus->setAccessibleName(QStringLiteral("Состояние чатов проекта"));
    m_projectTaskChatStatus->setTextFormat(Qt::PlainText);
    m_projectTaskChatStatus->setWordWrap(true);
    chatLayout->addWidget(m_projectTaskChatStatus);
    m_projectTaskChats = new QListWidget(chats);
    m_projectTaskChats->setAccessibleName(QStringLiteral("Чаты задач проекта"));
    m_projectTaskChats->setAccessibleDescription(QStringLiteral("Открыть чат в карточке задачи"));
    workspace::configureRows(m_projectTaskChats, workspace::Row::Directory);
    chatLayout->addWidget(m_projectTaskChats, 1);
    connect(m_projectTaskChats, &QListWidget::itemEntered, this, [this](const QListWidgetItem *) {
        m_projectTaskChats->viewport()->setCursor(Qt::PointingHandCursor);
    });
    connect(m_projectTaskChats, &QListWidget::viewportEntered, this, [this] {
        m_projectTaskChats->viewport()->unsetCursor();
    });
    connect(m_projectTaskChats->verticalScrollBar(), &QScrollBar::valueChanged, this, &ProjectsPanel::avatarsRequested);
    const auto openTaskChat = [this](const QListWidgetItem *item) {
        if (item && m_active && m_projectTabs->currentIndex() == 1
            && m_projectTaskChatScope == m_currentProjectId && !m_projectTaskChatScope.isEmpty())
            emit projectTaskRequested(item->data(workspace::TaskId).toString(), m_currentProjectId);
    };
    connect(m_projectTaskChats, &QListWidget::itemClicked, this, openTaskChat);
    connect(m_projectTaskChats, &QListWidget::itemActivated, this, openTaskChat);
    m_moreProjectTaskChats = new QPushButton(QStringLiteral("Ещё чаты"), chats);
    m_moreProjectTaskChats->setObjectName(QStringLiteral("moreProjectTaskChats"));
    connect(m_moreProjectTaskChats, &QPushButton::clicked, this, &ProjectsPanel::requestProjectTaskChatPage);
    chatLayout->addWidget(m_moreProjectTaskChats, 0, Qt::AlignLeft);
    connect(m_projectTabs, &QTabBar::currentChanged, this, &ProjectsPanel::onProjectScopeChanged);
    updateProjectTaskChatControls();

    splitter->addWidget(body);
    splitter->setStretchFactor(1, 1);
    connect(splitter, &QSplitter::splitterMoved, this, &ProjectsPanel::avatarsRequested);

    m_projectSearchTimer = new QTimer(this);
    m_projectSearchTimer->setSingleShot(true);
    m_projectSearchTimer->setInterval(300);
    connect(m_projectSearchTimer, &QTimer::timeout, this, &ProjectsPanel::runProjectTaskSearch);
    connectBackend();
}

void ProjectsPanel::connectBackend()
{
    connect(m_backend, &Backend::projectPinChanged, this, [this](const QString &id, bool pinned) {
        m_pendingPins.remove(operationKey(OperationKind::ProjectPin, id));
        for (auto &project : m_projectDirectory)
            if (project.id == id)
                project.pinned = pinned;
        if (m_active)
            showProjects();
    });
    connect(m_backend, &Backend::projectUnreadCountsChanged, this,
            [this](const QHash<QString, qint64> &counts, bool complete) {
        if (m_projectUnreadCountsComplete != complete || m_projectUnreadCounts != counts) {
            m_projectUnreadCounts = counts;
            m_projectUnreadCountsComplete = complete;
            updateProjectUnreadRows();
        }
    });
    connect(m_backend, &Backend::projectTaskChatsLoaded, this, &ProjectsPanel::onProjectTaskChatsLoaded);
    connect(m_backend, &Backend::projectTaskChatBadgesChanged, this, &ProjectsPanel::updateProjectTaskChatBadges);

    connect(m_backend, &Backend::projectsLoaded, this, [this](const QVector<Project> &projects) {
        const bool appended = projects.size() > m_projectDirectory.size() && !m_projectDirectory.isEmpty()
            && std::equal(m_projectDirectory.cbegin(), m_projectDirectory.cend(), projects.cbegin(),
                          [](const Project &old, const Project &now) { return old.id == now.id; });
        const auto scroll = m_projects->verticalScrollBar()->value();
        m_projectDirectory = projects;
        m_projectDirectoryLoaded = true;
        m_projectsPagePending = false;
        m_projectsPageFailed = false;
        emit directoryChanged(projects);
        if (m_active) {
            const bool selectingNotice = !m_linkedProjectId.isEmpty();
            showProjects();
            if (appended && !selectingNotice)
                m_projects->verticalScrollBar()->setValue(scroll);
            if (!m_linkedProjectId.isEmpty() && !m_projectsMoreAvailable) {
                emit statusChanged(QStringLiteral("Проект недоступен или отсутствует в списке"));
                m_linkedProjectId.clear();
            }
        }
        updateProjectsRailPagination();
        const auto link = std::exchange(m_pendingMessageProject, {});
        if (!link.url.isEmpty())
            openLinkedProject(link.target, link.url);
    });
    connect(m_backend, &Backend::projectsMoreAvailable, this, [this](bool available) {
        m_projectsMoreAvailable = available;
        emit moreProjectsChanged(available);
        updateProjectsRailPagination();
    });
    connect(m_backend, &Backend::projectsScopeLoaded, this, [this](bool membershipVerified) {
        const auto title = membershipVerified ? QStringLiteral("Мои проекты") : QStringLiteral("Доступные проекты");
        m_projectDirectoryTitle->setText(title);
        m_projects->setAccessibleName(title);
    });

    connect(m_backend, &Backend::boardLoaded, this, [this](const QString &projectId, const QVector<Stage> &stages) {
        emit formStagesLoaded(projectId, stages);
        if (!m_active || projectId != m_currentProjectId)
            return;
        m_stages = stages;
        m_loadedBoardTaskIds.clear();
        for (const auto &stage : stages)
            for (const auto &task : stage.tasks)
                m_loadedBoardTaskIds.insert(task.id);
        if (m_boardMoveRefreshProject == projectId) {
            m_boardMoveRefreshProject.clear();
            emit statusChanged(QStringLiteral("Задача перенесена"));
        }
        m_board->resetStagePageLoads();
        if (m_projectSearchDisplayed)
            displayProjectTaskResults();
        else
            m_board->setStages(stages);
        if (m_stageManager && m_stageManager->projectId() == projectId)
            m_stageManager->setStages(stages);
        emit currentStagesLoaded(projectId, stages);
    });
    connect(m_backend, &Backend::boardTasksPageLoaded, this,
            [this](const QString &project, const QString &stageId, const QVector<Task> &tasks, bool more) {
        if (project != m_currentProjectId || !m_board->stagePageLoading(stageId))
            return;
        const auto stage = std::find_if(m_stages.begin(), m_stages.end(), [&stageId](const Stage &column) {
            return column.id == stageId;
        });
        if (stage == m_stages.end()) {
            m_board->resetStagePageLoads(false);
            return;
        }
        const auto previousCount = stage->tasks.size();
        for (const auto &task : tasks) {
            if (task.id.isEmpty() || task.stageId != stageId || m_loadedBoardTaskIds.contains(task.id))
                continue;
            m_loadedBoardTaskIds.insert(task.id);
            stage->tasks.append(task);
        }
        stage->hasMoreTasks = more;
        if (!m_board->finishStagePage(stageId, !more || stage->tasks.size() > previousCount))
            emit statusChanged(QStringLiteral("Портал перестал возвращать задачи. F5 — обновить доску"));
        if (!m_projectSearchDisplayed)
            m_board->applyStagePage(*stage);
    });
    connect(m_backend, &Backend::projectMembersLoaded, this, [this](const QString &project, const auto &members) {
        if (m_projectTools && m_projectTools->projectId() == project)
            m_projectTools->setMembers(members);
    });
    connect(m_backend, &Backend::projectFilesLoaded, this,
            [this](const QString &project, const QString &folder, const auto &files, int next) {
        if (m_projectTools && m_projectTools->projectId() == project)
            m_projectTools->setFiles(folder, files, next);
    });
    connect(m_backend, &Backend::projectTimelineLoaded, this, [this](const QString &project, const auto &tasks, bool limited) {
        if (m_ganttMode && project == m_currentProjectId)
            m_projectTimeline->setTasks(tasks, limited);
    });
    connect(m_backend, &Backend::projectCreated, this, &ProjectsPanel::onProjectCreated);
    connect(m_backend, &Backend::boardUpdated, this, &ProjectsPanel::onBoardUpdated);
    connect(m_backend, &Backend::projectTasksFound, this, &ProjectsPanel::onProjectTasksFound);
}

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

void ProjectsPanel::resetAccount(bool previousAccount)
{
    m_pendingMessageProject = {};
    m_linkedProjectId.clear();
    if (previousAccount) {
        m_projectDirectory.clear();
        m_projectDirectoryLoaded = false;
        m_projectsMoreAvailable = false;
        m_projectsPagePending = false;
        m_projectsPageFailed = false;
        m_pendingProjectId.clear();
        updateProjectsRailPagination();
    }
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

void ProjectsPanel::updateColumnWidthContext(const QString &scope)
{
    m_board->setColumnWidthContext(m_currentProjectId, chatPreferenceKey(scope, QLatin1String("kanbanColumnWidths")));
}

void ProjectsPanel::refreshBoardAvatars()
{
    m_board->refreshAvatars();
}

void ProjectsPanel::setSectionActive(bool active)
{
    cancelPendingMessageProject();
    if (m_active && !active) {
        m_linkedProjectId.clear();
        m_projectSearchTimer->stop();
        ui::setStatusLabel(m_projectTaskSearchStatus, {});
        closeProjectTaskChats();
    }
    m_active = active;
    if (!active)
        closeTools();
}

void ProjectsPanel::closeTools()
{
    if (m_projectTools)
        m_projectTools->reject();
}

void ProjectsPanel::resumeSearch()
{
    if (!m_projectSearchDisplayed)
        runProjectTaskSearch();
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

bool ProjectsPanel::handleFailure(const QString &operation, OperationKind kind, const QStringList &ids, const QString &reason, QString &footer)
{
    switch (kind) {
    case OperationKind::ProjectTaskChats:
        if (!m_projectTaskChatScope.isEmpty() && m_projectTaskChatPending >= 0
            && ids.value(0) == m_projectTaskChatScope) {
            m_projectTaskChatPending = -1;
            m_projectTaskChatError = reason.isEmpty() ? QStringLiteral("Не удалось загрузить чаты задач") : reason;
            updateProjectTaskChatControls();
        }
        return true;
    case OperationKind::BoardColumn: {
        const auto stage = ids.value(1);
        if (ids.value(0) == m_currentProjectId &&
            m_board->stagePageLoading(stage)) {
            m_board->failStagePage(stage);
            emit statusChanged(QStringLiteral("%1 · F5 — обновить доску").arg(reason));
        }
        return true;
    }
    case OperationKind::ProjectMembers:
        if (m_projectTools && ids.value(0) == m_projectTools->projectId())
            m_projectTools->membersFailed(reason);
        return true;
    case OperationKind::ProjectFiles:
        if (m_projectTools && ids.value(0) == m_projectTools->projectId())
            m_projectTools->filesFailed(ids.value(1), reason);
        return true;
    case OperationKind::Timeline:
        if (!m_ganttMode || m_currentProjectId.isEmpty() || ids.value(0) != m_currentProjectId)
            break;
        m_projectTimeline->clear(reason);
        return true;
    case OperationKind::SearchProjectTasks: {
        const auto projectId = ids.value(0);
        const auto query = m_pendingProjectSearches.take(projectId);
        if (m_active && !m_ganttMode && projectId == m_currentProjectId && !query.isEmpty()) {
            if (query == m_filter->text().trimmed())
                ui::setStatusLabel(m_projectTaskSearchStatus, QStringLiteral("Поиск по проекту недоступен. %1").arg(reason));
            else
                runProjectTaskSearch();
        }
        return true;
    }
    case OperationKind::ProjectPin:
        if (m_pendingPins.remove(operation))
            emit statusChanged(reason);
        return true;
    case OperationKind::CreateProject:
        if (!m_pendingCreateProject)
            break;
        m_pendingCreateProject = false;
        if (m_projectForm)
            m_projectForm->setStatus(reason);
        break;
    case OperationKind::BoardEdit:
        if (m_pendingBoardEdit.isEmpty() || ids.value(0) != m_pendingBoardEdit)
            break;
        if (m_stageManager && m_stageManager->projectId() == m_pendingBoardEdit)
            m_stageManager->setStatus(reason);
        emit boardMoveFailed(m_pendingMoveTask, reason);
        m_pendingBoardEdit.clear();
        m_pendingMoveTask.clear();
        break;
    case OperationKind::Board:
        if (m_currentProjectId.isEmpty() || ids.value(0) != m_currentProjectId)
            break;
        m_board->resetStagePageLoads(false);
        if (m_stages.isEmpty())
            m_board->clearBoard(QStringLiteral("Не удалось загрузить доску. Нажмите «Обновить»"));
        else if (!m_projectSearchDisplayed)
            m_board->setStages(m_stages);
        if (m_currentProjectId == m_boardMoveRefreshProject)
            footer = QStringLiteral("Не удалось обновить доску после переноса. Нажмите F5");
        break;
    case OperationKind::Projects:
    case OperationKind::ProjectPageFallback: {
        const auto link = std::exchange(m_pendingMessageProject, {});
        if (!link.url.isEmpty())
            QDesktopServices::openUrl(link.url);
        m_linkedProjectId.clear();
        emit directoryLoadFailed();
        if (m_projectsPagePending) {
            m_projectsPagePending = false;
            m_projectsPageFailed = true;
            updateProjectsRailPagination();
        }
        if (kind == OperationKind::Projects && m_active && !m_projectDirectoryLoaded) {
            m_projects->clear();
            workspace::emptyRow(m_projects, QStringLiteral("Не удалось загрузить проекты"));
        }
        break;
    }
    default:
        break;
    }
    return false;
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

QString ProjectsPanel::currentProjectTitle() const
{
    return m_projectTitle->text();
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
