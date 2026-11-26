#pragma once

#include "core/models.h"
#include <QPointer>
#include <QWidget>
#include <functional>

class QMenu;
class QPushButton;

namespace bx::ui {

class PinnedMessageBar final : public QWidget {
public:
    explicit PinnedMessageBar(QWidget *parent);
    void setMessages(const QVector<Message> &messages);
    void setOnOpen(std::function<void(const QString &)> handler);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    static QString preview(const Message &message);
    void refreshText();
    void choosePinned();

    QPushButton *m_body;
    QPushButton *m_selector;
    QVector<Message> m_messages;
    QPointer<QMenu> m_menu;
    std::function<void(const QString &)> m_onOpen;
};

} // namespace bx::ui
