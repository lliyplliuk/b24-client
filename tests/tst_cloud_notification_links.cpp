#include "cloud/cloud_backend.h"
#include "core/notification_targets.h"

#include <QHostAddress>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QUrlQuery>
#include <algorithm>
#include <utility>

using namespace bx;

namespace {

class ContextFixture {
public:
    bool listen()
    {
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (auto *socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                                 [this, socket, pending = QByteArray()]() mutable {
                    pending += socket->readAll();
                    const auto end = pending.indexOf("\r\n\r\n");
                    if (end < 0)
                        return;
                    qsizetype length = 0;
                    for (const auto &header : pending.left(end).split('\n')) {
                        if (header.toLower().startsWith("content-length:"))
                            length = header.mid(15).trimmed().toLongLong();
                    }
                    if (pending.size() < end + 4 + length)
                        return;
                    const auto path = pending.left(end).split(' ').value(1);
                    const auto url = QUrl::fromEncoded(path);
                    const QUrlQuery form(QString::fromUtf8(pending.mid(end + 4, length)));
                    const auto response = respond(url, form);
                    QObject::disconnect(socket, &QTcpSocket::readyRead, socket, nullptr);
                    socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: "
                                  + QByteArray::number(response.size()) + "\r\n\r\n" + response);
                    socket->disconnectFromHost();
                });
            }
        });
        return server.listen(QHostAddress::LocalHost);
    }

    Session session() const
    {
        return {QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort())), "B24=test", "local-csrf"};
    }

    QString lookupDialog;
    QString readsId;
    QString personId;
    QString contextCommand;
    bool attemptedRead = false;
    bool wrongScope = false;
    bool wrongPerson = false;
    bool missingPersonName = false;
    int currentIdentityReads = 0;

private:
    QByteArray respond(const QUrl &url, const QUrlQuery &form)
    {
        const auto fields = form.queryItems(QUrl::FullyDecoded);
        const auto field = [&fields](const QString &key) {
            for (const auto &entry : fields) {
                if (entry.first == key)
                    return entry.second;
            }
            return QString();
        };
        if (QUrlQuery(url).queryItemValue(QStringLiteral("action")) == QLatin1String("im.v2.Chat.load")) {
            lookupDialog = field(QStringLiteral("dialogId"));
            return R"({"status":"success","data":{"chat":{"id":77},"messages":[]}})";
        }
        if (QUrlQuery(url).queryItemValue(QStringLiteral("e")) == QLatin1String("socialnetwork.v2.current-user")) {
            ++currentIdentityReads;
            return R"({"status":"success","data":[{"extension":"socialnetwork.v2.current-user","settings":{"id":5}}]})";
        }
        if (url.path() == QLatin1String("/rest/im.user.get.json")) {
            personId = field(QStringLiteral("ID"));
            if (missingPersonName)
                return R"({"result":{"id":5}})";
            return wrongPerson ? R"({"result":{"id":6,"name":"Different user"}})"
                : R"({"result":{"id":5,"first_name":"Анна","last_name":"Орлова","work_position":"Инженер","avatar":"/upload/resize_cache/anna.png"}})";
        }
        if (url.path() == QLatin1String("/rest/batch.json")) {
            if (!field(QStringLiteral("cmd[notify]")).isEmpty()) {
                return R"({"result":{"result":{"notify":{"notifications":[{"id":2,"text":"[url=/online/?IM_DIALOG=chat99&IM_MESSAGE=414]Go[/url]"}]}}}})";
            }
            contextCommand = field(QStringLiteral("cmd[context]"));
            attemptedRead = form.toString(QUrl::FullyDecoded).contains(QStringLiteral("Message.read"));
            const auto chat = wrongScope ? "9" : "77";
            return QByteArray("{\"result\":{\"result\":{\"context\":{\"messages\":[{\"id\":51,\"chatId\":77,\"text\":\"Before\"},")
                + "{\"id\":52,\"chatId\":" + chat + ",\"text\":\"Requested\"}]}}}}";
        }
        return R"({"result":{}})";
    }
    QTcpServer server;
};

} // namespace

class TestCloudNotificationLinks : public QObject {
    Q_OBJECT

private slots:
    void parsesExplicitObjectLinksWithoutHtmlRendering();
    void rejectsUnsafeAndAmbiguousLinks();
    void verifiedPullScope();
    void loadsRemoteContextAndPersonAfterScopeChecks();
    void loadsNamedProfileForConfirmedCurrentAccount();
};

