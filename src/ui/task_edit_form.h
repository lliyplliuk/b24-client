#pragma once

#include "ui/task_detail_editor.h"
#include <QDialog>

class QFormLayout;

namespace bx::ui {

class TaskFields;

class TaskEditForm final : public QDialog {
public:
    using Field = TaskDetailEditor::Field;

    explicit TaskEditForm(QWidget *parent = nullptr);

    void configure(const QString &projectId, bool canDetachProject);
    const QString &projectId() const { return m_draft.projectId; }
    void setProjectChoices(const QVector<Project> &projects);
    void setMoreProjectsAvailable(bool available);
    void setOnMoreProjects(std::function<void()> handler);
    void setOnStages(std::function<void(const QString &)> handler);
    void setPeopleSearch(const std::function<void(const QString &, const QString &)> &handler);
    void setPeopleResults(const QString &request, const QVector<Person> &people);
    void setPeopleSearchError(const QString &request);
    void setCurrentPerson(Person person);
    void setRequisiteAvatars(RequisiteAvatarSource source, std::function<void()> visibleChanged);
    QList<QUrl> visibleRequisiteAvatarUrls() const;
    void refreshRequisiteAvatars();

    void setStages(const QVector<Stage> &stages);
    void setStagesError(const QString &reason);

    void setOnSave(std::function<void(const TaskEdit &)> handler);
    void setEnabledForWrite(bool writable, bool uploading = false);
    void setStatus(const QString &text, bool loading = false);
    void setCreatedTask(const QString &task, const QString &projectToOpen);
    const QString &createdTask() const { return m_createdTask; }
    const QString &projectToOpen() const { return m_projectToOpen; }
    const QStringList &queuedFiles() const { return m_paths; }
    void confirmFirstFile();
    void reject() override;

private:
    QPushButton *addRequisite(QFormLayout *layout, QWidget *parent, const QString &caption, Field field);

    void syncDraftRequisites();

    void refreshRequisites();

    void loadStages();

    void refreshFileActions();

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

} // namespace bx::ui
