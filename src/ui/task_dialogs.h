#pragma once

#include "ui/workspace_presentation.h"
#include "ui/chat_widgets.h"
#include "ui/task_description_editor.h"
#include "ui/task_detail_editor.h"
#include "ui/task_relations.h"
#include "ui/typography.h"

#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QColorDialog>
#include <QDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QMessageBox>
#include <QTimer>
#include <QSplitter>
#include <QSettings>
#include <QScrollArea>
#include <QScreen>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QShortcut>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <functional>
#include <utility>

namespace bx::ui {

inline QString editorStyles()
{
    return QStringLiteral(R"(
        QDialog[workspaceDialog="true"] { background: #f4f4ef; }
        QLineEdit#workspaceEditor, QPlainTextEdit#workspaceEditor, QComboBox#workspaceEditor, QDateTimeEdit#workspaceEditor {
            color: #182d3b; background: #fffefa; border: 1px solid #cad7d7;
            border-radius: 7px; padding: 5px 7px;
            selection-background-color: #215c70; selection-color: #fffefa;
        }
        QLineEdit#workspaceEditor:focus, QPlainTextEdit#workspaceEditor:focus,
        QComboBox#workspaceEditor:focus { border-color: #215c70; }
        QLineEdit#workspaceEditor:read-only, QPlainTextEdit#workspaceEditor:read-only { background: #f5f5ef; }
        QLabel#fieldCaption { color: #435d68; font-size: 14px; font-weight: 600; }
        QWidget#taskSection { background: #fffefa; border: 1px solid #dfe7e0; border-radius: 9px; }
        QWidget#taskDetailsPane, QScrollArea#taskDetailsScroll { background: #f4f4ef; border: 0; }
        QLabel#taskCardHeading { color: #182d3b; font-size: 20px; font-weight: 650; }
        QLabel#taskSectionHeading { color: #215c70; font-size: 17px; font-weight: 650; }
        QPushButton#taskRequisiteRow { background: #f4f8f5; border: 1px solid #dce7e3; border-radius: 6px; }
        QPushButton#taskRequisiteRow:hover { background: #e7f1ed; border-color: #96b6ad; }
        QPushButton#taskRequisiteRow:focus { border-color: #215c70; }
        QPushButton#taskRequisiteRow:disabled { background: transparent; border-color: transparent; }
        QLabel#taskRequisiteValue { color: #182d3b; }
        QPushButton#taskRequisiteRow[stageField="true"]:enabled QLabel#taskRequisiteValue { color: #215c70; font-weight: 600; }
        QWidget#taskRequisiteDivider { background: #dfe7e0; }
        QFrame#taskRequisitePopup { background: #fffefa; border: 1px solid #9fbdb4; border-radius: 8px; }
        QListWidget#taskRequisiteChoices { background: #fffefa; border: 0; outline: none; color: #182d3b; }
        QWidget#taskPickerCurrent { background: #e8f2ed; border: 1px solid #cfe1d9; border-radius: 7px; }
        QWidget#taskPickerPending { background: #edf3f6; border: 1px solid #d5e2e8; border-radius: 7px; }
        QLabel#taskPickerCaption { color: #435d68; font-size: 14px; font-weight: 600; }
        QLabel#taskPickerValue { color: #182d3b; font-weight: 600; }
        QLabel#taskCopyFeedback { color: #215c70; font-size: 14px; }
        QFrame#taskSelectionField { background: #fffefa; border: 1px solid #8fb3a9; border-radius: 7px; }
        QLineEdit#taskSelectionSearch { background: transparent; border: 0; color: #182d3b; }
        QScrollArea#taskInlineSelectionScroll, QWidget#taskInlineSelectionItems { background: transparent; border: 0; }
        QWidget#taskInlineSelectionPill { background: #e3f0e9; border-radius: 5px; }
        QWidget#taskInlineSelectionPill[pending="true"] { background: #dfedf4; }
        QLabel#taskInlineSelectionValue { color: #173b43; font-weight: 600; }
        QWidget#taskInlineDraftDot { background: #28657b; border-radius: 3px; }
        QPushButton#taskInlineSelectionRemove { padding: 0; background: transparent; border: 0;
                                               color: #2f5b63; font-size: 14px; }
        QPushButton#taskInlineSelectionRemove:hover, QPushButton#taskInlineSelectionRemove:focus {
            background: #c8e0d6; border: 1px solid #759f92; }
        QListWidget#taskContent { background: transparent; }
        QSplitter#taskCardSplitter::handle:hover { background: #83aba5; }
        QPushButton#viewToggle:checked { border-color: #215c70; background: #dcecf0; }
    )");
}

inline void editor(QWidget *widget)
{
    widget->setObjectName(QStringLiteral("workspaceEditor"));
    auto palette = widget->palette();
    palette.setColor(QPalette::PlaceholderText, workspace::Muted);
    widget->setPalette(palette);
}

inline QPushButton *button(const QString &title, const QString &name, QWidget *parent)
{
    auto *result = new QPushButton(title, parent);
    result->setAccessibleName(name);
    result->setAutoDefault(false);
    return result;
}

inline void setStatusLabel(QLabel *label, const QString &text, bool loading = false)
{
    label->setText(text);
    setLoadingIndicator(label, loading && !text.isEmpty());
    label->setVisible(!text.isEmpty());
}

class TaskFields final : public QWidget {
public:
    explicit TaskFields(QWidget *parent, bool includeDeadline = true) : QWidget(parent)
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(5);
        layout->addWidget(workspace::label(QStringLiteral("Название"), "fieldCaption", this));
        m_title = new QLineEdit(this);
        editor(m_title);
        m_title->setAccessibleName(QStringLiteral("Заголовок задачи"));
        layout->addWidget(m_title);

