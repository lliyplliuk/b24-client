#pragma once

#include <QWidget>
#include <QPixmap>

namespace bx::ui {

class ConversationCanvas final : public QWidget {
public:
    explicit ConversationCanvas(QWidget *parent);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QPixmap m_tile;
};

} // namespace bx::ui
