#include "ui/main_window.h"
#include "ui/message_menu_window.h"
#include "ui/projects_panel.h"
#include "ui/chat_photo_viewer.h"
#include "ui/chat_video_previews.h"
#include "ui/chat_widgets.h"
#include "ui/task_dialogs.h"
#include "ui/reaction_catalog.h"
#include "ui/saved_file_offer.h"
#include "ui/typography.h"
#include "ui/workspace_directory.h"
#include "ui/workspace_presentation.h"
#include "ui/workspace_style.h"
#include "ui/worktime_panel.h"
#include "cloud/cloud_notification_links.h"
#include "core/download_target.h"
#include "core/notification_targets.h"
#include "core/origin_guard.h"
#include "core/operations.h"

#include <QAction>
#include <QTabBar>
#include <QSplitter>
#include <QSettings>
#include <QMenu>
#include <QScreen>
#include <QImageReader>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPromise>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <iterator>
#include <limits>
#include <memory>
#include <utility>
#include <memory>

namespace bx {

namespace {

using workspace::label;
using ui::clockLabel;
using ui::emptyState;
using ui::ListViewportAnchor;
using ui::listViewportAnchor;
using ui::restoreListViewport;
using ui::presentMessages;
using ui::showEmpty;

struct PhotoLoad {
    std::shared_ptr<QTemporaryDir> directory;
    QString target;
    QUrl source;
    QFuture<QImage> decode;
    bool downloading = true;
};

struct PhotoGallery {
    QVector<MessageFile> files;
    qsizetype index = 0;
    std::shared_ptr<PhotoLoad> load;
    QUrl pendingPreview;
};

QStringList photoKeys(const MessageFile &file)
{
    QStringList keys;
    if (!file.diskObjectId.isEmpty())
        keys.append(QStringLiteral("disk:") + file.diskObjectId);
    if (!file.downloadUrl.isEmpty())
        keys.append(QStringLiteral("download:") + file.downloadUrl.toString(QUrl::FullyEncoded));
    if (!file.id.isEmpty())
        keys.append(QStringLiteral("file:") + file.id);
    if (keys.isEmpty() && !file.previewUrl.isEmpty())
        keys.append(QStringLiteral("preview:") + file.previewUrl.toString(QUrl::FullyEncoded));
    return keys;
}

PhotoGallery photoGallery(const MessageFile &opened, const QVector<MessageFile> &loaded)
{
    PhotoGallery gallery;
    QHash<QString, qsizetype> indexes;
    const auto indexOf = [&indexes](const QStringList &keys) {
        for (const auto &key : keys)
            if (const auto it = indexes.constFind(key); it != indexes.cend())
                return it.value();
        return qsizetype(-1);
    };
    for (const auto &file : loaded) {
        if (file.isVideo || (file.downloadUrl.isEmpty() && file.previewUrl.isEmpty())
            || (!file.isImage && file.previewUrl.isEmpty()))
            continue;
        const auto keys = photoKeys(file);
        auto index = indexOf(keys);
        if (index < 0) {
            index = gallery.files.size();
            gallery.files.append(file);
        } else if (gallery.files[index].downloadUrl.isEmpty() && !file.downloadUrl.isEmpty()) {
            gallery.files[index] = file;
        }
        for (const auto &key : keys)
            if (!indexes.contains(key))
                indexes.insert(key, index);
    }
    gallery.index = indexOf(photoKeys(opened));
    if (gallery.index < 0) {
        gallery.index = gallery.files.size();
        gallery.files.append(opened);
    }
    return gallery;
}

QImage readMessagePhoto(const QString &path)
{
    constexpr qint64 maxEncodedSize = 24 * 1024 * 1024;
    constexpr qint64 maxPixels = 16 * 1024 * 1024;
    const auto bytes = QFileInfo(path).size();
    if (bytes <= 0 || bytes > maxEncodedSize)
        return {};
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);
    const auto format = reader.format();
    if (format != "jpeg" && format != "png" && format != "webp" && format != "gif")
        return {};
    const auto size = reader.size();
    if (size.isEmpty() || size.width() > 8192 || size.height() > 8192 ||
        qint64(size.width()) * size.height() > maxPixels)
        return {};
    return reader.read();
}

void switchDraft(ui::ChatComposer *composer, QHash<QString, ui::ChatDraft> &drafts,
                 const QString &from, const QString &to)
{
    composer->finishMessageEdit();
    if (!from.isEmpty()) {
        const auto draft = composer->draft();
        if (draft.isEmpty()) {
            drafts.remove(from);
        } else {
            constexpr int limit = 30;
            if (drafts.size() >= limit && !drafts.contains(from))
                drafts.erase(drafts.begin());
            drafts.insert(from, draft);
        }
    }
    composer->setDraft(drafts.take(to));
}

QSettings chatPreferences()
{
    return QSettings(QSettings::defaultFormat(), QSettings::UserScope, QStringLiteral("bitrix24-client"),
                     QStringLiteral("bitrix24-client"));
}

QUrl resolvedChatLink(const QUrl &portal, const QString &href)
{
    const QUrl url(href, QUrl::StrictMode);
    if (url.isRelative() && portal.isValid())
        return workspace::messageWebLink(portal.resolved(url).toString(QUrl::FullyEncoded));
    return workspace::messageWebLink(href);
}

QUrl publicLinkTarget(QUrl url)
{
    url.setFragment({});
    if (url.port() == 443)
        url.setPort(-1);
    return url;
}

bool allowsAutomaticPublicPreview(const QModelIndex &row)
{
    return row.data(workspace::Outgoing).toBool() && !row.data(workspace::SystemMessage).toBool()
        && row.data(workspace::ForwardByline).toString().isEmpty() && !row.data(workspace::MarkupBlock).toBool();
}

} // namespace

MainWindow::MainWindow(Backend *backend, bool readOnly, QWidget *parent, PublicLinkRequest publicLinkRequest)
    : QMainWindow(parent), m_backend(backend), m_readOnly(readOnly), m_online(backend->isDemo()),
      m_publicLinkRequest(std::move(publicLinkRequest)),
      m_chatSendController(ui::ComposerSendController::Surface::Chat, backend,
          [this](const QString &, const QString &status, bool loading) {
              setStatus(status, loading);
              updateComposers();
          }),
      m_taskSendController(ui::ComposerSendController::Surface::Task, backend,
          [this](const QString &id, const QString &status, bool loading) {
              if (m_card && m_card->taskId() == id)
                  m_card->setStatus(status, loading);
              else if (!status.isEmpty())
                  setStatus(status, loading);
              updateWriteAvailability();
          })
{
    m_chatPhotoDecodePool.setMaxThreadCount(1);
    m_chatVideoPreviews = std::make_unique<ui::ChatVideoPreviews>(backend, [this](bool posterChanged) {
        if (posterChanged)
            refreshChatMediaRows();
        scheduleAvatars();
    }, [this](QWidget *parent, const QString &error) {
        if (m_card && parent == m_card)
            m_card->setStatus(error);
        else
            setStatus(error);
    });
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
    m_stack->addWidget(buildChatPanel());
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
            [this](const MessageFile &file, QWidget *source) { saveMessageFile(file, source); });
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
    m_reactionAccountScope = m_backend->settingsScope();
    connect(m_backend, &Backend::accountScopeChanged, this, [this] {
        const auto scope = m_backend->settingsScope();
        if (scope != m_reactionAccountScope) {
            m_projectsPanel->resetAccount(!m_reactionAccountScope.isEmpty());
            if (!m_reactionAccountScope.isEmpty()) {
                m_chatImagePreviews.clear();
                m_pendingPastedChatImage = {};
                m_pendingCreateTask.clear();
                m_pendingCreateSubtask.clear();
                m_pendingUpload.clear();
                m_uploadName.clear();
                m_chatSendController.reset();
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
                for (auto &directory : m_chatDirectories)
                    directory = {};
                m_chatSearchTimer->stop();
                {
                    const QSignalBlocker input(m_chatSearch);
                    m_chatSearch->clear();
                }
                m_chats->clear();
                ui::setStatusLabel(m_chatSearchProgress, {});
                closeCurrentChat();
                m_messageSearchToggle->setChecked(false);
                // Closing either view saves its draft for ordinary navigation.
                m_messageSearches.clear();
                m_chatDrafts.clear();
                m_taskDrafts.clear();
                m_typingNames.clear();
                m_typingNamesPending.clear();
                setStatus({});
                updateWriteAvailability();
                m_chatSource.clear();
                m_taskChatPressActive = false;
                m_taskChatRefreshDeferred = false;
                m_retainedTaskChat = {};
                m_retainedTaskChatRow = -1;
                m_projectsPanel->resetTaskChats();
                m_taskChatBadgesRequested = false;
                updateChatUnreadBadges();
            }
            m_pendingReactionAdds.clear();
            m_reactionAccountScope = scope;
        }
        reloadChatVisibilityPreference();
        m_projectsPanel->updateColumnWidthContext(scope);
        m_railWorkdayCurrent = m_worktime->hasConfirmedWorkday();
        requestRailWorkday();
        updateRailWorkday();
    });

    m_chatSearchTimer = new QTimer(this);
    m_chatSearchTimer->setSingleShot(true);
    m_chatSearchTimer->setInterval(300);
    connect(m_chatSearchTimer, &QTimer::timeout, this, &MainWindow::runSearchChats);
    m_messageSearchTimer = new QTimer(this);
    m_messageSearchTimer->setSingleShot(true);
    m_messageSearchTimer->setInterval(300);
    connect(m_messageSearchTimer, &QTimer::timeout, this, &MainWindow::runMessageSearch);
    m_visibleChatReadTimer = new QTimer(this);
    m_visibleChatReadTimer->setObjectName(QStringLiteral("visibleChatReadTimer"));
    m_visibleChatReadTimer->setSingleShot(true);
    m_visibleChatReadTimer->setInterval(160);
    connect(m_visibleChatReadTimer, &QTimer::timeout, this, &MainWindow::reportVisibleChatRead);
    connect(qApp, &QGuiApplication::focusWindowChanged, this, [this](QWindow *) {
        scheduleVisibleChatRead();
        if (m_externalLinkHistory && m_online)
            scheduleAvatars();
    });
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *current) {
        if (current && (current->window() == this || current->window() == m_card))
            scheduleVisibleChatRead();
    });
    m_chatContactTimer = new QTimer(this);
    m_chatContactTimer->setSingleShot(true);
    connect(m_chatContactTimer, &QTimer::timeout, this, &MainWindow::updateChatContactPresentation);
    connectWorkspaceActions();

    connect(m_backend, &Backend::recentChatsLoaded, this,
            [this](const auto &chats) { onChatListLoaded(0, chats); });
    connect(m_backend, &Backend::taskChatsLoaded, this,
            [this](const auto &chats) { onChatListLoaded(1, chats); });
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
        m_chatComposer->refreshMentionAvatars();
        if (m_card) {
            m_card->refreshRequisiteAvatars();
            m_card->composer()->refreshMentionAvatars();
        }
        if (m_taskForm)
            m_taskForm->refreshRequisiteAvatars();
        updateAvatarRows();
        scheduleAvatars();
    });
    connect(m_backend, &Backend::chatPinChanged, this, [this](const QString &id, bool pinned) {
        m_pendingPins.remove(operationKey(OperationKind::ChatPin, id));
        for (auto &directory : m_chatDirectories) {
            for (auto *chats : {&directory.recent, &directory.found})
                for (auto &chat : *chats)
                    if (chat.id == id)
                        chat.pinned = pinned;
        }
        const auto &directory = m_chatDirectories[m_chatScope];
        if (m_lastSection == 0)
            showChatResults(directory.query.isEmpty() ? directory.recent : directory.found, true, true);
    });
    connect(m_backend, &Backend::chatContactChanged, this,
            [this](const QString &id, const QDateTime &onlineUntil, const QString &position) {
        const auto apply = [&](Chat &chat) {
            if (chat.id == id && !chat.taskChat && chat.kind == QStringLiteral("im")) {
                chat.onlineUntil = onlineUntil;
                chat.position = position;
            }
        };
        for (auto &directory : m_chatDirectories) {
            for (auto *chats : {&directory.recent, &directory.found})
                for (auto &chat : *chats)
                    apply(chat);
            apply(directory.linked);
        }
        for (int row = 0; row < m_chats->count(); ++row) {
            auto *item = m_chats->item(row);
            if (item->data(workspace::Id).toString() == id &&
                item->data(workspace::AvatarType).toInt() == int(workspace::AvatarKind::Person)) {
                item->setData(workspace::ContactOnlineUntil, onlineUntil);
                item->setData(workspace::ContactPosition, position);
            }
        }
        if (m_currentChatId == id && m_chatContactIsPerson) {
            m_chatContactUntil = onlineUntil;
            m_chatContactPosition = position;
        }
        updateChatContactPresentation();
    });
    connect(m_backend, &Backend::pinnedMessagesLoaded, this,
            [this](const QString &dialog, const QVector<Message> &messages) {
        if (dialog.isEmpty())
            return;
        if (dialog == m_currentChatId)
            m_chatPinned->setMessages(messages);
        if (m_card && dialog == m_card->taskChatDialogId())
            m_card->setPinnedMessages(messages);
    });
    connect(m_backend, &Backend::chatMessagesLoaded, this,
            [this](const QString &chatId, const QVector<Message> &messages) {
        if (m_lastSection == 0 && chatId == m_currentChatId && !m_messageSearchActive &&
            m_contextDialogId != chatId) {
            static_cast<ui::HistoryList *>(m_chatMessages)->showHistory(chatId, m_chatEmpty, messages);
            updateAvatarRows();
            scheduleAvatars();
        }
    });
    connect(m_backend, &Backend::olderChatMessagesLoaded, this,
            [this](const QString &scope, const QString &before, bool task, const QVector<Message> &messages, bool more) {
        if ((task && (!m_card || scope != m_card->taskId() || !m_cardSearchQuery.isEmpty()))
            || (!task && (m_lastSection != 0 || scope != m_currentChatId || m_messageSearchActive)))
            return;
        auto *history = static_cast<ui::HistoryList *>(task ? m_card->messageList() : m_chatMessages);
        if (history->addOlder(scope, before, messages, more)) {
            updateAvatarRows();
            scheduleAvatars();
        }
    });
    connect(m_backend, &Backend::chatReadConfirmed, this,
            [this](const QString &dialog, const QStringList &ids, qint64 readThrough) {
        if (dialog.isEmpty())
            return;
        if (m_currentChatId == dialog && !m_messageSearchActive && m_contextDialogId.isEmpty())
            static_cast<ui::HistoryList *>(m_chatMessages)->confirmRead(ids, readThrough);
        if (m_card && m_card->taskChatDialogId() == dialog && m_cardSearchQuery.isEmpty() && m_cardContextMessageId.isEmpty())
            static_cast<ui::HistoryList *>(m_card->messageList())->confirmRead(ids, readThrough);
    });
    connect(m_backend, &Backend::chatHistoryRemoved, this,
            [this](const QString &dialog, const QStringList &ids, qint64 through) {
        if (dialog.isEmpty())
            return;
        const bool cleared = ids.isEmpty() && through == 0;
        if (dialog == m_currentChatId) {
            static_cast<ui::HistoryList *>(m_chatMessages)->removeHistory(ids, through);
            if (cleared && !m_messageSearchActive) {
                resetMessageContext();
                showEmpty(m_chatMessages, m_chatEmpty, QStringLiteral("Сообщений пока нет"));
            }
        }
        if (m_card && dialog == m_card->taskChatDialogId()) {
            static_cast<ui::HistoryList *>(m_card->messageList())->removeHistory(ids, through);
            if (cleared && m_cardSearchQuery.isEmpty()) {
                m_cardContextMessageId.clear();
                m_card->setContextMode(false);
                m_card->setChatStatus(QStringLiteral("Сообщений пока нет"));
            }
        }
    });
    connect(m_backend, &Backend::chatBadgeAnchorAvailable, this, [this](const QString &dialog) {
        if (dialog.isEmpty())
            return;
        const auto *active = QApplication::activeWindow();
        if ((active == this && m_lastSection == 0 && m_currentChatId == dialog)
            || (m_card && active == m_card && m_card->taskChatDialogId() == dialog))
            scheduleVisibleChatRead();
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
        if (m_lastSection != 0 || m_currentChatId != dialogId ||
            m_contextDialogId != dialogId || m_contextMessageId != messageId)
            return;
        static_cast<ui::HistoryList *>(m_chatMessages)->showContext(dialogId, m_chatEmpty, messages);
        updateAvatarRows();
        scheduleAvatars();
        if (!focusChatMessage(messageId)) {
            resetMessageContext();
            m_backend->requestChatMessages(dialogId);
            setStatus(QStringLiteral("Сообщение не найдено в истории чата"));
        }
    });
    connect(m_backend, &Backend::personLoaded, this, [this](const Person &person) {
        rememberTypingName(person.id, person.name);
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
    connect(m_backend, &Backend::messageFileDownloaded, this, [this](const QUrl &url, const QString &path) {
        if (url != m_messageDownloadUrl || path != m_messageDownloadPath)
            return;
        m_messageDownloadUrl.clear();
        m_messageDownloadPath.clear();
        auto parent = std::exchange(m_messageDownloadParent, {});
        const auto notice = ui::plainTooltip(QStringLiteral("Файл сохранён: %1").arg(QFileInfo(path).fileName()));
        if (m_card && parent == m_card)
            m_card->setStatus(notice);
        setStatus(notice);
        ui::offerToOpenSavedFile(parent, path);
    });
    connect(m_backend, &Backend::messageImageLoaded, this, [this](const QUrl &url, const QByteArray &bytes) {
        if (!m_chatImagePreviews.complete(url, bytes))
            return;
        if (m_chatImagePreviews.photo(url))
            refreshChatMediaRows();
        scheduleAvatars();
    });
    const auto resetLinkPreviewAccess = [this](bool available) {
        if (m_linkPreviewAccess != available && !m_publicLinkPreviews.isEmpty())
            scheduleLinkPreviewLayout();
        m_linkPreviewAccess = available;
        resetLinkPreviews();
    };
    connect(m_backend, &Backend::messageLinkPreviewAccessChanged, this, resetLinkPreviewAccess);
    connect(m_backend, &Backend::accountScopeChanged, this, [this, resetLinkPreviewAccess] {
        ++m_publicLinkAccountGeneration;
        if (!m_publicLinkPreviews.isEmpty()) {
            m_publicLinkPreviews.clear();
            scheduleLinkPreviewLayout();
        }
        resetLinkPreviewAccess(m_linkPreviewAccess);
    });

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
        scheduleVisibleChatRead();
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
        m_chatSendController.confirm(scope, m_chatComposer, m_currentChatId, m_chatDrafts);
        m_taskSendController.confirm(scope, m_card ? m_card->composer() : nullptr,
                                     m_card ? m_card->taskId() : QString{}, m_taskDrafts);
    });

    reloadChatVisibilityPreference();
    updateComposers();
    refreshCurrentSection();
    updateChatUnreadBadges();
    QTimer::singleShot(0, this, [this] {
        m_events->ensureHistory();
        requestRailWorkday();
    });
}