        layout->addWidget(workspace::label(QStringLiteral("Описание"), "fieldCaption", this));
        m_description = new TaskDescriptionEditor(this);
        m_description->setAccessibleName(QStringLiteral("Описание задачи"));
        m_description->setMinimumHeight(62);
        layout->addWidget(m_description, 1);

        if (includeDeadline) {
            layout->addWidget(workspace::label(QStringLiteral("Срок"), "fieldCaption", this));
            auto *deadline = new QHBoxLayout;
            m_deadline = new QLineEdit(this);
            editor(m_deadline);
            m_deadline->setAccessibleName(QStringLiteral("Срок задачи"));
            m_deadline->setPlaceholderText(QStringLiteral("ГГГГ-ММ-ДД"));
            deadline->addWidget(m_deadline, 1);
            m_reset = button(QStringLiteral("Сбросить"), QStringLiteral("Сбросить срок"), this);
            connect(m_reset, &QPushButton::clicked, m_deadline, &QLineEdit::clear);
            deadline->addWidget(m_reset);
            layout->addLayout(deadline);
        }

        m_priority = new QCheckBox(QStringLiteral("Высокий приоритет"), this);
        m_priority->setAccessibleName(QStringLiteral("Высокий приоритет"));
        layout->addWidget(m_priority);
    }

    void setTask(const Task &task, bool reset = false)
    {
        const auto pending = reset ? TaskEdit{} : edit();
        const auto originalDescription = m_initial.description;
        const auto checksum = m_initial.descriptionChecksum;
        m_initial = task;
        if (pending.description && m_description->bbCode() != task.description) {
            m_initial.description = originalDescription;
            m_initial.descriptionChecksum = checksum;
        }
        if (!pending.title)
            m_title->setText(task.name);
        if (!pending.description)
            m_description->setBbCode(task.description);
        if (m_deadline && !pending.deadline)
            m_deadline->setText(task.due);
        if (!pending.highPriority)
            m_priority->setChecked(highPriority(task));
    }

    TaskEdit edit() const
    {
        TaskEdit result;
        if (m_title->text().trimmed() != m_initial.name)
            result.title = m_title->text().trimmed();
        if (m_description->bbCode() != m_initial.description) {
            result.description = m_description->bbCode();
            result.originalDescription = m_initial.description;
            if (!m_initial.descriptionChecksum.isEmpty())
                result.descriptionChecksum = m_initial.descriptionChecksum;
        }
        if (m_deadline && m_deadline->text().trimmed() != m_initial.due)
            result.deadline = m_deadline->text().trimmed();
        if (m_priority->isChecked() != highPriority(m_initial))
            result.highPriority = m_priority->isChecked();
        return result;
    }

    QString validation() const
    {
        if (m_title->text().trimmed().isEmpty())
            return QStringLiteral("Укажите название задачи");
        if (!m_deadline)
            return {};
        const auto value = m_deadline->text().trimmed();
        if (value != m_initial.due && !value.isEmpty() &&
            !QDate::fromString(value, Qt::ISODate).isValid() &&
            !QDateTime::fromString(value, Qt::ISODate).isValid())
            return QStringLiteral("Укажите дату в формате ГГГГ-ММ-ДД");
        return {};
    }

    void setWritable(bool writable, const TaskEditRights *rights = nullptr)
    {
        m_title->setReadOnly(!writable || (rights && !rights->title));
        m_description->setEditEnabled(writable && (!rights || rights->description));
        if (m_deadline) {
            m_deadline->setReadOnly(!writable || (rights && !rights->deadline));
            m_reset->setEnabled(!m_deadline->isReadOnly());
        }
        m_priority->setEnabled(writable && (!rights || rights->priority));
    }

    TaskDescriptionEditor *descriptionEditor() const { return m_description; }

private:
    static bool highPriority(const Task &task)
    {
        return task.priority.compare(QStringLiteral("Высокий"), Qt::CaseInsensitive) == 0
            || task.priority == QStringLiteral("high") || task.priority == QStringLiteral("2");
    }

    Task m_initial;
    QLineEdit *m_title = nullptr;
    TaskDescriptionEditor *m_description = nullptr;
    QLineEdit *m_deadline = nullptr;
    QPushButton *m_reset = nullptr;
    QCheckBox *m_priority = nullptr;
};

class TaskEditForm final : public QDialog {
public:
    using Field = TaskDetailEditor::Field;

