#pragma once

#include "core/models.h"
#include <QWidget>

class QLineEdit;
class QPushButton;
class QCheckBox;

namespace bx::ui {

class TaskDescriptionEditor;

class TaskFields final : public QWidget {
public:
    explicit TaskFields(QWidget *parent, bool includeDeadline = true);

    void setTask(const Task &task, bool reset = false);

    TaskEdit edit() const;

    QString validation() const;

    void setWritable(bool writable, const TaskEditRights *rights = nullptr);

    TaskDescriptionEditor *descriptionEditor() const { return m_description; }

private:
    static bool highPriority(const Task &task);

    Task m_initial;
    QLineEdit *m_title = nullptr;
    TaskDescriptionEditor *m_description = nullptr;
    QLineEdit *m_deadline = nullptr;
    QPushButton *m_reset = nullptr;
    QCheckBox *m_priority = nullptr;
};

} // namespace bx::ui
