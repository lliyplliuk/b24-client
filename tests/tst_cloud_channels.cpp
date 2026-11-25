#include "cloud/cloud_backend.h"
#include "core/notification_targets.h"
#include "core/tunables.h"

#include <QDir>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QTest>
#include <QUrlQuery>
#include <QUuid>
#include <algorithm>
#include <functional>
#include <utility>

using namespace bx;

namespace {

const QString loadAction = QStringLiteral("im.v2.Chat.load");
const QString uploadAction = QStringLiteral("ui.fileuploader.upload");
const QString attachAction = QStringLiteral("tasks.v2.File.attach");
const QString searchAction = QStringLiteral("ui.entityselector.doSearch");
const QString searchPath = QStringLiteral("/rest/im.dialog.messages.search.json");
const QString folderPath = QStringLiteral("/rest/im.disk.folder.get.json");
const QString commitPath = QStringLiteral("/rest/im.disk.file.commit.json");

struct Request {
    QUrl url;
    QByteArray body;
    QHash<QByteArray, QByteArray> headers;

    QString action() const { return QUrlQuery(url).queryItemValue(QStringLiteral("action")); }
    QString parameter(const QString &name) const
    {
        return QUrlQuery(url).queryItemValue(name, QUrl::FullyDecoded);
    }
    QString field(const QString &name) const
    {
        const auto fields = QUrlQuery(QString::fromUtf8(body)).queryItems(QUrl::FullyDecoded);
        for (const auto &[key, value] : fields) {
            if (key == name)
                return value;
        }
        return {};
    }
};

struct Response {
    QByteArray body;
    QByteArray status = "200 OK";
    QByteArray extraHeaders;
    QByteArray contentType = "application/json";
    qint64 advertisedLength = -1;
    bool holdOpen = false;
    bool deferred = false;
};
Response ajax(const QByteArray &body) { return {"{\"status\":\"success\",\"data\":" + body + "}", "200 OK", {}}; }
Response result(const QByteArray &body) { return {"{\"result\":" + body + "}", "200 OK", {}}; }

class Fixture {
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
                    if (end == -1)
                        return;
                    const auto header = pending.left(end).split('\n');
                    Request request;
                    request.url = QUrl::fromEncoded(header.first().trimmed().split(' ').value(1));
                    for (const auto &line : header.mid(1)) {
                        const auto colon = line.indexOf(':');
                        if (colon > 0)
                            request.headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
                    }
                    const auto length = request.headers.value("content-length").toLongLong();
                    if (pending.size() - end - 4 < length)
                        return;
                    request.body = pending.mid(end + 4, length);
                    requests.append(request);
                    Response response = responder ? responder(request) : Response{};
                    if (request.url.path() == QLatin1String("/rest/pull.config.get.json"))
                        response = result("{}");
                    const auto wire = "HTTP/1.1 " + response.status + "\r\n" + response.extraHeaders
                        + "Content-Type: " + response.contentType + "\r\nConnection: close\r\nContent-Length: "
                        + QByteArray::number(response.advertisedLength < 0 ? response.body.size() : response.advertisedLength)
                        + "\r\n\r\n" + response.body;
                    if (response.deferred) {
                        m_deferredSocket = socket;
                        m_deferredResponse = wire;
                        return;
                    }
                    socket->write(wire);
                    if (!response.holdOpen)
                        socket->disconnectFromHost();
                });
            }
        });
        return server.listen(QHostAddress::LocalHost);
    }
    Session session() const
    {
        return {QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort())),
                "BITRIX_TEST=synthetic", "local-csrf"};
    }

    QList<Request> matching(const QString &key) const
    {
        QList<Request> found;
        for (const auto &request : requests) {
            if (request.action() == key || request.url.path() == key)
                found.append(request);
        }
        return found;
    }

    bool hasDeferredResponse() const { return !m_deferredSocket.isNull(); }
    void releaseDeferredResponse()
    {
        if (!m_deferredSocket)
            return;
        m_deferredSocket->write(m_deferredResponse);
        m_deferredSocket->disconnectFromHost();
        m_deferredSocket.clear();
        m_deferredResponse.clear();
    }

    std::function<Response(const Request &)> responder;
    QList<Request> requests;
private:
    QTcpServer server;
    QPointer<QTcpSocket> m_deferredSocket;
    QByteArray m_deferredResponse;
};

const QByteArray fileId = "n123";
const QByteArray loadReply = R"({"chat":{"id":321},"messages":[{"id":52,"authorId":7,"text":"",
    "date":"2026-11-20T10:00:00Z","params":{"FILE_ID":[55]}}],"files":[{"id":55,"name":"existing.txt"}]})";
const QByteArray taskReply = R"({"task":{"id":"42","title":"Task","chatId":321,"ufTaskWebdavFiles":[77]}})";

} // namespace

class TestCloudChannels : public QObject {
    Q_OBJECT

private slots:
    void searchesOutsideRecentAndLoadedHistory();
    void uploadsChatAndTaskFiles();
    void uploadsChatImagesWithCaption_data();
    void uploadsChatImagesWithCaption();
    void doesNotCommitClipboardImageAfterAccountSwitch();
    void commitsFilesUsingFinalDiskId_data();
    void commitsFilesUsingFinalDiskId();
    void loadsTaskFileMetadataAndStreamsPhotoOriginal();
    void fallsBackToSmallAuthorizedTaskImageOriginals();
    void uploadFailureAndRedirectDoNotCommit();
    void decodesForwardedMp4AndItsAuthorizedPoster();
    void previewsOnlyRegisteredImagesAndQuotes();
    void previewsSmallAuthorizedChatImageOriginals();
    void previewsNativeResizeAndDeclaredOriginals_data();
    void previewsNativeResizeAndDeclaredOriginals();
    void reservesStandbyOriginalsWithoutEvictingPreferredImages();
    void linkPreviewsRequireAnAuthorizedMatchingPortalObject();
    void decodesPortalExternalLinkMetadata();
    void labelsOnlyVerifiedTaskCreationNotices();
    void enrichesTaskLinkPreviewWithCurrentStage();
    void loadsOnlyViewersOfTheExactOutgoingMessage();
    void streamsOriginalPhotoAndCancelsIndependently();
};

void TestCloudChannels::labelsOnlyVerifiedTaskCreationNotices()
{
    const QUrl portal(QStringLiteral("https://example.bitrix24.ru"));
    const auto generic = QStringLiteral("Уведомление Битрикс24");
    const auto created = QStringLiteral("Новая задача");
    const auto task = QStringLiteral("[URL=/workgroups/group/7/tasks/task/view/501/]Проверить печать[/URL]");
    const auto normalize = [&](const QString &action, const QString &object, const QString &module, const QString &title) {
        Notification notice;
        notice.type = module;
        notice.title = title;
        const auto subject = QStringLiteral("#AUTHOR# %1 %2").arg(action, object);
        decodeNotificationContent(notice, portal, QStringLiteral("#AUTHOR# ") + action,
                                  {{QStringLiteral("COMPONENT_PARAMS"), QJsonObject{{QStringLiteral("SUBJECT"), subject}}}},
                                  QStringLiteral("19"), QStringLiteral("Мария"));
        return notice;
    };
    const auto taskModule = QStringLiteral("tasks");
    const auto action = QStringLiteral("создала задачу");
    const auto notice = normalize(action, task, taskModule, generic);
    QCOMPARE(notice.title, created);
    QCOMPARE(notice.text, QStringLiteral("Мария создала задачу"));
    QVERIFY(notice.markup.contains(QStringLiteral("Проверить печать")));
    QVERIFY(std::any_of(notice.targets.cbegin(), notice.targets.cend(), [](const auto &target) {
        return target.kind == NotificationTargetKind::Task && target.id == QLatin1String("501");
    }));
    QVERIFY(std::any_of(notice.targets.cbegin(), notice.targets.cend(), [](const auto &target) {
        return target.kind == NotificationTargetKind::Person && target.id == QLatin1String("19");
    }));
    QCOMPARE(normalize(QStringLiteral("завершила задачу"), task, taskModule, generic).title, generic);
    QCOMPARE(normalize(action, task, QStringLiteral("im"), generic).title, generic);
    QCOMPARE(normalize(action, QStringLiteral("[URL=https://foreign.invalid/tasks/task/view/501/]Задача[/URL]"),
                       taskModule, generic).title, generic);
    const auto custom = QStringLiteral("Задача на согласование");
    QCOMPARE(normalize(action, task, taskModule, custom).title, custom);
}

void TestCloudChannels::searchesOutsideRecentAndLoadedHistory()
{
    Fixture fixture;
    QVERIFY(fixture.listen());
    fixture.responder = [](const Request &request) {
        if (request.action() == searchAction)
            return ajax(R"({"dialog":{"items":[{"id":"200","title":"Person outside recents","customData":{"user":{"id":200}}},{"id":"bad/query","title":"Ignore me"}]}})");
        if (request.action() == loadAction)
            return ajax(loadReply);
        if (request.action() == QLatin1String("tasks.task.get"))
            return ajax(taskReply);
        if (request.url.path() == searchPath) {
            const auto query = request.field(QStringLiteral("SEARCH_MESSAGE")).toUtf8();
            return result("{\"messages\":[{\"id\":500,\"author_id\":9,\"text\":\"" + query
                          + "\",\"files\":[61]}],"
                            "\"usersShort\":[{\"id\":9,\"name\":\"Remote sender\"}],"
                            "\"files\":[{\"id\":61,\"name\":\"/var/mock/remote.pdf\"}]}");
        }
        return ajax("{}");
    };

    CloudBackend backend(fixture.session(), true);
    QSignalSpy chats(&backend, &Backend::chatsFound);
    QSignalSpy messages(&backend, &Backend::messagesFound);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    backend.start();

    backend.searchChats(QStringLiteral("Person outside"));
    QTRY_COMPARE(chats.size(), 1);
    QCOMPARE(chats.first().at(0).toString(), QStringLiteral("Person outside"));
    const auto items = chats.first().at(1).value<QVector<Chat>>();
    QCOMPARE(items.size(), 1);
    QCOMPARE(items.first().id, QStringLiteral("200"));

    const auto searchRequest = fixture.matching(searchAction).first();
    const auto json = QJsonDocument::fromJson(searchRequest.body).object();
    QCOMPARE(json.value(QStringLiteral("dialog")).toObject().value(QStringLiteral("context")).toString(),
             QStringLiteral("IM_CHAT_SEARCH"));
    QCOMPARE(json.value(QStringLiteral("searchQuery")).toObject().value(QStringLiteral("queryWords")).toArray().size(), 2);

    backend.searchMessages(items.first().id, QStringLiteral("archived one"), false);
    backend.searchMessages(items.first().id, QStringLiteral("archived two"), false);
    QTRY_COMPARE(messages.size(), 2);
    QSet<QString> queries;
    for (const auto &response : messages) {
        QCOMPARE(response.at(0).toString(), items.first().id);
        const auto query = response.at(1).toString();
        queries.insert(query);
        const auto rows = response.at(3).value<QVector<Message>>();
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().text, query);
        QCOMPARE(rows.first().author, QStringLiteral("Remote sender"));
        QCOMPARE(rows.first().attachments, QStringList{QStringLiteral("remote.pdf")});
    }
    QCOMPARE(queries.size(), 2);
    const auto searches = fixture.matching(searchPath);
    QCOMPARE(searches.size(), 2);
    for (const auto &request : searches)
        QCOMPARE(request.field(QStringLiteral("CHAT_ID")), QStringLiteral("321"));

    backend.searchMessages(QStringLiteral("42"), QStringLiteral("task archive"), true);
    QTRY_COMPARE(messages.size(), 3);
    QCOMPARE(messages.last().at(0).toString(), QStringLiteral("42"));
    QCOMPARE(messages.last().at(1).toString(), QStringLiteral("task archive"));
    QVERIFY(messages.last().at(2).toBool());
    QCOMPARE(fixture.matching(searchPath).last().field(QStringLiteral("CHAT_ID")), QStringLiteral("321"));

    backend.requestChatMessages(items.first().id);
    QTRY_COMPARE(history.size(), 1);
    const auto rows = history.first().at(1).value<QVector<Message>>();
    QCOMPARE(rows.first().attachments, QStringList{QStringLiteral("existing.txt")});
}

