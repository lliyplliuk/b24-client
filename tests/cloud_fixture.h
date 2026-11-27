#pragma once

#include "cloud/cloud_backend.h"

#include <QJsonArray>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QWebSocketServer>

namespace cloud_test {

inline const QString ajaxPath = QStringLiteral("/bitrix/services/main/ajax.php");
inline const QString pushPath = QStringLiteral("/rest/pull.config.get.json");
inline const QString batchPath = QStringLiteral("/rest/batch.json");
inline const QByteArray fixtureCookie = "BITRIX_TEST=synthetic";
inline const QByteArray fixtureCsrf = "fixture-token";

QByteArray invalidCsrf(const QByteArray &token);
QByteArray ajaxResult(const QByteArray &data);

struct Request {
    QByteArray method;
    QByteArray body;
    QUrl url;
    QUrlQuery form;
    QHash<QByteArray, QByteArray> headers;

    QString action() const { return QUrlQuery(url).queryItemValue(QStringLiteral("action")); }
    QString field(const QString &key) const
    {
        for (const auto &item : form.queryItems(QUrl::FullyDecoded)) {
            if (item.first == key)
                return item.second;
        }
        return {};
    }
};

class CloudFixture final {
public:
    CloudFixture();
    bool start();
    bx::Session session() const;
    QList<Request> requestsFor(const QString &key) const;
    void pushIm(const QString &mid, const QString &command, const QJsonObject &params, int age = 0,
                const QString &module = QStringLiteral("im"));
    void pushMessage(const QString &mid, const QString &id, int chatId = 91, int author = 22,
                     bool important = false, int age = 0, bool messageMute = false);
    bool waitForPullDelivery();
    void closePull();
    void releaseHeldOnlineResponse();
    void releaseHeldChatResponse();
    void releaseHeldReadResponse(const QByteArray &extraHeaders = {});
    void releaseHeldBoardResponse();
    void releaseHeldDownload();
    void releaseHeldIdentity();
    void releaseHeldNotificationReply(bool fail = false);

    QList<Request> requests;
    QList<QNetworkRequest> socketRequests;
    bool redirectProjects = false;
    QString redirectLocation;
    bool holdNextChatResponse = false;
    bool closeChatSnapshotConnection = false;
    QByteArray chatResponseOverride;
    QByteArray chatResponseStatus;
    QByteArray chatResponseHeaders;
    QByteArray offeredCsrf;
    QByteArray acceptedCsrf;
    QByteArray challengeCookie;
    QByteArray nextCsrf;
    bool failReplacementProbe = false;
    QString holdNextReadResponse;
    bool holdNextOnlineResponse = false;
    bool holdNextIdentityResponse = false;
    bool rejectDeadline = false;
    bool rejectTaskSearch = false;
    bool malformedTaskSearch = false;
    bool extendedStageFixture = false;
    bool boardPagingFixture = false;
    QByteArray kanbanMoveResult = R"({"id":60})";
    bool holdNextBoardPage = false;
    mutable bool badNextBoardPage = false;
    bool omitRecentUser = false;
    bool unreadHistoryFixture = false;
    bool serverSoundMuted = false;
    bool portalStatusFixture = false;
    mutable QString portalStatus = QStringLiteral("online");
    mutable int serverGeneralVolume = 100;
    int serverMessageVolume = 100;
    mutable bool serverSiteDelivery = true;
    bool omitDesktopDelivery = false;
    QString serverNotificationScheme;
    bool ignoreSoundUpdate = false;
    QString holdNotificationReply;
    QJsonArray noticeFixture;
    QJsonObject noticeUsersFixture;
    QHash<QString, QJsonArray> olderNoticeFixtures;
    bool rejectNoticeBatch = false;
    bool richRecentFixture = false;
    bool dismissedChatsFixture = false;
    bool liveRecentFixture = false;
    bool chatBadgesFixture = false;
    QByteArray ordinaryRecentBadgeFixture;
    QByteArray recentHistoryFixture;
    QByteArray taskRecentBadgeFixture;
    QString taskChatTitle = QStringLiteral("Проверить маршрут");
    QByteArray messengerPage;
    bool approveVisibleRead = false;
    bool approveAnchorRead = true;
    QByteArray visibleReadResult = R"({"chatId":91,"lastId":804,"counter":0})";
    bool richMessagesFixture = false;
    bool replyPreviewFixture = false;
    bool chatActionsFixture = false;
    QByteArray taskShareResult;
    mutable bool taskSharedInSource = false;
    bool refuseChatAction = false;
    bool completeReactionCatalog = false;
    mutable QString addedReaction;
    bool omitReplySource = false;
    bool mismatchDownload = false;
    bool holdNextDownload = false;
    bool denyIdentity = false;
    bool denyCanonicalSession = false;
    bool holdNextIdentity = false;
    int currentUserId = 11;
    QString downloadRedirect;
    mutable bool pinnedProject = false;
    bool projectGridFixture = false;
    QString projectGridNavigation = QStringLiteral("classic");
    bool projectFallbackFixture = false;
    bool legacyProjectFallbackFixture = false;
    bool editableTaskFixture = false;
    QJsonObject actionTaskLegacy;
    QJsonArray taskAttachmentRows;
    QByteArray taskLegacyResponseOverride;
    QByteArray taskCurrentResponseOverride;
    QJsonObject actionTaskCurrent;
    QJsonObject taskRatingReply;
    QJsonObject taskDeletionReply;
    QHash<QString, QByteArray> projectTaskChatPages;
    QJsonArray taskChatSearchRows;
    QJsonObject projectUnreadDialogReplies;
    QJsonArray projectUnreadTaskReplies;
    mutable QList<qint64> taskAuditorIds{13};
    QByteArray legacyTaskAuditors = R"({"33":{"id":33,"name":"Наблюдатель","icon":"/upload/resize_cache/task-auditor-legacy.png"}})";
    bool presencePinsFixture = false;

private:
    void respond(QTcpSocket *socket, QByteArray &buffer);

