#pragma once

#include <QtCore/Qt>

class QObject;
class QSystemTrayIcon;

namespace bx {
struct Notification;

namespace desktop {

inline bool shouldShowNotification(Qt::ApplicationState applicationState, bool loggedOut)
{
    return !loggedOut && applicationState != Qt::ApplicationActive;
}

void showNotification(QSystemTrayIcon &tray, const Notification &notification, QObject *lifetime = nullptr);
void closeNotifications(QObject &lifetime);

} // namespace desktop
} // namespace bx