void TestCloudChannels::uploadsChatAndTaskFiles()
{
    Fixture fixture;
    QVERIFY(fixture.listen());
    fixture.responder = [](const Request &request) {
        if (request.action() == loadAction)
            return ajax(loadReply);
        if (request.action() == QLatin1String("tasks.task.get"))
            return ajax(taskReply);
        if (request.action() == QLatin1String("tasks.v2.File.listObjects"))
            return ajax(R"([{"id":77,"name":"task-existing.txt"}])");
        if (request.url.path() == folderPath)
            return result(R"({"ID":502})");
        if (request.action() == uploadAction) {
            if (request.headers.value("content-range").startsWith("bytes 0-"))
                return ajax(R"({"token":"n123"})");
            return ajax(R"({"token":"n124","file":{"id":6,"name":"received"}})");
        }
        if (request.url.path() == commitPath)
            return result(R"({"MESSAGE_ID":55})");
        if (request.action() == attachAction)
            return ajax(R"({})");
        return ajax("{}");
    };

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QFile file(dir.filePath(QStringLiteral("данные +.bin")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    constexpr int chunkSize = 10 * 1024 * 1024;
    QCOMPARE(file.write(QByteArray(chunkSize, 'X')), qint64(chunkSize));
    QCOMPARE(file.write("END"), qint64(3));
    file.close();

    CloudBackend backend(fixture.session(), true);
    QSignalSpy uploaded(&backend, &Backend::fileUploaded);
    QSignalSpy attached(&backend, &Backend::taskFileAttached);
    bool attachedBeforeCompletion = false;
    connect(&backend, &Backend::fileUploaded, this, [&](const QString &, const QString &kind) {
        if (kind == QLatin1String("task"))
            attachedBeforeCompletion = attached.size() == 1;
    });
    QSignalSpy task(&backend, &Backend::taskLoaded);
    backend.start();
    backend.requestTask(QStringLiteral("42"));
    QTRY_COMPARE(task.size(), 2);
    QCOMPARE(task.last().first().value<Task>().attachments, QStringList{QStringLiteral("task-existing.txt")});

    backend.uploadChatFile(QStringLiteral("200"), file.fileName(), false);
    QTRY_COMPARE(uploaded.size(), 1);
    QCOMPARE(uploaded.last().at(0).toString(), QStringLiteral("200"));
    QCOMPARE(uploaded.last().at(1).toString(), QStringLiteral("chat"));
    backend.uploadChatFile(QStringLiteral("42"), file.fileName(), true);
    QTRY_COMPARE(uploaded.size(), 2);
    QCOMPARE(uploaded.last().at(1).toString(), QStringLiteral("task-chat"));
    backend.attachTaskFile(QStringLiteral("42"), file.fileName());
    QTRY_COMPARE(uploaded.size(), 3);
    QCOMPARE(uploaded.last().at(1).toString(), QStringLiteral("task"));
    QVERIFY(attachedBeforeCompletion);
    QCOMPARE(attached.first().at(0).toString(), QStringLiteral("42"));
    QCOMPARE(attached.first().at(1).toString(), QStringLiteral("n123"));

    const auto uploads = fixture.matching(uploadAction);
    QCOMPARE(uploads.size(), 6);
    for (int i = 0; i < 6; ++i) {
        const auto &upload = uploads.at(i);
        QCOMPARE(upload.parameter(QStringLiteral("controller")), QStringLiteral("disk.uf.integration.diskUploaderController"));
        QCOMPARE(upload.headers.value("cookie"), QByteArray("BITRIX_TEST=synthetic"));
        QCOMPARE(upload.headers.value("x-bitrix-csrf-token"), QByteArray("local-csrf"));
        QCOMPARE(upload.headers.value("x-upload-content-name"), QUrl::toPercentEncoding(QStringLiteral("данные +.bin")));
        if (i % 2 == 0) {
            QCOMPARE(upload.parameter(QStringLiteral("token")), QString());
            QCOMPARE(upload.body.size(), chunkSize);
            QCOMPARE(upload.headers.value("content-range"), QByteArray("bytes 0-10485759/10485763"));
        } else {
            QCOMPARE(upload.parameter(QStringLiteral("token")), QStringLiteral("n123"));
            QCOMPARE(upload.body, QByteArray("END"));
            QCOMPARE(upload.headers.value("content-range"), QByteArray("bytes 10485760-10485762/10485763"));
        }
    }
    const auto options = QJsonDocument::fromJson(uploads.first().parameter(QStringLiteral("controllerOptions")).toUtf8()).object();
    QCOMPARE(options.value(QStringLiteral("folderId")).toInt(), 502);
    const auto optionsChat = options.value(QStringLiteral("chat")).toObject();
    QCOMPARE(optionsChat.value(QStringLiteral("chatId")).toInt(), 321);
    QCOMPARE(optionsChat.value(QStringLiteral("dialogId")).toString(), QStringLiteral("200"));
    const auto taskChatOptions = QJsonDocument::fromJson(uploads.at(2).parameter(QStringLiteral("controllerOptions")).toUtf8()).object();
    QCOMPARE(taskChatOptions.value(QStringLiteral("chat")).toObject().value(QStringLiteral("dialogId")).toString(),
             QStringLiteral("chat321"));
    QVERIFY(uploads.last().parameter(QStringLiteral("controllerOptions")).isEmpty());

    const auto commits = fixture.matching(commitPath);
    QCOMPARE(commits.size(), 2);
    for (const auto &commit : commits) {
        QCOMPARE(commit.field(QStringLiteral("chat_id")), QStringLiteral("321"));
        QCOMPARE(commit.field(QStringLiteral("upload_id")), QStringLiteral("123"));
        QCOMPARE(commit.field(QStringLiteral("message")), QString());
        QCOMPARE(commit.field(QStringLiteral("as_file")), QStringLiteral("Y"));
        QVERIFY(!QUuid(commit.field(QStringLiteral("file_template_id"))).isNull());
        QVERIFY(!QUuid(commit.field(QStringLiteral("template_id"))).isNull());
    }
    const auto attaches = fixture.matching(attachAction);
    QCOMPARE(attaches.size(), 1);
    const auto attach = QJsonDocument::fromJson(attaches.first().body).object();
    QCOMPARE(attach.value(QStringLiteral("task")).toObject().value(QStringLiteral("id")).toInt(), 42);
    QCOMPARE(attach.value(QStringLiteral("ids")).toArray().first().toString(), QString::fromLatin1(fileId));
}

void TestCloudChannels::uploadsChatImagesWithCaption_data()
{
    QTest::addColumn<bool>("task");
    QTest::addColumn<QString>("caption");
    QTest::addColumn<bool>("oversized");
    QTest::newRow("ordinary chat") << false << QStringLiteral("Снимок + столбец & строка\nПродолжение") << false;
    QTest::newRow("task chat") << true << QStringLiteral("Фото к задаче") << false;
    QTest::newRow("image without a caption") << false << QString() << false;
    QTest::newRow("caption at the limit") << true << QString(20000, QLatin1Char('a')) << false;
    QTest::newRow("caption too long") << false << QString(20001, QLatin1Char('a')) << false;
    QTest::newRow("image too large") << false << QString() << true;
}

void TestCloudChannels::uploadsChatImagesWithCaption()
{
    QFETCH(bool, task);
    QFETCH(QString, caption);
    QFETCH(bool, oversized);
    Fixture fixture;
    QVERIFY(fixture.listen());
    fixture.responder = [](const Request &request) {
        if (request.action() == loadAction)
            return ajax(loadReply);
        if (request.action() == QLatin1String("tasks.task.get"))
            return ajax(taskReply);
        if (request.url.path() == folderPath)
            return result(R"({"ID":502})");
        if (request.action() == uploadAction)
            return ajax(R"({"token":"n123","file":{"id":6,"serverFileId":"n456"}})");
        if (request.url.path() == commitPath)
            return result(R"({"MESSAGE_ID":55})");
        return ajax("{}");
    };
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QFile file(dir.filePath(QStringLiteral("Изображение.png")));
    const QByteArray image = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO9Z3s0AAAAASUVORK5CYII=");
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(image), qint64(image.size()));
    if (oversized)
        QVERIFY(file.resize(tunables::kChatClipboardPngBytes + 1));
    file.close();

    CloudBackend backend(fixture.session(), true);
    QSignalSpy uploaded(&backend, &Backend::fileUploaded);
    const auto scope = task ? QStringLiteral("42") : QStringLiteral("200");
    const auto operation = (task ? QStringLiteral("upload-task-chat:") : QStringLiteral("upload-chat:")) + scope;
    QStringList errors;
    connect(&backend, &Backend::failed, this, [&](const QString &name, const QString &reason) {
        if (name == operation)
            errors.append(reason);
    });
    backend.start();
    backend.uploadChatImage(scope, file.fileName(), task, caption);
    QTRY_COMPARE(uploaded.size() + errors.size(), 1);
    if (caption.size() > 20000 || oversized) {
        QCOMPARE(errors.size(), 1);
        QVERIFY(fixture.matching(folderPath).isEmpty());
        QVERIFY(fixture.matching(uploadAction).isEmpty());
        QVERIFY(fixture.matching(commitPath).isEmpty());
        return;
    }
    QCOMPARE(errors.size(), 0);
    QCOMPARE(uploaded.first().at(0).toString(), scope);
    QCOMPARE(uploaded.first().at(1).toString(), task ? QStringLiteral("task-chat") : QStringLiteral("chat"));
    const auto uploads = fixture.matching(uploadAction);
    QCOMPARE(uploads.size(), 1);
    QCOMPARE(uploads.first().body, image);
    QCOMPARE(uploads.first().headers.value("content-type"), QByteArray("image/png"));
    const auto commits = fixture.matching(commitPath);
    QCOMPARE(commits.size(), 1);
    QCOMPARE(commits.first().field(QStringLiteral("chat_id")), QStringLiteral("321"));
    QCOMPARE(commits.first().field(QStringLiteral("upload_id")), QStringLiteral("456"));
    QCOMPARE(commits.first().field(QStringLiteral("as_file")), QStringLiteral("N"));
    QCOMPARE(commits.first().field(QStringLiteral("message")), caption);
}

void TestCloudChannels::doesNotCommitClipboardImageAfterAccountSwitch()
{
    Fixture fixture;
    QVERIFY(fixture.listen());
    int userId = 11;
    bool deferUpload = true;
    fixture.responder = [&userId, &deferUpload](const Request &request) {
        if (request.action() == QLatin1String("im.v2.Recent.load"))
            return ajax("{\"userId\":" + QByteArray::number(userId) + ",\"recentItems\":[]}");
        if (request.url.path() == folderPath)
            return result(R"({"ID":502})");
        if (request.action() == uploadAction) {
            if (std::exchange(deferUpload, false)) {
                auto response = ajax(R"({"token":"n123","file":{"id":6,"serverFileId":"n123"}})");
                response.deferred = true;
                return response;
            }
            return ajax(R"({"token":"n456","file":{"id":7,"serverFileId":"n456"}})");
        }
        if (request.url.path() == commitPath)
            return result(R"({"MESSAGE_ID":55})");
        return ajax("{}");
    };
    auto directory = std::make_shared<QTemporaryDir>();
    QVERIFY(directory->isValid());
    const auto directoryPath = directory->path();
    QFile file(directory->filePath(QStringLiteral("Фото.png")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write("synthetic PNG bytes") > 0);
    file.close();

    CloudBackend backend(fixture.session(), true);
    QSignalSpy scopeChanged(&backend, &Backend::accountScopeChanged);
    QSignalSpy uploaded(&backend, &Backend::fileUploaded);
    backend.start();
    backend.requestRecentChats();
    QTRY_COMPARE(scopeChanged.size(), 1);
    const auto dialog = QStringLiteral("chat321");
    backend.uploadChatImage(dialog, file.fileName(), false, QStringLiteral("Старый аккаунт"), directory);
    QTRY_VERIFY(fixture.hasDeferredResponse());
    const std::weak_ptr<QTemporaryDir> pendingUploadDirectory = directory;
    directory.reset();
    QVERIFY(!pendingUploadDirectory.expired());
    userId = 12;
    backend.requestRecentChats();
    QTRY_COMPARE(scopeChanged.size(), 2);
    backend.uploadChatImage(dialog, file.fileName(), false, QStringLiteral("Новый аккаунт"));
    QTRY_COMPARE(uploaded.size(), 1);
    fixture.releaseDeferredResponse();
    QTRY_VERIFY(pendingUploadDirectory.expired());
    QVERIFY(!QFileInfo::exists(directoryPath));
    QCOMPARE(uploaded.size(), 1);
    const auto commits = fixture.matching(commitPath);
    QCOMPARE(commits.size(), 1);
    QCOMPARE(commits.first().field(QStringLiteral("message")), QStringLiteral("Новый аккаунт"));
    QCOMPARE(commits.first().field(QStringLiteral("upload_id")), QStringLiteral("456"));
}

void TestCloudChannels::commitsFilesUsingFinalDiskId_data()
{
    QTest::addColumn<bool>("chat");
    QTest::addColumn<QByteArray>("response");
    QTest::addColumn<bool>("committed");
    QTest::newRow("xls-task") << false
        << QByteArray(R"({"token":"signed.continuation","file":{"serverFileId":"n987"}})") << true;
    QTest::newRow("xls-task-chat") << true
        << QByteArray(R"({"token":"signed.continuation","file":{"serverFileId":"n987"}})") << true;
    QTest::newRow("invalid-final-id") << false
        << QByteArray(R"({"token":"n123","file":{"serverFileId":"n-987"}})") << false;
}

void TestCloudChannels::commitsFilesUsingFinalDiskId()
{
    QFETCH(bool, chat);
    QFETCH(QByteArray, response);
    QFETCH(bool, committed);

    Fixture fixture;
    QVERIFY(fixture.listen());
    fixture.responder = [&](const Request &request) {
        if (request.action() == loadAction)
            return ajax(loadReply);
        if (request.action() == QLatin1String("tasks.task.get"))
            return ajax(taskReply);
        if (request.url.path() == folderPath)
            return result(R"({"ID":502})");
        if (request.action() == uploadAction)
            return ajax(response);
        if (request.url.path() == commitPath)
            return result(R"({"MESSAGE_ID":55})");
        return ajax("{}");
    };

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto name = QStringLiteral("Проверка ОП (1).xls");
    QFile file(dir.filePath(name));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("XLS fixture"), 11);
    file.close();

    CloudBackend backend(fixture.session(), true);
    QSignalSpy uploaded(&backend, &Backend::fileUploaded);
    QSignalSpy attached(&backend, &Backend::taskFileAttached);
    QStringList uploadFailures;
    const auto operation = chat ? QStringLiteral("upload-task-chat:42") : QStringLiteral("attach-task:42");
    connect(&backend, &Backend::failed, this, [&](const QString &name, const QString &message) {
        if (name == operation)
            uploadFailures.append(message);
    });
    backend.start();
    if (chat)
        backend.uploadChatFile(QStringLiteral("42"), file.fileName(), true);
    else
        backend.attachTaskFile(QStringLiteral("42"), file.fileName());

    QTRY_COMPARE(uploaded.size() + uploadFailures.size(), 1);
    const auto uploads = fixture.matching(uploadAction);
    QCOMPARE(uploads.size(), 1);
    QCOMPARE(uploads.first().headers.value("x-upload-content-name"), QUrl::toPercentEncoding(name));
    QCOMPARE(uploads.first().body, QByteArray("XLS fixture"));

    const auto attaches = fixture.matching(attachAction);
    const auto commits = fixture.matching(commitPath);
    QCOMPARE(uploadFailures, committed ? QStringList{} :
        QStringList{QStringLiteral("Портал вернул неподдерживаемый формат данных")});
    QCOMPARE(uploaded.size(), committed ? 1 : 0);
    QCOMPARE(attaches.size(), committed && !chat ? 1 : 0);
    QCOMPARE(commits.size(), committed && chat ? 1 : 0);
    QCOMPARE(attached.size(), attaches.size());
    if (!attaches.isEmpty()) {
        const auto attach = QJsonDocument::fromJson(attaches.first().body).object();
        QCOMPARE(attach.value("task").toObject().value("id").toInt(), 42);
        QCOMPARE(attach.value("ids").toArray(), QJsonArray({QStringLiteral("n987")}));
        QCOMPARE(attached.first().at(1).toString(), QStringLiteral("n987"));
    }
    if (!commits.isEmpty())
        QCOMPARE(commits.first().field(QStringLiteral("upload_id")), QStringLiteral("987"));
}

void TestCloudChannels::loadsTaskFileMetadataAndStreamsPhotoOriginal()
{
    const auto png = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aRYAAAAAASUVORK5CYII=");
    Fixture fixture;
    QVERIFY(fixture.listen());
    fixture.responder = [&](const Request &request) -> Response {
        if (request.action() == QLatin1String("tasks.task.get"))
            return ajax(R"({"task":{"id":42,"title":"Rich task","description":"[disk file id=n123 width=600 height=434]",
                "ufTaskWebdavFiles":[77,"n456",88,89,77]}})");
        if (request.action() == QLatin1String("tasks.v2.File.listObjects"))
            return ajax(R"([{"id":77,"name":"photo.png","isImage":true,"serverPreviewWidth":200,"serverPreviewHeight":145,
                "downloadUrl":"/bitrix/tools/disk/uf.php?action=download&attachedId=77",
                "serverPreviewUrl":"/bitrix/services/main/ajax.php?action=disk.api.file.showImage&fileId=123",
                "customData":{"objectId":123}},
                {"id":"n456","name":"report.pdf","size":500,"downloadUrl":"/disk/downloadFile/456/"},
                {"id":88,"name":"restricted.png","isImage":true,"downloadUrl":"https://example.invalid/photo.png",
                "serverPreviewUrl":"/bitrix/tools/disk/uf.php?action=show&attachedId=89&signature=fixture&ncc=1"},
                {"id":89,"name":"inline.png","isImage":true,"serverPreviewWidth":240,"serverPreviewHeight":150,
                "customData":{"objectId":654},
                "downloadUrl":"/bitrix/services/main/ajax.php?action=disk.attachedObject.download&SITE_ID=s1&attachedObjectId=89&_efd=0",
                "serverPreviewUrl":"/bitrix/tools/disk/uf.php?width=240&height=150&_efd=0&signature=fixture&attachedId=89&action=show&ncc=1"},
                {"id":99,"name":"unrequested.txt","downloadUrl":"/disk/downloadFile/99/"}])");
        if (request.action() == QLatin1String("disk.api.file.showImage")
            || request.action() == QLatin1String("disk.attachedObject.download") ||
            request.url.path() == QLatin1String("/bitrix/tools/disk/uf.php"))
            return {png, "200 OK", {}, "image/png"};
        return ajax("{}");
    };
    CloudBackend backend(fixture.session(), true);
    QSignalSpy task(&backend, &Backend::taskLoaded);
    QSignalSpy previews(&backend, &Backend::messageImageLoaded);
    QSignalSpy downloads(&backend, &Backend::messageFileDownloaded);
    backend.start();
    backend.requestTask(QStringLiteral("42"));
    QTRY_COMPARE(task.size(), 2);
    const auto card = task.last().first().value<Task>();
    QCOMPARE(card.description, QStringLiteral("[disk file id=n123 width=600 height=434]"));
    QCOMPARE(card.files.size(), 4);
    const auto &photo = card.files.first();
    QCOMPARE(photo.id, QStringLiteral("77"));
    QCOMPARE(photo.diskObjectId, QStringLiteral("n123"));
    QVERIFY(photo.isImage);
    QCOMPARE(photo.previewSize, QSize(200, 145));
    QVERIFY(!photo.previewUrl.isEmpty());
    QCOMPARE(card.files.at(1).id, QStringLiteral("n456"));
    QCOMPARE(card.files.at(1).size, 500);
    QVERIFY(card.files.at(2).previewUrl.isEmpty());
    QVERIFY(card.files.at(2).downloadUrl.isEmpty());
    const auto &inlinePhoto = card.files.at(3);
    QCOMPARE(inlinePhoto.id, QStringLiteral("89"));
    QCOMPARE(inlinePhoto.diskObjectId, QStringLiteral("n654"));
    QVERIFY(!inlinePhoto.previewUrl.isEmpty());
    QVERIFY(inlinePhoto.downloadUrl != inlinePhoto.previewUrl);
    QCOMPARE(QUrlQuery(inlinePhoto.previewUrl).queryItemValue(QStringLiteral("action")), QStringLiteral("show"));
    QCOMPARE(QUrlQuery(inlinePhoto.previewUrl).queryItemValue(QStringLiteral("attachedId")), QStringLiteral("89"));
    const auto request = fixture.matching(QStringLiteral("tasks.v2.File.listObjects")).first();
    QCOMPARE(QJsonDocument::fromJson(request.body).object().value("ids").toArray(), QJsonArray({77, "n456", 88, 89}));

    backend.requestMessageImage(photo.previewUrl);
    QTRY_COMPARE(previews.size(), 1);
    QCOMPARE(previews.first().last().toByteArray(), png);
    backend.requestMessageImage(inlinePhoto.previewUrl);
    QTRY_COMPARE(previews.size(), 2);
    QCOMPARE(previews.last().first().toUrl(), inlinePhoto.previewUrl);
    QCOMPARE(previews.last().last().toByteArray(), png);
    QTemporaryDir target;
    QVERIFY(target.isValid());
    const auto path = target.filePath(QStringLiteral("original.png"));
    backend.downloadMessagePhoto(inlinePhoto.downloadUrl, path);
    QTRY_COMPARE(downloads.size(), 1);
    QFile original(path);
    QVERIFY(original.open(QIODevice::ReadOnly));
    QCOMPARE(original.readAll(), png);
}

