#include "ui/task_detail_editor.h"

#include "ui/task_selection_field.h"
#include "ui/task_picker_choices.h"
#include "ui/workspace_style.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace bx::ui {

TaskDetailEditor::TaskDetailEditor(QWidget *parent, Mode mode)
    : QWidget(parent), m_mode(mode)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 1);
    layout->setSpacing(4);
    m_heading = new QLabel(this);
    m_heading->setObjectName(QStringLiteral("fieldCaption"));
    layout->addWidget(m_heading);
    m_summary = new TaskPickerSummary(this);
    layout->addWidget(m_summary);
    m_editors = new QStackedWidget(this);
    m_editors->setObjectName(QStringLiteral("taskDetailEditors"));
    layout->addWidget(m_editors);

    m_creator = addPeopleField(Field::Creator, QStringLiteral("Постановщик"));
    m_assignee = addPeopleField(Field::Assignee,
        mode == Mode::NewTask ? QStringLiteral("Ответственный") : QStringLiteral("Исполнитель"));
    m_observers = addPeopleField(Field::Observers, QStringLiteral("Наблюдатели"), true);

    auto *deadlineRow = new QWidget(this);
    auto *deadlineEditor = new QVBoxLayout(deadlineRow);
    deadlineEditor->setContentsMargins(0, 0, 0, 0);
    deadlineEditor->setSpacing(4);
    auto *deadlineLayout = new QHBoxLayout;
    deadlineLayout->setSpacing(4);
    m_deadline = new QDateTimeEdit(deadlineRow);
    m_deadline->setObjectName(QStringLiteral("workspaceEditor"));
    m_deadline->setAccessibleName(QStringLiteral("Срок задачи"));
    m_deadline->setDisplayFormat(QStringLiteral("dd.MM.yyyy HH:mm"));
    m_deadline->setCalendarPopup(true);
    deadlineLayout->addWidget(m_deadline, 1);
    m_noDeadline = new QCheckBox(QStringLiteral("Без срока"), deadlineRow);
    m_noDeadline->setAccessibleName(QStringLiteral("Без срока"));
    deadlineLayout->addWidget(m_noDeadline);
    deadlineEditor->addLayout(deadlineLayout);
    m_deadlineReason = new QLineEdit(deadlineRow);
    m_deadlineReason->setObjectName(QStringLiteral("workspaceEditor"));
    m_deadlineReason->setAccessibleName(QStringLiteral("Причина изменения срока"));
    m_deadlineReason->setPlaceholderText(QStringLiteral("Причина изменения срока"));
    m_deadlineReason->setMaxLength(500);
    deadlineEditor->addWidget(m_deadlineReason);
    connect(m_noDeadline, &QCheckBox::toggled, m_deadline, [this](bool withoutDeadline) {
        m_deadline->setEnabled(!withoutDeadline);
        updateApply();
    });
    connect(m_deadline, &QDateTimeEdit::dateTimeChanged, this, [this] { updateApply(); });
    connect(m_deadlineReason, &QLineEdit::textChanged, this, [this] { updateApply(); });
    addField(Field::Deadline, QStringLiteral("Крайний срок"), deadlineRow);

    m_status = addCombo(Field::Status, QStringLiteral("Статус"));
    auto *projectEditor = new QWidget(this);
    auto *projectLayout = new QVBoxLayout(projectEditor);
    projectLayout->setContentsMargins(0, 0, 0, 0);
    projectLayout->setSpacing(4);
    m_projectSearch = new QLineEdit(projectEditor);
    m_projectSearch->setAccessibleName(QStringLiteral("Поиск проекта"));
    m_projectSearch->setPlaceholderText(QStringLiteral("Название проекта"));
    m_projectSearch->setClearButtonEnabled(true);
    m_projectField = new TaskSelectionField(m_projectSearch, projectEditor);
    m_projectField->iconFor = [this](const TaskSelectionField::Entry &entry) {
        const auto *project = projectChoice(entry.id);
        return project ? requisiteAvatarIcon(m_avatarSource, entry.name, entry.avatar, workspace::AvatarKind::Project,
            QColor(project->avatarColor), project->avatarVariant, project->avatarType) : QIcon();
    };
    m_projectField->onVisibleChanged = [this] { if (onAvatarsVisible) onAvatarsVisible(); };
    projectLayout->addWidget(m_projectField);
    m_project = new QListWidget(projectEditor);
    m_project->setObjectName(QStringLiteral("taskRequisiteChoices"));
    m_project->setAccessibleName(QStringLiteral("Проект задачи"));
    m_project->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_project->setIconSize(QSize(22, 22));
    m_project->setItemDelegate(new TaskPickerChoiceDelegate(m_project));
    projectLayout->addWidget(m_project);
    m_projectEmpty = new QLabel(QStringLiteral("Проекты не найдены"), projectEditor);
    m_projectEmpty->setObjectName(QStringLiteral("taskDetail"));
    m_projectEmpty->hide();
    projectLayout->addWidget(m_projectEmpty);
    connect(m_projectSearch, &QLineEdit::textChanged, this, [this] { fillProjects(); });
    connect(m_project, &QListWidget::itemClicked, this, &TaskDetailEditor::selectProject);
    connect(m_project, &QListWidget::itemActivated, this, &TaskDetailEditor::selectProject);
    connect(m_projectSearch, &QLineEdit::returnPressed, this, [this] {
        auto *choice = m_project->currentItem();
        selectProject(choice ? choice : m_project->count() == 1 ? m_project->item(0) : nullptr);
    });
    connect(m_project->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (onAvatarsVisible)
            onAvatarsVisible();
    });
    m_moreProjects = new QPushButton(QStringLiteral("Ещё проекты"), projectEditor);
    m_moreProjects->setAccessibleName(QStringLiteral("Загрузить следующие проекты"));
    m_moreProjects->setAutoDefault(false);
    connect(m_moreProjects, &QPushButton::clicked, this, [this] {
        if (onMoreProjects)
            onMoreProjects();
    });
    m_moreProjects->hide();
    projectLayout->addWidget(m_moreProjects, 0, Qt::AlignLeft);
    addField(Field::Project, QStringLiteral("Проект"), projectEditor);

    m_conflict = new QLabel(this);
    m_conflict->setObjectName(QStringLiteral("taskDetail"));
    m_conflict->setTextFormat(Qt::PlainText);
    m_conflict->setWordWrap(true);
    m_conflict->hide();
    layout->addWidget(m_conflict);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    m_cancel = new QPushButton(QStringLiteral("Отмена"), this);
    m_cancel->setAccessibleName(QStringLiteral("Отменить изменение реквизита"));
    m_cancel->setAutoDefault(false);
    connect(m_cancel, &QPushButton::clicked, this, [this] { finish(); });
    buttons->addWidget(m_cancel);
    m_apply = new QPushButton(QStringLiteral("Применить"), this);
    m_apply->setObjectName(QStringLiteral("primary"));
    m_apply->setAccessibleName(QStringLiteral("Применить изменение реквизита"));
    m_apply->setAutoDefault(false);
    connect(m_apply, &QPushButton::clicked, this, &TaskDetailEditor::submitPatch);
    buttons->addWidget(m_apply);
    layout->addLayout(buttons);
    hide();
}

