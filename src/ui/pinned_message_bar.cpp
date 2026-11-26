#include "ui/pinned_message_bar.h"

#include "ui/message_markup.h"
#include "ui/typography.h"
#include <QHBoxLayout>
#include <QMenu>
#include <QPushButton>

namespace bx::ui {

PinnedMessageBar::PinnedMessageBar(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("pinnedMessageBar"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setStyleSheet(QStringLiteral(R"(
            QWidget#pinnedMessageBar QPushButton {
                text-align: left; padding: 5px 10px; margin-bottom: 2px;
                color: #215c70; background: #eaf3ef; border: 1px solid #becec7; border-radius: 4px;
            }
            QWidget#pinnedMessageBar QPushButton#choosePinnedMessage { text-align: center; padding: 5px 0; }
            QWidget#pinnedMessageBar QPushButton:hover { background: #dfede7; }
            QWidget#pinnedMessageBar QPushButton:focus { border-color: #215c70; }
        )"));
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    m_body = new QPushButton(this);
    m_body->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_body->setAccessibleName(QStringLiteral("Закреплённые сообщения"));
    m_body->setAutoDefault(false);
    m_body->setCursor(Qt::PointingHandCursor);
    layout->addWidget(m_body, 1);
    m_selector = new QPushButton(QStringLiteral("▾"), this);
    m_selector->setObjectName(QStringLiteral("choosePinnedMessage"));
    m_selector->setAccessibleName(QStringLiteral("Выбрать закреплённое сообщение"));
    m_selector->setToolTip(m_selector->accessibleName());
    m_selector->setAutoDefault(false);
    m_selector->setCursor(Qt::PointingHandCursor);
    m_selector->setFixedWidth(32);
    m_selector->hide();
    layout->addWidget(m_selector);
    connect(m_body, &QPushButton::clicked, this, [this] {
        if (m_onOpen && !m_messages.isEmpty())
            m_onOpen(QString(m_messages.front().id));
    });
    connect(m_selector, &QPushButton::clicked, this, [this] { choosePinned(); });
    hide();
}

void PinnedMessageBar::setMessages(const QVector<Message> &messages)
{
    if (m_menu)
        m_menu->close();
    m_messages.clear();
    for (const auto &message : messages)
        if (!message.id.isEmpty())
            m_messages.append(message);
    m_selector->setVisible(m_messages.size() > 1);
    setVisible(!m_messages.isEmpty());
    refreshText();
}

void PinnedMessageBar::setOnOpen(std::function<void(const QString &)> handler)
{
    m_onOpen = std::move(handler);
}

void PinnedMessageBar::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    refreshText();
}

QString PinnedMessageBar::preview(const Message &message)
{
    const auto text = workspace::markupPlainText(message.text).simplified();
    return text.isEmpty() ? QStringLiteral("Сообщение №%1").arg(message.id) : text.left(180);
}

void PinnedMessageBar::refreshText()
{
    if (m_messages.isEmpty())
        return;
    const auto title = m_messages.size() == 1
        ? QStringLiteral("Закреплено · %1").arg(preview(m_messages.front()))
        : QStringLiteral("Закреплённые сообщения · %1 · %2").arg(m_messages.size()).arg(preview(m_messages.front()));
    const int reserve = m_selector->isHidden() ? 0 : m_selector->width() + 2;
    m_body->setText(m_body->fontMetrics().elidedText(title, Qt::ElideRight, qMax(1, width() - reserve - 26)));
    m_body->setToolTip(plainTooltip(title));
    m_body->setAccessibleDescription(preview(m_messages.front()));
}

void PinnedMessageBar::choosePinned()
{
    if (!m_onOpen || m_messages.size() < 2 || m_menu)
        return;
    auto *menu = new QMenu(this);
    m_menu = menu;
    connect(menu, &QMenu::aboutToHide, this, [this, menu] {
        m_menu.clear();
        menu->deleteLater();
    });
    for (const auto &message : m_messages) {
        auto *action = menu->addAction(QStringLiteral("%1 · №%2").arg(preview(message), message.id));
        connect(action, &QAction::triggered, this, [this, id = message.id] {
            if (m_onOpen)
                m_onOpen(id);
        });
    }
    menu->popup(m_selector->mapToGlobal(m_selector->rect().bottomLeft()));
}

} // namespace bx::ui
