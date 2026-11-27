#include "cloud_fixture.h"

#include <QSignalSpy>
#include <QTest>
#include <algorithm>

using namespace bx;
using namespace cloud_test;

class TestCloudAccount : public CloudTestCase {
    Q_OBJECT

private slots:
    void discardsConcurrentChatAndNoticeRepliesOnAccountChange();
    void ignoresNotificationRepliesFromPreviousAccount_data();
    void ignoresNotificationRepliesFromPreviousAccount();
    void recoversOnlyTheSameAccount();
    void discardsTaskProjectAndStageRepliesFromPreviousAccount_data();
    void discardsTaskProjectAndStageRepliesFromPreviousAccount();
    void ignoresTaskActionResponsesFromPreviousAccounts();
    void accountChangeDiscardsDeferredBoardTasksAndKnownProjectFolders();
    void discardsParallelTaskCardFromPreviousAccount();
    void discardsTaskAttachmentsFromPreviousAccount();
    void discardsCachedChatDataFromPreviousAccount();
    void discardsPendingProjectsAndPullCursorFromPreviousAccount();
    void settlesPendingSendOnAccountChange();
};

void TestCloudAccount::discardsConcurrentChatAndNoticeRepliesOnAccountChange()
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

void TestCloudAccount::discardsParallelTaskCardFromPreviousAccount()
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
    const auto route = QStringLiteral("tasks.task.get");
    fixture.holdNotificationReply = route;
    backend.requestTask(QStringLiteral("42"));
    QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());
    const auto previousReply = CloudTestAccess::pendingReplyForRoute(backend, route);
    QVERIFY(previousReply);
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
    QTRY_VERIFY(previousReply.isNull());
    QCOMPARE(cards.size(), 1);
    for (const auto &summary : summaries)
        QCOMPARE(summary.first().value<Task>().name, QStringLiteral("Current account task"));
}

void TestCloudAccount::discardsTaskAttachmentsFromPreviousAccount()
{
    CloudFixture fixture;
    fixture.ordinaryRecentBadgeFixture = R"({"userId":11,"recentItems":[],"chats":[]})";
    fixture.actionTaskLegacy = {{"id", 42}, {"ufTaskWebdavFiles", QJsonArray{7}}};
    fixture.actionTaskCurrent = {{"id", 42}};
    fixture.taskAttachmentRows = {QJsonObject{{"id", 7}, {"name", "old.txt"}}};
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy accounts(&backend, &Backend::accountScopeChanged);
    QSignalSpy cards(&backend, &Backend::taskLoaded);
    backend.start();
    backend.requestRecentChats();
    QTRY_COMPARE(accounts.size(), 1);

    const auto route = QStringLiteral("tasks.v2.File.listObjects");
    fixture.holdNotificationReply = route;
    backend.requestTask(QStringLiteral("42"));
    QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());
    const auto previousReply = CloudTestAccess::pendingReplyForRoute(backend, route);
    QVERIFY(previousReply);
    QCOMPARE(cards.size(), 1);
    fixture.ordinaryRecentBadgeFixture = R"({"userId":22,"recentItems":[],"chats":[]})";
    backend.requestRecentChats();
    QTRY_COMPARE(accounts.size(), 2);
    fixture.taskAttachmentRows = {QJsonObject{{"id", 7}, {"name", "current.txt"}}};
    backend.requestTask(QStringLiteral("42"));
    QTRY_COMPARE(cards.size(), 3);
    QCOMPARE(cards.last().first().value<Task>().attachments, QStringList{QStringLiteral("current.txt")});
    fixture.releaseHeldNotificationReply();
    QTRY_VERIFY(previousReply.isNull());
    QCOMPARE(cards.size(), 3);
}

