#include "cloud_fixture.h"
#include "signin/session_restore.h"

#include <QSignalSpy>
#include <QTest>

using namespace bx;
using namespace cloud_test;

class TestCloudHttp : public CloudTestCase {
    Q_OBJECT

private slots:
    void learnsRotatedCredentialsFromAcceptedPortalReplies();
    void recognizesExplicitAuthorizationRejection();
    void scopesAndCoalescesEndpointAuthorizationRejections();
    void refreshesCsrfWithoutRepeatingAnUnconfirmedWrite_data();
    void refreshesCsrfWithoutRepeatingAnUnconfirmedWrite();
    void limitsCsrfRefreshAttempts_data();
    void limitsCsrfRefreshAttempts();
    void refreshesSavedCsrfBeforeOpeningWorkspace_data();
    void refreshesSavedCsrfBeforeOpeningWorkspace();
    void refusesToFollowRedirectWithSession();
    void dropsDuplicateSendBeforePosting();
    void rejectedRequestUsesExplicitPolicy();
    void joinedSubscribersRespectAccountScope_data();
    void joinedSubscribersRespectAccountScope();
    void overlappingTaskSearchFailure();
};

void TestCloudHttp::dropsDuplicateSendBeforePosting()
{
    using namespace Qt::StringLiterals;
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    backend.start();
    const auto action = u"im.v2.Chat.Message.send"_s;
    const auto send = [&](const QString &text, int &completed) {
        CloudTestAccess::http(backend).ajax(action, {{u"dialogId"_s, u"chat91"_s}, {u"fields[message]"_s, text}},
                    u"send:chat:chat91"_s, [&completed](QJsonValue) { ++completed; });
    };
    int first = 0, duplicates = 0;
    send(u"Первое"_s, first);
    send(u"Первое"_s, duplicates);
    send(u"Второе"_s, duplicates);
    QTRY_COMPARE(first, 1);
    QCOMPARE(fixture.requestsFor(action).size(), 1);
    QCOMPARE(duplicates, 0);
}

void TestCloudHttp::rejectedRequestUsesExplicitPolicy()
{
    CloudBackend backend(Session{}, true);
    QSignalSpy failures(&backend, &Backend::failed);
    const auto operation = operationKey(OperationKind::TaskHistory, QStringLiteral("42"));
    int fallbacks = 0;
    const auto onData = [&](QJsonValue value) {
        QVERIFY(value.isNull() || value.isUndefined());
        ++fallbacks;
    };

    const auto url = CloudTestAccess::http(backend).actionUrl(QStringLiteral("tasks.task.history.list"));
    CloudTestAccess::http(backend).webRequest(url, {}, operation, QStringLiteral("data"), onData);
    QCOMPARE(fallbacks, 1);
    QVERIFY(failures.isEmpty());

    // The same optional endpoint is mandatory for this caller.
    CloudTestAccess::http(backend).webRequest(url, {}, operation, QStringLiteral("data"), onData, {}, {}, false, RequestPolicy::Required);
    QCOMPARE(fallbacks, 1);
    QCOMPARE(failures.size(), 1);
    QCOMPARE(failures.first().first().toString(), operation);
}

void TestCloudHttp::joinedSubscribersRespectAccountScope_data()
{
    QTest::addColumn<bool>("resetDuringDelivery");
    QTest::newRow("same-account") << false;
    QTest::newRow("first-subscriber-resets-account") << true;
}

void TestCloudHttp::joinedSubscribersRespectAccountScope()
{
    using namespace Qt::StringLiterals;
    QFETCH(bool, resetDuringDelivery);
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    backend.start();
    const auto action = u"tasks.task.list"_s;
    const auto operation = u"search-project-tasks:17"_s;
    int first = 0, second = 0;
    CloudTestAccess::http(backend).ajax(action, {}, operation, [&](QJsonValue) {
        ++first;
        if (resetDuringDelivery)
            CloudTestAccess::resetAccount(backend);
    });
    CloudTestAccess::http(backend).ajax(action, {}, operation, [&](QJsonValue) { ++second; });

    QTRY_COMPARE(first, 1);
    QCOMPARE(second, resetDuringDelivery ? 0 : 1);
    QCOMPARE(fixture.requestsFor(action).size(), 1);
}

