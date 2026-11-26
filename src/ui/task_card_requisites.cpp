#include "ui/task_card_requisites.h"

#include "ui/task_card_dialog.h"
#include "ui/task_dialog_controls.h"
#include "ui/task_picker_choices.h"

#include <QApplication>
#include <QFrame>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace bx::ui {

TaskCardRequisites::TaskCardRequisites(TaskCardDialog &dialog, QWidget *fields, QVBoxLayout *details,
                      QVBoxLayout *project, QVBoxLayout *observers)
    : QObject(&dialog), m_dialog(dialog)
{
    auto requisite = [](QVBoxLayout *group, const QString &caption,
                        std::optional<workspace::AvatarKind> kind = {}, bool multiple = false) {
        auto *row = new TaskRequisiteRow(caption, group->parentWidget(), kind, multiple);
        group->addWidget(row);
        return row;
    };
    using Field = TaskDetailEditor::Field;
    m_requisitePopup = new QFrame(&m_dialog);
    m_requisitePopup->setObjectName(QStringLiteral("taskRequisitePopup"));
    m_requisitePopup->installEventFilter(this);
    m_requisitePopupLayout = new QVBoxLayout(m_requisitePopup);
    m_requisitePopupLayout->setContentsMargins(11, 8, 11, 8);
    m_requisitePopup->hide();
    auto *popupEscape = new QShortcut(QKeySequence(Qt::Key_Escape), m_requisitePopup);
    popupEscape->setContext(Qt::WidgetWithChildrenShortcut);
    connect(popupEscape, &QShortcut::activated, this, [this] {
        if (m_stageOpen && m_cancelStage->isEnabled() && !hasStageDraft())
            closeStageEditor();
        else if (!m_stageOpen && !m_detailEditor->changed())
            m_detailEditor->cancel();
    });
    connect(m_dialog.m_detailsScroll->verticalScrollBar(), &QScrollBar::valueChanged,
            this, [this] { positionRequisitePopup(); });
    m_detailEditor = new TaskDetailEditor(fields);
    m_detailEditor->onSave = [this](const TaskEdit &edit) {
        if (!m_dialog.m_onSave)
            return;
        auto request = edit;
        request.requireDelegate = m_delegatingFromFooter && edit.assigneeId.has_value();
        m_dialog.m_onSave(request);
    };
    m_detailEditor->onAvatarsVisible = [this] {
        if (m_onRequisiteAvatarChoicesChanged)
            m_onRequisiteAvatarChoicesChanged();
    };
    m_detailEditor->onPickerLayoutChanged = [this] {
        QTimer::singleShot(0, m_requisitePopup, [this] {
            m_requisitePopupLayout->invalidate();
            positionRequisitePopup();
        });
    };
    m_detailEditor->onModeChanged = [this] {
        m_dialog.updateButtons();
        if (!m_detailEditor->active()) {
            m_delegatingFromFooter = false;
            hideRequisitePopup();
            if (m_activeDetail && m_activeDetail->isEnabled())
                m_activeDetail->setFocus(Qt::OtherFocusReason);
        }
    };
    auto connectRequisite = [this](TaskRequisiteRow *row, QVBoxLayout *group, Field field) {
        m_requisiteButtons.append({row, field});
        connect(row, &QPushButton::clicked, this, [this, row, group, field] {
            if (m_stageOpen || !m_detailEditor->canStart(field))
                return;
            m_activeDetail = row;
            if (auto *owner = m_detailEditor->parentWidget(); owner && owner->layout())
                owner->layout()->removeWidget(m_detailEditor);
            const bool picker = field != Field::Deadline && field != Field::Status;
            if (picker)
                openRequisitePopup(row, m_detailEditor);
            else
                group->insertWidget(group->indexOf(row) + 1, m_detailEditor);
            m_detailEditor->open(field);
            if (picker) {
                positionRequisitePopup();
                m_detailEditor->focusEditor();
            } else {
                scrollToRequisiteEditor(m_detailEditor);
            }
        });
    };
    m_creator = requisite(details, QStringLiteral("Постановщик"), workspace::AvatarKind::Person);
    connectRequisite(m_creator, details, Field::Creator);
    m_assignee = requisite(details, QStringLiteral("Исполнитель"), workspace::AvatarKind::Person);
    connectRequisite(m_assignee, details, Field::Assignee);
    m_deadline = requisite(details, QStringLiteral("Крайний срок"));
    connectRequisite(m_deadline, details, Field::Deadline);
    auto *divider = new QWidget(details->parentWidget());
    divider->setObjectName(QStringLiteral("taskRequisiteDivider"));
    divider->setFixedHeight(1);
    details->addWidget(divider);
    m_taskStatus = requisite(details, QStringLiteral("Статус"));
    connectRequisite(m_taskStatus, details, Field::Status);

    auto *createdRow = new QHBoxLayout;
    createdRow->setContentsMargins(8, 4, 8, 4);
    auto *createdCaption = workspace::label(QStringLiteral("Дата создания"), "fieldCaption", fields);
    createdCaption->setFixedWidth(100);
    createdRow->addWidget(createdCaption, 0, Qt::AlignTop);
    m_createdAt = workspace::label({}, "taskRequisiteValue", fields);
    m_createdAt->setAccessibleName(QStringLiteral("Дата создания"));
    m_createdAt->setTextInteractionFlags(Qt::TextSelectableByMouse);
    createdRow->addWidget(m_createdAt, 1);
    details->addLayout(createdRow);

    m_project = requisite(project, QStringLiteral("Проект"), workspace::AvatarKind::Project);
    connectRequisite(m_project, project, Field::Project);
    m_existingStage = requisite(project, QStringLiteral("Этап"));
    m_existingStage->setProperty("stageField", true);
    m_stageEditor = new QWidget(m_requisitePopup);
    auto *stageLayout = new QVBoxLayout(m_stageEditor);
    stageLayout->setContentsMargins(8, 3, 8, 4);
    stageLayout->setSpacing(5);
    m_stageSummary = new TaskPickerSummary(m_stageEditor);
    stageLayout->addWidget(m_stageSummary);
    m_stages = new QListWidget(m_stageEditor);
    m_stages->setItemDelegate(new TaskPickerChoiceDelegate(m_stages));
    m_stages->setObjectName(QStringLiteral("taskRequisiteChoices"));
    m_stages->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_stages->setIconSize(QSize(18, 18));
    m_stages->setAccessibleName(QStringLiteral("Этап задачи"));
    stageLayout->addWidget(m_stages);
    auto *stageActions = new QHBoxLayout;
    stageActions->addStretch(1);
    m_cancelStage = button(QStringLiteral("Отмена"), QStringLiteral("Отменить изменение этапа"), m_stageEditor);
    connect(m_cancelStage, &QPushButton::clicked, this, [this] { closeStageEditor(); });
    stageActions->addWidget(m_cancelStage);
    m_move = button(QStringLiteral("Повторить"), QStringLiteral("Повторить перенос задачи"), m_stageEditor);
    m_move->setObjectName(QStringLiteral("primary"));
    connect(m_move, &QPushButton::clicked, this, &TaskCardRequisites::submitStage);
    stageActions->addWidget(m_move);
    stageLayout->addLayout(stageActions);
    m_requisitePopupLayout->addWidget(m_stageEditor);
    connect(m_stages, &QListWidget::itemClicked, this, &TaskCardRequisites::selectStage);
    connect(m_stages, &QListWidget::itemActivated, this, &TaskCardRequisites::selectStage);
    connect(m_existingStage, &QPushButton::clicked, this, [this] {
        m_stageBaseline = m_dialog.m_task.stageId;
        m_stageChoice = m_stageBaseline;
        m_stageSubmitAttempted = false;
        m_stages->setCurrentItem(stageItem(m_stageBaseline));
        m_stageOpen = true;
        openRequisitePopup(m_existingStage, m_stageEditor);
        m_dialog.updateButtons();
        positionRequisitePopup();
        m_stages->setFocus(Qt::OtherFocusReason);
    });
    m_observers = requisite(observers, QStringLiteral("Наблюдатели"), workspace::AvatarKind::Person, true);
    connectRequisite(m_observers, observers, Field::Observers);
}