MainWindow::~MainWindow()
{
    if (m_chatPhoto)
        m_chatPhoto->close();
}

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
            setShowDismissedChats(show);
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
    setShowDismissedChats(show);
}

QVariantMap MainWindow::reactionFrequency() const
{
    const auto key = chatPreferenceKey(m_backend->settingsScope(), QLatin1String("reactionFrequency"));
    if (key.isEmpty())
        return {};
    auto settings = chatPreferences();
    settings.beginGroup(key);
    QVariantMap frequency;
    for (const auto &code : settings.childKeys()) {
        if (ui::findReaction(code))
            frequency.insert(code, settings.value(code));
    }
    return frequency;
}

void MainWindow::incrementReactionFrequency(const QString &code)
{
    const auto key = chatPreferenceKey(m_backend->settingsScope(), QLatin1String("reactionFrequency"));
    if (key.isEmpty())
        return;
    auto settings = chatPreferences();
    settings.beginGroup(key);
    const auto count = settings.value(code).toULongLong();
    if (count < std::numeric_limits<qulonglong>::max())
        settings.setValue(code, count + 1);
}

void MainWindow::setShowDismissedChats(bool show)
{
    if (m_showDismissedChats == show)
        return;
    m_showDismissedChats = show;
    if (m_lastSection == 0) {
        const auto &directory = m_chatDirectories[m_chatScope];
        showChatResults(directory.query.isEmpty() ? directory.recent : directory.found);
    }
}

QWidget *MainWindow::buildChatPanel()
{
    auto *panel = new QWidget;
    auto *root = new QHBoxLayout(panel);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *splitter = new DirectorySplitter(QStringLiteral("ui/chatDirectoryWidth"), 272, 218, panel);
    splitter->setObjectName(QStringLiteral("chatDirectorySplitter"));
    splitter->setAccessibleName(QStringLiteral("Ширина списка чатов"));
    root->addWidget(splitter);
    auto *side = directory(QStringLiteral("Чаты"), &m_chats, splitter);
    m_chats->setAccessibleName(QStringLiteral("Недавние чаты"));
    m_taskChatPressEnd = new QTimer(this);
    m_taskChatPressEnd->setSingleShot(true);
    connect(m_taskChatPressEnd, &QTimer::timeout, this, [this] {
        m_taskChatPressActive = false;
        if (m_lastSection == 0 && m_chats->isVisible() && std::exchange(m_taskChatRefreshDeferred, false) && m_chatScope == 1
            && m_chatDirectories[1].query.isEmpty())
            showChatResults(m_chatDirectories[1].recent, true, true);
    });
    m_chats->viewport()->installEventFilter(this);
    connect(m_chats, &QListWidget::currentItemChanged, this, &MainWindow::onChatSelected);
    const auto focusSelectedComposer = [this](const QListWidgetItem *item) {
        if (item && item->data(workspace::Id).toString() == m_currentChatId) {
            focusChatComposer();
            scheduleVisibleChatRead();
        }
    };
    connect(m_chats, &QListWidget::itemClicked, this, focusSelectedComposer);
    connect(m_chats, &QListWidget::itemActivated, this, focusSelectedComposer);
    auto *sideLayout = qobject_cast<QVBoxLayout *>(side->layout());
    sideLayout->setContentsMargins(8, 13, 8, 8);
    sideLayout->setSpacing(6);
    sideLayout->itemAt(0)->widget()->hide();
    m_chatTabs = new QTabBar(side);
    m_chatTabs->setObjectName(QStringLiteral("chatScopeTabs"));
    m_chatTabs->setAccessibleName(QStringLiteral("Список чатов"));
    m_chatTabs->addTab(QStringLiteral("Чаты"));
    m_chatTabs->addTab(QStringLiteral("Чаты задач"));
    m_chatTabs->setExpanding(true);
    m_chatTabs->setDrawBase(false);
    m_chatTabs->setUsesScrollButtons(false);
    m_chatTabs->setElideMode(Qt::ElideRight);
    m_chatTabs->ensurePolished();
    for (int scope = 0; scope < m_chatTabs->count(); ++scope) {
        auto *badge = new QLabel(m_chatTabs);
        badge->setObjectName(QStringLiteral("chatScopeUnreadBadge"));
        badge->setAlignment(Qt::AlignCenter);
        badge->setAttribute(Qt::WA_TransparentForMouseEvents);
        badge->ensurePolished();
        const auto metrics = badge->fontMetrics();
        // QTabBar positions the button with its current size when it is installed.
        badge->setFixedSize(qMax(20, metrics.horizontalAdvance(QStringLiteral("99+")) + 8), metrics.height() + 2);
        m_chatTabs->setTabButton(scope, QTabBar::RightSide, badge);
        badge->hide();
    }
    const auto margins = sideLayout->contentsMargins();
    side->setMinimumWidth(qMax(196, m_chatTabs->minimumSizeHint().width() + margins.left() + margins.right()));
    sideLayout->insertWidget(0, m_chatTabs);
    connect(m_chatTabs, &QTabBar::currentChanged, this, &MainWindow::onChatScopeChanged);
    m_chats->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_chats, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &point) { showChatPinMenu(m_chats, point); });
    connect(m_chats->verticalScrollBar(), &QScrollBar::valueChanged, this, &MainWindow::scheduleAvatars);
    auto *chatSearchRow = new QHBoxLayout;
    chatSearchRow->setSpacing(2);
    m_unreadOnlyButton = new ui::GlyphButton(ui::Glyph::Unread, QStringLiteral("Только непрочитанные чаты"), side);
    m_unreadOnlyButton->setCheckable(true);
    m_unreadOnlyButton->setToolTip(QStringLiteral("Только непрочитанные чаты"));
    chatSearchRow->addWidget(m_unreadOnlyButton);
    m_chatSearch = new QLineEdit(side);
    ui::editor(m_chatSearch);
    m_chatSearch->setAccessibleName(QStringLiteral("Поиск чатов"));
    m_chatSearch->setPlaceholderText(QStringLiteral("Поиск чатов"));
    chatSearchRow->addWidget(m_chatSearch, 1);
    m_chatSearchClear = new ui::GlyphButton(ui::Glyph::Close, QStringLiteral("Очистить поиск чатов"), side);
    chatSearchRow->addWidget(m_chatSearchClear);
    m_createGroupChat = ui::button(QStringLiteral("+"), QStringLiteral("Создать групповой чат"), side);
    m_createGroupChat->setToolTip(QStringLiteral("Создать групповой чат"));
    if (m_readOnly || m_backend->isDemo())
        m_createGroupChat->hide();
    connect(m_createGroupChat, &QPushButton::clicked, this, &MainWindow::promptGroupChat);
    chatSearchRow->addWidget(m_createGroupChat);
    sideLayout->insertLayout(1, chatSearchRow);
    m_chatSearchProgress = label({}, "quiet", side);
    m_chatSearchProgress->setAccessibleName(QStringLiteral("Состояние поиска чатов"));
    m_chatSearchProgress->hide();
    sideLayout->insertWidget(2, m_chatSearchProgress);
    connect(m_chatSearch, &QLineEdit::returnPressed, this, &MainWindow::runSearchChats);
    connect(m_chatSearch, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (text.trimmed().isEmpty())
            clearChatSearch();
        else
            m_chatSearchTimer->start();
    });
    connect(m_chatSearchClear, &QPushButton::clicked, this, &MainWindow::clearChatSearch);
    connect(m_unreadOnlyButton, &QPushButton::toggled, this, [this](bool checked) {
        if (m_unreadOnlyChats == checked)
            return;
        m_unreadOnlyChats = checked;
        showChatResults(m_chatSource, m_chatSourceSearching);
    });
    splitter->addWidget(side);

    auto *reading = new QWidget(splitter);
    reading->setMinimumWidth(325);
    reading->setObjectName(QStringLiteral("reading"));
    auto *layout = new QVBoxLayout(reading);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *top = new QWidget(reading);
    top->setObjectName(QStringLiteral("conversationHeader"));
    top->setMinimumHeight(55);
    auto *header = new QHBoxLayout(top);
    header->setContentsMargins(12, 6, 8, 6);
    header->setSpacing(5);
    m_chatAvatar = new ui::AvatarBadge(top);
    m_chatAvatar->hide();
    header->addWidget(m_chatAvatar);
    auto *chatHeading = new QWidget(top);
    auto *chatHeadingLayout = new QVBoxLayout(chatHeading);
    chatHeadingLayout->setContentsMargins(4, 0, 0, 0);
    chatHeadingLayout->setSpacing(0);
    m_chatTitle = label(QStringLiteral("Чаты"), "conversationTitle", chatHeading);
    m_chatTitle->setAccessibleName(QStringLiteral("Открытый чат"));
    m_chatTitle->setMinimumWidth(0);
    m_chatTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    chatHeadingLayout->addWidget(m_chatTitle);
    m_chatContactInfo = label({}, "quiet", chatHeading);
    m_chatContactInfo->setAccessibleName(QStringLiteral("Статус и должность контакта"));
    m_chatContactInfo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_chatContactInfo->hide();
    chatHeadingLayout->addWidget(m_chatContactInfo);
    header->addWidget(chatHeading, 1);
    m_openChatTask = ui::button(QStringLiteral("Открыть задачу"), QStringLiteral("Открыть карточку задачи"), top);
    connect(m_openChatTask, &QPushButton::clicked, this, [this] { openTaskCard(m_openChatTaskId); });
    m_openChatTask->hide();
    header->addWidget(m_openChatTask);
    auto *assets = ui::button(QStringLiteral("Вложения"), QStringLiteral("Медиа, файлы и ссылки чата"), top);
    connect(assets, &QPushButton::clicked, this, [this] { showChatAssets(m_currentChatId, this); });
    header->addWidget(assets);
    m_messageSearchToggle = new ui::GlyphButton(ui::Glyph::Search, QStringLiteral("Найти в чате"), top);
    m_messageSearchToggle->setCheckable(true);
    header->addWidget(m_messageSearchToggle);
    auto *refresh = new ui::GlyphButton(ui::Glyph::Refresh, QStringLiteral("Обновить чат (F5)"), top);
    connect(refresh, &QPushButton::clicked, this, &MainWindow::onRefresh);
    header->addWidget(refresh);
    layout->addWidget(top);
    m_chatPinned = new ui::PinnedMessageBar(reading);
    m_chatPinned->setOnOpen([this](const QString &id) { openChatMessageContext(m_currentChatId, id); });
    layout->addWidget(m_chatPinned);
    m_latestChat = new QPushButton(QStringLiteral("К последним сообщениям"), reading);
    m_latestChat->setAccessibleName(QStringLiteral("К последним сообщениям"));
    m_latestChat->hide();
    connect(m_latestChat, &QPushButton::clicked, this, [this] {
        resetMessageContext();
        m_backend->requestChatMessages(m_currentChatId);
    });
    layout->addWidget(m_latestChat, 0, Qt::AlignRight);

    auto *searchBar = new QWidget(reading);
    searchBar->setObjectName(QStringLiteral("conversationSearch"));
    auto *messageSearchRow = new QHBoxLayout(searchBar);
    messageSearchRow->setContentsMargins(12, 3, 8, 3);
    messageSearchRow->setSpacing(5);
    m_messageSearch = new QLineEdit(searchBar);
    ui::editor(m_messageSearch);
    m_messageSearch->setAccessibleName(QStringLiteral("Поиск по сообщениям"));
    m_messageSearch->setPlaceholderText(QStringLiteral("Поиск по истории чата"));
    messageSearchRow->addWidget(m_messageSearch, 1);
    m_messageSearchClear = new ui::GlyphButton(ui::Glyph::Close, QStringLiteral("Очистить поиск по сообщениям"), searchBar);
    messageSearchRow->addWidget(m_messageSearchClear);
    layout->addWidget(searchBar);
    searchBar->hide();
    connect(m_messageSearchToggle, &QPushButton::toggled, this, [this, searchBar](bool show) {
        searchBar->setVisible(show);
        if (show)
            m_messageSearch->setFocus(Qt::ShortcutFocusReason);
        else
            clearMessageSearch();
    });
    auto *find = new QAction(reading);
    find->setShortcut(QKeySequence::Find);
    find->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(find, &QAction::triggered, this, [this] {
        m_messageSearchToggle->setChecked(true);
        m_messageSearch->setFocus(Qt::ShortcutFocusReason);
    });
    reading->addAction(find);
    auto *closeSearch = new QAction(searchBar);
    closeSearch->setShortcut(QKeySequence(Qt::Key_Escape));
    closeSearch->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(closeSearch, &QAction::triggered, this, [this] {
        m_messageSearchToggle->setChecked(false);
        m_messageSearchToggle->setFocus(Qt::ShortcutFocusReason);
    });
    searchBar->addAction(closeSearch);
    connect(m_messageSearch, &QLineEdit::returnPressed, this, &MainWindow::runMessageSearch);
    connect(m_messageSearch, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (text.trimmed().isEmpty())
            clearMessageSearch();
        else
            m_messageSearchTimer->start();
    });
    connect(m_messageSearchClear, &QPushButton::clicked, this, &MainWindow::clearMessageSearch);

    auto *canvas = new ui::ConversationCanvas(reading);
    auto *timeline = new QVBoxLayout(canvas);
    timeline->setContentsMargins(12, 5, 12, 8);
    timeline->setSpacing(0);
    m_chatMessages = new ui::HistoryList(canvas);
    static_cast<ui::HistoryList *>(m_chatMessages)->setOnViewportChanged([this] { scheduleVisibleChatRead(); });
    m_chatMessages->setAccessibleName(QStringLiteral("Сообщения чата"));
    workspace::configureRows(m_chatMessages, workspace::Row::Message);
    bindMessageFiles(m_chatMessages, true);
    connect(m_chatMessages->verticalScrollBar(), &QScrollBar::valueChanged, this, &MainWindow::scheduleAvatars);
    auto *history = static_cast<ui::HistoryList *>(m_chatMessages);
    history->setOnBrowsingOlder([this](bool active) {
        m_latestChat->setVisible(active || !m_contextDialogId.isEmpty());
    });
    timeline->addWidget(history->olderButton(canvas), 0, Qt::AlignHCenter);
    timeline->addWidget(m_chatMessages, 1);
    history->setOnOlder([this](const QString &before) {
        if (m_lastSection == 0 && !m_currentChatId.isEmpty() && !m_messageSearchActive)
            m_backend->requestOlderChatMessages(m_currentChatId, before, false);
    });
    m_chatEmpty = emptyState(QStringLiteral("Загружаем чаты…"), canvas);
    ui::setLoadingIndicator(m_chatEmpty, true);
    timeline->addWidget(m_chatEmpty, 1);
    m_chatMessages->hide();
    layout->addWidget(canvas, 1);

    m_chatComposer = new ui::ChatComposer(reading, QStringLiteral("Текст сообщения"),
        QStringLiteral("Эмодзи в чате"), QStringLiteral("Прикрепить файл в чат"), QStringLiteral("Отправить сообщение"));
    m_chatInput = m_chatComposer->input();
    m_chatComposer->setEditLatest([this, history] { return editLatestMessage(history, m_currentChatId); });
    m_chatComposer->setTypingActions([this] {
        return writable() && m_lastSection == 0 && isActiveWindow() ? m_currentChatId : QString();
    }, [this](const QString &dialog) { m_backend->notifyChatTyping(dialog); });
    history->setComposer(m_chatInput);
    history->setReplyActions([this](const ui::ChatReply &reply) {
        m_chatComposer->setReply(reply);
        m_chatInput->setFocus(Qt::OtherFocusReason);
    }, [this] { return writable() && !m_currentChatId.isEmpty(); },
       [this](const QString &id) { openChatMessageContext(m_currentChatId, id); });
    history->setMessageMenu([this](QMenu &menu, const QPersistentModelIndex &index, const QString &href) {
        addMessageActions(menu, index, m_currentChatId, m_chatScope == 1, href);
    });
    history->setReactionActions([this](const QModelIndex &index, const QString &code, const QPoint &anchor) {
        actOnMessageReaction(index, m_currentChatId, code, anchor, this);
    }, [this] { return writable() && !m_backend->isDemo() && !m_currentChatId.isEmpty(); });
    m_chatEmoji = m_chatComposer->emoji();
    m_chatFile = m_chatComposer->file();
    m_chatSend = m_chatComposer->send();
    if (m_readOnly)
        m_chatComposer->setPlaceholderText(QStringLiteral("Режим чтения"));
    connect(m_chatFile, &QPushButton::clicked, this, [this] { pickFile(m_currentChatId, QStringLiteral("chat")); });
    connect(m_chatSend, &QPushButton::clicked, this, &MainWindow::onSendChat);
    connect(m_chatInput, &QPlainTextEdit::textChanged, this, &MainWindow::updateComposers);
    m_chatComposer->setEditStateChanged([this] { updateComposers(); });
    m_chatComposer->setImageDraftHandlers([this] { updateComposers(); },
                                          [this](const QString &error) { setStatus(error); });
    m_chatComposer->setMentionSearchHandler([this](const QString &id, const QString &query) {
        if (!m_currentChatId.isEmpty())
            m_backend->searchPeople(id, query, m_currentChatId);
        else
            m_chatComposer->setMentionResults(id, {});
    });
    m_chatComposer->setMentionAvatars([this](const Person &person) {
        return ui::requisiteAvatarIcon([this](const QUrl &url) { return m_avatars.object(url); },
                                       person.name, person.avatarUrl, workspace::AvatarKind::Person);
    }, [this] { scheduleAvatars(); });
    m_chatInput->installEventFilter(this);
    layout->addWidget(m_chatComposer);
    m_chatComposer->hide();
    splitter->addWidget(reading);
    splitter->setStretchFactor(1, 1);
    connect(splitter, &QSplitter::splitterMoved, this, &MainWindow::scheduleAvatars);
    return panel;
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
    if (event->type() == QEvent::ActivationChange && !isActiveWindow() && m_taskChatPressActive)
        m_taskChatPressEnd->start(0);
}

