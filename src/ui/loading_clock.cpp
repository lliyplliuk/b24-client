#include "ui/loading_clock.h"

#include <QApplication>
#include <QPointer>

namespace bx::ui {

LoadingClock *LoadingClock::shared()
{
    static QPointer<LoadingClock> clock;
    if (!clock)
        clock = new LoadingClock;
    return clock;
}

void LoadingClock::watch(QWidget *indicator)
{
    m_visible.insert(indicator);
    if (!m_timer.isActive())
        m_timer.start();
}

void LoadingClock::unwatch(QWidget *indicator)
{
    m_visible.remove(indicator);
    if (m_visible.isEmpty())
        m_timer.stop();
}

LoadingClock::LoadingClock() : QObject(qApp)
{
    m_timer.setTimerType(Qt::CoarseTimer);
    m_timer.setInterval(80);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        m_angle = (m_angle + 30) % 360;
        for (auto *indicator : std::as_const(m_visible))
            indicator->update();
    });
}

} // namespace bx::ui