    explicit TaskEditForm(QWidget *parent = nullptr) : QDialog(parent)
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

    void configure(const QString &projectId, bool canDetachProject)
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
    const QString &projectId() const { return m_draft.projectId; }
    void setProjectChoices(const QVector<Project> &projects)
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
    void setMoreProjectsAvailable(bool available)
    {
        m_detailEditor->setMoreProjectsAvailable(available);
        refreshRequisites();
    }
    void setOnMoreProjects(std::function<void()> handler) { m_detailEditor->onMoreProjects = std::move(handler); }
    void setOnStages(std::function<void(const QString &)> handler) { m_requestStages = std::move(handler); }
    void setPeopleSearch(const std::function<void(const QString &, const QString &)> &handler)
    {
        m_detailEditor->setPeopleSearch(handler);
        refreshRequisites();
    }
    void setPeopleResults(const QString &request, const QVector<Person> &people)
    {
        m_detailEditor->setPeopleResults(request, people);
    }
    void setPeopleSearchError(const QString &request) { m_detailEditor->setPeopleSearchError(request); }
    void setCurrentPerson(Person person)
    {
        m_currentPerson = std::move(person);
        // Let a person picker apply or cancel its in-progress choice before filling the draft.
        if (!m_detailEditor->active())
            syncDraftRequisites();
    }
    void setRequisiteAvatars(RequisiteAvatarSource source, std::function<void()> visibleChanged)
    {
        m_detailEditor->onAvatarsVisible = std::move(visibleChanged);
        m_detailEditor->setAvatarSource(std::move(source));
    }
    QList<QUrl> visibleRequisiteAvatarUrls() const { return m_detailEditor->visibleAvatarUrls(); }
    void refreshRequisiteAvatars() { m_detailEditor->refreshAvatars(); }

    void setStages(const QVector<Stage> &stages)
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
    void setStagesError(const QString &reason)
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

    void setOnSave(std::function<void(const TaskEdit &)> handler) { m_saveHandler = std::move(handler); }
    void setEnabledForWrite(bool writable, bool uploading = false)
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
    void setStatus(const QString &text, bool loading = false) { setStatusLabel(m_status, text, loading); }
    void setCreatedTask(const QString &task, const QString &projectToOpen)
    {
        m_createdTask = task;
        m_projectToOpen = projectToOpen;
        m_save->setText(QStringLiteral("Повторить загрузку"));
        m_save->setAccessibleName(m_save->text());
        m_cancel->setText(QStringLiteral("Закрыть"));
        m_cancel->setAccessibleName(m_cancel->text());
    }
    const QString &createdTask() const { return m_createdTask; }
    const QString &projectToOpen() const { return m_projectToOpen; }
    const QStringList &queuedFiles() const { return m_paths; }
    void confirmFirstFile()
    {
        if (!m_paths.isEmpty()) {
            m_paths.removeFirst();
            delete m_files->takeItem(0);
            refreshFileActions();
        }
    }
    void reject() override
    {
        if (!m_uploading)
            QDialog::reject();
    }

private:
    QPushButton *addRequisite(QFormLayout *layout, QWidget *parent, const QString &caption, Field field)
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

    void syncDraftRequisites()
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

    void refreshRequisites()
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

    void loadStages()
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

    void refreshFileActions()
    {
        m_fileQueue->setVisible(!m_paths.isEmpty());
        m_removeFile->setEnabled(m_writable && m_files->currentRow() >= 0);
        m_save->setEnabled(m_writable && (m_createdTask.isEmpty() || !m_paths.isEmpty()));
    }

    TaskFields *m_fields = nullptr;
    QStackedWidget *m_pages = nullptr;
    TaskDetailEditor *m_detailEditor = nullptr;
    QPushButton *m_assignee = nullptr;
    QPushButton *m_creator = nullptr;
    QPushButton *m_project = nullptr;
    QPushButton *m_observers = nullptr;
    QPushButton *m_requisiteFocus = nullptr;
    QComboBox *m_stage = nullptr;
    QPushButton *m_retryStages = nullptr;
    Task m_draft;
    std::optional<Person> m_currentPerson;
    bool m_stagesLoading = false;
    QWidget *m_fileQueue = nullptr;
    QListWidget *m_files = nullptr;
    QPushButton *m_removeFile = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_save = nullptr;
    QPushButton *m_cancel = nullptr;
    QStringList m_paths;
    QString m_createdTask;
    QString m_projectToOpen;
    bool m_writable = true;
    bool m_uploading = false;
    std::function<void(const TaskEdit &)> m_saveHandler;
    std::function<void(const QString &)> m_requestStages;
};

class TaskContentList final : public QListWidget {
public:
    TaskContentList(const QString &name, QWidget *parent) : QListWidget(parent)
    {
        setObjectName(QStringLiteral("taskContent"));
        setAccessibleName(name);
        workspace::configureRows(this, workspace::Row::TaskContent);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFixedHeight(0);
        connect(model(), &QAbstractItemModel::rowsInserted, this, [this] { requestHeight(); });
        connect(model(), &QAbstractItemModel::rowsRemoved, this, [this] { requestHeight(); });
        connect(model(), &QAbstractItemModel::modelReset, this, [this] { requestHeight(); });
    }