void MainWindow::onSectionChanged(int index)
{
    m_projectsPanel->setSectionActive(index == 1);
    m_events->setSectionActive(index == 2);
    m_lastSection = index;
    if (index != 0) {
        static_cast<ui::HistoryList *>(m_chatMessages)->setLiveReadHistory(false);
        resetMessageContext();
    }
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
    case 0: {
        const auto selectedChat = m_currentChatId;
        const auto refreshHistory = [this, &selectedChat] {
            if (!selectedChat.isEmpty() && selectedChat == m_currentChatId)
                m_backend->requestChatMessages(selectedChat);
        };
        if (reloadProjectDirectory && !m_messageSearchActive)
            refreshHistory();
        if (!m_chatDirectories[m_chatScope].query.isEmpty())
            runSearchChats();
        else
            requestCurrentChatList();
        if (m_messageSearchActive)
            runMessageSearch();
        else if (!reloadProjectDirectory)
            refreshHistory();
        break;
    }
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
    resetMessageContext();
    setStatus(QString());
    refreshCurrentSection();
}

void MainWindow::onChatSelected()
{
    const auto *item = m_chats->currentItem();
    if (!item)
        return;
    const auto id = item->data(workspace::Id).toString();
    const bool leftRetainedTask = !m_retainedTaskChat.id.isEmpty() && id != m_retainedTaskChat.id;
    openChatRow(*item);
    if (leftRetainedTask) {
        QTimer::singleShot(0, this, [this, id] {
            if (m_lastSection == 0 && m_chatScope == 1 && m_currentChatId == id
                && m_chatDirectories[1].query.isEmpty()
                && !m_retainedTaskChat.id.isEmpty() && id != m_retainedTaskChat.id)
                showChatResults(m_chatDirectories[1].recent, true, true);
        });
    }
}

void MainWindow::focusChatComposer()
{
    if (!m_currentChatId.isEmpty() && m_chatInput->isVisible() && !m_chatInput->isReadOnly())
        m_chatInput->setFocus(Qt::OtherFocusReason);
}

void MainWindow::scheduleVisibleChatRead()
{
    if (m_visibleChatReadTimer)
        m_visibleChatReadTimer->start();
}

void MainWindow::reportVisibleChatRead()
{
    if (!writable())
        return;
    const auto *active = QApplication::activeWindow();
    const auto *modal = QApplication::activeModalWidget();
    if (!active || active->isMinimized() || QApplication::activePopupWidget() || (modal && modal != active))
        return;
    if (m_card && active == m_card && m_cardDetailsReady && m_cardSearchQuery.isEmpty() && m_cardContextMessageId.isEmpty()) {
        const auto seen = static_cast<ui::HistoryList *>(m_card->messageList())->visibleMessageIds();
        if (!seen.all.isEmpty())
            m_backend->readTaskChatMessages(m_card->taskId(), seen.incoming, seen.all);
        return;
    }
    if (active != this || m_lastSection != 0 || m_currentChatId.isEmpty() ||
        m_messageSearchActive || !m_contextDialogId.isEmpty())
        return;
    const auto seen = static_cast<ui::HistoryList *>(m_chatMessages)->visibleMessageIds();
    if (!seen.all.isEmpty())
        m_backend->readChatMessages(m_currentChatId, seen.incoming, seen.all);
}

void MainWindow::updateChatContactPresentation()
{
    const auto now = QDateTime::currentDateTimeUtc();
    const bool known = m_chatContactIsPerson && m_chatContactUntil.isValid();
    const bool online = known && m_chatContactUntil > now;
    QStringList details;
    if (known)
        details.append(online ? QStringLiteral("В сети") : QStringLiteral("Не в сети"));
    if (m_chatContactIsPerson && !m_chatContactPosition.trimmed().isEmpty())
        details.append(m_chatContactPosition.simplified());
    const auto text = details.join(QStringLiteral(" · "));
    ui::setStatusLabel(m_chatContactInfo, text);
    m_chatContactInfo->setToolTip(ui::plainTooltip(text));
    m_chatAvatar->setOnline(online);
    m_chats->viewport()->update();
    m_chatContactTimer->stop();
    QDateTime next = online ? m_chatContactUntil : QDateTime();
    for (int i = 0; i < m_chats->count(); ++i) {
        const auto expiry = m_chats->item(i)->data(workspace::ContactOnlineUntil).toDateTime();
        if (expiry.isValid() && expiry > now && (!next.isValid() || expiry < next))
            next = expiry;
    }
    if (next.isValid())
        m_chatContactTimer->start(int(qBound(qint64(1), now.msecsTo(next) + 10, qint64(900000))));
}

