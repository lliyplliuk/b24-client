#pragma once

#include "ui/task_detail_editor.h"
#include "ui/task_requisite_row.h"

namespace bx::ui {

class TaskCardDialog;
class TaskPickerSummary;

// QObject provides the event filter and the connection context for lambdas.
// This helper declares no signals, slots or properties that require Q_OBJECT.
class TaskCardRequisites final : public QObject {
public:
    TaskCardRequisites(TaskCardDialog &dialog, QWidget *fields, QVBoxLayout *details,
                      QVBoxLayout *project, QVBoxLayout *observers);
    void showTask(const Task &task, bool changedProject);
    void syncStage(const Task &task, bool changedProject, const QString &previousStage);
    void updateWriteState();
    void resizePopup();
    void startDelegation();
    bool stageOpen() const { return m_stageOpen; }
    TaskDetailEditor *editor() const { return m_detailEditor; }
    bool hasStageDraft() const;
    void closeStageEditor();

    void setStages(const QVector<Stage> &stages);
    void setProjectCaption(const QString &caption);
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

protected:
    bool eventFilter(QObject *target, QEvent *event) override;

private:
    static QString personName(const QString &name, const QString &id);
    static void setRequisite(TaskRequisiteRow *row, const QString &text, const QUrl &url = {});
    void setObservers(const Task &task);
    void showRequisiteProject();
    QListWidgetItem *stageItem(const QString &id) const;
    void fitStageList();
    void updateStageSummary();
    void selectStage(QListWidgetItem *item);
    void submitStage();
    void openRequisitePopup(TaskRequisiteRow *row, QWidget *editor);
    void positionRequisitePopup();
    void hideRequisitePopup();
    void scrollToRequisiteEditor(QWidget *editor);

    TaskCardDialog &m_dialog;
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
    QListWidget *m_stages = nullptr;
    QPushButton *m_move = nullptr;
    bool m_delegatingFromFooter = false;
    std::function<void(const QString &)> m_onMove;
};

} // namespace bx::ui
