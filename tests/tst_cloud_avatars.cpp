#include "cloud/cloud_backend.h"
#include "cloud/avatar_redirect.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QUrlQuery>
#include <algorithm>
#include <functional>

using namespace bx;

namespace {

struct Request {
    QByteArray method;
    QUrl url;
    QHash<QByteArray, QByteArray> headers;
};

bool isHeldImageRequest(const Request &request)
{
    return request.method == "GET" && request.url.path().startsWith(QLatin1String("/upload/resize_cache/"));
}

struct Response {
    QByteArray status = "200 OK";
    QByteArray type = "application/json";
    QByteArray body;
    QByteArray additionalHeaders;
};

class LocalServer {
public:
    bool listen()
    {
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (auto *socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                                 [this, socket, received = QByteArray()]() mutable {
                    received += socket->readAll();
                    const auto endOfHeaders = received.indexOf("\r\n\r\n");
                    if (endOfHeaders < 0)
                        return;

                    const auto lines = received.left(endOfHeaders).split('\n');
                    const auto firstLine = lines.first().trimmed().split(' ');
                    Request request{firstLine.value(0), QUrl::fromEncoded(firstLine.value(1)), {}};
                    for (const auto &line : lines.mid(1)) {
                        const auto colon = line.indexOf(':');
                        if (colon > 0)
                            request.headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
                    }
                    if (received.size() < endOfHeaders + 4 + request.headers.value("content-length").toInt())
                        return;

                    requests.append(request);
                    const auto reply = respond(request);
                    QObject::disconnect(socket, &QTcpSocket::readyRead, socket, nullptr);
                    if (holdImages && isHeldImageRequest(request)) {
                        QObject::connect(socket, &QTcpSocket::disconnected, &server,
                                         [this] { ++disconnectedImages; });
                        return;
                    }
                    socket->write("HTTP/1.1 " + reply.status + "\r\nConnection: close\r\nContent-Type: "
                                  + reply.type + "\r\nContent-Length: " + QByteArray::number(reply.body.size())
                                  + "\r\n" + reply.additionalHeaders + "\r\n" + reply.body);
                    socket->disconnectFromHost();
                });
            }
        });
        return server.listen(QHostAddress::LocalHost);
    }

    QUrl origin() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort())); }
    Session session() const { return {origin(), "B24=synthetic", "synthetic-csrf"}; }

    int attempts(const QString &path) const
    {
        return std::count_if(requests.cbegin(), requests.cend(), [&path](const auto &request) {
            return request.url.path() == path;
        });
    }

    QList<Request> requests;
    std::function<Response(const Request &)> respond;
    bool holdImages = false;
    int disconnectedImages = 0;
private:
    QTcpServer server;
};

Response recents(const QStringList &avatars)
{
    QJsonArray items;
    QJsonArray chats;
    for (qsizetype i = 0; i < avatars.size(); ++i) {
        const auto id = QStringLiteral("chat%1").arg(i + 1);
        items.append(QJsonObject{{QStringLiteral("dialogId"), id}, {QStringLiteral("chatId"), i + 1}});
        chats.append(QJsonObject{{QStringLiteral("dialogId"), id}, {QStringLiteral("name"), id},
                                 {QStringLiteral("avatar"), avatars.at(i)}});
    }
    const auto rows = QJsonDocument(QJsonObject{{QStringLiteral("recentItems"), items},
                                                {QStringLiteral("chats"), chats},
                                                {QStringLiteral("userId"), 11}}).toJson(QJsonDocument::Compact);
    return {"200 OK", "application/json", "{\"status\":\"success\",\"data\":" + rows + '}', {}};
}

const QByteArray png = QByteArray::fromBase64(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aRYAAAAAASUVORK5CYII=");
const QString validPath = QStringLiteral("/upload/resize_cache/avatar.png");
const QString projectPath = QStringLiteral("/b12345/resize_cache/project.png");

QVector<Chat> firstChats(const QSignalSpy &spy)
{
    return spy.first().first().value<QVector<Chat>>();
}

} // namespace

class TestCloudAvatars : public QObject {
    Q_OBJECT
private slots:
    void acceptsMetadataImageAndDeduplicates();
    void rejectsUnapprovedUrlsAndRedirects();
    void restrictsPortalAvatarRelayToTheSameAsset();
    void rejectsUnsafeMimeAndOversizedImage();
    void capsConcurrentRequestsAndClearsPendingOnStop();
};

