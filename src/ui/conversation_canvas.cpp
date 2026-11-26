#include "ui/conversation_canvas.h"

#include <QPainter>

namespace bx::ui {

ConversationCanvas::ConversationCanvas(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    m_tile = QPixmap(32, 32);
    m_tile.fill(Qt::transparent);
    QPainter painter(&m_tile);
    painter.setPen(QColor("#d8e6de"));
    painter.drawPoint(4, 4);
    painter.drawPoint(20, 20);
    painter.setPen(QColor("#dbe8e0"));
    painter.drawLine(19, 20, 21, 20);
    painter.drawLine(20, 19, 20, 21);
}

void ConversationCanvas::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor("#e5efe9"));
    painter.drawTiledPixmap(rect(), m_tile);
}

} // namespace bx::ui
