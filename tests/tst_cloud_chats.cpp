#include "cloud_fixture.h"
#include "core/tunables.h"
#include "ui/reaction_catalog.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QTest>
#include <QUuid>
#include <algorithm>
#include <initializer_list>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

using namespace bx;
using namespace cloud_test;

class TestCloudChats : public CloudTestCase {
    Q_OBJECT

private slots:
    void refreshesTaskChatAfterProjectChange_data();
    void refreshesTaskChatAfterProjectChange();
    void loadsOlderHistoryWithScopedCursor();
    void retriesChatHistoryWithTheKnownChatId_data();
    void retriesChatHistoryWithTheKnownChatId();
    void recoversLatestHistoryAfterATemporaryFailure_data();
    void recoversLatestHistoryAfterATemporaryFailure();
    void retriesCoalescedHistoryRead_data();
    void retriesCoalescedHistoryRead();
    void coalescesManualHistoryRefreshWhileLatestReadIsInFlight();
    void reconcilesUnreadFromScopedActiveHistory();
    void ignoresParticipantCountsInUnreadBadges();
    void distinguishesPersonalDialogAndTaskWithSameId();
    void overlappingTaskChatTitles();
    void derivesUnreadAndRefreshesOnReadEvents();
    void separatesChatScopesAndUsesPortalMetadata();
    void searchesTaskChatsByAccessibleTaskTitleAndNumber();
    void loadsPersonalPresenceAndPinnedMessagesWithLiveChanges();
    void usesSenderIdentityAndDecodesForwardAndQuoteMetadata();
    void streamsOnlyRegisteredMessageFilesAndProtectsTargetOnMismatch();
    void refusingUnsafeDownloadTargetsLeavesLinkDestinationUntouched();
    void marksOnlyDismissedPersonalChatsAndUsesOneHydration();
    void immediatelyUpdatesLiveRecentAndProtectsItFromOldSnapshots();
    void loadsAuthoritativeUnreadCountsForBothChatSections();
    void totalsUnreadChatsOutsideRecentAndTracksPush();
    void deduplicatesRecentFallbackAndKeepsPushNewerThanTheSnapshot();
    void guardsRecentPreviewMessageScope();
    void loadsAndClearsPersonalAnchorsIndependentlyOfUnreadCounts();
    void marksOnlyTheVisibleDialogAfterConfirmedRead();
    void delayedRecentReplyDoesNotUndoConfirmedReadAfterSending_data();
    void delayedRecentReplyDoesNotUndoConfirmedReadAfterSending();
    void acknowledgesVisibleReactionAnchorsWithoutLosingNewPush();
    void exactReadLeavesOlderUnseenMessageEligible();
    void readsExplicitTaskUnreadBeforeNewerSystemCursor();
    void keepsAllExplicitUnreadRowsInTheNewlyLoadedOlderPage();
    void restoresReplyFromKeyedPhotoAndKeepsExactScope();
    void sendsOriginalReplyIdInBothChatScopes();
    void performsVerifiedMessageActions();
    void refreshesConfirmedReactions();
    void decodesFullReactionCatalog();
    void createsPrivateGroupAndChangesMembers();
    void loadsBoundedScopedMediaAndLinks();
    void sendsAndReceivesScopedTypingActivity();
};

void TestCloudChats::loadsAuthoritativeUnreadCountsForBothChatSections()
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

void TestCloudChats::totalsUnreadChatsOutsideRecentAndTracksPush()
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

void TestCloudChats::deduplicatesRecentFallbackAndKeepsPushNewerThanTheSnapshot()
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

void TestCloudChats::guardsRecentPreviewMessageScope()
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

void TestCloudChats::loadsAndClearsPersonalAnchorsIndependentlyOfUnreadCounts()
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

void TestCloudChats::marksOnlyTheVisibleDialogAfterConfirmedRead()
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
    const auto &http = CloudTestAccess::http(backend);
    QVERIFY(!http.contains(operationKey(OperationKind::ReadVisibleMessages, dialog)));
    QVERIFY(!http.contains(operationKey(OperationKind::ReadVisibleMessages, QStringLiteral("chat92"))));
    QVERIFY(!http.contains(operationKey(OperationKind::ReadVisibleAnchors, QStringLiteral("91"))));
    QVERIFY(!http.contains(operationKey(OperationKind::ReadVisibleAnchors, QStringLiteral("92"))));
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
    QVERIFY(!http.contains(operationKey(OperationKind::ReadVisibleMessages, dialog)));
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