    void requestHeight()
    {
        if (m_queued)
            return;
        m_queued = true;
        QTimer::singleShot(0, this, [this] {
            m_queued = false;
            int height = 0;
            for (int row = 0; row < count(); ++row)
                height += qMax(0, sizeHintForRow(row));
            if (minimumHeight() != height || maximumHeight() != height)
                setFixedHeight(height);
            doItemsLayout();
            if (m_onLayout)
                m_onLayout();
        });
    }
    void setOnLayout(std::function<void()> handler) { m_onLayout = std::move(handler); }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QListWidget::resizeEvent(event);
        requestHeight();
    }

private:
    bool m_queued = false;
    std::function<void()> m_onLayout;
};

class TaskRequisiteRow final : public QPushButton {
public:
    struct Entry {
        QString name;
        QUrl url;
        QString projectColor{};
        QString projectVariant{};
        QString projectType{};

        bool operator==(const Entry &other) const
        {
            return name == other.name && url == other.url && projectColor == other.projectColor
                && projectVariant == other.projectVariant && projectType == other.projectType;
        }
    };
    using AvatarSource = std::function<const QPixmap *(const QUrl &)>;

    TaskRequisiteRow(const QString &caption, QWidget *parent, std::optional<workspace::AvatarKind> kind = {},
                     bool multiple = false)
        : QPushButton(parent), m_caption(caption), m_kind(kind), m_multiple(multiple)
    {
        setObjectName(QStringLiteral("taskRequisiteRow"));
        setAccessibleName(caption);
        setAutoDefault(false);
        auto policy = QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
        auto *line = new QHBoxLayout(this);
        line->setContentsMargins(8, 4, 8, 4);
        line->setSpacing(9);
        auto *label = workspace::label(caption, "fieldCaption", this);
        label->setFixedWidth(100);
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        line->addWidget(label, 0, Qt::AlignTop);
        auto *content = new QWidget(this);
        content->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_values = new QVBoxLayout(content);
        m_values->setContentsMargins(0, 0, 0, 0);
        m_values->setSpacing(4);
        line->addWidget(content, 1);
    }

    QSize sizeHint() const override { return layout()->sizeHint(); }
    QSize minimumSizeHint() const override { return layout()->minimumSize(); }
    int heightForWidth(int width) const override { return layout()->totalHeightForWidth(width); }

