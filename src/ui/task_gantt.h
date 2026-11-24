#pragma once

#include "core/models.h"
#include "ui/board_widget.h"
#include "ui/workspace_style.h"

#include <QVBoxLayout>
#include <QScrollBar>
#include <functional>

namespace bx::ui {

class GanttRows final : public QStyledItemDelegate {
public:
    explicit GanttRows(QObject *parent) : QStyledItemDelegate(parent) {}

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return {0, 50}; }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        p->save();
        const auto rect = option.rect.adjusted(5, 1, -5, -1);
        if (option.state & QStyle::State_Selected)
            p->fillRect(rect, workspace::Selected);
        const int labelWidth = qBound(110, rect.width() / 3, 280);
        const auto title = index.data(workspace::Title).toString();
        const auto subtitle = index.data(workspace::Detail).toString();
        const QRect heading(rect.left() + 7, rect.top() + 4, labelWidth - 16, 22);
        p->setFont(workspace::rowFont(bodyTextPixels, true));
        p->setPen(workspace::Ink);
        p->drawText(heading, Qt::AlignVCenter, QFontMetrics(p->font()).elidedText(title, Qt::ElideRight, heading.width()));
        p->setFont(workspace::rowFont(secondaryTextPixels));
        p->setPen(workspace::Muted);
        const auto sub = heading.translated(0, 21);
        p->drawText(sub, Qt::AlignVCenter, QFontMetrics(p->font()).elidedText(subtitle, Qt::ElideRight, sub.width()));
        const QRect track(rect.left() + labelWidth, rect.top() + 17, std::max(1, rect.width() - labelWidth), 13);
        p->fillRect(track, QColor(QStringLiteral("#eef2ed")));
        const auto start = index.data(Qt::UserRole + 50).toDate();
        const auto end = index.data(Qt::UserRole + 51).toDate();
        if (m_first.isValid() && start.isValid() && end.isValid() && end >= start) {
            const auto width = std::max<qint64>(1, m_first.daysTo(m_last) + 1);
            const auto x = track.left() + int(double(m_first.daysTo(start)) / width * track.width());
            const auto right = track.left() + int(double(m_first.daysTo(end) + 1) / width * track.width());
            p->fillRect(QRect(x, track.top(), std::max(2, right - x), track.height()), workspace::Accent);
        }
        p->restore();
    }

    QDate m_first;
    QDate m_last;
};

class TaskGanttView final : public QWidget {
public:
    explicit TaskGanttView(QWidget *parent) : QWidget(parent)
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

    void clear(const QString &message, bool loading = false)
    {
        m_data.clear();
        m_loaded = false;
        m_tasks->clear();
        m_scale->setText(message);
        setLoadingIndicator(m_scale, loading);
    }

    void setTasks(const QVector<Task> &tasks, bool limited)
    {
        m_data = tasks;
        m_limited = limited;
        m_loaded = true;
        updateRows();
    }

    void setFilter(const QString &text)
    {
        const auto query = text.trimmed();
        if (query == m_filter)
            return;
        m_filter = query;
        if (m_loaded)
            updateRows();
    }

    std::function<void(const QString &)> onOpen;

private:
    void updateRows()
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

    QVector<Task> m_data;
    QString m_filter;
    bool m_loaded = false;
    bool m_limited = false;
    QLabel *m_scale = nullptr;
    QListWidget *m_tasks = nullptr;
    GanttRows *m_rows = nullptr;
};

} // namespace bx::ui