void TestCloudChats::delayedRecentReplyDoesNotUndoConfirmedReadAfterSending_data()
{
    QTest::addColumn<bool>("requestBeforeRead");
    QTest::addColumn<bool>("readResultHasCursor");
    QTest::newRow("in-flight-recent") << true << false;
    QTest::newRow("stale-cursor-after-read") << false << true;
    QTest::newRow("stale-unread-id-after-read") << false << false;
}

void TestCloudChats::delayedRecentReplyDoesNotUndoConfirmedReadAfterSending()
{
    QFETCH(bool, requestBeforeRead);
    QFETCH(bool, readResultHasCursor);
    CloudFixture fixture;
    fixture.chatBadgesFixture = true;
    fixture.holdNextOnlineResponse = true;
    fixture.approveVisibleRead = true;
    fixture.visibleReadResult = readResultHasCursor
        ? R"({"chatId":91,"counter":0,"lastId":800,"viewedMessages":[800]})"
        : R"({"chatId":91,"counter":0,"viewedMessages":[800]})";
    fixture.ordinaryRecentBadgeFixture = R"({"userId":11,
        "recentItems":[{"dialogId":"103","chatId":91,"messageId":800,"counter":1}],
        "chats":[{"id":91,"dialogId":"103","type":"user","lastId":799}],
        "messages":[{"id":800,"chatId":91,"authorId":103,"text":"Incoming"}]})";
    fixture.chatResponseOverride = ajaxResult(R"({"chat":{"id":91,"dialogId":"103","lastId":799,"unreadId":800},
        "messages":[{"id":800,"chatId":91,"authorId":103,"text":"Incoming","unread":true}]})");
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy recent(&backend, &Backend::recentChatsLoaded);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    QSignalSpy sent(&backend, &Backend::messageSent);
    QSignalSpy confirmed(&backend, &Backend::chatReadConfirmed);
    const auto row = [&recent] {
        return recent.isEmpty() ? Chat{} : recent.last().first().value<QVector<Chat>>().value(0);
    };
    const auto dialog = QStringLiteral("103");
    const auto recentRequest = QStringLiteral("im.v2.Recent.load");
    backend.start();
    backend.requestRecentChats();
    QTRY_COMPARE(row().unread, 1);
    backend.requestChatMessages(dialog);
    QTRY_VERIFY(!history.isEmpty());
    const auto acknowledgeRead = [&] {
        backend.readChatMessages(dialog, {QStringLiteral("800")});
        QTRY_COMPARE(confirmed.size(), 1);
        QCOMPARE(row().unread, 0);
    };
    if (!requestBeforeRead)
        acknowledgeRead();
    backend.sendChatMessage(dialog, QStringLiteral("Reply"));
    QTRY_COMPARE(sent.size(), 1);

    fixture.ordinaryRecentBadgeFixture = R"({"userId":11,
        "recentItems":[{"dialogId":"103","chatId":91,"messageId":801,"counter":1}],
        "chats":[{"id":91,"dialogId":"103","type":"user","lastId":799)";
    if (!requestBeforeRead && !readResultHasCursor)
        fixture.ordinaryRecentBadgeFixture += R"(,"unreadId":800)";
    fixture.ordinaryRecentBadgeFixture += R"(}],
        "messages":[{"id":801,"chatId":91,"authorId":11,"text":"Reply"}]})";
    const auto requests = fixture.requestsFor(recentRequest).size();
    fixture.holdNextReadResponse = recentRequest;
    backend.requestRecentChats();
    QTRY_VERIFY(fixture.requestsFor(recentRequest).size() > requests && fixture.holdNextReadResponse.isEmpty());
    if (requestBeforeRead)
        acknowledgeRead();

    fixture.releaseHeldReadResponse();
    QTRY_COMPARE(row().lastMessage, QStringLiteral("Reply"));
    QCOMPARE(row().unread, 0);
    QCOMPARE(backend.unreadChatCount().value_or(-1), 0);

    const auto repeatedRecent = recent.size();
    backend.requestRecentChats();
    QTRY_VERIFY(recent.size() > repeatedRecent);
    QCOMPARE(row().unread, 0);

    fixture.pushIm(QStringLiteral("incoming-after-reply"), QStringLiteral("message"),
        {{"chatId", 91}, {"dialogId", dialog}, {"counter", 1},
         {"message", QJsonObject{{"id", 802}, {"chatId", 91}, {"senderId", 103}, {"text", "New incoming"}}}});
    QTRY_COMPARE(row().unread, 1);
    QCOMPARE(backend.unreadChatCount().value_or(-1), 1);
}

