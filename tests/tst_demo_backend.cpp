#include "core/demo_backend.h"

#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QTest>

using namespace bx;

class TestDemoBackend : public QObject {
    Q_OBJECT
private slots:
    void chatsLoaded();
    void messagesLoaded();
    void projectsAndBoard();
    void timelineShowsSameTasksAsBoard();
    void taskAndTaskMessages();
    void notificationsLoaded();
    void sendMessageReportsId();
    void sentMessageAppearsInTheChatAndItsPreview();
    void emptyMessageRejected();
    void editsTasksAndStagesInTheirProject();
    void createsTasksAndSubtasksWithSelectedRequisites();
    void searchesMessagesAndKeepsAttachments();
};

void TestDemoBackend::chatsLoaded() {
    DemoBackend backend;
    QSignalSpy spy(&backend, &Backend::recentChatsLoaded);
    backend.requestRecentChats();
    QTRY_VERIFY(spy.count() > 0);
    const auto chats = spy.first().at(0).value<QVector<Chat>>();
    QVERIFY(!chats.isEmpty());
    QCOMPARE(chats.first().title, QStringLiteral("Отдел продаж"));
}

void TestDemoBackend::messagesLoaded() {
    DemoBackend backend;
    QSignalSpy spy(&backend, &Backend::chatMessagesLoaded);
    backend.requestChatMessages(QStringLiteral("c-1"));
    QTRY_VERIFY(spy.count() > 0);
    const auto messages = spy.first().at(1).value<QVector<Message>>();
    QCOMPARE(messages.size(), 3);
    QVERIFY(!messages.first().id.isEmpty());
    QVERIFY(messages.at(1).outgoing);
}

void TestDemoBackend::projectsAndBoard() {
    DemoBackend backend;
    QSignalSpy projectsSpy(&backend, &Backend::projectsLoaded);
    QSignalSpy boardSpy(&backend, &Backend::boardLoaded);
    backend.requestProjects();
    backend.requestProjectBoard(QStringLiteral("p-1"));
    QTRY_VERIFY(projectsSpy.count() > 0);
    QTRY_VERIFY(boardSpy.count() > 0);
    const auto projects = projectsSpy.first().at(0).value<QVector<Project>>();
    QCOMPARE(projects.size(), 2);
    const auto stages = boardSpy.first().at(1).value<QVector<Stage>>();
    QCOMPARE(stages.size(), 3);
    QVERIFY(!stages.first().tasks.isEmpty());
}

void TestDemoBackend::timelineShowsSameTasksAsBoard()
{
    DemoBackend backend;
    QSignalSpy board(&backend, &Backend::boardLoaded);
    QSignalSpy timeline(&backend, &Backend::projectTimelineLoaded);
    const auto project = QStringLiteral("p-1");
    backend.requestProjectBoard(project);
    backend.requestProjectTimeline(project);
    QTRY_COMPARE(board.size(), 1);
    QTRY_COMPARE(timeline.size(), 1);
    QCOMPARE(timeline.first().first().toString(), project);
    QCOMPARE(timeline.first().at(2).toBool(), false);
    QStringList expected;
    for (const auto &stage : board.first().at(1).value<QVector<Stage>>())
        for (const auto &task : stage.tasks)
            expected.append(task.id);
    QVERIFY(!expected.isEmpty());
    QStringList actual;
    for (const auto &task : timeline.first().at(1).value<QVector<Task>>())
        actual.append(task.id);
    QCOMPARE(actual, expected);
}

void TestDemoBackend::taskAndTaskMessages() {
    DemoBackend backend;
    QSignalSpy taskSpy(&backend, &Backend::taskLoaded);
    QSignalSpy msgSpy(&backend, &Backend::taskMessagesLoaded);
    backend.requestTask(QStringLiteral("t-2"));
    backend.requestTaskMessages(QStringLiteral("t-2"));
    QTRY_VERIFY(taskSpy.count() > 0);
    QTRY_VERIFY(msgSpy.count() > 0);
    const Task task = taskSpy.first().at(0).value<Task>();
    QCOMPARE(task.id, QStringLiteral("t-2"));
    QCOMPARE(task.stageName, QStringLiteral("В работе"));
    QCOMPARE(msgSpy.first().at(1).value<QVector<Message>>().size(), 2);
}

void TestDemoBackend::notificationsLoaded() {
    DemoBackend backend;
    QSignalSpy spy(&backend, &Backend::notificationsLoaded);
    backend.requestNotifications();
    QTRY_VERIFY(spy.count() > 0);
    const auto notifications = spy.first().at(0).value<QVector<Notification>>();
    QCOMPARE(notifications.size(), 2);
    QVERIFY(!notifications.first().read);
}

