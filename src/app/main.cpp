#include "app/desktop_notification.h"
#include "app/silent_session_restore.h"
#include "chromesignin/chrome_signin.h"
#include "cloud/cloud_backend.h"
#include "core/demo_backend.h"
#include "core/origin_guard.h"
#include "signin/session_restore.h"
#include "signin/session_store.h"
#include "ui/connection_dialog.h"
#include "ui/dialog_chrome.h"
#include "ui/main_window.h"
#include "ui/typography.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QIcon>
#include <QJsonDocument>
#include <QLibraryInfo>
#include <QMenu>
#include <QPointer>
#include <QSettings>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QTranslator>
#include <QWindow>
#include <algorithm>
#include <cstdio>
#include <optional>

using namespace bx;

namespace {

constexpr qsizetype sessionFrameLimit = 64 * 1024;
const auto lastPortalKey = QLatin1String(signin::lastPortalPreferenceKey);

QUrl asOrigin(QUrl url) {
    url.setPath(QString());
    url.setQuery(QString());
    url.setFragment(QString());
    if (url.scheme() == QStringLiteral("https") && url.port() == 443)
        url.setPort(-1);
    return url;
}

bool safeHeader(const QByteArray &value) {
    return !value.isEmpty() && value.size() < 16384
        && std::all_of(value.cbegin(), value.cend(), [](char character) {
            const auto byte = static_cast<unsigned char>(character);
            return byte >= 32 && byte != 127;
        });
}

bool validSession(const Session &session, bool allowLoopback) {
    const auto &url = session.origin;
    const bool allowed = url.scheme() == QStringLiteral("https")
                         || (allowLoopback && url.scheme() == QStringLiteral("http")
                             && OriginGuard::isLoopback(url));
    return session.isValid() && allowed && !url.authority().contains(QLatin1Char('@'))
        && safeHeader(session.cookie) && safeHeader(session.csrf);
}

Session parseSessionFrame(const QByteArray &line) {
    if (line.size() >= sessionFrameLimit)
        return {};
    const auto values = QJsonDocument::fromJson(line).object();
    const auto origin = values.value(QStringLiteral("origin"));
    const auto cookie = values.value(QStringLiteral("cookie"));
    const auto csrf = values.value(QStringLiteral("csrf"));
    if (!origin.isString() || !cookie.isString() || !csrf.isString())
        return {};

    const QUrl url(origin.toString(), QUrl::StrictMode);
    return {asOrigin(url), cookie.toString().toUtf8(), csrf.toString().toUtf8()};
}

Session sessionFromPipe() {
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly, QFileDevice::DontCloseHandle))
        return {};
    return parseSessionFrame(input.readLine(sessionFrameLimit + 1));
}