void TestCloudChats::acknowledgesVisibleReactionAnchorsWithoutLosingNewPush()
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
    QTRY_VERIFY(!CloudTestAccess::http(backend).contains(operationKey(OperationKind::ReadVisibleAnchors, QStringLiteral("91"))));
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

void TestCloudChats::exactReadLeavesOlderUnseenMessageEligible()
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

void TestCloudChats::readsExplicitTaskUnreadBeforeNewerSystemCursor()
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
    const auto readOperation = operationKey(OperationKind::ReadVisibleMessages, dialog);
    QVERIFY(!CloudTestAccess::http(backend).contains(readOperation));
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
    QVERIFY(!CloudTestAccess::http(backend).contains(readOperation));
    QCOMPARE(fixture.requestsFor(action).size(), 1);
}

void TestCloudChats::keepsAllExplicitUnreadRowsInTheNewlyLoadedOlderPage()
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

void TestCloudChats::restoresReplyFromKeyedPhotoAndKeepsExactScope()
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

void TestCloudChats::sendsOriginalReplyIdInBothChatScopes()
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

void TestCloudChats::performsVerifiedMessageActions()
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
    QVERIFY(accepted.first().at(1).toString().isEmpty());
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

void TestCloudChats::refreshesConfirmedReactions()
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

void TestCloudChats::decodesFullReactionCatalog()
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

void TestCloudChats::createsPrivateGroupAndChangesMembers()
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

void TestCloudChats::loadsBoundedScopedMediaAndLinks()
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

void TestCloudChats::sendsAndReceivesScopedTypingActivity()
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

void TestCloudChats::immediatelyUpdatesLiveRecentAndProtectsItFromOldSnapshots()
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
    QVERIFY(fixture.waitForPullDelivery());
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
    QVERIFY(fixture.waitForPullDelivery());
    QCOMPARE(ordinary.size(), 6);
}

void TestCloudChats::marksOnlyDismissedPersonalChatsAndUsesOneHydration()
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

void TestCloudChats::refusingUnsafeDownloadTargetsLeavesLinkDestinationUntouched()
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

void TestCloudChats::usesSenderIdentityAndDecodesForwardAndQuoteMetadata()
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

void TestCloudChats::streamsOnlyRegisteredMessageFilesAndProtectsTargetOnMismatch()
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

void TestCloudChats::separatesChatScopesAndUsesPortalMetadata()
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

void TestCloudChats::searchesTaskChatsByAccessibleTaskTitleAndNumber()
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
    const auto oldSearch = CloudTestAccess::pendingReplyForRoute(backend, url);
    QVERIFY(oldSearch);
    QSignalSpy accounts(&backend, &Backend::accountScopeChanged);
    fixture.ordinaryRecentBadgeFixture = R"({"userId":22,"recentItems":[],"chats":[]})";
    backend.requestRecentChats();
    QTRY_COMPARE(accounts.size(), 1);
    fixture.taskChatSearchRows = {};
    backend.searchTaskChats(query);
    QTRY_COMPARE(results.size(), 5);
    QVERIFY(results.last().at(1).value<QVector<Chat>>().isEmpty());
    fixture.releaseHeldNotificationReply();
    QTRY_VERIFY(oldSearch.isNull());
    QCOMPARE(results.size(), 5);
    QVERIFY(errors.isEmpty());
}

void TestCloudChats::loadsPersonalPresenceAndPinnedMessagesWithLiveChanges()
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

void TestCloudChats::derivesUnreadAndRefreshesOnReadEvents()
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

void TestCloudChats::overlappingTaskChatTitles()
{
    using namespace Qt::StringLiterals;
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy loaded(&backend, &Backend::taskChatsLoaded);
    backend.start();
    const auto requestTitles = [&](std::initializer_list<int> tasks) {
        QJsonArray rows;
        for (const int task : tasks) {
            rows.append(QJsonObject{{"dialogId", u"chat%1"_s.arg(task)}, {"type", "tasksTask"},
                {"entityLink", QJsonObject{{"id", task}, {"type", "TASKS_TASK"}}}});
        }
        fixture.taskRecentBadgeFixture = QJsonDocument(QJsonObject{{"userId", 11}, {"recentItems", rows}}).toJson();
        backend.requestTaskChats();
    };
    fixture.holdNextReadResponse = u"tasks.task.list"_s;
    fixture.taskChatTitle = u"Старая версия"_s;
    requestTitles({42, 43});
    QTRY_VERIFY(fixture.holdNextReadResponse.isEmpty());
    fixture.taskChatTitle = u"Новая версия"_s;
    requestTitles({42});
    QTRY_COMPARE(fixture.requestsFor(u"tasks.task.list"_s).size(), 2);
    QTRY_COMPARE(loaded.last().first().value<QVector<Chat>>().value(0).title, fixture.taskChatTitle);

    // Session renewal signals that the backend has processed the delayed HTTP reply.
    QSignalSpy renewed(&backend, &CloudBackend::sessionUpdated);
    fixture.releaseHeldReadResponse("Set-Cookie: BITRIX_TEST=rotated; Path=/; HttpOnly\r\n");
    QTRY_COMPARE(renewed.size(), 1);
    loaded.clear();
    requestTitles({42});
    QTRY_VERIFY(!loaded.isEmpty());
    QCOMPARE(loaded.first().first().value<QVector<Chat>>().value(0).title, fixture.taskChatTitle);
    QCOMPARE(fixture.requestsFor(u"tasks.task.list"_s).size(), 2);
}

