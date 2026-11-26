#include "cloud_fixture.h"
#include "core/tunables.h"
#include "ui/reaction_catalog.h"

#include <QHostAddress>
#include <QJsonDocument>
#include <QSettings>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTest>
#include <QWebSocket>
#include <utility>

using namespace bx;

namespace cloud_test {

QByteArray invalidCsrf(const QByteArray &token)
{
    return R"({"status":"error","errors":[{"code":"invalid_csrf","customData":{"csrf":")"
        + token + R"("}}]})";
}

QByteArray ajaxResult(const QByteArray &data)
{
    return "{\"status\":\"success\",\"data\":" + data + ",\"errors\":[]}";
}

CloudFixture::CloudFixture()
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

bool CloudFixture::start()
{
    return http.listen(QHostAddress::LocalHost) && websocket.listen(QHostAddress::LocalHost);
}

Session CloudFixture::session() const
{
    return {QUrl(QStringLiteral("http://127.0.0.1:%1").arg(http.serverPort())), fixtureCookie, fixtureCsrf};
}

QList<Request> CloudFixture::requestsFor(const QString &key) const
{
    QList<Request> found;
    for (const auto &req : requests) {
        if (req.url.path() == key || req.action() == key)
            found.append(req);
    }
    return found;
}

void CloudFixture::pushIm(const QString &mid, const QString &command, const QJsonObject &params, int age,
                          const QString &module)
{
    const QJsonObject body{{QStringLiteral("module_id"), module},
                           {QStringLiteral("command"), command}, {QStringLiteral("params"), params},
                           {QStringLiteral("extra"), QJsonObject{{QStringLiteral("server_time_ago"), age},
                               {QStringLiteral("server_time_unix"), QDateTime::currentSecsSinceEpoch() - age}}}};
    const QJsonArray events{QJsonObject{{QStringLiteral("mid"), mid}, {QStringLiteral("text"), body}}};
    clients.last()->sendTextMessage(QString::fromUtf8(QJsonDocument(events).toJson(QJsonDocument::Compact)));
}

void CloudFixture::pushMessage(const QString &mid, const QString &id, int chatId, int author,
                               bool important, int age, bool messageMute)
{
    const QJsonObject message{{QStringLiteral("id"), id}, {QStringLiteral("authorId"), author},
                              {QStringLiteral("isImportant"), important},
                              {QStringLiteral("text"), QStringLiteral("Новое тестовое сообщение")},
                              {QStringLiteral("params"), QJsonObject{{QStringLiteral("NOTIFY"), messageMute ? "N" : "Y"}}}};
    pushIm(mid, QStringLiteral("messageChat"), {{QStringLiteral("chatId"), chatId},
               {QStringLiteral("dialogId"), QStringLiteral("chat%1").arg(chatId)},
               {QStringLiteral("notify"), true}, {QStringLiteral("message"), message}}, age);
}

void CloudFixture::closePull()
{
    clients.last()->close(QWebSocketProtocol::CloseCodeNormal);
}

void CloudFixture::releaseHeldOnlineResponse()
{
    if (!heldOnlineSocket)
        return;
    heldOnlineSocket->write(heldOnlineResponse);
    heldOnlineSocket->disconnectFromHost();
    heldOnlineSocket.clear();
    heldOnlineResponse.clear();
}

void CloudFixture::releaseHeldChatResponse()
{
    if (!heldChatSocket)
        return;
    heldChatSocket->write(heldChatResponse);
    heldChatSocket->disconnectFromHost();
    heldChatSocket.clear();
    heldChatResponse.clear();
}

void CloudFixture::releaseHeldReadResponse(const QByteArray &extraHeaders)
{
    if (!heldReadSocket)
        return;
    heldReadResponse.insert(heldReadResponse.indexOf("\r\n") + 2, extraHeaders);
    heldReadSocket->write(heldReadResponse);
    heldReadSocket->disconnectFromHost();
    heldReadSocket.clear();
    heldReadResponse.clear();
}

void CloudFixture::releaseHeldBoardResponse()
{
    if (!heldBoardSocket)
        return;
    heldBoardSocket->write(heldBoardResponse);
    heldBoardSocket->disconnectFromHost();
    heldBoardSocket.clear();
    heldBoardResponse.clear();
}

void CloudFixture::releaseHeldDownload()
{
    if (!heldDownloadSocket)
        return;
    heldDownloadSocket->write(heldDownloadResponse);
    heldDownloadSocket->disconnectFromHost();
    heldDownloadSocket.clear();
    heldDownloadResponse.clear();
}