void TestCloudAccount::discardsCachedChatDataFromPreviousAccount()
{
    CloudFixture fixture;
    fixture.ordinaryRecentBadgeFixture = R"({"userId":11,"recentItems":[],"chats":[]})";
    fixture.richRecentFixture = true;
    fixture.taskChatTitle = QStringLiteral("Account A task");
    fixture.chatResponseOverride = ajaxResult(R"({"chatId":91,"pins":[{"messageId":690}],
        "messages":[{"id":710,"chatId":91,"authorId":11,"text":"Reply","replyId":690}],
        "additionalMessages":[{"id":690,"chatId":91,"authorId":22,"text":"Account A message"}]})");
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy accounts(&backend, &Backend::accountScopeChanged);
    QSignalSpy tasks(&backend, &Backend::taskChatsLoaded);
    QSignalSpy pins(&backend, &Backend::pinnedMessagesLoaded);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    backend.setForeground(false);
    backend.start();
    backend.requestRecentChats();
    QTRY_COMPARE(accounts.size(), 1);
    backend.requestTaskChats();
    QTRY_VERIFY(!tasks.isEmpty()
                && tasks.last().first().value<QVector<Chat>>().value(0).title == fixture.taskChatTitle);
    const auto dialog = QStringLiteral("chat91");
    backend.requestPinnedMessages(dialog);
    backend.requestChatMessages(dialog);
    QTRY_COMPARE(history.size(), 1);
    QTRY_COMPARE(pins.size(), 1);
    QCOMPARE(history.last().at(1).value<QVector<Message>>().value(0).replyText, QStringLiteral("Account A message"));
    QCOMPARE(pins.last().at(1).value<QVector<Message>>().value(0).text, QStringLiteral("Account A message"));

    fixture.ordinaryRecentBadgeFixture = R"({"userId":22,"recentItems":[],"chats":[]})";
    fixture.taskChatTitle = QStringLiteral("Account B task");
    fixture.chatResponseOverride = ajaxResult(R"({"chatId":91,"pins":[],
        "messages":[{"id":710,"chatId":91,"authorId":22,"text":"Reply","replyId":690}]})");
    backend.requestRecentChats();
    QTRY_COMPARE(accounts.size(), 2);
    pins.clear();
    tasks.clear();
    history.clear();
    backend.requestPinnedMessages(dialog);
    backend.requestChatMessages(dialog);
    backend.requestTaskChats();
    QTRY_VERIFY(!pins.isEmpty());
    QTRY_VERIFY(!history.isEmpty());
    QTRY_VERIFY(!tasks.isEmpty());
    for (const auto &snapshot : pins)
        QVERIFY(snapshot.at(1).value<QVector<Message>>().isEmpty());
    const auto reply = history.first().at(1).value<QVector<Message>>().value(0);
    QCOMPARE(reply.replyId, QStringLiteral("690"));
    QVERIFY(reply.replyText.isEmpty());
    QTRY_COMPARE(tasks.last().first().value<QVector<Chat>>().value(0).title, fixture.taskChatTitle);
    for (const auto &snapshot : tasks)
        QVERIFY(snapshot.first().value<QVector<Chat>>().value(0).title != QLatin1String("Account A task"));
}

