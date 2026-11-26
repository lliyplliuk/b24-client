#include "ui/task_card_actions.h"

#include "ui/task_card_dialog.h"
#include "ui/task_card_requisites.h"
#include "ui/task_dialog_controls.h"

#include <QAction>
#include <QHBoxLayout>
#include <QMenu>
#include <QPushButton>

namespace bx::ui {

TaskCardActions::TaskCardActions(TaskCardDialog &dialog)
    : QWidget(&dialog), m_dialog(dialog)
{
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    m_primaryTaskActionButton = button({}, {}, this);
    m_primaryTaskActionButton->setObjectName(QStringLiteral("taskPrimaryAction"));
    connect(m_primaryTaskActionButton, &QPushButton::clicked, this, [this] {
        if (m_primaryTaskAction)
            submitTaskStatus(*m_primaryTaskAction);
    });
    row->addWidget(m_primaryTaskActionButton);
    m_completeTaskAction = button(TaskDetailEditor::actionLabel(TaskStatusAction::Complete),
                                  QStringLiteral("Завершить задачу"), this);
    m_completeTaskAction->setObjectName(QStringLiteral("primary"));
    connect(m_completeTaskAction, &QPushButton::clicked, this, [this] { submitTaskStatus(TaskStatusAction::Complete); });
    row->addWidget(m_completeTaskAction);

    m_taskOverflowButton = button(QStringLiteral("Ещё"), QStringLiteral("Другие действия с задачей"), this);
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
        if (!m_dialog.canUseTaskActions() || !m_dialog.m_task.editRights.delegate || !m_dialog.m_task.editRights.assignee)
            return;
        m_dialog.m_requisites->startDelegation();
    });
    m_taskDeleteSeparator = more->addSeparator();
    m_taskDeleteAction = more->addAction(QStringLiteral("Удалить задачу"));
    connect(m_taskDeleteAction, &QAction::triggered, this, [this] {
        if (m_dialog.canDeleteTask() && m_dialog.m_onDelete)
            m_dialog.m_onDelete();
    });
    connect(more, &QMenu::aboutToShow, this, [this] {
        m_taskDeleteAction->setEnabled(m_dialog.canDeleteTask());
    });
    row->addWidget(m_taskOverflowButton);
    row->addStretch(1);

    m_taskRatingButton = button(QStringLiteral("Оценка"), QStringLiteral("Оценка задачи"), this);
    auto *rating = new QMenu(m_taskRatingButton);
    rating->setObjectName(QStringLiteral("taskRatingMenu"));
    m_taskRatingButton->setMenu(rating);
    for (const auto value : {TaskRating::Unrated, TaskRating::Positive, TaskRating::Negative}) {
        auto *item = rating->addAction(taskRatingLabel(value));
        item->setCheckable(true);
        connect(item, &QAction::triggered, this, [this, value] {
            if (m_dialog.canUseTaskActions() && m_dialog.m_task.editRights.rate && m_dialog.m_task.rating != value && m_dialog.m_onSave) {
                TaskEdit edit;
                edit.rating = value;
                m_dialog.m_onSave(edit);
            }
            refresh();
        });
        m_taskRatingMenuActions.append({value, item});
    }
    row->addWidget(m_taskRatingButton);
}

QString TaskCardActions::taskRatingLabel(TaskRating value)
{
    switch (value) {
    case TaskRating::Unrated: return QStringLiteral("Без оценки");
    case TaskRating::Positive: return QStringLiteral("Положительная");
    case TaskRating::Negative: return QStringLiteral("Отрицательная");
    }
    return {};
}

void TaskCardActions::submitTaskStatus(TaskStatusAction action)
{
    if (!m_dialog.canUseTaskActions() || !m_dialog.m_task.statusActions.contains(action) || !m_dialog.m_onSave)
        return;
    TaskEdit edit;
    edit.statusAction = action;
    m_dialog.m_onSave(edit);
}

void TaskCardActions::refresh()
{
    const bool enabled = m_dialog.canUseTaskActions();
    const auto &allowed = m_dialog.m_task.statusActions;
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

    const auto &rights = m_dialog.m_task.editRights;
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
    m_taskDeleteAction->setEnabled(enabled && rights.remove && m_dialog.m_input->document()->isEmpty()
                                   && m_dialog.m_composer->reply().id.isEmpty());
    overflow |= rights.remove;
    m_taskOverflowButton->setVisible(overflow);
    m_taskOverflowButton->setEnabled(enabled);

    const bool rated = m_dialog.m_task.rating != TaskRating::Unrated;
    m_taskRatingButton->setVisible(rights.rate || rated);
    m_taskRatingButton->setEnabled(enabled && rights.rate);
    m_taskRatingButton->setText(rated ? QStringLiteral("Оценка · %1").arg(taskRatingLabel(m_dialog.m_task.rating).toLower())
                                     : QStringLiteral("Оценка"));
    for (const auto &[value, item] : std::as_const(m_taskRatingMenuActions)) {
        item->setChecked(m_dialog.m_task.rating == value);
        item->setEnabled(enabled && rights.rate);
    }
    setVisible(m_primaryTaskAction.has_value() || complete || overflow || rights.rate || rated);
}

} // namespace bx::ui
