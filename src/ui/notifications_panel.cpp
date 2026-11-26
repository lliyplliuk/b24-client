#include "ui/notifications_panel.h"

#include "ui/main_window.h"
#include "ui/portal_dnd_control.h"
#include "ui/task_dialogs.h"
#include "ui/workspace_presentation.h"
#include "cloud/cloud_notification_links.h"
#include "core/notification_targets.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <utility>

namespace bx {
using ui::ListViewportAnchor;
using ui::clockLabel;
using ui::emptyState;
using ui::listViewportAnchor;
using ui::restoreListViewport;
using ui::showEmpty;
using workspace::label;

namespace {
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
} // namespace

NotificationsPanel::NotificationsPanel(Backend *backend, bool readOnly, Workspace workspace, QWidget *parent)
    : QWidget(parent), m_backend(backend), m_readOnly(readOnly), m_workspace(std::move(workspace))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 19, 24, 15);
    layout->setSpacing(12);
    auto *header = new QHBoxLayout;
    header->addWidget(label(QStringLiteral("События"), "pageTitle", this), 1);
    m_noticeReadAll = ui::button(QStringLiteral("Отметить все прочитанными"),
                                  QStringLiteral("Прочитать все уведомления"), this);
    header->addWidget(m_noticeReadAll, 0, Qt::AlignTop);
    auto *refresh = new QPushButton(QStringLiteral("Обновить"), this);
    refresh->setToolTip(QStringLiteral("Обновить уведомления (F5)"));
    connect(refresh, &QPushButton::clicked, this, &NotificationsPanel::refreshRequested);
    header->addWidget(refresh, 0, Qt::AlignTop);
    auto *settings = ui::button(QStringLiteral("Настройки"), QStringLiteral("Настройки уведомлений портала"), this);
    settings->setVisible(!m_backend->isDemo());
    header->addWidget(settings, 0, Qt::AlignTop);
    connect(settings, &QPushButton::clicked, this, [this] {
        auto *dialog = new QDialog(this);
        dialog->setObjectName(QStringLiteral("notificationSettingsDialog"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(dialog, &QDialog::finished, this, [this] {
            QTimer::singleShot(0, this, &NotificationsPanel::scheduleVisibleNoticeRead);
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
    m_noticeSearch = new QLineEdit(this);
    ui::editor(m_noticeSearch);
    m_noticeSearch->setAccessibleName(QStringLiteral("Поиск уведомлений"));
    m_noticeSearch->setPlaceholderText(QStringLiteral("Поиск уведомлений"));
    m_noticeSearch->setClearButtonEnabled(true);
    m_noticeSearch->setMaxLength(200);
    filters->addWidget(m_noticeSearch, 1);
    m_noticeUnreadOnly = new QCheckBox(QStringLiteral("Только непрочитанные"), this);
    m_noticeUnreadOnly->setAccessibleName(QStringLiteral("Только непрочитанные уведомления"));
    filters->addWidget(m_noticeUnreadOnly);
    m_noticeSort = new QComboBox(this);
    ui::editor(m_noticeSort);
    m_noticeSort->setAccessibleName(QStringLiteral("Порядок уведомлений"));
    m_noticeSort->addItems({QStringLiteral("Сначала новые"), QStringLiteral("Сначала старые")});
    filters->addWidget(m_noticeSort);
    layout->addLayout(filters);

    m_noticeRead = new QAction(QStringLiteral("Отметить прочитанным"), this);
    m_noticeRead->setObjectName(QStringLiteral("readNotice"));
    m_noticeDelete = new QAction(QStringLiteral("Удалить уведомление"), this);
    m_noticeDelete->setObjectName(QStringLiteral("deleteNotice"));
    m_noticeSearchTimer = new QTimer(this);
    m_noticeSearchTimer->setSingleShot(true);
    m_noticeSearchTimer->setInterval(280);
    connect(m_noticeSearchTimer, &QTimer::timeout, this, &NotificationsPanel::runNotificationSearch);
    connect(m_noticeSearch, &QLineEdit::textChanged, m_noticeSearchTimer, qOverload<>(&QTimer::start));
    connect(m_noticeSearch, &QLineEdit::returnPressed, this, &NotificationsPanel::runNotificationSearch);
    connect(m_noticeUnreadOnly, &QCheckBox::toggled, this, [this] {
        m_noticeExpandedId.clear();
        showNotifications(true, false);
    });
    connect(m_noticeSort, &QComboBox::currentIndexChanged, this, [this] { showNotifications(true, false); });

    m_notifications = new QListWidget(this);
    m_notifications->setAccessibleName(QStringLiteral("Уведомления"));
    m_noticeReadTimer = new QTimer(this);
    m_noticeReadTimer->setObjectName(QStringLiteral("noticeReadTimer"));
    m_noticeReadTimer->setSingleShot(true);
    m_noticeReadTimer->setInterval(160);
    connect(m_noticeReadTimer, &QTimer::timeout, this, &NotificationsPanel::readVisibleNotices);
    connect(m_notifications->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        emit previewsRequested();
        scheduleVisibleNoticeRead();
        scheduleOlderNotifications();
    });
    connect(m_notifications->verticalScrollBar(), &QScrollBar::rangeChanged,
            this, &NotificationsPanel::scheduleOlderNotifications);
    connect(m_notifications, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (item)
            m_noticeSelectedId = item->data(workspace::Id).toString();
        updateNotificationActions();
    });
    const auto beginAction = [this](const QString &action) {
        m_noticeAction = action;
        emit statusChanged(QStringLiteral("Обновляем уведомления…"), true);
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
    m_workspace.bindFiles(m_notifications);
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
    m_notificationsEmpty = emptyState(QStringLiteral("Уведомлений пока нет"), this);
    m_notificationsEmpty->setAccessibleName(QStringLiteral("Состояние уведомлений"));
    layout->addWidget(m_notificationsEmpty, 1);
    m_notifications->hide();

    connect(qApp, &QGuiApplication::focusWindowChanged, this, [this] { scheduleVisibleNoticeRead(); });
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive)
            scheduleVisibleNoticeRead();
    });
    connect(m_backend, &Backend::accountScopeChanged, this,
            [this, previousScope = backend->settingsScope()]() mutable {
        const auto scope = m_backend->settingsScope();
        if (scope == previousScope)
            return;
        if (!previousScope.isEmpty())
            resetNotificationAccount();
        previousScope = scope;
        if (!scope.isEmpty() && !m_noticeHistoryRequested && m_workspace.sessionReady()) {
            QTimer::singleShot(0, this, [this, scope] {
                if (m_backend->settingsScope() == scope)
                    ensureHistory();
            });
        }
    });
    connect(m_backend, &Backend::notificationsLoaded, this, [this](const QVector<Notification> &notifications) {
        auto listed = listedNotifications(notifications);
        bool newUnread = false;
        if (m_active) {
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
        if (m_active)
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
        if (m_active)
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
        if (m_active)
            showNotifications(false);
    });
    connect(m_backend, &Backend::notificationActionFinished, this, [this](const QString &action) {
        if (parseOperation(action).kind == OperationKind::ReadNotifications)
            finishVisibleNoticeRead(true);
        if (m_noticeAction == action) {
            m_noticeAction.clear();
            emit statusChanged(QString());
            updateNotificationActions();
        }
    });
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

void NotificationsPanel::setClientUpdateFeed(ClientUpdateFeed *feed)
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

void NotificationsPanel::updateNotificationRailBadge()
{
    QSet<QString> unread;
    for (const auto &notice : m_noticeHistory)
        if (!notice.read && !notice.id.isEmpty())
            unread.insert(notice.id);
    if (m_clientUpdates && !m_clientUpdates->notice().read && !m_clientUpdates->notice().id.isEmpty())
        unread.insert(m_clientUpdates->notice().id);
    emit unreadCountChanged(unread.size());
}

void NotificationsPanel::requestNotificationHistory()
{
    if (m_noticeHistoryState == NoticeHistoryState::Loading)
        return;
    m_noticeHistoryRequested = true;
    m_noticeHistoryState = NoticeHistoryState::Loading;
    if (m_active)
        showNotifications();
    m_backend->requestNotifications();
}

void NotificationsPanel::runNotificationSearch()
{
    m_noticeSearchTimer->stop();
    if (!m_active)
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

void NotificationsPanel::resetNotificationAccount()
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
        emit statusChanged(QString());
    }
    if (m_active) {
        showNotifications(false, false);
    } else {
        const QSignalBlocker blocker(m_notifications);
        const QSignalBlocker scrollBlocker(m_notifications->verticalScrollBar());
        m_notifications->clear();
        updateNotificationActions();
    }
    updateNotificationRailBadge();
}

void NotificationsPanel::showNotifications(bool markVisible, bool preserveScroll)
{
    if (!m_active)
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
    emit previewsRequested();
    if (markVisible)
        scheduleVisibleNoticeRead();
}

QVector<int> NotificationsPanel::visibleNoticeRows() const
{
    QVector<int> rows;
    if (!m_active || !m_notifications->isVisible())
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

bool NotificationsPanel::canReadVisibleNotices() const
{
    return m_active && writable() && m_noticeAction.isEmpty() && m_workspace.sessionReady()
        && !m_noticeSearchPending && !m_noticeSearchTimer->isActive()
        && !window()->isMinimized() && QApplication::activeWindow() == window()
        && QGuiApplication::applicationState() == Qt::ApplicationActive
        && !QApplication::activeModalWidget() && !QApplication::activePopupWidget();
}

void NotificationsPanel::scheduleVisibleNoticeRead()
{
    if (!m_noticeReadTimer || !canReadVisibleNotices())
        return;
    if (!m_pendingNoticeRead.isEmpty())
        m_readNoticesAfterPending = true;
    else
        m_noticeReadTimer->start();
}

void NotificationsPanel::readVisibleNotices()
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

void NotificationsPanel::finishVisibleNoticeRead(bool success)
{
    if (!success) {
        for (const auto &id : std::as_const(m_pendingNoticeRead))
            m_failedNoticeRead.insert(id);
    }
    m_pendingNoticeRead.clear();
    if (std::exchange(m_readNoticesAfterPending, false))
        scheduleVisibleNoticeRead();
}

void NotificationsPanel::updateNotificationActions()
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

void NotificationsPanel::toggleNoticeDetails(QListWidgetItem *item)
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
    emit previewsRequested();
}

QListWidgetItem *NotificationsPanel::noticeItem(const QString &id) const
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

void NotificationsPanel::showNoticeMenu(QListWidgetItem *item, const QPoint &position)
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
        QTimer::singleShot(0, this, &NotificationsPanel::scheduleVisibleNoticeRead);
    });
    const auto targets = item->data(workspace::NoticeTargets).value<QVector<NotificationTarget>>();
    const auto notice = item->data(workspace::NoticePresentation).value<workspace::NoticeRow>();
    const auto addTarget = [this, menu, id](const QString &label, const NotificationTarget &target) {
        connect(menu->addAction(label), &QAction::triggered, menu, [this, id, target] {
            if (m_active && noticeItem(id))
                emit targetRequested(target);
        });
    };
    if (notice.primaryTarget >= 0 && notice.primaryTarget < targets.size())
        addTarget(notice.primaryLabel, targets[notice.primaryTarget]);
    for (int n = 0; n < targets.size(); ++n)
        if (n != notice.primaryTarget)
            addTarget(actionLabel(targets[n]), targets[n]);
    for (const auto &href : item->data(workspace::MessageLinks).toStringList()) {
        const auto url = m_workspace.linkDestination(href);
        if (url.isEmpty())
            continue;
        const auto linked = portalLinkTarget(m_backend->portalOrigin(), url);
        if (linked && std::any_of(targets.cbegin(), targets.cend(), [&linked](const auto &target) {
            return target.kind == linked->kind && target.id == linked->id && target.messageId == linked->messageId;
        }))
            continue;
        auto *action = menu->addAction(QStringLiteral("Открыть ссылку · %1").arg(url.toDisplayString().left(100)));
        connect(action, &QAction::triggered, menu, [this, href, id] {
            if (m_active && noticeItem(id))
                emit linkRequested(href, m_notifications);
        });
    }
    if (!menu->isEmpty())
        menu->addSeparator();
    menu->addAction(m_noticeRead);
    menu->addAction(m_noticeDelete);
    menu->popup(m_notifications->viewport()->mapToGlobal(position));
}

