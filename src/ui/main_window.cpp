#include "ui/main_window.h"
#include "ui/chat_panel.h"
#include "ui/workspace_message_previews.h"
#include "ui/projects_panel.h"
#include "ui/message_files.h"
#include "ui/chat_widgets.h"
#include "ui/task_dialogs.h"
#include "ui/typography.h"
#include "ui/workspace_directory.h"
#include "ui/workspace_presentation.h"
#include "ui/workspace_style.h"
#include "ui/worktime_panel.h"
#include "cloud/cloud_notification_links.h"
#include "core/notification_targets.h"
#include "core/operations.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QPushButton>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <memory>
#include <utility>

namespace bx {

using workspace::label;

MainWindow::MainWindow(Backend *backend, bool readOnly, QWidget *parent, PublicLinkRequest publicLinkRequest)
    : QMainWindow(parent), m_backend(backend), m_readOnly(readOnly), m_online(backend->isDemo())
{
    m_messageFiles = std::make_unique<ui::MessageFiles>(backend, [this](bool posterChanged) {
        if (posterChanged)
            m_messagePreviews->refreshChatMediaRows();
        scheduleAvatars();
    }, [this](QWidget *parent, const QString &error) {
        if (!m_tasks->showFileStatus(parent, error))
            setStatus(error);
    }, [this](QWidget *parent, const QString &status, bool loading) {
        m_tasks->showFileStatus(parent, status, loading);
        setStatus(status, loading);
    });
    m_messagePreviews = std::make_unique<WorkspaceMessagePreviews>(backend, m_messageFiles.get(),
        std::move(publicLinkRequest), WorkspaceMessagePreviews::Workspace{
            [this] { return m_chatPanel->history(); }, [this] { return m_chatPanel->currentDialog(); },
            [this] { return m_chatPanel->linkedTaskId(); }, [this] { return m_tasks ? m_tasks->card() : nullptr; }, [this] { return m_events; },
            [this] { return m_online; }, [this] { return !m_sessionRecovery->isHidden(); },
            [this](const QUrl &url) { requestAvatar(url); }, [this](const QUrl &url) { return m_avatars.object(url); },
            [this] { scheduleAvatars(); }
        }, this);
    buildUi();

    connect(m_sections, &QListWidget::currentRowChanged, this, &MainWindow::onSectionChanged);
    connect(m_backend, &Backend::failed, this, &MainWindow::onFailed);
    connect(m_backend, &Backend::connectionChanged, this, &MainWindow::onConnectionChanged);
    connect(m_railWorkdayTimer, &QTimer::timeout, this, &MainWindow::updateRailWorkday);
    connect(qApp, &QGuiApplication::applicationStateChanged, this, &MainWindow::updateRailWorkday);
    const auto applyRailWorkday = [this](const WorkdayStatus &) {
        m_railWorkdayCurrent = m_worktime->hasConfirmedWorkday();
        updateRailWorkday();
    };
    connect(m_backend, &Backend::workdayLoaded, this, applyRailWorkday);
    connect(m_backend, &Backend::workdayChanged, this, applyRailWorkday);
    m_accountScope = m_backend->settingsScope();
    connect(m_backend, &Backend::accountScopeChanged, this, [this] {
        const auto scope = m_backend->settingsScope();
        if (scope != m_accountScope) {
            m_projectsPanel->resetAccount(!m_accountScope.isEmpty());
            if (!m_accountScope.isEmpty()) {
                m_messageFiles->imagePreviews().clear();
                m_upload = {};
                m_tasks->resetAccount();
                m_chatPanel->resetAccount();
                setStatus({});
                updateWriteAvailability();
                m_projectsPanel->resetTaskChats();
                m_chatPanel->updateChatUnreadBadges();
            }
            m_chatPanel->clearPendingReactions();
            m_accountScope = scope;
        }
        reloadChatVisibilityPreference();
        m_projectsPanel->updateColumnWidthContext(scope);
        m_railWorkdayCurrent = m_worktime->hasConfirmedWorkday();
        requestRailWorkday();
        updateRailWorkday();
    });

    m_tasks = std::make_unique<TaskCardController>(m_backend, m_chatPanel, m_projectsPanel,
        m_messagePreviews.get(), TaskCardController::Workspace{
            [this] { return writable(); }, [this] { return m_lastSection == 1; },
            [this](const QUrl &url) { return m_avatars.object(url); },
            [this](const QUrl &url) { requestAvatar(url); },
            [this](QListWidget *list, bool links) { bindMessageFiles(list, links); },
            [this](const QString &scope, const QString &kind) { pickFile(scope, kind); },
            [this](ui::ComposerSendController &sender, ui::ChatComposer *composer, const QString &scope) {
                sendFromComposer(sender, composer, scope);
            }, m_upload
        }, this);
    connect(m_tasks.get(), &TaskCardController::statusChanged, this, &MainWindow::setStatus);
    connect(m_tasks.get(), &TaskCardController::writeStateChanged, this, &MainWindow::updateWriteAvailability);
    connect(m_tasks.get(), &TaskCardController::avatarsRequested, this, &MainWindow::scheduleAvatars);
    connect(m_tasks.get(), &TaskCardController::avatarRowsChanged, this, &MainWindow::updateAvatarRows);
    connect(m_tasks.get(), &TaskCardController::historyClosed, this, &MainWindow::historyClosed);
    connect(m_backend, &Backend::fileUploaded, this, [this](const QString &scope, const QString &kind) {
        if (kind != QStringLiteral("chat") || m_upload.operation != operationKey(OperationKind::UploadChat, scope))
            return;
        const auto upload = std::exchange(m_upload, {});
        m_chatPanel->fileUploaded(scope, QStringLiteral("Файл загружен: %1").arg(upload.name), upload.pastedImage);
        m_chatPanel->updateComposers();
        updateWriteAvailability();
    });
    updateWriteAvailability();

    connect(m_backend, &Backend::avatarLoaded, this, &MainWindow::onAvatarLoaded);
    connect(m_backend, &Backend::personLoaded, this, [this](const Person &person) {
        if (!m_personDialog || m_personId != person.id)
            return;
        const auto name = person.name.simplified();
        m_personName->setText(name.isEmpty() ? QStringLiteral("Профиль сотрудника") : name);
        ui::setLoadingIndicator(m_personName, false);
        m_personPosition->setText(person.position.simplified());
        m_personPosition->setVisible(!person.position.trimmed().isEmpty());
        m_personAvatar->setTitle(name);
        m_personAvatarUrl = person.avatarUrl;
        updateAvatarRows();
        scheduleAvatars();
    });
    m_messagePreviews->connectBackend();

    reloadChatVisibilityPreference();
    m_chatPanel->updateComposers();
    refreshCurrentSection();
    m_chatPanel->updateChatUnreadBadges();
    QTimer::singleShot(0, this, [this] {
        m_events->ensureHistory();
        requestRailWorkday();
    });
}

MainWindow::~MainWindow() = default;

void MainWindow::setLogoutAvailable(bool available)
{
    m_logout->setEnabled(available);
    m_logout->setVisible(available);
}

void MainWindow::setSessionRecoveryAvailable(bool available)
{
    const bool recovering = !m_sessionRecovery->isHidden();
    m_sessionRecovery->setVisible(available);
    if (available) {
        m_railWorkdayCurrent = false;
    } else {
        requestRailWorkday();
        if (recovering)
            m_events->ensureHistory(true);
    }
    updateRailWorkday();
    m_events->scheduleOlderNotifications();
}

void MainWindow::showProfileSettings()
{
    if (!m_profileSettings) {
        m_profileSettings = new QDialog(this);
        m_profileSettings->setProperty("workspaceDialog", true);
        m_profileSettings->setWindowTitle(QStringLiteral("Настройки профиля"));
        auto *layout = new QVBoxLayout(m_profileSettings);
        layout->setContentsMargins(24, 20, 24, 20);
        layout->setSpacing(15);
        layout->addWidget(label(QStringLiteral("Список чатов"), "sectionTitle", m_profileSettings));
        const auto text = QStringLiteral("Показывать чаты с уволенными сотрудниками");
        m_dismissedChatsCheck = new QCheckBox(text, m_profileSettings);
        m_dismissedChatsCheck->setAccessibleName(text);
        layout->addWidget(m_dismissedChatsCheck);
        m_chatSettingsAccountHint = label(QStringLiteral("Настройка станет доступна после загрузки профиля."),
                                          "quiet", m_profileSettings);
        layout->addWidget(m_chatSettingsAccountHint);
        connect(m_dismissedChatsCheck, &QCheckBox::toggled, this, [this](bool show) {
            const auto key = chatPreferenceKey(m_backend->settingsScope(), QLatin1String("showDismissedChats"));
            if (key.isEmpty() && !m_backend->isDemo())
                return;
            if (!key.isEmpty()) {
                auto settings = chatPreferences();
                settings.setValue(key, show);
            }
            m_chatPanel->setShowDismissedChats(show);
        });
        auto *done = ui::button(QStringLiteral("Готово"), QStringLiteral("Закрыть настройки профиля"),
                                m_profileSettings);
        connect(done, &QPushButton::clicked, m_profileSettings, &QDialog::accept);
        layout->addWidget(done, 0, Qt::AlignRight);
        reloadChatVisibilityPreference();
    }
    m_profileSettings->open();
    m_dismissedChatsCheck->setFocus();
}

void MainWindow::reloadChatVisibilityPreference()
{
    const auto key = chatPreferenceKey(m_backend->settingsScope(), QLatin1String("showDismissedChats"));
    auto settings = chatPreferences();
    const bool show = !key.isEmpty() && settings.value(key, false).toBool();
    if (m_dismissedChatsCheck) {
        const bool canChange = !key.isEmpty() || m_backend->isDemo();
        const QSignalBlocker blocker(m_dismissedChatsCheck);
        m_dismissedChatsCheck->setChecked(show);
        m_dismissedChatsCheck->setEnabled(canChange);
        m_chatSettingsAccountHint->setVisible(!canChange);
    }
    m_chatPanel->setShowDismissedChats(show);
}

bool MainWindow::confirmWorkspaceClose()
{
    if (m_closingWorkspace)
        return false;
    const QScopedValueRollback closing(m_closingWorkspace, true);
    return m_tasks->confirmDiscard();
}

bool MainWindow::closeWorkspace()
{
    return !m_closingWorkspace && close();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_closingWorkspace) {
        event->ignore();
        return;
    }
    const QScopedValueRollback closing(m_closingWorkspace, true);
    if (!m_tasks->clearTask()) {
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    updateRailWorkday();
    m_events->scheduleOlderNotifications();
}

void MainWindow::hideEvent(QHideEvent *event)
{
    m_railWorkdayTimer->stop();
    QMainWindow::hideEvent(event);
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        updateRailWorkday();
        m_events->scheduleOlderNotifications();
    }
    if (event->type() == QEvent::ActivationChange && !isActiveWindow() && m_chatPanel)
        m_chatPanel->windowDeactivated();
}

