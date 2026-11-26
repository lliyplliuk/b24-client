#include "cloud/cloud_backend.h"
#include "core/tunables.h"
#include "signin/session_restore.h"
#include "ui/reaction_catalog.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QPointer>
#include <QSettings>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>
#include <QUuid>
#include <QWebSocketServer>
#include <utility>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

using namespace bx;

namespace {

const QString ajaxPath = QStringLiteral("/bitrix/services/main/ajax.php");
const QString pushPath = QStringLiteral("/rest/pull.config.get.json");
const QString batchPath = QStringLiteral("/rest/batch.json");
const QByteArray fixtureCookie = "BITRIX_TEST=synthetic";
const QByteArray fixtureCsrf = "fixture-token";

QByteArray invalidCsrf(const QByteArray &token)
{
    return R"({"status":"error","errors":[{"code":"invalid_csrf","customData":{"csrf":")"
        + token + R"("}}]})";
}

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

QByteArray ajaxResult(const QByteArray &data)
{
    return "{\"status\":\"success\",\"data\":" + data + ",\"errors\":[]}";
}

class CloudFixture final {
public:
    CloudFixture()
        : websocket(QStringLiteral("synthetic Bitrix24"), QWebSocketServer::NonSecureMode)
    {
        QObject::connect(&http, &QTcpServer::newConnection, &http, [this] {
            while (auto *socket = http.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                                 [this, socket, buffer = QByteArray()]() mutable {
                    buffer += socket->readAll();
                    respond(socket, buffer);
                });
            }
        });
        QObject::connect(&websocket, &QWebSocketServer::newConnection, &websocket, [this] {
            while (auto *socket = websocket.nextPendingConnection()) {
                socket->setParent(&websocket);
                clients.append(socket);
                socketRequests.append(socket->request());
            }
        });
    }

    bool start()
    {
        return http.listen(QHostAddress::LocalHost) && websocket.listen(QHostAddress::LocalHost);
    }

    Session session() const
    {
        return {QUrl(QStringLiteral("http://127.0.0.1:%1").arg(http.serverPort())), fixtureCookie, fixtureCsrf};
    }

    QList<Request> requestsFor(const QString &key) const
    {
        QList<Request> found;
        for (const auto &req : requests) {
            if (req.url.path() == key || req.action() == key)
                found.append(req);
        }
        return found;
    }

    void pushIm(const QString &mid, const QString &command, const QJsonObject &params, int age = 0,
                const QString &module = QStringLiteral("im"))
    {
        const QJsonObject body{{QStringLiteral("module_id"), module},
                               {QStringLiteral("command"), command}, {QStringLiteral("params"), params},
                               {QStringLiteral("extra"), QJsonObject{{QStringLiteral("server_time_ago"), age},
                                   {QStringLiteral("server_time_unix"), QDateTime::currentSecsSinceEpoch() - age}}}};
        const QJsonArray events{QJsonObject{{QStringLiteral("mid"), mid}, {QStringLiteral("text"), body}}};
        clients.last()->sendTextMessage(QString::fromUtf8(QJsonDocument(events).toJson(QJsonDocument::Compact)));
    }

    void pushMessage(const QString &mid, const QString &id, int chatId = 91, int author = 22,
                     bool important = false, int age = 0, bool messageMute = false)
    {
        const QJsonObject message{{QStringLiteral("id"), id}, {QStringLiteral("authorId"), author},
                                  {QStringLiteral("isImportant"), important},
                                  {QStringLiteral("text"), QStringLiteral("Новое тестовое сообщение")},
                                  {QStringLiteral("params"), QJsonObject{{QStringLiteral("NOTIFY"), messageMute ? "N" : "Y"}}}};
        pushIm(mid, QStringLiteral("messageChat"), {{QStringLiteral("chatId"), chatId},
                   {QStringLiteral("dialogId"), QStringLiteral("chat%1").arg(chatId)},
                   {QStringLiteral("notify"), true}, {QStringLiteral("message"), message}}, age);
    }

    void closePull()
    {
        clients.last()->close(QWebSocketProtocol::CloseCodeNormal);
    }

    void releaseHeldOnlineResponse()
    {
        if (!heldOnlineSocket)
            return;
        heldOnlineSocket->write(heldOnlineResponse);
        heldOnlineSocket->disconnectFromHost();
        heldOnlineSocket.clear();
        heldOnlineResponse.clear();
    }

    void releaseHeldChatResponse()
    {
        if (!heldChatSocket)
            return;
        heldChatSocket->write(heldChatResponse);
        heldChatSocket->disconnectFromHost();
        heldChatSocket.clear();
        heldChatResponse.clear();
    }

    void releaseHeldReadResponse()
    {
        if (!heldReadSocket)
            return;
        heldReadSocket->write(heldReadResponse);
        heldReadSocket->disconnectFromHost();
        heldReadSocket.clear();
        heldReadResponse.clear();
    }

    void releaseHeldBoardResponse()
    {
        if (!heldBoardSocket)
            return;
        heldBoardSocket->write(heldBoardResponse);
        heldBoardSocket->disconnectFromHost();
        heldBoardSocket.clear();
        heldBoardResponse.clear();
    }

    void releaseHeldDownload()
    {
        if (!heldDownloadSocket)
            return;
        heldDownloadSocket->write(heldDownloadResponse);
        heldDownloadSocket->disconnectFromHost();
        heldDownloadSocket.clear();
        heldDownloadResponse.clear();
    }

    void releaseHeldIdentity()
    {
        if (!heldIdentitySocket)
            return;
        heldIdentitySocket->write(heldIdentityResponse);
        heldIdentitySocket->disconnectFromHost();
        heldIdentitySocket.clear();
        heldIdentityResponse.clear();
    }

    void releaseHeldNotificationReply(bool fail = false)
    {
        if (!heldNotificationSocket)
            return;
        heldNotificationSocket->write(fail
            ? QByteArray("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\nContent-Length: 0\r\n\r\n")
            : heldNotificationResponse);
        heldNotificationSocket->disconnectFromHost();
        heldNotificationSocket.clear();
        heldNotificationResponse.clear();
    }

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
    void respond(QTcpSocket *socket, QByteArray &buffer)
    {
        const auto headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return;

        const auto lines = buffer.left(headerEnd).split('\n');
        const auto requestLine = lines.first().trimmed().split(' ');
        if (requestLine.size() < 2) {
            socket->disconnectFromHost();
            return;
        }
        Request request;
        request.method = requestLine.at(0);
        request.url = QUrl::fromEncoded(requestLine.at(1));
        for (const auto &line : lines) {
            const auto colon = line.indexOf(':');
            if (colon > 0)
                request.headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
        }
        const auto bodySize = request.headers.value("content-length").toInt();
        if (buffer.size() < headerEnd + 4 + bodySize)
            return;

        request.body = buffer.mid(headerEnd + 4, bodySize);
        request.form = QUrlQuery(QString::fromUtf8(request.body));
        requests.append(request);
        buffer.clear();
        if (closeChatSnapshotConnection && request.action() == QLatin1String("im.v2.Chat.load")) {
            socket->abort();
            return;
        }

        QByteArray status = "200 OK";
        QByteArray extra;
        QByteArray body;
        if (redirectProjects && request.action() == QStringLiteral("socialnetwork.api.workgroup.list")) {
            status = "307 Temporary Redirect";
            extra = "Location: " + redirectLocation.toUtf8() + "\r\n";
        } else if (!downloadRedirect.isEmpty() && request.url.path() == QStringLiteral("/disk/download.xlsx")) {
            status = "302 Found";
            extra = "Location: " + downloadRedirect.toUtf8() + "\r\n";
        } else if (denyIdentity && request.action() == QStringLiteral("main.bitrix.main.controller.loadext.resolveextensions")) {
            status = "401 Unauthorized";
        } else {
            body = reply(request);
            if (body.isEmpty())
                status = "404 Not Found";
        }
        if (request.action() == QStringLiteral("im.v2.Chat.load")) {
            extra += chatResponseHeaders;
            if (!chatResponseOverride.isEmpty())
                body = chatResponseOverride;
            if (!chatResponseStatus.isEmpty())
                status = chatResponseStatus;
        }
        const bool csrfProbe = request.action() == QLatin1String("im.v2.Recent.load")
            && request.field(QStringLiteral("limit")) == QLatin1String("1");
        if (csrfProbe && denyCanonicalSession)
            status = "401 Unauthorized";
        if (!offeredCsrf.isEmpty()
            && (csrfProbe || request.action() == QLatin1String("im.v2.Chat.Message.send"))) {
            const auto token = request.headers.value("x-bitrix-csrf-token");
            if (token != acceptedCsrf
                || (!challengeCookie.isEmpty() && request.headers.value("cookie") != challengeCookie)) {
                body = invalidCsrf(csrfProbe && token == offeredCsrf && !nextCsrf.isEmpty() ? nextCsrf : offeredCsrf);
                if (!challengeCookie.isEmpty())
                    extra += "Set-Cookie: " + challengeCookie + "; Path=/; HttpOnly\r\n";
            } else if (csrfProbe && failReplacementProbe) {
                status = "503 Service Unavailable";
                body = "{}";
            }
        }
        const auto mime = request.url.path() == QStringLiteral("/online/") || (projectGridFixture && request.method == "GET")
            ? "text/html" : "application/json";
        const auto response = "HTTP/1.1 " + status + "\r\n" + extra + "Connection: close\r\n"
            "Content-Type: " + QByteArray(mime) + "; charset=utf-8\r\n"
            "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
        if (holdNextBoardPage && request.action() == QStringLiteral("getColumnItems")) {
            holdNextBoardPage = false;
            heldBoardSocket = socket;
            heldBoardResponse = response;
            return;
        }
        if (holdNextOnlineResponse && request.url.path() == QLatin1String("/online/")) {
            holdNextOnlineResponse = false;
            heldOnlineSocket = socket;
            heldOnlineResponse = response;
            return;
        }
        if (holdNextIdentityResponse
            && request.action() == QLatin1String("main.bitrix.main.controller.loadext.resolveextensions")) {
            holdNextIdentityResponse = false;
            return; // Leave this socket waiting until the client cancels it.
        }
        if (!holdNotificationReply.isEmpty()
            && (request.action() == holdNotificationReply || request.url.path() == holdNotificationReply || (request.url.path() == batchPath &&
                request.field(QStringLiteral("cmd[notify]")).section(QLatin1Char('?'), 0, 0) == holdNotificationReply))) {
            holdNotificationReply.clear();
            heldNotificationSocket = socket;
            heldNotificationResponse = response;
            return;
        }
        if (holdNextChatResponse && request.action() == QStringLiteral("im.v2.Chat.load")) {
            holdNextChatResponse = false;
            heldChatSocket = socket;
            heldChatResponse = response;
            return;
        }
        if (!holdNextReadResponse.isEmpty() && request.action() == holdNextReadResponse) {
            holdNextReadResponse.clear();
            heldReadSocket = socket;
            heldReadResponse = response;
            return;
        }
        if (holdNextDownload && request.url.path() == QStringLiteral("/disk/download.xlsx")) {
            holdNextDownload = false;
            heldDownloadSocket = socket;
            heldDownloadResponse = response;
            return;
        }
        if (holdNextIdentity && request.action() == QStringLiteral("main.bitrix.main.controller.loadext.resolveextensions")) {
            holdNextIdentity = false;
            heldIdentitySocket = socket;
            heldIdentityResponse = response;
            return;
        }
        socket->write(response);
        socket->disconnectFromHost();
    }

    QByteArray reply(const Request &request) const
    {
        if (request.url.path() == QLatin1String("/rest/tasks.task.history.list.json")) {
            if (request.field(QStringLiteral("taskId")) == QLatin1String("43"))
                return R"({"result":{"list":[]}})";
            if (request.field(QStringLiteral("taskId")) != QLatin1String("42"))
                return R"({"error":"ACCESS_DENIED"})";
            return R"({"result":{"list":[{"id":71,"createdDate":"2026-11-20T13:05:00Z","field":"TITLE",
                "value":{"from":"Было","to":"Стало"},"user":{"id":11,"name":"Анна","lastName":"Смит",
                "login":"private-fixture@example.invalid"}},
                {"id":70,"createdDate":"2026-11-20T13:00:00Z","field":"NEW","value":{"from":null,"to":null}}]}})";
        }
        if (request.url.path() == QLatin1String("/rest/disk.storage.getList.json"))
            return R"({"result":[{"ID":"50","ENTITY_TYPE":"group","ENTITY_ID":"18","ROOT_OBJECT_ID":"51"},
                {"ID":"31","ENTITY_TYPE":"group","ENTITY_ID":"17","ROOT_OBJECT_ID":"32"},
                {"ID":"52","ENTITY_TYPE":"user","ENTITY_ID":"17","ROOT_OBJECT_ID":"53"}]})";
        if (request.url.path() == QLatin1String("/rest/disk.storage.getChildren.json"))
            return R"({"result":[{"ID":"77","TYPE":"folder","NAME":"Материалы","STORAGE_ID":"31","PARENT_ID":"32"},
                {"ID":"78","TYPE":"file","NAME":"Смета.xlsx","STORAGE_ID":"31","PARENT_ID":"32","SIZE":"9",
                    "DOWNLOAD_URL":"/disk/download.xlsx"},
                {"ID":"79","TYPE":"file","NAME":"Внешний.xlsx","STORAGE_ID":"31","PARENT_ID":"32",
                    "DOWNLOAD_URL":"https://foreign.invalid/file"},
                {"ID":"80","TYPE":"folder","NAME":"Другой диск","STORAGE_ID":"52","PARENT_ID":"32"}]})";
        if (request.url.path() == QLatin1String("/rest/disk.folder.getChildren.json"))
            return R"({"result":[]})";
        if (projectGridFixture && request.method == "GET") {
            const auto path = request.url.path();
            if (path == QLatin1String("/company/personal/user/11/tasks/"))
                return R"(<a href="http://127.0.0.1:1/tasks/projects/">Other</a>
                    <a href="/company/personal/user/11/tasks/projects/?view=archived&amp;view=mine"><span>Проекты</span></a>)";
            const auto gridPath = QStringLiteral("/company/personal/user/11/tasks/projects/");
            const bool sef = projectGridNavigation == QLatin1String("sef");
            const auto pagePath = [&](int page) {
                return sef ? gridPath + QStringLiteral("nav-projects/page-%1/size-16/").arg(page) : gridPath;
            };
            const auto pageValue = [&](int page) {
                return QStringLiteral("page-%1").arg(page)
                    + (projectGridNavigation == QLatin1String("classic") ? QString() : QStringLiteral("-size-16"));
            };
            const auto pageHref = [&](int page, const QByteArray &scope) {
                return sef ? pagePath(page).toUtf8() + "?" + scope
                           : "?nav-projects=" + pageValue(page).toUtf8() + "&amp;" + scope;
            };
            const auto query = QUrlQuery(request.url);
            const auto isPage = [&](int page) {
                return path == pagePath(page) && query.queryItemValue(QStringLiteral("grid")) == QLatin1String("tasks")
                    && query.allQueryItemValues(QStringLiteral("view")) == QStringList({QStringLiteral("archived"), QStringLiteral("mine")})
                    && (sef || query.queryItemValue(QStringLiteral("nav-projects")) == pageValue(page));
            };
            if (path == gridPath || isPage(2) || isPage(3)) {
                if (query.allQueryItemValues(QStringLiteral("ncc")) != QStringList{QStringLiteral("1")})
                    return R"(<div id="bxdynamic_fixture_start"></div><script>BX.frameCache.init();</script>)";
                const auto header = QByteArray(R"(<script>BX.Socialnetwork.Project.List.Controller.init("TASKS_PROJECT",
                    {"pinMode":"tasks_project"});</script>)");
                if (isPage(3))
                    return header + R"(<tr class="main-grid-row" data-id="50"><td>
                        <div class="socialnetwork-project-list-avatar" data-title="Команда Три"></div>
                    </td></tr>)";
                if (isPage(2))
                    return header + R"(<tr class="main-grid-row" data-id="43"><td>
                        <div class="socialnetwork-project-list-avatar" data-title="Команда Два" data-color="#B15EF5"></div>
                        <button class="main-grid-cell-content-action-pin" aria-pressed="true"></button>
                    </td></tr><a rel="next" href="?nav-other=page-3-size-16">Другая сетка</a>
                    <a rel="next" href=")" + pageHref(3, "grid=other&amp;view=archived&amp;view=mine") + R"(">Другая область</a>
                    <a rel="next" href=")" + pageHref(3, "view=mine&amp;view=archived&amp;grid=tasks") + R"(">Обратный порядок фильтров</a>
                    <a href=")" + pageHref(3, "view=archived&amp;view=mine&amp;grid=tasks") + R"(">Следующая</a>)";
                const auto nextHref = pageHref(2, "grid=tasks&amp;view=archived&amp;view=mine&amp;ncc=0");
                auto pager = QByteArray("<a href=\"") + nextHref + "\"><span>Следующая</span></a>";
                if (sef)
                    pager = "<div class=\"bx-pagination\"><ul><li class=\"bx-pag-next\">" + pager + "</li></ul></div>";
                if (projectGridNavigation == QLatin1String("size"))
                    pager = "<a rel=\"next\" href=\"" + nextHref + "\"><span>Следующая</span></a>";
                if (projectGridNavigation != QLatin1String("classic"))
                    pager.prepend(R"(<a href="?nav-other=page-2&amp;grid=other&amp;view=archived&amp;view=mine">Другая сетка на той же странице</a>)");
                if (projectGridNavigation == QLatin1String("ambiguous"))
                    pager = R"(<a href="?nav-other=page-2&amp;grid=other&amp;view=archived&amp;view=mine">Другой вид</a>)"
                        + QByteArray("<a href=\"") + nextHref + "\">Проекты</a>";
                pager.prepend(QByteArray("<a rel=\"next\" href=\"")
                    + pageHref(2, "grid=tasks&amp;view=archived&amp;view=mine&amp;view=all")
                    + "\">Другой последний фильтр</a>");
                return header + R"(<tr class="main-grid-row" data-id="17"><td>
                    <a class="socialnetwork-project-list-name-link" href="/workgroups/group/17/">Схема &amp; маршрут</a>
                    <span class="socialnetwork-project-list-avatar" data-title="Схема &amp; маршрут"
                        data-pic="/upload/project.png" data-border-variant="guest" data-color="#02BB9A"></span>
                    <button class="main-grid-cell-content-action-pin main-grid-cell-content-action-active"></button>
                    </td></tr><div><a href="https://foreign.invalid/company/personal/user/11/tasks/projects/?nav-projects=page-2-size-16">Внешняя</a>
                    <a href="/workgroups/projects/?nav-projects=page-2-size-16">Другая область</a>
                    <a href="?nav-projects=page-4-size-16">Пропуск</a>
                    <a href="?nav-projects=page-2&amp;nav-other=page-2">Два курсора</a>)" + pager + "</div>";
            }
        }
        if (dismissedChatsFixture && request.url.path() == QStringLiteral("/rest/im.user.list.get.json"))
            return R"({"result":{"106":{"active":false,"type":"user"},
                "107":{"id":107,"active":false,"type":"guest"},
                "108":{"id":109,"active":false,"type":"user"}}})";
        if (request.url.path() == QStringLiteral("/disk/download.xlsx"))
            return mismatchDownload ? QByteArray("short") : QByteArray("xlsx-data");
        if (request.url.path() == QStringLiteral("/online/"))
            return messengerPage;
        if (replyPreviewFixture && request.url.path() == QStringLiteral("/rest/im.dialog.messages.search.json")
            && request.field(QStringLiteral("CHAT_ID")) == QStringLiteral("91")) {
            return R"({"result":{"messages":[{"id":712,"chatId":91,"authorId":22,
                "text":"Найденный ответ","params":{"REPLY_ID":"690"}}]}})";
        }
        if (request.url.path() == pushPath) {
            const auto socketUrl = QByteArray("ws://127.0.0.1:") + QByteArray::number(websocket.serverPort()) + "/subws/";
            return "{\"result\":{\"server\":{\"websocket_secure\":\"" + socketUrl +
                "\"},\"channels\":{\"private\":{\"id\":\"synthetic-private\"},"
                "\"shared\":{\"id\":\"synthetic-shared\"}},\"clientId\":\"fixture\"}}";
        }
        if (request.url.path() == batchPath) {
            QJsonObject lookups;
            for (const auto &[name, value] : request.form.queryItems(QUrl::FullyDecoded)) {
                if (name.startsWith(QLatin1String("cmd[lookup_")) && name.endsWith(QLatin1Char(']'))
                    && value.startsWith(QLatin1String("im.dialog.get?DIALOG_ID=chat"))) {
                    const auto id = value.section(QLatin1String("=chat"), 1);
                    if (projectUnreadDialogReplies.contains(id))
                        lookups.insert(name.mid(4, name.size() - 5), projectUnreadDialogReplies.value(id));
                }
            }
            if (!lookups.isEmpty())
                return QJsonDocument(QJsonObject{{"result", QJsonObject{{"result", lookups}}}}).toJson(QJsonDocument::Compact);
            const auto command = request.field(QStringLiteral("cmd[notify]"));
            if (command.startsWith(QStringLiteral("im.notify.get?")) && command.contains(QStringLiteral("&LAST_ID="))) {
                if (!olderNoticeFixtures.contains(command))
                    return R"({"result":{"result_error":{"notify":{"error":"TEST_DENIED"}}}})";
                const QJsonObject payload{{"notifications", olderNoticeFixtures.value(command)}, {"users", noticeUsersFixture}};
                return QJsonDocument(QJsonObject{{"result", QJsonObject{{"result", QJsonObject{{"notify", payload}}}}}})
                    .toJson(QJsonDocument::Compact);
            }
            if (command.startsWith(QStringLiteral("im.notify.read.list?")))
                return rejectNoticeBatch ? R"({"result":{"result":{"notify":false}}})"
                                         : R"({"result":{"result":{"notify":true}}})";
            if (!noticeFixture.isEmpty() && (command.startsWith(QStringLiteral("im.notify.get?"))
                                           || command.startsWith(QStringLiteral("im.notify.history.search?")))) {
                const QJsonObject payload{{"notifications", noticeFixture}, {"users", noticeUsersFixture}};
                const QJsonObject value{{"result", QJsonObject{{"result", QJsonObject{{"notify", payload}}}}}};
                return QJsonDocument(value).toJson(QJsonDocument::Compact);
            }
            if (command.startsWith(QStringLiteral("im.notify.history.search?")))
                return R"({"result":{"result":{"notify":{"notifications":[{"id":77,"notify_module":"tasks",
                    "notify_title":"Новая задача","text":"Срочное событие","date":"2026-04-01T12:00:00Z","notify_read":"N"}]}}}})";
            if (command.startsWith(QStringLiteral("im.notify.read?")))
                return R"({"result":{"result":{"notify":true}}})";
            if (command == QStringLiteral("im.notify.read.all"))
                return R"({"result":{"result":{"notify":{"result":true}}}})";
            if (command.startsWith(QStringLiteral("im.notify.delete?")))
                return (command == QStringLiteral("im.notify.delete?ID=77") || command == QStringLiteral("im.notify.delete?ID=5"))
                    ? R"({"result":{"result":{"notify":true}}})"
                    : R"({"result":{"result":{"notify":false}}})";
            return R"({"result":{"result":{"notify":{"notifications":[{"id":5,"notify_module":"tasks",
                "notify_title":"Срок задачи","text":"Проверьте срок","date":"2026-04-01T12:00:00Z","notify_read":"Y"}]}}}})";
        }
        if (chatActionsFixture && request.url.path() == QStringLiteral("/rest/im.chat.file.get.json")) {
            if (!request.field(QStringLiteral("LAST_ID")).isEmpty())
                return R"({"result":{"list":[],"files":[]}})";
            QJsonArray rows;
            for (int id = 500; id > 450; --id)
                rows.append(QJsonObject{{"id", id}, {"chatId", 91}, {"messageId", 701}, {"fileId", 3}});
            const auto data = QJsonObject{{"list", rows}, {"files", QJsonArray{QJsonObject{
                {"id", 3}, {"name", "Фото.jpg"}, {"type", "image"}, {"urlDownload", "/disk/download.xlsx"}}}}};
            return QJsonDocument(QJsonObject{{"result", data}}).toJson(QJsonDocument::Compact);
        }
        if (chatActionsFixture && request.url.path() == QStringLiteral("/rest/im.chat.url.get.json"))
            return R"json({"result":{"list":[{"id":1,"chatId":91,"messageId":701,
                "url":{"source":"https://example.com/a","richData":{"name":"Ссылка"}}},
                {"id":2,"chatId":92,"url":{"source":"https://example.com/foreign"}},
                {"id":3,"chatId":91,"url":{"source":"javascript:alert(1)"}},
                {"id":4,"chatId":91,"url":{"source":"https://user@example.com/private"}}]}})json";
        if (chatActionsFixture && request.url.path() == QStringLiteral("/rest/im.v2.Chat.add.json"))
            return R"({"result":{"chatId":93}})";
        if (!taskShareResult.isEmpty() && request.url.path() == QStringLiteral("/rest/im.message.share.json")) {
            if (request.field(QStringLiteral("DIALOG_ID")) != QLatin1String("chat91")
                || request.field(QStringLiteral("MESSAGE_ID")) != QLatin1String("701")
                || request.field(QStringLiteral("TYPE")) != QLatin1String("TASK"))
                return R"({"result":false})";
            taskSharedInSource = taskShareResult == R"({"result":true})";
            return taskShareResult;
        }
        if (chatActionsFixture && (request.url.path() == QStringLiteral("/rest/im.v2.Chat.Message.pin.json")
            || request.url.path() == QStringLiteral("/rest/im.v2.Chat.Message.unpin.json")
            || request.url.path() == QStringLiteral("/rest/im.chat.favorite.add.json")
            || request.url.path() == QStringLiteral("/rest/im.chat.favorite.delete.json")
            || request.url.path() == QStringLiteral("/rest/im.chat.user.add.json")
            || request.url.path() == QStringLiteral("/rest/im.v2.Chat.deleteUser.json")))
            return R"({"result":true})";
        if (portalStatusFixture && request.url.path() == QStringLiteral("/rest/im.user.status.get.json")) {
            return QJsonDocument(QJsonObject{{"result", portalStatus}}).toJson(QJsonDocument::Compact);
        }
        if (portalStatusFixture && request.url.path() == QStringLiteral("/rest/im.user.status.set.json")) {
            portalStatus = request.field(QStringLiteral("STATUS"));
            return R"({"result":true})";
        }
        if (request.url.path() == QLatin1String("/rest/tasks.task.update.json"))
            return QJsonDocument(QJsonObject{{"result", taskRatingReply}}).toJson(QJsonDocument::Compact);
        if (request.url.path() == QLatin1String("/rest/tasks.task.delete.json"))
            return QJsonDocument(QJsonObject{{"result", taskDeletionReply}}).toJson(QJsonDocument::Compact);
        if (request.url.path() == QLatin1String("/rest/tasks.task.list.json")) {
            if (!request.field(QStringLiteral("filter[%TITLE]")).isEmpty()
                || !request.field(QStringLiteral("filter[ID]")).isEmpty())
                return QJsonDocument(QJsonObject{{"result", QJsonObject{{"tasks", taskChatSearchRows}}}})
                    .toJson(QJsonDocument::Compact);
            if (!request.field(QStringLiteral("filter[ID][0]")).isEmpty())
                return QJsonDocument(QJsonObject{{"result", QJsonObject{{"tasks", projectUnreadTaskReplies}}}})
                    .toJson(QJsonDocument::Compact);
            return projectTaskChatPages.value(request.field(QStringLiteral("filter[GROUP_ID]"))
                + QLatin1Char(':') + request.field(QStringLiteral("start")));
        }
        if (request.url.path() != ajaxPath)
            return {};

        const auto action = request.action();
        if (action == QStringLiteral("socialnetwork.api.workgroup.get"))
            return ajaxResult(R"({"ID":"17","OWNER_ID":11,"OWNER_DATA":{"ID":11,"FORMATTED_NAME":"Анна"},
                "MODERATOR_MEMBERS":[12],"ORDINARY_MEMBERS":[13],"INVITED_MEMBERS":[14],
                "LIST_OF_MEMBERS":{"12":{"id":12,"name":"Борис","lastName":"Смит"},"13":{"id":13,"name":"Дина"}},
                "LIST_OF_MEMBERS_AWAITING_INVITE":[{"id":15,"name":"Тим"}],
                "GROUP_MEMBERS_LIST":[{"id":14,"invited":true},{"id":15,"isAwaiting":true}]})");
        if (chatActionsFixture && action.startsWith(QLatin1String("im.v2.Chat.Message.Reaction."))) {
            if (!request.headers.value("content-type").startsWith("application/x-www-form-urlencoded")
                || request.field(QStringLiteral("messageId")) != QLatin1String("701")
                || request.field(QStringLiteral("reaction")).isEmpty())
                return R"({"status":"error","errors":[{"code":"FIXTURE_FORM_REQUIRED"}]})";
            if (refuseChatAction)
                return ajaxResult("false");
            if (action.endsWith(QLatin1String(".add")))
                addedReaction = request.field(QStringLiteral("reaction"));
            else
                addedReaction.clear();
            return ajaxResult("[]");
        }
        if (chatActionsFixture && action == QStringLiteral("im.v2.Chat.Message.update")) {
            if (!request.headers.value("content-type").startsWith("application/x-www-form-urlencoded")
                || request.field(QStringLiteral("id")) != QLatin1String("701")
                || request.field(QStringLiteral("fields[message]")).isEmpty())
                return R"({"status":"error","errors":[{"code":"FIXTURE_FORM_REQUIRED"}]})";
            return ajaxResult(refuseChatAction ? "false" : "[]");
        }
        if (chatActionsFixture && action == QStringLiteral("im.v2.Chat.Message.delete"))
            return ajaxResult(refuseChatAction ? "false" : "[]");
        if (chatActionsFixture && action == QStringLiteral("im.v2.Chat.Message.send")) {
            const auto body = QJsonDocument::fromJson(request.body).object();
            const auto forward = body.value(QStringLiteral("fields")).toObject().value(QStringLiteral("forwardIds")).toObject();
            if (!forward.isEmpty()) {
                const auto key = forward.constBegin().key();
                const auto response = refuseChatAction ? QJsonObject{{QStringLiteral("failureMap"), QJsonObject{{key, "denied"}}}}
                    : QJsonObject{{QStringLiteral("uuidMap"), QJsonObject{{key, 810}}}};
                return ajaxResult(QJsonDocument(response).toJson(QJsonDocument::Compact));
            }
        }
        if (action == QStringLiteral("tasks.v2.CheckList.get"))
            return ajaxResult(R"({"status":"success","revision":7,"checklist":[
                {"id":70,"nodeId":"parent","parentId":0,"title":"Проверки"},
                {"id":71,"nodeId":"test","parentId":70,"parentNodeId":"parent","title":"Включить","sortIndex":0,
                 "isComplete":false,"actions":{"toggle":true},"attachments":["n123"]}]})");
        if (action == QStringLiteral("tasks.v2.CheckList.complete"))
            return ajaxResult(R"({"status":"denied"})");
        if (action == QStringLiteral("tasks.v2.CheckList.save"))
            return ajaxResult(R"({"status":"success","revision":8,"checklist":[]})");
        if (action == QStringLiteral("tasks.v2.Task.Relation.Child.list"))
            return ajaxResult(R"({"tasks":[{"id":43,"title":"Дочерняя задача","group":{"id":17},"parent":{"id":42}}]})");
        if (action == QStringLiteral("tasks.v2.Task.Relation.Gantt.Dependence.list"))
            return ajaxResult(R"({"tasks":[{"id":43,"title":"Последователь","ganttLinks":{"42":"finish_start","99":"start_start"}},
                {"id":42,"title":"Выбранная задача","ganttLinks":{"44":"finish_finish"}},{"id":44,"title":"Предшественник"}]})");
        if (chatBadgesFixture && action == QStringLiteral("im.v2.Chat.Message.read")) {
            const bool form = request.headers.value("content-type").startsWith("application/x-www-form-urlencoded")
                && !request.field(QStringLiteral("ids[0]")).isEmpty();
            return approveVisibleRead && form ? ajaxResult(visibleReadResult)
                : QByteArrayLiteral(R"({"status":"error","errors":[{"code":"FIXTURE_DENIED"}]})");
        }
        if (chatBadgesFixture && action == QStringLiteral("im.v2.Anchor.read"))
            return request.headers.value("content-type").startsWith("application/x-www-form-urlencoded")
                && !request.field(QStringLiteral("messageIds[0]")).isEmpty()
                ? ajaxResult(approveAnchorRead ? "true" : "false")
                : QByteArrayLiteral(R"({"status":"error","errors":[{"code":"FIXTURE_DENIED"}]})");
        if (action == QStringLiteral("im.v2.Chat.Message.tail")) {
            if (request.field(QStringLiteral("chatId")) == QLatin1String("73"))
                return ajaxResult(R"({"chatId":73,"hasNextPage":false,"messages":[]})");
            return ajaxResult(R"({"chatId":91,"hasNextPage":true,"messages":[
                {"id":801,"chatId":91,"senderId":22,"text":"Cursor"},
                {"id":800,"chatId":91,"senderId":22,"text":"Recent older"},
                {"id":800,"chatId":91,"senderId":22,"text":"Duplicate"},
                {"id":798,"chatId":92,"senderId":22,"text":"Other chat"},
                {"id":799,"chatId":91,"senderId":22,"text":"Oldest"}]})");
        }
        if (chatBadgesFixture && action == QStringLiteral("im.v2.Counter.get"))
            return ajaxResult(R"({"userCounters":[
                {"chatId":91,"counter":127,"recentSections":["default"]},
                {"chatId":73,"counter":7,"isMuted":true,"recentSections":["tasksTask"]}]})");
        if (action == QStringLiteral("im.v2.Recent.load")) {
            const auto &badgeFixture = request.field(QStringLiteral("filter[recentSection]")) == QLatin1String("tasksTask")
                ? taskRecentBadgeFixture : ordinaryRecentBadgeFixture;
            if (!badgeFixture.isEmpty())
                return ajaxResult(badgeFixture);
            if (presencePinsFixture) {
                const auto lastActive = QDateTime::currentDateTimeUtc().addSecs(-960).toString(Qt::ISODate).toUtf8();
                return ajaxResult(R"({"userId":11,"recentItems":[{"dialogId":"103","chatId":91},{"dialogId":"104"},
                    {"dialogId":"chat73","chatId":73},{"dialogId":"105","type":"user"},
                    {"dialogId":"106","type":"private"},{"dialogId":"107","type":"bot"}],
                    "chats":[{"id":73,"dialogId":"chat73","type":"chat","name":"Команда"}],
                    "users":[{"id":103,"name":"Сотрудник","workPosition":"Инженер","active":true,"lastActivityDate":")"
                    + lastActive + R"("},{"id":104,"name":"Статус неизвестен","active":true},
                    {"id":107,"name":"Бот","workPosition":"Не должность"}]})");
            }
            if (chatBadgesFixture && request.field(QStringLiteral("filter[recentSection]")) == QStringLiteral("tasksTask"))
                return ajaxResult(R"({"userId":11,"recentItems":[{"dialogId":"chat73","chatId":73,"counter":0}],
                    "chats":[{"id":73,"dialogId":"chat73","type":"tasksTask","name":"Обсуждение"}]})");
            if (liveRecentFixture && request.field(QStringLiteral("filter[recentSection]")) == QStringLiteral("tasksTask"))
                return ajaxResult(R"({"userId":11,"recentItems":[],"chats":[]})");
            if (liveRecentFixture)
                return ajaxResult(R"({"userId":11,"recentItems":[{"dialogId":"chat91","chatId":91,"messageId":700,"counter":0}],
                    "chats":[{"id":91,"dialogId":"chat91","type":"chat","name":"Старое состояние","lastId":700}]})");
            if (dismissedChatsFixture)
                return ajaxResult(QByteArray(omitRecentUser ? "{" : "{\"userId\":11,") + R"("recentItems":[{"dialogId":"103","chatId":201,"type":"user","pinned":true},
                    {"dialogId":"104"},{"dialogId":"105","invited":{"originatorId":11,"canResend":true}},
                    {"dialogId":"106","chatId":202,"type":"user"},{"dialogId":"107"},{"dialogId":"108"},
                    {"dialogId":"chat103"},{"dialogId":"109","chatId":203,"type":"user"}],"users":[
                    {"id":103,"name":"Уволенный","active":false,"type":"user",
                        "internalAccount":false,"intranetUser":false},
                    {"id":104,"name":"Внешний","active":false,"type":"user","extranet":true},
                    {"id":105,"name":"Приглашённый","active":false,"type":"user"},
                    {"id":109,"name":"Уволенный","active":true,"type":"user"}],
                    "usersShort":[{"id":106,"name":"Без статуса"},{"id":107,"name":"Гость"},{"id":108,"name":"Без статуса"}],
                    "chats":[{"id":201,"dialogId":"103","type":"private"},{"id":202,"dialogId":"106","type":"private"},
                    {"id":203,"dialogId":"109","type":"private"},{"id":103,"dialogId":"chat103","type":"chat","name":"Команда"}]})");
            if (richRecentFixture && request.field(QStringLiteral("filter[recentSection]")) == QStringLiteral("tasksTask"))
                return ajaxResult(R"({"recentItems":[{"dialogId":"chat73","chatId":73,"pinned":true}],
                    "chats":[{"id":73,"dialogId":"chat73","type":"tasksTask","name":"Обсуждение задачи","userCounter":74,
                        "entityLink":{"id":"42","type":"TASKS_TASK"}}],"counters":[{"chatId":73,"counter":3}]})");
            if (richRecentFixture)
                return ajaxResult(R"({"recentItems":[{"dialogId":"103","chatId":81,"counter":0},
                    {"dialogId":"chat91","chatId":91,"unread":true,"pinned":true},
                    {"dialogId":"chat74","chatId":74}], "chats":[{"id":81,"dialogId":"103","type":"user"},
                    {"id":91,"dialogId":"chat91","name":"Общий чат","type":"chat"},
                    {"id":74,"dialogId":"chat74","name":"Старая задача","type":"tasks"}],
                    "counters":[{"chatId":81,"counter":7}],"usersShort":[{"id":103,"name":"Имя из портала","avatar":"/upload/person.png"}]})");
            if (omitRecentUser)
                return ajaxResult(R"({"recentItems":[],"chats":[],"messages":[]})");
            return ajaxResult(R"({"recentItems":[{"dialogId":"chat91","messageId":701,"counter":2}],
                "chats":[{"dialogId":"chat91","name":"Обычный чат"}],
                "messages":[{"id":701,"text":"Привет"}],"userId":11})");
        }
        if (action == QStringLiteral("main.bitrix.main.controller.loadext.resolveextensions"))
            return ajaxResult(R"([{"extension":"socialnetwork.v2.current-user","settings":{"id":)"
                + QByteArray::number(currentUserId) + "}}]");
        if (action == QStringLiteral("im.v2.Settings.General.list")) {
            QJsonObject sound{{"soundMuted", serverSoundMuted}, {"soundVolumeGeneral", serverGeneralVolume},
                              {"soundVolumeMessages", serverMessageVolume}};
            if (!serverNotificationScheme.isEmpty()) {
                sound.insert("notifyScheme", serverNotificationScheme);
                if (!omitDesktopDelivery)
                    sound.insert("notifySchemeSendSite", serverSiteDelivery);
                sound.insert("notifySchemeSendPush", true);
            }
            return ajaxResult(QJsonDocument(sound).toJson(QJsonDocument::Compact));
        }
        if (action == QStringLiteral("im.v2.Settings.General.update")) {
            const auto body = QJsonDocument::fromJson(request.body).object();
            if (!ignoreSoundUpdate && body.value("name") == QLatin1String("soundVolumeGeneral"))
                serverGeneralVolume = body.value("value").toInt();
            if (!ignoreSoundUpdate && body.value("name") == QLatin1String("notifySchemeSendSite"))
                serverSiteDelivery = body.value("value").toBool();
            return ajaxResult("true");
        }
        if (action == QStringLiteral("socialnetwork.api.workgroup.list") && legacyProjectFallbackFixture) {
            if (request.field(QStringLiteral("start")) == QLatin1String("50"))
                return ajaxResult(R"({"workgroups":[{"id":43,"name":"Доступен","additionalData":{"role":""}}]})");
            return ajaxResult(R"({"workgroups":[{"id":17,"name":"Роль не передана"}],"next":50})");
        }
        if (action == QStringLiteral("socialnetwork.api.workgroup.list") && projectFallbackFixture) {
            if (request.field(QStringLiteral("start")) == QLatin1String("50"))
                return ajaxResult(R"({"workgroups":[{"id":43,"name":"Участники страницы 2","pinned":true,
                    "avatarType":"project-3","additionalData":{"role":"E"}}]})");
            return ajaxResult(R"({"workgroups":[
                {"id":17,"name":"Мой проект","AVATAR_TYPE":"folder","additionalData":{"role":"K"}},
                {"id":19,"name":"Чужой проект","additionalData":{"role":""}},
                {"id":20,"name":"Приглашение","additionalData":{"role":"Z"}}],"next":50})");
        }
        if (action == QStringLiteral("socialnetwork.api.workgroup.list"))
            return ajaxResult(R"([{"ID":"17","NAME":"Проект испытаний","avatar":"/upload/project.png"}])");
        if (action == QStringLiteral("socialnetwork.v2.Project.changePin")) {
            pinnedProject = !pinnedProject;
            return ajaxResult(pinnedProject ? R"({"pinned":true})" : R"({"pinned":false})");
        }
        if (action == QStringLiteral("im.v2.Chat.pin") || action == QStringLiteral("im.v2.Chat.unpin"))
            return ajaxResult(R"(true)");
        if (action == QStringLiteral("im.v2.Chat.Mention.list"))
            return ajaxResult(R"({"users":[{"id":74,"name":"Участник","workPosition":" ",
                "work_position":"Инженер","avatar":"","avatarUrl":"/upload/participant.png"}]})");
        if (action == QStringLiteral("ui.entityselector.load")) {
            const auto id = QJsonDocument::fromJson(request.body).object().value("dialog").toObject().value("id");
            if (id == QLatin1String("initial-task-people-denied"))
                return R"({"status":"error","errors":[{"message":"Not available"}]})";
            QJsonArray items{
                QJsonObject{{"id", 75}, {"entityId", "user"}, {"title", "Ирина"}},
                QJsonObject{{"id", 76}, {"entityId", "user"}, {"title", "Сергей"}, {"avatar", "/upload/staff.png"}},
                QJsonObject{{"id", 88}, {"entityId", "user"}, {"title", "Вне списка"}},
                QJsonObject{{"id", 76}, {"entityId", "department"}, {"title", "Отдел"}}};
            QJsonArray recent{QJsonArray{"department", 76}, QJsonArray{"user", "76"}, QJsonArray{"user", 404},
                              QJsonArray{"user", 76}, QJsonArray{"user", 75}};
            for (int id = 1000; id < 1025; ++id) {
                items.append(QJsonObject{{"id", id}, {"entityId", "user"}, {"title", QString::number(id)}});
                recent.append(QJsonArray{"user", id});
            }
            if (id == QLatin1String("initial-task-people-no-recents"))
                recent = {};
            const QJsonObject dialog{{"items", items}, {"recentItems", recent}};
            return ajaxResult(QJsonDocument(QJsonObject{{"dialog", dialog}}).toJson(QJsonDocument::Compact));
        }
        if (action == QStringLiteral("ui.entityselector.doSearch")) {
            const auto body = QJsonDocument::fromJson(request.body).object();
            if (body.value("searchQuery").toObject().value("query") == QLatin1String("denied"))
                return R"({"status":"error","errors":[{"message":"Search unavailable"}]})";
            const auto dialog = body.value("dialog").toObject();
            const auto entities = dialog.value("entities").toArray();
            const auto entity = entities.isEmpty() ? QJsonObject{} : entities.first().toObject();
            if (entity.value("id") == QLatin1String("user"))
                return ajaxResult(R"({"dialog":{"items":[{"id":75,"entityId":"user","title":"Ирина","subtitle":"Работа"},
                    {"id":805,"entityId":"department","title":"Отдел"}]}})");
            if (entity.value("options").toObject().value("mentionSearch").toBool())
                return ajaxResult(R"({"dialog":{"items":[{"id":"74","entityType":"im-user",
                    "subtitle":{"text":"Инженер"},
                    "customData":{"user":{"name":"Иван","workPosition":"","avatar":"/upload/user.png"}}},
                    {"id":"75","entityType":"im-user","title":"Другой пользователь",
                    "customData":{"user":{"id":76,"avatar":"/upload/unmatched.png"}}},
                    {"id":"chat45","entityType":"chat","title":"Группа"}]}})");
        }
        if (action == QStringLiteral("ui.entityselector.doSearch") && dismissedChatsFixture)
            return ajaxResult(R"({"dialog":{"items":[{"id":"103","title":"Имя","entityType":"chat",
                "customData":{"user":{"id":103,"active":false,"type":"user","intranetUser":false}}},
                {"id":"104","title":"Снаружи","entityType":"chat","customData":{"user":{
                    "id":104,"active":false,"type":"collaber"}}}]}})");
        if (action == QStringLiteral("ui.entityselector.doSearch") && richRecentFixture)
            return ajaxResult(R"({"dialog":{"items":[{"id":"103","title":"Диалог 103","entityType":"chat",
                "customData":{"user":{"id":103,"name":"Имя из портала","avatar":"/upload/person.png"}}},
                {"id":"chat73","title":"Обсуждение задачи","entityType":"chat","customData":{"chat":{
                    "id":73,"type":"tasksTask","entityLink":{"id":"42","type":"TASKS_TASK"}}}}]}})");
        if (action == QStringLiteral("applyFilter") && boardPagingFixture) {
            QJsonArray tasks;
            tasks.append(QJsonObject{{"id", 1000}, {"columnId", 50}, {"data", QJsonObject{
                {"name", "Первая колонка"}, {"tags", QJsonArray{"Обсудить", "Бэкенд", "Обсудить"}},
                {"author", QJsonObject{{"id", 11}, {"name", "Постановщик"},
                    {"photo", QJsonObject{{"src", "/upload/resize_cache/user/author.png"}}}}},
                {"responsible", QJsonObject{{"id", "22"}, {"name", "Исполнитель"},
                    {"photo", QJsonObject{{"src", "/upload/resize_cache/user/assignee.png"}}}}}
            }}});
            for (int i = 1; i < tunables::kPageSizeBoardTasks; ++i)
                tasks.append(QJsonObject{{"id", 1000 + i}, {"columnId", 50}, {"data", QJsonObject{{"name", "Первая колонка"}}}});
            tasks.append(QJsonObject{{"id", 42}, {"columnId", 60}, {"data", QJsonObject{{"name", "Первая страница"}}}});
            const QJsonArray columns{
                QJsonObject{{"id", 50}, {"name", "Разбор"}, {"total", tunables::kPageSizeBoardTasks}},
                QJsonObject{{"id", 60}, {"name", "Рецензия"}, {"total", 3}},
                QJsonObject{{"id", 70}, {"name", "Ожидание"}, {"total", 1}}
            };
            return ajaxResult(QJsonDocument(QJsonObject{{"columns", columns}, {"items", tasks}}).toJson(QJsonDocument::Compact));
        }
        if (action == QStringLiteral("getColumnItems") && boardPagingFixture) {
            if (std::exchange(badNextBoardPage, false))
                return ajaxResult(R"({"items":{}})");
            if (request.field(QStringLiteral("columnId")) == QLatin1String("70"))
                return ajaxResult("[]");
            if (request.field(QStringLiteral("pageId")) == QLatin1String("2"))
                return ajaxResult(R"([{"id":42,"columnId":60,"data":{"name":"Дубль"}},
                    {"id":43,"columnId":60,"data":{"name":"Вторая страница","tags":["Проверить","","Проверить",42],
                        "author":{"id":"44","name":"Автор страницы","photo":{"src":"https://untrusted.invalid/avatar.png"}},
                        "responsible":{"id":22,"name":"Исполнитель без фотографии","photo":false}}},
                    {"id":45,"columnId":50,"data":{"name":"Чужая колонка"}},
                    {"id":46,"columnId":60,"data":{"name":"Чужой проект","groupId":999}}])");
            if (request.field(QStringLiteral("pageId")) == QLatin1String("3"))
                return ajaxResult(R"({"items":[{"id":44,"columnId":60,"data":{"name":"Третья страница"}}]})");
        }
        if (action == QStringLiteral("applyFilter")) {
            auto columns = QByteArray(R"([{"id":50,"name":"Разбор запроса","total":1})");
            if (extendedStageFixture)
                columns += R"(,{"id":"80","name":"Скрытая пагинацией","total":6},{"id":"90","name":"Неизвестно"})";
            columns += R"(,{"id":"60","name":"Рецензия","COLOR":"#FF9966","total":0}])";
            return ajaxResult("{\"columns\":" + columns +
                R"(,"items":[{"id":"42","columnId":50,"data":{"name":"Проверить маршрут"}}]})");
        }
        if (action == QStringLiteral("tasks.task.get")) {
            if (!taskLegacyResponseOverride.isEmpty())
                return taskLegacyResponseOverride;
            if (!actionTaskLegacy.isEmpty())
                return ajaxResult(QJsonDocument(QJsonObject{{"task", actionTaskLegacy}}).toJson(QJsonDocument::Compact));
            return ajaxResult(R"({"task":{"id":"42","title":"Проверить маршрут","stageId":50,
                "chatId":73,"description":"Сверить схему & уведомления","status":"3","groupId":"17",
                "group":{"id":17,"name":"Команда"},
                "creator":{"id":11,"name":"Автор","icon":"/upload/resize_cache/user/legacy-author.jpg"},
                "responsible":{"id":22,"name":"Исполнитель","icon":"/upload/resize_cache/user/old-role.jpg"},
                "tags":{"57":{"id":57,"title":"Устаревшая метка"}},"createdDate":"2026-04-01T12:00:00Z",
                "auditorsData":)" + legacyTaskAuditors + "}}");
        }
        if (action == QStringLiteral("tasks.task.list")) {
            const auto selections = request.form.queryItems(QUrl::FullyDecoded);
            if (selections.contains({QStringLiteral("select[]"), QStringLiteral("START_DATE_PLAN")}))
                return ajaxResult(R"({"tasks":[{"id":42,"title":"План","groupId":17,
                    "startDatePlan":"2026-08-01T09:00:00Z","endDatePlan":"2026-08-03T12:00:00Z"},
                    {"id":43,"title":"Без дат","groupId":17},{"id":99,"title":"Не этот проект","groupId":18}]})");
            if (request.field(QStringLiteral("filter[ID][0]")) == QStringLiteral("42"))
                return ajaxResult(R"({"tasks":[{"id":"42","title":"Проверить маршрут","groupId":17}]})");
            if (!request.field(QStringLiteral("filter[STAGE_ID]")).isEmpty())
                return ajaxResult(R"({"tasks":[]})");
            return ajaxResult(R"({"tasks":[{"id":"149","title":"Найти смету","groupId":"17","stageId":"60"},
                {"id":"150","title":"Найти смету другой команды","groupId":"18","stageId":"70"}]})");
        }
        if (action == QStringLiteral("tasks.v2.Task.get") && !taskCurrentResponseOverride.isEmpty())
            return taskCurrentResponseOverride;
        if (action == QStringLiteral("tasks.v2.Task.get") && !actionTaskCurrent.isEmpty())
            return ajaxResult(QJsonDocument(actionTaskCurrent).toJson(QJsonDocument::Compact));
        if (action == QStringLiteral("tasks.v2.Task.get") && editableTaskFixture) {
            auto task = QJsonDocument::fromJson(R"({"id":42,"creator":{"id":11,"name":"Постановщик",
                    "image":{"src":"https:\/\/cdn-ru.bitrix24.ru/b119/resize_cache/user/card.png"}},
                "responsible":{"id":12,"name":"Исполнитель"},
                "tags":[{"id":57,"name":"Регрессия"}],
                "createdTs":1764849882,"stage":{"id":60,"title":"Проверка"},"group":{"id":17,"name":"Проект"},
                "status":"pending","rights":{"edit":true,"changeDirector":true,"changeResponsible":true,
                "addAuditors":true,"watch":false,"deadline":true,"sort":true,"start":true,"complete":true,"pause":false}})").object();
            QJsonArray auditors;
            for (const auto personId : taskAuditorIds) {
                QJsonObject person{{"id", personId}, {"name", QStringLiteral("Наблюдатель")}};
                if (personId == 13)
                    person.insert(QStringLiteral("image"), QJsonObject{{"src", "/upload/resize_cache/task-auditor-modern.png"}});
                else if (personId == 44)
                    person.insert(QStringLiteral("image"), QJsonObject{{"src", "https://external.invalid/profile.png"}});
                else if (personId == 55)
                    person.insert(QStringLiteral("image"), QJsonValue::Null);
                auditors.append(person);
            }
            task.insert(QStringLiteral("auditors"), auditors);
            return ajaxResult(QJsonDocument(task).toJson(QJsonDocument::Compact));
        }
        if (action == QStringLiteral("tasks.v2.Task.get"))
            return ajaxResult(R"({"id":42,"description":"Сверить схему &amp; уведомления","descriptionChecksum":"fixture-checksum",
                "rights":{"edit":true,"deadline":true}})");
        if (editableTaskFixture && (action == QStringLiteral("tasks.v2.Task.Stakeholder.Auditor.add")
            || action == QStringLiteral("tasks.v2.Task.Stakeholder.Auditor.delete"))) {
            const auto payload = QJsonDocument::fromJson(request.body).object().value("task").toObject();
            for (const auto auditor : payload.value("auditors").toArray()) {
                const auto personId = auditor.toObject().value("id").toInteger();
                if (action == QStringLiteral("tasks.v2.Task.Stakeholder.Auditor.add"))
                    taskAuditorIds.append(personId);
                else
                    taskAuditorIds.removeAll(personId);
            }
            return ajaxResult(R"({"id":42})");
        }
        if (action == QStringLiteral("tasks.v2.Task.Stakeholder.Creator.update")
            || action == QStringLiteral("tasks.v2.Task.Stakeholder.Responsible.delegate")
            || action == QStringLiteral("tasks.v2.Task.Status.start")
            || action == QStringLiteral("tasks.v2.Task.Status.complete"))
            return ajaxResult(R"({"id":42})");
        if (action == QStringLiteral("moveStage"))
            return ajaxResult("true");
        if (action == QStringLiteral("tasks.v2.Group.Stage.list") && editableTaskFixture)
            return ajaxResult(R"([{"id":70,"title":"Завершение","sort":200,"color":"invalid"},
                {"id":60,"title":"Проверка","sort":100,"color":"00ADF2"}])");
        if (action == QStringLiteral("tasks.v2.Task.add"))
            return ajaxResult(R"({"id":44})");
        if (action == QStringLiteral("socialnetwork.V2.Project.add"))
            return ajaxResult(R"({"id":23})");
        if (action == QStringLiteral("tasks.v2.Task.update"))
            return ajaxResult(R"({"id":42})");
        if (action == QStringLiteral("tasks.v2.Task.Deadline.update") && rejectDeadline)
            return R"({"status":"success","data":{"id":42},"errors":[{"code":"DEADLINE_DENIED"}]})";
        if (action == QStringLiteral("tasks.v2.Task.Description.update")
            && QJsonDocument::fromJson(request.body).object().value("task").toObject().value("description") == QLatin1String("reject"))
            return ajaxResult("false");
        if (action == QStringLiteral("tasks.v2.Task.Description.update") || action == QStringLiteral("tasks.v2.Task.Deadline.update"))
            return ajaxResult(R"({"id":42})");
        if (action == QStringLiteral("moveTask"))
            return ajaxResult(kanbanMoveResult);
        if (action == QStringLiteral("modifyColumn") || action == QStringLiteral("moveColumn"))
            return ajaxResult(R"({"id":60})");
        if (action == QStringLiteral("im.v2.Chat.load")) {
            if (!recentHistoryFixture.isEmpty() && request.field(QStringLiteral("dialogId")) == QLatin1String("chat86762"))
                return ajaxResult(recentHistoryFixture);
            if (chatActionsFixture) {
                QJsonObject counts{{"like", 2}};
                QJsonArray own{QStringLiteral("like")};
                if (completeReactionCatalog) {
                    for (const auto &choice : ui::reactionChoices)
                        counts.insert(QString::fromLatin1(choice.code), 2);
                    for (char suffix = 'a'; suffix <= 'j'; ++suffix)
                        own.append(QStringLiteral("fixtureReaction") + QLatin1Char(suffix));
                }
                if (!addedReaction.isEmpty()) {
                    counts.insert(addedReaction, 1);
                    own.append(addedReaction);
                }
                QJsonArray messages{QJsonObject{{"id", 701}, {"senderId", 11}, {"text", "Привет"}}};
                if (taskSharedInSource)
                    messages.append(QJsonObject{{"id", 702}, {"senderId", 0}, {"text",
                        "[URL=/company/personal/user/11/tasks/task/view/42/]Задача из сообщения[/URL]"}});
                return ajaxResult(QJsonDocument(QJsonObject{{"chatId", 91},
                    {"messages", messages},
                    {"reactions", QJsonArray{QJsonObject{{"messageId", 701}, {"reactionCounters", counts},
                        {"ownReactions", own}}}}}).toJson(QJsonDocument::Compact));
            }
            if (presencePinsFixture) {
                if (request.field(QStringLiteral("dialogId")) == QLatin1String("103"))
                    return ajaxResult(R"({"chat":{"id":91,"dialogId":"103"},"messages":[{"id":5,"authorId":103,"text":"Обычное"}],
                        "pins":[{"messageId":12},{"messageId":8}],
                        "additionalMessages":[{"id":12,"authorId":103,"text":"Первое важное"},{"id":8,"authorId":11,"text":"Второе важное"}],
                        "users":[{"id":103,"name":"Сотрудник","workPosition":"Ведущий инженер"}]})");
                QJsonArray pins;
                for (int id = 100; id >= 51; --id)
                    pins.append(QJsonObject{{"messageId", id}});
                return ajaxResult(QJsonDocument(QJsonObject{{"chat", QJsonObject{{"id", 73}, {"dialogId", "chat73"}}},
                    {"messages", QJsonArray{}}, {"pins", pins}}).toJson(QJsonDocument::Compact));
            }
            if (replyPreviewFixture) {
                if (request.field(QStringLiteral("dialogId")) == QStringLiteral("chat92"))
                    return ajaxResult(R"({"chatId":92,"messages":[{"id":720,"chatId":92,"authorId":22,
                        "text":"Другой чат","replyId":690}]})");
                if (omitReplySource)
                    return ajaxResult(R"({"chatId":91,"messages":[{"id":710,"chatId":91,"authorId":11,
                        "text":"Ответ","params":{"replyId":690}}]})");
                return ajaxResult(R"({"chatId":91,"messages":[{"id":710,"chatId":91,"authorId":11,
                    "text":"Ответ","params":{"replyId":"690"}},{"id":711,"chatId":91,"authorId":22,
                    "text":"Другая цитата","replyId":691}],"additionalMessages":{
                        "690":{"chatId":91,"authorId":22,"text":"","params":{"FILE_ID":[401]}},
                        "691":{"id":691,"chatId":92,"authorId":22,"text":"Чужая переписка"}},
                    "files":{"401":{"name":"Снимок.jpg","type":"image","urlDownload":"/disk/download.xlsx"}},
                    "usersShort":[{"id":22,"name":"Автор снимка"}]})");
            }
            if (richMessagesFixture)
                return ajaxResult(R"({"messages":[{"id":701,"senderId":11,"text":"Мой ответ","replyId":690,"files":[3]},
                    {"id":702,"senderId":22,"text":"Переслано","forward":{"id":"689","userId":33}},
                    {"id":703,"authorId":0,"text":"Задача создана"},{"id":704,"text":"Нет отправителя"}],
                    "additionalMessages":[{"id":690,"senderId":22,"text":"Исходная цитата","files":[3,4]}],
                    "files":[{"id":3,"name":"Расчёт.xlsx","size":"9","urlDownload":"/disk/download.xlsx"},
                       {"id":4,"name":"Без ссылки.pdf","size":200,"urlDownload":"file:/external/private.pdf"}],
                    "usersShort":[{"id":22,"name":"Автор ответа","avatar":"/upload/author.png"},{"id":33,"name":"Источник"}]})");
            if (unreadHistoryFixture)
                return ajaxResult(R"({"chats":[{"id":91,"dialogId":"chat91","lastId":700}],"messages":[
                   {"id":700,"authorId":22,"text":"Прочитанное"},{"id":701,"authorId":22,"text":"Новое"},
                   {"id":702,"authorId":11,"text":"Исходящее","unread":true},
                   {"id":703,"authorId":22,"text":"Прочитанное отдельно","unread":false}]})");
            if (request.field(QStringLiteral("dialogId")) == QStringLiteral("chat73"))
                return ajaxResult(R"({"chat":{"id":73}})");
            return ajaxResult(R"({"messages":[{"id":701,"authorId":11,"text":"Привет"}]})");
        }
        if (action == QStringLiteral("im.v2.Chat.Pin.tail") && presencePinsFixture)
            return ajaxResult(R"({"pins":[{"messageId":50}],"hasNextPage":false,
                "additionalMessages":[{"id":50,"authorId":22,"text":"Закрепление из следующей страницы"}]})");
        if (action == QStringLiteral("im.v2.Chat.Message.list")) {
            return ajaxResult(R"({"messages":[{"id":800,"authorId":22,"text":"Чат задачи"}],
                "users":[{"id":22,"name":"Мария"}]})");
        }
        if (action == QStringLiteral("im.v2.Chat.Message.send"))
            return ajaxResult(R"({"id":801})");
        return {};
    }

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

} // namespace

class TestCloudBackend : public QObject {
    Q_OBJECT

private slots:
    void searchesPeopleWithinThePortalScope();
    void loadsInitialTaskPeopleInPortalRecentOrder();
    void confirmsPortalDndAndPreservesExternalStatus();
    void settlesInitialStatusBeforeShowingDesktopAlerts_data();
    void settlesInitialStatusBeforeShowingDesktopAlerts();
    void initTestCase();
    void loadsProjectTaskAndTaskChat();
    void refreshesTaskChatAfterProjectChange_data();
    void refreshesTaskChatAfterProjectChange();
    void loadsOlderHistoryWithScopedCursor();
    void readsAndWritesTaskChecklistAndChildRelations();
    void readsScopedGanttLinksAndExactPlanDates();
    void readsTaskHistoryAndKeepsDenialDistinctFromEmpty();
    void readsScopedProjectMembersAndDiskFiles();
    void readsOrdinaryChatAndNotificationHistory();
    void loadsOlderNotificationsOnDemandAndKeepsTheCursor();
    void advancesNotificationCursorAcrossTypesAndStopsOnRepeat();
    void retriesChatHistoryWithTheKnownChatId_data();
    void retriesChatHistoryWithTheKnownChatId();
    void recoversLatestHistoryAfterATemporaryFailure_data();
    void recoversLatestHistoryAfterATemporaryFailure();
    void coalescesManualHistoryRefreshWhileLatestReadIsInFlight();
    void reconcilesUnreadFromScopedActiveHistory();
    void ignoresParticipantCountsInUnreadBadges();
    void pagesProjectTaskChatsAndTracksOnlyTheirConfirmedBadges();
    void aggregatesUnreadProjectTasksWithoutOpeningTheirChatList();
    void searchesAndMutatesNotificationsOverTheSession();
    void decodesRichNotificationsFromHistoryAndPush();
    void resolvesNoticeAuthorFromMatchingUserOnly();
    void linksTaskChatEventsAndMarksSyntheticRowsLocally();
    void classifiesOrdinaryChatPushesWithoutSuppressingPopups();
    void marksExactNoticeIdsAsReadAfterBatchConfirmation();
    void ignoresNotificationRepliesFromPreviousAccount_data();
    void ignoresNotificationRepliesFromPreviousAccount();
    void reloadsNotificationSettingsAfterPortalUpdate();
    void notificationActionsKeepPushNewerThanTheRequest();
    void notificationSettingSnapshotsKeepNewerPush();
    void distinguishesPersonalDialogAndTaskWithSameId();
    void refreshesOnlyActiveViewsAndKeepsInflightInvalidations();
    void receivesInBackgroundAndResumesAfterDisconnect();
    void learnsRotatedCredentialsFromAcceptedPortalReplies();
    void recognizesExplicitAuthorizationRejection();
    void scopesAndCoalescesEndpointAuthorizationRejections();
    void refreshesCsrfWithoutRepeatingAnUnconfirmedWrite_data();
    void refreshesCsrfWithoutRepeatingAnUnconfirmedWrite();
    void limitsCsrfRefreshAttempts_data();
    void limitsCsrfRefreshAttempts();
    void refreshesSavedCsrfBeforeOpeningWorkspace_data();
    void refreshesSavedCsrfBeforeOpeningWorkspace();
    void recoversOnlyTheSameAccount();
    void catchesUpWhenAnActiveChatStopsReceivingPush();
    void refreshesVisibleInactiveHistory_data();
    void refreshesVisibleInactiveHistory();
    void desktopBannersCatchUpFromHistory_data();
    void desktopBannersCatchUpFromHistory();
    void refusesToFollowRedirectWithSession();
    void sendsTaskEditsAndKanbanMutations();
    void confirmedKanbanMoveSupersedesOutstandingBoardRead();
    void readsAndChangesStageColorWithoutRenaming();
    void loadsTaskRequisitesAndUsesVerifiedWriteActions();
    void discardsTaskProjectAndStageRepliesFromPreviousAccount_data();
    void discardsTaskProjectAndStageRepliesFromPreviousAccount();
    void loadsExactTaskRatingAndExplicitActionRights();
    void mutatesTaskRatingAndDeletionOnlyAfterExactPortalConfirmation();
    void ignoresTaskActionResponsesFromPreviousAccounts();
    void loadsObserverAvatarsWithoutIndividualProfileRequests();
    void loadsObserverAvatarsWithoutIndividualProfileRequests_data();
    void preservesRawTaskDescriptionAndRejectsAStaleEdit();
    void findsProjectTasksOutsideInitialBoard();
    void loadsBoardColumnsLazilyWithoutSkippingFirstPage();
    void discardsBoardPagesFromPreviousSnapshots();
    void refusesToDeleteStageWithoutConfirmedEmptyTotal();
    void resolvesCurrentCreationOwnerWithoutRecentIdentity();
    void createsTasksWithSelectedRequisites();
    void derivesUnreadAndRefreshesOnReadEvents();
    void gatesChatPopupsBySoundAndMessageSettings();
    void chatPopupsRespectExplicitDesktopDelivery_data();
    void chatPopupsRespectExplicitDesktopDelivery();
    void refreshesDesktopDeliveryWhenReturningToSimpleScheme_data();
    void refreshesDesktopDeliveryWhenReturningToSimpleScheme();
    void deliversChatPopupsWithExpertSettingsFromPush();
    void separatesChatScopesAndUsesPortalMetadata();
    void searchesTaskChatsByAccessibleTaskTitleAndNumber();
    void usesPortalPinActionsAndTheirAcknowledgements();
    void readsPortalTaskProjectsGridAndKeepsPinsAcrossPageLoads_data();
    void readsPortalTaskProjectsGridAndKeepsPinsAcrossPageLoads();
    void fallsBackToPaginatedProjectMembership();
    void keepsLegacyProjectScopeEvenWhenLaterPagesHaveRoles();
    void loadsPersonalPresenceAndPinnedMessagesWithLiveChanges();
    void usesSenderIdentityAndDecodesForwardAndQuoteMetadata();
    void streamsOnlyRegisteredMessageFilesAndProtectsTargetOnMismatch();
    void refusingUnsafeDownloadTargetsLeavesLinkDestinationUntouched();
    void projectsStillLoadWhenOptionalIdentityIsUnavailable();
    void marksOnlyDismissedPersonalChatsAndUsesOneHydration();
    void loadsProjectsDuringAnOptionalSettingsIdentityLookup();
    void immediatelyUpdatesLiveRecentAndProtectsItFromOldSnapshots();
    void loadsAuthoritativeUnreadCountsForBothChatSections();
    void totalsUnreadChatsOutsideRecentAndTracksPush();
    void deduplicatesRecentFallbackAndKeepsPushNewerThanTheSnapshot();
    void guardsRecentPreviewMessageScope();
    void loadsAndClearsPersonalAnchorsIndependentlyOfUnreadCounts();
    void marksOnlyTheVisibleDialogAfterConfirmedRead();
    void acknowledgesVisibleReactionAnchorsWithoutLosingNewPush();
    void exactReadLeavesOlderUnseenMessageEligible();
    void readsExplicitTaskUnreadBeforeNewerSystemCursor();
    void keepsAllExplicitUnreadRowsInTheNewlyLoadedOlderPage();
    void readsTaskCardOpenedOutsideProjects();
    void loadsAuthorizedTaskSummaryWhileDetailsArePending();
    void taskCardRequiresTheMatchingLegacyResponse_data();
    void taskCardRequiresTheMatchingLegacyResponse();
    void discardsParallelTaskCardFromPreviousAccount();
    void restoresReplyFromKeyedPhotoAndKeepsExactScope();
    void sendsOriginalReplyIdInBothChatScopes();
    void performsVerifiedMessageActions();
    void createsTaskWithOneNativeShareAndReloadsTheSource();
    void refreshesConfirmedReactions();
    void decodesFullReactionCatalog();
    void createsPrivateGroupAndChangesMembers();
    void loadsBoundedScopedMediaAndLinks();
    void sendsAndReceivesScopedTypingActivity();

private:
    QTemporaryDir m_settingsPath;
};

void TestCloudBackend::confirmsPortalDndAndPreservesExternalStatus()
{
    CloudFixture fixture;
    fixture.portalStatusFixture = true;
    fixture.liveRecentFixture = true;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy loaded(&backend, &Backend::portalStatusLoaded);
    QSignalSpy updated(&backend, &Backend::portalStatusUpdated);
    QSignalSpy errors(&backend, &Backend::failed);
    QSignalSpy connected(&backend, &Backend::connectionChanged);
    QSignalSpy recent(&backend, &Backend::recentChatsLoaded);
    QSignalSpy feed(&backend, &Backend::notificationsLoaded);
    QSignalSpy bubbles(&backend, &CloudBackend::notificationReceived);
    backend.start();
    backend.requestPortalStatus();
    backend.requestRecentChats();
    QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty(), 1500);
    QCOMPARE(loaded.last().first().toString(), QStringLiteral("online"));
    fixture.portalStatus = QStringLiteral("away");
    backend.updatePortalStatus(QStringLiteral("online"), QStringLiteral("dnd"));
    QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 1500);
    QCOMPARE(loaded.last().first().toString(), QStringLiteral("away"));
    const auto set = QStringLiteral("/rest/im.user.status.set.json");
    QVERIFY(fixture.requestsFor(set).isEmpty());

    backend.updatePortalStatus(QStringLiteral("away"), QStringLiteral("dnd"));
    QTRY_COMPARE_WITH_TIMEOUT(updated.size(), 1, 1500);
    QCOMPARE(updated.last().first().toString(), QStringLiteral("dnd"));
    QCOMPARE(fixture.requestsFor(set).last().field(QStringLiteral("STATUS")), QStringLiteral("dnd"));
    QTRY_COMPARE(connected.size(), 1);
    QTRY_VERIFY(!recent.isEmpty());
    fixture.pushIm(QStringLiteral("while-dnd"), QStringLiteral("notifyAdd"), {{"id", 999}, {"text", "quiet"}});
    QTRY_COMPARE(feed.size(), 1);
    QVERIFY(bubbles.isEmpty());
    backend.updatePortalStatus(QStringLiteral("dnd"), QStringLiteral("break"));
    QTRY_COMPARE_WITH_TIMEOUT(updated.size(), 2, 1500);
    QCOMPARE(updated.last().first().toString(), QStringLiteral("break"));
    QCOMPARE(fixture.requestsFor(set).last().field(QStringLiteral("STATUS")), QStringLiteral("break"));
    QVERIFY(fixture.requestsFor(QStringLiteral("/rest/im.user.status.get.json")).size() >= 6);
    const auto pushStatus = [&fixture](const QString &mid, const QString &status) {
        const QJsonObject user{{"id", 11}, {"status", status}};
        const QJsonObject users{{"11", user}};
        fixture.pushIm(mid, QStringLiteral("userStatus"), {{"users", users}}, 0, QStringLiteral("online"));
    };
    pushStatus(QStringLiteral("external-dnd"), QStringLiteral("dnd"));
    QTRY_COMPARE(loaded.last().first().toString(), QStringLiteral("dnd"));
    fixture.pushIm(QStringLiteral("still-dnd"), QStringLiteral("notifyAdd"), {{"id", 998}, {"text", "still quiet"}});
    QTRY_COMPARE(feed.size(), 2);
    QVERIFY(bubbles.isEmpty());
    pushStatus(QStringLiteral("external-online"), QStringLiteral("online"));
    QTRY_COMPARE(loaded.last().first().toString(), QStringLiteral("online"));
    fixture.pushIm(QStringLiteral("after-dnd"), QStringLiteral("notifyAdd"), {{"id", 997}, {"text", "visible popup"}});
    QTRY_COMPARE(bubbles.size(), 1);
}

void TestCloudBackend::settlesInitialStatusBeforeShowingDesktopAlerts_data()
{
    QTest::addColumn<QString>("status");
    QTest::addColumn<bool>("popup");
    QTest::addColumn<bool>("pushBeforeIdentity");
    QTest::newRow("confirmed DND") << QStringLiteral("dnd") << false << false;
    QTest::newRow("confirmed online") << QStringLiteral("online") << true << false;
    QTest::newRow("unsupported status") << QString() << true << false;
    QTest::newRow("DND Push before identity") << QStringLiteral("online") << false << true;
}

void TestCloudBackend::settlesInitialStatusBeforeShowingDesktopAlerts()
{
    QFETCH(QString, status);
    QFETCH(bool, popup);
    QFETCH(bool, pushBeforeIdentity);
    CloudFixture fixture;
    fixture.holdNextOnlineResponse = true;
    fixture.messengerPage = QStringLiteral(R"html(<script>
BX.Messenger.v2.Application.Launch('messenger', {"currentUser":{"id":11},"settings":{"status":"%1"},"counters":[],"anchors":[]});
</script>)html").arg(status).toUtf8();
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy connected(&backend, &Backend::connectionChanged);
    QSignalSpy statusReceived(&backend, &Backend::portalStatusLoaded);
    QSignalSpy feed(&backend, &Backend::notificationsLoaded);
    QSignalSpy bubbles(&backend, &CloudBackend::notificationReceived);
    backend.start();
    QTRY_COMPARE(connected.size(), 1);
    QTRY_VERIFY(!fixture.holdNextOnlineResponse);
    if (pushBeforeIdentity) {
        const QJsonObject current{{"id", 11}, {"status", "dnd"}};
        const QJsonObject other{{"id", 22}, {"status", "online"}};
        fixture.pushIm(QStringLiteral("status-before-identity"), QStringLiteral("userStatus"),
                       {{"users", QJsonObject{{"11", current}, {"22", other}}}}, 0, QStringLiteral("online"));
        const QJsonObject stale{{"id", 11}, {"status", "online"}};
        fixture.pushIm(QStringLiteral("older-status-before-identity"), QStringLiteral("userStatus"),
                       {{"users", QJsonObject{{"11", stale}}}}, 30, QStringLiteral("online"));
    }
    fixture.pushIm(QStringLiteral("during-bootstrap"), QStringLiteral("notifyAdd"), {{"id", 900}, {"text", "held status"}});
    QTRY_COMPARE(feed.size(), 1);
    QVERIFY(bubbles.isEmpty());
    fixture.releaseHeldOnlineResponse();
    if (!status.isEmpty()) {
        QTRY_COMPARE(statusReceived.size(), 1);
        QCOMPARE(statusReceived.first().first().toString(), pushBeforeIdentity ? QStringLiteral("dnd") : status);
    }
    if (popup)
        QTRY_COMPARE(bubbles.size(), 1);
    else
        QVERIFY(bubbles.isEmpty());
    fixture.pushIm(QStringLiteral("after-bootstrap"), QStringLiteral("notifyAdd"), {{"id", 901}, {"text", "settled status"}});
    QTRY_COMPARE(feed.size(), 2);
    QCOMPARE(bubbles.size(), popup ? 2 : 0);
}

void TestCloudBackend::loadsProjectsDuringAnOptionalSettingsIdentityLookup()
{
    CloudFixture fixture;
    fixture.dismissedChatsFixture = true;
    fixture.omitRecentUser = true;
    fixture.holdNextIdentity = true;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy projects(&backend, &Backend::projectsLoaded);
    backend.start();
    backend.requestRecentChats();
    const auto identity = QStringLiteral("main.bitrix.main.controller.loadext.resolveextensions");
    QTRY_COMPARE(fixture.requestsFor(identity).size(), 1);
    backend.requestProjects();
    QTRY_COMPARE_WITH_TIMEOUT(projects.size(), 1, 1200);
    fixture.releaseHeldIdentity();
}

void TestCloudBackend::loadsAuthoritativeUnreadCountsForBothChatSections()
{
    CloudFixture fixture;
    fixture.liveRecentFixture = true;
    fixture.chatBadgesFixture = true;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy ordinary(&backend, &Backend::recentChatsLoaded);
    QSignalSpy tasks(&backend, &Backend::taskChatsLoaded);
    backend.start();
    backend.requestRecentChats();
    backend.requestTaskChats();
    const auto unread = [](const QSignalSpy &spy) {
        return spy.isEmpty() ? -1 : spy.last().first().value<QVector<Chat>>().value(0).unread;
    };
    QTRY_COMPARE_WITH_TIMEOUT(unread(ordinary), 127, 1200);
    QTRY_COMPARE_WITH_TIMEOUT(unread(tasks), 7, 1200);
    QCOMPARE(fixture.requestsFor(QStringLiteral("im.v2.Counter.get")).size(), 1);
}

void TestCloudBackend::totalsUnreadChatsOutsideRecentAndTracksPush()
{
    CloudFixture fixture;
    fixture.messengerPage = R"HTML(<script>
        BX.Messenger.v2.Application.Launch('messenger', {"currentUser":{"id":11},"counters":[
            {"chatId":91,"counter":127},{"chatId":73,"counter":7,"isMuted":true},
            {"chatId":88,"counter":2},{"chatId":777,"counter":0,"isMarkedAsUnread":true}]});
        </script>)HTML";
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    backend.setForeground(false);
    QSignalSpy online(&backend, &Backend::connectionChanged);
    QSignalSpy counts(&backend, &Backend::unreadChatCountChanged);
    backend.start();
    QTRY_COMPARE(online.size(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(backend.unreadChatCount().value_or(-1), 137, 1200);
    QCOMPARE(counts.last().at(0).toLongLong(), 137);

    fixture.pushIm(QStringLiteral("read-off-page"), QStringLiteral("readMessageChat"),
                   {{"chatId", 88}, {"lastId", 800}, {"counter", 0}});
    QTRY_COMPARE(backend.unreadChatCount().value_or(-1), 135);
    fixture.pushIm(QStringLiteral("unmark-off-page"), QStringLiteral("chatUnread"),
                   {{"chatId", 777}, {"active", false}});
    QTRY_COMPARE(backend.unreadChatCount().value_or(-1), 134);
    fixture.pushIm(QStringLiteral("incoming-off-page"), QStringLiteral("messageChat"),
                   {{"chatId", 91}, {"counter", 130},
                    {"message", QJsonObject{{"id", 900}, {"senderId", 22}, {"text", "New message"}}}});
    QTRY_COMPARE(backend.unreadChatCount().value_or(-1), 137);
    fixture.pushIm(QStringLiteral("mark-zero"), QStringLiteral("chatUnread"),
                   {{"chatId", 999}, {"active", true}});
    QTRY_COMPARE(backend.unreadChatCount().value_or(-1), 138);

    QCOMPARE(fixture.requestsFor(QStringLiteral("im.v2.Recent.load")).size(), 0);
    QCOMPARE(fixture.requestsFor(QStringLiteral("im.v2.Counter.get")).size(), 0);
    backend.stop();
    QCOMPARE(backend.unreadChatCount().value_or(-1), 0);
    QCOMPARE(counts.last().at(0).toLongLong(), 0);
}

void TestCloudBackend::deduplicatesRecentFallbackAndKeepsPushNewerThanTheSnapshot()
{
    CloudFixture fixture;
    fixture.holdNextOnlineResponse = true;
    fixture.ordinaryRecentBadgeFixture = R"({"userId":11,
        "recentItems":[{"dialogId":"201","chatId":91,"counter":3}],
        "chats":[{"id":91,"dialogId":"201","type":"user","name":"Personal"}]})";
    fixture.taskRecentBadgeFixture = R"({"userId":11,
        "recentItems":[{"dialogId":"chat91","chatId":91,"counter":3},{"dialogId":"chat73","chatId":73,"unread":true}],
        "chats":[{"id":91,"dialogId":"chat91","type":"tasksTask","name":"Duplicate ID"},
                 {"id":73,"dialogId":"chat73","type":"tasksTask","name":"Marked task"}]})";
    fixture.messengerPage = R"HTML(<script>
        BX.Messenger.v2.Application.Launch('messenger', {"currentUser":{"id":11},"counters":[
            {"chatId":91,"counter":100},{"chatId":73,"counter":2}]});
        </script>)HTML";
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    backend.setForeground(false);
    QSignalSpy online(&backend, &Backend::connectionChanged);
    backend.start();
    backend.requestRecentChats();
    backend.requestTaskChats();
    QTRY_COMPARE(online.size(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(backend.unreadChatCount().value_or(-1), 4, 1200);

    fixture.pushIm(QStringLiteral("personal-push"), QStringLiteral("message"),
                   {{"dialogId", "201"}, {"chatId", 91}, {"counter", 5},
                    {"message", QJsonObject{{"id", 900}, {"senderId", 22}, {"text", "New message"}}}});
    QTRY_COMPARE(backend.unreadChatCount().value_or(-1), 6);
    fixture.releaseHeldOnlineResponse();
    QTRY_COMPARE(backend.unreadChatCount().value_or(-1), 7);
    QCOMPARE(fixture.requestsFor(QStringLiteral("im.v2.Counter.get")).size(), 0);
}

void TestCloudBackend::guardsRecentPreviewMessageScope()
{
    CloudFixture fixture;
    fixture.ordinaryRecentBadgeFixture = R"({"userId":11,
        "recentItems":[
            {"dialogId":"chat86761","chatId":86761,"messageId":5380997,"lastReadMessageId":5412589,
             "dateLastActivity":"2026-11-23T11:54:00+03:00"},
            {"dialogId":"chat153707","chatId":153707,"messageId":5380997},
            {"dialogId":"chat86762","messageId":5380997},
            {"dialogId":"chat86763","chatId":86763,"message":{"id":5380997,"chatId":153707,"text":"Foreign inline"}},
            {"dialogId":"chat86764","chatId":86764,"messageId":5412592}],
        "chats":[{"id":86761,"dialogId":"chat86761","type":"collab","name":"Affected group"}],
        "messages":[{"id":5412589,"chatId":86761,"text":"Actual latest","date":"2026-11-23T11:18:00+03:00"},
                    {"id":5380997,"chatId":153707,"text":"Other group content","date":"2026-11-16T18:13:57+03:00"},
                    {"id":5412588,"chatId":86761,"text":"Older same-chat message"},
                    {"id":5412591,"chatId":86763,"chat_id":153707,"text":"Conflicting aliases"},
                    {"id":5412592,"text":"Legacy preview"}]})";
    fixture.recentHistoryFixture = R"({"chatId":86762,"messages":[
        {"id":5412593,"chatId":86762,"authorId":22,"text":"Restored from open history","date":"2026-11-23T10:21:00+03:00"},
        {"id":5412594,"chatId":153707,"authorId":22,"text":"Foreign history row"}]})";
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    backend.setForeground(false);
    QSignalSpy recent(&backend, &Backend::recentChatsLoaded);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    QSignalSpy online(&backend, &Backend::connectionChanged);
    backend.start();
    backend.requestRecentChats();
    QTRY_VERIFY(!recent.isEmpty());
    QCOMPARE(recent.last().first().value<QVector<Chat>>().size(), 5);
    const auto chat = [&recent](const QString &id) {
        const auto chats = recent.last().first().value<QVector<Chat>>();
        for (const auto &entry : chats)
            if (entry.id == id)
                return entry;
        return Chat{};
    };
    QCOMPARE(chat(QStringLiteral("chat86761")).lastMessage, QStringLiteral("Actual latest"));
    QCOMPARE(chat(QStringLiteral("chat86761")).updated,
             QDateTime::fromString(QStringLiteral("2026-11-23T11:18:00+03:00"), Qt::ISODate));
    QCOMPARE(chat(QStringLiteral("chat153707")).lastMessage, QStringLiteral("Other group content"));
    QVERIFY(chat(QStringLiteral("chat86762")).lastMessage.isEmpty());
    QVERIFY(chat(QStringLiteral("chat86763")).lastMessage.isEmpty());
    QCOMPARE(chat(QStringLiteral("chat86764")).lastMessage, QStringLiteral("Legacy preview"));
    QCOMPARE(fixture.requestsFor(QStringLiteral("im.v2.Recent.load")).size(), 1);
    QVERIFY(fixture.requestsFor(QStringLiteral("im.v2.Chat.load")).isEmpty());
    QVERIFY(fixture.requestsFor(QStringLiteral("im.v2.Chat.Message.list")).isEmpty());

    backend.requestChatMessages(QStringLiteral("chat86762"));
    QTRY_VERIFY(!history.isEmpty());
    QCOMPARE(chat(QStringLiteral("chat86762")).lastMessage, QStringLiteral("Restored from open history"));
    QCOMPARE(fixture.requestsFor(QStringLiteral("im.v2.Chat.load")).size(), 1);
    QTRY_VERIFY(!online.isEmpty());
    fixture.pushIm(QStringLiteral("foreign-recent-message"), QStringLiteral("messageChat"),
                   {{"dialogId", "chat86761"}, {"chatId", 86761}, {"counter", 1},
                    {"message", QJsonObject{{"id", 5412595}, {"chatId", 153707}, {"senderId", 22},
                                             {"text", "Foreign push"}}}});
    fixture.pushIm(QStringLiteral("valid-recent-message"), QStringLiteral("messageChat"),
                   {{"dialogId", "chat86761"}, {"chatId", 86761}, {"counter", 1},
                    {"message", QJsonObject{{"id", 5412595}, {"chatId", 86761}, {"senderId", 22},
                                             {"text", "Valid push"}}}});
    QTRY_COMPARE(chat(QStringLiteral("chat86761")).lastMessage, QStringLiteral("Valid push"));
}

void TestCloudBackend::loadsAndClearsPersonalAnchorsIndependentlyOfUnreadCounts()
{
    CloudFixture fixture;
    fixture.liveRecentFixture = true;
    fixture.chatBadgesFixture = true;
    fixture.messengerPage = R"HTML(<!doctype html>
        <div data-example='<script>
            BX.Messenger.v2.Application.Launch("messenger", {"counters":[],"anchors":[]});
        </script>'></div>
        <script>BX.ready(function(){
            BX.Messenger.v2.Application.Launch('messenger', {
                "unrelatedNested":{"text":"} \"ignored\" {"},
                "currentUser":{"id":11},
                "counters":[{"chatId":91,"counter":127},{"chatId":73,"counter":7,"isMuted":true}],
                "anchors":[{"chatId":91,"messageId":801,"type":"MENTION","fromUserId":17,"userId":11},
                    {"chatId":91,"messageId":801,"type":"MENTION","fromUserId":27,"userId":11},
                    {"chatId":73,"messageId":803,"type":"REACTION","fromUserId":26,"userId":11}]
            }).then(() => {});
        });</script>)HTML";
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    backend.setForeground(false);
    QSignalSpy ordinary(&backend, &Backend::recentChatsLoaded);
    QSignalSpy tasks(&backend, &Backend::taskChatsLoaded);
    QSignalSpy connected(&backend, &Backend::connectionChanged);
    backend.start();
    backend.requestRecentChats();
    backend.requestTaskChats();
    const auto row = [](const QSignalSpy &spy) {
        return spy.isEmpty() ? Chat{} : spy.last().first().value<QVector<Chat>>().value(0);
    };
    QTRY_VERIFY(row(ordinary).hasMention);
    QTRY_VERIFY(row(tasks).hasReaction);
    QCOMPARE(row(ordinary).unread, 127);
    QCOMPARE(row(tasks).unread, 7);
    QVERIFY(!row(ordinary).hasReaction);
    QVERIFY(!row(tasks).hasMention);
    QVERIFY(fixture.requestsFor(QStringLiteral("im.v2.Counter.get")).isEmpty());
    QTRY_COMPARE(connected.size(), 1);

    fixture.pushIm(QStringLiteral("foreign-anchor"), QStringLiteral("addAnchor"),
                   {{"chatId", 73}, {"messageId", 804}, {"type", "mention"}, {"fromUserId", 17}, {"userId", 22}});
    fixture.pushIm(QStringLiteral("foreign-reaction"), QStringLiteral("reaction"),
                   {{"chatId", 91}, {"messageId", 802}, {"reaction", "like"}, {"actualReactions", 5}});
    fixture.pushIm(QStringLiteral("own-reaction"), QStringLiteral("addAnchor"),
                   {{"chatId", 91}, {"messageId", 802}, {"type", "reaction"}, {"fromUserId", 17}});
    QTRY_VERIFY(row(ordinary).hasReaction && row(ordinary).hasMention);
    QVERIFY(!row(tasks).hasMention);

    fixture.pushIm(QStringLiteral("one-mention-cleared"), QStringLiteral("deleteAnchor"),
                   {{"chatId", 91}, {"messageId", 801}, {"type", "mention"}, {"fromUserId", 17}});
    fixture.pushIm(QStringLiteral("task-anchors-cleared"), QStringLiteral("deleteChatAnchors"), {{"chatId", 73}});
    QTRY_VERIFY(!row(tasks).hasReaction);
    QVERIFY(row(ordinary).hasMention);
    QCOMPARE(row(tasks).unread, 7);
    fixture.pushIm(QStringLiteral("remaining-anchor-cleared"), QStringLiteral("deleteAnchor"),
                   {{"chatId", 91}, {"messageId", 801}, {"type", "mention"}, {"fromUserId", 27}});
    QTRY_VERIFY(!row(ordinary).hasMention);
    QVERIFY(row(ordinary).hasReaction);
    QCOMPARE(row(ordinary).unread, 127);

    fixture.pushIm(QStringLiteral("task-read"), QStringLiteral("readMessageChat"),
                   {{"chatId", 73}, {"dialogId", "chat73"}, {"lastId", 803}, {"counter", 0}});
    QTRY_COMPARE(row(tasks).unread, 0);
    QCOMPARE(row(ordinary).unread, 127);
}

void TestCloudBackend::marksOnlyTheVisibleDialogAfterConfirmedRead()
{
    CloudFixture fixture;
    fixture.chatBadgesFixture = true;
    fixture.liveRecentFixture = true;
    fixture.messengerPage = R"HTML(<script>
        BX.Messenger.v2.Application.Launch('messenger', {
            "currentUser":{"id":11},
            "counters":[{"chatId":91,"counter":2,"isMarkedAsUnread":true}],
            "anchors":[{"chatId":91,"messageId":803,"userId":11,"fromUserId":22,"type":"reaction"},
                {"chatId":91,"messageId":805,"userId":11,"fromUserId":22,"type":"mention"}]
        });
    </script>)HTML";
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy list(&backend, &Backend::recentChatsLoaded);
    QSignalSpy acknowledgements(&backend, &Backend::chatReadConfirmed);
    QSignalSpy connection(&backend, &Backend::connectionChanged);
    backend.start();
    backend.requestRecentChats();
    backend.requestChatMessages(QStringLiteral("chat91"));
    const auto dialog = QStringLiteral("chat91");
    const auto read = QStringLiteral("im.v2.Chat.Message.read");
    const auto anchors = QStringLiteral("im.v2.Anchor.read");
    const QStringList incoming{QStringLiteral("801"), QStringLiteral("804")};
    const QStringList visible{QStringLiteral("801"), QStringLiteral("803"), QStringLiteral("804")};
    const auto row = [&list] {
        return list.isEmpty() ? Chat{} : list.last().first().value<QVector<Chat>>().value(0);
    };
    QTRY_COMPARE(row().unread, 2);
    QCOMPARE(backend.unreadChatCount().value_or(-1), 2);
    QVERIFY(row().hasMention && row().hasReaction);

    backend.setForeground(false);
    backend.readChatMessages(dialog, incoming, visible);
    backend.setForeground(true);
    backend.readChatMessages(QStringLiteral("chat92"), incoming, visible);
    QTest::qWait(60);
    QVERIFY(fixture.requestsFor(read).isEmpty());
    QVERIFY(fixture.requestsFor(anchors).isEmpty());

    backend.readChatMessages(dialog, incoming, visible);
    QTRY_COMPARE(fixture.requestsFor(read).size(), 1);
    QTRY_COMPARE(fixture.requestsFor(anchors).size(), 1);
    QTRY_VERIFY(!row().hasReaction);
    QCOMPARE(row().unread, 2);
    QCOMPARE(backend.unreadChatCount().value_or(-1), 2);
    QVERIFY(row().hasMention);
    QVERIFY(acknowledgements.isEmpty());
    const auto sent = fixture.requestsFor(read).first();
    QCOMPARE(sent.field(QStringLiteral("chatId")), QStringLiteral("91"));
    QCOMPARE(sent.field(QStringLiteral("ids[0]")), QStringLiteral("801"));
    QCOMPARE(sent.field(QStringLiteral("ids[1]")), QStringLiteral("804"));
    QVERIFY(!sent.field(QStringLiteral("actionUuid")).isEmpty());
    const auto anchorRequest = fixture.requestsFor(anchors).first();
    QCOMPARE(anchorRequest.field(QStringLiteral("messageIds[0]")), QStringLiteral("803"));
    QVERIFY(anchorRequest.field(QStringLiteral("messageIds[1]")).isEmpty());

    fixture.visibleReadResult = R"({"chatId":"91","counter":"0"})";
    fixture.approveVisibleRead = true;
    backend.readChatMessages(dialog, incoming, visible);
    QTRY_COMPARE(row().unread, 0);
    QCOMPARE(backend.unreadChatCount().value_or(-1), 1);
    QVERIFY(row().hasMention);
    QVERIFY(row().markedUnread);
    QTRY_COMPARE(acknowledgements.size(), 1);
    QCOMPARE(acknowledgements.first().at(0).toString(), dialog);
    QCOMPARE(acknowledgements.first().at(1).toStringList(), incoming);
    QCOMPARE(acknowledgements.first().at(2).toLongLong(), 0);
    QCOMPARE(fixture.requestsFor(read).size(), 2);
    backend.readChatMessages(dialog, incoming, visible);
    QTest::qWait(60);
    QCOMPARE(fixture.requestsFor(read).size(), 2);

    fixture.holdNextReadResponse = read;
    fixture.visibleReadResult = R"({"chatId":91,"counter":0,"lastId":811,"viewedMessages":[810,811]})";
    const QString latestVisible = QStringLiteral("810");
    backend.readChatMessages(dialog, {latestVisible}, {latestVisible});
    QTRY_COMPARE(fixture.requestsFor(read).size(), 3);
    QTRY_VERIFY(!connection.isEmpty());
    fixture.pushIm(QStringLiteral("new-during-read"), QStringLiteral("messageChat"),
                   {{"dialogId", dialog}, {"chatId", 91}, {"counter", 1},
                    {"message", QJsonObject{{"id", 811}, {"senderId", 22}, {"text", "Новое вне экрана"}}}});
    QTRY_COMPARE(row().unread, 1);
    fixture.releaseHeldReadResponse();
    QTRY_COMPARE(acknowledgements.size(), 2);
    QCOMPARE(acknowledgements.last().at(1).toStringList(), QStringList{latestVisible});
    QCOMPARE(acknowledgements.last().at(2).toLongLong(), 0);
    QCOMPARE(row().unread, 1);

    const QString previousOffscreen = QStringLiteral("811");
    const QString olderOffscreen = QStringLiteral("809");
    backend.readChatMessages(dialog, {olderOffscreen, previousOffscreen});
    QTRY_COMPARE(fixture.requestsFor(read).size(), 4);
    const auto readAfterScroll = fixture.requestsFor(read).last();
    QCOMPARE(readAfterScroll.field(QStringLiteral("ids[0]")), olderOffscreen);
    QCOMPARE(readAfterScroll.field(QStringLiteral("ids[1]")), previousOffscreen);
    QTRY_COMPARE(row().unread, 0);
}

void TestCloudBackend::acknowledgesVisibleReactionAnchorsWithoutLosingNewPush()
{
    CloudFixture fixture;
    fixture.liveRecentFixture = true;
    fixture.chatBadgesFixture = true;
    fixture.messengerPage = R"HTML(<script>
        BX.Messenger.v2.Application.Launch('messenger', {
            "currentUser":{"id":11},
            "anchors":[{"chatId":91,"messageId":803,"userId":11,"fromUserId":22,"type":"reaction"}]
        });
    </script>)HTML";
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy list(&backend, &Backend::recentChatsLoaded);
    QSignalSpy connection(&backend, &Backend::connectionChanged);
    QSignalSpy available(&backend, &Backend::chatBadgeAnchorAvailable);
    QSignalSpy popups(&backend, &CloudBackend::notificationReceived);
    backend.start();
    backend.requestRecentChats();
    const auto dialog = QStringLiteral("chat91");
    const auto action = QStringLiteral("im.v2.Anchor.read");
    const QStringList outgoingVisible{QStringLiteral("803")};
    backend.requestChatMessages(dialog);
    const auto row = [&list] {
        return list.isEmpty() ? Chat{} : list.last().first().value<QVector<Chat>>().value(0);
    };
    QTRY_VERIFY(row().hasReaction);
    QTRY_VERIFY(!connection.isEmpty());

    fixture.approveAnchorRead = false;
    backend.readChatMessages(dialog, {}, outgoingVisible);
    QTRY_COMPARE(fixture.requestsFor(action).size(), 1);
    QTest::qWait(60);
    QVERIFY(row().hasReaction);
    fixture.approveAnchorRead = true;

    fixture.holdNextReadResponse = action;
    backend.readChatMessages(dialog, {}, outgoingVisible);
    QTRY_COMPARE(fixture.requestsFor(action).size(), 2);
    fixture.pushIm(QStringLiteral("other-message-anchor"), QStringLiteral("addAnchor"),
                   {{"chatId", 91}, {"messageId", 805}, {"userId", 11}, {"fromUserId", 22}, {"type", "mention"}});
    QTRY_VERIFY(row().hasMention);
    QVERIFY(row().hasReaction);
    fixture.releaseHeldReadResponse();
    QTRY_VERIFY(!row().hasReaction);
    QVERIFY(row().hasMention);

    const QJsonObject reaction{{"chatId", 91}, {"messageId", 803}, {"userId", 11}, {"fromUserId", 22}, {"type", "reaction"}};
    fixture.pushIm(QStringLiteral("reaction-again"), QStringLiteral("addAnchor"), reaction);
    QTRY_VERIFY(row().hasReaction);
    fixture.holdNextReadResponse = action;
    backend.readChatMessages(dialog, {}, outgoingVisible);
    QTRY_COMPARE(fixture.requestsFor(action).size(), 3);
    const auto pulsesBefore = available.size();
    fixture.pushIm(QStringLiteral("reaction-during-read"), QStringLiteral("addAnchor"), reaction);
    QTRY_COMPARE(available.size(), pulsesBefore + 1);
    QCOMPARE(available.last().at(0).toString(), dialog);
    fixture.releaseHeldReadResponse();
    QTRY_COMPARE(available.size(), pulsesBefore + 2);
    QVERIFY(row().hasReaction);

    backend.readChatMessages(dialog, {}, outgoingVisible);
    QTRY_COMPARE(fixture.requestsFor(action).size(), 4);
    QTRY_VERIFY(!row().hasReaction);
    QVERIFY(row().hasMention);
    QCOMPARE(fixture.requestsFor(action).last().field(QStringLiteral("messageIds[0]")), outgoingVisible.first());
    QVERIFY(fixture.requestsFor(QStringLiteral("im.v2.Chat.Message.read")).isEmpty());
    QVERIFY(fixture.requestsFor(QStringLiteral("im.v2.Chat.Message.Reaction.delete")).isEmpty());
    QVERIFY(popups.isEmpty());
}

void TestCloudBackend::exactReadLeavesOlderUnseenMessageEligible()
{
    CloudFixture fixture;
    fixture.chatBadgesFixture = true;
    fixture.liveRecentFixture = true;
    fixture.approveVisibleRead = true;
    fixture.visibleReadResult = R"({"chatId":91,"lastId":700,"counter":126,"exact":true,"viewedMessages":[900]})";
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy list(&backend, &Backend::recentChatsLoaded);
    backend.start();
    backend.requestRecentChats();
    const auto dialog = QStringLiteral("chat91");
    backend.requestChatMessages(dialog);
    const auto unread = [&list] {
        return list.isEmpty() ? -1 : list.last().first().value<QVector<Chat>>().value(0).unread;
    };
    QTRY_COMPARE(unread(), 127);
    backend.readChatMessages(dialog, {QStringLiteral("900")});
    QTRY_COMPARE(unread(), 126);

    fixture.visibleReadResult = R"({"chatId":91,"lastId":700,"counter":125,"exact":true,"viewedMessages":[899]})";
    backend.readChatMessages(dialog, {QStringLiteral("899"), QStringLiteral("900")});
    QTRY_COMPARE(unread(), 125);
    const auto requests = fixture.requestsFor(QStringLiteral("im.v2.Chat.Message.read"));
    QCOMPARE(requests.size(), 2);
    const auto sent = requests.last();
    QCOMPARE(sent.field(QStringLiteral("ids[0]")), QStringLiteral("899"));
    QVERIFY(sent.field(QStringLiteral("ids[1]")).isEmpty());
}

void TestCloudBackend::readsExplicitTaskUnreadBeforeNewerSystemCursor()
{
    CloudFixture fixture;
    fixture.chatBadgesFixture = true;
    fixture.approveVisibleRead = true;
    fixture.taskRecentBadgeFixture = R"({"userId":11,
        "recentItems":[{"dialogId":"chat154127","chatId":154127,"messageId":5415841,"unread":false}],
        "chats":[{"id":154127,"dialogId":"chat154127","type":"tasks","name":"Task chat",
                  "entityType":"TASKS_TASK","entityId":"122425"}]})";
    fixture.messengerPage = R"HTML(<script>
        BX.Messenger.v2.Application.Launch('messenger', {"currentUser":{"id":11},
            "counters":[{"chatId":154127,"counter":1,"type":"tasksTask","isMarkedAsUnread":false,"isNonAnswered":false}]});
        </script>)HTML";
    fixture.chatResponseOverride = ajaxResult(R"({"chat":{"id":154127,"dialogId":"chat154127","lastId":5415841,
        "lastMessageId":5415841,"unreadId":0,"userCounter":2},"messages":[
        {"id":5355385,"chatId":154127,"authorId":22,"unread":true,"text":"Earlier incoming"},
        {"id":5355386,"chatId":73,"authorId":22,"unread":true,"text":"Foreign history row"},
        {"id":5355387,"chatId":154127,"authorId":11,"unread":true,"text":"Own message"},
        {"id":5355388,"chatId":154127,"unread":true,"text":"Missing sender"},
        {"id":5355389,"chatId":154127,"authorId":11,"senderId":22,"unread":true,"text":"Conflicting sender"},
        {"id":5415839,"chatId":154127,"authorId":0,"unread":false,"text":"Task completed"},
        {"id":5415841,"chatId":154127,"authorId":0,"unread":false,"text":"Stage changed"}]})");
    fixture.visibleReadResult = R"({"chatId":154127,"counter":0,"viewedMessages":[5355385]})";
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy recent(&backend, &Backend::taskChatsLoaded);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    QSignalSpy confirmed(&backend, &Backend::chatReadConfirmed);
    backend.start();
    backend.requestTaskChats();
    const auto dialog = QStringLiteral("chat154127");
    const auto action = QStringLiteral("im.v2.Chat.Message.read");
    const auto older = QStringLiteral("5355385");
    const auto row = [&recent] {
        return recent.isEmpty() ? Chat{} : recent.last().first().value<QVector<Chat>>().value(0);
    };
    QTRY_COMPARE(row().id, dialog);
    QTRY_COMPARE(row().unread, 1);
    backend.requestChatMessages(dialog);
    QTRY_VERIFY(!history.isEmpty());
    const auto initial = history.last().at(1).value<QVector<Message>>();
    const auto first = std::find_if(initial.cbegin(), initial.cend(), [&older](const Message &item) {
        return item.id == older;
    });
    QVERIFY(first != initial.cend() && first->unread);
    QCOMPARE(row().unread, 1);
    const QStringList later{QStringLiteral("5355386"), QStringLiteral("5355387"), QStringLiteral("5355388"),
                            QStringLiteral("5355389"), QStringLiteral("5415839"), QStringLiteral("5415841")};
    backend.readChatMessages(dialog, later, later);
    QTest::qWait(50);
    QVERIFY(fixture.requestsFor(action).isEmpty());
    QCOMPARE(row().unread, 1);

    backend.readChatMessages(dialog, QStringList{older} + later);
    QTRY_COMPARE(fixture.requestsFor(action).size(), 1);
    const auto request = fixture.requestsFor(action).first();
    QCOMPARE(request.field(QStringLiteral("chatId")), QStringLiteral("154127"));
    QCOMPARE(request.field(QStringLiteral("ids[0]")), older);
    QVERIFY(request.field(QStringLiteral("ids[1]")).isEmpty());
    QTRY_COMPARE(row().unread, 0);
    QTRY_COMPARE(confirmed.size(), 1);
    QCOMPARE(confirmed.first().at(1).toStringList(), QStringList{older});

    // The same portal history snapshot can continue reporting unread after an exact receipt.
    const auto previous = history.size();
    backend.requestChatMessages(dialog);
    QTRY_VERIFY(history.size() > previous);
    const auto repeated = history.last().at(1).value<QVector<Message>>();
    const auto acknowledged = std::find_if(repeated.cbegin(), repeated.cend(), [&older](const Message &item) {
        return item.id == older;
    });
    QVERIFY(acknowledged != repeated.cend() && !acknowledged->unread);
    backend.readChatMessages(dialog, {older});
    QTest::qWait(50);
    QCOMPARE(fixture.requestsFor(action).size(), 1);
}

void TestCloudBackend::keepsAllExplicitUnreadRowsInTheNewlyLoadedOlderPage()
{
    CloudFixture fixture;
    fixture.chatBadgesFixture = true;
    fixture.approveVisibleRead = true;
    fixture.visibleReadResult = R"({"chatId":91,"counter":12,"viewedMessages":[900,899,898]})";
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    backend.start();
    backend.requestRecentChats();
    const auto dialog = QStringLiteral("chat91");
    const auto load = [&fixture, &backend, &dialog](int lastId, int size, bool includesSystem) {
        QJsonArray rows;
        for (int index = 0; index < size; ++index) {
            rows.append(QJsonObject{{"id", lastId - index}, {"chatId", 91},
                                    {"authorId", includesSystem && index == 0 ? 0 : 22},
                                    {"unread", true}, {"text", QStringLiteral("Unread history row")}});
        }
        fixture.chatResponseOverride = ajaxResult(QJsonDocument(QJsonObject{
            {"chat", QJsonObject{{"id", 91}, {"dialogId", dialog}, {"lastId", 3000}, {"unreadId", 0}}},
            {"messages", rows}}).toJson(QJsonDocument::Compact));
        backend.requestChatMessages(dialog);
    };
    load(1999, tunables::kPageSizeMessages, false);
    QTRY_COMPARE(history.size(), 1);
    load(2999, tunables::kPageSizeMessages, false);
    QTRY_COMPARE(history.size(), 2);
    load(900, 3, true);
    QTRY_COMPARE(history.size(), 3);
    const auto rows = history.last().at(1).value<QVector<Message>>();
    QCOMPARE(rows.size(), 3);
    QVERIFY(std::all_of(rows.cbegin(), rows.cend(), [](const Message &message) { return message.unread; }));
    QVERIFY(rows.last().system);
    const auto action = QStringLiteral("im.v2.Chat.Message.read");
    backend.readChatMessages(dialog, {QStringLiteral("900"), QStringLiteral("899"), QStringLiteral("898")});
    QTRY_COMPARE(fixture.requestsFor(action).size(), 1);
    const auto request = fixture.requestsFor(action).first();
    QCOMPARE(request.field(QStringLiteral("ids[0]")), QStringLiteral("900"));
    QCOMPARE(request.field(QStringLiteral("ids[1]")), QStringLiteral("899"));
    QCOMPARE(request.field(QStringLiteral("ids[2]")), QStringLiteral("898"));
}

void TestCloudBackend::readsTaskCardOpenedOutsideProjects()
{
    CloudFixture fixture;
    fixture.chatBadgesFixture = true;
    fixture.liveRecentFixture = true;
    fixture.approveVisibleRead = true;
    fixture.visibleReadResult = R"({"chatId":73,"lastId":800,"counter":0})";
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy list(&backend, &Backend::taskChatsLoaded);
    QSignalSpy history(&backend, &Backend::taskMessagesLoaded);
    backend.start();
    backend.requestTaskChats();
    backend.requestChatMessages(QStringLiteral("chat73"));
    backend.setActiveSection(2);
    backend.requestTaskMessages(QStringLiteral("42"));
    QTRY_COMPARE(history.size(), 1);
    const auto unread = [&list] {
        return list.isEmpty() ? -1 : list.last().first().value<QVector<Chat>>().value(0).unread;
    };
    QTRY_COMPARE(unread(), 7);

    const auto action = QStringLiteral("im.v2.Chat.Message.read");
    const QStringList visible{QStringLiteral("800")};
    backend.readChatMessages(QStringLiteral("chat73"), visible);
    backend.readTaskChatMessages(QStringLiteral("43"), visible);
    backend.readTaskChatMessages(QStringLiteral("42"), visible);
    backend.readTaskChatMessages(QStringLiteral("42"), {QStringLiteral("801")});
    QTRY_COMPARE(unread(), 0);
    QTRY_COMPARE(fixture.requestsFor(action).size(), 2);
    const auto requests = fixture.requestsFor(action);
    const auto first = requests.first();
    const auto second = requests.last();
    QCOMPARE(first.field(QStringLiteral("chatId")), QStringLiteral("73"));
    QCOMPARE(second.field(QStringLiteral("chatId")), QStringLiteral("73"));
    QCOMPARE(first.field(QStringLiteral("ids[0]")), QStringLiteral("800"));
    QCOMPARE(second.field(QStringLiteral("ids[0]")), QStringLiteral("801"));
}

void TestCloudBackend::loadsAuthorizedTaskSummaryWhileDetailsArePending()
{
    CloudFixture fixture;
    fixture.actionTaskLegacy = {{"id", 42}, {"title", "Initial task"}, {"description", "Early overview"},
        {"groupId", 17}, {"chatId", 73}, {"rights", QJsonObject{{"edit", true}}}};
    fixture.actionTaskCurrent = {{"id", 42}, {"rights", QJsonObject{{"edit", true}, {"remove", true}}}};
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy summaries(&backend, &Backend::taskSummaryLoaded);
    QSignalSpy cards(&backend, &Backend::taskLoaded);
    const auto task = QStringLiteral("42");
    const auto legacy = QStringLiteral("tasks.task.get");
    const auto details = QStringLiteral("tasks.v2.Task.get");
    backend.start();
    fixture.holdNotificationReply = details;
    backend.requestTask(task);
    backend.requestTask(task);
    QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());
    QTRY_COMPARE(summaries.size(), 1);
    QCOMPARE(cards.size(), 0);
    QCOMPARE(fixture.requestsFor(legacy).size(), 1);
    QCOMPARE(fixture.requestsFor(details).size(), 1);
    const auto summary = summaries.first().first().value<Task>();
    QCOMPARE(summary.id, task);
    QCOMPARE(summary.name, QStringLiteral("Initial task"));
    QCOMPARE(summary.description, QStringLiteral("Early overview"));
    QCOMPARE(summary.chatDialogId, QStringLiteral("chat73"));
    QVERIFY(!summary.editRights.description && !summary.editRights.remove);
    QVERIFY(summary.statusActions.isEmpty());

    // Refresh requested after the summary coalesces while its remaining reply is held.
    fixture.actionTaskLegacy.insert(QStringLiteral("title"), QStringLiteral("Newer task"));
    fixture.actionTaskCurrent.insert(QStringLiteral("rights"), QJsonObject{{"edit", true}, {"remove", false}});
    backend.requestTask(task);
    backend.requestTask(task);
    QCOMPARE(fixture.requestsFor(legacy).size(), 1);
    fixture.releaseHeldNotificationReply();
    QTRY_COMPARE(cards.size(), 1);
    const auto loaded = cards.first().first().value<Task>();
    QCOMPARE(loaded.name, QStringLiteral("Newer task"));
    QVERIFY(loaded.editRights.description);
    QVERIFY(!loaded.editRights.remove);
    QCOMPARE(fixture.requestsFor(legacy).size(), 2);
    QCOMPARE(fixture.requestsFor(details).size(), 2);

    fixture.actionTaskLegacy.insert(QStringLiteral("CHAT_ID"), 74);
    backend.requestTask(task);
    QTRY_COMPARE(cards.size(), 2);
    QVERIFY(cards.last().first().value<Task>().chatDialogId.isEmpty());
    QSignalSpy failures(&backend, &Backend::failed);
    backend.requestTaskMessages(task);
    QTRY_VERIFY(std::any_of(failures.cbegin(), failures.cend(), [](const QList<QVariant> &failure) {
        return failure.first().toString() == QLatin1String("task-messages:42");
    }));
    QCOMPARE(fixture.requestsFor(legacy).size(), 4);
    QVERIFY(fixture.requestsFor(QStringLiteral("im.v2.Chat.load")).isEmpty());
}

void TestCloudBackend::taskCardRequiresTheMatchingLegacyResponse_data()
{
    QTest::addColumn<int>("legacyId");
    QTest::addColumn<int>("detailsId");
    QTest::newRow("parallel exact responses") << 42 << 42;
    QTest::newRow("legacy read denied") << 0 << 42;
    QTest::newRow("legacy id does not match") << 43 << 42;
    QTest::newRow("optional details denied") << 42 << 0;
    QTest::newRow("optional details id does not match") << 42 << 43;
}

void TestCloudBackend::taskCardRequiresTheMatchingLegacyResponse()
{
    QFETCH(int, legacyId);
    QFETCH(int, detailsId);
    CloudFixture fixture;
    fixture.actionTaskLegacy = {{"id", legacyId}, {"title", "Authorised legacy task"}};
    fixture.actionTaskCurrent = {{"id", detailsId}, {"responsible", QJsonObject{{"id", 15}, {"name", "Details role"}}},
        {"rights", QJsonObject{{"edit", true}, {"remove", true}}}};
    const QByteArray denied = R"({"status":"error","errors":[{"code":"ACCESS_DENIED"}]})";
    if (legacyId == 0)
        fixture.taskLegacyResponseOverride = denied;
    if (detailsId == 0)
        fixture.taskCurrentResponseOverride = denied;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy summaries(&backend, &Backend::taskSummaryLoaded);
    QSignalSpy cards(&backend, &Backend::taskLoaded);
    QSignalSpy failures(&backend, &Backend::failed);
    backend.start();
    const auto legacy = QStringLiteral("tasks.task.get");
    const auto details = QStringLiteral("tasks.v2.Task.get");
    fixture.holdNotificationReply = legacy;
    backend.requestTask(QStringLiteral("42"));
    QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());
    QTRY_COMPARE(fixture.requestsFor(details).size(), 1);
    QVERIFY(summaries.isEmpty());
    QVERIFY(cards.isEmpty());
    fixture.releaseHeldNotificationReply();
    if (legacyId != 42) {
        QTRY_VERIFY(std::any_of(failures.cbegin(), failures.cend(), [](const QList<QVariant> &failure) {
            return failure.first().toString() == QLatin1String("task:42");
        }));
        QVERIFY(summaries.isEmpty());
        QVERIFY(cards.isEmpty());
        QVERIFY(fixture.requestsFor(QStringLiteral("tasks.v2.File.listObjects")).isEmpty());
    } else {
        QTRY_COMPARE(cards.size(), 1);
        const auto loaded = cards.first().first().value<Task>();
        QCOMPARE(loaded.id, QStringLiteral("42"));
        QCOMPARE(loaded.name, QStringLiteral("Authorised legacy task"));
        QCOMPARE(loaded.editRights.description, detailsId == 42);
        QCOMPARE(loaded.editRights.remove, detailsId == 42);
        QCOMPARE(loaded.assignee, detailsId == 42 ? QStringLiteral("Details role") : QString());
    }
}

void TestCloudBackend::discardsParallelTaskCardFromPreviousAccount()
{
    CloudFixture fixture;
    fixture.ordinaryRecentBadgeFixture = R"({"userId":11,"recentItems":[],"chats":[]})";
    fixture.actionTaskLegacy = {{"id", 42}, {"title", "Previous account task"}};
    fixture.actionTaskCurrent = {{"id", 42}, {"rights", QJsonObject{{"edit", true}, {"remove", true}}}};
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy accounts(&backend, &Backend::accountScopeChanged);
    QSignalSpy summaries(&backend, &Backend::taskSummaryLoaded);
    QSignalSpy cards(&backend, &Backend::taskLoaded);
    backend.start();
    backend.requestRecentChats();
    QTRY_COMPARE(accounts.size(), 1);
    fixture.holdNotificationReply = QStringLiteral("tasks.task.get");
    backend.requestTask(QStringLiteral("42"));
    QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());
    QTRY_COMPARE(fixture.requestsFor(QStringLiteral("tasks.v2.Task.get")).size(), 1);
    fixture.ordinaryRecentBadgeFixture = R"({"userId":22,"recentItems":[],"chats":[]})";
    backend.requestRecentChats();
    QTRY_COMPARE(accounts.size(), 2);
    fixture.actionTaskLegacy.insert(QStringLiteral("title"), QStringLiteral("Current account task"));
    fixture.actionTaskCurrent.insert(QStringLiteral("rights"), QJsonObject{});
    backend.requestTask(QStringLiteral("42"));
    QTRY_COMPARE(cards.size(), 1);
    QCOMPARE(cards.first().first().value<Task>().name, QStringLiteral("Current account task"));
    QVERIFY(!cards.first().first().value<Task>().editRights.remove);
    fixture.releaseHeldNotificationReply();
    QTest::qWait(50);
    QCOMPARE(cards.size(), 1);
    for (const auto &summary : summaries)
        QCOMPARE(summary.first().value<Task>().name, QStringLiteral("Current account task"));
}

void TestCloudBackend::restoresReplyFromKeyedPhotoAndKeepsExactScope()
{
    CloudFixture fixture;
    fixture.replyPreviewFixture = true;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    QSignalSpy search(&backend, &Backend::messagesFound);
    backend.setForeground(false);
    backend.start();
    backend.requestChatMessages(QStringLiteral("chat91"));
    QTRY_COMPARE(history.size(), 1);
    auto rows = history.last().at(1).value<QVector<Message>>();
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.at(0).replyId, QStringLiteral("690"));
    QCOMPARE(rows.at(0).replyAuthor, QStringLiteral("Автор снимка"));
    QCOMPARE(rows.at(0).replyFiles.value(0).name, QStringLiteral("Снимок.jpg"));
    QVERIFY(rows.at(0).replyFiles.value(0).isImage);
    QCOMPARE(rows.at(1).replyId, QStringLiteral("691"));
    QVERIFY(rows.at(1).replyAuthor.isEmpty());
    QVERIFY(rows.at(1).replyText.isEmpty());

    fixture.omitReplySource = true;
    backend.requestChatMessages(QStringLiteral("chat91"));
    QTRY_COMPARE(history.size(), 2);
    rows = history.last().at(1).value<QVector<Message>>();
    QCOMPARE(rows.at(0).replyFiles.value(0).name, QStringLiteral("Снимок.jpg"));
    backend.searchMessages(QStringLiteral("chat91"), QStringLiteral("ответ"), false);
    QTRY_COMPARE(search.size(), 1);
    const auto matches = search.last().at(3).value<QVector<Message>>();
    QCOMPARE(matches.value(0).replyId, QStringLiteral("690"));
    QCOMPARE(matches.value(0).replyFiles.value(0).name, QStringLiteral("Снимок.jpg"));
    QVERIFY(!matches.value(0).unread);

    backend.requestChatMessages(QStringLiteral("chat92"));
    QTRY_COMPARE(history.size(), 3);
    rows = history.last().at(1).value<QVector<Message>>();
    QCOMPARE(rows.at(0).replyId, QStringLiteral("690"));
    QVERIFY(rows.at(0).replyAuthor.isEmpty());
    QVERIFY(rows.at(0).replyFiles.isEmpty());
}

void TestCloudBackend::sendsOriginalReplyIdInBothChatScopes()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy sent(&backend, &Backend::messageSent);
    QSignalSpy taskHistory(&backend, &Backend::taskMessagesLoaded);
    QSignalSpy errors(&backend, &Backend::failed);
    backend.setForeground(false);
    backend.start();
    backend.requestTaskMessages(QStringLiteral("42"));
    QTRY_COMPARE(taskHistory.size(), 1);

    const auto action = QStringLiteral("im.v2.Chat.Message.send");
    backend.sendChatReply(QStringLiteral("chat91"), QStringLiteral("На фото"), QStringLiteral("690"));
    backend.sendTaskReply(QStringLiteral("42"), QStringLiteral("По задаче"), QStringLiteral("800"));
    QTRY_COMPARE(sent.size(), 2);
    auto sends = fixture.requestsFor(action);
    std::sort(sends.begin(), sends.end(), [](const Request &left, const Request &right) {
        return left.field(QStringLiteral("dialogId")) > right.field(QStringLiteral("dialogId"));
    });
    QCOMPARE(sends.size(), 2);
    QCOMPARE(sends.at(0).field(QStringLiteral("dialogId")), QStringLiteral("chat91"));
    QCOMPARE(sends.at(0).field(QStringLiteral("fields[replyId]")), QStringLiteral("690"));
    QCOMPARE(sends.at(0).field(QStringLiteral("fields[message]")), QStringLiteral("На фото"));
    QCOMPARE(sends.at(1).field(QStringLiteral("dialogId")), QStringLiteral("chat73"));
    QCOMPARE(sends.at(1).field(QStringLiteral("fields[replyId]")), QStringLiteral("800"));
    QVERIFY(!sends.at(1).field(QStringLiteral("fields[templateId]")).isEmpty());

    backend.sendChatReply(QStringLiteral("chat91"), QStringLiteral("Ошибочный ID"), QStringLiteral("chat690"));
    QCOMPARE(errors.size(), 1);
    QCOMPARE(errors.last().first().toString(), QStringLiteral("send:chat:chat91"));
    backend.sendTaskReply(QStringLiteral("42"), QStringLiteral("Пропавший ID"), {});
    QCOMPARE(errors.size(), 2);
    QCOMPARE(errors.last().first().toString(), QStringLiteral("send:task:42"));
    backend.sendChatMessage(QStringLiteral("chat91"), QStringLiteral("Без цитаты"));
    QTRY_COMPARE(sent.size(), 3);
    QCOMPARE(fixture.requestsFor(action).size(), 3);
    QVERIFY(fixture.requestsFor(action).last().field(QStringLiteral("fields[replyId]")).isEmpty());
}

void TestCloudBackend::createsTaskWithOneNativeShareAndReloadsTheSource()
{
    CloudFixture fixture;
    fixture.chatActionsFixture = true;
    fixture.taskShareResult = R"({"result":true})";
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy accepted(&backend, &Backend::chatActionSucceeded);
    QSignalSpy failed(&backend, &Backend::failed);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    QSignalSpy connection(&backend, &Backend::connectionChanged);
    backend.start();
    backend.setActiveSection(0);
    const auto dialog = QStringLiteral("chat91");
    const auto message = QStringLiteral("701");
    const auto method = QStringLiteral("/rest/im.message.share.json");
    backend.requestChatMessages(dialog);
    QTRY_VERIFY(!history.isEmpty());
    QTRY_VERIFY(!connection.isEmpty() && connection.last().first().toBool());
    QCOMPARE(history.last().at(1).value<QVector<Message>>().size(), 1);
    backend.changeMessage(dialog, message, MessageAction::CreateTask);
    backend.changeMessage(dialog, message, MessageAction::CreateTask);
    QTRY_COMPARE(accepted.size(), 1);
    QCOMPARE(accepted.first().first().toString(), dialog);
    QCOMPARE(fixture.requestsFor(method).size(), 1);
    const auto posted = fixture.requestsFor(method).first();
    QCOMPARE(posted.field(QStringLiteral("DIALOG_ID")), dialog);
    QCOMPARE(posted.field(QStringLiteral("MESSAGE_ID")), message);
    QCOMPARE(posted.field(QStringLiteral("TYPE")), QStringLiteral("TASK"));
    QTRY_COMPARE(history.last().at(1).value<QVector<Message>>().size(), 2);
    const auto rows = history.last().at(1).value<QVector<Message>>();
    QVERIFY(std::any_of(rows.cbegin(), rows.cend(), [](const auto &row) { return row.id == QLatin1String("702"); }));
    QVERIFY(fixture.requestsFor(QStringLiteral("im.v2.Chat.Message.send")).isEmpty());

    fixture.taskShareResult = R"({"result":false})";
    backend.changeMessage(dialog, message, MessageAction::CreateTask);
    QTRY_VERIFY(std::any_of(failed.cbegin(), failed.cend(), [](const auto &row) {
        return row.first().toString() == QLatin1String("create-task-from-message:chat91:701");
    }));
    QCOMPARE(fixture.requestsFor(method).size(), 2);
    QCOMPARE(accepted.size(), 1);
    backend.changeMessage(QStringLiteral("https://example.invalid/"), message, MessageAction::CreateTask);
    backend.changeMessage(QStringLiteral("chat92"), message, MessageAction::CreateTask);
    QVERIFY(std::any_of(failed.cbegin(), failed.cend(), [](const auto &row) {
        return row.first().toString() == QLatin1String("create-task-from-message:chat92:701");
    }));
    QCOMPARE(fixture.requestsFor(method).size(), 2);
}

void TestCloudBackend::performsVerifiedMessageActions()
{
    CloudFixture fixture;
    fixture.chatActionsFixture = true;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    backend.setForeground(false);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    QSignalSpy accepted(&backend, &Backend::chatActionSucceeded);
    QSignalSpy failed(&backend, &Backend::failed);
    backend.start();
    const auto dialog = QStringLiteral("chat91");
    const auto id = QStringLiteral("701");
    backend.requestChatMessages(dialog);
    QTRY_COMPARE(history.size(), 1);
    const auto message = history.first().at(1).value<QVector<Message>>().value(0);
    QCOMPARE(message.reactions.size(), 1);
    QCOMPARE(message.reactions.first().code, QStringLiteral("like"));
    QCOMPARE(message.reactions.first().count, 2);
    QVERIFY(message.reactions.first().own);

    const auto reactionCode = QStringLiteral("faceWithStuckOutTongueAndWinkingEye");
    backend.changeMessage(dialog, id, MessageAction::React, reactionCode);
    QTRY_COMPARE(accepted.size(), 1);
    const auto reaction = QStringLiteral("im.v2.Chat.Message.Reaction.add");
    const auto reactionData = fixture.requestsFor(reaction).value(0);
    QCOMPARE(reactionData.field(QStringLiteral("messageId")), id);
    QCOMPARE(reactionData.field(QStringLiteral("reaction")), reactionCode);
    backend.changeMessage(dialog, id, MessageAction::RemoveReaction, reactionCode);
    QTRY_COMPARE(accepted.size(), 2);
    const auto reactionDelete = QStringLiteral("im.v2.Chat.Message.Reaction.delete");
    QCOMPARE(fixture.requestsFor(reactionDelete).size(), 1);
    const auto removedReaction = fixture.requestsFor(reactionDelete).first();
    QCOMPARE(removedReaction.field(QStringLiteral("messageId")), id);
    QCOMPARE(removedReaction.field(QStringLiteral("reaction")), reactionCode);

    backend.forwardMessage(dialog, id, QStringLiteral("chat73"));
    QTRY_COMPARE(accepted.size(), 3);
    QCOMPARE(accepted.last().first().toString(), dialog);
    const auto send = QStringLiteral("im.v2.Chat.Message.send");
    const auto wire = QJsonDocument::fromJson(fixture.requestsFor(send).value(0).body).object();
    QCOMPARE(wire.value(QStringLiteral("dialogId")).toString(), QStringLiteral("chat73"));
    const auto map = wire.value(QStringLiteral("fields")).toObject().value(QStringLiteral("forwardIds")).toObject();
    QCOMPARE(map.size(), 1);
    QVERIFY(!QUuid::fromString(map.constBegin().key()).isNull());
    QCOMPARE(map.constBegin().value().toInt(), 701);

    fixture.refuseChatAction = true;
    backend.forwardMessage(dialog, id, QStringLiteral("chat73"));
    QTRY_COMPARE(failed.size(), 1);
    backend.changeMessage(dialog, id, MessageAction::Delete);
    QTRY_COMPARE(failed.size(), 2);
    QCOMPARE(accepted.size(), 3);
    QCOMPARE(history.size(), 1);

    fixture.refuseChatAction = false;
    const auto editedText = QStringLiteral("Новый текст + A&B\nВторая строка");
    backend.changeMessage(dialog, id, MessageAction::Edit, editedText);
    QTRY_COMPARE(accepted.size(), 4);
    const auto edits = fixture.requestsFor(QStringLiteral("im.v2.Chat.Message.update"));
    QCOMPARE(edits.size(), 1);
    QCOMPARE(edits.first().field(QStringLiteral("id")), id);
    QCOMPARE(edits.first().field(QStringLiteral("fields[message]")), editedText);
    backend.changeMessage(dialog, id, MessageAction::Delete);
    QTRY_COMPARE(accepted.size(), 5);
    const auto removed = QJsonDocument::fromJson(fixture.requestsFor(QStringLiteral("im.v2.Chat.Message.delete")).last().body).object();
    QCOMPARE(removed.value(QStringLiteral("messageIds")).toArray(), QJsonArray{701});

    for (const auto &[action, method] : QVector<QPair<MessageAction, QString>>{
            {MessageAction::Pin, QStringLiteral("/rest/im.v2.Chat.Message.pin.json")},
            {MessageAction::Unpin, QStringLiteral("/rest/im.v2.Chat.Message.unpin.json")},
            {MessageAction::Favorite, QStringLiteral("/rest/im.chat.favorite.add.json")},
            {MessageAction::Unfavorite, QStringLiteral("/rest/im.chat.favorite.delete.json")}}) {
        const auto before = accepted.size();
        backend.changeMessage(dialog, id, action);
        QTRY_COMPARE(accepted.size(), before + 1);
        const auto request = fixture.requestsFor(method).value(0);
        QCOMPARE(request.field(method.contains(QLatin1String("favorite")) ? QStringLiteral("MESSAGE_ID") : QStringLiteral("id")), id);
    }
}

void TestCloudBackend::refreshesConfirmedReactions()
{
    CloudFixture fixture;
    fixture.chatActionsFixture = true;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    QSignalSpy accepted(&backend, &Backend::chatActionSucceeded);
    QSignalSpy failed(&backend, &Backend::failed);
    QSignalSpy changed(&backend, &Backend::chatOwnReactionChanged);
    backend.start();
    const auto dialog = QStringLiteral("chat91");
    const auto id = QStringLiteral("701");
    const auto code = QStringLiteral("faceWithStuckOutTongueAndWinkingEye");
    auto includesOwnReaction = [&] {
        if (history.isEmpty())
            return false;
        const auto messages = history.last().at(1).value<QVector<Message>>();
        return !messages.isEmpty() && std::any_of(messages.first().reactions.cbegin(),
            messages.first().reactions.cend(), [&](const auto &reaction) {
                return reaction.code == code && reaction.own && reaction.count == 1;
            });
    };
    backend.requestChatMessages(dialog);
    QTRY_VERIFY(!history.isEmpty());
    QVERIFY(!includesOwnReaction());

    backend.changeMessage(dialog, id, MessageAction::React, code);
    QTRY_COMPARE(accepted.size(), 1);
    QTRY_VERIFY(includesOwnReaction());
    QCOMPARE(changed.size(), 1);
    QCOMPARE(changed.first(), QVariantList({dialog, id, code, true}));

    fixture.refuseChatAction = true;
    backend.changeMessage(dialog, id, MessageAction::RemoveReaction, code);
    QTRY_COMPARE(failed.size(), 1);
    QCOMPARE(accepted.size(), 1);
    QCOMPARE(changed.size(), 1);
    QVERIFY(includesOwnReaction());

    fixture.refuseChatAction = false;
    backend.changeMessage(dialog, id, MessageAction::RemoveReaction, code);
    QTRY_COMPARE(accepted.size(), 2);
    QTRY_VERIFY(!includesOwnReaction());
    QCOMPARE(changed.size(), 2);
    QCOMPARE(changed.last(), QVariantList({dialog, id, code, false}));

    fixture.refuseChatAction = true;
    backend.changeMessage(dialog, id, MessageAction::React, code);
    QTRY_COMPARE(failed.size(), 2);
    QCOMPARE(accepted.size(), 2);
    QCOMPARE(changed.size(), 2);
    QVERIFY(!includesOwnReaction());
}

void TestCloudBackend::decodesFullReactionCatalog()
{
    CloudFixture fixture;
    fixture.chatActionsFixture = true;
    fixture.completeReactionCatalog = true;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    backend.start();
    backend.requestChatMessages(QStringLiteral("chat91"));
    QTRY_VERIFY(!history.isEmpty());
    const auto messages = history.last().at(1).value<QVector<Message>>();
    QCOMPARE(messages.size(), 1);
    const auto &reactions = messages.first().reactions;
    for (const auto &choice : ui::reactionChoices) {
        const auto code = QLatin1String(choice.code);
        const auto found = std::find_if(reactions.cbegin(), reactions.cend(), [code](const auto &reaction) {
            return reaction.code == code;
        });
        QVERIFY2(found != reactions.cend(), choice.code);
        QCOMPARE(found->count, 2);
    }
    QCOMPARE(reactions.size(), 64);
    for (const auto &reaction : reactions) {
        if (reaction.code.startsWith(QLatin1String("fixtureReaction"))) {
            QCOMPARE(reaction.count, 0);
            QVERIFY(reaction.own);
        }
    }
}

void TestCloudBackend::createsPrivateGroupAndChangesMembers()
{
    CloudFixture fixture;
    fixture.chatActionsFixture = true;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy created(&backend, &Backend::groupChatCreated);
    QSignalSpy accepted(&backend, &Backend::chatActionSucceeded);
    QSignalSpy failed(&backend, &Backend::failed);
    backend.start();

    backend.createGroupChat(QStringLiteral("Команда"), {QStringLiteral("33"), QStringLiteral("33"), QStringLiteral("44")});
    QTRY_COMPARE(created.size(), 1);
    const auto dialog = created.first().first().toString();
    QCOMPARE(dialog, QStringLiteral("chat93"));
    const auto group = fixture.requestsFor(QStringLiteral("/rest/im.v2.Chat.add.json")).value(0);
    QCOMPARE(group.field(QStringLiteral("fields[type]")), QStringLiteral("CHAT"));
    QCOMPARE(group.field(QStringLiteral("fields[searchable]")), QStringLiteral("N"));
    QCOMPARE(group.field(QStringLiteral("fields[users][0]")), QStringLiteral("33"));
    QCOMPARE(group.field(QStringLiteral("fields[users][1]")), QStringLiteral("44"));

    backend.changeChatMember(dialog, QStringLiteral("55"), true);
    QTRY_COMPARE(accepted.size(), 1);
    const auto add = fixture.requestsFor(QStringLiteral("/rest/im.chat.user.add.json")).value(0);
    QCOMPARE(add.field(QStringLiteral("CHAT_ID")), QStringLiteral("93"));
    QCOMPARE(add.field(QStringLiteral("USERS[0]")), QStringLiteral("55"));
    QCOMPARE(add.field(QStringLiteral("HIDE_HISTORY")), QStringLiteral("Y"));
    backend.changeChatMember(dialog, QStringLiteral("55"), false);
    QTRY_COMPARE(accepted.size(), 2);
    const auto remove = fixture.requestsFor(QStringLiteral("/rest/im.v2.Chat.deleteUser.json")).value(0);
    QCOMPARE(remove.field(QStringLiteral("dialogId")), dialog);
    QCOMPARE(remove.field(QStringLiteral("userId")), QStringLiteral("55"));
    backend.changeChatMember(QStringLiteral("55"), QStringLiteral("33"), false);
    QCOMPARE(failed.size(), 1);
    QCOMPARE(fixture.requestsFor(QStringLiteral("/rest/im.v2.Chat.deleteUser.json")).size(), 1);
}

void TestCloudBackend::loadsBoundedScopedMediaAndLinks()
{
    CloudFixture fixture;
    fixture.chatActionsFixture = true;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy loaded(&backend, &Backend::chatAssetsLoaded);
    backend.start();
    const auto dialog = QStringLiteral("chat91");
    backend.requestChatAssets(dialog, ChatAssetKind::Media);
    QTRY_COMPARE(loaded.size(), 1);
    const auto media = loaded.first().at(3).value<QVector<ChatAsset>>();
    QCOMPARE(media.size(), 50);
    QVERIFY(media.first().file.isImage);
    QCOMPARE(media.first().file.name, QStringLiteral("Фото.jpg"));
    QCOMPARE(media.first().file.downloadUrl.path(), QStringLiteral("/disk/download.xlsx"));
    const auto next = loaded.first().at(4).toString();
    QCOMPARE(next, QStringLiteral("451"));
    backend.requestChatAssets(dialog, ChatAssetKind::Media, next);
    QTRY_COMPARE(loaded.size(), 2);
    QCOMPARE(loaded.last().at(3).value<QVector<ChatAsset>>().size(), 0);
    const auto pages = fixture.requestsFor(QStringLiteral("/rest/im.chat.file.get.json"));
    QCOMPARE(pages.size(), 2);
    QCOMPARE(pages.first().field(QStringLiteral("CHAT_ID")), QStringLiteral("91"));
    QCOMPARE(pages.first().field(QStringLiteral("GROUP")), QStringLiteral("media"));
    QCOMPARE(pages.last().field(QStringLiteral("LAST_ID")), next);

    backend.requestChatAssets(dialog, ChatAssetKind::Files);
    QTRY_COMPARE(loaded.size(), 3);
    QCOMPARE(fixture.requestsFor(QStringLiteral("/rest/im.chat.file.get.json")).last().field(QStringLiteral("GROUP")),
             QStringLiteral("file"));
    backend.requestChatAssets(dialog, ChatAssetKind::Links);
    QTRY_COMPARE(loaded.size(), 4);
    const auto links = loaded.last().at(3).value<QVector<ChatAsset>>();
    QCOMPARE(links.size(), 1);
    QCOMPARE(links.first().link, QUrl(QStringLiteral("https://example.com/a")));
    QCOMPARE(links.first().messageId, QStringLiteral("701"));
    QCOMPARE(fixture.requestsFor(QStringLiteral("/rest/im.chat.url.get.json")).first().field(QStringLiteral("LIMIT")), QStringLiteral("50"));
}

void TestCloudBackend::sendsAndReceivesScopedTypingActivity()
{
    CloudFixture fixture;
    fixture.chatActionsFixture = true;
    fixture.liveRecentFixture = true;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy state(&backend, &Backend::connectionChanged);
    QSignalSpy recent(&backend, &Backend::recentChatsLoaded);
    QSignalSpy typing(&backend, &Backend::chatTypingChanged);
    QSignalSpy seen(&backend, &Backend::chatMessagesSeen);
    backend.start();
    backend.requestRecentChats();
    const auto dialog = QStringLiteral("chat91");
    backend.requestChatMessages(dialog);
    QTRY_COMPARE(state.size(), 1);
    QTRY_VERIFY(!recent.isEmpty());
    backend.notifyChatTyping(dialog);
    backend.notifyChatTyping(dialog);
    backend.notifyChatTyping(QStringLiteral("chat92"));
    const auto action = QStringLiteral("im.v2.Chat.InputAction.notify");
    QTRY_COMPARE(fixture.requestsFor(action).size(), 1);
    const auto wire = QJsonDocument::fromJson(fixture.requestsFor(action).first().body).object();
    QCOMPARE(wire.value(QStringLiteral("dialogId")).toString(), dialog);
    QCOMPARE(wire.value(QStringLiteral("type")).toString(), QStringLiteral("writing"));
    fixture.pushIm(QStringLiteral("self-typing"), QStringLiteral("inputActionNotify"),
                   {{"dialogId", dialog}, {"userId", 11}, {"type", "writing"}});
    fixture.pushIm(QStringLiteral("not-typing"), QStringLiteral("inputActionNotify"),
                   {{"dialogId", dialog}, {"userId", 22}, {"type", "sendingFile"}});
    fixture.pushIm(QStringLiteral("peer-typing"), QStringLiteral("inputActionNotify"),
                   {{"dialogId", dialog}, {"userId", 22}, {"type", "writing"}, {"duration", 800}});
    QTRY_COMPARE(typing.size(), 1);
    QCOMPARE(typing.first().at(0).toString(), dialog);
    QCOMPARE(typing.first().at(1).toString(), QStringLiteral("22"));
    QCOMPARE(typing.first().at(2).toInt(), 800);
    fixture.pushIm(QStringLiteral("self-seen"), QStringLiteral("readMessageChatOpponent"),
                   {{"dialogId", dialog}, {"userId", 11}, {"viewedMessages", QJsonArray{700}}});
    fixture.pushIm(QStringLiteral("peer-seen"), QStringLiteral("readMessageChatOpponent"),
                   {{"dialogId", dialog}, {"userId", 22}, {"viewedMessages", QJsonArray{701}}});
    QTRY_COMPARE(seen.size(), 1);
    QCOMPARE(seen.first().at(0).toString(), dialog);
    QCOMPARE(seen.first().at(1).toStringList(), QStringList{QStringLiteral("701")});
}

void TestCloudBackend::immediatelyUpdatesLiveRecentAndProtectsItFromOldSnapshots()
{
    CloudFixture fixture;
    fixture.liveRecentFixture = true;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy ordinary(&backend, &Backend::recentChatsLoaded);
    QSignalSpy tasks(&backend, &Backend::taskChatsLoaded);
    QSignalSpy online(&backend, &Backend::connectionChanged);
    backend.setForeground(false);
    backend.setActiveSection(1);
    backend.start();
    backend.requestRecentChats();
    backend.requestTaskChats();
    QTRY_COMPARE(online.size(), 1);
    QTRY_COMPARE(ordinary.size(), 1);
    QTRY_COMPARE(tasks.size(), 1);

    const auto push = [&fixture](QString mid, int chatId, qint64 messageId, int count, int author, bool task, bool metadataOnly = false) {
        const auto dialog = QStringLiteral("chat%1").arg(chatId);
        const auto key = QString::number(chatId);
        const auto type = task ? QStringLiteral("tasksTask") : QStringLiteral("chat");
        const auto section = task ? type : QStringLiteral("default");
        const QJsonObject chat{{"id", chatId}, {"type", type}, {"name", QStringLiteral("Чат из события")}};
        fixture.pushIm(mid, QStringLiteral("messageChat"), {{"dialogId", dialog}, {"chatId", chatId}, {"counter", count},
            {"skipRecentUpdate", metadataOnly},
            {"userInChat", QJsonObject{{key, QJsonArray{11, 22}}}}, {"chat", QJsonObject{{key, chat}}},
            {"recentConfig", QJsonObject{{"sections", QJsonArray{section}}}},
            {"message", QJsonObject{{"id", messageId}, {"chatId", chatId}, {"senderId", author}, {"text", "Новое"}}}});
    };
    push(QStringLiteral("new-91"), 91, 901, 4, 22, false, true);
    QTRY_COMPARE_WITH_TIMEOUT(ordinary.size(), 2, 1200);
    QCOMPARE(ordinary.last().first().value<QVector<Chat>>().first().unread, 4);
    QCOMPARE(fixture.requestsFor(QStringLiteral("im.v2.Recent.load")).size(), 2);
    push(QStringLiteral("new-99"), 99, 902, 1, 22, false);
    QTRY_COMPARE(ordinary.size(), 3);
    QCOMPARE(ordinary.last().first().value<QVector<Chat>>().first().id, QStringLiteral("chat99"));
    push(QStringLiteral("new-task"), 73, 903, 2, 22, true);
    QTRY_COMPARE(tasks.size(), 2);
    QCOMPARE(tasks.last().first().value<QVector<Chat>>().first().unread, 2);
    QCOMPARE(tasks.last().first().value<QVector<Chat>>().first().id, QStringLiteral("chat73"));

    backend.requestRecentChats();
    QTRY_COMPARE(ordinary.size(), 4);
    const auto before = ordinary.last().first().value<QVector<Chat>>();
    const auto old = std::find_if(before.cbegin(), before.cend(), [](const Chat &chat) { return chat.id == QStringLiteral("chat91"); });
    QVERIFY(old != before.cend());
    QCOMPARE(old->unread, 4);

    fixture.pushIm(QStringLiteral("old-read"), QStringLiteral("readMessageChat"),
                   {{"dialogId", "chat91"}, {"chatId", 91}, {"lastId", 900}, {"counter", 0}, {"unread", false}}, 20);
    fixture.pushIm(QStringLiteral("old-partial"), QStringLiteral("readMessageChat"),
                   {{"dialogId", "chat91"}, {"chatId", 91}, {"lastId", 900}, {"counter", 1}, {"unread", false}}, 20);
    QTest::qWait(100);
    QCOMPARE(ordinary.size(), 4);

    fixture.pushIm(QStringLiteral("current-partial"), QStringLiteral("readMessageChat"),
                   {{"dialogId", "chat91"}, {"chatId", 91}, {"lastId", 900}, {"counter", 2}, {"unread", false}});
    QTRY_COMPARE(ordinary.size(), 5);
    const auto partial = ordinary.last().first().value<QVector<Chat>>();
    const auto stillUnread = std::find_if(partial.cbegin(), partial.cend(), [](const Chat &chat) { return chat.id == QStringLiteral("chat91"); });
    QVERIFY(stillUnread != partial.cend());
    QCOMPARE(stillUnread->unread, 2);

    fixture.pushIm(QStringLiteral("read-live"), QStringLiteral("readMessageChat"),
                   {{"dialogId", "chat91"}, {"chatId", 91}, {"lastId", 901}, {"counter", 0}, {"unread", false}});
    QTRY_COMPARE(ordinary.size(), 6);
    const auto read = ordinary.last().first().value<QVector<Chat>>();
    const auto cleared = std::find_if(read.cbegin(), read.cend(), [](const Chat &chat) { return chat.id == QStringLiteral("chat91"); });
    QVERIFY(cleared != read.cend());
    QCOMPARE(cleared->unread, 0);
    push(QStringLiteral("new-mid-old-message"), 91, 901, 4, 22, false);
    push(QStringLiteral("own-message"), 91, 904, 8, 11, false);
    QTest::qWait(150);
    QCOMPARE(ordinary.size(), 6);
}

void TestCloudBackend::marksOnlyDismissedPersonalChatsAndUsesOneHydration()
{
    CloudFixture fixture;
    fixture.dismissedChatsFixture = true;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy recent(&backend, &Backend::recentChatsLoaded);
    QSignalSpy search(&backend, &Backend::chatsFound);
    QSignalSpy scope(&backend, &Backend::accountScopeChanged);
    backend.start();
    backend.requestRecentChats();
    QTRY_VERIFY(!recent.isEmpty());
    QVERIFY(recent.first().first().value<QVector<Chat>>().first().dismissed);
    QTRY_COMPARE(recent.size(), 2);
    QCOMPARE(scope.size(), 1);
    QVERIFY(!backend.settingsScope().isEmpty());
    const auto chats = recent.last().first().value<QVector<Chat>>();
    QCOMPARE(chats.size(), 8);
    QVERIFY(chats.at(0).dismissed);
    QVERIFY(chats.at(0).pinned);
    QVERIFY(!chats.at(1).dismissed);
    QVERIFY(!chats.at(2).dismissed);
    QVERIFY(chats.at(3).dismissed);
    QVERIFY(!chats.at(4).dismissed);
    QVERIFY(!chats.at(5).dismissed);
    QVERIFY(!chats.at(6).dismissed);
    QCOMPARE(chats.at(7).title, chats.at(0).title);
    QVERIFY(!chats.at(7).dismissed);
    const auto statusRequests = fixture.requestsFor(QStringLiteral("/rest/im.user.list.get.json"));
    QCOMPARE(statusRequests.size(), 1);
    QCOMPARE(statusRequests.first().field(QStringLiteral("ID[0]")), QStringLiteral("106"));
    QCOMPARE(statusRequests.first().field(QStringLiteral("ID[1]")), QStringLiteral("107"));
    QCOMPARE(statusRequests.first().field(QStringLiteral("ID[2]")), QStringLiteral("108"));
    QVERIFY(statusRequests.first().field(QStringLiteral("ID[3]")).isEmpty());
    backend.searchChats(QStringLiteral("Имя"));
    QTRY_COMPARE(search.size(), 1);
    const auto found = search.first().at(1).value<QVector<Chat>>();
    QCOMPARE(found.size(), 2);
    QVERIFY(found.first().dismissed);
    QVERIFY(!found.last().dismissed);
}

void TestCloudBackend::projectsStillLoadWhenOptionalIdentityIsUnavailable()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend owner(fixture.session(), true);
    owner.start();
    QSignalSpy pinned(&owner, &Backend::projectPinChanged);
    owner.setProjectPinned(QStringLiteral("17"), true);
    QTRY_COMPARE(pinned.size(), 1);

    fixture.currentUserId = 22;
    CloudBackend other(fixture.session(), true);
    QSignalSpy otherProjects(&other, &Backend::projectsLoaded);
    other.start();
    other.requestProjects();
    QTRY_COMPARE(otherProjects.size(), 1);
    QVERIFY(!otherProjects.first().first().value<QVector<Project>>().first().pinned);

    fixture.denyIdentity = true;
    CloudBackend unknown(fixture.session(), true);
    QSignalSpy projects(&unknown, &Backend::projectsLoaded);
    QSignalSpy failures(&unknown, &Backend::failed);
    unknown.start();
    unknown.requestProjects();
    QTRY_COMPARE_WITH_TIMEOUT(projects.size(), 1, 1200);
    QVERIFY(!projects.first().first().value<QVector<Project>>().first().pinned);
    QVERIFY(failures.isEmpty());
}

void TestCloudBackend::refusingUnsafeDownloadTargetsLeavesLinkDestinationUntouched()
{
#ifdef Q_OS_UNIX
    CloudFixture fixture;
    fixture.richMessagesFixture = true;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    QSignalSpy failures(&backend, &Backend::failed);
    QSignalSpy saved(&backend, &Backend::messageFileDownloaded);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto destination = directory.filePath(QStringLiteral("private.xlsx"));
    const auto link = directory.filePath(QStringLiteral("selection.xlsx"));
    const auto fifo = directory.filePath(QStringLiteral("special.xlsx"));
    QFile original(destination);
    QVERIFY(original.open(QIODevice::WriteOnly));
    QCOMPARE(original.write("private bytes"), 13);
    original.close();
    QVERIFY(QFile::link(destination, link));
    QCOMPARE(::mkfifo(QFile::encodeName(fifo).constData(), 0600), 0);

    backend.start();
    backend.requestChatMessages(QStringLiteral("chat91"));
    QTRY_COMPARE(history.size(), 1);
    const auto url = history.first().at(1).value<QVector<Message>>().first().files.first().downloadUrl;
    backend.downloadMessageFile(url, link);
    backend.downloadMessageFile(url, fifo);
    QTRY_COMPARE_WITH_TIMEOUT(failures.size(), 2, 1200);
    QVERIFY(fixture.requestsFor(QStringLiteral("/disk/download.xlsx")).isEmpty());

    QVERIFY(QFile::remove(link));
    fixture.holdNextDownload = true;
    backend.downloadMessageFile(url, link);
    QTRY_COMPARE(fixture.requestsFor(QStringLiteral("/disk/download.xlsx")).size(), 1);
    QVERIFY(QFile::link(destination, link));
    fixture.releaseHeldDownload();
    QTRY_COMPARE(failures.size(), 3);
    QVERIFY(saved.isEmpty());
    QVERIFY(QFileInfo(link).isSymLink());
    QVERIFY(original.open(QIODevice::ReadOnly));
    QCOMPARE(original.readAll(), QByteArray("private bytes"));
#else
    QSKIP("Requires POSIX symbolic links and a FIFO fixture");
#endif
}

void TestCloudBackend::loadsInitialTaskPeopleInPortalRecentOrder()
{
    using namespace Qt::StringLiterals;
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    backend.start();
    QSignalSpy results(&backend, &Backend::peopleFound);
    backend.searchPeople(u"initial-task-people"_s, {});
    QTRY_COMPARE(results.size(), 1);
    QCOMPARE(results.first().first().toString(), u"initial-task-people"_s);
    const auto people = results.first().last().value<QVector<Person>>();
    QCOMPARE(people.size(), 20);
    QCOMPARE(people.at(0).name, u"Сергей"_s);
    QCOMPARE(people.at(0).avatarUrl.path(), u"/upload/staff.png"_s);
    QCOMPARE(people.at(1).name, u"Ирина"_s);
    QCOMPARE(people.at(2).id, u"1000"_s);
    QCOMPARE(people.last().id, u"1017"_s);
    const auto requests = fixture.requestsFor(u"ui.entityselector.load"_s);
    QCOMPARE(requests.size(), 1);
    const auto request = requests.first();
    QCOMPARE(QUrlQuery(request.url).queryItemValue(u"context"_s), u"tasks-card"_s);
    const auto payload = QJsonDocument::fromJson(request.body).object();
    QVERIFY(!payload.contains(u"searchQuery"_s));
    const auto dialog = payload.value(u"dialog"_s).toObject();
    QCOMPARE(dialog.value(u"context"_s).toString(), u"tasks-card"_s);
    QCOMPARE(dialog.value(u"recentItemsLimit"_s).toInt(), 20);
    QCOMPARE(dialog.value(u"entities"_s).toArray().first().toObject().value(u"id"_s).toString(), u"user"_s);
    QVERIFY(fixture.requestsFor(u"im.v2.Chat.Mention.list"_s).isEmpty());
    QVERIFY(fixture.requestsFor(u"ui.entityselector.doSearch"_s).isEmpty());

    backend.searchPeople(u"initial-task-people-no-recents"_s, {});
    QTRY_COMPARE(results.size(), 2);
    const auto fallback = results.last().last().value<QVector<Person>>();
    QCOMPARE(fallback.size(), 20);
    QCOMPARE(fallback.at(0).id, u"75"_s);
    QCOMPARE(fallback.at(1).id, u"76"_s);
    QCOMPARE(fallback.at(2).id, u"88"_s);

    QSignalSpy failed(&backend, &Backend::failed);
    backend.searchPeople(u"initial-task-people-denied"_s, {});
    QTRY_COMPARE(failed.size(), 1);
    QCOMPARE(failed.first().first().toString(), u"search-people:initial-task-people-denied"_s);
    QCOMPARE(results.size(), 2);
}

void TestCloudBackend::searchesPeopleWithinThePortalScope()
{
    using namespace Qt::StringLiterals;
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    backend.start();
    QSignalSpy results(&backend, &Backend::peopleFound);
    backend.searchPeople(u"pending"_s, {}, u"chat73"_s);
    QTRY_COMPARE(results.size(), 1);
    QCOMPARE(results[0][0].toString(), u"pending"_s);
    auto people = results[0][1].value<QVector<Person>>();
    QCOMPARE(people.size(), 1);
    QCOMPARE(people.first().id, u"74"_s);
    QCOMPARE(people.first().position, u"Инженер"_s);
    QVERIFY(!people.first().avatarUrl.isEmpty());
    auto requests = fixture.requestsFor(u"im.v2.Chat.Mention.list"_s);
    QCOMPARE(requests.size(), 1);
    QCOMPARE(requests.first().field(u"dialogId"_s), u"chat73"_s);
    QCOMPARE(requests.first().field(u"order[lastSendMessageId]"_s), u"desc"_s);

    backend.searchPeople(u"mention"_s, u"Иван-Пётр"_s, u"chat73"_s);
    QTRY_COMPARE(results.size(), 2);
    people = results[1][1].value<QVector<Person>>();
    QCOMPARE(people.size(), 1);
    QCOMPARE(people.first().name, u"Иван"_s);
    QCOMPARE(people.first().position, u"Инженер"_s);
    auto searches = fixture.requestsFor(u"ui.entityselector.doSearch"_s);
    QCOMPARE(searches.size(), 1);
    const auto mention = QJsonDocument::fromJson(searches.first().body).object();
    const auto config = mention.value(u"dialog"_s).toObject();
    const auto entity = config.value(u"entities"_s).toArray().first().toObject();
    const auto options = entity.value(u"options"_s).toObject();
    QCOMPARE(config.value(u"context"_s).toString(), u"IM_CHAT_SEARCH"_s);
    QCOMPARE(options.value(u"contextChatId"_s).toInteger(), 73);
    QVERIFY(options.value(u"mentionSearch"_s).toBool());
    QCOMPARE(options.value(u"exclude"_s).toArray(), QJsonArray{u"chats"_s});
    const QJsonArray words{u"Иван"_s, u"Пётр"_s};
    QCOMPARE(mention.value(u"searchQuery"_s).toObject().value(u"queryWords"_s).toArray(), words);

    backend.searchPeople(u"assignee"_s, u"Ирина"_s);
    QTRY_COMPARE(results.size(), 3);
    QCOMPARE(results[2][0].toString(), u"assignee"_s);
    people = results[2][1].value<QVector<Person>>();
    QCOMPARE(people.size(), 1);
    QCOMPARE(people.first().id, u"75"_s);
    QCOMPARE(people.first().position, u"Работа"_s);
    searches = fixture.requestsFor(u"ui.entityselector.doSearch"_s);
    QCOMPARE(searches.size(), 2);
    const auto staff = QJsonDocument::fromJson(searches.last().body).object();
    const auto staffConfig = staff.value(u"dialog"_s).toObject();
    QCOMPARE(staffConfig.value(u"context"_s).toString(), u"tasks-card"_s);
    QCOMPARE(QUrlQuery(searches.last().url).queryItemValue(u"context"_s), u"tasks-card"_s);
    const auto staffEntities = staffConfig.value(u"entities"_s).toArray();
    QCOMPARE(staffEntities.size(), 2);
    const auto staffEntity = staffEntities.first().toObject();
    QCOMPARE(staffEntity.value(u"id"_s).toString(), u"user"_s);
    QCOMPARE(staffEntities.last().toObject().value(u"id"_s).toString(), u"department"_s);
    QVERIFY(staffEntity.value(u"options"_s).toObject().value(u"emailUsers"_s).toBool());

    backend.searchPeople(u"unavailable"_s, u"denied"_s);
    QTRY_COMPARE(results.size(), 4);
    QCOMPARE(results[3][0].toString(), u"unavailable"_s);
    QVERIFY(results[3][1].value<QVector<Person>>().isEmpty());
}

void TestCloudBackend::initTestCase()
{
    QVERIFY(m_settingsPath.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsPath.path());
}

void TestCloudBackend::usesSenderIdentityAndDecodesForwardAndQuoteMetadata()
{
    CloudFixture fixture;
    fixture.richMessagesFixture = true;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    backend.start();
    backend.requestChatMessages(QStringLiteral("chat91"));
    QTRY_COMPARE(history.size(), 1);
    const auto messages = history.first().at(1).value<QVector<Message>>();
    QCOMPARE(messages.size(), 4);
    QVERIFY(messages.at(0).outgoing);
    QVERIFY(!messages.at(1).outgoing);
    QVERIFY(!messages.at(1).system);
    QVERIFY(messages.at(2).system);
    QVERIFY(!messages.at(2).outgoing);
    QVERIFY(!messages.at(3).system);
    QCOMPARE(messages.at(0).replyId, QStringLiteral("690"));
    QCOMPARE(messages.at(0).replyAuthor, QStringLiteral("Автор ответа"));
    QCOMPARE(messages.at(0).replyText, QStringLiteral("Исходная цитата"));
    QCOMPARE(messages.at(0).files.first().name, QStringLiteral("Расчёт.xlsx"));
    QCOMPARE(messages.at(0).files.first().size, 9);
    QCOMPARE(messages.at(0).replyFiles.size(), 2);
    QVERIFY(messages.at(0).replyFiles.last().downloadUrl.isEmpty());
    QCOMPARE(messages.at(1).authorAvatarUrl.path(), QStringLiteral("/upload/author.png"));
    QVERIFY(messages.at(1).forwarded);
    QCOMPARE(messages.at(1).forwardedAuthor, QStringLiteral("Источник"));
    QCOMPARE(fixture.requestsFor(QStringLiteral("main.bitrix.main.controller.loadext.resolveextensions")).size(), 1);
}

void TestCloudBackend::streamsOnlyRegisteredMessageFilesAndProtectsTargetOnMismatch()
{
    CloudFixture fixture;
    fixture.richMessagesFixture = true;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    QSignalSpy saved(&backend, &Backend::messageFileDownloaded);
    QSignalSpy failure(&backend, &Backend::failed);
    QTemporaryDir selection;
    QVERIFY(selection.isValid());
    backend.start();
    backend.requestChatMessages(QStringLiteral("chat91"));
    QTRY_COMPARE(history.size(), 1);
    const auto messages = history.first().at(1).value<QVector<Message>>();
    const auto requested = messages.first().files.first().downloadUrl;
    QVERIFY(requested.isValid());
    const auto path = selection.filePath(QStringLiteral("copy.xlsx"));
    backend.downloadMessageFile(requested, path);
    QTRY_COMPARE(saved.size(), 1);
    QCOMPARE(fixture.requestsFor(QStringLiteral("/disk/download.xlsx")).first().headers.value("cookie"), fixtureCookie);
    QFile actual(path);
    QVERIFY(actual.open(QIODevice::ReadOnly));
    QCOMPARE(actual.readAll(), QByteArray("xlsx-data"));
    actual.close();

    fixture.mismatchDownload = true;
    backend.downloadMessageFile(requested, path);
    QTRY_COMPARE(failure.size(), 1);
    QVERIFY(actual.open(QIODevice::ReadOnly));
    QCOMPARE(actual.readAll(), QByteArray("xlsx-data"));
    actual.close();

    backend.downloadMessageFile(requested.resolved(QUrl(QStringLiteral("/disk/unregistered.xlsx"))), path);
    QTRY_COMPARE(failure.size(), 2);
    QVERIFY(fixture.requestsFor(QStringLiteral("/disk/unregistered.xlsx")).isEmpty());

    CloudFixture foreign;
    QVERIFY(foreign.start());
    fixture.downloadRedirect = foreign.session().origin.resolved(QUrl(QStringLiteral("/disk/download.xlsx"))).toString();
    backend.downloadMessageFile(requested, path);
    QTRY_COMPARE(failure.size(), 3);
    QVERIFY(foreign.requests.isEmpty());
}

void TestCloudBackend::separatesChatScopesAndUsesPortalMetadata()
{
    CloudFixture fixture;
    fixture.richRecentFixture = true;
    fixture.taskChatSearchRows = {QJsonObject{{"id", 42}, {"chatId", 73}, {"groupId", 17}, {"title", "Проверить маршрут"}}};
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy ordinary(&backend, &Backend::recentChatsLoaded);
    QSignalSpy task(&backend, &Backend::taskChatsLoaded);
    QSignalSpy ordinarySearch(&backend, &Backend::chatsFound);
    QSignalSpy taskSearch(&backend, &Backend::taskChatsFound);
    backend.start();
    backend.requestRecentChats();
    QTRY_COMPARE(ordinary.size(), 1);
    const auto conversations = ordinary.first().first().value<QVector<Chat>>();
    QCOMPARE(conversations.size(), 2);
    QCOMPARE(conversations.at(0).id, QStringLiteral("103"));
    QCOMPARE(conversations.at(0).title, QStringLiteral("Имя из портала"));
    QCOMPARE(conversations.at(0).unread, 7);
    QCOMPARE(conversations.at(0).avatarUrl.path(), QStringLiteral("/upload/person.png"));
    QVERIFY(conversations.at(1).markedUnread);
    QVERIFY(conversations.at(1).pinned);
    QCOMPARE(conversations.at(1).unread, 0);

    backend.requestTaskChats();
    QTRY_VERIFY(!task.isEmpty());
    QTRY_COMPARE(task.last().first().value<QVector<Chat>>().first().title, QStringLiteral("Проверить маршрут"));
    const auto taskConversations = task.last().first().value<QVector<Chat>>();
    QCOMPARE(taskConversations.size(), 2);
    QVERIFY(taskConversations.first().taskChat);
    QCOMPARE(taskConversations.first().taskId, QStringLiteral("42"));
    QCOMPARE(taskConversations.first().unread, 3);
    QVERIFY(taskConversations.first().pinned);
    QCOMPARE(taskConversations.last().title, QStringLiteral("Старая задача"));
    QCOMPARE(fixture.requestsFor(QStringLiteral("im.v2.Recent.load")).last().field(QStringLiteral("filter[recentSection]")),
             QStringLiteral("tasksTask"));

    backend.searchChats(QStringLiteral("портал"));
    backend.searchTaskChats(QStringLiteral("маршрут"));
    QTRY_COMPARE(ordinarySearch.size(), 1);
    QTRY_COMPARE(taskSearch.size(), 1);
    const auto people = ordinarySearch.first().at(1).value<QVector<Chat>>();
    const auto tasks = taskSearch.first().at(1).value<QVector<Chat>>();
    QCOMPARE(people.size(), 1);
    QCOMPARE(people.first().title, QStringLiteral("Имя из портала"));
    QCOMPARE(people.first().kind, QStringLiteral("im"));
    QCOMPARE(tasks.size(), 1);
    QCOMPARE(tasks.first().title, QStringLiteral("Проверить маршрут"));
}

void TestCloudBackend::searchesTaskChatsByAccessibleTaskTitleAndNumber()
{
    CloudFixture fixture;
    fixture.messengerPage = R"HTML(<script>
        BX.Messenger.v2.Application.Launch('messenger', {"currentUser":{"id":11},
            "counters":[{"chatId":700,"counter":7,"type":"tasksTask"}]});
        </script>)HTML";
    fixture.taskChatSearchRows = {
        QJsonObject{{"id", "540"}, {"chatId", 700}, {"groupId", 17}, {"title", "Итоговый ПЛАН_2026"}},
        QJsonObject{{"id", 541}, {"chatId", 701}, {"groupId", 17}, {"title", "ПЛАНА2026"}},
        QJsonObject{{"id", 542}, {"chatId", 0}, {"groupId", 17}, {"title", "ПЛАН_2026"}},
        QJsonObject{{"id", 543}, {"chatId", 704}, {"CHAT_ID", 705}, {"groupId", 17}, {"title", "ПЛАН_2026"}},
        QJsonObject{{"id", 544}, {"chatId", 706}, {"groupId", 17}, {"GROUP_ID", 18}, {"title", "ПЛАН_2026"}},
        QJsonObject{{"id", 545}, {"chatId", 700}, {"groupId", 17}, {"title", "ПЛАН_2026"}},
        QJsonObject{{"id", 546}, {"ID", 547}, {"chatId", 707}, {"groupId", 17}, {"title", "ПЛАН_2026"}},
        QJsonObject{{"id", 548}, {"chatId", 708}, {"groupId", 17}, {"title", "ПЛАН_2026"}, {"TITLE", "Другая задача"}},
        QJsonObject{{"ID", "550"}, {"CHAT_ID", "710"}, {"GROUP_ID", "0"}, {"TITLE", "Личная задача"}}
    };
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy results(&backend, &Backend::taskChatsFound);
    backend.start();
    QTRY_COMPARE(backend.unreadChatCount().value_or(-1), 7);
    const auto url = QStringLiteral("/rest/tasks.task.list.json");
    const auto query = QStringLiteral("план_2026");
    backend.searchTaskChats(query);
    QTRY_COMPARE(results.size(), 1);
    const auto titleResults = results.last().at(1).value<QVector<Chat>>();
    QCOMPARE(titleResults.size(), 1);
    QCOMPARE(titleResults.first().taskId, QStringLiteral("540"));
    QCOMPARE(titleResults.first().id, QStringLiteral("chat700"));
    QCOMPARE(titleResults.first().serverChatId, 700);
    QCOMPARE(titleResults.first().unread, 7);
    QVERIFY(titleResults.first().taskChat);
    QCOMPARE(titleResults.first().title, QStringLiteral("Итоговый ПЛАН_2026"));
    QCOMPARE(results.last().first().toString(), query);
    const auto requests = fixture.requestsFor(url);
    QCOMPARE(requests.size(), 1);
    QCOMPARE(requests.first().method, QByteArray("POST"));
    QCOMPARE(requests.first().field(QStringLiteral("filter[%TITLE]")), query);
    QCOMPARE(requests.first().headers.value("cookie"), fixtureCookie);
    QCOMPARE(requests.first().field(QStringLiteral("sessid")).toLatin1(), fixtureCsrf);
    QStringList fields;
    for (const auto &[name, value] : requests.first().form.queryItems(QUrl::FullyDecoded))
        if (name == QLatin1String("select[]"))
            fields.append(value);
    fields.sort();
    QCOMPARE(fields, QStringList({QStringLiteral("CHAT_ID"), QStringLiteral("GROUP_ID"),
                                 QStringLiteral("ID"), QStringLiteral("TITLE")}));
    QVERIFY(fixture.requestsFor(QStringLiteral("ui.entityselector.doSearch")).isEmpty());
    QVERIFY(fixture.requestsFor(QStringLiteral("im.v2.Recent.load")).isEmpty());

    for (const auto &number : {QStringLiteral("550"), QStringLiteral("#550"), QStringLiteral("№ 550")}) {
        const auto before = results.size();
        backend.searchTaskChats(number);
        QTRY_COMPARE(results.size(), before + 1);
        const auto byNumber = results.last().at(1).value<QVector<Chat>>();
        QCOMPARE(byNumber.size(), 1);
        QCOMPARE(byNumber.first().taskId, QStringLiteral("550"));
        QCOMPARE(byNumber.first().id, QStringLiteral("chat710"));
        QCOMPARE(results.last().first().toString(), number);
        QCOMPARE(fixture.requestsFor(url).last().field(QStringLiteral("filter[ID]")), QStringLiteral("550"));
        QVERIFY(fixture.requestsFor(url).last().field(QStringLiteral("filter[%TITLE]")).isEmpty());
    }
    QSignalSpy errors(&backend, &Backend::failed);
    fixture.holdNotificationReply = url;
    backend.searchTaskChats(query);
    QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());
    QSignalSpy accounts(&backend, &Backend::accountScopeChanged);
    fixture.ordinaryRecentBadgeFixture = R"({"userId":22,"recentItems":[],"chats":[]})";
    backend.requestRecentChats();
    QTRY_COMPARE(accounts.size(), 1);
    fixture.taskChatSearchRows = {};
    backend.searchTaskChats(query);
    QTRY_COMPARE(results.size(), 5);
    QVERIFY(results.last().at(1).value<QVector<Chat>>().isEmpty());
    fixture.releaseHeldNotificationReply();
    QTest::qWait(50);
    QCOMPARE(results.size(), 5);
    QVERIFY(errors.isEmpty());
}

void TestCloudBackend::usesPortalPinActionsAndTheirAcknowledgements()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy chat(&backend, &Backend::chatPinChanged);
    QSignalSpy project(&backend, &Backend::projectPinChanged);
    QSignalSpy projects(&backend, &Backend::projectsLoaded);
    backend.start();
    backend.requestProjects();
    QTRY_COMPARE(projects.size(), 1);
    QCOMPARE(projects.last().first().value<QVector<Project>>().first().avatarUrl.path(), QStringLiteral("/upload/project.png"));
    backend.setChatPinned(QStringLiteral("chat73"), true);
    QTRY_COMPARE(chat.size(), 1);
    QCOMPARE(fixture.requestsFor(QStringLiteral("im.v2.Chat.pin")).first().field(QStringLiteral("dialogId")), QStringLiteral("chat73"));
    backend.setChatPinned(QStringLiteral("chat73"), false);
    QTRY_COMPARE(chat.size(), 2);
    QCOMPARE(fixture.requestsFor(QStringLiteral("im.v2.Chat.unpin")).size(), 1);
    backend.setProjectPinned(QStringLiteral("17"), true);
    QTRY_COMPARE(project.size(), 1);
    QVERIFY(project.first().at(1).toBool());
    const auto request = fixture.requestsFor(QStringLiteral("socialnetwork.v2.Project.changePin")).first();
    const auto json = QJsonDocument::fromJson(request.body).object();
    QCOMPARE(json.value("projectId").toInt(), 17);
    QCOMPARE(json.value("mode").toString(), QString());
    backend.requestProjects();
    QTRY_COMPARE(projects.size(), 2);
    QVERIFY(projects.last().first().value<QVector<Project>>().first().pinned);
    CloudBackend anotherSession(fixture.session(), true);
    QSignalSpy restored(&anotherSession, &Backend::projectsLoaded);
    anotherSession.start();
    anotherSession.requestProjects();
    QTRY_COMPARE(restored.size(), 1);
    QVERIFY(restored.last().first().value<QVector<Project>>().first().pinned);
    backend.setProjectPinned(QStringLiteral("17"), true);
    QTRY_COMPARE(project.size(), 2);
    QVERIFY(!project.last().at(1).toBool());
}

void TestCloudBackend::readsPortalTaskProjectsGridAndKeepsPinsAcrossPageLoads_data()
{
    QTest::addColumn<QString>("navigation");
    QTest::newRow("query") << QStringLiteral("classic");
    QTest::newRow("query-with-page-size") << QStringLiteral("size");
    QTest::newRow("sef-with-page-size") << QStringLiteral("sef");
    QTest::newRow("ambiguous-unmarked-next") << QStringLiteral("ambiguous");
}

void TestCloudBackend::readsPortalTaskProjectsGridAndKeepsPinsAcrossPageLoads()
{
    QFETCH(QString, navigation);
    CloudFixture fixture;
    fixture.projectGridFixture = true;
    fixture.projectGridNavigation = navigation;
    fixture.pinnedProject = true;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy projects(&backend, &Backend::projectsLoaded);
    QSignalSpy more(&backend, &Backend::projectsMoreAvailable);
    QSignalSpy pins(&backend, &Backend::projectPinChanged);
    QSignalSpy failures(&backend, &Backend::failed);
    backend.start();
    backend.requestProjects();
    QTRY_COMPARE(projects.size(), 1);
    auto rows = projects.last().first().value<QVector<Project>>();
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.first().name, QStringLiteral("Схема & маршрут"));
    QVERIFY(rows.first().pinned);
    QCOMPARE(rows.first().avatarUrl.path(), QStringLiteral("/upload/project.png"));
    QCOMPARE(rows.first().avatarColor, QStringLiteral("#02BB9A"));
    QCOMPARE(rows.first().avatarVariant, QStringLiteral("guest"));
    QCOMPARE(more.last().first().toBool(), navigation != QLatin1String("ambiguous"));
    if (navigation == QLatin1String("ambiguous"))
        return;

    backend.setProjectPinned(QStringLiteral("17"), false);
    QTRY_COMPARE(pins.size(), 1);
    QCOMPARE(pins.last().at(1).toBool(), false);
    const auto pin = fixture.requestsFor(QStringLiteral("socialnetwork.v2.Project.changePin")).first();
    QCOMPARE(QJsonDocument::fromJson(pin.body).object().value("mode").toString(), QStringLiteral("tasks_project"));
    backend.requestMoreProjects();
    QTRY_COMPARE(projects.size(), 2);
    rows = projects.last().first().value<QVector<Project>>();
    QCOMPARE(rows.size(), 2);
    QVERIFY(!rows.first().pinned);
    QVERIFY(rows.last().pinned);
    QCOMPARE(rows.last().name, QStringLiteral("Команда Два"));
    QCOMPARE(rows.last().avatarColor, QStringLiteral("#B15EF5"));
    QVERIFY(rows.last().avatarUrl.isEmpty());
    QCOMPARE(more.last().first().toBool(), true);
    backend.requestMoreProjects();
    QTRY_COMPARE(projects.size(), 3);
    rows = projects.last().first().value<QVector<Project>>();
    QCOMPARE(rows.size(), 3);
    QCOMPARE(rows.last().id, QStringLiteral("50"));
    QVERIFY(rows.at(1).pinned);
    QCOMPARE(more.last().first().toBool(), false);
    QVERIFY(fixture.requestsFor(QStringLiteral("socialnetwork.api.workgroup.list")).isEmpty());
    QVERIFY(failures.isEmpty());
}

void TestCloudBackend::fallsBackToPaginatedProjectMembership()
{
    CloudFixture fixture;
    fixture.projectFallbackFixture = true;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy projects(&backend, &Backend::projectsLoaded);
    QSignalSpy scope(&backend, &Backend::projectsScopeLoaded);
    QSignalSpy more(&backend, &Backend::projectsMoreAvailable);
    backend.start();
    backend.requestProjects();
    QTRY_COMPARE(projects.size(), 1);
    const auto first = projects.last().first().value<QVector<Project>>();
    QCOMPARE(first.size(), 1);
    QCOMPARE(first.first().id, QStringLiteral("17"));
    QCOMPARE(first.first().avatarType, QStringLiteral("folder"));
    QCOMPARE(more.last().first().toBool(), true);
    backend.requestMoreProjects();
    QTRY_COMPARE(projects.size(), 2);
    const auto second = projects.last().first().value<QVector<Project>>();
    QCOMPARE(second.size(), 2);
    QCOMPARE(second.last().id, QStringLiteral("43"));
    QCOMPARE(second.last().avatarType, QStringLiteral("project-3"));
    QVERIFY(second.last().pinned);
    QCOMPARE(scope.size(), 2);
    QVERIFY(scope.first().first().toBool());
    QVERIFY(scope.last().first().toBool());
    QCOMPARE(more.last().first().toBool(), false);
    const auto requests = fixture.requestsFor(QStringLiteral("socialnetwork.api.workgroup.list"));
    QCOMPARE(requests.size(), 2);
    QCOMPARE(requests.first().field(QStringLiteral("params[mode]")), QStringLiteral("mobile"));
    QCOMPARE(requests.first().field(QStringLiteral("select[4]")), QStringLiteral("AVATAR_TYPE"));
    QCOMPARE(requests.last().field(QStringLiteral("start")), QStringLiteral("50"));
}

void TestCloudBackend::keepsLegacyProjectScopeEvenWhenLaterPagesHaveRoles()
{
    CloudFixture fixture;
    fixture.legacyProjectFallbackFixture = true;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy projects(&backend, &Backend::projectsLoaded);
    QSignalSpy scope(&backend, &Backend::projectsScopeLoaded);
    QSignalSpy pins(&backend, &Backend::projectPinChanged);
    backend.start();
    backend.requestProjects();
    QTRY_COMPARE(projects.size(), 1);
    backend.requestMoreProjects();
    QTRY_COMPARE(projects.size(), 2);
    const auto rows = projects.last().first().value<QVector<Project>>();
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.first().id, QStringLiteral("17"));
    QCOMPARE(rows.last().id, QStringLiteral("43"));
    QCOMPARE(scope.size(), 2);
    QVERIFY(!scope.first().first().toBool());
    QVERIFY(!scope.last().first().toBool());
    backend.setProjectPinned(QStringLiteral("43"), true);
    QTRY_COMPARE(pins.size(), 1);
    const auto request = fixture.requestsFor(QStringLiteral("socialnetwork.v2.Project.changePin")).first();
    QCOMPARE(QJsonDocument::fromJson(request.body).object().value("mode").toString(), QString());
}

void TestCloudBackend::loadsPersonalPresenceAndPinnedMessagesWithLiveChanges()
{
    CloudFixture fixture;
    fixture.presencePinsFixture = true;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy directory(&backend, &Backend::recentChatsLoaded);
    QSignalSpy contacts(&backend, &Backend::chatContactChanged);
    QSignalSpy pins(&backend, &Backend::pinnedMessagesLoaded);
    QSignalSpy connected(&backend, &Backend::connectionChanged);
    backend.start();
    backend.requestRecentChats();
    QTRY_VERIFY(!directory.isEmpty());
    QTRY_VERIFY(!connected.isEmpty() && connected.last().first().toBool());
    const auto chats = directory.first().first().value<QVector<Chat>>();
    QCOMPARE(chats.size(), 6);
    QCOMPARE(chats.first().kind, QStringLiteral("im"));
    QCOMPARE(chats.first().position, QStringLiteral("Инженер"));
    QVERIFY(chats.first().onlineUntil.isValid() && chats.first().onlineUntil < QDateTime::currentDateTimeUtc());
    QCOMPARE(chats.at(1).kind, QStringLiteral("im"));
    QVERIFY(!chats.at(1).onlineUntil.isValid());
    QCOMPARE(chats.at(2).kind, QStringLiteral("chat"));
    QVERIFY(!chats.at(2).onlineUntil.isValid());
    QCOMPARE(chats.at(3).kind, QStringLiteral("im"));
    QCOMPARE(chats.at(4).kind, QStringLiteral("im"));
    QCOMPARE(chats.at(5).kind, QStringLiteral("bot"));
    QVERIFY(chats.at(5).position.isEmpty());

    backend.requestPinnedMessages(QStringLiteral("103"));
    backend.requestChatMessages(QStringLiteral("103"));
    QTRY_COMPARE(pins.size(), 1);
    QCOMPARE(fixture.requestsFor(QStringLiteral("im.v2.Chat.load")).size(), 1);
    auto rows = pins.last().at(1).value<QVector<Message>>();
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.first().id, QStringLiteral("12"));
    QCOMPARE(rows.first().text, QStringLiteral("Первое важное"));
    QTRY_COMPARE(contacts.size(), 1);
    QCOMPARE(contacts.last().at(2).toString(), QStringLiteral("Ведущий инженер"));

    const auto now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    fixture.pushIm(QStringLiteral("person-on-line"), QStringLiteral("userStatus"),
                   {{"users", QJsonObject{{"103", QJsonObject{{"id", 103}, {"last_activity_date", now}}}}}},
                   0, QStringLiteral("online"));
    QTRY_COMPARE(contacts.size(), 2);
    QCOMPARE(contacts.last().at(0).toString(), QStringLiteral("103"));
    QVERIFY(contacts.last().at(1).toDateTime() > QDateTime::currentDateTimeUtc());
    QCOMPARE(contacts.last().at(2).toString(), QStringLiteral("Ведущий инженер"));

    fixture.pushIm(QStringLiteral("new-common-pin"), QStringLiteral("pinAdd"),
                   {{"pin", QJsonObject{{"chatId", 91}, {"messageId", 21}}},
                    {"additionalMessages", QJsonArray{QJsonObject{{"id", 21}, {"text", "Новое закрепление"}}}}});
    QTRY_COMPARE(pins.size(), 2);
    QCOMPARE(pins.last().at(1).value<QVector<Message>>().first().id, QStringLiteral("21"));
    fixture.pushIm(QStringLiteral("remove-common-pin"), QStringLiteral("pinDelete"), {{"chatId", 91}, {"messageId", 21}});
    QTRY_COMPARE(pins.size(), 3);
    QCOMPARE(pins.last().at(1).value<QVector<Message>>().first().id, QStringLiteral("12"));

    backend.requestPinnedMessages(QStringLiteral("chat73"));
    QTRY_COMPARE(pins.size(), 5);
    QCOMPARE(pins.last().at(0).toString(), QStringLiteral("chat73"));
    rows = pins.last().at(1).value<QVector<Message>>();
    QCOMPARE(rows.size(), 51);
    QCOMPARE(rows.last().id, QStringLiteral("50"));
    const auto tail = fixture.requestsFor(QStringLiteral("im.v2.Chat.Pin.tail")).first();
    QCOMPARE(tail.field(QStringLiteral("dialogId")), QStringLiteral("chat73"));
    QCOMPARE(tail.field(QStringLiteral("filter[lastId]")), QStringLiteral("51"));
    QCOMPARE(tail.field(QStringLiteral("order[id]")), QStringLiteral("DESC"));
}

void TestCloudBackend::derivesUnreadAndRefreshesOnReadEvents()
{
    CloudFixture fixture;
    fixture.unreadHistoryFixture = true;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy recent(&backend, &Backend::recentChatsLoaded);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    QSignalSpy online(&backend, &Backend::connectionChanged);
    backend.start();
    backend.requestRecentChats();
    QTRY_COMPARE(recent.size(), 1);
    QTRY_COMPARE(online.size(), 1);
    backend.requestChatMessages(QStringLiteral("chat91"));
    QTRY_COMPARE(history.size(), 1);
    const auto messages = history.last().at(1).value<QVector<Message>>();
    QCOMPARE(messages.size(), 4);
    QVERIFY(!messages.at(0).unread);
    QVERIFY(messages.at(1).unread);
    QVERIFY(!messages.at(2).unread);
    QVERIFY(!messages.at(3).unread);
    fixture.pushIm(QStringLiteral("viewed"), QStringLiteral("readMessageChat"), {{"chatId", 91}, {"dialogId", "chat91"}, {"lastId", 701}});
    QTRY_COMPARE(history.size(), 2);
    QVERIFY(!history.last().at(1).value<QVector<Message>>().at(1).unread);
}

void TestCloudBackend::gatesChatPopupsBySoundAndMessageSettings()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy online(&backend, &Backend::connectionChanged);
    QSignalSpy popups(&backend, &CloudBackend::notificationReceived);
    backend.setForeground(false);
    backend.start();
    QTRY_COMPARE(online.size(), 1);
    fixture.pushMessage(QStringLiteral("open"), QStringLiteral("901"));
    QTRY_COMPARE(popups.size(), 1);
    const auto settings = fixture.requestsFor(QStringLiteral("im.v2.Settings.General.list"));
    QCOMPARE(settings.size(), 1);
    QCOMPARE(settings.first().field(QStringLiteral("userId")), QStringLiteral("11"));

    fixture.pushIm(QStringLiteral("global-mute"), QStringLiteral("settingsUpdate"), {{"soundMuted", true}});
    fixture.pushMessage(QStringLiteral("important"), QStringLiteral("902"), 91, 22, true);
    fixture.pushIm(QStringLiteral("global-unmute"), QStringLiteral("settingsUpdate"), {{"soundMuted", false}, {"soundVolumeMessages", 0}});
    fixture.pushMessage(QStringLiteral("volume-zero"), QStringLiteral("903"));
    fixture.pushIm(QStringLiteral("volume-normal"), QStringLiteral("settingsUpdate"), {{"soundVolumeMessages", 100}});
    fixture.pushIm(QStringLiteral("chat-mute"), QStringLiteral("chatMuteNotify"), {{"dialogId", "chat91"}, {"muted", true}});
    fixture.pushMessage(QStringLiteral("muted-chat"), QStringLiteral("904"), 91, 22, true);
    fixture.pushIm(QStringLiteral("chat-unmute"), QStringLiteral("chatMuteNotify"), {{"dialogId", "chat91"}, {"muted", false}});
    fixture.pushMessage(QStringLiteral("message-mute"), QStringLiteral("905"), 91, 22, true, 0, true);
    fixture.pushMessage(QStringLiteral("stale"), QStringLiteral("906"), 91, 22, false, 45);
    fixture.pushMessage(QStringLiteral("own"), QStringLiteral("907"), 91, 11);
    fixture.pushMessage(QStringLiteral("duplicate-id"), QStringLiteral("901"));
    fixture.pushIm(QStringLiteral("unopened-muted"), QStringLiteral("messageChat"),
                   {{"chatId", 92}, {"dialogId", "chat92"}, {"notify", true},
                    {"chat", QJsonObject{{"92", QJsonObject{{"id", 92}, {"isMuted", true}}}}},
                    {"message", QJsonObject{{"id", 909}, {"authorId", 22}, {"text", "Muted"}}}});
    fixture.pushMessage(QStringLiteral("eligible"), QStringLiteral("908"));
    QTRY_COMPARE(popups.size(), 2);
    QCOMPARE(popups.last().first().value<Notification>().id, QStringLiteral("message:908"));

    fixture.serverSoundMuted = true;
    fixture.closePull();
    QTRY_COMPARE_WITH_TIMEOUT(online.size(), 3, 5000);
    QTRY_COMPARE(fixture.requestsFor(QStringLiteral("im.v2.Settings.General.list")).size(), 2);
    fixture.pushMessage(QStringLiteral("muted-while-disconnected"), QStringLiteral("910"));
    QTest::qWait(100);
    QCOMPARE(popups.size(), 2);
}

void TestCloudBackend::chatPopupsRespectExplicitDesktopDelivery_data()
{
    QTest::addColumn<bool>("desktopDelivery");
    QTest::addColumn<int>("messageVolume");
    QTest::newRow("desktop on, sound volume zero") << true << 0;
    QTest::newRow("desktop off, sound volume positive") << false << 100;
}

void TestCloudBackend::chatPopupsRespectExplicitDesktopDelivery()
{
    QFETCH(bool, desktopDelivery);
    QFETCH(int, messageVolume);
    CloudFixture fixture;
    QVERIFY(fixture.start());
    fixture.serverNotificationScheme = QStringLiteral("simple");
    fixture.serverSiteDelivery = desktopDelivery;
    fixture.serverMessageVolume = messageVolume;
    CloudBackend backend(fixture.session(), true);
    QSignalSpy online(&backend, &Backend::connectionChanged);
    QSignalSpy entries(&backend, &Backend::notificationsLoaded);
    QSignalSpy popups(&backend, &CloudBackend::notificationReceived);
    backend.setForeground(false);
    backend.start();
    QTRY_COMPARE(online.size(), 1);
    fixture.pushMessage(QStringLiteral("desktop-before-toggle"), QStringLiteral("901"));
    QTRY_COMPARE(entries.size(), 1);
    QTRY_COMPARE(fixture.requestsFor(QStringLiteral("im.v2.Settings.General.list")).size(), 1);
    if (desktopDelivery)
        QTRY_COMPARE_WITH_TIMEOUT(popups.size(), 1, 500);
    else
        QTest::qWait(120);
    QCOMPARE(popups.size(), desktopDelivery ? 1 : 0);

    fixture.pushIm(QStringLiteral("desktop-settings-toggle"), QStringLiteral("settingsUpdate"),
                   {{"notifySchemeSendSite", !desktopDelivery}});
    fixture.pushMessage(QStringLiteral("desktop-after-toggle"), QStringLiteral("902"));
    QTRY_COMPARE(entries.size(), 2);
    QTRY_COMPARE(popups.size(), 1);
    const auto notice = popups.first().first().value<Notification>();
    QCOMPARE(notice.id, desktopDelivery ? QStringLiteral("message:901") : QStringLiteral("message:902"));
}

void TestCloudBackend::refreshesDesktopDeliveryWhenReturningToSimpleScheme_data()
{
    QTest::addColumn<bool>("usePush");
    QTest::newRow("scheme-only Push") << true;
    QTest::newRow("settings snapshots") << false;
}

void TestCloudBackend::refreshesDesktopDeliveryWhenReturningToSimpleScheme()
{
    QFETCH(bool, usePush);
    CloudFixture fixture;
    QVERIFY(fixture.start());
    fixture.serverNotificationScheme = QStringLiteral("simple");
    fixture.serverMessageVolume = 0;
    CloudBackend backend(fixture.session(), true);
    QSignalSpy online(&backend, &Backend::connectionChanged);
    QSignalSpy entries(&backend, &Backend::notificationsLoaded);
    QSignalSpy popups(&backend, &CloudBackend::notificationReceived);
    QSignalSpy settings(&backend, &Backend::notificationSettingsLoaded);
    backend.setForeground(false);
    backend.start();
    const auto list = QStringLiteral("im.v2.Settings.General.list");
    QTRY_COMPARE(online.size(), 1);
    fixture.pushMessage(QStringLiteral("desktop-on"), QStringLiteral("901"));
    QTRY_COMPARE(popups.size(), 1);
    QCOMPARE(fixture.requestsFor(list).size(), 1);

    if (usePush) {
        fixture.pushIm(QStringLiteral("desktop-use-expert"), QStringLiteral("settingsUpdate"),
                       {{"notifyScheme", "expert"}});
        fixture.serverSiteDelivery = false;
        fixture.pushIm(QStringLiteral("desktop-return-to-simple"), QStringLiteral("settingsUpdate"),
                       {{"notifyScheme", "simple"}});
    } else {
        fixture.omitDesktopDelivery = true;
        fixture.serverNotificationScheme = QStringLiteral("expert");
        backend.requestNotificationSettings();
        QTRY_COMPARE(settings.size(), 1);
        fixture.serverNotificationScheme = QStringLiteral("simple");
        backend.requestNotificationSettings();
        QTRY_COMPARE(settings.size(), 2);
    }
    fixture.pushMessage(QStringLiteral("desktop-disabled"), QStringLiteral("902"));
    QTRY_COMPARE(entries.size(), 2);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.requestsFor(list).size(), usePush ? 2 : 3, 500);
    QCOMPARE(popups.size(), 1);
}

void TestCloudBackend::deliversChatPopupsWithExpertSettingsFromPush()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    const auto settingsRequest = QStringLiteral("im.v2.Settings.General.list");
    fixture.holdNotificationReply = settingsRequest;
    CloudBackend backend(fixture.session(), true);
    QSignalSpy online(&backend, &Backend::connectionChanged);
    QSignalSpy recent(&backend, &Backend::recentChatsLoaded);
    QSignalSpy popups(&backend, &CloudBackend::notificationReceived);
    backend.setForeground(false);
    backend.start();
    backend.requestRecentChats();
    QTRY_COMPARE(online.size(), 1);
    QTRY_VERIFY(!recent.isEmpty());
    fixture.pushIm(QStringLiteral("desktop-expert-settings"), QStringLiteral("settingsUpdate"),
                   {{"notifyScheme", "expert"}, {"soundMuted", false},
                    {"soundVolumeGeneral", 100}, {"soundVolumeMessages", 100}});
    fixture.pushMessage(QStringLiteral("desktop-expert-message"), QStringLiteral("901"));
    QTRY_COMPARE_WITH_TIMEOUT(popups.size(), 1, 500);
    QVERIFY(fixture.requestsFor(settingsRequest).isEmpty());
}

void TestCloudBackend::resolvesCurrentCreationOwnerWithoutRecentIdentity()
{
    CloudFixture fixture;
    fixture.omitRecentUser = true;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy recent(&backend, &Backend::recentChatsLoaded);
    QSignalSpy createdTask(&backend, &Backend::taskCreated);
    QSignalSpy createdProject(&backend, &Backend::projectCreated);
    backend.start();
    backend.requestRecentChats();
    QTRY_COMPARE(recent.count(), 1);
    TaskEdit edit;
    edit.title = QStringLiteral("Задача без id в чатах");
    backend.createTask(QStringLiteral("17"), edit);
    QTRY_COMPARE(createdTask.count(), 1);
    const auto lookup = fixture.requestsFor(QStringLiteral("main.bitrix.main.controller.loadext.resolveextensions"));
    QCOMPARE(lookup.size(), 1);
    QCOMPARE(QUrlQuery(lookup.first().url).queryItemValue(QStringLiteral("e")), QStringLiteral("socialnetwork.v2.current-user"));
    QCOMPARE(lookup.first().field(QStringLiteral("extension[0]")), QStringLiteral("socialnetwork.v2.current-user"));
    QCOMPARE(lookup.first().field(QStringLiteral("coreLoaded")), QStringLiteral("Y"));
    const auto taskRequest = fixture.requestsFor(QStringLiteral("tasks.v2.Task.add")).first();
    const auto task = QJsonDocument::fromJson(taskRequest.body).object().value("task").toObject();
    QCOMPARE(task.value("responsible").toObject().value("id").toInt(), 11);
    backend.createProject(QStringLiteral("Проект с подтверждённым владельцем"));
    QTRY_COMPARE(createdProject.count(), 1);
    QCOMPARE(fixture.requestsFor(QStringLiteral("main.bitrix.main.controller.loadext.resolveextensions")).size(), 1);
}

void TestCloudBackend::createsTasksWithSelectedRequisites()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy created(&backend, &Backend::taskCreated);
    QSignalSpy child(&backend, &Backend::subtaskCreated);
    QSignalSpy failure(&backend, &Backend::failed);
    backend.start();

    const auto projectId = QStringLiteral("23");
    const auto parentId = QStringLiteral("42");
    TaskEdit draft;
    draft.title = QStringLiteral("Новая задача с реквизитами");
    draft.creatorId = QStringLiteral("77");
    draft.assigneeId = QStringLiteral("88");
    draft.stageId = QStringLiteral("60");
    draft.observerIds = QStringList{QStringLiteral("33"), QStringLiteral("55")};

    backend.createTask(projectId, draft);
    QTRY_COMPARE(created.count(), 1);
    QCOMPARE(created.first().first().toString(), projectId);

    backend.createSubtask(parentId, projectId, draft);
    QTRY_COMPARE(child.count(), 1);
    QCOMPARE(child.first().at(0).toString(), parentId);
    QCOMPARE(child.first().at(1).toString(), projectId);

    const auto requests = fixture.requestsFor(QStringLiteral("tasks.v2.Task.add"));
    QCOMPARE(requests.size(), 2);
    for (const auto &request : requests) {
        const auto task = QJsonDocument::fromJson(request.body).object().value("task").toObject();
        QCOMPARE(task.value("creator").toObject(), QJsonObject({{"id", 77}}));
        QCOMPARE(task.value("responsible").toObject(), QJsonObject({{"id", 88}}));
        QCOMPARE(task.value("responsibleCollection").toArray(), QJsonArray({QJsonObject{{"id", 88}}}));
        QCOMPARE(task.value("group").toObject(), QJsonObject({{"id", 23}}));
        QCOMPARE(task.value("stage").toObject(), QJsonObject({{"id", 60}}));
        QCOMPARE(task.value("auditors").toArray(), QJsonArray({QJsonObject{{"id", 33}}, QJsonObject{{"id", 55}}}));
    }
    const auto subtask = QJsonDocument::fromJson(requests.last().body).object().value("task").toObject();
    QCOMPARE(subtask.value("parent").toObject(), QJsonObject({{"id", 42}}));
    QCOMPARE(failure.count(), 0);

    draft.stageId = QStringLiteral("не стадия");
    backend.createTask(projectId, draft);
    QTRY_COMPARE(failure.count(), 1);
    QCOMPARE(failure.first().first().toString(), QStringLiteral("create-task:23"));

    draft.stageId = QStringLiteral("60");
    backend.createSubtask(parentId, {}, draft);
    QTRY_COMPARE(failure.count(), 2);
    QCOMPARE(failure.last().first().toString(), QStringLiteral("create-subtask:42"));
    QCOMPARE(fixture.requestsFor(QStringLiteral("tasks.v2.Task.add")).size(), 2);

    draft.stageId.reset();
    backend.createSubtask(parentId, {}, draft);
    QTRY_COMPARE(child.count(), 2);
    QVERIFY(child.last().at(1).toString().isEmpty());
    const auto detached = fixture.requestsFor(QStringLiteral("tasks.v2.Task.add")).last();
    const auto detachedTask = QJsonDocument::fromJson(detached.body).object().value("task").toObject();
    QCOMPARE(detachedTask.value("group").toObject(), QJsonObject({{"id", 0}}));
    QVERIFY(!detachedTask.contains("stage"));
}

void TestCloudBackend::refusesToDeleteStageWithoutConfirmedEmptyTotal()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    fixture.extendedStageFixture = true;
    CloudBackend backend(fixture.session(), true);
    QSignalSpy board(&backend, &Backend::boardLoaded);
    QSignalSpy failed(&backend, &Backend::failed);
    backend.start();
    backend.requestProjectBoard(QStringLiteral("17"));
    QTRY_COMPARE(board.count(), 1);
    const auto stages = board.first().at(1).value<QVector<Stage>>();
    QVERIFY(stages.at(1).tasks.isEmpty());
    QVERIFY(stages.at(1).taskCount.has_value());
    QCOMPARE(*stages.at(1).taskCount, 6);
    QVERIFY(!stages.at(2).taskCount.has_value());
    backend.deleteStage(QStringLiteral("17"), QStringLiteral("80"));
    QTRY_COMPARE(failed.count(), 1);
    backend.deleteStage(QStringLiteral("17"), QStringLiteral("90"));
    QTRY_COMPARE(failed.count(), 2);
    backend.deleteStage(QStringLiteral("17"), QStringLiteral("999"));
    QTRY_COMPARE(failed.count(), 3);
    QVERIFY(fixture.requestsFor(QStringLiteral("modifyColumn")).isEmpty());
}

void TestCloudBackend::loadsBoardColumnsLazilyWithoutSkippingFirstPage()
{
    CloudFixture fixture;
    fixture.boardPagingFixture = true;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy board(&backend, &Backend::boardLoaded);
    QSignalSpy page(&backend, &Backend::boardTasksPageLoaded);
    QSignalSpy failed(&backend, &Backend::failed);
    backend.start();
    const auto project = QStringLiteral("17");
    const auto stage = QStringLiteral("60");
    backend.requestProjectBoard(project);
    QTRY_COMPARE(board.count(), 1);
    const auto initial = board.first().at(1).value<QVector<Stage>>();
    QCOMPARE(initial.size(), 3);
    QVERIFY(!initial.at(0).hasMoreTasks);
    const auto first = initial.at(0).tasks.first();
    QCOMPARE(first.tags, (QStringList{QStringLiteral("Обсудить"), QStringLiteral("Бэкенд")}));
    QCOMPARE(first.creatorId, QStringLiteral("11"));
    QCOMPARE(first.createdBy, QStringLiteral("Постановщик"));
    QCOMPARE(first.creatorAvatarUrl.path(), QStringLiteral("/upload/resize_cache/user/author.png"));
    QCOMPARE(first.assigneeId, QStringLiteral("22"));
    QCOMPARE(first.assignee, QStringLiteral("Исполнитель"));
    QCOMPARE(first.assigneeAvatarUrl.path(), QStringLiteral("/upload/resize_cache/user/assignee.png"));
    QVERIFY(initial.at(1).tasks.isEmpty());
    QVERIFY(initial.at(1).hasMoreTasks);
    QVERIFY(fixture.requestsFor(QStringLiteral("getColumnItems")).isEmpty());

    backend.requestMoreBoardTasks(project, stage);
    QTRY_COMPARE(page.count(), 1);
    QCOMPARE(page.last().at(0).toString(), project);
    QCOMPARE(page.last().at(1).toString(), stage);
    auto tasks = page.last().at(2).value<QVector<Task>>();
    QCOMPARE(tasks.size(), 1);
    QCOMPARE(tasks.first().id, QStringLiteral("42"));
    QVERIFY(page.last().at(3).toBool());
    QVERIFY(fixture.requestsFor(QStringLiteral("getColumnItems")).isEmpty());

    fixture.badNextBoardPage = true;
    backend.requestMoreBoardTasks(project, stage);
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(page.count(), 1);
    QCOMPARE(fixture.requestsFor(QStringLiteral("getColumnItems")).last().field(QStringLiteral("pageId")),
             QStringLiteral("2"));

    backend.requestMoreBoardTasks(project, stage);
    backend.requestMoreBoardTasks(project, stage);
    QTRY_COMPARE(page.count(), 2);
    tasks = page.last().at(2).value<QVector<Task>>();
    QCOMPARE(tasks.size(), 1);
    QCOMPARE(tasks.first().id, QStringLiteral("43"));
    QCOMPARE(tasks.first().stageName, QStringLiteral("Рецензия"));
    QCOMPARE(tasks.first().tags, QStringList{QStringLiteral("Проверить")});
    QCOMPARE(tasks.first().createdBy, QStringLiteral("Автор страницы"));
    QCOMPARE(tasks.first().assignee, QStringLiteral("Исполнитель без фотографии"));
    QVERIFY(tasks.first().creatorAvatarUrl.isEmpty());
    QVERIFY(tasks.first().assigneeAvatarUrl.isEmpty());
    QVERIFY(page.last().at(3).toBool());
    const auto firstRequest = fixture.requestsFor(QStringLiteral("getColumnItems"));
    QCOMPARE(firstRequest.size(), 2);
    QCOMPARE(firstRequest.first().field(QStringLiteral("pageId")), QStringLiteral("2"));
    QCOMPARE(firstRequest.first().field(QStringLiteral("columnId")), stage);
    QCOMPARE(firstRequest.first().field(QStringLiteral("params[GROUP_ID]")), project);

    backend.requestMoreBoardTasks(project, stage);
    QTRY_COMPARE(page.count(), 3);
    tasks = page.last().at(2).value<QVector<Task>>();
    QCOMPARE(tasks.size(), 1);
    QCOMPARE(tasks.first().id, QStringLiteral("44"));
    QVERIFY(!page.last().at(3).toBool());
    QCOMPARE(fixture.requestsFor(QStringLiteral("getColumnItems")).last().field(QStringLiteral("pageId")),
             QStringLiteral("3"));
    backend.requestMoreBoardTasks(project, QStringLiteral("70"));
    QTRY_COMPARE(page.count(), 4);
    QVERIFY(page.last().at(2).value<QVector<Task>>().isEmpty());
    QVERIFY(!page.last().at(3).toBool());
    backend.requestMoreBoardTasks(QStringLiteral("18"), stage);
    backend.requestMoreBoardTasks(project, QStringLiteral("999"));
    QCOMPARE(failed.count(), 3);
    QCOMPARE(fixture.requestsFor(QStringLiteral("getColumnItems")).size(), 4);
    QVERIFY(fixture.requestsFor(QStringLiteral("tasks.task.get")).isEmpty());
    QVERIFY(fixture.requestsFor(QStringLiteral("tasks.v2.Task.get")).isEmpty());
}

void TestCloudBackend::discardsBoardPagesFromPreviousSnapshots()
{
    CloudFixture fixture;
    fixture.boardPagingFixture = true;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy board(&backend, &Backend::boardLoaded);
    QSignalSpy page(&backend, &Backend::boardTasksPageLoaded);
    QSignalSpy failed(&backend, &Backend::failed);
    backend.start();
    const auto project = QStringLiteral("17");
    const auto stage = QStringLiteral("60");
    backend.requestProjectBoard(project);
    QTRY_COMPARE(board.count(), 1);
    backend.requestMoreBoardTasks(project, stage);
    QTRY_COMPARE(page.count(), 1);
    fixture.holdNextBoardPage = true;
    fixture.badNextBoardPage = true;
    backend.requestMoreBoardTasks(project, stage);
    QTRY_COMPARE(fixture.requestsFor(QStringLiteral("getColumnItems")).size(), 1);
    backend.requestProjectBoard(QStringLiteral("18"));
    QTRY_COMPARE(board.count(), 2);
    backend.requestProjectBoard(project);
    QTRY_COMPARE(board.count(), 3);

    backend.requestMoreBoardTasks(project, stage);
    QTRY_COMPARE(page.count(), 2);
    backend.requestMoreBoardTasks(project, stage);
    QTRY_COMPARE(page.count(), 3);
    fixture.releaseHeldBoardResponse();
    QTest::qWait(70);
    QCOMPARE(page.count(), 3);
    QVERIFY(failed.isEmpty());
    QCOMPARE(page.last().at(2).value<QVector<Task>>().first().id, QStringLiteral("43"));
}

void TestCloudBackend::findsProjectTasksOutsideInitialBoard()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy found(&backend, &Backend::projectTasksFound);
    backend.start();
    backend.searchProjectTasks(QStringLiteral("17"), QStringLiteral("смету"));
    QTRY_COMPARE(found.count(), 1);
    QCOMPARE(found.first().first().toString(), QStringLiteral("17"));
    QCOMPARE(found.first().at(1).toString(), QStringLiteral("смету"));
    const auto matches = found.first().at(2).value<QVector<Task>>();
    QCOMPARE(matches.size(), 1);
    QCOMPARE(matches.first().id, QStringLiteral("149"));
    QCOMPARE(matches.first().stageId, QStringLiteral("60"));
    const auto request = fixture.requestsFor(QStringLiteral("tasks.task.list")).first();
    QCOMPARE(request.url.path(), ajaxPath);
    QCOMPARE(request.field(QStringLiteral("filter[GROUP_ID]")), QStringLiteral("17"));
    QCOMPARE(request.field(QStringLiteral("filter[TITLE]")), QStringLiteral("%смету%"));

    backend.searchProjectTasks(QStringLiteral("17"), QStringLiteral("#149"));
    QTRY_COMPARE(found.count(), 2);
    const auto byId = fixture.requestsFor(QStringLiteral("tasks.task.list")).last();
    QCOMPARE(byId.field(QStringLiteral("filter[ID]")), QStringLiteral("149"));
    QVERIFY(byId.field(QStringLiteral("filter[TITLE]")).isEmpty());

    backend.searchProjectTasks(QStringLiteral("17"), QStringLiteral("№ 149"));
    QTRY_COMPARE(found.count(), 3);
    const auto byNumber = fixture.requestsFor(QStringLiteral("tasks.task.list")).last();
    QCOMPARE(byNumber.field(QStringLiteral("filter[ID]")), QStringLiteral("149"));
    QVERIFY(byNumber.field(QStringLiteral("filter[TITLE]")).isEmpty());
}

void TestCloudBackend::loadsObserverAvatarsWithoutIndividualProfileRequests_data()
{
    QTest::addColumn<QByteArray>("legacyRows");
    QTest::newRow("differently-keyed") << QByteArray(R"({
        "33":{"id":88,"name":"Другой","icon":"/upload/resize_cache/other.png"},
        "legacy-row":{"id":33,"name":"Старое имя","icon":"/upload/resize_cache/task-auditor-legacy.png"},
        "explicit-empty":{"id":55,"name":"Без фото","icon":"/upload/resize_cache/old.png"}})");
    QTest::newRow("array") << QByteArray(R"([
        {"id":88,"name":"Другой","icon":"/upload/resize_cache/other.png"},
        {"id":33,"name":"Старое имя","icon":"/upload/resize_cache/task-auditor-legacy.png"},
        {"id":55,"name":"Без фото","icon":"/upload/resize_cache/old.png"}])");
}

void TestCloudBackend::loadsObserverAvatarsWithoutIndividualProfileRequests()
{
    QFETCH(QByteArray, legacyRows);
    CloudFixture fixture;
    fixture.editableTaskFixture = true;
    fixture.taskAuditorIds = {13, 33, 44, 55};
    fixture.legacyTaskAuditors = legacyRows;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy loaded(&backend, &Backend::taskLoaded);
    backend.start();
    backend.requestTask(QStringLiteral("42"));
    QTRY_COMPARE(loaded.size(), 1);
    const auto people = loaded.first().first().value<Task>().observerPeople;
    QCOMPARE(people.size(), 4);
    QCOMPARE(people.at(0).id, QStringLiteral("13"));
    QCOMPARE(people.at(0).avatarUrl.path(), QStringLiteral("/upload/resize_cache/task-auditor-modern.png"));
    QCOMPARE(people.at(1).id, QStringLiteral("33"));
    QCOMPARE(people.at(1).name, QStringLiteral("Наблюдатель"));
    QCOMPARE(people.at(1).avatarUrl.path(), QStringLiteral("/upload/resize_cache/task-auditor-legacy.png"));
    QVERIFY(people.at(2).avatarUrl.isEmpty());
    QVERIFY(people.at(3).avatarUrl.isEmpty());
    QVERIFY(fixture.requestsFor(QStringLiteral("im.user.get")).isEmpty());

    fixture.taskAuditorIds.clear();
    backend.requestTask(QStringLiteral("42"));
    QTRY_COMPARE(loaded.size(), 2);
    QVERIFY(loaded.last().first().value<Task>().observerPeople.isEmpty());
}

void TestCloudBackend::discardsTaskProjectAndStageRepliesFromPreviousAccount_data()
{
    QTest::addColumn<bool>("stage");
    QTest::addColumn<bool>("holdWrite");
    QTest::addColumn<bool>("failReply");
    QTest::newRow("project-preflight") << false << false << false;
    QTest::newRow("stage-preflight") << true << false << false;
    QTest::newRow("project-success") << false << true << false;
    QTest::newRow("stage-success") << true << true << false;
    QTest::newRow("project-failure") << false << true << true;
    QTest::newRow("stage-failure") << true << true << true;
}

void TestCloudBackend::discardsTaskProjectAndStageRepliesFromPreviousAccount()
{
    QFETCH(bool, stage);
    QFETCH(bool, holdWrite);
    QFETCH(bool, failReply);
    CloudFixture fixture;
    fixture.editableTaskFixture = true;
    fixture.ordinaryRecentBadgeFixture = R"({"userId":11,"recentItems":[],"chats":[]})";
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy accounts(&backend, &Backend::accountScopeChanged);
    QSignalSpy updates(&backend, &Backend::taskUpdated);
    QSignalSpy failures(&backend, &Backend::failed);
    backend.start();
    backend.requestRecentChats();
    QTRY_COMPARE(accounts.size(), 1);

    TaskEdit edit;
    if (stage)
        edit.stageId = QStringLiteral("70");
    else
        edit.projectId = QStringLiteral("18");
    const auto taskId = QStringLiteral("42");
    const auto preflight = QStringLiteral("tasks.v2.Task.get");
    const auto write = stage ? QStringLiteral("moveStage") : QStringLiteral("tasks.v2.Task.update");
    fixture.holdNotificationReply = holdWrite ? write : preflight;
    backend.updateTask(taskId, edit);
    QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());
    backend.updateTask(taskId, edit);

    fixture.ordinaryRecentBadgeFixture = R"({"userId":22,"recentItems":[],"chats":[]})";
    backend.requestRecentChats();
    QTRY_COMPARE(accounts.size(), 2);
    QCOMPARE(fixture.requestsFor(preflight).size(), 1);
    fixture.releaseHeldNotificationReply(failReply);
    QTest::qWait(50);
    QCOMPARE(fixture.requestsFor(write).size(), holdWrite ? 1 : 0);
    QVERIFY(updates.isEmpty());
    QVERIFY(std::none_of(failures.cbegin(), failures.cend(), [](const QList<QVariant> &failure) {
        return failure.first().toString() == QLatin1String("update-task:42");
    }));

    backend.updateTask(taskId, edit);
    QTRY_COMPARE(updates.size(), 1);
    QCOMPARE(updates.first().first().toString(), taskId);
    QCOMPARE(fixture.requestsFor(write).size(), holdWrite ? 2 : 1);
}

void TestCloudBackend::loadsTaskRequisitesAndUsesVerifiedWriteActions()
{
    CloudFixture fixture;
    fixture.editableTaskFixture = true;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy loaded(&backend, &Backend::taskLoaded);
    QSignalSpy projectStages(&backend, &Backend::projectStagesLoaded);
    QSignalSpy updated(&backend, &Backend::taskUpdated);
    QSignalSpy failed(&backend, &Backend::failed);
    backend.start();
    backend.requestTask(QStringLiteral("42"));
    QTRY_COMPARE(loaded.count(), 1);
    const auto task = loaded.first().at(0).value<Task>();
    QCOMPARE(task.creatorId, QStringLiteral("11"));
    QCOMPARE(task.assigneeId, QStringLiteral("12"));
    QCOMPARE(task.tags, QStringList{QStringLiteral("Регрессия")});
    QCOMPARE(task.creatorAvatarUrl.host(), QStringLiteral("cdn-ru.bitrix24.ru"));
    QVERIFY(task.assigneeAvatarUrl.isEmpty());
    QCOMPARE(task.observerPeople.size(), 1);
    QCOMPARE(task.observerPeople.first().id, QStringLiteral("13"));
    QCOMPARE(task.observerPeople.first().name, QStringLiteral("Наблюдатель"));
    QCOMPARE(task.stageName, QStringLiteral("Проверка"));
    QCOMPARE(task.statusCode, QStringLiteral("pending"));
    QVERIFY(task.editRights.creator && task.editRights.assignee && task.editRights.observers);
    QCOMPARE(task.statusActions, (QVector<TaskStatusAction>{TaskStatusAction::Start, TaskStatusAction::Complete}));
    backend.requestProjectStages(task.projectId);
    QTRY_COMPARE(projectStages.count(), 1);
    QCOMPARE(projectStages.first().at(0).toString(), QStringLiteral("17"));
    const auto choices = projectStages.first().at(1).value<QVector<Stage>>();
    QCOMPARE(choices.size(), 2);
    QCOMPARE(choices.first().name, QStringLiteral("Проверка"));
    QCOMPARE(choices.last().name, QStringLiteral("Завершение"));
    QCOMPARE(choices.first().color, QStringLiteral("00adf2"));
    QVERIFY(choices.last().color.isEmpty());

    TaskEdit creator;
    creator.creatorId = QStringLiteral("22");
    backend.updateTask(task.id, creator);
    QTRY_COMPARE(updated.count(), 1);
    const auto writePayload = [&fixture](const QString &action) {
        const auto requests = fixture.requestsFor(action);
        return requests.isEmpty() ? QJsonObject{} : QJsonDocument::fromJson(requests.last().body).object();
    };
    const auto creatorWrite = writePayload(QStringLiteral("tasks.v2.Task.Stakeholder.Creator.update"))
        .value(QStringLiteral("task")).toObject();
    QCOMPARE(writePayload(QStringLiteral("tasks.v2.Group.Stage.list")), QJsonObject({{"group", QJsonObject{{"id", 17}}}}));
    QCOMPARE(creatorWrite.value(QStringLiteral("creator")).toObject(), QJsonObject({{"id", 22}}));
    QCOMPARE(creatorWrite.value(QStringLiteral("responsible")).toObject(), QJsonObject({{"id", 12}}));

    TaskEdit observers;
    observers.observerIds = QStringList{QStringLiteral("13"), QStringLiteral("23"), QStringLiteral("24")};
    observers.originalObserverIds = QStringList{QStringLiteral("13")};
    backend.updateTask(task.id, observers);
    QTRY_COMPARE(updated.count(), 2);
    const auto added = writePayload(QStringLiteral("tasks.v2.Task.Stakeholder.Auditor.add")).value("task").toObject();
    QCOMPARE(added.value("auditors").toArray(), QJsonArray({QJsonObject{{"id", 23}}, QJsonObject{{"id", 24}}}));
    observers.originalObserverIds = observers.observerIds;
    observers.observerIds = QStringList{QStringLiteral("23")};
    backend.updateTask(task.id, observers);
    QTRY_COMPARE(updated.count(), 3);
    const auto removed = writePayload(QStringLiteral("tasks.v2.Task.Stakeholder.Auditor.delete")).value("task").toObject();
    QCOMPARE(removed.value("auditors").toArray(), QJsonArray({QJsonObject{{"id", 13}}, QJsonObject{{"id", 24}}}));

    TaskEdit assignee;
    assignee.assigneeId = QStringLiteral("21");
    backend.updateTask(task.id, assignee);
    QTRY_COMPARE(updated.count(), 4);
    const auto assigned = writePayload(QStringLiteral("tasks.v2.Task.Stakeholder.Responsible.delegate"))
        .value("task").toObject();
    QCOMPARE(assigned.value("responsible").toObject(), QJsonObject({{"id", 21}}));
    QCOMPARE(assigned.value("responsibleCollection").toArray(), QJsonArray({QJsonObject{{"id", 21}}}));

    TaskEdit deadline;
    deadline.deadline = QStringLiteral("2027-01-02T12:30:00Z");
    deadline.deadlineReason = QStringLiteral("Согласовано");
    backend.updateTask(task.id, deadline);
    QTRY_COMPARE(updated.count(), 5);
    const auto dated = writePayload(QStringLiteral("tasks.v2.Task.Deadline.update")).value("task").toObject();
    QCOMPARE(dated.value("deadlineChangeReason").toString(), QStringLiteral("Согласовано"));
    QCOMPARE(dated.value("deadlineTs").toInteger(), QDateTime::fromString(*deadline.deadline, Qt::ISODate).toSecsSinceEpoch());

    TaskEdit status;
    status.statusAction = TaskStatusAction::Complete;
    backend.updateTask(task.id, status);
    QTRY_COMPARE(updated.count(), 6);
    QCOMPARE(writePayload(QStringLiteral("tasks.v2.Task.Status.complete")),
             QJsonObject({{"task", QJsonObject{{"id", 42}}}, {"completeWithChecklist", false}}));

    TaskEdit group;
    group.projectId = QStringLiteral("18");
    backend.updateTask(task.id, group);
    QTRY_COMPARE(updated.count(), 7);
    QCOMPARE(writePayload(QStringLiteral("tasks.v2.Task.update")).value("task").toObject(),
             QJsonObject({{"id", 42}, {"group", QJsonObject{{"id", 18}}}, {"stage", QJsonObject{{"id", 0}}}}));

    TaskEdit stage;
    stage.stageId = QStringLiteral("70");
    backend.updateTask(task.id, stage);
    QTRY_COMPARE(updated.count(), 8);
    const auto stageWrite = fixture.requestsFor(QStringLiteral("moveStage")).last();
    QCOMPARE(QUrlQuery(stageWrite.url).queryItemValue(QStringLiteral("c")), QStringLiteral("bitrix:tasks.task"));
    QCOMPARE(stageWrite.field(QStringLiteral("taskId")), QStringLiteral("42"));
    QCOMPARE(stageWrite.field(QStringLiteral("stageId")), QStringLiteral("70"));

    group.projectId = task.projectId;
    backend.updateTask(task.id, group);
    QTRY_COMPARE(updated.count(), 9);
    QCOMPARE(fixture.requestsFor(QStringLiteral("tasks.v2.Task.update")).size(), 1);

    group.projectId = QStringLiteral("0");
    backend.updateTask(task.id, group);
    QTRY_COMPARE(updated.count(), 10);
    QCOMPARE(writePayload(QStringLiteral("tasks.v2.Task.update")).value("task").toObject(),
             QJsonObject({{"id", 42}, {"group", QJsonObject{{"id", 0}}}, {"stage", QJsonObject{{"id", 0}}}}));

    TaskEdit unavailable;
    unavailable.statusAction = TaskStatusAction::Pause;
    backend.updateTask(task.id, unavailable);
    QTRY_COMPARE(failed.count(), 1);
    QVERIFY(fixture.requestsFor(QStringLiteral("tasks.v2.Task.Status.pause")).isEmpty());
    observers.originalObserverIds = QStringList{QStringLiteral("999")};
    backend.updateTask(task.id, observers);
    QTRY_COMPARE(failed.count(), 2);
    QCOMPARE(fixture.requestsFor(QStringLiteral("tasks.v2.Task.Stakeholder.Auditor.add")).size(), 1);
    observers.originalObserverIds = QStringList{QStringLiteral("23")};
    observers.observerIds = QStringList{QStringLiteral("24")};
    backend.updateTask(task.id, observers);
    QTRY_COMPARE(failed.count(), 3);
    QCOMPARE(fixture.requestsFor(QStringLiteral("tasks.v2.Task.Stakeholder.Auditor.add")).size(), 1);
    QCOMPARE(fixture.requestsFor(QStringLiteral("tasks.v2.Task.Stakeholder.Auditor.delete")).size(), 1);
    observers.observerIds = QStringList{QStringLiteral("23"), QStringLiteral("11")};
    backend.updateTask(task.id, observers);
    QTRY_COMPARE(failed.count(), 4);
    QVERIFY(fixture.requestsFor(QStringLiteral("tasks.v2.Task.Audit.watch")).isEmpty());
}

void TestCloudBackend::loadsExactTaskRatingAndExplicitActionRights()
{
    CloudFixture fixture;
    fixture.actionTaskLegacy = {{"id", "42"}, {"title", "Task"}, {"mark", "P"},
                                {"action", QJsonObject{{"remove", true}, {"rate", true}, {"delegate", true}}}};
    fixture.actionTaskCurrent = {{"id", 42}, {"responsible", QJsonObject{{"id", 22}}},
        {"rights", QJsonObject{{"changeResponsible", true}, {"delegate", false}, {"mark", true}, {"remove", false}}}};
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy loaded(&backend, &Backend::taskLoaded);
    backend.start();
    backend.requestTask(QStringLiteral("42"));
    QTRY_COMPARE(loaded.size(), 1);
    auto task = loaded.last().first().value<Task>();
    QCOMPARE(task.rating, TaskRating::Positive);
    QVERIFY(task.editRights.rate);
    QVERIFY(!task.editRights.remove);
    QVERIFY(task.editRights.assignee);
    QVERIFY(!task.editRights.delegate);

    fixture.actionTaskCurrent.insert(QStringLiteral("mark"), QStringLiteral("none"));
    fixture.actionTaskCurrent.insert(QStringLiteral("rights"), QJsonObject{{"delegate", true}, {"mark", true}, {"remove", true}});
    backend.requestTask(QStringLiteral("42"));
    QTRY_COMPARE(loaded.size(), 2);
    task = loaded.last().first().value<Task>();
    QCOMPARE(task.rating, TaskRating::Unrated);
    QVERIFY(task.editRights.assignee && task.editRights.delegate && task.editRights.remove);
    QVERIFY(fixture.requestsFor(QStringLiteral("tasks.task.get")).last().form.queryItems(QUrl::FullyDecoded)
                .contains({QStringLiteral("select[]"), QStringLiteral("MARK")}));
}

void TestCloudBackend::mutatesTaskRatingAndDeletionOnlyAfterExactPortalConfirmation()
{
    CloudFixture fixture;
    fixture.actionTaskCurrent = {{"id", 42}, {"mark", "none"}, {"rights", QJsonObject{{"mark", false}, {"remove", false}}}};
    fixture.taskRatingReply = {{"task", QJsonObject{{"id", 42}, {"mark", "P"}}}};
    fixture.taskDeletionReply = {{"task", true}};
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    backend.start();
    QSignalSpy updated(&backend, &Backend::taskUpdated);
    QSignalSpy deleted(&backend, &Backend::taskDeleted);
    QSignalSpy errors(&backend, &Backend::failed);
    const auto taskId = QStringLiteral("42");
    const auto ratingPath = QStringLiteral("/rest/tasks.task.update.json");
    const auto deletePath = QStringLiteral("/rest/tasks.task.delete.json");
    TaskEdit positive;
    positive.rating = TaskRating::Positive;
    backend.updateTask(taskId, positive);
    QTRY_COMPARE(errors.size(), 1);
    QVERIFY(fixture.requestsFor(ratingPath).isEmpty());
    backend.deleteTask(taskId);
    QTRY_COMPARE(errors.size(), 2);
    QVERIFY(fixture.requestsFor(deletePath).isEmpty());
    fixture.actionTaskCurrent.insert(QStringLiteral("rights"), QJsonObject{{"mark", true}, {"remove", true}});
    fixture.actionTaskCurrent.insert(QStringLiteral("ID"), 43);
    backend.deleteTask(taskId);
    QTRY_COMPARE(errors.size(), 3);
    QVERIFY(fixture.requestsFor(deletePath).isEmpty());
    fixture.actionTaskCurrent.remove(QStringLiteral("ID"));

    fixture.taskRatingReply = {{"task", QJsonObject{{"id", 43}, {"mark", "P"}}}};
    backend.updateTask(taskId, positive);
    QTRY_COMPARE(errors.size(), 4);
    QVERIFY(updated.isEmpty());
    fixture.taskRatingReply = {{"task", QJsonObject{{"id", 42}, {"mark", "P"}}}};
    backend.updateTask(taskId, positive);
    backend.updateTask(taskId, positive);
    QTRY_COMPARE(updated.size(), 1);
    QCOMPARE(updated.last().first().toString(), taskId);
    const auto ratings = fixture.requestsFor(ratingPath);
    QCOMPARE(ratings.size(), 2);
    const auto request = QJsonDocument::fromJson(ratings.last().body).object();
    QCOMPARE(request.value("taskId").toInteger(), 42);
    QCOMPARE(request.value("fields").toObject().value("MARK"), QJsonValue("P"));
    QCOMPARE(ratings.last().headers.value("x-bitrix-csrf-token"), fixtureCsrf);

    TaskEdit clear;
    clear.rating = TaskRating::Unrated;
    fixture.taskRatingReply = {{"task", QJsonObject{{"id", 42}, {"mark", QJsonValue::Null}}}};
    backend.updateTask(taskId, clear);
    QTRY_COMPARE(updated.size(), 2);
    QVERIFY(QJsonDocument::fromJson(fixture.requestsFor(ratingPath).last().body).object().value("fields")
                .toObject().value("MARK").isNull());

    fixture.taskDeletionReply = {{"task", false}};
    backend.deleteTask(taskId);
    QTRY_COMPARE(errors.size(), 5);
    QVERIFY(deleted.isEmpty());
    fixture.taskDeletionReply = {{"task", true}};
    backend.deleteTask(taskId);
    backend.deleteTask(taskId);
    QTRY_COMPARE(deleted.size(), 1);
    QCOMPARE(deleted.last().first().toString(), taskId);
    const auto deletions = fixture.requestsFor(deletePath);
    QCOMPARE(deletions.size(), 2);
    QCOMPARE(deletions.last().method, QByteArray("POST"));
    QCOMPARE(deletions.last().field(QStringLiteral("taskId")), taskId);

    const auto delegateAction = QStringLiteral("tasks.v2.Task.Stakeholder.Responsible.delegate");
    fixture.actionTaskCurrent.insert(QStringLiteral("responsible"), QJsonObject{{"id", 22}});
    fixture.actionTaskCurrent.insert(QStringLiteral("rights"), QJsonObject{{"changeResponsible", true}, {"delegate", false}});
    TaskEdit delegate;
    delegate.assigneeId = QStringLiteral("23");
    delegate.requireDelegate = true;
    backend.updateTask(taskId, delegate);
    QTRY_COMPARE(errors.size(), 6);
    QVERIFY(fixture.requestsFor(delegateAction).isEmpty());
    fixture.actionTaskCurrent.insert(QStringLiteral("rights"), QJsonObject{{"delegate", true}});
    backend.updateTask(taskId, delegate);
    QTRY_COMPARE(updated.size(), 3);
    QCOMPARE(fixture.requestsFor(delegateAction).size(), 1);
}

void TestCloudBackend::ignoresTaskActionResponsesFromPreviousAccounts()
{
    CloudFixture fixture;
    fixture.ordinaryRecentBadgeFixture = R"({"userId":11,"recentItems":[],"chats":[]})";
    fixture.actionTaskCurrent = {{"id", 42}, {"status", "pending"}, {"responsible", QJsonObject{{"id", 22}}},
        {"rights", QJsonObject{{"remove", true}, {"start", true}, {"delegate", true}}}};
    fixture.taskDeletionReply = {{"task", true}};
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy accounts(&backend, &Backend::accountScopeChanged);
    QSignalSpy deleted(&backend, &Backend::taskDeleted);
    QSignalSpy updated(&backend, &Backend::taskUpdated);
    QSignalSpy errors(&backend, &Backend::failed);
    backend.start();
    backend.requestRecentChats();
    QTRY_COMPARE(accounts.size(), 1);
    const auto taskId = QStringLiteral("42");
    const auto deletePath = QStringLiteral("/rest/tasks.task.delete.json");
    fixture.holdNotificationReply = QStringLiteral("tasks.v2.Task.get");
    backend.deleteTask(taskId);
    QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());
    fixture.ordinaryRecentBadgeFixture = R"({"userId":22,"recentItems":[],"chats":[]})";
    backend.requestRecentChats();
    QTRY_COMPARE(accounts.size(), 2);
    fixture.releaseHeldNotificationReply();
    QTest::qWait(50);
    QVERIFY(fixture.requestsFor(deletePath).isEmpty());

    fixture.holdNotificationReply = deletePath;
    backend.deleteTask(taskId);
    QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());
    QCOMPARE(fixture.requestsFor(deletePath).size(), 1);
    fixture.ordinaryRecentBadgeFixture = R"({"userId":11,"recentItems":[],"chats":[]})";
    backend.requestRecentChats();
    QTRY_COMPARE(accounts.size(), 3);
    backend.deleteTask(taskId);
    QTRY_COMPARE(deleted.size(), 1);
    QCOMPARE(fixture.requestsFor(deletePath).size(), 2);
    fixture.releaseHeldNotificationReply();
    QTest::qWait(50);
    QCOMPARE(deleted.size(), 1);

    const auto statusAction = QStringLiteral("tasks.v2.Task.Status.start");
    const auto delegateAction = QStringLiteral("tasks.v2.Task.Stakeholder.Responsible.delegate");
    TaskEdit start;
    start.statusAction = TaskStatusAction::Start;
    TaskEdit delegate;
    delegate.assigneeId = QStringLiteral("23");
    delegate.requireDelegate = true;
    const QList<QPair<QString, TaskEdit>> edits{{statusAction, start}, {delegateAction, delegate}};
    for (const auto &[action, edit] : edits) {
        fixture.holdNotificationReply = QStringLiteral("tasks.v2.Task.get");
        backend.updateTask(taskId, edit);
        QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());
        const auto accountCount = accounts.size();
        fixture.ordinaryRecentBadgeFixture = action == statusAction
            ? R"({"userId":22,"recentItems":[],"chats":[]})"
            : R"({"userId":11,"recentItems":[],"chats":[]})";
        backend.requestRecentChats();
        QTRY_COMPARE(accounts.size(), accountCount + 1);
        fixture.releaseHeldNotificationReply();
        QTest::qWait(50);
        QVERIFY(fixture.requestsFor(action).isEmpty());
    }
    QVERIFY(errors.isEmpty());
    QVERIFY(updated.isEmpty());
}

void TestCloudBackend::sendsTaskEditsAndKanbanMutations()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy taskUpdated(&backend, &Backend::taskUpdated);
    QSignalSpy taskCreated(&backend, &Backend::taskCreated);
    QSignalSpy projectCreated(&backend, &Backend::projectCreated);
    QSignalSpy boardUpdated(&backend, &Backend::boardUpdated);
    QSignalSpy failure(&backend, &Backend::failed);
    backend.start();
    backend.requestRecentChats();
    QTRY_VERIFY(!fixture.requestsFor(QStringLiteral("im.v2.Recent.load")).isEmpty());
    TaskEdit edit;
    edit.title = QStringLiteral("Проверить форму");
    edit.description = QString();
    edit.deadline = QStringLiteral("2026-12-01T12:15:00+03:00");
    edit.highPriority = false;
    backend.updateTask(QStringLiteral("42"), edit);
    QTRY_COMPARE(taskUpdated.count(), 1);
    const auto update = fixture.requestsFor(QStringLiteral("tasks.v2.Task.update")).first();
    QCOMPARE(update.headers.value("content-type"), QByteArray("application/json"));
    QCOMPARE(update.headers.value("x-bitrix-csrf-token"), fixtureCsrf);
    QCOMPARE(update.headers.value("cookie"), fixtureCookie);
    const auto updateTask = QJsonDocument::fromJson(update.body).object().value("task").toObject();
    QCOMPARE(updateTask, QJsonObject({{"id", 42}, {"title", *edit.title}, {"priority", "average"}}));
    const auto description = fixture.requestsFor(QStringLiteral("tasks.v2.Task.Description.update")).first();
    const auto descriptionTask = QJsonDocument::fromJson(description.body).object().value("task").toObject();
    QCOMPARE(descriptionTask, QJsonObject({{"id", 42}, {"description", ""}, {"descriptionChecksum", "fixture-checksum"}}));
    const auto deadline = fixture.requestsFor(QStringLiteral("tasks.v2.Task.Deadline.update")).first();
    const auto deadlineTask = QJsonDocument::fromJson(deadline.body).object().value("task").toObject();
    QCOMPARE(deadlineTask.value("deadlineTs").toInteger(), QDateTime::fromString(*edit.deadline, Qt::ISODate).toSecsSinceEpoch());

    backend.createTask(QStringLiteral("17"), edit);
    QTRY_COMPARE(taskCreated.count(), 1);
    QCOMPARE(taskCreated.first().at(1).toString(), QStringLiteral("44"));
    const auto create = fixture.requestsFor(QStringLiteral("tasks.v2.Task.add")).first();
    const auto createBody = QJsonDocument::fromJson(create.body).object();
    QCOMPARE(createBody.value("task").toObject().value("group").toObject().value("id").toInt(), 17);
    QVERIFY(createBody.value("deadlineManuallySet").toBool());

    backend.createProject(QStringLiteral("Новый проект"));
    QTRY_COMPARE(projectCreated.count(), 1);
    QCOMPARE(projectCreated.first().first().toString(), QStringLiteral("23"));
    const auto addProject = fixture.requestsFor(QStringLiteral("socialnetwork.V2.Project.add")).first();
    const auto project = QJsonDocument::fromJson(addProject.body).object().value("project").toObject();
    QCOMPARE(addProject.headers.value("content-type"), QByteArray("application/json"));
    QCOMPARE(project.value("name").toString(), QStringLiteral("Новый проект"));
    QCOMPARE(project.value("ownerId").toInt(), 11);
    QCOMPARE(project.value("privacyType").toString(), QStringLiteral("closed"));
    QCOMPARE(project.value("baseFeatureId").toString(), QStringLiteral("chat"));
    QCOMPARE(project.value("permissions").toObject().value("tasks").toObject().value("create_tasks").toString(), QStringLiteral("K"));

    backend.saveStage(QStringLiteral("17"), {}, QStringLiteral("Готово"), {});
    QTRY_COMPARE(boardUpdated.count(), 1);
    const auto stage = fixture.requestsFor(QStringLiteral("modifyColumn")).first();
    QCOMPARE(stage.field(QStringLiteral("fields[columnName]")), QStringLiteral("Готово"));
    QCOMPARE(stage.field(QStringLiteral("fields[afterColumnId]")), QStringLiteral("60"));
    QCOMPARE(stage.field(QStringLiteral("params[GROUP_ID]")), QStringLiteral("17"));
    QCOMPARE(QUrlQuery(stage.url).queryItemValue(QStringLiteral("c")), QStringLiteral("bitrix:tasks.kanban"));

    backend.moveTask(QStringLiteral("17"), QStringLiteral("42"), QStringLiteral("60"));
    QTRY_COMPARE(boardUpdated.count(), 2);
    const auto move = fixture.requestsFor(QStringLiteral("moveTask")).first();
    QCOMPARE(move.field(QStringLiteral("itemId")), QStringLiteral("42"));
    QCOMPARE(move.field(QStringLiteral("columnId")), QStringLiteral("60"));
    QCOMPARE(move.field(QStringLiteral("beforeItemId")), QStringLiteral("0"));
    QVERIFY(failure.isEmpty());

    backend.saveStage(QStringLiteral("17"), QStringLiteral("60"), QStringLiteral("Рецензия команды"), QStringLiteral("0"));
    QTRY_COMPARE(boardUpdated.count(), 3);
    const auto rename = fixture.requestsFor(QStringLiteral("modifyColumn")).last();
    QCOMPARE(rename.field(QStringLiteral("fields[id]")), QStringLiteral("60"));
    QCOMPARE(rename.field(QStringLiteral("fields[columnColor]")), QStringLiteral("ff9966"));
    QCOMPARE(fixture.requestsFor(QStringLiteral("moveColumn")).first().field(QStringLiteral("afterColumnId")), QStringLiteral("0"));
    backend.deleteStage(QStringLiteral("17"), QStringLiteral("60"));
    QTRY_COMPARE(boardUpdated.count(), 4);
    QCOMPARE(fixture.requestsFor(QStringLiteral("modifyColumn")).last().field(QStringLiteral("fields[delete]")), QStringLiteral("1"));

    fixture.rejectDeadline = true;
    backend.updateTask(QStringLiteral("42"), edit);
    QTRY_COMPARE(failure.count(), 1);
    QCOMPARE(failure.first().first().toString(), QStringLiteral("update-task:42"));
    QVERIFY(!failure.first().at(1).toString().contains(QStringLiteral("DEADLINE_DENIED")));
    QCOMPARE(taskUpdated.count(), 1);
    QCOMPARE(fixture.requestsFor(QStringLiteral("tasks.v2.Task.Description.update")).size(), 1);
}

void TestCloudBackend::confirmedKanbanMoveSupersedesOutstandingBoardRead()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy loaded(&backend, &Backend::boardLoaded);
    QSignalSpy updated(&backend, &Backend::boardUpdated);
    QSignalSpy failed(&backend, &Backend::failed);
    connect(&backend, &Backend::boardUpdated, &backend, &Backend::requestProjectBoard);
    const auto project = QStringLiteral("17");
    const auto boardAction = QStringLiteral("applyFilter");
    const auto move = [&] { backend.moveTask(project, QStringLiteral("42"), QStringLiteral("60")); };
    fixture.holdNextReadResponse = boardAction;
    backend.start();
    backend.requestProjectBoard(project);
    QTRY_COMPARE(fixture.requestsFor(boardAction).size(), 1);
    for (const auto &result : {QByteArray("false"), QByteArray("null")}) {
        fixture.kanbanMoveResult = result;
        const auto previous = failed.size();
        move();
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), previous + 1, 1000);
        QCOMPARE(failed.last().first().toString(), QStringLiteral("board-edit:17"));
        QVERIFY(updated.isEmpty());
    }
    fixture.kanbanMoveResult = "true";
    move();
    QTRY_COMPARE(updated.size(), 1);
    QTRY_COMPARE(fixture.requestsFor(boardAction).size(), 2);
    QTRY_COMPARE(loaded.size(), 1);
    fixture.releaseHeldReadResponse();
    QTest::qWait(60);
    QCOMPARE(loaded.size(), 1);
    QCOMPARE(failed.size(), 2);
}

void TestCloudBackend::readsAndChangesStageColorWithoutRenaming()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy boardLoaded(&backend, &Backend::boardLoaded);
    QSignalSpy boardUpdated(&backend, &Backend::boardUpdated);
    QSignalSpy failed(&backend, &Backend::failed);
    const auto project = QStringLiteral("17");
    const auto stage = QStringLiteral("60");
    const auto title = QStringLiteral("Рецензия");
    const auto modify = QStringLiteral("modifyColumn");
    const auto color = QStringLiteral("fields[columnColor]");
    backend.start();
    backend.requestProjectBoard(project);
    QTRY_COMPARE(boardLoaded.size(), 1);
    const auto stages = boardLoaded.first().at(1).value<QVector<Stage>>();
    QVERIFY(stages.first().color.isEmpty());
    QCOMPARE(stages.last().color, QStringLiteral("ff9966"));

    backend.saveStage(project, stage, title, {}, QStringLiteral("rgb(0,0,0)"));
    QCOMPARE(failed.size(), 1);
    QCOMPARE(fixture.requestsFor(QStringLiteral("applyFilter")).size(), 1);
    QVERIFY(fixture.requestsFor(modify).isEmpty());

    backend.saveStage(project, stage, {}, {}, QStringLiteral("#FF9966"));
    QTRY_COMPARE(boardUpdated.size(), 1);
    QVERIFY(fixture.requestsFor(modify).isEmpty());

    backend.saveStage(project, stage, {}, {}, QStringLiteral("#47D1E2"));
    QTRY_COMPARE(boardUpdated.size(), 2);
    QCOMPARE(fixture.requestsFor(modify).size(), 1);
    const auto changed = fixture.requestsFor(modify).first();
    QCOMPARE(changed.field(QStringLiteral("fields[id]")), stage);
    QCOMPARE(changed.field(QStringLiteral("fields[columnName]")), title);
    QCOMPARE(changed.field(color), QStringLiteral("47d1e2"));
    QVERIFY(fixture.requestsFor(QStringLiteral("moveColumn")).isEmpty());

    backend.saveStage(project, {}, QStringLiteral("Новая"), {}, QStringLiteral("#468EE5"));
    QTRY_COMPARE(boardUpdated.size(), 3);
    QCOMPARE(fixture.requestsFor(modify).last().field(color), QStringLiteral("468ee5"));

    backend.saveStage(project, QStringLiteral("50"), QStringLiteral("Новый разбор"), {});
    QTRY_COMPARE(boardUpdated.size(), 4);
    QCOMPARE(fixture.requestsFor(modify).last().field(color), QStringLiteral("ace9fb"));
}

void TestCloudBackend::preservesRawTaskDescriptionAndRejectsAStaleEdit()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy updated(&backend, &Backend::taskUpdated);
    QSignalSpy failures(&backend, &Backend::failed);
    backend.start();
    const QString id = QStringLiteral("42");
    TaskEdit edit;
    edit.description = QStringLiteral(" [p]\n[b]Текст[/b][/p]\n[disk file id=n123 width=600 height=434][unknown]данные[/unknown] ");
    edit.originalDescription = QStringLiteral("Старая версия");
    backend.updateTask(id, edit);
    QTRY_COMPARE(failures.size(), 1);
    QVERIFY(fixture.requestsFor(QStringLiteral("tasks.v2.Task.Description.update")).isEmpty());
    QCOMPARE(updated.size(), 0);

    edit.originalDescription = QStringLiteral("Сверить схему & уведомления");
    backend.updateTask(id, edit);
    QTRY_COMPARE(updated.size(), 1);
    const auto description = fixture.requestsFor(QStringLiteral("tasks.v2.Task.Description.update")).first();
    const auto data = QJsonDocument::fromJson(description.body).object().value("task").toObject();
    QCOMPARE(data.value("description").toString(), *edit.description);
    QCOMPARE(data.value("descriptionChecksum").toString(), QStringLiteral("fixture-checksum"));

    edit.description = QStringLiteral("reject");
    backend.updateTask(id, edit);
    QTRY_COMPARE(failures.size(), 2);
    QCOMPARE(updated.size(), 1);
    QCOMPARE(fixture.requestsFor(QStringLiteral("tasks.v2.Task.Description.update")).size(), 2);
}

void TestCloudBackend::readsAndWritesTaskChecklistAndChildRelations()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy checklist(&backend, &Backend::taskChecklistLoaded);
    QSignalSpy changed(&backend, &Backend::taskChecklistChanged);
    QSignalSpy subtasks(&backend, &Backend::subtasksLoaded);
    QSignalSpy child(&backend, &Backend::subtaskCreated);
    QSignalSpy failed(&backend, &Backend::failed);
    backend.start();
    const auto id = QStringLiteral("42");
    backend.requestTaskChecklist(id);
    QTRY_COMPARE_WITH_TIMEOUT(checklist.size(), 1, 1000);
    const auto items = checklist.first().at(1).value<QVector<TaskChecklistItem>>();
    QCOMPARE(items.size(), 2);
    QCOMPARE(items.at(1).parentId, QStringLiteral("70"));
    QVERIFY(items.at(1).canToggle);
    QVERIFY(!items.at(1).complete);
    backend.setTaskChecklistComplete(id, QStringLiteral("71"), true);
    QTRY_COMPARE(failed.size(), 1);
    QVERIFY(changed.isEmpty());
    const auto toggle = fixture.requestsFor(QStringLiteral("tasks.v2.CheckList.complete")).first();
    const auto toggleRows = QJsonDocument::fromJson(toggle.body).object().value("task").toObject().value("checklist").toArray();
    QCOMPARE(toggleRows.size(), 1);
    QCOMPARE(toggleRows.first().toObject().value("id").toInt(), 71);
    QVERIFY(toggleRows.first().toObject().value("isComplete").toBool());
    backend.addTaskChecklistItem(id, QStringLiteral("70"), QStringLiteral("Следующий пункт"));
    QTRY_COMPARE(changed.size(), 1);
    const auto save = fixture.requestsFor(QStringLiteral("tasks.v2.CheckList.save")).first();
    const auto saveData = QJsonDocument::fromJson(save.body).object();
    QCOMPARE(saveData.value("baseRevision").toInt(), 7);
    const auto rows = saveData.value("task").toObject().value("checklist").toArray();
    QCOMPARE(rows.size(), 3);
    QCOMPARE(rows.at(1).toObject().value("attachments").toArray().first().toString(), QStringLiteral("n123"));
    QCOMPARE(rows.last().toObject().value("parentNodeId").toString(), QStringLiteral("parent"));
    QCOMPARE(rows.last().toObject().value("title").toString(), QStringLiteral("Следующий пункт"));

    backend.requestSubtasks(id);
    QTRY_COMPARE(subtasks.size(), 1);
    QCOMPARE(subtasks.first().at(1).value<QVector<Task>>().first().id, QStringLiteral("43"));
    TaskEdit edit;
    edit.title = QStringLiteral("Новая подзадача");
    backend.createSubtask(id, QStringLiteral("17"), edit);
    QTRY_COMPARE(child.size(), 1);
    QCOMPARE(child.first().first().toString(), id);
    const auto create = fixture.requestsFor(QStringLiteral("tasks.v2.Task.add")).first();
    const auto payload = QJsonDocument::fromJson(create.body).object().value("task").toObject();
    QCOMPARE(payload.value("parent").toObject().value("id").toInt(), 42);
    QCOMPARE(payload.value("group").toObject().value("id").toInt(), 17);
}

void TestCloudBackend::readsScopedGanttLinksAndExactPlanDates()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy dependencies(&backend, &Backend::taskDependenciesLoaded);
    QSignalSpy timeline(&backend, &Backend::projectTimelineLoaded);
    backend.start();
    backend.requestTaskDependencies(QStringLiteral("42"));
    QTRY_COMPARE(dependencies.size(), 1);
    const auto links = dependencies.first().at(1).value<QVector<TaskDependency>>();
    QCOMPARE(links.size(), 2);
    QCOMPARE(links.first().taskId, QStringLiteral("43"));
    QCOMPARE(links.first().type, QStringLiteral("finish_start"));
    QVERIFY(!links.first().prerequisite);
    QCOMPARE(links.last().taskId, QStringLiteral("44"));
    QVERIFY(links.last().prerequisite);
    backend.requestProjectTimeline(QStringLiteral("17"));
    QTRY_COMPARE(timeline.size(), 1);
    auto tasks = timeline.first().at(1).value<QVector<Task>>();
    QCOMPARE(tasks.size(), 2);
    QCOMPARE(tasks.first().planStart.date(), QDate(2026, 8, 1));
    QCOMPARE(tasks.first().planEnd.date(), QDate(2026, 8, 3));
    QVERIFY(!tasks.last().planStart.isValid());
    QVERIFY(!timeline.first().at(2).toBool());
}

void TestCloudBackend::readsScopedProjectMembersAndDiskFiles()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy members(&backend, &Backend::projectMembersLoaded);
    QSignalSpy files(&backend, &Backend::projectFilesLoaded);
    QSignalSpy errors(&backend, &Backend::failed);
    backend.start();
    const auto project = QStringLiteral("17");
    backend.requestProjectMembers(project);
    QTRY_COMPARE_WITH_TIMEOUT(members.size(), 1, 1000);
    const auto roster = members.first().at(1).value<QVector<ProjectMember>>();
    QCOMPARE(roster.size(), 5);
    QCOMPARE(roster.at(0).role, ProjectMemberRole::Owner);
    QCOMPARE(roster.at(0).name, QStringLiteral("Анна"));
    QCOMPARE(roster.at(1).role, ProjectMemberRole::Moderator);
    QCOMPARE(roster.at(1).name, QStringLiteral("Борис Смит"));
    QCOMPARE(roster.at(2).role, ProjectMemberRole::Member);
    QCOMPARE(roster.at(3).role, ProjectMemberRole::Invited);
    QCOMPARE(roster.at(4).role, ProjectMemberRole::Awaiting);

    backend.requestProjectFiles(project, {}, 0);
    QTRY_COMPARE(files.size(), 1);
    const auto entries = files.first().at(2).value<QVector<ProjectDiskItem>>();
    QCOMPARE(entries.size(), 3);
    QVERIFY(entries.at(0).folder);
    QCOMPARE(entries.at(1).file.downloadUrl.path(), QStringLiteral("/disk/download.xlsx"));
    QVERIFY(entries.at(2).file.downloadUrl.isEmpty());
    const auto storage = fixture.requestsFor(QStringLiteral("/rest/disk.storage.getList.json")).first();
    QCOMPARE(storage.field(QStringLiteral("filter[ENTITY_TYPE]")), QStringLiteral("group"));
    QCOMPARE(storage.field(QStringLiteral("filter[ENTITY_ID]")), project);
    const auto rootRequest = fixture.requestsFor(QStringLiteral("/rest/disk.storage.getChildren.json")).first();
    QCOMPARE(rootRequest.field(QStringLiteral("id")), QStringLiteral("31"));
    backend.requestProjectFiles(QStringLiteral("18"), QStringLiteral("77"), 0);
    QTRY_COMPARE(errors.size(), 1);
    QVERIFY(fixture.requestsFor(QStringLiteral("/rest/disk.folder.getChildren.json")).isEmpty());
    backend.requestProjectFiles(project, QStringLiteral("77"), 0);
    QTRY_COMPARE(files.size(), 2);
    QVERIFY(files.last().at(2).value<QVector<ProjectDiskItem>>().isEmpty());
    QCOMPARE(fixture.requestsFor(QStringLiteral("/rest/disk.folder.getChildren.json")).first().field(QStringLiteral("id")),
             QStringLiteral("77"));
}

void TestCloudBackend::readsTaskHistoryAndKeepsDenialDistinctFromEmpty()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy history(&backend, &Backend::taskHistoryLoaded);
    QSignalSpy errors(&backend, &Backend::failed);
    backend.start();
    backend.requestTaskHistory(QStringLiteral("42"));
    QTRY_COMPARE_WITH_TIMEOUT(history.size(), 1, 1000);
    QCOMPARE(history.first().first().toString(), QStringLiteral("42"));
    const auto rows = history.first().at(1).value<QVector<TaskHistoryEntry>>();
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.first().id, QStringLiteral("71"));
    QCOMPARE(rows.first().author, QStringLiteral("Анна Смит"));
    QCOMPARE(rows.first().detail, QStringLiteral("Было → Стало"));
    QVERIFY(rows.first().at > rows.last().at);
    backend.requestTaskHistory(QStringLiteral("43"));
    QTRY_COMPARE(history.size(), 2);
    QVERIFY(history.last().at(1).value<QVector<TaskHistoryEntry>>().isEmpty());
    backend.requestTaskHistory(QStringLiteral("44"));
    QTRY_COMPARE(errors.size(), 1);
    QCOMPARE(errors.first().first().toString(), QStringLiteral("task-history:44"));
    QCOMPARE(history.size(), 2);
}

void TestCloudBackend::loadsOlderHistoryWithScopedCursor()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy loaded(&backend, &Backend::olderChatMessagesLoaded);
    QSignalSpy removed(&backend, &Backend::chatHistoryRemoved);
    QSignalSpy connection(&backend, &Backend::connectionChanged);
    backend.start();
    backend.requestOlderChatMessages(QStringLiteral("chat91"), QStringLiteral("801"), false);
    QTRY_COMPARE(loaded.size(), 1);
    QCOMPARE(loaded.first().at(0).toString(), QStringLiteral("chat91"));
    QCOMPARE(loaded.first().at(1).toString(), QStringLiteral("801"));
    QCOMPARE(loaded.first().at(2).toBool(), false);
    QCOMPARE(loaded.first().at(4).toBool(), true);
    const auto rows = loaded.first().at(3).value<QVector<Message>>();
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.first().id, QStringLiteral("799"));
    QCOMPARE(rows.last().id, QStringLiteral("800"));
    const auto action = QStringLiteral("im.v2.Chat.Message.tail");
    auto requests = fixture.requestsFor(action);
    QCOMPARE(requests.size(), 1);
    QCOMPARE(requests.first().field(QStringLiteral("chatId")), QStringLiteral("91"));
    QCOMPARE(requests.first().field(QStringLiteral("filter[lastId]")), QStringLiteral("801"));
    QCOMPARE(requests.first().field(QStringLiteral("order[id]")), QStringLiteral("DESC"));
    QCOMPARE(requests.first().field(QStringLiteral("limit")), QStringLiteral("25"));

    backend.requestOlderChatMessages(QStringLiteral("42"), QStringLiteral("810"), true);
    QTRY_COMPARE(loaded.size(), 2);
    QCOMPARE(loaded.last().at(0).toString(), QStringLiteral("42"));
    QVERIFY(loaded.last().at(2).toBool());
    QVERIFY(loaded.last().at(3).value<QVector<Message>>().isEmpty());
    QVERIFY(!loaded.last().at(4).toBool());
    requests = fixture.requestsFor(action);
    QCOMPARE(requests.size(), 2);
    QCOMPARE(requests.last().field(QStringLiteral("chatId")), QStringLiteral("73"));
    QCOMPARE(requests.last().field(QStringLiteral("filter[lastId]")), QStringLiteral("810"));
    backend.requestOlderChatMessages(QStringLiteral("chat91"), QStringLiteral("bad"), false);
    QCOMPARE(fixture.requestsFor(action).size(), 2);
    QTRY_VERIFY(!connection.isEmpty());
    fixture.pushIm(QStringLiteral("wrong-chat-delete"), QStringLiteral("messageDelete"),
                   {{"dialogId", "chat91"}, {"chatId", 92}, {"id", 798}});
    fixture.pushIm(QStringLiteral("older-delete"), QStringLiteral("messageDeleteV2"),
                   {{"dialogId", "chat91"}, {"chatId", 91}, {"messages", QJsonArray{
                       QJsonObject{{"id", 798}, {"chatId", 92}}, QJsonObject{{"id", 799}, {"chatId", 91}}}}});
    QTRY_COMPARE(removed.size(), 1);
    QCOMPARE(removed.first().at(1).toStringList(), QStringList{QStringLiteral("799")});
    fixture.pushIm(QStringLiteral("older-history-clear"), QStringLiteral("chatHistoryDeletedForSelf"),
                   {{"dialogId", "chat91"}, {"chatId", 91}, {"lastId", 800}});
    QTRY_COMPARE(removed.size(), 2);
    QCOMPARE(removed.last().at(0).toString(), QStringLiteral("chat91"));
    QCOMPARE(removed.last().at(2).toLongLong(), 800);
    fixture.holdNextReadResponse = action;
    backend.requestOlderChatMessages(QStringLiteral("chat91"), QStringLiteral("801"), false);
    QTRY_COMPARE(fixture.requestsFor(action).size(), 3);
    fixture.pushIm(QStringLiteral("bulk-history-clear"), QStringLiteral("chatHistoryDeletedForAll"),
                   {{"dialogId", "chat91"}, {"chatId", 91}});
    QTRY_COMPARE(removed.size(), 3);
    QCOMPARE(removed.last().at(2).toLongLong(), 0);
    fixture.releaseHeldReadResponse();
    QTest::qWait(60);
    QCOMPARE(loaded.size(), 2);
}

void TestCloudBackend::loadsProjectTaskAndTaskChat()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy projects(&backend, &Backend::projectsLoaded);
    QSignalSpy board(&backend, &Backend::boardLoaded);
    QSignalSpy details(&backend, &Backend::taskLoaded);
    QSignalSpy history(&backend, &Backend::taskMessagesLoaded);
    QSignalSpy sent(&backend, &Backend::messageSent);
    QSignalSpy errors(&backend, &Backend::failed);
    backend.start();

    backend.requestProjects();
    QTRY_COMPARE(projects.size(), 1);
    const auto projectRows = projects.first().first().value<QVector<Project>>();
    QCOMPARE(projectRows.size(), 1);
    QCOMPARE(projectRows.first().id, QStringLiteral("17"));
    QCOMPARE(projectRows.first().name, QStringLiteral("Проект испытаний"));

    backend.requestProjectBoard(projectRows.first().id);
    QTRY_COMPARE(board.size(), 1);
    QCOMPARE(board.first().first().toString(), QStringLiteral("17"));
    const auto stages = board.first().at(1).value<QVector<Stage>>();
    QCOMPARE(stages.size(), 2);
    QCOMPARE(stages.first().name, QStringLiteral("Разбор запроса"));
    QCOMPARE(stages.last().name, QStringLiteral("Рецензия"));
    QVERIFY(stages.last().tasks.isEmpty());
    QCOMPARE(stages.first().tasks.size(), 1);
    const auto &task = stages.first().tasks.first();
    QCOMPARE(task.id, QStringLiteral("42"));
    QCOMPARE(task.name, QStringLiteral("Проверить маршрут"));
    QCOMPARE(task.stageName, stages.first().name);

    backend.requestTask(task.id);
    QTRY_COMPARE(details.size(), 1);
    const auto detail = details.first().first().value<Task>();
    QCOMPARE(detail.description, QStringLiteral("Сверить схему & уведомления"));
    QCOMPARE(detail.projectId, QStringLiteral("17"));
    QCOMPARE(detail.chatDialogId, QStringLiteral("chat73"));
    QCOMPARE(detail.project, QStringLiteral("Команда"));
    QCOMPARE(detail.status, QStringLiteral("Выполняется"));
    QCOMPARE(detail.createdBy, QStringLiteral("Автор"));
    QCOMPARE(detail.assignee, QStringLiteral("Исполнитель"));
    QCOMPARE(detail.observers, QStringList{QStringLiteral("Наблюдатель")});
    QVERIFY(detail.createdAt.isValid());

    backend.requestTaskMessages(task.id);
    QTRY_COMPARE(history.size(), 1);
    QCOMPARE(history.first().first().toString(), task.id);
    const auto messages = history.first().at(1).value<QVector<Message>>();
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().text, QStringLiteral("Чат задачи"));

    const auto text = QStringLiteral("Готово + 100% & проверено");
    backend.sendTaskMessage(task.id, text);
    QTRY_COMPARE(sent.size(), 1);
    QCOMPARE(sent.first().first().toString(), QStringLiteral("task:") + task.id);
    QCOMPARE(sent.first().at(1).toString(), QStringLiteral("801"));

    const auto boardRequests = fixture.requestsFor(QStringLiteral("applyFilter"));
    QCOMPARE(boardRequests.size(), 1);
    const auto &request = boardRequests.first();
    QCOMPARE(request.method, QByteArray("POST"));
    QCOMPARE(request.url.path(), ajaxPath);
    const QUrlQuery query(request.url);
    QCOMPARE(query.queryItemValue(QStringLiteral("mode")), QStringLiteral("class"));
    QCOMPARE(query.queryItemValue(QStringLiteral("c")), QStringLiteral("bitrix:tasks.kanban"));
    QCOMPARE(request.field(QStringLiteral("params[GROUP_ID]")), projectRows.first().id);
    QCOMPARE(request.field(QStringLiteral("personal")), QStringLiteral("N"));
    QCOMPARE(request.headers.value("cookie"), fixtureCookie);
    QCOMPARE(request.headers.value("x-bitrix-csrf-token"), fixtureCsrf);

    const auto fetches = fixture.requestsFor(QStringLiteral("im.v2.Chat.Message.list"));
    QVERIFY(!fetches.isEmpty());
    QCOMPARE(fetches.first().field(QStringLiteral("chatId")), QStringLiteral("73"));
    const auto sends = fixture.requestsFor(QStringLiteral("im.v2.Chat.Message.send"));
    QCOMPARE(sends.size(), 1);
    QCOMPARE(sends.first().field(QStringLiteral("dialogId")), QStringLiteral("chat73"));
    QCOMPARE(sends.first().field(QStringLiteral("fields[message]")), text);
    QVERIFY(!QUuid(sends.first().field(QStringLiteral("fields[templateId]"))).isNull());
    QCOMPARE(errors.size(), 0);
}

void TestCloudBackend::refreshesTaskChatAfterProjectChange_data()
{
    QTest::addColumn<bool>("localChange");
    QTest::addColumn<int>("section");
    QTest::addColumn<bool>("taskCard");
    QTest::addColumn<bool>("inflight");
    QTest::newRow("local-project") << true << 1 << true << false;
    QTest::newRow("portal-project") << false << 1 << true << false;
    QTest::newRow("portal-project-during-history-read") << false << 1 << true << true;
    QTest::newRow("portal-card-over-chats") << false << 0 << true << false;
    QTest::newRow("portal-task-chat-in-chats") << false << 0 << false << false;
}

void TestCloudBackend::refreshesTaskChatAfterProjectChange()
{
    QFETCH(bool, localChange);
    QFETCH(int, section);
    QFETCH(bool, taskCard);
    QFETCH(bool, inflight);
    CloudFixture fixture;
    fixture.editableTaskFixture = true;
    fixture.richRecentFixture = !taskCard;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy online(&backend, &Backend::connectionChanged);
    QSignalSpy activity(&backend, &Backend::taskUpdated);
    QSignalSpy previewInvalidations(&backend, &Backend::taskLinkPreviewInvalidated);
    QSignalSpy board(&backend, &Backend::boardLoaded);
    QSignalSpy taskHistory(&backend, &Backend::taskMessagesLoaded);
    QSignalSpy chatHistory(&backend, &Backend::chatMessagesLoaded);
    const auto &history = taskCard ? taskHistory : chatHistory;
    const auto taskId = QStringLiteral("42");
    const auto dialog = QStringLiteral("chat73");
    backend.start();
    QTRY_COMPARE(online.size(), 1);
    backend.setActiveSection(section);
    if (section == 1) {
        backend.requestProjectBoard(QStringLiteral("17"));
        QTRY_COMPARE(board.size(), 1);
    }
    if (taskCard) {
        backend.requestTaskMessages(taskId);
    } else {
        QSignalSpy tasks(&backend, &Backend::taskChatsLoaded);
        backend.requestTaskChats();
        QTRY_VERIFY(!tasks.isEmpty());
        backend.requestChatMessages(dialog);
    }
    QTRY_COMPARE(history.size(), 1);
    QCOMPARE(history.first().at(1).value<QVector<Message>>().first().text, QStringLiteral("Чат задачи"));

    const auto chatLoad = QStringLiteral("im.v2.Chat.load");
    if (inflight) {
        fixture.holdNextChatResponse = true;
        backend.requestTaskMessages(taskId);
        QTRY_COMPARE(fixture.requestsFor(chatLoad).size(), 2);
    }
    fixture.chatResponseOverride = R"({"status":"success","data":{"chatId":73,
        "messages":[{"id":802,"authorId":0,"text":"Проект задачи изменён"}]}})";
    if (localChange) {
        TaskEdit edit;
        edit.projectId = QStringLiteral("18");
        backend.updateTask(taskId, edit);
        QTRY_COMPARE(activity.size(), 1);
    } else {
        fixture.pushIm(QStringLiteral("different-task"), QStringLiteral("task_update"),
                       {{"taskId", 43}, {"groupId", 18}}, 0, QStringLiteral("tasks"));
        QTest::qWait(350);
        QCOMPARE(history.size(), 1);
        fixture.pushIm(QStringLiteral("moved-task"), QStringLiteral("task_update"),
                       {{"fields", QJsonObject{{"ID", 42}, {"GROUP_ID", 18}}}}, 0, QStringLiteral("tasks"));
        if (inflight) {
            // Let the board refresh prove the event was handled while history was still in flight.
            QTRY_COMPARE(board.size(), 2);
            QCOMPARE(fixture.requestsFor(chatLoad).size(), 2);
            fixture.releaseHeldChatResponse();
        }
    }
    QTRY_COMPARE_WITH_TIMEOUT(history.size(), inflight ? 3 : 2, 1500);
    if (!localChange) {
        QCOMPARE(previewInvalidations.size(), 2);
        QCOMPARE(previewInvalidations.last().first().toString(), taskId);
        QVERIFY(activity.isEmpty());
    }
    if (!localChange && section == 1)
        QTRY_COMPARE(board.size(), 2);
    QCOMPARE(history.last().at(0).toString(), taskCard ? taskId : dialog);
    const auto messages = history.last().at(1).value<QVector<Message>>();
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().text, QStringLiteral("Проект задачи изменён"));
    QVERIFY(messages.first().system);
}

void TestCloudBackend::retriesChatHistoryWithTheKnownChatId_data()
{
    QTest::addColumn<QByteArray>("status");
    QTest::addColumn<bool>("connectionClosed");
    QTest::newRow("http-500") << QByteArray("500 Internal Server Error") << false;
    QTest::newRow("ajax-error") << QByteArray("200 OK") << false;
    QTest::newRow("connection-closed") << QByteArray() << true;
}

void TestCloudBackend::retriesChatHistoryWithTheKnownChatId()
{
    QFETCH(QByteArray, status);
    QFETCH(bool, connectionClosed);
    CloudFixture fixture;
    fixture.presencePinsFixture = true;
    fixture.chatResponseOverride = R"({"status":"error","data":null,"errors":[{"code":"CHAT_LOAD_FAILED"}]})";
    fixture.chatResponseStatus = status;
    fixture.closeChatSnapshotConnection = connectionClosed;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    backend.setForeground(false);
    QSignalSpy recent(&backend, &Backend::recentChatsLoaded);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    QSignalSpy errors(&backend, &Backend::failed);
    backend.start();
    backend.requestRecentChats();
    QTRY_VERIFY(!recent.isEmpty());
    const auto dialog = QStringLiteral("103");
    backend.requestChatMessages(dialog);
    const auto loadAction = QStringLiteral("im.v2.Chat.load");
    const auto listAction = QStringLiteral("im.v2.Chat.Message.list");
    QTRY_COMPARE_WITH_TIMEOUT(history.size(), 1, 1500);
    QCOMPARE(history.first().first().toString(), dialog);
    const auto messages = history.first().at(1).value<QVector<Message>>();
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().text, QStringLiteral("Чат задачи"));
    if (connectionClosed)
        QVERIFY(!fixture.requestsFor(loadAction).isEmpty());
    else
        QCOMPARE(fixture.requestsFor(loadAction).size(), 1);
    const auto requests = fixture.requestsFor(listAction);
    QCOMPARE(requests.size(), 1);
    QCOMPARE(requests.first().field(QStringLiteral("chatId")), QStringLiteral("91"));
    QVERIFY(std::none_of(errors.cbegin(), errors.cend(), [&dialog](const QList<QVariant> &entry) {
        return entry.first().toString() == QStringLiteral("chat:") + dialog;
    }));
}

void TestCloudBackend::recoversLatestHistoryAfterATemporaryFailure_data()
{
    QTest::addColumn<QString>("mode");
    for (const auto *mode : {"snapshot", "fallback", "task", "exhausted", "switch", "background", "account", "malformed"})
        QTest::newRow(mode) << QString::fromLatin1(mode);
}

void TestCloudBackend::recoversLatestHistoryAfterATemporaryFailure()
{
    QFETCH(QString, mode);
    CloudFixture fixture;
    const bool changeAccount = mode == QLatin1String("account");
    const bool task = mode == QLatin1String("task");
    if (changeAccount)
        fixture.ordinaryRecentBadgeFixture = R"({"userId":11,"recentItems":[],"chats":[]})";
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy online(&backend, &Backend::connectionChanged);
    QSignalSpy history(&backend, task ? &Backend::taskMessagesLoaded : &Backend::chatMessagesLoaded);
    QSignalSpy errors(&backend, &Backend::failed);
    QSignalSpy accounts(&backend, &Backend::accountScopeChanged);
    backend.start();
    QTRY_VERIFY(!online.isEmpty());
    if (changeAccount) {
        backend.requestRecentChats();
        QTRY_COMPARE(accounts.size(), 1);
    }

    const auto snapshot = QStringLiteral("im.v2.Chat.load");
    const auto list = QStringLiteral("im.v2.Chat.Message.list");
    const bool fallback = task || mode == QLatin1String("fallback");
    const auto dialog = task ? QStringLiteral("chat73") : fallback ? QStringLiteral("chat91") : QStringLiteral("999");
    const auto operation = task ? QStringLiteral("task-messages:42") : QStringLiteral("chat:") + dialog;
    const auto historyErrors = [&] {
        return std::count_if(errors.cbegin(), errors.cend(), [&](const QList<QVariant> &row) {
            return row.first().toString() == operation;
        });
    };
    if (fallback && !task)
        fixture.chatResponseOverride = ajaxResult(R"({"chat":{"id":91}})");
    if (mode == QLatin1String("exhausted"))
        fixture.chatResponseStatus = "503 Service Unavailable";
    if (mode == QLatin1String("malformed"))
        fixture.chatResponseOverride = "invalid json";
    fixture.holdNotificationReply = fallback ? list : snapshot;
    if (task)
        backend.requestTaskMessages(QStringLiteral("42"));
    else
        backend.requestChatMessages(dialog);
    QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());
    fixture.releaseHeldNotificationReply(mode != QLatin1String("malformed"));
    if (mode == QLatin1String("switch") || mode == QLatin1String("background") || changeAccount) {
        QTest::qWait(80);
        QCOMPARE(historyErrors(), 0);
    }
    if (mode == QLatin1String("switch"))
        backend.requestChatMessages(QStringLiteral("chat92"));
    else if (mode == QLatin1String("background"))
        backend.setForeground(false);
    else if (changeAccount) {
        fixture.ordinaryRecentBadgeFixture = R"({"userId":22,"recentItems":[],"chats":[]})";
        backend.requestRecentChats();
        QTRY_COMPARE(accounts.size(), 2);
    }

    if (mode == QLatin1String("snapshot") || fallback) {
        QTRY_COMPARE_WITH_TIMEOUT(history.size(), 1, 2000);
        QCOMPARE(history.first().first().toString(), task ? QStringLiteral("42") : dialog);
        QCOMPARE(fixture.requestsFor(snapshot).size(), 2);
        QCOMPARE(fixture.requestsFor(list).size(), fallback ? 2 : 0);
        QCOMPARE(historyErrors(), 0);
    } else if (mode == QLatin1String("exhausted") || mode == QLatin1String("malformed")) {
        QTRY_COMPARE_WITH_TIMEOUT(historyErrors(), 1, 2000);
        QTest::qWait(500);
        QCOMPARE(historyErrors(), 1);
        QCOMPARE(fixture.requestsFor(snapshot).size(), mode == QLatin1String("exhausted") ? 2 : 1);
        QVERIFY(history.isEmpty());
    } else {
        QTest::qWait(750);
        const auto snapshots = fixture.requestsFor(snapshot);
        QCOMPARE(std::count_if(snapshots.cbegin(), snapshots.cend(), [&](const auto &request) {
            return request.field(QStringLiteral("dialogId")) == dialog;
        }), 1);
    }
}

void TestCloudBackend::coalescesManualHistoryRefreshWhileLatestReadIsInFlight()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy online(&backend, &Backend::connectionChanged);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    backend.start();
    QTRY_VERIFY(!online.isEmpty());
    const auto dialog = QStringLiteral("chat91");
    const auto action = QStringLiteral("im.v2.Chat.load");
    fixture.holdNextChatResponse = true;
    backend.requestChatMessages(dialog);
    QTRY_COMPARE(fixture.requestsFor(action).size(), 1);
    backend.requestChatMessages(dialog);
    backend.requestChatMessages(dialog);
    QCOMPARE(fixture.requestsFor(action).size(), 1);

    fixture.chatResponseOverride = ajaxResult(R"({"messages":[{"id":702,"authorId":11,"text":"Refreshed"}]})");
    fixture.releaseHeldChatResponse();
    QTRY_COMPARE_WITH_TIMEOUT(history.size(), 2, 2000);
    QCOMPARE(history.last().at(1).value<QVector<Message>>().first().text, QStringLiteral("Refreshed"));
    QTest::qWait(300);
    QCOMPARE(fixture.requestsFor(action).size(), 2);
}

void TestCloudBackend::readsOrdinaryChatAndNotificationHistory()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy recent(&backend, &Backend::recentChatsLoaded);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    QSignalSpy notices(&backend, &Backend::notificationsLoaded);
    QSignalSpy sent(&backend, &Backend::messageSent);
    QSignalSpy errors(&backend, &Backend::failed);
    backend.start();

    backend.requestRecentChats();
    QTRY_COMPARE(recent.size(), 1);
    const auto chats = recent.first().first().value<QVector<Chat>>();
    QCOMPARE(chats.size(), 1);
    QCOMPARE(chats.first().id, QStringLiteral("chat91"));
    QCOMPARE(chats.first().title, QStringLiteral("Обычный чат"));
    QCOMPARE(chats.first().lastMessage, QStringLiteral("Привет"));
    QCOMPARE(chats.first().unread, 2);

    backend.requestChatMessages(chats.first().id);
    backend.requestNotifications();
    QTRY_COMPARE(history.size(), 1);
    const auto messages = history.first().at(1).value<QVector<Message>>();
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().text, QStringLiteral("Привет"));
    QVERIFY(messages.first().outgoing);

    QTRY_COMPARE(notices.size(), 1);
    const auto notifications = notices.first().first().value<QVector<Notification>>();
    QCOMPARE(notifications.size(), 1);
    QCOMPARE(notifications.first().title, QStringLiteral("Срок задачи"));
    QVERIFY(notifications.first().read);

    const auto batches = fixture.requestsFor(batchPath);
    QCOMPARE(batches.size(), 1);
    QCOMPARE(batches.first().method, QByteArray("POST"));
    QCOMPARE(batches.first().field(QStringLiteral("cmd[notify]")),
             QStringLiteral("im.notify.get?LIMIT=50&CONVERT_TEXT=Y"));
    QCOMPARE(batches.first().field(QStringLiteral("sessid")).toLatin1(), fixtureCsrf);
    QVERIFY(!batches.first().form.hasQueryItem(QStringLiteral("auth")));

    backend.sendChatMessage(chats.first().id, QStringLiteral("Ответ"));
    QTRY_COMPARE(sent.size(), 1);
    QCOMPARE(sent.first().first().toString(), QStringLiteral("chat:") + chats.first().id);
    const auto sends = fixture.requestsFor(QStringLiteral("im.v2.Chat.Message.send"));
    QCOMPARE(sends.size(), 1);
    QCOMPARE(sends.first().field(QStringLiteral("dialogId")), chats.first().id);
    QCOMPARE(errors.size(), 0);
}

namespace {

QJsonArray notificationPage(int first, int last, int type)
{
    QJsonArray rows;
    for (int id = first; id >= last; --id) {
        const QDateTime created = QDateTime::fromSecsSinceEpoch(1775000000 + id, Qt::UTC);
        rows.append(QJsonObject{{"id", id}, {"notify_type", type}, {"notify_module", "tasks"}, {"notify_read", "N"},
            {"text", QStringLiteral("Уведомление №%1").arg(id)}, {"date", created.toString(Qt::ISODate)}});
    }
    return rows;
}

QString olderNotificationCommand(int id, int type)
{
    return QStringLiteral("im.notify.get?LIMIT=50&CONVERT_TEXT=Y&LAST_ID=%1&LAST_TYPE=%2").arg(id).arg(type);
}

} // namespace

void TestCloudBackend::loadsOlderNotificationsOnDemandAndKeepsTheCursor()
{
    CloudFixture fixture;
    fixture.noticeFixture = notificationPage(160, 111, 2);
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy pages(&backend, &Backend::notificationsLoaded);
    QSignalSpy state(&backend, &Backend::notificationHistoryPageStateChanged);
    QSignalSpy errors(&backend, &Backend::failed);
    QSignalSpy read(&backend, &Backend::notificationReadStateChanged);
    QSignalSpy removed(&backend, &Backend::notificationRemoved);
    QSignalSpy connection(&backend, &Backend::connectionChanged);
    backend.start();
    backend.requestNotifications();
    QTRY_COMPARE(pages.size(), 1);
    QTRY_VERIFY(!connection.isEmpty() && connection.last().first().toBool());
    QVERIFY(!state.isEmpty());
    QCOMPARE(state.last(), QList<QVariant>({true, false}));
    const auto firstOlder = olderNotificationCommand(111, 3);
    const auto secondOlder = olderNotificationCommand(61, 3);
    const auto countCommand = [&fixture](const QString &command) {
        const auto batches = fixture.requestsFor(batchPath);
        return std::count_if(batches.cbegin(), batches.cend(), [&command](const Request &request) {
            return request.field(QStringLiteral("cmd[notify]")) == command;
        });
    };
    QCOMPARE(countCommand(firstOlder), 0);

    backend.requestOlderNotifications();
    QCOMPARE(state.last(), QList<QVariant>({true, true}));
    QTRY_COMPARE(errors.size(), 1);
    QCOMPARE(errors.first().first().toString(), QStringLiteral("notifications-older"));
    QCOMPARE(state.last(), QList<QVariant>({true, false}));
    QCOMPARE(pages.size(), 1);

    fixture.olderNoticeFixtures.insert(firstOlder, notificationPage(110, 61, 4));
    fixture.holdNotificationReply = QStringLiteral("im.notify.get");
    backend.requestOlderNotifications();
    backend.requestOlderNotifications();
    QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());
    fixture.pushIm(QStringLiteral("history-newest-read"), QStringLiteral("notifyRead"), {{"list", QJsonArray{90}}});
    fixture.pushIm(QStringLiteral("history-newest-delete"), QStringLiteral("notifyDelete"), {{"id", 89}});
    QTRY_COMPARE(read.size(), 1);
    QTRY_COMPARE(removed.size(), 1);
    const auto beforeOlder = pages.size();
    fixture.releaseHeldNotificationReply();
    QTRY_COMPARE(pages.size(), beforeOlder + 1);
    QCOMPARE(countCommand(firstOlder), 2); // One failure and one explicit retry.
    const auto loaded = pages.last().first().value<QVector<Notification>>();
    QCOMPARE(loaded.size(), 99);
    QVERIFY(std::any_of(loaded.cbegin(), loaded.cend(), [](const Notification &row) {
        return row.id == QLatin1String("90") && row.read;
    }));
    QVERIFY(std::none_of(loaded.cbegin(), loaded.cend(), [](const Notification &row) { return row.id == QLatin1String("89"); }));

    fixture.noticeFixture = notificationPage(161, 112, 2);
    backend.requestNotifications();
    QTRY_COMPARE(pages.size(), beforeOlder + 2);
    const auto latest = pages.last().first().value<QVector<Notification>>();
    QCOMPARE(latest.first().id, QStringLiteral("161"));
    QCOMPARE(latest.last().id, QStringLiteral("61"));
    QCOMPARE(countCommand(secondOlder), 0);
    fixture.olderNoticeFixtures.insert(secondOlder, notificationPage(60, 59, 4));
    backend.requestOlderNotifications();
    QTRY_COMPARE(pages.size(), beforeOlder + 3);
    QCOMPARE(countCommand(secondOlder), 1);
    const auto all = pages.last().first().value<QVector<Notification>>();
    QCOMPARE(all.size(), 102);
    QCOMPARE(all.first().id, QStringLiteral("161"));
    QCOMPARE(all.last().id, QStringLiteral("59"));
    QCOMPARE(state.last(), QList<QVariant>({false, false}));
}

void TestCloudBackend::advancesNotificationCursorAcrossTypesAndStopsOnRepeat()
{
    CloudFixture fixture;
    fixture.noticeFixture = notificationPage(200, 151, 1);
    fixture.olderNoticeFixtures.insert(olderNotificationCommand(151, 1), notificationPage(242, 193, 4));
    fixture.olderNoticeFixtures.insert(olderNotificationCommand(193, 3), notificationPage(242, 193, 4));
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy pages(&backend, &Backend::notificationsLoaded);
    QSignalSpy state(&backend, &Backend::notificationHistoryPageStateChanged);
    backend.start();
    backend.requestNotifications();
    QTRY_COMPARE(pages.size(), 1);
    QVERIFY(!state.isEmpty());
    QCOMPARE(state.last(), QList<QVariant>({true, false}));
    backend.requestOlderNotifications();
    QTRY_COMPARE(pages.size(), 2);
    QCOMPARE(state.last(), QList<QVariant>({true, false}));
    backend.requestOlderNotifications();
    QTRY_VERIFY(!state.isEmpty() && state.last() == QList<QVariant>({false, false}));
    QCOMPARE(fixture.requestsFor(batchPath).last().field(QStringLiteral("cmd[notify]")), olderNotificationCommand(193, 3));
    backend.requestOlderNotifications();
    QCOMPARE(state.last(), QList<QVariant>({false, false}));
}

void TestCloudBackend::searchesAndMutatesNotificationsOverTheSession()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy found(&backend, &Backend::notificationsFound);
    QSignalSpy read(&backend, &Backend::notificationReadStateChanged);
    QSignalSpy removed(&backend, &Backend::notificationRemoved);
    QSignalSpy finished(&backend, &Backend::notificationActionFinished);
    QSignalSpy errors(&backend, &Backend::failed);
    backend.start();

    const auto text = QStringLiteral("Задача & 100% + #");
    backend.searchNotifications(text);
    QTRY_COMPARE(found.size(), 1);
    QCOMPARE(found.first().first().toString(), text);
    const auto results = found.first().last().value<QVector<Notification>>();
    QCOMPARE(results.size(), 1);
    const auto id = results.first().id;
    QVERIFY(!results.first().read);
    QCOMPARE(read.size(), 0);

    backend.markAllNotificationsRead();
    QTRY_COMPARE(finished.size(), 1);
    QCOMPARE(read.size(), 1);
    QCOMPARE(read.first().first().toStringList(), QStringList{id});
    QCOMPARE(read.first().last().toBool(), true);
    backend.markNotificationRead(id);
    QTRY_COMPARE(finished.size(), 2);
    QCOMPARE(read.size(), 2);
    backend.deleteNotification(QStringLiteral("9"));
    QTRY_COMPARE(errors.size(), 1);
    QCOMPARE(removed.size(), 0);
    backend.deleteNotification(id);
    QTRY_COMPARE(finished.size(), 3);
    QCOMPARE(removed.first().first().toString(), id);
    backend.deleteNotification(QStringLiteral("77&ID=9"));
    QCOMPARE(errors.size(), 2);

    const auto batches = fixture.requestsFor(batchPath);
    QCOMPARE(batches.size(), 5);
    const auto query = batches.first().field(QStringLiteral("cmd[notify]")).section(QLatin1Char('?'), 1);
    QCOMPARE(QUrlQuery(query).queryItemValue(QStringLiteral("SEARCH_TEXT"), QUrl::FullyDecoded), text);
    QCOMPARE(QUrlQuery(query).queryItems().size(), 1);
    QCOMPARE(batches.at(2).field(QStringLiteral("cmd[notify]")),
             QStringLiteral("im.notify.read?ID=77&ACTION=Y&ONLY_CURRENT=Y"));
    for (const auto &request : batches) {
        QCOMPARE(request.method, QByteArray("POST"));
        QCOMPARE(request.headers.value("cookie"), fixtureCookie);
        QCOMPARE(request.field(QStringLiteral("sessid")).toLatin1(), fixtureCsrf);
    }
}

void TestCloudBackend::reconcilesUnreadFromScopedActiveHistory()
{
    CloudFixture fixture;
    fixture.holdNextOnlineResponse = true;
    fixture.ordinaryRecentBadgeFixture = R"({"userId":11,
        "recentItems":[{"dialogId":"103","chatId":91,"messageId":702,"counter":1},
                       {"dialogId":"chat92","chatId":92,"counter":4}],
        "chats":[{"id":91,"dialogId":"103","type":"user","name":"Personal"},
                 {"id":92,"dialogId":"chat92","type":"chat","name":"Different chat"}]})";
    fixture.messengerPage = R"HTML(<script>
        BX.Messenger.v2.Application.Launch('messenger', {"currentUser":{"id":11},
            "counters":[{"chatId":91,"counter":9},{"chatId":92,"counter":6}]});
        </script>)HTML";
    fixture.chatResponseOverride = ajaxResult(R"({"chat":{"id":91,"dialogId":"103","lastId":700,"unreadId":701,"userCounter":2},
        "messages":[{"id":701,"chatId":91,"authorId":103,"unread":true,"text":"Older unread"},
                    {"id":702,"chatId":91,"authorId":11,"unread":false,"text":"Newer own"}]})");
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    backend.setForeground(false);
    QSignalSpy online(&backend, &Backend::connectionChanged);
    QSignalSpy recent(&backend, &Backend::recentChatsLoaded);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    QElapsedTimer counterCooldown;
    counterCooldown.start();
    backend.start();
    backend.requestRecentChats();
    QTRY_VERIFY(!recent.isEmpty() && !online.isEmpty());
    QTRY_COMPARE(backend.unreadChatCount().value_or(-1), 5);
    const auto unread = [&recent](const QString &id) {
        const auto chats = recent.last().first().value<QVector<Chat>>();
        for (const auto &chat : chats)
            if (chat.id == id)
                return chat.unread;
        return -1;
    };
    backend.requestChatMessages(QStringLiteral("103"));
    QTRY_COMPARE(history.size(), 1);
    QCOMPARE(unread(QStringLiteral("103")), 1);
    const auto firstHistory = history.first().last().value<QVector<Message>>();
    QCOMPARE(firstHistory.size(), 2);
    QVERIFY(firstHistory.first().unread);
    QVERIFY(!firstHistory.last().unread);
    fixture.releaseHeldOnlineResponse();
    QTRY_COMPARE(unread(QStringLiteral("chat92")), 6);
    QCOMPARE(unread(QStringLiteral("103")), 9);
    QCOMPARE(backend.unreadChatCount().value_or(-1), 15);

    fixture.holdNextChatResponse = true;
    fixture.chatResponseOverride = ajaxResult(R"({"chat":{"id":91,"dialogId":"103","lastId":702,"unreadId":0,"userCounter":2},
        "messages":[{"id":702,"chatId":91,"authorId":11,"unread":false,"text":"Newer own"}]})");
    backend.requestChatMessages(QStringLiteral("103"));
    QTRY_COMPARE(fixture.requestsFor(QStringLiteral("im.v2.Chat.load")).size(), 2);
    fixture.pushIm(QStringLiteral("new-unread"), QStringLiteral("message"),
                   {{"dialogId", "103"}, {"chatId", 91}, {"counter", 2},
                    {"message", QJsonObject{{"id", 703}, {"chatId", 91}, {"senderId", 103}, {"text", "New incoming"}}}});
    QTRY_COMPARE(unread(QStringLiteral("103")), 2);
    fixture.releaseHeldChatResponse();
    QTRY_COMPARE(history.size(), 2);
    QCOMPARE(unread(QStringLiteral("103")), 2);

    fixture.chatResponseOverride = ajaxResult(R"({"chat":{"id":91,"dialogId":"103","lastId":704,"unreadId":0,"userCounter":2},
        "messages":[{"id":703,"chatId":91,"authorId":103,"unread":false,"text":"Read elsewhere"},
                    {"id":704,"chatId":91,"authorId":11,"unread":false,"text":"Another own message"}]})");
    backend.requestChatMessages(QStringLiteral("103"));
    QTRY_COMPARE(history.size(), 3);
    QCOMPARE(unread(QStringLiteral("103")), 0);
    QCOMPARE(unread(QStringLiteral("chat92")), 6);
    QCOMPARE(backend.unreadChatCount().value_or(-1), 6);

    // Start the scoped snapshot first, then let a newer global snapshot finish before it.
    QTest::qWait(qMax<qint64>(0, 15050 - counterCooldown.elapsed()));
    fixture.holdNextChatResponse = true;
    fixture.chatResponseOverride = ajaxResult(R"({"chat":{"id":91,"dialogId":"103","lastId":704,"unreadId":0,"userCounter":2},
        "messages":[{"id":704,"chatId":91,"authorId":11,"unread":false,"text":"Own"}]})");
    backend.requestChatMessages(QStringLiteral("103"));
    QTRY_COMPARE(fixture.requestsFor(QStringLiteral("im.v2.Chat.load")).size(), 4);
    fixture.messengerPage = R"HTML(<script>
        BX.Messenger.v2.Application.Launch('messenger', {"currentUser":{"id":11},
            "counters":[{"chatId":91,"counter":3},{"chatId":92,"counter":7}]});
        </script>)HTML";
    backend.requestRecentChats();
    QTRY_COMPARE(unread(QStringLiteral("chat92")), 7);
    QCOMPARE(unread(QStringLiteral("103")), 3);
    fixture.releaseHeldChatResponse();
    QTRY_COMPARE(history.size(), 4);
    QCOMPARE(unread(QStringLiteral("103")), 3);
    QCOMPARE(backend.unreadChatCount().value_or(-1), 10);
    QVERIFY(fixture.requestsFor(QStringLiteral("im.v2.Chat.Message.read")).isEmpty());
}

void TestCloudBackend::ignoresParticipantCountsInUnreadBadges()
{
    CloudFixture fixture;
    fixture.holdNextOnlineResponse = true;
    fixture.chatBadgesFixture = true;
    fixture.ordinaryRecentBadgeFixture = R"({"userId":11,
        "recentItems":[{"dialogId":"chat91","chatId":91},{"dialogId":"chat92","chatId":92}],
        "chats":[{"id":91,"dialogId":"chat91","type":"chat","name":"First group"},
                 {"id":92,"dialogId":"chat92","type":"chat","name":"Second group","userCounter":74}]})";
    fixture.messengerPage = R"HTML(<script>
        BX.Messenger.v2.Application.Launch('messenger', {"currentUser":{"id":11},
            "counters":[{"chatId":99,"counter":3}]});
        </script>)HTML";
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy online(&backend, &Backend::connectionChanged);
    QSignalSpy recent(&backend, &Backend::recentChatsLoaded);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    QSignalSpy confirmed(&backend, &Backend::chatReadConfirmed);
    const auto first = QStringLiteral("chat91");
    const auto second = QStringLiteral("chat92");
    const auto read = QStringLiteral("im.v2.Chat.Message.read");
    const auto unread = [&recent](const QString &dialog) {
        if (!recent.isEmpty()) {
            for (const auto &chat : recent.last().first().value<QVector<Chat>>()) {
                if (chat.id == dialog)
                    return chat.unread;
            }
        }
        return -1;
    };
    backend.start();
    backend.requestRecentChats();
    QTRY_VERIFY(!online.isEmpty() && !fixture.holdNextOnlineResponse);
    QTRY_COMPARE(unread(first), 0);
    QCOMPARE(unread(second), 0);

    fixture.chatResponseOverride = ajaxResult(R"({"chat":{"id":91,"dialogId":"chat91","lastId":700,
        "lastMessageId":700,"unreadId":0,"userCounter":16},"messages":[
        {"id":700,"chatId":91,"authorId":22,"text":"Already read","unread":false,"viewed":true}]})");
    backend.requestChatMessages(first);
    QTRY_VERIFY(!history.isEmpty());
    QCOMPARE(unread(first), 0);
    QVERIFY(!history.last().last().value<QVector<Message>>().last().unread);
    backend.readChatMessages(first, {QStringLiteral("700")});
    QVERIFY(fixture.requestsFor(read).isEmpty());

    // A first unread ID can mark an individual message while a count is still unavailable.
    fixture.chatResponseOverride = ajaxResult(R"({"chat":{"id":92,"dialogId":"chat92","lastId":800,
        "lastMessageId":801,"unreadId":801,"userCounter":74},"messages":[
        {"id":801,"chatId":92,"authorId":22,"text":"Reported unread","unread":true}]})");
    backend.requestChatMessages(second);
    QTRY_COMPARE(history.last().first().toString(), second);
    QCOMPARE(unread(second), 0);
    QVERIFY(history.last().last().value<QVector<Message>>().last().unread);
    fixture.releaseHeldOnlineResponse();
    QTRY_COMPARE(backend.unreadChatCount().value_or(-1), 3);
    QCOMPARE(unread(second), 0);
    auto count = history.size();
    backend.requestChatMessages(second);
    QTRY_VERIFY(history.size() > count);
    QCOMPARE(unread(second), 0);

    fixture.pushIm(QStringLiteral("new-after-snapshot"), QStringLiteral("messageChat"),
                   {{"dialogId", second}, {"chatId", 92}, {"counter", 1},
                    {"message", QJsonObject{{"id", 803}, {"chatId", 92}, {"senderId", 22}, {"text", "New incoming"}}}});
    QTRY_COMPARE(unread(second), 1);
    QCOMPARE(backend.unreadChatCount().value_or(-1), 4);
    fixture.chatResponseOverride = ajaxResult(R"({"chat":{"id":92,"dialogId":"chat92","lastId":802,
        "lastMessageId":803,"unreadId":803,"userCounter":74},"messages":[
        {"id":803,"chatId":92,"authorId":22,"text":"New incoming","unread":true}]})");
    count = history.size();
    backend.requestChatMessages(second);
    QTRY_VERIFY(history.size() > count);
    QCOMPARE(unread(second), 1);
    QVERIFY(history.last().last().value<QVector<Message>>().last().unread);

    const auto latest = QStringLiteral("803");
    fixture.approveVisibleRead = true;
    fixture.visibleReadResult = R"({"chatId":92,"counter":0,"viewedMessages":[803]})";
    backend.readChatMessages(second, {latest}, {latest});
    QTRY_COMPARE(confirmed.size(), 1);
    QCOMPARE(confirmed.last().first().toString(), second);
    QCOMPARE(confirmed.last().at(1).toStringList(), QStringList{latest});
    QCOMPARE(unread(second), 0);
    QCOMPARE(backend.unreadChatCount().value_or(-1), 3);
    const auto writes = fixture.requestsFor(read);
    QCOMPARE(writes.size(), 1);
    QCOMPARE(writes.first().field(QStringLiteral("chatId")), QStringLiteral("92"));
    QCOMPARE(writes.first().field(QStringLiteral("ids[0]")), latest);
    QVERIFY(writes.first().field(QStringLiteral("ids[1]")).isEmpty());

    count = history.size();
    backend.requestChatMessages(second);
    QTRY_VERIFY(history.size() > count);
    QCOMPARE(unread(second), 0);
    QVERIFY(!history.last().last().value<QVector<Message>>().last().unread);
    backend.readChatMessages(second, {latest}, {latest});
    QTest::qWait(30);
    QCOMPARE(fixture.requestsFor(read).size(), 1);
}

void TestCloudBackend::pagesProjectTaskChatsAndTracksOnlyTheirConfirmedBadges()
{
    CloudFixture fixture;
    fixture.chatBadgesFixture = true;
    fixture.messengerPage = R"HTML(<script>
        BX.Messenger.v2.Application.Launch('messenger', {"currentUser":{"id":11},
            "counters":[{"chatId":173,"counter":4,"isMarkedAsUnread":true}]});
        </script>)HTML";
    const auto firstProject = QStringLiteral("17");
    const auto secondProject = QStringLiteral("18");
    const auto firstChat = QStringLiteral("chat173");
    const auto secondChat = QStringLiteral("chat174");
    const auto route = QStringLiteral("/rest/tasks.task.list.json");
    const auto page = [](const QJsonArray &tasks, int next = -1) {
        QJsonObject response{{"result", QJsonObject{{"tasks", tasks}}}};
        if (next >= 0)
            response.insert("next", next);
        return QJsonDocument(response).toJson(QJsonDocument::Compact);
    };
    QJsonArray first{
        QJsonObject{{"id", "51"}, {"groupId", "17"}, {"group", QJsonObject{{"id", 17}}},
            {"chatId", "173"}, {"title", "Current"}, {"activityDate", "2026-11-23T13:10:00+03:00"}, {"userCounter", 99}},
        QJsonObject{{"id", 52}, {"groupId", 17}, {"GROUP_ID", 18}, {"chatId", 175}, {"title", "Conflicting group"}},
        QJsonObject{{"id", 53}, {"groupId", 18}, {"chatId", 176}, {"title", "Foreign group"}},
        QJsonObject{{"id", 54}, {"groupId", 17}, {"chatId", 173}, {"title", "Repeated chat"}},
        QJsonObject{{"id", 55}, {"groupId", 17}, {"chatId", 177}, {"CHAT_ID", 178}, {"title", "Conflicting chat"}},
        QJsonObject{{"id", 56}, {"chatId", 179}, {"title", "No verified group"}},
        QJsonObject{{"id", 57}, {"groupId", 17}, {"group", QJsonObject{{"id", 18}}},
            {"chatId", 180}, {"title", "Conflicting nested group"}}
    };
    while (first.size() < 50)
        first.append(QJsonObject{{"id", 60 + first.size()}, {"groupId", 17}, {"chatId", 0}, {"title", "No chat"}});
    fixture.projectTaskChatPages.insert(QStringLiteral("17:0"), page(first, 50));
    fixture.projectTaskChatPages.insert(QStringLiteral("17:50"), page({
        QJsonObject{{"id", 51}, {"groupId", 17}, {"chatId", 171}, {"title", "Repeated task"}},
        QJsonObject{{"ID", 32}, {"group", QJsonObject{{"ID", 17}}}, {"CHAT_ID", 174}, {"TITLE", "Older"}}}));
    fixture.projectTaskChatPages.insert(QStringLiteral("18:0"), page({}));
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy pages(&backend, &Backend::projectTaskChatsLoaded);
    QSignalSpy badges(&backend, &Backend::projectTaskChatBadgesChanged);
    QSignalSpy failures(&backend, &Backend::failed);
    QSignalSpy online(&backend, &Backend::connectionChanged);
    backend.start();
    backend.requestProjectTaskChats(firstProject);
    QTRY_COMPARE(pages.size(), 1);
    QCOMPARE(pages.last().at(0).toString(), firstProject);
    QCOMPARE(pages.last().at(1).toInt(), 0);
    QCOMPARE(pages.last().at(3).toInt(), 50);
    const auto items = pages.last().at(2).value<QVector<Chat>>();
    QCOMPARE(items.size(), 1);
    QCOMPARE(items.first().id, firstChat);
    QCOMPARE(items.first().serverChatId, 173);
    QCOMPARE(items.first().taskId, QStringLiteral("51"));
    QVERIFY(items.first().taskChat);
    QVERIFY(items.first().updated.isValid());
    const auto requests = fixture.requestsFor(route);
    QCOMPARE(requests.size(), 1);
    QCOMPARE(requests.first().method, QByteArray("POST"));
    QCOMPARE(requests.first().field(QStringLiteral("filter[GROUP_ID]")), firstProject);
    QCOMPARE(requests.first().field(QStringLiteral("start")), QStringLiteral("0"));
    QVERIFY(requests.first().form.queryItems(QUrl::FullyDecoded)
        .contains({QStringLiteral("select[]"), QStringLiteral("CHAT_ID")}));
    const auto current = [&] {
        if (!badges.isEmpty())
            return badges.last().at(1).value<QVector<Chat>>().value(0);
        return items.first();
    };
    QTRY_COMPARE(current().unread, 4);
    QVERIFY(current().markedUnread);
    QTRY_VERIFY(!online.isEmpty());
    backend.requestProjectTaskChats(firstProject, 50);
    QTRY_COMPARE(pages.size(), 2);
    QCOMPARE(pages.last().at(1).toInt(), 50);
    QCOMPARE(pages.last().at(3).toInt(), -1);
    const auto more = pages.last().at(2).value<QVector<Chat>>();
    QCOMPARE(more.size(), 1);
    QCOMPARE(more.first().id, secondChat);
    QCOMPARE(fixture.requestsFor(route).last().field(QStringLiteral("start")), QStringLiteral("50"));

    // This task chat was never in the Recent list or the baseline full counter set.
    fixture.pushIm(QStringLiteral("project-live-message"), QStringLiteral("messageChat"),
        {{"chatId", 174}, {"dialogId", secondChat}, {"counter", 2},
         {"message", QJsonObject{{"id", 680}, {"chatId", 174}, {"senderId", 22}, {"text", "New"}}}});
    QTRY_COMPARE(current().id, secondChat);
    QCOMPARE(current().unread, 2);
    fixture.pushIm(QStringLiteral("project-live-read"), QStringLiteral("readMessageChat"),
        {{"chatId", 174}, {"dialogId", secondChat}, {"counter", 0}, {"lastId", 680}});
    QTRY_COMPARE(current().unread, 0);
    QCOMPARE(badges.last().at(0).toString(), firstProject);
    QCOMPARE(fixture.requestsFor(route).size(), 2);

    fixture.holdNotificationReply = route;
    backend.requestProjectTaskChats(firstProject);
    QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());
    backend.requestProjectTaskChats(secondProject);
    QTRY_COMPARE(pages.size(), 3);
    QCOMPARE(pages.last().at(0).toString(), secondProject);
    QCOMPARE(pages.last().at(3).toInt(), -1);
    fixture.releaseHeldNotificationReply();
    QTest::qWait(80);
    QCOMPARE(pages.size(), 3);
    QVERIFY(failures.isEmpty());
    backend.requestProjectTaskChats({});
    const auto before = badges.size();
    fixture.pushIm(QStringLiteral("project-closed-message"), QStringLiteral("messageChat"),
        {{"chatId", 174}, {"dialogId", secondChat}, {"counter", 3},
         {"message", QJsonObject{{"id", 682}, {"chatId", 174}, {"senderId", 22}, {"text", "New"}}}});
    QTest::qWait(80);
    QCOMPARE(badges.size(), before);
}

void TestCloudBackend::aggregatesUnreadProjectTasksWithoutOpeningTheirChatList()
{
    CloudFixture fixture;
    fixture.messengerPage = R"HTML(<script>
        BX.Messenger.v2.Application.Launch('messenger', {"currentUser":{"id":11},"counters":[
            {"chatId":173,"counter":4,"type":"tasksTask","parentChatId":890,"isMarkedAsUnread":true},
            {"chatId":174,"counter":2,"type":"tasksTask","parentChatId":890},
            {"chatId":190,"counter":12,"type":"tasksTask","parentChatId":0},
            {"chatId":191,"counter":44,"type":"tasksTask","parentChatId":0},
            {"chatId":192,"counter":15,"type":"chat"},
            {"chatId":193,"counter":0,"type":"tasksTask","parentChatId":890,"isMarkedAsUnread":true,"isNonAnswered":true}
        ]});
        </script>)HTML";
    const auto dialog = [](qint64 chat, const char *kind, const char *entity, qint64 id) {
        return QJsonObject{{"id", chat}, {"dialog_id", QStringLiteral("chat") + QString::number(chat)},
                           {"type", kind}, {"entity_type", entity}, {"entity_id", id},
                           {"entity_link", QJsonObject{{"type", QByteArrayView(entity) == "TASKS_TASK" ? "TASKS" : entity},
                                                      {"id", id}}}};
    };
    fixture.projectUnreadDialogReplies = {{"890", dialog(890, "collab", "SONET_GROUP", 17)},
        {"190", dialog(190, "tasksTask", "TASKS_TASK", 101)}, {"191", dialog(191, "tasksTask", "TASKS_TASK", 102)}};
    fixture.projectUnreadTaskReplies = {QJsonObject{{"id", 101}, {"chatId", 190}, {"groupId", 18}},
        QJsonObject{{"id", 102}, {"chatId", 191}, {"groupId", 0}},
        QJsonObject{{"id", 110}, {"chatId", 192}, {"groupId", 17}}};
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy changes(&backend, &Backend::projectUnreadCountsChanged);
    backend.start();
    QTRY_COMPARE(backend.unreadChatCount().value_or(-1), 78);
    QVERIFY(fixture.requestsFor(batchPath).isEmpty());
    const auto taskList = QStringLiteral("/rest/tasks.task.list.json");
    QVERIFY(fixture.requestsFor(taskList).isEmpty());
    backend.setActiveSection(1);
    const auto counts = [&changes] {
        return changes.isEmpty() ? QHash<QString, qint64>{} : changes.last().at(0).value<QHash<QString, qint64>>();
    };
    const auto complete = [&changes] { return !changes.isEmpty() && changes.last().at(1).toBool(); };
    QTRY_VERIFY(complete());
    QCOMPARE(counts().size(), 2);
    QCOMPARE(counts().value(QStringLiteral("17")), 6);
    QCOMPARE(counts().value(QStringLiteral("18")), 12);
    QCOMPARE(fixture.requestsFor(batchPath).size(), 1);
    const auto form = fixture.requestsFor(batchPath).first().form.queryItems(QUrl::FullyDecoded);
    QCOMPARE(std::count_if(form.cbegin(), form.cend(), [](const auto &part) {
        return part.first.startsWith(QLatin1String("cmd[lookup_"));
    }), 3);
    QCOMPARE(fixture.requestsFor(taskList).size(), 1);
    const auto groupRequests = fixture.requestsFor(taskList).first();
    QVERIFY(!groupRequests.field(QStringLiteral("filter[ID][0]")).isEmpty());
    QVERIFY(!groupRequests.field(QStringLiteral("filter[ID][1]")).isEmpty());
    QVERIFY(groupRequests.field(QStringLiteral("filter[GROUP_ID]")).isEmpty());

    fixture.pushIm(QStringLiteral("project-aggregate-read"), QStringLiteral("readMessageChat"),
        {{"chatId", 173}, {"dialogId", "chat173"}, {"counter", 0}, {"lastId", 800}});
    QTRY_COMPARE(counts().value(QStringLiteral("17")), 2);
    fixture.pushIm(QStringLiteral("project-aggregate-new"), QStringLiteral("messageChat"),
        {{"chatId", 174}, {"dialogId", "chat174"}, {"counter", 7},
         {"message", QJsonObject{{"id", 801}, {"chatId", 174}, {"senderId", 22}}}});
    QTRY_COMPARE(counts().value(QStringLiteral("17")), 7);
    fixture.pushIm(QStringLiteral("project-aggregate-move"), QStringLiteral("task_update"),
        {{"taskId", 101}, {"fields", QJsonObject{{"GROUP_ID", 17}}}}, 0, QStringLiteral("tasks"));
    QTRY_COMPARE(counts().value(QStringLiteral("17")), 19);
    QVERIFY(!counts().contains(QStringLiteral("18")));
    QVERIFY(complete());

    // The parent-resolved task has no individual identity yet. The second
    // authenticated move must outrank the already queued lookup response.
    QSignalSpy moves(&backend, &Backend::taskLinkPreviewInvalidated);
    fixture.projectUnreadTaskReplies = {QJsonObject{{"id", 777}, {"chatId", 174}, {"groupId", 18}}};
    fixture.holdNotificationReply = taskList;
    fixture.pushIm(QStringLiteral("unknown-task-first-move"), QStringLiteral("task_update"),
        {{"taskId", 777}, {"fields", QJsonObject{{"GROUP_ID", 18}}}}, 0, QStringLiteral("tasks"));
    QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());
    fixture.pushIm(QStringLiteral("unknown-task-second-move"), QStringLiteral("task_update"),
        {{"taskId", 777}, {"fields", QJsonObject{{"GROUP_ID", 19}}}}, 0, QStringLiteral("tasks"));
    QTRY_COMPARE(moves.size(), 2);
    QCOMPARE(fixture.requestsFor(taskList).size(), 2);
    fixture.releaseHeldNotificationReply();
    QTRY_COMPARE(counts().value(QStringLiteral("19")), 7);
    QCOMPARE(counts().value(QStringLiteral("17")), 12);
    QVERIFY(!counts().contains(QStringLiteral("18")));

    backend.setActiveSection(0);
    fixture.pushIm(QStringLiteral("project-aggregate-read-away"), QStringLiteral("readMessageChat"),
        {{"chatId", 174}, {"dialogId", "chat174"}, {"counter", 0}, {"lastId", 801}});
    // Reading messages doesn't clear chat173's separate manual unread mark.
    QTRY_COMPARE(backend.unreadChatCount().value_or(-1), 73);
    QCOMPARE(counts().value(QStringLiteral("19")), 7);
    backend.setActiveSection(1);
    QTRY_COMPARE(counts().value(QStringLiteral("17")), 12);
    QVERIFY(!counts().contains(QStringLiteral("19")));
    QCOMPARE(fixture.requestsFor(batchPath).size(), 1);
    QCOMPARE(fixture.requestsFor(taskList).size(), 2);
    backend.stop();
    QVERIFY(counts().isEmpty());
    QVERIFY(!complete());
}

void TestCloudBackend::decodesRichNotificationsFromHistoryAndPush()
{
    CloudFixture fixture;
    fixture.noticeFixture = QJsonDocument::fromJson(R"json([
        {"id":41,"notify_module":"bizproc","notify_title":"","text":"","notify_read":"N","params":{"ATTACH":[
           {"ID":1,"DESCRIPTION":"Выполнено действие","BLOCKS":[
             {"GRID":[{"NAME":"Задача","VALUE":"Принтер","LINK":"/workgroups/group/119/tasks/task/view/43/"}]},
             {"MESSAGE":"Готово"}]}]}},
        {"id":42,"notify_module":"tasks","text":"Назначение задачи","notify_read":"N","params":{
          "COMPONENT_ID":"DefaultEntity","COMPONENT_PARAMS":{"PLAIN_TEXT":"Принтер",
            "SUBJECT":"Назначена [URL=/workgroups/group/119/tasks/task/view/42/]Задача 42[/URL] <script>Злоумышленник</script> [URL=javascript:alert(1)]Неверная ссылка[/URL]"}}}
    ])json").array();
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy online(&backend, &Backend::connectionChanged);
    QSignalSpy loaded(&backend, &Backend::notificationsLoaded);
    QSignalSpy found(&backend, &Backend::notificationsFound);
    backend.setForeground(false);
    backend.start();
    backend.requestNotifications();
    QTRY_COMPARE(loaded.size(), 1);
    QTRY_COMPARE(online.size(), 1);
    const auto notices = loaded.first().first().value<QVector<Notification>>();
    QCOMPARE(notices.size(), 2);
    const auto byId = [&notices](const QString &id) {
        return *std::find_if(notices.cbegin(), notices.cend(), [&id](const Notification &row) {
            return row.id == id;
        });
    };
    const auto attachment = byId(QStringLiteral("41"));
    QVERIFY(attachment.text.contains(QStringLiteral("Выполнено действие")));
    QVERIFY(attachment.text.contains(QStringLiteral("Принтер")));
    QVERIFY(attachment.markup.contains(QStringLiteral("[URL=")));
    QCOMPARE(attachment.targets.size(), 1);
    QCOMPARE(attachment.targets.front().id, QStringLiteral("43"));
    QCOMPARE(attachment.targets.front().projectId, QStringLiteral("119"));

    const auto task = byId(QStringLiteral("42"));
    QCOMPARE(task.text, QStringLiteral("Назначение задачи"));
    QVERIFY(task.markup.contains(QStringLiteral("Задача 42[/URL]")));
    QVERIFY(!task.markup.contains(QStringLiteral("Злоумышленник")));
    QVERIFY(!task.markup.contains(QStringLiteral("[URL=javascript:")));
    QCOMPARE(task.targets.size(), 1);
    QCOMPARE(task.targets.front().id, QStringLiteral("42"));

    backend.searchNotifications(QStringLiteral("Принтер"));
    QTRY_COMPARE(found.size(), 1);
    QCOMPARE(found.first().last().value<QVector<Notification>>().first().markup, attachment.markup);
    auto pushed = fixture.noticeFixture.first().toObject();
    pushed.insert(QStringLiteral("id"), 51);
    fixture.pushIm(QStringLiteral("rich-notice"), QStringLiteral("notifyAdd"),
                   {{"id", 51}, {"notify", pushed}});
    QTRY_COMPARE(loaded.size(), 2);
    const auto incoming = loaded.last().first().value<QVector<Notification>>().first();
    QCOMPARE(incoming.id, QStringLiteral("51"));
    QCOMPARE(incoming.text, attachment.text);
    QCOMPARE(incoming.markup, attachment.markup);
    QCOMPARE(incoming.targets.size(), 1);
    QCOMPARE(incoming.targets.front().id, attachment.targets.front().id);
}

void TestCloudBackend::resolvesNoticeAuthorFromMatchingUserOnly()
{
    CloudFixture fixture;
    const QJsonObject content{{"COMPONENT_PARAMS", QJsonObject{
        {"SUBJECT", "#AUTHOR# назначил [URL=/tasks/task/view/123511/]задачу[/URL]"}}}};
    fixture.noticeFixture = {
        QJsonObject{{"id", 41}, {"author_id", 121}, {"text", "#AUTHOR# назначил задачу"}, {"params", content}},
        QJsonObject{{"id", 42}, {"author_id", 122}, {"text", "#AUTHOR# назначил задачу"}, {"params", content}},
        QJsonObject{{"id", 43}, {"author_id", 121}, {"text", "[URL=/company/personal/user/999/]#AUTHOR#[/URL]"},
                    {"params", QJsonObject{
            {"COMPONENT_PARAMS", QJsonObject{{"SUBJECT", "[URL=/company/personal/user/999/]#AUTHOR#[/URL] назначил"}}}}}}
    };
    const QJsonObject verified{{"id", 121}, {"name", "Автор [URL=bad]"}};
    fixture.noticeUsersFixture = {{"121", verified}, {"122", QJsonObject{{"id", 123}, {"name", "Чужое имя"}}}};
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    backend.setForeground(false);
    QSignalSpy online(&backend, &Backend::connectionChanged);
    QSignalSpy loaded(&backend, &Backend::notificationsLoaded);
    QSignalSpy found(&backend, &Backend::notificationsFound);
    backend.start();
    backend.requestNotifications();
    QTRY_COMPARE(loaded.size(), 1);
    const auto notices = loaded.first().first().value<QVector<Notification>>();
    QCOMPARE(notices.size(), 3);
    const auto verifiedNotice = notices.first();
    QVERIFY(verifiedNotice.markup.contains(QStringLiteral("/company/personal/user/121/")));
    QVERIFY(!verifiedNotice.markup.contains(QStringLiteral("[URL=bad]")));
    QVERIFY(verifiedNotice.text.startsWith(QStringLiteral("Автор")));
    QVERIFY(!verifiedNotice.markup.contains(QStringLiteral("#AUTHOR#")));
    QVERIFY(!notices.at(1).markup.contains(QStringLiteral("#AUTHOR#")));
    QVERIFY(!notices.at(1).text.contains(QStringLiteral("#AUTHOR#")));
    QVERIFY(!notices.at(1).markup.contains(QStringLiteral("Чужое имя")));
    QVERIFY(notices.last().markup.contains(QStringLiteral("/company/personal/user/121/")));
    QVERIFY(!notices.last().markup.contains(QStringLiteral("/company/personal/user/999/")));
    QCOMPARE(notices.last().targets.size(), 1);
    QCOMPARE(notices.last().targets.first().id, QStringLiteral("121"));
    backend.searchNotifications(QStringLiteral("задачу"));
    QTRY_COMPARE(found.size(), 1);
    QCOMPARE(found.first().last().value<QVector<Notification>>().first().markup, verifiedNotice.markup);

    QTRY_COMPARE(online.size(), 1);
    fixture.pushIm(QStringLiteral("explicit-author"), QStringLiteral("notifyAdd"),
                   {{"id", 44}, {"notify", QJsonObject{{"id", 44}, {"authorId", 121}, {"params", content}}},
                    {"user", verified}});
    QTRY_COMPARE(loaded.size(), 2);
    const auto pushed = loaded.last().first().value<QVector<Notification>>().first();
    QCOMPARE(pushed.id, QStringLiteral("44"));
    QCOMPARE(pushed.markup, verifiedNotice.markup);
}

void TestCloudBackend::linksTaskChatEventsAndMarksSyntheticRowsLocally()
{
    CloudFixture fixture;
    fixture.taskRecentBadgeFixture = R"({"userId":11,
        "recentItems":[{"dialogId":"chat155345","chatId":155345}],
        "chats":[{"id":155345,"dialogId":"chat155345","type":"tasksTask",
                  "name":"Известная задача","entityLink":{"type":"TASKS","id":123485}}]})";
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    backend.setForeground(false);
    QSignalSpy online(&backend, &Backend::connectionChanged);
    QSignalSpy loaded(&backend, &Backend::notificationsLoaded);
    QSignalSpy tasks(&backend, &Backend::taskChatsLoaded);
    QSignalSpy read(&backend, &Backend::notificationReadStateChanged);
    QSignalSpy finished(&backend, &Backend::notificationActionFinished);
    backend.start();
    QTRY_COMPARE(online.size(), 1);
    fixture.pushIm(QStringLiteral("task-chat-exact"), QStringLiteral("messageChat"),
                   {{"dialogId", "chat86761"}, {"chatId", 86761},
                    {"chat", QJsonObject{{"86761", QJsonObject{{"id", 86761}, {"type", "tasksTask"},
                                                               {"entityType", "TASKS"}, {"entityId", 123511}}}}},
                    {"message", QJsonObject{{"id", 901}, {"chatId", 86761}, {"senderId", 22}, {"text", "Готово"}}}});
    QTRY_COMPARE(loaded.size(), 1);
    const auto notice = loaded.last().first().value<QVector<Notification>>().first();
    QCOMPARE(notice.id, QStringLiteral("message:901"));
    QCOMPARE(notice.targets.size(), 2);
    QCOMPARE(notice.targets.first().kind, NotificationTargetKind::Chat);
    QCOMPARE(notice.targets.first().id, QStringLiteral("chat86761"));
    QCOMPARE(notice.targets.first().messageId, QStringLiteral("901"));
    QVERIFY(notice.targets.first().taskChat);
    QCOMPARE(notice.targets.last().kind, NotificationTargetKind::Task);
    QCOMPARE(notice.targets.last().id, QStringLiteral("123511"));
    QVERIFY(notice.markup.contains(QStringLiteral("/tasks/task/view/123511/")));
    QVERIFY(notice.markup.contains(QStringLiteral("#123511")));

    backend.markNotificationsRead({notice.id, QStringLiteral("message:999"), notice.id});
    QCOMPARE(read.size(), 1);
    QCOMPARE(read.first().first().toStringList(), QStringList{notice.id});
    QCOMPARE(finished.size(), 1);
    QVERIFY(fixture.requestsFor(batchPath).isEmpty());
    QVERIFY(fixture.requestsFor(QStringLiteral("im.v2.Chat.Message.read")).isEmpty());
    backend.requestNotifications();
    QTRY_COMPARE(loaded.size(), 2);
    const auto rows = loaded.last().first().value<QVector<Notification>>();
    QVERIFY(std::any_of(rows.cbegin(), rows.cend(), [&notice](const auto &row) { return row.id == notice.id && row.read; }));

    backend.requestTaskChatBadges();
    QTRY_COMPARE(tasks.size(), 1);
    fixture.pushIm(QStringLiteral("task-chat-cached"), QStringLiteral("messageChat"),
                   {{"dialogId", "chat155345"}, {"chatId", 155345},
                    {"message", QJsonObject{{"id", 902}, {"chatId", 155345}, {"senderId", 22}, {"text", "Готово"}}}});
    QTRY_COMPARE(loaded.size(), 3);
    const auto cached = loaded.last().first().value<QVector<Notification>>().first();
    QCOMPARE(cached.id, QStringLiteral("message:902"));
    QCOMPARE(cached.targets.size(), 2);
    QCOMPARE(cached.targets.last().id, QStringLiteral("123485"));
    backend.markNotificationsRead({cached.id, QStringLiteral("77")});
    QCOMPARE(read.last().first().toStringList(), QStringList{cached.id});
    QTRY_COMPARE(finished.size(), 2);
    const auto command = fixture.requestsFor(batchPath).last().field(QStringLiteral("cmd[notify]"));
    QCOMPARE(command.section(QLatin1Char('?'), 0, 0), QStringLiteral("im.notify.read.list"));
    QCOMPARE(QUrlQuery(command.section(QLatin1Char('?'), 1)).queryItems(QUrl::FullyDecoded),
             (QList<QPair<QString, QString>>{{QStringLiteral("IDS[]"), QStringLiteral("77")},
                                            {QStringLiteral("ACTION"), QStringLiteral("Y")}}));

    fixture.pushIm(QStringLiteral("task-chat-foreign-metadata"), QStringLiteral("messageChat"),
                   {{"dialogId", "chat155346"}, {"chatId", 155346},
                    {"chat", QJsonObject{{"155346", QJsonObject{{"id", 155345}, {"type", "tasksTask"},
                                                                {"entityType", "TASKS_TASK"}, {"entityId", 123485}}}}},
                    {"message", QJsonObject{{"id", 903}, {"chatId", 155346}, {"senderId", 22}, {"text", "Без задачи"}}}});
    QTRY_COMPARE(loaded.size(), 4);
    const auto unconfirmed = loaded.last().first().value<QVector<Notification>>().first();
    QCOMPARE(unconfirmed.id, QStringLiteral("message:903"));
    QCOMPARE(unconfirmed.targets.size(), 1);
    QCOMPARE(unconfirmed.targets.first().kind, NotificationTargetKind::Chat);
}

void TestCloudBackend::classifiesOrdinaryChatPushesWithoutSuppressingPopups()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    backend.setForeground(false);
    QSignalSpy online(&backend, &Backend::connectionChanged);
    QSignalSpy loaded(&backend, &Backend::notificationsLoaded);
    QSignalSpy popups(&backend, &CloudBackend::notificationReceived);
    backend.start();
    QTRY_COMPARE(online.size(), 1);
    fixture.pushIm(QStringLiteral("personal-chat"), QStringLiteral("message"),
                   {{"dialogId", "103"}, {"chatId", 93}, {"notify", true},
                    {"message", QJsonObject{{"id", 901}, {"chatId", 93}, {"senderId", 103},
                        {"text", "[URL=/tasks/task/view/501/]Ссылка на задачу[/URL]"}}}});
    QTRY_COMPARE(popups.size(), 1);
    const auto personal = popups.first().first().value<Notification>();
    QCOMPARE(personal.id, QStringLiteral("message:901"));
    QVERIFY(personal.ordinaryChatMessage);
    QCOMPARE(personal.targets.first().id, QStringLiteral("103"));
    QCOMPARE(personal.targets.first().messageId, QStringLiteral("901"));
    QVERIFY(std::any_of(personal.targets.cbegin(), personal.targets.cend(), [](const auto &target) {
        return target.kind == NotificationTargetKind::Task;
    }));

    fixture.pushMessage(QStringLiteral("group-chat"), QStringLiteral("902"));
    QTRY_COMPARE(popups.size(), 2);
    const auto group = popups.last().first().value<Notification>();
    QCOMPARE(group.id, QStringLiteral("message:902"));
    QCOMPARE(group.targets.first().id, QStringLiteral("chat91"));
    QCOMPARE(group.targets.first().messageId, QStringLiteral("902"));
    QVERIFY(group.ordinaryChatMessage);
    fixture.pushMessage(QStringLiteral("group-chat-retry"), QStringLiteral("902"));
    fixture.pushIm(QStringLiteral("group-no-id"), QStringLiteral("messageChat"),
                   {{"dialogId", "chat91"}, {"chatId", 91}, {"notify", true},
                    {"message", QJsonObject{{"authorId", 22}, {"text", "Missing message ID"}}}});
    QTRY_COMPARE(popups.size(), 3);
    const auto noId = popups.last().first().value<Notification>();
    QCOMPARE(noId.id, QStringLiteral("push:group-no-id"));
    QVERIFY(noId.ordinaryChatMessage && noId.targets.isEmpty());

    fixture.pushIm(QStringLiteral("task-chat"), QStringLiteral("messageChat"),
                   {{"dialogId", "chat86761"}, {"chatId", 86761}, {"notify", true},
                    {"chat", QJsonObject{{"86761", QJsonObject{{"id", 86761}, {"type", "tasksTask"},
                                                               {"entityType", "TASKS"}, {"entityId", 501}}}}},
                    {"message", QJsonObject{{"id", 903}, {"authorId", 22}, {"text", "Task message"}}}});
    fixture.pushIm(QStringLiteral("portal-notification"), QStringLiteral("notifyAdd"),
                   {{"id", 904}, {"text", "[URL=/online/?IM_DIALOG=chat91&IM_MESSAGE=902]Настоящее уведомление[/URL]"}});
    QTRY_COMPARE(loaded.size(), 5);
    const auto records = loaded.last().first().value<QVector<Notification>>();
    QCOMPARE(records.size(), 5);
    QCOMPARE(records.at(0).id, QStringLiteral("904"));
    QVERIFY(!records.at(0).ordinaryChatMessage);
    QCOMPARE(records.at(0).targets.first().id, QStringLiteral("chat91"));
    QCOMPARE(records.at(1).id, QStringLiteral("message:903"));
    QVERIFY(!records.at(1).ordinaryChatMessage);
    QVERIFY(records.at(1).targets.first().taskChat);
    QVERIFY(records.at(2).ordinaryChatMessage && records.at(3).ordinaryChatMessage && records.at(4).ordinaryChatMessage);
    QTRY_COMPARE(popups.size(), 5);

    fixture.pushIm(QStringLiteral("task-chat-sections"), QStringLiteral("messageChat"),
                   {{"dialogId", "chat86762"}, {"chatId", 86762},
                    {"recentConfig", QJsonObject{{"sections", QJsonArray{"tasksTask"}}}},
                    {"message", QJsonObject{{"id", 905}, {"authorId", 22}, {"text", "Task message with no task ID"}}}});
    QTRY_COMPARE(loaded.size(), 6);
    const auto sectionTask = loaded.last().first().value<QVector<Notification>>().first();
    QCOMPARE(sectionTask.id, QStringLiteral("message:905"));
    QVERIFY(!sectionTask.ordinaryChatMessage);
    QVERIFY(sectionTask.targets.first().taskChat);

    for (int id = 1100; id <= 1200; ++id) {
        const auto value = QString::number(id);
        fixture.pushMessage(QStringLiteral("quiet-chat-") + value, value, 91, 22, false, 60);
    }
    QTRY_COMPARE(loaded.size(), 107);
    const auto visibleIds = [&loaded] {
        QStringList ids;
        for (const auto &row : loaded.last().first().value<QVector<Notification>>())
            if (!row.ordinaryChatMessage)
                ids.append(row.id);
        return ids;
    };
    QStringList visible{QStringLiteral("message:905"), QStringLiteral("904"), QStringLiteral("message:903")};
    QCOMPARE(visibleIds(), visible);
    QCOMPARE(loaded.last().first().value<QVector<Notification>>().size(), 103);
    QCOMPARE(popups.size(), 5);
    fixture.noticeFixture = QJsonArray{QJsonObject{{"id", 906}, {"notify_type", 2}, {"notify_module", "tasks"},
        {"text", "Portal history row"}, {"date", QDateTime::currentDateTimeUtc().addSecs(-120).toString(Qt::ISODate)}}};
    backend.requestNotifications();
    QTRY_COMPARE(loaded.size(), 108);
    visible.append(QStringLiteral("906"));
    QCOMPARE(visibleIds(), visible);
    QCOMPARE(loaded.last().first().value<QVector<Notification>>().size(), 104);
}

void TestCloudBackend::marksExactNoticeIdsAsReadAfterBatchConfirmation()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    backend.setForeground(false);
    QSignalSpy online(&backend, &Backend::connectionChanged);
    QSignalSpy read(&backend, &Backend::notificationReadStateChanged);
    QSignalSpy finished(&backend, &Backend::notificationActionFinished);
    QSignalSpy errors(&backend, &Backend::failed);
    backend.start();
    QTRY_COMPARE(online.size(), 1);
    fixture.holdNotificationReply = QStringLiteral("im.notify.read.list");
    backend.markNotificationsRead({QStringLiteral("42"), QStringLiteral("41"), QStringLiteral("42"),
                                   QStringLiteral("0"), QStringLiteral("43&ID=9"), QStringLiteral("041")});
    QTRY_COMPARE_WITH_TIMEOUT(fixture.requestsFor(batchPath).size(), 1, 1500);
    const auto request = fixture.requestsFor(batchPath).first();
    const auto command = request.field(QStringLiteral("cmd[notify]"));
    QCOMPARE(command.section(QLatin1Char('?'), 0, 0), QStringLiteral("im.notify.read.list"));
    const auto parameters = QUrlQuery(command.section(QLatin1Char('?'), 1)).queryItems(QUrl::FullyDecoded);
    QCOMPARE(parameters, (QList<QPair<QString, QString>>{{QStringLiteral("IDS[]"), QStringLiteral("42")},
              {QStringLiteral("IDS[]"), QStringLiteral("41")}, {QStringLiteral("ACTION"), QStringLiteral("Y")}}));
    QCOMPARE(request.headers.value("cookie"), fixtureCookie);
    QCOMPARE(request.headers.value("x-bitrix-csrf-token"), fixtureCsrf);
    QVERIFY(read.isEmpty());

    fixture.pushIm(QStringLiteral("newer-batch-unread"), QStringLiteral("notifyUnread"), {{"list", QJsonArray{42}}});
    QTRY_COMPARE(read.size(), 1);
    QCOMPARE(read.first().first().toStringList(), QStringList{QStringLiteral("42")});
    QCOMPARE(read.first().last().toBool(), false);
    fixture.releaseHeldNotificationReply();
    QTRY_COMPARE(read.size(), 2);
    QCOMPARE(read.last().first().toStringList(), QStringList{QStringLiteral("41")});
    QCOMPARE(read.last().last().toBool(), true);
    QCOMPARE(finished.first().first().toString(), QStringLiteral("read-notifications"));
    QVERIFY(errors.isEmpty());

    fixture.rejectNoticeBatch = true;
    backend.markNotificationsRead({QStringLiteral("43")});
    QTRY_COMPARE(errors.size(), 1);
    QCOMPARE(errors.first().first().toString(), QStringLiteral("read-notifications"));
    QCOMPARE(read.size(), 2);
}

void TestCloudBackend::ignoresNotificationRepliesFromPreviousAccount_data()
{
    QTest::addColumn<QString>("action");
    QTest::addColumn<bool>("rejectOldBatch");
    QTest::newRow("history") << QStringLiteral("im.notify.get") << false;
    QTest::newRow("search") << QStringLiteral("im.notify.history.search") << false;
    QTest::newRow("read-success") << QStringLiteral("im.notify.read.list") << false;
    QTest::newRow("read-failure") << QStringLiteral("im.notify.read.list") << true;
}

void TestCloudBackend::ignoresNotificationRepliesFromPreviousAccount()
{
    QFETCH(QString, action);
    QFETCH(bool, rejectOldBatch);
    CloudFixture fixture;
    fixture.ordinaryRecentBadgeFixture = R"({"userId":11,"recentItems":[],"chats":[]})";
    fixture.noticeFixture = {QJsonObject{{"id", 41}, {"text", "First account"}, {"notify_read", "N"}}};
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    backend.setForeground(false);
    QSignalSpy accounts(&backend, &Backend::accountScopeChanged);
    QSignalSpy loaded(&backend, &Backend::notificationsLoaded);
    QSignalSpy found(&backend, &Backend::notificationsFound);
    QSignalSpy read(&backend, &Backend::notificationReadStateChanged);
    QSignalSpy finished(&backend, &Backend::notificationActionFinished);
    QSignalSpy errors(&backend, &Backend::failed);
    backend.start();

    fixture.holdNotificationReply = QStringLiteral("im.notify.get");
    backend.requestNotifications();
    QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());
    backend.requestRecentChats();
    QTRY_COMPARE(accounts.size(), 1);
    fixture.releaseHeldNotificationReply();
    QTRY_COMPARE(loaded.size(), 1);
    QCOMPARE(loaded.first().first().value<QVector<Notification>>().first().id, QStringLiteral("41"));
    loaded.clear();

    const auto send = [&] {
        if (action == QLatin1String("im.notify.get"))
            backend.requestNotifications();
        else if (action == QLatin1String("im.notify.history.search"))
            backend.searchNotifications(QStringLiteral("учётная запись"));
        else
            backend.markNotificationsRead({QStringLiteral("41")});
    };
    fixture.holdNotificationReply = action;
    fixture.rejectNoticeBatch = rejectOldBatch;
    send();
    QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());
    fixture.ordinaryRecentBadgeFixture = R"({"userId":22,"recentItems":[],"chats":[]})";
    backend.requestRecentChats();
    QTRY_COMPARE(accounts.size(), 2);

    fixture.noticeFixture = {QJsonObject{{"id", 99}, {"text", "Second account"}, {"notify_read", "N"}}};
    fixture.rejectNoticeBatch = false;
    const auto &outcome = action == QLatin1String("im.notify.get") ? loaded
        : action == QLatin1String("im.notify.history.search") ? found : finished;
    send();
    QTRY_COMPARE_WITH_TIMEOUT(outcome.size(), 1, 1500);
    if (action != QLatin1String("im.notify.read.list")) {
        const auto notices = outcome.first().last().value<QVector<Notification>>();
        QCOMPARE(notices.size(), 1);
        QCOMPARE(notices.first().id, QStringLiteral("99"));
    } else {
        QCOMPARE(read.size(), 1);
    }
    fixture.releaseHeldNotificationReply();
    QTest::qWait(120);
    QCOMPARE(outcome.size(), 1);
    QCOMPARE(errors.size(), 0);
    QCOMPARE(read.size(), action == QLatin1String("im.notify.read.list") ? 1 : 0);
}

void TestCloudBackend::reloadsNotificationSettingsAfterPortalUpdate()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    fixture.serverNotificationScheme = QStringLiteral("simple");
    CloudBackend backend(fixture.session(), true);
    QSignalSpy settings(&backend, &Backend::notificationSettingsLoaded);
    QSignalSpy errors(&backend, &Backend::failed);
    backend.start();
    backend.requestNotificationSettings();
    QTRY_COMPARE(settings.size(), 1);
    const auto general = QStringLiteral("soundVolumeGeneral");
    const auto site = QStringLiteral("notifySchemeSendSite");
    auto confirmed = [&settings] { return settings.last().first().toMap(); };
    QCOMPARE(confirmed().value(general).toInt(), 100);
    QCOMPARE(confirmed().value(site).toBool(), true);
    QVERIFY(!confirmed().contains(QStringLiteral("notifySchemeSendEmail")));
    backend.updateNotificationSetting(general, 35);
    QTRY_COMPARE(settings.size(), 2);
    QCOMPARE(confirmed().value(general).toInt(), 35);
    const auto writes = fixture.requestsFor(QStringLiteral("im.v2.Settings.General.update"));
    QCOMPARE(writes.size(), 1);
    const auto wire = QJsonDocument::fromJson(writes.first().body).object();
    QCOMPARE(wire.value("name").toString(), general);
    QCOMPARE(wire.value("value").toInt(), 35);
    QVERIFY(!QUuid(wire.value("actionUuid").toString()).isNull());
    QCOMPARE(writes.first().headers.value("x-bitrix-csrf-token"), fixtureCsrf);

    fixture.ignoreSoundUpdate = true;
    backend.updateNotificationSetting(general, 36);
    QTRY_COMPARE(errors.size(), 1);
    QCOMPARE(confirmed().value(general).toInt(), 35);
    backend.updateNotificationSetting(QStringLiteral("notifyScheme"), 0);
    QCOMPARE(errors.size(), 2);
    QCOMPARE(fixture.requestsFor(QStringLiteral("im.v2.Settings.General.update")).size(), 2);

    fixture.ignoreSoundUpdate = false;
    backend.updateNotificationSetting(site, 0);
    QTRY_COMPARE(settings.size(), 4);
    QVERIFY(!confirmed().value(site).toBool());
    const auto delivery = fixture.requestsFor(QStringLiteral("im.v2.Settings.General.update")).last();
    QCOMPARE(QJsonDocument::fromJson(delivery.body).object().value("value"), QJsonValue(false));
    fixture.serverNotificationScheme = QStringLiteral("expert");
    backend.requestNotificationSettings();
    QTRY_COMPARE(settings.size(), 5);
    QVERIFY(!confirmed().contains(site));
    backend.updateNotificationSetting(site, 1);
    QCOMPARE(errors.size(), 3);
    QCOMPARE(fixture.requestsFor(QStringLiteral("im.v2.Settings.General.update")).size(), 3);
}

void TestCloudBackend::notificationActionsKeepPushNewerThanTheRequest()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy online(&backend, &Backend::connectionChanged);
    QSignalSpy found(&backend, &Backend::notificationsFound);
    QSignalSpy read(&backend, &Backend::notificationReadStateChanged);
    QSignalSpy removed(&backend, &Backend::notificationRemoved);
    QSignalSpy finished(&backend, &Backend::notificationActionFinished);
    backend.start();
    QTRY_VERIFY(!online.isEmpty() && online.last().first().toBool());
    const auto id = QStringLiteral("77");
    backend.searchNotifications(QStringLiteral("задача"));
    QTRY_COMPARE(found.size(), 1);

    fixture.holdNotificationReply = QStringLiteral("im.notify.read");
    backend.markNotificationRead(id);
    QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());
    fixture.pushIm(QStringLiteral("newer-unread"), QStringLiteral("notifyUnread"), {{"list", QJsonArray{77}}});
    QTRY_COMPARE(read.size(), 1);
    QVERIFY(!read.last().at(1).toBool());
    fixture.releaseHeldNotificationReply();
    QTRY_COMPARE(finished.size(), 1);
    QCOMPARE(read.size(), 1);

    fixture.holdNotificationReply = QStringLiteral("im.notify.read.all");
    backend.markAllNotificationsRead();
    QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());
    fixture.pushIm(QStringLiteral("newer-delete"), QStringLiteral("notifyDelete"), {{"id", 77}});
    QTRY_COMPARE(removed.size(), 1);
    fixture.pushIm(QStringLiteral("stale-unread"), QStringLiteral("notifyUnread"), {{"list", QJsonArray{77}}});
    fixture.releaseHeldNotificationReply();
    QTRY_COMPARE(finished.size(), 2);
    QCOMPARE(read.size(), 1);

    QSignalSpy history(&backend, &Backend::notificationsLoaded);
    backend.requestNotifications();
    QTRY_COMPARE(history.size(), 1);
    const auto rows = history.last().first().value<QVector<Notification>>();
    QCOMPARE(rows.first().id, QStringLiteral("5"));
    fixture.holdNotificationReply = QStringLiteral("im.notify.delete");
    backend.deleteNotification(rows.first().id);
    QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());
    fixture.pushIm(QStringLiteral("before-delete-ack"), QStringLiteral("notifyRead"), {{"list", QJsonArray{5}}});
    QTRY_COMPARE(read.size(), 2);
    fixture.releaseHeldNotificationReply();
    QTRY_COMPARE(finished.size(), 3);
    QCOMPARE(removed.size(), 2);
    QCOMPARE(removed.last().first().toString(), rows.first().id);
}

void TestCloudBackend::notificationSettingSnapshotsKeepNewerPush()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    fixture.serverNotificationScheme = QStringLiteral("simple");
    CloudBackend backend(fixture.session(), true);
    QSignalSpy online(&backend, &Backend::connectionChanged);
    QSignalSpy settings(&backend, &Backend::notificationSettingsLoaded);
    QSignalSpy errors(&backend, &Backend::failed);
    backend.start();
    QTRY_VERIFY(!online.isEmpty() && online.last().first().toBool());
    const auto list = QStringLiteral("im.v2.Settings.General.list");
    fixture.holdNotificationReply = list;
    backend.requestNotificationSettings();
    QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());
    fixture.pushIm(QStringLiteral("newer-settings"), QStringLiteral("settingsUpdate"),
                   {{"soundMuted", true}, {"notifyScheme", "expert"}});
    // A following websocket notification proves the settings Push was handled before the HTTP reply.
    QSignalSpy barrier(&backend, &Backend::notificationReadStateChanged);
    fixture.pushIm(QStringLiteral("settings-barrier"), QStringLiteral("notifyUnread"), {{"list", QJsonArray{101}}});
    QTRY_COMPARE(barrier.size(), 1);
    fixture.releaseHeldNotificationReply();
    QTRY_COMPARE(settings.size(), 1);
    auto latest = [&settings] { return settings.last().first().toMap(); };
    QVERIFY(latest().value(QStringLiteral("soundMuted")).toBool());
    QVERIFY(!latest().contains(QStringLiteral("notifySchemeSendSite")));
    QVERIFY(settings.last().at(1).toString().isEmpty());

    const auto volume = QStringLiteral("soundVolumeGeneral");
    fixture.holdNotificationReply = list;
    backend.updateNotificationSetting(volume, 30);
    QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());
    fixture.pushIm(QStringLiteral("newer-volume"), QStringLiteral("settingsUpdate"), {{"soundVolumeGeneral", 75}});
    fixture.pushIm(QStringLiteral("volume-barrier"), QStringLiteral("notifyUnread"), {{"list", QJsonArray{102}}});
    QTRY_COMPARE(barrier.size(), 2);
    fixture.releaseHeldNotificationReply();
    QTRY_COMPARE(settings.size(), 2);
    QCOMPARE(latest().value(volume).toInt(), 75);
    QCOMPARE(settings.last().at(1).toString(), volume);
    QCOMPARE(errors.size(), 1);
}

void TestCloudBackend::distinguishesPersonalDialogAndTaskWithSameId()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy details(&backend, &Backend::taskLoaded);
    QSignalSpy sent(&backend, &Backend::messageSent);
    QSignalSpy errors(&backend, &Backend::failed);
    backend.start();

    const auto sharedId = QStringLiteral("42");
    backend.requestTask(sharedId);
    QTRY_COMPARE(details.size(), 1);
    backend.sendChatMessage(sharedId, QStringLiteral("Личный чат"));
    backend.sendTaskMessage(sharedId, QStringLiteral("Чат задачи"));
    backend.sendChatMessage(sharedId, QStringLiteral("Повтор до подтверждения"));
    QTRY_COMPARE(sent.size(), 2);

    QSet<QString> acknowledgements;
    for (const auto &result : sent)
        acknowledgements.insert(result.first().toString());
    QVERIFY(acknowledgements.contains(QStringLiteral("chat:42")));
    QVERIFY(acknowledgements.contains(QStringLiteral("task:42")));

    const auto sends = fixture.requestsFor(QStringLiteral("im.v2.Chat.Message.send"));
    QCOMPARE(sends.size(), 2);
    QSet<QString> destinations;
    for (const auto &request : sends)
        destinations.insert(request.field(QStringLiteral("dialogId")));
    QVERIFY(destinations.contains(sharedId));
    QVERIFY(destinations.contains(QStringLiteral("chat73")));

    backend.sendChatMessage(sharedId, QStringLiteral(" "));
    backend.sendTaskMessage(sharedId, QStringLiteral(" "));
    QCOMPARE(errors.size(), 2);
    QCOMPARE(errors.first().first().toString(), QStringLiteral("send:chat:42"));
    QCOMPARE(errors.last().first().toString(), QStringLiteral("send:task:42"));
}

void TestCloudBackend::refreshesOnlyActiveViewsAndKeepsInflightInvalidations()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    fixture.holdNextChatResponse = true;
    CloudBackend backend(fixture.session(), true);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    QSignalSpy recent(&backend, &Backend::recentChatsLoaded);
    QSignalSpy online(&backend, &Backend::connectionChanged);
    backend.start();
    QTRY_COMPARE(online.size(), 1);

    const auto chatLoad = QStringLiteral("im.v2.Chat.load");
    backend.requestChatMessages(QStringLiteral("chat91"));
    QTRY_COMPARE(fixture.requestsFor(chatLoad).size(), 1);

    fixture.pushMessage(QStringLiteral("unrelated"), QStringLiteral("901"), 92);
    QTRY_COMPARE(recent.size(), 1);
    QCOMPARE(history.size(), 0);
    QCOMPARE(fixture.requestsFor(chatLoad).size(), 1);

    fixture.pushMessage(QStringLiteral("related"), QStringLiteral("902"));
    QTRY_COMPARE(recent.size(), 2);
    QCOMPARE(fixture.requestsFor(chatLoad).size(), 1);
    fixture.releaseHeldChatResponse();
    QTRY_COMPARE(history.size(), 2);
    QCOMPARE(fixture.requestsFor(chatLoad).size(), 2);

    backend.setActiveSection(2);
    fixture.pushMessage(QStringLiteral("inactive"), QStringLiteral("903"));
    QTest::qWait(300);
    QCOMPARE(fixture.requestsFor(chatLoad).size(), 2);
    const auto recentBeforeSwitch = recent.size();

    backend.setActiveSection(0);
    QTRY_COMPARE(fixture.requestsFor(chatLoad).size(), 3);
    QTRY_COMPARE(recent.size(), recentBeforeSwitch + 1);
}

void TestCloudBackend::receivesInBackgroundAndResumesAfterDisconnect()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy connected(&backend, &Backend::connectionChanged);
    QSignalSpy notices(&backend, &CloudBackend::notificationReceived);
    QSignalSpy errors(&backend, &Backend::failed);
    backend.setForeground(false);
    backend.start();
    QTRY_COMPARE(connected.size(), 1);
    QVERIFY(connected.first().first().toBool());

    const auto configurations = fixture.requestsFor(pushPath);
    QCOMPARE(configurations.size(), 1);
    QCOMPARE(configurations.first().field(QStringLiteral("CACHE")), QStringLiteral("N"));
    QCOMPARE(configurations.first().field(QStringLiteral("sessid")).toLatin1(), fixtureCsrf);
    QCOMPARE(fixture.socketRequests.size(), 1);
    const auto &handshake = fixture.socketRequests.first();
    const QUrlQuery first(handshake.url());
    QCOMPARE(first.queryItemValue(QStringLiteral("CHANNEL_ID")), QStringLiteral("synthetic-private/synthetic-shared"));
    QCOMPARE(first.queryItemValue(QStringLiteral("format")), QStringLiteral("json"));
    QVERIFY(!first.hasQueryItem(QStringLiteral("sessid")));
    QVERIFY(!handshake.hasRawHeader("Cookie"));
    QVERIFY(!handshake.hasRawHeader("X-Bitrix-Csrf-Token"));

    fixture.pushIm(QStringLiteral("old-chat-setting"), QStringLiteral("chatMuteNotify"), {{"dialogId", "chat92"}, {"muted", false}});
    fixture.pushMessage(QStringLiteral("first-event"), QStringLiteral("901"));
    QTRY_COMPARE(notices.size(), 1);
    const auto received = notices.first().first().value<Notification>();
    QCOMPARE(received.id, QStringLiteral("message:901"));
    QCOMPARE(received.text, QStringLiteral("Новое тестовое сообщение"));

    fixture.closePull();
    QTRY_COMPARE_WITH_TIMEOUT(connected.size(), 3, 5000);
    QVERIFY(!connected.at(1).first().toBool());
    QVERIFY(connected.at(2).first().toBool());
    QCOMPARE(fixture.socketRequests.size(), 2);
    const QUrlQuery resumed(fixture.socketRequests.last().url());
    QCOMPARE(resumed.queryItemValue(QStringLiteral("mid")), QStringLiteral("first-event"));

    fixture.pushMessage(QStringLiteral("first-event"), QStringLiteral("999"));
    fixture.pushIm(QStringLiteral("muted-on-reconnect"), QStringLiteral("messageChat"),
                   {{"chatId", 92}, {"dialogId", "chat92"}, {"notify", true},
                    {"chat", QJsonObject{{"92", QJsonObject{{"id", 92}, {"isMuted", true}}}}},
                    {"message", QJsonObject{{"id", 998}, {"authorId", 22}, {"text", "Muted on reconnect"}}}});
    fixture.pushMessage(QStringLiteral("second-event"), QStringLiteral("902"));
    QTRY_COMPARE(notices.size(), 2);
    QCOMPARE(notices.last().first().value<Notification>().id, QStringLiteral("message:902"));
    QVERIFY(fixture.requestsFor(QStringLiteral("im.v2.Recent.load")).isEmpty());
    QCOMPARE(errors.size(), 0);
}

void TestCloudBackend::learnsRotatedCredentialsFromAcceptedPortalReplies()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    fixture.chatResponseHeaders = "Set-Cookie: BITRIX_TEST=rotated; Path=/; HttpOnly\r\n"
        "Set-Cookie: scoped=must-not-escape; Path=/bitrix\r\n"
        "Set-Cookie: other=must-not-escape; Domain=unrelated.invalid; Path=/\r\n"
        "X-Bitrix-Csrf-Token: bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\r\n";

    CloudBackend backend(fixture.session(), true);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    backend.start();
    backend.requestChatMessages(QStringLiteral("chat91"));
    QTRY_COMPARE(history.size(), 1);

    fixture.chatResponseHeaders.clear();
    backend.requestChatMessages(QStringLiteral("chat91"));
    QTRY_COMPARE(history.size(), 2);

    const auto requests = fixture.requestsFor(QStringLiteral("im.v2.Chat.load"));
    QCOMPARE(requests.size(), 2);
    QCOMPARE(requests.first().headers.value("cookie"), fixtureCookie);
    QCOMPARE(requests.last().headers.value("cookie"), QByteArray("BITRIX_TEST=rotated"));
    QCOMPARE(requests.last().headers.value("x-bitrix-csrf-token"), QByteArray(32, 'b'));
    QCOMPARE(requests.last().field(QStringLiteral("sessid")), QString(32, QLatin1Char('b')));
}

void TestCloudBackend::recognizesExplicitAuthorizationRejection()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy online(&backend, &Backend::connectionChanged);
    QSignalSpy errors(&backend, &Backend::failed);
    backend.start();
    QTRY_COMPARE(online.size(), 1);
    fixture.chatResponseOverride = R"({"status":"error","data":null,"errors":[{"code":"invalid_authentication"}]})";
    fixture.denyCanonicalSession = true;
    backend.requestChatMessages(QStringLiteral("chat91"));
    QTRY_COMPARE(errors.size(), 1);
    QTRY_COMPARE(fixture.requestsFor(QStringLiteral("im.v2.Recent.load")).size(), 1);
    QTRY_COMPARE(online.size(), 2);
    QVERIFY(!online.last().first().toBool());
}

void TestCloudBackend::scopesAndCoalescesEndpointAuthorizationRejections()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    QSignalSpy errors(&backend, &Backend::failed);
    QSignalSpy rejected(&backend, &CloudBackend::sessionRejected);
    backend.start();
    fixture.holdNotificationReply = QStringLiteral("im.v2.Recent.load");
    fixture.chatResponseOverride = R"({"status":"error","data":null,"errors":[{"code":"invalid_authentication"}]})";
    backend.requestChatMessages(QStringLiteral("chat91"));
    QTRY_COMPARE(errors.size(), 1);
    QTRY_COMPARE(fixture.requestsFor(QStringLiteral("im.v2.Recent.load")).size(), 1);
    QCOMPARE(errors.first().first().toString(), QStringLiteral("chat:chat91"));
    QVERIFY(fixture.requestsFor(QStringLiteral("im.v2.Chat.Message.list")).isEmpty());

    backend.requestChatMessages(QStringLiteral("chat91"));
    QTRY_COMPARE(errors.size(), 2);
    QCOMPARE(fixture.requestsFor(QStringLiteral("im.v2.Recent.load")).size(), 1);
    QCOMPARE(rejected.size(), 0);

    fixture.chatResponseOverride.clear();
    fixture.releaseHeldNotificationReply();
    backend.requestChatMessages(QStringLiteral("chat91"));
    QTRY_COMPARE(history.size(), 1);
    QCOMPARE(rejected.size(), 0);
}

void TestCloudBackend::refreshesCsrfWithoutRepeatingAnUnconfirmedWrite_data()
{
    QTest::addColumn<QByteArray>("challengeCookie");
    QTest::newRow("token-only") << QByteArray();
    QTest::newRow("token-and-cookie") << QByteArray("BITRIX_TEST=rotated");
}

void TestCloudBackend::refreshesCsrfWithoutRepeatingAnUnconfirmedWrite()
{
    QFETCH(QByteArray, challengeCookie);
    CloudFixture fixture;
    QVERIFY(fixture.start());
    fixture.offeredCsrf = fixture.acceptedCsrf = QByteArray(32, 'b');
    fixture.challengeCookie = challengeCookie;
    const auto expectedCookie = challengeCookie.isEmpty() ? fixture.session().cookie : challengeCookie;
    const auto probeAction = QStringLiteral("im.v2.Recent.load");
    const auto sendAction = QStringLiteral("im.v2.Chat.Message.send");
    CloudBackend backend(fixture.session(), true);
    backend.setForeground(false);
    QSignalSpy errors(&backend, &Backend::failed);
    QSignalSpy rejected(&backend, &CloudBackend::sessionRejected);
    QSignalSpy sent(&backend, &Backend::messageSent);
    Session updated;
    connect(&backend, &CloudBackend::sessionUpdated, this, [&](const Session &session) { updated = session; });
    backend.start();

    // Deliver a reply generated using the old token after the newer token has been accepted.
    fixture.holdNextChatResponse = true;
    fixture.chatResponseOverride = invalidCsrf(QByteArray(32, 'c'));
    backend.requestChatMessages(QStringLiteral("chat91"));
    QTRY_COMPARE(fixture.requestsFor(QStringLiteral("im.v2.Chat.load")).size(), 1);
    fixture.chatResponseOverride.clear();
    backend.sendChatMessage(QStringLiteral("chat91"), QStringLiteral("Проверка отправки"));
    QTRY_COMPARE(updated.csrf, fixture.acceptedCsrf);
    QCOMPARE(updated.cookie, expectedCookie);
    QCOMPARE(fixture.requestsFor(probeAction).size(), 1);
    QCOMPARE(fixture.requestsFor(probeAction).first().field(QStringLiteral("sessid")),
             QString::fromLatin1(fixture.acceptedCsrf));
    QCOMPARE(fixture.requestsFor(probeAction).first().headers.value("cookie"), expectedCookie);
    QCOMPARE(fixture.requestsFor(sendAction).size(), 1);
    QCOMPARE(sent.size(), 0);
    fixture.releaseHeldChatResponse();
    QTRY_COMPARE(errors.size(), 2);
    QCOMPARE(rejected.size(), 0);
    QCOMPARE(fixture.requestsFor(probeAction).size(), 1);

    backend.sendChatMessage(QStringLiteral("chat91"), QStringLiteral("Явная повторная отправка"));
    QTRY_COMPARE(sent.size(), 1);
    QCOMPARE(fixture.requestsFor(sendAction).size(), 2);
    QCOMPARE(fixture.requestsFor(sendAction).last().headers.value("x-bitrix-csrf-token"), fixture.acceptedCsrf);
    QCOMPARE(fixture.requestsFor(sendAction).last().headers.value("cookie"), expectedCookie);
}

void TestCloudBackend::limitsCsrfRefreshAttempts_data()
{
    QTest::addColumn<QByteArray>("offered");
    QTest::addColumn<int>("probes");
    QTest::addColumn<bool>("canonicalRenews");
    QTest::newRow("malformed") << QByteArray("invalid") << 1 << false;
    QTest::newRow("same-token") << QByteArray(32, 'a') << 2 << true;
    QTest::newRow("second-challenge") << QByteArray(32, 'b') << 1 << false;
}

void TestCloudBackend::limitsCsrfRefreshAttempts()
{
    QFETCH(QByteArray, offered);
    QFETCH(int, probes);
    QFETCH(bool, canonicalRenews);
    CloudFixture fixture;
    QVERIFY(fixture.start());
    fixture.offeredCsrf = offered;
    fixture.nextCsrf = fixture.acceptedCsrf = QByteArray(32, 'c');
    auto session = fixture.session();
    session.csrf = QByteArray(32, 'a');
    CloudBackend backend(session, true);
    backend.setForeground(false);
    QSignalSpy rejected(&backend, &CloudBackend::sessionRejected);
    QByteArray renewedCsrf;
    connect(&backend, &CloudBackend::sessionUpdated, this, [&](const Session &verified) { renewedCsrf = verified.csrf; });
    backend.start();
    backend.sendChatMessage(QStringLiteral("chat91"), QStringLiteral("Одна попытка"));
    if (canonicalRenews) {
        QTRY_COMPARE(renewedCsrf, fixture.acceptedCsrf);
        QCOMPARE(rejected.size(), 0);
    } else {
        QTRY_COMPARE(rejected.size(), 1);
        QVERIFY(renewedCsrf.isEmpty());
    }
    const auto canonical = fixture.requestsFor(QStringLiteral("im.v2.Recent.load"));
    QCOMPARE(canonical.size(), probes);
    if (canonicalRenews) {
        QCOMPARE(canonical.first().headers.value("x-bitrix-csrf-token"), session.csrf);
        QCOMPARE(canonical.last().headers.value("x-bitrix-csrf-token"), renewedCsrf);
    }
    QCOMPARE(fixture.requestsFor(QStringLiteral("im.v2.Chat.Message.send")).size(), 1);
}

void TestCloudBackend::refreshesSavedCsrfBeforeOpeningWorkspace_data()
{
    refreshesCsrfWithoutRepeatingAnUnconfirmedWrite_data();
}

void TestCloudBackend::refreshesSavedCsrfBeforeOpeningWorkspace()
{
    QFETCH(QByteArray, challengeCookie);
    CloudFixture fixture;
    QVERIFY(fixture.start());
    fixture.offeredCsrf = fixture.acceptedCsrf = QByteArray(32, 'b');
    fixture.challengeCookie = challengeCookie;
    namespace signin = bx::signin;
    signin::SessionRestore restore;
    auto status = signin::RestoreStatus::Unavailable;
    Session updated;
    int replies = 0;
    connect(&restore, &signin::SessionRestore::verified, this,
            [&](signin::RestoreStatus result, const Session &session) {
        status = result;
        updated = session;
        ++replies;
    });
    auto saved = fixture.session();
    saved.csrf = QByteArray(32, 'a');
    restore.verify(saved, true);
    QTRY_COMPARE(replies, 1);
    QCOMPARE(status, signin::RestoreStatus::Authenticated);
    QCOMPARE(updated.csrf, fixture.acceptedCsrf);
    QCOMPARE(updated.cookie, challengeCookie.isEmpty() ? saved.cookie : challengeCookie);
    const auto requests = fixture.requestsFor(QStringLiteral("im.v2.Recent.load"));
    QCOMPARE(requests.size(), 2);
    QCOMPARE(requests.first().field(QStringLiteral("sessid")), QString::fromLatin1(saved.csrf));
    QCOMPARE(requests.first().headers.value("cookie"), saved.cookie);
    QCOMPARE(requests.last().field(QStringLiteral("sessid")), QString::fromLatin1(updated.csrf));
    QCOMPARE(requests.last().headers.value("cookie"), updated.cookie);

    fixture.failReplacementProbe = true;
    restore.verify(saved, true);
    QTRY_COMPARE(replies, 2);
    QCOMPARE(status, signin::RestoreStatus::Unavailable);
    QVERIFY(!updated.isValid());
}

void TestCloudBackend::recoversOnlyTheSameAccount()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy chats(&backend, &Backend::recentChatsLoaded);
    QSignalSpy struck(&backend, &CloudBackend::sessionRejected);
    QSignalSpy recovered(&backend, &CloudBackend::sessionRecovered);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    QSignalSpy errors(&backend, &Backend::failed);
    backend.start();
    backend.requestRecentChats();
    QTRY_VERIFY(!chats.isEmpty());

    fixture.chatResponseOverride = R"({"status":"error","data":null,"errors":[{"code":"invalid_csrf"}]})";
    fixture.denyCanonicalSession = true;
    backend.requestChatMessages(QStringLiteral("chat91"));
    QTRY_COMPARE(struck.size(), 1);
    fixture.chatResponseOverride.clear();
    fixture.denyCanonicalSession = false;

    auto fresh = fixture.session();
    fresh.cookie = "BITRIX_TEST=fresh";
    fixture.currentUserId = 22;
    errors.clear();
    backend.recoverSession(fresh);
    QTRY_VERIFY(!errors.isEmpty());
    QCOMPARE(errors.first().first().toString(), QStringLiteral("session-recovery"));
    QCOMPARE(recovered.size(), 0);
    QCOMPARE(history.size(), 0);

    fixture.currentUserId = 11;
    int browserResultChecks = 0;
    backend.recoverSession(fresh, [&] {
        ++browserResultChecks;
        return false;
    });
    QTRY_COMPARE(browserResultChecks, 1);
    QCOMPARE(recovered.size(), 0);
    QCOMPARE(history.size(), 0);

    const auto identity = QStringLiteral("main.bitrix.main.controller.loadext.resolveextensions");
    const auto previousRequests = fixture.requestsFor(identity).size();
    fixture.holdNextIdentityResponse = true;
    backend.recoverSession(fresh, [] { return true; });
    QTRY_COMPARE(fixture.requestsFor(identity).size(), previousRequests + 1);
    backend.cancelSessionRecovery();
    backend.recoverSession(fresh);
    QTRY_COMPARE(recovered.size(), 1);
    QCOMPARE(fixture.requestsFor(identity).size(), previousRequests + 2);
    QTRY_VERIFY(!history.isEmpty());
    const auto requests = fixture.requestsFor(QStringLiteral("im.v2.Chat.load"));
    QCOMPARE(requests.last().headers.value("cookie"), fresh.cookie);
    QCOMPARE(struck.size(), 1);
}

void TestCloudBackend::catchesUpWhenAnActiveChatStopsReceivingPush()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy online(&backend, &Backend::connectionChanged);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    backend.start();
    QTRY_COMPARE(online.size(), 1);
    auto *timer = backend.findChild<QTimer *>(QStringLiteral("active-history-catchup"));
    QVERIFY(timer);
    QVERIFY(!timer->isActive());

    backend.requestChatMessages(QStringLiteral("chat91"));
    QTRY_COMPARE(history.size(), 1);
    QVERIFY(timer->isActive());
    QVERIFY(QMetaObject::invokeMethod(timer, "timeout"));
    QTRY_COMPARE(history.size(), 2);
    QCOMPARE(online.size(), 1);

    backend.setForeground(false);
    QVERIFY(!timer->isActive());
    backend.setForeground(true);
    QTRY_COMPARE(history.size(), 3);
    QVERIFY(timer->isActive());
    backend.setActiveSection(2);
    QVERIFY(!timer->isActive());
}

void TestCloudBackend::refreshesVisibleInactiveHistory_data()
{
    QTest::addColumn<bool>("task");
    QTest::newRow("ordinary conversation") << false;
    QTest::newRow("task conversation over another section") << true;
}

void TestCloudBackend::refreshesVisibleInactiveHistory()
{
    QFETCH(bool, task);
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy online(&backend, &Backend::connectionChanged);
    QSignalSpy history(&backend, task ? &Backend::taskMessagesLoaded : &Backend::chatMessagesLoaded);
    QSignalSpy recent(&backend, &Backend::recentChatsLoaded);
    backend.setForeground(false);
    backend.setWorkspaceVisible(true);
    if (task)
        backend.setActiveSection(1);
    backend.start();
    QTRY_COMPARE(online.size(), 1);
    if (task) {
        backend.requestTaskMessages(QStringLiteral("42"));
    } else {
        backend.requestRecentChats();
        QTRY_VERIFY(!recent.isEmpty());
        backend.requestChatMessages(QStringLiteral("chat91"));
    }
    QTRY_COMPARE(history.size(), 1);
    const auto recentReads = fixture.requestsFor(QStringLiteral("im.v2.Recent.load")).size();
    const auto push = [&](const QString &suffix, const QString &id) {
        fixture.pushMessage(QStringLiteral("workspace-visible-") + suffix, id, task ? 73 : 91);
    };
    push(QStringLiteral("one"), QStringLiteral("901"));
    push(QStringLiteral("two"), QStringLiteral("902"));
    push(QStringLiteral("three"), QStringLiteral("903"));
    QTRY_COMPARE_WITH_TIMEOUT(history.size(), 2, 1500);
    QTest::qWait(350);
    QCOMPARE(history.size(), 2);
    QCOMPARE(fixture.requestsFor(QStringLiteral("im.v2.Recent.load")).size(), recentReads + (task ? 0 : 1));

    auto *timer = backend.findChild<QTimer *>(QStringLiteral("active-history-catchup"));
    QVERIFY(timer && timer->isActive());
    QCOMPARE(timer->interval(), 15000);
    QVERIFY(QMetaObject::invokeMethod(timer, "timeout"));
    QTRY_COMPARE(history.size(), 3);

    backend.setWorkspaceVisible(false);
    QVERIFY(!timer->isActive());
    push(QStringLiteral("hidden"), QStringLiteral("904"));
    QTest::qWait(400);
    QCOMPARE(history.size(), 3);
    backend.setWorkspaceVisible(true);
    QTRY_COMPARE(history.size(), 4);
    QVERIFY(timer->isActive());
    backend.clearOpenHistory(task);
    QVERIFY(!timer->isActive());
    push(QStringLiteral("closed"), QStringLiteral("905"));
    QTest::qWait(400);
    QCOMPARE(history.size(), 4);
    QVERIFY(fixture.requestsFor(QStringLiteral("im.v2.Chat.Message.read")).isEmpty());
    QVERIFY(fixture.requestsFor(QStringLiteral("applyFilter")).isEmpty());
}

void TestCloudBackend::desktopBannersCatchUpFromHistory_data()
{
    QTest::addColumn<bool>("task");
    QTest::newRow("ordinary") << false;
    QTest::newRow("task") << true;
}

void TestCloudBackend::desktopBannersCatchUpFromHistory()
{
    QFETCH(bool, task);
    CloudFixture fixture;
    fixture.portalStatusFixture = true;
    fixture.serverNotificationScheme = QStringLiteral("simple");
    QVERIFY(fixture.start());
    const int chatId = task ? 73 : 91;
    const auto dialog = QStringLiteral("chat%1").arg(chatId);
    const auto row = [chatId](int id, int author = 22, bool unread = true, int age = 0, bool silent = false) {
        return QJsonObject{{"id", id}, {"chatId", chatId}, {"authorId", author}, {"text", "New message"},
            {"unread", unread}, {"date", QDateTime::currentDateTimeUtc().addSecs(-age).toString(Qt::ISODate)},
            {"params", silent ? QJsonObject{{"NOTIFY", "N"}} : QJsonObject{}}};
    };
    QJsonArray rows{row(900)};
    const auto publish = [&] {
        fixture.chatResponseOverride = ajaxResult(QJsonDocument(QJsonObject{
            {"chat", QJsonObject{{"id", chatId}, {"dialogId", dialog}}}, {"messages", rows}}).toJson(QJsonDocument::Compact));
    };
    publish();
    CloudBackend backend(fixture.session(), true);
    QSignalSpy connection(&backend, &Backend::connectionChanged);
    QSignalSpy history(&backend, task ? &Backend::taskMessagesLoaded : &Backend::chatMessagesLoaded);
    QSignalSpy failures(&backend, &Backend::failed);
    QSignalSpy banners(&backend, &CloudBackend::notificationReceived);
    QSignalSpy remoteNotices(&backend, &Backend::notificationsLoaded);
    backend.setForeground(false);
    backend.setWorkspaceVisible(true);
    if (task)
        backend.setActiveSection(1);
    backend.start();
    QTRY_VERIFY(!connection.isEmpty());
    const auto requestHistory = [&] {
        if (task)
            backend.requestTaskMessages(QStringLiteral("42"));
        else
            backend.requestChatMessages(dialog);
    };
    const auto load = [&] {
        const auto before = history.size();
        requestHistory();
        QTRY_VERIFY(history.size() > before);
    };
    load();
    QCOMPARE(banners.size(), 0);

    auto foreignRow = row(999);
    foreignRow.insert(QStringLiteral("chatId"), chatId + 1);
    const auto historyOperation = task ? QStringLiteral("task-messages:42") : QStringLiteral("chat:") + dialog;
    const auto historyErrors = [&] {
        return std::count_if(failures.cbegin(), failures.cend(), [&](const auto &result) {
            return result.first().toString() == historyOperation;
        });
    };
    fixture.chatResponseOverride = ajaxResult(QJsonDocument(QJsonObject{
        {"chat", QJsonObject{{"id", chatId + 1}}}, {"messages", QJsonArray{foreignRow}}}).toJson(QJsonDocument::Compact));
    load();
    QCOMPARE(historyErrors(), 0);
    QCOMPARE(history.size(), 2);
    const auto listAction = QStringLiteral("im.v2.Chat.Message.list");
    const auto listCount = fixture.requestsFor(listAction).size();
    fixture.chatResponseOverride = ajaxResult(QJsonDocument(QJsonObject{{"chatId", chatId + 1}}).toJson(QJsonDocument::Compact));
    requestHistory();
    QTRY_COMPARE(historyErrors(), 1);
    QCOMPARE(fixture.requestsFor(listAction).size(), listCount);
    QCOMPARE(history.size(), 2);
    QCOMPARE(banners.size(), 0);

    fixture.holdNotificationReply = QStringLiteral("im.v2.Settings.General.list");
    rows.append(row(901));
    publish();
    load();
    QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());
    QCOMPARE(banners.size(), 0);
    rows.removeLast();
    rows.append(row(901, 22, false));
    rows.append(row(902));
    rows.append(foreignRow);
    publish();
    load();
    QCOMPARE(banners.size(), 0);
    fixture.releaseHeldNotificationReply();
    QTRY_COMPARE_WITH_TIMEOUT(banners.size(), 1, 650);
    const auto delivered = banners.first().first().value<Notification>();
    QCOMPARE(delivered.id, QStringLiteral("message:902"));
    QVERIFY(!delivered.targets.isEmpty());
    QCOMPARE(delivered.targets.first().id, dialog);
    QCOMPARE(delivered.targets.first().taskChat, task);
    rows.removeLast();
    fixture.pushMessage(QStringLiteral("history-before-push"), QStringLiteral("902"), chatId);
    QTest::qWait(350);
    QCOMPARE(banners.size(), 1);

    fixture.pushMessage(QStringLiteral("push-before-history"), QStringLiteral("903"), chatId);
    QTRY_COMPARE(banners.size(), 2);
    rows.append(row(903));
    rows.append(row(904, 22, false));
    rows.append(row(905, 11));
    rows.append(row(906, 0));
    rows.append(row(907, 22, true, 60));
    rows.append(row(908, 22, true, 0, true));
    publish();
    load();
    QCOMPARE(banners.size(), 2);

    const auto pushWithFlags = [&](int id, const QJsonObject &params, const QJsonValue &notify = {}) {
        QJsonObject event{{"chatId", chatId}, {"dialogId", dialog},
            {"message", QJsonObject{{"id", id}, {"authorId", 22}, {"text", "New message"}, {"params", params}}}};
        if (notify.isBool())
            event.insert(QStringLiteral("notify"), notify);
        fixture.pushIm(QStringLiteral("history-notify-%1").arg(id), QStringLiteral("messageChat"), event);
    };
    const auto beforeNotices = remoteNotices.size();
    pushWithFlags(909, {}); // Unclear Push metadata must neither notify directly nor veto a later verified history row.
    pushWithFlags(910, {}, false);
    pushWithFlags(911, {{"NOTIFY", "N"}}, true);
    QTRY_VERIFY(remoteNotices.size() >= beforeNotices + 3);
    QCOMPARE(banners.size(), 2);
    for (int id = 909; id <= 911; ++id)
        rows.append(row(id));
    publish();
    load();
    QTRY_COMPARE(banners.size(), 3);
    QCOMPARE(banners.last().first().value<Notification>().id, QStringLiteral("message:909"));

    fixture.pushIm(QStringLiteral("history-chat-muted"), QStringLiteral("chatMuteNotify"),
                   {{"dialogId", dialog}, {"muted", true}});
    QTest::qWait(30);
    rows.append(row(912));
    publish();
    load();
    QCOMPARE(banners.size(), 3);

    fixture.pushIm(QStringLiteral("history-chat-unmuted"), QStringLiteral("chatMuteNotify"),
                   {{"dialogId", dialog}, {"muted", false}});
    backend.setForeground(true);
    rows.append(row(913));
    publish();
    load();
    const auto foregroundHistoryIsCurrent = [&] {
        const auto messages = history.last().at(1).value<QVector<Message>>();
        return std::any_of(messages.cbegin(), messages.cend(), [](const Message &message) {
            return message.id == QLatin1String("913");
        });
    };
    QTRY_VERIFY(foregroundHistoryIsCurrent());
    backend.setForeground(false);
    load();
    QCOMPARE(banners.size(), 3);
    QVERIFY(fixture.requestsFor(QStringLiteral("im.v2.Chat.Message.read")).isEmpty());
}

void TestCloudBackend::refusesToFollowRedirectWithSession()
{
    CloudFixture original;
    CloudFixture otherOrigin;
    QVERIFY(original.start());
    QVERIFY(otherOrigin.start());
    original.redirectProjects = true;
    auto destination = otherOrigin.session().origin;
    destination.setPath(QStringLiteral("/redirect-destination"));
    original.redirectLocation = destination.toString();

    CloudBackend backend(original.session(), true);
    QSignalSpy projects(&backend, &Backend::projectsLoaded);
    QSignalSpy errors(&backend, &Backend::failed);
    backend.start();
    backend.requestProjects();
    QTRY_VERIFY(!errors.isEmpty());
    QCOMPARE(errors.first().first().toString(), QStringLiteral("projects"));
    QVERIFY(projects.isEmpty());
    QVERIFY(otherOrigin.requests.isEmpty());
}

QTEST_GUILESS_MAIN(TestCloudBackend)
#include "tst_cloud_backend.moc"