    QByteArray reply(const Request &request) const;

    QTcpServer http;
    QWebSocketServer websocket;
    QList<QWebSocket *> clients;
    QPointer<QTcpSocket> heldChatSocket;
    QPointer<QTcpSocket> heldBoardSocket;
    QByteArray heldBoardResponse;
    QPointer<QTcpSocket> heldReadSocket;
    QByteArray heldReadResponse;
    QByteArray heldChatResponse;
    QPointer<QTcpSocket> heldOnlineSocket;
    QByteArray heldOnlineResponse;
    QPointer<QTcpSocket> heldDownloadSocket;
    QByteArray heldDownloadResponse;
    QPointer<QTcpSocket> heldIdentitySocket;
    QByteArray heldIdentityResponse;
    QPointer<QTcpSocket> heldNotificationSocket;
    QByteArray heldNotificationResponse;
};

class CloudTestCase : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

private:
    QTemporaryDir m_settingsPath;
};

} // namespace cloud_test

class CloudTestAccess {
public:
    static bx::PortalHttp &http(bx::CloudBackend &backend) { return backend.m_portalHttp; }
    static QNetworkReply *pendingReply(bx::CloudBackend &backend, const QString &operation)
    {
        return http(backend).pendingRequests().key(operation);
    }
    static QPointer<QNetworkReply> pendingReplyForRoute(bx::CloudBackend &backend, const QString &route)
    {
        QPointer<QNetworkReply> match;
        const auto pending = http(backend).pendingRequests();
        for (auto *reply : pending.keys()) {
            const auto url = reply->url();
            if (url.path() != route && QUrlQuery(url).queryItemValue(QStringLiteral("action")) != route)
                continue;
            if (match)
                return {}; // The route must identify exactly one outstanding request.
            match = reply;
        }
        return match;
    }
    static void resetAccount(bx::CloudBackend &backend) { backend.resetAccountScopedState(); }
    static quint64 sessionRevision(const bx::CloudBackend &backend) { return backend.m_sessionRevision; }
    static void expireChatBadgeCooldown(bx::CloudBackend &backend) { backend.m_chatBadgeRefresh.invalidate(); }
    static void refreshNow(bx::CloudBackend &backend)
    {
        backend.m_refresh.stop();
        backend.refreshDirtyViews();
    }
};