void TestCloudChats::loadsOlderHistoryWithScopedCursor()
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
    const auto olderReply = CloudTestAccess::pendingReplyForRoute(backend, action);
    QVERIFY(olderReply);
    fixture.releaseHeldReadResponse();
    QTRY_VERIFY(olderReply.isNull());
    QCOMPARE(loaded.size(), 2);
}

void TestCloudChats::refreshesTaskChatAfterProjectChange_data()
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

void TestCloudChats::refreshesTaskChatAfterProjectChange()
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
        QTRY_COMPARE(previewInvalidations.size(), 1);
        QCOMPARE(previewInvalidations.first().first().toString(), QStringLiteral("43"));
        const auto operation = taskCard ? operationKey(OperationKind::TaskMessages, taskId)
                                        : operationKey(OperationKind::Chat, dialog);
        CloudTestAccess::refreshNow(backend);
        QCOMPARE(CloudTestAccess::http(backend).contains(operation), inflight);
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

void TestCloudChats::retriesChatHistoryWithTheKnownChatId_data()
{
    QTest::addColumn<QByteArray>("status");
    QTest::addColumn<bool>("connectionClosed");
    QTest::newRow("http-500") << QByteArray("500 Internal Server Error") << false;
    QTest::newRow("ajax-error") << QByteArray("200 OK") << false;
    QTest::newRow("connection-closed") << QByteArray() << true;
}

void TestCloudChats::retriesChatHistoryWithTheKnownChatId()
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

void TestCloudChats::recoversLatestHistoryAfterATemporaryFailure_data()
{
    QTest::addColumn<QString>("mode");
    for (const auto *mode : {"snapshot", "fallback", "task", "exhausted", "switch", "reopen", "background", "account", "malformed"})
        QTest::newRow(mode) << QString::fromLatin1(mode);
}

void TestCloudChats::recoversLatestHistoryAfterATemporaryFailure()
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
    if (mode == QLatin1String("reopen")) {
        auto *original = CloudTestAccess::pendingReply(backend, operation);
        QVERIFY(original);
        connect(original, &QNetworkReply::finished, &backend, [&] {
            backend.clearOpenHistory(false);
            backend.requestChatMessages(dialog);
            // Process the queued refresh while the old retry still occupies this key.
            CloudTestAccess::refreshNow(backend);
        });
    }
    const auto &http = CloudTestAccess::http(backend);
    // Account changes clear the operation key before the scheduled retry callback runs.
    // Its subscriber lives until that callback releases the stale request.
    std::weak_ptr<int> retrySubscriber;
    if (changeAccount) {
        const auto lifetime = std::make_shared<int>();
        retrySubscriber = lifetime;
        CloudTestAccess::http(backend).ajax(snapshot,
            {{QStringLiteral("dialogId"), dialog}, {QStringLiteral("messageLimit"), QStringLiteral("25")}},
            operationKey(OperationKind::ChatHistoryRead, operation), [lifetime](QJsonValue) {}, operation);
    }
    if (changeAccount)
        QVERIFY(!retrySubscriber.expired());
    auto *original = CloudTestAccess::pendingReply(backend, operation);
    QVERIFY(original);
    QSignalSpy originalFinished(original, &QNetworkReply::finished);
    fixture.releaseHeldNotificationReply(mode != QLatin1String("malformed"));
    if (mode == QLatin1String("switch") || mode == QLatin1String("background") || changeAccount) {
        QTRY_COMPARE(originalFinished.size(), 1);
        QVERIFY(http.contains(operation));
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

    if (mode == QLatin1String("snapshot") || mode == QLatin1String("reopen") || fallback) {
        QTRY_COMPARE_WITH_TIMEOUT(history.size(), 1, 2000);
        QCOMPARE(history.first().first().toString(), task ? QStringLiteral("42") : dialog);
        QCOMPARE(fixture.requestsFor(snapshot).size(), 2);
        QCOMPARE(fixture.requestsFor(list).size(), fallback ? 2 : 0);
        QCOMPARE(historyErrors(), 0);
    } else if (mode == QLatin1String("exhausted") || mode == QLatin1String("malformed")) {
        QTRY_COMPARE_WITH_TIMEOUT(historyErrors(), 1, 2000);
        QVERIFY(!http.contains(operation));
        QCOMPARE(historyErrors(), 1);
        QCOMPARE(fixture.requestsFor(snapshot).size(), mode == QLatin1String("exhausted") ? 2 : 1);
        QVERIFY(history.isEmpty());
    } else {
        QTRY_VERIFY(!http.contains(operation));
        if (changeAccount)
            QTRY_VERIFY(retrySubscriber.expired());
        const auto snapshots = fixture.requestsFor(snapshot);
        QCOMPARE(std::count_if(snapshots.cbegin(), snapshots.cend(), [&](const auto &request) {
            return request.field(QStringLiteral("dialogId")) == dialog;
        }), 1);
    }
}