Session sessionFromBrowser(const QString &enteredPortal, bool portalGiven, const QString &feedback = {},
                           bool recoverCurrent = false) {
    using signin::RestoreStatus;
    QSettings settings(QStringLiteral("bitrix24-client"), QStringLiteral("bitrix24-client"));
    const auto requested = ConnectionDialog::normalizePortal(
        portalGiven ? enteredPortal : settings.value(lastPortalKey).toString());
    ConnectionDialog dialog(requested);
    if (!feedback.isEmpty())
        dialog.setError(feedback);
    ChromeSignin signIn;
    signin::SessionStore store;
    signin::SessionRestore restore;
    SilentSessionRestore startup;
    Session session;
    Session restoring;
    Session pendingSave;
    QUrl attemptedRestore;
    QUrl activeBrowser;
    bool openOnFallback = false;

    const auto accept = [&](const Session &candidate) {
        session = candidate;
        if (!recoverCurrent)
            settings.setValue(lastPortalKey, candidate.origin.toString(QUrl::FullyEncoded));
        dialog.accept();
        startup.finish();
    };
    const auto launchBrowser = [&](const QUrl &portal) {
        restore.cancel();
        store.cancel();
        pendingSave = {};
        activeBrowser = portal;
        dialog.setWaiting(recoverCurrent
            ? QStringLiteral("Войдите под тем же аккаунтом на странице Битрикс24. Рабочее окно обновится автоматически.")
            : QStringLiteral("Завершите вход на странице Битрикс24. Клиент откроется автоматически."));
        signIn.start(portal);
    };
    const auto handleBlockedRestore = [&] {
        if (!signin::isSessionRestoreBlocked(settings, attemptedRestore))
            return false;
        if (openOnFallback)
            launchBrowser(attemptedRestore);
        else {
            dialog.setError(QStringLiteral("Чтобы открыть этот портал снова, войдите через браузер."));
            startup.finish();
        }
        return true;
    };
    const auto startRestore = [&](const QUrl &portal, bool openBrowserIfNeeded) {
        activeBrowser.clear();
        signIn.cancel();
        attemptedRestore = portal;
        openOnFallback = openBrowserIfNeeded;
        if (handleBlockedRestore())
            return;
        dialog.setCheckingSavedSession();
        store.load(portal);
    };
    const auto cancel = [&] {
        activeBrowser.clear();
        attemptedRestore.clear();
        pendingSave = {};
        signIn.cancel();
        store.cancel();
        restore.cancel();
    };
    QObject::connect(&dialog, &ConnectionDialog::connectRequested, &signIn, [&](const QUrl &portal) {
        if (recoverCurrent) {
            if (portal == requested)
                launchBrowser(portal);
            else
                dialog.setError(QStringLiteral("Введите адрес текущего портала, чтобы восстановить этот вход."));
        } else if (attemptedRestore != portal)
            startRestore(portal, true);
        else
            launchBrowser(portal);
    });
    QObject::connect(&dialog, &ConnectionDialog::addressEdited, &signIn, cancel);
    QObject::connect(&dialog, &QDialog::rejected, &signIn, cancel);
    QObject::connect(&store, &signin::SessionStore::loaded, &dialog, [&](const QByteArray &payload) {
        if (attemptedRestore.isEmpty() || attemptedRestore != dialog.portal() || handleBlockedRestore())
            return;
        const auto candidate = signin::sessionFromStoredPayload(payload, attemptedRestore);
        if (candidate.isValid()) {
            restoring = candidate;
            restore.verify(candidate);
        } else if (openOnFallback) {
            launchBrowser(attemptedRestore);
        } else {
            dialog.resetFeedback();
            startup.finish();
        }
    });
    QObject::connect(&restore, &signin::SessionRestore::verified, &dialog,
                     [&](RestoreStatus status, const Session &candidate) {
        if (attemptedRestore.isEmpty() || attemptedRestore != dialog.portal() || handleBlockedRestore())
            return;
        if (status == RestoreStatus::Authenticated) {
            if (candidate.cookie != restoring.cookie || candidate.csrf != restoring.csrf) {
                pendingSave = candidate;
                store.save(candidate);
            } else {
                accept(candidate);
            }
        } else {
            switch (restoreFailureAction(status, openOnFallback)) {
            case RestoreFailureAction::OfferRetry:
                attemptedRestore.clear();
                dialog.setError(QStringLiteral("Не удалось проверить сохранённый вход. Проверьте подключение и повторите попытку."));
                startup.finish();
                break;
            case RestoreFailureAction::OpenBrowser:
                launchBrowser(attemptedRestore);
                break;
            case RestoreFailureAction::OfferBrowser:
                dialog.setError(QStringLiteral("Портал отклонил сохранённый вход. Откройте браузер, чтобы обновить подключение."));
                startup.finish();
                break;
            }
        }
    });
    QObject::connect(&signIn, &ChromeSignin::failed, &dialog, [&](const QString &message) {
        if (!activeBrowser.isEmpty() && activeBrowser == dialog.portal())
            dialog.setError(message);
    });
    QObject::connect(&signIn, &ChromeSignin::sessionReady, &dialog, [&](const Session &candidate) {
        if (activeBrowser.isEmpty() || activeBrowser != dialog.portal() || candidate.origin != activeBrowser)
            return;
        if (!validSession(candidate, false)) {
            dialog.setError(QStringLiteral("Портал не подтвердил вход. Откройте браузер и попробуйте ещё раз."));
            return;
        }
        if (recoverCurrent) {
            accept(candidate);
            return;
        }
        pendingSave = candidate;
        store.save(candidate);
    });
    QObject::connect(&store, &signin::SessionStore::saved, &dialog, [&](bool persisted) {
        if (!pendingSave.isValid() || pendingSave.origin != dialog.portal())
            return;
        if (persisted)
            signin::clearSessionRestoreBlock(settings, pendingSave.origin);
        accept(pendingSave);
    });

    if (!requested.isEmpty() && !recoverCurrent) {
        startup.run(dialog, [&] { startRestore(requested, false); });
    } else {
        if (!requested.isEmpty())
            QTimer::singleShot(0, &dialog, [&] { launchBrowser(requested); });
        dialog.exec();
    }
    return session;
}

