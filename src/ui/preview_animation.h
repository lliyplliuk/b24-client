#pragma once

#include <QObject>
#include <QPointer>

class QWidget;

namespace bx::ui {
class LoadingClock;
}

namespace bx::workspace {

class PreviewAnimation final : public QObject {
public:
    explicit PreviewAnimation(QWidget *viewport);
    ~PreviewAnimation() override;

    void setActive(bool active);
    int angle() const;

protected:
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    void updateClock();
    void stopClock();

    QWidget *m_viewport;
    QPointer<QWidget> m_window;
    QPointer<ui::LoadingClock> m_clock;
    bool m_active = false;
};

} // namespace bx::workspace
