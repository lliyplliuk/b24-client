#include "ui/main_window.h"
#include "ui/message_menu_window.h"
#include "ui/portal_dnd_control.h"
#include "ui/board_widget.h"
#include "ui/chat_photo_viewer.h"
#include "ui/chat_video_previews.h"
#include "ui/chat_widgets.h"
#include "ui/task_dialogs.h"
#include "ui/task_gantt.h"
#include "ui/project_tools.h"
#include "ui/reaction_catalog.h"
#include "ui/saved_file_offer.h"
#include "ui/typography.h"
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
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
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
#include <QSpinBox>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
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
using ui::presentMessages;
using ui::showEmpty;

class DirectorySplitter final : public QSplitter {
public:
    DirectorySplitter(const QString &key, int regularWidth, int compactWidth, QWidget *parent)
        : QSplitter(Qt::Horizontal, parent), m_key(key), m_regularWidth(regularWidth),
          m_compactWidth(compactWidth)
    {
        setHandleWidth(5);
        setChildrenCollapsible(false);
        setOpaqueResize(false);
        QSettings settings(QStringLiteral("bitrix24-client"), QStringLiteral("bitrix24-client"));
        m_preferredWidth = qMax(0, settings.value(m_key).toInt());
        connect(this, &QSplitter::splitterMoved, this, [this](int position, int) {
            m_preferredWidth = position - contentsRect().left();
            QSettings settings(QStringLiteral("bitrix24-client"), QStringLiteral("bitrix24-client"));
            settings.setValue(m_key, m_preferredWidth);
        });
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QSplitter::resizeEvent(event);
        restoreWidth();
    }

    void showEvent(QShowEvent *event) override
    {
        QSplitter::showEvent(event);
        restoreWidth();
    }

private:
    void restoreWidth()
    {
        if (!isVisible() || count() != 2)
            return;
        const int available = contentsRect().width() - handle(1)->width();
        const int minimum = widget(0)->minimumWidth();
        const int maximum = qMax(minimum, available - widget(1)->minimumWidth());
        const int preferred = m_preferredWidth > 0 ? m_preferredWidth
            : (window()->width() < 855 ? m_compactWidth : m_regularWidth);
        const int left = qBound(minimum, preferred, maximum);
        setSizes({left, qMax(0, available - left)});
    }

    const QString m_key;
    const int m_regularWidth;
    const int m_compactWidth;
    int m_preferredWidth = 0;
};

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

QVector<Notification> listedNotifications(QVector<Notification> notifications)
{
    notifications.removeIf([](const Notification &notice) { return notice.ordinaryChatMessage; });
    for (auto &notice : notifications) {
        if (notice.displayTitle.isNull() && notice.excerpt.isNull())
            prepareNotificationDisplay(notice);
    }
    return notifications;
}

QString actionLabel(const NotificationTarget &target, bool preferTargetLabel = true)
{
    if (preferTargetLabel) {
        const auto label = workspace::markupPlainText(target.label.left(256)).simplified();
        if (!label.isEmpty())
            return label;
    }
    switch (target.kind) {
    case NotificationTargetKind::Chat:
        if (target.taskChat && !target.messageId.isEmpty())
            return QStringLiteral("Открыть в чате задачи");
        return target.messageId.isEmpty() ? QStringLiteral("Открыть чат") : QStringLiteral("Открыть сообщение");
    case NotificationTargetKind::Task:
        return QStringLiteral("Открыть задачу");
    case NotificationTargetKind::Person:
        return QStringLiteral("Открыть профиль");
    case NotificationTargetKind::Project:
        return QStringLiteral("Открыть проект");
    }
    return {};
}

void presentNoticeRow(QListWidgetItem *item, const workspace::NoticeRow &notice)
{
    item->setData(workspace::NoticePresentation, QVariant::fromValue(notice));
    QStringList text{notice.day, item->data(workspace::Title).toString(), item->data(workspace::Meta).toString(),
        notice.expanded ? item->data(workspace::Detail).toString() : notice.excerpt};
    text.removeAll(QString());
    item->setData(Qt::AccessibleTextRole, text.join(QStringLiteral(". ")));
    QStringList details;
    if (item->data(workspace::Count).toBool())
        details.append(QStringLiteral("Непрочитанное"));
    details.append(notice.expanded ? QStringLiteral("Подробности раскрыты") : QStringLiteral("Enter — подробности"));
    if (notice.primaryTarget >= 0)
        details.append(QStringLiteral("Ctrl+Enter — %1").arg(notice.primaryLabel));
    details.append(QStringLiteral("Shift+F10 — действия"));
    item->setData(Qt::AccessibleDescriptionRole, details.join(QStringLiteral(". ")));
}

QWidget *directory(const QString &title, QListWidget **list, QWidget *parent, QLabel **titleLabel = nullptr)
{
    auto *side = new QWidget(parent);
    side->setObjectName(QStringLiteral("directory"));
    auto *layout = new QVBoxLayout(side);
    layout->setContentsMargins(10, 20, 9, 10);
    layout->setSpacing(10);
    auto *heading = label(title, "sectionTitle", side);
    if (titleLabel)
        *titleLabel = heading;
    heading->setContentsMargins(7, 0, 0, 0);
    layout->addWidget(heading);
    *list = new QListWidget(side);
    (*list)->setAccessibleName(title);
    workspace::configureRows(*list, workspace::Row::Directory);
    layout->addWidget(*list, 1);
    return side;
}