class ReadProbe final : public QObject {
public:
    ReadProbe(CloudBackend *backend, QObject *parent) : QObject(parent), m_backend(backend) {
        connect(backend, &Backend::recentChatsLoaded, this, [this](const QVector<Chat> &items) {
            if (m_result.contains(QStringLiteral("chats")))
                return;
            record(QStringLiteral("chats"), int(items.size()));
            if (!items.isEmpty())
                m_backend->requestChatMessages(items.first().id);
        });
        connect(backend, &Backend::chatMessagesLoaded, this, [this](const QString &, const QVector<Message> &items) {
            record(QStringLiteral("messages"), int(items.size()));
        });
        connect(backend, &Backend::projectsLoaded, this, [this](const QVector<Project> &items) {
            if (m_result.contains(QStringLiteral("projects")))
                return;
            record(QStringLiteral("projects"), int(items.size()));
            for (const auto &project : items.mid(0, 4))
                m_projects.append(project.id);
            nextBoard();
        });
        connect(backend, &Backend::boardLoaded, this, [this](const QString &, const QVector<Stage> &stages) {
            int taskCount = 0;
            QString firstTask;
            for (const auto &stage : stages) {
                taskCount += int(stage.tasks.size());
                if (firstTask.isEmpty() && !stage.tasks.isEmpty())
                    firstTask = stage.tasks.front().id;
            }
            record(QStringLiteral("stages"), int(stages.size()));
            record(QStringLiteral("boardTasks"), taskCount);
            if (firstTask.isEmpty()) {
                nextBoard();
                return;
            }
            m_backend->requestTask(firstTask);
        });
        connect(backend, &Backend::taskLoaded, this, [this](const Task &task) {
            record(QStringLiteral("task"), true);
            m_backend->requestTaskMessages(task.id);
        });
        connect(backend, &Backend::taskMessagesLoaded, this, [this](const QString &, const QVector<Message> &items) {
            record(QStringLiteral("taskMessages"), int(items.size()));
        });
        connect(backend, &Backend::notificationsLoaded, this, [this](const QVector<Notification> &items) {
            record(QStringLiteral("notifications"), int(items.size()));
        });
        connect(backend, &Backend::connectionChanged, this, [this](bool online) {
            if (online)
                record(QStringLiteral("pushConnected"), true);
        });
        connect(backend, &Backend::failed, this, [this](const QString &operation, const QString &) {
            m_result.insert(QStringLiteral("failed:") + operation.section(QLatin1Char(':'), 0, 0), true);
            if (operation.startsWith(QStringLiteral("board:")))
                nextBoard();
        });
        QTimer::singleShot(15000, this, [this] { finish(false); });
        backend->setForeground(false);
        backend->start();
        backend->requestRecentChats();
        backend->requestProjects();
        backend->requestNotifications();
    }

private:
    void nextBoard() {
        if (!m_projects.isEmpty())
            m_backend->requestProjectBoard(m_projects.takeFirst());
    }

    void record(const QString &key, const QJsonValue &value) {
        m_result.insert(key, value);
        if (m_result.contains(QStringLiteral("messages")) && m_result.contains(QStringLiteral("task"))
            && m_result.contains(QStringLiteral("taskMessages"))
            && m_result.contains(QStringLiteral("notifications"))
            && m_result.contains(QStringLiteral("pushConnected"))) {
            finish(true);
        }
    }

    void finish(bool ok) {
        if (m_finished)
            return;
        m_finished = true;
        const auto output = QJsonDocument(m_result).toJson(QJsonDocument::Compact);
        fprintf(stdout, "%s\n", output.constData());
        QTimer::singleShot(0, qApp, [ok] { QCoreApplication::exit(ok ? 0 : 2); });
    }

    CloudBackend *m_backend;
    QStringList m_projects;
    QJsonObject m_result;
    bool m_finished = false;
};

struct WorkspaceResult {
    int exitCode;
    bool loggedOut;
};

bool forgetPortal(const QUrl &origin)
{
    signin::SessionStore store;
    QEventLoop wait;
    bool forgotten = false;
    QObject::connect(&store, &signin::SessionStore::forgotten, &wait, [&](bool success) {
        forgotten = success;
        wait.quit();
    });
    store.forget(origin);
    wait.exec();

    if (forgotten) {
        QSettings settings(QStringLiteral("bitrix24-client"), QStringLiteral("bitrix24-client"));
        signin::clearSessionRestoreBlock(settings, origin);
    }
    return forgotten;
}

