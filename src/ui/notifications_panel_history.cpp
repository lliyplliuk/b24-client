#include "ui/notifications_panel.h"

#include "cloud/cloud_notification_links.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <algorithm>

namespace bx {
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
} // namespace

void NotificationsPanel::connectNotificationHistory()
{
    connect(m_backend, &Backend::accountScopeChanged, this,
            [this, previousScope = m_backend->settingsScope()]() mutable {
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