QString TaskDetailEditor::formattedDate(const QString &value)
{
    if (const auto date = QDate::fromString(value, Qt::ISODate); value.size() == 10 && date.isValid())
        return date.toString(QStringLiteral("dd.MM.yyyy"));
    const auto timestamp = QDateTime::fromString(value, Qt::ISODate);
    return timestamp.isValid() ? timestamp.toLocalTime().toString(QStringLiteral("dd.MM.yyyy  HH:mm")) : value;
}

void TaskDetailEditor::setTask(const Task &task)
{
    const bool isPicker = m_active == Field::Creator || m_active == Field::Assignee
        || m_active == Field::Observers || m_active == Field::Project;
    const bool followPortal = isPicker && !changed();
    m_task = task;
    if (followPortal)
        m_baseline = task;
    switch (m_active) {
    case Field::Creator:
        m_creator->setConfirmedPeople({{task.creatorId, task.createdBy, task.creatorAvatarUrl}}, followPortal);
        break;
    case Field::Assignee:
        m_assignee->setConfirmedPeople({{task.assigneeId, task.assignee, task.assigneeAvatarUrl}}, followPortal);
        break;
    case Field::Observers:
        m_observers->setConfirmedPeople(task.observerPeople, followPortal);
        break;
    case Field::Project:
        if (followPortal) {
            m_projectChoice = projectOption(task.projectId);
            m_projectChoiceName.clear();
        }
        fillProjects();
        break;
    default: break;
    }
    refreshSummary();
    updateControls();
}