void TestDemoBackend::sendMessageReportsId() {
    DemoBackend backend;
    QSignalSpy spy(&backend, &Backend::messageSent);
    backend.sendChatMessage(QStringLiteral("c-1"), QStringLiteral("Здравствуйте"));
    QTRY_VERIFY(spy.count() > 0);
    QCOMPARE(spy.first().first().toString(), QStringLiteral("chat:c-1"));
    QVERIFY(!spy.first().at(1).toString().isEmpty());
}

void TestDemoBackend::sentMessageAppearsInTheChatAndItsPreview()
{
    DemoBackend backend;
    QSignalSpy chats(&backend, &Backend::chatMessagesLoaded);
    QSignalSpy tasks(&backend, &Backend::taskMessagesLoaded);
    QSignalSpy previews(&backend, &Backend::recentChatsLoaded);
    QSignalSpy taskPreviews(&backend, &Backend::taskChatsLoaded);
    const auto latest = QStringLiteral("Новое сообщение");
    backend.sendChatMessage(QStringLiteral("c-1"), latest);
    backend.sendTaskMessage(QStringLiteral("t-1"), latest);
    QTRY_COMPARE(chats.size(), 2);
    QTRY_COMPARE(tasks.size(), 1);
    QTRY_COMPARE(previews.size(), 1);
    QTRY_COMPARE(taskPreviews.size(), 1);
    QCOMPARE(chats.first().at(1).value<QVector<Message>>().constLast().text, latest);
    QCOMPARE(chats.last().at(1).value<QVector<Message>>().constLast().text, latest);
    QCOMPARE(tasks.first().at(1).value<QVector<Message>>().constLast().text, latest);
    QCOMPARE(previews.first().first().value<QVector<Chat>>().first().lastMessage, latest);
    QCOMPARE(taskPreviews.first().first().value<QVector<Chat>>().first().lastMessage, latest);
}

void TestDemoBackend::emptyMessageRejected() {
    DemoBackend backend;
    QSignalSpy spy(&backend, &Backend::failed);
    backend.sendTaskMessage(QStringLiteral("t-2"), QStringLiteral("   "));
    QTRY_VERIFY(spy.count() > 0);
    QCOMPARE(spy.first().at(0).toString(), QStringLiteral("send:task:t-2"));
}

void TestDemoBackend::editsTasksAndStagesInTheirProject()
{
    DemoBackend backend;
    QSignalSpy projectCreated(&backend, &Backend::projectCreated);
    QSignalSpy taskCreated(&backend, &Backend::taskCreated);
    QSignalSpy taskUpdated(&backend, &Backend::taskUpdated);
    QSignalSpy boardUpdated(&backend, &Backend::boardUpdated);
    QSignalSpy boardLoaded(&backend, &Backend::boardLoaded);
    QSignalSpy failed(&backend, &Backend::failed);
    backend.createProject(QStringLiteral("Проверка"));
    QTRY_COMPARE(projectCreated.count(), 1);
    const auto project = projectCreated.first().first().toString();
    backend.saveStage(project, {}, QStringLiteral("Первая"), QStringLiteral("0"), QStringLiteral("#00ADF2"));
    QTRY_COMPARE(boardUpdated.count(), 1);
    backend.requestProjectBoard(project);
    QTRY_COMPARE(boardLoaded.count(), 1);
    const auto initial = boardLoaded.first().at(1).value<QVector<Stage>>().first();
    const auto firstStage = initial.id;
    QCOMPARE(initial.color, QStringLiteral("00adf2"));
    backend.saveStage(project, firstStage, {}, {}, QStringLiteral("47d1e2"));
    QTRY_COMPARE(boardUpdated.count(), 2);
    backend.saveStage(project, firstStage, QStringLiteral("Первая стадия"), {});
    QTRY_COMPARE(boardUpdated.count(), 3);

    TaskEdit draft;
    draft.title = QStringLiteral("Проверить доску");
    draft.description = QStringLiteral("Проверка");
    backend.createTask(project, draft);
    QTRY_COMPARE(taskCreated.count(), 1);
    const auto task = taskCreated.first().at(1).toString();
    TaskEdit edit;
    edit.description = QString();
    edit.highPriority = true;
    backend.updateTask(task, edit);
    QTRY_COMPARE(taskUpdated.count(), 1);
    backend.requestProjectBoard(project);
    QTRY_COMPARE(boardLoaded.count(), 2);
    const auto stages = boardLoaded.last().at(1).value<QVector<Stage>>();
    QCOMPARE(stages.first().id, firstStage);
    QCOMPARE(stages.first().color, QStringLiteral("47d1e2"));
    QCOMPARE(stages.first().name, QStringLiteral("Первая стадия"));
    QCOMPARE(stages.first().tasks.first().name, *draft.title);
    QVERIFY(stages.first().tasks.first().description.isEmpty());
    QCOMPARE(stages.first().tasks.first().priority, QStringLiteral("Высокий"));
    backend.deleteStage(project, firstStage);
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(failed.first().first().toString(), QStringLiteral("board-edit:") + project);
}