void TestCloudChannels::fallsBackToSmallAuthorizedTaskImageOriginals()
{
    const auto jpeg = QByteArray::fromBase64(
        "/9j/4AAQSkZJRgABAQAAAAAAAAD/2wBDAAMCAgICAgMCAgIDAwMDBAYEBAQEBAgGBgUGCQgKCgkICQkKDA8MCgsOCwkJ"
        "DRENDg8QEBEQCgwSExIQEw8QEBD/2wBDAQMDAwQDBAgEBAgQCwkLEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQ"
        "EBAQEBAQEBAQEBAQEBD/wAARCAABAAEDAREAAhEBAxEB/8QAFAABAAAAAAAAAAAAAAAAAAAABv/EABQQAQAAAAAAAAAA"
        "AAAAAAD/xAAUAQEAAAAAAAAAAAAAAAAAAAAH/8QAFBEBAAAAAAAAAAAAAAAAAAAAAP/aAAwDAQACEQMRAD8APjItv//Z");
    Fixture fixture;
    QVERIFY(fixture.listen());
    fixture.responder = [&](const Request &request) -> Response {
        if (request.action() == QLatin1String("tasks.task.get"))
            return ajax(R"({"task":{"id":42,"title":"Task","ufTaskWebdavFiles":[90,91,92,93,"n94",95]}})");
        if (request.action() == QLatin1String("tasks.v2.File.listObjects"))
            return ajax(R"([
                {"id":90,"name":"small.jpg","size":173000,"isImage":true,"downloadUrl":"/bitrix/tools/disk/uf.php?action=download&attachedId=90"},
                {"id":91,"name":"mismatch.jpg","size":200,"isImage":true,"downloadUrl":"/bitrix/tools/disk/uf.php?action=download&attachedId=90"},
                {"id":92,"name":"large.jpg","size":524289,"isImage":true,"downloadUrl":"/bitrix/tools/disk/uf.php?action=download&attachedId=92"},
                {"id":93,"name":"rejected-preview.jpg","size":173000,"isImage":true,"serverPreviewUrl":"/bitrix/tools/disk/uf.php?action=show&attachedId=90",
                 "downloadUrl":"/bitrix/services/main/ajax.php?action=disk.attachedObject.download&attachedObjectId=93"},
                {"id":"n94","name":"not-raster.jpg","size":200,"isImage":true,"downloadUrl":"/disk/downloadFile/94/"},
                {"id":95,"name":"unknown-size.jpg","isImage":true,"downloadUrl":"/bitrix/tools/disk/uf.php?action=download&attachedId=95"}
            ])");
        if (request.url.path() == QLatin1String("/bitrix/tools/disk/uf.php") && request.parameter(QStringLiteral("attachedId")) == QLatin1String("90"))
            return {jpeg, "200 OK", {}, "image/jpeg"};
        if (request.action() == QLatin1String("disk.attachedObject.download"))
            return {jpeg, "200 OK", {}, "application/octet-stream"};
        if (request.url.path() == QLatin1String("/disk/downloadFile/94/"))
            return {QByteArrayLiteral("<html>Denied</html>"), "200 OK", {}, "text/html"};
        return ajax("{}");
    };
    CloudBackend backend(fixture.session(), true);
    QSignalSpy task(&backend, &Backend::taskLoaded);
    QSignalSpy previews(&backend, &Backend::messageImageLoaded);
    backend.start();
    backend.requestTask(QStringLiteral("42"));
    QTRY_COMPARE(task.size(), 2);
    const auto files = task.last().first().value<Task>().files;
    QCOMPARE(files.size(), 6);
    for (const int row : {0, 3, 4}) {
        QVERIFY(!files.at(row).previewUrl.isEmpty());
        QCOMPARE(files.at(row).previewUrl, files.at(row).downloadUrl);
    }
    for (const int row : {1, 2, 5}) {
        QVERIFY(files.at(row).previewUrl.isEmpty());
        QVERIFY(!files.at(row).downloadUrl.isEmpty());
    }
    QVERIFY(fixture.matching(QStringLiteral("/bitrix/tools/disk/uf.php")).isEmpty());
    QVERIFY(fixture.matching(QStringLiteral("disk.attachedObject.download")).isEmpty());
    for (const int row : {0, 3, 4}) {
        backend.requestMessageImage(files.at(row).previewUrl);
        const int count = row == 0 ? 1 : row == 3 ? 2 : 3;
        QTRY_COMPARE(previews.size(), count);
        QCOMPARE(previews.last().last().toByteArray(), row == 4 ? QByteArray{} : jpeg);
    }
}