void TaskDetailEditor::setProjectChoices(const QVector<Project> &projects)
{
    m_projects = projects;
    if (m_active == Field::Project)
        fillProjects();
    updateControls();
}

void TaskDetailEditor::setCanDetachProject(bool allowed)
{
    m_canDetachProject = allowed;
    if (m_active == Field::Project)
        fillProjects();
    updateControls();
}

void TaskDetailEditor::setMoreProjectsAvailable(bool available)
{
    m_moreProjects->setVisible(available);
    updateControls();
}

void TaskDetailEditor::setWriteState(bool writable, bool busy)
{
    m_writable = writable;
    m_busy = busy;
    updateControls();
}

void TaskDetailEditor::setPeopleSearch(const std::function<void(const QString &, const QString &)> &handler)
{
    for (auto *people : {m_creator, m_assignee, m_observers})
        people->setSearchHandler(handler);
    m_peopleSearchAvailable = bool(handler);
    updateControls();
}

void TaskDetailEditor::setPeopleResults(const QString &id, const QVector<Person> &people)
{
    for (auto *picker : {m_creator, m_assignee, m_observers})
        picker->setResults(id, people);
}

bool TaskDetailEditor::setPeopleSearchError(const QString &id)
{
    return m_creator->showRequestError(id) || m_assignee->showRequestError(id) || m_observers->showRequestError(id);
}

bool TaskDetailEditor::canStart(Field field) const
{
    return m_writable && !m_busy && !active() && allowed(field);
}

void TaskDetailEditor::open(Field field)
{
    start(field);
}

void TaskDetailEditor::cancel()
{
    if (active() && !m_busy) finish();
}

void TaskDetailEditor::focusEditor()
{
    if (m_focusEditor)
        m_focusEditor->setFocus(Qt::OtherFocusReason);
}

void TaskDetailEditor::setAvatarSource(RequisiteAvatarSource source)
{
    m_avatarSource = std::move(source);
    for (auto *picker : {m_creator, m_assignee, m_observers})
        picker->setAvatarSource(m_avatarSource, [this] { if (onAvatarsVisible) onAvatarsVisible(); });
    refreshAvatars();
}

void TaskDetailEditor::fitChoiceLists()
{
    for (auto *picker : {m_creator, m_assignee, m_observers})
        picker->fitSuggestions();
    fitProjectList();
}

QList<QUrl> TaskDetailEditor::visibleAvatarUrls() const
{
    QList<QUrl> urls;
    for (const auto *picker : {m_creator, m_assignee, m_observers})
        urls.append(picker->visibleAvatarUrls());
    urls.append(m_projectField->visibleAvatarUrls());
    if (m_project->isVisible()) {
        for (int i = 0; i < m_project->count(); ++i) {
            const auto *item = m_project->item(i);
            const auto url = item->data(workspace::AvatarUrl).toUrl();
            if (!url.isEmpty() && m_project->viewport()->rect().intersects(m_project->visualItemRect(item)))
                urls.append(url);
        }
    }
    return urls;
}