void TestCloudChats::retriesCoalescedHistoryRead_data()
{
    QTest::addColumn<bool>("retryFails");
    QTest::newRow("success") << false;
    QTest::newRow("exhausted") << true;
}

void TestCloudChats::retriesCoalescedHistoryRead()
{
    QFETCH(bool, retryFails);
    using namespace Qt::StringLiterals;
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy online(&backend, &Backend::connectionChanged);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    QSignalSpy failed(&backend, &Backend::failed);
    backend.start();
    QTRY_VERIFY(!online.isEmpty());

    const auto action = u"im.v2.Chat.load"_s;
    const auto dialog = u"999"_s;
    const auto key = u"chat:"_s + dialog;
    if (retryFails)
        fixture.chatResponseStatus = "503 Service Unavailable";
    fixture.holdNotificationReply = action;
    backend.requestChatMessages(dialog);
    QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());

    // Subscribe at the request layer. The public refresh API separately queues a newer snapshot.
    QList<QJsonValue> results;
    QStringList errors;
    const auto subscribe = [&] {
        CloudTestAccess::http(backend).ajax(action, {{u"dialogId"_s, dialog}, {u"messageLimit"_s, u"25"_s}},
                     u"chat-history-read:"_s + key, {
                         [&](QJsonValue data) { results.append(data); },
                         [&](const QString &error) { errors.append(error); }
                     }, key);
    };
    subscribe();
    auto *original = CloudTestAccess::pendingReply(backend, key);
    QVERIFY(original);
    // Qt runs this after the backend's finished handler has scheduled the retry.
    connect(original, &QNetworkReply::finished, &backend, subscribe);
    fixture.releaseHeldNotificationReply(true);
    if (retryFails) {
        QTRY_COMPARE(errors.size(), 2);
        QCOMPARE(errors.first(), errors.last());
        QCOMPARE(failed.size(), 1);
        QCOMPARE(failed.first().first().toString(), key);
        QCOMPARE(failed.first().last().toString(), errors.first());
        QVERIFY(results.isEmpty());
        QVERIFY(history.isEmpty());
    } else {
        QTRY_COMPARE(history.size(), 1);
        QCOMPARE(results.size(), 2);
        QCOMPARE(results.first(), results.last());
        QVERIFY(!results.first().toObject().value(u"messages"_s).toArray().isEmpty());
        QVERIFY(errors.isEmpty());
    }
    QCOMPARE(fixture.requestsFor(action).size(), 2);
}

void TestCloudChats::coalescesManualHistoryRefreshWhileLatestReadIsInFlight()
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
    CloudTestAccess::refreshNow(backend);
    QVERIFY(!CloudTestAccess::http(backend).contains(operationKey(OperationKind::Chat, dialog)));
    QCOMPARE(fixture.requestsFor(action).size(), 2);
}

void TestCloudChats::reconcilesUnreadFromScopedActiveHistory()
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
    CloudTestAccess::expireChatBadgeCooldown(backend);
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

void TestCloudChats::ignoresParticipantCountsInUnreadBadges()
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
    QVERIFY(!CloudTestAccess::http(backend).contains(operationKey(OperationKind::ReadVisibleMessages, second)));
    QCOMPARE(fixture.requestsFor(read).size(), 1);
}

void TestCloudChats::distinguishesPersonalDialogAndTaskWithSameId()
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

QTEST_GUILESS_MAIN(TestCloudChats)
#include "tst_cloud_chats.moc"