void TestCloudChannels::uploadFailureAndRedirectDoNotCommit()
{
    for (const bool redirect : {false, true}) {
        Fixture fixture;
        QVERIFY(fixture.listen());
        fixture.responder = [redirect](const Request &request) {
            if (request.action() == loadAction)
                return ajax(loadReply);
            if (request.url.path() == folderPath)
                return result(R"({"ID":5})");
            if (request.action() == uploadAction)
                return redirect ? Response{{}, "307 Temporary Redirect", "Location: https://not-the-portal.invalid/\r\n"}
                                : ajax(R"({"token":"n123"})");
            return ajax("{}");
        };
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile file(dir.filePath(QStringLiteral("file.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("payload"), qint64(7));
        file.close();

        CloudBackend backend(fixture.session(), true);
        QSignalSpy uploaded(&backend, &Backend::fileUploaded);
        QSignalSpy errors(&backend, &Backend::failed);
        backend.start();
        backend.uploadChatFile(QStringLiteral("200"), file.fileName(), false);
        QTRY_VERIFY(std::any_of(errors.cbegin(), errors.cend(), [](const auto &error) {
            return error.first().toString() == QLatin1String("upload-chat:200");
        }));
        QCOMPARE(uploaded.size(), 0);
        QCOMPARE(fixture.matching(uploadAction).size(), 1);
        QCOMPARE(fixture.matching(commitPath).size(), 0);
    }
}

void TestCloudChannels::decodesForwardedMp4AndItsAuthorizedPoster()
{
    const auto ftyp = QByteArray::fromHex("00000018667479706d7034320000000069736f6d6d703432");
    Fixture outside;
    QVERIFY(outside.listen());
    Fixture fixture;
    QVERIFY(fixture.listen());
    fixture.responder = [&](const Request &request) -> Response {
        if (request.action() == loadAction) {
            return ajax(R"({"chat":{"id":321},"messages":[
                {"id":51,"forward":{"id":49,"userId":7},"files":[55,56,57,58,59,60,61,62,63]},
                {"id":52,"replyId":51}],
                "files":[
                    {"id":55,"name":"forwarded.mp4","size":24,"type":"file","urlPreview":"/disk/showFile/55/",
                     "urlDownload":"/bitrix/services/main/ajax.php?action=disk.api.file.download&SITE_ID=s1&humanRE=Y&fileId=55&exact=Y&_esd=synthetic&fileName=forwarded.mp4"},
                    {"id":56,"name":"generic.txt","size":120,"type":"file","urlPreview":"/disk/showFile/56/","urlDownload":"/disk/downloadFile/56/"},
                    {"id":57,"name":"cover.png","size":200,"type":"image","urlPreview":"/disk/showFile/57/"},
                    {"id":58,"name":"remote.mp4","size":24,"type":"video","urlDownload":"https://outside.invalid/clip.mp4"},
                    {"id":59,"name":"redirect.mp4","size":24,"type":"video","urlDownload":"/disk/downloadFile/59/"},
                    {"id":60,"name":"arbitrary.mp4","size":24,"type":"video","urlDownload":"/different/action"},
                    {"id":61,"name":"invalid.mp4","size":24,"type":"video","urlDownload":"/disk/downloadFile/61/"},
                    {"id":62,"name":"remote-redirect.mp4","size":24,"type":"video","urlDownload":"/disk/downloadFile/62/"},
                    {"id":63,"name":"alias.mp4","size":24,"type":"video",
                     "urlDownload":"/bitrix/services/main/ajax.php?action=disk.api.file.download&fileId=63&action%5B0%5D=unsafe"}
                ]})");
        }
        if (request.action() == QLatin1String("disk.api.file.download"))
            return {ftyp, "200 OK", {}, "application/octet-stream"};
        if (request.url.path() == QLatin1String("/disk/downloadFile/59/"))
            return {{}, "302 Found", "Location: /different/action\r\n"};
        if (request.url.path() == QLatin1String("/disk/downloadFile/61/"))
            return {QByteArray(24, 'x'), "200 OK", {}, "video/mp4"};
        if (request.url.path() == QLatin1String("/disk/downloadFile/62/"))
            return {{}, "302 Found", "Location: " + outside.session().origin.toEncoded() + "/disk/downloadFile/62/\r\n"};
        return ajax("{}");
    };
    CloudBackend backend(fixture.session(), true);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    QSignalSpy downloaded(&backend, &Backend::messageVideoDownloaded);
    QSignalSpy rejected(&backend, &Backend::messageVideoDownloadFailed);
    QSignalSpy genericDownloads(&backend, &Backend::messageFileDownloaded);
    backend.start();
    backend.requestChatMessages(QStringLiteral("chat321"));
    QTRY_COMPARE(history.size(), 1);
    const auto messages = history.first().at(1).value<QVector<Message>>();
    QCOMPARE(messages.size(), 2);
    QVERIFY(messages.first().forwarded);
    const auto files = messages.first().files;
    QCOMPARE(files.size(), 9);
    QVERIFY(files.at(0).isVideo);
    QCOMPARE(files.at(0).previewUrl.path(), QStringLiteral("/disk/showFile/55/"));
    QCOMPARE(files.at(0).downloadUrl.path(), QStringLiteral("/bitrix/services/main/ajax.php"));
    QVERIFY(!files.at(1).isVideo && files.at(1).previewUrl.isEmpty());
    QVERIFY(files.at(2).isImage && !files.at(2).isVideo);
    QVERIFY(files.at(3).downloadUrl.isEmpty());
    QVERIFY(messages.last().replyFiles.first().isVideo);
    QCOMPARE(messages.last().replyFiles.first().previewUrl, files.at(0).previewUrl);

    QTemporaryDir target;
    QVERIFY(target.isValid());
    const auto clip = target.filePath(QStringLiteral("clip.mp4"));
    backend.downloadMessageVideo(files.at(0).downloadUrl, clip);
    QTRY_COMPARE(downloaded.size(), 1);
    QCOMPARE(downloaded.first().at(0).toUrl(), files.at(0).downloadUrl);
    QCOMPARE(downloaded.first().at(1).toString(), clip);
    QFile saved(clip);
    QVERIFY(saved.open(QIODevice::ReadOnly));
    QCOMPARE(saved.readAll(), ftyp);
    QCOMPARE(genericDownloads.size(), 0);
    const auto videoReads = fixture.matching(QStringLiteral("disk.api.file.download"));
    QCOMPARE(videoReads.size(), 1);
    QCOMPARE(videoReads.first().headers.value("cookie"), QByteArray("BITRIX_TEST=synthetic"));
    QVERIFY(!videoReads.first().headers.contains("x-bitrix-csrf-token"));

    for (const int row : {1, 3, 4, 5, 6, 7, 8}) {
        const auto failedTarget = target.filePath(QStringLiteral("failed-%1.mp4").arg(row));
        const auto failures = rejected.size();
        backend.downloadMessageVideo(files.at(row).downloadUrl, failedTarget);
        QTRY_COMPARE(rejected.size(), failures + 1);
        QCOMPARE(rejected.last().at(0).toString(), failedTarget);
        QVERIFY(!QFileInfo::exists(failedTarget));
    }
    QCOMPARE(fixture.matching(QStringLiteral("/different/action")).size(), 0);
    QCOMPARE(fixture.matching(QStringLiteral("/disk/downloadFile/56/")).size(), 0);
    QCOMPARE(outside.matching(QStringLiteral("/disk/downloadFile/62/")).size(), 0);
    QCOMPARE(fixture.matching(QStringLiteral("disk.api.file.download")).size(), 1);
}

void TestCloudChannels::previewsOnlyRegisteredImagesAndQuotes()
{
    const auto png = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aRYAAAAAASUVORK5CYII=");
    Fixture remote;
    QVERIFY(remote.listen());
    remote.responder = [&png](const Request &) { return Response{png, "200 OK", {}, "image/png"}; };
    Fixture fixture;
    QVERIFY(fixture.listen());
    fixture.responder = [&](const Request &request) -> Response {
        if (request.action() == loadAction) {
            return ajax(R"({"chat":{"id":321},"messages":[{"id":51,"files":[55,56,57,59,60,61,62]},
                {"id":52,"replyId":53,"text":"Quoted"}],
                "additionalMessages":[{"id":53,"files":[55]}],
                "files":[
                    {"id":55,"name":"photo.png","size":231,"type":"image","image":{"width":"600","height":400},
                     "urlPreview":"/bitrix/services/main/ajax.php?action=disk.api.file.download&fileId=55&exact=N",
                     "urlShow":"/bitrix/services/main/ajax.php?action=disk.api.file.showImage&fileId=55",
                     "urlDownload":"/disk/downloadFile/55/"},
                    {"id":56,"name":"document.txt","type":"file","urlPreview":"/upload/resize_cache/fake.png"},
                    {"id":57,"name":"fallback.png","type":"image","urlShow":"/disk/showFile/57/"},
                    {"id":59,"name":"redirect.png","type":"image","urlPreview":"/disk/showFile/59/"},
                    {"id":60,"name":"large.png","type":"image","urlPreview":"/disk/showFile/60/"},
                    {"id":61,"name":"original.png","type":"image","urlShow":"/disk/downloadFile/61/","urlDownload":"/disk/downloadFile/61/"},
                    {"id":62,"name":"wrong-id.png","type":"image","urlPreview":"/disk/showFile/63/"}
                ]})");
        }
        if (request.action() == QLatin1String("disk.api.file.download"))
            return {png, "200 OK", {}, "image/png"};
        if (request.url.path() == QLatin1String("/disk/showFile/57/")
            || request.url.path() == QLatin1String("/disk/downloadFile/55/"))
            return {png, "200 OK", {}, "image/png"};
        if (request.url.path() == QLatin1String("/disk/showFile/59/"))
            return {{}, "302 Found", "Location: " + remote.session().origin.toEncoded() + "/image.png\r\n"};
        if (request.url.path() == QLatin1String("/disk/showFile/60/"))
            return {png + QByteArray(512 * 1024, 'x'), "200 OK", {}, "image/png"};
        return ajax("{}");
    };
    CloudBackend backend(fixture.session(), true);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    QSignalSpy fetched(&backend, &Backend::messageImageLoaded);
    backend.start();
    backend.requestChatMessages(QStringLiteral("chat321"));
    QTRY_COMPARE(history.size(), 1);
    const auto messages = history.first().at(1).value<QVector<Message>>();
    QCOMPARE(messages.size(), 2);
    const auto files = messages.first().files;
    QCOMPARE(files.size(), 7);
    const auto photo = files.at(0);
    QVERIFY(photo.previewUrl.isValid());
    QCOMPARE(photo.previewSize, QSize(600, 400));
    QCOMPARE(photo.downloadUrl.path(), QStringLiteral("/disk/downloadFile/55/"));
    QCOMPARE(photo.originalPreviewUrl, photo.downloadUrl);
    QCOMPARE(messages.last().replyFiles.first().previewUrl, photo.previewUrl);
    QCOMPARE(messages.last().replyFiles.first().originalPreviewUrl, photo.originalPreviewUrl);
    QVERIFY(files.at(1).previewUrl.isEmpty());
    QVERIFY(files.at(2).previewUrl.isValid());
    QVERIFY(files.at(5).previewUrl.isEmpty());
    QVERIFY(files.at(6).previewUrl.isEmpty());
    QCOMPARE(fixture.matching(QStringLiteral("disk.api.file.download")).size(), 0);

    backend.requestMessageImage(photo.previewUrl);
    backend.requestMessageImage(photo.previewUrl);
    QTRY_COMPARE(fetched.size(), 1);
    QCOMPARE(fetched.first().last().toByteArray(), png);
    auto requests = fixture.matching(QStringLiteral("disk.api.file.download"));
    QCOMPARE(requests.size(), 1);
    QCOMPARE(requests.first().headers.value("cookie"), QByteArray("BITRIX_TEST=synthetic"));
    QVERIFY(!requests.first().headers.contains("x-bitrix-csrf-token"));
    backend.requestMessageImage(photo.previewUrl);
    QTRY_COMPARE(fetched.size(), 2);
    QCOMPARE(fixture.matching(QStringLiteral("disk.api.file.download")).size(), 1);
    QCOMPARE(fixture.matching(photo.downloadUrl.path()).size(), 0);

    backend.requestMessageImage(files.at(2).previewUrl);
    backend.requestMessageImage(files.at(3).previewUrl);
    backend.requestMessageImage(files.at(4).previewUrl);
    backend.requestMessageImage(photo.originalPreviewUrl);
    QTRY_COMPARE(fetched.size(), 6);
    QHash<QUrl, QByteArray> bytesByUrl;
    for (const auto &entry : fetched)
        bytesByUrl.insert(entry.first().toUrl(), entry.last().toByteArray());
    QCOMPARE(bytesByUrl.value(files.at(2).previewUrl), png);
    QVERIFY(bytesByUrl.value(files.at(3).previewUrl).isEmpty());
    QVERIFY(bytesByUrl.value(files.at(4).previewUrl).isEmpty());
    QCOMPARE(bytesByUrl.value(photo.originalPreviewUrl), png);
    QCOMPARE(remote.matching(QStringLiteral("/image.png")).size(), 0);
    QCOMPARE(fixture.matching(photo.downloadUrl.path()).size(), 1);
}