void TaskDetailEditor::refreshAvatars()
{
    m_projectField->refreshIcons();
    for (auto *picker : {m_creator, m_assignee, m_observers})
        picker->refreshAvatars();
    for (int i = 0; i < m_project->count(); ++i) {
        auto *item = m_project->item(i);
        const auto id = item->data(Qt::UserRole).toString();
        if (id != QLatin1String("0"))
            item->setIcon(requisiteAvatarIcon(m_avatarSource, item->text(),
                item->data(workspace::AvatarUrl).toUrl(), workspace::AvatarKind::Project,
                QColor(item->data(workspace::ProjectAvatarColor).toString()),
                item->data(workspace::ProjectAvatarVariant).toString(),
                item->data(workspace::ProjectAvatarType).toString()));
    }
}

bool TaskDetailEditor::changed() const
{
    return patch().hasChanges();
}

void TaskDetailEditor::accept(Task &task)
{
    const auto pending = patch();
    if (pending.creatorId) {
        task.creatorId = *pending.creatorId;
        task.createdBy = m_creator->people().first().name;
        task.creatorAvatarUrl = m_creator->people().first().avatarUrl;
    }
    if (pending.assigneeId) {
        task.assigneeId = *pending.assigneeId;
        task.assignee = m_assignee->people().first().name;
        task.assigneeAvatarUrl = m_assignee->people().first().avatarUrl;
    }
    if (pending.observerIds) {
        task.observerPeople = m_observers->people();
        task.observers.clear();
        for (const auto &person : task.observerPeople)
            task.observers.append(person.name);
    }
    if (pending.deadline)
        task.due = *pending.deadline;
    if (pending.projectId) {
        const bool detached = *pending.projectId == QLatin1String("0");
        task.projectId = detached ? QString() : *pending.projectId;
        task.project = detached ? QString() : m_projectChoiceName;
        task.stageId.clear();
        task.stageName.clear();
    }
    finish();
}

void TaskDetailEditor::addField(Field field, const QString &caption, QWidget *control)
{
    m_rows.append({field, caption, control});
    m_editors->addWidget(control);
}

TaskPeopleInput *TaskDetailEditor::addPeopleField(Field field, const QString &caption, bool multiple)
{
    auto *picker = new TaskPeopleInput(caption, multiple, this);
    picker->onChange = [this] { updateApply(); };
    if (field == Field::Assignee) {
        picker->onExplicitSelection = [this] {
            if (m_active == Field::Assignee)
                submitPatch();
        };
    }
    picker->onLayoutChanged = [this] {
        m_editors->layout()->invalidate();
        m_editors->updateGeometry();
        layout()->invalidate();
        updateGeometry();
        if (onPickerLayoutChanged)
            onPickerLayoutChanged();
    };
    addField(field, caption, picker);
    return picker;
}

QComboBox *TaskDetailEditor::combo(const QString &caption)
{
    auto *combo = new QComboBox(this);
    combo->setObjectName(QStringLiteral("workspaceEditor"));
    combo->setAccessibleName(caption + QStringLiteral(" задачи"));
    connect(combo, &QComboBox::currentIndexChanged, this, [this] { updateApply(); });
    return combo;
}

QComboBox *TaskDetailEditor::addCombo(Field field, const QString &caption)
{
    auto *control = combo(caption);
    addField(field, caption, control);
    return control;
}

QString TaskDetailEditor::actionLabel(TaskStatusAction action)
{
    switch (action) {
    case TaskStatusAction::Start: return QStringLiteral("Начать выполнение");
    case TaskStatusAction::Pause: return QStringLiteral("Приостановить");
    case TaskStatusAction::Complete: return QStringLiteral("Завершить");
    case TaskStatusAction::Renew: return QStringLiteral("Возобновить");
    case TaskStatusAction::Defer: return QStringLiteral("Отложить");
    case TaskStatusAction::Approve: return QStringLiteral("Принять результат");
    case TaskStatusAction::Disapprove: return QStringLiteral("Вернуть на доработку");
    case TaskStatusAction::Take: return QStringLiteral("Взять в работу");
    }
    return {};
}

QStringList TaskDetailEditor::ids(const QVector<TaskPerson> &people)
{
    QStringList result;
    for (const auto &person : people)
        if (!person.id.isEmpty() && !result.contains(person.id))
            result.append(person.id);
    return result;
}

