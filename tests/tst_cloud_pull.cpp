#include "cloud_fixture.h"

#include <QSignalSpy>
#include <QTest>

using namespace bx;
using namespace cloud_test;

class TestCloudPull : public CloudTestCase {
    Q_OBJECT

private slots:
    void refreshesOnlyActiveViewsAndKeepsInflightInvalidations();
    void receivesInBackgroundAndResumesAfterDisconnect();
    void catchesUpWhenAnActiveChatStopsReceivingPush();
    void refreshesVisibleInactiveHistory_data();
    void refreshesVisibleInactiveHistory();
    void dispatchesPullByModuleAndDeduplicatesAcrossModules();
};

void TestCloudPull::refreshesOnlyActiveViewsAndKeepsInflightInvalidations()
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
    QVERIFY(fixture.waitForPullDelivery());
    CloudTestAccess::refreshNow(backend);
    QVERIFY(!CloudTestAccess::http(backend).contains(operationKey(OperationKind::Chat, QStringLiteral("chat91"))));
    QCOMPARE(fixture.requestsFor(chatLoad).size(), 2);
    const auto recentBeforeSwitch = recent.size();

    backend.setActiveSection(0);
    QTRY_COMPARE(fixture.requestsFor(chatLoad).size(), 3);
    QTRY_COMPARE(recent.size(), recentBeforeSwitch + 1);
}

void TestCloudPull::receivesInBackgroundAndResumesAfterDisconnect()
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

void TestCloudPull::catchesUpWhenAnActiveChatStopsReceivingPush()
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

void TestCloudPull::refreshesVisibleInactiveHistory_data()
{
    QTest::addColumn<bool>("task");
    QTest::newRow("ordinary conversation") << false;
    QTest::newRow("task conversation over another section") << true;
}

void TestCloudPull::refreshesVisibleInactiveHistory()
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
    QVERIFY(fixture.waitForPullDelivery());
    const auto historyOperation = task ? operationKey(OperationKind::TaskMessages, QStringLiteral("42"))
                                       : operationKey(OperationKind::Chat, QStringLiteral("chat91"));
    CloudTestAccess::refreshNow(backend);
    QVERIFY(!CloudTestAccess::http(backend).contains(historyOperation));
    QCOMPARE(history.size(), 2);
    QTRY_COMPARE(fixture.requestsFor(QStringLiteral("im.v2.Recent.load")).size(), recentReads + (task ? 0 : 1));

    auto *timer = backend.findChild<QTimer *>(QStringLiteral("active-history-catchup"));
    QVERIFY(timer && timer->isActive());
    QCOMPARE(timer->interval(), 15000);
    QVERIFY(QMetaObject::invokeMethod(timer, "timeout"));
    QTRY_COMPARE(history.size(), 3);

    backend.setWorkspaceVisible(false);
    QVERIFY(!timer->isActive());
    push(QStringLiteral("hidden"), QStringLiteral("904"));
    QVERIFY(fixture.waitForPullDelivery());
    CloudTestAccess::refreshNow(backend);
    QVERIFY(!timer->isActive());
    QVERIFY(!CloudTestAccess::http(backend).contains(historyOperation));
    QCOMPARE(history.size(), 3);
    backend.setWorkspaceVisible(true);
    QTRY_COMPARE(history.size(), 4);
    QVERIFY(timer->isActive());
    backend.clearOpenHistory(task);
    QVERIFY(!timer->isActive());
    push(QStringLiteral("closed"), QStringLiteral("905"));
    QVERIFY(fixture.waitForPullDelivery());
    CloudTestAccess::refreshNow(backend);
    QVERIFY(!timer->isActive());
    QVERIFY(!CloudTestAccess::http(backend).contains(historyOperation));
    QCOMPARE(history.size(), 4);
    QVERIFY(fixture.requestsFor(QStringLiteral("im.v2.Chat.Message.read")).isEmpty());
    QVERIFY(fixture.requestsFor(QStringLiteral("applyFilter")).isEmpty());
}

void TestCloudPull::dispatchesPullByModuleAndDeduplicatesAcrossModules()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy connected(&backend, &Backend::connectionChanged);
    QSignalSpy invalidated(&backend, &Backend::taskLinkPreviewInvalidated);
    QSignalSpy notices(&backend, &Backend::notificationsLoaded);
    backend.start();
    QTRY_VERIFY(!connected.isEmpty() && connected.last().first().toBool());

    const QJsonObject task{{"fields", QJsonObject{{"ID", 37}, {"GROUP_ID", 5}}}};
    fixture.pushIm(QStringLiteral("wrong-module"), QStringLiteral("taskUpdate"), task);
    fixture.pushIm(QStringLiteral("shared-id"), QStringLiteral("taskUpdate"), task, 0, QStringLiteral("tasks"));
    fixture.pushIm(QStringLiteral("shared-id"), QStringLiteral("notifyAdd"), {{"id", 81}, {"text", "duplicate"}});
    fixture.pushIm(QStringLiteral("new-id"), QStringLiteral("notifyAdd"), {{"id", 82}, {"text", "new notice"}});

    QTRY_VERIFY(!notices.isEmpty());
    QCOMPARE(invalidated.size(), 1);
    QCOMPARE(invalidated.first().first().toString(), QStringLiteral("37"));
    const auto feed = notices.last().first().value<QVector<Notification>>();
    QCOMPARE(feed.size(), 1);
    QCOMPARE(feed.first().id, QStringLiteral("82"));
}

QTEST_GUILESS_MAIN(TestCloudPull)
#include "tst_cloud_pull.moc"