void TestCloudChannels::previewsSmallAuthorizedChatImageOriginals()
{
    const auto png = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aRYAAAAAASUVORK5CYII=");
    Fixture fixture;
    QVERIFY(fixture.listen());
    fixture.responder = [&](const Request &request) -> Response {
        if (request.action() == QLatin1String("tasks.task.get"))
            return ajax(R"({"task":{"id":42,"title":"Task","chatId":321}})");
        if (request.action() == loadAction)
            return ajax(R"({"chat":{"id":321},"messages":[{"id":51,"files":[65,66,67,68,69,70,71,72,73,74,75,76]}],
                "files":[
                    {"id":65,"NAME":"screenshot.PNG","SIZE":"53350","TYPE":"IMAGE",
                     "urlDownload":"/disk/downloadFile/65/"},
                    {"id":66,"name":"photo.png","size":250,"type":"file","mimeType":"image/png",
                     "urlPreview":"/disk/showFile/65/",
                     "urlDownload":"/bitrix/services/main/ajax.php?action=disk.api.file.download&fileId=66"},
                    {"id":67,"name":"generic.png","size":250,"type":"file",
                     "urlDownload":"/disk/downloadFile/67/"},
                    {"id":68,"name":"vector.svg","size":250,"type":"image","mimeType":"image/svg+xml",
                     "urlDownload":"/disk/downloadFile/68/"},
                    {"id":69,"name":"large.png","size":524289,"type":"image",
                     "urlDownload":"/disk/downloadFile/69/"},
                    {"id":70,"name":"unknown.png","type":"image","urlDownload":"/disk/downloadFile/70/"},
                    {"id":71,"name":"mismatch.png","size":250,"type":"image","urlDownload":"/disk/downloadFile/69/"},
                    {"id":72,"name":"remote.png","size":250,"type":"image","urlDownload":"https://outside.invalid/p.png"},
                    {"id":73,"name":"not-raster.png","size":250,"type":"image","urlDownload":"/disk/downloadFile/73/"},
                    {"id":74,"name":"Screenshot","extension":"png","size":250,"type":"file",
                     "mediaUrl":{"preview":{"250":"/disk/showFile/74/"}}},
                    {"id":75,"name":"attached.png","size":250,"type":"image",
                     "urlDownload":"/bitrix/services/main/ajax.php?action=disk.attachedObject.download&attachedObjectId=75"},
                    {"id":76,"name":"attached-uf.png","size":250,"type":"image",
                     "urlDownload":"/bitrix/tools/disk/uf.php?action=download&attachedId=76"}
                ]})");
        if (request.url.path() == QLatin1String("/disk/downloadFile/73/"))
            return {"not a raster", "200 OK", {}, "application/octet-stream"};
        if (request.url.path() == QLatin1String("/disk/showFile/74/"))
            return {png, "200 OK", {}, "image/png"};
        return {png, "200 OK", {}, "application/octet-stream"};
    };

    CloudBackend backend(fixture.session(), true);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    QSignalSpy taskHistory(&backend, &Backend::taskMessagesLoaded);
    QSignalSpy fetched(&backend, &Backend::messageImageLoaded);
    backend.start();
    backend.requestChatMessages(QStringLiteral("chat321"));
    QTRY_COMPARE(history.size(), 1);
    backend.requestTaskMessages(QStringLiteral("42"));
    QTRY_COMPARE(taskHistory.size(), 1);
    for (const auto &response : {history.first(), taskHistory.first()}) {
        const auto messages = response.at(1).value<QVector<Message>>();
        QCOMPARE(messages.size(), 1);
        const auto files = messages.first().files;
        QCOMPARE(files.size(), 12);
        QCOMPARE(files.first().size, qint64(53350));
        for (const int row : {0, 1, 2, 8}) {
            QVERIFY(files.at(row).isImage);
            QVERIFY(!files.at(row).previewUrl.isEmpty());
            QCOMPARE(files.at(row).previewUrl, files.at(row).downloadUrl);
        }
        for (const int row : {3, 4, 5, 6, 7, 10, 11})
            QVERIFY(files.at(row).previewUrl.isEmpty());
        QVERIFY(files.at(9).isImage);
        QCOMPARE(files.at(9).previewUrl.path(), QStringLiteral("/disk/showFile/74/"));
    }

    const auto files = history.first().at(1).value<QVector<Message>>().first().files;
    QVERIFY(fixture.matching(QStringLiteral("/disk/downloadFile/65/")).isEmpty());
    for (const int row : {0, 1, 2, 8, 9}) {
        const auto expected = fetched.size() + 1;
        backend.requestMessageImage(files.at(row).previewUrl);
        QTRY_COMPARE(fetched.size(), expected);
        QCOMPARE(fetched.last().first().toUrl(), files.at(row).previewUrl);
        QCOMPARE(fetched.last().last().toByteArray(), row == 8 ? QByteArray() : png);
    }
    for (const int row : {4, 10, 11}) {
        QVERIFY(!files.at(row).downloadUrl.isEmpty());
        const auto expected = fetched.size() + 1;
        backend.requestMessageImage(files.at(row).downloadUrl);
        QCOMPARE(fetched.size(), expected);
        QCOMPARE(fetched.last().first().toUrl(), files.at(row).downloadUrl);
        QVERIFY(fetched.last().last().toByteArray().isEmpty());
    }
    QVERIFY(fixture.matching(QStringLiteral("/disk/downloadFile/69/")).isEmpty());
    QVERIFY(fixture.matching(QStringLiteral("disk.attachedObject.download")).isEmpty());
    QVERIFY(fixture.matching(QStringLiteral("/bitrix/tools/disk/uf.php")).isEmpty());
}