bool TaskDetailEditor::samePeople(QStringList left, QStringList right)
{
    left.sort();
    right.sort();
    return left == right;
}

bool TaskDetailEditor::allowed(Field field) const
{
    const auto &rights = m_task.editRights;
    switch (field) {
    case Field::Creator: return rights.creator && m_peopleSearchAvailable;
    case Field::Assignee: return rights.assignee && m_peopleSearchAvailable;
    case Field::Observers:
        return rights.observers && m_peopleSearchAvailable
            && ids(m_task.observerPeople).size() >= m_task.observers.size();
    case Field::Deadline: return rights.deadline;
    case Field::Status: return !m_task.statusActions.isEmpty();
    case Field::Project:
        return rights.project && ((m_canDetachProject && !m_task.projectId.isEmpty())
            || !m_moreProjects->isHidden()
            || std::any_of(m_projects.cbegin(), m_projects.cend(), [this](const Project &project) {
                return !project.id.isEmpty() && project.id != m_task.projectId;
            }));
    case Field::None: return false;
    }
    return false;
}

void TaskDetailEditor::start(Field field)
{
    if (!canStart(field))
        return;
    m_baseline = m_task;
    m_active = field;
    m_autoSubmitAttempted = false;
    QWidget *focus = nullptr;
    TaskPeopleInput *peoplePicker = nullptr;
    switch (field) {
    case Field::Creator:
        m_creator->setPeople({{m_task.creatorId, m_task.createdBy, m_task.creatorAvatarUrl}});
        peoplePicker = m_creator;
        focus = peoplePicker->input();
        break;
    case Field::Assignee:
        m_assignee->setPeople({{m_task.assigneeId, m_task.assignee, m_task.assigneeAvatarUrl}});
        peoplePicker = m_assignee;
        focus = peoplePicker->input();
        break;
    case Field::Observers:
        m_observers->setPeople(m_task.observerPeople);
        peoplePicker = m_observers;
        focus = peoplePicker->input();
        break;
    case Field::Deadline: {
        const auto timestamp = QDateTime::fromString(m_task.due, Qt::ISODate);
        const auto date = QDate::fromString(m_task.due, Qt::ISODate);
        const auto defaultDate = date.isValid() ? date : QDate::currentDate();
        m_deadline->setDateTime(timestamp.isValid() ? timestamp.toLocalTime()
                                                   : QDateTime(defaultDate, QTime(18, 0)));
        m_noDeadline->setChecked(m_task.due.isEmpty());
        m_deadlineInitially = m_deadline->dateTime();
        m_deadlineReason->clear();
        focus = m_noDeadline->isChecked() ? static_cast<QWidget *>(m_noDeadline) : m_deadline;
        break;
    }
    case Field::Status:
        m_status->clear();
        m_status->addItem(QStringLiteral("Выберите действие"));
        for (const auto value : m_task.statusActions)
            m_status->addItem(actionLabel(value), int(value));
        focus = m_status;
        break;
    case Field::Project:
        m_projectChoice = projectOption(m_task.projectId);
        m_projectChoiceName.clear();
        m_projectSearch->clear();
        fillProjects();
        focus = m_projectSearch;
        break;
    case Field::None:
        return;
    }
    for (const auto &row : m_rows) {
        row.editor->setSizePolicy(QSizePolicy::Preferred, row.field == field ? QSizePolicy::Preferred
                                                                             : QSizePolicy::Ignored);
        if (row.field == field) {
            m_heading->setText(row.caption);
            m_editors->setCurrentWidget(row.editor);
        }
    }
    show();
    refreshSummary();
    updateControls();
    m_focusEditor = focus;
    focusEditor();
    if (onModeChanged)
        onModeChanged();
    if (peoplePicker)
        peoplePicker->showInitialSuggestions();
}

QString TaskDetailEditor::projectOption(const QString &projectId)
{
    return projectId.isEmpty() ? QStringLiteral("0") : projectId;
}

