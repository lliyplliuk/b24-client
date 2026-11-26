#pragma once

#include <QListWidget>
#include <functional>

namespace bx::ui {

class TaskContentList final : public QListWidget {
public:
    TaskContentList(const QString &name, QWidget *parent);

    void requestHeight();
    void setOnLayout(std::function<void()> handler);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    bool m_queued = false;
    std::function<void()> m_onLayout;
};

} // namespace bx::ui
