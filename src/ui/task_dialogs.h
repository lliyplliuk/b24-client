#pragma once

#include "ui/workspace_presentation.h"
#include "ui/chat_widgets.h"
#include "ui/task_description_editor.h"
#include "ui/task_detail_editor.h"
#include "ui/task_relations.h"

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
        m_retryStages->setToolTip(QStringLiteral("<qt>%1</qt>").arg(reason.toHtmlEscaped()));
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
            button->setToolTip(QStringLiteral("<qt>%1</qt>").arg(value.toHtmlEscaped()));
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
            m_observers->setToolTip(QStringLiteral("<qt>%1</qt>").arg(people.join(QStringLiteral(", ")).toHtmlEscaped()));
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
    TaskCardDialog(const Task &task, const QString &projectId, QWidget *parent, bool detailsLoaded = true)
        : QDialog(parent, Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
                          Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint),
          m_task(task), m_projectId(projectId)
    {
        setProperty("workspaceDialog", true);
        setWindowTitle(QStringLiteral("Задача №%1").arg(task.id));
        setObjectName(QStringLiteral("taskCardDialog"));
        setAccessibleName(QStringLiteral("Карточка задачи"));
        // Keep modal input handling while letting the window manager maximize the native window.
        setWindowModality(Qt::WindowModal);
        resize(900, 630);
        setMinimumSize(660, 420);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(16, 13, 16, 12);
        layout->setSpacing(9);
        auto *header = new QHBoxLayout;
        auto *number = workspace::label(QStringLiteral("№%1").arg(task.id), "quiet", this);
        number->setAccessibleName(QStringLiteral("Номер задачи"));
        header->addWidget(number);
        auto *copyNumber = new GlyphButton(Glyph::Copy, QStringLiteral("Скопировать номер"), this);
        copyNumber->setFixedSize(26, 26);
        connect(copyNumber, &QPushButton::clicked, this, [this] {
            copy(m_task.id, QStringLiteral("Номер скопирован"));
        });
        header->addWidget(copyNumber);
        m_copyLink = new GlyphButton(Glyph::Link, QStringLiteral("Скопировать ссылку"), this);
        m_copyLink->setFixedSize(26, 26);
        setLink({});
        connect(m_copyLink, &QPushButton::clicked, this, [this] {
            copy(m_link, QStringLiteral("Ссылка скопирована"));
        });
        header->addWidget(m_copyLink);
        m_copyFeedback = workspace::label({}, "taskCopyFeedback", this);
        m_copyFeedback->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        header->addWidget(m_copyFeedback, 1);
        m_copyFeedbackTimer.setSingleShot(true);
        m_copyFeedbackTimer.setInterval(1800);
        connect(&m_copyFeedbackTimer, &QTimer::timeout, m_copyFeedback, &QLabel::clear);
        m_maximize = button(QStringLiteral("Развернуть"), QStringLiteral("Развернуть карточку задачи"), this);
        connect(m_maximize, &QPushButton::clicked, this, [this] {
            if (isMaximized())
                showNormal();
            else
                showMaximized();
        });
        header->addWidget(m_maximize);
        layout->addLayout(header);
        m_taskTitle = workspace::label(QString(), "taskCardHeading", this);
        m_taskTitle->setAccessibleName(QStringLiteral("Название открытой задачи"));
        m_taskTitle->setWordWrap(true);
        layout->addWidget(m_taskTitle);

        m_splitter = new QSplitter(Qt::Horizontal, this);
        m_splitter->setObjectName(QStringLiteral("taskCardSplitter"));
        m_splitter->setAccessibleName(QStringLiteral("Ширина реквизитов задачи"));
        m_splitter->setChildrenCollapsible(false);
        m_splitter->setHandleWidth(5);
        m_splitter->setOpaqueResize(false);
        m_detailsScroll = new QScrollArea(m_splitter);
        m_detailsScroll->setObjectName(QStringLiteral("taskDetailsScroll"));
        m_detailsScroll->setAccessibleName(QStringLiteral("Реквизиты и файлы задачи"));
        m_detailsScroll->setWidgetResizable(true);
        m_detailsScroll->setFrameShape(QFrame::NoFrame);
        m_detailsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_detailsScroll->setMinimumWidth(280);
        auto *fields = new QWidget(m_detailsScroll);
        fields->setObjectName(QStringLiteral("taskDetailsPane"));
        m_detailsScroll->setWidget(fields);
        auto *form = new QVBoxLayout(fields);
        form->setContentsMargins(0, 0, 6, 0);
        form->setSpacing(9);
        auto section = [fields, form](const QString &title) {
            auto *container = new QWidget(fields);
            container->setObjectName(QStringLiteral("taskSection"));
            auto *content = new QVBoxLayout(container);
            content->setContentsMargins(10, 8, 10, 9);
            content->setSpacing(5);
            auto *heading = new QHBoxLayout;
            if (!title.isEmpty())
                heading->addWidget(workspace::label(title, "taskSectionHeading", container), 1);
            content->addLayout(heading);
            form->addWidget(container);
            return std::make_pair(content, heading);
        };
        auto [description, descriptionHeader] = section(QStringLiteral("Описание"));
        m_edit = button(QStringLiteral("Изменить"), QStringLiteral("Изменить задачу"), fields);
        connect(m_edit, &QPushButton::clicked, this, [this] { setEditingDescription(true); });
        descriptionHeader->addWidget(m_edit);
        m_descriptionRows = new TaskContentList(QStringLiteral("Описание и встроенные файлы"), fields);
        description->addWidget(m_descriptionRows);
        m_fields = new TaskFields(fields, false);
        description->addWidget(m_fields);
        m_editActions = new QWidget(fields);
        auto *editActions = new QHBoxLayout(m_editActions);
        editActions->setContentsMargins(0, 3, 0, 0);
        editActions->addStretch(1);
        m_cancelEdit = button(QStringLiteral("Отменить"), QStringLiteral("Отменить изменения задачи"), m_editActions);
        connect(m_cancelEdit, &QPushButton::clicked, this, [this] {
            m_fields->setTask(m_task, true);
            setEditingDescription(false);
            setStatus({});
        });
        editActions->addWidget(m_cancelEdit);
        m_save = button(QStringLiteral("Сохранить"), QStringLiteral("Сохранить задачу"), m_editActions);
        m_save->setObjectName(QStringLiteral("primary"));
        connect(m_save, &QPushButton::clicked, this, [this] {
            const auto error = m_fields->validation();
            const auto edit = m_fields->edit();
            const auto &rights = m_task.editRights;
            if (!error.isEmpty()) {
                setStatus(error);
            } else if ((edit.title && !rights.title) || (edit.description && !rights.description)
                       || (edit.highPriority && !rights.priority)) {
                setStatus(QStringLiteral("Портал больше не разрешает сохранить часть изменений"));
            } else if (m_onSave) {
                m_onSave(edit);
            }
        });
        editActions->addWidget(m_save);
        description->addWidget(m_editActions);
        m_fields->hide();
        m_editActions->hide();

        auto [attachments, attachmentsHeader] = section(QStringLiteral("Файлы"));
        m_attach = button(QStringLiteral("Добавить файл"), QStringLiteral("Прикрепить файл к задаче"), fields);
        connect(m_attach, &QPushButton::clicked, this, [this] {
            if (m_onFile)
                m_onFile(true);
        });
        attachmentsHeader->addWidget(m_attach);
        m_attachmentRows = new TaskContentList(QStringLiteral("Вложения задачи"), fields);
        attachments->addWidget(m_attachmentRows);

        auto [details, detailsHeader] = section(QStringLiteral("Реквизиты"));
        auto [project, projectHeader] = section({});
        auto [observers, observersHeader] = section({});
        auto requisite = [](QVBoxLayout *group, const QString &caption,
                            std::optional<workspace::AvatarKind> kind = {}, bool multiple = false) {
            auto *row = new TaskRequisiteRow(caption, group->parentWidget(), kind, multiple);
            group->addWidget(row);
            return row;
        };
        using Field = TaskDetailEditor::Field;
        m_requisitePopup = new QFrame(this);
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
        connect(m_detailsScroll->verticalScrollBar(), &QScrollBar::valueChanged,
                this, [this] { positionRequisitePopup(); });
        m_detailEditor = new TaskDetailEditor(fields);
        m_detailEditor->onSave = [this](const TaskEdit &edit) {
            if (!m_onSave)
                return;
            auto request = edit;
            request.requireDelegate = m_delegatingFromFooter && edit.assigneeId.has_value();
            m_onSave(request);
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
            updateButtons();
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
        connect(m_move, &QPushButton::clicked, this, &TaskCardDialog::submitStage);
        stageActions->addWidget(m_move);
        stageLayout->addLayout(stageActions);
        m_requisitePopupLayout->addWidget(m_stageEditor);
        connect(m_stages, &QListWidget::itemClicked, this, &TaskCardDialog::selectStage);
        connect(m_stages, &QListWidget::itemActivated, this, &TaskCardDialog::selectStage);
        connect(m_existingStage, &QPushButton::clicked, this, [this] {
            m_stageBaseline = m_task.stageId;
            m_stageChoice = m_stageBaseline;
            m_stageSubmitAttempted = false;
            m_stages->setCurrentItem(stageItem(m_stageBaseline));
            m_stageOpen = true;
            openRequisitePopup(m_existingStage, m_stageEditor);
            updateButtons();
            positionRequisitePopup();
            m_stages->setFocus(Qt::OtherFocusReason);
        });
        m_observers = requisite(observers, QStringLiteral("Наблюдатели"), workspace::AvatarKind::Person, true);
        connectRequisite(m_observers, observers, Field::Observers);

        auto relations = section(QStringLiteral("Чек-листы"));
        m_relations = new TaskRelationsPanel(fields);
        relations.first->addWidget(m_relations);
        form->addStretch(1);
        m_splitter->addWidget(m_detailsScroll);

        auto *chat = new QWidget(m_splitter);
        chat->setMinimumWidth(285);
        chat->setObjectName(QStringLiteral("reading"));
        auto *thread = new QVBoxLayout(chat);
        thread->setContentsMargins(0, 0, 0, 0);
        thread->setSpacing(0);
        auto *top = new QWidget(chat);
        top->setObjectName(QStringLiteral("conversationHeader"));
        auto *head = new QHBoxLayout(top);
        head->setContentsMargins(9, 1, 5, 1);
        head->addWidget(workspace::label(QStringLiteral("Чат задачи"), "conversationTitle", top), 1);
        auto *searchToggle = new GlyphButton(Glyph::Search, QStringLiteral("Найти в чате задачи"), top);
        m_searchToggle = searchToggle;
        searchToggle->setCheckable(true);
        head->addWidget(searchToggle);
        thread->addWidget(top);
        m_pinned = new PinnedMessageBar(chat);
        thread->addWidget(m_pinned);
        m_contextBack = button(QStringLiteral("К последним сообщениям"), QStringLiteral("К последним сообщениям задачи"), chat);
        m_contextBack->hide();
        connect(m_contextBack, &QPushButton::clicked, this, [this] {
            if (m_onLatest)
                m_onLatest();
        });
        thread->addWidget(m_contextBack, 0, Qt::AlignRight);

        auto *searchBar = new QWidget(chat);
        searchBar->setObjectName(QStringLiteral("conversationSearch"));
        auto *search = new QHBoxLayout(searchBar);
        search->setContentsMargins(7, 2, 4, 2);
        search->setSpacing(4);
        m_messageSearch = new QLineEdit(searchBar);
        editor(m_messageSearch);
        m_messageSearch->setAccessibleName(QStringLiteral("Поиск по чату карточки"));
        m_messageSearch->setPlaceholderText(QStringLiteral("Поиск по истории"));
        search->addWidget(m_messageSearch, 1);
        auto *clear = new GlyphButton(Glyph::Close, QStringLiteral("Очистить поиск в карточке"), searchBar);
        connect(clear, &QPushButton::clicked, m_messageSearch, &QLineEdit::clear);
        connect(m_messageSearch, &QLineEdit::returnPressed, this, [this] { searchMessages(); });
        connect(m_messageSearch, &QLineEdit::textChanged, this, [this](const QString &text) {
            if (text.trimmed().isEmpty())
                searchMessages();
        });
        search->addWidget(clear);
        thread->addWidget(searchBar);
        searchBar->hide();
        connect(searchToggle, &QPushButton::toggled, this, [this, searchBar](bool show) {
            searchBar->setVisible(show);
            if (show)
                m_messageSearch->setFocus(Qt::ShortcutFocusReason);
            else
                m_messageSearch->clear();
        });
        auto *find = new QAction(chat);
        find->setShortcut(QKeySequence::Find);
        find->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        connect(find, &QAction::triggered, this, [this, searchToggle] {
            searchToggle->setChecked(true);
            m_messageSearch->setFocus(Qt::ShortcutFocusReason);
        });
        chat->addAction(find);

        auto *canvas = new ConversationCanvas(chat);
        auto *timeline = new QVBoxLayout(canvas);
        timeline->setContentsMargins(6, 4, 6, 5);
        timeline->setSpacing(0);
        m_messages = new HistoryList(canvas);
        m_messages->setAccessibleName(QStringLiteral("Чат карточки"));
        workspace::configureRows(m_messages, workspace::Row::Message);
        static_cast<HistoryList *>(m_messages)->setOnBrowsingOlder([this](bool active) {
            m_contextBack->setVisible(active || m_contextMode);
        });
        timeline->addWidget(static_cast<HistoryList *>(m_messages)->olderButton(canvas), 0, Qt::AlignHCenter);
        timeline->addWidget(m_messages, 1);
        m_empty = workspace::label(QStringLiteral("Загружаем чат…"), "empty", canvas);
        setLoadingIndicator(m_empty, true);
        m_empty->setAlignment(Qt::AlignCenter);
        timeline->addWidget(m_empty, 1);
        m_messages->hide();
        thread->addWidget(canvas, 1);

        m_composer = new ChatComposer(chat, QStringLiteral("Текст в чате карточки"),
            QStringLiteral("Эмодзи в карточке"), QStringLiteral("Отправить файл в чат задачи"),
            QStringLiteral("Отправить в карточке"));
        m_input = m_composer->input();
        static_cast<HistoryList *>(m_messages)->setComposer(m_input);
        m_input->installEventFilter(this);
        connect(m_input, &QPlainTextEdit::textChanged, this, [this] { updateButtons(); });
        m_emoji = m_composer->emoji();
        m_file = m_composer->file();
        connect(m_file, &QPushButton::clicked, this, [this] {
            if (m_onFile)
                m_onFile(false);
        });
        m_send = m_composer->send();
        connect(m_send, &QPushButton::clicked, this, [this] { send(); });
        m_composer->setEditStateChanged([this] { updateButtons(); });
        thread->addWidget(m_composer);
        m_splitter->addWidget(chat);
        m_splitter->setStretchFactor(1, 1);
        QSettings settings(QStringLiteral("bitrix24-client"), QStringLiteral("bitrix24-client"));
        m_detailsWidth = qBound(0, settings.value(QStringLiteral("ui/taskCardDetailsWidth")).toInt(), 2000);
        restoreDetailsWidth();
        connect(m_splitter, &QSplitter::splitterMoved, this, [this] {
            m_detailsWidth = m_splitter->sizes().first();
            QSettings settings(QStringLiteral("bitrix24-client"), QStringLiteral("bitrix24-client"));
            settings.setValue(QStringLiteral("ui/taskCardDetailsWidth"), m_detailsWidth);
        });
        layout->addWidget(m_splitter, 1);
        addTaskActions(layout);
        m_status = workspace::label(QString(), "quiet", this);
        m_status->setAccessibleName(QStringLiteral("Состояние карточки"));
        layout->addWidget(m_status);
        m_status->hide();
        setTask(task, detailsLoaded);
        updateButtons();
    }

    void setLink(const QString &link)
    {
        m_link = link;
        m_copyLink->setEnabled(!link.isEmpty());
        m_copyLink->setToolTip(link.isEmpty() ? QStringLiteral("Ссылка на задачу недоступна")
                                             : m_copyLink->accessibleName());
    }
    void setStatus(const QString &text, bool loading = false) { setStatusLabel(m_status, text, loading); }
    void setTask(const Task &task, bool detailsLoaded = true)
    {
        const auto previousStage = m_task.stageId;
        const bool changedProject = task.projectId != m_projectId
            && (!task.projectId.isEmpty() || !m_task.projectId.isEmpty());
        if (changedProject) {
            m_projectId = task.projectId;
            m_projectCaption.clear();
            setLink({});
        }
        m_task = task;
        m_taskTitle->setText(task.name.isEmpty() ? QStringLiteral("Задача") : task.name);
        m_fields->setTask(task);
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
        showTaskContent(task, detailsLoaded);
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
        updateButtons();
    }

    void setStages(const QVector<Stage> &stages)
    {
        const QSignalBlocker blocker(m_stages);
        m_stages->clear();
        for (const auto &stage : stages) {
            auto *option = new QListWidgetItem(stage.name, m_stages);
            option->setData(Qt::UserRole, stage.id);
            option->setFlags(option->flags() & ~Qt::ItemIsUserCheckable);
            option->setIcon(taskStageColorIcon(stage.color));
            if (stage.id == m_task.stageId) {
                option->setCheckState(Qt::Checked);
                option->setData(Qt::AccessibleDescriptionRole, QStringLiteral("Текущий этап"));
            }
        }
        fitStageList();
        auto *current = stageItem(m_task.stageId);
        auto *draft = stageItem(m_stageChoice);
        m_stages->setCurrentItem(m_stageOpen && draft ? draft : current);
        setRequisite(m_existingStage, current ? current->text() : m_task.stageName);
        updateButtons();
    }

    void setWriteState(bool writable, bool editing, bool moving, bool sending, bool uploading)
    {
        m_writable = writable;
        m_editing = editing;
        m_moving = moving;
        m_sending = sending;
        m_uploading = uploading;
        updateButtons();
    }
    QString taskId() const { return m_task.id; }
    QString projectId() const { return m_projectId; }
    QPlainTextEdit *messageInput() const { return m_input; }
    ChatComposer *composer() const { return m_composer; }
    QListWidget *messageList() const { return m_messages; }
    TaskContentList *descriptionList() const { return m_descriptionRows; }
    TaskContentList *attachmentList() const { return m_attachmentRows; }
    TaskDescriptionEditor *descriptionEditor() const { return m_fields->descriptionEditor(); }
    QScrollBar *detailsScrollBar() const { return m_detailsScroll->verticalScrollBar(); }
    void setProjectCaption(const QString &caption)
    {
        m_projectCaption = caption;
        showRequisiteProject();
        refreshRequisiteAvatars();
    }
    void acceptEdit()
    {
        Task updated = m_task;
        if (m_stageOpen) {
            closeStageEditor();
            return;
        }
        if (m_detailEditor->active()) {
            m_detailEditor->accept(updated);
            setTask(updated);
            return;
        }
        if (!m_editMode)
            return;
        const auto pending = m_fields->edit();
        if (pending.title)
            updated.name = *pending.title;
        if (pending.description) {
            updated.description = *pending.description;
            updated.descriptionChecksum.clear();
        }
        if (pending.deadline)
            updated.due = *pending.deadline;
        if (pending.highPriority)
            updated.priority = *pending.highPriority ? QStringLiteral("2") : QStringLiteral("1");
        m_fields->setTask(updated, true);
        setTask(updated);
        setEditingDescription(false);
    }
    QString searchQuery() const { return m_messageSearch->text().trimmed(); }
    QString taskChatDialogId() const { return m_task.chatDialogId; }
    TaskRelationsPanel *relations() const { return m_relations; }
    void setPinnedMessages(const QVector<Message> &messages) { m_pinned->setMessages(messages); }
    void setOnPinned(std::function<void(const QString &)> handler) { m_pinned->setOnOpen(std::move(handler)); }
    void setOnLatest(std::function<void()> handler) { m_onLatest = std::move(handler); }
    bool focusMessage(const QString &id) { return static_cast<HistoryList *>(m_messages)->focusOnMessage(id); }
    void setContextMode(bool active)
    {
        m_contextMode = active;
        m_contextBack->setVisible(active);
        if (active) {
            const QSignalBlocker search(m_messageSearch);
            const QSignalBlocker toggle(m_searchToggle);
            m_messageSearch->clear();
            m_searchToggle->setChecked(false);
            m_messageSearch->parentWidget()->hide();
            static_cast<HistoryList *>(m_messages)->setLiveReadHistory(false);
        } else {
            static_cast<HistoryList *>(m_messages)->returnToLatest();
        }
    }
    void showMessages(const QVector<Message> &messages, MessageView view = MessageView::History)
    {
        auto *history = static_cast<HistoryList *>(m_messages);
        if (view == MessageView::History) {
            history->showHistory(taskId(), m_empty, messages);
        } else if (view == MessageView::Context) {
            history->showContext(taskId(), m_empty, messages);
        } else {
            history->setLiveReadHistory(false);
            presentMessages(m_messages, m_empty, messages, view);
        }
    }
    void setChatStatus(const QString &text, bool loading = false)
    {
        static_cast<HistoryList *>(m_messages)->setLiveReadHistory(false);
        showEmpty(m_messages, m_empty, text, loading);
    }

    void setOnCopy(std::function<void(const QString &)> handler) { m_onCopy = std::move(handler); }
    void setOnSave(std::function<void(const TaskEdit &)> handler) { m_onSave = std::move(handler); }
    void setOnDelete(std::function<void()> handler) { m_onDelete = std::move(handler); }
    bool canDeleteTask() const
    {
        return m_task.editRights.remove && canUseTaskActions() && !hasUnsavedChanges();
    }
    void cancelFooterDelegation()
    {
        if (m_delegatingFromFooter)
            m_detailEditor->cancel();
    }
    void setProjectChoices(const QVector<Project> &projects)
    {
        m_requisiteProjects = projects;
        m_detailEditor->setProjectChoices(projects);
        showRequisiteProject();
        refreshRequisiteAvatars();
        updateButtons();
    }
    void setCanDetachProject(bool allowed)
    {
        m_detailEditor->setCanDetachProject(allowed);
        updateButtons();
    }
    void setMoreProjectsAvailable(bool available)
    {
        m_detailEditor->setMoreProjectsAvailable(available);
        updateButtons();
    }
    void setRequisiteAvatars(TaskRequisiteRow::AvatarSource source)
    {
        m_requisiteAvatars = std::move(source);
        m_detailEditor->setAvatarSource(m_requisiteAvatars);
        refreshRequisiteAvatars();
    }
    void setOnRequisiteAvatarChoicesChanged(std::function<void()> changed)
    {
        m_onRequisiteAvatarChoicesChanged = std::move(changed);
    }
    QList<QUrl> visibleRequisiteAvatarUrls() const
    {
        QList<QUrl> urls;
        urls.append(m_detailEditor->visibleAvatarUrls());
        for (const auto *row : {m_creator, m_assignee, m_project, m_observers})
            urls.append(row->visibleAvatarUrls(m_detailsScroll->viewport()));
        return urls;
    }
    void refreshRequisiteAvatars()
    {
        for (auto *row : {m_creator, m_assignee, m_project, m_observers})
            row->refreshAvatars(m_requisiteAvatars);
        m_detailEditor->refreshAvatars();
    }
    void setOnMoreProjects(std::function<void()> handler) { m_detailEditor->onMoreProjects = std::move(handler); }
    void setPeopleSearch(const std::function<void(const QString &, const QString &)> &handler)
    {
        m_detailEditor->setPeopleSearch(handler);
        updateButtons();
    }
    void setPeopleResults(const QString &id, const QVector<Person> &people) { m_detailEditor->setPeopleResults(id, people); }
    bool setPeopleSearchError(const QString &id) { return m_detailEditor->setPeopleSearchError(id); }
    void setOnMove(std::function<void(const QString &)> handler) { m_onMove = std::move(handler); }
    void setOnSend(std::function<void(const QString &)> handler) { m_onSend = std::move(handler); }
    void setOnSearch(std::function<void(const QString &)> handler) { m_onSearch = std::move(handler); }
    void setOnFile(std::function<void(bool)> handler) { m_onFile = std::move(handler); }

    // Несохранённые изменения карточки: черновик сообщения, правки полей, идущая отправка или загрузка.
    bool hasUnsavedChanges() const
    {
        if (m_sending || m_uploading || m_editing)
            return true;
        if (m_composer && !m_composer->draft().isEmpty())
            return true;
        return (m_fields && m_editMode && m_fields->edit().hasChanges())
            || (m_detailEditor && m_detailEditor->changed()) || hasStageDraft();
    }

    // Диалог подтверждения отбрасывания правок. По умолчанию — предупреждение с выбором.
    void setDiscardConfirmation(std::function<bool()> handler) { m_confirmDiscard = std::move(handler); }

    // Спрашивает пользователя перед отбрасыванием правок. false — закрытие отменено.
    bool confirmDiscardIfNeeded(bool remember = true)
    {
        if (m_discardConfirmed || !hasUnsavedChanges())
            return true;
        const bool confirmed = m_confirmDiscard ? m_confirmDiscard() : confirmDiscardDialog();
        if (confirmed && remember)
            m_discardConfirmed = true;
        return confirmed;
    }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        if (!confirmDiscardIfNeeded()) {
            event->ignore();
            return;
        }
        QDialog::closeEvent(event);
    }

    void reject() override
    {
        if (m_composer->cancelMessageEdit())
            return;
        if (!confirmDiscardIfNeeded())
            return;
        QDialog::reject();
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QDialog::resizeEvent(event);
        if (m_splitter)
            restoreDetailsWidth();
        if (m_popupAnchor) {
            m_detailEditor->fitChoiceLists();
            fitStageList();
        }
        positionRequisitePopup();
    }
    void changeEvent(QEvent *event) override
    {
        QDialog::changeEvent(event);
        if (event->type() == QEvent::WindowStateChange && m_maximize) {
            m_maximize->setText(isMaximized() ? QStringLiteral("Восстановить") : QStringLiteral("Развернуть"));
            m_maximize->setAccessibleName(isMaximized() ? QStringLiteral("Восстановить размер карточки задачи")
                                                      : QStringLiteral("Развернуть карточку задачи"));
            QTimer::singleShot(0, this, [this] { restoreDetailsWidth(); });
        }
    }
    bool eventFilter(QObject *target, QEvent *event) override
    {
        if (target == m_requisitePopup && event->type() == QEvent::LayoutRequest)
            positionRequisitePopup();
        if (target == m_input && event->type() == QEvent::KeyPress && !m_composer->composing()) {
            auto *key = static_cast<QKeyEvent *>(event);
            if (m_composer->handleComposerKey(key))
                return true;
            if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
                !(key->modifiers() & Qt::ShiftModifier)) {
                send();
                return true;
            }
        }
        return QDialog::eventFilter(target, event);
    }