void TestCloudAccount::discardsPendingProjectsAndPullCursorFromPreviousAccount()
{
    CloudFixture fixture;
    fixture.ordinaryRecentBadgeFixture = R"({"userId":11,"recentItems":[],"chats":[]})";
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy accounts(&backend, &Backend::accountScopeChanged);
    QSignalSpy projects(&backend, &Backend::projectsLoaded);
    QSignalSpy feed(&backend, &Backend::notificationsLoaded);
    backend.setForeground(false);
    backend.start();
    backend.requestRecentChats();
    QTRY_COMPARE(accounts.size(), 1);
    QTRY_COMPARE(fixture.socketRequests.size(), 1);
    fixture.pushIm(QStringLiteral("account-a-cursor"), QStringLiteral("notifyAdd"),
                   {{"id", 900}, {"text", "Account A notification"}});
    QTRY_COMPARE(feed.size(), 1);
    fixture.holdNotificationReply = QStringLiteral("socialnetwork.api.workgroup.list");
    backend.requestProjects();
    QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());

    fixture.currentUserId = 22;
    fixture.ordinaryRecentBadgeFixture = R"({"userId":22,"recentItems":[],"chats":[]})";
    fixture.projectFallbackFixture = true;
    backend.requestRecentChats();
    QTRY_COMPARE(accounts.size(), 2);
    fixture.releaseHeldNotificationReply();
    backend.requestProjects();
    QTRY_COMPARE(projects.size(), 1);
    QCOMPARE(projects.first().first().value<QVector<Project>>().value(0).name, QStringLiteral("Мой проект"));
    QTRY_COMPARE(fixture.socketRequests.size(), 2);
    QCOMPARE(fixture.requestsFor(pushPath).size(), 2);
    const QUrlQuery subscription(fixture.socketRequests.last().url());
    QVERIFY(!subscription.hasQueryItem(QStringLiteral("mid")));
    QVERIFY(!subscription.hasQueryItem(QStringLiteral("tag")));
    QVERIFY(!subscription.hasQueryItem(QStringLiteral("time")));
    QCOMPARE(projects.size(), 1);
}

void TestCloudAccount::settlesPendingSendOnAccountChange()
{
    CloudFixture fixture;
    fixture.ordinaryRecentBadgeFixture = R"({"userId":11,"recentItems":[],"chats":[]})";
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy accounts(&backend, &Backend::accountScopeChanged);
    QSignalSpy sent(&backend, &Backend::messageSent);
    QSignalSpy errors(&backend, &Backend::failed);
    const auto sendFailures = [&errors] {
        return std::count_if(errors.cbegin(), errors.cend(), [](const QList<QVariant> &row) {
            return row.first().toString() == QLatin1String("send:chat:chat91");
        });
    };
    backend.setForeground(false);
    backend.start();
    backend.requestRecentChats();
    QTRY_COMPARE(accounts.size(), 1);
    const auto action = QStringLiteral("im.v2.Chat.Message.send");
    const auto dialog = QStringLiteral("chat91");
    fixture.chatResponseOverride = ajaxResult(R"({"chatId":91,"pins":[{"messageId":690}],
        "additionalMessages":[{"id":690,"chatId":91,"text":"Account A pin"}]})");
    QSignalSpy pins(&backend, &Backend::pinnedMessagesLoaded);
    backend.requestPinnedMessages(dialog);
    QTRY_COMPARE(pins.size(), 1);
    QCOMPARE(pins.first().at(1).value<QVector<Message>>().value(0).text, QStringLiteral("Account A pin"));

    bool replayedPinsOnFailure = false;
    connect(&backend, &Backend::failed, &pins, [&](const QString &operation, const QString &) {
        if (operation == QLatin1String("send:chat:chat91")) {
            const auto before = pins.size();
            backend.requestPinnedMessages(dialog);
            replayedPinsOnFailure = pins.size() != before && !pins.last().at(1).value<QVector<Message>>().isEmpty();
        }
    });
    fixture.holdNotificationReply = action;
    backend.sendChatMessage(dialog, QStringLiteral("Account A message"));
    QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());

    fixture.chatResponseOverride = ajaxResult(R"({"chatId":91,"pins":[]})");
    fixture.currentUserId = 22;
    fixture.ordinaryRecentBadgeFixture = R"({"userId":22,"recentItems":[],"chats":[]})";
    backend.requestRecentChats();
    QTRY_COMPARE(accounts.size(), 2);
    QTRY_COMPARE(sendFailures(), 1);
    QVERIFY(!replayedPinsOnFailure);
    QVERIFY(sent.isEmpty());
    backend.sendChatMessage(dialog, QStringLiteral("Account B message"));
    QTRY_COMPARE(sent.size(), 1);
    QCOMPARE(sent.first().first().toString(), QStringLiteral("chat:chat91"));
    QCOMPARE(fixture.requestsFor(action).size(), 2);
    QCOMPARE(fixture.requestsFor(action).last().field(QStringLiteral("fields[message]")), QStringLiteral("Account B message"));

    fixture.releaseHeldNotificationReply();
    QSignalSpy recent(&backend, &Backend::recentChatsLoaded);
    backend.requestRecentChats();
    QTRY_VERIFY(!recent.isEmpty());
    QCOMPARE(sent.size(), 1);
    QCOMPARE(sendFailures(), 1);
}