void TestCloudNotificationLinks::parsesExplicitObjectLinksWithoutHtmlRendering()
{
    const QUrl portal(QStringLiteral("https://tenant.bitrix24.ru"));
    const auto body = QStringLiteral(
        "<a class=\"todo\" href=\"/online/?IM_DIALOG=chat77&amp;IM_MESSAGE=52\"><b>Искомое</b></a> "
        "[url=/workgroups/group/14/tasks/task/view/42/]Посмотреть задачу[/url] "
        "<A HREF='https://tenant.bitrix24.ru/company/personal/user/5/'>Анна</A> "
        "[url]/workgroups/group/14/tasks/[/url] "
        "<a href=\"/online/?IM_TASK=chat88&amp;IM_MESSAGE=13&amp;IM_THREAD=Y\">Обсуждение</a>");
    const auto links = notificationTargets(portal, body);
    QCOMPARE(links.size(), 5);
    QCOMPARE(links.at(0).kind, NotificationTargetKind::Chat);
    QCOMPARE(links.at(0).id, QStringLiteral("chat77"));
    QCOMPARE(links.at(0).messageId, QStringLiteral("52"));
    QCOMPARE(links.at(0).label, QStringLiteral("Искомое"));
    QCOMPARE(links.at(1).kind, NotificationTargetKind::Task);
    QCOMPARE(links.at(1).projectId, QStringLiteral("14"));
    QCOMPARE(links.at(1).id, QStringLiteral("42"));
    QCOMPARE(links.at(2).kind, NotificationTargetKind::Person);
    QCOMPARE(links.at(2).id, QStringLiteral("5"));
    QCOMPARE(links.at(3).kind, NotificationTargetKind::Project);
    QCOMPARE(links.at(3).id, QStringLiteral("14"));
    QCOMPARE(links.at(4).id, QStringLiteral("chat88"));
    QCOMPARE(links.at(4).messageId, QStringLiteral("13"));
    QVERIFY(links.at(4).taskChat);
}

void TestCloudNotificationLinks::rejectsUnsafeAndAmbiguousLinks()
{
    const QUrl portal(QStringLiteral("https://tenant.bitrix24.ru"));
    const auto body = QStringLiteral(
        "Сообщения 36 и 42 "
        "[url=https://tenant.bitrix24.ru.bad/online/?IM_DIALOG=chat7]fake[/url] "
        "<a href='javascript:alert(1)'>unsafe</a> "
        "<a href='https://user:pass@tenant.bitrix24.ru/online/?IM_DIALOG=chat8'>credentials</a> "
        "<a href='/online/?IM_DIALOG=chat9&IM_DIALOG=chat10'>repeated</a> "
        "<a href='/online/?IM_DIALOG=chat9&IM_TASK=chat10'>collision</a> "
        "<a href='/online/?IM_DIALOG=chat8&IM_MESSAGE=42x'>number suffix</a> "
        "<a href='/online/?IM_TASK=42'>numeric task chat unrelated to task UID</a> "
        "<a data-href='/online/?IM_DIALOG=chat8'>fake attr</a> "
        "<a title=\"fake href='/online/?IM_DIALOG=chat8'\">fake quoted attr</a> "
        "<div title='[url=/online/?IM_DIALOG=chat8]hidden[/url]'></div> "
        "<div title=\"<a href='/online/?IM_DIALOG=chat8'>hidden anchor</a>\"></div> "
        "<!-- <a href='/online/?IM_DIALOG=chat8'>commented</a> --> "
        "<script>[url=/online/?IM_DIALOG=chat8]script[/url]</script> "
        "<a href='/ arbitrary/?IM_DIALOG=chat8'>wrong route</a>");
    QVERIFY(notificationTargets(portal, body).isEmpty());
    QVERIFY(notificationTargets(portal,
        QStringLiteral("<div title=\"<a href='/online/?IM_DIALOG=chat8'>unterminated</a>")).isEmpty());

    const auto realAnchor = notificationTargets(portal,
        QStringLiteral("<a href='/online/?IM_DIALOG=chat11'><span title='fake </a>'>Чат</span></a>"));
    QCOMPARE(realAnchor.size(), 1);
    QCOMPARE(realAnchor.first().id, QStringLiteral("chat11"));
    QCOMPARE(realAnchor.first().label, QStringLiteral("Чат"));

    QString longBody;
    for (int i = 1; i <= 20; ++i)
        longBody += QStringLiteral("[url=/online/?IM_DIALOG=chat%1]item[/url]").arg(i);
    QCOMPARE(notificationTargets(portal, longBody).size(), 8);
}

void TestCloudNotificationLinks::verifiedPullScope()
{
    const QJsonObject message{{QStringLiteral("id"), 52}, {QStringLiteral("chatId"), 77}};
    QJsonObject params{{QStringLiteral("dialogId"), QStringLiteral("9")}, {QStringLiteral("chatId"), 77},
                       {QStringLiteral("message"), message}};
    const auto personal = incomingMessageTarget(params, false);
    QVERIFY(personal.has_value());
    QCOMPARE(personal->id, QStringLiteral("9"));
    QCOMPARE(personal->messageId, QStringLiteral("52"));
    params.remove(QStringLiteral("dialogId"));
    QVERIFY(!incomingMessageTarget(params, false).has_value());
    QCOMPARE(incomingMessageTarget(params, true)->id, QStringLiteral("chat77"));
    params.insert(QStringLiteral("dialogId"), QStringLiteral("chat9"));
    QVERIFY(!incomingMessageTarget(params, true).has_value());
}

