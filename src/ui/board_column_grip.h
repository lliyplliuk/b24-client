#pragma once

#include <QWidget>

#include <functional>

namespace bx {

class BoardColumnGrip final : public QWidget {
public:
    BoardColumnGrip(QWidget *column, const QString &title, std::function<void(int, bool)> resize);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void cancelDrag();
    void positionGrip();

    std::function<void(int, bool)> m_resize;
    qreal m_startX = 0;
    int m_initialWidth = 0;
    bool m_dragging = false;
};

} // namespace bx