void TestDemoBackend::createsTasksAndSubtasksWithSelectedRequisites()
{
    DemoBackend backend;
    QSignalSpy created(&backend, &Backend::taskCreated);
    QSignalSpy child(&backend, &Backend::subtaskCreated);
    QSignalSpy children(&backend, &Backend::subtasksLoaded);
    QSignalSpy loaded(&backend, &Backend::taskLoaded);
    const auto project = QStringLiteral("p-2");

    TaskEdit draft;
    draft.title = QStringLiteral("Заполненная задача");
    draft.creatorId = QStringLiteral("3");
    draft.assigneeId = QStringLiteral("2");
    draft.observerIds = QStringList{QStringLiteral("1")};
    draft.stageId = QStringLiteral("s-2");
    backend.createTask(project, draft);
    QTRY_COMPARE(created.count(), 1);
    const auto parentId = created.first().at(1).toString();
    backend.requestTask(parentId);
    QTRY_COMPARE(loaded.count(), 1);
    const auto task = loaded.first().first().value<Task>();
    QCOMPARE(task.projectId, project);
    QCOMPARE(task.stageId, *draft.stageId);
    QCOMPARE(task.creatorId, *draft.creatorId);
    QCOMPARE(task.createdBy, QStringLiteral("Тимлид"));
    QCOMPARE(task.assigneeId, *draft.assigneeId);
    QCOMPARE(task.assignee, QStringLiteral("Ирина Соколова"));
    QCOMPARE(task.observers, QStringList{QStringLiteral("Алексей Морозов")});
    QCOMPARE(task.observerPeople.first().id, draft.observerIds->first());

    draft.title = QStringLiteral("Заполненная подзадача");
    draft.stageId = QStringLiteral("s-3");
    backend.createSubtask(parentId, project, draft);
    QTRY_COMPARE(child.count(), 1);
    QCOMPARE(child.first().at(0).toString(), parentId);
    QCOMPARE(child.first().at(1).toString(), project);
    backend.requestSubtasks(parentId);
    QTRY_COMPARE(children.count(), 1);
    const auto subtasks = children.first().at(1).value<QVector<Task>>();
    QCOMPARE(subtasks.size(), 1);
    QCOMPARE(subtasks.first().id, child.first().at(2).toString());
    QCOMPARE(subtasks.first().projectId, project);
    QCOMPARE(subtasks.first().stageId, *draft.stageId);
    QCOMPARE(subtasks.first().creatorId, *draft.creatorId);
    QCOMPARE(subtasks.first().assigneeId, *draft.assigneeId);
    QCOMPARE(subtasks.first().observers, task.observers);

    draft.stageId.reset();
    backend.createSubtask(parentId, {}, draft);
    QTRY_COMPARE(child.count(), 2);
    QVERIFY(child.last().at(1).toString().isEmpty());
    backend.requestTask(child.last().at(2).toString());
    QTRY_COMPARE(loaded.count(), 2);
    const auto detached = loaded.last().first().value<Task>();
    QVERIFY(detached.projectId.isEmpty());
    QVERIFY(detached.stageId.isEmpty());
}

void TestDemoBackend::searchesMessagesAndKeepsAttachments()
{
    DemoBackend backend;
    const QString scope = QStringLiteral("t-2");
    QTemporaryFile attachment;
    QVERIFY(attachment.open());
    QSignalSpy uploaded(&backend, &Backend::fileUploaded);
    QSignalSpy search(&backend, &Backend::messagesFound);
    QSignalSpy loaded(&backend, &Backend::taskLoaded);
    backend.attachTaskFile(scope, attachment.fileName());
    backend.uploadChatFile(scope, attachment.fileName(), true);
    QTRY_COMPARE(uploaded.count(), 2);
    QCOMPARE(uploaded.first().at(1).toString(), QStringLiteral("task"));
    QCOMPARE(uploaded.last().at(1).toString(), QStringLiteral("task-chat"));
    backend.requestTask(scope);
    backend.searchMessages(scope, QFileInfo(attachment.fileName()).fileName(), true);
    QTRY_COMPARE(loaded.count(), 1);
    QTRY_COMPARE(search.count(), 1);
    const auto files = loaded.first().first().value<Task>().attachments;
    QCOMPARE(files, QStringList{QFileInfo(attachment.fileName()).fileName()});
    const auto messages = search.first().at(3).value<QVector<Message>>();
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().attachments, files);
}

QTEST_GUILESS_MAIN(TestDemoBackend)
#include "tst_demo_backend.moc"