void TestCloudAvatars::acceptsMetadataImageAndDeduplicates()
{
    LocalServer server;
    server.respond = [](const Request &request) -> Response {
        if (request.url.path() == validPath || request.url.path() == projectPath)
            return {"200 OK", "image/png", png, {}};
        const auto action = QUrlQuery(request.url).queryItemValue(QStringLiteral("action"));
        if (action == QLatin1String("im.v2.Recent.load"))
            return recents({validPath});
        if (action == QLatin1String("socialnetwork.api.workgroup.list")) {
            const auto rows = QJsonArray{QJsonObject{{QStringLiteral("id"), 10}, {QStringLiteral("name"), QStringLiteral("Project")},
                                                    {QStringLiteral("avatar"), projectPath}}};
            const auto result = QJsonDocument(QJsonObject{{QStringLiteral("status"), QStringLiteral("success")},
                                                          {QStringLiteral("data"), rows}}).toJson(QJsonDocument::Compact);
            return {"200 OK", "application/json", result, {}};
        }
        return {"200 OK", "application/json", "{\"result\":{}}", {}};
    };
    QVERIFY(server.listen());

    CloudBackend backend(server.session(), true);
    QSignalSpy chatSpy(&backend, &Backend::recentChatsLoaded);
    QSignalSpy projectSpy(&backend, &Backend::projectsLoaded);
    QSignalSpy avatarSpy(&backend, &Backend::avatarLoaded);
    backend.start();
    backend.requestRecentChats();
    QTRY_COMPARE(chatSpy.size(), 1);

    const auto avatar = firstChats(chatSpy).first().avatarUrl;
    QCOMPARE(avatar, server.origin().resolved(QUrl(validPath)));
    backend.requestProjects();
    QTRY_COMPARE(projectSpy.size(), 1);
    const auto projects = projectSpy.first().first().value<QVector<Project>>();
    QCOMPARE(projects.size(), 1);
    const auto projectAvatar = projects.first().avatarUrl;
    QCOMPARE(projectAvatar, server.origin().resolved(QUrl(projectPath)));

    backend.requestAvatar(avatar);
    backend.requestAvatar(avatar);
    QTRY_COMPARE(avatarSpy.size(), 1);
    QCOMPARE(avatarSpy.first().first().toUrl(), avatar);
    QCOMPARE(avatarSpy.first().last().toByteArray(), png);
    QCOMPARE(server.attempts(validPath), 1);
    const auto request = *std::find_if(server.requests.cbegin(), server.requests.cend(), [](const auto &value) {
        return value.url.path() == validPath;
    });
    QCOMPARE(request.method, QByteArray("GET"));
    QCOMPARE(request.headers.value("cookie"), QByteArray("B24=synthetic"));
    QVERIFY(!request.headers.contains("x-bitrix-csrf-token"));
    QVERIFY(!request.headers.contains("authorization"));
    QVERIFY(!request.headers.contains("referer"));

    backend.requestAvatar(avatar);
    QTRY_COMPARE(avatarSpy.size(), 2);
    QCOMPARE(avatarSpy.last().last().toByteArray(), png);
    QCOMPARE(server.attempts(validPath), 1);
    backend.requestAvatar(projectAvatar);
    QTRY_COMPARE(avatarSpy.size(), 3);
    QCOMPARE(avatarSpy.last().first().toUrl(), projectAvatar);
    QCOMPARE(avatarSpy.last().last().toByteArray(), png);
    QCOMPARE(server.attempts(projectPath), 1);

    backend.stop();
    backend.start();
    backend.requestAvatar(avatar);
    QTRY_COMPARE(avatarSpy.size(), 4);
    QVERIFY(avatarSpy.last().last().toByteArray().isEmpty());
    QCOMPARE(server.attempts(validPath), 1);
}