void MainWindow::openChatRow(const QListWidgetItem &item)
{
    const auto chatId = item.data(workspace::Id).toString();
    if (chatId.isEmpty())
        return;
    const auto name = item.data(workspace::Title).toString();
    m_chatTitle->setText(name);
    m_chatTitle->setToolTip(ui::plainTooltip(name));
    const auto avatar = workspace::AvatarKind(item.data(workspace::AvatarType).toInt());
    m_chatAvatar->setTitle(name, avatar);
    m_chatContactIsPerson = m_chatScope == 0 && avatar == workspace::AvatarKind::Person;
    m_chatContactUntil = item.data(workspace::ContactOnlineUntil).toDateTime();
    m_chatContactPosition = item.data(workspace::ContactPosition).toString();
    updateChatContactPresentation();
    m_chatAvatarUrl = item.data(workspace::AvatarUrl).toUrl();
    if (const auto *photo = m_avatars.object(m_chatAvatarUrl))
        m_chatAvatar->setPhoto(*photo);
    const auto taskId = item.data(workspace::TaskId).toString();
    const bool taskChanged = m_openChatTaskId != taskId;
    m_openChatTaskId = taskId;
    m_openChatTask->setVisible(!m_openChatTaskId.isEmpty());
    m_chatDirectories[m_chatScope].selected = chatId;
    scheduleAvatars();
    if (chatId == m_currentChatId) {
        if (taskChanged)
            scheduleLinkPreviewLayout();
        return;
    }
    m_projectsPanel->cancelPendingMessageProject();
    resetLinkPreviews();
    m_chatComposer->dismissEmoji();
    switchDraft(m_chatComposer, m_chatDrafts, m_currentChatId, chatId);
    m_messageSearchTimer->stop();
    {
        const QSignalBlocker blocker(m_messageSearch);
        m_messageSearch->clear();
    }
    m_messageSearchActive = false;
    m_messageSearchQuery.clear();
    m_messageSearchScope.clear();
    m_currentChatId = chatId;
    static_cast<ui::HistoryList *>(m_chatMessages)->resetHistory(chatId);
    m_chatPinned->setMessages({});
    if (m_contextDialogId != chatId)
        resetMessageContext();
    m_chatComposer->show();
    showEmpty(m_chatMessages, m_chatEmpty, QStringLiteral("Загружаем сообщения…"), true);
    setStatus(QString());
    updateComposers();
    if (m_contextDialogId != chatId)
        m_backend->requestChatMessages(chatId);
    m_backend->requestPinnedMessages(chatId);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (m_chats && watched == m_chats->viewport()) {
        const auto type = event->type();
        if (m_lastSection == 0 && m_chatScope == 1 && m_chatDirectories[1].query.isEmpty()
            && (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick)) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            const auto *row = m_chats->itemAt(mouse->position().toPoint());
            if (mouse->button() == Qt::LeftButton && row && !row->data(workspace::Id).toString().isEmpty()) {
                m_taskChatPressActive = true;
                m_taskChatPressEnd->stop();
            }
        } else if (m_taskChatPressActive && (type == QEvent::UngrabMouse || type == QEvent::Hide
                   || (type == QEvent::Leave && !QGuiApplication::mouseButtons().testFlag(Qt::LeftButton))
                   || (type == QEvent::MouseButtonRelease && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton))) {
            // Let QListWidget process the release and emit itemClicked before rebuilding its model.
            m_taskChatPressEnd->start(0);
        } else if (type == QEvent::Show && m_taskChatRefreshDeferred) {
            m_taskChatPressEnd->start(0);
        }
    }
    if (event->type() == QEvent::KeyPress && watched == m_chatInput && !m_chatComposer->composing()) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (m_chatComposer->handleComposerKey(key))
            return true;
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
            !(key->modifiers() & Qt::ShiftModifier)) {
            onSendChat();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
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

void MainWindow::onSendChat()
{
    if (m_chatComposer->submitMessageEdit())
        return;
    sendFromComposer(m_chatSendController, m_chatComposer, m_currentChatId);
}

void MainWindow::sendFromComposer(ui::ComposerSendController &controller, ui::ChatComposer *composer, const QString &id)
{
    const bool task = controller.isTask();
    controller.send(*composer, id, writable(), pastedChatImagePending(id, task),
                    [this, composer, id, task](const QString &caption) {
                        uploadPastedChatImage(composer, id, task, caption);
                    });
}

void MainWindow::onPeopleFound(const QString &requestId, const QVector<Person> &people)
{
    m_chatComposer->setMentionResults(requestId, people);
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
    const bool activeChatHistory = m_lastSection == 0 && !m_currentChatId.isEmpty() && !m_messageSearchActive
        && kind == OperationKind::Chat && ids.value(0) == m_currentChatId;
    const bool activeTaskHistory = m_card && m_cardSearchQuery.isEmpty()
        && kind == OperationKind::TaskMessages && ids.value(0) == m_card->taskId();
    // These reads are ancillary, or belong to history the user has already left.
    // Access failures and other diagnostics still use the normal failure path.
    if (networkFailure && (kind == OperationKind::Push
        || kind == OperationKind::TaskChatTitles
        || kind == OperationKind::ChatHistoryIdentity
        || kind == OperationKind::Pins
        || (kind == OperationKind::Chat && !activeChatHistory)
        || (kind == OperationKind::TaskMessages && !activeTaskHistory)
        || (kind == OperationKind::Chats && (m_lastSection != 0 || m_chatScope != 0))))
        return;

    if (m_events->handleFailure(operation, kind, ids, reason))
        return;

    bool statusPresented = false;
    QString footer = reason;
    if (m_projectsPanel->handleFailure(operation, kind, ids, reason, footer))
        return;
    switch (kind) {
    case OperationKind::SearchPeople: {
        const auto request = ids.value(0);
        m_chatComposer->setMentionSearchError(request);
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
        const auto task = ids.value(0) == QLatin1String("task");
        if (!task || m_card) {
            auto *history = static_cast<ui::HistoryList *>(task ? m_card->messageList() : m_chatMessages);
            history->olderFailed(ids.value(1), ids.value(2), reason);
        }
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
        if (m_chatComposer->matchesEdit(dialog, id)) {
            m_chatComposer->failMessageEdit();
            setStatus(reason);
        }
        return;
    }
    case OperationKind::MessageReaction:
        m_pendingReactionAdds.remove(operation);
        [[fallthrough]];
    case OperationKind::MessageAction:
    case OperationKind::ForwardMessage:
    case OperationKind::CreateTaskFromMessage:
    case OperationKind::ChatMember: {
        const auto dialog = ids.value(0);
        if (m_card && m_card->taskChatDialogId() == dialog)
            m_card->setStatus(reason);
        setStatus(reason);
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
        if (ids.value(0) == m_contextDialogId && ids.value(1) == m_contextMessageId) {
            const auto dialogId = m_contextDialogId;
            resetMessageContext();
            m_backend->requestChatMessages(dialogId);
            setStatus(reason);
            return;
        }
        break;
    case OperationKind::MessageFileDownload:
        if (!m_messageDownloadUrl.isEmpty()) {
            m_messageDownloadUrl.clear();
            m_messageDownloadPath.clear();
            auto parent = std::exchange(m_messageDownloadParent, {});
            if (m_card && parent == m_card)
                m_card->setStatus(reason);
            setStatus(reason);
        }
        return;
    case OperationKind::Person:
        rememberTypingName(ids.value(0), {});
        if (m_personDialog && ids.value(0) == m_personId) {
            m_personName->setText(reason);
            ui::setLoadingIndicator(m_personName, false);
        }
        return;
    case OperationKind::ChatPin:
        if (m_pendingPins.remove(operation))
            setStatus(reason);
        return;
    case OperationKind::SearchChats:
    case OperationKind::SearchTaskChats: {
        const auto expectedKind = m_chatScope == 0 ? OperationKind::SearchChats : OperationKind::SearchTaskChats;
        if (m_lastSection == 0 && !m_chatDirectories[m_chatScope].query.isEmpty() &&
            kind == expectedKind && ids.value(0) == m_chatSearch->text().trimmed()) {
            ui::setStatusLabel(m_chatSearchProgress, {});
            showChatResults({});
            setStatus(reason);
        }
        return;
    }
    case OperationKind::SearchChat:
    case OperationKind::SearchTaskChat: {
        const auto query = m_messageSearches.take(operation);
        if (query.isEmpty())
            return;
        if (m_card && kind == OperationKind::SearchTaskChat && ids.value(0) == m_card->taskId() &&
            !m_cardSearchQuery.isEmpty()) {
            if (query != m_cardSearchQuery)
                requestMessageSearch(m_card->taskId(), m_cardSearchQuery, true);
            else if (query == m_card->searchQuery())
                m_card->setChatStatus(reason);
        } else if (m_lastSection == 0 && m_messageSearchActive &&
                   kind == OperationKind::SearchChat && ids.value(0) == m_messageSearchScope) {
            if (query != m_messageSearchQuery)
                requestMessageSearch(m_messageSearchScope, m_messageSearchQuery, false);
            else if (query == m_messageSearch->text().trimmed())
                showEmpty(m_chatMessages, m_chatEmpty, reason);
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
            updateComposers();
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
    case OperationKind::SendChat:
    case OperationKind::SendTask: {
        const auto id = ids.value(0);
        const bool task = kind == OperationKind::SendTask;
        auto &controller = task ? m_taskSendController : m_chatSendController;
        if (controller.fail(id, reason, networkFailure))
            statusPresented = !task || networkFailure || !m_card || m_card->taskId() != id;
        break;
    }
    case OperationKind::Chat: {
        if (!activeChatHistory)
            break;
        const auto recovery = QStringLiteral("Не удалось загрузить сообщения. Нажмите «Обновить» или F5");
        if (m_chatMessages->count() == 0)
            showEmpty(m_chatMessages, m_chatEmpty, recovery);
        if (networkFailure)
            footer = recovery;
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
    case OperationKind::TaskChats:
        if (m_lastSection != 0 || m_chatScope != 1)
            return;
        [[fallthrough]];
    case OperationKind::Chats:
        if (m_lastSection != 0 || (kind == OperationKind::Chats && m_chatScope != 0)
            || m_chatDirectories[m_chatScope].loaded)
            break;
        m_chats->clear();
        workspace::emptyRow(m_chats, QStringLiteral("Не удалось загрузить чаты"));
        if (m_currentChatId.isEmpty())
            showEmpty(m_chatMessages, m_chatEmpty, QStringLiteral("Выберите чат"));
        break;
    default:
        break;
    }
    if (!statusPresented)
        setStatus(footer);
    updateComposers();
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
    updateComposers();
    updateWriteAvailability();
    if (restored) {
        requestRailWorkday();
        scheduleVisibleChatRead();
        m_events->connectionRestored();
        if (m_externalLinkHistory)
            scheduleAvatars();
    }
    updateRailWorkday();
}

void MainWindow::updateComposers()
{
    const auto wire = m_chatComposer->wireText();
    const auto text = wire.trimmed();
    const bool enabled = writable() && !m_currentChatId.isEmpty();
    const bool editing = m_chatComposer->editingMessage();
    const bool saving = m_chatComposer->messageEditPending();
    const bool hasImage = m_chatComposer->hasDraftImage();
    const bool sending = m_chatSendController.pending() || pastedChatImagePending(m_currentChatId, false);
    m_chatInput->setReadOnly(m_readOnly || m_currentChatId.isEmpty() || saving);
    const bool canSubmit = editing ? m_chatComposer->canSaveMessageEdit(wire) : !sending
        && text.size() <= ui::kMessageCharacters && (hasImage ? m_pendingUpload.isEmpty() : !text.isEmpty());
    m_chatSend->setEnabled(enabled && canSubmit);
    m_chatSend->setToolTip(editing ? (saving ? QStringLiteral("Сохраняем сообщение…") : QStringLiteral("Сохранить сообщение"))
        : text.size() > ui::kMessageCharacters ? QStringLiteral("Максимум 20 000 символов")
        : sending || (hasImage && !m_pendingUpload.isEmpty()) ? QStringLiteral("Отправка…")
        : QStringLiteral("Отправить сообщение"));
    m_chatEmoji->setEnabled(enabled && !saving);
    m_chatFile->setEnabled(enabled && !editing && m_pendingUpload.isEmpty());
}

void MainWindow::setStatus(const QString &text, bool loading)
{
    ui::setStatusLabel(m_status, text, loading);
}

void MainWindow::connectWorkspaceActions()
{
    connect(m_backend, &Backend::chatMessagesSeen, this, [this](const QString &dialog, const QStringList &ids) {
        const QSet<QString> seen(ids.cbegin(), ids.cend());
        const auto mark = [&seen](QListWidget *history) {
            for (int i = 0; i < history->count(); ++i) {
                auto *item = history->item(i);
                if (item->data(workspace::Outgoing).toBool() && !item->data(workspace::MessageSeen).toBool()
                    && seen.contains(item->data(workspace::ReceiptMessageId).toString())) {
                    item->setData(workspace::MessageSeen, true);
                    item->setData(Qt::AccessibleTextRole,
                                  item->data(Qt::AccessibleTextRole).toString() + QStringLiteral(". Просмотрено"));
                }
            }
        };
        if (m_card && m_card->taskChatDialogId() == dialog)
            mark(m_card->messageList());
        if (m_currentChatId == dialog)
            mark(m_chatMessages);
    });
    connect(m_backend, &Backend::chatTypingChanged, this, &MainWindow::handleChatTyping);
    connect(m_backend, &Backend::chatActionSucceeded, this, [this](const QString &dialog, const QString &message) {
        if (message.isEmpty())
            return;
        if (m_card && m_card->taskChatDialogId() == dialog)
            m_card->setStatus(message);
        setStatus(message);
    });
    connect(m_backend, &Backend::messageEditAccepted, this,
            [this](const QString &dialog, const QString &id, const QString &text) {
        if (m_chatComposer->matchesEdit(dialog, id))
            m_chatComposer->acceptMessageEdit(text);
        if (m_card && m_card->composer()->matchesEdit(dialog, id))
            m_card->composer()->acceptMessageEdit(text);
        if (m_currentChatId == dialog)
            static_cast<ui::HistoryList *>(m_chatMessages)->confirmMessageEdit(id, text);
        if (m_card && m_card->taskChatDialogId() == dialog)
            static_cast<ui::HistoryList *>(m_card->messageList())->confirmMessageEdit(id, text);
    });
    connect(m_backend, &Backend::chatOwnReactionChanged, this,
            [this](const QString &dialog, const QString &id, const QString &code, bool present) {
        if (present)
            recordConfirmedReaction(dialog, id, code);
        if (m_currentChatId == dialog)
            static_cast<ui::HistoryList *>(m_chatMessages)->confirmMessageReaction(id, code, present);
        if (m_card && m_card->taskChatDialogId() == dialog)
            static_cast<ui::HistoryList *>(m_card->messageList())->confirmMessageReaction(id, code, present);
    });
    connect(m_backend, &Backend::groupChatCreated, this, [this](const QString &) {
        if (m_lastSection == 0 && m_chatScope == 0)
            m_backend->requestRecentChats();
        setStatus(QStringLiteral("Групповой чат создан"));
    });
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
    connect(m_backend, &Backend::taskLinkPreviewInvalidated, this, &MainWindow::invalidateTaskLinkPreview);
    connect(m_backend, &Backend::taskUpdated, this, &MainWindow::invalidateTaskLinkPreview);
    connect(m_backend, &Backend::taskUpdated, this, &MainWindow::onTaskUpdated);
    connect(m_backend, &Backend::taskDeleted, this, &MainWindow::onTaskDeleted);
    connect(m_backend, &Backend::taskCreated, this, &MainWindow::onTaskCreated);
    connect(m_backend, &Backend::chatsFound, this,
            [this](const auto &query, const auto &chats) { onChatsFound(0, query, chats); });
    connect(m_backend, &Backend::taskChatsFound, this,
            [this](const auto &query, const auto &chats) { onChatsFound(1, query, chats); });
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
    resetLinkPreviews();
    bindMessageFiles(card->messageList(), true);
    auto *history = static_cast<ui::HistoryList *>(card->messageList());
    history->setOnViewportChanged([this] { scheduleVisibleChatRead(); });
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
        return m_card == card && m_cardDetailsReady && editLatestMessage(history, card->taskChatDialogId());
    });
    history->setMessageMenu([this, card](QMenu &menu, const QPersistentModelIndex &index, const QString &href) {
        if (m_card == card && m_cardDetailsReady)
            addMessageActions(menu, index, card->taskChatDialogId(), true, href);
    });
    history->setReactionActions([this, card](const QModelIndex &index, const QString &code, const QPoint &anchor) {
        if (m_card == card && m_cardDetailsReady)
            actOnMessageReaction(index, card->taskChatDialogId(), code, anchor, card);
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
            requestMessageSearch(card->taskId(), query, true);
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
        resetLinkPreviews();
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
    invalidateTaskLinkPreview(taskId);
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

void MainWindow::onChatListLoaded(int scope, const QVector<Chat> &chats)
{
    auto &directory = m_chatDirectories[scope];
    directory.recent = chats;
    directory.loaded = true;
    updateChatUnreadBadges();
    if (m_lastSection == 0 && scope == m_chatScope && directory.query.isEmpty())
        showChatResults(chats, true, true);
    if (scope == 0 && m_lastSection == 0 && m_chatScope == 0 && !m_taskChatBadgesRequested
        && !m_chatDirectories[1].loaded && m_sessionRecovery->isHidden()) {
        m_taskChatBadgesRequested = true;
        m_backend->requestTaskChatBadges();
    }
}

void MainWindow::updateChatUnreadBadges()
{
    QHash<QString, qint64> serverIds;
    for (const auto &directory : m_chatDirectories) {
        for (const auto &chat : directory.recent) {
            if (!chat.id.isEmpty() && chat.serverChatId > 0)
                serverIds.insert(chat.id, chat.serverChatId);
        }
    }
    struct Counter {
        int unread = 0;
        int source = -1;
    };
    QHash<QString, Counter> counters;
    for (int scope = 0; scope < 2; ++scope) {
        for (const auto &chat : m_chatDirectories[scope].recent) {
            if (chat.id.isEmpty())
                continue;
            auto id = serverIds.value(chat.id);
            if (id <= 0 && chat.id.startsWith(QLatin1String("chat")))
                id = portalTaskIdentifier(chat.id.mid(4)).toLongLong();
            const auto key = id > 0 ? QStringLiteral("server:%1").arg(id) : QStringLiteral("dialog:") + chat.id;
            // A row from the dedicated task list supersedes older copies in the ordinary list.
            const int source = scope == 1 ? 2 : int(chat.taskChat || !chat.taskId.isEmpty());
            const auto unread = qMax(chat.unread, int(chat.markedUnread));
            auto &counter = counters[key];
            if (source > counter.source)
                counter = {unread, source};
            else if (source == counter.source)
                counter.unread = qMax(counter.unread, unread);
        }
    }
    qint64 counts[2]{};
    for (const auto &counter : counters)
        counts[counter.source > 0 ? 1 : 0] += counter.unread;
    for (int scope = 0; scope < 2; ++scope)
        setChatTabUnreadCount(scope, counts[scope]);
    setRailUnreadCount(0, counts[0] + counts[1]);
}

void MainWindow::handleChatTyping(const QString &dialog, const QString &user, int duration)
{
    auto *task = m_card && m_card->isVisible() && m_card->taskChatDialogId() == dialog ? m_card->composer() : nullptr;
    auto *chat = m_lastSection == 0 && m_currentChatId == dialog && m_chatComposer->isVisible()
        ? m_chatComposer : nullptr;
    if (!task && !chat)
        return;

    const auto id = portalTaskIdentifier(user);
    const auto &writer = id.isEmpty() ? user : id;
    QString name;
    const QString *cached = nullptr;
    if (duration > 0 && !id.isEmpty()) {
        const auto &recent = m_chatDirectories[0].recent;
        const auto contact = std::find_if(recent.cbegin(), recent.cend(), [&id](const Chat &entry) {
            return entry.id == id && !entry.taskChat && !entry.title.trimmed().isEmpty();
        });
        if (contact != recent.cend())
            name = contact->title.simplified().left(96);
        if (name.isEmpty()) {
            cached = m_typingNames.object(id);
            if (cached)
                name = *cached;
        }
    }
    for (auto *composer : {task, chat})
        if (composer)
            composer->setTypingIndicator(writer, duration, name);

    if (duration > 0 && !id.isEmpty() && name.isEmpty() && !cached && !isMinimized()
        && m_typingNamesPending.size() < 8 && !m_typingNamesPending.contains(id)) {
        m_typingNamesPending.insert(id);
        m_backend->requestPerson(id);
    }
}

void MainWindow::rememberTypingName(const QString &user, const QString &name)
{
    if (!m_typingNamesPending.remove(user))
        return;
    const auto display = name.simplified().left(96);
    m_typingNames.insert(user, new QString(display));
    if (m_card && m_card->isVisible())
        m_card->composer()->setTypingName(user, display);
    if (m_lastSection == 0 && m_chatComposer->isVisible())
        m_chatComposer->setTypingName(user, display);
}

void MainWindow::setChatTabUnreadCount(int scope, qint64 count)
{
    if (m_chatTabs->tabData(scope).toLongLong() == count)
        return;
    m_chatTabs->setTabData(scope, count);
    auto *badge = static_cast<QLabel *>(m_chatTabs->tabButton(scope, QTabBar::RightSide));
    if (count == 0) {
        badge->hide();
        badge->clear();
        badge->setAccessibleName({});
        m_chatTabs->setTabToolTip(scope, {});
        return;
    }
    const auto detail = QStringLiteral("Непрочитанных сообщений · %1").arg(count);
    badge->setText(count > 99 ? QStringLiteral("99+") : QString::number(count));
    badge->setAccessibleName(detail);
    badge->show();
    m_chatTabs->setTabToolTip(scope, m_chatTabs->tabText(scope) + QStringLiteral(" · ") + detail);
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

void MainWindow::onChatsFound(int scope, const QString &query, const QVector<Chat> &chats)
{
    auto &directory = m_chatDirectories[scope];
    if (query.isEmpty() || query != directory.query)
        return;
    directory.found = chats;
    if (m_lastSection == 0 && scope == m_chatScope && query == m_chatSearch->text().trimmed()) {
        ui::setStatusLabel(m_chatSearchProgress, {});
        showChatResults(chats);
    }
}

void MainWindow::requestCurrentChatList()
{
    if (!m_chatDirectories[m_chatScope].loaded) {
        m_chats->clear();
        workspace::loadingRow(m_chats, QStringLiteral("Загружаем чаты…"));
    }
    if (m_chatScope == 0)
        m_backend->requestRecentChats();
    else
        m_backend->requestTaskChats();
}

void MainWindow::closeCurrentChat(bool scopeChanged)
{
    m_chatComposer->dismissEmoji();
    switchDraft(m_chatComposer, m_chatDrafts, m_currentChatId, {});
    if (scopeChanged || !m_currentChatId.isEmpty())
        emit historyClosed(false);
    m_currentChatId.clear();
    static_cast<ui::HistoryList *>(m_chatMessages)->resetHistory();
    resetMessageContext();
    clearMessageSearch();
    m_chatTitle->setText(m_chatTabs->tabText(m_chatScope));
    m_chatTitle->setToolTip({});
    m_chatAvatar->setTitle({});
    m_chatContactIsPerson = false;
    updateChatContactPresentation();
    m_chatPinned->setMessages({});
    m_chatAvatarUrl.clear();
    m_openChatTaskId.clear();
    m_openChatTask->hide();
    m_chatComposer->hide();
    showEmpty(m_chatMessages, m_chatEmpty, QStringLiteral("Выберите чат"));
    updateComposers();
}

void MainWindow::onChatScopeChanged(int scope)
{
    if (scope < 0 || scope > 1 || scope == m_chatScope)
        return;
    m_chatSearchTimer->stop();
    ui::setStatusLabel(m_chatSearchProgress, {});
    m_chatDirectories[m_chatScope].input = m_chatSearch->text();
    m_chatScope = scope;
    closeCurrentChat(true);
    auto &directory = m_chatDirectories[scope];
    {
        const QSignalBlocker blocker(m_chatSearch);
        m_chatSearch->setText(directory.input);
    }
    const auto searchLabel = scope == 0 ? QStringLiteral("Поиск чатов") : QStringLiteral("Поиск чатов задач");
    m_chatSearch->setAccessibleName(searchLabel);
    m_chatSearch->setPlaceholderText(scope == 0 ? searchLabel : QStringLiteral("Название или номер задачи"));
    m_createGroupChat->setVisible(scope == 0 && !m_readOnly && !m_backend->isDemo());
    m_chats->setAccessibleName(scope == 0 ? QStringLiteral("Недавние чаты") : QStringLiteral("Чаты задач"));
    setStatus({});
    if (!directory.query.isEmpty()) {
        const auto &initial = std::any_of(directory.found.cbegin(), directory.found.cend(), [&directory](const Chat &chat) {
            return chat.id == directory.selected;
        }) ? directory.found : directory.recent;
        showChatResults(initial, false);
    }
    showChatResults(directory.query.isEmpty() ? directory.recent : directory.found);
    if (!m_chatSearch->text().trimmed().isEmpty())
        runSearchChats();
    else
        requestCurrentChatList();
}

void MainWindow::requestMessageSearch(const QString &scope, const QString &query, bool task)
{
    const auto operation = operationKey(task ? OperationKind::SearchTaskChat : OperationKind::SearchChat, scope);
    if (m_messageSearches.contains(operation))
        return;
    m_messageSearches.insert(operation, query);
    m_backend->searchMessages(scope, query, task);
}

void MainWindow::onMessagesFound(const QString &scopeId, const QString &query, bool task,
                                 const QVector<Message> &messages)
{
    const auto operation = operationKey(task ? OperationKind::SearchTaskChat : OperationKind::SearchChat, scopeId);
    if (m_messageSearches.value(operation) != query)
        return;
    m_messageSearches.remove(operation);
    if (task) {
        if (!m_card || m_card->taskId() != scopeId || m_cardSearchQuery.isEmpty())
            return;
        if (query != m_cardSearchQuery) {
            requestMessageSearch(scopeId, m_cardSearchQuery, true);
        } else if (query == m_card->searchQuery()) {
            m_card->showMessages(messages, ui::MessageView::Search);
            updateAvatarRows();
            scheduleAvatars();
        }
        return;
    }
    if (m_lastSection != 0 || scopeId != m_messageSearchScope || !m_messageSearchActive)
        return;
    if (query != m_messageSearchQuery)
        requestMessageSearch(scopeId, m_messageSearchQuery, false);
    else if (query == m_messageSearch->text().trimmed())
        showMessageResults(messages);
}

void MainWindow::onFileUploaded(const QString &scope, const QString &kind)
{
    const auto operation = operationKey(kind == QStringLiteral("task") ? OperationKind::AttachTask
        : kind == QStringLiteral("task-chat") ? OperationKind::UploadTaskChat : OperationKind::UploadChat, scope);
    if (m_pendingUpload != operation)
        return;
    if (m_pendingPastedChatImage.image) {
        const auto sent = std::exchange(m_pendingPastedChatImage, {});
        if (kind == QStringLiteral("task-chat")) {
            clearSentDraft(m_card ? m_card->composer() : nullptr, m_taskDrafts,
                           m_card ? m_card->taskId() : QString{}, scope, sent);
        } else {
            clearSentDraft(m_chatComposer, m_chatDrafts, m_currentChatId, scope, sent);
        }
    }
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
        updateComposers();
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
    if (kind != QStringLiteral("chat") && m_card && m_card->taskId() == scope) {
        m_card->setStatus(status);
        if (kind == QStringLiteral("task"))
            m_backend->requestTask(scope);
        else if (m_cardSearchQuery.isEmpty())
            m_backend->requestTaskMessages(scope);
    } else {
        setStatus(status);
        if (kind == QStringLiteral("chat") && m_currentChatId == scope && !m_messageSearchActive)
            m_backend->requestChatMessages(scope);
    }
    updateComposers();
    updateWriteAvailability();
}

void MainWindow::runSearchChats()
{
    m_chatSearchTimer->stop();
    if (m_lastSection != 0)
        return;
    const auto query = m_chatSearch->text().trimmed();
    if (query.isEmpty()) {
        clearChatSearch();
        return;
    }
    auto &directory = m_chatDirectories[m_chatScope];
    if (directory.query != query)
        directory.found.clear();
    directory.query = query;
    ui::setStatusLabel(m_chatSearchProgress, m_chatScope == 0 ? QStringLiteral("Ищем чаты…") : QStringLiteral("Ищем задачи…"), true);
    if (m_chatScope == 0)
        m_backend->searchChats(query);
    else
        m_backend->searchTaskChats(query);
}

void MainWindow::clearChatSearch()
{
    m_chatSearchTimer->stop();
    ui::setStatusLabel(m_chatSearchProgress, {});
    auto &directory = m_chatDirectories[m_chatScope];
    const bool hadSearch = !directory.query.isEmpty() || !m_chatSearch->text().isEmpty();
    {
        const QSignalBlocker blocker(m_chatSearch);
        m_chatSearch->clear();
    }
    directory.query.clear();
    directory.found.clear();
    if (hadSearch) {
        showChatResults(directory.recent);
        requestCurrentChatList();
    }
}

void MainWindow::runMessageSearch()
{
    m_messageSearchTimer->stop();
    if (m_lastSection != 0)
        return;
    const auto query = m_messageSearch->text().trimmed();
    if (query.isEmpty()) {
        clearMessageSearch();
        return;
    }
    if (m_currentChatId.isEmpty()) {
        setStatus(QStringLiteral("Откройте чат для поиска по истории"));
        return;
    }
    resetMessageContext();
    m_messageSearchQuery = query;
    m_messageSearchScope = m_currentChatId;
    m_messageSearchActive = true;
    static_cast<ui::HistoryList *>(m_chatMessages)->setLiveReadHistory(false);
    showEmpty(m_chatMessages, m_chatEmpty, QStringLiteral("Ищем сообщения…"), true);
    requestMessageSearch(m_currentChatId, query, false);
}

void MainWindow::clearMessageSearch()
{
    m_messageSearchTimer->stop();
    const bool hadSearch = m_messageSearchActive || !m_messageSearch->text().isEmpty();
    {
        const QSignalBlocker blocker(m_messageSearch);
        m_messageSearch->clear();
    }
    m_messageSearchQuery.clear();
    m_messageSearchScope.clear();
    m_messageSearchActive = false;
    if (hadSearch && !m_currentChatId.isEmpty()) {
        showEmpty(m_chatMessages, m_chatEmpty, QStringLiteral("Загружаем сообщения…"), true);
        m_backend->requestChatMessages(m_currentChatId);
    }
}

void MainWindow::showChatResults(const QVector<Chat> &chats, bool useSearchMode, bool backgroundRefresh)
{
    const auto &directory = m_chatDirectories[m_chatScope];
    const bool searching = useSearchMode && !directory.query.isEmpty();
    const bool taskRecent = m_chatScope == 1 && !searching;
    if (m_taskChatPressActive && taskRecent && backgroundRefresh) {
        m_taskChatRefreshDeferred = true;
        return;
    }
    m_taskChatPressActive = false;
    m_taskChatRefreshDeferred = false;
    const bool preserveViewport = taskRecent && backgroundRefresh
        && (!m_chatSource.isEmpty() || !m_retainedTaskChat.id.isEmpty());
    const auto anchor = preserveViewport ? listViewportAnchor(m_chats) : ListViewportAnchor{};
    const auto initialId = m_currentChatId.isEmpty() ? directory.selected : m_currentChatId;
    const auto *current = m_chats->currentItem();
    const int previousRow = preserveViewport && current && !m_currentChatId.isEmpty()
        && current->data(workspace::Id).toString() == m_currentChatId ? m_chats->row(current) : -1;
    const bool wasRetained = previousRow >= 0 && m_retainedTaskChat.id == initialId;
    const int retainedRow = wasRetained ? m_retainedTaskChatRow : previousRow;
    const auto selectedRect = previousRow >= 0 ? m_chats->visualItemRect(current) : QRect{};
    Chat previousTask;
    if (previousRow >= 0) {
        const auto previous = std::find_if(m_chatSource.cbegin(), m_chatSource.cend(), [&initialId](const Chat &chat) {
            return chat.taskChat && chat.id == initialId;
        });
        if (previous != m_chatSource.cend())
            previousTask = *previous;
        else if (wasRetained)
            previousTask = m_retainedTaskChat;
    }
    m_retainedTaskChat = {};
    m_retainedTaskChatRow = -1;
    auto ordered = chats;
    m_chatSource = chats;
    m_chatSourceSearching = useSearchMode;
    if (!searching && !directory.linked.id.isEmpty() &&
        std::none_of(ordered.cbegin(), ordered.cend(), [&directory](const Chat &chat) {
            return chat.id == directory.linked.id;
        })) {
        ordered.prepend(directory.linked);
    }
    const auto hidden = [this](const Chat &chat) {
        return !m_showDismissedChats && chat.dismissed && !chat.taskChat;
    };
    Chat hiddenLinked;
    if (!searching && !initialId.isEmpty() && initialId == directory.linked.id) {
        const auto match = std::find_if(ordered.cbegin(), ordered.cend(), [&initialId, &hidden](const Chat &chat) {
            return chat.id == initialId && hidden(chat);
        });
        if (match != ordered.cend())
            hiddenLinked = *match;
    }
    ordered.erase(std::remove_if(ordered.begin(), ordered.end(), hidden), ordered.end());
    if (m_unreadOnlyChats) {
        ordered.erase(std::remove_if(ordered.begin(), ordered.end(), [&previousTask](const Chat &chat) {
            return chat.unread <= 0 && !chat.markedUnread && !(chat.taskChat && chat.id == previousTask.id);
        }), ordered.end());
    }
    const auto rank = [](const Chat &chat) {
        if (chat.pinned)
            return 0;
        return chat.unread > 0 || chat.markedUnread ? 1 : 2;
    };
    std::stable_sort(ordered.begin(), ordered.end(), [&rank](const Chat &a, const Chat &b) {
        return rank(a) < rank(b);
    });
    if (!previousTask.id.isEmpty()) {
        const auto active = std::find_if(ordered.begin(), ordered.end(), [&initialId](const Chat &chat) {
            return chat.id == initialId;
        });
        if (active == ordered.end() || active->taskChat) {
            // Hold the row for the lifetime of the selection, independently of its changing badges.
            m_retainedTaskChat = active == ordered.end() ? previousTask : *active;
            m_retainedTaskChatRow = retainedRow;
            if (active != ordered.end())
                ordered.erase(active);
            const qsizetype pinnedCount = std::partition_point(ordered.cbegin(), ordered.cend(),
                [](const Chat &entry) { return entry.pinned; }) - ordered.cbegin();
            const auto first = m_retainedTaskChat.pinned ? qsizetype(0) : pinnedCount;
            const auto last = m_retainedTaskChat.pinned ? pinnedCount : ordered.size();
            ordered.insert(qBound(first, qsizetype(retainedRow), last), m_retainedTaskChat);
        }
    }
    {
        const QSignalBlocker blocker(m_chats);
        const QSignalBlocker scrollBlocker(preserveViewport ? m_chats->verticalScrollBar() : nullptr);
        m_chats->clear();
        QListWidgetItem *selected = nullptr;
        for (const auto &chat : ordered) {
            auto *item = ui::addChatRow(m_chats, chat);
            if (chat.id == initialId)
                selected = item;
        }
        if (searching || (m_unreadOnlyChats && !m_currentChatId.isEmpty()) || !hiddenLinked.id.isEmpty()) {
            if (selected)
                m_chats->setCurrentItem(selected);
        } else {
            selectAvailable(m_chats, selected);
        }
        if (ordered.isEmpty()) {
            m_projectsPanel->closeTools();
            auto text = m_chatScope == 0 ? QStringLiteral("Чатов пока нет") : QStringLiteral("Чатов задач пока нет");
            if (searching)
                text = QStringLiteral("Ничего не найдено");
            else if (m_unreadOnlyChats)
                text = QStringLiteral("Непрочитанных чатов пока нет");
            else if (!directory.loaded)
                text = QStringLiteral("Загружаем чаты…");
            if (!searching && !m_unreadOnlyChats && !directory.loaded)
                workspace::loadingRow(m_chats, text);
            else
                workspace::emptyRow(m_chats, text);
        }
        if (preserveViewport) {
            if (selected && !m_retainedTaskChat.id.isEmpty() && m_chats->viewport()->rect().intersects(selectedRect))
                restoreListViewport(m_chats, {{initialId}, selectedRect.top(), anchor.scroll});
            else
                restoreListViewport(m_chats, anchor);
        }
    }
    if (m_chats->currentItem() && !ordered.isEmpty())
        onChatSelected();
    else if (!hiddenLinked.id.isEmpty()) {
        // Explicit notification links can open a hidden chat without listing it.
        const std::unique_ptr<QListWidgetItem> item(ui::addChatRow(nullptr, hiddenLinked));
        openChatRow(*item);
    } else if (!searching && !(m_unreadOnlyChats && !m_currentChatId.isEmpty())) {
        closeCurrentChat();
    }
    updateAvatarRows();
    scheduleAvatars();
}

void MainWindow::showChatPinMenu(QListWidget *list, const QPoint &point)
{
    const auto *item = list->itemAt(point);
    if (!item)
        return;
    const auto id = item->data(workspace::Id).toString();
    if (id.isEmpty())
        return;
    const auto op = operationKey(OperationKind::ChatPin, id);
    const bool pinned = item->data(workspace::Pinned).toBool();
    auto *menu = ui::directoryPinMenu(this, pinned, [this, op] {
        return writable() && !m_pendingPins.contains(op);
    }, [this, op, id, pinned] {
        setStatus(QString());
        m_pendingPins.insert(op);
        m_backend->setChatPinned(id, !pinned);
    });
    if (!m_backend->isDemo()) {
        connect(menu->addAction(QStringLiteral("Медиа, файлы и ссылки")), &QAction::triggered,
                this, [this, id] { showChatAssets(id, this); });
    }
    if (m_chatScope == 0 && id.startsWith(QLatin1String("chat")) && !m_backend->isDemo()) {
        menu->addSeparator();
        connect(menu->addAction(QStringLiteral("Добавить участника")), &QAction::triggered,
                this, [this, id] { promptChatMember(id, true); });
        connect(menu->addAction(QStringLiteral("Удалить участника")), &QAction::triggered,
                this, [this, id] { promptChatMember(id, false); });
    }
    menu->popup(list->viewport()->mapToGlobal(point));
}

void MainWindow::scheduleAvatars()
{
    if (m_avatarUpdateQueued)
        return;
    m_avatarUpdateQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_avatarUpdateQueued = false;
        loadVisibleAvatars();
        loadVisibleChatPreviews();
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
    const auto *composer = m_card && m_card->isVisible() ? m_card->composer() : m_chatComposer;
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
        requestAvatar(m_chatAvatarUrl);
        requestVisible(m_chats);
        requestVisible(m_chatMessages);
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
        if (list == m_chatMessages || (m_card && list == m_card->messageList()))
            list->viewport()->update();
    };
    updateList(m_chats);
    m_projectsPanel->updateAvatars(updateList);
    updateList(m_chatMessages);
    if (m_card)
        updateList(m_card->messageList());
    if (const auto *photo = m_avatars.object(m_chatAvatarUrl))
        m_chatAvatar->setPhoto(*photo);
    if (m_personDialog) {
        if (const auto *photo = m_avatars.object(m_personAvatarUrl))
            m_personAvatar->setPhoto(*photo);
    }
    m_events->repaintRows();
}

bool MainWindow::isCurrentTaskLink(const QListWidget *history, const QUrl &url) const
{
    QString taskId;
    if (m_card && history == m_card->messageList())
        taskId = m_card->taskId();
    else if (history == m_chatMessages)
        taskId = m_openChatTaskId;
    const auto currentTaskId = portalTaskIdentifier(taskId);
    return !currentTaskId.isEmpty() && portalTaskUrl(url).taskId == currentTaskId;
}

void MainWindow::refreshChatMediaRows()
{
    for (auto *list : {m_chatMessages, m_card ? m_card->messageList() : nullptr}) {
        if (list && list->isVisible())
            static_cast<ui::HistoryList *>(list)->refreshRowSizes();
    }
    if (m_card) {
        m_card->descriptionList()->requestHeight();
        m_card->attachmentList()->requestHeight();
    }
}

void MainWindow::loadVisibleChatPreviews()
{
    const auto load = [this](QListWidget *list) {
        auto *delegate = static_cast<workspace::RowPainter *>(list->itemDelegate());
        const auto visible = list->viewport()->visibleRegion().boundingRect();
        if (!list->isVisible() || list->count() == 0 || visible.isEmpty()) {
            delegate->setPreviewAnimation(false);
            return;
        }
        constexpr int nearby = 125;
        const auto first = list->indexAt(QPoint(4, qMax(0, visible.top() - nearby)));
        const int start = first.isValid() ? qMax(0, first.row() - 3) : 0;
        const bool chatHistory = list == m_chatMessages || (m_card && list == m_card->messageList());
        const bool taskContent = m_card && (list == m_card->descriptionList() || list == m_card->attachmentList());
        const auto portal = m_backend->portalOrigin();
        const OriginGuard guard(portal);
        qint64 newestExternalLink = 0;
        QPair<QString, QUrl> publicCandidate;
        bool animatePreviews = false;
        const bool foreground = chatHistory && !m_publicLinkPending && !isMinimized()
            && QApplication::activeWindow() == list->window()
            && QGuiApplication::applicationState() == Qt::ApplicationActive;
        for (int row = start; row < list->count(); ++row) {
            const auto *item = list->item(row);
            const auto bounds = list->visualItemRect(item);
            if (bounds.top() > visible.bottom() + nearby)
                break;
            if (bounds.bottom() < visible.top() - nearby)
                continue;
            if (chatHistory) {
                const auto id = item->data(workspace::ReceiptMessageId).toString();
                const auto links = item->data(workspace::MessageLinks).toStringList();
                for (const auto &href : links) {
                    const auto url = resolvedChatLink(portal, href);
                    if (guard.isSameOrigin(url)) {
                        if (!isCurrentTaskLink(list, url))
                            loadMessageLinkPreview(href);
                    } else if (!url.isEmpty() && m_linkPreviewAccess && !id.isEmpty()) {
                        newestExternalLink = qMax(newestExternalLink, id.toLongLong());
                        const auto *preview = m_externalLinkHistory == list
                            ? m_externalLinkPreviews.object({id, url}) : nullptr;
                        if (preview && m_chatImagePreviews.begin(preview->thumbnailUrl))
                            m_backend->requestMessageImage(preview->thumbnailUrl);
                        if (foreground && publicCandidate.second.isEmpty() && !preview
                            && allowsAutomaticPublicPreview(list->model()->index(row, 0))
                            && bounds.intersected(visible).height() >= qMin(24, bounds.height())
                            && needsPublicLinkPreview(list, id, url))
                            publicCandidate = {id, url};
                    }
                }
            }
            const auto loadImage = [this, taskContent, &bounds, &visible](const MessageFile &file) {
                if (file.isVideo)
                    return;
                const auto url = m_chatImagePreviews.requestUrl(file.previewUrl, file.originalPreviewUrl);
                if (taskContent && url == file.downloadUrl && !bounds.intersects(visible))
                    return;
                if (m_chatImagePreviews.begin(url))
                    m_backend->requestMessageImage(url);
            };
            const auto files = item->data(workspace::MessageFiles).value<QVector<MessageFile>>();
            for (const auto &file : files)
                loadImage(file);
            if (bounds.intersects(visible)) {
                const auto index = list->model()->index(row, 0);
                if (chatHistory && m_sessionRecovery->isHidden()) {
                    if (const auto representative = delegate->visibleQuoteMedia(index, bounds, visible))
                        loadImage(*representative);
                    const auto videos = delegate->visibleVideos(index, bounds, visible);
                    for (const auto &file : videos)
                        m_chatVideoPreviews->request(file);
                }
                animatePreviews = animatePreviews || delegate->hasVisibleLoadingPreview(index, bounds, visible);
            }
        }
        delegate->setPreviewAnimation(animatePreviews);
        if (chatHistory) {
            requestExternalLinkPreviews(list, newestExternalLink);
            if (!publicCandidate.second.isEmpty())
                requestPublicLinkPreview(list, publicCandidate.first, publicCandidate.second);
        }
    };
    if (m_card) {
        static_cast<workspace::RowPainter *>(m_chatMessages->itemDelegate())->setPreviewAnimation(false);
        load(m_card->messageList());
        load(m_card->descriptionList());
        load(m_card->attachmentList());
    } else {
        load(m_chatMessages);
    }
    for (const auto &href : m_events->visiblePreviewLinks())
        loadMessageLinkPreview(href);
}

void MainWindow::requestExternalLinkPreviews(QListWidget *history, qint64 newest)
{
    if (!m_linkPreviewAccess || m_externalLinkPending || newest <= m_latestExternalLinkMessage)
        return;
    const bool task = m_card && history == m_card->messageList();
    const auto scope = task ? m_card->taskId() : m_currentChatId;
    if (scope.isEmpty())
        return;
    m_latestExternalLinkMessage = newest;
    for (int row = 0; row < history->count(); ++row)
        m_latestExternalLinkMessage = qMax(m_latestExternalLinkMessage,
                                           history->item(row)->data(workspace::ReceiptMessageId).toLongLong());
    m_externalLinkPending = true;
    m_externalLinkHistory = history;
    m_backend->requestChatLinkPreviews(scope, task,
        [window = QPointer<MainWindow>(this), generation = m_externalLinkGeneration](QVector<ChatAsset> assets) {
            if (!window)
                return;
            window->m_externalLinkPending = false;
            if (generation == window->m_externalLinkGeneration) {
                const OriginGuard guard(window->m_backend->portalOrigin());
                bool added = false;
                for (auto &asset : assets) {
                    window->m_latestExternalLinkMessage = qMax(window->m_latestExternalLinkMessage,
                                                               asset.messageId.toLongLong());
                    if (asset.messageId.isEmpty() || asset.link.isEmpty() || guard.isSameOrigin(asset.link)
                        || (asset.description.isEmpty() && asset.thumbnailUrl.isEmpty()
                            && (asset.name.isEmpty() || asset.name == asset.link.toDisplayString())))
                        continue;
                    const auto key = qMakePair(asset.messageId, asset.link);
                    window->m_externalLinkPreviews.insert(key, new ChatAsset(std::move(asset)));
                    added = true;
                }
                if (added)
                    window->scheduleLinkPreviewLayout();
            }
            window->scheduleAvatars();
        });
}

bool MainWindow::needsPublicLinkPreview(QListWidget *history, const QString &id, const QUrl &url) const
{
    if (!history || !m_online || !m_linkPreviewAccess || m_backend->isDemo() || !m_sessionRecovery->isHidden()
        || history != m_externalLinkHistory || !history->isVisible() || m_externalLinkPending
        || id.toLongLong() > m_latestExternalLinkMessage || OriginGuard(m_backend->portalOrigin()).isSameOrigin(url)
        || !ui::PublicLinkPreviewFetcher::accepts(url, !m_publicLinkRequest)
        || m_externalLinkPreviews.contains({id, url}))
        return false;
    return !id.isEmpty() && !m_publicLinkPreviews.contains(publicLinkTarget(url));
}

void MainWindow::requestPublicLinkPreview(QListWidget *history, const QString &id, const QUrl &url)
{
    if (m_publicLinkPending || !needsPublicLinkPreview(history, id, url))
        return;
    m_publicLinkPending = true;
    const auto target = publicLinkTarget(url);
    const auto finish = [window = QPointer<MainWindow>(this), target,
                         account = m_publicLinkAccountGeneration](std::optional<ui::PublicLinkMetadata> result) {
        if (!window)
            return;
        window->m_publicLinkPending = false;
        if (window->m_publicLinkAccountGeneration == account) {
            auto metadata = result.value_or(ui::PublicLinkMetadata{});
            const qsizetype imageCost = (metadata.thumbnail.sizeInBytes() + 8191) / 8192;
            const bool hasContent = !metadata.title.isEmpty() || !metadata.description.isEmpty() || imageCost > 0;
            window->m_publicLinkPreviews.insert(target, new ui::PublicLinkMetadata(std::move(metadata)), 1 + imageCost);
            if (hasContent)
                window->scheduleLinkPreviewLayout();
        }
        window->scheduleAvatars();
    };
    if (!m_publicLinkRequest && !m_publicLinkFetcher)
        m_publicLinkFetcher = new ui::PublicLinkPreviewFetcher(this);
    const bool started = m_publicLinkRequest ? m_publicLinkRequest(target, finish) : m_publicLinkFetcher->request(target, finish);
    if (!started)
        finish(std::nullopt);
}

void MainWindow::addPublicLinkPreviewActions(QMenu &menu, const QPersistentModelIndex &message,
                                             const std::function<QPersistentModelIndex()> &resolve)
{
    auto *history = m_externalLinkHistory.data();
    if (!history || history->model() != message.model() || message.data(workspace::SystemMessage).toBool()
        || allowsAutomaticPublicPreview(message))
        return;
    const auto id = message.data(workspace::ReceiptMessageId).toString();
    const auto links = message.data(workspace::MessageLinks).toStringList();
    QVector<QPair<QString, QUrl>> available;
    for (const auto &href : links) {
        const auto url = resolvedChatLink(m_backend->portalOrigin(), href);
        if (needsPublicLinkPreview(history, id, url))
            available.append({href, url});
    }
    for (const auto &[href, url] : available) {
        auto caption = QStringLiteral("Загрузить предпросмотр ссылки");
        if (available.size() > 1)
            caption += QStringLiteral(" · %1").arg(url.toDisplayString(QUrl::RemoveQuery | QUrl::RemoveFragment).left(96));
        auto *action = menu.addAction(caption);
        action->setEnabled(!m_publicLinkPending);
        connect(action, &QAction::triggered, this, [this, history = QPointer<QListWidget>(history), resolve, id, href, url] {
            const auto message = resolve();
            if (message.isValid() && message.data(workspace::ReceiptMessageId).toString() == id
                && !allowsAutomaticPublicPreview(message) && !message.data(workspace::SystemMessage).toBool()
                && message.data(workspace::MessageLinks).toStringList().contains(href))
                requestPublicLinkPreview(history, id, url);
        });
    }
}

void MainWindow::loadMessageLinkPreview(const QString &href)
{
    constexpr qsizetype concurrentLinkPreviews = 3;
    if (!m_linkPreviewAccess)
        return;
    const auto portal = m_backend->portalOrigin();
    const auto url = resolvedChatLink(portal, href);
    if (url.isEmpty() || !OriginGuard(portal).isSameOrigin(url))
        return;
    if (const auto *preview = m_chatLinkPreviews.object(url)) {
        if (!preview->title.isEmpty()) {
            requestAvatar(preview->imageUrl());
            requestAvatar(preview->creatorAvatarUrl);
            requestAvatar(preview->assigneeAvatarUrl);
        }
        return;
    }
    if (m_linkPreviewRequests >= concurrentLinkPreviews || m_pendingLinkPreviews.contains(url)
        || !portalLinkPreviewTarget(portal, url))
        return;
    const auto request = ++m_lastLinkPreviewRequest;
    m_pendingLinkPreviews.insert(url, request);
    ++m_linkPreviewRequests;
    m_backend->requestMessageLinkPreview(url,
        [window = QPointer<MainWindow>(this), url, request](MessageLinkPreview preview) {
            if (!window)
                return;
            --window->m_linkPreviewRequests;
            auto &pending = window->m_pendingLinkPreviews;
            const auto current = pending.find(url);
            if (current != pending.end() && current.value() == request) {
                pending.erase(current);
                const bool available = !preview.title.isEmpty();
                window->m_chatLinkPreviews.insert(url, new MessageLinkPreview(std::move(preview)));
                if (available)
                    window->scheduleLinkPreviewLayout();
            }
            window->scheduleAvatars();
        });
}

void MainWindow::invalidateTaskLinkPreview(const QString &taskId)
{
    if (taskId.isEmpty())
        return;
    const auto refersToTask = [&taskId](const QUrl &url) { return portalTaskUrl(url).taskId == taskId; };
    const auto removedPending = m_pendingLinkPreviews.removeIf(
        [&refersToTask](auto entry) { return refersToTask(entry.key()); });
    bool removedCached = false;
    bool removedCard = false;
    for (const auto &url : m_chatLinkPreviews.keys()) {
        if (!refersToTask(url))
            continue;
        const std::unique_ptr<MessageLinkPreview> preview(m_chatLinkPreviews.take(url));
        removedCached = true;
        removedCard = removedCard || (preview && !preview->title.isEmpty());
    }
    if (removedCard)
        scheduleLinkPreviewLayout();
    if (m_linkPreviewAccess && (removedCached || removedPending))
        scheduleAvatars();
}

void MainWindow::resetLinkPreviews()
{
    ++m_externalLinkGeneration;
    m_latestExternalLinkMessage = 0;
    m_externalLinkHistory.clear();
    m_pendingLinkPreviews.clear();
    if (!m_chatLinkPreviews.isEmpty() || !m_externalLinkPreviews.isEmpty()) {
        m_chatLinkPreviews.clear();
        m_externalLinkPreviews.clear();
        refreshLinkPreviewRows();
    }
    if (m_linkPreviewAccess)
        scheduleAvatars();
}

void MainWindow::scheduleLinkPreviewLayout()
{
    if (m_linkPreviewLayoutQueued)
        return;
    m_linkPreviewLayoutQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_linkPreviewLayoutQueued = false;
        refreshLinkPreviewRows();
    });
}

void MainWindow::refreshLinkPreviewRows()
{
    for (auto *list : {m_chatMessages, m_card ? m_card->messageList() : nullptr}) {
        if (list)
            static_cast<ui::HistoryList *>(list)->refreshRowSizes();
    }
    m_events->refreshPreviewRows();
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
            openLinkedChat(target);
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

void MainWindow::openLinkedChat(const NotificationTarget &target)
{
    int scope = target.taskChat ? 1 : 0;
    Chat linkedChat;
    for (int candidate = 0; candidate < 2; ++candidate) {
        const auto &directory = m_chatDirectories[candidate];
        for (const auto *list : {&directory.recent, &directory.found}) {
            const auto match = std::find_if(list->cbegin(), list->cend(), [&target](const Chat &chat) {
                return chat.id == target.id;
            });
            if (match != list->cend()) {
                scope = candidate;
                linkedChat = *match;
            }
        }
    }
    const bool sectionWillReloadChat = m_lastSection != 0 && m_chatScope == scope &&
        m_currentChatId == target.id && !m_messageSearchActive && m_contextDialogId.isEmpty();
    m_sections->setCurrentRow(0);
    m_chatTabs->setCurrentIndex(scope);
    auto &directory = m_chatDirectories[scope];
    m_chatSearchTimer->stop();
    m_messageSearchTimer->stop();
    if (m_messageSearchActive)
        showEmpty(m_chatMessages, m_chatEmpty, QStringLiteral("Загружаем сообщение…"), true);
    {
        const QSignalBlocker blocker(m_messageSearch);
        m_messageSearch->clear();
    }
    m_messageSearchActive = false;
    m_messageSearchQuery.clear();
    m_messageSearchScope.clear();
    {
        const QSignalBlocker blocker(m_chatSearch);
        m_chatSearch->clear();
    }
    directory.input.clear();
    directory.query.clear();
    directory.found.clear();
    ui::setStatusLabel(m_chatSearchProgress, {});
    if (linkedChat.id.isEmpty()) {
        linkedChat.id = target.id;
        linkedChat.title = QStringLiteral("Диалог %1").arg(target.id);
        linkedChat.taskChat = scope == 1;
    }
    directory.linked = linkedChat;
    if (target.messageId.isEmpty()) {
        resetMessageContext();
    } else if (m_currentChatId != target.id) {
        m_contextDialogId = target.id;
        m_contextMessageId = target.messageId;
    }
    const auto oldChat = m_currentChatId;
    if (oldChat != target.id) {
        switchDraft(m_chatComposer, m_chatDrafts, oldChat, {});
        m_currentChatId.clear();
        static_cast<ui::HistoryList *>(m_chatMessages)->resetHistory();
    }
    directory.selected = target.id;
    showChatResults(directory.recent);
    if (target.messageId.isEmpty()) {
        if (oldChat == target.id && !sectionWillReloadChat)
            m_backend->requestChatMessages(target.id);
        focusChatComposer();
        return;
    }
    openChatMessageContext(target.id, target.messageId);
}

void MainWindow::openChatMessageContext(const QString &dialogId, const QString &messageId)
{
    const auto target = portalTaskIdentifier(messageId);
    if (m_lastSection != 0 || dialogId.isEmpty() || dialogId != m_currentChatId || target.isEmpty())
        return;
    const bool searching = m_messageSearchActive;
    m_messageSearchTimer->stop();
    {
        const QSignalBlocker search(m_messageSearch);
        const QSignalBlocker toggle(m_messageSearchToggle);
        m_messageSearch->clear();
        m_messageSearchToggle->setChecked(false);
        m_messageSearch->parentWidget()->hide();
    }
    m_messageSearchActive = false;
    m_messageSearchQuery.clear();
    m_messageSearchScope.clear();
    if (!searching && focusChatMessage(target))
        return;
    m_contextDialogId = dialogId;
    m_contextMessageId = target;
    m_latestChat->show();
    static_cast<ui::HistoryList *>(m_chatMessages)->setLiveReadHistory(false);
    showEmpty(m_chatMessages, m_chatEmpty, QStringLiteral("Загружаем сообщение…"), true);
    m_backend->requestMessageContext(dialogId, target);
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

void MainWindow::resetMessageContext()
{
    m_contextDialogId.clear();
    m_contextMessageId.clear();
    if (m_chatMessages)
        static_cast<ui::HistoryList *>(m_chatMessages)->returnToLatest();
    if (m_latestChat)
        m_latestChat->hide();
}

bool MainWindow::focusChatMessage(const QString &id)
{
    return static_cast<ui::HistoryList *>(m_chatMessages)->focusOnMessage(id);
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

void MainWindow::showMessageResults(const QVector<Message> &messages)
{
    static_cast<ui::HistoryList *>(m_chatMessages)->setLiveReadHistory(false);
    presentMessages(m_chatMessages, m_chatEmpty, messages, ui::MessageView::Search);
    updateAvatarRows();
    scheduleAvatars();
}

void MainWindow::openMessageViewers(QListWidget *history, const QString &messageId, const QPoint &anchor)
{
    const bool task = m_card && history == m_card->messageList();
    if (!task && history != m_chatMessages)
        return;
    const auto scope = task ? m_card->taskId() : m_currentChatId;
    if (scope.isEmpty() || portalTaskIdentifier(messageId).isEmpty())
        return;

    const auto *screen = QGuiApplication::screenAt(anchor);
    if (!screen)
        screen = history->screen();
    if (!screen || screen->availableGeometry().isEmpty())
        return;
    const auto available = screen->availableGeometry();

    const auto sameHistory = [window = QPointer<MainWindow>(this), source = QPointer<QListWidget>(history),
                              scope, task, directory = m_chatScope] {
        return window && source && source->isVisible()
            && (task ? window->m_card && source == window->m_card->messageList() && scope == window->m_card->taskId()
                     : window->m_lastSection == 0 && source == window->m_chatMessages
                       && directory == window->m_chatScope && scope == window->m_currentChatId);
    };

    auto *menu = new QFrame(history, Qt::Popup);
    menu->setObjectName(QStringLiteral("messageViewersMenu"));
    menu->setAccessibleName(QStringLiteral("Кто прочитал сообщение"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setFrameShape(QFrame::StyledPanel);
    menu->setAutoFillBackground(true);
    menu->setMinimumWidth(qMin(260, available.width()));
    menu->setMaximumSize(available.size());
    auto *layout = new QVBoxLayout(menu);
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    layout->setContentsMargins(10, 8, 10, 8);
    auto *heading = new QLabel(QStringLiteral("Просмотрели сообщение"), menu);
    heading->setFont(workspace::rowFont(ui::bodyTextPixels, true));
    heading->setWordWrap(true);
    layout->addWidget(heading);
    auto *status = new QLabel(QStringLiteral("Загружаем список…"), menu);
    status->setObjectName(QStringLiteral("messageViewersStatus"));
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    layout->addWidget(status);
    auto *rows = new QListWidget(menu);
    rows->setAccessibleName(QStringLiteral("Список прочитавших"));
    rows->setObjectName(QStringLiteral("messageViewersList"));
    rows->setIconSize(QSize(22, 22));
    rows->setSelectionMode(QAbstractItemView::NoSelection);
    rows->setFrameShape(QFrame::NoFrame);
    rows->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    const int rowWidth = qBound(1, available.width() - 22, 230);
    rows->setMinimumWidth(rowWidth);
    rows->hide();
    layout->addWidget(rows);
    auto *actions = new QHBoxLayout;
    auto *retry = new QPushButton(QStringLiteral("Повторить"), menu);
    retry->setAccessibleName(QStringLiteral("Повторить загрузку списка прочитавших"));
    retry->setAutoDefault(false);
    retry->hide();
    actions->addWidget(retry);
    actions->addStretch();
    auto *close = new QPushButton(QStringLiteral("Закрыть"), menu);
    close->setAutoDefault(false);
    actions->addWidget(close);
    layout->addLayout(actions);
    connect(close, &QPushButton::clicked, menu, &QWidget::close);
    connect(m_backend, &Backend::accountScopeChanged, menu, &QWidget::close);
    if (!task) {
        const auto closeIfScopeChanged = [menu, sameHistory] {
            if (!sameHistory())
                menu->close();
        };
        connect(m_chats, &QListWidget::currentItemChanged, menu, closeIfScopeChanged, Qt::QueuedConnection);
        connect(m_chatTabs, &QTabBar::currentChanged, menu, closeIfScopeChanged, Qt::QueuedConnection);
        connect(m_stack, &QStackedWidget::currentChanged, menu, closeIfScopeChanged, Qt::QueuedConnection);
    }
    connect(m_backend, &Backend::messageLinkPreviewAccessChanged, menu, [menu](bool available) {
        if (!available)
            menu->close();
    });

    const auto refreshAvatars = [this, rows] {
        if (!rows->isVisible())
            return;
        for (int row = 0; row < rows->count(); ++row) {
            auto *item = rows->item(row);
            if (!rows->viewport()->rect().intersects(rows->visualItemRect(item)))
                continue;
            const auto url = item->data(workspace::AvatarUrl).toUrl();
            const auto *photo = m_avatars.object(url);
            QPixmap pixmap(22, 22);
            pixmap.fill(Qt::transparent);
            {
                QPainter painter(&pixmap);
                painter.setRenderHint(QPainter::Antialiasing);
                workspace::paintAvatar(painter, pixmap.rect(), item->text(), workspace::AvatarKind::Person,
                                       photo ? *photo : QPixmap{});
            }
            item->setIcon(QIcon(pixmap));
            requestAvatar(url);
        }
    };
    connect(rows->verticalScrollBar(), &QScrollBar::valueChanged, menu, refreshAvatars);
    connect(m_backend, &Backend::avatarLoaded, menu, refreshAvatars, Qt::QueuedConnection);
    constexpr int rowHeight = 30;
    const auto positionPopup = [anchor, available, menu, layout, rows] {
        layout->invalidate();
        const int width = qBound(menu->minimumWidth(), layout->sizeHint().width(), available.width());
        const auto heightForWidth = [layout, width] {
            const int wrapped = layout->totalHeightForWidth(width);
            return wrapped >= 0 ? wrapped : layout->sizeHint().height();
        };
        if (!rows->isHidden()) {
            const int wanted = qMin(rows->count(), 6) * rowHeight + 2;
            rows->setFixedHeight(0);
            layout->invalidate();
            rows->setFixedHeight(qBound(0, available.height() - heightForWidth(), wanted));
            layout->invalidate();
        }
        const int height = qMin(available.height(), heightForWidth());
        menu->resize(width, height);
        layout->activate();
        const int maxX = available.right() - width + 1;
        const int maxY = available.bottom() - height + 1;
        const int y = anchor.y() > maxY ? anchor.y() - height : anchor.y();
        menu->move(qBound(available.left(), anchor.x(), maxX), qBound(available.top(), y, maxY));
    };

    const auto request = [this, scope, messageId, task, sameHistory, popup = QPointer<QFrame>(menu),
                          status, heading, rows, retry, rowWidth, refreshAvatars, positionPopup] {
        if (!popup || !popup->isVisible())
            return;
        if (!sameHistory()) {
            popup->close();
            return;
        }
        status->setText(QStringLiteral("Загружаем список…"));
        status->show();
        rows->hide();
        retry->setEnabled(false);
        retry->hide();
        positionPopup();
        m_backend->requestMessageViewers(scope, messageId, task,
            [popup, sameHistory, status, heading, rows, retry, rowWidth, refreshAvatars, positionPopup]
            (std::optional<MessageViewers> result) {
                if (!popup || !popup->isVisible())
                    return;
                if (!sameHistory()) {
                    popup->close();
                    return;
                }
                if (!result) {
                    status->setText(QStringLiteral("Не удалось получить список"));
                    retry->setEnabled(true);
                    retry->show();
                } else if (result->people.isEmpty()) {
                    status->setText(QStringLiteral("Портал ещё не подтвердил просмотры"));
                } else {
                    if (result->count >= 0)
                        heading->setText(QStringLiteral("Просмотрели сообщение · %1").arg(result->count));
                    rows->clear();
                    for (const auto &person : result->people) {
                        auto *row = new QListWidgetItem(person.name, rows);
                        row->setData(workspace::AvatarUrl, person.avatarUrl);
                        row->setSizeHint(QSize(rowWidth, rowHeight));
                    }
                    rows->show();
                    if (result->hasMore)
                        status->setText(QStringLiteral("Показаны первые %1").arg(rows->count()));
                    else
                        status->hide();
                }
                positionPopup();
                refreshAvatars();
            });
    };
    connect(retry, &QPushButton::clicked, menu, request);
    positionPopup();
    menu->show();
    request();
}

void MainWindow::bindMessageFiles(QListWidget *list, bool linkCards)
{
    auto *delegate = static_cast<workspace::RowPainter *>(list->itemDelegate());
    delegate->setMessageViewersHandler([this, list](const QString &id, const QPoint &anchor) {
        openMessageViewers(list, id, anchor);
    });
    delegate->setMessagePreviewSource([this](const QUrl &url) { return m_chatImagePreviews.photo(url); });
    delegate->setVideoPreviewSource([this](const MessageFile &file) { return m_chatVideoPreviews->poster(file); });
    delegate->setPreviewPendingSource([this](const MessageFile &file) {
        if (file.isVideo)
            return m_chatVideoPreviews->posterPending(file);
        return m_chatImagePreviews.pending(m_chatImagePreviews.requestUrl(file.previewUrl, file.originalPreviewUrl));
    });
    delegate->setMessageVideoHandler([this, list](const QModelIndex &index, const MessageFile &file) {
        playMessageVideo(file, list->window(), [list = QPointer<QListWidget>(list),
                         current = QPersistentModelIndex(index), file] {
            return list && list->isVisible() && current.isValid()
                && workspace::RowPainter::hasOriginal(current, file);
        });
    });
    delegate->setMessagePhotoHandler([this, list](const MessageFile &file) {
        QVector<MessageFile> loaded;
        if (dynamic_cast<ui::HistoryList *>(list)) {
            for (int row = 0; row < list->count(); ++row) {
                const auto *item = list->item(row);
                if (item->isHidden())
                    continue;
                loaded += item->data(workspace::MessageFiles).value<QVector<MessageFile>>();
            }
        }
        openMessagePhoto(file, list->window(), loaded);
    });
    const auto open = [this, list](const MessageFile &file) { saveMessageFile(file, list->window()); };
    delegate->setMessageDownloadHandler(open);
    delegate->setMessagePhotoMenuHandler([this, list, delegate](const QModelIndex &index, const MessageFile &file,
                                                      const QRect &anchor) {
        const auto *thumb = delegate->filePreview(file);
        const auto pixels = thumb && !file.isVideo ? thumb->toImage() : QImage();
        const auto samePhoto = [this, list, current = QPersistentModelIndex(index), file,
                                account = m_backend->settingsScope()] {
            return account == m_backend->settingsScope() && list->isVisible() && current.isValid()
                && workspace::RowPainter::hasPhotoPreview(current, file);
        };
        std::function<void()> download;
        if (workspace::RowPainter::downloadable(file) || !pixels.isNull()) {
            download = [this, list, samePhoto, file, pixels] {
                if (samePhoto())
                    saveMessageFile(file, list->window(), pixels);
            };
        }
        std::function<QImage()> copy;
        if (!pixels.isNull())
            copy = [samePhoto, pixels] { return samePhoto() ? pixels : QImage(); };
        QMenu *menu = nullptr;
        if (file.isVideo) {
            menu = new QMenu(list);
            connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
            auto *save = menu->addAction(QStringLiteral("Сохранить видео"));
            save->setEnabled(bool(download));
            if (download)
                connect(save, &QAction::triggered, menu, std::move(download));
        } else {
            menu = ui::createImageContextMenu(list, std::move(download), std::move(copy));
        }
        connect(m_backend, &Backend::accountScopeChanged, menu, &QMenu::close);
        connect(list->model(), &QAbstractItemModel::modelReset, menu, &QMenu::close);
        const auto closeIfStale = [menu, samePhoto] {
            if (!samePhoto())
                menu->close();
        };
        connect(list->model(), &QAbstractItemModel::rowsRemoved, menu, closeIfStale);
        connect(list->model(), &QAbstractItemModel::dataChanged, menu, closeIfStale);
        ui::showMessageMenuWindow(*menu, anchor);
    });
    delegate->setMessageLinkHandler([this, list](const QString &href) { openMessageLink(href, list); });
    if (linkCards) {
        delegate->setLinkAvatarSource([this](const QUrl &url) { return m_avatars.object(url); });
        delegate->setLinkThumbnailSource([this](const QUrl &url) -> const QImage * {
            const auto *preview = m_publicLinkPreviews.object(url);
            return preview && !preview->thumbnail.isNull() ? &preview->thumbnail : nullptr;
        });
        delegate->setLinkPreviewSource([this, list](const QModelIndex &index, const QString &href) -> MessageLinkPreview {
            const auto portal = m_backend->portalOrigin();
            const auto url = resolvedChatLink(portal, href);
            if (url.isEmpty())
                return {};
            if (OriginGuard(portal).isSameOrigin(url)) {
                if (isCurrentTaskLink(list, url))
                    return {};
                const auto *preview = m_chatLinkPreviews.object(url);
                return preview ? *preview : MessageLinkPreview{};
            }
            if (m_externalLinkHistory == list) {
                const auto id = index.data(workspace::ReceiptMessageId).toString();
                if (const auto *asset = m_externalLinkPreviews.object({id, url})) {
                    MessageLinkPreview preview{asset->name.isEmpty() ? url.host() : asset->name, {url.host()}};
                    if (!asset->description.isEmpty())
                        preview.details.prepend(asset->description);
                    preview.thumbnailUrl = asset->thumbnailUrl;
                    return preview;
                }
            }
            if (m_linkPreviewAccess && (list == m_chatMessages || (m_card && list == m_card->messageList()))) {
                const auto target = publicLinkTarget(url);
                const auto *preview = m_publicLinkPreviews.object(target);
                if (preview && (!preview->title.isEmpty() || !preview->description.isEmpty() || !preview->thumbnail.isNull())) {
                    QStringList details{url.host()};
                    if (!preview->description.isEmpty())
                        details.prepend(preview->description);
                    MessageLinkPreview card{preview->title.isEmpty() ? url.host() : preview->title, details};
                    if (!preview->thumbnail.isNull())
                        card.thumbnailUrl = target;
                    return card;
                }
            }
            const auto path = url.path(QUrl::FullyDecoded);
            return {url.host(), path.isEmpty() || path == QStringLiteral("/") ? QStringList{} : QStringList{path.left(180)}};
        });
    }
    connect(list, &QListWidget::itemActivated, this, [open, list, delegate](QListWidgetItem *item) {
        auto files = item->data(workspace::MessageFiles).value<QVector<MessageFile>>();
        const auto index = list->model()->index(list->row(item), 0);
        const auto photos = delegate->photoOriginals(index);
        files.erase(std::remove_if(files.begin(), files.end(), [&photos](const MessageFile &file) {
            return file.isVideo || file.downloadUrl.isEmpty() || std::any_of(photos.cbegin(), photos.cend(), [&file](const auto &photo) {
                return photo.downloadUrl == file.downloadUrl;
            });
        }), files.end());
        if (files.size() == 1) {
            open(files.front());
        } else if (!files.isEmpty()) {
            auto *menu = new QMenu(list);
            connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
            for (const auto &file : files)
                connect(menu->addAction(file.name), &QAction::triggered, list, [open, file] { open(file); });
            menu->popup(list->viewport()->mapToGlobal(list->visualItemRect(item).center()));
        } else if (!photos.isEmpty()) {
            delegate->activateMedia(index);
        }
    });
}

void MainWindow::openMessagePhoto(const MessageFile &file, QWidget *parent, const QVector<MessageFile> &loaded)
{
    if (m_chatPhoto)
        return;
    const auto gallery = std::make_shared<PhotoGallery>(photoGallery(file, loaded));
    const auto scope = m_backend->settingsScope();
    auto *viewer = new ui::ChatPhotoDialog(parent, file.name, {});
    m_chatPhoto = viewer;
    const auto cancel = [backend = QPointer<Backend>(m_backend), gallery] {
        gallery->pendingPreview.clear();
        const auto load = std::exchange(gallery->load, {});
        if (!load)
            return;
        if (std::exchange(load->downloading, false) && backend)
            backend->cancelMessagePhoto(load->target);
        if (load->decode.isRunning())
            load->decode.cancel();
        load->directory.reset();
    };
    connect(viewer, &QDialog::finished, this, [this, viewer, cancel] {
        cancel();
        m_chatPhotoDecodePool.clear();
        if (m_chatPhoto == viewer)
            m_chatPhoto.clear();
    });
    connect(viewer, &QObject::destroyed, m_backend, cancel);
    connect(m_backend, &Backend::accountScopeChanged, viewer, &QDialog::reject);
    connect(m_backend, &Backend::messagePhotoDownloadFailed, viewer,
            [viewer, gallery](const QString &path, const QString &reason) {
        const auto load = gallery->load;
        if (!load || path != load->target || !std::exchange(load->downloading, false))
            return;
        load->directory.reset();
        viewer->showFailure(reason);
    });
    connect(m_backend, &Backend::messageFileDownloaded, viewer,
            [this, viewer, gallery, scope](const QUrl &url, const QString &path) {
        const auto load = gallery->load;
        if (!load || url != load->source || path != load->target || !std::exchange(load->downloading, false))
            return;
        auto promise = std::make_shared<QPromise<QImage>>();
        promise->start();
        load->decode = promise->future();
        auto *watcher = new QFutureWatcher<QImage>(viewer);
        connect(watcher, &QFutureWatcher<QImage>::finished, viewer,
                [this, viewer, watcher, load, gallery, scope] {
            const auto future = watcher->future();
            load->decode = QFuture<QImage>();
            if (m_chatPhoto == viewer && gallery->load == load &&
                m_backend->settingsScope() == scope && !future.isCanceled()) {
                const auto image = future.resultCount() > 0 ? future.result() : QImage();
                if (image.isNull())
                    viewer->showFailure(QStringLiteral("Формат или размер оригинала не поддерживается просмотром"));
                else
                    viewer->showOriginal(image);
            }
            watcher->deleteLater();
        });
        watcher->setFuture(load->decode);
        m_chatPhotoDecodePool.start([promise, directory = std::exchange(load->directory, {}), path]() mutable {
            if (!promise->isCanceled())
                promise->addResult(readMessagePhoto(path));
            directory.reset();
            promise->finish();
        });
    });
    const auto showPreview = [this, viewer, gallery, scope] {
        const auto url = gallery->pendingPreview;
        if (url.isEmpty() || m_chatPhoto != viewer || m_backend->settingsScope() != scope)
            return;
        if (const auto *preview = m_chatImagePreviews.photo(url)) {
            gallery->pendingPreview.clear();
            viewer->showOriginal(preview->toImage());
        } else if (m_chatImagePreviews.failed(url)) {
            gallery->pendingPreview.clear();
            viewer->showFailure(QStringLiteral("Не удалось загрузить миниатюру фотографии"));
        } else if (m_chatImagePreviews.begin(url)) {
            m_backend->requestMessageImage(url);
        }
    };
    // MainWindow has already updated the shared preview cache when this handler runs.
    // Completions for other images can free a request slot for the selected preview.
    connect(m_backend, &Backend::messageImageLoaded, viewer, showPreview);
    const auto show = [this, viewer, gallery, scope, cancel, showPreview] {
        if (m_chatPhoto != viewer || m_backend->settingsScope() != scope)
            return;
        cancel();
        m_chatPhotoDecodePool.clear();
        const auto &file = gallery->files.at(gallery->index);
        const bool hasOriginal = workspace::RowPainter::downloadable(file);
        viewer->setPhoto(file.name, gallery->index, gallery->files.size(), hasOriginal);
        if (!hasOriginal) {
            const auto &url = file.previewUrl;
            if (!url.isValid() || url.scheme() != QStringLiteral("https") || url.host().isEmpty() ||
                !url.userInfo().isEmpty() || url.hasFragment()) {
                viewer->showFailure(QStringLiteral("Оригинал фотографии недоступен"));
            } else {
                gallery->pendingPreview = url;
                showPreview();
            }
            return;
        }
        auto load = std::make_shared<PhotoLoad>();
        load->directory = std::make_shared<QTemporaryDir>(QDir::temp().filePath(QStringLiteral("bitrix24-photo-XXXXXX")));
        if (!load->directory->isValid()) {
            viewer->showFailure(QStringLiteral("Не удалось подготовить временный файл для фотографии"));
            return;
        }
        load->target = load->directory->filePath(QStringLiteral("original"));
        load->source = file.downloadUrl;
        gallery->load = load;
        m_backend->downloadMessagePhoto(load->source, load->target);
    };
    viewer->setNavigationAction([gallery, show](int offset) {
        const auto index = gallery->index + offset;
        if (index >= 0 && index < gallery->files.size()) {
            gallery->index = index;
            show();
        }
    });
    viewer->setSaveAction([this, viewer, gallery, scope](const QImage &preview) {
        if (m_chatPhoto == viewer && m_backend->settingsScope() == scope)
            saveMessageFile(gallery->files.at(gallery->index), viewer, preview);
    });
    viewer->show();
    show();
}

void MainWindow::playMessageVideo(const MessageFile &file, QWidget *parent, std::function<bool()> stillVisible)
{
    m_chatVideoPreviews->play(file, parent, std::move(stillVisible));
}

void MainWindow::saveMessageFile(const MessageFile &file, QWidget *parent, const QImage &preview)
{
    const bool hasOriginal = workspace::RowPainter::downloadable(file);
    if ((!hasOriginal && preview.isNull()) || m_messageSaveDialog || !m_messageDownloadUrl.isEmpty())
        return;
    auto *dialog = new QFileDialog(parent, QStringLiteral("Сохранить вложение"));
    dialog->setObjectName(QStringLiteral("messageFileSaveDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->setFileMode(QFileDialog::AnyFile);
    auto name = ui::attachmentNames({file.name}).value(0);
    if (!hasOriginal) {
        dialog->setDefaultSuffix(QStringLiteral("png"));
        dialog->setNameFilter(QStringLiteral("Изображения PNG (*.png)"));
        const auto stem = QFileInfo(name).completeBaseName();
        name = (stem.isEmpty() || stem == QStringLiteral(".") || stem == QStringLiteral("..")
                ? QStringLiteral("Изображение") : stem) + QStringLiteral(".png");
    }
    if (!name.isEmpty() && name != QStringLiteral(".") && name != QStringLiteral(".."))
        dialog->selectFile(name);
    m_messageSaveDialog = dialog;
    connect(m_backend, &Backend::accountScopeChanged, dialog, &QDialog::reject);
    connect(dialog, &QDialog::finished, this, [this, dialog] {
        if (m_messageSaveDialog == dialog)
            m_messageSaveDialog = nullptr;
    });
    connect(dialog, &QObject::destroyed, this, [this, dialog] {
        if (m_messageSaveDialog == dialog)
            m_messageSaveDialog = nullptr;
    });
    connect(dialog, &QFileDialog::fileSelected, this,
            [this, url = file.downloadUrl, parent = QPointer<QWidget>(parent),
             account = m_backend->settingsScope(), preview = hasOriginal ? QImage() : preview](const QString &path) {
        if (path.isEmpty() || account != m_backend->settingsScope())
            return;
        if (!preview.isNull()) {
            const auto target = QDir::cleanPath(path);
            QSaveFile output(target);
            output.setDirectWriteFallback(false);
            const bool opened = safeDownloadTarget(target) && output.open(QIODevice::WriteOnly);
            const bool encoded = opened && safeDownloadTarget(target) && preview.save(&output, "PNG");
            const bool saved = encoded && safeDownloadTarget(target) && output.commit();
            const auto status = saved ? QStringLiteral("Изображение сохранено")
                                      : QStringLiteral("Не удалось сохранить изображение");
            setStatus(status);
            if (m_card && parent == m_card)
                m_card->setStatus(status);
            return;
        }
        m_messageDownloadUrl = url;
        m_messageDownloadPath = path;
        m_messageDownloadParent = parent;
        setStatus(QStringLiteral("Скачиваем файл…"), true);
        if (m_card && parent == m_card)
            m_card->setStatus(QStringLiteral("Скачиваем файл…"), true);
        m_backend->downloadMessageFile(url, path);
    });
    dialog->open();
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
    updateComposers();
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
    updateComposers();
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
    updateComposers();
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
    updateComposers();
    updateWriteAvailability();
    m_backend->attachTaskFile(task, path);
    return true;
}

} // namespace bx