    void setEditable(bool editable)
    {
        setEnabled(editable);
        setCursor(editable ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }

    void setEntries(QVector<Entry> entries)
    {
        if (m_values->count() && m_entries == entries)
            return;
        m_entries = std::move(entries);
        while (auto *item = m_values->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        m_avatars.clear();
        auto display = m_entries;
        if (display.isEmpty())
            display.append({QStringLiteral("—"), {}});
        QStringList descriptions;
        for (const auto &entry : display) {
            const auto &name = entry.name;
            auto *line = new QWidget(m_values->parentWidget());
            auto *parts = new QHBoxLayout(line);
            parts->setContentsMargins(0, 0, 0, 0);
            parts->setSpacing(5);
            const auto color = QColor(entry.projectColor);
            const bool mark = m_kind != workspace::AvatarKind::Project
                || hasRequisiteProjectMark(entry.url, color, entry.projectVariant, entry.projectType);
            if (m_kind && !m_entries.isEmpty() && (mark || !entry.url.isEmpty())) {
                auto *avatar = new AvatarBadge(line);
                avatar->setObjectName(QStringLiteral("taskRequisiteAvatar"));
                avatar->setFixedSize(21, 21);
                avatar->setTitle(name, *m_kind, color, entry.projectVariant, entry.projectType);
                avatar->setVisible(mark);
                parts->addWidget(avatar, 0, Qt::AlignTop);
                m_avatars.append({avatar, entry.url, mark});
            }
            auto *value = workspace::label(name, "taskRequisiteValue", line);
            auto policy = value->sizePolicy();
            policy.setHorizontalPolicy(QSizePolicy::Ignored);
            value->setSizePolicy(policy);
            value->setAccessibleName(m_multiple ? name : m_caption);
            parts->addWidget(value, 1);
            m_values->addWidget(line);
            descriptions.append(name);
        }
        setAccessibleDescription(descriptions.join(QStringLiteral(", ")));
        updateGeometry();
    }

    QList<QUrl> visibleAvatarUrls(const QWidget *viewport) const
    {
        QList<QUrl> urls;
        for (const auto &[avatar, url, mark] : m_avatars) {
            const auto *target = mark ? avatar : avatar->parentWidget();
            if (!url.isEmpty() && target->isVisibleTo(viewport)
                && viewport->rect().intersects(QRect(target->mapTo(viewport, QPoint()), target->size())))
                urls.append(url);
        }
        return urls;
    }

    void refreshAvatars(const AvatarSource &source)
    {
        for (const auto &[avatar, url, mark] : m_avatars) {
            const auto *photo = source && !url.isEmpty() ? source(url) : nullptr;
            avatar->setPhoto(photo ? *photo : QPixmap());
            avatar->setVisible(mark || (photo && !photo->isNull()));
        }
    }

private:
    QString m_caption;
    std::optional<workspace::AvatarKind> m_kind;
    bool m_multiple = false;
    QVBoxLayout *m_values = nullptr;
    QVector<Entry> m_entries;
    struct Avatar {
        AvatarBadge *widget;
        QUrl url;
        bool showWithoutPhoto;
    };
    QVector<Avatar> m_avatars;
};

class TaskCardDialog final : public QDialog {
public:
    TaskCardDialog(const Task &task, const QString &projectId, QWidget *parent, bool detailsLoaded = true);
    void setLink(const QString &link);
    void setStatus(const QString &text, bool loading = false);
    void setTask(const Task &task, bool detailsLoaded = true);
    void setStages(const QVector<Stage> &stages);
    void setWriteState(bool writable, bool editing, bool moving, bool sending, bool uploading);
    QString taskId() const { return m_task.id; }
    QString projectId() const { return m_projectId; }
    QPlainTextEdit *messageInput() const { return m_input; }
    ChatComposer *composer() const { return m_composer; }
    QListWidget *messageList() const { return m_messages; }
    TaskContentList *descriptionList() const { return m_descriptionRows; }
    TaskContentList *attachmentList() const { return m_attachmentRows; }
    TaskDescriptionEditor *descriptionEditor() const;
    QScrollBar *detailsScrollBar() const;
    void setProjectCaption(const QString &caption);
    void acceptEdit();
    QString searchQuery() const;
    QString taskChatDialogId() const { return m_task.chatDialogId; }
    TaskRelationsPanel *relations() const { return m_relations; }
    void setPinnedMessages(const QVector<Message> &messages);
    void setOnPinned(std::function<void(const QString &)> handler);
    void setOnLatest(std::function<void()> handler);
    bool focusMessage(const QString &id);
    void setContextMode(bool active);
    void showMessages(const QVector<Message> &messages, MessageView view = MessageView::History);
    void setChatStatus(const QString &text, bool loading = false);
    void setOnCopy(std::function<void(const QString &)> handler);
    void setOnSave(std::function<void(const TaskEdit &)> handler);
    void setOnDelete(std::function<void()> handler);
    bool canDeleteTask() const;
    void cancelFooterDelegation();
    void setProjectChoices(const QVector<Project> &projects);
    void setCanDetachProject(bool allowed);
    void setMoreProjectsAvailable(bool available);
    void setRequisiteAvatars(TaskRequisiteRow::AvatarSource source);
    void setOnRequisiteAvatarChoicesChanged(std::function<void()> changed);
    QList<QUrl> visibleRequisiteAvatarUrls() const;
    void refreshRequisiteAvatars();
    void setOnMoreProjects(std::function<void()> handler);
    void setPeopleSearch(const std::function<void(const QString &, const QString &)> &handler);
    void setPeopleResults(const QString &id, const QVector<Person> &people);
    bool setPeopleSearchError(const QString &id);
    void setOnMove(std::function<void(const QString &)> handler);
    void setOnSend(std::function<void()> handler);
    void setOnSearch(std::function<void(const QString &)> handler);
    void setOnFile(std::function<void(bool)> handler);

    // Несохранённые изменения карточки: черновик сообщения, правки полей, идущая отправка или загрузка.
    bool hasUnsavedChanges() const;

    // Диалог подтверждения отбрасывания правок. По умолчанию — предупреждение с выбором.
    void setDiscardConfirmation(std::function<bool()> handler);

    // Спрашивает пользователя перед отбрасыванием правок. false — закрытие отменено.
    bool confirmDiscardIfNeeded(bool remember = true);

protected:
    void closeEvent(QCloseEvent *event) override;
    void reject() override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *target, QEvent *event) override;

private:
    bool confirmDiscardDialog();
    void send();
    void searchMessages();
    void copy(const QString &value, const QString &feedback);
    void restoreDetailsWidth();
    void setEditingDescription(bool enabled);
    static QString personName(const QString &name, const QString &id);
    static void setRequisite(TaskRequisiteRow *row, const QString &text, const QUrl &url = {});
    void setObservers(const Task &task);
    void showRequisiteProject();
    QListWidgetItem *stageItem(const QString &id) const;
    void fitStageList();
    void updateStageSummary();
    void selectStage(QListWidgetItem *item);
    void submitStage();
    bool hasStageDraft() const;
    void openRequisitePopup(TaskRequisiteRow *row, QWidget *editor);
    void positionRequisitePopup();
    void hideRequisitePopup();
    void scrollToRequisiteEditor(QWidget *editor);
    void closeStageEditor();
    static void addTaskFile(TaskContentList *list, const MessageFile &file, int serial);
    void showTaskContent(const Task &task, bool detailsLoaded);
    void addTaskActions(QVBoxLayout *layout);
    static QString taskRatingLabel(TaskRating value);
    bool canUseTaskActions() const;
    void submitTaskStatus(TaskStatusAction action);
    void updateTaskActions();
    void updateButtons();

    Task m_task;
    QString m_projectId;
    QString m_link;
    QLabel *m_taskTitle = nullptr;
    QString m_projectCaption;
    TaskRequisiteRow *m_creator = nullptr;
    TaskRequisiteRow *m_assignee = nullptr;
    TaskRequisiteRow *m_deadline = nullptr;
    TaskRequisiteRow *m_taskStatus = nullptr;
    QLabel *m_createdAt = nullptr;
    TaskRequisiteRow *m_project = nullptr;
    TaskRequisiteRow *m_observers = nullptr;
    TaskRequisiteRow *m_existingStage = nullptr;
    TaskRequisiteRow *m_activeDetail = nullptr;
    QVector<QPair<TaskRequisiteRow *, TaskDetailEditor::Field>> m_requisiteButtons;
    QVector<Project> m_requisiteProjects;
    TaskRequisiteRow::AvatarSource m_requisiteAvatars;
    std::function<void()> m_onRequisiteAvatarChoicesChanged;
    QWidget *m_stageEditor = nullptr;
    TaskPickerSummary *m_stageSummary = nullptr;
    QFrame *m_requisitePopup = nullptr;
    QVBoxLayout *m_requisitePopupLayout = nullptr;
    TaskRequisiteRow *m_popupAnchor = nullptr;
    QPushButton *m_cancelStage = nullptr;
    QString m_stageBaseline;
    QString m_stageChoice;
    bool m_stageOpen = false;
    bool m_stageSubmitAttempted = false;
    TaskDetailEditor *m_detailEditor = nullptr;
    QSplitter *m_splitter = nullptr;
    QScrollArea *m_detailsScroll = nullptr;
    TaskContentList *m_descriptionRows = nullptr;
    TaskContentList *m_attachmentRows = nullptr;
    TaskRelationsPanel *m_relations = nullptr;
    QWidget *m_editActions = nullptr;
    QPushButton *m_edit = nullptr;
    QPushButton *m_cancelEdit = nullptr;
    QPushButton *m_maximize = nullptr;
    int m_detailsWidth = 0;
    bool m_editMode = false;
    QLabel *m_empty = nullptr;
    QLabel *m_status = nullptr;
    TaskFields *m_fields = nullptr;
    QListWidget *m_messages = nullptr;
    QPlainTextEdit *m_input = nullptr;
    ChatComposer *m_composer = nullptr;
    QLineEdit *m_messageSearch = nullptr;
    QPushButton *m_searchToggle = nullptr;
    PinnedMessageBar *m_pinned = nullptr;
    QPushButton *m_contextBack = nullptr;
    bool m_contextMode = false;
    QListWidget *m_stages = nullptr;
    QPushButton *m_copyLink = nullptr;
    QLabel *m_copyFeedback = nullptr;
    QTimer m_copyFeedbackTimer;
    QPushButton *m_move = nullptr;
    QPushButton *m_save = nullptr;
    QPushButton *m_attach = nullptr;
    QPushButton *m_file = nullptr;
    QPushButton *m_send = nullptr;
    QPushButton *m_emoji = nullptr;
    bool m_writable = false;
    bool m_editing = false;
    bool m_moving = false;
    bool m_sending = false;
    bool m_uploading = false;
    bool m_discardConfirmed = false;
    bool m_delegatingFromFooter = false;
    QWidget *m_taskActions = nullptr;
    QPushButton *m_primaryTaskActionButton = nullptr;
    QPushButton *m_completeTaskAction = nullptr;
    QPushButton *m_taskOverflowButton = nullptr;
    QPushButton *m_taskRatingButton = nullptr;
    QAction *m_taskDelegateAction = nullptr;
    QAction *m_taskDeleteAction = nullptr;
    QAction *m_taskDeleteSeparator = nullptr;
    std::optional<TaskStatusAction> m_primaryTaskAction;
    QVector<QPair<TaskStatusAction, QAction *>> m_taskStatusMenuActions;
    QVector<QPair<TaskRating, QAction *>> m_taskRatingMenuActions;
    std::function<void(const QString &)> m_onCopy;
    std::function<void(const TaskEdit &)> m_onSave;
    std::function<void()> m_onDelete;
    std::function<void(const QString &)> m_onMove;
    std::function<void()> m_onSend;
    std::function<void(const QString &)> m_onSearch;
    std::function<void()> m_onLatest;
    std::function<void(bool)> m_onFile;
    std::function<bool()> m_confirmDiscard;
};

class NameDialog final : public QDialog {
public:
    explicit NameDialog(QWidget *parent) : QDialog(parent)
    {
        setProperty("workspaceDialog", true);
        setWindowTitle(QStringLiteral("Новый проект"));
        setObjectName(QStringLiteral("nameDialog"));
        setAccessibleName(QStringLiteral("Новый проект"));
        auto *layout = new QVBoxLayout(this);
        layout->addWidget(workspace::label(QStringLiteral("Название"), "fieldCaption", this));
        m_input = new QLineEdit(this);
        editor(m_input);
        m_input->setAccessibleName(QStringLiteral("Название"));
        layout->addWidget(m_input);
        m_status = workspace::label(QString(), "quiet", this);
        layout->addWidget(m_status);
        m_status->hide();
        auto *actions = new QHBoxLayout;
        actions->addStretch(1);
        auto *close = button(QStringLiteral("Отмена"), QStringLiteral("Отмена"), this);
        connect(close, &QPushButton::clicked, this, &QDialog::reject);
        actions->addWidget(close);
        m_create = button(QStringLiteral("Создать"), QStringLiteral("Подтвердить название"), this);
        m_create->setObjectName(QStringLiteral("primary"));
        const auto submit = [this] {
            if (m_create->isEnabled() && m_onCreate)
                m_onCreate(m_input->text().trimmed());
        };
        connect(m_create, &QPushButton::clicked, this, submit);
        connect(m_input, &QLineEdit::returnPressed, this, submit);
        connect(m_input, &QLineEdit::textChanged, this, [this] { updateButton(); });
        actions->addWidget(m_create);
        layout->addLayout(actions);
        updateButton();
    }