void TestCloudAvatars::restrictsPortalAvatarRelayToTheSameAsset()
{
    const QUrl portal(QStringLiteral("https://tenant.bitrix24.ru"));
    const QUrl source(QStringLiteral("https://tenant.bitrix24.ru/b123/resize_cache/avatar.jpg?v=2"));
    const QUrl target(QStringLiteral("https://cdn-ru.bitrix24.ru/b123/resize_cache/avatar.jpg?v=2"));
    QCOMPARE(cloud::avatarCdnRedirect(portal, source, 301, target), target);
    QVERIFY(cloud::avatarCdnRedirect(portal, source, 302, target).isEmpty());
    QVERIFY(cloud::avatarCdnRedirect(portal, target, 301, source).isEmpty());
    QVERIFY(cloud::avatarCdnRedirect(portal, source, 301, source).isEmpty());

    for (const auto &candidate : {
             QStringLiteral("http://cdn-ru.bitrix24.ru/b123/resize_cache/avatar.jpg?v=2"),
             QStringLiteral("https://cdn-ru.bitrix24.ru.invalid/b123/resize_cache/avatar.jpg?v=2"),
             QStringLiteral("https://cdn-ru.bitrix24.ru:444/b123/resize_cache/avatar.jpg?v=2"),
             QStringLiteral("https://profile@cdn-ru.bitrix24.ru/b123/resize_cache/avatar.jpg?v=2"),
             QStringLiteral("https://cdn-ru.bitrix24.ru/b456/resize_cache/avatar.jpg?v=2"),
             QStringLiteral("https://cdn-ru.bitrix24.ru/b123/resize_cache/avatar.jpg?v=3"),
             QStringLiteral("https://cdn-ru.bitrix24.ru/b123/resize_cache/avatar.jpg?v=2#other")}) {
        QVERIFY(cloud::avatarCdnRedirect(portal, source, 301, QUrl(candidate)).isEmpty());
    }
    const QUrl nonCache(QStringLiteral("https://tenant.bitrix24.ru/b123/other/avatar.jpg"));
    const QUrl unrelated(QStringLiteral("https://cdn-ru.bitrix24.ru/b123/other/avatar.jpg"));
    QVERIFY(cloud::avatarCdnRedirect(portal, nonCache, 301, unrelated).isEmpty());
    const QUrl secretSource(QStringLiteral("https://tenant.bitrix24.ru/b123/resize_cache/avatar.jpg?sessid=secret"));
    const QUrl secretTarget(QStringLiteral("https://cdn-ru.bitrix24.ru/b123/resize_cache/avatar.jpg?sessid=secret"));
    QVERIFY(cloud::avatarCdnRedirect(portal, secretSource, 301, secretTarget).isEmpty());
}

void TestCloudAvatars::rejectsUnapprovedUrlsAndRedirects()
{
    LocalServer receiver;
    receiver.respond = [](const Request &) -> Response { return {"200 OK", "image/png", png, {}}; };
    QVERIFY(receiver.listen());
    const auto other = receiver.origin().resolved(QUrl(validPath));
    const auto redirectPath = QStringLiteral("/upload/resize_cache/redirect.png");
    const auto forgedPath = QStringLiteral("/upload/resize_cache/unseen.png");
    const auto cdn = QUrl(QStringLiteral("https://cdn-ru.bitrix24.ru/b123/resize_cache/known"));
    const auto lookalike = QStringLiteral("https://cdn-ru.bitrix24.ru.invalid/b123/resize_cache/known");
    LocalServer server;
    server.respond = [&](const Request &request) -> Response {
        if (request.url.path() == redirectPath)
            return {"302 Found", "text/html", {}, "Location: " + other.toEncoded() + "\r\n"};
        if (QUrlQuery(request.url).queryItemValue(QStringLiteral("action")) == QLatin1String("im.v2.Recent.load"))
            return recents({redirectPath, other.toString(), cdn.toString(), lookalike,
                            QStringLiteral("/portal-setting.php"), QStringLiteral("/upload/%2E%2E/portal-setting.php"),
                            QStringLiteral("/upload/%252e%252e/portal-setting.php"),
                            QStringLiteral("https://cdn-ru.bitrix24.ru/b123/resize_cache/known?sessid=synthetic"),
                            QStringLiteral("/b123/resize_cache/avatar with space.png"),
                            QStringLiteral("/b123/resize_cache/avatar.png?x=with space"),
                            QStringLiteral("/upload/other space.png")});
        return {"200 OK", "application/json", "{\"result\":{}}", {}};
    };
    QVERIFY(server.listen());
    CloudBackend backend(server.session(), true);
    QSignalSpy chatSpy(&backend, &Backend::recentChatsLoaded);
    QSignalSpy avatarSpy(&backend, &Backend::avatarLoaded);
    backend.start();
    backend.requestRecentChats();
    QTRY_COMPARE(chatSpy.size(), 1);

    const auto rows = firstChats(chatSpy);
    QCOMPARE(rows.size(), 11);
    QCOMPARE(rows.at(0).avatarUrl, server.origin().resolved(QUrl(redirectPath)));
    QVERIFY(rows.at(1).avatarUrl.isEmpty());
    QCOMPARE(rows.at(2).avatarUrl, cdn);
    QVERIFY(rows.at(3).avatarUrl.isEmpty());
    QVERIFY(rows.at(4).avatarUrl.isEmpty());
    QVERIFY(rows.at(5).avatarUrl.isEmpty());
    QVERIFY(rows.at(6).avatarUrl.isEmpty());
    QVERIFY(rows.at(7).avatarUrl.isEmpty());
    QCOMPARE(rows.at(8).avatarUrl.path(QUrl::FullyEncoded), QStringLiteral("/b123/resize_cache/avatar%20with%20space.png"));
    QVERIFY(rows.at(9).avatarUrl.isEmpty());
    QVERIFY(rows.at(10).avatarUrl.isEmpty());

    backend.requestAvatar(rows.at(0).avatarUrl);
    backend.requestAvatar(other);
    backend.requestAvatar(server.origin().resolved(QUrl(forgedPath)));
    QTRY_COMPARE(avatarSpy.size(), 3);
    for (const auto &outcome : avatarSpy)
        QVERIFY(outcome.last().toByteArray().isEmpty());
    QCOMPARE(server.attempts(redirectPath), 1);
    QCOMPARE(server.attempts(forgedPath), 0);
    QCOMPARE(receiver.attempts(validPath), 0);
}

