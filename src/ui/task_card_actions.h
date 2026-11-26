#pragma once

#include "core/models.h"
#include <QWidget>

class QAction;
class QPushButton;

namespace bx::ui {

class TaskCardDialog;

class TaskCardActions final : public QWidget {
public:
    explicit TaskCardActions(TaskCardDialog &dialog);
    void refresh();

private:
    void submitTaskStatus(TaskStatusAction action);
    static QString taskRatingLabel(TaskRating value);

    TaskCardDialog &m_dialog;
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
};

} // namespace bx::ui
