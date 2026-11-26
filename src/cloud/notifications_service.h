#pragma once

#include "cloud/portal_http.h"
#include "core/models.h"

#include <QCache>
#include <QJsonArray>
#include <QJsonObject>
#include <QQueue>
#include <QUrlQuery>

namespace bx {

struct ChatTimeline;

class NotificationsHost : public virtual SessionStateHost {
public:
    virtual QUrl portalOrigin() const = 0;
    virtual QString currentUserId() const = 0;
    virtual void withCurrentUser(const QString &operation, std::function<void(qint64)> onUser) = 0;
    virtual void failShape(const QString &operation) = 0;
    virtual QString recentTitle(const QString &dialog, const QString &fallback) const = 0;
    virtual bool chatForeground() const = 0;
    virtual bool chatOnline() const = 0;
    virtual bool chatsSectionActive() const = 0;
    virtual bool popupWorkspaceVisible() const = 0;
    virtual QString popupPortalStatus() const = 0;
    virtual bool initialStatusPending() const = 0;
    virtual void popupIdentityResolved() = 0;
    virtual void rememberDialog(const QString &dialogId, const QJsonObject &payload, bool rememberCounter) = 0;
};

class NotificationsService final : public QObject {
    Q_OBJECT
public:
    NotificationsService(NotificationsHost &host, const ChatTimeline &timeline, PortalHttp &http, QObject *parent);
    QString historyRequestKey() const;
    void reset();
    void resetSettings();
    void clearHistory(bool task);
    void clearPendingPopups();
    bool reconnectSettingsNeeded() const;
    void resetReconnectSettings();
    void settleInitialPortalStatus();
    void handleOperationFailure(const QString &operation);
    void requestNotifications();
    void requestOlderNotifications();
    void searchNotifications(const QString &query);
    void markNotificationRead(const QString &id);
    void markNotificationsRead(const QStringList &ids);
    void markAllNotificationsRead();
    void deleteNotification(const QString &id);
    void requestNotificationSettings();
    void updateNotificationSetting(const QString &name, int value);
    void handleSettingsPull(const QJsonObject &params);
    void handleNotificationStatePull(const QString &command, const QJsonObject &params);
    void handleIncomingNoticePull(const QString &command, const QString &dialog, const QString &chatId, const QJsonObject &params, const QJsonObject &extra, const QString &mid);
    void reconcileHistoryPopups(const QString &dialog, qint64 chatId, bool task, const QJsonObject &history,
                                const QVector<Message> &messages);
    void loadChatPopupSettings();
    void deliverChatPopups();
signals:
    void notificationsLoaded(const QVector<Notification> &notifications);
    void notificationHistoryPageStateChanged(bool hasMore, bool loading);
    void notificationsFound(const QString &query, const QVector<Notification> &notifications);
    void notificationReadStateChanged(const QStringList &ids, bool read);
    void notificationRemoved(const QString &id);
    void notificationActionFinished(const QString &operation);
    void notificationSettingsLoaded(const QVariantMap &settings, const QString &updated = {});
    void notificationReceived(const Notification &notification);
    void failed(const QString &operation, const QString &reason);
private:
    struct ChatPopup {
        Notification entry;
        QString dialog;
        QString author;
        QJsonValue muteList;
        bool fromHistory = false;
    };
    struct ChatPopupHistory {
        QString dialog;
        std::optional<qint64> through;
    };
    struct NotificationCursor {
        qint64 id = 0;
        int type = 0;
    };
    struct NotificationMutation {
        quint64 revision;
        bool read;
        bool removed;
    };
    void requestNotificationSettings(const QString &operation, const QString &updated = {}, int expected = -1,
                                     std::optional<quint64> started = {});
    enum class ChatPopupSettingsSource { Push, InitialSnapshot, RefreshSnapshot };
    void notificationBatch(const QString &method, const QUrlQuery &parameters, const QString &operation,
                           std::function<void(QJsonValue)> onResult, const QString &requestKey = {});
    QVector<Notification> decodeNotifications(const QJsonArray &rows, const QJsonValue &users, quint64 requestedAt) const;
    static NotificationCursor notificationCursor(const QJsonArray &rows);
    void publishNotificationHistoryPageState();
    void publishNotificationPage(QVector<Notification> items);
    void updateNotificationReadState(const QStringList &ids, bool read, std::optional<quint64> started = {});
    void removeNotification(const QString &id);
    void prepareMessagePullNotice(Notification &entry, const QString &command, const QString &content, const QJsonObject &params);
    void preparePortalPullNotice(Notification &entry, const QString &content, const QJsonObject &notification, const QJsonObject &params);
    bool rememberChatPopup(const QString &dialog, const QString &noticeId);
    void queueChatPopup(ChatPopup popup);
    bool noticeHistoryRead(const QString &dialog, qint64 id) const;
    bool noticeConfirmedRead(const QString &dialog, qint64 id) const;
    bool watchingPopupDialog(const QString &dialog) const;
    QString notificationTaskId(const QString &dialog, const QJsonObject &eventChat) const;
    bool chatMuted(const ChatPopup &popup) const;
    std::optional<bool> chatPopupsEnabled() const;
    void applyChatPopupSettings(const QJsonObject &settings, ChatPopupSettingsSource source);
    static void trimNotificationHistory(QVector<Notification> &notices, int olderPages);
    static QString noticeAuthorId(const QJsonObject &notice, const QJsonObject &outer = {});
    static QString noticeAuthorName(const QString &id, const QJsonValue &users);
    NotificationsHost &m_host;
    const ChatTimeline &m_timeline;
    PortalHttp &m_portalHttp;
    QJsonObject m_chatPopupSettings;
    QVariantMap m_notificationSettings;
    QHash<QString, QPair<quint64, QJsonValue>> m_notificationSettingChanges;
    quint64 m_notificationSettingsRevision = 0;
    QQueue<ChatPopup> m_pendingChatPopups;
    QCache<QString, bool> m_seenChatPopups{256};
    ChatPopupHistory m_chatPopupHistory;
    ChatPopupHistory m_taskPopupHistory;
    qint64 m_nextChatPopupSettingsRequest = 0;
    quint64 m_chatPopupSettingsGeneration = 0;
    bool m_chatPopupSettingsKnown = false;

    QVector<Notification> m_notifications;
    QVector<Notification> m_notificationSearchRows;
    QString m_notificationSearchQuery;
    QHash<QString, NotificationMutation> m_notificationMutations;
    quint64 m_notificationRevision = 0;
    NotificationCursor m_notificationOlderCursor;
    int m_notificationOlderPages = 0;
    bool m_loadingOlderNotifications = false;
    QQueue<Notification> m_pendingInitialAlerts;
};

} // namespace bx