void TaskCardRequisites::showTask(const Task &task, bool changedProject)
{
    if (changedProject)
        m_projectCaption.clear();
    m_detailEditor->setTask(task);
    setRequisite(m_creator, personName(task.createdBy, task.creatorId), task.creatorAvatarUrl);
    setRequisite(m_assignee, personName(task.assignee, task.assigneeId), task.assigneeAvatarUrl);
    setRequisite(m_deadline, task.due.isEmpty()
        ? QStringLiteral("Без срока") : TaskDetailEditor::formattedDate(task.due));
    const auto deadline = QDateTime::fromString(task.due, Qt::ISODate);
    static const QStringList openStatuses{QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"),
        QStringLiteral("6"), QStringLiteral("pending"), QStringLiteral("in_progress"), QStringLiteral("deferred")};
    const bool overdue = openStatuses.contains(task.statusCode) && deadline.isValid()
        && (task.due.size() == 10 ? deadline.date() < QDate::currentDate()
                                 : deadline < QDateTime::currentDateTime());
    const auto deadlineStyle = overdue
        ? QStringLiteral("QLabel#taskRequisiteValue { color: #a3372d; font-weight: 600; }") : QString();
    if (m_deadline->styleSheet() != deadlineStyle)
        m_deadline->setStyleSheet(deadlineStyle);
    setRequisite(m_taskStatus, task.status);
    m_createdAt->setText(task.createdAt.isValid()
        ? task.createdAt.toLocalTime().toString(QStringLiteral("dd.MM.yyyy  HH:mm")) : QStringLiteral("—"));
    showRequisiteProject();
    setObservers(task);
    const auto *selected = changedProject ? nullptr : stageItem(task.stageId);
    setRequisite(m_existingStage, selected ? selected->text() : task.stageName);
    refreshRequisiteAvatars();
}

