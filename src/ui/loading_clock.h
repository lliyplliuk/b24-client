#pragma once

#include <QWidget>
#include <QSet>
#include <QTimer>

namespace bx::ui {

class LoadingClock final : public QObject {
public:
    static LoadingClock *shared();
    void watch(QWidget *indicator);
    void unwatch(QWidget *indicator);
    int angle() const { return m_angle; }
    bool isRunning() const { return m_timer.isActive(); }

private:
    LoadingClock();
    QSet<QWidget *> m_visible;
    QTimer m_timer;
    int m_angle = 0;
};

} // namespace bx::ui