void CloudFixture::releaseHeldIdentity()
{
    if (!heldIdentitySocket)
        return;
    heldIdentitySocket->write(heldIdentityResponse);
    heldIdentitySocket->disconnectFromHost();
    heldIdentitySocket.clear();
    heldIdentityResponse.clear();
}

void CloudFixture::releaseHeldNotificationReply(bool fail)
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

void CloudFixture::respond(QTcpSocket *socket, QByteArray &buffer)
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

QByteArray CloudFixture::reply(const Request &request) const
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
    if (action == QStringLiteral("tasks.v2.File.listObjects"))
        return ajaxResult(QJsonDocument(taskAttachmentRows).toJson(QJsonDocument::Compact));
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
        if (malformedTaskSearch)
            return ajaxResult("{}");
        if (rejectTaskSearch)
            return R"({"status":"error","errors":[{"code":"TASK_SEARCH_FAILED"}]})";
        const auto selections = request.form.queryItems(QUrl::FullyDecoded);
        if (selections.contains({QStringLiteral("select[]"), QStringLiteral("START_DATE_PLAN")}))
            return ajaxResult(R"({"tasks":[{"id":42,"title":"План","groupId":17,
                    "startDatePlan":"2026-08-01T09:00:00Z","endDatePlan":"2026-08-03T12:00:00Z"},
                    {"id":43,"title":"Без дат","groupId":17},{"id":99,"title":"Не этот проект","groupId":18}]})");
        if (request.field(QStringLiteral("filter[ID][0]")) == QStringLiteral("42"))
            return ajaxResult(QJsonDocument(QJsonObject{{"tasks", QJsonArray{
                QJsonObject{{"id", "42"}, {"title", taskChatTitle}, {"groupId", 17}}}}}).toJson(QJsonDocument::Compact));
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

void CloudTestCase::initTestCase()
{
    QVERIFY(m_settingsPath.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsPath.path());
}

void CloudAccountTestCase::discardsConcurrentChatAndNoticeRepliesOnAccountChange()
{
    CloudFixture fixture;
    fixture.ordinaryRecentBadgeFixture = R"({"userId":11,"recentItems":[],"chats":[]})";
    fixture.noticeFixture = {QJsonObject{{"id", 41}, {"text", "Old notice"}}};
    fixture.chatResponseOverride = ajaxResult(R"({"chatId":91,"messages":[{"id":710,"text":"Old message"}]})");
    QVERIFY(fixture.start());

    CloudBackend backend(fixture.session(), true);
    backend.setForeground(false);
    QSignalSpy accounts(&backend, &Backend::accountScopeChanged);
    QSignalSpy messages(&backend, &Backend::chatMessagesLoaded);
    QSignalSpy notices(&backend, &Backend::notificationsLoaded);
    backend.start();
    backend.requestRecentChats();
    QTRY_COMPARE(accounts.size(), 1);

    fixture.holdNextChatResponse = true;
    fixture.holdNotificationReply = QStringLiteral("im.notify.get");
    backend.requestChatMessages(QStringLiteral("chat91"));
    backend.requestNotifications();
    QTRY_VERIFY(!fixture.holdNextChatResponse && fixture.holdNotificationReply.isEmpty());

    fixture.currentUserId = 22;
    fixture.ordinaryRecentBadgeFixture = R"({"userId":22,"recentItems":[],"chats":[]})";
    backend.requestRecentChats();
    QTRY_COMPARE(accounts.size(), 2);
    messages.clear();
    notices.clear();
    fixture.releaseHeldChatResponse();
    fixture.releaseHeldNotificationReply();

    fixture.chatResponseOverride = ajaxResult(R"({"chatId":91,"messages":[{"id":711,"text":"New message"}]})");
    fixture.noticeFixture = {QJsonObject{{"id", 42}, {"text", "New notice"}}};
    backend.requestChatMessages(QStringLiteral("chat91"));
    backend.requestNotifications();
    QTRY_COMPARE(messages.size(), 1);
    QTRY_COMPARE(notices.size(), 1);
    QCOMPARE(messages.first().at(1).value<QVector<Message>>().value(0).id, QStringLiteral("711"));
    QCOMPARE(notices.first().first().value<QVector<Notification>>().value(0).id, QStringLiteral("42"));
}

} // namespace cloud_test