void TaskCardRequisites::syncStage(const Task &task, bool changedProject, const QString &previousStage)
{
    if (changedProject) {
        closeStageEditor();
        setStages({});
    } else if (previousStage != task.stageId) {
        const QSignalBlocker blocker(m_stages);
        if (auto *previous = stageItem(previousStage)) {
            previous->setData(Qt::CheckStateRole, {});
            previous->setData(Qt::AccessibleDescriptionRole, {});
        }
        if (auto *current = stageItem(task.stageId)) {
            current->setCheckState(Qt::Checked);
            current->setData(Qt::AccessibleDescriptionRole, QStringLiteral("Текущий этап"));
        }
        if (!m_stageOpen)
            m_stages->setCurrentItem(stageItem(task.stageId));
    }
}

void TaskCardRequisites::setStages(const QVector<Stage> &stages)
{
    const QSignalBlocker blocker(m_stages);
    m_stages->clear();
    for (const auto &stage : stages) {
        auto *option = new QListWidgetItem(stage.name, m_stages);
        option->setData(Qt::UserRole, stage.id);
        option->setFlags(option->flags() & ~Qt::ItemIsUserCheckable);
        option->setIcon(taskStageColorIcon(stage.color));
        if (stage.id == m_dialog.m_task.stageId) {
            option->setCheckState(Qt::Checked);
            option->setData(Qt::AccessibleDescriptionRole, QStringLiteral("Текущий этап"));
        }
    }
    fitStageList();
    auto *current = stageItem(m_dialog.m_task.stageId);
    auto *draft = stageItem(m_stageChoice);
    m_stages->setCurrentItem(m_stageOpen && draft ? draft : current);
    setRequisite(m_existingStage, current ? current->text() : m_dialog.m_task.stageName);
    m_dialog.updateButtons();
}

