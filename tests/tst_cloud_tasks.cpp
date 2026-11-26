#include "cloud_fixture.h"

#include <QJsonDocument>
#include <QSignalSpy>
#include <QTest>
#include <QUuid>
#include <algorithm>

using namespace bx;
using namespace cloud_test;

class TestCloudTasks : public CloudTestCase {
    Q_OBJECT

private slots:
    void searchesPeopleWithinThePortalScope();
    void loadsInitialTaskPeopleInPortalRecentOrder();
    void loadsProjectTaskAndTaskChat();
    void readsAndWritesTaskChecklistAndChildRelations();
    void readsScopedGanttLinksAndExactPlanDates();
    void readsTaskHistoryAndKeepsDenialDistinctFromEmpty();
    void sendsTaskEditsAndKanbanMutations();
    void loadsTaskRequisitesAndUsesVerifiedWriteActions();
    void loadsExactTaskRatingAndExplicitActionRights();
    void mutatesTaskRatingAndDeletionOnlyAfterExactPortalConfirmation();
    void loadsObserverAvatarsWithoutIndividualProfileRequests();
    void loadsObserverAvatarsWithoutIndividualProfileRequests_data();
    void preservesRawTaskDescriptionAndRejectsAStaleEdit();
    void overlappingTaskSearches();
    void resolvesCurrentCreationOwnerWithoutRecentIdentity();
    void createsTasksWithSelectedRequisites();
    void readsTaskCardOpenedOutsideProjects();
    void loadsAuthorizedTaskSummaryWhileDetailsArePending();
    void taskCardRequiresTheMatchingLegacyResponse_data();
    void taskCardRequiresTheMatchingLegacyResponse();
    void createsTaskWithOneNativeShareAndReloadsTheSource();
};

void TestCloudTasks::readsTaskCardOpenedOutsideProjects()
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

void TestCloudTasks::loadsAuthorizedTaskSummaryWhileDetailsArePending()
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

void TestCloudTasks::taskCardRequiresTheMatchingLegacyResponse_data()
{
    QTest::addColumn<int>("legacyId");
    QTest::addColumn<int>("detailsId");
    QTest::newRow("parallel exact responses") << 42 << 42;
    QTest::newRow("legacy read denied") << 0 << 42;
    QTest::newRow("legacy id does not match") << 43 << 42;
    QTest::newRow("optional details denied") << 42 << 0;
    QTest::newRow("optional details id does not match") << 42 << 43;
}

void TestCloudTasks::taskCardRequiresTheMatchingLegacyResponse()
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

void TestCloudTasks::createsTaskWithOneNativeShareAndReloadsTheSource()
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

void TestCloudTasks::loadsInitialTaskPeopleInPortalRecentOrder()
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
    backend.searchPeople(u"initial-task-people-denied"_s, {});
    QTRY_VERIFY(!failed.isEmpty());
    QCOMPARE(failed.size(), 1);
    QCOMPARE(failed.first().first().toString(), u"search-people:initial-task-people-denied"_s);
    QCOMPARE(results.size(), 2);
}

void TestCloudTasks::searchesPeopleWithinThePortalScope()
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

void TestCloudTasks::resolvesCurrentCreationOwnerWithoutRecentIdentity()
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

void TestCloudTasks::createsTasksWithSelectedRequisites()
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

void TestCloudTasks::overlappingTaskSearches()
{
    using namespace Qt::StringLiterals;
    CloudFixture fixture;
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy found(&backend, &Backend::projectTasksFound);
    backend.start();
    backend.searchProjectTasks(u"17"_s, u"#149"_s);
    backend.searchProjectTasks(u"17"_s, u"#149"_s);
    backend.searchProjectTasks(u"17"_s, u"#150"_s);

    QTRY_COMPARE(found.size(), 3);
    QSet<QString> queries;
    for (const auto &entry : found) {
        const auto query = entry.at(1).toString();
        queries.insert(query);
        const auto tasks = entry.at(2).value<QVector<Task>>();
        if (query == u"#150") {
            QVERIFY(tasks.isEmpty());
        } else {
            QCOMPARE(tasks.size(), 1);
            QCOMPARE(tasks.first().id, u"149"_s);
        }
    }
    QCOMPARE(queries, (QSet<QString>{u"#149"_s, u"#150"_s}));
    const auto requests = fixture.requestsFor(u"tasks.task.list"_s);
    QCOMPARE(requests.size(), 2);
    QSet<QString> ids;
    for (const auto &request : requests)
        ids.insert(request.field(u"filter[ID]"_s));
    QCOMPARE(ids, (QSet<QString>{u"149"_s, u"150"_s}));
}

void TestCloudTasks::loadsObserverAvatarsWithoutIndividualProfileRequests_data()
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

void TestCloudTasks::loadsObserverAvatarsWithoutIndividualProfileRequests()
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

void TestCloudTasks::loadsTaskRequisitesAndUsesVerifiedWriteActions()
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

void TestCloudTasks::loadsExactTaskRatingAndExplicitActionRights()
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

void TestCloudTasks::mutatesTaskRatingAndDeletionOnlyAfterExactPortalConfirmation()
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

void TestCloudTasks::sendsTaskEditsAndKanbanMutations()
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

void TestCloudTasks::preservesRawTaskDescriptionAndRejectsAStaleEdit()
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

void TestCloudTasks::readsAndWritesTaskChecklistAndChildRelations()
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

void TestCloudTasks::readsScopedGanttLinksAndExactPlanDates()
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

void TestCloudTasks::readsTaskHistoryAndKeepsDenialDistinctFromEmpty()
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

void TestCloudTasks::loadsProjectTaskAndTaskChat()
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

QTEST_GUILESS_MAIN(TestCloudTasks)
#include "tst_cloud_tasks.moc"