QListWidgetItem *TaskDetailEditor::listedProject(const QString &id) const
{
    for (int row = 0; row < m_project->count(); ++row)
        if (m_project->item(row)->data(Qt::UserRole).toString() == id)
            return m_project->item(row);
    return nullptr;
}

void TaskDetailEditor::selectProject(QListWidgetItem *item)
{
    if (m_active != Field::Project || !m_project->isEnabled() || !item)
        return;
    const auto choice = item->data(Qt::UserRole).toString();
    if (choice.isEmpty() || choice == m_projectChoice)
        return;
    if (auto *previous = listedProject(m_projectChoice)) {
        previous->setData(TaskPickerDraft, false);
        if (previous->checkState() != Qt::Checked)
            previous->setData(Qt::AccessibleDescriptionRole, {});
    }
    m_projectChoice = choice;
    m_projectChoiceName = item->text();
    const bool draft = choice != projectOption(m_task.projectId);
    item->setData(TaskPickerDraft, draft);
    if (draft)
        item->setData(Qt::AccessibleDescriptionRole, QStringLiteral("Новый проект"));
    refreshProjectPill();
    updateApply();
    submitPatch();
}

bool TaskDetailEditor::appliesOnSelection() const
{
    return m_active == Field::Project || m_active == Field::Assignee;
}

void TaskDetailEditor::submitPatch()
{
    if (!onSave || !m_apply->isEnabled())
        return;
    if (appliesOnSelection()) {
        m_autoSubmitAttempted = true;
        updateApply();
    }
    onSave(patch());
}

void TaskDetailEditor::fitProjectList()
{
    const int limit = window()->height() >= 570 ? 6 : 4;
    m_project->setFixedHeight(qMax(40, qMin(limit, m_project->count()) * TaskPickerChoiceDelegate::RowHeight + 5));
}

void TaskDetailEditor::fillProjects()
{
    const QSignalBlocker blocker(m_project);
    m_project->clear();
    const auto query = m_projectSearch->text().trimmed();
    const auto currentId = projectOption(m_task.projectId);
    const auto add = [this, &query, &currentId](const QString &name, const QString &id, const Project *project = nullptr) {
        if (!name.contains(query, Qt::CaseInsensitive) && !id.contains(query))
            return;
        auto *item = new QListWidgetItem(name, m_project);
        item->setData(Qt::UserRole, id);
        if (project) {
            item->setData(workspace::AvatarUrl, project->avatarUrl);
            item->setData(workspace::ProjectAvatarColor, project->avatarColor);
            item->setData(workspace::ProjectAvatarVariant, project->avatarVariant);
            item->setData(workspace::ProjectAvatarType, project->avatarType);
        }
        item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
        if (id == currentId) {
            item->setCheckState(Qt::Checked);
            item->setData(Qt::AccessibleDescriptionRole, QStringLiteral("Текущий проект"));
        }
        if (id == m_projectChoice) {
            m_project->setCurrentItem(item);
            item->setData(TaskPickerDraft, id != currentId);
            if (id != currentId)
                item->setData(Qt::AccessibleDescriptionRole, QStringLiteral("Новый проект"));
        }
    };
    const auto *current = projectChoice(m_task.projectId);
    const auto currentName = m_task.projectId.isEmpty() ? QStringLiteral("Без проекта")
        : !m_task.project.isEmpty() ? m_task.project
        : current ? current->name : QStringLiteral("Текущий проект");
    add(currentName, currentId, current);
    for (const auto &project : m_projects)
        if (!project.id.isEmpty() && project.id != m_task.projectId)
            add(project.name, project.id, &project);
    if (m_canDetachProject && !m_task.projectId.isEmpty())
        add(QStringLiteral("Без проекта"), QStringLiteral("0"));
    fitProjectList();
    m_projectEmpty->setVisible(!m_project->count());
    m_project->setVisible(m_project->count() > 0);
    refreshProjectPill();
    updateApply();
    refreshAvatars();
    if (onAvatarsVisible)
        onAvatarsVisible();
}

