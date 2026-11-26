#include "ui/main_window.h"
#include "ui/chat_panel.h"
#include "ui/workspace_message_previews.h"
#include "ui/message_menu_window.h"
#include "ui/projects_panel.h"
#include "ui/message_files.h"
#include "ui/workspace_message_viewers.h"
#include "ui/chat_widgets.h"
#include "ui/task_dialogs.h"
#include "ui/reaction_catalog.h"
#include "ui/typography.h"
#include "ui/workspace_directory.h"
#include "ui/workspace_presentation.h"
#include "ui/workspace_style.h"
#include "ui/worktime_panel.h"
#include "cloud/cloud_notification_links.h"
#include "core/notification_targets.h"
#include "core/origin_guard.h"
#include "core/operations.h"

#include <QAction>
#include <QTabBar>
#include <QMenu>
#include <QImageReader>
#include <QBuffer>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <iterator>
#include <limits>
#include <memory>
#include <utility>

namespace bx {

using workspace::label;

MainWindow::MainWindow(Backend *backend, bool readOnly, QWidget *parent, PublicLinkRequest publicLinkRequest)
    : QMainWindow(parent), m_backend(backend), m_readOnly(readOnly), m_online(backend->isDemo()),
      m_taskMessageSearch(backend, true),
      m_taskSendController(ui::ComposerSendController::Surface::Task, backend,
          [this](const QString &id, const QString &status, bool loading) {
              if (m_card && m_card->taskId() == id)
                  m_card->setStatus(status, loading);
              else if (!status.isEmpty())
                  setStatus(status, loading);
              updateWriteAvailability();
          })
{
    m_messageFiles = std::make_unique<ui::MessageFiles>(backend, [this](bool posterChanged) {
        if (posterChanged)
            m_messagePreviews->refreshChatMediaRows();
        scheduleAvatars();
    }, [this](QWidget *parent, const QString &error) {
        if (m_card && parent == m_card)
            m_card->setStatus(error);
        else
            setStatus(error);
    }, [this](QWidget *parent, const QString &status, bool loading) {
        if (m_card && parent == m_card)
            m_card->setStatus(status, loading);
        setStatus(status, loading);
    });
    m_messagePreviews = std::make_unique<WorkspaceMessagePreviews>(backend, m_messageFiles.get(),
        std::move(publicLinkRequest), WorkspaceMessagePreviews::Workspace{
            [this] { return m_chatPanel->history(); }, [this] { return m_chatPanel->currentDialog(); },
            [this] { return m_chatPanel->linkedTaskId(); }, [this] { return m_card; }, [this] { return m_events; },
            [this] { return m_online; }, [this] { return !m_sessionRecovery->isHidden(); },
            [this](const QUrl &url) { requestAvatar(url); }, [this](const QUrl &url) { return m_avatars.object(url); },
            [this] { scheduleAvatars(); }
        }, this);
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
        [this] { return !m_pendingUpload.isEmpty(); },
        [this] { return m_taskSendController.pending(); },
        [this] { return m_sessionRecovery && m_sessionRecovery->isHidden(); },
        [this] { return m_card; },
        [this](const QWidget *active) { return readActiveTask(active); },
        [this](const QUrl &url) { return m_avatars.object(url); },
        [this](const QUrl &url) { requestAvatar(url); },
        [this](QListWidget *list) { bindMessageFiles(list, true); },
        [this](const QString &id, const QString &kind) { pickFile(id, kind); },
        [this](const QString &id, bool task) { return pastedChatImagePending(id, task); },
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
        [this] { return clearTask(); },
        [this](const QUrl &url) { return m_avatars.object(url); }
    }, m_stack);
    connect(m_projectsPanel, &ProjectsPanel::statusChanged, this, &MainWindow::setStatus);
    connect(m_projectsPanel, &ProjectsPanel::avatarsRequested, this, &MainWindow::scheduleAvatars);
    connect(m_projectsPanel, &ProjectsPanel::avatarRowsChanged, this, &MainWindow::updateAvatarRows);
    connect(m_projectsPanel, &ProjectsPanel::writeStateChanged, this, &MainWindow::updateWriteAvailability);
    connect(m_projectsPanel, &ProjectsPanel::taskRequested, this, &MainWindow::openTaskCard);
    connect(m_projectsPanel, &ProjectsPanel::projectTaskRequested, this, &MainWindow::openTaskCardForProject);
    connect(m_projectsPanel, &ProjectsPanel::taskCreationRequested, this, &MainWindow::createTaskInProject);
    connect(m_projectsPanel, &ProjectsPanel::refreshRequested, this, &MainWindow::onRefresh);
    connect(m_projectsPanel, &ProjectsPanel::fileSaveRequested, this,
            [this](const MessageFile &file, QWidget *source) { m_messageFiles->saveMessageFile(file, source); });
    connect(m_projectsPanel, &ProjectsPanel::directoryChanged, this, [this](const QVector<Project> &projects) {
        if (m_card) {
            m_card->setProjectChoices(projects);
            scheduleAvatars();
        }
        if (m_taskForm)
            m_taskForm->setProjectChoices(projects);
    });
    connect(m_projectsPanel, &ProjectsPanel::moreProjectsChanged, this, [this](bool available) {
        if (m_card)
            m_card->setMoreProjectsAvailable(available);
        if (m_taskForm)
            m_taskForm->setMoreProjectsAvailable(available);
    });
    connect(m_projectsPanel, &ProjectsPanel::formStagesLoaded, this, [this](const QString &id, const QVector<Stage> &stages) {
        if (m_taskForm && m_taskForm->projectId() == id)
            m_taskForm->setStages(stages);
    });
    connect(m_projectsPanel, &ProjectsPanel::currentStagesLoaded, this, [this](const QString &id, const QVector<Stage> &stages) {
        if (m_card && m_card->projectId() == id)
            m_card->setStages(stages);
    });
    connect(m_projectsPanel, &ProjectsPanel::taskMoved, this, [this](const QString &id) {
        if (m_card && m_card->taskId() == id) {
            m_card->setStatus(QStringLiteral("Задача перенесена"));
            m_backend->requestTask(id);
        }
    });
    connect(m_projectsPanel, &ProjectsPanel::sectionRequested, this, [this] {
        if (m_lastSection == 1)
            refreshCurrentSection(false);
        else
            m_sections->setCurrentRow(1);
    });
    connect(m_projectsPanel, &ProjectsPanel::boardMoveFailed, this, [this](const QString &id, const QString &reason) {
        if (m_card && m_card->taskId() == id)
            m_card->setStatus(reason);
    });
    connect(m_projectsPanel, &ProjectsPanel::directoryLoadFailed, this, [this] { m_cardRequestedProjectDirectory = false; });
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
                m_pendingPastedChatImage = {};
                m_pendingCreateTask.clear();
                m_pendingCreateSubtask.clear();
                m_pendingUpload.clear();
                m_uploadName.clear();
                m_taskSendController.reset();
                if (m_descriptionUploadCard)
                    m_descriptionUploadCard->descriptionEditor()->setUploadStatus({});
                clearDescriptionUpload();
                if (m_taskForm)
                    m_taskForm->done(QDialog::Rejected);
                m_pendingTaskEdit.clear();
                m_pendingTaskEditCard.clear();
                m_pendingTaskEditProject.clear();
                m_pendingTaskEditTargetProject.clear();
                m_pendingTaskDelete.clear();
                m_pendingTaskDeleteCard.clear();
                m_pendingTaskDeleteProject.clear();
                if (m_card)
                    m_card->done(QDialog::Rejected);
                m_chatPanel->resetAccount();
                m_taskMessageSearch.reset();
                m_taskDrafts.clear();
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

    connectWorkspaceActions();

    connect(m_backend, &Backend::avatarLoaded, this, [this](const QUrl &url, const QByteArray &bytes) {
        if (!m_requestedAvatars.remove(url))
            return;
        auto *photo = new QPixmap;
        if (!bytes.isEmpty() && bytes.size() <= 512 * 1024) {
            QBuffer input;
            input.setData(bytes);
            input.open(QIODevice::ReadOnly);
            QImageReader reader(&input);
            const auto format = reader.format();
            const auto size = reader.size();
            if ((format == "png" || format == "jpeg" || format == "webp" || format == "gif") &&
                size.isValid() && !size.isEmpty() && size.width() <= 1200 && size.height() <= 1200) {
                const auto scaled = size.scaled(144, 144, Qt::KeepAspectRatio);
                reader.setScaledSize(QSize(qMax(1, scaled.width()), qMax(1, scaled.height())));
                const auto image = reader.read();
                if (!image.isNull() && image.width() <= 1200 && image.height() <= 1200) {
                    const int side = qMin(image.width(), image.height());
                    const QRect crop((image.width() - side) / 2, (image.height() - side) / 2, side, side);
                    *photo = QPixmap::fromImage(image.copy(crop).scaled(72, 72, Qt::IgnoreAspectRatio,
                                                                        Qt::SmoothTransformation));
                }
            }
        }
        m_avatars.insert(url, photo);
        m_projectsPanel->refreshBoardAvatars();
        m_chatPanel->refreshMentionAvatars();
        if (m_card) {
            m_card->refreshRequisiteAvatars();
            m_card->composer()->refreshMentionAvatars();
        }
        if (m_taskForm)
            m_taskForm->refreshRequisiteAvatars();
        updateAvatarRows();
        scheduleAvatars();
    });
    connect(m_backend, &Backend::pinnedMessagesLoaded, this,
            [this](const QString &dialog, const QVector<Message> &messages) {
        if (dialog.isEmpty())
            return;
        if (m_card && dialog == m_card->taskChatDialogId())
            m_card->setPinnedMessages(messages);
    });
    connect(m_backend, &Backend::olderChatMessagesLoaded, this,
            [this](const QString &scope, const QString &before, bool task, const QVector<Message> &messages, bool more) {
        if (!task || !m_card || scope != m_card->taskId() || !m_cardSearchQuery.isEmpty())
            return;
        auto *history = static_cast<ui::HistoryList *>(m_card->messageList());
        if (history->addOlder(scope, before, messages, more)) {
            updateAvatarRows();
            scheduleAvatars();
        }
    });
    connect(m_backend, &Backend::chatReadConfirmed, this,
            [this](const QString &dialog, const QStringList &ids, qint64 readThrough) {
        if (dialog.isEmpty())
            return;
        if (m_card && m_card->taskChatDialogId() == dialog && m_cardSearchQuery.isEmpty() && m_cardContextMessageId.isEmpty())
            static_cast<ui::HistoryList *>(m_card->messageList())->confirmRead(ids, readThrough);
    });
    connect(m_backend, &Backend::chatHistoryRemoved, this,
            [this](const QString &dialog, const QStringList &ids, qint64 through) {
        if (dialog.isEmpty())
            return;
        const bool cleared = ids.isEmpty() && through == 0;
        if (m_card && dialog == m_card->taskChatDialogId()) {
            static_cast<ui::HistoryList *>(m_card->messageList())->removeHistory(ids, through);
            if (cleared && m_cardSearchQuery.isEmpty()) {
                m_cardContextMessageId.clear();
                m_card->setContextMode(false);
                m_card->setChatStatus(QStringLiteral("Сообщений пока нет"));
            }
        }
    });

    connect(m_backend, &Backend::messageContextLoaded, this,
            [this](const QString &dialogId, const QString &messageId, const QVector<Message> &messages) {
        if (m_card && !m_cardContextMessageId.isEmpty() && dialogId == m_card->taskChatDialogId() &&
            messageId == m_cardContextMessageId) {
            m_card->showMessages(messages, ui::MessageView::Context);
            scheduleAvatars();
            if (!m_card->focusMessage(messageId)) {
                m_cardContextMessageId.clear();
                m_card->setContextMode(false);
                m_backend->requestTaskMessages(m_card->taskId());
                m_card->setStatus(QStringLiteral("Сообщение не найдено в истории чата задачи"));
            }
            return;
        }
        m_chatPanel->showMessageContext(dialogId, messageId, messages);
    });
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

    connect(m_backend, &Backend::peopleFound, this, &MainWindow::onPeopleFound);
    connect(m_backend, &Backend::projectStagesLoaded, this,
            [this](const QString &projectId, const QVector<Stage> &stages) {
        if (m_taskForm && m_taskForm->projectId() == projectId)
            m_taskForm->setStages(stages);
        if (!m_card || m_card->projectId() != projectId || m_cardRequestedStages != projectId)
            return;
        if (projectId != m_projectsPanel->currentProjectId() || m_projectsPanel->stages().isEmpty())
            m_card->setStages(stages);
    });

    connect(m_backend, &Backend::taskSummaryLoaded, this, &MainWindow::onTaskSummaryLoaded);
    connect(m_backend, &Backend::taskLoaded, this, [this](const Task &task) {
        if (!m_card || task.id != m_card->taskId())
            return;
        auto *const card = m_card;
        m_cardSummaryPending = false;
        const auto previousDialog = m_card->taskChatDialogId();
        if (!m_cardDetailsReady)
            card->setStatus({});
        m_card->setTask(task);
        const auto projectId = m_card->projectId();
        if (!m_projectsPanel->directoryLoaded() && !m_cardRequestedProjectDirectory
            && (task.editRights.project || !projectId.isEmpty())) {
            m_cardRequestedProjectDirectory = true;
            m_backend->requestProjects();
            if (m_card != card)
                return;
        }
        if (projectId == m_projectsPanel->currentProjectId() && !m_projectsPanel->stages().isEmpty()) {
            m_card->setStages(m_projectsPanel->stages());
        } else if (task.editRights.stage && !projectId.isEmpty() && m_cardRequestedStages != projectId) {
            m_cardRequestedStages = projectId;
            m_backend->requestProjectStages(projectId);
        }
        updateTaskCardChatBinding(previousDialog);
        if (!task.projectId.isEmpty())
            m_card->setLink(ui::taskLink(m_backend->portalOrigin(), task.projectId, task.id));
        scheduleAvatars();
        m_cardDetailsReady = true;
        updateWriteAvailability();
        if (m_loadCardChat) {
            m_loadCardChat = false;
            m_backend->requestTaskMessages(task.id);
        }
        m_chatPanel->scheduleVisibleChatRead();
    });
    connect(m_backend, &Backend::taskMessagesLoaded, this,
            [this](const QString &taskId, const QVector<Message> &messages) {
        if (m_card && taskId == m_card->taskId() && m_cardSearchQuery.isEmpty() && m_cardContextMessageId.isEmpty()) {
            m_card->showMessages(messages);
            updateAvatarRows();
            scheduleAvatars();
        }
    });

    connect(m_backend, &Backend::messageSent, this, [this](const QString &scope, const QString &) {
        m_taskSendController.confirm(scope, m_card ? m_card->composer() : nullptr,
                                     m_card ? m_card->taskId() : QString{}, m_taskDrafts);
    });

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