void MainWindow::onSectionChanged(int index)
{
    m_projectsPanel->setSectionActive(index == 1);
    m_events->setSectionActive(index == 2);
    m_lastSection = index;
    m_chatPanel->setSectionActive(index == 0);
    m_stack->setCurrentIndex(index);
    emit activeSectionChanged(index);
    setStatus(QString());
    refreshCurrentSection(false);
    if (index == 2)
        m_events->scrollToTop();
    if (index == 1)
        m_projectsPanel->resumeSearch();
}

void MainWindow::refreshCurrentSection(bool reloadProjectDirectory)
{
    switch (m_lastSection) {
    case 0:
        m_chatPanel->refresh(reloadProjectDirectory);
        break;
    case 1: {
        m_projectsPanel->refresh(reloadProjectDirectory);
        m_tasks->refresh();
        break;
    }
    case 2:
        m_events->refresh(reloadProjectDirectory);
        break;
    case 3:
        m_worktime->refresh(reloadProjectDirectory);
        break;
    default:
        break;
    }
}

void MainWindow::onRefresh()
{
    if (m_lastSection == 2)
        m_events->clearReadAndPageFailures();
    for (const auto &url : m_avatars.keys()) {
        const auto *photo = m_avatars.object(url);
        if (photo && photo->isNull())
            m_avatars.remove(url);
    }
    m_chatPanel->resetMessageContext();
    setStatus(QString());
    refreshCurrentSection();
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    if (!m_projectsPanel)
        return;
    scheduleAvatars();
    if (m_events)
        m_events->scheduleVisibleNoticeRead();
}

