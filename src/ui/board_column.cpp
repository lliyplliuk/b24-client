#include "ui/board_column.h"
#include "ui/board_drag_list.h"
#include "core/models.h"
#include "ui/workspace_rows.h"

#include <QLabel>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QPainter>
#include <QVBoxLayout>

#include <cmath>

namespace bx {

namespace {
QString stageHeaderStyle(const QString &color)
{
    const QColor supplied(QLatin1Char('#') + color);
    const QColor background = supplied.isValid() ? supplied : QColor("#dce5e4");
    const auto linear = [](qreal channel) {
        return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
    };
    const qreal luminance = 0.2126 * linear(background.redF())
        + 0.7152 * linear(background.greenF()) + 0.0722 * linear(background.blueF());
    const bool light = luminance >= 0.179;
    const auto text = light ? QStringLiteral("#000000") : QStringLiteral("#ffffff");
    const auto outline = light ? QStringLiteral("rgba(0,0,0,75)") : QStringLiteral("rgba(255,255,255,120)");
    return QStringLiteral(
        "QWidget#kanbanStageHeader { background: %1; border-radius: 7px; }"
        "QLabel#columnTitle { color: %2; }"
        "QLabel#stageCount { color: %2; background: transparent; border: 1px solid %3; }")
        .arg(background.name(), text, outline);
}
} // namespace

BoardColumn::BoardColumn(BoardWidget *board, const Stage &stage, QWidget *parent)
    : QWidget(parent), m_list(new BoardDragList(board, stage.id, this))
{
    setAcceptDrops(true);
    setObjectName(QStringLiteral("column"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(7, 7, 13, 7);
    layout->setSpacing(6);

    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("kanbanStageHeader"));
    header->setStyleSheet(stageHeaderStyle(stage.color));
    auto *heading = new QHBoxLayout(header);
    heading->setContentsMargins(8, 5, 6, 5);
    heading->setSpacing(6);
    const auto title = stage.name.isEmpty() ? QStringLiteral("Без названия") : stage.name;
    auto *name = workspace::label(title, "columnTitle", header);
    heading->addWidget(name, 1);
    m_count = workspace::label(QString(), "stageCount", header);
    m_count->setMinimumHeight(21);
    m_count->setMinimumWidth(23);
    m_count->setContentsMargins(3, 0, 3, 0);
    m_count->setAlignment(Qt::AlignCenter);
    heading->addWidget(m_count, 0, Qt::AlignVCenter);
    layout->addWidget(header);

    m_list->setAccessibleName(title);
    m_list->setProperty("stageId", stage.id);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setMouseTracking(true);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(m_list, 1);
}

QString BoardColumn::title() const
{
    return m_list->accessibleName();
}

void BoardColumn::setTaskCount(qsizetype total)
{
    m_count->setText(QString::number(total));
    m_count->setAccessibleName(QStringLiteral("Задач в колонке: %1").arg(total));
}

void BoardColumn::dragEnterEvent(QDragEnterEvent *event)
{
    updateDrop(event);
}

void BoardColumn::dragMoveEvent(QDragMoveEvent *event)
{
    updateDrop(event);
}

void BoardColumn::dragLeaveEvent(QDragLeaveEvent *event)
{
    setDropHighlight(false);
    event->accept();
}

void BoardColumn::dropEvent(QDropEvent *event)
{
    setDropHighlight(false);
    if (auto *source = acceptedSource(event)) {
        source->setDropStage(m_list->stageId());
        event->setDropAction(Qt::MoveAction);
        event->accept();
    } else {
        event->ignore();
    }
}

void BoardColumn::paintEvent(QPaintEvent *event)
{
    QWidget::paintEvent(event);
    if (!m_dropHighlight)
        return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(workspace::Accent, 2));
    p.drawRoundedRect(rect().adjusted(1, 1, -2, -2), 10, 10);
}

BoardDragList *BoardColumn::acceptedSource(const QDropEvent *event) const
{
    auto *source = dynamic_cast<BoardDragList *>(event->source());
    return source && source->canDrop(event, m_list) ? source : nullptr;
}

void BoardColumn::updateDrop(QDropEvent *event)
{
    const bool allowed = acceptedSource(event);
    setDropHighlight(allowed);
    if (allowed) {
        event->setDropAction(Qt::MoveAction);
        event->accept();
    } else {
        event->ignore();
    }
}

void BoardColumn::setDropHighlight(bool highlighted)
{
    if (m_dropHighlight == highlighted)
        return;
    m_dropHighlight = highlighted;
    update();
}

} // namespace bx