void TestCloudAvatars::rejectsUnsafeMimeAndOversizedImage()
{
    const auto mimePath = QStringLiteral("/upload/resize_cache/vector.png");
    const auto signaturePath = QStringLiteral("/upload/resize_cache/invalid.png");
    const auto largePath = QStringLiteral("/upload/resize_cache/large.png");
    LocalServer server;
    server.respond = [&](const Request &request) -> Response {
        if (request.url.path() == mimePath)
            return {"200 OK", "image/svg+xml", png, {}};
        if (request.url.path() == signaturePath)
            return {"200 OK", "image/png", "<html>login</html>", {}};
        if (request.url.path() == largePath)
            return {"200 OK", "image/png", png + QByteArray(128 * 1024, 'a'), {}};
        if (QUrlQuery(request.url).queryItemValue(QStringLiteral("action")) == QLatin1String("im.v2.Recent.load"))
            return recents({mimePath, signaturePath, largePath});
        return {"200 OK", "application/json", "{\"result\":{}}", {}};
    };
    QVERIFY(server.listen());
    CloudBackend backend(server.session(), true);
    QSignalSpy chatSpy(&backend, &Backend::recentChatsLoaded);
    QSignalSpy avatarSpy(&backend, &Backend::avatarLoaded);
    backend.start();
    backend.requestRecentChats();
    QTRY_COMPARE(chatSpy.size(), 1);

    for (const auto &row : firstChats(chatSpy))
        backend.requestAvatar(row.avatarUrl);
    QTRY_COMPARE(avatarSpy.size(), 3);
    for (const auto &outcome : avatarSpy)
        QVERIFY(outcome.last().toByteArray().isEmpty());
    QCOMPARE(server.attempts(mimePath), 1);
    QCOMPARE(server.attempts(signaturePath), 1);
    QCOMPARE(server.attempts(largePath), 1);
}

void TestCloudAvatars::capsConcurrentRequestsAndClearsPendingOnStop()
{
    QStringList paths;
    for (int i = 0; i < 6; ++i)
        paths.append(QStringLiteral("/upload/resize_cache/avatar%1.png").arg(i));
    LocalServer server;
    server.holdImages = true;
    server.respond = [&](const Request &request) -> Response {
        if (QUrlQuery(request.url).queryItemValue(QStringLiteral("action")) == QLatin1String("im.v2.Recent.load"))
            return recents(paths);
        return {"200 OK", "application/json", "{\"result\":{}}", {}};
    };
    QVERIFY(server.listen());
    CloudBackend backend(server.session(), true);
    QSignalSpy chatSpy(&backend, &Backend::recentChatsLoaded);
    QSignalSpy avatarSpy(&backend, &Backend::avatarLoaded);
    backend.start();
    backend.requestRecentChats();
    QTRY_COMPARE(chatSpy.size(), 1);
    for (const auto &row : firstChats(chatSpy))
        backend.requestAvatar(row.avatarUrl);

    const auto countImages = [&] {
        return std::count_if(server.requests.cbegin(), server.requests.cend(), isHeldImageRequest);
    };
    QTRY_COMPARE(countImages(), 4);
    backend.stop();
    QTRY_COMPARE(server.disconnectedImages, 4);
    QCOMPARE(countImages(), 4);
    QCOMPARE(avatarSpy.size(), 0);
}

QTEST_GUILESS_MAIN(TestCloudAvatars)
#include "tst_cloud_avatars.moc"