void TestCloudChannels::previewsNativeResizeAndDeclaredOriginals_data()
{
    QTest::addColumn<bool>("nativeResize");
    QTest::addColumn<bool>("largeOriginal");
    QTest::newRow("download-alias-prefers-native-resize") << true << true;
    QTest::newRow("native-resize-with-bounded-original") << true << false;
    QTest::newRow("declared-generic-image-without-suffix") << false << false;
    QTest::newRow("download-alias-over-limit") << false << true;
}

void TestCloudChannels::previewsNativeResizeAndDeclaredOriginals()
{
    QFETCH(bool, nativeResize);
    QFETCH(bool, largeOriginal);
    const auto preview = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aRYAAAAAASUVORK5CYII=");
    auto original = preview;
    if (largeOriginal)
        original.append(QByteArray(512 * 1024, 'x'));
    const auto downloadAction = QStringLiteral("disk.api.file.download");
    const auto resizeAction = QStringLiteral("disk.api.file.showImage");
    const auto download = QStringLiteral("/bitrix/services/main/ajax.php?action=disk.api.file.download&fileId=77&exact=N");
    const auto resized = QStringLiteral("/bitrix/services/main/ajax.php?action=disk.api.file.showImage&fileId=77"
                                        "&width=1280&height=1280&signature=synthetic");

    Fixture fixture;
    QVERIFY(fixture.listen());
    fixture.responder = [&](const Request &request) -> Response {
        if (request.action() == loadAction) {
            QJsonObject file{{"id", 77}, {"name", "pic_179553001"}, {"size", original.size()},
                             {"type", "image"}, {"mimeType", "application/octet-stream"}, {"urlDownload", download}};
            if (largeOriginal) {
                // The portal can report the same original in both native preview fields.
                file.insert("urlPreview", fixture.session().origin.resolved(QUrl(download)).toString());
                file.insert("mediaUrl", QJsonObject{{"preview", QJsonObject{{"250", download}}}});
            }
            if (nativeResize)
                file.insert("urlShow", resized);
            return ajax(QJsonDocument(QJsonObject{
                {"chat", QJsonObject{{"id", 321}}},
                {"messages", QJsonArray{QJsonObject{{"id", 51}, {"files", QJsonArray{77}}}}},
                {"files", QJsonArray{file}}
            }).toJson(QJsonDocument::Compact));
        }
        if (request.action() == resizeAction)
            return {preview, "200 OK", {}, "image/png"};
        if (request.action() == downloadAction)
            return {original, "200 OK", {}, "application/octet-stream"};
        return ajax("{}");
    };

    CloudBackend backend(fixture.session(), true);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    QSignalSpy fetched(&backend, &Backend::messageImageLoaded);
    QSignalSpy saved(&backend, &Backend::messageFileDownloaded);
    backend.start();
    backend.requestChatMessages(QStringLiteral("chat321"));
    QTRY_COMPARE(history.size(), 1);
    const auto messages = history.first().at(1).value<QVector<Message>>();
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().files.size(), 1);
    const auto photo = messages.first().files.first();
    QVERIFY(photo.isImage);
    QCOMPARE(photo.downloadUrl, fixture.session().origin.resolved(QUrl(download)));
    QCOMPARE(photo.originalPreviewUrl, nativeResize && !largeOriginal ? photo.downloadUrl : QUrl());
    if (nativeResize || !largeOriginal) {
        QCOMPARE(photo.previewUrl, fixture.session().origin.resolved(QUrl(nativeResize ? resized : download)));
        backend.requestMessageImage(photo.previewUrl);
        QTRY_COMPARE(fetched.size(), 1);
        QCOMPARE(fetched.first().at(1).toByteArray(), preview);
    } else {
        QVERIFY(photo.previewUrl.isEmpty());
        backend.requestMessageImage(photo.downloadUrl);
        QCOMPARE(fetched.size(), 1);
        QVERIFY(fetched.first().at(1).toByteArray().isEmpty());
    }
    QCOMPARE(fixture.matching(resizeAction).size(), nativeResize ? 1 : 0);
    QCOMPARE(fixture.matching(downloadAction).size(), !nativeResize && !largeOriginal ? 1 : 0);
    if (nativeResize && !largeOriginal) {
        backend.requestMessageImage(photo.downloadUrl);
        QTRY_COMPARE(fetched.size(), 2);
        QCOMPARE(fetched.last().at(1).toByteArray(), original);
        QCOMPARE(fixture.matching(downloadAction).size(), 1);
    }

    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const auto path = temp.filePath(QStringLiteral("photo.png"));
    backend.downloadMessagePhoto(photo.downloadUrl, path);
    QTRY_COMPARE(saved.size(), 1);
    QFile downloaded(path);
    QVERIFY(downloaded.open(QIODevice::ReadOnly));
    QCOMPARE(downloaded.readAll(), original);
}

void TestCloudChannels::reservesStandbyOriginalsWithoutEvictingPreferredImages()
{
    const auto png = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aRYAAAAAASUVORK5CYII=");
    Fixture fixture;
    QVERIFY(fixture.listen());
    fixture.responder = [&](const Request &request) -> Response {
        if (request.action() == loadAction) {
            QJsonArray messages, files;
            for (int message = 0; message < 3; ++message) {
                QJsonArray attachmentIds;
                for (int index = 0; index < 43; ++index) {
                    const auto id = message * 43 + index + 1;
                    files.append(QJsonObject{{"id", id}, {"name", "photo.png"}, {"size", png.size()}, {"type", "image"},
                        {"urlShow", QStringLiteral("/disk/showFile/%1/").arg(id)},
                        {"urlDownload", QStringLiteral("/disk/downloadFile/%1/").arg(id)}});
                    attachmentIds.append(id);
                }
                messages.append(QJsonObject{{"id", 1000 + message}, {"files", attachmentIds}});
            }
            return ajax(QJsonDocument(QJsonObject{
                {"chat", QJsonObject{{"id", 321}}}, {"messages", messages}, {"files", files}
            }).toJson(QJsonDocument::Compact));
        }
        return {png, "200 OK", {}, "image/png"};
    };

    CloudBackend backend(fixture.session(), true);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    QSignalSpy fetched(&backend, &Backend::messageImageLoaded);
    backend.start();
    backend.requestChatMessages(QStringLiteral("chat321"));
    QTRY_COMPARE(history.size(), 1);
    const auto messages = history.first().at(1).value<QVector<Message>>();
    QCOMPARE(messages.size(), 3);
    for (const auto &message : messages)
        QCOMPARE(message.files.size(), 43);
    const auto first = messages.first().files.first();
    QCOMPARE(first.previewUrl.path(), QStringLiteral("/disk/showFile/1/"));
    QCOMPARE(first.originalPreviewUrl, first.downloadUrl);
    QCOMPARE(fixture.matching(first.previewUrl.path()).size(), 0);
    QCOMPARE(fixture.matching(first.originalPreviewUrl.path()).size(), 0);

    backend.requestMessageImage(first.previewUrl);
    QTRY_COMPARE(fetched.size(), 1);
    QCOMPARE(fetched.first().at(1).toByteArray(), png);
    QCOMPARE(fixture.matching(first.previewUrl.path()).size(), 1);
    QCOMPARE(fixture.matching(first.originalPreviewUrl.path()).size(), 0);

    backend.requestMessageImage(first.originalPreviewUrl);
    QTRY_COMPARE(fetched.size(), 2);
    QCOMPARE(fetched.last().at(1).toByteArray(), png);
    QCOMPARE(fixture.matching(first.originalPreviewUrl.path()).size(), 1);
}