void TestCloudAccount::accountChangeDiscardsDeferredBoardTasksAndKnownProjectFolders()
{
    using namespace Qt::StringLiterals;
    CloudFixture fixture;
    fixture.boardPagingFixture = true;
    fixture.ordinaryRecentBadgeFixture = R"({"userId":11,"recentItems":[],"chats":[]})";
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy accounts(&backend, &Backend::accountScopeChanged);
    QSignalSpy boards(&backend, &Backend::boardLoaded);
    QSignalSpy pages(&backend, &Backend::boardTasksPageLoaded);
    QSignalSpy files(&backend, &Backend::projectFilesLoaded);
    QSignalSpy failures(&backend, &Backend::failed);
    backend.setForeground(false);
    backend.start();
    backend.requestRecentChats();
    QTRY_COMPARE(accounts.size(), 1);

    const auto project = u"17"_s;
    const auto stage = u"60"_s;
    const auto folder = u"77"_s;
    backend.requestProjectBoard(project);
    backend.requestProjectFiles(project, {}, 0);
    QTRY_COMPARE(boards.size(), 1);
    QTRY_COMPARE(files.size(), 1);
    backend.requestProjectFiles(project, folder, 0);
    QTRY_COMPARE(files.size(), 2);

    fixture.currentUserId = 22;
    fixture.ordinaryRecentBadgeFixture = R"({"userId":22,"recentItems":[],"chats":[]})";
    backend.requestRecentChats();
    QTRY_COMPARE(accounts.size(), 2);
    failures.clear();
    backend.requestMoreBoardTasks(project, stage);
    backend.requestProjectFiles(project, folder, 0);
    QCOMPARE(failures.size(), 2);
    QCOMPARE(failures.at(0).first().toString(), operationKey(OperationKind::BoardColumn, project, stage));
    QCOMPARE(failures.at(1).first().toString(), operationKey(OperationKind::ProjectFiles, project, folder));
    QVERIFY(pages.isEmpty());
    QCOMPARE(files.size(), 2);

    backend.requestProjectBoard(project);
    backend.requestProjectFiles(project, {}, 0);
    QTRY_COMPARE(boards.size(), 2);
    QTRY_COMPARE(files.size(), 3);
    backend.requestMoreBoardTasks(project, stage);
    backend.requestProjectFiles(project, folder, 0);
    QTRY_COMPARE(pages.size(), 1);
    QTRY_COMPARE(files.size(), 4);
    QCOMPARE(failures.size(), 2);
}

void TestCloudAccount::discardsTaskProjectAndStageRepliesFromPreviousAccount_data()
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

void TestCloudAccount::discardsTaskProjectAndStageRepliesFromPreviousAccount()
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
    const auto heldRoute = holdWrite ? write : preflight;
    fixture.holdNotificationReply = heldRoute;
    backend.updateTask(taskId, edit);
    QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());
    const auto previousReply = CloudTestAccess::pendingReplyForRoute(backend, heldRoute);
    QVERIFY(previousReply);
    backend.updateTask(taskId, edit);

    fixture.ordinaryRecentBadgeFixture = R"({"userId":22,"recentItems":[],"chats":[]})";
    backend.requestRecentChats();
    QTRY_COMPARE(accounts.size(), 2);
    QCOMPARE(fixture.requestsFor(preflight).size(), 1);
    fixture.releaseHeldNotificationReply(failReply);
    QTRY_VERIFY(previousReply.isNull());
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

