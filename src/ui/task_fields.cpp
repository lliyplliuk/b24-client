#include "ui/task_fields.h"

#include "ui/task_description_editor.h"
#include "ui/task_dialog_controls.h"
#include "ui/workspace_style.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace bx::ui {

TaskFields::TaskFields(QWidget *parent, bool includeDeadline)
    : QWidget(parent)
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

void TaskFields::setTask(const Task &task, bool reset)
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

TaskEdit TaskFields::edit() const
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

QString TaskFields::validation() const
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

void TaskFields::setWritable(bool writable, const TaskEditRights *rights)
{
    m_title->setReadOnly(!writable || (rights && !rights->title));
    m_description->setEditEnabled(writable && (!rights || rights->description));
    if (m_deadline) {
        m_deadline->setReadOnly(!writable || (rights && !rights->deadline));
        m_reset->setEnabled(!m_deadline->isReadOnly());
    }
    m_priority->setEnabled(writable && (!rights || rights->priority));
}

bool TaskFields::highPriority(const Task &task)
{
    return task.priority.compare(QStringLiteral("Высокий"), Qt::CaseInsensitive) == 0
        || task.priority == QStringLiteral("high") || task.priority == QStringLiteral("2");
}

} // namespace bx::ui