void TaskDetailEditor::finish()
{
    for (auto *picker : {m_creator, m_assignee, m_observers})
        picker->dismiss();
    m_active = Field::None;
    hide();
    updateControls();
    if (onModeChanged)
        onModeChanged();
}

TaskEdit TaskDetailEditor::patch() const
{
    TaskEdit edit;
    switch (m_active) {
    case Field::Creator:
    case Field::Assignee: {
        const bool creator = m_active == Field::Creator;
        const auto &people = (creator ? m_creator : m_assignee)->people();
        if (!people.isEmpty() && !people.first().id.isEmpty()
            && people.first().id != (creator ? m_baseline.creatorId : m_baseline.assigneeId))
            (creator ? edit.creatorId : edit.assigneeId) = people.first().id;
        break;
    }
    case Field::Observers: {
        const auto selected = ids(m_observers->people());
        const auto baseline = ids(m_baseline.observerPeople);
        if (!samePeople(selected, baseline)) {
            edit.observerIds = selected;
            edit.originalObserverIds = baseline;
        }
        break;
    }
    case Field::Deadline:
        if (m_noDeadline->isChecked()) {
            if (!m_baseline.due.isEmpty())
                edit.deadline = QString();
        } else if (m_baseline.due.isEmpty() || m_deadline->dateTime() != m_deadlineInitially) {
            const auto date = m_deadline->dateTime();
            edit.deadline = date.toOffsetFromUtc(date.offsetFromUtc()).toString(Qt::ISODate);
        }
        if (edit.deadline && !m_deadlineReason->text().trimmed().isEmpty())
            edit.deadlineReason = m_deadlineReason->text().trimmed();
        break;
    case Field::Status:
        if (m_status->currentData().isValid())
            edit.statusAction = TaskStatusAction(m_status->currentData().toInt());
        break;
    case Field::Project:
        if (m_projectChoice != projectOption(m_baseline.projectId))
            edit.projectId = m_projectChoice;
        break;
    case Field::None:
        break;
    }
    return edit;
}

bool TaskDetailEditor::stillCurrent() const
{
    switch (m_active) {
    case Field::Creator: return m_baseline.creatorId == m_task.creatorId;
    case Field::Assignee: return m_baseline.assigneeId == m_task.assigneeId;
    case Field::Observers: return samePeople(ids(m_baseline.observerPeople), ids(m_task.observerPeople));
    case Field::Deadline: return m_baseline.due == m_task.due;
    case Field::Status:
        return m_baseline.statusCode == m_task.statusCode && m_baseline.statusActions == m_task.statusActions;
    case Field::Project: return m_baseline.projectId == m_task.projectId;
    case Field::None: return true;
    }
    return false;
}

const Project *TaskDetailEditor::projectChoice(const QString &id) const
{
    if (id.isEmpty() || id == QLatin1String("0"))
        return nullptr;
    const auto match = std::find_if(m_projects.cbegin(), m_projects.cend(), [&id](const Project &project) {
        return project.id == id;
    });
    return match == m_projects.cend() ? nullptr : &*match;
}

void TaskDetailEditor::refreshProjectPill()
{
    const auto *selected = projectChoice(m_projectChoice);
    const bool draft = m_projectChoice != projectOption(m_task.projectId);
    auto caption = draft ? m_projectChoiceName : m_task.project;
    if (caption.isEmpty() && selected)
        caption = selected->name;
    if (caption.isEmpty())
        caption = m_projectChoice == QLatin1String("0") ? QStringLiteral("Без проекта")
                                                        : QStringLiteral("Проект №%1").arg(m_projectChoice);
    m_projectField->setSelection({{m_projectChoice, caption,
        selected ? selected->avatarUrl : QUrl(), draft}});
}

void TaskDetailEditor::refreshSummary()
{
    const bool summaryVisible = m_active == Field::Deadline || m_active == Field::Status;
    m_summary->setVisible(summaryVisible);
    if (m_active == Field::Deadline)
        m_summary->setCurrent(m_task.due.isEmpty() ? QStringLiteral("Без срока") : formattedDate(m_task.due));
    else if (m_active == Field::Status)
        m_summary->setCurrent(m_task.status);
}