void TestCloudChannels::linkPreviewsRequireAnAuthorizedMatchingPortalObject()
{
    Fixture fixture;
    QVERIFY(fixture.listen());
    fixture.responder = [](const Request &request) {
        if (request.action() == QStringLiteral("tasks.task.get")) {
            if (request.field(QStringLiteral("taskId")) == QStringLiteral("42")) {
                return ajax(R"({"task":{"id":"42","title":"Printer","status":"2","description":"[B]Replace[/B]",
                    "creator":{"name":"Anna"},"responsible":{"name":"Max"},
                    "groupId":"7","group":{"id":"9","name":"Old project"}}})");
            }
            if (request.field(QStringLiteral("taskId")) == QStringLiteral("43"))
                return Response{"{\"status\":\"error\"}", "403 Forbidden", {}};
            if (request.field(QStringLiteral("taskId")) == QStringLiteral("45"))
                return ajax(R"({"task":{"id":"45","title":"Limited access","groupId":"8"}})");
            if (request.field(QStringLiteral("taskId")) == QStringLiteral("46"))
                return ajax(R"({"task":{"id":"46","title":"No project","groupId":"0",
                    "group":{"id":"9","name":"Old project"}}})");
            return ajax(R"({"task":{"id":"42","title":"Wrong ID"}})");
        }
        if (request.action() == QStringLiteral("socialnetwork.api.workgroup.get"))
            return Response{R"({"status":"error","errors":[{"code":"SONET_CONTROLLER_WORKGROUP_EMPTY"}]})", "200 OK", {}};
        if (request.action() == QStringLiteral("socialnetwork.api.workgroup.list")) {
            const auto id = request.field(QStringLiteral("filter[ID]"));
            if (id == QStringLiteral("7"))
                return ajax(R"({"workgroups":[{"id":7,"name":"IT","description":"Maintenance",
                    "avatarType":null,"avatar":"/b42/resize_cache/project.png"}]})");
            if (id == QStringLiteral("10"))
                return ajax(R"({"workgroups":[{"id":10,"name":"Operations","description":"Help desk",
                    "avatarType":"folder","avatar":"/bitrix/images/socialnetwork/project/folder.png"}]})");
            return ajax(R"({"workgroups":[
                {"ID":9,"id":8,"name":"Wrong project","avatar":"/upload/other.png"},
                {"ID":8,"id":9,"name":"Also wrong","avatar":"/upload/other.png"}]})");
        }
        if (request.url.path() == QStringLiteral("/rest/im.user.get.json"))
            return result(R"({"id":19,"name":"Anna","workPosition":"Engineer","avatar":"/upload/person.png"})");
        if (request.action() == loadAction) {
            const auto dialog = request.field(QStringLiteral("dialogId"));
            if (dialog == QStringLiteral("chat321"))
                return ajax(R"({"chat":{"id":321,"dialogId":"chat321","name":"Discussion","avatar":"/upload/chat.png"},"messages":[]})");
            if (dialog == QStringLiteral("chat322"))
                return ajax(R"({"chat":{"id":322,"dialogId":"chat321","name":"Wrong dialog","avatar":"/upload/other.png"}})");
            return Response{"{\"status\":\"error\"}", "401 Unauthorized", {}};
        }
        return result("{}");
    };
    CloudBackend backend(fixture.session(), true);
    QSignalSpy rejected(&backend, &CloudBackend::sessionRejected);
    backend.start();

    const auto portal = fixture.session().origin;
    const auto requestPreview = [&backend, &portal](const QString &path) {
        auto preview = std::make_shared<std::optional<MessageLinkPreview>>();
        backend.requestMessageLinkPreview(portal.resolved(QUrl(path)),
            [preview](MessageLinkPreview data) { preview->emplace(std::move(data)); });
        return preview;
    };
    const auto allowed = requestPreview(QStringLiteral("/workgroups/group/9/tasks/task/view/42/"));
    QTRY_VERIFY(allowed->has_value());
    QCOMPARE((*allowed)->title, QStringLiteral("Printer"));
    QCOMPARE((*allowed)->kind, MessageLinkPreviewKind::Task);
    QCOMPARE((*allowed)->details.size(), 5);
    QVERIFY((*allowed)->details.at(3).endsWith(QStringLiteral("Replace")));
    QCOMPARE((*allowed)->details.at(4), QStringLiteral("Проект · IT"));
    const auto projectAction = QStringLiteral("socialnetwork.api.workgroup.list");
    const auto taskProjects = fixture.matching(projectAction);
    QCOMPARE(taskProjects.size(), 1);
    QCOMPARE(taskProjects.front().field(QStringLiteral("filter[ID]")), QStringLiteral("7"));
    QVERIFY(taskProjects.front().field(QStringLiteral("params[IS_ADMIN]")).isEmpty());
    QCOMPARE(taskProjects.front().headers.value("cookie"), fixture.session().cookie);
    QVERIFY((*allowed)->project.has_value());
    QVERIFY((*allowed)->project->avatarType.isEmpty());
    QCOMPARE((*allowed)->imageUrl(), portal.resolved(QUrl(QStringLiteral("/b42/resize_cache/project.png"))));

    const auto denied = requestPreview(QStringLiteral("/tasks/task/view/43/"));
    const auto mismatched = requestPreview(QStringLiteral("/tasks/task/view/44/"));
    QTRY_VERIFY(denied->has_value() && mismatched->has_value());
    QVERIFY((*denied)->title.isEmpty());
    QVERIFY((*mismatched)->title.isEmpty());
    QCOMPARE(rejected.size(), 0);
    auto taskRequests = fixture.matching(QStringLiteral("tasks.task.get"));
    QCOMPARE(taskRequests.size(), 3);
    QCOMPARE(taskRequests.front().headers.value("cookie"), fixture.session().cookie);
    QCOMPARE(taskRequests.front().headers.value("x-bitrix-csrf-token"), fixture.session().csrf);

    const auto project = requestPreview(QStringLiteral("/workgroups/group/7/"));
    const auto profile = requestPreview(QStringLiteral("/company/personal/user/19/"));
    QTRY_VERIFY(project->has_value() && profile->has_value());
    QCOMPARE((*project)->title, QStringLiteral("IT"));
    QCOMPARE((*project)->kind, MessageLinkPreviewKind::Project);
    QCOMPARE((*project)->details, QStringList{QStringLiteral("Maintenance")});
    QCOMPARE((*project)->imageUrl(), (*allowed)->imageUrl());
    QCOMPARE((*profile)->title, QStringLiteral("Anna"));
    QCOMPARE((*profile)->kind, MessageLinkPreviewKind::Person);
    QCOMPARE((*profile)->avatarUrl, portal.resolved(QUrl(QStringLiteral("/upload/person.png"))));

    const auto chat = requestPreview(QStringLiteral("/online/?IM_DIALOG=chat321"));
    const auto wrongChat = requestPreview(QStringLiteral("/online/?IM_DIALOG=chat322"));
    const auto deniedChat = requestPreview(QStringLiteral("/online/?IM_DIALOG=chat323"));
    QTRY_VERIFY(chat->has_value() && wrongChat->has_value() && deniedChat->has_value());
    QCOMPARE((*chat)->title, QStringLiteral("Discussion"));
    QCOMPARE((*chat)->kind, MessageLinkPreviewKind::Chat);
    QCOMPARE((*chat)->avatarUrl, portal.resolved(QUrl(QStringLiteral("/upload/chat.png"))));
    QVERIFY((*wrongChat)->title.isEmpty() && (*deniedChat)->title.isEmpty());
    QVERIFY((*wrongChat)->avatarUrl.isEmpty() && (*deniedChat)->avatarUrl.isEmpty());
    QCOMPARE(rejected.size(), 0);

    const auto unsupported = requestPreview(QStringLiteral("/unsupported/private"));
    const auto external = requestPreview(QStringLiteral("https://example.invalid/tasks/task/view/42/"));
    const auto messageLink = requestPreview(QStringLiteral("/online/?IM_DIALOG=chat321&IM_MESSAGE=52"));
    QVERIFY(unsupported->has_value() && external->has_value() && messageLink->has_value());
    QVERIFY((*unsupported)->title.isEmpty() && (*external)->title.isEmpty() && (*messageLink)->title.isEmpty());
    QCOMPARE(fixture.matching(QStringLiteral("tasks.task.get")).size(), 3);
    QCOMPARE(fixture.matching(loadAction).size(), 3);
    QVERIFY(fixture.matching(QStringLiteral("/unsupported/private")).isEmpty());

    const auto deniedProject = requestPreview(QStringLiteral("/workgroups/group/7/tasks/task/view/45/"));
    const auto noProject = requestPreview(QStringLiteral("/workgroups/group/7/tasks/task/view/46/"));
    QTRY_VERIFY(deniedProject->has_value() && noProject->has_value());
    QCOMPARE((*deniedProject)->title, QStringLiteral("Limited access"));
    QCOMPARE((*noProject)->title, QStringLiteral("No project"));
    QVERIFY(!(*deniedProject)->project && !(*noProject)->project);
    QVERIFY((*noProject)->details.isEmpty());
    QCOMPARE(fixture.matching(projectAction).size(), 3);

    const auto systemIcon = requestPreview(QStringLiteral("/workgroups/group/10/"));
    QTRY_VERIFY(systemIcon->has_value());
    QVERIFY((*systemIcon)->project.has_value());
    QCOMPARE((*systemIcon)->project->avatarType, QStringLiteral("folder"));
    QVERIFY((*systemIcon)->imageUrl().isEmpty());
    QCOMPARE(fixture.matching(projectAction).size(), 4);
    QVERIFY(fixture.matching(QStringLiteral("socialnetwork.api.workgroup.get")).isEmpty());
}

void TestCloudChannels::decodesPortalExternalLinkMetadata()
{
    Fixture fixture;
    QVERIFY(fixture.listen());
    fixture.responder = [](const Request &request) {
        if (request.url.path() == QStringLiteral("/rest/im.chat.url.get.json"))
            return result(R"({"list":[
                {"id":5,"chatId":73,"messageId":17,"url":{"source":"https://example.test/article",
                    "richData":{"name":"A real article","description":"The actual description",
                                "previewUrl":"/upload/article.png"}}},
                {"id":6,"chatId":73,"messageId":18,"url":{"source":"https://example.test/another",
                    "richData":{"name":"Another article","previewUrl":"https://outside.invalid/pixel.png"}}},
                {"id":7,"chatId":74,"messageId":17,"url":{"source":"https://example.test/article",
                    "richData":{"name":"Another room"}}}
            ]})");
        return result("{}");
    };
    CloudBackend backend(fixture.session(), true);
    backend.start();
    auto loaded = std::make_shared<std::optional<QVector<ChatAsset>>>();
    backend.requestChatLinkPreviews(QStringLiteral("chat73"), false,
        [loaded](QVector<ChatAsset> items) { loaded->emplace(std::move(items)); });
    QTRY_VERIFY(loaded->has_value());
    const auto &items = **loaded;
    QCOMPARE(items.size(), 2);
    QCOMPARE(items[0].messageId, QStringLiteral("17"));
    QCOMPARE(items[0].name, QStringLiteral("A real article"));
    QCOMPARE(items[0].description, QStringLiteral("The actual description"));
    QCOMPARE(items[0].thumbnailUrl, fixture.session().origin.resolved(QUrl(QStringLiteral("/upload/article.png"))));
    QVERIFY(items[1].thumbnailUrl.isEmpty());
}

void TestCloudChannels::enrichesTaskLinkPreviewWithCurrentStage()
{
    Fixture fixture;
    QVERIFY(fixture.listen());
    fixture.responder = [](const Request &request) {
        if (request.action() == QStringLiteral("tasks.task.get")) {
            const auto id = request.field(QStringLiteral("taskId"));
            return ajax(QByteArrayLiteral("{\"task\":{\"id\":") + id.toUtf8()
                + QByteArrayLiteral(R"(,"title":"Printer","groupId":7,"stageId":11,"status":2,
                    "createdBy":19,"creator":{"id":19,"name":"Anna","icon":"/upload/creator.png"},
                    "responsibleId":20,"responsible":{"id":21,"name":"Max","icon":"/upload/wrong.png"}}})"));
        }
        if (request.action() == QStringLiteral("socialnetwork.api.workgroup.list"))
            return ajax(R"({"workgroups":[{"id":7,"name":"IT","avatarType":"folder"}]})");
        if (request.action() == QStringLiteral("tasks.v2.Group.Stage.list"))
            return ajax(R"([{"id":10,"title":"Wrong stage","color":"#f00000"},
                            {"id":11,"title":"Queue","color":"#8d4d77"}])");
        return result("{}");
    };
    CloudBackend backend(fixture.session(), true);
    backend.start();
    const auto portal = fixture.session().origin;
    const auto makePreview = [&backend, &portal](int id) {
        auto value = std::make_shared<std::optional<MessageLinkPreview>>();
        backend.requestMessageLinkPreview(portal.resolved(QUrl(QStringLiteral("/tasks/task/view/%1/").arg(id))),
            [value](MessageLinkPreview preview) { value->emplace(std::move(preview)); });
        return value;
    };
    const auto first = makePreview(41);
    const auto second = makePreview(42);
    QTRY_VERIFY(first->has_value() && second->has_value());
    QVERIFY(first->value().details.contains(QStringLiteral("Статус · Ждёт выполнения")));
    QVERIFY(first->value().details.contains(QStringLiteral("Стадия · Queue")));
    QVERIFY(second->value().details.contains(QStringLiteral("Стадия · Queue")));
    QCOMPARE(first->value().stageColor, QStringLiteral("8d4d77"));
    QCOMPARE(first->value().creatorAvatarUrl, portal.resolved(QUrl(QStringLiteral("/upload/creator.png"))));
    QVERIFY(first->value().assigneeAvatarUrl.isEmpty());
    QCOMPARE(fixture.matching(QStringLiteral("tasks.v2.Group.Stage.list")).size(), 1);
}