private:
    bool confirmDiscardDialog()
    {
        auto *box = new QMessageBox(QMessageBox::Warning, QStringLiteral("Несохранённые изменения"),
            QStringLiteral("В карточке задачи есть несохранённые изменения. Закрыть карточку без сохранения?"),
            QMessageBox::NoButton, this);
        box->setAccessibleName(QStringLiteral("Несохранённые изменения в карточке"));
        auto *cancel = box->addButton(QStringLiteral("Отмена"), QMessageBox::RejectRole);
        auto *discard = box->addButton(QStringLiteral("Закрыть без сохранения"), QMessageBox::DestructiveRole);
        box->setDefaultButton(cancel);
        box->exec();
        box->deleteLater();
        return box->clickedButton() == discard;
    }

    void send()
    {
        if (m_composer->submitMessageEdit())
            return;
        if (m_onSend && m_send->isEnabled()) {
            m_composer->cancelTyping();
            m_onSend(m_composer->wireText());
        }
    }
    void searchMessages()
    {
        if (m_onSearch)
            m_onSearch(searchQuery());
    }
    void copy(const QString &value, const QString &feedback)
    {
        if (!m_onCopy || value.isEmpty())
            return;
        m_onCopy(value);
        m_copyFeedback->setText(feedback);
        m_copyFeedbackTimer.start();
    }
    void restoreDetailsWidth()
    {
        const int available = width() - 37;
        const int preferred = m_detailsWidth > 0 ? m_detailsWidth : qMin(440, available * 53 / 100);
        const int left = qBound(280, preferred, qMax(280, available - 285));
        m_splitter->setSizes({left, qMax(285, available - left)});
    }
    void setEditingDescription(bool enabled)
    {
        m_editMode = enabled;
        m_fields->setVisible(enabled);
        m_editActions->setVisible(enabled);
        m_descriptionRows->setVisible(!enabled);
        m_edit->setVisible(!enabled);
        updateButtons();
    }
    static QString personName(const QString &name, const QString &id)
    {
        return name.isEmpty() && !id.isEmpty() ? QStringLiteral("Сотрудник №%1").arg(id) : name;
    }
    static void setRequisite(TaskRequisiteRow *row, const QString &text, const QUrl &url = {})
    {
        row->setEntries(text.isEmpty() ? QVector<TaskRequisiteRow::Entry>() : QVector<TaskRequisiteRow::Entry>{{text, url}});
    }
    void setObservers(const Task &task)
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
    void showRequisiteProject()
    {
        const auto item = std::find_if(m_requisiteProjects.cbegin(), m_requisiteProjects.cend(), [this](const Project &project) {
            return !m_projectId.isEmpty() && project.id == m_projectId;
        });
        auto name = m_task.project.isEmpty() ? m_projectCaption : m_task.project;
        if (name.isEmpty() && item != m_requisiteProjects.cend())
            name = item->name;
        if (name.isEmpty() || item == m_requisiteProjects.cend()) {
            setRequisite(m_project, name);
            return;
        }
        m_project->setEntries({{name, item->avatarUrl, item->avatarColor, item->avatarVariant, item->avatarType}});
    }
    QListWidgetItem *stageItem(const QString &id) const
    {
        if (!id.isEmpty()) {
            for (int i = 0; i < m_stages->count(); ++i)
                if (m_stages->item(i)->data(Qt::UserRole).toString() == id)
                    return m_stages->item(i);
        }
        return nullptr;
    }
    void fitStageList()
    {
        const int visibleRows = height() >= 570 ? 7 : 4;
        m_stages->setFixedHeight(qMax(40, qMin(visibleRows, m_stages->count()) * TaskPickerChoiceDelegate::RowHeight + 5));
    }
    void updateStageSummary()
    {
        const auto *confirmed = stageItem(m_task.stageId);
        const auto *candidate = stageItem(m_stageChoice);
        m_stageSummary->setCurrent(confirmed ? confirmed->text() : m_task.stageName,
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
    void selectStage(QListWidgetItem *item)
    {
        if (!m_stageOpen || !m_stages->isEnabled() || !item)
            return;
        const auto choice = item->data(Qt::UserRole).toString();
        if (choice.isEmpty() || choice == m_stageChoice)
            return;
        m_stageChoice = choice;
        updateButtons();
        submitStage();
    }
    void submitStage()
    {
        if (!m_onMove || !m_move->isEnabled())
            return;
        m_stageSubmitAttempted = true;
        updateButtons();
        m_onMove(m_stageChoice);
    }
    bool hasStageDraft() const
    {
        return m_stageOpen && !m_stageChoice.isEmpty() && m_stageChoice != m_stageBaseline;
    }
    void openRequisitePopup(TaskRequisiteRow *row, QWidget *editor)
    {
        m_popupAnchor = row;
        m_detailsScroll->ensureWidgetVisible(row, 4, 4);
        if (m_requisitePopupLayout->indexOf(editor) < 0)
            m_requisitePopupLayout->addWidget(editor);
        m_requisitePopup->setAccessibleName(row->accessibleName());
        m_requisitePopup->show();
        m_requisitePopup->raise();
    }
    void positionRequisitePopup()
    {
        if (!m_popupAnchor || !m_requisitePopup->isVisible())
            return;
        const QRect bounds = rect().adjusted(9, 9, -9, -9);
        const QRect row(m_popupAnchor->mapTo(this, QPoint()), m_popupAnchor->size());
        const int width = qMin(bounds.width(), qBound(280, row.width() + 35, 475));
        const int height = qMin(bounds.height(), m_requisitePopup->sizeHint().height());
        const int below = row.bottom() + 5;
        const int y = below + height <= bounds.bottom() ? below : row.top() - height - 5;
        m_requisitePopup->setGeometry(qBound(bounds.left(), row.left(), bounds.right() - width + 1),
                                     qBound(bounds.top(), y, bounds.bottom() - height + 1), width, height);
    }
    void hideRequisitePopup()
    {
        m_popupAnchor = nullptr;
        m_requisitePopup->hide();
    }
    void scrollToRequisiteEditor(QWidget *editor)
    {
        QTimer::singleShot(0, editor, [this, editor] {
            if (!editor->isVisible())
                return;
            auto *focus = QApplication::focusWidget();
            m_detailsScroll->ensureWidgetVisible(focus && editor->isAncestorOf(focus) ? focus : editor, 8, 8);
        });
    }
    void closeStageEditor()
    {
        if (!m_stageOpen)
            return;
        m_stageOpen = false;
        m_stageBaseline.clear();
        m_stageChoice.clear();
        m_stageSubmitAttempted = false;
        hideRequisitePopup();
        updateButtons();
        if (m_existingStage->isEnabled())
            m_existingStage->setFocus(Qt::OtherFocusReason);
    }
    static void addTaskFile(TaskContentList *list, const MessageFile &file, int serial)
    {
        auto *item = workspace::addRow(list, {}, {}, {}, QString::number(serial));
        item->setData(workspace::MessageFiles, QVariant::fromValue(QVector<MessageFile>{file}));
        item->setData(Qt::AccessibleTextRole, file.name);
    }
    void showTaskContent(const Task &task, bool detailsLoaded)
    {
        static const QRegularExpression embedded(QStringLiteral("\\[disk\\s+file\\b([^\\]\\r\\n]*)\\]"),
                                                  QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression id(QStringLiteral("(?:^|\\s)id\\s*=\\s*[\"']?(n?[0-9]+)(?=[\"'\\s]|$)"),
                                           QRegularExpression::CaseInsensitiveOption);
        QString description;
        QVector<MessageFile> inlineFiles;
        QSet<QString> seen;
        qsizetype start = 0;
        auto matches = embedded.globalMatch(task.description);
        while (matches.hasNext()) {
            const auto match = matches.next();
            description += task.description.sliced(start, match.capturedStart() - start);
            const auto fileId = id.match(match.captured(1)).captured(1);
            auto file = std::find_if(task.files.cbegin(), task.files.cend(), [&fileId](const MessageFile &entry) {
                return !fileId.isEmpty() && (entry.id == fileId || entry.diskObjectId == fileId);
            });
            if (file == task.files.cend()) {
                description += QStringLiteral("Вложение недоступно");
            } else {
                description += QStringLiteral("Файл в описании");
                const auto key = file->id.isEmpty() ? file->diskObjectId : file->id;
                if (!seen.contains(key)) {
                    inlineFiles.append(*file);
                    seen.insert(key);
                }
            }
            start = match.capturedEnd();
        }
        description += task.description.sliced(start);
        m_descriptionRows->clear();
        auto *body = workspace::addMarkupRow(m_descriptionRows, {},
            description.trimmed().isEmpty() ? QStringLiteral("Описание пока не добавлено") : description,
            {}, QStringLiteral("task-description"));
        body->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        for (int i = 0; i < inlineFiles.size(); ++i)
            addTaskFile(m_descriptionRows, inlineFiles[i], i + 1);
        m_attachmentRows->clear();
        auto unavailable = attachmentNames(task.attachments);
        for (const auto &file : task.files) {
            unavailable.removeAll(attachmentNames({file.name}).value(0));
            addTaskFile(m_attachmentRows, file, m_attachmentRows->count() + 1);
        }
        for (const auto &name : unavailable) {
            MessageFile file;
            file.name = name;
            addTaskFile(m_attachmentRows, file, m_attachmentRows->count() + 1);
        }
        if (!m_attachmentRows->count())
            workspace::emptyRow(m_attachmentRows, detailsLoaded ? QStringLiteral("Файлов пока нет")
                                                                : QStringLiteral("Файлы ещё не загружены"));
        m_descriptionRows->requestHeight();
        m_attachmentRows->requestHeight();
    }
    void addTaskActions(QVBoxLayout *layout)
    {
        m_taskActions = new QWidget(this);
        auto *row = new QHBoxLayout(m_taskActions);
        row->setContentsMargins(0, 0, 0, 0);
        m_primaryTaskActionButton = button({}, {}, m_taskActions);
        m_primaryTaskActionButton->setObjectName(QStringLiteral("taskPrimaryAction"));
        connect(m_primaryTaskActionButton, &QPushButton::clicked, this, [this] {
            if (m_primaryTaskAction)
                submitTaskStatus(*m_primaryTaskAction);
        });
        row->addWidget(m_primaryTaskActionButton);
        m_completeTaskAction = button(TaskDetailEditor::actionLabel(TaskStatusAction::Complete),
                                      QStringLiteral("Завершить задачу"), m_taskActions);
        m_completeTaskAction->setObjectName(QStringLiteral("primary"));
        connect(m_completeTaskAction, &QPushButton::clicked, this, [this] { submitTaskStatus(TaskStatusAction::Complete); });
        row->addWidget(m_completeTaskAction);

        m_taskOverflowButton = button(QStringLiteral("Ещё"), QStringLiteral("Другие действия с задачей"), m_taskActions);
        auto *more = new QMenu(m_taskOverflowButton);
        more->setObjectName(QStringLiteral("taskOverflowMenu"));
        m_taskOverflowButton->setMenu(more);
        for (const auto action : {TaskStatusAction::Start, TaskStatusAction::Pause, TaskStatusAction::Renew,
                                  TaskStatusAction::Approve, TaskStatusAction::Disapprove, TaskStatusAction::Take,
                                  TaskStatusAction::Defer}) {
            auto *item = more->addAction(TaskDetailEditor::actionLabel(action));
            connect(item, &QAction::triggered, this, [this, action] { submitTaskStatus(action); });
            m_taskStatusMenuActions.append({action, item});
        }
        m_taskDelegateAction = more->addAction(QStringLiteral("Делегировать"));
        connect(m_taskDelegateAction, &QAction::triggered, this, [this] {
            if (!canUseTaskActions() || !m_task.editRights.delegate || !m_task.editRights.assignee)
                return;
            m_delegatingFromFooter = true;
            m_assignee->click();
            if (!m_detailEditor->active())
                m_delegatingFromFooter = false;
        });
        m_taskDeleteSeparator = more->addSeparator();
        m_taskDeleteAction = more->addAction(QStringLiteral("Удалить задачу"));
        connect(m_taskDeleteAction, &QAction::triggered, this, [this] {
            if (canDeleteTask() && m_onDelete)
                m_onDelete();
        });
        connect(more, &QMenu::aboutToShow, this, [this] {
            m_taskDeleteAction->setEnabled(canDeleteTask());
        });
        row->addWidget(m_taskOverflowButton);
        row->addStretch(1);

        m_taskRatingButton = button(QStringLiteral("Оценка"), QStringLiteral("Оценка задачи"), m_taskActions);
        auto *rating = new QMenu(m_taskRatingButton);
        rating->setObjectName(QStringLiteral("taskRatingMenu"));
        m_taskRatingButton->setMenu(rating);
        for (const auto value : {TaskRating::Unrated, TaskRating::Positive, TaskRating::Negative}) {
            auto *item = rating->addAction(taskRatingLabel(value));
            item->setCheckable(true);
            connect(item, &QAction::triggered, this, [this, value] {
                if (canUseTaskActions() && m_task.editRights.rate && m_task.rating != value && m_onSave) {
                    TaskEdit edit;
                    edit.rating = value;
                    m_onSave(edit);
                }
                updateTaskActions();
            });
            m_taskRatingMenuActions.append({value, item});
        }
        row->addWidget(m_taskRatingButton);
        layout->addWidget(m_taskActions);
    }

    static QString taskRatingLabel(TaskRating value)
    {
        switch (value) {
        case TaskRating::Unrated: return QStringLiteral("Без оценки");
        case TaskRating::Positive: return QStringLiteral("Положительная");
        case TaskRating::Negative: return QStringLiteral("Отрицательная");
        }
        return {};
    }

    bool canUseTaskActions() const
    {
        return m_writable && !m_editing && !m_moving && !m_sending && !m_uploading && !m_editMode
            && !m_stageOpen && !m_detailEditor->active() && !m_composer->editingMessage();
    }

    void submitTaskStatus(TaskStatusAction action)
    {
        if (!canUseTaskActions() || !m_task.statusActions.contains(action) || !m_onSave)
            return;
        TaskEdit edit;
        edit.statusAction = action;
        m_onSave(edit);
    }

    void updateTaskActions()
    {
        if (!m_taskActions)
            return;
        const bool enabled = canUseTaskActions();
        const auto &allowed = m_task.statusActions;
        m_primaryTaskAction.reset();
        for (const auto action : {TaskStatusAction::Start, TaskStatusAction::Pause, TaskStatusAction::Renew,
                                  TaskStatusAction::Approve, TaskStatusAction::Take}) {
            if (allowed.contains(action)) {
                m_primaryTaskAction = action;
                break;
            }
        }
        m_primaryTaskActionButton->setVisible(m_primaryTaskAction.has_value());
        m_primaryTaskActionButton->setEnabled(enabled);
        if (m_primaryTaskAction) {
            const auto text = TaskDetailEditor::actionLabel(*m_primaryTaskAction);
            m_primaryTaskActionButton->setText(text);
            m_primaryTaskActionButton->setAccessibleName(text);
        }
        const bool complete = allowed.contains(TaskStatusAction::Complete);
        m_completeTaskAction->setVisible(complete);
        m_completeTaskAction->setEnabled(enabled);

        const auto &rights = m_task.editRights;
        const bool delegate = rights.delegate && rights.assignee;
        bool overflow = delegate;
        for (const auto &[action, item] : std::as_const(m_taskStatusMenuActions)) {
            const bool visible = allowed.contains(action) && m_primaryTaskAction != action;
            item->setVisible(visible);
            item->setEnabled(enabled && visible);
            overflow |= visible;
        }
        m_taskDelegateAction->setVisible(delegate);
        m_taskDelegateAction->setEnabled(enabled && delegate);
        m_taskDeleteSeparator->setVisible(overflow && rights.remove);
        m_taskDeleteAction->setVisible(rights.remove);
        m_taskDeleteAction->setEnabled(enabled && rights.remove && m_input->document()->isEmpty()
                                       && m_composer->reply().id.isEmpty());
        overflow |= rights.remove;
        m_taskOverflowButton->setVisible(overflow);
        m_taskOverflowButton->setEnabled(enabled);

        const bool rated = m_task.rating != TaskRating::Unrated;
        m_taskRatingButton->setVisible(rights.rate || rated);
        m_taskRatingButton->setEnabled(enabled && rights.rate);
        m_taskRatingButton->setText(rated ? QStringLiteral("Оценка · %1").arg(taskRatingLabel(m_task.rating).toLower())
                                         : QStringLiteral("Оценка"));
        for (const auto &[value, item] : std::as_const(m_taskRatingMenuActions)) {
            item->setChecked(m_task.rating == value);
            item->setEnabled(enabled && rights.rate);
        }
        m_taskActions->setVisible(m_primaryTaskAction.has_value() || complete || overflow || rights.rate || rated);
    }

    void updateButtons()
    {
        m_relations->setWritable(m_writable);
        if (!m_send)
            return;
        const auto &rights = m_task.editRights;
        const bool allowsDescriptionEdits = rights.title || rights.description || rights.priority;
        m_detailEditor->setWriteState(m_writable && !m_editMode && !m_stageOpen, m_editing || m_moving);
        for (const auto &[button, field] : m_requisiteButtons)
            button->setEditable(m_detailEditor->canStart(field));
        m_fields->setWritable(m_writable && m_editMode && !m_editing, &rights);
        m_fields->descriptionEditor()->setUploadEnabled(m_writable && m_editMode && !m_editing && !m_uploading
                                                         && rights.description);
        m_edit->setEnabled(m_writable && !m_editing && !m_moving && !m_detailEditor->active()
                           && !m_stageOpen && allowsDescriptionEdits);
        m_cancelEdit->setEnabled(!m_editing && !m_uploading);
        m_save->setEnabled(m_writable && m_editMode && !m_editing && !m_uploading && allowsDescriptionEdits);
        const bool mayMove = m_writable && rights.stage && !m_moving && !m_editing && !m_editMode
            && !m_detailEditor->active() && m_stages->count() > 0;
        const bool stageCurrent = m_task.stageId == m_stageBaseline;
        m_existingStage->setEditable(mayMove && !m_stageOpen);
        m_stageEditor->setVisible(m_stageOpen);
        if (m_stageOpen)
            updateStageSummary();
        m_stages->setEnabled(m_stageOpen && mayMove);
        m_cancelStage->setEnabled(m_stageOpen && !m_moving && !m_editing);
        m_move->setVisible(hasStageDraft() && m_stageSubmitAttempted && !m_moving && !m_editing);
        m_move->setEnabled(m_stageOpen && mayMove && stageCurrent && hasStageDraft() && stageItem(m_stageChoice));
        m_move->setToolTip(stageCurrent ? QString() : QStringLiteral("Этап изменился на портале. Откройте поле заново."));
        const bool editingMessage = m_composer->editingMessage();
        const bool savingMessage = m_composer->messageEditPending();
        m_input->setReadOnly(!m_writable || savingMessage);
        m_emoji->setEnabled(m_writable && !savingMessage);
        m_attach->setEnabled(m_writable && !m_uploading);
        m_file->setEnabled(m_writable && !m_uploading && !editingMessage);
        const auto wire = m_composer->wireText();
        const auto text = wire.trimmed();
        const bool canSubmit = editingMessage ? m_composer->canSaveMessageEdit(wire)
                                              : !m_sending && !text.isEmpty() && text.size() <= 20000;
        m_send->setEnabled(m_writable && canSubmit);
        m_send->setToolTip(editingMessage ? (savingMessage ? QStringLiteral("Сохраняем сообщение…") : QStringLiteral("Сохранить сообщение"))
                                           : m_send->accessibleName());
        updateTaskActions();
    }

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
    std::function<void(const QString &)> m_onSend;
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