void MainWindow::onFailed(const QString &operation, const QString &reason)
{
    const auto [kind, ids] = parseOperation(operation);
    const bool networkFailure = reason.startsWith(QStringLiteral("Ошибка сети или истекло время ожидания"));
    // These reads are ancillary, or belong to history the user has already left.
    // Access failures and other diagnostics still use the normal failure path.
    if (networkFailure && (kind == OperationKind::Push
        || kind == OperationKind::TaskChatTitles
        || kind == OperationKind::ChatHistoryIdentity
        || kind == OperationKind::Pins))
        return;

    if (m_events->handleFailure(operation, kind, ids, reason))
        return;

    bool statusPresented = false;
    QString footer = reason;
    if (m_projectsPanel->handleFailure(operation, kind, ids, reason, footer))
        return;
    if (m_chatPanel->handleFailure(operation, kind, ids, reason, networkFailure, footer, statusPresented))
        return;
    if (m_tasks->handleFailure(operation, kind, ids, reason, networkFailure, statusPresented))
        return;
    switch (kind) {
    case OperationKind::Workday:
    case OperationKind::WorkdayChange:
    case OperationKind::WorktimeSheet:
    case OperationKind::WorktimeExport:
        return; // The worktime panel presents its own request-scoped errors.
    case OperationKind::MessageFileDownload:
        m_messageFiles->downloadFailed(reason);
        return;
    case OperationKind::Person:
        if (m_personDialog && ids.value(0) == m_personId) {
            m_personName->setText(reason);
            ui::setLoadingIndicator(m_personName, false);
        }
        return;
    case OperationKind::UploadChat:
        if (operation != m_upload.operation)
            return;
        m_upload = {};
        break;
    default:
        break;
    }
    if (!statusPresented)
        setStatus(footer);
    m_chatPanel->updateComposers();
    updateWriteAvailability();
}