void NotificationsPanel::scheduleOlderNotifications()
{
    if (m_noticePageCheckQueued || !m_active || !m_noticeHasOlder || m_noticeOlderLoading || m_noticeOlderFailed)
        return;
    m_noticePageCheckQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_noticePageCheckQueued = false;
        if (!m_active || !window()->isVisible() || window()->isMinimized() || !m_noticeHasOlder || m_noticeOlderLoading || m_noticeOlderFailed
            || !m_workspace.online() || !m_workspace.sessionReady() || m_noticeHistoryState != NoticeHistoryState::Ready
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

void NotificationsPanel::openNotificationTarget(const QListWidgetItem *item, int action)
{
    if (!m_active || !item)
        return;
    const auto targets = item->data(workspace::NoticeTargets).value<QVector<NotificationTarget>>();
    if (action >= 0 && action < targets.size())
        emit targetRequested(targets[action]);
}


void NotificationsPanel::setSectionActive(bool active)
{
    if (active)
        clearReadAndPageFailures();
    else if (m_active) {
        m_noticeReadTimer->stop();
        m_readNoticesAfterPending = false;
        m_noticeExpandedId.clear();
    }
    m_active = active;
}

void NotificationsPanel::clearReadAndPageFailures()
{
    m_failedNoticeRead.clear();
    m_noticeOlderFailed = false;
}

void NotificationsPanel::refresh(bool reload)
{
    if (!m_noticeHistoryRequested || reload || m_noticeHistoryState == NoticeHistoryState::Failed)
        requestNotificationHistory();
    else
        showNotifications();
    if (m_noticeSearch->text().trimmed() != m_noticeQuery || m_noticeQuery.size() >= 3)
        runNotificationSearch();
}

void NotificationsPanel::ensureHistory(bool retryFailed)
{
    if (m_workspace.sessionReady() && (!m_noticeHistoryRequested || (retryFailed && m_noticeHistoryState == NoticeHistoryState::Failed)))
        requestNotificationHistory();
}

void NotificationsPanel::connectionRestored()
{
    if (!m_active && m_noticeHistoryState == NoticeHistoryState::Failed && m_workspace.sessionReady())
        requestNotificationHistory();
    clearReadAndPageFailures();
    scheduleVisibleNoticeRead();
    scheduleOlderNotifications();
}

void NotificationsPanel::scrollToTop()
{
    m_notifications->scrollToTop();
}

void NotificationsPanel::repaintRows()
{
    if (m_notifications->isVisible())
        m_notifications->viewport()->update();
}

void NotificationsPanel::refreshPreviewRows()
{
    if (m_notifications->isVisible()) {
        const auto anchor = listViewportAnchor(m_notifications);
        const QSignalBlocker blocker(m_notifications->verticalScrollBar());
        restoreListViewport(m_notifications, anchor);
        m_notifications->viewport()->update();
    }
}

QStringList NotificationsPanel::visiblePreviewLinks() const
{
    QStringList result;
    for (const int row : visibleNoticeRows()) {
        const auto *item = m_notifications->item(row);
        if (!item->data(workspace::NoticePresentation).value<workspace::NoticeRow>().expanded)
            continue;
        const auto links = item->data(workspace::MessageLinks).toStringList();
        result.append(links);
    }
    return result;
}

bool NotificationsPanel::handleFailure(const QString &operation, OperationKind kind, const QStringList &ids, const QString &reason)
{
    if (!m_noticeAction.isEmpty() && operation == m_noticeAction) {
        m_noticeAction.clear();
        updateNotificationActions();
        emit statusChanged(reason);
        return true;
    }
    switch (kind) {
    case OperationKind::NotificationSettings:
    case OperationKind::NotificationSettingsUpdate: {
        const auto *settings = findChild<QDialog *>(QStringLiteral("notificationSettingsDialog"));
        return settings && settings->isVisible();
    }
    case OperationKind::ReadNotifications:
        finishVisibleNoticeRead(false);
        return true;
    case OperationKind::Notifications:
        m_noticeHistoryState = NoticeHistoryState::Failed;
        if (m_active) {
            showNotifications();
            emit statusChanged(reason);
        }
        return true;
    case OperationKind::NotificationsOlder:
        m_noticeOlderFailed = true;
        if (m_active && m_noticeQuery.isEmpty())
            emit statusChanged(reason);
        return true;
    case OperationKind::SearchNotifications:
        if (ids.value(0) == m_noticeQuery &&
            m_noticeQuery == m_noticeSearch->text().trimmed()) {
            m_noticeSearchPending = false;
            showNotifications();
            emit statusChanged(reason);
        }
        return true;
    default:
        return false;
    }
}

} // namespace bx