QLabel *emptyState(const QString &text, QWidget *parent)
{
    auto *result = label(text, "empty", parent);
    result->setAlignment(Qt::AlignCenter);
    return result;
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

void selectAvailable(QListWidget *list, QListWidgetItem *selection)
{
    if (!selection && list->count() > 0 && !list->item(0)->data(workspace::Id).toString().isEmpty())
        selection = list->item(0);
    list->setCurrentItem(selection);
}

struct ListViewportAnchor {
    QStringList successors;
    int offset = 0;
    int scroll = 0;
};

ListViewportAnchor listViewportAnchor(const QListWidget *list)
{
    ListViewportAnchor anchor;
    anchor.scroll = list->verticalScrollBar()->value();
    const auto first = list->indexAt(QPoint(4, 0));
    if (first.isValid()) {
        anchor.offset = list->visualRect(first).top();
        for (int row = first.row(); row < list->count(); ++row)
            anchor.successors.append(list->item(row)->data(workspace::Id).toString());
    }
    return anchor;
}

void restoreListViewport(QListWidget *list, const ListViewportAnchor &anchor)
{
    list->doItemsLayout();
    auto *scroll = list->verticalScrollBar();
    QHash<QString, QListWidgetItem *> byId;
    for (int row = 0; row < list->count(); ++row) {
        auto *item = list->item(row);
        byId.insert(item->data(workspace::Id).toString(), item);
    }
    for (const auto &id : anchor.successors) {
        if (const auto *item = byId.value(id)) {
            scroll->setValue(scroll->value() + list->visualItemRect(item).top() - anchor.offset);
            return;
        }
    }
    scroll->setValue(anchor.scroll);
}

QString chatPreferenceKey(const QString &accountScope, QLatin1String preference)
{
    if (accountScope.isEmpty())
        return {};
    const auto key = QCryptographicHash::hash(accountScope.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QStringLiteral("ui/accounts/") + QString::fromLatin1(key) + QLatin1Char('/') + preference;
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
    m_stack->addWidget(buildProjectsPanel());
    m_stack->addWidget(buildNotificationsPanel());
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
            m_pendingMessageProject = {};
            m_noticeProjectId.clear();
            if (!m_reactionAccountScope.isEmpty()) {
                m_projectDirectory.clear();
                m_projectDirectoryLoaded = false;
                m_projectsMoreAvailable = false;
                m_projectsPagePending = false;
                m_projectsPageFailed = false;
                m_pendingProjectId.clear();
                updateProjectsRailPagination();
                m_chatImagePreviews.clear();
                m_pendingPastedChatImage = {};
                resetNotificationAccount();
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
                closeProjectTaskChats();
                if (m_projectUnreadCountsComplete || !m_projectUnreadCounts.isEmpty()) {
                    m_projectUnreadCounts.clear();
                    m_projectUnreadCountsComplete = false;
                    updateProjectUnreadRows();
                }
                m_taskChatBadgesRequested = false;
                updateChatUnreadBadges();
            }
            m_pendingReactionAdds.clear();
            m_reactionAccountScope = scope;
            if (!scope.isEmpty() && !m_noticeHistoryRequested && m_sessionRecovery->isHidden()) {
                QTimer::singleShot(0, this, [this, scope] {
                    if (m_backend->settingsScope() == scope && !m_noticeHistoryRequested &&
                        m_sessionRecovery->isHidden())
                        requestNotificationHistory();
                });
            }
        }
        reloadChatVisibilityPreference();
        m_board->setColumnWidthContext(m_currentProjectId, chatPreferenceKey(scope, QLatin1String("kanbanColumnWidths")));
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
        scheduleVisibleNoticeRead();
        if (m_externalLinkHistory && m_online)
            scheduleAvatars();
    });
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive)
            scheduleVisibleNoticeRead();
    });
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *current) {
        if (current && (current->window() == this || current->window() == m_card))
            scheduleVisibleChatRead();
    });
    m_chatContactTimer = new QTimer(this);
    m_chatContactTimer->setSingleShot(true);
    connect(m_chatContactTimer, &QTimer::timeout, this, &MainWindow::updateChatContactPresentation);
    m_projectSearchTimer = new QTimer(this);
    m_projectSearchTimer->setSingleShot(true);
    m_projectSearchTimer->setInterval(300);
    connect(m_projectSearchTimer, &QTimer::timeout, this, &MainWindow::runProjectTaskSearch);
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
        m_board->refreshAvatars();
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
    connect(m_backend, &Backend::projectPinChanged, this, [this](const QString &id, bool pinned) {
        m_pendingPins.remove(operationKey(OperationKind::ProjectPin, id));
        for (auto &project : m_projectDirectory)
            if (project.id == id)
                project.pinned = pinned;
        if (m_lastSection == 1)
            showProjects();
    });
    connect(m_backend, &Backend::projectUnreadCountsChanged, this,
            [this](const QHash<QString, qint64> &counts, bool complete) {
        if (m_projectUnreadCountsComplete != complete || m_projectUnreadCounts != counts) {
            m_projectUnreadCounts = counts;
            m_projectUnreadCountsComplete = complete;
            updateProjectUnreadRows();
        }
    });
    connect(m_backend, &Backend::projectTaskChatsLoaded, this, &MainWindow::onProjectTaskChatsLoaded);
    connect(m_backend, &Backend::projectTaskChatBadgesChanged, this, &MainWindow::updateProjectTaskChatBadges);

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
    connect(m_backend, &Backend::projectsLoaded, this, [this](const QVector<Project> &projects) {
        const bool appended = projects.size() > m_projectDirectory.size() && !m_projectDirectory.isEmpty()
            && std::equal(m_projectDirectory.cbegin(), m_projectDirectory.cend(), projects.cbegin(),
                          [](const Project &old, const Project &now) { return old.id == now.id; });
        const auto scroll = m_projects->verticalScrollBar()->value();
        m_projectDirectory = projects;
        m_projectDirectoryLoaded = true;
        m_projectsPagePending = false;
        m_projectsPageFailed = false;
        if (m_card) {
            m_card->setProjectChoices(m_projectDirectory);
            scheduleAvatars();
        }
        if (m_taskForm)
            m_taskForm->setProjectChoices(m_projectDirectory);
        if (m_lastSection == 1) {
            const bool selectingNotice = !m_noticeProjectId.isEmpty();
            showProjects();
            if (appended && !selectingNotice)
                m_projects->verticalScrollBar()->setValue(scroll);
            if (!m_noticeProjectId.isEmpty() && !m_projectsMoreAvailable) {
                setStatus(QStringLiteral("Проект недоступен или отсутствует в списке"));
                m_noticeProjectId.clear();
            }
        }
        updateProjectsRailPagination();
        const auto link = std::exchange(m_pendingMessageProject, {});
        if (!link.url.isEmpty())
            openPortalTarget(link.target, nullptr, link.url);
    });
    connect(m_backend, &Backend::projectsMoreAvailable, this, [this](bool available) {
        m_projectsMoreAvailable = available;
        if (m_card)
            m_card->setMoreProjectsAvailable(available);
        if (m_taskForm)
            m_taskForm->setMoreProjectsAvailable(available);
        updateProjectsRailPagination();
    });
    connect(m_backend, &Backend::projectsScopeLoaded, this, [this](bool membershipVerified) {
        const auto title = membershipVerified ? QStringLiteral("Мои проекты") : QStringLiteral("Доступные проекты");
        m_projectDirectoryTitle->setText(title);
        m_projects->setAccessibleName(title);
    });

    connect(m_backend, &Backend::boardLoaded, this, [this](const QString &projectId, const QVector<Stage> &stages) {
        if (m_taskForm && m_taskForm->projectId() == projectId)
            m_taskForm->setStages(stages);
        if (m_lastSection != 1 || projectId != m_currentProjectId)
            return;
        m_stages = stages;
        m_loadedBoardTaskIds.clear();
        for (const auto &stage : stages)
            for (const auto &task : stage.tasks)
                m_loadedBoardTaskIds.insert(task.id);
        if (m_boardMoveRefreshProject == projectId) {
            m_boardMoveRefreshProject.clear();
            setStatus(QStringLiteral("Задача перенесена"));
        }
        m_board->resetStagePageLoads();
        if (m_projectSearchDisplayed)
            displayProjectTaskResults();
        else
            m_board->setStages(stages);
        if (m_stageManager && m_stageManager->projectId() == projectId)
            m_stageManager->setStages(stages);
        if (m_card && m_card->projectId() == projectId)
            m_card->setStages(stages);
    });
    connect(m_backend, &Backend::boardTasksPageLoaded, this,
            [this](const QString &project, const QString &stageId, const QVector<Task> &tasks, bool more) {
        if (project != m_currentProjectId || !m_board->stagePageLoading(stageId))
            return;
        const auto stage = std::find_if(m_stages.begin(), m_stages.end(), [&stageId](const Stage &column) {
            return column.id == stageId;
        });
        if (stage == m_stages.end()) {
            m_board->resetStagePageLoads(false);
            return;
        }
        const auto previousCount = stage->tasks.size();
        for (const auto &task : tasks) {
            if (task.id.isEmpty() || task.stageId != stageId || m_loadedBoardTaskIds.contains(task.id))
                continue;
            m_loadedBoardTaskIds.insert(task.id);
            stage->tasks.append(task);
        }
        stage->hasMoreTasks = more;
        if (!m_board->finishStagePage(stageId, !more || stage->tasks.size() > previousCount))
            setStatus(QStringLiteral("Портал перестал возвращать задачи. F5 — обновить доску"));
        if (!m_projectSearchDisplayed)
            m_board->applyStagePage(*stage);
    });
    connect(m_backend, &Backend::projectStagesLoaded, this,
            [this](const QString &projectId, const QVector<Stage> &stages) {
        if (m_taskForm && m_taskForm->projectId() == projectId)
            m_taskForm->setStages(stages);
        if (!m_card || m_card->projectId() != projectId || m_cardRequestedStages != projectId)
            return;
        if (projectId != m_currentProjectId || m_stages.isEmpty())
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
        if (!m_projectDirectoryLoaded && !m_cardRequestedProjectDirectory
            && (task.editRights.project || !projectId.isEmpty())) {
            m_cardRequestedProjectDirectory = true;
            m_backend->requestProjects();
            if (m_card != card)
                return;
        }
        if (projectId == m_currentProjectId && !m_stages.isEmpty()) {
            m_card->setStages(m_stages);
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

    connect(m_backend, &Backend::notificationsLoaded, this, [this](const QVector<Notification> &notifications) {
        auto listed = listedNotifications(notifications);
        bool newUnread = false;
        if (m_lastSection == 2) {
            QSet<QString> previouslyUnread;
            for (const auto &notice : m_noticeHistory)
                if (!notice.read)
                    previouslyUnread.insert(notice.id);
            newUnread = std::any_of(listed.cbegin(), listed.cend(), [&previouslyUnread](const auto &notice) {
                return !notice.read && !previouslyUnread.contains(notice.id);
            });
        }
        m_noticeHistoryState = NoticeHistoryState::Ready;
        m_noticeHistory = std::move(listed);
        updateNotificationRailBadge();
        if (m_lastSection == 2)
            showNotifications(newUnread);
    });
    connect(m_backend, &Backend::notificationsFound, this,
            [this](const QString &query, const QVector<Notification> &notifications) {
        if (query != m_noticeQuery || query != m_noticeSearch->text().trimmed())
            return;
        m_noticeSearchPending = false;
        m_noticeSearchResults = listedNotifications(notifications);
        showNotifications();
    });
    connect(m_backend, &Backend::notificationHistoryPageStateChanged, this, [this](bool hasMore, bool loading) {
        m_noticeHasOlder = hasMore;
        m_noticeOlderLoading = loading;
        scheduleOlderNotifications();
    });
    connect(m_backend, &Backend::notificationReadStateChanged, this,
            [this](const QStringList &ids, bool read) {
        if (read && m_noticeUnreadOnly->isChecked() && ids.contains(m_noticeExpandedId)
            && (m_noticeAction == operationKey(OperationKind::ReadNotification) ||
                m_noticeAction == operationKey(OperationKind::ReadAllNotifications)))
            m_noticeExpandedId.clear();
        for (auto *source : {&m_noticeHistory, &m_noticeSearchResults})
            for (auto &notice : *source)
                if (ids.contains(notice.id))
                    notice.read = read;
        updateNotificationRailBadge();
        if (m_lastSection == 2)
            showNotifications(false);
    });
    connect(m_backend, &Backend::notificationRemoved, this, [this](const QString &id) {
        for (auto *source : {&m_noticeHistory, &m_noticeSearchResults})
            source->erase(std::remove_if(source->begin(), source->end(), [&id](const Notification &notice) {
                return notice.id == id;
            }), source->end());
        if (m_noticeSelectedId == id)
            m_noticeSelectedId.clear();
        updateNotificationRailBadge();
        if (m_lastSection == 2)
            showNotifications(false);
    });
    connect(m_backend, &Backend::notificationActionFinished, this, [this](const QString &action) {
        if (parseOperation(action).kind == OperationKind::ReadNotifications)
            finishVisibleNoticeRead(true);
        if (m_noticeAction == action) {
            m_noticeAction.clear();
            setStatus(QString());
            updateNotificationActions();
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
        if (!m_noticeHistoryRequested && m_sessionRecovery->isHidden())
            requestNotificationHistory();
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
        if (recovering && (!m_noticeHistoryRequested || m_noticeHistoryState == NoticeHistoryState::Failed))
            requestNotificationHistory();
    }
    updateRailWorkday();
    scheduleOlderNotifications();
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
            [this](const QPoint &point) { showPinMenu(m_chats, point, false); });
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

QWidget *MainWindow::buildProjectsPanel()
{
    auto *panel = new QWidget;
    auto *root = new QHBoxLayout(panel);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto *splitter = new DirectorySplitter(QStringLiteral("ui/projectDirectoryWidth"), 205, 151, panel);
    splitter->setObjectName(QStringLiteral("projectDirectorySplitter"));
    splitter->setAccessibleName(QStringLiteral("Ширина списка проектов"));
    root->addWidget(splitter);
    auto *side = directory(QStringLiteral("Проекты"), &m_projects, splitter, &m_projectDirectoryTitle);
    side->setMinimumWidth(132);
    connect(m_projects, &QListWidget::currentItemChanged, this, [this] {
        m_pendingMessageProject = {};
        onProjectSelected();
    });
    m_projects->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_projects, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &point) { showPinMenu(m_projects, point, true); });
    connect(m_projects->verticalScrollBar(), &QScrollBar::valueChanged, this, &MainWindow::scheduleAvatars);
    auto *projectSearchRow = new QHBoxLayout;
    projectSearchRow->setSpacing(2);
    m_projectNameSearch = new QLineEdit(side);
    ui::editor(m_projectNameSearch);
    m_projectNameSearch->setAccessibleName(QStringLiteral("Поиск по названию проектов"));
    m_projectNameSearch->setPlaceholderText(QStringLiteral("Название проекта"));
    m_projectNameSearch->setClearButtonEnabled(true);
    projectSearchRow->addWidget(m_projectNameSearch, 1);
    auto *projectSideLayout = qobject_cast<QVBoxLayout *>(side->layout());
    if (projectSideLayout)
        projectSideLayout->insertLayout(1, projectSearchRow);
    connect(m_projectNameSearch, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (m_projectNameQuery == text.trimmed())
            return;
        m_projectNameQuery = text.trimmed();
        showProjects();
    });
    m_moreProjectsButton = new QPushButton(QStringLiteral("Ещё проекты"), side);
    m_moreProjectsButton->setObjectName(QStringLiteral("moreProjects"));
    m_moreProjectsButton->hide();
    connect(m_moreProjectsButton, &QPushButton::clicked, this, &MainWindow::requestNextProjectsPage);
    side->layout()->addWidget(m_moreProjectsButton);
    m_projects->installEventFilter(this);
    m_projects->viewport()->installEventFilter(this);
    auto *projectScroll = m_projects->verticalScrollBar();
    connect(projectScroll, &QScrollBar::actionTriggered, this, [this, projectScroll](int action) {
        if (action == QAbstractSlider::SliderSingleStepAdd || action == QAbstractSlider::SliderPageStepAdd
            || action == QAbstractSlider::SliderToMaximum
            || (action == QAbstractSlider::SliderMove && projectScroll->isSliderDown()
                && projectScroll->sliderPosition() > projectScroll->value()))
            scheduleProjectsPageCheck();
    });
    connect(projectScroll, &QScrollBar::rangeChanged, this, &MainWindow::updateProjectsRailPagination);
    m_newProjectButton = new QPushButton(QStringLiteral("Новый проект"), side);
    m_newProjectButton->setAccessibleName(QStringLiteral("Создать проект"));
    connect(m_newProjectButton, &QPushButton::clicked, this, &MainWindow::createProject);
    side->layout()->addWidget(m_newProjectButton);
    splitter->addWidget(side);

    auto *body = new QWidget(splitter);
    body->setMinimumWidth(430);
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(16, 17, 16, 14);
    layout->setSpacing(12);
    auto *header = new QFormLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setHorizontalSpacing(10);
    header->setVerticalSpacing(6);
    header->setRowWrapPolicy(QFormLayout::WrapLongRows);
    header->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto *heading = new QWidget(body);
    auto *headingLayout = new QHBoxLayout(heading);
    headingLayout->setContentsMargins(0, 0, 0, 0);
    m_projectAvatar = new ui::AvatarBadge(body);
    m_projectAvatar->hide();
    headingLayout->addWidget(m_projectAvatar);
    m_projectTitle = label(QStringLiteral("Проекты"), "pageTitle", body);
    m_projectTitle->setAccessibleName(QStringLiteral("Открытый проект"));
    headingLayout->addWidget(m_projectTitle, 1);
    auto *actions = new QWidget(body);
    auto *actionsLayout = new QHBoxLayout(actions);
    actionsLayout->setContentsMargins(0, 0, 0, 0);
    actionsLayout->addStretch(1);
    m_projectToolsButton = new QPushButton(QStringLiteral("О проекте"), body);
    m_projectToolsButton->setAccessibleName(QStringLiteral("Участники и файлы проекта"));
    connect(m_projectToolsButton, &QPushButton::clicked, this, &MainWindow::openProjectTools);
    actionsLayout->addWidget(m_projectToolsButton);
    m_newTaskButton = new QPushButton(QStringLiteral("Новая задача"), body);
    m_newTaskButton->setObjectName(QStringLiteral("primary"));
    m_newTaskButton->setAccessibleName(QStringLiteral("Создать задачу"));
    connect(m_newTaskButton, &QPushButton::clicked, this, &MainWindow::createTaskInProject);
    actionsLayout->addWidget(m_newTaskButton);
    auto *refresh = new QPushButton(QStringLiteral("Обновить"), body);
    refresh->setToolTip(QStringLiteral("Обновить проект (F5)"));
    connect(refresh, &QPushButton::clicked, this, &MainWindow::onRefresh);
    actionsLayout->addWidget(refresh);
    header->addRow(heading, actions);
    layout->addLayout(header);

    m_projectTabs = new QTabBar(body);
    m_projectTabs->setObjectName(QStringLiteral("projectScopeTabs"));
    m_projectTabs->setAccessibleName(QStringLiteral("Раздел проекта"));
    m_projectTabs->setExpanding(false);
    m_projectTabs->addTab(QStringLiteral("Задачи"));
    m_projectTabs->addTab(QStringLiteral("Чаты"));
    layout->addWidget(m_projectTabs);
    m_projectViews = new QStackedWidget(body);
    layout->addWidget(m_projectViews, 1);
    auto *tasks = new QWidget(m_projectViews);
    m_projectViews->addWidget(tasks);
    auto *taskLayout = new QVBoxLayout(tasks);
    taskLayout->setContentsMargins(0, 0, 0, 0);
    taskLayout->setSpacing(12);

    auto *tools = new QHBoxLayout;
    tools->setSpacing(5);
    auto *viewOptions = new QButtonGroup(body);
    m_kanbanButton = new QPushButton(QStringLiteral("Канбан"), body);
    m_kanbanButton->setObjectName(QStringLiteral("viewToggle"));
    m_kanbanButton->setCheckable(true);
    m_kanbanButton->setChecked(true);
    viewOptions->addButton(m_kanbanButton);
    m_kanbanButton->setAccessibleName(QStringLiteral("Вид Канбан"));
    connect(m_kanbanButton, &QPushButton::clicked, this, [this] {
        showProjectTimeline(false);
        m_board->setView(BoardWidget::View::Kanban);
    });
    tools->addWidget(m_kanbanButton, 0);
    m_listButton = new QPushButton(QStringLiteral("Список"), body);
    m_listButton->setObjectName(QStringLiteral("viewToggle"));
    m_listButton->setCheckable(true);
    viewOptions->addButton(m_listButton);
    m_listButton->setAccessibleName(QStringLiteral("Вид Список"));
    connect(m_listButton, &QPushButton::clicked, this, [this] {
        showProjectTimeline(false);
        m_board->setView(BoardWidget::View::List);
    });
    tools->addWidget(m_listButton, 0);
    auto *gantt = new QPushButton(QStringLiteral("Гант"), body);
    gantt->setObjectName(QStringLiteral("viewToggle"));
    gantt->setCheckable(true);
    gantt->setAccessibleName(QStringLiteral("Вид Гант"));
    viewOptions->addButton(gantt);
    connect(gantt, &QPushButton::clicked, this, [this] { showProjectTimeline(true); });
    tools->addWidget(gantt, 0);
    m_filter = new QLineEdit(body);
    ui::editor(m_filter);
    m_filter->setAccessibleName(QStringLiteral("Фильтр задач"));
    m_filter->setPlaceholderText(QStringLiteral("Название или №"));
    m_filter->setClearButtonEnabled(true);
    connect(m_filter, &QLineEdit::textChanged, this, [this](const QString &text) {
        const bool hadResults = m_projectSearchDisplayed;
        m_projectSearchDisplayed = false;
        m_projectSearchResults.clear();
        m_projectTimeline->setFilter(text);
        if (m_ganttMode)
            return;
        if (hadResults)
            m_board->setStages(m_stages);
        m_board->setFilter(text);
        const bool searching = m_lastSection == 1 && m_projectTabs->currentIndex() == 0
            && !m_currentProjectId.isEmpty() && !text.trimmed().isEmpty();
        ui::setStatusLabel(m_projectTaskSearchStatus, searching ? QStringLiteral("Ищем задачи в проекте…") : QString(), searching);
        if (searching)
            m_projectSearchTimer->start();
        else
            m_projectSearchTimer->stop();
    });
    connect(m_filter, &QLineEdit::returnPressed, this, &MainWindow::runProjectTaskSearch);
    tools->addWidget(m_filter, 1);
    m_stagesButton = new QPushButton(QStringLiteral("Этапы"), body);
    m_stagesButton->setAccessibleName(QStringLiteral("Управление этапами"));
    connect(m_stagesButton, &QPushButton::clicked, this, [this] { openStageManager(); });
    tools->addWidget(m_stagesButton, 0);
    taskLayout->addLayout(tools);
    m_projectTaskSearchStatus = label({}, "quiet", body);
    m_projectTaskSearchStatus->setAccessibleName(QStringLiteral("Состояние поиска задач"));
    m_projectTaskSearchStatus->setWordWrap(true);
    m_projectTaskSearchStatus->hide();
    taskLayout->addWidget(m_projectTaskSearchStatus);

    m_board = new BoardWidget(body);
    m_board->setAvatarSource([this](const QUrl &url) { return m_avatars.object(url); });
    connect(m_board, &BoardWidget::visibleCardsChanged, this, &MainWindow::scheduleAvatars);
    taskLayout->addWidget(m_board, 1);
    connect(m_board, &BoardWidget::taskClicked, this, &MainWindow::openTaskCard);
    m_board->setTaskMoveGuard([this](const QString &task, const QString &from, const QString &to) {
        return canMoveBoardTask(task, from, to);
    });
    connect(m_board, &BoardWidget::taskMoveRequested, this,
            [this](const QString &task, const QString &from, const QString &to) {
        if (to.isEmpty() || !canMoveBoardTask(task, from, to))
            return;
        const auto project = m_currentProjectId;
        m_pendingBoardEdit = project;
        m_pendingMoveTask = task;
        setStatus(QStringLiteral("Переносим задачу…"));
        updateWriteAvailability();
        m_backend->moveTask(project, task, to);
    });
    connect(m_board, &BoardWidget::loadMoreRequested, this, [this](const QString &stage) {
        if (m_lastSection != 1 || m_projectTabs->currentIndex() != 0 || m_ganttMode
            || m_currentProjectId.isEmpty() || !m_filter->text().trimmed().isEmpty()
            || m_board->stagePageLoading(stage))
            return;
        m_board->setStagePageLoading(stage, true);
        m_backend->requestMoreBoardTasks(m_currentProjectId, stage);
    });
    m_projectTimeline = new ui::TaskGanttView(body);
    m_projectTimeline->onOpen = [this](const QString &id) { openTaskCard(id); };
    m_projectTimeline->hide();
    taskLayout->addWidget(m_projectTimeline, 1);

    auto *chats = new QWidget(m_projectViews);
    m_projectViews->addWidget(chats);
    auto *chatLayout = new QVBoxLayout(chats);
    chatLayout->setContentsMargins(0, 0, 0, 0);
    chatLayout->setSpacing(6);
    m_projectTaskChatStatus = label({}, "quiet", chats);
    m_projectTaskChatStatus->setAccessibleName(QStringLiteral("Состояние чатов проекта"));
    m_projectTaskChatStatus->setTextFormat(Qt::PlainText);
    m_projectTaskChatStatus->setWordWrap(true);
    chatLayout->addWidget(m_projectTaskChatStatus);
    m_projectTaskChats = new QListWidget(chats);
    m_projectTaskChats->setAccessibleName(QStringLiteral("Чаты задач проекта"));
    m_projectTaskChats->setAccessibleDescription(QStringLiteral("Открыть чат в карточке задачи"));
    workspace::configureRows(m_projectTaskChats, workspace::Row::Directory);
    chatLayout->addWidget(m_projectTaskChats, 1);
    connect(m_projectTaskChats, &QListWidget::itemEntered, this, [this](const QListWidgetItem *) {
        m_projectTaskChats->viewport()->setCursor(Qt::PointingHandCursor);
    });
    connect(m_projectTaskChats, &QListWidget::viewportEntered, this, [this] {
        m_projectTaskChats->viewport()->unsetCursor();
    });
    connect(m_projectTaskChats->verticalScrollBar(), &QScrollBar::valueChanged, this, &MainWindow::scheduleAvatars);
    const auto openTaskChat = [this](const QListWidgetItem *item) {
        if (item && m_lastSection == 1 && m_projectTabs->currentIndex() == 1
            && m_projectTaskChatScope == m_currentProjectId && !m_projectTaskChatScope.isEmpty())
            openTaskCardForProject(item->data(workspace::TaskId).toString(), m_currentProjectId);
    };
    connect(m_projectTaskChats, &QListWidget::itemClicked, this, openTaskChat);
    connect(m_projectTaskChats, &QListWidget::itemActivated, this, openTaskChat);
    m_moreProjectTaskChats = new QPushButton(QStringLiteral("Ещё чаты"), chats);
    m_moreProjectTaskChats->setObjectName(QStringLiteral("moreProjectTaskChats"));
    connect(m_moreProjectTaskChats, &QPushButton::clicked, this, &MainWindow::requestProjectTaskChatPage);
    chatLayout->addWidget(m_moreProjectTaskChats, 0, Qt::AlignLeft);
    connect(m_projectTabs, &QTabBar::currentChanged, this, &MainWindow::onProjectScopeChanged);
    updateProjectTaskChatControls();

    splitter->addWidget(body);
    splitter->setStretchFactor(1, 1);
    connect(splitter, &QSplitter::splitterMoved, this, &MainWindow::scheduleAvatars);
    return panel;
}

QWidget *MainWindow::buildNotificationsPanel()
{
    auto *panel = new QWidget;
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(24, 19, 24, 15);
    layout->setSpacing(12);
    auto *header = new QHBoxLayout;
    header->addWidget(label(QStringLiteral("События"), "pageTitle", panel), 1);
    m_noticeReadAll = ui::button(QStringLiteral("Отметить все прочитанными"),
                                  QStringLiteral("Прочитать все уведомления"), panel);
    header->addWidget(m_noticeReadAll, 0, Qt::AlignTop);
    auto *refresh = new QPushButton(QStringLiteral("Обновить"), panel);
    refresh->setToolTip(QStringLiteral("Обновить уведомления (F5)"));
    connect(refresh, &QPushButton::clicked, this, &MainWindow::onRefresh);
    header->addWidget(refresh, 0, Qt::AlignTop);
    auto *settings = ui::button(QStringLiteral("Настройки"), QStringLiteral("Настройки уведомлений портала"), panel);
    settings->setVisible(!m_backend->isDemo());
    header->addWidget(settings, 0, Qt::AlignTop);
    connect(settings, &QPushButton::clicked, this, [this] {
        auto *dialog = new QDialog(this);
        dialog->setObjectName(QStringLiteral("notificationSettingsDialog"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(dialog, &QDialog::finished, this, [this] {
            QTimer::singleShot(0, this, &MainWindow::scheduleVisibleNoticeRead);
        });
        dialog->setWindowTitle(QStringLiteral("Уведомления портала"));
        dialog->setMinimumWidth(390);
        auto *content = new QVBoxLayout(dialog);
        auto *status = label(QStringLiteral("Загружаем настройки…"), "minorLabel", dialog);
        status->setWordWrap(true);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
        auto *close = buttons->button(QDialogButtonBox::Close);
        close->setText(QStringLiteral("Закрыть"));
        close->setIcon(QIcon());
        auto *retry = buttons->addButton(QStringLiteral("Повторить загрузку"), QDialogButtonBox::ActionRole);
        retry->setAccessibleName(QStringLiteral("Повторить загрузку настроек"));
        retry->hide();
        const auto loadSettings = [this, retry, status] {
            retry->hide();
            status->setText(QStringLiteral("Загружаем настройки…"));
            ui::setLoadingIndicator(status, true);
            m_backend->requestNotificationSettings();
        };
        connect(retry, &QPushButton::clicked, dialog, loadSettings);
        auto addSetting = [this, dialog, content, status](const QString &title, const QString &name, bool volume) {
            auto *container = new QWidget(dialog);
            auto *row = new QHBoxLayout(container);
            row->setContentsMargins(0, 0, 0, 0);
            std::function<int()> value;
            std::function<void(int)> setValue;
            if (volume) {
                auto *input = new QSpinBox(container);
                input->setRange(0, 100);
                input->setSuffix(QStringLiteral(" %"));
                input->setAccessibleName(title);
                auto *caption = new QLabel(title, container);
                caption->setBuddy(input);
                row->addWidget(caption, 1);
                row->addWidget(input);
                value = [input] { return input->value(); };
                setValue = [input](int number) { input->setValue(number); };
            } else {
                auto *input = new QCheckBox(title, container);
                row->addWidget(input, 1);
                value = [input] { return int(input->isChecked()); };
                setValue = [input](int number) { input->setChecked(number != 0); };
            }
            auto *save = new QPushButton(QStringLiteral("Сохранить"), container);
            save->setAccessibleName(QStringLiteral("Сохранить настройку «%1»").arg(title));
            row->addWidget(save);
            content->addWidget(container);
            container->hide();
            connect(save, &QPushButton::clicked, dialog, [this, dialog, status, name, value] {
                if (!writable() || !dialog->property("pendingSetting").toString().isEmpty())
                    return;
                dialog->setProperty("pendingSetting", name);
                status->setText(QStringLiteral("Сохраняем…"));
                ui::setLoadingIndicator(status, true);
                m_backend->updateNotificationSetting(name, value());
            });
            connect(m_backend, &Backend::notificationSettingsLoaded, container,
                    [this, dialog, container, name, value, setValue](const QVariantMap &confirmed, const QString &updated) {
                const auto current = confirmed.value(name);
                container->setVisible(current.isValid());
                container->setEnabled(writable());
                const auto previous = container->property("confirmed");
                const bool awaiting = dialog->property("pendingSetting").toString() == name;
                if (current.isValid() && (updated == name || (!awaiting && (!previous.isValid() || previous.toInt() == value()))))
                    setValue(current.toInt());
                container->setProperty("confirmed", current);
            });
        };
        addSetting(QStringLiteral("Без звука"), QStringLiteral("soundMuted"), false);
        addSetting(QStringLiteral("Общая громкость"), QStringLiteral("soundVolumeGeneral"), true);
        addSetting(QStringLiteral("Громкость сообщений"), QStringLiteral("soundVolumeMessages"), true);
        addSetting(QStringLiteral("На компьютере"), QStringLiteral("notifySchemeSendSite"), false);
        addSetting(QStringLiteral("По почте"), QStringLiteral("notifySchemeSendEmail"), false);
        addSetting(QStringLiteral("В мобильном приложении"), QStringLiteral("notifySchemeSendPush"), false);
        connect(m_backend, &Backend::notificationSettingsLoaded, dialog,
                [this, dialog, status, retry](const QVariantMap &confirmed, const QString &updated) {
            const auto pending = dialog->property("pendingSetting").toString();
            if (!pending.isEmpty() && pending != updated)
                return;
            dialog->setProperty("pendingSetting", QString());
            retry->setVisible(confirmed.isEmpty());
            status->setText(confirmed.isEmpty() ? QStringLiteral("Портал не вернул настройки уведомлений")
                : writable() ? QString() : QStringLiteral("Настройки открыты для чтения"));
            ui::setLoadingIndicator(status, false);
        });
        connect(m_backend, &Backend::failed, dialog, [dialog, status, retry](const QString &action, const QString &reason) {
            const auto kind = parseOperation(action).kind;
            if (kind == OperationKind::NotificationSettingsUpdate ||
                (kind == OperationKind::NotificationSettings && dialog->property("pendingSetting").toString().isEmpty())) {
                dialog->setProperty("pendingSetting", QString());
                status->setText(reason);
                ui::setLoadingIndicator(status, false);
                if (kind == OperationKind::NotificationSettings)
                    retry->show();
            }
        });
        ui::addPortalDndControl(content, m_backend, [this] { return writable(); });
        content->addWidget(status);
        content->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
        dialog->setWindowModality(Qt::WindowModal);
        dialog->open();
        loadSettings();
    });
    layout->addLayout(header);
    auto *filters = new QHBoxLayout;
    m_noticeSearch = new QLineEdit(panel);
    ui::editor(m_noticeSearch);
    m_noticeSearch->setAccessibleName(QStringLiteral("Поиск уведомлений"));
    m_noticeSearch->setPlaceholderText(QStringLiteral("Поиск уведомлений"));
    m_noticeSearch->setClearButtonEnabled(true);
    m_noticeSearch->setMaxLength(200);
    filters->addWidget(m_noticeSearch, 1);
    m_noticeUnreadOnly = new QCheckBox(QStringLiteral("Только непрочитанные"), panel);
    m_noticeUnreadOnly->setAccessibleName(QStringLiteral("Только непрочитанные уведомления"));
    filters->addWidget(m_noticeUnreadOnly);
    m_noticeSort = new QComboBox(panel);
    ui::editor(m_noticeSort);
    m_noticeSort->setAccessibleName(QStringLiteral("Порядок уведомлений"));
    m_noticeSort->addItems({QStringLiteral("Сначала новые"), QStringLiteral("Сначала старые")});
    filters->addWidget(m_noticeSort);
    layout->addLayout(filters);

    m_noticeRead = new QAction(QStringLiteral("Отметить прочитанным"), panel);
    m_noticeRead->setObjectName(QStringLiteral("readNotice"));
    m_noticeDelete = new QAction(QStringLiteral("Удалить уведомление"), panel);
    m_noticeDelete->setObjectName(QStringLiteral("deleteNotice"));
    m_noticeSearchTimer = new QTimer(panel);
    m_noticeSearchTimer->setSingleShot(true);
    m_noticeSearchTimer->setInterval(280);
    connect(m_noticeSearchTimer, &QTimer::timeout, this, &MainWindow::runNotificationSearch);
    connect(m_noticeSearch, &QLineEdit::textChanged, m_noticeSearchTimer, qOverload<>(&QTimer::start));
    connect(m_noticeSearch, &QLineEdit::returnPressed, this, &MainWindow::runNotificationSearch);
    connect(m_noticeUnreadOnly, &QCheckBox::toggled, this, [this] {
        m_noticeExpandedId.clear();
        showNotifications(true, false);
    });
    connect(m_noticeSort, &QComboBox::currentIndexChanged, this, [this] { showNotifications(true, false); });

    m_notifications = new QListWidget(panel);
    m_notifications->setAccessibleName(QStringLiteral("Уведомления"));
    m_noticeReadTimer = new QTimer(panel);
    m_noticeReadTimer->setObjectName(QStringLiteral("noticeReadTimer"));
    m_noticeReadTimer->setSingleShot(true);
    m_noticeReadTimer->setInterval(160);
    connect(m_noticeReadTimer, &QTimer::timeout, this, &MainWindow::readVisibleNotices);
    connect(m_notifications->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        scheduleAvatars();
        scheduleVisibleNoticeRead();
        scheduleOlderNotifications();
    });
    connect(m_notifications->verticalScrollBar(), &QScrollBar::rangeChanged,
            this, &MainWindow::scheduleOlderNotifications);
    connect(m_notifications, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (item)
            m_noticeSelectedId = item->data(workspace::Id).toString();
        updateNotificationActions();
    });
    const auto beginAction = [this](const QString &action) {
        m_noticeAction = action;
        setStatus(QStringLiteral("Обновляем уведомления…"), true);
        updateNotificationActions();
    };
    connect(m_noticeRead, &QAction::triggered, this, [this, beginAction] {
        const auto *row = m_notifications->currentItem();
        if (!row || !m_noticeRead->isEnabled())
            return;
        const auto id = row->data(workspace::Id).toString();
        if (m_clientUpdates && id == m_clientUpdates->notice().id) {
            m_clientUpdates->markRead();
            return;
        }
        beginAction(operationKey(OperationKind::ReadNotification));
        m_backend->markNotificationRead(id);
    });
    connect(m_noticeReadAll, &QPushButton::clicked, this, [this, beginAction] {
        if (!m_noticeReadAll->isEnabled())
            return;
        if (m_clientUpdates)
            m_clientUpdates->markRead();
        if (!writable() || (m_noticeHistory.isEmpty() && m_noticeSearchResults.isEmpty()))
            return;
        beginAction(operationKey(OperationKind::ReadAllNotifications));
        m_backend->markAllNotificationsRead();
    });
    connect(m_noticeDelete, &QAction::triggered, this, [this, beginAction] {
        const auto *row = m_notifications->currentItem();
        if (!row || !m_noticeDelete->isEnabled())
            return;
        const auto id = row->data(workspace::Id).toString();
        if (QMessageBox::question(this, QStringLiteral("Удалить уведомление"),
                QStringLiteral("Удалить выбранное уведомление?"), QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel) != QMessageBox::Yes || !writable() || !m_noticeAction.isEmpty())
            return;
        beginAction(operationKey(OperationKind::DeleteNotification));
        m_backend->deleteNotification(id);
    });
    auto *delegate = workspace::configureRows(m_notifications, workspace::Row::Notice);
    bindMessageFiles(m_notifications, true);
    delegate->setNoticeHandlers([this](const QModelIndex &index, int target) {
        QTimer::singleShot(0, this, [this, index = QPersistentModelIndex(index), target] {
            if (index.isValid())
                openNotificationTarget(m_notifications->item(index.row()), target);
        });
    }, [this](const QModelIndex &index) {
        toggleNoticeDetails(m_notifications->item(index.row()));
    }, [this](const QModelIndex &index, const QPoint &position) {
        showNoticeMenu(m_notifications->item(index.row()), position);
    });
    const auto noticeShortcut = [this](const QList<QKeySequence> &keys) {
        auto *action = new QAction(m_notifications);
        action->setShortcuts(keys);
        action->setShortcutContext(Qt::WidgetShortcut);
        m_notifications->addAction(action);
        return action;
    };
    connect(noticeShortcut({QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter), QKeySequence(Qt::Key_Space)}),
            &QAction::triggered, this, [this] { toggleNoticeDetails(m_notifications->currentItem()); });
    connect(noticeShortcut({QKeySequence(Qt::CTRL | Qt::Key_Return), QKeySequence(Qt::CTRL | Qt::Key_Enter)}),
            &QAction::triggered, this, [this] {
        if (const auto *item = m_notifications->currentItem()) {
            const auto notice = item->data(workspace::NoticePresentation).value<workspace::NoticeRow>();
            openNotificationTarget(item, notice.primaryTarget);
        }
    });
    connect(noticeShortcut({QKeySequence(Qt::Key_Menu), QKeySequence(Qt::SHIFT | Qt::Key_F10)}),
            &QAction::triggered, this, [this] {
        auto *item = m_notifications->currentItem();
        if (item)
            showNoticeMenu(item, m_notifications->visualItemRect(item).topLeft());
    });
    m_notifications->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_notifications, &QListWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        showNoticeMenu(m_notifications->itemAt(position), position);
    });
    layout->addWidget(m_notifications, 1);
    m_notificationsEmpty = emptyState(QStringLiteral("Уведомлений пока нет"), panel);
    m_notificationsEmpty->setAccessibleName(QStringLiteral("Состояние уведомлений"));
    layout->addWidget(m_notificationsEmpty, 1);
    m_notifications->hide();
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
    scheduleOlderNotifications();
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
        scheduleOlderNotifications();
    }
    if (event->type() == QEvent::ActivationChange && !isActiveWindow() && m_taskChatPressActive)
        m_taskChatPressEnd->start(0);
}

