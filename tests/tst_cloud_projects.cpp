#include "cloud_fixture.h"

#include <QJsonDocument>
#include <QSignalSpy>
#include <QTest>
#include <algorithm>

using namespace bx;
using namespace cloud_test;

class TestCloudProjects : public CloudTestCase {
    Q_OBJECT

private slots:
    void readsScopedProjectMembersAndDiskFiles();
    void pagesProjectTaskChatsAndTracksOnlyTheirConfirmedBadges();
    void aggregatesUnreadProjectTasksWithoutOpeningTheirChatList();
    void confirmedKanbanMoveSupersedesOutstandingBoardRead();
    void readsAndChangesStageColorWithoutRenaming();
    void findsProjectTasksOutsideInitialBoard();
    void loadsBoardColumnsLazilyWithoutSkippingFirstPage();
    void discardsBoardPagesFromPreviousSnapshots();
    void refusesToDeleteStageWithoutConfirmedEmptyTotal();
    void usesPortalPinActionsAndTheirAcknowledgements();
    void readsPortalTaskProjectsGridAndKeepsPinsAcrossPageLoads_data();
    void readsPortalTaskProjectsGridAndKeepsPinsAcrossPageLoads();
    void fallsBackToPaginatedProjectMembership();
    void keepsLegacyProjectScopeEvenWhenLaterPagesHaveRoles();
    void projectsStillLoadWhenOptionalIdentityIsUnavailable();
    void loadsProjectsDuringAnOptionalSettingsIdentityLookup();
};

void TestCloudProjects::loadsProjectsDuringAnOptionalSettingsIdentityLookup()
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

void TestCloudProjects::projectsStillLoadWhenOptionalIdentityIsUnavailable()
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

void TestCloudProjects::usesPortalPinActionsAndTheirAcknowledgements()
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

void TestCloudProjects::readsPortalTaskProjectsGridAndKeepsPinsAcrossPageLoads_data()
{
    QTest::addColumn<QString>("navigation");
    QTest::newRow("query") << QStringLiteral("classic");
    QTest::newRow("query-with-page-size") << QStringLiteral("size");
    QTest::newRow("sef-with-page-size") << QStringLiteral("sef");
    QTest::newRow("ambiguous-unmarked-next") << QStringLiteral("ambiguous");
}

void TestCloudProjects::readsPortalTaskProjectsGridAndKeepsPinsAcrossPageLoads()
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

void TestCloudProjects::fallsBackToPaginatedProjectMembership()
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

void TestCloudProjects::keepsLegacyProjectScopeEvenWhenLaterPagesHaveRoles()
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

void TestCloudProjects::refusesToDeleteStageWithoutConfirmedEmptyTotal()
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

void TestCloudProjects::loadsBoardColumnsLazilyWithoutSkippingFirstPage()
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

void TestCloudProjects::discardsBoardPagesFromPreviousSnapshots()
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

void TestCloudProjects::findsProjectTasksOutsideInitialBoard()
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

void TestCloudProjects::confirmedKanbanMoveSupersedesOutstandingBoardRead()
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

void TestCloudProjects::readsAndChangesStageColorWithoutRenaming()
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

void TestCloudProjects::readsScopedProjectMembersAndDiskFiles()
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

void TestCloudProjects::pagesProjectTaskChatsAndTracksOnlyTheirConfirmedBadges()
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

void TestCloudProjects::aggregatesUnreadProjectTasksWithoutOpeningTheirChatList()
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

QTEST_GUILESS_MAIN(TestCloudProjects)
#include "tst_cloud_projects.moc"
