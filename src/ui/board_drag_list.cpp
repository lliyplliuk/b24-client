#include "ui/board_drag_list.h"
#include "ui/board_widget.h"
#include "ui/workspace_row_style.h"

#include <QGuiApplication>
#include <QCursor>
#include <QDrag>
#include <QDropEvent>
#include <QMimeData>
#include <QPointer>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>

namespace bx {

namespace {
constexpr auto CardDragType = "application/x-bitrix24-client-kanban-task";
} // namespace

BoardDragList::BoardDragList(BoardWidget *board, const QString &stageId, QWidget *parent)
    : QListWidget(parent), m_board(board), m_stageId(stageId)
{
    setDragDropMode(QAbstractItemView::DragOnly);
}

bool BoardDragList::retireIfDragging()
{
    if (m_activeTask.isEmpty())
        return false;
    m_retired = true;
    return true;
}

bool BoardDragList::canDrop(const QDropEvent *event, const BoardDragList *target) const
{
    return !m_retired && m_board == target->m_board && target != this
        && !m_activeTask.isEmpty() && !target->m_stageId.isEmpty()
        && (event->possibleActions() & Qt::MoveAction)
        && event->mimeData()->data(CardDragType) == m_activeTask.toUtf8()
        && m_board->m_taskMoveGuard && m_board->m_taskMoveGuard(m_activeTask, m_stageId, target->m_stageId);
}

void BoardDragList::setDropStage(const QString &stageId)
{
    m_dropStage = stageId;
}

void BoardDragList::startDrag(Qt::DropActions)
{
    const auto *task = currentItem();
    if (QGuiApplication::mouseButtons() != Qt::LeftButton || !task || !m_board->m_taskMoveGuard)
        return;
    const auto taskId = task->data(workspace::Id).toString();
    if (taskId.isEmpty() || !m_board->m_taskMoveGuard(taskId, m_stageId, {}))
        return;

    auto *mime = new QMimeData;
    mime->setData(CardDragType, taskId.toUtf8());
    auto *drag = new QDrag(this);
    drag->setMimeData(mime);
    const auto card = visualItemRect(task).intersected(viewport()->visibleRegion().boundingRect());
    if (!card.isEmpty()) {
        drag->setPixmap(viewport()->grab(card));
        const auto cursor = viewport()->mapFromGlobal(QCursor::pos());
        drag->setHotSpot(card.contains(cursor) ? cursor - card.topLeft() : card.center() - card.topLeft());
    }

    // Scroll across wide boards only while the pointer is at a visible edge during a drag.
    QTimer edgeScroll;
    edgeScroll.setInterval(55);
    connect(&edgeScroll, &QTimer::timeout, this, [this] {
        const auto *viewport = m_board->m_scroll->viewport();
        const auto point = viewport->mapFromGlobal(QCursor::pos());
        if (m_retired || !viewport->isVisible() || !viewport->rect().contains(point))
            return;
        constexpr int edge = 32;
        constexpr int step = 16;
        auto *scroll = m_board->m_scroll->horizontalScrollBar();
        if (point.x() < edge)
            scroll->setValue(scroll->value() - step);
        else if (point.x() >= viewport->width() - edge)
            scroll->setValue(scroll->value() + step);
    });
    edgeScroll.start();
    const auto revision = m_board->m_columnRevision;
    const QPointer<BoardDragList> source(this);
    m_activeTask = taskId;
    m_dropStage.clear();
    const auto action = drag->exec(Qt::MoveAction);
    if (!source)
        return;
    m_activeTask.clear();
    if (m_retired) {
        parentWidget()->deleteLater();
        return;
    }
    if (action != Qt::MoveAction || m_dropStage.isEmpty())
        return;

    // The server can synchronously rebuild columns. Finish Qt's source drag event first.
    QMetaObject::invokeMethod(m_board, [board = m_board, revision, taskId, from = m_stageId, to = m_dropStage] {
        if (revision == board->m_columnRevision && board->m_taskMoveGuard && board->m_taskMoveGuard(taskId, from, to))
            emit board->taskMoveRequested(taskId, from, to);
    }, Qt::QueuedConnection);
}

bool BoardDragList::viewportEvent(QEvent *event)
{
    if (event->type() == QEvent::Resize) {
        const auto *resize = static_cast<QResizeEvent *>(event);
        if (resize->size().width() != resize->oldSize().width())
            scheduleDelayedItemsLayout();
    }
    return QListWidget::viewportEvent(event);
}

} // namespace bx