void TestCloudAccount::ignoresTaskActionResponsesFromPreviousAccounts()
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
    const auto preflight = QStringLiteral("tasks.v2.Task.get");
    fixture.holdNotificationReply = preflight;
    backend.deleteTask(taskId);
    QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());
    const auto previousPreflight = CloudTestAccess::pendingReplyForRoute(backend, preflight);
    QVERIFY(previousPreflight);
    fixture.ordinaryRecentBadgeFixture = R"({"userId":22,"recentItems":[],"chats":[]})";
    backend.requestRecentChats();
    QTRY_COMPARE(accounts.size(), 2);
    fixture.releaseHeldNotificationReply();
    QTRY_VERIFY(previousPreflight.isNull());
    QVERIFY(fixture.requestsFor(deletePath).isEmpty());

    fixture.holdNotificationReply = deletePath;
    backend.deleteTask(taskId);
    QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());
    QCOMPARE(fixture.requestsFor(deletePath).size(), 1);
    const auto previousDelete = CloudTestAccess::pendingReplyForRoute(backend, deletePath);
    QVERIFY(previousDelete);
    fixture.ordinaryRecentBadgeFixture = R"({"userId":11,"recentItems":[],"chats":[]})";
    backend.requestRecentChats();
    QTRY_COMPARE(accounts.size(), 3);
    backend.deleteTask(taskId);
    QTRY_COMPARE(deleted.size(), 1);
    QCOMPARE(fixture.requestsFor(deletePath).size(), 2);
    fixture.releaseHeldNotificationReply();
    QTRY_VERIFY(previousDelete.isNull());
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
        fixture.holdNotificationReply = preflight;
        backend.updateTask(taskId, edit);
        QTRY_VERIFY(fixture.holdNotificationReply.isEmpty());
        const auto previousAction = CloudTestAccess::pendingReplyForRoute(backend, preflight);
        QVERIFY(previousAction);
        const auto accountCount = accounts.size();
        fixture.ordinaryRecentBadgeFixture = action == statusAction
            ? R"({"userId":22,"recentItems":[],"chats":[]})"
            : R"({"userId":11,"recentItems":[],"chats":[]})";
        backend.requestRecentChats();
        QTRY_COMPARE(accounts.size(), accountCount + 1);
        fixture.releaseHeldNotificationReply();
        QTRY_VERIFY(previousAction.isNull());
        QVERIFY(fixture.requestsFor(action).isEmpty());
    }
    QVERIFY(errors.isEmpty());
    QVERIFY(updated.isEmpty());
}

void TestCloudAccount::ignoresNotificationRepliesFromPreviousAccount_data()
{
    QTest::addColumn<QString>("action");
    QTest::addColumn<bool>("rejectOldBatch");
    QTest::newRow("history") << QStringLiteral("im.notify.get") << false;
    QTest::newRow("search") << QStringLiteral("im.notify.history.search") << false;
    QTest::newRow("read-success") << QStringLiteral("im.notify.read.list") << false;
    QTest::newRow("read-failure") << QStringLiteral("im.notify.read.list") << true;
}

void TestCloudAccount::ignoresNotificationRepliesFromPreviousAccount()
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
    const auto previousReply = CloudTestAccess::pendingReplyForRoute(backend, batchPath);
    QVERIFY(previousReply);
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
    QTRY_VERIFY(previousReply.isNull());
    QCOMPARE(outcome.size(), 1);
    QCOMPARE(errors.size(), 0);
    QCOMPARE(read.size(), action == QLatin1String("im.notify.read.list") ? 1 : 0);
}

void TestCloudAccount::recoversOnlyTheSameAccount()
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

QTEST_GUILESS_MAIN(TestCloudAccount)
#include "tst_cloud_account.moc"
