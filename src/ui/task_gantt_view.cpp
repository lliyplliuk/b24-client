#include "ui/task_gantt_view.h"

#include "ui/board_widget.h"
#include "ui/gantt_rows.h"
#include "ui/loading_indicator.h"
#include "ui/workspace_style.h"
#include <QScrollBar>
#include <QVBoxLayout>

namespace bx::ui {

TaskGanttView::TaskGanttView(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 2, 0, 0);
    m_scale = workspace::label(QString(), "quiet", this);
    m_scale->setAccessibleName(QStringLiteral("Период плановых дат"));
    layout->addWidget(m_scale);
    m_tasks = new QListWidget(this);
    m_tasks->setAccessibleName(QStringLiteral("Диаграмма Ганта проекта"));
    m_tasks->setUniformItemSizes(true);
    m_rows = new GanttRows(m_tasks);
    m_tasks->setItemDelegate(m_rows);
    layout->addWidget(m_tasks, 1);
    connect(m_tasks, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        const auto id = item->data(workspace::Id).toString();
        if (!id.isEmpty() && onOpen)
            onOpen(id);
    });
}

void TaskGanttView::clear(const QString &message, bool loading)
{
    m_data.clear();
    m_loaded = false;
    m_tasks->clear();
    m_scale->setText(message);
    setLoadingIndicator(m_scale, loading);
}

void TaskGanttView::setTasks(const QVector<Task> &tasks, bool limited)
{
    m_data = tasks;
    m_limited = limited;
    m_loaded = true;
    updateRows();
}

void TaskGanttView::setFilter(const QString &text)
{
    const auto query = text.trimmed();
    if (query == m_filter)
        return;
    m_filter = query;
    if (m_loaded)
        updateRows();
}

void TaskGanttView::updateRows()
{
    const auto selected = m_tasks->currentItem()
        ? m_tasks->currentItem()->data(workspace::Id).toString() : QString();
    const auto position = m_tasks->verticalScrollBar()->value();
    m_tasks->clear();
    m_rows->m_first = {};
    m_rows->m_last = {};
    for (const auto &task : m_data) {
        const auto start = task.planStart.toLocalTime().date();
        const auto end = task.planEnd.toLocalTime().date();
        const bool planned = start.isValid() && end.isValid() && end >= start;
        if (planned) {
            if (!m_rows->m_first.isValid() || start < m_rows->m_first)
                m_rows->m_first = start;
            if (!m_rows->m_last.isValid() || end > m_rows->m_last)
                m_rows->m_last = end;
        }
        if (!BoardWidget::matchesTaskFilter(task, m_filter))
            continue;
        auto *item = workspace::addRow(m_tasks, task.name,
            planned ? QStringLiteral("%1 — %2").arg(start.toString(QStringLiteral("dd.MM.yyyy")),
                                                      end.toString(QStringLiteral("dd.MM.yyyy")))
                    : QStringLiteral("Без планового интервала"), {}, task.id);
        item->setData(Qt::UserRole + 50, start);
        item->setData(Qt::UserRole + 51, end);
        if (!selected.isEmpty() && task.id == selected)
            m_tasks->setCurrentItem(item);
    }
    QString range = m_data.isEmpty() ? QStringLiteral("Нет задач") : QStringLiteral("У задач нет плановых интервалов");
    if (!m_filter.isEmpty() && m_tasks->count() == 0) {
        range = m_limited ? QStringLiteral("В загруженных задачах совпадений нет")
                          : QStringLiteral("Ничего не найдено");
    } else if (m_rows->m_first.isValid()) {
        range = QStringLiteral("Плановые даты %1 — %2").arg(m_rows->m_first.toString(QStringLiteral("dd.MM.yyyy")),
                                                           m_rows->m_last.toString(QStringLiteral("dd.MM.yyyy")));
    }
    if (m_limited)
        range += QStringLiteral(" · Показана часть задач");
    m_scale->setText(range);
    setLoadingIndicator(m_scale, false);
    m_tasks->verticalScrollBar()->setValue(position);
}

} // namespace bx::ui