void TaskCardRequisites::setProjectCaption(const QString &caption)
{
    m_projectCaption = caption;
    showRequisiteProject();
    refreshRequisiteAvatars();
}

void TaskCardRequisites::cancelFooterDelegation()
{
    if (m_delegatingFromFooter)
        m_detailEditor->cancel();
}

void TaskCardRequisites::setProjectChoices(const QVector<Project> &projects)
{
    m_requisiteProjects = projects;
    m_detailEditor->setProjectChoices(projects);
    showRequisiteProject();
    refreshRequisiteAvatars();
    m_dialog.updateButtons();
}

void TaskCardRequisites::setCanDetachProject(bool allowed)
{
    m_detailEditor->setCanDetachProject(allowed);
    m_dialog.updateButtons();
}

void TaskCardRequisites::setMoreProjectsAvailable(bool available)
{
    m_detailEditor->setMoreProjectsAvailable(available);
    m_dialog.updateButtons();
}

void TaskCardRequisites::setRequisiteAvatars(TaskRequisiteRow::AvatarSource source)
{
    m_requisiteAvatars = std::move(source);
    m_detailEditor->setAvatarSource(m_requisiteAvatars);
    refreshRequisiteAvatars();
}

void TaskCardRequisites::setOnRequisiteAvatarChoicesChanged(std::function<void()> changed)
{
    m_onRequisiteAvatarChoicesChanged = std::move(changed);
}

QList<QUrl> TaskCardRequisites::visibleRequisiteAvatarUrls() const
{
    QList<QUrl> urls;
    urls.append(m_detailEditor->visibleAvatarUrls());
    for (const auto *row : {m_creator, m_assignee, m_project, m_observers})
        urls.append(row->visibleAvatarUrls(m_dialog.m_detailsScroll->viewport()));
    return urls;
}

void TaskCardRequisites::refreshRequisiteAvatars()
{
    for (auto *row : {m_creator, m_assignee, m_project, m_observers})
        row->refreshAvatars(m_requisiteAvatars);
    m_detailEditor->refreshAvatars();
}

void TaskCardRequisites::setOnMoreProjects(std::function<void()> handler)
{
    m_detailEditor->onMoreProjects = std::move(handler);
}

void TaskCardRequisites::setPeopleSearch(const std::function<void(const QString &, const QString &)> &handler)
{
    m_detailEditor->setPeopleSearch(handler);
    m_dialog.updateButtons();
}

void TaskCardRequisites::setPeopleResults(const QString &id, const QVector<Person> &people)
{
    m_detailEditor->setPeopleResults(id, people);
}

bool TaskCardRequisites::setPeopleSearchError(const QString &id)
{
    return m_detailEditor->setPeopleSearchError(id);
}

void TaskCardRequisites::setOnMove(std::function<void(const QString &)> handler)
{
    m_onMove = std::move(handler);
}

QString TaskCardRequisites::personName(const QString &name, const QString &id)
{
    return name.isEmpty() && !id.isEmpty() ? QStringLiteral("Сотрудник №%1").arg(id) : name;
}

void TaskCardRequisites::setRequisite(TaskRequisiteRow *row, const QString &text, const QUrl &url)
{
    row->setEntries(text.isEmpty() ? QVector<TaskRequisiteRow::Entry>() : QVector<TaskRequisiteRow::Entry>{{text, url}});
}

void TaskCardRequisites::setObservers(const Task &task)
{
    QVector<TaskRequisiteRow::Entry> observers;
    QStringList remaining = task.observers;
    for (const auto &person : task.observerPeople) {
        const auto name = personName(person.name, person.id);
        if (!name.isEmpty())
            observers.append({name, person.avatarUrl});
        remaining.removeOne(person.name);
    }
    for (const auto &name : std::as_const(remaining)) {
        if (!name.isEmpty())
            observers.append({name, {}});
    }
    m_observers->setEntries(std::move(observers));
}

