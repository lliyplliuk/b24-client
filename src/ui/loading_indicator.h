#pragma once

#include "ui/loading_clock.h"
#include <QLabel>
#include <QPointer>
#include <QPainter>

namespace bx::ui {

void paintLoadingRing(QPainter &painter, const QRectF &bounds, const QColor &foreground, int angle);

void setLoadingIndicator(QLabel *label, bool loading);

class LoadingIndicator final : public QWidget {
public:
    explicit LoadingIndicator(QLabel *label);
    ~LoadingIndicator() override;
    void setLoading(bool loading);

protected:
    bool eventFilter(QObject *object, QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void paintEvent(QPaintEvent *) override;

private:
    void alignIndicator();
    void updateClock();
    QLabel *const m_label;
    QPointer<QWidget> m_window;
    QPointer<LoadingClock> m_clock;
    QMargins m_margins;
    bool m_loading = false;
};

} // namespace bx::ui
