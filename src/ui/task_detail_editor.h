#pragma once

#include "core/models.h"
#include "ui/task_people_input.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QDateTimeEdit;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QStackedWidget;

namespace bx::ui {

class TaskPickerSummary;

class TaskDetailEditor final : public QWidget {
public:
    enum class Field { None, Creator, Assignee, Observers, Deadline, Status, Project };
    enum class Mode { ExistingTask, NewTask };

    explicit TaskDetailEditor(QWidget *parent, Mode mode = Mode::ExistingTask);
    static QString formattedDate(const QString &value);
    static QString actionLabel(TaskStatusAction action);
    void setTask(const Task &task);
    void setProjectChoices(const QVector<Project> &projects);
    void setCanDetachProject(bool allowed);
    void setMoreProjectsAvailable(bool available);
    void setWriteState(bool writable, bool busy);
    void setPeopleSearch(const std::function<void(const QString &, const QString &)> &handler);
    void setPeopleResults(const QString &id, const QVector<Person> &people);
    bool setPeopleSearchError(const QString &id);
    bool active() const { return m_active != Field::None; }
    bool canStart(Field field) const;
    void open(Field field);
    void cancel();
    void focusEditor();
    void setAvatarSource(RequisiteAvatarSource source);
    void fitChoiceLists();
    QList<QUrl> visibleAvatarUrls() const;
    void refreshAvatars();
    std::function<void()> onAvatarsVisible;
    std::function<void()> onPickerLayoutChanged;
    bool changed() const;
    void accept(Task &task);
    std::function<void(const TaskEdit &)> onSave;
    std::function<void()> onModeChanged;
    std::function<void()> onMoreProjects;

private:
    struct FormRow {
        Field field;
        QString caption;
        QWidget *editor;
    };
    void addField(Field field, const QString &caption, QWidget *control);
    TaskPeopleInput *addPeopleField(Field field, const QString &caption, bool multiple = false);
    QComboBox *combo(const QString &caption);
    QComboBox *addCombo(Field field, const QString &caption);

    static QStringList ids(const QVector<TaskPerson> &people);
    static bool samePeople(QStringList left, QStringList right);
    bool allowed(Field field) const;
    void start(Field field);
    static QString projectOption(const QString &projectId);
    QListWidgetItem *listedProject(const QString &id) const;
    void selectProject(QListWidgetItem *item);
    bool appliesOnSelection() const;
    void submitPatch();
    void fitProjectList();
    void fillProjects();
    void finish();
    TaskEdit patch() const;
    bool stillCurrent() const;
    const Project *projectChoice(const QString &id) const;
    void refreshProjectPill();
    void refreshSummary();
    void showPending(const TaskEdit &edit);
    void updateControls();
    QString conflictText() const;
    void updateApply();

    const Mode m_mode;
    QLabel *m_heading = nullptr;
    TaskPickerSummary *m_summary = nullptr;
    QLabel *m_conflict = nullptr;
    QStackedWidget *m_editors = nullptr;
    QPushButton *m_cancel = nullptr;
    QPushButton *m_apply = nullptr;
    TaskPeopleInput *m_creator = nullptr;
    TaskPeopleInput *m_assignee = nullptr;
    TaskPeopleInput *m_observers = nullptr;
    QDateTimeEdit *m_deadline = nullptr;
    QLineEdit *m_deadlineReason = nullptr;
    QCheckBox *m_noDeadline = nullptr;
    QComboBox *m_status = nullptr;
    QListWidget *m_project = nullptr;
    QLineEdit *m_projectSearch = nullptr;
    TaskSelectionField *m_projectField = nullptr;
    QLabel *m_projectEmpty = nullptr;
    QWidget *m_focusEditor = nullptr;
    QString m_projectChoice;
    QString m_projectChoiceName;
    RequisiteAvatarSource m_avatarSource;
    QPushButton *m_moreProjects = nullptr;
    QVector<FormRow> m_rows;
    QVector<Project> m_projects;
    Task m_task;
    Task m_baseline;
    QDateTime m_deadlineInitially;
    Field m_active = Field::None;
    bool m_autoSubmitAttempted = false;
    bool m_writable = false;
    bool m_busy = false;
    bool m_peopleSearchAvailable = false;
    bool m_canDetachProject = false;
};

} // namespace bx::ui