void TaskCardRequisites::showRequisiteProject()
{
    const auto item = std::find_if(m_requisiteProjects.cbegin(), m_requisiteProjects.cend(), [this](const Project &project) {
        return !m_dialog.m_projectId.isEmpty() && project.id == m_dialog.m_projectId;
    });
    auto name = m_dialog.m_task.project.isEmpty() ? m_projectCaption : m_dialog.m_task.project;
    if (name.isEmpty() && item != m_requisiteProjects.cend())
        name = item->name;
    if (name.isEmpty() || item == m_requisiteProjects.cend()) {
        setRequisite(m_project, name);
        return;
    }
    m_project->setEntries({{name, item->avatarUrl, item->avatarColor, item->avatarVariant, item->avatarType}});
}

QListWidgetItem *TaskCardRequisites::stageItem(const QString &id) const
{
    if (!id.isEmpty()) {
        for (int i = 0; i < m_stages->count(); ++i)
            if (m_stages->item(i)->data(Qt::UserRole).toString() == id)
                return m_stages->item(i);
    }
    return nullptr;
}

void TaskCardRequisites::fitStageList()
{
    const int visibleRows = m_dialog.height() >= 570 ? 7 : 4;
    m_stages->setFixedHeight(qMax(40, qMin(visibleRows, m_stages->count()) * TaskPickerChoiceDelegate::RowHeight + 5));
}

void TaskCardRequisites::updateStageSummary()
{
    const auto *confirmed = stageItem(m_dialog.m_task.stageId);
    const auto *candidate = stageItem(m_stageChoice);
    m_stageSummary->setCurrent(confirmed ? confirmed->text() : m_dialog.m_task.stageName,
                               confirmed ? confirmed->icon() : QIcon());
    const bool draft = candidate && candidate != confirmed;
    m_stageSummary->setPending(draft ? candidate->text() : QString());
    for (int i = 0; i < m_stages->count(); ++i) {
        auto *row = m_stages->item(i);
        row->setData(TaskPickerDraft, draft && row == candidate);
        row->setData(Qt::AccessibleDescriptionRole, row == confirmed ? QStringLiteral("Текущий этап")
                     : draft && row == candidate ? QStringLiteral("Новый этап") : QString());
    }
}

void TaskCardRequisites::selectStage(QListWidgetItem *item)
{
    if (!m_stageOpen || !m_stages->isEnabled() || !item)
        return;
    const auto choice = item->data(Qt::UserRole).toString();
    if (choice.isEmpty() || choice == m_stageChoice)
        return;
    m_stageChoice = choice;
    m_dialog.updateButtons();
    submitStage();
}

void TaskCardRequisites::submitStage()
{
    if (!m_onMove || !m_move->isEnabled())
        return;
    m_stageSubmitAttempted = true;
    m_dialog.updateButtons();
    m_onMove(m_stageChoice);
}

bool TaskCardRequisites::hasStageDraft() const
{
    return m_stageOpen && !m_stageChoice.isEmpty() && m_stageChoice != m_stageBaseline;
}

void TaskCardRequisites::openRequisitePopup(TaskRequisiteRow *row, QWidget *editor)
{
    m_popupAnchor = row;
    m_dialog.m_detailsScroll->ensureWidgetVisible(row, 4, 4);
    if (m_requisitePopupLayout->indexOf(editor) < 0)
        m_requisitePopupLayout->addWidget(editor);
    m_requisitePopup->setAccessibleName(row->accessibleName());
    m_requisitePopup->show();
    m_requisitePopup->raise();
}

