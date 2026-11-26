#include "cloud_fixture.h"
#include "core/tunables.h"

#include <QJsonDocument>
#include <QSignalSpy>
#include <QTest>
#include <QUuid>
#include <algorithm>

using namespace bx;
using namespace cloud_test;

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


class TestCloudNotifications : public CloudTestCase {
    Q_OBJECT

private slots:
    void confirmsPortalDndAndPreservesExternalStatus();
    void settlesInitialStatusBeforeShowingDesktopAlerts_data();
    void settlesInitialStatusBeforeShowingDesktopAlerts();
    void readsOrdinaryChatAndNotificationHistory();
    void loadsOlderNotificationsOnDemandAndKeepsTheCursor();
    void advancesNotificationCursorAcrossTypesAndStopsOnRepeat();
    void searchesAndMutatesNotificationsOverTheSession();
    void decodesRichNotificationsFromHistoryAndPush();
    void resolvesNoticeAuthorFromMatchingUserOnly();
    void linksTaskChatEventsAndMarksSyntheticRowsLocally();
    void evictsLeastRecentlyUsedNotificationTitle();
    void classifiesOrdinaryChatPushesWithoutSuppressingPopups();
    void marksExactNoticeIdsAsReadAfterBatchConfirmation();
    void reloadsNotificationSettingsAfterPortalUpdate();
    void notificationActionsKeepPushNewerThanTheRequest();
    void notificationSettingSnapshotsKeepNewerPush();
    void desktopBannersCatchUpFromHistory_data();
    void desktopBannersCatchUpFromHistory();
    void gatesChatPopupsBySoundAndMessageSettings();
    void chatPopupsRespectExplicitDesktopDelivery_data();
    void chatPopupsRespectExplicitDesktopDelivery();
    void refreshesDesktopDeliveryWhenReturningToSimpleScheme_data();
    void refreshesDesktopDeliveryWhenReturningToSimpleScheme();
    void deliversChatPopupsWithExpertSettingsFromPush();
};

void TestCloudNotifications::confirmsPortalDndAndPreservesExternalStatus()
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

void TestCloudNotifications::settlesInitialStatusBeforeShowingDesktopAlerts_data()
{
    QTest::addColumn<QString>("status");
    QTest::addColumn<bool>("popup");
    QTest::addColumn<bool>("pushBeforeIdentity");
    QTest::newRow("confirmed DND") << QStringLiteral("dnd") << false << false;
    QTest::newRow("confirmed online") << QStringLiteral("online") << true << false;
    QTest::newRow("unsupported status") << QString() << true << false;
    QTest::newRow("DND Push before identity") << QStringLiteral("online") << false << true;
}

void TestCloudNotifications::settlesInitialStatusBeforeShowingDesktopAlerts()
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

void TestCloudNotifications::gatesChatPopupsBySoundAndMessageSettings()
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

void TestCloudNotifications::chatPopupsRespectExplicitDesktopDelivery_data()
{
    QTest::addColumn<bool>("desktopDelivery");
    QTest::addColumn<int>("messageVolume");
    QTest::newRow("desktop on, sound volume zero") << true << 0;
    QTest::newRow("desktop off, sound volume positive") << false << 100;
}

void TestCloudNotifications::chatPopupsRespectExplicitDesktopDelivery()
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

void TestCloudNotifications::refreshesDesktopDeliveryWhenReturningToSimpleScheme_data()
{
    QTest::addColumn<bool>("usePush");
    QTest::newRow("scheme-only Push") << true;
    QTest::newRow("settings snapshots") << false;
}

void TestCloudNotifications::refreshesDesktopDeliveryWhenReturningToSimpleScheme()
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

void TestCloudNotifications::deliversChatPopupsWithExpertSettingsFromPush()
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

void TestCloudNotifications::readsOrdinaryChatAndNotificationHistory()
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

void TestCloudNotifications::loadsOlderNotificationsOnDemandAndKeepsTheCursor()
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

void TestCloudNotifications::advancesNotificationCursorAcrossTypesAndStopsOnRepeat()
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

void TestCloudNotifications::searchesAndMutatesNotificationsOverTheSession()
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

void TestCloudNotifications::decodesRichNotificationsFromHistoryAndPush()
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

void TestCloudNotifications::resolvesNoticeAuthorFromMatchingUserOnly()
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

void TestCloudNotifications::linksTaskChatEventsAndMarksSyntheticRowsLocally()
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
    QCOMPARE(notice.excerpt, QStringLiteral("Сообщение в чате задачи: Готово"));
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

void TestCloudNotifications::evictsLeastRecentlyUsedNotificationTitle()
{
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy online(&backend, &Backend::connectionChanged);
    QSignalSpy recent(&backend, &Backend::recentChatsLoaded);
    QSignalSpy notices(&backend, &Backend::notificationsLoaded);
    backend.start();
    QTRY_COMPARE(online.size(), 1);

    const auto load = [&](int first, int count) {
        QJsonArray rows;
        for (int id = first; id < first + count; ++id) {
            rows.append(QJsonObject{{"dialogId", QStringLiteral("chat%1").arg(id)}, {"type", "chat"},
                                    {"title", QStringLiteral("Chat %1").arg(id)}});
        }
        fixture.ordinaryRecentBadgeFixture = QJsonDocument(QJsonObject{{"userId", 11}, {"recentItems", rows}}).toJson();
        recent.clear();
        backend.requestRecentChats();
        QTRY_VERIFY(!recent.isEmpty());
        QCOMPARE(recent.last().first().value<QVector<Chat>>().size(), count);
    };
    int messageId = 900;
    const auto checkTitle = [&](int id, const QString &title) {
        const QString message = QString::number(++messageId);
        notices.clear();
        fixture.pushMessage(message, message, id);
        QTRY_VERIFY(!notices.isEmpty());
        const auto notice = notices.last().first().value<QVector<Notification>>().first();
        QCOMPARE(notice.id, QStringLiteral("message:") + message);
        QCOMPARE(notice.title, title);
    };

    constexpr int page = tunables::kPageSizeChats;
    load(1, page);
    load(1 + page, page);
    checkTitle(1, QStringLiteral("Chat 1")); // Reading the oldest title protects it from eviction.
    load(1 + 2 * page, 2);
    checkTitle(1, QStringLiteral("Chat 1"));
    checkTitle(2, QStringLiteral("Новое сообщение"));
    checkTitle(3, QStringLiteral("Новое сообщение"));
    checkTitle(4, QStringLiteral("Chat 4"));
}

void TestCloudNotifications::classifiesOrdinaryChatPushesWithoutSuppressingPopups()
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

void TestCloudNotifications::marksExactNoticeIdsAsReadAfterBatchConfirmation()
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

void TestCloudNotifications::reloadsNotificationSettingsAfterPortalUpdate()
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

void TestCloudNotifications::notificationActionsKeepPushNewerThanTheRequest()
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

void TestCloudNotifications::notificationSettingSnapshotsKeepNewerPush()
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

void TestCloudNotifications::desktopBannersCatchUpFromHistory_data()
{
    QTest::addColumn<bool>("task");
    QTest::newRow("ordinary") << false;
    QTest::newRow("task") << true;
}

void TestCloudNotifications::desktopBannersCatchUpFromHistory()
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

QTEST_GUILESS_MAIN(TestCloudNotifications)
#include "tst_cloud_notifications.moc"
