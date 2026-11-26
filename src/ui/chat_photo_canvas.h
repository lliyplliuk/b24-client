#pragma once

#include <QWidget>
#include <QImage>
#include <functional>

namespace bx::ui {

class ChatPhotoCanvas final : public QWidget {
public:
    explicit ChatPhotoCanvas(QWidget *parent);
    void setOriginal(QImage image);
    QSize imageSize() const { return m_photo.size(); }
    const QImage &image() const { return m_photo; }
    qreal zoomFactor() const { return m_zoom; }
    bool canZoomOut() const { return m_zoom > 1.0; }
    bool canZoomIn() const { return m_zoom < maximumZoom(); }
    void setZoomChanged(std::function<void()> handler);
    void zoomIn();
    void zoomOut();
    void fit();

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    QSize fittedSize() const;
    qreal maximumZoom() const;
    QSizeF photoSize() const;
    void clampPan();
    void changeZoom(qreal level, const QPointF &anchor);
    QImage m_photo;
    qreal m_zoom = 1.0;
    QPointF m_pan;
    QPointF m_dragPoint;
    bool m_dragging = false;
    std::function<void()> m_zoomChanged;
};

} // namespace bx::ui