void MainWindow::onConnectionChanged(bool online)
{
    const bool restored = !m_online && online;
    m_online = online;
    if (!online)
        m_railWorkdayCurrent = false;
    m_worktime->setPortalOnline(online);
    m_mode->setText(m_backend->isDemo() ? QStringLiteral("Демо")
                                      : (online ? QStringLiteral("В сети") : QStringLiteral("Нет связи")));
    m_mode->setProperty("online", online);
    m_mode->style()->unpolish(m_mode);
    m_mode->style()->polish(m_mode);
    m_chatPanel->updateComposers();
    updateWriteAvailability();
    if (restored) {
        requestRailWorkday();
        m_chatPanel->scheduleVisibleChatRead();
        m_events->connectionRestored();
        m_messagePreviews->scheduleExternalLinkPreviews();
    }
    updateRailWorkday();
}

void MainWindow::setStatus(const QString &text, bool loading)
{
    ui::setStatusLabel(m_status, text, loading);
}

void MainWindow::updateWriteAvailability()
{
    m_events->updateNotificationActions();
    m_projectsPanel->setWriteAvailability(writable(), m_tasks->creationPending());
    m_tasks->updateWriteState();
}

QString MainWindow::taskLinkFor(const QString &taskId) const
{
    return ui::taskLink(m_backend->portalOrigin(), m_projectsPanel->currentProjectId(), taskId);
}

void MainWindow::openTaskCard(const QString &taskId)
{
    m_tasks->openTaskCardForProject(taskId, m_lastSection == 1 ? m_projectsPanel->currentProjectId() : QString());
}

QUrl MainWindow::messageLinkDestination(const QString &href) const
{
    const auto target = workspace::messageLinkTarget(href);
    if (target.isEmpty())
        return {};
    QUrl url(target, QUrl::StrictMode);
    if (url.isRelative()) {
        const auto origin = m_backend->portalOrigin();
        if (!origin.isValid() || origin.scheme() != QStringLiteral("https"))
            return {};
        url = origin.resolved(url);
    }
    return workspace::messageWebLink(url.toString(QUrl::FullyEncoded));
}

void MainWindow::openMessageLink(const QString &href, QWidget *source)
{
    const auto url = messageLinkDestination(href);
    if (url.isEmpty())
        return;
    if (url.scheme() == QStringLiteral("https")) {
        if (const auto target = portalLinkTarget(m_backend->portalOrigin(), url)) {
            openPortalTarget(*target, source, url);
            return;
        }
    }
    m_projectsPanel->cancelPendingMessageProject();
    QDesktopServices::openUrl(url);
}

void MainWindow::setClientUpdateFeed(ClientUpdateFeed *feed)
{
    m_events->setClientUpdateFeed(feed);
}

void MainWindow::setRailUnreadCount(int row, qint64 count)
{
    auto *item = m_sections->item(row);
    if (item->data(ui::SectionUnreadCount).toLongLong() == count)
        return;
    item->setData(ui::SectionUnreadCount, count);
    const auto label = row == 0 ? QStringLiteral("Непрочитанных сообщений · %1")
                                : QStringLiteral("Непрочитанных уведомлений · %1");
    setRailStatus(row, count > 0 ? label.arg(count) : QString());
}

void MainWindow::setRailStatus(int row, const QString &detail)
{
    auto *item = m_sections->item(row);
    if (item->data(Qt::AccessibleDescriptionRole).toString() == detail)
        return;
    item->setData(Qt::AccessibleDescriptionRole, detail);
    const auto shortcut = QStringLiteral("%1 (Alt+%2)").arg(item->text()).arg(row + 1);
    item->setToolTip(detail.isEmpty() ? shortcut : shortcut + QStringLiteral(" · ") + detail);
}