void TestCloudHttp::overlappingTaskSearchFailure()
{
    using namespace Qt::StringLiterals;
    CloudFixture fixture;
    QVERIFY(fixture.start());
    fixture.rejectTaskSearch = true;
    CloudBackend backend(fixture.session(), true);
    QSignalSpy failures(&backend, &Backend::failed);
    backend.start();
    QStringList errors;
    int successes = 0;
    const PortalHttp::Handler subscriber{
        [&](QJsonValue) { ++successes; },
        [&](const QString &reason) { errors.append(reason); }
    };
    const auto operation = u"search-project-tasks:17"_s;
    CloudTestAccess::http(backend).ajax(u"tasks.task.list"_s, {}, operation, subscriber);
    CloudTestAccess::http(backend).ajax(u"tasks.task.list"_s, {}, operation, subscriber);
    QTRY_VERIFY(!failures.isEmpty());
    QCOMPARE(failures.size(), 1);
    QCOMPARE(failures.first().first().toString(), operation);
    const auto reason = failures.first().at(1).toString();
    QVERIFY(!reason.isEmpty());
    QCOMPARE(errors, (QStringList{reason, reason}));
    QCOMPARE(successes, 0);
    QCOMPARE(fixture.requestsFor(u"tasks.task.list"_s).size(), 1);

    fixture.rejectTaskSearch = false;
    fixture.malformedTaskSearch = true;
    failures.clear();
    backend.searchProjectTasks(u"17"_s, u"#149"_s);
    backend.searchProjectTasks(u"17"_s, u"#149"_s);
    QTRY_VERIFY(!failures.isEmpty());
    QCOMPARE(failures.size(), 1);
    QCOMPARE(failures.first().first().toString(), operation);
    QCOMPARE(fixture.requestsFor(u"tasks.task.list"_s).size(), 2);
}

void TestCloudHttp::learnsRotatedCredentialsFromAcceptedPortalReplies()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    fixture.chatResponseHeaders = "Set-Cookie: BITRIX_TEST=rotated; Path=/; HttpOnly\r\n"
        "Set-Cookie: scoped=must-not-escape; Path=/bitrix\r\n"
        "Set-Cookie: other=must-not-escape; Domain=unrelated.invalid; Path=/\r\n"
        "X-Bitrix-Csrf-Token: bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\r\n";

    CloudBackend backend(fixture.session(), true);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    QSignalSpy updated(&backend, &CloudBackend::sessionUpdated);
    backend.start();
    const auto revision = CloudTestAccess::sessionRevision(backend);
    backend.requestChatMessages(QStringLiteral("chat91"));
    QTRY_COMPARE(history.size(), 1);

    QCOMPARE(updated.size(), 1);
    QCOMPARE(CloudTestAccess::sessionRevision(backend), revision + 1);
    backend.requestChatMessages(QStringLiteral("chat91"));
    QTRY_COMPARE(history.size(), 2);
    QCOMPARE(updated.size(), 1);
    QCOMPARE(CloudTestAccess::sessionRevision(backend), revision + 1);

    const auto requests = fixture.requestsFor(QStringLiteral("im.v2.Chat.load"));
    QCOMPARE(requests.size(), 2);
    QCOMPARE(requests.first().headers.value("cookie"), fixtureCookie);
    QCOMPARE(requests.last().headers.value("cookie"), QByteArray("BITRIX_TEST=rotated"));
    QCOMPARE(requests.last().headers.value("x-bitrix-csrf-token"), QByteArray(32, 'b'));
    QCOMPARE(requests.last().field(QStringLiteral("sessid")), QString(32, QLatin1Char('b')));
}

void TestCloudHttp::recognizesExplicitAuthorizationRejection()
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

void TestCloudHttp::scopesAndCoalescesEndpointAuthorizationRejections()
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

void TestCloudHttp::refreshesCsrfWithoutRepeatingAnUnconfirmedWrite_data()
{
    QTest::addColumn<QByteArray>("challengeCookie");
    QTest::newRow("token-only") << QByteArray();
    QTest::newRow("token-and-cookie") << QByteArray("BITRIX_TEST=rotated");
}

void TestCloudHttp::refreshesCsrfWithoutRepeatingAnUnconfirmedWrite()
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

void TestCloudHttp::limitsCsrfRefreshAttempts_data()
{
    QTest::addColumn<QByteArray>("offered");
    QTest::addColumn<int>("probes");
    QTest::addColumn<bool>("canonicalRenews");
    QTest::newRow("malformed") << QByteArray("invalid") << 1 << false;
    QTest::newRow("same-token") << QByteArray(32, 'a') << 2 << true;
    QTest::newRow("second-challenge") << QByteArray(32, 'b') << 1 << false;
}

void TestCloudHttp::limitsCsrfRefreshAttempts()
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

void TestCloudHttp::refreshesSavedCsrfBeforeOpeningWorkspace_data()
{
    refreshesCsrfWithoutRepeatingAnUnconfirmedWrite_data();
}

void TestCloudHttp::refreshesSavedCsrfBeforeOpeningWorkspace()
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

void TestCloudHttp::refusesToFollowRedirectWithSession()
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

QTEST_GUILESS_MAIN(TestCloudHttp)
#include "tst_cloud_http.moc"
