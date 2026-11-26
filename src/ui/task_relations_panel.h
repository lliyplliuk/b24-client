#pragma once

#include "core/models.h"
#include <QTreeWidget>
#include <QListWidget>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <functional>

namespace bx::ui {

class TaskRelationsPanel final : public QWidget {
public:
    explicit TaskRelationsPanel(QWidget *parent);

    std::function<void(const QString &, const QString &)> onAdd;
    std::function<void(const QString &, bool)> onToggle;
    std::function<void(const QString &)> onOpenChild;
    std::function<void()> onCreateChild;
    std::function<void()> onHistory;
    void setChecklist(const QVector<TaskChecklistItem> &items);
    void setSubtasks(const QVector<Task> &tasks);
    void setDependencies(const QVector<TaskDependency> &dependencies);
    void setHistory(const QVector<TaskHistoryEntry> &history, bool limited);
    void historyFailed();
    void setWritable(bool writable);
    bool checklistPending() const { return m_checklistPending; }
    void checklistReadFailed();
    void subtasksReadFailed();
    void dependenciesReadFailed();
    void setChecklistPending(bool pending);
    void confirmChecklistChange();

private:
    void updateControls();
    QTreeWidget *m_checklist = nullptr;
    QListWidget *m_children = nullptr;
    QListWidget *m_dependencies = nullptr;
    QListWidget *m_history = nullptr;
    QComboBox *m_group = nullptr;
    QLineEdit *m_title = nullptr;
    QPushButton *m_add = nullptr;
    QPushButton *m_newChild = nullptr;
    bool m_writable = false;
    bool m_checklistLoaded = false;
    bool m_checklistPending = false;
    bool m_adding = false;
    bool m_historyRequested = false;
};

} // namespace bx::ui
