#include "ui/board_column_grip.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

#include <utility>

namespace bx {

BoardColumnGrip::BoardColumnGrip(QWidget *column, const QString &title, std::function<void(int, bool)> resize)
    : QWidget(column), m_resize(std::move(resize))
{
    setObjectName(QStringLiteral("kanbanColumnGrip"));
    setCursor(Qt::SplitHCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(QStringLiteral("Ширина колонки %1").arg(title));
    setAccessibleDescription(QStringLiteral("Перетащите границу или используйте стрелки влево и вправо"));
    setToolTip(accessibleDescription());
    setMouseTracking(true);
    column->installEventFilter(this);
    positionGrip();
}

bool BoardColumnGrip::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize)
        positionGrip();
    return QWidget::eventFilter(watched, event);
}

bool BoardColumnGrip::event(QEvent *event)
{
    if (event->type() == QEvent::UngrabMouse || event->type() == QEvent::Hide)
        cancelDrag();
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave
        || event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)
        update();
    return QWidget::event(event);
}

void BoardColumnGrip::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const bool active = underMouse() || hasFocus() || m_dragging;
    painter.setPen(QPen(active ? QColor("#397b77") : QColor("#bdcecc"), active ? 2 : 1));
    painter.drawLine(rect().center().x(), 10, rect().center().x(), 34);
}

void BoardColumnGrip::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    m_dragging = true;
    m_startX = event->globalPosition().x();
    m_initialWidth = parentWidget()->width();
    update();
}

void BoardColumnGrip::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging && event->buttons().testFlag(Qt::LeftButton))
        m_resize(m_initialWidth + qRound(event->globalPosition().x() - m_startX), false);
    else
        cancelDrag();
}

void BoardColumnGrip::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        m_resize(parentWidget()->width(), true);
        update();
    }
}

void BoardColumnGrip::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
        const int direction = event->key() == Qt::Key_Left ? -1 : 1;
        m_resize(parentWidget()->width() + direction * 16, true);
        return;
    }
    QWidget::keyPressEvent(event);
}

void BoardColumnGrip::cancelDrag()
{
    if (m_dragging) {
        m_dragging = false;
        m_resize(m_initialWidth, false);
        update();
    }
}

void BoardColumnGrip::positionGrip()
{
    setGeometry(parentWidget()->width() - 10, 0, 9, parentWidget()->height());
}

} // namespace bx