void TestCloudChannels::loadsOnlyViewersOfTheExactOutgoingMessage()
{
    Fixture fixture;
    QVERIFY(fixture.listen());
    bool wrongResponse = false;
    bool member = true;
    bool wrongHistory = false;
    fixture.responder = [&](const Request &request) {
        if (request.action() == QStringLiteral("main.bitrix.main.controller.loadext.resolveextensions"))
            return ajax(R"([{"extension":"socialnetwork.v2.current-user","settings":{"id":9}}])");
        if (request.action() == loadAction) {
            if (wrongHistory)
                return ajax(R"({"chatId":74,"chat":{"id":73,"dialogId":"chat73","role":"member"},
                    "messages":[{"id":81,"authorId":9,"text":"Wrong envelope","viewedByOthers":true}]})");
            const QByteArray role = member ? "member" : "none";
            return ajax(QByteArrayLiteral(R"({"chat":{"id":73,"dialogId":"chat73","role":")") + role
                + QByteArrayLiteral(R"(","lastMessageViews":{"messageId":71,"countOfViewers":4}},
                    "messages":[{"id":71,"chatId":73,"authorId":9,"text":"My message","viewedByOthers":true},
                                {"id":72,"chatId":73,"authorId":18,"text":"Other message"}]})"));
        }
        if (request.action() == QStringLiteral("tasks.task.get"))
            return ajax(R"({"task":{"id":42,"title":"Task","chatId":73}})");
        if (request.url.path() == QStringLiteral("/rest/im.v2.Chat.Message.tailViewers.json")) {
            if (wrongResponse)
                return result(R"({"views":[{"id":1,"messageId":81,"userId":18}],"users":[]})");
            return result(R"({"views":[{"id":1,"messageId":71,"userId":18},
                {"id":2,"messageId":71,"userId":18},{"id":3,"messageId":71,"userId":9}],
                "users":[{"id":18,"name":"Anna","avatar":"/upload/anna.png"},{"id":25,"name":"Unrelated user"}]})");
        }
        return result("{}");
    };
    CloudBackend backend(fixture.session(), true);
    backend.start();
    const auto call = [&backend](const QString &scope, const QString &id, bool task) {
        auto reply = std::make_shared<std::optional<std::optional<MessageViewers>>>();
        backend.requestMessageViewers(scope, id, task, [reply](std::optional<MessageViewers> value) {
            reply->emplace(std::move(value));
        });
        return reply;
    };
    const auto path = QStringLiteral("/rest/im.v2.Chat.Message.tailViewers.json");
    const auto dialog = QStringLiteral("chat73");
    const auto message = QStringLiteral("71");
    QVERIFY(!call(dialog, message, false)->value());
    QSignalSpy loaded(&backend, &CloudBackend::chatMessagesLoaded);
    backend.requestChatMessages(dialog);
    QTRY_COMPARE(loaded.size(), 1);
    QVERIFY(!call(dialog, QStringLiteral("72"), false)->value());
    QVERIFY(fixture.matching(path).isEmpty());
    const auto viewers = call(dialog, message, false);
    QTRY_VERIFY(viewers->has_value());
    QVERIFY(viewers->value());
    QCOMPARE(viewers->value()->people.size(), 1);
    QCOMPARE(viewers->value()->people[0].name, QStringLiteral("Anna"));
    QCOMPARE(viewers->value()->people[0].avatarUrl, fixture.session().origin.resolved(QUrl(QStringLiteral("/upload/anna.png"))));
    QCOMPARE(viewers->value()->count, 4);
    QCOMPARE(fixture.matching(path).first().field(QStringLiteral("id")), message);
    const auto taskId = QStringLiteral("42");
    QSignalSpy taskLoaded(&backend, &CloudBackend::taskMessagesLoaded);
    backend.requestTaskMessages(taskId);
    QTRY_COMPARE(taskLoaded.size(), 1);
    wrongResponse = true;
    const auto rejected = call(taskId, message, true);
    QTRY_VERIFY(rejected->has_value());
    QVERIFY(!rejected->value());
    QCOMPARE(fixture.matching(path).size(), 2);
    wrongHistory = true;
    backend.requestChatMessages(dialog);
    QTRY_COMPARE(loaded.size(), 2);
    const auto mismatchedHistory = call(dialog, QStringLiteral("81"), false);
    QTRY_VERIFY(mismatchedHistory->has_value());
    QVERIFY(!mismatchedHistory->value());
    QCOMPARE(fixture.matching(path).size(), 2);
    wrongHistory = false;
    member = false;
    backend.requestChatMessages(dialog);
    QTRY_COMPARE(loaded.size(), 3);
    QVERIFY(!call(dialog, message, false)->value());
    QCOMPARE(fixture.matching(path).size(), 2);
}

void TestCloudChannels::streamsOriginalPhotoAndCancelsIndependently()
{
    const auto original = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aRYAAAAAASUVORK5CYII=");
    const auto filePath = [](int id) { return QStringLiteral("/disk/downloadFile/%1/").arg(id); };
    Fixture fixture;
    QVERIFY(fixture.listen());
    fixture.responder = [&](const Request &request) -> Response {
        if (request.action() == loadAction) {
            return ajax(R"({"chat":{"id":321},"messages":[{"id":51,"files":[55,56,57,58,59,60]}],"files":[
                {"id":55,"name":"photo.png","type":"image","urlDownload":"/disk/downloadFile/55/"},
                {"id":56,"name":"in-progress.png","type":"image","urlDownload":"/disk/downloadFile/56/"},
                {"id":57,"name":"notes.txt","type":"file","urlDownload":"/disk/downloadFile/57/"},
                {"id":58,"name":"too-large.png","type":"image","size":25165825,"urlDownload":"/disk/downloadFile/58/"},
                {"id":59,"name":"unknown-size.png","type":"image","urlDownload":"/disk/downloadFile/59/"},
                {"id":60,"name":"wrong-type.png","type":"image","urlDownload":"/disk/downloadFile/60/"}
            ]})");
        }
        if (request.url.path() == filePath(55))
            return {original, "200 OK", {}, "image/png"};
        if (request.url.path() == filePath(56))
            return {original.left(16), "200 OK", {}, "image/png", original.size(), true};
        if (request.url.path() == filePath(57))
            return {"ordinary", "200 OK", {}, "text/plain"};
        if (request.url.path() == filePath(59))
            return {original.left(16), "200 OK", {}, "image/png", 24 * 1024 * 1024 + 1, true};
        if (request.url.path() == filePath(60))
            return {R"({"error":"INVALID"})", "200 OK", {}, "application/json"};
        return ajax("{}");
    };
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    CloudBackend backend(fixture.session(), true);
    QSignalSpy history(&backend, &Backend::chatMessagesLoaded);
    QSignalSpy done(&backend, &Backend::messageFileDownloaded);
    QSignalSpy failures(&backend, &Backend::messagePhotoDownloadFailed);
    QSignalSpy ordinaryFailures(&backend, &Backend::failed);
    backend.start();
    backend.requestChatMessages(QStringLiteral("chat321"));
    QTRY_COMPARE(history.size(), 1);
    const auto files = history.first().at(1).value<QVector<Message>>().first().files;
    QCOMPARE(files.size(), 6);
    QCOMPARE(fixture.matching(filePath(55)).size(), 0);

    const auto photoPath = dir.filePath(QStringLiteral("original.png"));
    backend.downloadMessagePhoto(files.at(0).downloadUrl, photoPath);
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(done.first().first().toUrl(), files.at(0).downloadUrl);
    QCOMPARE(done.first().last().toString(), photoPath);
    QFile downloaded(photoPath);
    QVERIFY(downloaded.open(QIODevice::ReadOnly));
    QCOMPARE(downloaded.readAll(), original);

    const auto pendingPath = dir.filePath(QStringLiteral("cancel.png"));
    backend.downloadMessagePhoto(files.at(1).downloadUrl, pendingPath);
    QTRY_COMPARE(fixture.matching(filePath(56)).size(), 1);
    backend.cancelMessagePhoto(pendingPath);

    const auto ordinaryPath = dir.filePath(QStringLiteral("save.txt"));
    backend.downloadMessageFile(files.at(2).downloadUrl, ordinaryPath);
    QTRY_COMPARE(done.size(), 2);
    QFile saved(ordinaryPath);
    QVERIFY(saved.open(QIODevice::ReadOnly));
    QCOMPARE(saved.readAll(), QByteArray("ordinary"));
    QVERIFY(!QFileInfo::exists(pendingPath));
    QCOMPARE(failures.size(), 0);

    backend.downloadMessagePhoto(files.at(2).downloadUrl, pendingPath);
    QCOMPARE(failures.size(), 1);
    QCOMPARE(failures.last().first().toString(), pendingPath);
    QCOMPARE(fixture.matching(filePath(57)).size(), 1);

    backend.downloadMessagePhoto(files.at(3).downloadUrl, pendingPath);
    QCOMPARE(failures.size(), 2);
    QCOMPARE(failures.last().first().toString(), pendingPath);
    QCOMPARE(fixture.matching(filePath(58)).size(), 0);

    backend.downloadMessagePhoto(files.at(4).downloadUrl, pendingPath);
    QTRY_COMPARE(failures.size(), 3);
    QCOMPARE(failures.last().first().toString(), pendingPath);
    QCOMPARE(fixture.matching(filePath(59)).size(), 1);
    backend.downloadMessagePhoto(files.at(5).downloadUrl, pendingPath);
    QTRY_COMPARE(failures.size(), 4);
    QCOMPARE(failures.last().first().toString(), pendingPath);
    QCOMPARE(done.size(), 2);
    QVERIFY(!QFileInfo::exists(pendingPath));
    const QStringList committedFiles{QStringLiteral("original.png"), QStringLiteral("save.txt")};
    QTRY_COMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden), committedFiles);
    QVERIFY(std::none_of(ordinaryFailures.cbegin(), ordinaryFailures.cend(), [](const auto &entry) {
        return entry.first().toString() == QLatin1String("message-file-download");
    }));
}

QTEST_GUILESS_MAIN(TestCloudChannels)
#include "tst_cloud_channels.moc"
