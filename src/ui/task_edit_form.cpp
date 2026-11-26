#include "ui/task_edit_form.h"

#include "ui/task_description_editor.h"
#include "ui/task_dialog_controls.h"
#include "ui/task_fields.h"
#include "ui/task_picker_choices.h"
#include "ui/workspace_presentation.h"

#include <QComboBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QListWidget>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace bx::ui {

TaskEditForm::TaskEditForm(QWidget *parent)
    : QDialog(parent)
{
    setProperty("workspaceDialog", true);
    setWindowTitle(QStringLiteral("Новая задача"));
    setAccessibleName(QStringLiteral("Форма новой задачи"));
    setMinimumWidth(400);
    const auto *availableScreen = screen();
    resize(490, availableScreen ? qMin(630, availableScreen->availableGeometry().height() - 40) : 630);
    auto *layout = new QVBoxLayout(this);
    m_pages = new QStackedWidget(this);
    layout->addWidget(m_pages, 1);
    auto *mainPage = new QWidget(m_pages);
    auto *mainLayout = new QVBoxLayout(mainPage);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea(mainPage);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setFocusPolicy(Qt::NoFocus);
    auto *body = new QWidget(scroll);
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    scroll->setWidget(body);
    mainLayout->addWidget(scroll, 1);
    m_pages->addWidget(mainPage);
    m_detailEditor = new TaskDetailEditor(m_pages, TaskDetailEditor::Mode::NewTask);
    m_pages->addWidget(m_detailEditor);
    m_detailEditor->onModeChanged = [this] {
        if (m_detailEditor->active()) {
            m_pages->setCurrentWidget(m_detailEditor);
            m_detailEditor->fitChoiceLists();
            m_detailEditor->focusEditor();
        } else {
            m_pages->setCurrentIndex(0);
            syncDraftRequisites();
            if (m_requisiteFocus)
                m_requisiteFocus->setFocus(Qt::OtherFocusReason);
        }
    };
    m_detailEditor->onSave = [this](const TaskEdit &edit) {
        m_detailEditor->accept(m_draft);
        if (edit.projectId)
            loadStages();
    };
    auto *back = new QShortcut(QKeySequence(Qt::Key_Escape), m_detailEditor);
    back->setContext(Qt::WidgetWithChildrenShortcut);
    connect(back, &QShortcut::activated, m_detailEditor, &TaskDetailEditor::cancel);

    m_fields = new TaskFields(body);
    m_fields->descriptionEditor()->setCompactHeight();
    bodyLayout->addWidget(m_fields, 1);
    auto *requisites = new QFormLayout;
    requisites->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_assignee = addRequisite(requisites, body, QStringLiteral("Ответственный"), Field::Assignee);
    m_creator = addRequisite(requisites, body, QStringLiteral("Постановщик"), Field::Creator);
    m_project = addRequisite(requisites, body, QStringLiteral("Проект"), Field::Project);
    m_stage = new QComboBox(body);
    editor(m_stage);
    m_stage->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_stage->setAccessibleName(QStringLiteral("Стадия"));
    auto *stageRow = new QHBoxLayout;
    stageRow->addWidget(m_stage, 1);
    m_retryStages = button(QStringLiteral("Повторить"), QStringLiteral("Повторить загрузку стадий"), body);
    m_retryStages->hide();
    connect(m_retryStages, &QPushButton::clicked, this, [this] { loadStages(); });
    stageRow->addWidget(m_retryStages);
    requisites->addRow(workspace::label(QStringLiteral("Стадия"), "fieldCaption", body), stageRow);
    m_observers = addRequisite(requisites, body, QStringLiteral("Наблюдатели"), Field::Observers);
    bodyLayout->addLayout(requisites);

    m_fileQueue = new QWidget(body);
    auto *files = new QVBoxLayout(m_fileQueue);
    files->setContentsMargins(0, 0, 0, 0);
    files->setSpacing(5);
    files->addWidget(workspace::label(QStringLiteral("Файлы к задаче"), "fieldCaption", m_fileQueue));
    auto *fileRow = new QHBoxLayout;
    m_files = new QListWidget(m_fileQueue);
    m_files->setAccessibleName(QStringLiteral("Файлы к новой задаче"));
    m_files->setFixedHeight(70);
    fileRow->addWidget(m_files, 1);
    m_removeFile = button(QStringLiteral("Убрать"), QStringLiteral("Убрать выбранный файл"), m_fileQueue);
    fileRow->addWidget(m_removeFile, 0, Qt::AlignTop);
    files->addLayout(fileRow);
    bodyLayout->addWidget(m_fileQueue);
    m_fileQueue->hide();
    connect(m_files, &QListWidget::currentRowChanged, this, [this] { refreshFileActions(); });
    connect(m_removeFile, &QPushButton::clicked, this, [this] {
        const auto row = m_files->currentRow();
        if (m_removeFile->isEnabled() && row >= 0) {
            m_paths.removeAt(row);
            delete m_files->takeItem(row);
            refreshFileActions();
        }
    });
    m_fields->descriptionEditor()->setAttachHandler([this](const QStringList &paths) {
        if (!m_writable || !m_createdTask.isEmpty())
            return;
        bool unavailable = false;
        for (const auto &path : paths) {
            const QFileInfo file(path);
            if (!file.isFile() || !file.isReadable() || file.size() <= 0) {
                unavailable = true;
                continue;
            }
            const auto local = file.absoluteFilePath();
            if (!local.isEmpty() && !m_paths.contains(local)) {
                m_paths.append(local);
                m_files->addItem(file.fileName());
            }
        }
        if (m_files->currentRow() < 0 && !m_paths.isEmpty())
            m_files->setCurrentRow(0);
        setStatus(unavailable ? QStringLiteral("Недоступные и пустые файлы не добавлены.") : QString());
        refreshFileActions();
    });
    m_status = workspace::label(QString(), "quiet", mainPage);
    m_status->setAccessibleName(QStringLiteral("Состояние формы"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    mainLayout->addWidget(m_status);
    m_status->hide();
    auto *actions = new QHBoxLayout;
    actions->addStretch(1);
    m_cancel = button(QStringLiteral("Отмена"), QStringLiteral("Отмена"), this);
    connect(m_cancel, &QPushButton::clicked, this, &QDialog::reject);
    actions->addWidget(m_cancel);
    m_save = button(QStringLiteral("Сохранить"), QStringLiteral("Сохранить задачу"), this);
    m_save->setObjectName(QStringLiteral("primary"));
    connect(m_save, &QPushButton::clicked, this, [this] {
        const auto error = m_fields->validation();
        if (!error.isEmpty())
            setStatus(error);
        else if (m_saveHandler) {
            auto edit = m_fields->edit();
            if (!m_draft.creatorId.isEmpty())
                edit.creatorId = m_draft.creatorId;
            if (!m_draft.assigneeId.isEmpty())
                edit.assigneeId = m_draft.assigneeId;
            if (!m_draft.observerPeople.isEmpty()) {
                QStringList ids;
                for (const auto &person : m_draft.observerPeople)
                    ids.append(person.id);
                edit.observerIds = std::move(ids);
            }
            const auto stage = m_stage->currentData().toString();
            if (!stage.isEmpty())
                edit.stageId = stage;
            m_saveHandler(edit);
        }
    });
    actions->addWidget(m_save);
    mainLayout->addLayout(actions);
}

void TaskEditForm::configure(const QString &projectId, bool canDetachProject)
{
    m_draft.projectId = projectId;
    m_draft.editRights.assignee = true;
    m_draft.editRights.creator = true;
    m_draft.editRights.project = true;
    m_draft.editRights.observers = true;
    m_detailEditor->setCanDetachProject(canDetachProject);
    m_detailEditor->setTask(m_draft);
    setEnabledForWrite(m_writable);
    loadStages();
}

void TaskEditForm::setProjectChoices(const QVector<Project> &projects)
{
    for (const auto &project : projects) {
        if (project.id == m_draft.projectId) {
            m_draft.project = project.name;
            m_detailEditor->setTask(m_draft);
            break;
        }
    }
    m_detailEditor->setProjectChoices(projects);
    refreshRequisites();
}

void TaskEditForm::setMoreProjectsAvailable(bool available)
{
    m_detailEditor->setMoreProjectsAvailable(available);
    refreshRequisites();
}

void TaskEditForm::setOnMoreProjects(std::function<void()> handler)
{
    m_detailEditor->onMoreProjects = std::move(handler);
}

void TaskEditForm::setOnStages(std::function<void(const QString &)> handler)
{
    m_requestStages = std::move(handler);
}

void TaskEditForm::setPeopleSearch(const std::function<void(const QString &, const QString &)> &handler)
{
    m_detailEditor->setPeopleSearch(handler);
    refreshRequisites();
}

void TaskEditForm::setPeopleResults(const QString &request, const QVector<Person> &people)
{
    m_detailEditor->setPeopleResults(request, people);
}

void TaskEditForm::setPeopleSearchError(const QString &request)
{
    m_detailEditor->setPeopleSearchError(request);
}

void TaskEditForm::setCurrentPerson(Person person)
{
    m_currentPerson = std::move(person);
    // Let a person picker apply or cancel its in-progress choice before filling the draft.
    if (!m_detailEditor->active())
        syncDraftRequisites();
}

void TaskEditForm::setRequisiteAvatars(RequisiteAvatarSource source, std::function<void()> visibleChanged)
{
    m_detailEditor->onAvatarsVisible = std::move(visibleChanged);
    m_detailEditor->setAvatarSource(std::move(source));
}

QList<QUrl> TaskEditForm::visibleRequisiteAvatarUrls() const
{
    return m_detailEditor->visibleAvatarUrls();
}

void TaskEditForm::refreshRequisiteAvatars()
{
    m_detailEditor->refreshAvatars();
}

void TaskEditForm::setStages(const QVector<Stage> &stages)
{
    const auto selected = m_stage->currentData().toString();
    const QSignalBlocker blocker(m_stage);
    m_stage->clear();
    for (const auto &stage : stages) {
        if (!stage.id.isEmpty() && stage.id != QLatin1String("0"))
            m_stage->addItem(taskStageColorIcon(stage.color), stage.name, stage.id);
    }
    if (m_stage->count() == 0)
        m_stage->addItem(QStringLiteral("Нет стадий"));
    m_stage->setCurrentIndex(qMax(0, m_stage->findData(selected)));
    m_stagesLoading = false;
    m_retryStages->hide();
    refreshRequisites();
}

void TaskEditForm::setStagesError(const QString &reason)
{
    if (!m_stagesLoading)
        return;
    const QSignalBlocker blocker(m_stage);
    m_stage->clear();
    m_stage->addItem(QStringLiteral("Не удалось загрузить"));
    m_stagesLoading = false;
    m_retryStages->setToolTip(plainTooltip(reason));
    m_retryStages->show();
    refreshRequisites();
}

void TaskEditForm::setOnSave(std::function<void(const TaskEdit &)> handler)
{
    m_saveHandler = std::move(handler);
}

void TaskEditForm::setEnabledForWrite(bool writable, bool uploading)
{
    m_writable = writable;
    m_uploading = uploading;
    m_fields->setWritable(writable && m_createdTask.isEmpty());
    if (!writable && m_detailEditor->active())
        m_detailEditor->cancel();
    m_detailEditor->setWriteState(writable && m_createdTask.isEmpty(), false);
    refreshRequisites();
    m_cancel->setEnabled(!uploading);
    refreshFileActions();
}

void TaskEditForm::setStatus(const QString &text, bool loading)
{
    setStatusLabel(m_status, text, loading);
}

void TaskEditForm::setCreatedTask(const QString &task, const QString &projectToOpen)
{
    m_createdTask = task;
    m_projectToOpen = projectToOpen;
    m_save->setText(QStringLiteral("Повторить загрузку"));
    m_save->setAccessibleName(m_save->text());
    m_cancel->setText(QStringLiteral("Закрыть"));
    m_cancel->setAccessibleName(m_cancel->text());
}

void TaskEditForm::confirmFirstFile()
{
    if (!m_paths.isEmpty()) {
        m_paths.removeFirst();
        delete m_files->takeItem(0);
        refreshFileActions();
    }
}

void TaskEditForm::reject()
{
    if (!m_uploading)
        QDialog::reject();
}

QPushButton *TaskEditForm::addRequisite(QFormLayout *layout, QWidget *parent, const QString &caption, Field field)
{
    auto *control = button({}, caption, parent);
    control->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    connect(control, &QPushButton::clicked, this, [this, control, field] {
        m_requisiteFocus = control;
        m_detailEditor->open(field);
    });
    layout->addRow(workspace::label(caption, "fieldCaption", parent), control);
    return control;
}

void TaskEditForm::syncDraftRequisites()
{
    if (m_currentPerson && !m_currentPerson->id.isEmpty() && !m_currentPerson->name.isEmpty()) {
        const auto &person = *m_currentPerson;
        if (m_draft.assigneeId.isEmpty()) {
            m_draft.assigneeId = person.id;
            m_draft.assignee = person.name;
            m_draft.assigneeAvatarUrl = person.avatarUrl;
        }
        if (m_draft.creatorId.isEmpty()) {
            m_draft.creatorId = person.id;
            m_draft.createdBy = person.name;
            m_draft.creatorAvatarUrl = person.avatarUrl;
        }
    }
    m_detailEditor->setTask(m_draft);
    refreshRequisites();
}

void TaskEditForm::refreshRequisites()
{
    const auto profileStatus = m_currentPerson ? QStringLiteral("Профиль недоступен · Выбрать")
                                               : QStringLiteral("Загружаем профиль…");
    const auto setChoice = [this](QPushButton *button, Field field, const QString &value) {
        auto caption = value;
        caption.replace(QLatin1Char('&'), QStringLiteral("&&"));
        button->setText(caption);
        button->setToolTip(plainTooltip(value));
        button->setEnabled(m_detailEditor->canStart(field));
    };
    setChoice(m_assignee, Field::Assignee, m_draft.assignee.isEmpty() ? profileStatus : m_draft.assignee);
    setChoice(m_creator, Field::Creator, m_draft.createdBy.isEmpty() ? profileStatus : m_draft.createdBy);
    const auto project = m_draft.projectId.isEmpty() ? QStringLiteral("Без проекта")
        : m_draft.project.isEmpty() ? QStringLiteral("Проект №%1").arg(m_draft.projectId) : m_draft.project;
    setChoice(m_project, Field::Project, project);
    const auto people = m_draft.observers;
    const auto observers = people.isEmpty() ? QStringLiteral("Добавить")
        : people.size() == 1 ? people.first() : QStringLiteral("%1 и ещё %2").arg(people.first()).arg(people.size() - 1);
    setChoice(m_observers, Field::Observers, observers);
    if (!people.isEmpty())
        m_observers->setToolTip(plainTooltip(people.join(QStringLiteral(", "))));
    const bool editable = m_writable && m_createdTask.isEmpty();
    m_stage->setEnabled(editable && m_stage->currentData().isValid());
    m_retryStages->setEnabled(editable);
}

void TaskEditForm::loadStages()
{
    const QSignalBlocker blocker(m_stage);
    m_stage->clear();
    m_retryStages->hide();
    m_stagesLoading = !m_draft.projectId.isEmpty() && bool(m_requestStages);
    m_stage->addItem(m_stagesLoading ? QStringLiteral("Загружаем стадии…") : QStringLiteral("Нет стадий"));
    refreshRequisites();
    if (m_stagesLoading)
        m_requestStages(m_draft.projectId);
}

void TaskEditForm::refreshFileActions()
{
    m_fileQueue->setVisible(!m_paths.isEmpty());
    m_removeFile->setEnabled(m_writable && m_files->currentRow() >= 0);
    m_save->setEnabled(m_writable && (m_createdTask.isEmpty() || !m_paths.isEmpty()));
}

} // namespace bx::ui
