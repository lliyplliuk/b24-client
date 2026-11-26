#include "ui/main_window.h"
#include "ui/chat_panel.h"
#include "ui/workspace_message_previews.h"
#include "ui/projects_panel.h"
#include "ui/message_files.h"
#include "ui/section_rail_painter.h"
#include "ui/task_dialog_controls.h"
#include "ui/typography.h"
#include "ui/workspace_style.h"
#include "ui/worktime_panel.h"

#include <QAction>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace bx {

using workspace::label;

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("Битрикс24"));
    setMinimumSize(720, 500);
    resize(1100, 710);
    setStyleSheet(workspace::styleSheet() + ui::editorStyles());

    auto *central = new QWidget(this);
    central->setObjectName(QStringLiteral("workspace"));
    auto *layout = new QHBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *rail = new QWidget(central);
    rail->setObjectName(QStringLiteral("rail"));
    rail->setFixedWidth(94);
    auto *nav = new QVBoxLayout(rail);
    nav->setContentsMargins(6, 14, 6, 10);
    nav->setSpacing(13);
    auto *brand = label(QStringLiteral("B24"), "brand", rail);
    brand->setAccessibleName(QStringLiteral("Битрикс24"));
    brand->setAlignment(Qt::AlignCenter);
    brand->setFixedSize(37, 37);
    nav->addWidget(brand, 0, Qt::AlignHCenter);
    m_sections = new QListWidget(rail);
    m_sections->setObjectName(QStringLiteral("sections"));
    m_sections->setAccessibleName(QStringLiteral("Разделы"));
    m_sections->setFrameShape(QFrame::NoFrame);
    m_sections->setMouseTracking(true);
    m_sections->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_sections->setTextElideMode(Qt::ElideNone);
    m_sections->setItemDelegate(new ui::SectionRailPainter(m_sections));
    m_sections->addItems({QStringLiteral("Чаты"), QStringLiteral("Проекты"), QStringLiteral("Уведомления"),
                          QStringLiteral("Время")});
    m_sections->setCurrentRow(0);
    nav->addWidget(m_sections, 1);
    m_mode = label(m_backend->isDemo() ? QStringLiteral("Демо") : QStringLiteral("Связь…"), "mode", rail);
    m_mode->setAlignment(Qt::AlignCenter);
    m_mode->setToolTip(ui::plainTooltip(m_backend->displayName()));
    nav->addWidget(m_mode);
    rail->setStyleSheet(QStringLiteral(R"(
        QPushButton[railAction="true"] {
            color: #e3f2ed; background: #254953; border: 1px solid #496a70;
            border-radius: 7px; padding: 5px 3px; font-size: 14px;
        }
        QPushButton[railAction="true"]:hover { background: #315963; }
        QPushButton[railAction="true"]:focus { border-color: #a3d1c4; }
        QPushButton[railAction="true"]:pressed { background: #3a646a; }
    )"));
    auto *profile = ui::button(QStringLiteral("Профиль"), QStringLiteral("Настройки профиля"), rail);
    profile->setProperty("railAction", true);
    profile->setToolTip(QStringLiteral("Настройки профиля"));
    connect(profile, &QPushButton::clicked, this, &MainWindow::showProfileSettings);
    nav->addWidget(profile);
    m_logout = ui::button(QStringLiteral("Выйти"), QStringLiteral("Выйти из аккаунта"), rail);
    m_logout->setObjectName(QStringLiteral("logoutButton"));
    m_logout->setProperty("railAction", true);
    m_logout->setToolTip(QStringLiteral("Выйти из аккаунта"));
    connect(m_logout, &QPushButton::clicked, this, &MainWindow::logoutRequested);
    nav->addWidget(m_logout);
    setLogoutAvailable(false);
    layout->addWidget(rail);

    auto *content = new QWidget(central);
    auto *main = new QVBoxLayout(content);
    main->setContentsMargins(0, 0, 0, 0);
    main->setSpacing(0);
    m_stack = new QStackedWidget(content);
    m_chatPanel = new ChatPanel(m_backend, m_readOnly, {
        [this] { return writable(); },
        [this] { return !m_upload.operation.isEmpty(); },
        [this] { return m_tasks && m_tasks->sendPending(); },
        [this] { return m_sessionRecovery && m_sessionRecovery->isHidden(); },
        [this] { return m_tasks ? m_tasks->card() : nullptr; },
        [this](const QWidget *active) { return m_tasks && m_tasks->readActiveTask(active); },
        [this](const QUrl &url) { return m_avatars.object(url); },
        [this](const QUrl &url) { requestAvatar(url); },
        [this](QListWidget *list) { bindMessageFiles(list, true); },
        [this](const QString &id, const QString &kind) { pickFile(id, kind); },
        [this](const QString &id, bool task) { return m_upload.pastedImagePending(id, task); },
        [this](ui::ComposerSendController &controller, ui::ChatComposer *composer, const QString &id) {
            sendFromComposer(controller, composer, id);
        },
        [this](const QString &href) { return messageLinkDestination(href); },
        m_messagePreviews.get(),
        m_messageFiles.get()
    }, m_stack);
    connect(m_chatPanel, &ChatPanel::statusChanged, this, &MainWindow::setStatus);
    connect(m_chatPanel, &ChatPanel::avatarsRequested, this, &MainWindow::scheduleAvatars);
    connect(m_chatPanel, &ChatPanel::avatarRowsChanged, this, &MainWindow::updateAvatarRows);
    connect(m_chatPanel, &ChatPanel::historyClosed, this, &MainWindow::historyClosed);
    connect(m_chatPanel, &ChatPanel::taskRequested, this, &MainWindow::openTaskCard);
    connect(m_chatPanel, &ChatPanel::sectionRequested, this, [this] { m_sections->setCurrentRow(0); });
    connect(m_chatPanel, &ChatPanel::refreshRequested, this, &MainWindow::onRefresh);
    connect(m_chatPanel, &ChatPanel::linkRequested, this, [this](const QString &href) { openMessageLink(href); });
    connect(m_chatPanel, &ChatPanel::unreadCountChanged, this, [this](qint64 count) { setRailUnreadCount(0, count); });
    connect(m_chatPanel, &ChatPanel::dialogChanged, this, [this] { m_projectsPanel->cancelPendingMessageProject(); });
    connect(m_chatPanel, &ChatPanel::directoryEmpty, this, [this] { m_projectsPanel->closeTools(); });
    m_stack->addWidget(m_chatPanel);
    m_projectsPanel = new ProjectsPanel(m_backend, {
        [this] { return writable(); },
        [this] { return !m_tasks || m_tasks->clearTask(); },
        [this](const QUrl &url) { return m_avatars.object(url); }
    }, m_stack);
    connect(m_projectsPanel, &ProjectsPanel::statusChanged, this, &MainWindow::setStatus);
    connect(m_projectsPanel, &ProjectsPanel::avatarsRequested, this, &MainWindow::scheduleAvatars);
    connect(m_projectsPanel, &ProjectsPanel::avatarRowsChanged, this, &MainWindow::updateAvatarRows);
    connect(m_projectsPanel, &ProjectsPanel::writeStateChanged, this, &MainWindow::updateWriteAvailability);
    connect(m_projectsPanel, &ProjectsPanel::taskRequested, this, &MainWindow::openTaskCard);
    connect(m_projectsPanel, &ProjectsPanel::refreshRequested, this, &MainWindow::onRefresh);
    connect(m_projectsPanel, &ProjectsPanel::fileSaveRequested, this,
            [this](const MessageFile &file, QWidget *source) { m_messageFiles->saveMessageFile(file, source); });
    connect(m_projectsPanel, &ProjectsPanel::sectionRequested, this, [this] {
        if (m_lastSection == 1)
            refreshCurrentSection(false);
        else
            m_sections->setCurrentRow(1);
    });
    m_stack->addWidget(m_projectsPanel);
    m_events = new NotificationsPanel(m_backend, m_readOnly, {
        [this] { return m_online; },
        [this] { return m_sessionRecovery->isHidden(); },
        [this](const QString &href) { return messageLinkDestination(href); },
        [this](QListWidget *list) { bindMessageFiles(list, true); }
    }, m_stack);
    connect(m_events, &NotificationsPanel::refreshRequested, this, &MainWindow::onRefresh);
    connect(m_events, &NotificationsPanel::statusChanged, this, &MainWindow::setStatus);
    connect(m_events, &NotificationsPanel::unreadCountChanged, this, [this](qint64 count) { setRailUnreadCount(2, count); });
    connect(m_events, &NotificationsPanel::previewsRequested, this, &MainWindow::scheduleAvatars);
    connect(m_events, &NotificationsPanel::targetRequested, this, [this](const NotificationTarget &target) { openPortalTarget(target); });
    connect(m_events, &NotificationsPanel::linkRequested, this, &MainWindow::openMessageLink);
    m_stack->addWidget(m_events);
    m_worktime = new ui::WorktimePanel(m_backend, m_stack, m_readOnly);
    m_stack->addWidget(m_worktime);
    m_railWorkdayTimer = new QTimer(this);
    m_railWorkdayTimer->setObjectName(QStringLiteral("railWorkdayTimer"));
    m_railWorkdayTimer->setSingleShot(true);
    m_railWorkdayTimer->setTimerType(Qt::PreciseTimer);
    main->addWidget(m_stack, 1);
    m_sessionRecovery = new QWidget(content);
    auto *recovery = new QHBoxLayout(m_sessionRecovery);
    recovery->setContentsMargins(15, 5, 15, 5);
    recovery->addWidget(label(QStringLiteral("Вход в портал истёк"), "status", m_sessionRecovery), 1);
    auto *renew = ui::button(QStringLiteral("Обновить вход"),
                            QStringLiteral("Восстановить подключение к порталу"), m_sessionRecovery);
    recovery->addWidget(renew);
    connect(renew, &QPushButton::clicked, this, &MainWindow::sessionRecoveryRequested);
    m_sessionRecovery->hide();
    main->addWidget(m_sessionRecovery);
    m_status = label(QString(), "status", content);
    m_status->setContentsMargins(15, 7, 15, 7);
    m_status->setAccessibleName(QStringLiteral("Состояние"));
    m_status->hide();
    main->addWidget(m_status);
    layout->addWidget(content, 1);
    setCentralWidget(central);

    auto *refresh = new QAction(this);
    refresh->setShortcut(QKeySequence(Qt::Key_F5));
    connect(refresh, &QAction::triggered, this, &MainWindow::onRefresh);
    addAction(refresh);
    for (int i = 0; i < m_sections->count(); ++i) {
        auto *action = new QAction(this);
        const auto shortcut = QStringLiteral("Alt+%1").arg(i + 1);
        action->setShortcut(QKeySequence(shortcut));
        m_sections->item(i)->setToolTip(QStringLiteral("%1 (%2)").arg(m_sections->item(i)->text(), shortcut));
        connect(action, &QAction::triggered, this, [this, i] { m_sections->setCurrentRow(i); });
        addAction(action);
    }
}

} // namespace bx
