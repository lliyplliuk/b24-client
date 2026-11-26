#pragma once

#include "core/backend.h"
#include "core/operations.h"

#include <QPointer>
#include <QSet>
#include <QWidget>
#include <functional>

class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTimer;

namespace bx {

class ClientUpdateFeed final : public QObject {
    Q_OBJECT
public:
    void publish(const QString &tag, const QUrl &downloadUrl);
    const Notification &notice() const { return m_notice; }
    void markRead();

signals:
    void changed();

private:
    Notification m_notice;
};

class NotificationsPanel final : public QWidget {
    Q_OBJECT
public:
    struct Workspace {
        std::function<bool()> online;
        std::function<bool()> sessionReady;
        std::function<QUrl(const QString &)> linkDestination;
        std::function<void(QListWidget *)> bindFiles;
    };

    NotificationsPanel(Backend *backend, bool readOnly, Workspace workspace, QWidget *parent);
    void setClientUpdateFeed(ClientUpdateFeed *feed);
    void setSectionActive(bool active);
    void refresh(bool reload);
    void ensureHistory(bool retryFailed = false);
    void connectionRestored();
    void clearReadAndPageFailures();
    void scrollToTop();
    void refreshPreviewRows();
    void repaintRows();
    QStringList visiblePreviewLinks() const;
    bool handleFailure(const QString &operation, OperationKind kind, const QStringList &ids, const QString &reason);
    void scheduleOlderNotifications();
    void scheduleVisibleNoticeRead();
    void updateNotificationActions();

signals:
    void refreshRequested();
    void statusChanged(const QString &text, bool loading = false);
    void unreadCountChanged(qint64 unread);
    void previewsRequested();
    void targetRequested(const NotificationTarget &target);
    void linkRequested(const QString &href, QWidget *source);

private:
    bool writable() const { return !m_readOnly && m_workspace.online(); }
    void requestNotificationHistory();
    void updateNotificationRailBadge();
    void openNotificationTarget(const QListWidgetItem *item, int target);
    QListWidgetItem *noticeItem(const QString &id) const;
    void toggleNoticeDetails(QListWidgetItem *item);
    void showNoticeMenu(QListWidgetItem *item, const QPoint &position);
    void runNotificationSearch();
    void resetNotificationAccount();
    void showNotifications(bool markVisible = true, bool preserveScroll = true);
    QVector<int> visibleNoticeRows() const;
    void readVisibleNotices();
    void finishVisibleNoticeRead(bool success);
    bool canReadVisibleNotices() const;

    Backend *m_backend;
    bool m_readOnly;
    Workspace m_workspace;
    bool m_active = false;
    QListWidget *m_notifications = nullptr;
    QLabel *m_notificationsEmpty = nullptr;
    QPointer<ClientUpdateFeed> m_clientUpdates;
    QVector<Notification> m_noticeHistory;
    QVector<Notification> m_noticeSearchResults;
    QString m_noticeSelectedId;
    QString m_noticeExpandedId;
    QString m_noticeQuery;
    QString m_noticeAction;
    QTimer *m_noticeSearchTimer = nullptr;
    QTimer *m_noticeReadTimer = nullptr;
    QStringList m_pendingNoticeRead;
    QSet<QString> m_failedNoticeRead;
    bool m_readNoticesAfterPending = false;
    QLineEdit *m_noticeSearch = nullptr;
    QCheckBox *m_noticeUnreadOnly = nullptr;
    QComboBox *m_noticeSort = nullptr;
    QAction *m_noticeRead = nullptr;
    QPushButton *m_noticeReadAll = nullptr;
    QAction *m_noticeDelete = nullptr;
    enum class NoticeHistoryState { Ready, Loading, Failed };
    NoticeHistoryState m_noticeHistoryState = NoticeHistoryState::Ready;
    bool m_noticeHistoryRequested = false;
    bool m_noticeHasOlder = false;
    bool m_noticeOlderLoading = false;
    bool m_noticeOlderFailed = false;
    bool m_noticePageCheckQueued = false;
    bool m_noticeSearchPending = false;
};

} // namespace bx
