#include "ui/board_widget.h"
#include "ui/board_column.h"
#include "ui/board_column_grip.h"
#include "ui/board_card_painter.h"
#include "ui/board_drag_list.h"
#include "ui/loading_indicator.h"
#include "ui/workspace_rows.h"

#include <QApplication>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>

namespace bx {

namespace {
constexpr int DefaultColumnWidth = 232;
constexpr int MinimumColumnWidth = 150;
constexpr int MaximumColumnWidth = 640;

QSettings boardPreferences()
{
    return QSettings(QSettings::defaultFormat(), QSettings::UserScope, QStringLiteral("bitrix24-client"),
                     QStringLiteral("bitrix24-client"));
}
} // namespace

void BoardWidget::clearColumns()
{
    ++m_columnRevision;
    while (auto *item = m_columnsLayout->takeAt(0)) {
        auto *column = dynamic_cast<BoardColumn *>(item->widget());
        // A refresh can arrive inside QDrag's event loop. Keep its source alive until it unwinds.
        if (column && column->list()->retireIfDragging())
            column->hide();
        else
            delete item->widget();
        delete item;
    }
}

void BoardWidget::setColumnWidthContext(const QString &projectId, const QString &accountPreference)
{
    QString group;
    if (!projectId.isEmpty() && !accountPreference.isEmpty())
        group = accountPreference + QLatin1Char('/') + QString::fromLatin1(QUrl::toPercentEncoding(projectId));
    if (m_widthProject == projectId && m_widthGroup == group)
        return;

    m_widthProject = projectId;
    m_widthGroup = group;
    m_columnWidths.clear();
    if (!m_stages.isEmpty() && m_view == View::Kanban)
        setStages(m_stages, m_alreadyFiltered);
}

void BoardWidget::setStages(const QVector<Stage> &stages, bool alreadyFiltered)
{
    m_stages = stages;
    m_alreadyFiltered = alreadyFiltered;
    ui::setLoadingIndicator(m_empty, false);
    updatePageStatus();
    const int previousScroll = m_scroll->horizontalScrollBar()->value();
    const auto *focused = QApplication::focusWidget();
    const auto focusedStage = focused && m_container->isAncestorOf(focused)
        ? focused->property("stageId").toString() : QString();
    QHash<QString, int> columnScrolls;
    for (const auto *column : m_container->findChildren<QListWidget *>())
        columnScrolls.insert(column->property("stageId").toString(), column->verticalScrollBar()->value());
    clearColumns();
    if (m_view == View::List) {
        rebuildList();
        m_scroll->hide();
        const bool showList = !m_stages.isEmpty() || !m_filter.trimmed().isEmpty();
        m_list->setVisible(showList);
        if (m_stages.isEmpty()) {
            m_empty->setText(QStringLiteral("На этой доске пока нет этапов"));
            m_empty->setVisible(!showList);
        } else {
            m_empty->hide();
        }
        return;
    }
    m_list->hide();
    if (stages.isEmpty()) {
        m_scroll->hide();
        m_empty->setText(m_filter.trimmed().isEmpty() ? QStringLiteral("На этой доске пока нет этапов")
                                                    : noMatchesText());
        m_empty->show();
        return;
    }
    QWidget *focus = nullptr;
    auto settings = boardPreferences();
    for (const auto &stage : stages) {
        auto *column = new BoardColumn(this, stage, m_container);
        const auto widthKey = m_widthGroup.isEmpty() || stage.id.isEmpty() ? QString()
            : m_widthGroup + QLatin1Char('/') + QString::fromLatin1(QUrl::toPercentEncoding(stage.id));
        const int savedWidth = m_columnWidths.value(stage.id, widthKey.isEmpty() ? DefaultColumnWidth
            : settings.value(widthKey, DefaultColumnWidth).toInt());
        const int initialWidth = qBound(MinimumColumnWidth, savedWidth, MaximumColumnWidth);
        column->setFixedWidth(initialWidth);
        auto *list = column->list();
        list->setItemDelegate(new BoardCardPainter(list, [this](const QUrl &url) {
            return m_avatarSource && !url.isEmpty() ? m_avatarSource(url) : nullptr;
        }));
        watchPageScroll(list);
        connect(list->verticalScrollBar(), &QScrollBar::valueChanged, this, &BoardWidget::visibleCardsChanged);
        int shown = 0;
        for (const auto &task : stage.tasks) {
            if (!matches(task))
                continue;
            ++shown;
            auto *item = BoardCardPainter::addTask(list, task);
            if (!task.id.isEmpty() && task.id == m_selectedTaskId)
                list->setCurrentItem(item);
        }
        const auto total = m_filter.trimmed().isEmpty() ? stage.taskCount.value_or(stage.tasks.size()) : shown;
        column->setTaskCount(total);
        const bool moreAvailable = m_filter.trimmed().isEmpty() && !m_alreadyFiltered && stage.hasMoreTasks;
        if (shown == 0 && !moreAvailable)
            workspace::emptyRow(list, m_filter.trimmed().isEmpty() ? QStringLiteral("Нет задач") : noMatchesText());
        if (stage.id == focusedStage)
            focus = list;

        const auto openTask = [this, list](QListWidgetItem *item) {
            const auto taskId = item->data(workspace::Id).toString();
            if (taskId.isEmpty())
                return;
            m_selectedTaskId = taskId;
            for (auto *columnList : m_container->findChildren<QListWidget *>()) {
                if (columnList != list)
                    columnList->clearSelection();
            }
            emit taskClicked(taskId);
        };
        connect(list, &QListWidget::itemClicked, this, openTask);
        connect(list, &QListWidget::itemActivated, this, openTask);
        auto *grip = new BoardColumnGrip(column, column->title(), [this, column, id = stage.id, widthKey, initialWidth](int proposed, bool finished) {
            const int width = qBound(MinimumColumnWidth, proposed, MaximumColumnWidth);
            if (column->width() != width)
                column->setFixedWidth(width);
            if (finished && !id.isEmpty() && m_columnWidths.value(id, initialWidth) != width) {
                m_columnWidths.insert(id, width);
                if (!widthKey.isEmpty()) {
                    auto settings = boardPreferences();
                    settings.setValue(widthKey, width);
                }
            }
        });
        grip->show();
        m_columnsLayout->addWidget(column);
        if (const int previous = columnScrolls.value(stage.id); previous > 0) {
            list->doItemsLayout();
            list->verticalScrollBar()->setValue(previous);
        }
    }
    m_columnsLayout->addStretch(1);
    m_empty->hide();
    m_scroll->show();
    m_scroll->horizontalScrollBar()->setValue(previousScroll);
    if (focus)
        focus->setFocus(Qt::OtherFocusReason);
    emit visibleCardsChanged();
}

} // namespace bx
