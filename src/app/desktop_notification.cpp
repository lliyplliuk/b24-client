#include "app/desktop_notification.h"

#include "core/models.h"
#include "ui/message_markup.h"

#include <QPointer>
#include <QSystemTrayIcon>

#ifdef Q_OS_LINUX
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QHash>
#include <QQueue>
#include <memory>
#endif

namespace bx::desktop {

#ifdef Q_OS_LINUX
namespace {

const auto notificationService = QStringLiteral("org.freedesktop.Notifications");
const auto notificationPath = QStringLiteral("/org/freedesktop/Notifications");

struct ShownPopup {
    QString server;
    uint id;
};

void closePopup(const ShownPopup &popup)
{
    if (!popup.server.startsWith(QLatin1Char(':')) || popup.id == 0)
        return;
    auto request = QDBusMessage::createMethodCall(popup.server, notificationPath,
        notificationService, QStringLiteral("CloseNotification"));
    request.setArguments({popup.id});
    QDBusConnection::sessionBus().send(request);
}

struct PopupScope {
    bool active = true;
    QQueue<ShownPopup> popups;
};

QHash<QObject *, std::shared_ptr<PopupScope>> popupScopes;

std::shared_ptr<PopupScope> notificationScope(QObject &owner)
{
    auto &scope = popupScopes[&owner];
    if (!scope) {
        scope = std::make_shared<PopupScope>();
        QObject::connect(&owner, &QObject::destroyed, [](QObject *destroyed) { closeNotifications(*destroyed); });
    }
    return scope;
}

} // namespace
#endif

void closeNotifications(QObject &lifetime)
{
#ifdef Q_OS_LINUX
    const auto scope = popupScopes.take(&lifetime);
    if (scope) {
        scope->active = false;
        for (const auto &popup : scope->popups)
            closePopup(popup);
        scope->popups.clear();
    }
#else
    Q_UNUSED(lifetime);
#endif
}

void showNotification(QSystemTrayIcon &tray, const Notification &notification, QObject *lifetime)
{
    const auto title = workspace::markupPlainText(notification.title.left(256)).simplified().left(96);
    const auto text = workspace::markupPlainText(notification.text.left(2000)).simplified().left(320);
    const auto showTrayMessage = [safeTray = QPointer<QSystemTrayIcon>(&tray)] {
        if (safeTray && safeTray->isVisible() && QSystemTrayIcon::isSystemTrayAvailable()
            && QSystemTrayIcon::supportsMessages()) {
            safeTray->showMessage(QStringLiteral("Битрикс24"), QStringLiteral("Новое уведомление"),
                                  QSystemTrayIcon::Information, 6000);
        }
    };

#ifdef Q_OS_LINUX
    const auto bus = QDBusConnection::sessionBus();
    if (bus.isConnected()) {
        const auto scope = notificationScope(lifetime ? *lifetime : tray);
        auto request = QDBusMessage::createMethodCall(notificationService, notificationPath, notificationService,
            QStringLiteral("Notify"));
        const QVariantMap hints{{QStringLiteral("desktop-entry"), QStringLiteral("bitrix24-client")}};
        request.setArguments({QStringLiteral("Битрикс24"), uint(0), QStringLiteral("bitrix24-client"),
                              title, text.toHtmlEscaped(), QStringList{}, hints, 6000});
        auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(request, 2500), QCoreApplication::instance());
        QObject::connect(watcher, &QDBusPendingCallWatcher::finished, watcher, [watcher, scope, showTrayMessage] {
            const auto type = watcher->error().type();
            if (type == QDBusError::NoError) {
                const QDBusPendingReply<uint> reply = *watcher;
                if (reply.isValid()) {
                    ShownPopup popup{watcher->reply().service(), reply.value()};
                    if (scope->active) {
                        if (scope->popups.size() >= 32)
                            closePopup(scope->popups.dequeue());
                        scope->popups.enqueue(std::move(popup));
                    } else {
                        closePopup(popup);
                    }
                }
            }
            // A timeout can mean the notification is already visible. Only fall back when it had no recipient.
            if (scope->active && (type == QDBusError::ServiceUnknown || type == QDBusError::NoServer || type == QDBusError::Disconnected))
                showTrayMessage();
            watcher->deleteLater();
        });
        return;
    }
#else
    Q_UNUSED(title);
    Q_UNUSED(text);
    Q_UNUSED(lifetime);
#endif
    showTrayMessage();
}

} // namespace bx::desktop