WorkspaceResult runWorkspace(QApplication &app, const QIcon &icon, Backend &backend,
                             CloudBackend *cloud, bool readOnly, const QUrl &logoutPortal = {})
{
    app.setQuitOnLastWindowClosed(true);
    std::optional<bool> connectionOnline;
    QSystemTrayIcon tray(icon);
    QObject::connect(&backend, &Backend::connectionChanged, &tray,
                     [&connectionOnline](bool online) { connectionOnline = online; });

    QMenu trayMenu;
    tray.setToolTip(QStringLiteral("Битрикс24"));
    QPointer<MainWindow> window;
    bool loggedOut = false;
    enum class Recovery { None, WaitingForWindow, Browser, Identity, Manual };
    Recovery recovery = Recovery::None;
    bool reauthDialogOpen = false;
    ChromeSignin passiveSignin;
    QElapsedTimer recoveryElapsed;
    QTimer identityDeadline;
    identityDeadline.setSingleShot(true);
    quint64 recoveryAttempt = 0;
    signin::SessionStore sessionStore;
    Session pendingSession;
    bool savingSession = false;
    const auto saveLatestSession = [&] {
        if (loggedOut || savingSession || !pendingSession.isValid())
            return;
        savingSession = true;
        sessionStore.save(pendingSession);
        pendingSession = {};
    };
    const auto canRecover = cloud && !readOnly && !logoutPortal.isEmpty();
    const auto restoreAllowed = [&] {
        if (loggedOut || reauthDialogOpen || !window || !window->isVisible())
            return false;
        QSettings settings(QStringLiteral("bitrix24-client"), QStringLiteral("bitrix24-client"));
        return !signin::isSessionRestoreBlocked(settings, logoutPortal);
    };
    const auto requireManualSignin = [&] {
        if (recovery != Recovery::Browser && recovery != Recovery::Identity
            && recovery != Recovery::WaitingForWindow)
            return;
        const bool verifying = recovery == Recovery::Identity;
        recovery = Recovery::Manual;
        ++recoveryAttempt;
        identityDeadline.stop();
        passiveSignin.cancel();
        if (verifying)
            cloud->cancelSessionRecovery();
        if (window && !loggedOut) {
            qInfo("Portal background sign-in did not recover authorization");
            window->setSessionRecoveryAvailable(true);
        }
    };
    const auto startPassiveSignin = [&] {
        if (recovery != Recovery::WaitingForWindow || !window || !window->isVisible())
            return;
        if (!restoreAllowed()) {
            requireManualSignin();
            return;
        }
        recovery = Recovery::Browser;
        ++recoveryAttempt;
        recoveryElapsed.start();
        qInfo("Portal background sign-in started");
        passiveSignin.restoreInBackground(logoutPortal);
    };
    if (canRecover) {
        QObject::connect(&identityDeadline, &QTimer::timeout, &tray, requireManualSignin);
        QObject::connect(&passiveSignin, &ChromeSignin::failed, &tray, [&] {
            if (recovery == Recovery::Browser)
                requireManualSignin();
        });
        QObject::connect(&passiveSignin, &ChromeSignin::sessionReady, &tray, [&](const Session &candidate) {
            if (recovery != Recovery::Browser)
                return;
            const auto remaining = ChromeSignin::backgroundDeadlineMs - recoveryElapsed.elapsed();
            if (remaining <= 0 || candidate.origin != logoutPortal || !restoreAllowed()) {
                requireManualSignin();
                return;
            }
            recovery = Recovery::Identity;
            identityDeadline.start(static_cast<int>(remaining));
            const auto attempt = recoveryAttempt;
            cloud->recoverSession(candidate, [&, attempt] {
                return recovery == Recovery::Identity && attempt == recoveryAttempt
                    && !recoveryElapsed.hasExpired(ChromeSignin::backgroundDeadlineMs) && restoreAllowed();
            });
        });
        QObject::connect(cloud, &Backend::failed, &tray, [&](const QString &operation, const QString &) {
            if (recovery == Recovery::Identity && operation == QLatin1String("session-recovery"))
                requireManualSignin();
        });
        QObject::connect(&sessionStore, &signin::SessionStore::saved, &tray, [&](bool) {
            savingSession = false;
            saveLatestSession();
        });
        QObject::connect(cloud, &CloudBackend::sessionUpdated, &tray, [&](const Session &session) {
            pendingSession = session;
            saveLatestSession();
        });
        QObject::connect(cloud, &CloudBackend::sessionRejected, &tray, [&] {
            recovery = Recovery::WaitingForWindow;
            QTimer::singleShot(0, &tray, startPassiveSignin);
        });
        QObject::connect(cloud, &CloudBackend::sessionRecovered, &tray, [&] {
            recovery = Recovery::None;
            identityDeadline.stop();
            if (window) {
                window->setSessionRecoveryAvailable(false);
                window->setStatus(QStringLiteral("Вход в портал обновлён"));
            }
        });
    }
    const auto updateWorkspaceActivity = [&] {
        const auto *native = window ? window->windowHandle() : nullptr;
        const bool visible = native && native->isVisible() && native->visibility() != QWindow::Minimized;
        cloud->setWorkspaceVisible(visible);
        cloud->setForeground(visible && app.applicationState() == Qt::ApplicationActive);
    };
    if (cloud)
        QObject::connect(&app, &QGuiApplication::applicationStateChanged, &tray, updateWorkspaceActivity);
    const auto showWindow = [&] {
        if (loggedOut)
            return;
        const bool creating = !window;
        if (creating) {
            if (cloud)
                cloud->setActiveSection(0);
            window = new MainWindow(&backend, readOnly);
            window->setLogoutAvailable(!logoutPortal.isEmpty());
            QObject::connect(window, &MainWindow::logoutRequested, &tray, [&] {
                if (logoutPortal.isEmpty() || loggedOut)
                    return;
                if (!window->confirmWorkspaceClose())
                    return;
                QSettings settings(QStringLiteral("bitrix24-client"), QStringLiteral("bitrix24-client"));
                if (!signin::blockSessionRestore(settings, logoutPortal)) {
                    window->setStatus(QStringLiteral("Не удалось сохранить выход из портала. Проверьте доступность настроек."));
                    return;
                }
                loggedOut = true;
                requireManualSignin();
                sessionStore.cancel();
                pendingSession = {};
                window->hide();
                tray.hide();
                app.exit(0);
            });
            if (connectionOnline)
                window->onConnectionChanged(*connectionOnline);
            window->setSessionRecoveryAvailable(recovery == Recovery::Manual);
            window->setAttribute(Qt::WA_DeleteOnClose);
            if (cloud) {
                QObject::connect(window, &MainWindow::activeSectionChanged, cloud, &CloudBackend::setActiveSection);
                QObject::connect(window, &MainWindow::historyClosed, cloud, &CloudBackend::clearOpenHistory);
                QObject::connect(window, &QObject::destroyed, cloud, [cloud] {
                    cloud->setForeground(false);
                    cloud->setWorkspaceVisible(false);
                    cloud->clearOpenHistory(false);
                    cloud->clearOpenHistory(true);
                });
            }
            if (canRecover) {
                QObject::connect(window, &QObject::destroyed, &tray, [&] {
                    if (recovery == Recovery::Browser || recovery == Recovery::Identity)
                        requireManualSignin();
                });
                QObject::connect(window, &MainWindow::sessionRecoveryRequested, &tray, [&] {
                    if (recovery != Recovery::Manual || reauthDialogOpen || loggedOut)
                        return;
                    reauthDialogOpen = true;
                    const auto candidate = sessionFromBrowser(logoutPortal.toString(), true, {}, true);
                    reauthDialogOpen = false;
                    if (!loggedOut && candidate.isValid())
                        cloud->recoverSession(candidate);
                });
            }
        }
        window->showNormal();
        if (cloud && creating && window->windowHandle())
            QObject::connect(window->windowHandle(), &QWindow::visibilityChanged, &tray, updateWorkspaceActivity);
        window->raise();
        window->activateWindow();
        if (recovery == Recovery::WaitingForWindow)
            QTimer::singleShot(0, &tray, startPassiveSignin);
        if (cloud)
            updateWorkspaceActivity();
    };

    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        app.setQuitOnLastWindowClosed(false);
        trayMenu.addAction(QStringLiteral("Открыть Битрикс24"), &tray, showWindow);
        trayMenu.addAction(QStringLiteral("Выйти"), &tray, [&] {
            if (!window || window->closeWorkspace())
                app.exit(0);
        });
        tray.setContextMenu(&trayMenu);
        QObject::connect(&tray, &QSystemTrayIcon::activated, &tray, [&](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger)
                showWindow();
        });
        if (cloud)
            QObject::connect(&tray, &QSystemTrayIcon::messageClicked, &tray, showWindow);
        tray.show();
    }
    if (cloud) {
        QObject::connect(cloud, &CloudBackend::notificationReceived, &tray, [&](const Notification &notice) {
            if (desktop::shouldShowNotification(app.applicationState(), loggedOut))
                desktop::showNotification(tray, notice, cloud);
        });
    }

    showWindow();
    backend.start();
    const auto exitCode = app.exec();
    requireManualSignin();
    if (cloud)
        desktop::closeNotifications(*cloud);
    backend.stop();
    app.setQuitOnLastWindowClosed(false);
    delete window.data();
    return {exitCode, loggedOut};
}

} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    ui::installApplicationTypography(app);
    ui::installDialogChrome(app);
    QTranslator qtTranslator;
    if (qtTranslator.load(QStringLiteral("qtbase_ru"), QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        app.installTranslator(&qtTranslator);
    app.setQuitOnLastWindowClosed(false);
    QCoreApplication::setApplicationName(QStringLiteral("bitrix24-client"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0-pilot"));
    app.setDesktopFileName(QStringLiteral("bitrix24-client"));
    QIcon icon;
    for (const auto size : {16, 22, 24, 32, 48, 64, 128, 256})
        icon.addFile(QStringLiteral(":/app-icon/%1.png").arg(size), QSize(size, size));
    app.setWindowIcon(icon);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Лёгкий клиент облачного Битрикс24."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption demoOpt(QStringLiteral("demo"), QStringLiteral("Демо-режим без сети."));
    const QCommandLineOption portalOpt(QStringLiteral("portal"), QStringLiteral("Адрес портала."),
                                       QStringLiteral("url"));
    const QCommandLineOption stdinOpt(QStringLiteral("session-stdin"),
                                      QStringLiteral("Импортировать сессию через stdin. Режим чтения."));
    const QCommandLineOption probeOpt(QStringLiteral("probe"),
                                      QStringLiteral("Проверить чтение и Push&Pull без окна."));
    const QCommandLineOption loopbackOpt(QStringLiteral("allow-loopback"),
                                         QStringLiteral("Разрешить loopback для локальных тестов."));
    parser.addOptions({demoOpt, portalOpt, stdinOpt, probeOpt, loopbackOpt});
    parser.process(app);

    const bool readOnly = parser.isSet(stdinOpt);
    const bool probe = parser.isSet(probeOpt);
    const bool allowLoopback = parser.isSet(loopbackOpt);
    const bool demo = parser.isSet(demoOpt);
    const bool portalGiven = parser.isSet(portalOpt);
    if ((probe && !readOnly) || (readOnly && (demo || portalGiven)) || (demo && portalGiven)) {
        fputs("Use --probe with --session-stdin. Choose only one of --session-stdin, --portal and --demo.\n",
              stderr);
        return 1;
    }

    if (demo) {
        DemoBackend backend;
        return runWorkspace(app, icon, backend, nullptr, false).exitCode;
    }
    if (readOnly) {
        auto session = sessionFromPipe();
        if (!validSession(session, allowLoopback)) {
            fputs("Invalid session input. Expected HTTPS origin, cookie and csrf.\n", stderr);
            return 1;
        }
        CloudBackend backend(std::move(session), allowLoopback);
        if (probe) {
            ReadProbe diagnostic(&backend, &app);
            return app.exec();
        }
        return runWorkspace(app, icon, backend, &backend, true).exitCode;
    }

    auto session = sessionFromBrowser(parser.value(portalOpt), portalGiven);
    while (session.isValid()) {
        const auto origin = session.origin;
        {
            CloudBackend backend(std::move(session), allowLoopback);
            const auto result = runWorkspace(app, icon, backend, &backend, false, origin);
            if (!result.loggedOut)
                return result.exitCode;
        }
        const auto feedback = forgetPortal(origin) ? QString{}
            : QStringLiteral("Не удалось удалить сохранённую сессию: вход по ней заблокирован. "
                             "Введите адрес портала, чтобы продолжить.");

        // A command-line portal only selects the initial workspace.
        session = sessionFromBrowser({}, true, feedback);
    }
    return 0;
}
