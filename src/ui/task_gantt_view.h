#pragma once

#include "core/models.h"
#include <QListWidget>
#include <QLabel>
#include <functional>

namespace bx::ui {

class GanttRows;

class TaskGanttView final : public QWidget {
public:
    explicit TaskGanttView(QWidget *parent);
    void clear(const QString &message, bool loading = false);
    void setTasks(const QVector<Task> &tasks, bool limited);
    void setFilter(const QString &text);

    std::function<void(const QString &)> onOpen;

private:
    void updateRows();
    QVector<Task> m_data;
    QString m_filter;
    bool m_loaded = false;
    bool m_limited = false;
    QLabel *m_scale = nullptr;
    QListWidget *m_tasks = nullptr;
    GanttRows *m_rows = nullptr;
};

} // namespace bx::ui