void MainWindow::onSectionChanged(int index)
{
    m_pendingMessageProject = {};
    if (index == 2) {
        m_failedNoticeRead.clear();
        m_noticeOlderFailed = false;
    } else if (m_lastSection == 2) {
        m_noticeReadTimer->stop();
        m_readNoticesAfterPending = false;
        m_noticeExpandedId.clear();
    }
    if (m_lastSection == 1 && index != 1) {
        m_noticeProjectId.clear();
        m_projectSearchTimer->stop();
        ui::setStatusLabel(m_projectTaskSearchStatus, {});
        closeProjectTaskChats();
    }
    m_lastSection = index;
    if (index != 1 && m_projectTools)
        m_projectTools->reject();
    if (index != 0) {
        static_cast<ui::HistoryList *>(m_chatMessages)->setLiveReadHistory(false);
        resetMessageContext();
    }
    m_stack->setCurrentIndex(index);
    emit activeSectionChanged(index);
    setStatus(QString());
    refreshCurrentSection(false);
    if (index == 2)
        m_notifications->scrollToTop();
    if (index == 1 && !m_projectSearchDisplayed)
        runProjectTaskSearch();
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
        const auto previousProject = m_currentProjectId;
        const bool lookingUpNotice = !m_noticeProjectId.isEmpty();
        const bool noticeNeedsRefresh = lookingUpNotice && !m_projectsMoreAvailable &&
            std::none_of(m_projectDirectory.cbegin(), m_projectDirectory.cend(),
                         [this](const Project &project) { return project.id == m_noticeProjectId; });
        if (reloadProjectDirectory || !m_projectDirectoryLoaded || noticeNeedsRefresh) {
            if (!m_projectDirectoryLoaded) {
                m_projects->clear();
                workspace::loadingRow(m_projects, QStringLiteral("Загружаем проекты…"));
            }
            m_backend->requestProjects();
        } else {
            const auto scroll = m_projects->verticalScrollBar()->value();
            showProjects();
            if (!lookingUpNotice)
                m_projects->verticalScrollBar()->setValue(scroll);
        }
        if (!m_currentProjectId.isEmpty() && m_currentProjectId == previousProject) {
            if (m_projectTabs->currentIndex() == 1) {
                if (reloadProjectDirectory)
                    refreshProjectTaskChats(m_currentProjectId);
                else
                    openProjectTaskChats();
            } else if (m_ganttMode) {
                m_projectTaskViewRequested = true;
                m_backend->requestProjectTimeline(m_currentProjectId);
            } else {
                m_projectTaskViewRequested = true;
                requestCurrentProjectBoard();
            }
        }
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
        if (!m_noticeHistoryRequested || reloadProjectDirectory || m_noticeHistoryState == NoticeHistoryState::Failed)
            requestNotificationHistory();
        else
            showNotifications();
        if (m_noticeSearch->text().trimmed() != m_noticeQuery || m_noticeQuery.size() >= 3)
            runNotificationSearch();
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
    if (m_lastSection == 2) {
        m_failedNoticeRead.clear();
        m_noticeOlderFailed = false;
    }
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
    m_pendingMessageProject = {};
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

void MainWindow::onProjectSelected()
{
    auto *item = m_projects->currentItem();
    if (!item)
        return;
    const auto projectId = item->data(workspace::Id).toString();
    if (projectId.isEmpty())
        return;
    if (projectId != m_currentProjectId && !clearTask()) {
        const QSignalBlocker blocker(m_projects);
        for (int i = 0; i < m_projects->count(); ++i) {
            if (m_projects->item(i)->data(workspace::Id).toString() == m_currentProjectId) {
                m_projects->setCurrentRow(i);
                return;
            }
        }
        m_projects->setCurrentItem(nullptr);
        return;
    }
    const auto name = item->data(workspace::Title).toString();
    m_projectTitle->setText(name);
    m_projectTitle->setToolTip(ui::plainTooltip(name));
    m_projectAvatar->setTitle(name, workspace::AvatarKind::Project,
                             item->data(workspace::ProjectAvatarColor).value<QColor>(),
                             item->data(workspace::ProjectAvatarVariant).toString(),
                             item->data(workspace::ProjectAvatarType).toString());
    m_projectAvatarUrl = item->data(workspace::AvatarUrl).toUrl();
    if (const auto *photo = m_avatars.object(m_projectAvatarUrl))
        m_projectAvatar->setPhoto(*photo);
    scheduleAvatars();
    if (projectId == m_currentProjectId)
        return;
    if (m_projectTools)
        m_projectTools->reject();
    closeProjectTaskChats();
    m_currentProjectId = projectId;
    updateProjectTaskChatTab();
    m_projectTaskViewRequested = false;
    m_stages.clear();
    m_loadedBoardTaskIds.clear();
    m_projectSearchDisplayed = false;
    m_projectSearchResults.clear();
    m_filter->clear();
    updateWriteAvailability();
    setStatus(QString());
    m_board->clearBoard(QStringLiteral("Загружаем доску…"), !m_ganttMode);
    m_board->setColumnWidthContext(projectId,
        chatPreferenceKey(m_backend->settingsScope(), QLatin1String("kanbanColumnWidths")));
    if (m_projectTabs->currentIndex() == 1)
        openProjectTaskChats();
    else
        ensureProjectTaskView();
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
    if (m_projects && watched == m_projects->viewport() && event->type() == QEvent::Wheel) {
        const auto *wheel = static_cast<QWheelEvent *>(event);
        const auto delta = wheel->pixelDelta().isNull() ? wheel->angleDelta() : wheel->pixelDelta();
        if (delta.y() < 0 && -qint64(delta.y()) >= qAbs(qint64(delta.x())))
            scheduleProjectsPageCheck();
    } else if (watched == m_projects && event->type() == QEvent::KeyPress) {
        const auto key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Down || key == Qt::Key_PageDown || key == Qt::Key_End)
            scheduleProjectsPageCheck();
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
    if (!m_projects)
        return;
    scheduleAvatars();
    scheduleVisibleNoticeRead();
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

    if (!m_noticeAction.isEmpty() && operation == m_noticeAction) {
        m_noticeAction.clear();
        updateNotificationActions();
        setStatus(reason);
        return;
    }

    bool statusPresented = false;
    QString footer = reason;
    switch (kind) {
    case OperationKind::ProjectTaskChats:
        if (!m_projectTaskChatScope.isEmpty() && m_projectTaskChatPending >= 0
            && ids.value(0) == m_projectTaskChatScope) {
            m_projectTaskChatPending = -1;
            m_projectTaskChatError = reason.isEmpty() ? QStringLiteral("Не удалось загрузить чаты задач") : reason;
            updateProjectTaskChatControls();
        }
        return;
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
    case OperationKind::NotificationSettings:
    case OperationKind::NotificationSettingsUpdate: {
        const auto *settings = findChild<QDialog *>(QStringLiteral("notificationSettingsDialog"));
        if (settings && settings->isVisible())
            return; // The dialog owns recovery while it is open.
        break;
    }
    case OperationKind::BoardColumn: {
        const auto stage = ids.value(1);
        if (ids.value(0) == m_currentProjectId &&
            m_board->stagePageLoading(stage)) {
            m_board->failStagePage(stage);
            setStatus(QStringLiteral("%1 · F5 — обновить доску").arg(reason));
        }
        return;
    }
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
    case OperationKind::ProjectMembers:
        if (m_projectTools && ids.value(0) == m_projectTools->projectId())
            m_projectTools->membersFailed(reason);
        return;
    case OperationKind::ProjectFiles:
        if (m_projectTools && ids.value(0) == m_projectTools->projectId())
            m_projectTools->filesFailed(ids.value(1), reason);
        return;
    case OperationKind::ReadNotifications:
        finishVisibleNoticeRead(false);
        return;
    case OperationKind::Notifications:
        m_noticeHistoryState = NoticeHistoryState::Failed;
        if (m_lastSection == 2) {
            showNotifications();
            setStatus(reason);
        }
        return;
    case OperationKind::NotificationsOlder:
        m_noticeOlderFailed = true;
        if (m_lastSection == 2 && m_noticeQuery.isEmpty())
            setStatus(reason);
        return;
    case OperationKind::SearchNotifications:
        if (ids.value(0) == m_noticeQuery &&
            m_noticeQuery == m_noticeSearch->text().trimmed()) {
            m_noticeSearchPending = false;
            showNotifications();
            setStatus(reason);
        }
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
    case OperationKind::Timeline:
        if (!m_ganttMode || m_currentProjectId.isEmpty() || ids.value(0) != m_currentProjectId)
            break;
        m_projectTimeline->clear(reason);
        return;
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
    case OperationKind::SearchProjectTasks: {
        const auto projectId = ids.value(0);
        const auto query = m_pendingProjectSearches.take(projectId);
        if (m_lastSection == 1 && !m_ganttMode && projectId == m_currentProjectId && !query.isEmpty()) {
            if (query == m_filter->text().trimmed())
                ui::setStatusLabel(m_projectTaskSearchStatus, QStringLiteral("Поиск по проекту недоступен. %1").arg(reason));
            else
                runProjectTaskSearch();
        }
        return;
    }
    case OperationKind::ChatPin:
    case OperationKind::ProjectPin:
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
    case OperationKind::CreateProject:
        if (!m_pendingCreateProject)
            break;
        m_pendingCreateProject = false;
        if (m_projectForm)
            m_projectForm->setStatus(reason);
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
    case OperationKind::BoardEdit:
        if (m_pendingBoardEdit.isEmpty() || ids.value(0) != m_pendingBoardEdit)
            break;
        if (m_stageManager && m_stageManager->projectId() == m_pendingBoardEdit)
            m_stageManager->setStatus(reason);
        if (m_card && m_card->taskId() == m_pendingMoveTask)
            m_card->setStatus(reason);
        m_pendingBoardEdit.clear();
        m_pendingMoveTask.clear();
        break;
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
    case OperationKind::Board:
        if (m_currentProjectId.isEmpty() || ids.value(0) != m_currentProjectId)
            break;
        m_board->resetStagePageLoads(false);
        if (m_stages.isEmpty())
            m_board->clearBoard(QStringLiteral("Не удалось загрузить доску. Нажмите «Обновить»"));
        else if (!m_projectSearchDisplayed)
            m_board->setStages(m_stages);
        if (m_currentProjectId == m_boardMoveRefreshProject)
            footer = QStringLiteral("Не удалось обновить доску после переноса. Нажмите F5");
        break;
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
    case OperationKind::Projects:
    case OperationKind::ProjectPageFallback: {
        const auto link = std::exchange(m_pendingMessageProject, {});
        if (!link.url.isEmpty())
            QDesktopServices::openUrl(link.url);
        m_noticeProjectId.clear();
        m_cardRequestedProjectDirectory = false;
        if (m_projectsPagePending) {
            m_projectsPagePending = false;
            m_projectsPageFailed = true;
            updateProjectsRailPagination();
        }
        if (kind == OperationKind::Projects && m_lastSection == 1 && !m_projectDirectoryLoaded) {
            m_projects->clear();
            workspace::emptyRow(m_projects, QStringLiteral("Не удалось загрузить проекты"));
        }
        break;
    }
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
        if (m_lastSection != 2 && m_noticeHistoryState == NoticeHistoryState::Failed && m_sessionRecovery->isHidden())
            requestNotificationHistory();
        scheduleVisibleChatRead();
        m_failedNoticeRead.clear();
        m_noticeOlderFailed = false;
        scheduleVisibleNoticeRead();
        scheduleOlderNotifications();
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
    connect(m_backend, &Backend::projectMembersLoaded, this, [this](const QString &project, const auto &members) {
        if (m_projectTools && m_projectTools->projectId() == project)
            m_projectTools->setMembers(members);
    });
    connect(m_backend, &Backend::projectFilesLoaded, this,
            [this](const QString &project, const QString &folder, const auto &files, int next) {
        if (m_projectTools && m_projectTools->projectId() == project)
            m_projectTools->setFiles(folder, files, next);
    });
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
    connect(m_backend, &Backend::projectTimelineLoaded, this, [this](const QString &project, const auto &tasks, bool limited) {
        if (m_ganttMode && project == m_currentProjectId)
            m_projectTimeline->setTasks(tasks, limited);
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
        if (!project.isEmpty() && project == m_currentProjectId && !refreshProjectTaskChats(project)) {
            if (m_ganttMode)
                m_backend->requestProjectTimeline(project);
            else
                requestCurrentProjectBoard();
        }
        updateWriteAvailability();
    });
    connect(m_backend, &Backend::taskLinkPreviewInvalidated, this, &MainWindow::invalidateTaskLinkPreview);
    connect(m_backend, &Backend::taskUpdated, this, &MainWindow::invalidateTaskLinkPreview);
    connect(m_backend, &Backend::taskUpdated, this, &MainWindow::onTaskUpdated);
    connect(m_backend, &Backend::taskDeleted, this, &MainWindow::onTaskDeleted);
    connect(m_backend, &Backend::taskCreated, this, &MainWindow::onTaskCreated);
    connect(m_backend, &Backend::projectCreated, this, &MainWindow::onProjectCreated);
    connect(m_backend, &Backend::boardUpdated, this, &MainWindow::onBoardUpdated);
    connect(m_backend, &Backend::chatsFound, this,
            [this](const auto &query, const auto &chats) { onChatsFound(0, query, chats); });
    connect(m_backend, &Backend::taskChatsFound, this,
            [this](const auto &query, const auto &chats) { onChatsFound(1, query, chats); });
    connect(m_backend, &Backend::messagesFound, this, &MainWindow::onMessagesFound);
    connect(m_backend, &Backend::projectTasksFound, this, &MainWindow::onProjectTasksFound);
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
    updateNotificationActions();
    m_newTaskButton->setEnabled(writable() && !m_currentProjectId.isEmpty() && m_pendingCreateTask.isEmpty());
    m_projectToolsButton->setEnabled(!m_currentProjectId.isEmpty());
    m_newProjectButton->setEnabled(writable() && !m_pendingCreateProject);
    m_stagesButton->setEnabled(writable() && !m_currentProjectId.isEmpty() && m_pendingBoardEdit.isEmpty());
    if (m_card) {
        const bool deleting = !m_pendingTaskDelete.isEmpty();
        m_card->setWriteState(writable() && m_cardDetailsReady && !deleting, !m_pendingTaskEdit.isEmpty() || deleting,
                              !m_pendingBoardEdit.isEmpty(),
                              m_taskSendController.pending() || pastedChatImagePending(m_card->taskId(), true),
                              !m_pendingUpload.isEmpty());
    }
    if (m_stageManager)
        m_stageManager->setWritable(writable() && m_pendingBoardEdit != m_stageManager->projectId());
    if (m_taskForm) {
        const auto task = m_taskForm->createdTask();
        const bool canWrite = writable() && m_pendingCreateTask.isEmpty() && m_pendingCreateSubtask.isEmpty()
            && (task.isEmpty() || m_pendingUpload.isEmpty());
        m_taskForm->setEnabledForWrite(canWrite, !task.isEmpty()
            && m_pendingUpload == operationKey(OperationKind::AttachTask, task));
    }
    if (m_projectForm)
        m_projectForm->setWritable(writable() && !m_pendingCreateProject);
}

bool MainWindow::canMoveBoardTask(const QString &taskId, const QString &sourceStage, const QString &targetStage) const
{
    if (!writable() || m_lastSection != 1 || m_ganttMode || m_board->view() != BoardWidget::View::Kanban
        || m_currentProjectId.isEmpty() || m_currentProjectId == m_boardMoveRefreshProject
        || !m_pendingBoardEdit.isEmpty() || taskId.isEmpty() || sourceStage == targetStage
        || targetStage == QLatin1String("0"))
        return false;

    const auto boardStage = [this](const QString &id) {
        const bool demoId = m_backend->isDemo() && id.startsWith(QLatin1String("s-"));
        const auto number = demoId ? id.mid(2) : id;
        bool parsed = false;
        const auto value = number.toLongLong(&parsed);
        return parsed && value >= 0 && number == QString::number(value)
            ? std::find_if(m_stages.cbegin(), m_stages.cend(), [&id](const Stage &column) { return column.id == id; })
            : m_stages.cend();
    };
    const auto source = boardStage(sourceStage);
    if (source == m_stages.cend() || (!targetStage.isEmpty() && boardStage(targetStage) == m_stages.cend()))
        return false;
    return std::any_of(source->tasks.cbegin(), source->tasks.cend(), [this, &taskId](const Task &task) {
        return task.id == taskId && (task.projectId.isEmpty() || task.projectId == m_currentProjectId);
    });
}

QString MainWindow::taskLinkFor(const QString &taskId) const
{
    return ui::taskLink(m_backend->portalOrigin(), m_currentProjectId, taskId);
}

void MainWindow::openTaskCard(const QString &taskId)
{
    openTaskCardForProject(taskId, m_lastSection == 1 ? m_currentProjectId : QString());
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
    m_pendingMessageProject = {};
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
    const bool knownProject = !projectId.isEmpty() && projectId == m_currentProjectId;
    if (m_card) {
        if (m_card->taskId() == taskId && m_card->projectId() == projectId) {
            m_card->activateWindow();
            return;
        }
        if (!clearTask())
            return;
    }

    Task task;
    task.id = taskId;
    for (const auto &stage : knownProject ? m_stages : QVector<Stage>()) {
        for (const auto &candidate : stage.tasks) {
            if (candidate.id == taskId) {
                task = candidate;
                break;
            }
        }
    }
    if (task.name.isEmpty() && knownProject) {
        for (const auto &candidate : m_projectSearchResults) {
            if (candidate.id == taskId) {
                task = candidate;
                break;
            }
        }
    }

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
    card->setProjectChoices(m_projectDirectory);
    card->setCanDetachProject(!m_backend->isDemo());
    card->setMoreProjectsAvailable(m_projectsMoreAvailable);
    card->setOnMoreProjects([this] { requestNextProjectsPage(); });
    m_loadCardChat = true;
    m_cardDetailsReady = false;
    m_cardSummaryPending = true;
    m_cardRequestedProjectDirectory = false;
    m_cardRequestedStages.clear();
    m_cardSearchQuery.clear();
    m_cardContextMessageId.clear();
    switchDraft(card->composer(), m_taskDrafts, {}, taskId);
    if (knownProject && card->projectId() == m_currentProjectId) {
        card->setStages(m_stages);
        card->setProjectCaption(m_projectTitle->text());
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
        m_pendingMessageProject = {};
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
    if (!project.isEmpty() && project == m_currentProjectId && !refreshProjectTaskChats(project)) {
        if (m_ganttMode)
            m_backend->requestProjectTimeline(project);
        else
            requestCurrentProjectBoard();
    }
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
    if (!m_currentProjectId.isEmpty() && (project == m_currentProjectId || targetProject == m_currentProjectId)
        && !refreshProjectTaskChats(m_currentProjectId)) {
        if (m_ganttMode)
            m_backend->requestProjectTimeline(m_currentProjectId);
        else
            requestCurrentProjectBoard();
    }
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
    if (projectId == m_currentProjectId) {
        if (!refreshProjectTaskChats(projectId)) {
            if (m_ganttMode)
                m_backend->requestProjectTimeline(projectId);
            else
                requestCurrentProjectBoard();
        }
    }
    if (requested && !attaching && m_lastSection == 1)
        openTaskCardForProject(taskId, projectId);
    setStatus(QStringLiteral("Задача создана"));
    updateWriteAvailability();
}

void MainWindow::onProjectCreated(const QString &projectId)
{
    if (m_pendingCreateProject) {
        m_pendingCreateProject = false;
        if (m_projectForm)
            m_projectForm->accept();
        m_pendingProjectId = projectId;
    }
    m_backend->requestProjects();
    setStatus(QStringLiteral("Проект создан"));
    updateWriteAvailability();
}

void MainWindow::onBoardUpdated(const QString &projectId)
{
    bool refreshSearch = false;
    if (!m_pendingBoardEdit.isEmpty() && projectId == m_pendingBoardEdit) {
        if (m_stageManager && m_stageManager->projectId() == projectId)
            m_stageManager->setStatus(QStringLiteral("Этапы обновлены"));
        const auto movedTask = m_pendingMoveTask;
        m_pendingBoardEdit.clear();
        m_pendingMoveTask.clear();
        if (!movedTask.isEmpty() && projectId == m_currentProjectId) {
            m_boardMoveRefreshProject = projectId;
            refreshSearch = m_projectSearchDisplayed;
            setStatus(QStringLiteral("Задача перенесена"));
        }
        if (m_card && !movedTask.isEmpty() && movedTask == m_card->taskId()) {
            m_card->setStatus(QStringLiteral("Задача перенесена"));
            m_backend->requestTask(movedTask);
        }
    }
    if (projectId == m_currentProjectId)
        requestCurrentProjectBoard();
    if (refreshSearch)
        runProjectTaskSearch();
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

void ClientUpdateFeed::publish(const QString &tag, const QUrl &downloadUrl)
{
    const auto id = QStringLiteral("client-update:") + tag;
    if (m_notice.id == id)
        return;
    m_notice = {};
    m_notice.id = id;
    m_notice.title = QStringLiteral("Доступно обновление Битрикс24 %1 для Windows").arg(tag);
    m_notice.text = QStringLiteral("Скачать ZIP");
    m_notice.markup = QStringLiteral("[URL=%1]%2[/URL]").arg(downloadUrl.toString(), m_notice.text);
    prepareNotificationDisplay(m_notice);
    m_notice.createdAt = QDateTime::currentDateTimeUtc();
    emit changed();
}

void ClientUpdateFeed::markRead()
{
    if (m_notice.id.isEmpty() || m_notice.read)
        return;
    m_notice.read = true;
    emit changed();
}

void MainWindow::setClientUpdateFeed(ClientUpdateFeed *feed)
{
    if (m_clientUpdates)
        disconnect(m_clientUpdates, nullptr, this, nullptr);
    m_clientUpdates = feed;
    const auto refresh = [this] {
        updateNotificationRailBadge();
        showNotifications();
    };
    connect(feed, &ClientUpdateFeed::changed, this, refresh);
    refresh();
}

void MainWindow::updateNotificationRailBadge()
{
    QSet<QString> unread;
    for (const auto &notice : m_noticeHistory)
        if (!notice.read && !notice.id.isEmpty())
            unread.insert(notice.id);
    if (m_clientUpdates && !m_clientUpdates->notice().read && !m_clientUpdates->notice().id.isEmpty())
        unread.insert(m_clientUpdates->notice().id);
    setRailUnreadCount(2, unread.size());
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

void MainWindow::requestNotificationHistory()
{
    if (m_noticeHistoryState == NoticeHistoryState::Loading)
        return;
    m_noticeHistoryRequested = true;
    m_noticeHistoryState = NoticeHistoryState::Loading;
    if (m_lastSection == 2)
        showNotifications();
    m_backend->requestNotifications();
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

void MainWindow::runProjectTaskSearch()
{
    m_projectSearchTimer->stop();
    const auto query = m_filter->text().trimmed();
    if (m_lastSection != 1 || m_projectTabs->currentIndex() != 0 || m_ganttMode
        || m_currentProjectId.isEmpty() || query.isEmpty())
        return;
    ui::setStatusLabel(m_projectTaskSearchStatus, QStringLiteral("Ищем задачи в проекте…"), true);
    if (m_pendingProjectSearches.contains(m_currentProjectId))
        return;
    m_pendingProjectSearches.insert(m_currentProjectId, query);
    m_backend->searchProjectTasks(m_currentProjectId, query);
}

void MainWindow::onProjectTasksFound(const QString &projectId, const QString &query, const QVector<Task> &tasks)
{
    if (m_pendingProjectSearches.value(projectId) != query)
        return;
    m_pendingProjectSearches.remove(projectId);
    if (m_lastSection != 1 || m_ganttMode || projectId != m_currentProjectId)
        return;
    if (query != m_filter->text().trimmed()) {
        runProjectTaskSearch();
        return;
    }
    m_projectSearchResults = tasks;
    m_projectSearchDisplayed = true;
    ui::setStatusLabel(m_projectTaskSearchStatus, {});
    displayProjectTaskResults();
}

void MainWindow::displayProjectTaskResults()
{
    auto displayed = m_stages;
    for (auto &stage : displayed)
        stage.tasks.clear();
    for (const auto &task : m_projectSearchResults) {
        auto stage = std::find_if(displayed.begin(), displayed.end(), [&task](const Stage &candidate) {
            return candidate.id == task.stageId;
        });
        if (stage == displayed.end()) {
            displayed.append({task.stageId,
                task.stageName.isEmpty() ? QStringLiteral("Без этапа") : task.stageName, {}, {}});
            stage = std::prev(displayed.end());
        }
        stage->tasks.append(task);
    }
    m_board->setStages(displayed, true);
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
        if (m_projectTools)
            m_projectTools->reject();
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

void MainWindow::onProjectScopeChanged(int scope)
{
    m_projectViews->setCurrentIndex(scope);
    if (scope == 1) {
        m_projectSearchTimer->stop();
        ui::setStatusLabel(m_projectTaskSearchStatus, {});
        openProjectTaskChats();
    } else {
        closeProjectTaskChats();
        ensureProjectTaskView();
        if (!m_projectSearchDisplayed)
            runProjectTaskSearch();
    }
    scheduleAvatars();
}

void MainWindow::ensureProjectTaskView()
{
    if (m_projectTaskViewRequested || m_currentProjectId.isEmpty() || m_lastSection != 1)
        return;
    m_projectTaskViewRequested = true;
    if (m_ganttMode) {
        m_projectTimeline->clear(QStringLiteral("Загружаем плановые даты…"), true);
        m_backend->requestProjectTimeline(m_currentProjectId);
    } else {
        requestCurrentProjectBoard();
    }
}

void MainWindow::openProjectTaskChats()
{
    if (m_lastSection != 1 || m_currentProjectId.isEmpty() || m_projectTaskChatScope == m_currentProjectId)
        return;
    closeProjectTaskChats();
    m_projectTaskChatScope = m_currentProjectId;
    requestProjectTaskChatPage();
}

void MainWindow::closeProjectTaskChats()
{
    if (m_projectTaskChatScope.isEmpty())
        return;
    m_projectTaskChatScope.clear();
    m_projectTaskChatError.clear();
    m_projectTaskChatNext = 0;
    m_projectTaskChatPending = -1;
    m_projectTaskChatUnread = 0;
    m_projectTaskChatRows.clear();
    m_projectTaskChats->clear();
    updateProjectTaskChatControls();
    m_backend->requestProjectTaskChats({}, 0);
}

bool MainWindow::refreshProjectTaskChats(const QString &project)
{
    if (project.isEmpty() || project != m_currentProjectId || m_lastSection != 1 || m_projectTabs->currentIndex() != 1)
        return false;
    m_projectTaskViewRequested = false;
    closeProjectTaskChats();
    openProjectTaskChats();
    return true;
}

void MainWindow::requestProjectTaskChatPage()
{
    if (m_lastSection != 1 || m_projectTabs->currentIndex() != 1
        || m_projectTaskChatScope.isEmpty() || m_projectTaskChatScope != m_currentProjectId
        || m_projectTaskChatPending >= 0 || m_projectTaskChatNext < 0)
        return;
    m_projectTaskChatError.clear();
    m_projectTaskChatPending = m_projectTaskChatNext;
    updateProjectTaskChatControls();
    m_backend->requestProjectTaskChats(m_projectTaskChatScope, m_projectTaskChatPending);
}

void MainWindow::onProjectTaskChatsLoaded(const QString &project, int offset, const QVector<Chat> &rows, int nextOffset)
{
    if (project.isEmpty() || project != m_currentProjectId || project != m_projectTaskChatScope
        || m_projectTaskChatPending < 0 || offset != m_projectTaskChatPending)
        return;
    m_projectTaskChatPending = -1;
    m_projectTaskChatNext = nextOffset > offset ? nextOffset : -1;
    for (const auto &chat : rows) {
        if (!chat.taskChat || chat.taskId.isEmpty() || chat.id.isEmpty())
            continue;
        const auto found = m_projectTaskChatRows.constFind(chat.id);
        if (found == m_projectTaskChatRows.cend()) {
            m_projectTaskChatRows.insert(chat.id, ui::addChatRow(m_projectTaskChats, chat));
            m_projectTaskChatUnread += qMax(0, chat.unread);
        }
    }
    updateProjectTaskChatBadges(project, rows);
    updateProjectTaskChatControls();
    scheduleAvatars();
}

void MainWindow::updateProjectTaskChatBadges(const QString &project, const QVector<Chat> &rows)
{
    if (project.isEmpty() || project != m_projectTaskChatScope || project != m_currentProjectId)
        return;
    const auto previous = m_projectTaskChatUnread;
    for (const auto &chat : rows) {
        auto *item = m_projectTaskChatRows.value(chat.id);
        if (item) {
            m_projectTaskChatUnread += qMax(0, chat.unread) - qMax(0, item->data(workspace::Count).toInt());
            ui::setChatRowBadges(item, chat);
        }
    }
    if (m_projectTaskChatUnread != previous)
        updateProjectTaskChatTab();
}

void MainWindow::updateProjectTaskChatTab()
{
    const bool confirmed = m_projectUnreadCountsComplete;
    const auto aggregate = m_projectUnreadCounts.value(m_currentProjectId);
    const bool hasScopedPage = !m_currentProjectId.isEmpty() && m_projectTaskChatScope == m_currentProjectId;
    const auto scoped = hasScopedPage ? m_projectTaskChatUnread : qint64(0);
    const auto unread = confirmed ? aggregate : qMax(aggregate, scoped);
    const bool partial = !confirmed && (!hasScopedPage || m_projectTaskChatNext >= 0 || aggregate > scoped);
    const auto caption = unread <= 0 ? QStringLiteral("Чаты")
        : partial ? QStringLiteral("Чаты (%1+)").arg(unread) : QStringLiteral("Чаты (%1)").arg(unread);
    m_projectTabs->setTabText(1, caption);
    const auto scope = unread <= 0 ? QString()
        : confirmed ? QStringLiteral("Непрочитанных сообщений в чатах проекта: %1").arg(unread)
        : aggregate > scoped ? QStringLiteral("Непрочитанных сообщений в чатах проекта: не менее %1").arg(unread)
                             : QStringLiteral("Непрочитанных сообщений в загруженных чатах: %1").arg(unread);
    m_projectTabs->setTabToolTip(1, scope);
    m_projectTabs->setAccessibleTabName(1, scope.isEmpty() ? QStringLiteral("Чаты")
                                                        : QStringLiteral("Чаты. ") + scope);
}

void MainWindow::updateProjectTaskChatControls()
{
    const bool loading = m_projectTaskChatPending >= 0;
    const bool empty = m_projectTaskChatRows.isEmpty();
    QString status = m_projectTaskChatError;
    if (empty && status.isEmpty()) {
        if (m_projectTaskChatScope.isEmpty())
            status = QStringLiteral("Выберите проект");
        else if (loading)
            status = QStringLiteral("Загружаем чаты задач…");
        else if (m_projectTaskChatNext > 0)
            status = QStringLiteral("В загруженных задачах нет чатов");
        else
            status = QStringLiteral("В этом проекте пока нет чатов задач");
    }
    ui::setStatusLabel(m_projectTaskChatStatus, status, empty && loading);
    m_projectTaskChats->setVisible(!empty);
    m_moreProjectTaskChats->setVisible(!m_projectTaskChatError.isEmpty() || m_projectTaskChatNext > 0);
    m_moreProjectTaskChats->setEnabled(!loading);
    m_moreProjectTaskChats->setText(loading ? QStringLiteral("Загружаем…")
        : m_projectTaskChatError.isEmpty() ? QStringLiteral("Ещё чаты") : QStringLiteral("Повторить"));
    updateProjectTaskChatTab();
}

void MainWindow::updateProjectUnreadRows()
{
    if (!m_projects || !m_projectTabs)
        return;
    for (int row = 0; row < m_projects->count(); ++row) {
        auto *item = m_projects->item(row);
        const auto id = item->data(workspace::Id).toString();
        if (!id.isEmpty())
            ui::setProjectRowUnreadCount(item, m_projectUnreadCounts.value(id), !m_projectUnreadCountsComplete);
    }
    updateProjectTaskChatTab();
}

void MainWindow::scheduleProjectsPageCheck()
{
    if (m_projectsPageCheckQueued || !m_projectsMoreAvailable || m_projectsPagePending || m_projectsPageFailed)
        return;
    m_projectsPageCheckQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_projectsPageCheckQueued = false;
        maybeLoadMoreProjects();
    });
}

void MainWindow::maybeLoadMoreProjects()
{
    if (m_lastSection != 1 || !m_projects->isVisible() || m_projectsPageFailed)
        return;
    const auto *bar = m_projects->verticalScrollBar();
    const auto span = qint64(bar->maximum()) - bar->minimum();
    if (span > 0 && 3 * (qint64(bar->value()) - bar->minimum()) >= 2 * span)
        requestNextProjectsPage();
}

void MainWindow::requestNextProjectsPage()
{
    if (!m_projectDirectoryLoaded || !m_projectsMoreAvailable || m_projectsPagePending)
        return;
    m_projectsPagePending = true;
    updateProjectsRailPagination();
    m_backend->requestMoreProjects();
}

void MainWindow::updateProjectsRailPagination()
{
    const auto *bar = m_projects->verticalScrollBar();
    // Allow for the button's own height when deciding whether the list can scroll without it.
    const auto buttonSpace = m_moreProjectsButton->isHidden() ? 0
        : m_moreProjectsButton->height() + m_projects->parentWidget()->layout()->spacing();
    const bool shortList = bar->maximum() - bar->minimum() <= buttonSpace;
    m_moreProjectsButton->setVisible(m_projectsMoreAvailable
        && (m_projectsPageFailed || shortList || !m_projectNameQuery.isEmpty() || !m_noticeProjectId.isEmpty()));
    m_moreProjectsButton->setEnabled(m_projectsMoreAvailable && !m_projectsPagePending);
    m_moreProjectsButton->setText(m_projectsPagePending ? QStringLiteral("Загружаем…")
        : m_projectsPageFailed ? QStringLiteral("Повторить загрузку проектов") : QStringLiteral("Ещё проекты"));
}

void MainWindow::showProjects()
{
    auto ordered = m_projectDirectory;
    if (!m_projectNameQuery.isEmpty()) {
        // Поиск по названию идёт по загруженному каталогу: пагинация и подгрузка сохраняются.
        ordered.erase(std::remove_if(ordered.begin(), ordered.end(), [this](const Project &project) {
            return !project.name.contains(m_projectNameQuery, Qt::CaseInsensitive);
        }), ordered.end());
    }
    std::stable_partition(ordered.begin(), ordered.end(), [](const Project &project) { return project.pinned; });
    {
        const QSignalBlocker blocker(m_projects);
        m_projects->clear();
        QListWidgetItem *selected = nullptr;
        QListWidgetItem *linked = nullptr;
        for (const auto &project : ordered) {
            auto *item = workspace::addRow(m_projects, project.name, project.group, {}, project.id);
            item->setData(workspace::AvatarType, int(workspace::AvatarKind::Project));
            item->setData(workspace::AvatarUrl, project.avatarUrl);
            item->setData(workspace::ProjectAvatarColor, QColor(project.avatarColor));
            item->setData(workspace::ProjectAvatarVariant, project.avatarVariant);
            item->setData(workspace::ProjectAvatarType, project.avatarType);
            item->setData(workspace::Pinned, project.pinned);
            ui::setProjectRowUnreadCount(item, m_projectUnreadCounts.value(project.id), !m_projectUnreadCountsComplete);
            if (project.id == m_currentProjectId)
                selected = item;
            if (project.id == m_noticeProjectId)
                linked = item;
        }
        if (linked) {
            selected = linked;
            m_noticeProjectId.clear();
        }
        for (int i = 0; i < m_projects->count() && !m_pendingProjectId.isEmpty(); ++i) {
            auto *item = m_projects->item(i);
            if (item->data(workspace::Id).toString() == m_pendingProjectId) {
                selected = item;
                m_pendingProjectId.clear();
            }
        }
        if (m_projectNameQuery.isEmpty())
            selectAvailable(m_projects, selected);
        else if (selected)
            m_projects->setCurrentItem(selected);
        if (ordered.isEmpty()) {
            const auto noMatches = !m_projectsMoreAvailable ? QStringLiteral("Проектов по запросу нет")
                : QStringLiteral("В загруженных проектах совпадений нет");
            workspace::emptyRow(m_projects, m_projectNameQuery.isEmpty()
                ? QStringLiteral("Проектов пока нет") : noMatches);
        }
    }
    updateProjectsRailPagination();
    if (ordered.isEmpty() && m_projectNameQuery.isEmpty()) {
        if (!clearTask())
            return;
        closeProjectTaskChats();
        m_currentProjectId.clear();
        updateProjectTaskChatTab();
        m_projectTaskViewRequested = false;
        m_projectTitle->setText(QStringLiteral("Проекты"));
        m_projectAvatar->setTitle({});
        m_projectAvatarUrl.clear();
        m_board->clearBoard(QStringLiteral("Доски появятся здесь, когда у вас будут проекты"));
        m_projectTimeline->clear(QStringLiteral("Выберите проект"));
        updateWriteAvailability();
    } else {
        onProjectSelected();
    }
    updateAvatarRows();
    scheduleAvatars();
}

void MainWindow::showPinMenu(QListWidget *list, const QPoint &point, bool project)
{
    const auto *item = list->itemAt(point);
    if (!item)
        return;
    const auto id = item->data(workspace::Id).toString();
    if (id.isEmpty())
        return;
    const auto op = operationKey(project ? OperationKind::ProjectPin : OperationKind::ChatPin, id);
    const auto pinned = item->data(workspace::Pinned).toBool();
    auto *menu = new QMenu(this);
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    auto *action = menu->addAction(pinned ? QStringLiteral("Открепить") : QStringLiteral("Закрепить"));
    action->setEnabled(writable() && !m_pendingPins.contains(op));
    connect(action, &QAction::triggered, this, [this, id, pinned, project, op] {
        if (!writable() || m_pendingPins.contains(op))
            return;
        setStatus(QString());
        m_pendingPins.insert(op);
        if (project)
            m_backend->setProjectPinned(id, !pinned);
        else
            m_backend->setChatPinned(id, !pinned);
    });
    if (!project && !m_backend->isDemo()) {
        connect(menu->addAction(QStringLiteral("Медиа, файлы и ссылки")), &QAction::triggered,
                this, [this, id] { showChatAssets(id, this); });
    }
    if (!project && m_chatScope == 0 && id.startsWith(QLatin1String("chat")) && !m_backend->isDemo()) {
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
        requestAvatar(m_projectAvatarUrl);
        requestVisible(m_projects);
        if (m_projectTabs->currentIndex() == 1) {
            requestVisible(m_projectTaskChats);
        } else {
            for (const auto &url : m_board->visibleAvatarUrls())
                requestAvatar(url);
        }
    }
    if (m_card)
        requestVisible(m_card->messageList());
    if (m_personDialog)
        requestAvatar(m_personAvatarUrl);
}

void MainWindow::updateAvatarRows()
{
    for (auto *list : {m_chats, m_projects, m_projectTaskChats, m_chatMessages, m_card ? m_card->messageList() : nullptr}) {
        if (!list)
            continue;
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
    }
    if (const auto *photo = m_avatars.object(m_chatAvatarUrl))
        m_chatAvatar->setPhoto(*photo);
    if (const auto *photo = m_avatars.object(m_projectAvatarUrl))
        m_projectAvatar->setPhoto(*photo);
    if (m_personDialog) {
        if (const auto *photo = m_avatars.object(m_personAvatarUrl))
            m_personAvatar->setPhoto(*photo);
    }
    if (m_notifications->isVisible())
        m_notifications->viewport()->update();
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
    for (const int row : visibleNoticeRows()) {
        const auto *item = m_notifications->item(row);
        if (!item->data(workspace::NoticePresentation).value<workspace::NoticeRow>().expanded)
            continue;
        const auto links = item->data(workspace::MessageLinks).toStringList();
        for (const auto &href : links)
            loadMessageLinkPreview(href);
    }
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
    if (m_notifications->isVisible()) {
        const auto anchor = listViewportAnchor(m_notifications);
        const QSignalBlocker blocker(m_notifications->verticalScrollBar());
        restoreListViewport(m_notifications, anchor);
        m_notifications->viewport()->update();
    }
}

void MainWindow::runNotificationSearch()
{
    m_noticeSearchTimer->stop();
    if (m_lastSection != 2)
        return;
    const auto query = m_noticeSearch->text().trimmed();
    const bool queryChanged = m_noticeQuery != query;
    if (queryChanged) {
        m_noticeQuery = query;
        m_noticeExpandedId.clear();
        m_noticeSearchResults.clear();
        m_failedNoticeRead.clear();
    }
    m_noticeSearchPending = query.size() >= 3;
    showNotifications(true, !queryChanged);
    if (m_noticeSearchPending)
        m_backend->searchNotifications(query);
}

void MainWindow::resetNotificationAccount()
{
    m_noticeReadTimer->stop();
    m_noticeSearchTimer->stop();
    m_pendingNoticeRead.clear();
    m_failedNoticeRead.clear();
    m_readNoticesAfterPending = false;
    m_noticeHistory.clear();
    m_noticeHistoryRequested = false;
    m_noticeHistoryState = NoticeHistoryState::Ready;
    m_noticeHasOlder = false;
    m_noticeOlderLoading = false;
    m_noticeOlderFailed = false;
    m_noticeSearchResults.clear();
    m_noticeSearchPending = false;
    m_noticeSelectedId.clear();
    m_noticeExpandedId.clear();
    m_noticeQuery.clear();
    {
        const QSignalBlocker blocker(m_noticeSearch);
        m_noticeSearch->clear();
    }
    if (!m_noticeAction.isEmpty()) {
        m_noticeAction.clear();
        setStatus(QString());
    }
    if (m_lastSection == 2) {
        showNotifications(false, false);
    } else {
        const QSignalBlocker blocker(m_notifications);
        const QSignalBlocker scrollBlocker(m_notifications->verticalScrollBar());
        m_notifications->clear();
        updateNotificationActions();
    }
    updateNotificationRailBadge();
}

void MainWindow::showNotifications(bool markVisible, bool preserveScroll)
{
    if (m_lastSection != 2)
        return;
    const bool remoteSearch = m_noticeQuery.size() >= 3;
    auto notices = remoteSearch ? m_noticeSearchResults : m_noticeHistory;
    if (m_clientUpdates) {
        const auto &local = m_clientUpdates->notice();
        if (!local.id.isEmpty() && (local.title.contains(m_noticeQuery, Qt::CaseInsensitive)
                                    || local.text.contains(m_noticeQuery, Qt::CaseInsensitive)))
            notices.append(local);
    }
    notices.erase(std::remove_if(notices.begin(), notices.end(), [this, remoteSearch](const Notification &notice) {
        return (m_noticeUnreadOnly->isChecked() && notice.read && notice.id != m_noticeExpandedId) ||
            (!remoteSearch && !m_noticeQuery.isEmpty() &&
             !notice.title.contains(m_noticeQuery, Qt::CaseInsensitive) &&
             !notice.text.contains(m_noticeQuery, Qt::CaseInsensitive));
    }), notices.end());
    const bool ascending = m_noticeSort->currentIndex() == 1;
    std::stable_sort(notices.begin(), notices.end(), [ascending](const Notification &a, const Notification &b) {
        if (a.createdAt != b.createdAt)
            return ascending ? a.createdAt < b.createdAt : a.createdAt > b.createdAt;
        return ascending ? a.id.toLongLong() < b.id.toLongLong() : a.id.toLongLong() > b.id.toLongLong();
    });
    const auto anchor = preserveScroll ? listViewportAnchor(m_notifications) : ListViewportAnchor{};
    const QSignalBlocker blocker(m_notifications);
    const QSignalBlocker scrollBlocker(m_notifications->verticalScrollBar());
    if (notices.isEmpty()) {
        const bool loading = remoteSearch ? m_noticeSearchPending : m_noticeHistoryState == NoticeHistoryState::Loading;
        auto message = QStringLiteral("Уведомлений пока нет");
        if (loading)
            message = remoteSearch ? QStringLiteral("Ищем уведомления…") : QStringLiteral("Загружаем уведомления…");
        else if (!remoteSearch && m_noticeHistory.isEmpty() && m_noticeHistoryState == NoticeHistoryState::Failed)
            message = QStringLiteral("Не удалось загрузить уведомления");
        else if (!m_noticeQuery.isEmpty())
            message = QStringLiteral("Уведомлений по запросу нет");
        else if (m_noticeUnreadOnly->isChecked())
            message = QStringLiteral("Непрочитанных уведомлений нет");
        showEmpty(m_notifications, m_notificationsEmpty, message, loading);
        updateNotificationActions();
        return;
    }
    m_notifications->clear();
    QListWidgetItem *selected = nullptr;
    QDate previousDay;
    const auto yesterday = QDate::currentDate().addDays(-1);
    for (const auto &notice : notices) {
        const auto &content = notice.markup.isEmpty() ? notice.text : notice.markup;
        auto *item = workspace::addMarkupRow(m_notifications, notice.displayTitle, content,
                                             clockLabel(notice.createdAt), notice.id);
        item->setData(workspace::Count, !notice.read);
        item->setData(workspace::NoticeTargets, QVariant::fromValue(notice.targets));
        workspace::NoticeRow row;
        const auto day = notice.createdAt.toLocalTime().date();
        if (day.isValid() && day != previousDay)
            row.day = day == yesterday ? QStringLiteral("Вчера") : ui::messageDayLabel(day);
        previousDay = day;
        row.excerpt = notice.excerpt;
        const auto message = std::find_if(notice.targets.cbegin(), notice.targets.cend(), [](const auto &target) {
            return target.kind == NotificationTargetKind::Chat && !target.messageId.isEmpty();
        });
        const auto task = std::find_if(notice.targets.cbegin(), notice.targets.cend(), [](const auto &target) {
            return target.kind == NotificationTargetKind::Task;
        });
        if (!notice.targets.isEmpty()) {
            const auto primary = message != notice.targets.cend() ? message
                : task != notice.targets.cend() ? task : notice.targets.cbegin();
            row.primaryLabel = actionLabel(*primary, false);
            row.primaryTarget = int(std::distance(notice.targets.cbegin(), primary));
        }
        row.expanded = !m_noticeExpandedId.isEmpty() && notice.id == m_noticeExpandedId;
        presentNoticeRow(item, row);
        if (notice.id == m_noticeSelectedId)
            selected = item;
    }
    if (selected)
        m_notifications->setCurrentItem(selected);
    m_notificationsEmpty->hide();
    m_notifications->show();
    restoreListViewport(m_notifications, anchor);
    updateNotificationActions();
    scheduleAvatars();
    if (markVisible)
        scheduleVisibleNoticeRead();
}

QVector<int> MainWindow::visibleNoticeRows() const
{
    QVector<int> rows;
    if (m_lastSection != 2 || !m_notifications->isVisible())
        return rows;
    const auto visible = m_notifications->viewport()->visibleRegion().boundingRect();
    if (visible.isEmpty())
        return rows;
    const auto *delegate = static_cast<const workspace::RowPainter *>(m_notifications->itemDelegate());
    const auto first = m_notifications->indexAt(QPoint(visible.left() + 2, visible.top()));
    for (int row = first.isValid() ? first.row() : 0; row < m_notifications->count(); ++row) {
        const auto item = m_notifications->model()->index(row, 0);
        const auto bounds = m_notifications->visualRect(item);
        if (bounds.top() > visible.bottom())
            break;
        const auto card = delegate->receiptArea(item, bounds);
        if (!card.isEmpty() && card.intersected(visible).height() >= qMin(24, card.height()))
            rows.append(row);
    }
    return rows;
}

bool MainWindow::canReadVisibleNotices() const
{
    return m_lastSection == 2 && writable() && m_noticeAction.isEmpty() && m_sessionRecovery->isHidden()
        && !m_noticeSearchPending && !m_noticeSearchTimer->isActive()
        && !isMinimized() && QApplication::activeWindow() == this
        && QGuiApplication::applicationState() == Qt::ApplicationActive
        && !QApplication::activeModalWidget() && !QApplication::activePopupWidget();
}

void MainWindow::scheduleVisibleNoticeRead()
{
    if (!m_noticeReadTimer || !canReadVisibleNotices())
        return;
    if (!m_pendingNoticeRead.isEmpty())
        m_readNoticesAfterPending = true;
    else
        m_noticeReadTimer->start();
}

void MainWindow::readVisibleNotices()
{
    if (!canReadVisibleNotices() || !m_pendingNoticeRead.isEmpty())
        return;
    bool readClientUpdate = false;
    for (const int row : visibleNoticeRows()) {
        const auto *item = m_notifications->item(row);
        if (!item->data(workspace::Count).toBool())
            continue;
        auto id = item->data(workspace::Id).toString();
        if (m_clientUpdates && id == m_clientUpdates->notice().id) {
            readClientUpdate = true;
            continue;
        }
        if (!isLocalNotificationId(id))
            id = portalTaskIdentifier(id);
        if (!id.isEmpty() && !m_failedNoticeRead.contains(id) && !m_pendingNoticeRead.contains(id))
            m_pendingNoticeRead.append(id);
    }
    if (readClientUpdate)
        m_clientUpdates->markRead();
    if (!m_pendingNoticeRead.isEmpty())
        m_backend->markNotificationsRead(QStringList(m_pendingNoticeRead));
}

void MainWindow::finishVisibleNoticeRead(bool success)
{
    if (!success) {
        for (const auto &id : std::as_const(m_pendingNoticeRead))
            m_failedNoticeRead.insert(id);
    }
    m_pendingNoticeRead.clear();
    if (std::exchange(m_readNoticesAfterPending, false))
        scheduleVisibleNoticeRead();
}

void MainWindow::updateNotificationActions()
{
    scheduleOlderNotifications();
    const bool canWrite = writable() && m_noticeAction.isEmpty();
    const auto *row = m_notifications->currentItem();
    const bool validRow = row && !portalTaskIdentifier(row->data(workspace::Id).toString()).isEmpty();
    const bool localUnread = m_clientUpdates && !m_clientUpdates->notice().id.isEmpty() && !m_clientUpdates->notice().read;
    const bool localSelected = localUnread && row && row->data(workspace::Id).toString() == m_clientUpdates->notice().id;
    m_noticeRead->setEnabled(localSelected || (canWrite && validRow && row->data(workspace::Count).toBool()));
    m_noticeDelete->setEnabled(canWrite && validRow);
    const bool portalRows = !m_noticeHistory.isEmpty() || !m_noticeSearchResults.isEmpty();
    m_noticeReadAll->setEnabled(m_noticeAction.isEmpty() && (localUnread || (canWrite && portalRows)));
    const auto tip = m_readOnly ? QStringLiteral("Недоступно в режиме чтения") : QString();
    m_noticeRead->setToolTip(tip);
    m_noticeDelete->setToolTip(tip);
    m_noticeReadAll->setToolTip(tip);
}

void MainWindow::toggleNoticeDetails(QListWidgetItem *item)
{
    if (!item)
        return;
    const auto id = item->data(workspace::Id).toString();
    if (id.isEmpty())
        return;
    const auto anchor = listViewportAnchor(m_notifications);
    const auto previous = std::exchange(m_noticeExpandedId, {});
    auto row = item->data(workspace::NoticePresentation).value<workspace::NoticeRow>();
    row.expanded = !row.expanded;
    if (row.expanded)
        m_noticeExpandedId = id;
    if (auto *other = noticeItem(previous)) {
        if (m_noticeUnreadOnly->isChecked() && !other->data(workspace::Count).toBool()) {
            showNotifications();
            return;
        }
        if (other != item) {
            auto previousRow = other->data(workspace::NoticePresentation).value<workspace::NoticeRow>();
            previousRow.expanded = false;
            presentNoticeRow(other, previousRow);
        }
    }
    presentNoticeRow(item, row);
    restoreListViewport(m_notifications, anchor);
    scheduleAvatars();
}

QListWidgetItem *MainWindow::noticeItem(const QString &id) const
{
    if (!id.isEmpty()) {
        for (int row = 0; row < m_notifications->count(); ++row) {
            auto *item = m_notifications->item(row);
            if (item->data(workspace::Id).toString() == id)
                return item;
        }
    }
    return nullptr;
}

void MainWindow::showNoticeMenu(QListWidgetItem *item, const QPoint &position)
{
    if (!item)
        return;
    m_notifications->setCurrentItem(item);
    const auto id = item->data(workspace::Id).toString();
    auto *menu = new QMenu(m_notifications);
    menu->setObjectName(QStringLiteral("noticeMenu"));
    connect(m_backend, &Backend::accountScopeChanged, menu, [this, menu, scope = m_backend->settingsScope()] {
        if (m_backend->settingsScope() != scope)
            menu->close();
    });
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    connect(menu, &QMenu::aboutToHide, this, [this] {
        QTimer::singleShot(0, this, &MainWindow::scheduleVisibleNoticeRead);
    });
    const auto targets = item->data(workspace::NoticeTargets).value<QVector<NotificationTarget>>();
    const auto notice = item->data(workspace::NoticePresentation).value<workspace::NoticeRow>();
    const auto addTarget = [this, menu, id](const QString &label, const NotificationTarget &target) {
        connect(menu->addAction(label), &QAction::triggered, menu, [this, id, target] {
            if (m_lastSection == 2 && noticeItem(id))
                openPortalTarget(target);
        });
    };
    if (notice.primaryTarget >= 0 && notice.primaryTarget < targets.size())
        addTarget(notice.primaryLabel, targets[notice.primaryTarget]);
    for (int n = 0; n < targets.size(); ++n)
        if (n != notice.primaryTarget)
            addTarget(actionLabel(targets[n]), targets[n]);
    for (const auto &href : item->data(workspace::MessageLinks).toStringList()) {
        const auto url = messageLinkDestination(href);
        if (url.isEmpty())
            continue;
        const auto linked = portalLinkTarget(m_backend->portalOrigin(), url);
        if (linked && std::any_of(targets.cbegin(), targets.cend(), [&linked](const auto &target) {
            return target.kind == linked->kind && target.id == linked->id && target.messageId == linked->messageId;
        }))
            continue;
        auto *action = menu->addAction(QStringLiteral("Открыть ссылку · %1").arg(url.toDisplayString().left(100)));
        connect(action, &QAction::triggered, menu, [this, href, id] {
            if (m_lastSection == 2 && noticeItem(id))
                openMessageLink(href, m_notifications);
        });
    }
    if (!menu->isEmpty())
        menu->addSeparator();
    menu->addAction(m_noticeRead);
    menu->addAction(m_noticeDelete);
    menu->popup(m_notifications->viewport()->mapToGlobal(position));
}

void MainWindow::scheduleOlderNotifications()
{
    if (m_noticePageCheckQueued || m_lastSection != 2 || !m_noticeHasOlder || m_noticeOlderLoading || m_noticeOlderFailed)
        return;
    m_noticePageCheckQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_noticePageCheckQueued = false;
        if (m_lastSection != 2 || !isVisible() || isMinimized() || !m_noticeHasOlder || m_noticeOlderLoading || m_noticeOlderFailed
            || !m_online || !m_sessionRecovery->isHidden() || m_noticeHistoryState != NoticeHistoryState::Ready
            || !m_noticeQuery.isEmpty() || !m_noticeSearch->text().trimmed().isEmpty())
            return;
        if (!m_notifications->count() && m_noticeUnreadOnly->isChecked())
            return;
        const auto *scroll = m_notifications->verticalScrollBar();
        const auto olderEdge = m_noticeSort->currentIndex() == 1 ? scroll->minimum() : scroll->maximum();
        if (!m_notifications->count() || scroll->value() == olderEdge)
            m_backend->requestOlderNotifications();
    });
}

void MainWindow::openNotificationTarget(const QListWidgetItem *item, int action)
{
    if (m_lastSection != 2 || !item)
        return;
    const auto targets = item->data(workspace::NoticeTargets).value<QVector<NotificationTarget>>();
    if (action >= 0 && action < targets.size())
        openPortalTarget(targets[action]);
}

void MainWindow::openPortalTarget(NotificationTarget target, QWidget *source, const QUrl &messageUrl)
{
    if (target.id.isEmpty())
        return;
    m_pendingMessageProject = {};
    switch (target.kind) {
    case NotificationTargetKind::Chat:
        if (m_card && m_card->taskChatDialogId() == target.id) {
            m_card->activateWindow();
            if (!target.messageId.isEmpty())
                openPinnedTaskMessage(target.messageId);
        } else if (clearTask()) {
            openNotificationChat(target);
        }
        break;
    case NotificationTargetKind::Task:
        openTaskCardForProject(target.id, target.projectId);
        break;
    case NotificationTargetKind::Person:
        openPerson(target.id, source);
        break;
    case NotificationTargetKind::Project: {
        if (!messageUrl.isEmpty() && std::none_of(m_projectDirectory.cbegin(), m_projectDirectory.cend(),
                [&target](const Project &project) { return project.id == target.id; })) {
            if (m_projectDirectoryLoaded && (!m_projectsMoreAvailable || m_projectsPageFailed)) {
                QDesktopServices::openUrl(messageUrl);
            } else {
                m_pendingMessageProject = {target, messageUrl};
                if (!m_projectDirectoryLoaded)
                    m_backend->requestProjects();
                else
                    requestNextProjectsPage();
            }
            return;
        }
        if (!clearTask())
            return;
        const QSignalBlocker blocker(m_projectNameSearch);
        m_projectNameSearch->clear();
        m_projectNameQuery.clear();
        m_pendingProjectId.clear();
        m_noticeProjectId = target.id;
        if (m_lastSection == 1)
            refreshCurrentSection(false);
        else
            m_sections->setCurrentRow(1);
        break;
    }
    }
}

void MainWindow::openNotificationChat(const NotificationTarget &target)
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
    form->setOnMoreProjects([this] { requestNextProjectsPage(); });
    form->setOnStages([this, form](const QString &project) {
        if (project == m_currentProjectId && !m_stages.isEmpty())
            form->setStages(m_stages);
        else
            m_backend->requestProjectStages(project);
    });
    form->configure(projectId, canDetachProject);
    form->setProjectChoices(m_projectDirectory);
    form->setMoreProjectsAvailable(m_projectsMoreAvailable);
    m_backend->requestCurrentPerson([window = QPointer<MainWindow>(this), guard = QPointer<ui::TaskEditForm>(form)](Person person) {
        if (window && guard && window->m_taskForm == guard)
            guard->setCurrentPerson(std::move(person));
    });
    if (!m_projectDirectoryLoaded)
        m_backend->requestProjects();
}

void MainWindow::createTaskInProject()
{
    if (!writable() || m_currentProjectId.isEmpty() || m_taskForm || !m_pendingCreateTask.isEmpty() || !m_pendingCreateSubtask.isEmpty())
        return;
    auto *form = new ui::TaskEditForm(this);
    bindTaskForm(form, m_currentProjectId, false);
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

void MainWindow::openProjectTools()
{
    if (m_currentProjectId.isEmpty())
        return;
    if (m_projectTools) {
        m_projectTools->activateWindow();
        return;
    }
    const auto project = m_currentProjectId;
    auto *tools = new ui::ProjectToolsDialog(project, m_projectTitle->text(), this);
    m_projectTools = tools;
    tools->onMembers = [this, project] { m_backend->requestProjectMembers(project); };
    tools->onFiles = [this, project](const QString &folder, int offset) {
        m_backend->requestProjectFiles(project, folder, offset);
    };
    tools->onDownload = [this, tools](const MessageFile &file) { saveMessageFile(file, tools); };
    connect(tools, &QDialog::finished, this, [this, tools] {
        if (m_projectTools == tools)
            m_projectTools.clear();
    });
    tools->open();
    tools->reload();
}

void MainWindow::showProjectTimeline(bool show)
{
    if (m_ganttMode == show)
        return;
    m_ganttMode = show;
    m_projectTimeline->setVisible(show);
    m_board->setVisible(!show);
    m_stagesButton->setVisible(!show);
    m_filter->setToolTip(show ? QStringLiteral("Поиск по загруженным задачам диаграммы") : QString());
    if (show) {
        m_projectSearchTimer->stop();
        ui::setStatusLabel(m_projectTaskSearchStatus, {});
        m_projectTimeline->clear(m_currentProjectId.isEmpty() ? QStringLiteral("Выберите проект")
                                                             : QStringLiteral("Загружаем плановые даты…"),
                                 !m_currentProjectId.isEmpty());
        if (!m_currentProjectId.isEmpty())
            m_backend->requestProjectTimeline(m_currentProjectId);
    } else if (!m_currentProjectId.isEmpty()) {
        m_board->setFilter(m_filter->text());
        if (!m_filter->text().trimmed().isEmpty() && !m_projectSearchDisplayed) {
            m_board->setStages(m_stages);
            ui::setStatusLabel(m_projectTaskSearchStatus, QStringLiteral("Ищем задачи в проекте…"), true);
            m_projectSearchTimer->start();
        }
        requestCurrentProjectBoard();
    }
}

void MainWindow::requestCurrentProjectBoard()
{
    m_board->resetStagePageLoads(false);
    m_backend->requestProjectBoard(m_currentProjectId);
}

void MainWindow::createProject()
{
    if (!writable() || m_projectForm || m_pendingCreateProject)
        return;
    auto *form = new ui::NameDialog(this);
    form->setAttribute(Qt::WA_DeleteOnClose);
    m_projectForm = form;
    form->setOnCreate([this, form](const QString &name) {
        if (!writable() || m_pendingCreateProject)
            return;
        m_pendingCreateProject = true;
        form->setStatus(QStringLiteral("Создаём проект…"), true);
        updateWriteAvailability();
        m_backend->createProject(name);
    });
    connect(form, &QDialog::finished, this, [this] { m_projectForm = nullptr; });
    form->open();
}

void MainWindow::openStageManager()
{
    if (!writable() || m_currentProjectId.isEmpty() || m_stageManager || !m_pendingBoardEdit.isEmpty())
        return;
    auto *manager = new ui::StageManagerDialog(m_currentProjectId, this);
    manager->setAttribute(Qt::WA_DeleteOnClose);
    m_stageManager = manager;
    manager->setStages(m_stages);
    manager->setOnSave([this, manager](const QString &stage, const QString &title, const QString &after,
                                      const QString &color) {
        if (!writable() || !m_pendingBoardEdit.isEmpty())
            return;
        m_pendingBoardEdit = manager->projectId();
        manager->setStatus(QStringLiteral("Сохраняем этап…"), true);
        updateWriteAvailability();
        m_backend->saveStage(manager->projectId(), stage, title, after, color);
    });
    manager->setOnDelete([this, manager](const QString &stage) {
        if (!writable() || !m_pendingBoardEdit.isEmpty())
            return;
        const auto answer = QMessageBox::question(manager, QStringLiteral("Удалить этап"),
            QStringLiteral("Удалить пустой этап?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes || !writable())
            return;
        m_pendingBoardEdit = manager->projectId();
        manager->setStatus(QStringLiteral("Удаляем этап…"), true);
        updateWriteAvailability();
        m_backend->deleteStage(manager->projectId(), stage);
    });
    connect(manager, &QDialog::finished, this, [this] { m_stageManager = nullptr; });
    manager->open();
}

} // namespace bx