void TaskCardRequisites::positionRequisitePopup()
{
    if (!m_popupAnchor || !m_requisitePopup->isVisible())
        return;
    const QRect bounds = m_dialog.rect().adjusted(9, 9, -9, -9);
    const QRect row(m_popupAnchor->mapTo(&m_dialog, QPoint()), m_popupAnchor->size());
    const int width = qMin(bounds.width(), qBound(280, row.width() + 35, 475));
    const int height = qMin(bounds.height(), m_requisitePopup->sizeHint().height());
    const int below = row.bottom() + 5;
    const int y = below + height <= bounds.bottom() ? below : row.top() - height - 5;
    m_requisitePopup->setGeometry(qBound(bounds.left(), row.left(), bounds.right() - width + 1),
                                 qBound(bounds.top(), y, bounds.bottom() - height + 1), width, height);
}

void TaskCardRequisites::hideRequisitePopup()
{
    m_popupAnchor = nullptr;
    m_requisitePopup->hide();
}

void TaskCardRequisites::scrollToRequisiteEditor(QWidget *editor)
{
    QTimer::singleShot(0, editor, [this, editor] {
        if (!editor->isVisible())
            return;
        auto *focus = QApplication::focusWidget();
        m_dialog.m_detailsScroll->ensureWidgetVisible(focus && editor->isAncestorOf(focus) ? focus : editor, 8, 8);
    });
}

void TaskCardRequisites::closeStageEditor()
{
    if (!m_stageOpen)
        return;
    m_stageOpen = false;
    m_stageBaseline.clear();
    m_stageChoice.clear();
    m_stageSubmitAttempted = false;
    hideRequisitePopup();
    m_dialog.updateButtons();
    if (m_existingStage->isEnabled())
        m_existingStage->setFocus(Qt::OtherFocusReason);
}

void TaskCardRequisites::resizePopup()
{
    if (m_popupAnchor) {
        m_detailEditor->fitChoiceLists();
        fitStageList();
    }
    positionRequisitePopup();
}

bool TaskCardRequisites::eventFilter(QObject *target, QEvent *event)
{
    if (target == m_requisitePopup && event->type() == QEvent::LayoutRequest)
        positionRequisitePopup();
    return QObject::eventFilter(target, event);
}

void TaskCardRequisites::startDelegation()
{
    m_delegatingFromFooter = true;
    m_assignee->click();
    if (!m_detailEditor->active())
        m_delegatingFromFooter = false;
}

void TaskCardRequisites::updateWriteState()
{
    const auto &rights = m_dialog.m_task.editRights;
    m_detailEditor->setWriteState(m_dialog.m_writable && !m_dialog.m_editMode && !m_stageOpen, m_dialog.m_editing || m_dialog.m_moving);
    for (const auto &[button, field] : m_requisiteButtons)
        button->setEditable(m_detailEditor->canStart(field));
    const bool mayMove = m_dialog.m_writable && rights.stage && !m_dialog.m_moving && !m_dialog.m_editing && !m_dialog.m_editMode
        && !m_detailEditor->active() && m_stages->count() > 0;
    const bool stageCurrent = m_dialog.m_task.stageId == m_stageBaseline;
    m_existingStage->setEditable(mayMove && !m_stageOpen);
    m_stageEditor->setVisible(m_stageOpen);
    if (m_stageOpen)
        updateStageSummary();
    m_stages->setEnabled(m_stageOpen && mayMove);
    m_cancelStage->setEnabled(m_stageOpen && !m_dialog.m_moving && !m_dialog.m_editing);
    m_move->setVisible(hasStageDraft() && m_stageSubmitAttempted && !m_dialog.m_moving && !m_dialog.m_editing);
    m_move->setEnabled(m_stageOpen && mayMove && stageCurrent && hasStageDraft() && stageItem(m_stageChoice));
    m_move->setToolTip(stageCurrent ? QString() : QStringLiteral("Этап изменился на портале. Откройте поле заново."));
}

} // namespace bx::ui
