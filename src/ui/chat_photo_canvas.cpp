#include "ui/chat_photo_canvas.h"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

namespace bx::ui {

ChatPhotoCanvas::ChatPhotoCanvas(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("chatPhotoCanvas"));
    setAccessibleName(QStringLiteral("Фотография"));
    setMinimumHeight(180);
}

void ChatPhotoCanvas::setOriginal(QImage image)
{
    m_photo = std::move(image);
    m_zoom = 1.0;
    m_pan = {};
    m_dragging = false;
    unsetCursor();
    update();
}

void ChatPhotoCanvas::setZoomChanged(std::function<void()> handler)
{
    m_zoomChanged = std::move(handler);
}

void ChatPhotoCanvas::zoomIn()
{
    changeZoom(m_zoom * 1.25, rect().center());
}

void ChatPhotoCanvas::zoomOut()
{
    changeZoom(m_zoom / 1.25, rect().center());
}

void ChatPhotoCanvas::fit()
{
    changeZoom(1.0, rect().center());
}

void ChatPhotoCanvas::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor("#142329"));
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QRectF photo(QPointF{}, photoSize());
    photo.moveCenter(QRectF(rect()).center() + m_pan);
    painter.drawImage(photo, m_photo, QRectF(m_photo.rect()));
}

void ChatPhotoCanvas::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    clampPan();
    if (m_zoomChanged)
        m_zoomChanged();
}

void ChatPhotoCanvas::wheelEvent(QWheelEvent *event)
{
    const auto delta = !event->angleDelta().isNull() ? event->angleDelta().y() : event->pixelDelta().y();
    if (delta != 0) {
        changeZoom(delta > 0 ? m_zoom * 1.25 : m_zoom / 1.25, event->position());
        event->accept();
    } else {
        QWidget::wheelEvent(event);
    }
}

void ChatPhotoCanvas::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && canZoomOut()) {
        m_dragging = true;
        m_dragPoint = event->position();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
    } else {
        QWidget::mousePressEvent(event);
    }
}

void ChatPhotoCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    m_pan += event->position() - m_dragPoint;
    m_dragPoint = event->position();
    clampPan();
    update();
}

void ChatPhotoCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_dragging && event->button() == Qt::LeftButton) {
        m_dragging = false;
        setCursor(canZoomOut() ? Qt::OpenHandCursor : Qt::ArrowCursor);
        event->accept();
    } else {
        QWidget::mouseReleaseEvent(event);
    }
}

QSize ChatPhotoCanvas::fittedSize() const
{
    return m_photo.size().scaled(size(), Qt::KeepAspectRatio);
}

qreal ChatPhotoCanvas::maximumZoom() const
{
    const auto fitted = fittedSize();
    const qreal nativeWidth = qreal(m_photo.width()) / qMax(1, fitted.width());
    const qreal nativeHeight = qreal(m_photo.height()) / qMax(1, fitted.height());
    return qMax(5.0, qMax(nativeWidth, nativeHeight));
}

QSizeF ChatPhotoCanvas::photoSize() const
{
    return QSizeF(fittedSize()) * m_zoom;
}

void ChatPhotoCanvas::clampPan()
{
    const auto actual = photoSize();
    const auto horizontal = qMax(0.0, (actual.width() - width()) / 2);
    const auto vertical = qMax(0.0, (actual.height() - height()) / 2);
    m_pan.setX(qBound(-horizontal, m_pan.x(), horizontal));
    m_pan.setY(qBound(-vertical, m_pan.y(), vertical));
}

void ChatPhotoCanvas::changeZoom(qreal level, const QPointF &anchor)
{
    const auto previous = m_zoom;
    m_zoom = qBound(1.0, level, maximumZoom());
    if (m_zoom == previous)
        return;
    m_pan += (anchor - QRectF(rect()).center() - m_pan) * (1.0 - m_zoom / previous);
    clampPan();
    setCursor(canZoomOut() ? Qt::OpenHandCursor : Qt::ArrowCursor);
    if (m_zoomChanged)
        m_zoomChanged();
    update();
}

} // namespace bx::ui