bool MainWindow::clearTask()
{
    return !m_card || m_card->close();
}

bool MainWindow::confirmWorkspaceClose()
{
    if (m_closingWorkspace)
        return false;
    const QScopedValueRollback closing(m_closingWorkspace, true);
    return !m_card || m_card->confirmDiscardIfNeeded(false);
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
    if (!clearTask()) {
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
        if (m_card) {
            m_backend->requestTask(m_card->taskId());
            if (!m_card->relations()->checklistPending())
                m_backend->requestTaskChecklist(m_card->taskId());
            m_backend->requestSubtasks(m_card->taskId());
            m_backend->requestTaskDependencies(m_card->taskId());
        }
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

void MainWindow::sendFromComposer(ui::ComposerSendController &controller, ui::ChatComposer *composer, const QString &id)
{
    const bool task = controller.isTask();
    controller.send(*composer, id, writable(), pastedChatImagePending(id, task),
                    [this, composer, id, task](const QString &caption) {
                        uploadPastedChatImage(composer, id, task, caption);
                    });
}

bool MainWindow::readActiveTask(const QWidget *active)
{
    if (!m_card || active != m_card || !m_cardDetailsReady || !m_cardSearchQuery.isEmpty() || !m_cardContextMessageId.isEmpty())
        return false;
    const auto seen = static_cast<ui::HistoryList *>(m_card->messageList())->visibleMessageIds();
    if (!seen.all.isEmpty())
        m_backend->readTaskChatMessages(m_card->taskId(), seen.incoming, seen.all);
    return true;
}

void MainWindow::onPeopleFound(const QString &requestId, const QVector<Person> &people)
{
    if (m_taskForm)
        m_taskForm->setPeopleResults(requestId, people);
    if (m_card) {
        m_card->composer()->setMentionResults(requestId, people);
        m_card->setPeopleResults(requestId, people);
    }
}

void MainWindow::onFailed(const QString &operation, const QString &reason)
{
    const auto [kind, ids] = parseOperation(operation);
    const bool networkFailure = reason.startsWith(QStringLiteral("Ошибка сети или истекло время ожидания"));
    const bool activeTaskHistory = m_card && m_cardSearchQuery.isEmpty()
        && kind == OperationKind::TaskMessages && ids.value(0) == m_card->taskId();
    // These reads are ancillary, or belong to history the user has already left.
    // Access failures and other diagnostics still use the normal failure path.
    if (networkFailure && (kind == OperationKind::Push
        || kind == OperationKind::TaskChatTitles
        || kind == OperationKind::ChatHistoryIdentity
        || kind == OperationKind::Pins
        || (kind == OperationKind::TaskMessages && !activeTaskHistory)))
        return;

    if (m_events->handleFailure(operation, kind, ids, reason))
        return;

    bool statusPresented = false;
    QString footer = reason;
    if (m_projectsPanel->handleFailure(operation, kind, ids, reason, footer))
        return;
    if (m_chatPanel->handleFailure(operation, kind, ids, reason, networkFailure, footer, statusPresented))
        return;
    switch (kind) {
    case OperationKind::SearchPeople: {
        const auto request = ids.value(0);
        if (m_taskForm)
            m_taskForm->setPeopleSearchError(request);
        if (m_card) {
            m_card->composer()->setMentionSearchError(request);
            m_card->setPeopleSearchError(request);
        }
        return;
    }
    case OperationKind::Workday:
    case OperationKind::WorkdayChange:
    case OperationKind::WorktimeSheet:
    case OperationKind::WorktimeExport:
        return; // The worktime panel presents its own request-scoped errors.
    case OperationKind::OlderChat: {
        if (m_card)
            static_cast<ui::HistoryList *>(m_card->messageList())->olderFailed(ids.value(1), ids.value(2), reason);
        return;
    }
    case OperationKind::ProjectStages:
        if (m_taskForm && ids.value(0) == m_taskForm->projectId())
            m_taskForm->setStagesError(reason);
        if (m_card && ids.value(0) == m_cardRequestedStages) {
            m_cardRequestedStages.clear();
            m_card->setStatus(reason);
        }
        return;
    case OperationKind::TaskHistory:
        if (m_card && ids.value(0) == m_card->taskId())
            m_card->relations()->historyFailed();
        return;
    case OperationKind::MessageEdit: {
        const auto dialog = ids.value(0);
        const auto id = ids.value(1);
        if (m_card && m_card->composer()->matchesEdit(dialog, id)) {
            m_card->composer()->failMessageEdit();
            m_card->setStatus(reason);
        }
        return;
    }
    case OperationKind::Checklist:
    case OperationKind::ChecklistWrite:
    case OperationKind::Subtasks:
    case OperationKind::GanttLinks:
        if (!m_card || ids.value(0) != m_card->taskId())
            break;
        if (kind == OperationKind::Checklist)
            m_card->relations()->checklistReadFailed();
        else if (kind == OperationKind::ChecklistWrite)
            m_card->relations()->setChecklistPending(false);
        else if (kind == OperationKind::Subtasks)
            m_card->relations()->subtasksReadFailed();
        else
            m_card->relations()->dependenciesReadFailed();
        m_card->setStatus(reason);
        return;
    case OperationKind::CreateSubtask:
        if (m_pendingCreateSubtask.isEmpty() || ids.value(0) != m_pendingCreateSubtask)
            break;
        m_pendingCreateSubtask.clear();
        if (m_taskForm)
            m_taskForm->setStatus(reason);
        updateWriteAvailability();
        return;
    case OperationKind::MessageContext:
        if (m_card && !m_cardContextMessageId.isEmpty() && ids.value(0) == m_card->taskChatDialogId()
            && ids.value(1) == m_cardContextMessageId) {
            m_cardContextMessageId.clear();
            m_card->setContextMode(false);
            m_card->setStatus(reason);
            m_backend->requestTaskMessages(m_card->taskId());
            return;
        }
        if (m_chatPanel->failMessageContext(ids, reason))
            return;
        break;
    case OperationKind::MessageFileDownload:
        m_messageFiles->downloadFailed(reason);
        return;
    case OperationKind::Person:
        if (m_personDialog && ids.value(0) == m_personId) {
            m_personName->setText(reason);
            ui::setLoadingIndicator(m_personName, false);
        }
        return;
    case OperationKind::SearchTaskChat: {
        const auto query = m_taskMessageSearch.takeFailed(ids.value(0));
        if (query.isEmpty())
            return;
        if (m_card && ids.value(0) == m_card->taskId() &&
            !m_cardSearchQuery.isEmpty()) {
            if (query != m_cardSearchQuery)
                m_taskMessageSearch.request(m_card->taskId(), m_cardSearchQuery);
            else if (query == m_card->searchQuery())
                m_card->setChatStatus(reason);
        }
        return;
    }
    case OperationKind::UploadChat:
    case OperationKind::UploadTaskChat:
    case OperationKind::AttachTask: {
        if (operation != m_pendingUpload)
            return;
        const auto scope = ids.value(0);
        const bool task = kind != OperationKind::UploadChat;
        m_pendingUpload.clear();
        m_pendingPastedChatImage = {};
        const auto name = m_uploadName;
        m_uploadName.clear();
        if (m_taskForm && m_taskForm->createdTask() == scope && kind == OperationKind::AttachTask) {
            m_taskForm->setStatus(QStringLiteral("Задача №%1 создана. Не удалось загрузить файл «%2». %3")
                                  .arg(scope, name, reason));
            m_chatPanel->updateComposers();
            updateWriteAvailability();
            return;
        }
        bool reconcileTask = false;
        if (m_descriptionUploadTask == scope && kind == OperationKind::AttachTask) {
            reconcileTask = m_descriptionUploadConfirmed;
            if (m_card && m_descriptionUploadCard == m_card)
                m_card->descriptionEditor()->setUploadStatus(reason);
            clearDescriptionUpload();
        }
        if (task && m_card && m_card->taskId() == scope) {
            m_card->setStatus(reason);
            if (reconcileTask)
                m_backend->requestTask(scope);
        }
        break;
    }
    case OperationKind::CreateTask:
        if (m_pendingCreateTask.isEmpty() || ids.value(0) != m_pendingCreateTask)
            break;
        m_pendingCreateTask.clear();
        if (m_taskForm)
            m_taskForm->setStatus(reason);
        break;
    case OperationKind::UpdateTask:
        if (m_pendingTaskEdit.isEmpty() || ids.value(0) != m_pendingTaskEdit)
            break;
        if (m_card && m_card->taskId() == m_pendingTaskEdit) {
            m_card->setStatus(networkFailure
                ? QStringLiteral("Не удалось подтвердить сохранение задачи. Проверьте изменения на портале перед повтором")
                : reason);
            statusPresented = networkFailure;
        }
        m_pendingTaskEdit.clear();
        m_pendingTaskEditCard.clear();
        m_pendingTaskEditProject.clear();
        m_pendingTaskEditTargetProject.clear();
        break;
    case OperationKind::DeleteTask:
        if (m_pendingTaskDelete.isEmpty() || ids.value(0) != m_pendingTaskDelete)
            break;
        if (m_card && m_card == m_pendingTaskDeleteCard)
            m_card->setStatus(networkFailure
                ? QStringLiteral("Не удалось подтвердить удаление задачи. Проверьте её на портале перед повтором")
                : reason);
        m_pendingTaskDelete.clear();
        m_pendingTaskDeleteCard.clear();
        m_pendingTaskDeleteProject.clear();
        updateWriteAvailability();
        return;
    case OperationKind::SendTask: {
        const auto id = ids.value(0);
        if (m_taskSendController.fail(id, reason, networkFailure))
            statusPresented = networkFailure || !m_card || m_card->taskId() != id;
        break;
    }
    case OperationKind::Task:
    case OperationKind::TaskMessages: {
        if (!m_card || ids.value(0) != m_card->taskId())
            break;
        const bool detailsFailed = kind == OperationKind::Task;
        if (detailsFailed)
            m_cardSummaryPending = false;
        const auto retryCard = detailsFailed
            ? QStringLiteral("Не удалось загрузить задачу. Откройте карточку заново")
            : QStringLiteral("Не удалось загрузить чат задачи. Откройте карточку заново");
        m_card->setStatus(networkFailure ? retryCard : reason);
        statusPresented = networkFailure;
        if (m_cardSearchQuery.isEmpty() && (!detailsFailed || m_loadCardChat)
            && (!networkFailure || m_card->messageList()->count() == 0))
            m_card->setChatStatus(QStringLiteral("Не удалось загрузить чат задачи"));
        break;
    }
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

void MainWindow::connectWorkspaceActions()
{
    connect(m_backend, &Backend::taskChecklistLoaded, this, [this](const QString &id, const auto &items) {
        if (m_card && m_card->taskId() == id)
            m_card->relations()->setChecklist(items);
    });
    connect(m_backend, &Backend::taskChecklistChanged, this, [this](const QString &id) {
        if (m_card && m_card->taskId() == id) {
            m_card->relations()->confirmChecklistChange();
            m_card->setStatus(QString());
            m_backend->requestTaskChecklist(id);
        }
    });
    connect(m_backend, &Backend::subtasksLoaded, this, [this](const QString &id, const auto &tasks) {
        if (m_card && m_card->taskId() == id)
            m_card->relations()->setSubtasks(tasks);
    });
    connect(m_backend, &Backend::taskDependenciesLoaded, this, [this](const QString &id, const auto &dependencies) {
        if (m_card && m_card->taskId() == id)
            m_card->relations()->setDependencies(dependencies);
    });
    connect(m_backend, &Backend::taskHistoryLoaded, this, [this](const QString &id, const auto &history, bool limited) {
        if (m_card && m_card->taskId() == id)
            m_card->relations()->setHistory(history, limited);
    });
    connect(m_backend, &Backend::subtaskCreated, this, [this](const QString &parent, const QString &project, const QString &task) {
        if (!task.isEmpty() && !m_pendingCreateSubtask.isEmpty() && parent == m_pendingCreateSubtask) {
            m_pendingCreateSubtask.clear();
            beginTaskFormFiles(task);
        }
        if (m_card && m_card->taskId() == parent) {
            m_card->setStatus(QStringLiteral("Подзадача создана"));
            m_backend->requestSubtasks(parent);
        }
        if (!project.isEmpty())
            m_projectsPanel->refreshTasks(project);
        updateWriteAvailability();
    });
    connect(m_backend, &Backend::taskLinkPreviewInvalidated, m_messagePreviews.get(), &WorkspaceMessagePreviews::invalidateTaskLinkPreview);
    connect(m_backend, &Backend::taskUpdated, m_messagePreviews.get(), &WorkspaceMessagePreviews::invalidateTaskLinkPreview);
    connect(m_backend, &Backend::taskUpdated, this, &MainWindow::onTaskUpdated);
    connect(m_backend, &Backend::taskDeleted, this, &MainWindow::onTaskDeleted);
    connect(m_backend, &Backend::taskCreated, this, &MainWindow::onTaskCreated);
    connect(m_backend, &Backend::messagesFound, this, &MainWindow::onMessagesFound);
    connect(m_backend, &Backend::fileUploaded, this, &MainWindow::onFileUploaded);
    connect(m_backend, &Backend::taskFileAttached, this, [this](const QString &taskId, const QString &fileId) {
        if (taskId != m_descriptionUploadTask || m_pendingUpload != operationKey(OperationKind::AttachTask, taskId))
            return;
        if (m_card && m_descriptionUploadCard == m_card) {
            m_card->descriptionEditor()->insertAttachment(fileId, m_uploadName, m_descriptionUploadSize);
            m_descriptionUploadLinked = true;
        }
    });
    updateWriteAvailability();
}

void MainWindow::updateWriteAvailability()
{
    m_events->updateNotificationActions();
    m_projectsPanel->setWriteAvailability(writable(), !m_pendingCreateTask.isEmpty());
    if (m_card) {
        const bool deleting = !m_pendingTaskDelete.isEmpty();
        m_card->setWriteState(writable() && m_cardDetailsReady && !deleting, !m_pendingTaskEdit.isEmpty() || deleting,
                              m_projectsPanel->boardEditPending(),
                              m_taskSendController.pending() || pastedChatImagePending(m_card->taskId(), true),
                              !m_pendingUpload.isEmpty());
    }
    if (m_taskForm) {
        const auto task = m_taskForm->createdTask();
        const bool canWrite = writable() && m_pendingCreateTask.isEmpty() && m_pendingCreateSubtask.isEmpty()
            && (task.isEmpty() || m_pendingUpload.isEmpty());
        m_taskForm->setEnabledForWrite(canWrite, !task.isEmpty()
            && m_pendingUpload == operationKey(OperationKind::AttachTask, task));
    }
}

QString MainWindow::taskLinkFor(const QString &taskId) const
{
    return ui::taskLink(m_backend->portalOrigin(), m_projectsPanel->currentProjectId(), taskId);
}

void MainWindow::openTaskCard(const QString &taskId)
{
    openTaskCardForProject(taskId, m_lastSection == 1 ? m_projectsPanel->currentProjectId() : QString());
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

void MainWindow::onTaskSummaryLoaded(const Task &task)
{
    if (!m_card || !m_cardSummaryPending || m_cardDetailsReady || task.id != m_card->taskId())
        return;
    m_cardSummaryPending = false;
    auto summary = task;
    summary.editRights = {};
    summary.statusActions.clear();
    const auto previousDialog = m_card->taskChatDialogId();
    m_card->setTask(summary, false);
    m_card->setStatus({});
    if (!summary.projectId.isEmpty())
        m_card->setLink(ui::taskLink(m_backend->portalOrigin(), summary.projectId, summary.id));
    updateTaskCardChatBinding(previousDialog);
    scheduleAvatars();
    const auto &dialog = summary.chatDialogId;
    if (m_loadCardChat && dialog.startsWith(QLatin1String("chat"))
        && !portalTaskIdentifier(dialog.mid(4)).isEmpty()) {
        m_loadCardChat = false;
        m_backend->requestTaskMessages(summary.id);
    }
}

void MainWindow::updateTaskCardChatBinding(const QString &previousDialog)
{
    if (!m_card || m_card->taskChatDialogId() == previousDialog)
        return;
    const auto dialog = m_card->taskChatDialogId();
    m_card->composer()->dismissMentionSearch();
    m_card->setPinnedMessages({});
    m_cardContextMessageId.clear();
    m_card->setContextMode(false);
    if (!dialog.isEmpty())
        m_backend->requestPinnedMessages(dialog);
}

void MainWindow::openTaskCardForProject(const QString &taskId, const QString &projectId)
{
    if (taskId.isEmpty())
        return;
    const bool knownProject = !projectId.isEmpty() && projectId == m_projectsPanel->currentProjectId();
    if (m_card) {
        if (m_card->taskId() == taskId && m_card->projectId() == projectId) {
            m_card->activateWindow();
            return;
        }
        if (!clearTask())
            return;
    }

    const Task task = m_projectsPanel->cachedTask(taskId, projectId);
    auto *card = new ui::TaskCardDialog(task, projectId, this, false);
    card->setRequisiteAvatars([this](const QUrl &url) { return m_avatars.object(url); });
    card->setOnRequisiteAvatarChoicesChanged([this] { scheduleAvatars(); });
    m_messagePreviews->resetLinkPreviews();
    bindMessageFiles(card->messageList(), true);
    static_cast<workspace::RowPainter *>(card->messageList()->itemDelegate())->setMessageViewersHandler(
        [this, card](const QString &id, const QPoint &anchor) { openTaskMessageViewers(card->messageList(), id, anchor); });
    auto *history = static_cast<ui::HistoryList *>(card->messageList());
    history->setOnViewportChanged([this] { m_chatPanel->scheduleVisibleChatRead(); });
    history->setOnOlder([this, card](const QString &before) {
        if (m_card == card && m_cardSearchQuery.isEmpty())
            m_backend->requestOlderChatMessages(card->taskId(), before, true);
    });
    history->setReplyActions([card](const ui::ChatReply &reply) {
        card->composer()->setReply(reply);
        card->messageInput()->setFocus(Qt::OtherFocusReason);
    }, [this, card] { return m_card == card && m_cardDetailsReady && writable(); },
       [this](const QString &id) { openPinnedTaskMessage(id); });
    card->composer()->setTypingActions([this, card] {
        return m_card == card && m_cardDetailsReady && writable() && card->isActiveWindow()
            ? card->taskChatDialogId() : QString();
    }, [this](const QString &dialog) { m_backend->notifyChatTyping(dialog); });
    card->composer()->setEditLatest([this, card, history] {
        return m_card == card && m_cardDetailsReady && m_chatPanel->editLatestMessage(history, card->taskChatDialogId());
    });
    history->setMessageMenu([this, card](QMenu &menu, const QPersistentModelIndex &index, const QString &href) {
        if (m_card == card && m_cardDetailsReady)
            m_chatPanel->addMessageActions(menu, index, card->taskChatDialogId(), true, href);
    });
    history->setReactionActions([this, card](const QModelIndex &index, const QString &code, const QPoint &anchor) {
        if (m_card == card && m_cardDetailsReady)
            m_chatPanel->actOnMessageReaction(index, card->taskChatDialogId(), code, anchor, card);
    }, [this, card] {
        return m_card == card && m_cardDetailsReady && writable() && !m_backend->isDemo() && !card->taskChatDialogId().isEmpty();
    });
    bindMessageFiles(card->descriptionList());
    bindMessageFiles(card->attachmentList());
    card->descriptionList()->setOnLayout([this] { scheduleAvatars(); });
    card->attachmentList()->setOnLayout([this] { scheduleAvatars(); });
    connect(card->detailsScrollBar(), &QScrollBar::valueChanged, this, &MainWindow::scheduleAvatars);
    connect(card->messageList()->verticalScrollBar(), &QScrollBar::valueChanged, this, &MainWindow::scheduleAvatars);
    card->setAttribute(Qt::WA_DeleteOnClose);
    m_card = card;
    card->composer()->setMentionSearchHandler([this, card](const QString &id, const QString &query) {
        const auto scope = card->taskChatDialogId();
        if (!scope.isEmpty())
            m_backend->searchPeople(id, query, scope);
        else
            card->composer()->setMentionResults(id, {});
    });
    card->composer()->setMentionAvatars([this](const Person &person) {
        return ui::requisiteAvatarIcon([this](const QUrl &url) { return m_avatars.object(url); },
                                       person.name, person.avatarUrl, workspace::AvatarKind::Person);
    }, [this] { scheduleAvatars(); });
    card->setPeopleSearch([this](const QString &id, const QString &query) {
        m_backend->searchPeople(id, query, {});
    });
    card->setProjectChoices(m_projectsPanel->projectDirectory());
    card->setCanDetachProject(!m_backend->isDemo());
    card->setMoreProjectsAvailable(m_projectsPanel->moreAvailable());
    card->setOnMoreProjects([this] { m_projectsPanel->requestNextProjectsPage(); });
    m_loadCardChat = true;
    m_cardDetailsReady = false;
    m_cardSummaryPending = true;
    m_cardRequestedProjectDirectory = false;
    m_cardRequestedStages.clear();
    m_cardSearchQuery.clear();
    m_cardContextMessageId.clear();
    switchDraft(card->composer(), m_taskDrafts, {}, taskId);
    if (knownProject && card->projectId() == m_projectsPanel->currentProjectId()) {
        card->setStages(m_projectsPanel->stages());
        card->setProjectCaption(m_projectsPanel->currentProjectTitle());
    }
    card->setLink(ui::taskLink(m_backend->portalOrigin(), projectId, taskId));
    card->setOnCopy([](const QString &text) {
        QGuiApplication::clipboard()->setText(text);
    });
    const auto saveTaskEdit = [this, card](const TaskEdit &edit) {
        if (m_card != card || !m_cardDetailsReady || !writable()
            || !m_pendingTaskEdit.isEmpty() || !m_pendingTaskDelete.isEmpty())
            return;
        if (!edit.hasChanges()) {
            card->setStatus(QStringLiteral("Нет изменений"));
            return;
        }
        m_pendingTaskEdit = card->taskId();
        m_pendingTaskEditCard = card;
        m_pendingTaskEditProject = card->projectId();
        m_pendingTaskEditTargetProject = edit.projectId.value_or(QString());
        card->setStatus(QStringLiteral("Сохраняем…"), true);
        updateWriteAvailability();
        m_backend->updateTask(card->taskId(), edit);
    };
    card->setOnSave(saveTaskEdit);
    card->setOnDelete([this, card] { confirmTaskDeletion(card); });
    card->setOnMove([saveTaskEdit](const QString &stage) {
        TaskEdit edit;
        edit.stageId = stage;
        saveTaskEdit(edit);
    });
    card->setOnSend([this, card] {
        sendFromComposer(m_taskSendController, card->composer(), card->taskId());
    });
    card->setOnSearch([this, card](const QString &query) {
        if (query.isEmpty()) {
            if (m_cardSearchQuery.isEmpty())
                return;
            m_cardSearchQuery.clear();
            card->setChatStatus(QStringLiteral("Загружаем сообщения…"), true);
            m_backend->requestTaskMessages(card->taskId());
        } else {
            m_cardContextMessageId.clear();
            card->setContextMode(false);
            m_cardSearchQuery = query;
            card->setChatStatus(QStringLiteral("Ищем сообщения…"), true);
            m_taskMessageSearch.request(card->taskId(), query);
        }
    });
    card->relations()->onAdd = [this, card](const QString &group, const QString &title) {
        m_backend->addTaskChecklistItem(card->taskId(), group, title);
    };
    card->relations()->onToggle = [this, card](const QString &item, bool complete) {
        m_backend->setTaskChecklistComplete(card->taskId(), item, complete);
    };
    card->relations()->onOpenChild = [this](const QString &id) {
        openTaskCardForProject(id, {});
    };
    card->relations()->onCreateChild = [this, card] { createSubtask(card); };
    card->relations()->onHistory = [this, card] { m_backend->requestTaskHistory(card->taskId()); };
    card->setOnPinned([this](const QString &id) { openPinnedTaskMessage(id); });
    card->setOnLatest([this, card] {
        if (m_card != card)
            return;
        m_cardContextMessageId.clear();
        card->setContextMode(false);
        m_backend->requestTaskMessages(card->taskId());
    });
    card->setOnFile([this, card](bool attachment) {
        pickFile(card->taskId(), attachment ? QStringLiteral("task") : QStringLiteral("task-chat"));
    });
    card->descriptionEditor()->setAttachHandler([this, taskId](const QStringList &paths) {
        queueDescriptionFiles(taskId, paths);
    });
    card->descriptionEditor()->setImagePasteHandler([this, taskId](const QImage &image) {
        pasteDescriptionPhoto(taskId, image);
    });
    connect(card, &QDialog::finished, this, [this, card] {
        if (m_card != card)
            return;
        m_projectsPanel->cancelPendingMessageProject();
        switchDraft(card->composer(), m_taskDrafts, card->taskId(), {});
        m_card = nullptr;
        m_messagePreviews->resetLinkPreviews();
        emit historyClosed(true);
        m_cardSearchQuery.clear();
        m_cardContextMessageId.clear();
        m_loadCardChat = false;
        m_cardDetailsReady = false;
        m_cardSummaryPending = false;
    });
    updateWriteAvailability();
    if (writable())
        card->messageInput()->setFocus(Qt::OtherFocusReason);
    card->setStatus(QStringLiteral("Загружаем задачу…"), true);
    card->open();
    scheduleAvatars();
    if (!card->taskChatDialogId().isEmpty())
        m_backend->requestPinnedMessages(card->taskChatDialogId());
    m_backend->requestTask(taskId);
    m_backend->requestTaskChecklist(taskId);
    m_backend->requestSubtasks(taskId);
    m_backend->requestTaskDependencies(taskId);
}

void MainWindow::confirmTaskDeletion(ui::TaskCardDialog *card)
{
    if (!card || m_card != card || !writable() || !card->canDeleteTask()
        || !m_pendingTaskEdit.isEmpty() || !m_pendingTaskDelete.isEmpty())
        return;
    const auto taskId = portalTaskIdentifier(card->taskId());
    if (taskId.isEmpty())
        return;
    auto *confirmation = new QMessageBox(QMessageBox::Warning, QStringLiteral("Удаление задачи"),
        QStringLiteral("Удалить задачу №%1?").arg(taskId), QMessageBox::Yes | QMessageBox::Cancel, card);
    confirmation->setObjectName(QStringLiteral("taskDeleteConfirmation"));
    confirmation->setAttribute(Qt::WA_DeleteOnClose);
    confirmation->setDefaultButton(QMessageBox::Cancel);
    confirmation->setEscapeButton(QMessageBox::Cancel);
    confirmation->button(QMessageBox::Yes)->setText(QStringLiteral("Удалить"));
    const auto scope = m_backend->settingsScope();
    connect(confirmation, &QMessageBox::finished, this,
            [this, target = QPointer<ui::TaskCardDialog>(card), taskId, scope](int result) {
        if (result != QMessageBox::Yes || !target || m_card != target || target->taskId() != taskId
            || scope != m_backend->settingsScope() || !writable() || !target->canDeleteTask()
            || !m_pendingTaskEdit.isEmpty() || !m_pendingTaskDelete.isEmpty())
            return;
        m_pendingTaskDelete = taskId;
        m_pendingTaskDeleteCard = target;
        m_pendingTaskDeleteProject = target->projectId();
        target->setStatus(QStringLiteral("Удаляем задачу…"), true);
        updateWriteAvailability();
        m_backend->deleteTask(taskId);
    });
    confirmation->open();
}

void MainWindow::onTaskDeleted(const QString &taskId)
{
    if (m_pendingTaskDelete.isEmpty() || m_pendingTaskDelete != taskId)
        return;
    m_pendingTaskDelete.clear();
    const auto card = std::exchange(m_pendingTaskDeleteCard, {});
    const auto project = std::exchange(m_pendingTaskDeleteProject, {});
    m_messagePreviews->invalidateTaskLinkPreview(taskId);
    updateWriteAvailability();
    if (card && card == m_card && card->taskId() == taskId)
        card->close();
    if (!project.isEmpty())
        m_projectsPanel->refreshTasks(project);
    setStatus(QStringLiteral("Задача удалена"));
}

void MainWindow::onTaskUpdated(const QString &taskId)
{
    const bool matches = !m_pendingTaskEdit.isEmpty() && taskId == m_pendingTaskEdit;
    const auto editorCard = m_pendingTaskEditCard;
    const auto project = matches ? m_pendingTaskEditProject : QString();
    const auto targetProject = matches ? m_pendingTaskEditTargetProject : QString();
    if (matches) {
        m_pendingTaskEdit.clear();
        m_pendingTaskEditCard.clear();
        m_pendingTaskEditProject.clear();
        m_pendingTaskEditTargetProject.clear();
    }
    if (m_card && m_card->taskId() == taskId) {
        if (matches && editorCard == m_card) {
            m_cardDetailsReady = false;
            m_card->acceptEdit();
        }
        m_card->setStatus(QStringLiteral("Задача обновлена"));
        m_backend->requestTask(taskId);
    }
    if (!m_projectsPanel->currentProjectId().isEmpty())
        m_projectsPanel->refreshTasks(project, targetProject);
    updateWriteAvailability();
}

void MainWindow::onTaskCreated(const QString &projectId, const QString &taskId)
{
    if (taskId.isEmpty())
        return;
    const bool requested = !m_pendingCreateTask.isEmpty() && projectId == m_pendingCreateTask;
    bool attaching = false;
    if (requested) {
        m_pendingCreateTask.clear();
        attaching = beginTaskFormFiles(taskId, projectId);
    }
    m_projectsPanel->refreshTasks(projectId);
    if (requested && !attaching && m_lastSection == 1)
        openTaskCardForProject(taskId, projectId);
    setStatus(QStringLiteral("Задача создана"));
    updateWriteAvailability();
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

void MainWindow::onMessagesFound(const QString &scopeId, const QString &query, bool task,
                                 const QVector<Message> &messages)
{
    if (!task || !m_taskMessageSearch.accept(scopeId, query) || !m_card ||
        m_card->taskId() != scopeId || m_cardSearchQuery.isEmpty())
        return;
    if (query != m_cardSearchQuery) {
        m_taskMessageSearch.request(scopeId, m_cardSearchQuery);
    } else if (query == m_card->searchQuery()) {
        m_card->showMessages(messages, ui::MessageView::Search);
        updateAvatarRows();
        scheduleAvatars();
    }
}

void MainWindow::onFileUploaded(const QString &scope, const QString &kind)
{
    const auto operation = operationKey(kind == QStringLiteral("task") ? OperationKind::AttachTask
        : kind == QStringLiteral("task-chat") ? OperationKind::UploadTaskChat : OperationKind::UploadChat, scope);
    if (m_pendingUpload != operation)
        return;
    const auto sent = std::exchange(m_pendingPastedChatImage, {});
    if (sent.image && kind == QStringLiteral("task-chat"))
        clearSentDraft(m_card ? m_card->composer() : nullptr, m_taskDrafts,
                       m_card ? m_card->taskId() : QString{}, scope, sent);
    if (kind == QStringLiteral("task") && m_taskForm && m_taskForm->createdTask() == scope) {
        m_pendingUpload.clear();
        m_uploadName.clear();
        m_taskForm->confirmFirstFile();
        if (!m_taskForm->queuedFiles().isEmpty()) {
            if (!uploadNextTaskFormFile())
                m_taskForm->setStatus(QStringLiteral("Задача №%1 создана. Повторите загрузку оставшихся файлов.").arg(scope));
        } else {
            const auto project = m_taskForm->projectToOpen();
            m_taskForm->accept();
            setStatus(QStringLiteral("Файлы прикреплены к задаче №%1").arg(scope));
            if (!project.isEmpty() && m_lastSection == 1)
                openTaskCardForProject(scope, project);
        }
        m_chatPanel->updateComposers();
        updateWriteAvailability();
        return;
    }
    const bool descriptionUpload = kind == QStringLiteral("task") && scope == m_descriptionUploadTask;
    if (descriptionUpload) {
        m_descriptionUploadConfirmed = true;
        m_descriptionPaste.reset();
        if (m_card && m_descriptionUploadCard == m_card)
            m_card->descriptionEditor()->setUploadStatus(m_descriptionUploadLinked
                ? QStringLiteral("Файл вставлен в описание")
                : QStringLiteral("Файл прикреплён к задаче. Ссылка для вставки недоступна."));
    }
    m_pendingUpload.clear();
    const auto status = QStringLiteral("Файл загружен: %1").arg(m_uploadName);
    m_uploadName.clear();
    if (descriptionUpload && uploadNextDescriptionFile())
        return;
    if (kind == QStringLiteral("chat")) {
        m_chatPanel->fileUploaded(scope, status, sent);
    } else if (m_card && m_card->taskId() == scope) {
        m_card->setStatus(status);
        if (kind == QStringLiteral("task"))
            m_backend->requestTask(scope);
        else if (m_cardSearchQuery.isEmpty())
            m_backend->requestTaskMessages(scope);
    } else {
        setStatus(status);
    }
    m_chatPanel->updateComposers();
    updateWriteAvailability();
}

void MainWindow::scheduleAvatars()
{
    if (m_avatarUpdateQueued)
        return;
    m_avatarUpdateQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_avatarUpdateQueued = false;
        loadVisibleAvatars();
        m_messagePreviews->loadVisibleChatPreviews();
    });
}

void MainWindow::requestAvatar(const QUrl &url)
{
    if (m_requestedAvatars.size() >= 4 || !url.isValid() || url.scheme() != QStringLiteral("https") ||
        url.host().isEmpty() || !url.userInfo().isEmpty() || m_avatars.contains(url) || m_requestedAvatars.contains(url))
        return;
    m_requestedAvatars.insert(url);
    m_backend->requestAvatar(url);
}

void MainWindow::loadVisibleAvatars()
{
    const auto requestVisible = [this](QListWidget *list) {
        if (!list || !list->isVisible())
            return;
        const auto first = list->indexAt(QPoint(4, 3));
        for (int row = first.isValid() ? first.row() : 0; row < list->count(); ++row) {
            const auto *item = list->item(row);
            const auto rect = list->visualItemRect(item);
            if (rect.top() >= list->viewport()->height())
                break;
            if (rect.bottom() >= 0)
                requestAvatar(item->data(workspace::AvatarUrl).toUrl());
        }
    };
    const auto *composer = m_card && m_card->isVisible() ? m_card->composer() : m_chatPanel->composer();
    for (const auto &url : composer->visibleMentionAvatarUrls())
        requestAvatar(url);
    if (m_card && m_card->isVisible()) {
        for (const auto &url : m_card->visibleRequisiteAvatarUrls())
            requestAvatar(url);
    }
    if (m_taskForm && m_taskForm->isVisible()) {
        for (const auto &url : m_taskForm->visibleRequisiteAvatarUrls())
            requestAvatar(url);
    }
    if (m_lastSection == 0) {
        m_chatPanel->requestVisibleAvatars([this](const QUrl &url) { requestAvatar(url); }, requestVisible);
    } else if (m_lastSection == 1) {
        m_projectsPanel->requestVisibleAvatars([this](const QUrl &url) { requestAvatar(url); }, requestVisible);
    }
    if (m_card)
        requestVisible(m_card->messageList());
    if (m_personDialog)
        requestAvatar(m_personAvatarUrl);
}

void MainWindow::updateAvatarRows()
{
    const auto updateList = [this](QListWidget *list) {
        for (int i = 0; i < list->count(); ++i) {
            auto *item = list->item(i);
            const auto url = item->data(workspace::AvatarUrl).toUrl();
            if (!url.isEmpty()) {
                const auto *photo = m_avatars.object(url);
                item->setData(workspace::AvatarImage, photo ? QVariant::fromValue(*photo) : QVariant{});
            }
        }
        if (list == m_chatPanel->history() || (m_card && list == m_card->messageList()))
            list->viewport()->update();
    };
    m_chatPanel->updateAvatars(updateList);
    m_projectsPanel->updateAvatars(updateList);
    if (m_card)
        updateList(m_card->messageList());
    if (m_personDialog) {
        if (const auto *photo = m_avatars.object(m_personAvatarUrl))
            m_personAvatar->setPhoto(*photo);
    }
    m_events->repaintRows();
}

void MainWindow::openPortalTarget(NotificationTarget target, QWidget *source, const QUrl &messageUrl)
{
    if (target.id.isEmpty())
        return;
    m_projectsPanel->cancelPendingMessageProject();
    switch (target.kind) {
    case NotificationTargetKind::Chat:
        if (m_card && m_card->taskChatDialogId() == target.id) {
            m_card->activateWindow();
            if (!target.messageId.isEmpty())
                openPinnedTaskMessage(target.messageId);
        } else if (clearTask()) {
            m_chatPanel->openLinkedChat(target);
        }
        break;
    case NotificationTargetKind::Task:
        openTaskCardForProject(target.id, target.projectId);
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

void MainWindow::openPinnedTaskMessage(const QString &messageId)
{
    if (!m_card)
        return;
    const auto dialog = m_card->taskChatDialogId();
    const auto target = portalTaskIdentifier(messageId);
    if (dialog.isEmpty() || target.isEmpty())
        return;
    const bool searching = !m_cardSearchQuery.isEmpty();
    m_cardSearchQuery.clear();
    if (!searching && m_card->focusMessage(target))
        return;
    m_cardContextMessageId = target;
    m_card->setContextMode(true);
    m_card->setChatStatus(QStringLiteral("Загружаем сообщение…"), true);
    m_backend->requestMessageContext(dialog, target);
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

void MainWindow::openTaskMessageViewers(QListWidget *history, const QString &messageId, const QPoint &anchor)
{
    if (!m_card || history != m_card->messageList())
        return;
    const auto scope = m_card->taskId();
    const auto sameHistory = [window = QPointer<MainWindow>(this), source = QPointer<QListWidget>(history), scope] {
        return window && source && source->isVisible() && window->m_card
            && source == window->m_card->messageList() && scope == window->m_card->taskId();
    };
    workspace::openMessageViewers(m_backend, history, messageId, anchor, scope, true, sameHistory, {
        [this](const QUrl &url) { return m_avatars.object(url); },
        [this](const QUrl &url) { requestAvatar(url); }
    });
}

void MainWindow::bindMessageFiles(QListWidget *list, bool linkCards)
{
    auto *delegate = static_cast<workspace::RowPainter *>(list->itemDelegate());
    m_messageFiles->bindMessageFiles(list);
    delegate->setMessageLinkHandler([this, list](const QString &href) { openMessageLink(href, list); });
    if (linkCards)
        m_messagePreviews->bindLinkCards(list, delegate);
}

void MainWindow::uploadPastedChatImage(ui::ChatComposer *composer, const QString &scope, bool task, const QString &caption)
{
    if (!m_pendingUpload.isEmpty())
        return;
    const auto draft = composer->draft();
    if (!draft.image)
        return;
    const auto feedback = [this, task, &scope](const QString &status, bool loading = false) {
        if (task && m_card && m_card->taskId() == scope)
            m_card->setStatus(status, loading);
        else
            setStatus(status, loading);
    };
    if (!draft.reply.id.isEmpty()) {
        feedback(QStringLiteral("Чтобы отправить изображение, отмените ответ или удалите изображение из черновика"));
        return;
    }
    m_pendingUpload = operationKey(task ? OperationKind::UploadTaskChat : OperationKind::UploadChat, scope);
    m_pendingPastedChatImage = draft;
    m_uploadName = QFileInfo(draft.image->path).fileName();
    feedback(QStringLiteral("Отправляем изображение…"), true);
    m_chatPanel->updateComposers();
    updateWriteAvailability();
    m_backend->uploadChatImage(scope, draft.image->path, task, caption, draft.image->directory);
}

bool MainWindow::pastedChatImagePending(const QString &scope, bool task) const
{
    return m_pendingPastedChatImage.image && m_pendingUpload ==
        operationKey(task ? OperationKind::UploadTaskChat : OperationKind::UploadChat, scope);
}

void MainWindow::pickFile(const QString &scope, const QString &kind)
{
    if (!writable() || !m_pendingUpload.isEmpty() || scope.isEmpty())
        return;
    const bool attachment = kind == QStringLiteral("task");
    const bool taskChat = kind == QStringLiteral("task-chat");
    QWidget *parent = this;
    if (m_card && (attachment || taskChat))
        parent = m_card;
    const auto path = QFileDialog::getOpenFileName(parent, QStringLiteral("Выберите файл"));
    if (path.isEmpty() || !writable())
        return;
    m_pendingUpload = operationKey(attachment ? OperationKind::AttachTask
        : taskChat ? OperationKind::UploadTaskChat : OperationKind::UploadChat, scope);
    m_uploadName = QFileInfo(path).fileName();
    const auto status = QStringLiteral("Загружаем %1…").arg(m_uploadName);
    if (m_card && (attachment || taskChat) && m_card->taskId() == scope)
        m_card->setStatus(status, true);
    else
        setStatus(status, true);
    m_chatPanel->updateComposers();
    updateWriteAvailability();
    if (attachment)
        m_backend->attachTaskFile(scope, path);
    else
        m_backend->uploadChatFile(scope, path, taskChat);
}

void MainWindow::queueDescriptionFiles(const QString &taskId, const QStringList &paths)
{
    if (!writable() || !m_card || m_card->taskId() != taskId || paths.isEmpty() ||
        !m_pendingUpload.isEmpty() || !m_descriptionUploadTask.isEmpty())
        return;
    m_descriptionUploadTask = taskId;
    m_descriptionUploadCard = m_card;
    m_descriptionUploadPaths = paths;
    uploadNextDescriptionFile();
}

bool MainWindow::uploadNextDescriptionFile()
{
    if (!writable() || !m_card || m_descriptionUploadCard != m_card ||
        m_card->taskId() != m_descriptionUploadTask ||
        m_descriptionUploadPaths.isEmpty()) {
        clearDescriptionUpload();
        return false;
    }
    const auto taskId = m_descriptionUploadTask;
    const auto path = m_descriptionUploadPaths.takeFirst();
    m_pendingUpload = operationKey(OperationKind::AttachTask, taskId);
    m_uploadName = QFileInfo(path).fileName();
    m_descriptionUploadLinked = false;
    m_descriptionUploadSize = QImageReader(path).size();
    const auto status = QStringLiteral("Загружаем %1…").arg(m_uploadName);
    m_card->descriptionEditor()->setUploadStatus(status, true);
    m_chatPanel->updateComposers();
    updateWriteAvailability();
    m_backend->attachTaskFile(taskId, path);
    return true;
}

void MainWindow::clearDescriptionUpload()
{
    m_descriptionUploadTask.clear();
    m_descriptionUploadCard.clear();
    m_descriptionUploadPaths.clear();
    m_descriptionUploadSize = {};
    m_descriptionUploadLinked = false;
    m_descriptionUploadConfirmed = false;
    m_descriptionPaste.reset();
}

void MainWindow::pasteDescriptionPhoto(const QString &taskId, const QImage &image)
{
    if (!writable() || !m_card || m_card->taskId() != taskId || !m_pendingUpload.isEmpty() ||
        !m_descriptionUploadTask.isEmpty())
        return;
    if (image.isNull() || qint64(image.width()) * image.height() > 12 * 1024 * 1024) {
        m_card->descriptionEditor()->setUploadStatus(QStringLiteral("Изображение слишком большое для вставки"));
        return;
    }
    auto directory = std::make_unique<QTemporaryDir>(QDir::temp().filePath(QStringLiteral("bitrix24-task-XXXXXX")));
    const auto path = directory->filePath(QStringLiteral("Из буфера.png"));
    if (!directory->isValid() || !image.save(path, "PNG")) {
        m_card->descriptionEditor()->setUploadStatus(QStringLiteral("Не удалось подготовить изображение для загрузки"));
        return;
    }
    m_descriptionPaste = std::move(directory);
    queueDescriptionFiles(taskId, {path});
}

void MainWindow::bindTaskForm(ui::TaskEditForm *form, const QString &projectId, bool canDetachProject)
{
    form->setAttribute(Qt::WA_DeleteOnClose);
    m_taskForm = form;
    connect(form, &QDialog::finished, this, [this] { m_taskForm = nullptr; });
    form->setPeopleSearch([this](const QString &id, const QString &query) {
        m_backend->searchPeople(id, query, {});
    });
    form->setRequisiteAvatars([this](const QUrl &url) { return m_avatars.object(url); },
                              [this] { scheduleAvatars(); });
    form->setOnMoreProjects([this] { m_projectsPanel->requestNextProjectsPage(); });
    form->setOnStages([this, form](const QString &project) {
        if (project == m_projectsPanel->currentProjectId() && !m_projectsPanel->stages().isEmpty())
            form->setStages(m_projectsPanel->stages());
        else
            m_backend->requestProjectStages(project);
    });
    form->configure(projectId, canDetachProject);
    form->setProjectChoices(m_projectsPanel->projectDirectory());
    form->setMoreProjectsAvailable(m_projectsPanel->moreAvailable());
    m_backend->requestCurrentPerson([window = QPointer<MainWindow>(this), guard = QPointer<ui::TaskEditForm>(form)](Person person) {
        if (window && guard && window->m_taskForm == guard)
            guard->setCurrentPerson(std::move(person));
    });
    if (!m_projectsPanel->directoryLoaded())
        m_backend->requestProjects();
}

void MainWindow::createTaskInProject()
{
    if (!writable() || m_projectsPanel->currentProjectId().isEmpty() || m_taskForm || !m_pendingCreateTask.isEmpty() || !m_pendingCreateSubtask.isEmpty())
        return;
    auto *form = new ui::TaskEditForm(this);
    bindTaskForm(form, m_projectsPanel->currentProjectId(), false);
    form->setOnSave([this, form](const TaskEdit &edit) {
        if (!writable() || !m_pendingCreateTask.isEmpty())
            return;
        if (!form->createdTask().isEmpty()) {
            uploadNextTaskFormFile();
            return;
        }
        const auto project = form->projectId();
        m_pendingCreateTask = project;
        form->setStatus(QStringLiteral("Создаём задачу…"), true);
        updateWriteAvailability();
        m_backend->createTask(project, edit);
    });
    form->open();
}

void MainWindow::createSubtask(ui::TaskCardDialog *card)
{
    if (card != m_card || !writable() || m_taskForm || !m_pendingCreateTask.isEmpty() || !m_pendingCreateSubtask.isEmpty())
        return;
    auto *form = new ui::TaskEditForm(card);
    form->setWindowTitle(QStringLiteral("Новая подзадача"));
    form->setAccessibleName(QStringLiteral("Форма новой подзадачи"));
    const auto parent = card->taskId();
    bindTaskForm(form, card->projectId(), true);
    form->setOnSave([this, form, parent](const TaskEdit &edit) {
        if (!writable() || !m_pendingCreateSubtask.isEmpty())
            return;
        if (!form->createdTask().isEmpty()) {
            uploadNextTaskFormFile();
            return;
        }
        const auto project = form->projectId();
        m_pendingCreateSubtask = parent;
        form->setStatus(QStringLiteral("Создаём подзадачу…"), true);
        updateWriteAvailability();
        m_backend->createSubtask(parent, project, edit);
    });
    form->open();
}

bool MainWindow::beginTaskFormFiles(const QString &taskId, const QString &projectToOpen)
{
    if (!m_taskForm)
        return false;
    if (m_taskForm->queuedFiles().isEmpty()) {
        m_taskForm->accept();
        return false;
    }
    m_taskForm->setCreatedTask(taskId, projectToOpen);
    if (!uploadNextTaskFormFile())
        m_taskForm->setStatus(QStringLiteral("Задача №%1 создана. Повторите загрузку оставшихся файлов.").arg(taskId));
    return true;
}

bool MainWindow::uploadNextTaskFormFile()
{
    if (!writable() || !m_pendingUpload.isEmpty() || !m_taskForm
        || m_taskForm->createdTask().isEmpty() || m_taskForm->queuedFiles().isEmpty())
        return false;
    const auto task = m_taskForm->createdTask();
    const auto path = m_taskForm->queuedFiles().first();
    m_pendingUpload = operationKey(OperationKind::AttachTask, task);
    m_uploadName = QFileInfo(path).fileName();
    m_taskForm->setStatus(QStringLiteral("Загружаем «%1»…").arg(m_uploadName), true);
    m_chatPanel->updateComposers();
    updateWriteAvailability();
    m_backend->attachTaskFile(task, path);
    return true;
}

} // namespace bx