    void setOnCreate(std::function<void(const QString &)> handler) { m_onCreate = std::move(handler); }
    void setStatus(const QString &text, bool loading = false) { setStatusLabel(m_status, text, loading); }
    void setWritable(bool writable)
    {
        m_writable = writable;
        m_input->setReadOnly(!writable);
        updateButton();
    }

private:
    void updateButton()
    {
        m_create->setEnabled(m_writable && !m_input->text().trimmed().isEmpty());
    }

    QLineEdit *m_input = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_create = nullptr;
    bool m_writable = true;
    std::function<void(const QString &)> m_onCreate;
};

class StageManagerDialog final : public QDialog {
public:
    explicit StageManagerDialog(const QString &projectId, QWidget *parent)
        : QDialog(parent), m_projectId(projectId)
    {
        setProperty("workspaceDialog", true);
        setWindowTitle(QStringLiteral("Этапы доски"));
        setObjectName(QStringLiteral("stageManagerDialog"));
        setAccessibleName(QStringLiteral("Управление этапами"));
        resize(525, 400);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(14, 12, 14, 10);
        layout->setSpacing(7);
        m_stages = new QListWidget(this);
        m_stages->setAccessibleName(QStringLiteral("Список этапов"));
        workspace::configureRows(m_stages, workspace::Row::Card);
        layout->addWidget(m_stages, 1);
        auto *row = new QHBoxLayout;
        m_title = new QLineEdit(this);
        editor(m_title);
        m_title->setAccessibleName(QStringLiteral("Название этапа"));
        m_title->setPlaceholderText(QStringLiteral("Название этапа"));
        row->addWidget(m_title, 1);
        m_color = button(QStringLiteral("Цвет…"), QStringLiteral("Изменить цвет этапа"), this);
        connect(m_color, &QPushButton::clicked, this, [this] { chooseColor(); });
        row->addWidget(m_color);
        m_add = button(QStringLiteral("Добавить"), QStringLiteral("Добавить этап"), this);
        connect(m_add, &QPushButton::clicked, this, [this] {
            if (m_onSave)
                m_onSave({}, m_title->text().trimmed(), {}, {});
        });
        row->addWidget(m_add);
        layout->addLayout(row);
        auto *actions = new QHBoxLayout;
        m_rename = button(QStringLiteral("Переименовать"), QStringLiteral("Переименовать этап"), this);
        connect(m_rename, &QPushButton::clicked, this, [this] {
            if (m_onSave)
                m_onSave(currentStage(), m_title->text().trimmed(), {}, {});
        });
        actions->addWidget(m_rename);
        m_up = button(QStringLiteral("Выше"), QStringLiteral("Переместить этап выше"), this);
        connect(m_up, &QPushButton::clicked, this, [this] { move(-1); });
        actions->addWidget(m_up);
        m_down = button(QStringLiteral("Ниже"), QStringLiteral("Переместить этап ниже"), this);
        connect(m_down, &QPushButton::clicked, this, [this] { move(1); });
        actions->addWidget(m_down);
        m_remove = button(QStringLiteral("Удалить"), QStringLiteral("Удалить этап"), this);
        connect(m_remove, &QPushButton::clicked, this, [this] {
            if (m_onDelete && m_remove->isEnabled())
                m_onDelete(currentStage());
        });
        actions->addWidget(m_remove);
        actions->addStretch(1);
        auto *close = button(QStringLiteral("Готово"), QStringLiteral("Закрыть управление этапами"), this);
        connect(close, &QPushButton::clicked, this, &QDialog::reject);
        actions->addWidget(close);
        layout->addLayout(actions);
        m_status = workspace::label(QString(), "quiet", this);
        m_status->setAccessibleName(QStringLiteral("Состояние этапов"));
        layout->addWidget(m_status);
        m_status->hide();
        connect(m_stages, &QListWidget::currentRowChanged, this, [this](int index) {
            if (validRow(index))
                m_title->setText(m_data.at(index).name);
            updateColorSwatch();
            updateButtons();
        });
        connect(m_title, &QLineEdit::textChanged, this, [this] { updateButtons(); });
        updateButtons();
    }

