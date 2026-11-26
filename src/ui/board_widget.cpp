#include "ui/board_widget.h"
#include "ui/board_card_painter.h"
#include "ui/loading_indicator.h"
#include "ui/workspace_rows.h"

#include <QLabel>
#include <QListWidget>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>

#include <utility>

namespace bx {

namespace {
constexpr int ColumnGap = 12;
} // namespace

QListWidgetItem *BoardWidget::addListedTask(QListWidget *list, const Task &task, const Stage &stage)
{
    return workspace::addRow(list, task.name, stage.name.isEmpty() ? task.assignee : stage.name, {}, task.id);
}

BoardWidget::BoardWidget(QWidget *parent) : QWidget(parent)
{
    setAccessibleName(QStringLiteral("Доска проекта"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_empty = workspace::label(QStringLiteral("Выберите проект, чтобы открыть доску"), "empty", this);
    m_empty->setAlignment(Qt::AlignCenter);
    root->addWidget(m_empty, 1);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_container = new QWidget(m_scroll);
    m_columnsLayout = new QHBoxLayout(m_container);
    m_columnsLayout->setContentsMargins(0, 0, 0, 0);
    m_columnsLayout->setSpacing(ColumnGap);
    m_scroll->setWidget(m_container);
    m_scroll->viewport()->installEventFilter(this);
    connect(m_scroll->horizontalScrollBar(), &QScrollBar::valueChanged, this, &BoardWidget::visibleCardsChanged);
    m_scroll->hide();
    root->addWidget(m_scroll, 1);

    m_list = new QListWidget(this);
    m_list->setAccessibleName(QStringLiteral("Список задач"));
    workspace::configureRows(m_list, workspace::Row::Card);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    watchPageScroll(m_list);
    m_list->hide();
    const auto openTask = [this](QListWidgetItem *item) {
        const auto taskId = item->data(workspace::Id).toString();
        if (taskId.isEmpty())
            return;
        m_selectedTaskId = taskId;
        emit taskClicked(taskId);
    };
    connect(m_list, &QListWidget::itemClicked, this, openTask);
    connect(m_list, &QListWidget::itemActivated, this, openTask);
    root->addWidget(m_list, 1);
    m_pageStatus = workspace::label({}, "quiet", this);
    m_pageStatus->setMinimumHeight(m_pageStatus->fontMetrics().height() + 4);
    m_pageStatus->hide();
    root->addWidget(m_pageStatus);
}

void BoardWidget::setAvatarSource(BoardCardPainter::AvatarSource source)
{
    m_avatarSource = std::move(source);
    refreshAvatars();
}

void BoardWidget::setTaskMoveGuard(TaskMoveGuard guard)
{
    m_taskMoveGuard = std::move(guard);
}

QList<QUrl> BoardWidget::visibleAvatarUrls() const
{
    QList<QUrl> urls;
    if (m_view != View::Kanban || !m_scroll->isVisible())
        return urls;
    for (auto *column : m_container->findChildren<QListWidget *>()) {
        auto *viewport = column->viewport();
        const auto *outer = m_scroll->viewport();
        const QRect outerRect(viewport->mapFrom(outer, QPoint()), outer->size());
        const auto visible = viewport->rect().intersected(outerRect);
        if (!viewport->isVisible() || visible.isEmpty())
            continue;
        const auto first = column->indexAt(QPoint(visible.center().x(), visible.top()));
        for (int row = first.isValid() ? first.row() : 0; row < column->count(); ++row) {
            const auto *item = column->item(row);
            const auto rowRect = column->visualItemRect(item);
            if (rowRect.top() > visible.bottom())
                break;
            if (!rowRect.intersects(visible))
                continue;
            for (const int role : {BoardCardPainter::CardCreatorAvatar, BoardCardPainter::CardAssigneeAvatar}) {
                const auto url = item->data(role).toUrl();
                if (!url.isEmpty() && !urls.contains(url))
                    urls.append(url);
            }
        }
    }
    return urls;
}

void BoardWidget::refreshAvatars()
{
    if (m_view == View::Kanban && m_scroll->isVisible()) {
        for (auto *column : m_container->findChildren<QListWidget *>())
            column->viewport()->update();
    }
}

bool BoardWidget::matches(const Task &task) const
{
    return m_alreadyFiltered || matchesTaskFilter(task, m_filter);
}

bool BoardWidget::matchesTaskFilter(const Task &task, const QString &query)
{
    const auto title = query.trimmed();
    if (title.isEmpty())
        return true;
    auto id = title;
    if (id.startsWith(QLatin1Char('#')) || id.startsWith(QChar(u'№')))
        id.remove(0, 1);
    id = id.trimmed();
    return task.name.contains(title, Qt::CaseInsensitive) || (!id.isEmpty() && task.id.contains(id));
}

QString BoardWidget::noMatchesText() const
{
    return m_alreadyFiltered ? QStringLiteral("Ничего не найдено")
                             : QStringLiteral("В загруженных задачах совпадений нет");
}

void BoardWidget::setFilter(const QString &text)
{
    if (m_filter == text)
        return;
    m_filter = text;
    setStages(m_stages);
}

void BoardWidget::setView(View view)
{
    if (m_view == view)
        return;
    m_view = view;
    setStages(m_stages, m_alreadyFiltered);
    emit viewChanged(view);
}

void BoardWidget::rebuildList()
{
    const int position = m_list->verticalScrollBar()->value();
    m_list->clear();
    for (const auto &stage : m_stages) {
        for (const auto &task : stage.tasks) {
            if (!matches(task))
                continue;
            auto *item = addListedTask(m_list, task, stage);
            if (!task.id.isEmpty() && task.id == m_selectedTaskId)
                m_list->setCurrentItem(item);
        }
    }
    if (m_list->count() == 0 && !hasMoreStagePages())
        workspace::emptyRow(m_list, m_filter.trimmed().isEmpty() ? QStringLiteral("Задач пока нет")
                                                                 : noMatchesText());
    m_list->verticalScrollBar()->setValue(position);
}

void BoardWidget::clearBoard(const QString &emptyText, bool loading)
{
    m_selectedTaskId.clear();
    m_stages.clear();
    resetStagePageLoads();
    m_alreadyFiltered = false;
    clearColumns();
    m_list->clear();
    m_scroll->hide();
    m_list->hide();
    m_empty->setText(emptyText);
    m_empty->show();
    ui::setLoadingIndicator(m_empty, loading);
}

} // namespace bx
