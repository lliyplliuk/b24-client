#include "ui/preview_animation.h"

#include "ui/loading_indicator.h"

namespace bx::workspace {

PreviewAnimation::PreviewAnimation(QWidget *viewport) : m_viewport(viewport)
{
    viewport->installEventFilter(this);
    connect(viewport, &QObject::destroyed, this, [this] {
        stopClock();
        m_viewport = nullptr;
    });
}

PreviewAnimation::~PreviewAnimation()
{
    stopClock();
}

void PreviewAnimation::setActive(bool active)
{
    if (m_viewport && (m_active || active)) {
        m_active = active;
        updateClock();
        m_viewport->update();
    }
}

int PreviewAnimation::angle() const
{
    return m_clock ? m_clock->angle() : 0;
}

bool PreviewAnimation::eventFilter(QObject *object, QEvent *event)
{
    if (object == m_window && event->type() == QEvent::WindowStateChange)
        updateClock();
    if (object == m_viewport && (event->type() == QEvent::Show || event->type() == QEvent::Hide
                                 || event->type() == QEvent::StyleChange))
        updateClock();
    return false;
}

void PreviewAnimation::updateClock()
{
    if (!m_viewport)
        return;
    auto *window = m_viewport->window();
    if (m_active && m_window != window) {
        if (m_window)
            m_window->removeEventFilter(this);
        m_window = window;
        window->installEventFilter(this);
    }
    if (m_active && m_viewport->isVisible() && !window->isMinimized()
        && m_viewport->style()->styleHint(QStyle::SH_Widget_Animation_Duration, nullptr, m_viewport) > 0) {
        if (!m_clock) {
            m_clock = ui::LoadingClock::shared();
            m_clock->watch(m_viewport);
        }
    } else {
        stopClock();
    }
}

void PreviewAnimation::stopClock()
{
    if (m_clock) {
        m_clock->unwatch(m_viewport);
        m_clock.clear();
    }
}

} // namespace bx::workspace