void TestCloudNotificationLinks::loadsRemoteContextAndPersonAfterScopeChecks()
{
    ContextFixture fixture;
    QVERIFY(fixture.listen());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy context(&backend, &Backend::messageContextLoaded);
    QSignalSpy users(&backend, &Backend::personLoaded);
    QSignalSpy notifications(&backend, &Backend::notificationsLoaded);
    QSignalSpy errors(&backend, &Backend::failed);
    backend.start();

    backend.requestNotifications();
    QTRY_COMPARE(notifications.size(), 1);
    const auto rows = notifications.first().first().value<QVector<Notification>>();
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.first().text, QStringLiteral("Go"));
    QCOMPARE(rows.first().targets.size(), 1);
    QCOMPARE(rows.first().targets.first().id, QStringLiteral("chat99"));
    QCOMPARE(rows.first().targets.first().messageId, QStringLiteral("414"));

    backend.requestMessageContext(QStringLiteral("9"), QStringLiteral("52"));
    QTRY_COMPARE(context.size(), 1);
    QCOMPARE(fixture.lookupDialog, QStringLiteral("9"));
    QCOMPARE(fixture.contextCommand, QStringLiteral("im.v2.Chat.Message.getContext?id=52&range=25"));
    QVERIFY(!fixture.attemptedRead);
    QCOMPARE(context.first().at(0).toString(), QStringLiteral("9"));
    QCOMPARE(context.first().at(1).toString(), QStringLiteral("52"));
    QCOMPARE(context.first().at(2).value<QVector<Message>>().last().text, QStringLiteral("Requested"));

    fixture.wrongScope = true;
    backend.requestMessageContext(QStringLiteral("9"), QStringLiteral("52"));
    QTRY_VERIFY(std::any_of(errors.cbegin(), errors.cend(), [](const auto &failure) {
        return failure.first().toString() == QLatin1String("message-context:9:52");
    }));
    QCOMPARE(context.size(), 1);

    backend.requestPerson(QStringLiteral("5"));
    QTRY_COMPARE(users.size(), 1);
    QCOMPARE(fixture.personId, QStringLiteral("5"));
    const auto person = users.first().first().value<Person>();
    QCOMPARE(person.id, QStringLiteral("5"));
    QCOMPARE(person.name, QStringLiteral("Анна Орлова"));
    QCOMPARE(person.position, QStringLiteral("Инженер"));
    QCOMPARE(person.avatarUrl.path(), QStringLiteral("/upload/resize_cache/anna.png"));

    fixture.wrongPerson = true;
    backend.requestPerson(QStringLiteral("5"));
    QTRY_VERIFY(std::any_of(errors.cbegin(), errors.cend(), [](const auto &failure) {
        return failure.first().toString() == QLatin1String("person:5");
    }));
    QCOMPARE(users.size(), 1);
}

void TestCloudNotificationLinks::loadsNamedProfileForConfirmedCurrentAccount()
{
    ContextFixture fixture;
    QVERIFY(fixture.listen());
    CloudBackend backend(fixture.session(), true);
    backend.start();
    QVector<Person> people;
    const auto receive = [&people](Person person) { people.append(std::move(person)); };
    backend.requestCurrentPerson(receive);
    backend.requestCurrentPerson(receive);
    QTRY_COMPARE(people.size(), 2);
    QVERIFY(fixture.currentIdentityReads > 0);
    QCOMPARE(fixture.personId, QStringLiteral("5"));
    for (const auto &person : std::as_const(people)) {
        QCOMPARE(person.id, QStringLiteral("5"));
        QCOMPARE(person.name, QStringLiteral("Анна Орлова"));
        QCOMPARE(person.avatarUrl.path(), QStringLiteral("/upload/resize_cache/anna.png"));
    }

    fixture.wrongPerson = true;
    backend.requestCurrentPerson(receive);
    QTRY_COMPARE(people.size(), 3);
    QVERIFY(people.last().id.isEmpty());
    fixture.wrongPerson = false;
    fixture.missingPersonName = true;
    backend.requestCurrentPerson(receive);
    QTRY_COMPARE(people.size(), 4);
    QVERIFY(people.last().id.isEmpty() && people.last().name.isEmpty());
}

QTEST_GUILESS_MAIN(TestCloudNotificationLinks)
#include "tst_cloud_notification_links.moc"
