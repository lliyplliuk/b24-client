#include "ui/loading_indicator.h"

#include <QEvent>
#include <QStyle>
#include <utility>

namespace bx::ui {

void paintLoadingRing(QPainter &painter, const QRectF &bounds, const QColor &foreground, int angle)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(Qt::NoBrush);
    auto track = foreground;
    track.setAlpha(55);
    const auto ring = bounds.adjusted(1.4, 1.4, -1.4, -1.4);
    painter.setPen(QPen(track, 1.6));
    painter.drawEllipse(ring);
    painter.setPen(QPen(foreground, 1.6, Qt::SolidLine, Qt::RoundCap));
    painter.drawArc(ring, angle * -16, 110 * 16);
    painter.restore();
}

void setLoadingIndicator(QLabel *label, bool loading)
{
    auto *indicator = static_cast<LoadingIndicator *>(
        label->findChild<QWidget *>(QStringLiteral("sharedLoadingIndicator"), Qt::FindDirectChildrenOnly));
    if (!indicator && loading)
        indicator = new LoadingIndicator(label);
    if (indicator)
        indicator->setLoading(loading);
}

LoadingIndicator::LoadingIndicator(QLabel *label) : QWidget(label), m_label(label)
{
    setObjectName(QStringLiteral("sharedLoadingIndicator"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    setAccessibleName(QString());
    setFixedSize(14, 14);
    label->installEventFilter(this);
    hide();
}

LoadingIndicator::~LoadingIndicator()
{
    if (m_clock)
        m_clock->unwatch(this);
}

void LoadingIndicator::setLoading(bool loading)
{
    if (m_loading == loading) {
        if (loading)
            alignIndicator();
        return;
    }
    m_loading = loading;
    if (loading) {
        m_margins = m_label->contentsMargins();
        const auto gap = width() + 5;
        m_label->setContentsMargins(m_margins.left() + gap, m_margins.top(),
                                    m_margins.right(), m_margins.bottom());
        alignIndicator();
        show();
    } else {
        hide();
        m_label->setContentsMargins(m_margins);
    }
}

bool LoadingIndicator::eventFilter(QObject *object, QEvent *event)
{
    if (object == m_label && m_loading
        && (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest
            || event->type() == QEvent::FontChange))
        alignIndicator();
    if (object == m_window && event->type() == QEvent::WindowStateChange)
        updateClock();
    return QWidget::eventFilter(object, event);
}

void LoadingIndicator::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (m_window != window()) {
        if (m_window)
            m_window->removeEventFilter(this);
        m_window = window();
        m_window->installEventFilter(this);
    }
    alignIndicator();
    updateClock();
}

void LoadingIndicator::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    if (m_clock)
        m_clock->unwatch(this);
}

void LoadingIndicator::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    paintLoadingRing(painter, rect(), palette().color(QPalette::WindowText), m_clock ? m_clock->angle() : 0);
}

void LoadingIndicator::alignIndicator()
{
    const int textWidth = m_label->fontMetrics().horizontalAdvance(m_label->text());
    const int left = m_margins.left() + m_label->margin();
    const int right = m_margins.right() + m_label->margin();
    const int gap = width() + 5;
    const int remaining = qMax(0, m_label->width() - left - right - textWidth - gap);
    const int offset = m_label->alignment().testFlag(Qt::AlignHCenter) ? remaining / 2
        : m_label->alignment().testFlag(Qt::AlignRight) ? remaining : 0;
    move(left + offset, qMax(0, (m_label->height() - height()) / 2));
}

void LoadingIndicator::updateClock()
{
    if (m_loading && isVisible() && !window()->isMinimized()
        && style()->styleHint(QStyle::SH_Widget_Animation_Duration, nullptr, this) > 0) {
        m_clock = LoadingClock::shared();
        m_clock->watch(this);
    } else if (m_clock) {
        m_clock->unwatch(this);
        m_clock.clear();
    }
}

} // namespace bx::ui