    QString projectId() const { return m_projectId; }

    void setStages(const QVector<Stage> &stages)
    {
        const auto selected = currentStage();
        const QSignalBlocker blocker(m_stages);
        m_data = stages;
        m_stages->clear();
        for (const auto &stage : stages) {
            const auto count = stage.taskCount ? QStringLiteral("Задач: %1").arg(*stage.taskCount)
                : QStringLiteral("Загружено: %1").arg(stage.tasks.size());
            auto *item = workspace::addRow(m_stages, stage.name, count, {}, stage.id);
            if (stage.id == selected)
                m_stages->setCurrentItem(item);
        }
        updateColorSwatch();
        updateButtons();
    }
    void setStatus(const QString &text, bool loading = false) { setStatusLabel(m_status, text, loading); }
    void setWritable(bool writable)
    {
        m_writable = writable;
        updateButtons();
    }
    void setOnSave(std::function<void(const QString &, const QString &, const QString &, const QString &)> handler)
    {
        m_onSave = std::move(handler);
    }
    void setOnDelete(std::function<void(const QString &)> handler) { m_onDelete = std::move(handler); }

private:
    bool validRow(int index) const { return index >= 0 && index < m_data.size(); }

    QString currentStage() const
    {
        const auto *item = m_stages->currentItem();
        return item ? item->data(workspace::Id).toString() : QString();
    }

