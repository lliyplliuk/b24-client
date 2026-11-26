#include "ui/projects_panel.h"

#include "ui/board_widget.h"
#include "ui/chat_widgets.h"
#include "ui/project_tools.h"
#include "ui/task_dialogs.h"
#include "ui/task_gantt.h"
#include "ui/workspace_directory.h"
#include "ui/workspace_presentation.h"

#include <QButtonGroup>
#include <QFormLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
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

} // namespace bx
