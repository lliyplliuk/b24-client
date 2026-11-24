#pragma once

class QObject;
class QSystemTrayIcon;

namespace bx {
struct Notification;

namespace desktop {

void showNotification(QSystemTrayIcon &tray, const Notification &notification, QObject *lifetime = nullptr);
void closeNotifications(QObject &lifetime);

} // namespace desktop
} // namespace bx