    void chooseColor()
    {
        const int row = m_stages->currentRow();
        if (!m_writable || !m_onSave || !validRow(row))
            return;
        const auto &stage = m_data.at(row);
        const QColor initial(QLatin1Char('#') + stage.color);
        auto *picker = new QColorDialog(initial.isValid() ? initial : QColor("#dce5e4"), this);
        picker->setAttribute(Qt::WA_DeleteOnClose);
        picker->setOption(QColorDialog::DontUseNativeDialog);
        picker->setWindowTitle(QStringLiteral("Цвет этапа"));
        connect(picker, &QColorDialog::colorSelected, this, [this, id = stage.id](const QColor &color) {
            const int selected = m_stages->currentRow();
            if (!m_writable || !m_onSave || !validRow(selected) || !color.isValid())
                return;
            const auto &current = m_data.at(selected);
            const auto hex = color.name(QColor::HexRgb).mid(1);
            if (current.id == id && current.color != hex)
                m_onSave(id, {}, {}, hex);
        });
        picker->open();
    }

    void move(int offset)
    {
        const int current = m_stages->currentRow();
        if (!m_onSave || !validRow(current) || !validRow(current + offset))
            return;
        const auto &stage = m_data.at(current);
        const int predecessor = offset < 0 ? current - 2 : current + 1;
        m_onSave(stage.id, stage.name, predecessor < 0 ? QStringLiteral("0") : m_data.at(predecessor).id, {});
    }

    void updateColorSwatch()
    {
        const int row = m_stages->currentRow();
        const QColor color(validRow(row) ? QLatin1Char('#') + m_data.at(row).color : QString());
        if (color.isValid()) {
            QPixmap swatch(13, 13);
            swatch.fill(color);
            m_color->setIcon(QIcon(swatch));
        } else {
            m_color->setIcon({});
        }
    }

    void updateButtons()
    {
        const int row = m_stages->currentRow();
        const bool selected = validRow(row);
        const bool title = !m_title->text().trimmed().isEmpty();
        m_title->setReadOnly(!m_writable);
        m_add->setEnabled(m_writable && title);
        m_color->setEnabled(m_writable && selected);
        m_rename->setEnabled(m_writable && selected && title);
        m_up->setEnabled(m_writable && selected && row > 0);
        m_down->setEnabled(m_writable && selected && row + 1 < m_data.size());
        const auto count = selected ? m_data.at(row).taskCount : std::optional<qsizetype>();
        m_remove->setEnabled(m_writable && count && *count == 0 && m_data.at(row).tasks.isEmpty());
        m_remove->setToolTip(!selected ? QString() : (!count
            ? QStringLiteral("Портал не подтвердил, что этап пуст") : (*count > 0
            ? QStringLiteral("Чтобы удалить этап, сначала перенесите из него задачи") : QString())));
    }

    QString m_projectId;
    QVector<Stage> m_data;
    QListWidget *m_stages = nullptr;
    QLineEdit *m_title = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_add = nullptr;
    QPushButton *m_color = nullptr;
    QPushButton *m_rename = nullptr;
    QPushButton *m_up = nullptr;
    QPushButton *m_down = nullptr;
    QPushButton *m_remove = nullptr;
    bool m_writable = true;
    std::function<void(const QString &, const QString &, const QString &, const QString &)> m_onSave;
    std::function<void(const QString &)> m_onDelete;
};

} // namespace bx::ui