void TaskDetailEditor::showPending(const TaskEdit &edit)
{
    QString pending;
    if (edit.deadline)
        pending = edit.deadline->isEmpty() ? QStringLiteral("Без срока") : formattedDate(*edit.deadline);
    else if (edit.statusAction)
        pending = actionLabel(*edit.statusAction);
    m_summary->setPending(pending, edit.statusAction ? QStringLiteral("Действие") : QStringLiteral("Будет"));
}

void TaskDetailEditor::updateControls()
{
    m_editors->setEnabled(m_writable && !m_busy && allowed(m_active));
    if (m_busy)
        for (auto *picker : {m_creator, m_assignee, m_observers})
            picker->dismiss();
    m_cancel->setEnabled(!m_busy);
    updateApply();
}

QString TaskDetailEditor::conflictText() const
{
    QString confirmed;
    switch (m_active) {
    case Field::Creator: confirmed = m_task.createdBy; break;
    case Field::Assignee: confirmed = m_task.assignee; break;
    case Field::Observers:
        if (m_task.observers.isEmpty()) {
            confirmed = QStringLiteral("Без наблюдателей");
        } else {
            confirmed = m_task.observers.first().left(70);
            if (m_task.observers.size() > 1)
                confirmed += QStringLiteral(" и ещё %1").arg(m_task.observers.size() - 1);
        }
        break;
    case Field::Project:
        confirmed = m_task.project;
        if (confirmed.isEmpty())
            confirmed = m_task.projectId.isEmpty() ? QStringLiteral("Без проекта")
                                                  : QStringLiteral("Проект №%1").arg(m_task.projectId);
        break;
    default: break;
    }
    return confirmed.isEmpty() ? QStringLiteral("Поле изменилось на портале. Откройте его заново.")
        : QStringLiteral("Сейчас на портале: %1. Откройте поле заново.").arg(confirmed);
}

void TaskDetailEditor::updateApply()
{
    const bool current = stillCurrent();
    const bool needsReason = m_active == Field::Deadline && m_task.deadlineReasonRequired;
    m_deadlineReason->setVisible(needsReason);
    const auto edit = patch();
    showPending(edit);
    const bool projectChoiceListed = !edit.projectId || listedProject(*edit.projectId);
    const bool projectAvailable = !edit.projectId || (*edit.projectId == QLatin1String("0") && m_canDetachProject)
        || std::any_of(m_projects.cbegin(), m_projects.cend(),
                       [&edit](const Project &project) { return project.id == *edit.projectId; });
    bool mixedObservers = false;
    if (m_mode == Mode::ExistingTask && edit.observerIds && edit.originalObserverIds) {
        const auto hasExtra = [](const QStringList &from, const QStringList &other) {
            return std::any_of(from.cbegin(), from.cend(), [&other](const QString &id) {
                return !other.contains(id);
            });
        };
        mixedObservers = hasExtra(*edit.observerIds, *edit.originalObserverIds)
            && hasExtra(*edit.originalObserverIds, *edit.observerIds);
    }
    m_conflict->setText(!current ? conflictText()
                                : QStringLiteral("Добавление и удаление наблюдателей сохраните отдельно"));
    m_conflict->setVisible(!current || mixedObservers);
    const bool autoApply = appliesOnSelection();
    m_apply->setText(autoApply ? QStringLiteral("Повторить") : QStringLiteral("Применить"));
    m_apply->setAccessibleName(autoApply ? QStringLiteral("Повторить изменение реквизита")
                                        : QStringLiteral("Применить изменение реквизита"));
    m_apply->setVisible(!autoApply || (m_autoSubmitAttempted && !m_busy && edit.hasChanges()));
    m_apply->setEnabled(m_writable && !m_busy && allowed(m_active) && current && projectAvailable && projectChoiceListed
                        && !mixedObservers && edit.hasChanges() && (!needsReason || edit.deadlineReason));
}

} // namespace bx::ui