void MainWindow::requestRailWorkday()
{
    if (m_online && m_sessionRecovery->isHidden() && !m_railWorkdayCurrent)
        m_worktime->requestWorkdaySummary();
}

void MainWindow::updateRailWorkday()
{
    if (!m_railWorkdayTimer)
        return;
    const auto state = m_online && m_railWorkdayCurrent && m_sessionRecovery->isHidden()
        ? m_worktime->workdayState() : WorkdayState::Unavailable;
    const bool running = state == WorkdayState::Running;
    const bool paused = state == WorkdayState::Paused;
    const auto elapsed = running || paused ? m_worktime->elapsedWorkdaySeconds() : -1;
    const auto duration = elapsed >= 0
        ? QStringLiteral("%1:%2").arg(elapsed / 3600, 2, 10, QLatin1Char('0'))
                                 .arg((elapsed / 60) % 60, 2, 10, QLatin1Char('0')) : QString();
    auto *item = m_sections->item(3);
    if (item->data(ui::SectionWorkdayStatus).toInt() != int(state))
        item->setData(ui::SectionWorkdayStatus, int(state));
    if (item->data(ui::SectionWorkdayElapsed).toString() != duration)
        item->setData(ui::SectionWorkdayElapsed, duration);
    const auto label = running ? QStringLiteral("В работе") : paused ? QStringLiteral("На паузе") : QString();
    setRailStatus(3, duration.isEmpty() ? label : label + QStringLiteral(" · ") + duration);
    if (running && elapsed >= 0 && isVisible() && !isMinimized()
        && QGuiApplication::applicationState() == Qt::ApplicationActive)
        m_railWorkdayTimer->start(int(60 - elapsed % 60) * 1000);
    else
        m_railWorkdayTimer->stop();
}

void MainWindow::openPortalTarget(NotificationTarget target, QWidget *source, const QUrl &messageUrl)
{
    if (target.id.isEmpty())
        return;
    m_projectsPanel->cancelPendingMessageProject();
    switch (target.kind) {
    case NotificationTargetKind::Chat:
        if (!m_tasks->activateChat(target) && m_tasks->clearTask()) {
            m_chatPanel->openLinkedChat(target);
        }
        break;
    case NotificationTargetKind::Task:
        m_tasks->openTaskCardForProject(target.id, target.projectId);
        break;
    case NotificationTargetKind::Person:
        openPerson(target.id, source);
        break;
    case NotificationTargetKind::Project: {
        m_projectsPanel->openLinkedProject(target, messageUrl);
        break;
    }
    }
}

void MainWindow::openPerson(const QString &id, QWidget *source)
{
    auto *parentWindow = source ? source->window() : this;
    if (m_personDialog) {
        if (m_personId == id && m_personDialog->parentWidget() == parentWindow) {
            m_personDialog->raise();
            m_personDialog->activateWindow();
            return;
        }
        m_personDialog->close();
    }
    auto *dialog = new QDialog(parentWindow);
    dialog->setObjectName(QStringLiteral("personProfileDialog"));
    dialog->setWindowTitle(QStringLiteral("Профиль сотрудника"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setMinimumWidth(300);
    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(18, 16, 18, 14);
    auto *heading = new QHBoxLayout;
    m_personAvatar = new ui::AvatarBadge(dialog);
    m_personAvatar->hide();
    heading->addWidget(m_personAvatar);
    m_personName = label(QStringLiteral("Загружаем профиль…"), "conversationTitle", dialog);
    ui::setLoadingIndicator(m_personName, true);
    m_personName->setAccessibleName(QStringLiteral("Имя сотрудника"));
    m_personName->setWordWrap(true);
    heading->addWidget(m_personName, 1);
    layout->addLayout(heading);
    m_personPosition = label({}, "meta", dialog);
    m_personPosition->setAccessibleName(QStringLiteral("Должность"));
    m_personPosition->setWordWrap(true);
    m_personPosition->hide();
    layout->addWidget(m_personPosition);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("Закрыть"));
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);
    m_personDialog = dialog;
    m_personId = id;
    m_personAvatarUrl.clear();
    connect(dialog, &QDialog::finished, this, [this, dialog] {
        if (m_personDialog == dialog) {
            m_personDialog = nullptr;
            m_personId.clear();
            m_personAvatarUrl.clear();
        }
    });
    dialog->show();
    m_backend->requestPerson(id);
}

} // namespace bx
