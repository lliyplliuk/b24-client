#include "ui/message_menu_window.h"

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QScreen>
#include <QTimer>

namespace bx::ui {
namespace {

class MessageMenuWindow final : public QObject {
public:
    explicit MessageMenuWindow(QMenu &menu) : QObject(&menu), m_root(menu)
    {
        prepare(menu);
        qApp->installEventFilter(this);
        m_deactivation.setSingleShot(true);
        m_deactivation.setInterval(25);
        connect(&m_deactivation, &QTimer::timeout, this, [this] {
            if (QGuiApplication::applicationState() != Qt::ApplicationActive)
                m_root.close();
        });
        connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive)
                m_deactivation.stop();
            // A menu-to-menu focus transfer can briefly deactivate Qt on X11 and offscreen.
            else
                m_deactivation.start();
        });
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (!m_root.isVisible())
            return false;
        if (event->type() == QEvent::Hide && watched == m_root.parentWidget()) {
            m_root.close();
            return false;
        }
        const bool press = event->type() == QEvent::MouseButtonPress;
        const bool move = event->type() == QEvent::MouseMove;
        const auto *widget = qobject_cast<QWidget *>(watched);
        const bool leave = event->type() == QEvent::Leave && widget && m_menus.contains(qobject_cast<QMenu *>(watched));
        if (!press && !move && !leave && event->type() != QEvent::MouseButtonRelease)
            return false;
        const auto point = leave ? QCursor::pos() : static_cast<QMouseEvent *>(event)->globalPosition().toPoint();
        bool insideTree = false;
        for (const auto &menu : m_menus) {
            if (!menu || !menu->isVisible())
                continue;
            const bool inside = menu->rect().contains(menu->mapFromGlobal(point));
            insideTree |= inside;
            if (!leave && widget && widget->window() == menu && !inside) {
                menu->setActiveAction(nullptr);
                return true;
            }
        }
        if (press && !insideTree)
            m_root.close();
        if ((move || leave) && !insideTree) {
            for (const auto &menu : m_menus)
                if (menu && menu->isVisible())
                    menu->setActiveAction(nullptr);
        }
        return false;
    }

private:
    void prepare(QMenu &menu)
    {
        // Tool windows do not acquire Qt's desktop popup input grab.
        menu.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
        m_menus.append(&menu);
        connect(&menu, &QMenu::triggered, this, [this](const QAction *action) {
            if (action && !action->menu())
                m_root.close();
        });
        for (auto *action : menu.actions())
            if (auto *submenu = action->menu())
                prepare(*submenu);
    }

    QMenu &m_root;
    QList<QPointer<QMenu>> m_menus;
    QTimer m_deactivation;
};

} // namespace

void showMessageMenuWindow(QMenu &menu, const QRect &globalAnchor)
{
    // Wayland positions popup surfaces relative to their parent window.
    const bool wayland = QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
    if (!wayland)
        new MessageMenuWindow(menu);
    auto *screen = QGuiApplication::screenAt(globalAnchor.center());
    if (!screen)
        screen = menu.screen();
    const auto available = screen->availableGeometry();
    const auto size = menu.sizeHint();
    constexpr int gap = 4;
    const int below = globalAnchor.bottom() + 1 + gap;
    const int above = globalAnchor.top() - size.height() - gap;
    const int y = below + size.height() <= available.bottom() + 1 ? below : above;
    const int right = qMax(available.left(), available.right() - size.width() + 1);
    const int bottom = qMax(available.top(), available.bottom() - size.height() + 1);
    menu.popup({qBound(available.left(), globalAnchor.left(), right), qBound(available.top(), y, bottom)});
    if (wayland)
        return;
    if (!menu.rect().contains(menu.mapFromGlobal(QCursor::pos())))
        menu.setActiveAction(nullptr);
    menu.activateWindow();
    if (!menu.focusWidget())
        menu.setFocus(Qt::PopupFocusReason);
}

} // namespace bx::ui
