#include "core/demo_backend.h"
#include "ui/board_widget.h"
#include "ui/chat_photo_viewer.h"
#include "ui/main_window.h"
#include "ui/portal_dnd_control.h"
#include "ui/reaction_palette.h"
#include "ui/task_dialogs.h"
#include "ui/typography.h"
#include "ui/worktime_panel.h"
#include "ui/workspace_presentation.h"
#include "ui/workspace_style.h"

#include <QApplication>
#include <QTemporaryDir>
#include <QTabBar>
#include <QTabWidget>
#include <QSplitter>
#include <QSettings>
#include <QScreen>
#include <QMenu>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QColorDialog>
#include <QContextMenuEvent>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QScrollArea>
#include <QScrollBar>
#include <QStandardPaths>
#include <QStyleOptionViewItem>
#include <QTest>
#include <QTemporaryFile>
#include <QTextDocument>
#include <QTextEdit>
#include <QTimer>
#include <QUrlQuery>
#include <QWindow>
#include <QWheelEvent>

#include <memory>
#include <tuple>

using namespace bx;

namespace {

template<typename Widget>
Widget *findAccessible(const QWidget *window, const QString &name)
{
    for (auto *child : window->findChildren<Widget *>()) {
        if (child->accessibleName() == name)
            return child;
    }
    return nullptr;
}

QMenu *visibleMessageMenu(const QString &name = QStringLiteral("messageContextMenu"))
{
    for (auto *window : QApplication::topLevelWidgets()) {
        if (window->isVisible() && window->objectName() == name)
            return qobject_cast<QMenu *>(window);
    }
    return nullptr;
}

} // namespace

class LocalFileOpener : public QObject {
    Q_OBJECT
public:
    LocalFileOpener() { QDesktopServices::setUrlHandler(QStringLiteral("file"), this, "open"); }
    ~LocalFileOpener() override { QDesktopServices::unsetUrlHandler(QStringLiteral("file")); }
    QList<QUrl> opened;

private slots:
    void open(const QUrl &url) { opened.append(url); }
};

// Fixture transport for native workspace interactions.

class FixtureBackend : public Backend {
    Q_OBJECT
public:
    explicit FixtureBackend(QObject *parent = nullptr) : Backend(parent) {}

    QString displayName() const override { return QStringLiteral("Тестовый портал"); }
    bool isDemo() const override { return demoMode; }
    QString settingsScope() const override { return accountScope; }
    QUrl portalOrigin() const override { return QUrl(QStringLiteral("https://example.bitrix24.ru")); }
    void start() override {}
    void stop() override {}

    void requestRecentChats() override
    {
        chatRefreshCalls.append(QStringLiteral("directory"));
        emit recentChatsLoaded(chats);
    }
    QStringList chatRefreshCalls;
    void requestTaskChats() override
    {
        ++taskChatRequests;
        if (!holdTaskChatDirectory)
            emit taskChatsLoaded(taskChats);
    }
    void requestTaskChatBadges() override
    {
        ++taskChatBadgeRequests;
        if (provideTaskBadges)
            emit taskChatsLoaded(taskChats);
    }
    int taskChatRequests = 0;
    bool holdTaskChatDirectory = false;
    int taskChatBadgeRequests = 0;
    bool provideTaskBadges = false;
    std::optional<qint64> unreadChatCount() const override { return railUnread; }
    std::optional<qint64> railUnread;
    void requestAvatar(const QUrl &url) override
    {
        avatarRequests.append(url);
        if (!holdAvatarResponses)
            emit avatarLoaded(url, avatarImages.value(url, avatarBytes));
    }
    void requestMessageImage(const QUrl &url) override
    {
        imageRequests.append(url);
        if (holdMessageImages)
            return;
        const auto bytes = messageImages.value(url);
        QTimer::singleShot(0, this, [this, url, bytes] { emit messageImageLoaded(url, bytes); });
    }
    bool holdMessageImages = false;
    void downloadMessageVideo(const QUrl &url, const QString &path) override
    {
        videoRequests.append({url, path});
    }
    void cancelMessageVideo(const QString &path) override
    {
        cancelledVideos.append(path);
    }
    QVector<QPair<QUrl, QString>> videoRequests;
    QStringList cancelledVideos;

    void requestMessageLinkPreview(const QUrl &url, LinkPreviewHandler complete) override
    {
        if (holdLinkPreviews)
            linkPreviewRequests.append({url, std::move(complete)});
        else
            complete({});
    }
    bool holdLinkPreviews = false;
    QVector<QPair<QUrl, LinkPreviewHandler>> linkPreviewRequests;
    void requestMessageViewers(const QString &scope, const QString &id, bool task, MessageViewersHandler complete) override
    {
        viewedReceipts.append({scope, id, task});
        if (holdViewerResponses)
            viewerReplies.append(std::move(complete));
        else
            complete(MessageViewers{{{QStringLiteral("19"), QStringLiteral("Анна Волкова"), {}, viewerAvatar}}, 1, false});
    }
    bool holdViewerResponses = false;
    QVector<MessageViewersHandler> viewerReplies;
    struct ViewedReceipt {
        QString scope;
        QString id;
        bool task;
    };
    QVector<ViewedReceipt> viewedReceipts;
    QUrl viewerAvatar{QStringLiteral("https://example.bitrix24.ru/upload/viewer.png")};
    void requestChatLinkPreviews(const QString &scope, bool task, ChatLinkPreviewHandler complete) override
    {
        if (holdExternalLinkPreviews)
            externalLinkPreviewRequests.append({qMakePair(scope, task), std::move(complete)});
        else
            complete({});
    }
    bool holdExternalLinkPreviews = false;
    QVector<QPair<QPair<QString, bool>, ChatLinkPreviewHandler>> externalLinkPreviewRequests;
    void setChatPinned(const QString &id, bool pinned) override
    {
        pinTarget = id;
        pinValue = pinned;
    }
    void setProjectPinned(const QString &id, bool pinned) override
    {
        pinTarget = id;
        pinValue = pinned;
    }
    void searchTaskChats(const QString &query) override { searchedTaskChats = query; }
    void requestChatMessages(const QString &chatId) override
    {
        chatRefreshCalls.append(chatId);
        if (chatId == QStringLiteral("c-1") || chatId == QStringLiteral("chat91"))
            emit chatMessagesLoaded(chatId, messages);
        else if (chatId == QStringLiteral("task-dialog"))
            emit chatMessagesLoaded(chatId, taskMessages);
        else if (chatId == QStringLiteral("chat42"))
            emit chatMessagesLoaded(chatId, linkedMessages);
    }
    void requestOlderChatMessages(const QString &scope, const QString &before, bool task) override
    {
        olderScope = scope;
        olderBefore = before;
        olderTask = task;
    }
    QString olderScope;
    QString olderBefore;
    bool olderTask = false;
    struct ReadCall {
        QString scope;
        QStringList incoming;
        QStringList visible;
        bool task;
    };
    void readChatMessages(const QString &dialog, const QStringList &incoming, const QStringList &visible) override
    {
        reads.append({dialog, incoming, visible, false});
    }
    void readTaskChatMessages(const QString &task, const QStringList &incoming, const QStringList &visible) override
    {
        reads.append({task, incoming, visible, true});
    }

    void requestPinnedMessages(const QString &dialog) override
    {
        pinnedRequests.append(dialog);
        emit pinnedMessagesLoaded(dialog, pinnedMessages.value(dialog));
    }

    void requestMessageContext(const QString &dialogId, const QString &messageId) override
    {
        contextDialog = dialogId;
        contextMessage = messageId;
    }
    void requestPerson(const QString &id) override
    {
        requestedPerson = id;
        personRequests.append(id);
    }
    QStringList personRequests;
    void requestCurrentPerson(PersonHandler complete) override
    {
        if (holdCurrentPerson)
            currentPersonRequests.append(std::move(complete));
        else
            complete(currentPerson);
    }
    Person currentPerson{QStringLiteral("19"), QStringLiteral("Виктория Сергеева"), {}, {}};
    bool holdCurrentPerson = false;
    QList<PersonHandler> currentPersonRequests;
    void downloadMessageFile(const QUrl &url, const QString &path) override
    {
        downloadedUrl = url;
        downloadedPath = path;
        if (!holdMessageFileDownload)
            emit messageFileDownloaded(url, path);
    }
    void downloadMessagePhoto(const QUrl &url, const QString &path) override
    {
        photoUrl = url;
        photoPath = path;
        ++photoDownloads;
    }
    void cancelMessagePhoto(const QString &path) override { photoCancels.append(path); }
    void requestChatAssets(const QString &scope, ChatAssetKind kind, const QString &cursor) override
    {
        emit chatAssetsLoaded(scope, kind, cursor, kind == ChatAssetKind::Media ? mediaAssets : QVector<ChatAsset>{}, {});
    }
    QVector<ChatAsset> mediaAssets;
    void requestProjects() override
    {
        ++projectRequests;
        emit projectsMoreAvailable(!moreProjects.isEmpty());
        emit projectsLoaded(projects);
    }
    void requestMoreProjects() override
    {
        ++projectMoreRequests;
        if (holdProjectMore)
            return;
        emit projectsMoreAvailable(false);
        emit projectsLoaded(projects + moreProjects);
    }
    void requestProjectTaskChats(const QString &projectId, int offset) override
    {
        projectTaskChatRequests.append({projectId, offset});
        if (!projectId.isEmpty() && !holdProjectTaskChats)
            emit projectTaskChatsLoaded(projectId, offset, {}, -1);
    }
    QList<QPair<QString, int>> projectTaskChatRequests;
    bool holdProjectTaskChats = false;
    void requestProjectStages(const QString &projectId) override
    {
        projectStageRequests.append(projectId);
        if (!holdProjectStages)
            emit projectStagesLoaded(projectId, projectStageChoices);
    }
    void requestProjectTimeline(const QString &projectId) override
    {
        timelineRequests.append(projectId);
        emit projectTimelineLoaded(projectId, timelineTasks, timelineLimited);
    }
    void requestMoreBoardTasks(const QString &projectId, const QString &stageId) override
    {
        boardPageRequests.append({projectId, stageId});
    }
    void requestProjectMembers(const QString &projectId) override
    {
        emit projectMembersLoaded(projectId, projectToolsFixture
            ? QVector<ProjectMember>{{QStringLiteral("11"), QStringLiteral("Анна"), {}, ProjectMemberRole::Owner}}
            : QVector<ProjectMember>{});
    }
    void requestProjectFiles(const QString &projectId, const QString &folderId, int) override
    {
        if (!projectToolsFixture) {
            emit projectFilesLoaded(projectId, folderId, {}, -1);
        } else if (folderId.isEmpty()) {
            const ProjectDiskItem folder{{QStringLiteral("77"), QStringLiteral("Общие материалы"), -1, {}}, true};
            emit projectFilesLoaded(projectId, folderId, {folder}, -1);
        } else {
            emit failed(QStringLiteral("project-files:%1:%2").arg(projectId, folderId), QStringLiteral("Доступ запрещён"));
        }
    }
    void requestProjectBoard(const QString &projectId) override
    {
        ++boardRequests;
        if (projectId == QStringLiteral("101") && !holdBoardRead)
            emit boardLoaded(projectId, stages);
    }
    void requestTask(const QString &taskId) override
    {
        taskReadRequests.append(taskId);
        if (holdTaskDetails)
            return;
        for (const auto &stage : stages) {
            for (const auto &task : stage.tasks) {
                if (task.id == taskId) {
                    emit taskLoaded(task);
                    return;
                }
            }
        }
    }
    void requestTaskMessages(const QString &taskId) override
    {
        taskChatReadRequests.append(taskId);
        if (taskId == QStringLiteral("501"))
            emit taskMessagesLoaded(taskId, taskMessages);
    }
    bool holdTaskDetails = false;
    QStringList taskChatReadRequests;
    void searchPeople(const QString &requestId, const QString &query, const QString &dialogId) override
    {
        lastPeopleRequestId = requestId;
        peopleRequests.append({query, dialogId});
        emit peopleFound(requestId, people);
    }
    void requestNotifications() override
    {
        ++noticeHistoryRequests;
        if (!holdNotifications)
            emit notificationsLoaded(notifications);
    }
    void requestOlderNotifications() override
    {
        ++olderNoticeRequests;
        emit notificationHistoryPageStateChanged(true, true);
    }
    void requestWorkday() override
    {
        ++workdayRequests;
        if (!holdWorkday)
            emit workdayLoaded(workdayStatus);
    }
    int noticeHistoryRequests = 0;
    int olderNoticeRequests = 0;
    int workdayRequests = 0;
    bool holdWorkday = false;
    WorkdayStatus workdayStatus;
    void searchNotifications(const QString &query) override { noticeQuery = query; }
    void markNotificationRead(const QString &id) override { noticeReadId = id; }
    void markNotificationsRead(const QStringList &ids) override { noticeReadBatches.append(ids); }
    QVector<QStringList> noticeReadBatches;
    void markAllNotificationsRead() override { ++noticeReadAll; }
    void deleteNotification(const QString &id) override { noticeDeleteId = id; }
    void requestNotificationSettings() override
    {
        ++noticeSettingsReads;
        if (noticeSettingsReadFails)
            emit failed(QStringLiteral("notification-settings"), QStringLiteral("Портал временно недоступен"));
        else
            emit notificationSettingsLoaded(noticeSettings);
    }
    void updateNotificationSetting(const QString &name, int value) override
    {
        noticeSettingWrites.append({name, value});
    }

    void sendChatMessage(const QString &chatId, const QString &text) override
    {
        ++chatSendRequests;
        sentChat = text;
        if (!holdMessageSends)
            emit messageSent(QStringLiteral("chat:") + chatId, QStringLiteral("m-1"));
    }
    void sendTaskMessage(const QString &taskId, const QString &text) override
    {
        ++taskSendRequests;
        sentTask = text;
        if (!holdMessageSends)
            emit messageSent(QStringLiteral("task:") + taskId, QStringLiteral("m-2"));
    }
    void sendChatReply(const QString &chatId, const QString &text, const QString &replyId) override
    {
        sentReplyScope = QStringLiteral("chat:") + chatId;
        sentReplyText = text;
        sentReplyId = replyId;
    }
    void sendTaskReply(const QString &taskId, const QString &text, const QString &replyId) override
    {
        sentReplyScope = QStringLiteral("task:") + taskId;
        sentReplyText = text;
        sentReplyId = replyId;
    }

    void updateTask(const QString &taskId, const TaskEdit &edit) override
    {
        ++taskUpdateRequests;
        lastUpdate = edit;
        updatedTask = taskId;
        if (!holdTaskUpdate)
            emit taskUpdated(taskId);
    }
    void deleteTask(const QString &taskId) override { deletedTasks.append(taskId); }
    QStringList deletedTasks;
    void createTask(const QString &projectId, const TaskEdit &edit) override
    {
        createdProject = projectId;
        lastCreate = edit;
        ++createTaskCalls;
        if (!holdTaskCreation)
            emit taskCreated(projectId, QStringLiteral("502"));
    }
    void createSubtask(const QString &parentId, const QString &projectId, const TaskEdit &edit) override
    {
        createdParent = parentId;
        createdProject = projectId;
        lastCreate = edit;
        ++createTaskCalls;
        if (!holdTaskCreation)
            emit subtaskCreated(parentId, projectId, QStringLiteral("502"));
    }
    void createProject(const QString &name) override
    {
        createdProjectName = name;
        projects.append({QStringLiteral("102"), name, {}});
        emit projectCreated(QStringLiteral("102"));
    }
    void moveTask(const QString &projectId, const QString &taskId, const QString &stageId) override
    {
        movedProject = projectId;
        movedStage = stageId;
        movedTask = taskId;
    }
    void saveStage(const QString &projectId, const QString &stageId, const QString &title,
                   const QString &afterStageId, const QString &color) override
    {
        savedStage = stageId;
        savedStageTitle = title;
        savedAfterStage = afterStageId;
        savedStageColor = color;
        if (!color.isEmpty()) {
            for (auto &stage : stages) {
                if (stage.id == stageId)
                    stage.color = color;
            }
        }
        emit boardUpdated(projectId);
    }
    void deleteStage(const QString &projectId, const QString &stageId) override
    {
        deletedStage = stageId;
        emit boardUpdated(projectId);
    }
    void searchChats(const QString &query) override { searchedChats = query; }
    void searchMessages(const QString &scopeId, const QString &query, bool task) override
    {
        searchedScope = scopeId;
        searchedQuery = query;
        searchedTask = task;
    }
    void searchProjectTasks(const QString &projectId, const QString &query) override
    {
        searchedProject = projectId;
        searchedTaskQuery = query;
    }
    void uploadChatFile(const QString &scopeId, const QString &localPath, bool task) override
    {
        uploadedPath = localPath;
        uploadedScope = scopeId;
        if (!holdUpload)
            emit fileUploaded(scopeId, task ? QStringLiteral("task-chat") : QStringLiteral("chat"));
    }
    void uploadChatImage(const QString &scopeId, const QString &localPath, bool task, const QString &caption,
                         std::shared_ptr<QTemporaryDir>) override
    {
        ++imageUploadRequests;
        uploadedCaption = caption;
        imageUploadForTask = task;
        uploadChatFile(scopeId, localPath, task);
    }
    void attachTaskFile(const QString &taskId, const QString &localPath) override
    {
        attachedPath = localPath;
        attachedTask = taskId;
        attachmentRequests.append({taskId, localPath});
        if (holdTaskAttachment)
            return;
        if (!attachmentEmbedId.isEmpty())
            emit taskFileAttached(taskId, attachmentEmbedId);
        emit fileUploaded(taskId, QStringLiteral("task"));
    }

    void changeMessage(const QString &dialog, const QString &message, MessageAction action, const QString &value) override
    {
        messageActionDialog = dialog;
        messageActionId = message;
        messageAction = action;
        messageActionValue = value;
    }

    QString messageActionDialog;
    QString messageActionId;
    MessageAction messageAction = MessageAction::React;
    QString messageActionValue;
    QVector<Chat> chats;
    QString accountScope;
    bool demoMode = true;
    QVector<Chat> taskChats;
    QByteArray avatarBytes;
    QHash<QUrl, QByteArray> avatarImages;
    bool holdAvatarResponses = false;
    QList<QUrl> avatarRequests;
    QList<QUrl> imageRequests;
    QHash<QUrl, QByteArray> messageImages;
    QString searchedTaskChats;
    QString pinTarget;
    QString contextDialog;
    QString contextMessage;
    QString requestedPerson;
    QUrl downloadedUrl;
    QString downloadedPath;
    bool holdMessageFileDownload = false;
    QUrl photoUrl;
    QString photoPath;
    QStringList photoCancels;
    int photoDownloads = 0;
    bool pinValue = false;
    QVector<Message> messages;
    QVector<ReadCall> reads;
    QStringList pinnedRequests;
    QHash<QString, QVector<Message>> pinnedMessages;
    QVector<Message> linkedMessages;
    QVector<Project> projects;
    QVector<Project> moreProjects;
    bool holdProjectMore = false;
    int projectRequests = 0;
    int boardRequests = 0;
    bool holdBoardRead = false;
    int projectMoreRequests = 0;
    QStringList projectStageRequests;
    QVector<Stage> projectStageChoices;
    bool holdProjectStages = false;
    QStringList timelineRequests;
    QVector<Task> timelineTasks;
    bool timelineLimited = false;
    QVector<QPair<QString, QString>> boardPageRequests;
    QVector<QPair<QString, QString>> peopleRequests;
    QString lastPeopleRequestId;
    QVector<Person> people;
    bool projectToolsFixture = false;
    QVector<Stage> stages;
    QVector<Message> taskMessages;
    QVector<Notification> notifications;
    bool holdNotifications = false;
    QString noticeQuery;
    QString noticeReadId;
    QString noticeDeleteId;
    int noticeReadAll = 0;
    QVariantMap noticeSettings;
    int noticeSettingsReads = 0;
    bool noticeSettingsReadFails = false;
    QVector<QPair<QString, int>> noticeSettingWrites;
    bool holdMessageSends = false;
    int chatSendRequests = 0;
    int taskSendRequests = 0;
    bool holdTaskUpdate = false;
    int taskUpdateRequests = 0;
    bool holdUpload = false;
    bool holdTaskAttachment = false;
    QVector<QPair<QString, QString>> attachmentRequests;
    bool holdTaskCreation = false;
    int createTaskCalls = 0;
    QStringList taskReadRequests;

    TaskEdit lastUpdate;
    TaskEdit lastCreate;
    QString updatedTask;
    QString createdProject;
    QString createdParent;
    QString createdProjectName;
    QString savedStage;
    QString savedStageTitle;
    QString savedAfterStage;
    QString savedStageColor;
    QString deletedStage;
    QString movedProject;
    QString movedStage;
    QString movedTask;
    QString searchedProject;
    QString searchedTaskQuery;
    QString searchedChats;
    QString searchedScope;
    QString searchedQuery;
    bool searchedTask = false;
    QString uploadedPath;
    QString uploadedScope;
    QString uploadedCaption;
    int imageUploadRequests = 0;
    bool imageUploadForTask = false;
    QString attachedPath;
    QString attachmentEmbedId;
    QString attachedTask;
    QString sentChat;
    QString sentTask;
    QString sentReplyScope;
    QString sentReplyText;
    QString sentReplyId;
};

FixtureBackend *makeFixture()
{
    auto *backend = new FixtureBackend;
    Chat chat;
    chat.id = QStringLiteral("c-1");
    chat.title = QStringLiteral("Рабочий чат");
    backend->chats = {chat};
    Message message;
    message.id = QStringLiteral("m-1");
    message.author = QStringLiteral("Иван");
    message.text = QStringLiteral("Привет");
    backend->messages = {message};
    backend->taskMessages = {message};
    Project project;
    project.id = QStringLiteral("101");
    project.name = QStringLiteral("Проект один");
    backend->projects = {project};
    Stage stage;
    stage.id = QStringLiteral("601");
    stage.name = QStringLiteral("В работе");
    Task task;
    task.id = QStringLiteral("501");
    task.name = QStringLiteral("Починить кран");
    task.description = QStringLiteral("Течёт");
    task.due = QStringLiteral("2026-01-02");
    task.priority = QStringLiteral("Обычный");
    task.stageId = stage.id;
    task.stageName = stage.name;
    task.editRights.title = true;
    task.editRights.description = true;
    task.editRights.priority = true;
    task.editRights.stage = true;
    stage.tasks = {task};
    stage.taskCount = 1;
    Stage empty;
    empty.id = QStringLiteral("602");
    empty.name = QStringLiteral("Готово");
    empty.taskCount = 0;
    backend->stages = {stage, empty};
    return backend;
}

class PilotUiTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(m_settings.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
    }

    void showsUnreadAndWorkdayWithoutOpeningTheirSections()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->chats.first().unread = 3;
        backend->chats.first().serverChatId = 67;
        auto duplicate = backend->chats.first();
        duplicate.id = QStringLiteral("chat67");
        Chat other;
        other.id = QStringLiteral("chat68");
        other.serverChatId = 68;
        other.unread = 4;
        backend->taskChats = {duplicate, other};
        Notification unread;
        unread.id = QStringLiteral("notice-1");
        auto read = unread;
        read.id = QStringLiteral("notice-2");
        read.read = true;
        backend->notifications = {unread, read};
        backend->workdayStatus.state = WorkdayState::Running;
        backend->workdayStatus.elapsedSeconds = 71 * 60 + 59;

        MainWindow window(backend.get());
        window.show();
        QApplication::setActiveWindow(&window);
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        const auto detail = [sections](int row) {
            return sections->item(row)->data(Qt::AccessibleDescriptionRole).toString();
        };
        QCOMPARE(sections->currentRow(), 0);
        QTRY_COMPARE(backend->workdayRequests, 1);
        QCOMPARE(backend->noticeHistoryRequests, 1);
        QVERIFY(detail(0).contains(QStringLiteral("3")));
        emit backend->taskChatsLoaded(backend->taskChats);
        QVERIFY(detail(0).contains(QStringLiteral("7")));
        QVERIFY(detail(2).contains(QStringLiteral("1")));
        QTRY_VERIFY(detail(3).contains(QStringLiteral("01:11")));
        const auto screenshot = qEnvironmentVariable("B24_RAIL_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(sections->grab().save(screenshot));
        QTRY_VERIFY(detail(3).contains(QStringLiteral("01:12")));
        QCOMPARE(backend->workdayRequests, 1);
        sections->setCurrentRow(3);
        QCOMPARE(backend->workdayRequests, 1);
        backend->railUnread = 21;
        emit backend->unreadChatCountChanged(21);
        emit backend->recentChatsLoaded(backend->chats);
        QVERIFY(detail(0).contains(QStringLiteral("7")));
        emit backend->notificationReadStateChanged(QStringList{unread.id}, true);
        QVERIFY(detail(2).isEmpty());
        sections->setCurrentRow(2);
        QCOMPARE(backend->noticeHistoryRequests, 1);

        window.onConnectionChanged(false);
        QVERIFY(detail(3).isEmpty());
        backend->holdWorkday = true;
        window.onConnectionChanged(true);
        QCOMPARE(backend->workdayRequests, 2);
        QVERIFY(detail(3).isEmpty());
        window.onConnectionChanged(false);
        backend->holdWorkday = false;
        backend->workdayStatus.state = WorkdayState::Paused;
        backend->workdayStatus.elapsedSeconds = 89 * 60;
        window.onConnectionChanged(true);
        QCOMPARE(backend->workdayRequests, 3);
        QVERIFY(detail(3).contains(QStringLiteral("01:29")));
        auto *clock = window.findChild<QTimer *>(QStringLiteral("railWorkdayTimer"));
        QVERIFY(clock && !clock->isActive());
    }

    void showsSeparateUnreadBadgesOnChatTabs()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->railUnread = 150;
        backend->provideTaskBadges = true;
        auto ordinary = backend->chats.first();
        ordinary.serverChatId = 67;
        ordinary.unread = 3;
        auto duplicate = ordinary;
        duplicate.id = QStringLiteral("legacy-task");
        duplicate.serverChatId = 71;
        duplicate.unread = 7;
        auto task = duplicate;
        task.id = QStringLiteral("chat71");
        task.serverChatId = 0;
        task.taskChat = true;
        task.taskId = QStringLiteral("501");
        task.unread = 2;
        auto otherTask = task;
        otherTask.id = QStringLiteral("chat72");
        otherTask.taskId = QStringLiteral("502");
        otherTask.unread = 4;
        backend->chats = {ordinary, duplicate};
        backend->taskChats = {task, otherTask};

        MainWindow window(backend.get());
        window.show();
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("chatScopeTabs"));
        auto *rail = window.findChild<QListWidget *>(QStringLiteral("sections"));
        QVERIFY(tabs && rail);
        QCOMPARE(backend->taskChatBadgeRequests, 1);
        QCOMPARE(backend->taskChatRequests, 0);
        QCOMPARE(tabs->currentIndex(), 0);
        QCOMPARE(tabs->tabData(0).toLongLong(), 3);
        QCOMPARE(tabs->tabData(1).toLongLong(), 6);
        QCOMPARE(tabs->tabText(0), QStringLiteral("Чаты"));
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты задач"));
        const auto railCount = [rail] { return rail->item(0)->data(ui::SectionUnreadCount).toLongLong(); };
        QCOMPARE(railCount(), 9);
        for (int scope = 0; scope < 2; ++scope) {
            auto *badge = qobject_cast<QLabel *>(tabs->tabButton(scope, QTabBar::RightSide));
            QVERIFY(badge);
            QCOMPARE(badge->text(), scope == 0 ? QStringLiteral("3") : QStringLiteral("6"));
        }
        const auto screenshot = qEnvironmentVariable("B24_CHAT_TABS_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(tabs->grab().save(screenshot));

        backend->taskChats[0].unread = 0;
        emit backend->taskChatsLoaded(backend->taskChats);
        QCOMPARE(tabs->tabData(0).toLongLong(), 3);
        QCOMPARE(tabs->tabData(1).toLongLong(), 4);
        QCOMPARE(railCount(), 7);
        backend->chats[0].unread = 0;
        backend->taskChats[1].unread = 95;
        emit backend->recentChatsLoaded(backend->chats);
        emit backend->taskChatsLoaded(backend->taskChats);
        QCOMPARE(tabs->tabData(0).toLongLong(), 0);
        QCOMPARE(tabs->tabData(1).toLongLong(), 95);
        QCOMPARE(railCount(), 95);
        emit backend->unreadChatCountChanged(150);
        QCOMPARE(railCount(), 95);
        backend->chats[0].unread = 2;
        backend->taskChats[1].unread = 96;
        emit backend->taskChatsLoaded(backend->taskChats);
        emit backend->recentChatsLoaded(backend->chats);
        QCOMPARE(tabs->tabData(0).toLongLong(), 2);
        QCOMPARE(tabs->tabData(1).toLongLong(), 96);
        QCOMPARE(railCount(), 98);
        tabs->setCurrentIndex(1);
        QCOMPARE(backend->taskChatRequests, 1);
        QCOMPARE(railCount(), 98);
        backend->chats[0].unread = 0;
        emit backend->recentChatsLoaded(backend->chats);
        backend->taskChats[1].unread = 0;
        emit backend->taskChatsLoaded(backend->taskChats);
        QCOMPARE(tabs->tabData(1).toLongLong(), 0);
        const auto *blankBadge = qobject_cast<QLabel *>(tabs->tabButton(1, QTabBar::RightSide));
        QVERIFY(!blankBadge || (!blankBadge->isVisible() && blankBadge->text().isEmpty()));
        QCOMPARE(railCount(), 0);
        emit backend->recentChatsLoaded(backend->chats);
        emit backend->unreadChatCountChanged(150);
        QCOMPARE(backend->taskChatBadgeRequests, 1);
        QCOMPARE(tabs->tabData(0).toLongLong(), 0);
        QCOMPARE(railCount(), 0);

        backend->taskChats[1].unread = 95;
        emit backend->taskChatsLoaded(backend->taskChats);
        backend->accountScope = QStringLiteral("fixture/chat-badges-first");
        emit backend->accountScopeChanged();
        QCOMPARE(railCount(), 95);
        backend->accountScope = QStringLiteral("fixture/chat-badges-second");
        emit backend->accountScopeChanged();
        QCOMPARE(tabs->tabData(0).toLongLong(), 0);
        QCOMPARE(tabs->tabData(1).toLongLong(), 0);
        QCOMPARE(railCount(), 0);
    }

    void chatTabUnreadBadgesStayInsideTheirTabsOnResize()
    {
        const auto applicationFont = QApplication::font();
        const auto restoreFont = qScopeGuard([applicationFont] { QApplication::setFont(applicationFont); });
        ui::installApplicationTypography(*qApp);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->provideTaskBadges = true;
        auto task = backend->chats.first();
        task.id = QStringLiteral("chat72");
        task.taskChat = true;
        task.taskId = QStringLiteral("502");
        task.unread = 1;
        backend->taskChats = {task};
        MainWindow window(backend.get());
        window.resize(1400, 690);
        window.show();
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("chatScopeTabs"));
        auto *splitter = window.findChild<QSplitter *>(QStringLiteral("chatDirectorySplitter"));
        QVERIFY(tabs && splitter);
        QTRY_VERIFY(tabs->isVisible());
        const int compactWidth = splitter->widget(0)->minimumWidth();
        const int wideWidth = compactWidth + 130;
        const auto setDirectoryWidth = [splitter](int width) {
            const int available = splitter->contentsRect().width() - splitter->handle(1)->width();
            splitter->setSizes({width, available - width});
            QTRY_COMPARE(splitter->sizes().first(), width);
        };
        setDirectoryWidth(wideWidth);
        const int wideTabWidth = tabs->width();
        const auto assertBadge = [tabs](int scope, const QString &text) {
            const auto *badge = qobject_cast<QLabel *>(tabs->tabButton(scope, QTabBar::RightSide));
            if (text.isEmpty()) {
                QVERIFY(!badge || !badge->isVisible());
                return;
            }
            QVERIFY(badge && badge->isVisible());
            QCOMPARE(badge->text(), text);
            const auto message = QStringLiteral("scope %1, tab %2×%3 at %4, badge %5×%6 at %7, tabs width %8")
                .arg(scope).arg(tabs->tabRect(scope).width()).arg(tabs->tabRect(scope).height())
                .arg(tabs->tabRect(scope).x()).arg(badge->width()).arg(badge->height())
                .arg(badge->x()).arg(tabs->width());
            QVERIFY2(tabs->rect().contains(badge->geometry()), qPrintable(message));
            QVERIFY2(tabs->tabRect(scope).contains(badge->geometry()), qPrintable(message));
            QCOMPARE(badge->visibleRegion().boundingRect(), badge->rect());
        };
        assertBadge(0, {});
        assertBadge(1, QStringLiteral("1"));

        for (const auto width : {compactWidth, wideWidth}) {
            window.resize(width == compactWidth ? 900 : 1400, 690);
            QCoreApplication::processEvents();
            setDirectoryWidth(width);
            QTRY_VERIFY(width == compactWidth ? tabs->width() < wideTabWidth : tabs->width() == wideTabWidth);
            const QRect first = tabs->tabRect(0);
            const QRect second = tabs->tabRect(1);
            for (int scope : {0, 1}) {
                tabs->setCurrentIndex(scope);
                for (const int count : {1, 10, 127, 0}) {
                    backend->chats[0].unread = scope == 0 ? 0 : count;
                    backend->taskChats[0].unread = scope == 0 ? count : 0;
                    emit backend->recentChatsLoaded(backend->chats);
                    emit backend->taskChatsLoaded(backend->taskChats);
                    QCoreApplication::processEvents();
                    const auto label = count > 99 ? QStringLiteral("99+")
                        : count > 0 ? QString::number(count) : QString();
                    assertBadge(scope, {});
                    assertBadge(1 - scope, label);
                    QCOMPARE(tabs->tabData(1 - scope).toLongLong(), qint64(count));
                    QCOMPARE(tabs->tabRect(0), first);
                    QCOMPARE(tabs->tabRect(1), second);
                    const auto screenshot = qEnvironmentVariable("B24_CHAT_TAB_GEOMETRY_SCREENSHOT");
                    if (!screenshot.isEmpty() && count == 127) {
                        QVERIFY(tabs->grab().save(QStringLiteral("%1-%2-%3.png").arg(screenshot).arg(width).arg(scope)));
                    }
                }
            }
        }
    }

    void quickReactionsAndFullPaletteWorkInBothHistories()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        backend->chats.first().id = QStringLiteral("chat91");
        auto &message = backend->messages.first();
        message.id = QStringLiteral("701");
        message.text = QStringLiteral("Готово завтра\nнаписать отчёт");
        message.outgoing = true;
        message.reactions = {{QStringLiteral("fire"), 3, true}};
        auto taskMessage = message;
        taskMessage.id = QStringLiteral("801");
        taskMessage.text = QStringLiteral("[B]Готово[/B]\nОк");
        backend->taskMessages = {taskMessage};
        backend->stages.first().tasks.first().chatDialogId = QStringLiteral("chat67");
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        emit backend->connectionChanged(true);
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        QTRY_COMPARE(history->count(), 1);

        const auto bubble = [](QListWidget *rows, int row = 0) {
            const auto *delegate = static_cast<const workspace::RowPainter *>(rows->itemDelegate());
            return delegate->receiptArea(rows->model()->index(row, 0), rows->visualItemRect(rows->item(row)));
        };
        const auto bodyPoint = [&bubble](QListWidget *rows) {
            const auto rect = bubble(rows);
            return QPoint(rect.left() + 12, rect.top() + 6
                + (rows->item(0)->data(workspace::Title).toString().isEmpty() ? 0 : 20)
                + QFontMetrics(workspace::rowFont(ui::bodyTextPixels)).height() / 2);
        };
        const auto chipPoint = [&bubble](QListWidget *rows) {
            const auto rect = bubble(rows);
            return QPoint(rect.left() + 15, rect.bottom() - 10);
        };
        auto *clipboard = QGuiApplication::clipboard();
        const auto copySelection = [clipboard](QListWidget *rows, const QString &expected) {
            clipboard->clear();
            QTest::keyClick(rows, Qt::Key_C, Qt::ControlModifier);
            QCOMPARE(clipboard->text(), expected);
        };
        const auto selectWord = [&bodyPoint](QListWidget *rows) {
            const auto point = bodyPoint(rows) + QPoint(14, 0);
            QTest::mouseDClick(rows->viewport(), Qt::LeftButton, Qt::NoModifier, point);
            QTest::mouseRelease(rows->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        };
        selectWord(history);
        copySelection(history, QStringLiteral("Готово"));
        QVERIFY(backend->messageActionId.isEmpty());
        QTest::keyClick(history, Qt::Key_Right, Qt::ShiftModifier);
        copySelection(history, QStringLiteral("Готово "));

        const auto metrics = QFontMetrics(workspace::rowFont(ui::bodyTextPixels));
        const auto start = bodyPoint(history);
        const auto end = start + QPoint(metrics.horizontalAdvance(QStringLiteral("написать")), metrics.lineSpacing());
        auto *viewport = history->viewport();
        QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, start);
        QMouseEvent drag(QEvent::MouseMove, end, viewport->mapToGlobal(end),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(viewport, &drag);
        QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, end);
        copySelection(history, QStringLiteral("Готово завтра\nнаписать"));
        QTest::keyClick(history, Qt::Key_Escape);
        clipboard->clear();
        QTest::keyClick(history, Qt::Key_C, Qt::ControlModifier);
        QVERIFY(clipboard->text() != QStringLiteral("Готово завтра\nнаписать"));

        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, chipPoint(history));
        QCOMPARE(backend->messageActionDialog, QStringLiteral("chat91"));
        QCOMPARE(backend->messageActionId, message.id);
        QCOMPARE(backend->messageActionValue, QStringLiteral("fire"));
        QCOMPARE(backend->messageAction, MessageAction::RemoveReaction);

        emit backend->chatOwnReactionChanged(QStringLiteral("chat92"), message.id, QStringLiteral("fire"), false);
        QCOMPARE(history->item(0)->data(workspace::MessageReactionCounts).toMap().value(QStringLiteral("fire")).toInt(), 3);
        emit backend->chatOwnReactionChanged(QStringLiteral("chat91"), message.id, QStringLiteral("fire"), false);
        emit backend->chatOwnReactionChanged(QStringLiteral("chat91"), message.id, QStringLiteral("fire"), false);
        QCOMPARE(history->item(0)->data(workspace::MessageReactionCounts).toMap().value(QStringLiteral("fire")).toInt(), 2);
        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, chipPoint(history));
        QCOMPARE(backend->messageAction, MessageAction::React);

        auto *more = findAccessible<QPushButton>(history, QStringLiteral("Действия сообщения"));
        QVERIFY(more && more->isHidden());
        QCoreApplication::processEvents();
        const bool offscreen = QGuiApplication::platformName() == QLatin1String("offscreen");
        const auto hover = [offscreen](QListWidget *rows, const QPoint &point) {
            auto *viewport = rows->viewport();
            if (offscreen) {
                QMouseEvent move(QEvent::MouseMove, point, viewport->mapToGlobal(point),
                                 Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                QCoreApplication::sendEvent(viewport, &move);
            } else {
                QTest::mouseMove(viewport, point);
            }
        };
        const auto verifyRightSide = [&bubble, &hover](QListWidget *rows, QPushButton *button) {
            const auto rect = bubble(rows);
            const int edge = rows->viewport()->rect().right();
            QVERIFY(edge - rect.right() > 50);
            for (const int y : {rect.top(), rect.center().y(), rect.bottom()}) {
                hover(rows, QPoint(edge, rect.top() - 1));
                QTRY_VERIFY(button->isHidden());
                hover(rows, QPoint(edge, y));
                QTRY_VERIFY(button->isVisible());
            }
            hover(rows, QPoint(edge, rect.bottom() + 1));
            QTRY_VERIFY(button->isHidden());
        };
        verifyRightSide(history, more);
        hover(history, QPoint(bubble(history).right() + 2, bubble(history).center().y()));
        QTRY_VERIFY(more->isVisible());
        QVERIFY(more->geometry().left() > bubble(history).right());
        const auto between = QPoint(bubble(history).right() + 1, more->geometry().center().y());
        QTest::mouseMove(history->viewport(), between);
        QVERIFY(more->isVisible());
        QTest::mouseMove(more, more->rect().center());
        QVERIFY(more->isVisible());
        QTest::mouseClick(more, Qt::LeftButton);
        auto *actions = visibleMessageMenu();
        QVERIFY(actions && actions->objectName() == QStringLiteral("messageContextMenu"));
        QAction *reactionAction = nullptr;
        for (auto *action : actions->actions())
            if (action->text() == QStringLiteral("Реакция"))
                reactionAction = action;
        QVERIFY(reactionAction && reactionAction->menu());
        auto *reactions = reactionAction->menu();
        const QPointer<QMenu> originalMenu(actions);
        QAction *allReactions = nullptr;
        for (auto *action : reactions->actions())
            if (action->text() == QStringLiteral("Все реакции…"))
                allReactions = action;
        QVERIFY(allReactions);
        if (offscreen) {
            allReactions->trigger();
        } else {
            actions->setActiveAction(reactionAction);
            QTest::keyClick(actions, Qt::Key_Right);
            QTRY_VERIFY_WITH_TIMEOUT(reactions->isVisible(), 1800);
            reactions->setActiveAction(allReactions);
            QTest::keyClick(reactions, Qt::Key_Return);
        }
        QTRY_VERIFY_WITH_TIMEOUT(visibleMessageMenu(QStringLiteral("reactionPalette")), 1800);
        auto *popup = visibleMessageMenu(QStringLiteral("reactionPalette"));
        QVERIFY(popup && popup->objectName() == QStringLiteral("reactionPalette"));
        auto *choices = findAccessible<QListWidget>(popup, QStringLiteral("Выберите реакцию"));
        QVERIFY(choices);
        QCOMPARE(choices->count(), 58);
        QSet<QString> keys;
        for (int row = 0; row < choices->count(); ++row) {
            const auto *item = choices->item(row);
            keys.insert(item->data(Qt::UserRole).toString());
            QVERIFY(!item->text().isEmpty() && !item->data(Qt::AccessibleTextRole).toString().isEmpty());
        }
        QCOMPARE(keys.size(), 58);
        QVERIFY(keys.contains(QStringLiteral("saluteFace")) && keys.contains(QStringLiteral("smilingFaceWithHalo")));
        choices->setCurrentRow(choices->count() - 1);
        QTest::keyClick(choices, Qt::Key_Return);
        QCOMPARE(backend->messageActionValue, QStringLiteral("smilingFaceWithHalo"));
        QCOMPARE(backend->messageAction, MessageAction::React);
        if (originalMenu)
            originalMenu->hide();
        QCoreApplication::processEvents();

        emit backend->connectionChanged(false);
        backend->messageActionId.clear();
        selectWord(history);
        copySelection(history, QStringLiteral("Готово"));
        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, chipPoint(history));
        QVERIFY(backend->messageActionId.isEmpty());
        emit backend->connectionChanged(true);

        auto nextMessage = message;
        nextMessage.id = QStringLiteral("702");
        selectWord(history);
        copySelection(history, QStringLiteral("Готово"));
        emit backend->chatMessagesLoaded(QStringLiteral("chat91"), {message, nextMessage});
        QTRY_COMPARE(history->count(), 2);
        copySelection(history, QStringLiteral("Готово"));

        auto revisedMessage = message;
        revisedMessage.text = QStringLiteral("Исправлено завтра\nнаписать отчёт");
        emit backend->chatMessagesLoaded(QStringLiteral("chat91"), {revisedMessage, nextMessage});
        QTRY_COMPARE(history->item(0)->data(workspace::Detail).toString(), revisedMessage.text);
        clipboard->clear();
        QTest::keyClick(history, Qt::Key_C, Qt::ControlModifier);
        QVERIFY(clipboard->text() != QStringLiteral("Готово"));

        QCoreApplication::processEvents();
        const auto firstBubble = bubble(history);
        const auto nextBubble = bubble(history, 1);
        const int edge = history->viewport()->rect().right();
        hover(history, QPoint(edge, firstBubble.center().y()));
        QTRY_VERIFY(more->isVisible());
        QTRY_VERIFY(firstBubble.contains(QPoint(firstBubble.center().x(), more->geometry().center().y())));
        hover(history, QPoint(edge, nextBubble.center().y()));
        QTRY_VERIFY(more->isVisible());
        QTRY_VERIFY(nextBubble.contains(QPoint(nextBubble.center().x(), more->geometry().center().y())));

        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        QApplication::setActiveWindow(card);
        auto *taskHistory = findAccessible<QListWidget>(card, QStringLiteral("Чат карточки"));
        QVERIFY(taskHistory);
        QTRY_COMPARE(taskHistory->count(), 1);
        selectWord(taskHistory);
        copySelection(taskHistory, QStringLiteral("Готово"));
        QVERIFY(backend->messageActionId.isEmpty());
        QTest::keyClick(taskHistory, Qt::Key_A, Qt::ControlModifier);
        copySelection(taskHistory, QStringLiteral("Готово\nОк"));
        QTest::mouseClick(taskHistory->viewport(), Qt::LeftButton, Qt::NoModifier, chipPoint(taskHistory));
        QCOMPARE(backend->messageActionDialog, QStringLiteral("chat67"));
        QCOMPARE(backend->messageActionId, taskMessage.id);
        QCOMPARE(backend->messageActionValue, QStringLiteral("fire"));
        QCOMPARE(backend->messageAction, MessageAction::RemoveReaction);

        auto *taskMore = findAccessible<QPushButton>(taskHistory, QStringLiteral("Действия сообщения"));
        QVERIFY(taskMore && taskMore->isHidden());
        card->resize(660, 500);
        QCoreApplication::processEvents();
        QVERIFY(taskHistory->viewport()->rect().contains(bodyPoint(taskHistory)));
        verifyRightSide(taskHistory, taskMore);
        hover(taskHistory, QPoint(taskHistory->viewport()->rect().right(), bubble(taskHistory).center().y()));
        QTRY_VERIFY_WITH_TIMEOUT(taskMore->isVisible(), 1800);
        QVERIFY(taskHistory->viewport()->rect().contains(taskMore->geometry()));
        if (!offscreen) {
            QTest::mouseMove(taskMore, taskMore->rect().center());
            QTRY_VERIFY(taskMore->isVisible());
        }
        QTest::mouseClick(taskMore, Qt::LeftButton);
        auto *taskMenu = visibleMessageMenu();
        QVERIFY(taskMenu && taskMenu->objectName() == QStringLiteral("messageContextMenu"));
        const auto taskActions = taskMenu->actions();
        QVERIFY(std::any_of(taskActions.cbegin(), taskActions.cend(), [](const QAction *action) {
            return action->text() == QStringLiteral("Реакция");
        }));
        taskMenu->hide();
        QCoreApplication::processEvents();
        hover(taskHistory, bodyPoint(taskHistory) + QPoint(1, 0));
        QTRY_VERIFY_WITH_TIMEOUT(taskMore->isVisible(), 1800);
        const auto taskPoint = bodyPoint(taskHistory);
        QWheelEvent wheel(taskPoint, taskHistory->viewport()->mapToGlobal(taskPoint), {}, QPoint(0, -120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(taskHistory->viewport(), &wheel);
        QVERIFY(taskMore->isHidden());
        hover(taskHistory, taskPoint + QPoint(2, 0));
        QTRY_VERIFY_WITH_TIMEOUT(taskMore->isVisible(), 1800);
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(taskHistory, &leave);
        QVERIFY(taskMore->isHidden());

        auto *entry = taskHistory->item(0);
        entry->setData(workspace::Outgoing, false);
        entry->setData(workspace::Meta, QString());
        entry->setData(workspace::MessageReactionCounts, QVariantMap());
        const auto modelIndex = taskHistory->model()->index(0, 0);
        const auto *delegate = taskHistory->itemDelegate();
        const QStyleOptionViewItem option;
        const int height = delegate->sizeHint(option, modelIndex).height();
        entry->setData(workspace::ReceiptMessageId, QString());
        QCOMPARE(delegate->sizeHint(option, modelIndex).height(), height);
    }

    void ranksConfirmedReactionAdditionsPerAccount()
    {
        const auto dialog = QStringLiteral("chat91");
        const auto id = QStringLiteral("701");
        const auto neutral = QStringLiteral("neutralFace");
        const auto heart = QStringLiteral("redHeart");
        const auto like = QStringLiteral("like");
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        backend->accountScope = QStringLiteral("reaction-frequency-portal-account-one");
        backend->chats.first().id = dialog;
        backend->messages.first().id = id;

        const auto getQuickMenu = [](MainWindow &window) -> QMenu * {
            auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
            if (!history || !history->count())
                return nullptr;
            history->setCurrentRow(0);
            const auto point = history->visualItemRect(history->item(0)).center();
            QContextMenuEvent event(QContextMenuEvent::Keyboard, point, history->viewport()->mapToGlobal(point));
            QApplication::sendEvent(history->viewport(), &event);
            const auto *menu = visibleMessageMenu();
            if (menu) {
                for (auto *action : menu->actions()) {
                    if (action->text() == QStringLiteral("Реакция"))
                        return action->menu();
                }
            }
            return nullptr;
        };
        const auto checkFirst = [&](MainWindow &window, const QString &code) {
            auto *quick = getQuickMenu(window);
            QVERIFY(quick);
            const auto label = quick->actions().first()->text();
            qobject_cast<QWidget *>(quick->parent())->hide();
            QVERIFY2(label.startsWith(workspace::reactionGlyph(code)), qPrintable(label));
        };
        const auto triggerQuick = [&](MainWindow &window, const QString &code) {
            auto *quick = getQuickMenu(window);
            QVERIFY(quick);
            QAction *choice = nullptr;
            for (auto *action : quick->actions()) {
                if (action->text().startsWith(workspace::reactionGlyph(code))) {
                    choice = action;
                    break;
                }
            }
            QVERIFY(choice);
            choice->trigger();
            qobject_cast<QWidget *>(quick->parent())->hide();
            QCOMPARE(backend->messageActionValue, code);
        };
        {
            MainWindow window(backend.get());
            window.show();
            QApplication::setActiveWindow(&window);
            emit backend->connectionChanged(true);
            QTRY_VERIFY(findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"))->count() > 0);
            checkFirst(window, like);
            triggerQuick(window, heart);
            emit backend->chatOwnReactionChanged(QStringLiteral("chat92"), id, heart, true);
            checkFirst(window, like);
            emit backend->failed(Backend::reactionOperationKey(dialog, id, heart), QStringLiteral("Отклонено"));
            emit backend->chatOwnReactionChanged(dialog, id, heart, true);
            checkFirst(window, like);
            emit backend->chatOwnReactionChanged(dialog, id, heart, false);

            triggerQuick(window, neutral);
            emit backend->chatOwnReactionChanged(dialog, id, neutral, true);
            emit backend->chatOwnReactionChanged(dialog, id, neutral, true);
            checkFirst(window, neutral);
            triggerQuick(window, neutral);
            QCOMPARE(backend->messageAction, MessageAction::RemoveReaction);
            emit backend->chatOwnReactionChanged(dialog, id, neutral, false);
            checkFirst(window, neutral);

            triggerQuick(window, heart);
            triggerQuick(window, neutral);
            emit backend->chatOwnReactionChanged(dialog, id, neutral, true);
            emit backend->chatOwnReactionChanged(dialog, id, heart, true);
            checkFirst(window, neutral);

            triggerQuick(window, heart);
            emit backend->chatOwnReactionChanged(dialog, id, heart, false);
            triggerQuick(window, heart);
            emit backend->chatOwnReactionChanged(dialog, id, heart, true);
            checkFirst(window, heart);
        }
        {
            MainWindow window(backend.get());
            window.show();
            QApplication::setActiveWindow(&window);
            emit backend->connectionChanged(true);
            QTRY_VERIFY(findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"))->count() > 0);
            checkFirst(window, heart);
            backend->accountScope = QStringLiteral("reaction-frequency-portal-account-two");
            emit backend->accountScopeChanged();
            checkFirst(window, like);
        }

        const auto halo = QStringLiteral("smilingFaceWithHalo");
        ui::ReactionPalette palette({}, ui::rankedReactionChoices({{heart, 2}, {halo, 3}}), nullptr);
        const auto *choices = findAccessible<QListWidget>(&palette, QStringLiteral("Выберите реакцию"));
        QVERIFY(choices);
        QCOMPARE(choices->count(), 58);
        QCOMPARE(choices->item(0)->data(Qt::UserRole).toString(), halo);
        QCOMPARE(choices->item(1)->data(Qt::UserRole).toString(), heart);
    }

    void exposesReactionAndMessagePermalinkFromHistoryMenu()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        backend->chats.first().id = QStringLiteral("chat91");
        backend->messages.first().id = QStringLiteral("701");
        backend->messages.first().outgoing = true;
        backend->messages.first().reactions = {{QStringLiteral("like"), 2, true}};
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        emit backend->connectionChanged(true);
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        QTRY_COMPARE(history->count(), 1);
        const auto getMenu = [history] {
            history->setCurrentRow(0);
            const auto point = history->visualItemRect(history->item(0)).center();
            QContextMenuEvent event(QContextMenuEvent::Keyboard, point, history->viewport()->mapToGlobal(point));
            QApplication::sendEvent(history->viewport(), &event);
            return visibleMessageMenu();
        };
        auto *menu = getMenu();
        QVERIFY(menu);
        const auto findAction = [](QMenu *menu, const QString &label) {
            for (auto *action : menu->actions()) {
                if (action->text() == label)
                    return action;
            }
            return static_cast<QAction *>(nullptr);
        };
        auto *reaction = findAction(menu, QStringLiteral("Реакция"));
        QVERIFY(reaction && reaction->menu());
        auto *own = reaction->menu()->actions().first();
        QVERIFY(own->isChecked());
        own->trigger();
        QCOMPARE(backend->messageActionDialog, QStringLiteral("chat91"));
        QCOMPARE(backend->messageActionId, QStringLiteral("701"));
        QCOMPARE(backend->messageActionValue, QStringLiteral("like"));
        QCOMPARE(backend->messageAction, MessageAction::RemoveReaction);
        auto *copy = findAction(menu, QStringLiteral("Скопировать ссылку на сообщение"));
        QVERIFY(copy);
        copy->trigger();
        QCOMPARE(QGuiApplication::clipboard()->text(),
                 QStringLiteral("https://example.bitrix24.ru/online/?IM_DIALOG=chat91&IM_MESSAGE=701"));
        menu->hide();
        emit backend->chatMessagesSeen(QStringLiteral("chat92"), {QStringLiteral("701")});
        QVERIFY(!history->item(0)->data(workspace::MessageSeen).toBool());
        emit backend->chatMessagesSeen(QStringLiteral("chat91"), {QStringLiteral("701")});
        QVERIFY(history->item(0)->data(workspace::MessageSeen).toBool());
    }

    void confirmsOnlyTheOriginalMessageWithoutClosingWorkspace_data()
    {
        QTest::addColumn<bool>("taskChat");
        QTest::newRow("ordinary-chat") << false;
        QTest::newRow("task-card-chat") << true;
    }

    void confirmsOnlyTheOriginalMessageWithoutClosingWorkspace()
    {
        QFETCH(bool, taskChat);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        backend->accountScope = QStringLiteral("fixture/delete-message");
        const auto dialog = taskChat ? QStringLiteral("chat67") : QStringLiteral("chat91");
        const auto messageId = QStringLiteral("701");
        backend->chats.first().id = QStringLiteral("chat91");
        Message message;
        message.id = messageId;
        message.outgoing = true;
        message.text = QStringLiteral("Сообщение для удаления");
        Message neighbour = message;
        neighbour.id = QStringLiteral("702");
        neighbour.text = QStringLiteral("Оставить это сообщение");
        backend->messages = {message, neighbour};
        backend->taskMessages = backend->messages;
        backend->stages.first().tasks.first().chatDialogId = dialog;
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        emit backend->connectionChanged(true);
        QWidget *owner = &window;
        if (taskChat) {
            window.openTaskCard(QStringLiteral("501"));
            owner = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
            QVERIFY(owner && owner->isVisible());
        }
        auto *history = findAccessible<QListWidget>(owner, taskChat ? QStringLiteral("Чат карточки")
                                                                   : QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        QTRY_COMPARE(history->count(), 2);
        const auto selectDelete = [&](bool confirm, bool replace) {
            history->setCurrentRow(0);
            const auto point = history->visualItemRect(history->item(0)).center();
            QContextMenuEvent event(QContextMenuEvent::Keyboard, point, history->viewport()->mapToGlobal(point));
            QApplication::sendEvent(history->viewport(), &event);
            auto *menu = visibleMessageMenu();
            QVERIFY(menu);
            QAction *remove = nullptr;
            for (auto *entry : menu->actions())
                if (entry->text() == QStringLiteral("Удалить…"))
                    remove = entry;
            QVERIFY(remove);
            menu->setActiveAction(remove);
            bool answered = false;
            bool correctParent = false;
            bool defaultCancel = false;
            bool idleBeforeAnswer = false;
            QTimer probe;
            connect(&probe, &QTimer::timeout, &window, [&] {
                auto *question = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                if (!question || question->windowTitle() != QStringLiteral("Удалить сообщение"))
                    return;
                probe.stop();
                answered = true;
                correctParent = question->parentWidget() == owner;
                idleBeforeAnswer = backend->messageActionId.isEmpty();
                auto *cancel = question->button(QMessageBox::Cancel);
                if (!cancel)
                    cancel = question->button(QMessageBox::No);
                defaultCancel = question->defaultButton() == cancel;
                if (replace)
                    history->item(0)->setData(workspace::ReceiptMessageId, QStringLiteral("999"));
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                if (auto *button = confirm ? question->button(QMessageBox::Yes) : cancel)
                    button->click();
            });
            probe.start(1);
            QTest::keyClick(menu, Qt::Key_Return);
            QTRY_VERIFY(answered);
            QVERIFY(correctParent);
            QVERIFY(defaultCancel);
            QVERIFY(idleBeforeAnswer);
            QVERIFY(window.isVisible() && owner->isVisible());
        };
        selectDelete(false, false);
        QVERIFY(backend->messageActionId.isEmpty());
        selectDelete(true, true);
        QVERIFY(backend->messageActionId.isEmpty());
        history->item(0)->setData(workspace::ReceiptMessageId, messageId);
        selectDelete(true, false);
        QCOMPARE(backend->messageActionDialog, dialog);
        QCOMPARE(backend->messageActionId, messageId);
        QCOMPARE(backend->messageAction, MessageAction::Delete);
        emit backend->chatHistoryRemoved(dialog, {messageId});
        QTRY_COMPARE(history->count(), 1);
        QCOMPARE(history->item(0)->data(workspace::ReceiptMessageId).toString(), neighbour.id);
        QVERIFY(window.isVisible() && owner->isVisible());
    }

    void inlineMessageEditingRestoresDraftAndKeepsTheTaskCardOpen()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        backend->chats.first().id = QStringLiteral("chat91");
        auto &message = backend->messages.first();
        message.id = QStringLiteral("701");
        message.text = QStringLiteral("Первая строка\nВторая строка");
        message.outgoing = true;
        auto taskMessage = message;
        taskMessage.id = QStringLiteral("801");
        backend->taskMessages = {taskMessage};
        backend->stages.first().tasks.first().chatDialogId = QStringLiteral("chat67");
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        emit backend->connectionChanged(true);

        const auto edit = [](QListWidget *history) {
            history->setCurrentRow(0);
            const auto point = history->visualItemRect(history->item(0)).center();
            QContextMenuEvent event(QContextMenuEvent::Keyboard, point, history->viewport()->mapToGlobal(point));
            QApplication::sendEvent(history->viewport(), &event);
            auto *menu = visibleMessageMenu();
            QVERIFY(menu);
            auto *action = [&]() -> QAction * {
                for (auto *candidate : menu->actions())
                    if (candidate->text() == QStringLiteral("Редактировать…"))
                        return candidate;
                return nullptr;
            }();
            QVERIFY(action);
            QTimer::singleShot(0, [] {
                auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (modal && modal->inherits("QInputDialog"))
                    modal->reject();
            });
            menu->hide();
            action->trigger();
        };
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *composer = dynamic_cast<ui::ChatComposer *>(
            window.findChild<QFrame *>(QStringLiteral("chatComposer")));
        QVERIFY(history && composer);
        QTRY_COMPARE(history->count(), 1);
        ui::ChatReply draftReply;
        draftReply.id = QStringLiteral("700");
        draftReply.author = QStringLiteral("Автор");
        draftReply.preview = QStringLiteral("Исходное сообщение");
        composer->setDraft({QStringLiteral("Мой черновик"), draftReply, {}});
        edit(history);
        QCOMPARE(composer->input()->toPlainText(), message.text);
        QVERIFY(composer->reply().id.isEmpty());
        QTest::keyClick(composer->input(), Qt::Key_Escape);
        QCOMPARE(composer->input()->toPlainText(), QStringLiteral("Мой черновик"));
        QCOMPARE(composer->reply().id, draftReply.id);
        QVERIFY(backend->messageActionId.isEmpty());
        edit(history);
        composer->input()->moveCursor(QTextCursor::End);
        QTest::keyClick(composer->input(), Qt::Key_Return, Qt::ShiftModifier);
        QTest::keyClicks(composer->input(), QStringLiteral("line 3"));
        QTest::keyClick(composer->input(), Qt::Key_Return);
        QCOMPARE(backend->messageAction, MessageAction::Edit);
        QCOMPARE(backend->messageActionId, QStringLiteral("701"));
        const auto text = message.text + QStringLiteral("\nline 3");
        QCOMPARE(backend->messageActionValue, text);
        emit backend->failed(QStringLiteral("message-edit:chat91:701"), QStringLiteral("Не удалось сохранить"));
        QCOMPARE(composer->input()->toPlainText(), text);
        QTest::keyClick(composer->input(), Qt::Key_Return);
        emit backend->messageEditAccepted(QStringLiteral("chat91"), QStringLiteral("701"), text);
        QCOMPARE(composer->input()->toPlainText(), QStringLiteral("Мой черновик"));
        QCOMPARE(composer->reply().id, draftReply.id);
        QCOMPARE(history->item(0)->data(workspace::RawMessageBody).toString(), text);

        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        QApplication::setActiveWindow(card);
        auto *taskHistory = findAccessible<QListWidget>(card, QStringLiteral("Чат карточки"));
        auto *taskComposer = dynamic_cast<ui::ChatComposer *>(
            card->findChild<QFrame *>(QStringLiteral("chatComposer")));
        QVERIFY(taskHistory && taskComposer);
        QTRY_COMPARE(taskHistory->count(), 1);
        taskComposer->input()->setPlainText(QStringLiteral("Черновик задачи"));
        edit(taskHistory);
        QCOMPARE(taskComposer->input()->toPlainText(), taskMessage.text);
        QTest::keyClick(taskComposer->input(), Qt::Key_Escape);
        QVERIFY(card->isVisible());
        QCOMPARE(taskComposer->input()->toPlainText(), QStringLiteral("Черновик задачи"));
    }

    void upEditsLatestEligibleMessageOnlyFromEmptyComposer()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        backend->chats.first().id = QStringLiteral("chat91");
        backend->stages.first().tasks.first().chatDialogId = QStringLiteral("chat67");
        const auto now = QDateTime::currentDateTime();
        Message earlier{QStringLiteral("701"), QStringLiteral("Я"), QStringLiteral("Раннее"), now.addSecs(-40), true};
        Message latest{QStringLiteral("702"), QStringLiteral("Я"), QStringLiteral("Последнее своё"), now.addSecs(-30), true};
        Message incoming{QStringLiteral("703"), QStringLiteral("Другой"), QStringLiteral("Входящее"), now.addSecs(-20)};
        Message system{QStringLiteral("704"), {}, QStringLiteral("Системное"), now.addSecs(-10), true};
        system.system = true;
        backend->messages = {earlier, latest, incoming, system};
        backend->taskMessages = {earlier, latest, incoming};
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        emit backend->connectionChanged(true);

        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *composer = dynamic_cast<ui::ChatComposer *>(
            window.findChild<QFrame *>(QStringLiteral("chatComposer")));
        QVERIFY(history && composer);
        QTRY_COMPARE(history->count(), 4);
        auto *input = composer->input();
        history->setCurrentRow(3);
        QTest::keyClick(history, Qt::Key_Up);
        QCOMPARE(history->currentRow(), 2);
        QVERIFY(!composer->editingMessage());
        input->setFocus();
        QTest::keyClick(input, Qt::Key_Up, Qt::ShiftModifier);
        QVERIFY(!composer->editingMessage());
        input->setPlainText(QStringLiteral("Черновик"));
        QTest::keyClick(input, Qt::Key_Up);
        QCOMPARE(input->toPlainText(), QStringLiteral("Черновик"));
        QVERIFY(!composer->editingMessage());
        input->clear();
        ui::ChatReply reply;
        reply.id = incoming.id;
        reply.preview = incoming.text;
        composer->setReply(reply);
        QTest::keyClick(input, Qt::Key_Up);
        QVERIFY(!composer->editingMessage());
        QCOMPARE(composer->reply().id, reply.id);
        composer->setReply({});
        QInputMethodEvent preedit(QStringLiteral("か"), {});
        QApplication::sendEvent(input, &preedit);
        QTest::keyClick(input, Qt::Key_Up);
        QVERIFY(!composer->editingMessage());
        QInputMethodEvent endPreedit;
        QApplication::sendEvent(input, &endPreedit);
        QTest::keyClick(input, Qt::Key_Up);
        QVERIFY(composer->matchesEdit(QStringLiteral("chat91"), latest.id));
        QCOMPARE(input->toPlainText(), latest.text);
        QTest::keyClick(input, Qt::Key_Escape);
        QVERIFY(input->toPlainText().isEmpty());
        QVERIFY(backend->messageActionId.isEmpty());

        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        QApplication::setActiveWindow(card);
        auto *taskHistory = findAccessible<QListWidget>(card, QStringLiteral("Чат карточки"));
        auto *taskComposer = dynamic_cast<ui::ChatComposer *>(
            card->findChild<QFrame *>(QStringLiteral("chatComposer")));
        QVERIFY(taskHistory && taskComposer);
        QTRY_COMPARE(taskHistory->count(), 3);
        taskComposer->input()->setFocus();
        QTest::keyClick(taskComposer->input(), Qt::Key_Up);
        QVERIFY(taskComposer->matchesEdit(QStringLiteral("chat67"), latest.id));
        QCOMPARE(taskComposer->input()->toPlainText(), latest.text);
    }

    void growsComposerForVisualLinesAndShrinksAfterResize()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        auto *composer = new ui::ChatComposer(&host, QStringLiteral("Input"), QStringLiteral("Emoji"),
                                               QStringLiteral("File"), QStringLiteral("Send"));
        layout->addWidget(composer);
        layout->addStretch();
        host.resize(420, 440);
        host.show();
        auto *input = composer->input();
        QVERIFY(input->isVisible());
        const auto minimum = input->height();
        QTest::keyClick(input, Qt::Key_Return, Qt::ShiftModifier);
        QTRY_VERIFY(input->height() > minimum);
        input->clear();
        QTRY_COMPARE(input->height(), minimum);
        input->setPlainText(QStringLiteral("wrapped word ").repeated(9));
        QCOMPARE(input->document()->blockCount(), 1);
        QTRY_VERIFY(input->height() > minimum);
        const auto wrapped = input->height();
        host.resize(1200, 440);
        QTRY_VERIFY(input->height() < wrapped);
        input->setPlainText(QStringLiteral("line\n").repeated(35));
        QTRY_VERIFY(input->verticalScrollBar()->maximum() > 0);
        const auto capped = input->height();
        input->appendPlainText(QStringLiteral("line\n").repeated(20));
        QTRY_COMPARE(input->height(), capped);
        input->clear();
        QTRY_COMPARE(input->height(), minimum);
    }

    void reportsOnlyCommittedUserTextToTypingIndicator()
    {
        ui::ChatTextEdit input(nullptr);
        int edits = 0;
        input.userEdited = [&edits] { ++edits; };
        input.setPlainText(QStringLiteral("Черновик"));
        QCOMPARE(edits, 0);
        QInputMethodEvent preedit(QStringLiteral("か"), {});
        QApplication::sendEvent(&input, &preedit);
        QVERIFY(input.composing());
        QCOMPARE(edits, 0);
        QInputMethodEvent commit;
        commit.setCommitString(QStringLiteral("か"));
        QApplication::sendEvent(&input, &commit);
        QVERIFY(!input.composing());
        QCOMPARE(edits, 1);
        QTest::keyClicks(&input, QStringLiteral("x"));
        QCOMPARE(edits, 2);
    }

    void requiresAnExplicitStatusAfterPortalDnd()
    {
        class StatusFixture final : public FixtureBackend {
        public:
            void requestPortalStatus() override { emit portalStatusLoaded(QStringLiteral("dnd")); }
            void updatePortalStatus(const QString &expected, const QString &status) override
            {
                previous = expected;
                requested = status;
                emit portalStatusLoaded(status);
                emit portalStatusUpdated(status);
            }
            QString previous;
            QString requested;
        } backend;
        backend.demoMode = false;
        QWidget panel;
        QVBoxLayout layout(&panel);
        ui::addPortalDndControl(&layout, &backend, [] { return true; });
        auto *status = findAccessible<QComboBox>(&panel, QStringLiteral("Новый статус на портале"));
        auto *apply = findAccessible<QPushButton>(&panel, QStringLiteral("Установить статус на портале"));
        QVERIFY(status && apply);
        QVERIFY(status->currentData().toString().isEmpty());
        QVERIFY(!apply->isEnabled());
        status->setCurrentIndex(status->findData(QStringLiteral("away")));
        QVERIFY(apply->isEnabled());
        apply->click();
        QCOMPARE(backend.previous, QStringLiteral("dnd"));
        QCOMPARE(backend.requested, QStringLiteral("away"));
        QVERIFY(!apply->isEnabled());
    }

    void asynchronousAvatarRepliesPaintProjectLogosAndKanbanPeople()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdAvatarResponses = true;
        const auto asset = [](const QString &name) {
            return QUrl(QStringLiteral("https://example.bitrix24.ru/b123/resize_cache/") + name);
        };
        const auto picture = [](const QColor &color) {
            QImage image(48, 48, QImage::Format_RGB32);
            image.fill(color);
            QByteArray bytes;
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
            return bytes;
        };
        const auto logo = asset(QStringLiteral("folder.png"));
        const auto creator = asset(QStringLiteral("creator.png"));
        const auto assignee = asset(QStringLiteral("assignee.png"));
        const auto offscreen = asset(QStringLiteral("offscreen.png"));
        const QColor logoColor("#3e8268"), creatorColor("#ae4985"), assigneeColor("#416fa4");
        backend->avatarImages = {{logo, picture(logoColor)}, {creator, picture(creatorColor)},
                                 {assignee, picture(assigneeColor)}};
        backend->projects.first().avatarUrl = logo;
        backend->projects.first().avatarType = QStringLiteral("folder");
        for (int i = 0; i < 34; ++i) {
            Project project;
            project.id = QString::number(200 + i);
            project.name = QStringLiteral("Проект %1").arg(i + 1);
            project.avatarType = QStringLiteral("folder");
            if (i < 4) {
                project.avatarUrl = asset(QStringLiteral("project-%1.png").arg(i));
                backend->avatarImages.insert(project.avatarUrl, picture(logoColor));
            } else if (i == 33) {
                project.avatarUrl = offscreen;
            }
            backend->projects.append(project);
        }
        auto &stage = backend->stages.first();
        stage.tasks.first().creatorAvatarUrl = creator;
        stage.tasks.first().assigneeAvatarUrl = assignee;
        const auto stageName = stage.name;
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        findAccessible<QListWidget>(&window, QStringLiteral("Разделы"))->setCurrentRow(1);
        auto *projects = findAccessible<QListWidget>(&window, QStringLiteral("Проекты"));
        auto *cards = findAccessible<QListWidget>(&window, stageName);
        QVERIFY(projects && cards && cards->count() > 0);
        QTRY_COMPARE(backend->avatarRequests.size(), 4);
        QVERIFY(!backend->avatarRequests.contains(offscreen));
        QVERIFY(backend->avatarRequests.contains(logo));
        QVERIFY(!backend->avatarRequests.contains(creator));
        QVERIFY(!backend->avatarRequests.contains(assignee));
        for (const auto &url : backend->avatarRequests) {
            QVERIFY(backend->avatarImages.contains(url));
            emit backend->avatarLoaded(url, url == logo ? QByteArray() : backend->avatarImages.value(url));
        }
        QTRY_COMPARE(backend->avatarRequests.size(), 7);
        const auto finalBatch = backend->avatarRequests.mid(4);
        for (const auto &url : finalBatch) {
            QVERIFY(backend->avatarImages.contains(url));
            emit backend->avatarLoaded(url, url == creator ? QByteArray() : backend->avatarImages.value(url));
        }
        QCoreApplication::processEvents();
        QCOMPARE(backend->avatarRequests.count(logo), 1);
        QCOMPARE(backend->avatarRequests.count(creator), 1);
        QCOMPARE(backend->avatarRequests.count(assignee), 1);
        QVERIFY(!backend->avatarRequests.contains(offscreen));

        const auto containsPaint = [](QListWidget *list, const QColor &color) {
            const auto image = list->viewport()->grab().toImage();
            const auto pixels = list->visualItemRect(list->item(0)).intersected(image.rect());
            for (int y = pixels.top(); y <= pixels.bottom(); ++y)
                for (int x = pixels.left(); x <= pixels.right(); ++x)
                    if (image.pixelColor(x, y) == color)
                        return true;
            return false;
        };
        QVERIFY(containsPaint(cards, assigneeColor));
        QVERIFY(!containsPaint(projects, logoColor));
        QVERIFY(!containsPaint(cards, creatorColor));

        QTest::keyClick(&window, Qt::Key_F5);
        QTRY_COMPARE_WITH_TIMEOUT(backend->avatarRequests.size(), 9, 1200);
        QCOMPARE(backend->avatarRequests.count(logo), 2);
        QCOMPARE(backend->avatarRequests.count(creator), 2);
        QCOMPARE(backend->avatarRequests.count(assignee), 1);
        QVERIFY(!backend->avatarRequests.contains(offscreen));
        emit backend->avatarLoaded(logo, backend->avatarImages.value(logo));
        emit backend->avatarLoaded(creator, backend->avatarImages.value(creator));
        cards = findAccessible<QListWidget>(&window, stageName);
        QVERIFY(cards);
        QVERIFY(containsPaint(projects, logoColor));
        QVERIFY(containsPaint(cards, creatorColor));
        QVERIFY(containsPaint(cards, assigneeColor));
    }

    void kanbanColumnEdgesResizeIndependentlyAndPersist()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        const auto account = QStringLiteral("fixture/kanban-widths");
        backend->accountScope = account;
        auto first = backend->stages.first();
        first.name = QStringLiteral("Первая колонка");
        auto task = first.tasks.first();
        first.tasks.clear();
        for (int i = 0; i < 20; ++i) {
            task.id = QString::number(650 + i);
            first.tasks.append(task);
        }
        backend->stages = {first};
        for (int i = 1; i <= 5; ++i) {
            Stage stage;
            stage.id = QString::number(50 + i);
            stage.name = QStringLiteral("Этап %1").arg(i);
            stage.tasks = {task};
            backend->stages.append(stage);
        }
        Project another;
        another.id = QStringLiteral("102");
        another.name = QStringLiteral("Другой проект");
        backend->projects.append(another);

        const auto moveGrip = [](QWidget *grip, const QPoint &global, int delta, Qt::MouseButtons buttons) {
            const auto point = global + QPoint(delta, 0);
            QMouseEvent move(QEvent::MouseMove, grip->mapFromGlobal(point), point,
                             Qt::NoButton, buttons, Qt::NoModifier);
            QApplication::sendEvent(grip, &move);
        };
        const auto drag = [&](QWidget *grip, int delta) {
            const auto local = grip->rect().center();
            const auto global = grip->mapToGlobal(local);
            QTest::mousePress(grip, Qt::LeftButton, Qt::NoModifier, local);
            moveGrip(grip, global, delta, Qt::LeftButton);
            QTest::mouseRelease(grip, Qt::LeftButton, Qt::NoModifier, local);
        };

        {
            MainWindow window(backend.get());
            window.resize(960, 650);
            window.show();
            QApplication::setActiveWindow(&window);
            auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
            QVERIFY(sections);
            sections->setCurrentRow(1);
            auto *board = window.findChild<BoardWidget *>();
            QVERIFY(board);
            auto *scroll = board->findChild<QScrollArea *>();
            auto *firstList = findAccessible<QListWidget>(board, first.name);
            auto *nextList = findAccessible<QListWidget>(board, backend->stages.at(1).name);
            QVERIFY(scroll && firstList && nextList);
            firstList->setCurrentRow(3);
            const auto *selected = firstList->currentItem();
            const auto *firstColumn = firstList->parentWidget();
            const auto *nextColumn = nextList->parentWidget();
            QTRY_COMPARE(firstColumn->width(), 232);
            QCOMPARE(nextColumn->width(), 232);
            QTRY_COMPARE(nextColumn->x() - firstColumn->x(), 244);
            auto *grip = firstColumn->findChild<QWidget *>(QStringLiteral("kanbanColumnGrip"));
            QVERIFY(grip && grip->isVisible());
            QVERIFY(grip->geometry().left() > firstList->geometry().right());
            QVERIFY(!findAccessible<QComboBox>(&window, QStringLiteral("Количество видимых колонок канбана")));
            const int loaded = backend->boardRequests;
            drag(grip, 90);
            QTRY_COMPARE(firstColumn->width(), 322);
            QCOMPARE(nextColumn->width(), 232);
            QCOMPARE(firstList->currentItem(), selected);
            QCOMPARE(backend->boardRequests, loaded);

            const auto local = grip->rect().center();
            const auto global = grip->mapToGlobal(local);
            QTest::mousePress(grip, Qt::LeftButton, Qt::NoModifier, local);
            moveGrip(grip, global, -60, Qt::LeftButton);
            QCOMPARE(firstColumn->width(), 262);
            moveGrip(grip, global, -80, Qt::NoButton);
            QCOMPARE(firstColumn->width(), 322);
            QTest::mouseRelease(grip, Qt::LeftButton, Qt::NoModifier, local);
            QTest::keyClick(grip, Qt::Key_Right);
            QCOMPARE(firstColumn->width(), 338);
            QTest::keyClick(grip, Qt::Key_Left);
            QCOMPARE(firstColumn->width(), 322);

            window.resize(720, 500);
            QTRY_COMPARE(window.width(), 720);
            QCOMPARE(firstColumn->width(), 322);
            QCOMPARE(nextColumn->width(), 232);
            scroll->horizontalScrollBar()->setValue(scroll->horizontalScrollBar()->maximum());
            auto *last = findAccessible<QListWidget>(board, backend->stages.last().name);
            QVERIFY(last);
            auto *lastGrip = last->parentWidget()->findChild<QWidget *>(QStringLiteral("kanbanColumnGrip"));
            QVERIFY(lastGrip);
            QTRY_VERIFY(lastGrip->mapTo(scroll->viewport(), QPoint()).x() + lastGrip->width() <= scroll->viewport()->width());
            drag(lastGrip, -40);
            QTRY_COMPARE(last->parentWidget()->width(), 192);
            drag(lastGrip, 72);
            QTRY_COMPARE(last->parentWidget()->width(), 264);
            QCOMPARE(firstColumn->width(), 322);
            const auto widthOfFirst = [&] {
                auto *list = findAccessible<QListWidget>(board, first.name);
                return list ? list->parentWidget()->width() : -1;
            };

            QTest::keyClick(&window, Qt::Key_F5);
            QTRY_VERIFY(backend->boardRequests > loaded);
            QTRY_COMPARE(widthOfFirst(), 322);
            auto *filter = findAccessible<QLineEdit>(&window, QStringLiteral("Фильтр задач"));
            QVERIFY(filter);
            filter->setText(QStringLiteral("нет совпадений"));
            QTRY_COMPARE(widthOfFirst(), 322);
            filter->clear();
            auto *projects = findAccessible<QListWidget>(&window, QStringLiteral("Проекты"));
            QVERIFY(projects && projects->count() == 2);
            projects->setCurrentRow(1);
            emit backend->boardLoaded(another.id, backend->stages);
            QTRY_COMPARE(widthOfFirst(), 232);
            projects->setCurrentRow(0);
            QTRY_COMPARE(widthOfFirst(), 322);

            backend->accountScope = QStringLiteral("fixture/another-user");
            emit backend->accountScopeChanged();
            QTRY_COMPARE(widthOfFirst(), 232);
        }

        backend->accountScope = account;
        MainWindow reopened(backend.get());
        reopened.resize(960, 650);
        reopened.show();
        auto *sections = findAccessible<QListWidget>(&reopened, QStringLiteral("Разделы"));
        QVERIFY(sections);
        sections->setCurrentRow(1);
        auto *firstList = findAccessible<QListWidget>(&reopened, first.name);
        QVERIFY(firstList);
        QTRY_COMPARE(firstList->parentWidget()->width(), 322);
    }

    void kanbanCardsWrapAndLoadOnlyVisiblePeople()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        const QUrl creator(QStringLiteral("https://example.bitrix24.ru/upload/creator.png"));
        const QUrl assignee(QStringLiteral("https://example.bitrix24.ru/upload/assignee.png"));
        const QUrl lower(QStringLiteral("https://example.bitrix24.ru/upload/lower.png"));
        const QUrl later(QStringLiteral("https://example.bitrix24.ru/upload/later.png"));
        const QColor creatorColor("#bd4380");
        const QColor assigneeColor("#336cab");
        auto image = [](const QColor &color) {
            QImage pixels(32, 32, QImage::Format_RGB32);
            pixels.fill(color);
            QByteArray bytes;
            QBuffer output(&bytes);
            output.open(QIODevice::WriteOnly);
            pixels.save(&output, "PNG");
            return bytes;
        };
        backend->avatarImages = {{creator, image(creatorColor)}, {assignee, image(assigneeColor)}};
        auto stage = backend->stages.first();
        stage.name = QStringLiteral("В работе");
        auto card = stage.tasks.first();
        card.name = QStringLiteral("Сверить статусы возвратов СверхдлинныйИдентификаторНакладной");
        card.createdBy = QStringLiteral("Мария Котова");
        card.assignee = QStringLiteral("Игорь Смирнов");
        card.tags = {QStringLiteral("Техподдержка"), QStringLiteral("Логистика"), QStringLiteral("Возвраты")};
        card.creatorAvatarUrl = creator;
        card.assigneeAvatarUrl = assignee;
        card.due = QStringLiteral("23 ноября, 09:00");
        stage.tasks = {card};
        Task shortCard = card;
        shortCard.name = QStringLiteral("Сверить статусы");
        shortCard.tags.clear();
        shortCard.due.clear();
        for (int i = 0; i < 14; ++i) {
            shortCard.id = QString::number(720 + i);
            stage.tasks.append(shortCard);
        }
        stage.tasks.last().creatorAvatarUrl = lower;
        stage.taskCount = stage.tasks.size();
        backend->stages = {stage};
        for (int i = 1; i <= 6; ++i) {
            auto next = stage;
            next.id = QString::number(40 + i);
            next.name = QStringLiteral("Этап %1").arg(i);
            next.tasks = {shortCard};
            next.taskCount = next.tasks.size();
            if (i == 6)
                next.tasks.first().creatorAvatarUrl = later;
            backend->stages.append(next);
        }

        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        findAccessible<QListWidget>(&window, QStringLiteral("Разделы"))->setCurrentRow(1);
        auto *board = window.findChild<BoardWidget *>();
        QVERIFY(board);
        auto *column = findAccessible<QListWidget>(board, stage.name);
        QVERIFY(column && column->count() >= 2);
        const auto *first = column->item(0);
        QTRY_VERIFY_WITH_TIMEOUT(column->visualItemRect(first).height() > 120, 1200);
        QCOMPARE(first->data(workspace::Title).toString(), card.name);
        QVERIFY(first->toolTip().contains(QStringLiteral("Постановщик: Мария Котова")));
        QVERIFY(first->toolTip().contains(QStringLiteral("Исполнитель: Игорь Смирнов")));
        QVERIFY(first->data(Qt::AccessibleDescriptionRole).toString().contains(QStringLiteral("Логистика")));
        QTRY_COMPARE(backend->avatarRequests.count(creator), 1);
        QTRY_COMPARE(backend->avatarRequests.count(assignee), 1);
        QVERIFY(!backend->avatarRequests.contains(lower));
        QVERIFY(!backend->avatarRequests.contains(later));
        const auto wide = qEnvironmentVariable("B24_KANBAN_WIDE_SCREENSHOT");
        if (!wide.isEmpty())
            QVERIFY(window.grab().save(wide));

        window.resize(720, 500);
        QCoreApplication::processEvents();
        QTRY_COMPARE_WITH_TIMEOUT(column->visualItemRect(first).height(), column->sizeHintForRow(0), 1000);
        const auto rect = column->visualItemRect(first);
        QVERIFY(rect.height() > column->visualItemRect(column->item(1)).height());
        QStyleOptionViewItem option;
        option.initFrom(column->viewport());
        option.rect = QRect(QPoint(), rect.size());
        QImage snapshot(option.rect.size(), QImage::Format_ARGB32_Premultiplied);
        snapshot.fill(Qt::transparent);
        {
            QPainter painter(&snapshot);
            column->itemDelegate()->paint(&painter, option, column->model()->index(0, 0));
        }
        const auto boundsOf = [&snapshot](const QColor &color) {
            QRect bounds;
            for (int y = 0; y < snapshot.height(); ++y)
                for (int x = 0; x < snapshot.width(); ++x)
                    if (snapshot.pixelColor(x, y) == color)
                        bounds = bounds.united(QRect(x, y, 1, 1));
            return bounds;
        };
        const auto creatorBounds = boundsOf(creatorColor);
        const auto assigneeBounds = boundsOf(assigneeColor);
        QVERIFY(creatorBounds.height() >= 18);
        QVERIFY(assigneeBounds.height() >= 18);
        QVERIFY(creatorBounds.right() < assigneeBounds.left());
        const auto compact = qEnvironmentVariable("B24_KANBAN_COMPACT_SCREENSHOT");
        if (!compact.isEmpty())
            QVERIFY(window.grab().save(compact));

        column->scrollToBottom();
        QTRY_COMPARE(backend->avatarRequests.count(lower), 1);
        auto *scroll = board->findChild<QScrollArea *>();
        QVERIFY(scroll && scroll->horizontalScrollBar()->maximum() > 0);
        scroll->horizontalScrollBar()->setValue(scroll->horizontalScrollBar()->maximum());
        QTRY_COMPARE(backend->avatarRequests.count(later), 1);

        board->setView(BoardWidget::View::List);
        auto *list = findAccessible<QListWidget>(board, QStringLiteral("Список задач"));
        QVERIFY(list && list->isVisible());
        QCOMPARE(list->item(0)->text().section(QLatin1Char('\n'), 0, 0), card.name);
    }

    void boardOpensTaskAndTaskChat()
    {
        DemoBackend demo;
        demo.start();
        QPointer<MainWindow> window = new MainWindow(&demo);
        window->setAttribute(Qt::WA_DeleteOnClose);
        window->resize(960, 650);
        window->show();
        QApplication::setActiveWindow(window);

        auto *sections = findAccessible<QListWidget>(window, QStringLiteral("Разделы"));
        auto *projects = findAccessible<QListWidget>(window, QStringLiteral("Проекты"));
        QVERIFY(sections);
        QVERIFY(projects);
        QTest::keyClick(window, Qt::Key_2, Qt::AltModifier);
        QTRY_COMPARE(sections->currentRow(), 1);
        QTRY_VERIFY(projects->currentItem());

        auto *board = window->findChild<BoardWidget *>();
        QVERIFY(board);
        // Доска приходит асинхронно: ждём колонки этапов (в списке задач есть всегда).
        QTRY_VERIFY(board->findChildren<QListWidget *>().size() > 1);
        QListWidget *work = nullptr;
        QTRY_VERIFY((work = findAccessible<QListWidget>(board, QStringLiteral("В работе"))));
        QVERIFY(work->count() > 0);
        work->setFocus();
        QTest::keyClick(work, Qt::Key_Down);
        QTest::keyClick(work, Qt::Key_Return);

        const auto checkTask = [&window](const QString &name) {
            auto *card = window->findChild<QDialog *>(QStringLiteral("taskCardDialog"));
            QVERIFY(card);
            QVERIFY(card->isVisible());
            QCOMPARE(card->windowModality(), Qt::WindowModal);
            auto *title = findAccessible<QLineEdit>(card, QStringLiteral("Заголовок задачи"));
            auto *messages = findAccessible<QListWidget>(card, QStringLiteral("Чат карточки"));
            QVERIFY(title);
            QVERIFY(messages);
            QTRY_COMPARE(title->text(), name);
            QTRY_COMPARE(messages->count(), 2);
        };
        checkTask(QStringLiteral("Собрать тестовый стенд"));
        QPointer<QDialog> card = window->findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        const auto modalPath = qEnvironmentVariable("B24_TASK_SCREENSHOT");
        if (!modalPath.isEmpty())
            QVERIFY(card->grab().save(modalPath));
        QTest::keyClick(card, Qt::Key_Escape);
        QTRY_VERIFY(card.isNull());

        const auto screenshotPath = qEnvironmentVariable("B24_SMOKE_SCREENSHOT");
        if (!screenshotPath.isEmpty())
            QVERIFY(window->grab().save(screenshotPath));
        const auto compactPath = qEnvironmentVariable("B24_COMPACT_SCREENSHOT");
        if (!compactPath.isEmpty()) {
            window->resize(720, 500);
            QTest::qWait(60);
            QVERIFY(window->grab().save(compactPath));
        }
        for (const auto &stage : {QStringLiteral("К выполнению"), QStringLiteral("Готово")}) {
            auto *column = findAccessible<QListWidget>(board, stage);
            QVERIFY(column);
            const auto expected = column->item(0)->text().section(QLatin1Char('\n'), 0, 0);
            column->setCurrentRow(0);
            QTest::keyClick(column, Qt::Key_Return);
            checkTask(expected);
            card = window->findChild<QDialog *>(QStringLiteral("taskCardDialog"));
            QTest::keyClick(card, Qt::Key_Escape);
            QTRY_VERIFY(card.isNull());
        }

        bool ok = false;
        const int sampleMs = qEnvironmentVariableIntValue("B24_SMOKE_SAMPLE_MS", &ok);
        const bool sample = ok && sampleMs > 0 && sampleMs <= 10000;
        if (sample) {
            qInfo("B24_UI_OPEN");
            QTest::qWait(sampleMs);
        }
        window->close();
        QTRY_VERIFY(window.isNull());
        if (sample) {
            qInfo("B24_UI_CLOSED");
            QTest::qWait(sampleMs);
        }
        demo.stop();
    }

    void chatSendsFromKeyboardAndRestoresDraft()
    {
        DemoBackend demo;
        demo.start();
        MainWindow window(&demo);
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);

        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
        auto *messages = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *composer = findAccessible<QPlainTextEdit>(&window, QStringLiteral("Текст сообщения"));
        QVERIFY(chats);
        QVERIFY(messages);
        QVERIFY(composer);
        QTRY_COMPARE(messages->count(), 3);
        QTRY_VERIFY(chats->currentItem());
        composer->setFocus();

        const auto screenshotPath = qEnvironmentVariable("B24_CHAT_SCREENSHOT");
        if (!screenshotPath.isEmpty())
            QVERIFY(window.grab().save(screenshotPath));

        QTest::keyClicks(composer, QStringLiteral("First line"));
        QTest::keyClick(composer, Qt::Key_Return, Qt::ShiftModifier);
        QTest::keyClicks(composer, QStringLiteral("Second line"));
        QCOMPARE(composer->toPlainText(), QStringLiteral("First line\nSecond line"));
        chats->setCurrentRow(1);
        QVERIFY(composer->toPlainText().isEmpty());
        chats->setCurrentRow(0);
        QCOMPARE(composer->toPlainText(), QStringLiteral("First line\nSecond line"));

        QSignalSpy sent(&demo, &Backend::messageSent);
        QTest::keyClick(composer, Qt::Key_Return);
        QTRY_COMPARE(sent.count(), 1);
        QCOMPARE(sent.first().first().toString(), QStringLiteral("chat:c-1"));
        QTRY_VERIFY(composer->toPlainText().isEmpty());

        QTest::keyClick(&window, Qt::Key_3, Qt::AltModifier);
        auto *notifications = findAccessible<QListWidget>(&window, QStringLiteral("Уведомления"));
        QVERIFY(notifications);
        QTRY_COMPARE(notifications->count(), 1);
        QCOMPARE(notifications->item(0)->data(workspace::Id).toString(), QStringLiteral("n-2"));
    }

    void scopesTimeoutRecoveryToTheAffectedOperation_data()
    {
        QTest::addColumn<QString>("kind");
        for (const auto *kind : {"push", "history", "send-chat", "task-history", "send-task", "task-update"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }

    void scopesTimeoutRecoveryToTheAffectedOperation()
    {
        QFETCH(QString, kind);
        const auto timeout = QStringLiteral("Ошибка сети или истекло время ожидания");
        const auto initial = QStringLiteral("Портал вернул неизвестный ответ");
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdMessageSends = true;
        backend->holdTaskUpdate = true;
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *status = findAccessible<QLabel>(&window, QStringLiteral("Состояние"));
        auto *messages = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *input = findAccessible<QPlainTextEdit>(&window, QStringLiteral("Текст сообщения"));
        QVERIFY(status && messages && input);
        QTRY_VERIFY(messages->count() > 0);
        emit backend->failed(QStringLiteral("unclassified"), initial);
        QCOMPARE(status->text(), initial);
        if (kind == QLatin1String("push")) {
            for (const auto *operation : {"push", "pins:c-1", "chat-history-identity", "task-chat-titles"}) {
                emit backend->failed(QString::fromLatin1(operation), timeout);
                QCOMPARE(status->text(), initial);
            }
            emit backend->failed(QStringLiteral("chat:previous-dialog"), timeout);
            QCOMPARE(status->text(), initial);
            const auto access = QStringLiteral("Действие требует новой веб-сессии портала");
            emit backend->failed(QStringLiteral("push"), access);
            QCOMPARE(status->text(), access);
            const auto diagnostic = QStringLiteral("Портал не предоставил допустимый защищённый канал уведомлений");
            emit backend->failed(QStringLiteral("push"), diagnostic);
            QCOMPARE(status->text(), diagnostic);
            emit backend->failed(QStringLiteral("unclassified"), timeout);
            QCOMPARE(status->text(), timeout);
            return;
        }
        if (kind == QLatin1String("history")) {
            const auto count = messages->count();
            emit backend->failed(QStringLiteral("chat:c-1"), timeout);
            QCOMPARE(messages->count(), count);
            QVERIFY(status->text().contains(QStringLiteral("сообщения")));
            QVERIFY(status->text().contains(QStringLiteral("«Обновить»")));
            return;
        }
        if (kind == QLatin1String("send-chat")) {
            input->setPlainText(QStringLiteral("Черновик сообщения"));
            QTest::keyClick(input, Qt::Key_Return);
            QCOMPARE(backend->chatSendRequests, 1);
            emit backend->failed(QStringLiteral("send:chat:c-1"),
                                 timeout + QStringLiteral(". Проверьте историю перед повторной отправкой"));
            QCOMPARE(input->toPlainText(), QStringLiteral("Черновик сообщения"));
            QVERIFY(status->text().contains(QStringLiteral("подтвердить отправку")));
            QVERIFY(status->text().contains(QStringLiteral("историю перед повторной отправкой")));
            QCOMPARE(backend->chatSendRequests, 1);
            return;
        }

        window.openTaskCard(QStringLiteral("501"));
        auto *card = dynamic_cast<ui::TaskCardDialog *>(
            window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        QVERIFY(card);
        auto *cardStatus = findAccessible<QLabel>(card, QStringLiteral("Состояние карточки"));
        QVERIFY(cardStatus);
        emit backend->failed(QStringLiteral("unclassified"), initial);
        if (kind == QLatin1String("task-history")) {
            emit backend->taskMessagesLoaded(QStringLiteral("501"), backend->messages);
            const auto count = card->messageList()->count();
            QVERIFY(count > 0);
            emit backend->failed(QStringLiteral("task-messages:501"), timeout);
            QCOMPARE(card->messageList()->count(), count);
            QVERIFY(cardStatus->text().contains(QStringLiteral("чат задачи")));
            QVERIFY(cardStatus->text().contains(QStringLiteral("карточку заново")));
        } else if (kind == QLatin1String("send-task")) {
            card->messageInput()->setPlainText(QStringLiteral("Ответ по задаче"));
            QTest::keyClick(card->messageInput(), Qt::Key_Return);
            QCOMPARE(backend->taskSendRequests, 1);
            emit backend->failed(QStringLiteral("send:task:501"),
                                 timeout + QStringLiteral(". Проверьте историю перед повторной отправкой"));
            QCOMPARE(card->messageInput()->toPlainText(), QStringLiteral("Ответ по задаче"));
            QVERIFY(cardStatus->text().contains(QStringLiteral("подтвердить отправку")));
            QVERIFY(cardStatus->text().contains(QStringLiteral("историю перед повторной отправкой")));
            QCOMPARE(backend->taskSendRequests, 1);
        } else {
            auto *edit = findAccessible<QPushButton>(card, QStringLiteral("Изменить задачу"));
            QVERIFY(edit && edit->isEnabled());
            edit->click();
            auto *title = findAccessible<QLineEdit>(card, QStringLiteral("Заголовок задачи"));
            auto *save = findAccessible<QPushButton>(card, QStringLiteral("Сохранить задачу"));
            QVERIFY(title && save);
            title->setText(QStringLiteral("Изменённая задача"));
            save->click();
            QCOMPARE(backend->taskUpdateRequests, 1);
            emit backend->failed(QStringLiteral("update-task:501"),
                                 timeout + QStringLiteral(". Проверьте сохранённые изменения перед повтором"));
            QCOMPARE(title->text(), QStringLiteral("Изменённая задача"));
            QVERIFY(save->isEnabled());
            QVERIFY(cardStatus->text().contains(QStringLiteral("подтвердить сохранение")));
            QVERIFY(cardStatus->text().contains(QStringLiteral("перед повтором")));
            QCOMPARE(backend->taskUpdateRequests, 1);
        }
        QCOMPARE(status->text(), initial);
    }

    void taskChatSearchUsesTaskPromptAndHidesGroupCreation()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        backend->accountScope = QStringLiteral("fixture/task-chat-search-first");
        MainWindow window(backend.get());
        window.resize(960, 720);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *tabs = findAccessible<QTabBar>(&window, QStringLiteral("Список чатов"));
        auto *query = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск чатов"));
        auto *create = findAccessible<QPushButton>(&window, QStringLiteral("Создать групповой чат"));
        QVERIFY(tabs && query && create);
        QVERIFY(create->isVisible());
        tabs->setCurrentIndex(1);
        QVERIFY(create->isHidden());
        QCOMPARE(query->placeholderText(), QStringLiteral("Название или номер задачи"));
        const auto taskNumber = QStringLiteral("№ 99123");
        query->setText(taskNumber);
        QTest::keyClick(query, Qt::Key_Return);
        QCOMPARE(backend->searchedTaskChats, taskNumber);
        QVERIFY(backend->searchedChats.isEmpty());
        Chat result;
        result.id = QStringLiteral("chat555");
        result.title = QStringLiteral("Сборка наличия");
        result.taskChat = true;
        result.taskId = QStringLiteral("99123");
        emit backend->taskChatsFound(taskNumber, {result});
        auto *rows = findAccessible<QListWidget>(&window, QStringLiteral("Чаты задач"));
        QVERIFY(rows);
        QCOMPARE(rows->count(), 1);
        QCOMPARE(rows->item(0)->data(workspace::Id).toString(), result.id);

        backend->accountScope = QStringLiteral("fixture/task-chat-search-second");
        emit backend->accountScopeChanged();
        QVERIFY(query->text().isEmpty());
        QCOMPARE(rows->count(), 0);
        emit backend->taskChatsFound(taskNumber, {result});
        QCOMPARE(rows->count(), 0);

        tabs->setCurrentIndex(0);
        QVERIFY(create->isVisible());
        QCOMPARE(query->placeholderText(), QStringLiteral("Поиск чатов"));
        query->setText(QStringLiteral("Команда"));
        QTest::keyClick(query, Qt::Key_Return);
        QCOMPARE(backend->searchedChats, QStringLiteral("Команда"));
        QCOMPARE(backend->searchedTaskChats, taskNumber);
    }

    void chatWheelUsesCompactNotchesAndKeepsNativePreciseScrolling()
    {
        ui::HistoryList history(nullptr);
        QListWidget native;
        QLabel state;
        QLabel nativeState;
        const auto scope = QStringLiteral("chat51");
        QVector<Message> messages;
        for (int index = 101; index <= 140; ++index) {
            Message message;
            message.id = QString::number(index);
            message.text = QStringLiteral("Сообщение %1").arg(index);
            messages.append(message);
        }
        for (auto *list : {static_cast<QListWidget *>(&history), &native}) {
            workspace::configureRows(list, workspace::Row::Message);
            list->resize(480, 260);
            list->show();
        }
        history.showHistory(scope, &state, messages);
        ui::presentMessages(&native, &nativeState, messages);
        QTRY_VERIFY(history.verticalScrollBar()->maximum() > 500);
        QTRY_COMPARE(native.verticalScrollBar()->maximum(), history.verticalScrollBar()->maximum());
        const auto send = [](QListWidget &list, QPoint angle, QPoint pixels = {},
                             Qt::ScrollPhase phase = Qt::NoScrollPhase,
                             Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
            auto *view = list.viewport();
            const auto pos = view->rect().center();
            QWheelEvent wheel(pos, view->mapToGlobal(pos), pixels, angle, Qt::NoButton, modifiers, phase, false);
            QApplication::sendEvent(view, &wheel);
        };
        const auto movement = [&send](QListWidget &list, QPoint angle, QPoint pixels = {},
                                     Qt::ScrollPhase phase = Qt::NoScrollPhase,
                                     Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
            auto *scroll = list.verticalScrollBar();
            const int start = scroll->maximum() / 2;
            scroll->setValue(start);
            send(list, angle, pixels, phase, modifiers);
            return scroll->value() - start;
        };
        const int nativeNotch = movement(native, {0, -120});
        QVERIFY(nativeNotch > 3);
        const int chatNotch = qRound(nativeNotch / 8.0);
        auto *scroll = history.verticalScrollBar();
        const int middle = scroll->maximum() / 2;
        scroll->setValue(middle);
        send(history, {0, -120});
        QCOMPARE(scroll->value(), middle);
        QTRY_COMPARE_WITH_TIMEOUT(scroll->value(), middle + chatNotch, 500);
        send(history, {0, 120});
        QCOMPARE(scroll->value(), middle + chatNotch);
        QTRY_COMPARE_WITH_TIMEOUT(scroll->value(), middle, 500);

        send(history, {0, -120});
        send(history, {0, -120});
        QCOMPARE(scroll->value(), middle);
        QTRY_COMPARE_WITH_TIMEOUT(scroll->value(), middle + 2 * chatNotch, 500);

        const int beforeChange = scroll->value();
        send(history, {0, -120});
        send(history, {0, 120});
        QTRY_COMPARE_WITH_TIMEOUT(scroll->value(), beforeChange - chatNotch, 500);

        const auto precisePixels = QPoint(0, -17);
        const int preciseMovement = movement(native, {0, -120}, precisePixels, Qt::ScrollUpdate);
        const int beforePrecise = scroll->value();
        send(history, {0, -120});
        send(history, {0, -120}, precisePixels, Qt::ScrollUpdate);
        QCOMPARE(scroll->value(), beforePrecise + preciseMovement);
        QTest::qWait(150);
        QCOMPARE(scroll->value(), beforePrecise + preciseMovement);

        send(history, {0, -120});
        scroll->setValue(middle);
        QTest::qWait(150);
        QCOMPARE(scroll->value(), middle);
        QVERIFY(qAbs(movement(history, {0, -30}) - movement(native, {0, -30})) <= 1);
        QCOMPARE(movement(history, {0, -120}, {}, Qt::NoScrollPhase, Qt::ControlModifier),
                 movement(native, {0, -120}, {}, Qt::NoScrollPhase, Qt::ControlModifier));

        QStringList older;
        history.setOnOlder([&older](const QString &before) { older.append(before); });
        const int nearTop = scroll->minimum() + qMax(1, chatNotch / 2);
        scroll->setValue(nearTop);
        send(history, {0, 120});
        send(history, {0, 120});
        send(history, {0, -120});
        QTRY_COMPARE_WITH_TIMEOUT(scroll->value(), nearTop + chatNotch, 500);
        QVERIFY(older.isEmpty());

        scroll->setValue(nearTop);
        send(history, {0, 120});
        send(history, {0, 120});
        QTRY_COMPARE_WITH_TIMEOUT(scroll->value(), scroll->minimum(), 500);
        QTRY_COMPARE_WITH_TIMEOUT(older, QStringList{QStringLiteral("101")}, 500);
        send(history, {0, 120});
        QCOMPARE(older, QStringList{QStringLiteral("101")});
    }

    void olderPagesContinuePastCacheLimitAndFromMessageContext()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        const auto dialog = backend->chats.first().id;
        const auto taskDialog = QStringLiteral("chat51");
        backend->stages.first().tasks.first().chatDialogId = taskDialog;
        const auto message = [](int id) {
            Message value;
            value.id = QString::number(id);
            value.text = QStringLiteral("Сообщение %1").arg(id);
            return value;
        };
        backend->messages.clear();
        for (int id = 101; id <= 300; ++id)
            backend->messages.append(message(id));
        backend->taskMessages = {message(400)};
        backend->pinnedMessages.insert(dialog, {message(90)});
        backend->pinnedMessages.insert(taskDialog, {message(95)});
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        const auto rowId = [](QListWidget *history, int row) {
            return history->item(row)->data(workspace::Id).toString();
        };
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *pager = window.findChild<QPushButton *>(QStringLiteral("olderChatMessages"));
        auto *latest = findAccessible<QPushButton>(&window, QStringLiteral("К последним сообщениям"));
        QVERIFY(history && pager && latest);
        QTRY_COMPARE(history->count(), 200);
        history->scrollToTop();
        QVERIFY(pager->isVisible());
        pager->click();
        QCOMPARE(backend->olderBefore, QStringLiteral("101"));
        emit backend->olderChatMessagesLoaded(dialog, QStringLiteral("101"), false, {message(100)}, true);
        QCOMPARE(history->count(), 200);
        QCOMPARE(rowId(history, 0), QStringLiteral("100"));
        QCOMPARE(rowId(history, 199), QStringLiteral("299"));
        QVERIFY(pager->isVisible() && latest->isVisible());
        emit backend->chatMessagesLoaded(dialog, backend->messages);
        QCOMPARE(rowId(history, 0), QStringLiteral("100"));
        latest->click();
        QTRY_COMPARE(history->count(), 200);
        QCOMPARE(rowId(history, 0), QStringLiteral("101"));
        QVERIFY(!latest->isVisible());

        auto *pin = findAccessible<QPushButton>(&window, QStringLiteral("Закреплённые сообщения"));
        QVERIFY(pin && pin->isVisible());
        pin->click();
        QCOMPARE(backend->contextMessage, QStringLiteral("90"));
        emit backend->messageContextLoaded(dialog, QStringLiteral("90"), {message(89), message(90), message(91)});
        QVERIFY(pager->isVisible());
        pager->click();
        QCOMPARE(backend->olderBefore, QStringLiteral("89"));
        emit backend->olderChatMessagesLoaded(dialog, QStringLiteral("89"), false, {message(88)}, false);
        QCOMPARE(rowId(history, 0), QStringLiteral("88"));
        QVERIFY(!pager->isVisible() && latest->isVisible());

        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        auto *taskPin = findAccessible<QPushButton>(card, QStringLiteral("Закреплённые сообщения"));
        auto *taskPager = card->findChild<QPushButton *>(QStringLiteral("olderChatMessages"));
        auto *taskHistory = findAccessible<QListWidget>(card, QStringLiteral("Чат карточки"));
        QVERIFY(taskPin && taskPin->isVisible() && taskPager && taskHistory);
        taskPin->click();
        QCOMPARE(backend->contextMessage, QStringLiteral("95"));
        emit backend->messageContextLoaded(taskDialog, QStringLiteral("95"), {message(94), message(95)});
        QVERIFY(taskPager->isVisible());
        taskPager->click();
        QCOMPARE(backend->olderScope, QStringLiteral("501"));
        QCOMPARE(backend->olderBefore, QStringLiteral("94"));
        QVERIFY(backend->olderTask);
        emit backend->olderChatMessagesLoaded(QStringLiteral("501"), QStringLiteral("94"), true, {message(93)}, false);
        QCOMPARE(rowId(taskHistory, 0), QStringLiteral("93"));
        emit backend->pinnedMessagesLoaded(taskDialog, {message(92)});
        taskPin->click();
        QCOMPARE(backend->contextMessage, QStringLiteral("92"));
        emit backend->chatHistoryRemoved(taskDialog, {});
        emit backend->messageContextLoaded(taskDialog, QStringLiteral("92"), {message(92)});
        QCOMPARE(taskHistory->count(), 0);

        emit backend->pinnedMessagesLoaded(dialog, {message(87)});
        pin->click();
        QCOMPARE(backend->contextMessage, QStringLiteral("87"));
        emit backend->chatHistoryRemoved(dialog, {});
        emit backend->messageContextLoaded(dialog, QStringLiteral("87"), {message(87)});
        QCOMPARE(history->count(), 0);
    }

    void earlierHistoryKeepsViewportAndChatScope()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto other = backend->chats.first();
        other.id = QStringLiteral("chat42");
        other.title = QStringLiteral("Другая переписка");
        backend->chats.append(other);
        const auto message = [](int id) {
            Message entry;
            entry.id = QString::number(id);
            entry.text = QStringLiteral("Сообщение №%1").arg(id);
            return entry;
        };
        backend->messages.clear();
        for (int id = 101; id <= 125; ++id)
            backend->messages.append(message(id));
        backend->linkedMessages = {message(501)};
        backend->taskMessages = {message(601)};
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *editor = findAccessible<QPlainTextEdit>(&window, QStringLiteral("Текст сообщения"));
        auto *pager = window.findChild<QPushButton *>(QStringLiteral("olderChatMessages"));
        QVERIFY(chats && history && editor && pager);
        QTRY_COMPARE(history->count(), 25);
        history->scrollToTop();
        editor->setPlainText(QStringLiteral("Сохранённый черновик"));
        QTRY_VERIFY(pager->isVisible());
        pager->click();
        QCOMPARE(backend->olderBefore, QStringLiteral("101"));
        QVERIFY(!backend->olderTask);
        QVERIFY(!pager->isEnabled());
        emit backend->olderChatMessagesLoaded(QStringLiteral("c-1"), QStringLiteral("999"), false,
                                              {message(31)}, true);
        QCOMPARE(history->count(), 25);
        emit backend->olderChatMessagesLoaded(QStringLiteral("c-1"), QStringLiteral("101"), false,
                                              {message(99), message(100), message(101)}, true);
        QTRY_COMPARE(history->count(), 27);
        QCOMPARE(history->item(0)->data(workspace::Id).toString(), QStringLiteral("99"));
        QTRY_VERIFY(history->itemAt(QPoint(4, 1)));
        QCOMPARE(history->itemAt(QPoint(4, 1))->data(workspace::Id).toString(), QStringLiteral("101"));
        QCOMPARE(editor->toPlainText(), QStringLiteral("Сохранённый черновик"));

        auto latest = backend->messages;
        latest.removeFirst();
        latest.append(message(126));
        emit backend->chatMessagesLoaded(QStringLiteral("c-1"), latest);
        QCOMPARE(history->count(), 28);
        QCOMPARE(history->item(27)->data(workspace::Id).toString(), QStringLiteral("126"));
        latest.removeFirst();
        emit backend->chatMessagesLoaded(QStringLiteral("c-1"), latest);
        QCOMPARE(history->count(), 27);
        QCOMPARE(history->item(3)->data(workspace::Id).toString(), QStringLiteral("103"));
        pager->click();
        QCOMPARE(backend->olderBefore, QStringLiteral("99"));
        emit backend->chatHistoryRemoved(QStringLiteral("c-1"), {QStringLiteral("97"), QStringLiteral("98")});
        emit backend->olderChatMessagesLoaded(QStringLiteral("c-1"), QStringLiteral("99"), false,
                                              {message(97), message(98)}, true);
        QCOMPARE(history->item(0)->data(workspace::Id).toString(), QStringLiteral("99"));
        pager->click();
        QCOMPARE(backend->olderBefore, QStringLiteral("97"));
        emit backend->olderChatMessagesLoaded(QStringLiteral("c-1"), QStringLiteral("97"), false,
                                              {message(96)}, true);
        QCOMPARE(history->item(0)->data(workspace::Id).toString(), QStringLiteral("96"));
        emit backend->chatHistoryRemoved(QStringLiteral("c-1"), {}, 100);
        QCOMPARE(history->item(0)->data(workspace::Id).toString(), QStringLiteral("101"));
        QVERIFY(!pager->isVisible());
        emit backend->chatHistoryRemoved(QStringLiteral("c-1"), {});
        QVERIFY(history->count() == 0);
        chats->setCurrentRow(1);
        QTRY_COMPARE(history->count(), 1);
        emit backend->olderChatMessagesLoaded(QStringLiteral("c-1"), QStringLiteral("101"), false,
                                              {message(98)}, false);
        QCOMPARE(history->item(0)->data(workspace::Id).toString(), QStringLiteral("501"));

        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        auto *taskHistory = findAccessible<QListWidget>(card, QStringLiteral("Чат карточки"));
        auto *taskPager = card->findChild<QPushButton *>(QStringLiteral("olderChatMessages"));
        QVERIFY(taskHistory && taskPager);
        QTRY_VERIFY(taskPager->isVisible());
        taskPager->click();
        QCOMPARE(backend->olderScope, QStringLiteral("501"));
        QCOMPARE(backend->olderBefore, QStringLiteral("601"));
        QVERIFY(backend->olderTask);
        emit backend->olderChatMessagesLoaded(QStringLiteral("501"), QStringLiteral("601"), true,
                                              {message(600)}, false);
        QTRY_COMPARE(taskHistory->count(), 2);
        QCOMPARE(taskHistory->item(0)->data(workspace::Id).toString(), QStringLiteral("600"));
        QVERIFY(!taskPager->isVisible());
    }

    void replyDraftsStayScopedAndOpenExactOriginal()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto second = backend->chats.first();
        second.id = QStringLiteral("chat42");
        second.title = QStringLiteral("Другой чат");
        backend->chats.append(second);
        Message photo;
        photo.id = QStringLiteral("871");
        photo.outgoing = true;
        photo.files = {{QStringLiteral("88"), QStringLiteral("Снимок.jpg")}};
        Message reply;
        reply.id = QStringLiteral("872");
        reply.author = QStringLiteral("Анна");
        reply.text = QStringLiteral("Ответ");
        reply.replyId = QStringLiteral("999");
        backend->messages = {photo, reply};
        backend->taskMessages = {photo, reply};
        backend->stages[0].tasks[0].chatDialogId = QStringLiteral("chat51");
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
        auto *input = findAccessible<QPlainTextEdit>(&window, QStringLiteral("Текст сообщения"));
        auto *strip = findAccessible<QLabel>(&window, QStringLiteral("Цитата в черновике"));
        QVERIFY(history && chats && input && strip);
        QTRY_COMPARE(history->count(), 2);
        QCOMPARE(history->item(1)->data(workspace::ReplyPreview).toString(), QStringLiteral("Сообщение №999"));

        const auto chooseReply = [](QListWidget *list, int row) {
            list->setCurrentRow(row);
            const auto point = list->visualItemRect(list->item(row)).center();
            QContextMenuEvent event(QContextMenuEvent::Keyboard, point, list->viewport()->mapToGlobal(point));
            QApplication::sendEvent(list->viewport(), &event);
            auto *menu = visibleMessageMenu();
            QVERIFY(menu);
            QCOMPARE(menu->actions().first()->text(), QStringLiteral("Ответить"));
            menu->setActiveAction(menu->actions().first());
            QTest::keyClick(menu, Qt::Key_Return);
        };

        chooseReply(history, 0);
        QTRY_VERIFY(strip->isVisible());
        QVERIFY(strip->text().contains(QStringLiteral("Снимок.jpg")));
        input->setPlainText(QStringLiteral("Черновик"));
        chats->setCurrentRow(1);
        QVERIFY(input->toPlainText().isEmpty());
        QVERIFY(!strip->isVisible());
        chats->setCurrentRow(0);
        QCOMPARE(input->toPlainText(), QStringLiteral("Черновик"));
        QVERIFY(strip->isVisible());
        QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(backend->sentReplyScope, QStringLiteral("chat:c-1"));
        QCOMPARE(backend->sentReplyId, QStringLiteral("871"));
        QCOMPARE(backend->sentReplyText, QStringLiteral("Черновик"));
        emit backend->failed(QStringLiteral("send:chat:c-1"), QStringLiteral("Нет сети"));
        QCOMPARE(input->toPlainText(), QStringLiteral("Черновик"));
        QVERIFY(strip->isVisible());
        QTest::keyClick(input, Qt::Key_Return);
        chats->setCurrentRow(1);
        input->setPlainText(QStringLiteral("Другой черновик"));
        emit backend->messageSent(QStringLiteral("chat:c-1"), QStringLiteral("900"));
        QCOMPARE(input->toPlainText(), QStringLiteral("Другой черновик"));
        chats->setCurrentRow(0);
        QVERIFY(input->toPlainText().isEmpty());
        QVERIFY(!strip->isVisible());

        const auto clickQuote = [](QListWidget *list) {
            const auto row = list->visualItemRect(list->item(1));
            const auto *painter = static_cast<const workspace::RowPainter *>(list->itemDelegate());
            const auto bubble = painter->receiptArea(list->model()->index(1, 0), row);
            QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, bubble.topLeft() + QPoint(30, 30));
        };
        clickQuote(history);
        QCOMPARE(backend->contextDialog, QStringLiteral("c-1"));
        QCOMPARE(backend->contextMessage, QStringLiteral("999"));

        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        sections->setCurrentRow(1);
        window.openTaskCard(QStringLiteral("501"));
        auto *task = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(task);
        auto *taskHistory = findAccessible<QListWidget>(task, QStringLiteral("Чат карточки"));
        auto *taskInput = findAccessible<QPlainTextEdit>(task, QStringLiteral("Текст в чате карточки"));
        auto *taskStrip = findAccessible<QLabel>(task, QStringLiteral("Цитата в черновике"));
        QVERIFY(taskHistory && taskInput && taskStrip);
        QTRY_COMPARE(taskHistory->count(), 2);
        chooseReply(taskHistory, 1);
        QVERIFY(taskStrip->isVisible());
        taskInput->setPlainText(QStringLiteral("Ответ в задаче"));
        QTest::keyClick(taskInput, Qt::Key_Return);
        QCOMPARE(backend->sentReplyScope, QStringLiteral("task:501"));
        QCOMPARE(backend->sentReplyId, QStringLiteral("872"));
        QCOMPARE(backend->sentReplyText, QStringLiteral("Ответ в задаче"));
        emit backend->failed(QStringLiteral("send:task:501"), QStringLiteral("Нет сети"));
        auto *cancel = findAccessible<QPushButton>(task, QStringLiteral("Отменить ответ"));
        QVERIFY(cancel && cancel->isVisible());
        QTest::keyClick(cancel, Qt::Key_Space);
        QVERIFY(!taskStrip->isVisible());
        QCOMPARE(taskInput->toPlainText(), QStringLiteral("Ответ в задаче"));
        clickQuote(taskHistory);
        QCOMPARE(backend->contextDialog, QStringLiteral("chat51"));
        QCOMPARE(backend->contextMessage, QStringLiteral("999"));
    }
    void namesVisibleTypersWithoutRestartingTheExpiry()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto &personal = backend->chats.first();
        personal.id = QStringLiteral("22");
        personal.kind = QStringLiteral("im");
        personal.title = QStringLiteral("Вера Смирнова");
        const auto taskDialog = QStringLiteral("chat51");
        backend->stages.first().tasks.first().chatDialogId = taskDialog;
        MainWindow window(backend.get());
        window.show();
        QApplication::setActiveWindow(&window);
        auto *typing = findAccessible<QLabel>(&window, QStringLiteral("Индикатор набора сообщения"));
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
        QVERIFY(typing && chats);
        QTRY_VERIFY(chats->currentItem() && chats->currentItem()->data(workspace::Id).toString() == personal.id);
        emit backend->chatTypingChanged(QStringLiteral("chat999"), QStringLiteral("99"), 1000);
        emit backend->chatTypingChanged(personal.id, QStringLiteral("99"), 0);
        QVERIFY(backend->personRequests.isEmpty());

        emit backend->chatTypingChanged(personal.id, personal.id, 1000);
        QVERIFY(typing->isVisible());
        QCOMPARE(typing->text(), QStringLiteral("Вера Смирнова печатает…"));
        QCOMPARE(typing->textFormat(), Qt::PlainText);
        QVERIFY(backend->personRequests.isEmpty());

        const auto another = QStringLiteral("23");
        emit backend->chatTypingChanged(personal.id, another, 1000);
        emit backend->chatTypingChanged(personal.id, another, 1000);
        QCOMPARE(typing->text(), QStringLiteral("Печатает…"));
        QCOMPARE(backend->personRequests, QStringList{another});
        QTest::qWait(750);
        emit backend->personLoaded(Person{another, QStringLiteral("Павел <дежурный>"), {}, {}});
        QCOMPARE(typing->text(), QStringLiteral("Павел <дежурный> печатает…"));
        QTest::qWait(400);
        QVERIFY(!typing->isVisible());
        emit backend->chatTypingChanged(personal.id, another, 1000);
        QCOMPARE(typing->text(), QStringLiteral("Павел <дежурный> печатает…"));
        QCOMPARE(backend->personRequests.size(), 1);

        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card && card->isVisible());
        auto *taskTyping = findAccessible<QLabel>(card, QStringLiteral("Индикатор набора сообщения"));
        QVERIFY(taskTyping);
        emit backend->chatTypingChanged(taskDialog, another, 1000);
        QCOMPARE(taskTyping->text(), QStringLiteral("Павел <дежурный> печатает…"));
        const auto next = QStringLiteral("24");
        emit backend->chatTypingChanged(taskDialog, next, 1000);
        QCOMPARE(backend->personRequests.last(), next);
        emit backend->chatTypingChanged(taskDialog, next, 0);
        emit backend->personLoaded(Person{next, QStringLiteral("Поздний ответ"), {}, {}});
        QVERIFY(!taskTyping->isVisible());
    }

    void chatHistoryTypingUsesComposerAndPreservesNavigation_data()
    {
        QTest::addColumn<bool>("readOnly");
        QTest::newRow("writable") << false;
        QTest::newRow("read-only") << true;
    }

    void chatHistoryTypingUsesComposerAndPreservesNavigation()
    {
        QFETCH(bool, readOnly);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->messages.first().text = QStringLiteral("Alpha");
        Message second;
        second.id = QStringLiteral("2");
        second.text = QStringLiteral("Beta");
        backend->messages.append(second);
        backend->taskMessages = backend->messages;
        Chat taskChat;
        taskChat.id = QStringLiteral("task-dialog");
        taskChat.taskChat = true;
        taskChat.title = QStringLiteral("Чат задачи");
        taskChat.taskId = QStringLiteral("501");
        backend->taskChats = {taskChat};
        MainWindow window(backend.get(), readOnly);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *input = findAccessible<QPlainTextEdit>(&window, QStringLiteral("Текст сообщения"));
        auto *search = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск по сообщениям"));
        QVERIFY(chats && history && input && search);
        QTRY_COMPARE(history->count(), 2);
        QTest::mouseClick(chats->viewport(), Qt::LeftButton, Qt::NoModifier, chats->visualItemRect(chats->item(0)).center());
        QTRY_COMPARE(input->hasFocus(), !readOnly);

        history->setFocus();
        history->setCurrentRow(0);
        QTest::keyClick(history, Qt::Key_Down);
        QCOMPARE(history->currentRow(), 1);
        history->setCurrentRow(0);
        QTest::keyClick(history, Qt::Key_B);
        QCOMPARE(history->currentRow(), 0);
        QCOMPARE(input->toPlainText(), readOnly ? QString() : QStringLiteral("b"));
        QTRY_COMPARE(input->hasFocus(), !readOnly);
        if (!readOnly) {
            history->setFocus();
            QInputMethodEvent ime;
            ime.setCommitString(QStringLiteral("текст"));
            QCoreApplication::sendEvent(history, &ime);
            QCOMPARE(input->toPlainText(), QStringLiteral("bтекст"));
        }

        history->setFocus();
        QTest::keyClick(history, Qt::Key_F, Qt::ControlModifier);
        QTRY_VERIFY(search->hasFocus());
        QTest::keyClicks(search, QStringLiteral("Beta"));
        QCOMPARE(search->text(), QStringLiteral("Beta"));
        QCOMPARE(input->toPlainText(), readOnly ? QString() : QStringLiteral("bтекст"));

        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("chatScopeTabs"));
        QVERIFY(tabs);
        tabs->setCurrentIndex(1);
        QTRY_COMPARE(chats->count(), 1);
        chats->setFocus();
        QTest::keyClick(chats, Qt::Key_Return);
        QTRY_COMPARE(input->hasFocus(), !readOnly);
        QVERIFY(input->toPlainText().isEmpty());
        history->setFocus();
        QTest::keyClick(history, Qt::Key_T);
        QCOMPARE(input->toPlainText(), readOnly ? QString() : QStringLiteral("t"));
        tabs->setCurrentIndex(0);
        QCOMPARE(input->toPlainText(), readOnly ? QString() : QStringLiteral("bтекст"));

        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        sections->setCurrentRow(1);
        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        auto *taskInput = findAccessible<QPlainTextEdit>(card, QStringLiteral("Текст в чате карточки"));
        auto *taskHistory = findAccessible<QListWidget>(card, QStringLiteral("Чат карточки"));
        QVERIFY(taskInput && taskHistory);
        QTRY_COMPARE(taskInput->hasFocus(), !readOnly);
        QTRY_COMPARE(taskHistory->count(), 2);
        taskHistory->setFocus();
        QTest::keyClick(taskHistory, Qt::Key_C);
        QCOMPARE(taskInput->toPlainText(), readOnly ? QString() : QStringLiteral("c"));
    }

    void onlyVisibleForegroundHistoryMarksActualMessageIds()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->messages.clear();
        for (int index = 0; index < 32; ++index) {
            Message message;
            message.id = QString::number(1000 + index);
            message.text = QStringLiteral("Сообщение %1").arg(index);
            backend->messages.append(message);
        }
        backend->messages.last().outgoing = true;
        Message withoutId;
        withoutId.text = QStringLiteral("Нет идентификатора портала");
        backend->messages.append(withoutId);
        for (const auto &id : {QStringLiteral("2001"), QStringLiteral("2002")}) {
            Message taskMessage;
            taskMessage.id = id;
            taskMessage.text = QStringLiteral("Сообщение в задаче");
            backend->taskMessages.append(taskMessage);
        }
        MainWindow window(backend.get());
        window.resize(930, 555);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *search = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск по сообщениям"));
        auto *searchToggle = findAccessible<QPushButton>(&window, QStringLiteral("Найти в чате"));
        QVERIFY(history && search && searchToggle);
        QTRY_VERIFY(!backend->reads.isEmpty());
        const auto last = backend->reads.constLast();
        QCOMPARE(last.scope, QStringLiteral("c-1"));
        QVERIFY(!last.task && last.visible.contains(QStringLiteral("1031")));
        QVERIFY(!last.incoming.contains(QStringLiteral("1031")));
        QVERIFY(!last.visible.contains(QStringLiteral("1000")));
        QVERIFY(!last.visible.contains(QStringLiteral("33")));

        QWidget other;
        other.show();
        QApplication::setActiveWindow(&other);
        const auto beforeBackground = backend->reads.size();
        history->scrollToTop();
        QTest::qWait(230);
        QCOMPARE(backend->reads.size(), beforeBackground);
        other.close();
        QApplication::setActiveWindow(&window);
        history->setFocus(Qt::OtherFocusReason);
        QTRY_VERIFY(backend->reads.size() > beforeBackground);
        QVERIFY(backend->reads.constLast().incoming.contains(QStringLiteral("1000")));
        QVERIFY(!backend->reads.constLast().visible.contains(QStringLiteral("1031")));

        const auto beforeSearch = backend->reads.size();
        searchToggle->click();
        const auto query = QStringLiteral("Проверка");
        search->setText(query);
        QTest::keyClick(search, Qt::Key_Return);
        Message found;
        found.id = QStringLiteral("9001");
        found.text = query;
        emit backend->messagesFound(QStringLiteral("c-1"), query, false, {found});
        QTRY_COMPARE(history->count(), 1);
        QTest::qWait(230);
        QCOMPARE(backend->reads.size(), beforeSearch);

        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        sections->setCurrentRow(1);
        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        QApplication::setActiveWindow(card);
        QTRY_VERIFY(backend->reads.size() > beforeSearch);
        const auto task = backend->reads.constLast();
        QVERIFY(task.task);
        QCOMPARE(task.scope, QStringLiteral("501"));
        QCOMPARE(task.incoming, QStringList({QStringLiteral("2001"), QStringLiteral("2002")}));
    }

    void visibleHistoryRetriesReadOnReconnectAndReselect()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        Message message;
        message.id = QStringLiteral("701");
        message.text = QStringLiteral("Новое сообщение");
        message.unread = true;
        backend->messages = {message};
        MainWindow window(backend.get());
        window.resize(930, 555);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
        QVERIFY(history && chats);
        QTRY_COMPARE(history->count(), 1);
        QTest::qWait(220);
        QVERIFY(backend->reads.isEmpty());

        emit backend->connectionChanged(true);
        QTRY_VERIFY(!backend->reads.isEmpty());
        QCOMPARE(backend->reads.last().scope, QStringLiteral("c-1"));
        QCOMPARE(backend->reads.last().incoming, QStringList{message.id});
        const auto before = backend->reads.size();
        QVERIFY(chats->currentItem());
        QTest::mouseClick(chats->viewport(), Qt::LeftButton, Qt::NoModifier,
                          chats->visualItemRect(chats->currentItem()).center());
        QTRY_VERIFY(backend->reads.size() > before);
        QCOMPARE(backend->reads.last().incoming, QStringList{message.id});
    }

    void liveUnreadDividerAdvancesOnlyOnMatchingConfirmedRead()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        for (int id = 701; id <= 719; ++id) {
            Message incoming;
            incoming.id = QString::number(id);
            incoming.text = QStringLiteral("Непрочитанное");
            incoming.unread = true;
            backend->messages.append(incoming);
        }
        backend->stages.first().tasks.first().chatDialogId = QStringLiteral("chat67");
        backend->taskMessages = {backend->messages.at(1), backend->messages.at(2)};
        MainWindow window(backend.get());
        window.resize(930, 555);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        const auto divider = [](QListWidget *list) {
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->data(workspace::UnreadDivider).toBool())
                    return list->item(i)->data(workspace::ReceiptMessageId).toString();
            return QString();
        };
        QTRY_COMPARE(divider(history), QStringLiteral("701"));
        emit backend->chatReadConfirmed(QStringLiteral("chat999"), {QStringLiteral("701")}, 0);
        QCOMPARE(divider(history), QStringLiteral("701"));
        emit backend->chatReadConfirmed(QStringLiteral("c-1"), {QStringLiteral("701"), QStringLiteral("703")}, 0);
        QCOMPARE(divider(history), QStringLiteral("702"));
        QVERIFY(!history->item(1)->data(Qt::AccessibleTextRole).toString().startsWith(workspace::UnreadMessages));
        QVERIFY(history->item(2)->data(Qt::AccessibleTextRole).toString().startsWith(workspace::UnreadMessages));
        emit backend->chatReadConfirmed(QStringLiteral("c-1"), {}, 717);
        QCOMPARE(divider(history), QStringLiteral("718"));
        QVERIFY(history->item(history->count() - 1)->data(workspace::MessageUnread).toBool());
        emit backend->chatReadConfirmed(QStringLiteral("c-1"), {}, 719);
        QVERIFY(divider(history).isEmpty());

        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        auto *taskHistory = findAccessible<QListWidget>(card, QStringLiteral("Чат карточки"));
        QVERIFY(taskHistory);
        QCOMPARE(divider(taskHistory), QStringLiteral("701"));
        emit backend->chatReadConfirmed(QStringLiteral("c-1"), {}, 702);
        QCOMPARE(divider(taskHistory), QStringLiteral("701"));
        emit backend->chatReadConfirmed(QStringLiteral("chat67"), {QStringLiteral("701")}, 0);
        QCOMPARE(divider(taskHistory), QStringLiteral("702"));
    }

    void lateReactionAnchorReadsOnlyItsVisibleActiveHistory()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->messages.clear();
        for (int id = 701; id <= 728; ++id) {
            Message sent;
            sent.id = QString::number(id);
            sent.text = QStringLiteral("Исходящее сообщение");
            sent.outgoing = true;
            backend->messages.append(sent);
        }
        backend->stages.first().tasks.first().chatDialogId = QStringLiteral("chat67");
        Message taskMessage = backend->messages.last();
        taskMessage.id = QStringLiteral("801");
        backend->taskMessages = {taskMessage};
        MainWindow window(backend.get());
        window.resize(930, 555);
        window.show();
        QApplication::setActiveWindow(&window);
        QTRY_VERIFY(!backend->reads.isEmpty());
        QVERIFY(backend->reads.last().visible.contains(QStringLiteral("728")));
        QVERIFY(backend->reads.last().incoming.isEmpty());

        const auto regularBefore = backend->reads.size();
        backend->chats.first().hasReaction = true;
        emit backend->recentChatsLoaded(backend->chats);
        emit backend->chatBadgeAnchorAvailable(QStringLiteral("chat999"));
        QTest::qWait(210);
        QCOMPARE(backend->reads.size(), regularBefore);

        emit backend->chatBadgeAnchorAvailable(QStringLiteral("c-1"));
        QTRY_VERIFY_WITH_TIMEOUT(backend->reads.size() > regularBefore, 1800);
        QCOMPARE(backend->reads.last().scope, QStringLiteral("c-1"));
        QVERIFY(backend->reads.last().visible.contains(QStringLiteral("728")));
        QVERIFY(!backend->reads.last().visible.contains(QStringLiteral("701")));
        QVERIFY(backend->reads.last().incoming.isEmpty());

        const auto afterRegular = backend->reads.size();
        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        QApplication::setActiveWindow(card);
        QTRY_VERIFY(backend->reads.size() > afterRegular);
        QVERIFY(backend->reads.last().task);
        const auto taskBefore = backend->reads.size();
        emit backend->chatBadgeAnchorAvailable(QStringLiteral("c-1"));
        QTest::qWait(210);
        QCOMPARE(backend->reads.size(), taskBefore);
        emit backend->chatBadgeAnchorAvailable(QStringLiteral("chat67"));
        QTRY_VERIFY_WITH_TIMEOUT(backend->reads.size() > taskBefore, 1800);
        QVERIFY(backend->reads.last().task);
        QCOMPARE(backend->reads.last().scope, QStringLiteral("501"));
        QCOMPARE(backend->reads.last().visible, QStringList{taskMessage.id});
    }

    void contactPresenceAndPinnedHeaderFollowOpenedDialogs()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto &contact = backend->chats.first();
        contact.kind = QStringLiteral("im");
        contact.position = QStringLiteral("Инженер");
        contact.onlineUntil = QDateTime::currentDateTimeUtc().addSecs(30);
        Chat taskChat;
        taskChat.id = QStringLiteral("task-dialog");
        taskChat.title = QStringLiteral("Чат задачи");
        taskChat.taskChat = true;
        taskChat.taskId = QStringLiteral("501");
        backend->taskChats = {taskChat};
        backend->stages.first().tasks.first().chatDialogId = taskChat.id;
        const auto message = [](QString id, QString text) {
            Message result;
            result.id = id;
            result.text = text;
            return result;
        };
        const auto first = message(QStringLiteral("41"), QStringLiteral("Первое закрепление"));
        const auto second = message(QStringLiteral("42"), QStringLiteral("Второе закрепление"));
        const auto taskPin = message(QStringLiteral("52"), QStringLiteral("Закрепление задачи"));
        backend->pinnedMessages.insert(contact.id, {first, second});
        backend->pinnedMessages.insert(taskChat.id, {taskPin});
        MainWindow window(backend.get());
        window.resize(940, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *header = findAccessible<QLabel>(&window, QStringLiteral("Статус и должность контакта"));
        auto *bar = findAccessible<QPushButton>(&window, QStringLiteral("Закреплённые сообщения"));
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
        QVERIFY(header && bar && chats);
        QTRY_VERIFY(header->isVisible() && bar->isVisible());
        QCOMPARE(header->text(), QStringLiteral("В сети · Инженер"));
        QCOMPARE(backend->pinnedRequests, QStringList{contact.id});
        const auto row = chats->visualItemRect(chats->item(0));
        const auto hasOnlineDot = [chats, row] {
            const auto shot = chats->viewport()->grab().toImage();
            const auto area = row.intersected(shot.rect());
            for (int y = area.top(); y <= area.bottom(); ++y)
                for (int x = area.left(); x <= area.left() + 54 && x <= area.right(); ++x)
                    if (shot.pixelColor(x, y) == QColor("#24875b"))
                        return true;
            return false;
        };
        QVERIFY(hasOnlineDot());
        const auto capture = qEnvironmentVariable("B24_CHAT_HEADER_SCREENSHOT");
        if (!capture.isEmpty())
            QVERIFY(window.grab().save(capture));

        emit backend->chatContactChanged(contact.id, QDateTime::currentDateTimeUtc().addMSecs(180), contact.position);
        QTRY_COMPARE(header->text(), QStringLiteral("Не в сети · Инженер"));
        QVERIFY(!hasOnlineDot());
        emit backend->chatContactChanged(contact.id, {}, contact.position);
        QCOMPARE(header->text(), contact.position);
        emit backend->pinnedMessagesLoaded(QStringLiteral("other"), {});
        QVERIFY(bar->isVisible());

        bar->setFocus();
        QTest::keyClick(bar, Qt::Key_Space);
        QCOMPARE(backend->contextDialog, contact.id);
        QCOMPARE(backend->contextMessage, first.id);
        QVERIFY(!QApplication::activePopupWidget());
        auto *choosePin = findAccessible<QPushButton>(&window, QStringLiteral("Выбрать закреплённое сообщение"));
        QVERIFY(choosePin && choosePin->isVisible());
        choosePin->setFocus();
        QTest::keyClick(choosePin, Qt::Key_Space);
        QTRY_VERIFY(QApplication::activePopupWidget());
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        QVERIFY(menu && menu->actions().size() == 2);
        menu->actions().at(1)->trigger();
        menu->close();
        QCOMPARE(backend->contextDialog, contact.id);
        QCOMPARE(backend->contextMessage, second.id);

        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("chatScopeTabs"));
        QVERIFY(tabs);
        tabs->setCurrentIndex(1);
        QVERIFY(!header->isVisible());
        QVERIFY(bar->isVisible());
        bar->click();
        QCOMPARE(backend->contextDialog, taskChat.id);
        QCOMPARE(backend->contextMessage, taskPin.id);

        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        sections->setCurrentRow(1);
        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        auto *taskBar = findAccessible<QPushButton>(card, QStringLiteral("Закреплённые сообщения"));
        QVERIFY(taskBar && taskBar->isVisible());
        taskBar->click();
        QCOMPARE(backend->contextDialog, taskChat.id);
        QCOMPARE(backend->contextMessage, taskPin.id);
        emit backend->messageContextLoaded(taskChat.id, taskPin.id, {taskPin});
        auto *messages = findAccessible<QListWidget>(card, QStringLiteral("Чат карточки"));
        QVERIFY(messages && messages->currentItem());
        QCOMPARE(messages->currentItem()->data(workspace::Id).toString(), taskPin.id);
        auto *latest = findAccessible<QPushButton>(card, QStringLiteral("К последним сообщениям задачи"));
        QVERIFY(latest && latest->isVisible());
        latest->click();
        QVERIFY(!latest->isVisible());
    }

    void pinnedJumpsKeepLoadedHistoryAndShowStickyDates()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        QVector<Message> history;
        const QDate earlier(2026, 2, 10);
        const QDate later(2026, 2, 11);
        for (int row = 0; row < 38; ++row) {
            Message message;
            message.id = QString::number(701 + row);
            message.text = QStringLiteral("Сообщение %1").arg(row);
            message.createdAt = QDateTime(row < 20 ? earlier : later, QTime(12, row));
            history.append(message);
        }
        const auto localPin = history.at(6);
        backend->messages = history;
        backend->taskMessages = history;
        backend->stages.first().tasks.first().chatDialogId = QStringLiteral("chat67");
        backend->pinnedMessages.insert(QStringLiteral("c-1"), {localPin, history.first()});
        backend->pinnedMessages.insert(QStringLiteral("chat67"), {localPin});
        MainWindow window(backend.get());
        window.resize(940, 570);
        window.show();
        QApplication::setActiveWindow(&window);

        const auto verify = [&](QWidget *surface, const QString &historyName) {
            auto *messages = findAccessible<QListWidget>(surface, historyName);
            auto *bar = findAccessible<QPushButton>(surface, QStringLiteral("Закреплённые сообщения"));
            QVERIFY(messages && bar && bar->isVisible());
            QTRY_VERIFY(!backend->reads.isEmpty());
            QTest::qWait(180);
            const auto reads = backend->reads.size();
            bar->click();
            QVERIFY(!QApplication::activePopupWidget());
            QVERIFY(backend->contextMessage.isEmpty());
            QTRY_VERIFY(messages->currentItem());
            QCOMPARE(messages->currentItem()->data(workspace::Id).toString(), localPin.id);
            QCOMPARE(messages->count(), history.size());
            const auto focused = messages->visualItemRect(messages->currentItem());
            QVERIFY(qAbs(focused.center().y() - messages->viewport()->rect().center().y()) <= 40);
            QTest::qWait(180);
            QCOMPARE(backend->reads.size(), reads);

            auto *day = messages->findChild<QLabel *>(QStringLiteral("chatStickyDay"));
            QVERIFY(day);
            messages->scrollToItem(messages->item(7), QAbstractItemView::PositionAtTop);
            QTRY_VERIFY(day->isVisible());
            QCOMPARE(day->text(), QStringLiteral("10.02.2026"));
            messages->setCurrentRow(messages->count() - 1);
            QTest::keyClick(messages, Qt::Key_End);
            QTRY_COMPARE(day->text(), QStringLiteral("11.02.2026"));
            messages->verticalScrollBar()->setValue(messages->verticalScrollBar()->minimum());
            QTRY_VERIFY(!day->isVisible());
        };
        verify(&window, QStringLiteral("Сообщения чата"));

        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        QApplication::setActiveWindow(card);
        verify(card, QStringLiteral("Чат карточки"));
    }

    void showsProjectToolsAndDistinguishesFolderDenial()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->projectToolsFixture = true;
        MainWindow window(backend.get());
        window.resize(1020, 690);
        window.show();
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        sections->setCurrentRow(1);
        auto *open = findAccessible<QPushButton>(&window, QStringLiteral("Участники и файлы проекта"));
        QVERIFY(open && open->isEnabled());
        open->click();
        auto *tools = findAccessible<QDialog>(&window, QStringLiteral("Инструменты проекта"));
        QVERIFY(tools);
        auto *members = findAccessible<QListWidget>(tools, QStringLiteral("Список участников проекта"));
        QVERIFY(members);
        QCOMPARE(members->count(), 1);
        QCOMPARE(members->item(0)->data(workspace::Detail).toString(), QStringLiteral("Владелец"));
        emit backend->projectMembersLoaded(QStringLiteral("202"), {});
        QCOMPARE(members->item(0)->data(workspace::Title).toString(), QStringLiteral("Анна"));

        auto *tabs = tools->findChild<QTabWidget *>();
        QVERIFY(tabs);
        tabs->setCurrentIndex(1);
        auto *files = findAccessible<QListWidget>(tools, QStringLiteral("Список файлов проекта"));
        QVERIFY(files);
        QCOMPARE(files->item(0)->data(workspace::Title).toString(), QStringLiteral("Общие материалы"));
        files->setCurrentRow(0);
        emit files->itemActivated(files->item(0));
        QCOMPARE(files->item(0)->data(workspace::Detail).toString(), QStringLiteral("Файлы недоступны"));
        auto *status = findAccessible<QLabel>(tools, QStringLiteral("Состояние инструментов проекта"));
        QVERIFY(status);
        QCOMPARE(status->text(), QStringLiteral("Доступ запрещён"));
    }

    void taskCardShowsNumberAndCopiesNumberAndLink()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);

        window.openTaskCard(QStringLiteral("501"));
        auto *card = dynamic_cast<ui::TaskCardDialog *>(
            window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        QVERIFY(card);
        QApplication::setActiveWindow(card);
        auto *number = findAccessible<QLabel>(card, QStringLiteral("Номер задачи"));
        QVERIFY(number);
        QCOMPARE(number->text(), QStringLiteral("№501"));

        auto *copyNumber = findAccessible<QPushButton>(card, QStringLiteral("Скопировать номер"));
        auto *copyLink = findAccessible<QPushButton>(card, QStringLiteral("Скопировать ссылку"));
        QVERIFY(copyNumber && copyLink);
        QVERIFY(copyNumber->text().isEmpty() && copyLink->text().isEmpty());
        QCOMPARE(copyNumber->toolTip(), copyNumber->accessibleName());
        QCOMPARE(copyLink->toolTip(), copyLink->accessibleName());
        QVERIFY(number->geometry().right() < copyNumber->geometry().left());
        QVERIFY(copyNumber->geometry().right() < copyLink->geometry().left());
        QVERIFY(copyLink->geometry().left() - number->geometry().right() < 48);
        auto *feedback = card->findChild<QLabel *>(QStringLiteral("taskCopyFeedback"));
        QVERIFY(feedback && feedback->text().isEmpty());
        const auto linkGeometry = copyLink->geometry();

        copyNumber->setFocus();
        QVERIFY(copyNumber->hasFocus());
        QTest::keyClick(copyNumber, Qt::Key_Space);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("501"));
        QCOMPARE(feedback->text(), QStringLiteral("Номер скопирован"));
        QVERIFY(copyNumber->hasFocus());
        QCoreApplication::sendPostedEvents(card, QEvent::LayoutRequest);
        QCOMPARE(copyLink->geometry(), linkGeometry);

        QTest::mouseClick(copyLink, Qt::LeftButton);
        QCOMPARE(QGuiApplication::clipboard()->text(),
                 QStringLiteral("https://example.bitrix24.ru/workgroups/group/101/tasks/task/view/501/"));
        QCOMPARE(feedback->text(), QStringLiteral("Ссылка скопирована"));
        QCoreApplication::sendPostedEvents(card, QEvent::LayoutRequest);
        QCOMPARE(copyLink->geometry(), linkGeometry);
        QVERIFY(QTest::qWaitFor([feedback] { return feedback->text().isEmpty(); }, 3000));

        const auto clipboard = QGuiApplication::clipboard()->text();
        card->setLink(QString());
        QVERIFY(!copyLink->isEnabled());
        QTest::mouseClick(copyLink, Qt::LeftButton);
        QCOMPARE(QGuiApplication::clipboard()->text(), clipboard);
        QVERIFY(feedback->text().isEmpty());

        QTest::keyClick(card, Qt::Key_Escape);
        QVERIFY(!card->isVisible());
    }

    void taskCardActionsRequireRightsAndWaitForConfirmation()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdTaskUpdate = true;
        auto &task = backend->stages.first().tasks.first();
        task.projectId = QStringLiteral("101");
        task.statusActions = {TaskStatusAction::Start, TaskStatusAction::Complete, TaskStatusAction::Defer};
        task.editRights.assignee = true;
        task.editRights.remove = true;
        task.editRights.rate = true;
        task.rating = TaskRating::Positive;
        task.assigneeId = QStringLiteral("11");
        task.assignee = QStringLiteral("Анна");
        backend->people = {{QStringLiteral("14"), QStringLiteral("Вера"), {}, {}}};
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        auto *projects = findAccessible<QListWidget>(&window, QStringLiteral("Проекты"));
        QVERIFY(sections && projects);
        sections->setCurrentRow(1);
        QTRY_COMPARE(projects->count(), 1);
        projects->setCurrentRow(0);
        QTRY_VERIFY(backend->boardRequests > 0);
        window.openTaskCard(QStringLiteral("501"));
        auto *card = static_cast<ui::TaskCardDialog *>(window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        QVERIFY(card && card->isVisible());
        auto *primary = card->findChild<QPushButton *>(QStringLiteral("taskPrimaryAction"));
        auto *complete = findAccessible<QPushButton>(card, QStringLiteral("Завершить задачу"));
        auto *more = card->findChild<QMenu *>(QStringLiteral("taskOverflowMenu"));
        auto *rating = card->findChild<QMenu *>(QStringLiteral("taskRatingMenu"));
        QVERIFY(primary && complete && more && rating);
        const auto action = [](QMenu *menu, const QString &label) {
            for (auto *entry : menu->actions())
                if (entry->text() == label)
                    return entry;
            return static_cast<QAction *>(nullptr);
        };
        auto *delegate = action(more, QStringLiteral("Делегировать"));
        auto *defer = action(more, QStringLiteral("Отложить"));
        auto *remove = action(more, QStringLiteral("Удалить задачу"));
        auto *positive = action(rating, QStringLiteral("Положительная"));
        auto *negative = action(rating, QStringLiteral("Отрицательная"));
        QVERIFY(delegate && defer && remove && positive && negative);
        QTRY_VERIFY(primary->isVisible() && primary->isEnabled());
        QVERIFY(complete->isVisible());
        QVERIFY(!delegate->isVisible());
        QVERIFY(defer->isVisible() && remove->isVisible());
        QVERIFY(positive->isChecked() && !negative->isChecked());

        primary->click();
        QVERIFY(backend->lastUpdate.statusAction == TaskStatusAction::Start);
        QVERIFY(!primary->isEnabled() && !complete->isEnabled());
        task.statusActions = {TaskStatusAction::Pause, TaskStatusAction::Complete, TaskStatusAction::Defer};
        emit backend->taskUpdated(task.id);
        QCOMPARE(primary->text(), QStringLiteral("Приостановить"));
        defer->trigger();
        QVERIFY(backend->lastUpdate.statusAction == TaskStatusAction::Defer);
        emit backend->taskUpdated(task.id);
        negative->trigger();
        QVERIFY(backend->lastUpdate.rating == TaskRating::Negative);
        QVERIFY(positive->isChecked() && !negative->isChecked());
        task.rating = TaskRating::Negative;
        task.editRights.delegate = true;
        emit backend->taskUpdated(task.id);
        QVERIFY(negative->isChecked() && !positive->isChecked());
        QVERIFY(delegate->isVisible());
        delegate->trigger();
        auto *picker = card->findChild<QWidget *>(QStringLiteral("taskRequisitePopup"));
        QVERIFY(picker && picker->isVisible());
        QVERIFY(!complete->isEnabled() && !remove->isEnabled());
        auto *cancel = findAccessible<QPushButton>(picker, QStringLiteral("Отменить изменение реквизита"));
        QVERIFY(cancel);
        cancel->click();
        QTRY_VERIFY(!picker->isVisible());
        const auto selectAssignee = [picker] {
            auto *search = findAccessible<QLineEdit>(picker, QStringLiteral("Поиск сотрудника · Исполнитель"));
            QVERIFY(search && search->isVisible());
            const auto shownSuggestions = [picker]() -> QListWidget * {
                for (auto *list : picker->findChildren<QListWidget *>())
                    if (list->isVisible() && list->accessibleName() == QStringLiteral("Найденные сотрудники"))
                        return list;
                return nullptr;
            };
            QTRY_VERIFY(shownSuggestions());
            auto *suggestions = shownSuggestions();
            QTRY_VERIFY(!suggestions->findItems(QStringLiteral("Вера"), Qt::MatchExactly).isEmpty());
            suggestions->setCurrentItem(suggestions->findItems(QStringLiteral("Вера"), Qt::MatchExactly).first());
            QTest::keyClick(search, Qt::Key_Return);
        };
        auto *assignee = findAccessible<QPushButton>(card, QStringLiteral("Исполнитель"));
        QVERIFY(assignee && assignee->isEnabled());
        assignee->click();
        selectAssignee();
        QCOMPARE(backend->lastUpdate.assigneeId.value_or(QString()), QStringLiteral("14"));
        QVERIFY(!backend->lastUpdate.requireDelegate);
        emit backend->taskUpdated(task.id);
        delegate->trigger();
        selectAssignee();
        QCOMPARE(backend->lastUpdate.assigneeId.value_or(QString()), QStringLiteral("14"));
        QVERIFY(backend->lastUpdate.requireDelegate);
        emit backend->taskUpdated(task.id);

        card->messageInput()->setPlainText(QStringLiteral("Черновик"));
        QVERIFY(!remove->isEnabled());
        card->messageInput()->clear();
        QVERIFY(remove->isEnabled());
        remove->trigger();
        auto *confirm = card->findChild<QMessageBox *>(QStringLiteral("taskDeleteConfirmation"));
        QVERIFY(confirm && confirm->isVisible());
        QVERIFY(confirm->defaultButton() == confirm->button(QMessageBox::Cancel));
        confirm->button(QMessageBox::Cancel)->click();
        QVERIFY(backend->deletedTasks.isEmpty());
        QTRY_VERIFY(!card->findChild<QMessageBox *>(QStringLiteral("taskDeleteConfirmation")));
        remove->trigger();
        confirm = card->findChild<QMessageBox *>(QStringLiteral("taskDeleteConfirmation"));
        QVERIFY(confirm);
        confirm->button(QMessageBox::Yes)->click();
        QCOMPARE(backend->deletedTasks, QStringList{task.id});
        QVERIFY(card->isVisible() && !remove->isEnabled());
        emit backend->taskDeleted(QStringLiteral("502"));
        QVERIFY(card->isVisible());
        emit backend->failed(QStringLiteral("delete-task:") + task.id, QStringLiteral("Портал отклонил удаление"));
        QVERIFY(card->isVisible() && remove->isEnabled());
        QTRY_VERIFY(!card->findChild<QMessageBox *>(QStringLiteral("taskDeleteConfirmation")));
        remove->trigger();
        confirm = card->findChild<QMessageBox *>(QStringLiteral("taskDeleteConfirmation"));
        QVERIFY(confirm);
        confirm->button(QMessageBox::Yes)->click();
        QCOMPARE(backend->deletedTasks, (QStringList{task.id, task.id}));
        const auto boards = backend->boardRequests;
        emit backend->taskDeleted(task.id);
        QTRY_VERIFY(!window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        QCOMPARE(backend->boardRequests, boards + 1);
    }

    void taskCardSavesEditAndSendsMessage()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        backend->holdTaskUpdate = true;
        backend->stages.first().tasks.first().priority = QStringLiteral("2");
        window.openTaskCard(QStringLiteral("501"));

        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        auto *edit = findAccessible<QPushButton>(card, QStringLiteral("Изменить задачу"));
        QVERIFY(edit && edit->isEnabled());
        edit->click();
        auto *title = findAccessible<QLineEdit>(card, QStringLiteral("Заголовок задачи"));
        QVERIFY(title);
        auto *priority = findAccessible<QCheckBox>(card, QStringLiteral("Высокий приоритет"));
        QVERIFY(priority);
        QVERIFY(priority->isChecked());
        priority->setChecked(false);
        title->setText(QStringLiteral("Починить кран и трубу"));
        auto *save = findAccessible<QPushButton>(card, QStringLiteral("Сохранить задачу"));
        QVERIFY(save);
        auto late = backend->stages.first().tasks.first();
        late.attachments = {QStringLiteral("название.txt")};
        emit backend->taskLoaded(late);
        QCOMPARE(title->text(), QStringLiteral("Починить кран и трубу"));
        save->click();
        QVERIFY(!save->isEnabled());
        QCOMPARE(backend->updatedTask, QStringLiteral("501"));
        QVERIFY(backend->lastUpdate.title.has_value());
        QCOMPARE(*backend->lastUpdate.title, QStringLiteral("Починить кран и трубу"));
        QCOMPARE(backend->lastUpdate.highPriority, std::optional<bool>(false));

        emit backend->failed(QStringLiteral("update-task:501"), QStringLiteral("Изменение отклонено"));
        QVERIFY(save->isEnabled());
        QCOMPARE(title->text(), QStringLiteral("Починить кран и трубу"));
        auto *input = findAccessible<QPlainTextEdit>(card, QStringLiteral("Текст в чате карточки"));
        QVERIFY(input);
        input->setPlainText(QStringLiteral("Сообщение в карточке"));
        auto *send = findAccessible<QPushButton>(card, QStringLiteral("Отправить в карточке"));
        QVERIFY(send);
        send->click();
        QCOMPARE(backend->sentTask, QStringLiteral("Сообщение в карточке"));
        input->setPlainText(QStringLiteral("Незаконченный ответ"));
        static_cast<ui::TaskCardDialog *>(card)->setDiscardConfirmation([] { return true; });
        QPointer<QDialog> beforeClose(card);
        QTest::keyClick(card, Qt::Key_Escape);
        QTRY_VERIFY(beforeClose.isNull());
        window.openTaskCard(QStringLiteral("501"));
        card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        input = findAccessible<QPlainTextEdit>(card, QStringLiteral("Текст в чате карточки"));
        QCOMPARE(input->toPlainText(), QStringLiteral("Незаконченный ответ"));
    }

    void taskProjectPickerUsesProjectBrandingForTheRowChoiceAndPill()
    {
        Task task;
        task.id = QStringLiteral("501");
        task.name = QStringLiteral("Монтаж");
        task.projectId = QStringLiteral("101");
        task.project = QStringLiteral("WEB разработка");
        task.editRights.project = true;

        QWidget parent;
        ui::TaskCardDialog card(task, task.projectId, &parent);
        card.setCanDetachProject(true);
        card.setWriteState(true, false, false, false, false);
        QPixmap photo(32, 32);
        const QColor logoColor(QStringLiteral("#e13b52"));
        photo.fill(logoColor);
        bool logoLoaded = false;
        Project project{task.projectId, task.project, {}};
        project.avatarType = QStringLiteral("folder");
        project.avatarColor = QStringLiteral("#2059ad");
        project.avatarUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/upload/web-project.png"));
        card.setRequisiteAvatars([&](const QUrl &url) -> const QPixmap * {
            return logoLoaded && url == project.avatarUrl ? &photo : nullptr;
        });
        card.show();
        QApplication::setActiveWindow(&card);

        auto *projectRow = findAccessible<QPushButton>(&card, QStringLiteral("Проект"));
        QVERIFY(projectRow && projectRow->isEnabled());
        projectRow->click();
        auto *choices = findAccessible<QListWidget>(&card, QStringLiteral("Проект задачи"));
        auto *search = findAccessible<QLineEdit>(&card, QStringLiteral("Поиск проекта"));
        QVERIFY(choices && choices->isVisible() && search && search->isVisible());
        const auto chipIcon = [search]() {
            for (const auto *pill : search->parentWidget()->findChildren<QWidget *>(QStringLiteral("taskInlineSelectionPill"))) {
                if (!pill->isVisible())
                    continue;
                for (const auto *label : pill->findChildren<QLabel *>()) {
                    if (!label->pixmap().isNull())
                        return label->pixmap().toImage();
                }
            }
            return QImage{};
        };
        const auto rowIcon = [projectRow]() {
            auto *avatar = projectRow->findChild<QWidget *>(QStringLiteral("taskRequisiteAvatar"));
            return avatar && avatar->isVisible() ? avatar->grab().toImage() : QImage{};
        };
        const auto suggestionIcon = [choices] {
            return choices->currentItem() ? choices->currentItem()->icon().pixmap(22, 22).toImage() : QImage{};
        };
        const auto fallback = suggestionIcon();
        const auto fallbackChip = chipIcon();
        const auto fallbackRow = rowIcon();
        card.setProjectChoices({project});
        QCOMPARE(choices->currentItem()->data(Qt::UserRole).toString(), project.id);
        QPixmap expected(22, 22);
        expected.fill(Qt::transparent);
        {
            QPainter painter(&expected);
            painter.setRenderHint(QPainter::Antialiasing);
            workspace::paintAvatar(painter, QRect(0, 0, 21, 21), project.name, workspace::AvatarKind::Project,
                                   {}, QColor(project.avatarColor), project.avatarVariant, project.avatarType);
        }
        QCOMPARE(suggestionIcon(), expected.toImage());
        QVERIFY(chipIcon() != fallbackChip);
        QTRY_VERIFY(rowIcon() != fallbackRow);
        QVERIFY(card.visibleRequisiteAvatarUrls().contains(project.avatarUrl));
        QVERIFY(fallback.isNull() && fallbackChip.isNull() && fallbackRow.isNull());

        project.avatarType.clear();
        project.avatarVariant = QStringLiteral("guest");
        card.setProjectChoices({project});
        QVERIFY(suggestionIcon().isNull() && chipIcon().isNull() && rowIcon().isNull());
        QVERIFY(card.visibleRequisiteAvatarUrls().contains(project.avatarUrl));
        const int searchOffsetWithoutLogo = search->x();
        logoLoaded = true;
        card.refreshRequisiteAvatars();
        QCOMPARE(suggestionIcon().pixelColor(10, 10), logoColor);
        QCOMPARE(chipIcon().pixelColor(10, 10), logoColor);
        QTRY_VERIFY(!rowIcon().isNull());
        QCOMPARE(rowIcon().pixelColor(10, 10), logoColor);
        QVERIFY(search->x() > searchOffsetWithoutLogo);
    }

    void taskCardSavesAnExplicitPickerChoiceAndCanRetry_data()
    {
        QTest::addColumn<QString>("field");
        QTest::newRow("project") << QStringLiteral("Проект");
        QTest::newRow("stage") << QStringLiteral("Этап");
        QTest::newRow("assignee") << QStringLiteral("Исполнитель");
    }

    void taskCardSavesAnExplicitPickerChoiceAndCanRetry()
    {
        QFETCH(QString, field);
        const bool project = field == QStringLiteral("Проект");
        const bool stageField = field == QStringLiteral("Этап");
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto &task = backend->stages.first().tasks.first();
        task.projectId = QStringLiteral("101");
        task.project = backend->projects.first().name;
        task.editRights.project = true;
        task.editRights.stage = true;
        task.editRights.assignee = true;
        task.assigneeId = QStringLiteral("12");
        task.assignee = QStringLiteral("Анна");
        backend->projects.append({QStringLiteral("102"), QStringLiteral("Склад"), {}});
        backend->people = {{QStringLiteral("12"), QStringLiteral("Анна"), {}, {}},
                           {QStringLiteral("14"), QStringLiteral("Вера"), {}, {}}};
        backend->holdTaskUpdate = true;
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        QApplication::setActiveWindow(card);
        auto *button = findAccessible<QPushButton>(card, field);
        QVERIFY(button && button->isEnabled());
        button->click();

        auto *popup = card->findChild<QFrame *>(QStringLiteral("taskRequisitePopup"));
        QVERIFY(popup && popup->isVisible());
        const auto listName = project ? QStringLiteral("Проект задачи")
            : stageField ? QStringLiteral("Этап задачи") : QStringLiteral("Найденные сотрудники");
        QListWidget *list = nullptr;
        QTRY_VERIFY([&] {
            for (auto *candidate : popup->findChildren<QListWidget *>()) {
                if (candidate->accessibleName() == listName && candidate->isVisible() && candidate->count() >= 2) {
                    list = candidate;
                    return true;
                }
            }
            return false;
        }());
        auto *activation = project || stageField ? static_cast<QWidget *>(list)
            : findAccessible<QLineEdit>(popup, QStringLiteral("Поиск сотрудника · Исполнитель"));
        QVERIFY(activation);
        const auto retryName = stageField ? QStringLiteral("Повторить перенос задачи")
            : QStringLiteral("Повторить изменение реквизита");
        auto *retry = findAccessible<QPushButton>(popup, retryName);
        if (retry)
            QVERIFY(!retry->isVisible());
        QListWidgetItem *confirmed = nullptr;
        for (int row = 0; row < list->count(); ++row)
            if (list->item(row)->checkState() == Qt::Checked)
                confirmed = list->item(row);
        QVERIFY(confirmed);
        list->setCurrentItem(confirmed);
        QTest::keyClick(activation, Qt::Key_Return);
        QCOMPARE(backend->taskUpdateRequests, 0);
        QVERIFY(popup->isVisible());
        QTRY_VERIFY(list->isVisible());
        QTest::keyClick(activation, Qt::Key_Down);
        QCOMPARE(backend->taskUpdateRequests, 0);
        const auto destination = project ? QStringLiteral("Склад")
            : stageField ? backend->stages[1].name : QStringLiteral("Вера");
        const auto candidates = list->findItems(destination, Qt::MatchExactly);
        QCOMPARE(candidates.size(), 1);
        list->setCurrentItem(candidates.first());
        QCOMPARE(backend->taskUpdateRequests, 0);
        if (project) {
            const auto target = list->visualItemRect(candidates.first()).center();
            QVERIFY(list->viewport()->rect().contains(target));
            QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, target);
        } else {
            QTest::keyClick(activation, Qt::Key_Return);
        }
        QCOMPARE(backend->taskUpdateRequests, 1);
        const auto verifyPatch = [&] {
            QCOMPARE(backend->updatedTask, QStringLiteral("501"));
            QCOMPARE(backend->lastUpdate.projectId, project ? std::optional<QString>(QStringLiteral("102")) : std::nullopt);
            QCOMPARE(backend->lastUpdate.stageId, stageField ? std::optional<QString>(QStringLiteral("602")) : std::nullopt);
            QCOMPARE(backend->lastUpdate.assigneeId, !project && !stageField
                         ? std::optional<QString>(QStringLiteral("14")) : std::nullopt);
        };
        verifyPatch();
        QTest::keyClick(activation, Qt::Key_Return);
        QCOMPARE(backend->taskUpdateRequests, 1);

        emit backend->failed(QStringLiteral("update-task:501"), QStringLiteral("Портал отклонил изменение"));
        retry = findAccessible<QPushButton>(popup, retryName);
        QTRY_VERIFY(retry && retry->isVisible() && retry->isEnabled());
        if (project || stageField)
            list->setCurrentItem(confirmed);
        retry->click();
        QCOMPARE(backend->taskUpdateRequests, 2);
        verifyPatch();
        if (project) {
            task.projectId = QStringLiteral("102");
            task.project = destination;
        } else if (stageField) {
            task.stageId = QStringLiteral("602");
            task.stageName = destination;
        } else {
            task.assigneeId = QStringLiteral("14");
            task.assignee = destination;
        }
        emit backend->taskUpdated(task.id);
        QTRY_VERIFY(!popup->isVisible());
    }

    void taskCardAppliesOnePermittedRequisiteAndKeepsDraftOnConflict()
    {
        Task task;
        task.id = QStringLiteral("501");
        task.name = QStringLiteral("Монтаж");
        task.projectId = QStringLiteral("101");
        task.project = QStringLiteral("Стройка");
        task.stageId = QStringLiteral("601");
        task.stageName = QStringLiteral("Работа");
        task.createdBy = QStringLiteral("Постановщик");
        task.creatorId = QStringLiteral("11");
        task.assignee = QStringLiteral("Исполнитель");
        task.assigneeId = QStringLiteral("12");
        task.observers = {QStringLiteral("Анна")};
        task.observerPeople = {{QStringLiteral("13"), QStringLiteral("Анна")}};
        task.due = QStringLiteral("2026-11-23T09:00:00+03:00");
        task.createdAt = QDateTime(QDate(2026, 11, 15), QTime(12, 40));
        task.statusCode = QStringLiteral("3");
        task.status = QStringLiteral("Выполняется");
        task.editRights.title = true;
        task.editRights.assignee = true;
        task.editRights.observers = true;
        task.editRights.deadline = true;
        task.editRights.project = true;
        task.editRights.stage = true;
        task.statusActions = {TaskStatusAction::Pause, TaskStatusAction::Complete};
        task.deadlineReasonRequired = true;

        QWidget parent;
        bx::ui::TaskCardDialog card(task, task.projectId, &parent);
        card.setProjectChoices({{QStringLiteral("102"), QStringLiteral("Склад"), {}}});
        card.setWriteState(true, false, false, false, false);
        QString request;
        QString query;
        card.setPeopleSearch([&](const QString &id, const QString &term) {
            request = id;
            query = term;
        });
        QVector<TaskEdit> edits;
        card.setOnSave([&edits](const TaskEdit &edit) { edits.append(edit); });
        card.show();
        QApplication::setActiveWindow(&card);
        const auto *deadline = findAccessible<QLabel>(&card, QStringLiteral("Крайний срок"));
        const auto *created = findAccessible<QLabel>(&card, QStringLiteral("Дата создания"));
        QVERIFY(deadline && created);
        QCOMPARE(deadline->text(), QDateTime::fromString(task.due, Qt::ISODate)
                                   .toLocalTime().toString(QStringLiteral("dd.MM.yyyy  HH:mm")));
        QCOMPARE(created->text(), QStringLiteral("15.11.2026  12:40"));
        auto *apply = findAccessible<QPushButton>(&card, QStringLiteral("Применить изменение реквизита"));
        auto *cancel = findAccessible<QPushButton>(&card, QStringLiteral("Отменить изменение реквизита"));
        QVERIFY(apply && cancel);
        QVERIFY(!findAccessible<QPushButton>(&card, QStringLiteral("Изменить реквизиты задачи")));
        const auto row = [&card](const QString &label) {
            return findAccessible<QPushButton>(&card, label);
        };
        QVERIFY(row(QStringLiteral("Постановщик")) && !row(QStringLiteral("Постановщик"))->isEnabled());
        QVERIFY(!row(QStringLiteral("Дата создания")));
        auto *observers = row(QStringLiteral("Наблюдатели"));
        QVERIFY(observers && observers->isEnabled());
        QVERIFY(findAccessible<QLabel>(observers, QStringLiteral("Анна")));
        observers->setFocus();
        QTest::keyClick(observers, Qt::Key_Space);
        auto *search = findAccessible<QLineEdit>(&card, QStringLiteral("Поиск сотрудника · Наблюдатели"));
        QVERIFY(search && search->isVisible());
        auto *popup = card.findChild<QFrame *>(QStringLiteral("taskRequisitePopup"));
        QVERIFY(popup && popup->isVisible() && popup->isAncestorOf(search));
        QVERIFY(card.rect().contains(popup->geometry()));
        QTest::keyClicks(search, "bor");
        QTRY_COMPARE(query, QStringLiteral("bor"));
        card.setPeopleResults(QStringLiteral("stale"), {{QStringLiteral("99"), QStringLiteral("Чужой ответ"), {}, {}}});
        QVERIFY(!apply->isEnabled());
        card.setPeopleResults(request, {{QStringLiteral("14"), QStringLiteral("Борис"), {}, {}}});
        if (const auto path = qEnvironmentVariable("B24_REQUISITE_POPUP_DIR"); !path.isEmpty()) {
            QTest::qWait(20);
            QVERIFY(card.grab().save(path + QStringLiteral("/people-popup.png")));
        }
        QTest::keyClick(search, Qt::Key_Return);
        QVERIFY(card.hasUnsavedChanges() && apply->isEnabled());
        auto *added = findAccessible<QPushButton>(&card, QStringLiteral("Удалить наблюдателя Борис"));
        QVERIFY(added);
        QTRY_VERIFY(added->isVisible());
        apply->click();
        QCOMPARE(edits.size(), 1);
        const QStringList oldObservers{QStringLiteral("13")};
        const QStringList newObservers{QStringLiteral("13"), QStringLiteral("14")};
        QCOMPARE(edits.last().originalObserverIds, std::optional<QStringList>(oldObservers));
        QCOMPARE(edits.last().observerIds, std::optional<QStringList>(newObservers));
        QVERIFY(!edits.last().creatorId && !edits.last().deadline && !edits.last().statusAction);
        auto *removeOriginal = findAccessible<QPushButton>(&card, QStringLiteral("Удалить наблюдателя Анна"));
        QVERIFY(removeOriginal);
        removeOriginal->click();
        QVERIFY(card.hasUnsavedChanges() && !apply->isEnabled());

        task.observers.append(QStringLiteral("Вера"));
        task.observerPeople.append({QStringLiteral("15"), QStringLiteral("Вера")});
        card.setTask(task);
        QVERIFY(findAccessible<QLabel>(observers, QStringLiteral("Вера")));
        QVERIFY(card.hasUnsavedChanges() && !apply->isEnabled());
        cancel->click();
        QVERIFY(!card.hasUnsavedChanges());
        QVERIFY(row(QStringLiteral("Крайний срок")));
        row(QStringLiteral("Крайний срок"))->click();
        auto *withoutDeadline = findAccessible<QCheckBox>(&card, QStringLiteral("Без срока"));
        auto *reason = findAccessible<QLineEdit>(&card, QStringLiteral("Причина изменения срока"));
        QVERIFY(withoutDeadline && reason && reason->isVisible());
        withoutDeadline->setChecked(true);
        QVERIFY(!apply->isEnabled());
        reason->setText(QStringLiteral("Новая оценка"));
        QVERIFY(apply->isEnabled());
        apply->click();
        QCOMPARE(edits.size(), 2);
        QCOMPARE(edits.last().deadline, std::optional<QString>(QString()));
        QCOMPARE(edits.last().deadlineReason, std::optional<QString>(QStringLiteral("Новая оценка")));
        card.acceptEdit();

        QVERIFY(row(QStringLiteral("Статус")));
        row(QStringLiteral("Статус"))->click();
        auto *status = findAccessible<QComboBox>(&card, QStringLiteral("Статус задачи"));
        QVERIFY(status && status->isVisible());
        QCOMPARE(status->count(), 3);
        QCOMPARE(status->itemText(1), QStringLiteral("Приостановить"));
        status->setCurrentIndex(2);
        apply->click();
        QCOMPARE(edits.last().statusAction, std::optional<TaskStatusAction>(TaskStatusAction::Complete));
        card.acceptEdit();

        card.setStages({{QStringLiteral("601"), QStringLiteral("Работа"), {}, 0}});
        QVERIFY(row(QStringLiteral("Проект")));
        row(QStringLiteral("Проект"))->click();
        auto *project = findAccessible<QListWidget>(&card, QStringLiteral("Проект задачи"));
        QVERIFY(project && project->isVisible());
        auto *projectSearch = findAccessible<QLineEdit>(&card, QStringLiteral("Поиск проекта"));
        QVERIFY(projectSearch && projectSearch->isVisible() && popup->isAncestorOf(projectSearch));
        auto *projectField = projectSearch->parentWidget();
        QCOMPARE(projectField->objectName(), QStringLiteral("taskSelectionField"));
        auto *currentProjectPill = projectField->findChild<QLabel *>(QStringLiteral("taskInlineSelectionValue"));
        QVERIFY(currentProjectPill);
        QCOMPARE(currentProjectPill->text(), QStringLiteral("Стройка"));
        if (const auto path = qEnvironmentVariable("B24_REQUISITE_POPUP_DIR"); !path.isEmpty()) {
            QTest::qWait(20);
            QVERIFY(card.grab().save(path + QStringLiteral("/project-popup.png")));
        }
        QTest::keyClick(projectSearch, Qt::Key_Escape);
        QTRY_VERIFY(!popup->isVisible());
        QVERIFY(row(QStringLiteral("Проект"))->isEnabled());
        row(QStringLiteral("Проект"))->click();
        QVERIFY(projectSearch->isVisible());
        projectSearch->setText(QStringLiteral("Склад"));
        QCOMPARE(project->count(), 1);
        const auto previousEdits = edits.size();
        project->setCurrentRow(0);
        QCOMPARE(edits.size(), previousEdits);
        QTest::keyClick(projectSearch, Qt::Key_Return);
        QCOMPARE(edits.size(), previousEdits + 1);
        QCOMPARE(edits.last().projectId, std::optional<QString>(QStringLiteral("102")));
        QCOMPARE(apply->accessibleName(), QStringLiteral("Повторить изменение реквизита"));
        QVERIFY(apply->isEnabled());
        auto portalProject = task;
        portalProject.projectId = QStringLiteral("105");
        portalProject.project = QStringLiteral("Архив");
        card.setTask(portalProject);
        QVERIFY(!apply->isEnabled());
        QCOMPARE(projectSearch->text(), QStringLiteral("Склад"));
        bool foundProjectConflict = false;
        for (const auto *label : projectField->parentWidget()->parentWidget()->parentWidget()->findChildren<QLabel *>())
            foundProjectConflict |= label->isVisible() && label->text().contains(QStringLiteral("Сейчас на портале: Архив"));
        QVERIFY(foundProjectConflict);
        card.setTask(task);
        QVERIFY(apply->isEnabled());
        projectSearch->setText(QStringLiteral("Стройка"));
        QVERIFY(!apply->isEnabled());
        apply->click();
        QCOMPARE(edits.size(), previousEdits + 1);
        projectSearch->clear();
        QVERIFY(project->currentItem());
        QCOMPARE(project->currentItem()->text(), QStringLiteral("Склад"));
        QVERIFY(apply->isEnabled());
        apply->click();
        QCOMPARE(edits.size(), previousEdits + 2);
        QCOMPARE(edits.last().projectId, std::optional<QString>(QStringLiteral("102")));
        card.acceptEdit();
        QCOMPARE(card.projectId(), QStringLiteral("102"));
        auto *stage = findAccessible<QListWidget>(&card, QStringLiteral("Этап задачи"));
        QVERIFY(stage);
        QCOMPARE(stage->count(), 0);

        card.setCanDetachProject(true);
        row(QStringLiteral("Проект"))->click();
        const auto noProject = project->findItems(QStringLiteral("Без проекта"), Qt::MatchExactly);
        QCOMPARE(noProject.size(), 1);
        project->setCurrentItem(noProject.first());
        QTest::keyClick(project, Qt::Key_Return);
        QCOMPARE(edits.last().projectId, std::optional<QString>(QStringLiteral("0")));
        card.acceptEdit();
        QVERIFY(card.projectId().isEmpty());
    }

    void mentionSuggestionsLoadVisiblePhotosAndKeepKeyboardSelection()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdAvatarResponses = true;
        const QUrl portrait(QStringLiteral("https://example.bitrix24.ru/upload/mention-anna.png"));
        const QUrl offscreen(QStringLiteral("https://example.bitrix24.ru/upload/mention-olga.png"));
        const auto name = QStringLiteral("Анна Орлова");
        const auto position = QStringLiteral("Руководитель отдела по работе с клиентами");
        backend->people = {{QStringLiteral("42"), name, position, portrait},
                           {QStringLiteral("43"), QStringLiteral("Михаил Новиков"), {}, {}}};
        for (int i = 0; i < 8; ++i)
            backend->people.append({QString::number(50 + i), QStringLiteral("Сотрудник %1").arg(i), {}, {}});
        backend->people.append({QStringLiteral("60"), QStringLiteral("Ольга Волкова"), {}, offscreen});

        MainWindow window(backend.get());
        window.resize(900, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *input = findAccessible<QPlainTextEdit>(&window, QStringLiteral("Текст сообщения"));
        auto *suggestions = findAccessible<QListWidget>(&window, QStringLiteral("Найденные сотрудники"));
        QVERIFY(input && suggestions);
        input->setFocus();
        QTest::keyClicks(input, "@");
        QTRY_VERIFY(suggestions->isVisible());
        QTRY_COMPARE(suggestions->count(), backend->people.size());
        QTRY_COMPARE(backend->avatarRequests.count(portrait), 1);
        QVERIFY(!backend->avatarRequests.contains(offscreen));
        QCOMPARE(suggestions->item(0)->font().weight(), QFont::DemiBold);
        QVERIFY(suggestions->item(0)->data(Qt::AccessibleTextRole).toString().contains(position));
        QVERIFY(!suggestions->item(1)->icon().isNull());

        QImage photo(36, 36, QImage::Format_RGB32);
        const QColor red(QStringLiteral("#ef392b"));
        photo.fill(red);
        QByteArray png;
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(photo.save(&buffer, "PNG"));
        emit backend->avatarLoaded(portrait, png);
        const auto photoColor = [suggestions] {
            const auto icon = suggestions->item(0)->icon().pixmap(22, 22).toImage();
            return icon.isNull() ? QColor() : icon.pixelColor(10, 10);
        };
        QTRY_COMPARE(photoColor(), red);

        suggestions->scrollToItem(suggestions->item(suggestions->count() - 1));
        QTRY_COMPARE(backend->avatarRequests.count(offscreen), 1);
        QTest::keyClick(input, Qt::Key_Down);
        QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(input->toPlainText().trimmed(), QStringLiteral("@") + name);
        QVERIFY(!suggestions->isVisible());
        QVERIFY(backend->sentChat.isEmpty());
    }

    void scopedMentionsSendFromChatAndTaskCardWithLazyTaskChoices()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto &task = backend->stages.first().tasks.first();
        task.projectId = QStringLiteral("101");
        task.project = QStringLiteral("Проект один");
        task.chatDialogId = QStringLiteral("chat451");
        task.editRights.project = true;
        backend->people = {{QStringLiteral("42"), QStringLiteral("Анна"), {}, {}}};
        backend->projects.append({QStringLiteral("102"), QStringLiteral("Проект два"), {}});
        backend->projectStageChoices = {{QStringLiteral("601"), QStringLiteral("Работа"), {}, {}},
                                        {QStringLiteral("602"), QStringLiteral("Готово"), {}, {}}};
        backend->moreProjects = {{QStringLiteral("103"), QStringLiteral("Проект с новой страницы"), {}}};
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *input = findAccessible<QPlainTextEdit>(&window, QStringLiteral("Текст сообщения"));
        QVERIFY(input);
        input->setFocus();
        QTest::keyClicks(input, "@");
        QTRY_COMPARE(backend->peopleRequests.size(), 1);
        QCOMPARE(backend->peopleRequests.last().first, QString());
        QCOMPARE(backend->peopleRequests.last().second, QStringLiteral("c-1"));
        QTest::keyClick(input, Qt::Key_Down);
        QTest::keyClick(input, Qt::Key_Return);
        QVERIFY(backend->sentChat.isEmpty());
        QCOMPARE(input->toPlainText().trimmed(), QStringLiteral("@Анна"));
        QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(backend->sentChat, QStringLiteral("[USER=42]Анна[/USER]"));

        QCOMPARE(backend->projectRequests, 0);
        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        QApplication::setActiveWindow(card);
        QCOMPARE(backend->projectRequests, 1);
        QCOMPARE(backend->projectStageRequests, QStringList{QStringLiteral("101")});
        auto *stage = findAccessible<QListWidget>(card, QStringLiteral("Этап задачи"));
        auto *move = findAccessible<QPushButton>(card, QStringLiteral("Повторить перенос задачи"));
        QVERIFY(stage && move && stage->count() == 2);
        auto *currentStage = findAccessible<QLabel>(card, QStringLiteral("Этап"));
        QVERIFY(currentStage);
        QCOMPARE(currentStage->text(), QStringLiteral("Работа"));
        auto *requisite = findAccessible<QPushButton>(card, QStringLiteral("Проект"));
        QVERIFY(requisite && requisite->isEnabled());
        requisite->click();
        auto *moreProjects = findAccessible<QPushButton>(card, QStringLiteral("Загрузить следующие проекты"));
        auto *projectChoices = findAccessible<QListWidget>(card, QStringLiteral("Проект задачи"));
        QVERIFY(moreProjects && moreProjects->isVisible() && projectChoices);
        QVERIFY(projectChoices->findItems(QStringLiteral("Проект с новой страницы"), Qt::MatchExactly).isEmpty());
        moreProjects->click();
        QCOMPARE(backend->projectMoreRequests, 1);
        QVERIFY(!projectChoices->findItems(QStringLiteral("Проект с новой страницы"), Qt::MatchExactly).isEmpty());
        auto *cancelRequisite = findAccessible<QPushButton>(card, QStringLiteral("Отменить изменение реквизита"));
        QVERIFY(cancelRequisite);
        cancelRequisite->click();
        auto *taskInput = findAccessible<QPlainTextEdit>(card, QStringLiteral("Текст в чате карточки"));
        QVERIFY(taskInput);
        taskInput->setFocus();
        QTest::keyClicks(taskInput, "+");
        QTRY_COMPARE(backend->peopleRequests.size(), 2);
        QCOMPARE(backend->peopleRequests.last().first, QString());
        QCOMPARE(backend->peopleRequests.last().second, QStringLiteral("chat451"));
        QTest::keyClick(taskInput, Qt::Key_Down);
        QTest::keyClick(taskInput, Qt::Key_Return);
        QVERIFY(backend->sentTask.isEmpty());
        QCOMPARE(taskInput->toPlainText().trimmed(), QStringLiteral("@Анна"));
        QTest::keyClick(taskInput, Qt::Key_Return);
        QCOMPARE(backend->sentTask, QStringLiteral("[USER=42]Анна[/USER]"));

        backend->holdTaskUpdate = true;
        auto *stageRequisite = findAccessible<QPushButton>(card, QStringLiteral("Этап"));
        QVERIFY(stageRequisite && stageRequisite->isEnabled());
        stageRequisite->click();
        QVERIFY(stage->isVisible());
        stage->setCurrentRow(1);
        QVERIFY(!move->isVisible());
        QTest::keyClick(stage, Qt::Key_Return);
        QCOMPARE(backend->lastUpdate.stageId, std::optional<QString>(QStringLiteral("602")));
        QCOMPARE(backend->updatedTask, QStringLiteral("501"));
        QVERIFY(backend->movedTask.isEmpty());
    }

    void taskPeoplePickerLoadsInitialSuggestionsAndRestoresAfterTyping()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto &task = backend->stages.first().tasks.first();
        task.assigneeId = QStringLiteral("12");
        task.assignee = QStringLiteral("Анна");
        task.editRights.assignee = true;
        backend->holdTaskUpdate = true;
        backend->people = {{QStringLiteral("12"), QStringLiteral("Анна"), {}, {}},
                           {QStringLiteral("13"), QStringLiteral("Вера"), {}, {}},
                           {QStringLiteral("21"), QStringLiteral("Алексей"), {}, {}},
                           {QStringLiteral("22"), QStringLiteral("Галина"), {}, {}},
                           {QStringLiteral("23"), QStringLiteral("Ирина"), {}, {}},
                           {QStringLiteral("24"), QStringLiteral("Сергей"), {}, {}},
                           {QStringLiteral("25"), QStringLiteral("Владислав"), {}, {}},
                           {QStringLiteral("26"), QStringLiteral("Татьяна"), {}, {}}};
        const auto initialCount = backend->people.size();
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        window.openTaskCard(task.id);
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        QCOMPARE(backend->peopleRequests.size(), 0);
        auto *assignee = findAccessible<QPushButton>(card, QStringLiteral("Исполнитель"));
        QVERIFY(assignee && assignee->isEnabled());
        assignee->click();
        auto *search = findAccessible<QLineEdit>(card, QStringLiteral("Поиск сотрудника · Исполнитель"));
        QVERIFY(search && search->isVisible());
        QTRY_COMPARE_WITH_TIMEOUT(backend->peopleRequests.size(), 1, 1000);
        const QPair<QString, QString> initialRequest;
        QCOMPARE(backend->peopleRequests.first(), initialRequest);
        const auto initialId = backend->lastPeopleRequestId;
        QListWidget *suggestions = nullptr;
        for (auto *list : card->findChildren<QListWidget *>()) {
            if (list->isVisible() && list->accessibleName() == QStringLiteral("Найденные сотрудники"))
                suggestions = list;
        }
        QVERIFY(suggestions);
        QCOMPARE(suggestions->count(), initialCount);
        QTRY_VERIFY(suggestions->viewport()->visibleRegion().boundingRect().height()
                    >= 6 * suggestions->sizeHintForRow(0));
        QCOMPARE(suggestions->item(0)->text(), QStringLiteral("Анна"));
        QCOMPARE(suggestions->item(0)->checkState(), Qt::Checked);
        QVERIFY(!suggestions->item(0)->icon().isNull());
        auto *popup = card->findChild<QFrame *>(QStringLiteral("taskRequisitePopup"));
        QVERIFY(popup);
        auto *field = search->parentWidget();
        QCOMPARE(field->objectName(), QStringLiteral("taskSelectionField"));
        const auto pillValues = [](const QWidget *inputField) {
            QList<QLabel *> labels;
            for (auto *label : inputField->findChildren<QLabel *>(QStringLiteral("taskInlineSelectionValue")))
                if (label->isVisible())
                    labels.append(label);
            return labels;
        };
        QTRY_COMPARE(pillValues(field).size(), 1);
        QCOMPARE(pillValues(field).front()->text(), QStringLiteral("Анна"));
        QVERIFY(!pillValues(field).front()->parentWidget()->property("pending").toBool());
        QVERIFY(search->width() >= 100);
        for (const auto *summary : popup->findChildren<QLabel *>()) {
            if (summary->accessibleName() == QStringLiteral("Текущее значение"))
                QVERIFY(!summary->isVisible());
        }
        QTest::keyClick(search, Qt::Key_Down);
        QTest::keyClick(search, Qt::Key_Down);
        QCOMPARE(suggestions->currentItem()->text(), QStringLiteral("Вера"));
        QCOMPARE(suggestions->item(0)->checkState(), Qt::Checked);
        QCOMPARE(pillValues(field).front()->text(), QStringLiteral("Анна"));
        QVERIFY(!pillValues(field).front()->parentWidget()->property("pending").toBool());

        if (const auto dir = qEnvironmentVariable("B24_PICKER_DESIGN_DIR"); !dir.isEmpty()) {
            QTest::qWait(20);
            QVERIFY(card->grab().save(dir + QStringLiteral("/people-wide.png")));
        }

        backend->people = {{QStringLiteral("14"), QStringLiteral("Борис"), {}, {}}};
        QTest::keyClicks(search, "bor");
        QTRY_COMPARE(backend->peopleRequests.size(), 2);
        QCOMPARE(backend->peopleRequests.last().first, QStringLiteral("bor"));
        QCOMPARE(suggestions->count(), 1);
        QCOMPARE(suggestions->item(0)->text(), QStringLiteral("Борис"));
        const auto *status = findAccessible<QLabel>(&window, QStringLiteral("Состояние"));
        QVERIFY(status);
        const auto previousStatus = status->text();
        emit backend->failed(QStringLiteral("search-people:") + initialId, QStringLiteral("Просроченный поиск"));
        QCOMPARE(status->text(), previousStatus);
        QCOMPARE(suggestions->item(0)->text(), QStringLiteral("Борис"));
        auto portalTask = task;
        portalTask.assignee = QStringLiteral("Галина");
        portalTask.assigneeId = QStringLiteral("15");
        emit backend->taskLoaded(portalTask);
        QCOMPARE(search->text(), QStringLiteral("bor"));
        QTRY_COMPARE(pillValues(field).size(), 1);
        QCOMPARE(pillValues(field).front()->text(), QStringLiteral("Галина"));
        QVERIFY(!pillValues(field).front()->parentWidget()->property("pending").toBool());
        portalTask.assignee = QStringLiteral("Галина Петрова");
        emit backend->taskLoaded(portalTask);
        QTRY_COMPARE(pillValues(field).size(), 1);
        QCOMPARE(pillValues(field).front()->text(), QStringLiteral("Галина Петрова"));
        emit backend->taskLoaded(task);
        QTest::keyClick(search, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(search, Qt::Key_Backspace);
        QTRY_COMPARE(suggestions->count(), initialCount);
        QCOMPARE(suggestions->item(0)->checkState(), Qt::Checked);
        QCOMPARE(suggestions->item(1)->text(), QStringLiteral("Вера"));
        QCOMPARE(backend->peopleRequests.size(), 2);
        const int wideListHeight = suggestions->height();
        card->resize(660, 420);
        QTRY_VERIFY(suggestions->height() < wideListHeight);
        QVERIFY(card->rect().contains(popup->geometry()));
        if (const auto dir = qEnvironmentVariable("B24_PICKER_DESIGN_DIR"); !dir.isEmpty()) {
            QTest::qWait(30);
            QVERIFY(card->grab().save(dir + QStringLiteral("/people-compact.png")));
        }

        for (int i = 0; i < 2; ++i)
            QTest::keyClick(search, Qt::Key_Down);
        QTest::keyClick(search, Qt::Key_Return);
        QCOMPARE(backend->lastUpdate.assigneeId, std::optional<QString>(QStringLiteral("13")));
        emit backend->failed(QStringLiteral("update-task:501"), QStringLiteral("Портал отклонил изменение"));
        QTRY_COMPARE(pillValues(field).size(), 1);
        QCOMPARE(pillValues(field).front()->text(), QStringLiteral("Вера"));
        QVERIFY(pillValues(field).front()->parentWidget()->property("pending").toBool());
        QCOMPARE(suggestions->item(0)->checkState(), Qt::Checked);
        QVERIFY(suggestions->item(1)->data(bx::ui::TaskPickerDraft).toBool());
        auto *cancel = findAccessible<QPushButton>(popup, QStringLiteral("Отменить изменение реквизита"));
        QVERIFY(cancel && cancel->isVisible());
        auto *applyPerson = findAccessible<QPushButton>(popup, QStringLiteral("Повторить изменение реквизита"));
        QVERIFY(applyPerson && applyPerson->isEnabled());
        emit backend->taskLoaded(portalTask);
        QVERIFY(!applyPerson->isEnabled());
        QTRY_COMPARE(pillValues(field).size(), 1);
        QCOMPARE(pillValues(field).front()->text(), QStringLiteral("Вера"));
        QVERIFY(pillValues(field).front()->parentWidget()->property("pending").toBool());
        const auto labels = popup->findChildren<QLabel *>();
        QVERIFY(std::any_of(labels.cbegin(), labels.cend(), [](const QLabel *label) {
            return label->isVisible() && label->text().contains(QStringLiteral("Сейчас на портале: Галина Петрова"));
        }));
        cancel->click();
        auto updated = task;
        updated.editRights.observers = true;
        updated.observers = {
            QStringLiteral("Анастасия Александровна Константинопольская"),
            QStringLiteral("Екатерина Дмитриевна Великогородская"),
            QStringLiteral("Валентина Вячеславовна Новосибирская")};
        for (int i = 0; i < updated.observers.size(); ++i)
            updated.observerPeople.append({QString::number(100 + i), updated.observers.at(i), {}});
        emit backend->taskLoaded(updated);
        backend->people = {{QStringLiteral("200"), QStringLiteral("Новый наблюдатель"), {}, {}}};
        auto *observers = findAccessible<QPushButton>(card, QStringLiteral("Наблюдатели"));
        QVERIFY(observers && observers->isEnabled());
        observers->click();
        auto *observerSearch = findAccessible<QLineEdit>(popup, QStringLiteral("Поиск сотрудника · Наблюдатели"));
        QVERIFY(observerSearch && observerSearch->isVisible());
        QTRY_COMPARE(backend->peopleRequests.size(), 3);
        const auto *observerField = observerSearch->parentWidget();
        while (observerField && observerField->objectName() != QStringLiteral("taskSelectionField"))
            observerField = observerField->parentWidget();
        QVERIFY(observerField);
        QTRY_COMPARE(pillValues(observerField).size(), 3);
        const auto *firstValue = pillValues(observerField).front();
        QCOMPARE(firstValue->accessibleName(), updated.observers.first());
        QTRY_COMPARE(firstValue->text(), updated.observers.first());
        const auto fieldRect = [observerField](const QWidget *child) {
            return QRect(child->mapTo(observerField, QPoint()), child->size());
        };
        const auto lastChip = [&] { return fieldRect(pillValues(observerField).last()->parentWidget()); };
        QTRY_VERIFY(lastChip().top() > fieldRect(firstValue->parentWidget()).top());
        const auto wrapsInOrder = [&] {
            for (const auto *value : pillValues(observerField)) {
                const auto *chip = value->parentWidget();
                if (!observerField->rect().contains(fieldRect(chip))
                    || !chip->visibleRegion().boundingRect().contains(chip->rect()))
                    return false;
            }
            const auto input = fieldRect(observerSearch);
            return input.top() > lastChip().bottom() || input.left() > lastChip().right();
        };
        QVERIFY(wrapsInOrder());
        QTest::keyClick(observerSearch, Qt::Key_Down);
        QTest::keyClick(observerSearch, Qt::Key_Return);
        QTRY_COMPARE(pillValues(observerField).size(), 4);
        QCOMPARE(pillValues(observerField).last()->accessibleName(), QStringLiteral("Новый наблюдатель"));
        QVERIFY(observerSearch->width() >= 100);
        QTRY_VERIFY(wrapsInOrder());
        QVERIFY(pillValues(observerField).last()->parentWidget()->property("pending").toBool());
        auto *apply = findAccessible<QPushButton>(popup, QStringLiteral("Применить изменение реквизита"));
        QVERIFY(apply && apply->isEnabled());
        const auto withinPopup = [popup](const QWidget *control) {
            return popup->rect().contains(QRect(control->mapTo(popup, QPoint()), control->size()));
        };
        QTRY_VERIFY(withinPopup(apply) && withinPopup(cancel));
        if (const auto dir = qEnvironmentVariable("B24_OBSERVER_WRAP_DIR"); !dir.isEmpty()) {
            QVERIFY(popup->grab().save(dir + QStringLiteral("/observers-narrow.png")));
            card->resize(1070, 650);
            QTRY_VERIFY(popup->width() >= 390);
            QTRY_VERIFY(wrapsInOrder());
            QTest::qWait(30);
            QVERIFY(popup->grab().save(dir + QStringLiteral("/observers-wide.png")));
        }
        auto *remove = findAccessible<QPushButton>(popup, QStringLiteral("Удалить наблюдателя Новый наблюдатель"));
        QVERIFY(remove && remove->isVisible());
        remove->click();
        QTRY_COMPARE(pillValues(observerField).size(), 3);
        QVERIFY(!apply->isEnabled());
    }

    void taskRequisitesLoadAndPaintScopedPortraits()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        const QUrl creator(QStringLiteral("https://example.bitrix24.ru/upload/card-creator.png"));
        const QUrl assignee(QStringLiteral("https://example.bitrix24.ru/upload/card-assignee.png"));
        const QUrl observer(QStringLiteral("https://example.bitrix24.ru/upload/card-observer.png"));
        const QUrl project(QStringLiteral("https://example.bitrix24.ru/upload/card-project.png"));
        const QColor color("#bf327c");
        QImage image(32, 32, QImage::Format_RGB32);
        image.fill(color);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(image.save(&buffer, "PNG"));
        for (const auto &url : {creator, assignee, observer, project})
            backend->avatarImages.insert(url, bytes);
        auto &task = backend->stages.first().tasks.first();
        task.projectId = backend->projects.first().id;
        task.status = QStringLiteral("Выполняется");
        task.statusCode = QStringLiteral("3");
        task.statusActions = {TaskStatusAction::Complete};
        task.createdAt = QDateTime(QDate(2026, 1, 1), QTime(9, 15));
        task.creatorId = QStringLiteral("72");
        task.createdBy = QStringLiteral("Алексей Смирнов");
        task.creatorAvatarUrl = creator;
        task.assigneeId = QStringLiteral("73");
        task.assignee = QStringLiteral("Ирина Волкова");
        task.assigneeAvatarUrl = assignee;
        task.observerPeople = {{QStringLiteral("74"), QStringLiteral("Анна Петрова"), observer},
                               {QStringLiteral("75"), QStringLiteral("Иван Соколов"), {}}};
        task.observers = {QStringLiteral("Анна Петрова"), QStringLiteral("Иван Соколов")};
        backend->projects.first().avatarUrl = project;
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        window.openTaskCard(task.id);
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        card->resize(660, 420);
        auto *details = card->findChild<QScrollArea *>(QStringLiteral("taskDetailsScroll"));
        QVERIFY(details);
        const auto checkPhoto = [&](const QString &caption, const QUrl &url) {
            auto *row = findAccessible<QPushButton>(card, caption);
            QVERIFY(row);
            auto *badge = row->findChild<QWidget *>(QStringLiteral("taskRequisiteAvatar"));
            QVERIFY2(badge, qPrintable(caption));
            details->ensureWidgetVisible(badge, 4, 4);
            QTRY_VERIFY(backend->avatarRequests.contains(url));
            const auto centerColor = [badge] {
                const auto pixels = badge->grab().toImage();
                return pixels.pixelColor(pixels.rect().center());
            };
            QTRY_COMPARE(centerColor(), color);
            QCOMPARE(backend->avatarRequests.count(url), 1);
        };
        checkPhoto(QStringLiteral("Постановщик"), creator);
        checkPhoto(QStringLiteral("Исполнитель"), assignee);
        checkPhoto(QStringLiteral("Проект"), project);
        checkPhoto(QStringLiteral("Наблюдатели"), observer);
        const auto *observers = findAccessible<QPushButton>(card, QStringLiteral("Наблюдатели"));
        QVERIFY(observers);
        QCOMPARE(observers->findChildren<QWidget *>(QStringLiteral("taskRequisiteAvatar")).size(), 2);
        if (const auto output = qEnvironmentVariable("B24_REQUISITE_SCREENSHOT"); !output.isEmpty()) {
            card->resize(900, 650);
            details->ensureWidgetVisible(findAccessible<QPushButton>(card, QStringLiteral("Наблюдатели")), 4, 4);
            QTest::qWait(20);
            QVERIFY(card->grab().save(output));
        }
    }

    void taskFilesPaintSmallJpegOriginalWhenVisible()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto &task = backend->stages.first().tasks.first();
        task.description = QStringLiteral("Описание\n").repeated(30);
        MessageFile photo;
        photo.id = QStringLiteral("90");
        photo.name = QStringLiteral("Упаковка.jpg");
        photo.size = 173000;
        photo.isImage = true;
        photo.downloadUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/bitrix/tools/disk/uf.php?action=download&attachedId=90"));
        photo.previewUrl = photo.downloadUrl;
        task.files = {photo};
        task.attachments = {photo.name};
        const QColor fill("#a46640");
        QByteArray bytes;
        QBuffer buffer(&bytes);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QImage jpeg(240, 140, QImage::Format_RGB32);
        jpeg.fill(fill);
        QVERIFY(jpeg.save(&buffer, "JPEG", 92));
        backend->messageImages.insert(photo.previewUrl, bytes);

        MainWindow window(backend.get());
        window.resize(960, 690);
        window.show();
        window.openTaskCard(task.id);
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        card->resize(760, 520);
        auto *details = card->findChild<QScrollArea *>(QStringLiteral("taskDetailsScroll"));
        auto *files = findAccessible<QListWidget>(card, QStringLiteral("Вложения задачи"));
        QVERIFY(details && files);
        details->setFixedHeight(110);
        QTRY_COMPARE(files->count(), 1);
        QTRY_VERIFY(files->height() > 0);
        QTRY_VERIFY(details->verticalScrollBar()->maximum() > 0);
        details->verticalScrollBar()->setValue(details->verticalScrollBar()->minimum());
        QTRY_VERIFY(!files->viewport()->visibleRegion().intersects(files->visualItemRect(files->item(0))));
        QVERIFY(!backend->imageRequests.contains(photo.previewUrl));

        details->ensureWidgetVisible(files, 0, 6);
        QTRY_VERIFY(files->viewport()->visibleRegion().intersects(files->visualItemRect(files->item(0))));
        QTRY_COMPARE(backend->imageRequests.count(photo.previewUrl), 1);
        QTRY_VERIFY(files->visualItemRect(files->item(0)).height() > 110);
        QTRY_VERIFY(files->visualItemRect(files->item(0)).bottom() < files->viewport()->height());
        details->setFixedHeight(300);
        details->ensureWidgetVisible(files);
        const auto row = files->visualItemRect(files->item(0));
        const auto sample = files->viewport()->grab().toImage().pixelColor(row.left() + 65, row.top() + 45);
        QVERIFY(qAbs(sample.red() - fill.red()) < 5 && qAbs(sample.green() - fill.green()) < 5
                && qAbs(sample.blue() - fill.blue()) < 5);
        if (const auto capture = qEnvironmentVariable("B24_TASK_FILES_SCREENSHOT"); !capture.isEmpty())
            QVERIFY(files->viewport()->grab().save(capture));
        QTest::mouseClick(files->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(row.left() + 65, row.top() + 45));
        QTRY_VERIFY(card->findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));
        QCOMPARE(backend->photoUrl, photo.downloadUrl);
    }

    void taskCardShowsAuthorizedSummaryWhileFinalDetailsArePending()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdTaskDetails = true;
        backend->accountScope = QStringLiteral("fixture/task-summary-first");
        MainWindow window(backend.get());
        window.resize(960, 690);
        window.show();
        window.onConnectionChanged(true);
        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        auto *title = findAccessible<QLabel>(card, QStringLiteral("Название открытой задачи"));
        auto *status = findAccessible<QLabel>(card, QStringLiteral("Состояние карточки"));
        auto *files = findAccessible<QListWidget>(card, QStringLiteral("Вложения задачи"));
        auto *edit = findAccessible<QPushButton>(card, QStringLiteral("Изменить задачу"));
        auto *attach = findAccessible<QPushButton>(card, QStringLiteral("Прикрепить файл к задаче"));
        auto *input = findAccessible<QPlainTextEdit>(card, QStringLiteral("Текст в чате карточки"));
        auto *history = static_cast<ui::TaskCardDialog *>(card)->messageList();
        QVERIFY(title && status && files && edit && attach && input);
        QVERIFY(status->isVisible());
        QVERIFY(backend->taskChatReadRequests.isEmpty());
        QCOMPARE(files->item(0)->data(workspace::Detail).toString(), QStringLiteral("Файлы ещё не загружены"));
        const auto pinsBeforeSummary = backend->pinnedRequests;

        Task summary = backend->stages.first().tasks.first();
        summary.id = QStringLiteral("502");
        summary.name = QStringLiteral("Чужая задача");
        summary.chatDialogId = QStringLiteral("chat42");
        emit backend->taskSummaryLoaded(summary);
        QVERIFY(title->text() != summary.name);
        QVERIFY(backend->taskChatReadRequests.isEmpty());
        QCOMPARE(backend->pinnedRequests, pinsBeforeSummary);
        summary.id = QStringLiteral("501");
        summary.name = QStringLiteral("Краткие данные задачи");
        emit backend->taskSummaryLoaded(summary);
        QCOMPARE(title->text(), summary.name);
        QVERIFY(status->isHidden());
        QVERIFY(!edit->isEnabled() && !attach->isEnabled() && input->isReadOnly());
        QCOMPARE(files->item(0)->data(workspace::Detail).toString(), QStringLiteral("Файлы ещё не загружены"));
        QCOMPARE(backend->taskChatReadRequests, QStringList{summary.id});
        const auto expectedPins = pinsBeforeSummary + QStringList{summary.chatDialogId};
        QCOMPARE(backend->pinnedRequests, expectedPins);
        QTRY_COMPARE(history->count(), backend->taskMessages.size());

        auto full = summary;
        full.name = QStringLiteral("Полная карточка");
        emit backend->taskLoaded(full);
        QCOMPARE(title->text(), full.name);
        QVERIFY(edit->isEnabled() && attach->isEnabled() && !input->isReadOnly());
        QCOMPARE(files->item(0)->data(workspace::Detail).toString(), QStringLiteral("Файлов пока нет"));
        emit backend->taskSummaryLoaded(summary);
        QCOMPARE(title->text(), full.name);
        QCOMPARE(backend->taskChatReadRequests, QStringList{summary.id});
        QCOMPARE(backend->pinnedRequests, expectedPins);

        card->close();
        QTRY_VERIFY(!window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        window.openTaskCard(summary.id);
        card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        title = findAccessible<QLabel>(card, QStringLiteral("Название открытой задачи"));
        QVERIFY(title);
        const auto initialTitle = title->text();
        backend->accountScope = QStringLiteral("fixture/task-summary-second");
        emit backend->accountScopeChanged();
        emit backend->taskSummaryLoaded(summary);
        QCOMPARE(title->text(), initialTitle);
        QCOMPARE(backend->taskChatReadRequests, QStringList{summary.id});
    }

    void taskCardShowsRichDetailsAndEmbedsPasteOnConfirmedUpload()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto &task = backend->stages.first().tasks.first();
        task.description = QStringLiteral("[B]Проверить крепёж[/B]\n[custom]Служебный текст[/custom]\n"
                                          "[disk file id=n12345 width=240 height=150]\n[disk file id=n99999]");
        task.descriptionChecksum = QStringLiteral("test-checksum");
        task.createdBy = QStringLiteral("Алексей Смирнов");
        task.assignee = QStringLiteral("Ирина Волкова");
        task.project = QStringLiteral("Рабочий проект");
        task.status = QStringLiteral("В работе");
        task.createdAt = QDateTime(QDate(2026, 4, 12), QTime(11, 0));
        task.observers = {QStringLiteral("Ольга Иванова")};
        MessageFile photo;
        photo.id = QStringLiteral("5643");
        photo.diskObjectId = QStringLiteral("n12345");
        photo.name = QStringLiteral("Крепёж.png");
        photo.isImage = true;
        photo.previewSize = QSize(240, 150);
        photo.previewUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/preview/task-463"));
        photo.downloadUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/download/task-463"));
        task.files = {photo};
        task.attachments = {photo.name, QStringLiteral("Счёт.pdf")};
        const QColor fill("#9d694c");
        {
            QByteArray bytes;
            QBuffer buffer(&bytes);
            QVERIFY(buffer.open(QIODevice::WriteOnly));
            QImage image(240, 150, QImage::Format_RGB32);
            image.fill(fill);
            QVERIFY(image.save(&buffer, "PNG"));
            backend->messageImages.insert(photo.previewUrl, bytes);
        }
        MainWindow window(backend.get());
        window.resize(960, 690);
        window.show();
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        sections->setCurrentRow(1);
        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        auto *description = findAccessible<QListWidget>(card, QStringLiteral("Описание и встроенные файлы"));
        auto *attachments = findAccessible<QListWidget>(card, QStringLiteral("Вложения задачи"));
        auto *creator = findAccessible<QLabel>(card, QStringLiteral("Постановщик"));
        auto *editor = findAccessible<QTextEdit>(card, QStringLiteral("Описание задачи"));
        QVERIFY(description && attachments && creator && editor);
        auto *detailsPane = card->findChild<QWidget *>(QStringLiteral("taskDetailsPane"));
        QVERIFY(detailsPane && detailsPane->layout());
        auto *detailsLayout = detailsPane->layout();
        const int descriptionSection = detailsLayout->indexOf(description->parentWidget());
        QVERIFY(descriptionSection >= 0);
        QCOMPARE(detailsLayout->indexOf(attachments->parentWidget()), descriptionSection + 1);
        QVERIFY(detailsLayout->itemAt(descriptionSection + 2)->widget()->isAncestorOf(creator));
        QVERIFY(!editor->isVisible());
        QCOMPARE(creator->text(), task.createdBy);
        QCOMPARE(description->count(), 2);
        QCOMPARE(attachments->count(), 2);
        const auto rendered = description->item(0)->data(workspace::MarkupHtml).toString();
        QVERIFY(rendered.contains(QStringLiteral("<b>Проверить крепёж</b>")));
        QVERIFY(!rendered.contains(QStringLiteral("[disk file")));
        QVERIFY(rendered.contains(QStringLiteral("Вложение недоступно")));
        QTRY_COMPARE(backend->imageRequests.count(photo.previewUrl), 1);
        QTRY_VERIFY(description->visualItemRect(description->item(1)).height() > 110);
        QTRY_VERIFY(description->visualItemRect(description->item(1)).bottom() < description->viewport()->height());
        const auto capture = qEnvironmentVariable("B24_TASK_RICH_SCREENSHOT");
        if (!capture.isEmpty())
            QVERIFY(card->grab().save(capture));
        const auto rect = description->visualItemRect(description->item(1));
        const QPoint photoPoint(rect.left() + 32, rect.top() + 30);
        QContextMenuEvent photoMenuEvent(QContextMenuEvent::Mouse, photoPoint,
                                         description->viewport()->mapToGlobal(photoPoint));
        QCoreApplication::sendEvent(description->viewport(), &photoMenuEvent);
        QTRY_VERIFY(visibleMessageMenu(QStringLiteral("imageContextMenu")));
        visibleMessageMenu(QStringLiteral("imageContextMenu"))->actions().last()->trigger();
        QTRY_VERIFY(!visibleMessageMenu(QStringLiteral("imageContextMenu")));
        QCOMPARE(QGuiApplication::clipboard()->image().pixelColor(3, 3), fill);
        QTest::mouseClick(description->viewport(), Qt::LeftButton, Qt::NoModifier,
                          photoPoint);
        QTRY_VERIFY(card->findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));
        QCOMPARE(backend->photoUrl, photo.downloadUrl);
        card->findChild<QDialog *>(QStringLiteral("chatPhotoViewer"))->reject();
        QTRY_VERIFY(!card->findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));

        auto *edit = findAccessible<QPushButton>(card, QStringLiteral("Изменить задачу"));
        auto *save = findAccessible<QPushButton>(card, QStringLiteral("Сохранить задачу"));
        QVERIFY(edit && save);
        edit->click();
        QTRY_VERIFY(editor->isVisible() && !editor->isReadOnly());
        auto cursor = editor->textCursor();
        cursor.movePosition(QTextCursor::End);
        editor->setTextCursor(cursor);
        backend->attachmentEmbedId = QStringLiteral("n77777");
        const QImage pasted(20, 12, QImage::Format_RGB32);
        QGuiApplication::clipboard()->setImage(pasted);
        editor->paste();
        QCOMPARE(backend->attachedTask, task.id);
        QVERIFY(!QFileInfo::exists(backend->attachedPath));
        save->click();
        QVERIFY(backend->lastUpdate.description);
        QCOMPARE(backend->lastUpdate.originalDescription, std::optional<QString>(task.description));
        QCOMPARE(backend->lastUpdate.descriptionChecksum, std::optional<QString>(task.descriptionChecksum));
        QVERIFY(backend->lastUpdate.description->contains(QStringLiteral("[custom]Служебный текст[/custom]")));
        QVERIFY(backend->lastUpdate.description->contains(QStringLiteral("[disk file id=n12345")));
        QVERIFY(backend->lastUpdate.description->contains(QStringLiteral("[disk file id=n77777 width=20 height=12]")));
        QTRY_VERIFY(!editor->isVisible());
    }

    void taskDescriptionAttachmentQueueRefreshesAtEnd_data()
    {
        QTest::addColumn<bool>("failSecond");
        QTest::newRow("both confirmed") << false;
        QTest::newRow("partial success") << true;
    }

    void taskDescriptionAttachmentQueueRefreshesAtEnd()
    {
        QFETCH(bool, failSecond);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdTaskAttachment = true;
        backend->holdTaskUpdate = true;
        const auto taskId = QStringLiteral("501");
        auto &task = backend->stages.first().tasks.first();
        MainWindow window(backend.get());
        window.show();
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        sections->setCurrentRow(1);
        window.openTaskCard(taskId);
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        auto *edit = findAccessible<QPushButton>(card, QStringLiteral("Изменить задачу"));
        auto *save = findAccessible<QPushButton>(card, QStringLiteral("Сохранить задачу"));
        auto *editor = findAccessible<QTextEdit>(card, QStringLiteral("Описание задачи"));
        auto *files = findAccessible<QListWidget>(card, QStringLiteral("Вложения задачи"));
        QVERIFY(edit && save && editor && files);
        edit->click();
        editor->moveCursor(QTextCursor::End);
        const auto draft = QStringLiteral("Мой несохранённый черновик");
        editor->insertPlainText(draft);
        QTemporaryDir input;
        QVERIFY(input.isValid());
        const auto first = input.filePath(QStringLiteral("первый.txt"));
        const auto second = input.filePath(QStringLiteral("второй.txt"));
        for (const auto &path : {first, second}) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("content"), 7);
        }
        auto *clipboard = new QMimeData;
        clipboard->setUrls({QUrl::fromLocalFile(first), QUrl::fromLocalFile(second)});
        QGuiApplication::clipboard()->setMimeData(clipboard);
        const int previousReads = backend->taskReadRequests.count(taskId);
        editor->paste();
        QCOMPARE(backend->attachedPath, first);
        const auto acknowledge = [&](const QString &path, const QString &diskId) {
            MessageFile file;
            file.name = QFileInfo(path).fileName();
            file.diskObjectId = diskId;
            task.files.append(file);
            emit backend->taskFileAttached(taskId, diskId);
            emit backend->fileUploaded(taskId, QStringLiteral("task"));
        };
        acknowledge(first, QStringLiteral("n81"));
        QCOMPARE(backend->attachedPath, second);
        QCOMPARE(backend->taskReadRequests.count(taskId), previousReads);
        if (failSecond)
            emit backend->failed(QStringLiteral("attach-task:501"), QStringLiteral("Второй файл отклонён"));
        else
            acknowledge(second, QStringLiteral("n82"));
        QCOMPARE(backend->taskReadRequests.count(taskId), previousReads + 1);
        QCOMPARE(files->count(), failSecond ? 1 : 2);
        QCOMPARE(files->item(0)->data(workspace::MessageFiles).value<QVector<MessageFile>>().front().name,
                 QFileInfo(first).fileName());
        QVERIFY(editor->toPlainText().contains(draft));
        QVERIFY(save->isEnabled());
        save->click();
        QVERIFY(backend->lastUpdate.description);
        QVERIFY(backend->lastUpdate.description->contains(QStringLiteral("id=n81")));
        QCOMPARE(backend->lastUpdate.description->contains(QStringLiteral("id=n82")), !failSecond);
    }

    void resetsExistingTaskAttachmentOnAccountChange_data()
    {
        QTest::addColumn<bool>("description");
        QTest::newRow("task attachments") << false;
        QTest::newRow("description batch") << true;
    }

    void resetsExistingTaskAttachmentOnAccountChange()
    {
        QFETCH(bool, description);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->accountScope = QStringLiteral("fixture/existing-attachment-first");
        backend->holdTaskAttachment = true;
        const auto task = backend->stages.first().tasks.first();
        MainWindow window(backend.get());
        window.show();
        window.openTaskCard(task.id);
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        auto *attach = findAccessible<QPushButton>(card, QStringLiteral("Прикрепить файл к задаче"));
        auto *edit = findAccessible<QPushButton>(card, QStringLiteral("Изменить задачу"));
        auto *editor = findAccessible<QTextEdit>(card, QStringLiteral("Описание задачи"));
        auto *descriptionStatus = card->findChild<QLabel *>(QStringLiteral("taskDescriptionUploadStatus"));
        QVERIFY(attach && edit && editor && descriptionStatus);
        QTemporaryDir input;
        QVERIFY(input.isValid());
        const auto first = input.filePath(QStringLiteral("target.txt"));
        const auto second = input.filePath(QStringLiteral("queued.txt"));
        for (const auto &path : {first, second}) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("content"), 7);
        }
        const auto choose = [&window, attach](const QString &path) {
            QTimer::singleShot(0, &window, [path] {
                if (auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                    dialog->selectFile(path);
                    QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
                }
            });
            attach->click();
        };
        QTRY_VERIFY(attach->isEnabled());
        if (description) {
            edit->click();
            auto *clipboard = new QMimeData;
            clipboard->setUrls({QUrl::fromLocalFile(first), QUrl::fromLocalFile(second)});
            QGuiApplication::clipboard()->setMimeData(clipboard);
            editor->paste();
            QVERIFY(descriptionStatus->isVisible());
        } else {
            choose(first);
        }
        QCOMPARE(backend->attachedPath, first);
        QVERIFY(!attach->isEnabled());
        backend->accountScope = QStringLiteral("fixture/existing-attachment-second");
        emit backend->accountScopeChanged();
        emit backend->taskLoaded(task);
        QVERIFY(attach->isEnabled());
        QVERIFY(descriptionStatus->isHidden());
        emit backend->taskFileAttached(task.id, QStringLiteral("n123456"));
        emit backend->fileUploaded(task.id, QStringLiteral("task"));
        QCOMPARE(backend->attachmentRequests.size(), 1);
        QVERIFY(!editor->toPlainText().contains(QStringLiteral("target.txt")));
        choose(second);
        QCOMPARE(backend->attachmentRequests.size(), 2);
        QCOMPARE(backend->attachedTask, task.id);
        QCOMPARE(backend->attachedPath, second);
    }

    void taskCardMaximizeChangesNativeFrameGeometry()
    {
        const auto platform = QApplication::platformName();
        if (platform != QLatin1String("xcb") && !platform.startsWith(QLatin1String("wayland")))
            QSKIP("Requires a native window manager with enough room to maximize");
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        MainWindow window(backend.get());
        window.resize(970, 650);
        window.show();
        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        QVERIFY(QTest::qWaitForWindowExposed(card));
        const auto available = card->screen()->availableGeometry().size();
        if (available.width() < 1100 || available.height() < 770)
            QSKIP("Native test screen is too small to distinguish the restored frame");
        auto *maximize = findAccessible<QPushButton>(card, QStringLiteral("Развернуть карточку задачи"));
        QVERIFY(maximize);
        QCOMPARE(card->windowModality(), Qt::WindowModal);
        const auto restoredSize = card->frameGeometry().size();
        maximize->click();
        QTRY_VERIFY_WITH_TIMEOUT(card->frameGeometry().width() >= available.width() - 48
                                 && card->frameGeometry().height() >= available.height() - 72, 3000);
        QVERIFY(card->frameGeometry().width() > restoredSize.width()
                && card->frameGeometry().height() > restoredSize.height());
        maximize->click();
        QTRY_COMPARE(card->frameGeometry().size(), restoredSize);
        QCOMPARE(card->windowModality(), Qt::WindowModal);
    }

    void taskCardSplitterPersistsAfterMaximizeAndReopen()
    {
        QSettings settings(QStringLiteral("bitrix24-client"), QStringLiteral("bitrix24-client"));
        const auto key = QStringLiteral("ui/taskCardDetailsWidth");
        settings.remove(key);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        MainWindow window(backend.get());
        window.resize(960, 690);
        window.show();
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        sections->setCurrentRow(1);
        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        card->resize(900, 540);
        auto *splitter = card->findChild<QSplitter *>(QStringLiteral("taskCardSplitter"));
        auto *maximize = findAccessible<QPushButton>(card, QStringLiteral("Развернуть карточку задачи"));
        QVERIFY(splitter && maximize);
        const int old = splitter->sizes().first();
        auto *handle = splitter->handle(1);
        const auto start = handle->rect().center();
        QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(handle, start + QPoint(48, 0));
        QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier, start + QPoint(48, 0));
        QTRY_VERIFY(splitter->sizes().first() > old + 25);
        const int saved = splitter->sizes().first();
        QCOMPARE(settings.value(key).toInt(), saved);
        maximize->click();
        QTRY_VERIFY(card->isMaximized());
        QVERIFY(splitter->sizes().at(1) >= 285);
        maximize->click();
        QTRY_VERIFY(!card->isMaximized());
        QTRY_COMPARE(splitter->sizes().first(), saved);
        card->resize(660, 420);
        QTRY_VERIFY(splitter->sizes().first() >= 280 && splitter->sizes().at(1) >= 285);
        card->close();
        QTRY_VERIFY(!window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        window.openTaskCard(QStringLiteral("501"));
        card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        card->resize(900, 540);
        splitter = card->findChild<QSplitter *>(QStringLiteral("taskCardSplitter"));
        QVERIFY(splitter);
        QTRY_COMPARE(splitter->sizes().first(), saved);
        card->resize(660, 420);
        auto *description = findAccessible<QListWidget>(card, QStringLiteral("Описание и встроенные файлы"));
        QVERIFY(description);
        QTRY_VERIFY(description->viewport()->visibleRegion().boundingRect().height() >=
                    description->visualItemRect(description->item(0)).height());
        const auto compact = qEnvironmentVariable("B24_TASK_CARD_COMPACT_SCREENSHOT");
        if (!compact.isEmpty())
            QVERIFY(card->grab().save(compact));
        settings.remove(key);
    }

    void movesTaskToAnotherColumn()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->stages[1].color = QStringLiteral("22cc99");
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        backend->holdTaskUpdate = true;
        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        auto *stageRequisite = findAccessible<QPushButton>(card, QStringLiteral("Этап"));
        QVERIFY(stageRequisite && stageRequisite->isEnabled());
        stageRequisite->click();
        auto *stage = findAccessible<QListWidget>(card, QStringLiteral("Этап задачи"));
        QVERIFY(stage && stage->isVisible());
        QCOMPARE(stage->count(), 2);
        auto *popup = card->findChild<QFrame *>(QStringLiteral("taskRequisitePopup"));
        QVERIFY(popup && popup->isVisible() && popup->isAncestorOf(stage));
        QCOMPARE(stage->item(0)->checkState(), Qt::Checked);
        QVERIFY(!stage->item(1)->icon().isNull());
        if (const auto path = qEnvironmentVariable("B24_REQUISITE_POPUP_DIR"); !path.isEmpty()) {
            QTest::qWait(20);
            QVERIFY(card->grab().save(path + QStringLiteral("/stage-popup.png")));
        }
        stage->setCurrentRow(1);
        auto *move = findAccessible<QPushButton>(card, QStringLiteral("Повторить перенос задачи"));
        QVERIFY(move && !move->isVisible());
        QVERIFY(backend->updatedTask.isEmpty());
        QTest::keyClick(stage, Qt::Key_Return);
        QCOMPARE(backend->updatedTask, QStringLiteral("501"));
        QCOMPARE(backend->lastUpdate.stageId, std::optional<QString>(QStringLiteral("602")));
        QVERIFY(backend->movedTask.isEmpty());
        QVERIFY(!move->isEnabled());
        emit backend->failed(QStringLiteral("update-task:501"), QStringLiteral("Портал отклонил перенос"));
        QVERIFY(move->isEnabled());
    }

    void kanbanMoveWaitsForServerAndRejectsUnavailableStages()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->stages.append({QStringLiteral("search-only"), QStringLiteral("Без этапа"), {}});
        backend->stages.append({QStringLiteral("0"), QStringLiteral("Этап не назначен"), {}});
        MainWindow window(backend.get());
        window.resize(1060, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        auto *board = window.findChild<BoardWidget *>();
        auto *status = window.findChild<QLabel *>(QStringLiteral("status"));
        QVERIFY(board && status);
        const auto request = [board](const QString &from, const QString &to) {
            return QMetaObject::invokeMethod(board, "taskMoveRequested", Qt::DirectConnection,
                Q_ARG(QString, QStringLiteral("501")), Q_ARG(QString, from), Q_ARG(QString, to));
        };
        const auto hasTask = [board](const QString &stage) {
            const auto *list = findAccessible<QListWidget>(board, stage);
            return list && list->count() && list->item(0)->data(workspace::Id).toString() == QStringLiteral("501");
        };
        QVERIFY(hasTask(QStringLiteral("В работе")));
        QVERIFY(request(QStringLiteral("601"), QStringLiteral("601")));
        QVERIFY(request(QStringLiteral("601"), QStringLiteral("search-only")));
        QVERIFY(request(QStringLiteral("601"), QStringLiteral("0")));
        QVERIFY(request(QStringLiteral("602"), QStringLiteral("601")));
        QVERIFY(backend->movedTask.isEmpty());
        QVERIFY(request(QStringLiteral("601"), QStringLiteral("602")));
        QCOMPARE(backend->movedProject, QStringLiteral("101"));
        QCOMPARE(backend->movedTask, QStringLiteral("501"));
        QCOMPARE(backend->movedStage, QStringLiteral("602"));
        QCOMPARE(status->text(), QStringLiteral("Переносим задачу…"));
        QVERIFY(hasTask(QStringLiteral("В работе")));
        QVERIFY(!hasTask(QStringLiteral("Готово")));
        backend->movedTask.clear();
        QVERIFY(request(QStringLiteral("601"), QStringLiteral("602")));
        QVERIFY(backend->movedTask.isEmpty());
        emit backend->failed(QStringLiteral("board-edit:101"), QStringLiteral("Портал отклонил перенос"));
        QCOMPARE(status->text(), QStringLiteral("Портал отклонил перенос"));
        QVERIFY(hasTask(QStringLiteral("В работе")));
        QVERIFY(request(QStringLiteral("601"), QStringLiteral("602")));
        QCOMPARE(backend->movedTask, QStringLiteral("501"));
        backend->stages[1].tasks.append(backend->stages[0].tasks.takeFirst());
        backend->stages[1].tasks.first().stageId = QStringLiteral("602");
        backend->holdBoardRead = true;
        const auto reloads = backend->boardRequests;
        emit backend->boardUpdated(QStringLiteral("101"));
        QCOMPARE(backend->boardRequests, reloads + 1);
        QCOMPARE(status->text(), QStringLiteral("Задача перенесена"));
        backend->movedTask.clear();
        QVERIFY(request(QStringLiteral("601"), QStringLiteral("602")));
        QVERIFY(backend->movedTask.isEmpty());
        emit backend->failed(QStringLiteral("board:101"), QStringLiteral("Временный сбой"));
        QCOMPARE(status->text(), QStringLiteral("Не удалось обновить доску после переноса. Нажмите F5"));
        QVERIFY(hasTask(QStringLiteral("В работе")));
        backend->holdBoardRead = false;
        QTest::keyClick(&window, Qt::Key_F5);
        QCOMPARE(backend->boardRequests, reloads + 2);
        QCOMPARE(status->text(), QStringLiteral("Задача перенесена"));
        QVERIFY(hasTask(QStringLiteral("Готово")));
        QVERIFY(!hasTask(QStringLiteral("В работе")));
        backend->movedTask.clear();
        emit backend->connectionChanged(false);
        QVERIFY(request(QStringLiteral("602"), QStringLiteral("601")));
        QVERIFY(backend->movedTask.isEmpty());
    }

    void kanbanNativeDragMovesOnlyBetweenRealColumns()
    {
        const auto pointerTool = QStandardPaths::findExecutable(QStringLiteral("xdotool"));
        if (QGuiApplication::platformName() != QLatin1String("xcb") || pointerTool.isEmpty())
            QSKIP("Native pointer test requires X11 and xdotool");

        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->stages.append({QStringLiteral("synthetic"), QStringLiteral("Без этапа"), {}});
        MainWindow window(backend.get());
        window.resize(1080, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        auto *board = window.findChild<BoardWidget *>();
        auto *source = findAccessible<QListWidget>(board, QStringLiteral("В работе"));
        auto *target = findAccessible<QListWidget>(board, QStringLiteral("Готово"));
        auto *synthetic = findAccessible<QListWidget>(board, QStringLiteral("Без этапа"));
        QVERIFY(source && target && synthetic);
        QVERIFY(target->isVisible());
        const auto from = source->viewport()->mapToGlobal(source->visualItemRect(source->item(0)).center());
        const auto targetArea = target->viewport()->rect().intersected(target->viewport()->visibleRegion().boundingRect());
        QVERIFY(!targetArea.isEmpty());
        const auto drag = [&](const QPoint &to, const QString &button = QStringLiteral("1")) {
            QProcess pointer;
            pointer.start(pointerTool, {QStringLiteral("mousemove"), QString::number(from.x()), QString::number(from.y()),
                QStringLiteral("mousedown"), button,
                QStringLiteral("sleep"), QStringLiteral("0.13"),
                QStringLiteral("mousemove"), QString::number(from.x() + 26), QString::number(from.y()),
                QStringLiteral("sleep"), QStringLiteral("0.13"),
                QStringLiteral("mousemove"), QString::number(to.x()), QString::number(to.y()),
                QStringLiteral("sleep"), QStringLiteral("0.13"),
                QStringLiteral("mouseup"), button});
            QVERIFY(pointer.waitForStarted());
            QTRY_COMPARE_WITH_TIMEOUT(pointer.state(), QProcess::NotRunning, 3000);
            QCOMPARE(pointer.exitCode(), 0);
        };
        const auto to = target->viewport()->mapToGlobal(targetArea.center());
        drag(to, QStringLiteral("2"));
        QVERIFY2(backend->movedTask.isEmpty(), "middle-button gesture started a drag");
        drag(from + QPoint(20, 25));
        QVERIFY2(backend->movedTask.isEmpty(), "same-column drop requested a move");
        const auto invalidArea = synthetic->viewport()->visibleRegion().boundingRect();
        QVERIFY(!invalidArea.isEmpty());
        drag(synthetic->viewport()->mapToGlobal(invalidArea.center()));
        QVERIFY2(backend->movedTask.isEmpty(), "synthetic stage accepted a drop");
        QVERIFY(!window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        drag(to);
        QTRY_COMPARE(backend->movedTask, QStringLiteral("501"));
        QCOMPARE(backend->movedStage, QStringLiteral("602"));
        QVERIFY(!window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
    }

    void listViewShowsFilteredTasks()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);

        auto *listView = findAccessible<QPushButton>(&window, QStringLiteral("Вид Список"));
        QVERIFY(listView);
        listView->click();
        auto *list = findAccessible<QListWidget>(&window, QStringLiteral("Список задач"));
        QVERIFY(list);
        QTRY_COMPARE(list->count(), 1);
        QVERIFY(list->item(0)->text().contains(QStringLiteral("Починить кран")));

        auto *filter = findAccessible<QLineEdit>(&window, QStringLiteral("Фильтр задач"));
        QVERIFY(filter);
        filter->setText(QStringLiteral("№501"));
        QTRY_COMPARE(list->count(), 1);
        filter->setText(QStringLiteral("несуществующая"));
        QTRY_COMPARE(list->count(), 1);
        auto *searchStatus = findAccessible<QLabel>(&window, QStringLiteral("Состояние поиска задач"));
        QVERIFY(searchStatus && searchStatus->isVisible());
        QCOMPARE(searchStatus->text(), QStringLiteral("Ищем задачи в проекте…"));
        QVERIFY(!list->item(0)->text().contains(QStringLiteral("Ничего не найдено")));

        filter->setText(QStringLiteral("Архив"));
        QTest::keyClick(filter, Qt::Key_Return);
        QCOMPARE(backend->searchedProject, QStringLiteral("101"));
        QCOMPARE(backend->searchedTaskQuery, QStringLiteral("Архив"));
        auto remote = backend->stages.first().tasks.first();
        remote.id = QStringLiteral("799");
        remote.name = QStringLiteral("Проверить архивную задачу");
        emit backend->projectTasksFound(QStringLiteral("101"), QStringLiteral("Архив"), QVector<Task>{remote});
        QVERIFY(searchStatus->isHidden());
        QCOMPARE(list->item(0)->data(workspace::Id).toString(), remote.id);
        emit backend->boardLoaded(QStringLiteral("101"), backend->stages);
        QCOMPARE(list->item(0)->data(workspace::Id).toString(), remote.id);
        filter->clear();
        QCOMPARE(list->item(0)->data(workspace::Id).toString(), QStringLiteral("501"));
    }

    void projectSearchResumesAfterLeavingTheSection()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        auto *sections = window.findChild<QListWidget *>(QStringLiteral("sections"));
        auto *filter = findAccessible<QLineEdit>(&window, QStringLiteral("Фильтр задач"));
        auto *searchStatus = findAccessible<QLabel>(&window, QStringLiteral("Состояние поиска задач"));
        QVERIFY(sections && filter && searchStatus);
        sections->setCurrentRow(1);
        filter->setText(QStringLiteral("Архив"));
        QVERIFY(searchStatus->isVisible());

        sections->setCurrentRow(0);
        QVERIFY(searchStatus->isHidden());
        QVERIFY(backend->searchedTaskQuery.isEmpty());
        sections->setCurrentRow(1);
        QTRY_COMPARE(backend->searchedTaskQuery, QStringLiteral("Архив"));
        QVERIFY(searchStatus->isVisible());

        sections->setCurrentRow(0);
        QVERIFY(searchStatus->isHidden());
        emit backend->projectTasksFound(QStringLiteral("101"), QStringLiteral("Архив"), {});
        backend->searchedTaskQuery.clear();
        sections->setCurrentRow(1);
        QTRY_COMPARE(backend->searchedTaskQuery, QStringLiteral("Архив"));
        emit backend->projectTasksFound(QStringLiteral("101"), QStringLiteral("Архив"), {});
        QVERIFY(searchStatus->isHidden());
    }

    void ganttFiltersWithoutServerSearchAndRefreshesAfterTaskCreation()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        Task first = backend->stages.first().tasks.first();
        first.planStart = QDateTime(QDate(2026, 11, 21), QTime(9, 0));
        first.planEnd = first.planStart.addDays(2);
        Task second = first;
        second.id = QStringLiteral("502");
        second.name = QStringLiteral("Проверить склад");
        backend->timelineTasks = {first, second};
        backend->timelineLimited = true;
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        auto *gantt = findAccessible<QPushButton>(&window, QStringLiteral("Вид Гант"));
        auto *filter = findAccessible<QLineEdit>(&window, QStringLiteral("Фильтр задач"));
        auto *timeline = findAccessible<QListWidget>(&window, QStringLiteral("Диаграмма Ганта проекта"));
        auto *range = findAccessible<QLabel>(&window, QStringLiteral("Период плановых дат"));
        QVERIFY(gantt && filter && timeline && range);
        gantt->click();
        QVERIFY(filter->isVisible());
        QTRY_COMPARE(timeline->count(), 2);
        filter->setText(QStringLiteral("№502"));
        QTest::keyClick(filter, Qt::Key_Return);
        QCOMPARE(timeline->count(), 1);
        QCOMPARE(timeline->item(0)->data(workspace::Id).toString(), second.id);
        QVERIFY(backend->searchedTaskQuery.isEmpty());
        filter->setText(QStringLiteral("архив"));
        QCOMPARE(timeline->count(), 0);
        QVERIFY(range->text().contains(QStringLiteral("В загруженных задачах")));

        filter->clear();
        const auto before = backend->timelineRequests.size();
        auto third = second;
        third.name = QStringLiteral("Уточнить сроки");
        third.id = QStringLiteral("503");
        backend->timelineTasks.append(third);
        emit backend->taskCreated(QStringLiteral("101"), third.id);
        QCOMPARE(backend->timelineRequests.size(), before + 1);
        QCOMPARE(timeline->count(), 3);
    }

    void boardLoadsNextPageOnlyOnUserScrollAtTwoThirds()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        const auto sample = backend->stages.first().tasks.first();
        const auto firstStage = backend->stages.first().id;
        const auto project = QStringLiteral("101");
        const auto tasks = [&sample](int first, int count) {
            QVector<Task> result;
            for (int i = 0; i < count; ++i) {
                auto task = sample;
                task.id = QString::number(first + i);
                result.append(task);
            }
            return result;
        };
        backend->stages.first().tasks = tasks(500, 24);
        backend->stages.first().taskCount = 79;
        backend->stages.first().hasMoreTasks = true;
        backend->stages[1].hasMoreTasks = true;
        for (int i = 3; i <= 7; ++i)
            backend->stages.append({QString::number(600 + i), QStringLiteral("Этап %1").arg(i), {}});
        backend->stages.last().hasMoreTasks = true;

        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        const auto scrollDown = [](QListWidget *list, QPoint angle = QPoint(0, -120), QPoint pixels = {},
                                   Qt::ScrollPhase phase = Qt::NoScrollPhase) {
            auto *viewport = list->viewport();
            const auto point = viewport->rect().center();
            QWheelEvent wheel(point, viewport->mapToGlobal(point), pixels, angle,
                              Qt::NoButton, Qt::NoModifier, phase, false);
            QApplication::sendEvent(viewport, &wheel);
        };
        const auto loadingVisible = [](const QLabel *label) {
            const auto *indicator = label->findChild<QWidget *>(QStringLiteral("sharedLoadingIndicator"));
            return indicator && indicator->isVisible();
        };
        auto *board = window.findChild<BoardWidget *>();
        QVERIFY(board);
        QPointer<QListWidget> column = findAccessible<QListWidget>(board, QStringLiteral("В работе"));
        QTRY_VERIFY(column && column->verticalScrollBar()->maximum() > 100);
        QVERIFY(!board->findChild<QPushButton *>(QStringLiteral("moreStageTasks")));
        QCOMPARE(column->count(), 24);
        QTest::qWait(70);
        QVERIFY(backend->boardPageRequests.isEmpty());

        auto *scrollbar = column->verticalScrollBar();
        scrollDown(column);
        QTest::qWait(60);
        QVERIFY(backend->boardPageRequests.isEmpty());
        scrollbar->setValue(scrollbar->maximum());
        QTest::qWait(60);
        QVERIFY(backend->boardPageRequests.isEmpty());
        scrollbar->setValue(0);
        scrollbar->setSliderPosition(scrollbar->maximum());
        QTest::qWait(60);
        QVERIFY(backend->boardPageRequests.isEmpty());
        scrollDown(column, QPoint(120, -1));
        scrollDown(column, QPoint(120, -1), QPoint(120, -1));
        QTest::qWait(60);
        QVERIFY(backend->boardPageRequests.isEmpty());
        const int threshold = (2 * scrollbar->maximum() + 2) / 3;
        scrollbar->setValue(threshold - 1);
        scrollDown(column, QPoint(0, -120), {}, Qt::ScrollMomentum);
        QTRY_COMPARE(backend->boardPageRequests.size(), 1);
        QCOMPARE(backend->boardPageRequests.last().second, firstStage);
        auto *status = board->findChild<QLabel *>(QStringLiteral("quiet"), Qt::FindDirectChildrenOnly);
        QVERIFY(status && status->isVisible() && loadingVisible(status));
        const auto second = tasks(524, 25);
        emit backend->boardTasksPageLoaded(QStringLiteral("999"), firstStage, second, false);
        QCOMPARE(column->count(), 24);
        scrollDown(column);
        QTest::qWait(60);
        QCOMPARE(backend->boardPageRequests.size(), 1);

        column->setCurrentRow(5);
        const auto *selected = column->currentItem();
        const auto previousPosition = scrollbar->value();
        emit backend->boardTasksPageLoaded(project, firstStage, second, true);
        QVERIFY(column);
        QCOMPARE(column->count(), 49);
        QCOMPARE(column->currentItem(), selected);
        QCOMPARE(scrollbar->value(), previousPosition);
        QVERIFY(!loadingVisible(status));
        QVERIFY(!status->isVisible());
        QVERIFY(findAccessible<QLabel>(board, QStringLiteral("Задач в колонке: 79")));
        QTest::qWait(60);
        QCOMPARE(backend->boardPageRequests.size(), 1);

        scrollbar->setValue(0);
        QStyleOptionSlider options;
        options.initFrom(scrollbar);
        options.orientation = Qt::Vertical;
        options.minimum = scrollbar->minimum();
        options.maximum = scrollbar->maximum();
        options.pageStep = scrollbar->pageStep();
        options.singleStep = scrollbar->singleStep();
        options.sliderPosition = scrollbar->sliderPosition();
        options.sliderValue = scrollbar->value();
        const auto handle = scrollbar->style()->subControlRect(QStyle::CC_ScrollBar, &options,
                                                               QStyle::SC_ScrollBarSlider, scrollbar);
        QVERIFY(!handle.isEmpty());
        QTest::mousePress(scrollbar, Qt::LeftButton, Qt::NoModifier, handle.center());
        const QPoint bottom(handle.center().x(), scrollbar->height() - 3);
        QMouseEvent drag(QEvent::MouseMove, bottom, scrollbar->mapToGlobal(bottom),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(scrollbar, &drag);
        QTest::mouseRelease(scrollbar, Qt::LeftButton, Qt::NoModifier, bottom);
        QTRY_COMPARE(backend->boardPageRequests.size(), 2);
        QCOMPARE(backend->boardPageRequests.last().second, firstStage);
        emit backend->boardTasksPageLoaded(project, firstStage, tasks(549, 15), true);
        QCOMPARE(column->count(), 64);

        auto *listButton = findAccessible<QPushButton>(&window, QStringLiteral("Вид Список"));
        QVERIFY(listButton);
        listButton->click();
        auto *list = findAccessible<QListWidget>(board, QStringLiteral("Список задач"));
        QVERIFY(list && list->count() == 64);
        list->setFocus();
        QTest::keyClick(list, Qt::Key_End);
        QTRY_COMPARE(backend->boardPageRequests.size(), 3);
        QCOMPARE(backend->boardPageRequests.last().second, firstStage);
        auto third = tasks(564, 15);
        third.prepend(second.first());
        emit backend->boardTasksPageLoaded(project, firstStage, third, false);
        QCOMPARE(list->count(), 79);
        QCOMPARE(list->item(78)->data(workspace::Id).toString(), QStringLiteral("578"));
        QTest::qWait(60);
        QCOMPARE(backend->boardPageRequests.size(), 3);

        auto *kanbanButton = findAccessible<QPushButton>(&window, QStringLiteral("Вид Канбан"));
        QVERIFY(kanbanButton);
        kanbanButton->click();
        auto *shortColumn = findAccessible<QListWidget>(board, QStringLiteral("Готово"));
        QVERIFY(shortColumn);
        QCOMPARE(shortColumn->verticalScrollBar()->maximum(), 0);
        scrollDown(shortColumn);
        QTRY_COMPARE(backend->boardPageRequests.size(), 4);
        QCOMPARE(backend->boardPageRequests.last().second, backend->stages.at(1).id);
        emit backend->failed(QStringLiteral("board-column:101:602"), QStringLiteral("Нет связи"));
        QVERIFY(!loadingVisible(status));
        scrollDown(shortColumn);
        QTest::qWait(60);
        QCOMPARE(backend->boardPageRequests.size(), 4);
    }

    void refreshesTheSelectedHistoryFirstAndKeepsItsDraft()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        MainWindow window(backend.get());
        window.show();
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *input = findAccessible<QPlainTextEdit>(&window, QStringLiteral("Текст сообщения"));
        auto *refresh = findAccessible<QPushButton>(&window, QStringLiteral("Обновить чат (F5)"));
        if (!refresh)
            refresh = findAccessible<QPushButton>(&window, QStringLiteral("Обновить чаты (F5)"));
        QVERIFY(history && input && refresh);
        QTRY_VERIFY(history->count() > 0);
        input->setPlainText(QStringLiteral("Черновик"));

        const auto verifyRefresh = [&] {
            QCOMPARE(backend->chatRefreshCalls, QStringList({QStringLiteral("c-1"), QStringLiteral("directory")}));
            QCOMPARE(input->toPlainText(), QStringLiteral("Черновик"));
            QVERIFY(history->count() > 0);
        };
        backend->chatRefreshCalls.clear();
        refresh->click();
        verifyRefresh();
        backend->chatRefreshCalls.clear();
        QApplication::setActiveWindow(&window);
        input->setFocus();
        QTest::keyClick(input, Qt::Key_F5);
        verifyRefresh();
    }

    void searchesChatsAndMessagesWithoutPushOverwrite()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);

        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
        auto *messages = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(chats);
        QVERIFY(messages);
        QTRY_COMPARE(messages->count(), 1);

        auto *chatSearch = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск чатов"));
        QVERIFY(chatSearch);
        chatSearch->setText(QStringLiteral("работ"));
        QTest::keyClick(chatSearch, Qt::Key_Return);
        QCOMPARE(backend->searchedChats, QStringLiteral("работ"));

        Chat found;
        found.id = QStringLiteral("c-9");
        found.title = QStringLiteral("Найденный чат");
        found.lastMessage = QStringLiteral("[B]Из поиска[/B]");
        emit backend->chatsFound(QStringLiteral("работ"), QVector<Chat>{found});
        emit backend->recentChatsLoaded(backend->chats);
        QTRY_COMPARE(chats->count(), 1);
        QCOMPARE(chats->item(0)->data(workspace::Title).toString(), QStringLiteral("Найденный чат"));
        QCOMPARE(chats->item(0)->data(workspace::Detail).toString(), QStringLiteral("Из поиска"));

        auto *messageSearch = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск по сообщениям"));
        QVERIFY(messageSearch);
        auto *showMessageSearch = findAccessible<QPushButton>(&window, QStringLiteral("Найти в чате"));
        QVERIFY(showMessageSearch);
        QVERIFY(!messageSearch->isVisible());
        QTest::mouseClick(showMessageSearch, Qt::LeftButton);
        QTRY_VERIFY(messageSearch->isVisible() && messageSearch->hasFocus());
        messageSearch->setText(QStringLiteral("Привет"));
        QTest::keyClick(messageSearch, Qt::Key_Return);
        QCOMPARE(backend->searchedScope, QStringLiteral("c-1"));
        QCOMPARE(backend->searchedQuery, QStringLiteral("Привет"));
        QCOMPARE(backend->searchedTask, false);

        Message hit;
        hit.id = QStringLiteral("m-9");
        hit.author = QStringLiteral("Иван");
        hit.text = QStringLiteral("Привет [B]из поиска[/B]");
        emit backend->messagesFound(QStringLiteral("c-1"), QStringLiteral("Привет"), false,
                                    QVector<Message>{hit});
        QTRY_COMPARE(messages->count(), 1);

        Message push;
        push.id = QStringLiteral("m-10");
        push.text = QStringLiteral("Свежее сообщение");
        emit backend->chatMessagesLoaded(QStringLiteral("c-1"), QVector<Message>{push, push});
        QTest::qWait(60);
        QCOMPARE(messages->count(), 1);
        QCOMPARE(messages->item(0)->data(workspace::Detail).toString(), QStringLiteral("Привет из поиска"));

        auto *clear = findAccessible<QPushButton>(&window, QStringLiteral("Очистить поиск по сообщениям"));
        QVERIFY(clear);
        clear->click();
        QTRY_COMPARE(messages->count(), 1);
        QTest::mouseClick(showMessageSearch, Qt::LeftButton);
        QVERIFY(!messageSearch->isVisible());
        QCOMPARE(messages->item(0)->data(workspace::Detail).toString(), backend->messages.front().text);

        QTest::mouseClick(showMessageSearch, Qt::LeftButton);
        QTRY_VERIFY(messageSearch->hasFocus());
        messageSearch->setText(QStringLiteral("Нет такого сообщения"));
        QTest::keyClick(messageSearch, Qt::Key_Return);
        const auto query = backend->searchedQuery;
        QCOMPARE(query, QStringLiteral("Нет такого сообщения"));
        emit backend->messagesFound(QStringLiteral("c-1"), query, false, {});
        QTest::keyClick(messageSearch, Qt::Key_Escape);
        QVERIFY(!messageSearch->isVisible());
        QVERIFY(showMessageSearch->hasFocus());
        QCOMPARE(messages->item(0)->data(workspace::Detail).toString(), backend->messages.front().text);
    }

    void chatDirectoriesKeepDraftSearchSelectionAndConfirmedPin()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto makeChat = [](const char *id, const QString &title, int unread = 0, bool pinned = false) {
            Chat chat;
            chat.id = QString::fromLatin1(id);
            chat.title = title;
            chat.unread = unread;
            chat.pinned = pinned;
            chat.kind = QStringLiteral("im");
            return chat;
        };
        const QUrl photo(QStringLiteral("https://example.bitrix24.ru/avatar.png"));
        {
            QBuffer buffer(&backend->avatarBytes);
            QVERIFY(buffer.open(QIODevice::WriteOnly));
            QImage image(180, 150, QImage::Format_ARGB32);
            image.fill(QColor("#53938a"));
            QVERIFY(image.save(&buffer, "PNG"));
        }
        const auto regular = makeChat("second", QStringLiteral("Анна Лебедева"));
        auto unread = makeChat("unread", QStringLiteral("Поддержка"), 4);
        unread.avatarUrl = photo;
        auto manuallyUnread = makeChat("marked", QStringLiteral("Документы"));
        manuallyUnread.markedUnread = true;
        backend->chats += {regular, unread, manuallyUnread, makeChat("pinned", QStringLiteral("Команда"), 0, true)};
        auto task = makeChat("task-dialog", QStringLiteral("Починить кран"), 2);
        task.taskChat = true;
        task.taskId = QStringLiteral("501");
        backend->taskChats = {makeChat("another-task", QStringLiteral("Обновить справку")), task};
        backend->taskMessages.first().author = QStringLiteral("Анна Лебедева");
        backend->taskMessages.first().authorAvatarUrl = photo;
        backend->taskMessages.first().text = QStringLiteral("Смета уже у поставщика, можно начинать.");
        backend->taskMessages.first().forwarded = true;
        backend->taskMessages.first().forwardedAuthor = QStringLiteral("Иван Соколов");
        Message own;
        own.id = QStringLiteral("my-message");
        own.outgoing = true;
        own.text = QStringLiteral("Начинаем сегодня.");
        own.replyId = backend->taskMessages.first().id;
        backend->taskMessages.append(own);
        backend->projects.first().avatarUrl = photo;
        Project pinnedProject;
        pinnedProject.id = QStringLiteral("102");
        pinnedProject.name = QStringLiteral("Поддержка");
        pinnedProject.pinned = true;
        backend->projects.append(pinnedProject);
        MainWindow window(backend.get());
        window.resize(720, 500);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("chatScopeTabs"));
        auto *list = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
        auto *search = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск чатов"));
        auto *input = findAccessible<QPlainTextEdit>(&window, QStringLiteral("Текст сообщения"));
        QVERIFY(tabs);
        QVERIFY(list);
        QVERIFY(search);
        QVERIFY(input);
        const auto idAt = [](QListWidget *rows, int row) {
            const auto *item = rows->item(row);
            return item ? item->data(workspace::Id).toString() : QString();
        };
        QCOMPARE(idAt(list, 0), QStringLiteral("pinned"));
        QCOMPARE(idAt(list, 1), unread.id);
        QCOMPARE(idAt(list, 2), manuallyUnread.id);
        QVERIFY(list->item(2)->data(workspace::MarkedUnread).toBool());
        QTRY_VERIFY(!list->item(1)->data(workspace::AvatarImage).value<QPixmap>().isNull());
        QCOMPARE(backend->avatarRequests.count(photo), 1);
        list->setCurrentRow(4);
        QCOMPARE(idAt(list, 4), regular.id);
        input->setPlainText(QStringLiteral("Обычный черновик"));
        search->setText(QStringLiteral("Анна"));
        QTest::keyClick(search, Qt::Key_Return);
        emit backend->chatsFound(QStringLiteral("Анна"), {regular});
        tabs->setCurrentIndex(1);
        QVERIFY(search->text().isEmpty());
        QCOMPARE(idAt(list, 0), task.id);
        QCOMPARE(list->item(0)->data(workspace::Title).toString(), task.title);
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        QCOMPARE(history->item(1)->data(workspace::ReplyPreview).toString(), backend->taskMessages.first().text);
        QVERIFY(input->toPlainText().isEmpty());
        input->setPlainText(QStringLiteral("Черновик задачи"));
        search->setText(QStringLiteral("кран"));
        QTest::keyClick(search, Qt::Key_Return);
        QCOMPARE(backend->searchedTaskChats, QStringLiteral("кран"));
        emit backend->chatsFound(QStringLiteral("Анна"), {regular});
        emit backend->taskChatsFound(QStringLiteral("кран"), {task});
        QCOMPARE(idAt(list, 0), task.id);
        auto *openTask = findAccessible<QPushButton>(&window, QStringLiteral("Открыть карточку задачи"));
        QVERIFY(openTask && openTask->isVisible());
        openTask->click();
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        card->close();
        tabs->setCurrentIndex(0);
        QCOMPARE(search->text(), QStringLiteral("Анна"));
        QCOMPARE(idAt(list, 0), regular.id);
        QCOMPARE(input->toPlainText(), QStringLiteral("Обычный черновик"));
        QVERIFY(!openTask->isVisible());
        tabs->setCurrentIndex(1);
        QCOMPARE(search->text(), QStringLiteral("кран"));
        QCOMPARE(input->toPlainText(), QStringLiteral("Черновик задачи"));
        auto *clear = findAccessible<QPushButton>(&window, QStringLiteral("Очистить поиск чатов"));
        QVERIFY(clear);
        clear->click();

        auto openPin = [](QListWidget *rows, int row, const QString &label) {
            const auto point = rows->visualItemRect(rows->item(row)).center();
            emit rows->customContextMenuRequested(point);
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            QVERIFY(menu);
            QCOMPARE(menu->actions().front()->text(), label);
            menu->actions().front()->trigger();
            menu->close();
        };
        openPin(list, 1, QStringLiteral("Закрепить"));
        QCOMPARE(backend->pinTarget, QStringLiteral("another-task"));
        QVERIFY(backend->pinValue);
        QCOMPARE(idAt(list, 0), task.id);
        emit backend->chatPinChanged(backend->pinTarget, true);
        QCOMPARE(idAt(list, 0), QStringLiteral("another-task"));
        QCOMPARE(list->currentItem()->data(workspace::Id).toString(), task.id);
        QCOMPARE(input->toPlainText(), QStringLiteral("Черновик задачи"));
        const auto capture = qEnvironmentVariable("B24_DIRECTORY_COMPACT_SCREENSHOT");
        if (!capture.isEmpty()) {
            QCoreApplication::processEvents();
            QVERIFY(window.grab().save(capture));
        }

        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        sections->setCurrentRow(1);
        auto *projects = findAccessible<QListWidget>(&window, QStringLiteral("Проекты"));
        QVERIFY(projects);
        QCOMPARE(idAt(projects, 0), pinnedProject.id);
        QTRY_VERIFY(!projects->item(1)->data(workspace::AvatarImage).value<QPixmap>().isNull());
        QCOMPARE(backend->avatarRequests.count(photo), 1);
        const auto projectCapture = qEnvironmentVariable("B24_PROJECT_AVATAR_SCREENSHOT");
        if (!projectCapture.isEmpty()) {
            projects->setCurrentRow(1);
            QCoreApplication::processEvents();
            QVERIFY(window.grab().save(projectCapture));
            projects->setCurrentRow(0);
        }
        openPin(projects, 0, QStringLiteral("Открепить"));
        QCOMPARE(backend->pinTarget, pinnedProject.id);
        QVERIFY(!backend->pinValue);
        emit backend->failed(QStringLiteral("project-pin:102"), QStringLiteral("Ошибка записи"));
        QCOMPARE(idAt(projects, 0), pinnedProject.id);
        openPin(projects, 0, QStringLiteral("Открепить"));
        emit backend->projectPinChanged(pinnedProject.id, false);
        QCOMPARE(idAt(projects, 0), QStringLiteral("101"));
        QCOMPARE(projects->currentItem()->data(workspace::Id).toString(), pinnedProject.id);
    }

    void chatDirectoryWidthPersistsAfterDragging()
    {
        QSettings settings(QStringLiteral("bitrix24-client"), QStringLiteral("bitrix24-client"));
        const auto key = QStringLiteral("ui/chatDirectoryWidth");
        settings.remove(key);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        int savedWidth = 0;
        {
            MainWindow window(backend.get());
            window.resize(960, 650);
            window.show();
            auto *splitter = window.findChild<QSplitter *>(QStringLiteral("chatDirectorySplitter"));
            QVERIFY(splitter);
            QVERIFY(splitter->sizes().first() >= 196);
            auto *handle = splitter->handle(1);
            const auto start = handle->rect().center();
            const int old = splitter->sizes().first();
            QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, start);
            QTest::mouseMove(handle, start + QPoint(63, 0));
            QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier, start + QPoint(63, 0));
            QTRY_VERIFY(splitter->sizes().first() > old + 30);
            savedWidth = splitter->sizes().first();
            QCOMPARE(settings.value(key).toInt(), savedWidth);
        }
        MainWindow restored(backend.get());
        restored.resize(960, 650);
        restored.show();
        auto *splitter = restored.findChild<QSplitter *>(QStringLiteral("chatDirectorySplitter"));
        QVERIFY(splitter);
        QTRY_COMPARE(splitter->sizes().first(), savedWidth);
        restored.resize(720, 500);
        QTRY_VERIFY(splitter->sizes().at(1) >= 325);
        restored.resize(960, 650);
        QTRY_COMPARE(splitter->sizes().first(), savedWidth);
        settings.remove(key);
    }

    void projectDirectoryWidthPersistsIndependentlyFromChat()
    {
        QSettings settings(QStringLiteral("bitrix24-client"), QStringLiteral("bitrix24-client"));
        const auto projectKey = QStringLiteral("ui/projectDirectoryWidth");
        const auto chatKey = QStringLiteral("ui/chatDirectoryWidth");
        settings.remove(projectKey);
        const auto clearWidths = qScopeGuard([&] {
            settings.remove(projectKey);
            settings.remove(chatKey);
        });
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        int chatWidth = 0;
        int savedWidth = 0;
        {
            MainWindow window(backend.get());
            window.resize(960, 650);
            window.show();
            auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
            auto *chats = window.findChild<QSplitter *>(QStringLiteral("chatDirectorySplitter"));
            QVERIFY(sections && chats);
            chatWidth = qMax(320, chats->widget(0)->minimumWidth() + 32);
            settings.setValue(chatKey, chatWidth);
            sections->setCurrentRow(1);
            auto *splitter = window.findChild<QSplitter *>(QStringLiteral("projectDirectorySplitter"));
            QVERIFY(splitter);
            QTRY_VERIFY(splitter->isVisible());
            const int old = splitter->sizes().first();
            QVERIFY(old >= 132);
            auto *handle = splitter->handle(1);
            const auto start = handle->rect().center();
            QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, start);
            QTest::mouseMove(handle, start + QPoint(63, 0));
            QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier, start + QPoint(63, 0));
            QTRY_VERIFY(splitter->sizes().first() > old + 30);
            savedWidth = splitter->sizes().first();
            QCOMPARE(settings.value(projectKey).toInt(), savedWidth);
            QCOMPARE(settings.value(chatKey).toInt(), chatWidth);
        }
        MainWindow restored(backend.get());
        const auto wideWindow = qMax(960, chatWidth + 550);
        restored.resize(wideWindow, 650);
        restored.show();
        auto *sections = findAccessible<QListWidget>(&restored, QStringLiteral("Разделы"));
        auto *splitter = restored.findChild<QSplitter *>(QStringLiteral("projectDirectorySplitter"));
        auto *chats = restored.findChild<QSplitter *>(QStringLiteral("chatDirectorySplitter"));
        QVERIFY(sections && splitter && chats);
        QTRY_COMPARE(chats->sizes().first(), chatWidth);
        sections->setCurrentRow(1);
        QTRY_COMPARE(splitter->sizes().first(), savedWidth);
        restored.resize(720, 500);
        QTRY_VERIFY(splitter->sizes().at(1) >= 430);
        restored.resize(wideWindow, 650);
        QTRY_COMPARE(splitter->sizes().first(), savedWidth);
    }

    void directoryWidthSurvivesRestartAtManualWidth_data()
    {
        QTest::addColumn<QString>("key");
        QTest::addColumn<QString>("splitterName");
        QTest::addColumn<int>("section");
        QTest::addColumn<int>("bodyMinimum");
        QTest::newRow("chats") << QStringLiteral("ui/chatDirectoryWidth")
            << QStringLiteral("chatDirectorySplitter") << 0 << 325;
        QTest::newRow("projects") << QStringLiteral("ui/projectDirectoryWidth")
            << QStringLiteral("projectDirectorySplitter") << 1 << 430;
    }

    void directoryWidthSurvivesRestartAtManualWidth()
    {
        QFETCH(QString, key);
        QFETCH(QString, splitterName);
        QFETCH(int, section);
        QFETCH(int, bodyMinimum);
        QSettings settings(QStringLiteral("bitrix24-client"), QStringLiteral("bitrix24-client"));
        settings.remove(key);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        int savedWidth = 0;
        {
            MainWindow window(backend.get());
            window.resize(1750, 700);
            window.show();
            auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
            auto *splitter = window.findChild<QSplitter *>(splitterName);
            QVERIFY(sections && splitter);
            sections->setCurrentRow(section);
            QTRY_VERIFY(splitter->isVisible());
            auto *handle = splitter->handle(1);
            QVERIFY(handle);
            const auto start = handle->rect().center();
            QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, start);
            QTest::mouseMove(handle, start + QPoint(880, 0));
            QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier, start + QPoint(880, 0));
            QTRY_VERIFY(splitter->widget(0)->width() > 1000);
            savedWidth = splitter->widget(0)->width();
            QCOMPARE(settings.value(key).toInt(), savedWidth);
        }
        {
            MainWindow restored(backend.get());
            restored.resize(1750, 700);
            restored.show();
            auto *sections = findAccessible<QListWidget>(&restored, QStringLiteral("Разделы"));
            auto *splitter = restored.findChild<QSplitter *>(splitterName);
            QVERIFY(sections && splitter);
            sections->setCurrentRow(section);
            QTRY_COMPARE(splitter->widget(0)->width(), savedWidth);

            restored.resize(900, 600);
            QTRY_VERIFY(splitter->widget(0)->width() < savedWidth);
            QTRY_VERIFY(splitter->widget(1)->width() >= bodyMinimum);
            QCOMPARE(settings.value(key).toInt(), savedWidth);

            sections->setCurrentRow(1 - section);
            restored.resize(1750, 700);
            sections->setCurrentRow(section);
            QTRY_COMPARE(splitter->widget(0)->width(), savedWidth);
        }
        settings.remove(key);
    }

    void periodicUpdateStaysLocalAcrossPortalSnapshots()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdNotifications = true;
        backend->accountScope = QStringLiteral("fixture/first-account");
        ClientUpdateFeed feed;
        MainWindow window(backend.get());
        window.setClientUpdateFeed(&feed);
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        auto *notices = findAccessible<QListWidget>(&window, QStringLiteral("Уведомления"));
        auto *search = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск уведомлений"));
        auto *read = findAccessible<QPushButton>(&window, QStringLiteral("Прочитать уведомление"));
        QVERIFY(sections && notices && search && read);
        const QUrl url(QStringLiteral("https://github.com/lliyplliuk/b24-client/releases/download/v1.2.3/bitrix24-client-v1.2.3-windows-x64.zip"));
        feed.publish(QStringLiteral("v1.2.3"), url);
        QVERIFY(window.isHidden());
        QCOMPARE(sections->currentRow(), 0);
        QVERIFY(sections->item(2)->data(Qt::AccessibleDescriptionRole).toString().contains(QStringLiteral("1")));
        window.show();
        sections->setCurrentRow(2);
        emit backend->notificationsLoaded({});
        QCOMPARE(notices->count(), 1);
        QVERIFY(notices->item(0)->data(workspace::MarkupHtml).toString().contains(url.toString()));
        search->setText(QStringLiteral("1.2.3"));
        QTest::keyClick(search, Qt::Key_Return);
        emit backend->notificationsFound(search->text(), {});
        QCOMPARE(notices->count(), 1);
        notices->setCurrentRow(0);
        read->click();
        QVERIFY(feed.notice().read);
        QVERIFY(backend->noticeReadId.isEmpty());
        QVERIFY(backend->noticeReadBatches.isEmpty());
        feed.publish(QStringLiteral("v1.2.3"), url);
        QVERIFY(feed.notice().read);
        search->setText(QStringLiteral("другое"));
        QTest::keyClick(search, Qt::Key_Return);
        emit backend->notificationsFound(search->text(), {});
        QCOMPARE(notices->count(), 0);
        backend->accountScope = QStringLiteral("fixture/another-account");
        emit backend->accountScopeChanged();
        QCOMPARE(notices->count(), 1);
        QVERIFY(!notices->item(0)->data(workspace::Count).toBool());
        QVERIFY(sections->item(2)->data(Qt::AccessibleDescriptionRole).toString().isEmpty());
        QApplication::setActiveWindow(&window);
        window.onConnectionChanged(true);
        feed.publish(QStringLiteral("v1.2.4"), QUrl(url.toString().replace(QStringLiteral("1.2.3"), QStringLiteral("1.2.4"))));
        QTRY_VERIFY(feed.notice().read);
        QCOMPARE(notices->count(), 1);
        QVERIFY(backend->noticeReadBatches.isEmpty());
    }

    void notificationFiltersKeepPendingAndFailedHistoryVisible()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdNotifications = true;
        MainWindow window(backend.get());
        window.show();
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        auto *status = findAccessible<QLabel>(&window, QStringLiteral("Состояние уведомлений"));
        auto *search = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск уведомлений"));
        auto *filter = findAccessible<QCheckBox>(&window, QStringLiteral("Только непрочитанные уведомления"));
        auto *sort = findAccessible<QComboBox>(&window, QStringLiteral("Порядок уведомлений"));
        QVERIFY(sections && status && search && filter && sort);
        sections->setCurrentRow(2);
        filter->setChecked(true);
        sort->setCurrentIndex(1);
        QVERIFY(status->isVisible());
        QCOMPARE(status->text(), QStringLiteral("Загружаем уведомления…"));
        auto *indicator = status->findChild<QWidget *>(QStringLiteral("sharedLoadingIndicator"));
        QVERIFY(indicator && indicator->isVisible());

        search->setText(QStringLiteral("задача"));
        QTest::keyClick(search, Qt::Key_Return);
        QCOMPARE(status->text(), QStringLiteral("Ищем уведомления…"));
        emit backend->failed(QStringLiteral("notifications"), QStringLiteral("Нет ответа"));
        QCOMPARE(status->text(), QStringLiteral("Ищем уведомления…"));
        QVERIFY(indicator->isVisible());
        search->clear();
        QTest::keyClick(search, Qt::Key_Return);
        QCOMPARE(status->text(), QStringLiteral("Не удалось загрузить уведомления"));
        QVERIFY(indicator->isHidden());
        sort->setCurrentIndex(0);
        QCOMPARE(status->text(), QStringLiteral("Не удалось загрузить уведомления"));

        emit backend->notificationsLoaded({});
        QCOMPARE(status->text(), QStringLiteral("Непрочитанных уведомлений нет"));
        QVERIFY(indicator->isHidden());
    }

    void rendersNoticeBodyLinksAndKeepsDistinctTitle()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdLinkPreviews = true;
        Notification structured;
        structured.id = QStringLiteral("801");
        structured.title = QStringLiteral("Уведомление Битрикс24");
        structured.text = QStringLiteral("Заявка на печать. Задача");
        structured.markup = QStringLiteral("Заявка на печать\n[URL=/workgroups/group/7/tasks/task/view/501/]Задача[/URL]");
        structured.createdAt = QDateTime(QDate(2026, 11, 23), QTime(7, 12));
        Notification titled;
        titled.id = QStringLiteral("800");
        titled.title = QStringLiteral("Регламент работы");
        titled.text = QStringLiteral("Изменён график");
        titled.createdAt = structured.createdAt.addSecs(-60);
        backend->notifications = {structured, titled};
        for (int i = 0; i < 16; ++i) {
            auto archived = titled;
            archived.id = QString::number(799 - i);
            archived.createdAt = titled.createdAt.addSecs(-i - 1);
            if (i == 15)
                archived.markup = QStringLiteral("[URL=/tasks/task/view/999/]Старая задача[/URL]");
            backend->notifications.append(std::move(archived));
        }
        MainWindow window(backend.get(), true);
        window.resize(1000, 750);
        window.show();
        auto *sections = window.findChild<QListWidget *>(QStringLiteral("sections"));
        QVERIFY(sections);
        sections->setCurrentRow(2);
        auto *list = findAccessible<QListWidget>(&window, QStringLiteral("Уведомления"));
        QVERIFY(list);
        QTRY_COMPARE(list->count(), 18);
        QVERIFY(list->item(0)->data(workspace::Title).toString().isEmpty());
        QCOMPARE(list->item(1)->data(workspace::Title).toString(), titled.title);
        QVERIFY(list->item(0)->data(workspace::MarkupHtml).toString().contains(QStringLiteral("href=")));
        QTRY_COMPARE(backend->linkPreviewRequests.size(), 1);
        QCOMPARE(backend->linkPreviewRequests.constFirst().first.path(),
                 QStringLiteral("/workgroups/group/7/tasks/task/view/501/"));
        auto complete = std::move(backend->linkPreviewRequests.first().second);
        const auto withoutPreview = list->visualItemRect(list->item(0)).height();
        const auto withoutPreviewHint = list->sizeHintForRow(0);
        complete({QStringLiteral("Восстановить печать"), {QStringLiteral("Проект · IT")}, MessageLinkPreviewKind::Task});
        QCoreApplication::processEvents();
        list->doItemsLayout();
        QCOMPARE(list->sizeHintForRow(0), withoutPreviewHint);
        QCOMPARE(list->visualItemRect(list->item(0)).height(), withoutPreview);
        QTRY_VERIFY(list->visualItemRect(list->item(1)).top() > list->visualItemRect(list->item(0)).bottom());
        const auto screenshot = qEnvironmentVariable("B24_NOTICE_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(list->viewport()->grab().save(screenshot));
        auto *painter = static_cast<workspace::RowPainter *>(list->itemDelegate());
        const auto index = list->model()->index(0, 0);
        const auto rect = list->visualRect(index);
        QPoint link;
        for (int y = rect.top(); y < rect.bottom() && link.isNull(); ++y)
            for (int x = rect.left(); x < rect.right(); ++x)
                if (!painter->anchorAt(index, rect, QPoint(x, y)).isEmpty()) {
                    link = QPoint(x, y);
                    break;
                }
        QVERIFY(!link.isNull());
        QMouseEvent move(QEvent::MouseMove, link, list->viewport()->mapToGlobal(link),
                         Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(list->viewport(), &move);
        QCOMPARE(list->viewport()->cursor().shape(), Qt::PointingHandCursor);
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, link);
        QTRY_VERIFY(window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        QVERIFY(backend->noticeReadBatches.isEmpty());
    }

    void marksOnlyVisibleNotificationsAndWaitsForTheNextScroll()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        for (int id = 950; id > 934; --id) {
            Notification notice;
            notice.id = QString::number(id);
            notice.title = QStringLiteral("Событие");
            notice.text = QString(5, QLatin1Char('\n')) + QStringLiteral("Подробности");
            backend->notifications.append(notice);
        }
        MainWindow window(backend.get());
        window.resize(900, 540);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *sections = window.findChild<QListWidget *>(QStringLiteral("sections"));
        auto *filter = findAccessible<QCheckBox>(&window, QStringLiteral("Только непрочитанные уведомления"));
        QVERIFY(sections && filter);
        QVERIFY(backend->noticeReadBatches.isEmpty());
        filter->setChecked(true);
        sections->setCurrentRow(2);
        auto *list = findAccessible<QListWidget>(&window, QStringLiteral("Уведомления"));
        QVERIFY(list);
        QTRY_COMPARE(list->count(), 16);
        QDialog options(&window);
        options.setWindowModality(Qt::WindowModal);
        options.show();
        QApplication::setActiveWindow(&options);
        QTest::qWait(180);
        QVERIFY(backend->noticeReadBatches.isEmpty());
        options.close();
        QApplication::setActiveWindow(&window);
        list->verticalScrollBar()->setValue(1);
        list->verticalScrollBar()->setValue(0);
        QTRY_COMPARE(backend->noticeReadBatches.size(), 1);
        const auto first = backend->noticeReadBatches.constFirst();
        QVERIFY(!first.isEmpty());
        QVERIFY(first.contains(QStringLiteral("950")));
        QVERIFY(!first.contains(QStringLiteral("935")));
        for (const auto &id : first) {
            const auto row = 950 - id.toInt();
            QVERIFY(row >= 0 && row < list->count());
            QVERIFY(list->visualItemRect(list->item(row)).intersects(list->viewport()->rect()));
        }
        emit backend->notificationReadStateChanged(first, true);
        emit backend->notificationActionFinished(QStringLiteral("read-notifications"));
        QTRY_COMPARE(list->count(), 16 - first.size());
        QCOMPARE(list->indexAt(QPoint(4, 4)).data(workspace::Id).toString(), QString::number(950 - first.size()));
        QTest::qWait(180);
        QCOMPARE(backend->noticeReadBatches.size(), 1);

        list->verticalScrollBar()->setValue(list->verticalScrollBar()->maximum());
        QTRY_COMPARE(backend->noticeReadBatches.size(), 2);
        QVERIFY(backend->noticeReadBatches.last().contains(QStringLiteral("935")));
        emit backend->failed(QStringLiteral("read-notifications"), QStringLiteral("Ошибка фикстуры"));
        QTest::qWait(180);
        QCOMPARE(backend->noticeReadBatches.size(), 2);
    }

    void ordinaryChatPopupsDoNotAppearInEventsOrSearch()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->chats.first().unread = 4;
        Notification ordinary;
        ordinary.id = QStringLiteral("push:ordinary-message");
        ordinary.title = QStringLiteral("Техническая поддержка");
        ordinary.text = QStringLiteral("Событие разговора с поддержкой");
        ordinary.targets = {{NotificationTargetKind::Chat, QStringLiteral("chat95"), QStringLiteral("801")},
                            {NotificationTargetKind::Task, QStringLiteral("501")}};
        ordinary.ordinaryChatMessage = true;
        Notification task;
        task.id = QStringLiteral("message:802");
        task.title = QStringLiteral("Чат задачи");
        task.text = QStringLiteral("Событие чата задачи");
        task.targets = {{NotificationTargetKind::Chat, QStringLiteral("chat96"), QStringLiteral("802"), {}, {}, true}};
        Notification notice;
        notice.id = QStringLiteral("803");
        notice.type = QStringLiteral("im");
        notice.title = QStringLiteral("Уведомление портала");
        notice.text = QStringLiteral("Событие портала");
        notice.targets = {ordinary.targets.first()};
        backend->notifications = {ordinary, task, notice};
        MainWindow window(backend.get());
        window.resize(900, 700);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *sections = window.findChild<QListWidget *>(QStringLiteral("sections"));
        auto *list = findAccessible<QListWidget>(&window, QStringLiteral("Уведомления"));
        auto *search = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск уведомлений"));
        QVERIFY(sections && list && search);
        QTRY_COMPARE(sections->item(0)->data(ui::SectionUnreadCount).toLongLong(), 4);
        QTRY_COMPARE(sections->item(2)->data(ui::SectionUnreadCount).toLongLong(), 2);
        sections->setCurrentRow(2);
        const auto listedIds = [list] {
            QStringList ids;
            for (int row = 0; row < list->count(); ++row)
                ids.append(list->item(row)->data(workspace::Id).toString());
            ids.sort();
            return ids;
        };
        const QStringList expected{notice.id, task.id};
        QCOMPARE(listedIds(), expected);
        QTRY_COMPARE(backend->noticeReadBatches.size(), 1);
        auto read = backend->noticeReadBatches.first();
        read.sort();
        QCOMPARE(read, expected);

        search->setText(QStringLiteral("т"));
        QTRY_COMPARE(listedIds(), expected);
        search->setText(QStringLiteral("событие"));
        QTRY_COMPARE(backend->noticeQuery, search->text());
        emit backend->notificationsFound(search->text(), {ordinary, task, notice});
        QCOMPARE(listedIds(), expected);
        emit backend->notificationsFound(search->text(), {ordinary});
        QVERIFY(listedIds().isEmpty());
        QCOMPARE(sections->item(2)->data(ui::SectionUnreadCount).toLongLong(), 2);
    }

    void marksVisibleSyntheticNoticeThroughTheSameViewportBatch()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        Notification synthetic;
        synthetic.id = QStringLiteral("message:951");
        synthetic.text = QStringLiteral("Новое событие в чате задачи");
        synthetic.createdAt = QDateTime::currentDateTime().addDays(1);
        backend->notifications = {synthetic};
        MainWindow window(backend.get());
        window.resize(900, 540);
        window.show();
        QApplication::setActiveWindow(&window);
        window.findChild<QListWidget *>(QStringLiteral("sections"))->setCurrentRow(2);
        QTRY_COMPARE(backend->noticeReadBatches.size(), 1);
        QCOMPARE(backend->noticeReadBatches.first(), QStringList{synthetic.id});
    }

    void resetsVisibleNoticesWhenTheAccountChanges()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->accountScope = QStringLiteral("fixture/first-account");
        Notification oldNotice;
        oldNotice.id = QStringLiteral("971");
        oldNotice.text = QStringLiteral("Событие первого аккаунта");
        backend->notifications = {oldNotice};
        MainWindow window(backend.get());
        window.show();
        QApplication::setActiveWindow(&window);
        auto *sections = window.findChild<QListWidget *>(QStringLiteral("sections"));
        auto *list = findAccessible<QListWidget>(&window, QStringLiteral("Уведомления"));
        auto *search = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск уведомлений"));
        QVERIFY(sections && list && search);
        sections->setCurrentRow(2);
        QTRY_COMPARE(backend->noticeReadBatches, QVector<QStringList>{{oldNotice.id}});
        search->setText(QStringLiteral("старый запрос"));

        backend->holdNotifications = true;
        backend->accountScope = QStringLiteral("fixture/second-account");
        emit backend->accountScopeChanged();
        QCOMPARE(list->count(), 0);
        QVERIFY(search->text().isEmpty());
        QVERIFY(sections->item(2)->data(Qt::AccessibleDescriptionRole).toString().isEmpty());
        QTRY_COMPARE(backend->noticeHistoryRequests, 2);
        Notification newNotice;
        newNotice.id = QStringLiteral("972");
        newNotice.text = QStringLiteral("Событие второго аккаунта");
        emit backend->notificationsLoaded({newNotice});
        QTRY_COMPARE(backend->noticeReadBatches.size(), 2);
        QCOMPARE(backend->noticeReadBatches.last(), QStringList{newNotice.id});
        QVERIFY(backend->noticeQuery.isEmpty());
    }

    void loadsEarlierNotificationsOnlyOnExplicitRequest()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->accountScope = QStringLiteral("fixture/first-account");
        const auto newest = QDateTime::currentDateTimeUtc();
        for (int id = 970; id >= 955; --id) {
            Notification item;
            item.id = QString::number(id);
            item.text = QStringLiteral("Задача %1").arg(id);
            item.createdAt = newest.addSecs(id - 970);
            item.read = id == 970;
            backend->notifications.append(item);
        }
        MainWindow window(backend.get(), true);
        window.resize(780, 500);
        window.show();
        findAccessible<QListWidget>(&window, QStringLiteral("Разделы"))->setCurrentRow(2);
        auto *list = findAccessible<QListWidget>(&window, QStringLiteral("Уведомления"));
        auto *search = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск уведомлений"));
        auto *filter = findAccessible<QCheckBox>(&window, QStringLiteral("Только непрочитанные уведомления"));
        auto *sort = findAccessible<QComboBox>(&window, QStringLiteral("Порядок уведомлений"));
        auto *older = findAccessible<QPushButton>(&window, QStringLiteral("Более ранние уведомления"));
        QVERIFY(list && search && filter && sort && older);
        QTRY_COMPARE(list->count(), 16);
        QVERIFY(!older->isVisible());
        emit backend->notificationHistoryPageStateChanged(true, false);
        QVERIFY(older->isVisible() && older->isEnabled());
        window.onConnectionChanged(false);
        QVERIFY(!older->isVisible());
        window.onConnectionChanged(true);
        QVERIFY(older->isVisible() && older->isEnabled());
        search->setText(QStringLiteral("задача"));
        QVERIFY(!older->isVisible());
        QTRY_COMPARE(backend->noticeQuery, search->text());
        search->clear();
        QTRY_VERIFY(older->isVisible() && older->isEnabled());
        filter->setChecked(true);
        sort->setCurrentIndex(1);
        QCOMPARE(list->count(), 15);
        list->scrollToItem(list->item(5), QAbstractItemView::PositionAtTop);
        const auto top = list->indexAt(QPoint(4, 0)).data(workspace::Id).toString();
        QVERIFY(!top.isEmpty());
        QCOMPARE(backend->olderNoticeRequests, 0);
        older->click();
        QCOMPARE(backend->olderNoticeRequests, 1);
        QVERIFY(older->isVisible() && !older->isEnabled());
        older->click();
        QCOMPARE(backend->olderNoticeRequests, 1);

        Notification earlier;
        earlier.id = QStringLiteral("954");
        earlier.text = QStringLiteral("Ранняя задача");
        earlier.createdAt = newest.addSecs(-16);
        backend->notifications.append(earlier);
        emit backend->notificationsLoaded(backend->notifications);
        QCOMPARE(list->count(), 16);
        QCOMPARE(list->item(0)->data(workspace::Id).toString(), earlier.id);
        QCOMPARE(list->indexAt(QPoint(4, 0)).data(workspace::Id).toString(), top);
        QVERIFY(filter->isChecked());
        QCOMPARE(sort->currentIndex(), 1);
        emit backend->notificationHistoryPageStateChanged(false, false);
        QVERIFY(!older->isVisible());
        emit backend->notificationHistoryPageStateChanged(true, false);
        QVERIFY(older->isVisible());
        backend->notifications.clear();
        backend->accountScope = QStringLiteral("fixture/second-account");
        emit backend->accountScopeChanged();
        QVERIFY(!older->isVisible());
        QCOMPARE(backend->olderNoticeRequests, 1);
        QVERIFY(backend->noticeReadBatches.isEmpty());
    }

    void notificationActionsWaitForAcknowledgement()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        Notification old;
        old.id = QStringLiteral("51");
        old.title = QStringLiteral("Старая задача");
        old.createdAt = QDateTime::currentDateTimeUtc().addSecs(-60);
        old.read = true;
        auto recent = old;
        recent.id = QStringLiteral("52");
        recent.title = QStringLiteral("Новая задача");
        recent.createdAt = old.createdAt.addSecs(30);
        recent.read = false;
        backend->notifications = {old, recent};
        MainWindow window(backend.get());
        window.show();
        findAccessible<QListWidget>(&window, QStringLiteral("Разделы"))->setCurrentRow(2);
        auto *rows = findAccessible<QListWidget>(&window, QStringLiteral("Уведомления"));
        auto *search = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск уведомлений"));
        auto *filter = findAccessible<QCheckBox>(&window, QStringLiteral("Только непрочитанные уведомления"));
        auto *sort = findAccessible<QComboBox>(&window, QStringLiteral("Порядок уведомлений"));
        auto *read = findAccessible<QPushButton>(&window, QStringLiteral("Прочитать уведомление"));
        auto *readAll = findAccessible<QPushButton>(&window, QStringLiteral("Прочитать все уведомления"));
        auto *remove = findAccessible<QPushButton>(&window, QStringLiteral("Удалить уведомление"));
        QVERIFY(rows && search && filter && sort && read && readAll && remove);
        QCOMPARE(rows->count(), 2);
        rows->setCurrentRow(0);
        QCOMPARE(rows->currentItem()->data(workspace::Id).toString(), recent.id);
        sort->setCurrentIndex(1);
        QCOMPARE(rows->currentRow(), 1);
        filter->setChecked(true);
        QCOMPARE(rows->count(), 1);
        search->setText(QStringLiteral("задача"));
        QTRY_COMPARE(backend->noticeQuery, search->text());
        emit backend->notificationsFound(QStringLiteral("другой запрос"), {recent});
        QCOMPARE(rows->count(), 0);
        emit backend->notificationsFound(search->text(), {old, recent});
        QCOMPARE(rows->count(), 1);
        QVERIFY(backend->noticeReadId.isEmpty());
        read->click();
        QCOMPARE(backend->noticeReadId, recent.id);
        QCOMPARE(rows->count(), 1);
        QVERIFY(!remove->isEnabled());
        emit backend->failed(QStringLiteral("read-notification"), QStringLiteral("Не удалось"));
        QVERIFY(read->isEnabled());
        read->click();
        emit backend->notificationReadStateChanged({recent.id}, true);
        emit backend->notificationActionFinished(QStringLiteral("read-notification"));
        QCOMPARE(rows->count(), 0);
        filter->setChecked(false);
        QCOMPARE(rows->currentItem()->data(workspace::Id).toString(), recent.id);
        readAll->click();
        QCOMPARE(backend->noticeReadAll, 1);
        emit backend->notificationActionFinished(QStringLiteral("read-all-notifications"));
        QTimer::singleShot(0, &window, [] {
            auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (dialog)
                dialog->button(QMessageBox::Yes)->click();
        });
        remove->click();
        QCOMPARE(backend->noticeDeleteId, recent.id);
        QCOMPARE(rows->count(), 2);
        emit backend->notificationRemoved(recent.id);
        emit backend->notificationActionFinished(QStringLiteral("delete-notification"));
        QCOMPARE(rows->count(), 1);
    }

    void notificationSettingsKeepPendingWriteAcrossSnapshots()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        const auto muted = QStringLiteral("soundMuted");
        backend->noticeSettings = {{muted, false}};
        MainWindow window(backend.get());
        window.show();
        emit backend->connectionChanged(true);
        findAccessible<QListWidget>(&window, QStringLiteral("Разделы"))->setCurrentRow(2);
        auto *settings = findAccessible<QPushButton>(&window, QStringLiteral("Настройки уведомлений портала"));
        QVERIFY(settings);
        settings->click();
        auto *save = findAccessible<QPushButton>(&window, QStringLiteral("Сохранить настройку «Без звука»"));
        QVERIFY(save);
        auto *toggle = save->parentWidget()->findChild<QCheckBox *>();
        QVERIFY(toggle);
        toggle->setChecked(true);
        save->click();
        QCOMPARE(backend->noticeSettingWrites.size(), 1);
        emit backend->notificationSettingsLoaded({{muted, false}});
        QVERIFY(toggle->isChecked());
        save->click();
        QCOMPARE(backend->noticeSettingWrites.size(), 1);
        emit backend->notificationSettingsLoaded({{muted, true}}, muted);
        toggle->setChecked(false);
        save->click();
        QCOMPARE(backend->noticeSettingWrites.size(), 2);
        QCOMPARE(backend->noticeSettingWrites.last().second, 0);
    }

    void notificationSettingsOfferRecoveryAndAreOmittedInDemo()
    {
        const auto caption = QStringLiteral("Настройки уведомлений портала");
        DemoBackend demo;
        MainWindow demoWindow(&demo);
        demoWindow.show();
        findAccessible<QListWidget>(&demoWindow, QStringLiteral("Разделы"))->setCurrentRow(2);
        auto *demoSettings = findAccessible<QPushButton>(&demoWindow, caption);
        QVERIFY(!demoSettings || !demoSettings->isVisible());
        demoWindow.hide();

        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        backend->noticeSettingsReadFails = true;
        backend->noticeSettings = {{QStringLiteral("soundMuted"), false}};
        MainWindow window(backend.get());
        window.show();
        emit backend->connectionChanged(true);
        findAccessible<QListWidget>(&window, QStringLiteral("Разделы"))->setCurrentRow(2);
        auto *settings = findAccessible<QPushButton>(&window, caption);
        QVERIFY(settings && settings->isVisible());
        settings->click();
        QCOMPARE(backend->noticeSettingsReads, 1);
        QPointer<QDialog> dialog = window.findChild<QDialog *>(QStringLiteral("notificationSettingsDialog"));
        QVERIFY(dialog && dialog->isVisible());
        auto *buttons = dialog->findChild<QDialogButtonBox *>();
        QVERIFY(buttons);
        const auto *close = buttons->button(QDialogButtonBox::Close);
        QVERIFY(close && close->text() == QStringLiteral("Закрыть"));
        auto *retry = findAccessible<QPushButton>(dialog, QStringLiteral("Повторить загрузку настроек"));
        QVERIFY(retry && retry->isVisible());
        const auto *workspaceStatus = findAccessible<QLabel>(&window, QStringLiteral("Состояние"));
        QVERIFY(workspaceStatus && !workspaceStatus->isVisible());
        backend->noticeSettingsReadFails = false;
        retry->click();
        QCOMPARE(backend->noticeSettingsReads, 2);
        QVERIFY(!retry->isVisible());
        const auto *save = findAccessible<QPushButton>(dialog, QStringLiteral("Сохранить настройку «Без звука»"));
        QVERIFY(save && save->isVisible() && save->isEnabled());
        QTest::keyClick(dialog, Qt::Key_Escape);
        QTRY_VERIFY(!dialog || !dialog->isVisible());
    }

    void noticesOpenTypedObjectsAndContext()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto makeNotice = [](QString id, NotificationTarget target) {
            Notification notice;
            notice.id = id;
            notice.title = QStringLiteral("Событие в рабочей группе");
            notice.text = QStringLiteral("[URL=https://example.invalid/test]Текст события[/URL]");
            notice.targets = {target};
            return notice;
        };
        backend->notifications = {
            makeNotice(QStringLiteral("n-1"), {NotificationTargetKind::Chat, QStringLiteral("chat42"),
                QStringLiteral("42"), {}, QStringLiteral("Открыть сообщение"), true}),
            makeNotice(QStringLiteral("n-2"), {NotificationTargetKind::Task, QStringLiteral("501"),
                {}, QStringLiteral("101"), QStringLiteral("Открыть задачу")}),
            makeNotice(QStringLiteral("n-3"), {NotificationTargetKind::Person, QStringLiteral("19"),
                {}, {}, QStringLiteral("Анна Лебедева")}),
            makeNotice(QStringLiteral("n-4"), {NotificationTargetKind::Project, QStringLiteral("101"),
                {}, {}, QStringLiteral("Открыть проект")}),
            makeNotice(QStringLiteral("n-5"), {NotificationTargetKind::Chat, QStringLiteral("chat42"),
                {}, {}, QStringLiteral("Открыть чат"), true}),
        };
        backend->notifications.first().text += QStringLiteral(" [URL=javascript:alert(1)]Небезопасная ссылка[/URL]");
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        auto *notices = findAccessible<QListWidget>(&window, QStringLiteral("Уведомления"));
        QVERIFY(sections);
        QVERIFY(notices);
        sections->setCurrentRow(2);
        QTRY_COMPARE(notices->count(), 5);
        const auto *firstNotice = notices->item(0);
        const auto html = firstNotice->data(workspace::MarkupHtml).toString();
        QVERIFY(html.contains(QStringLiteral("href=\"https://example.invalid/test\"")));
        QVERIFY(!html.contains(QStringLiteral("href=\"javascript:")));
        QCOMPARE(firstNotice->data(workspace::MessageLinks).toStringList(),
                 QStringList{QStringLiteral("https://example.invalid/test")});
        const auto capture = qEnvironmentVariable("B24_NOTICE_OBJECT_SCREENSHOT");
        if (!capture.isEmpty())
            QVERIFY(window.grab().save(capture));
        const auto first = notices->visualItemRect(notices->item(0));
        QTest::mouseClick(notices->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(first.left() + 50, first.bottom() - 20));
        QTRY_COMPARE(backend->contextDialog, QStringLiteral("chat42"));
        QCOMPARE(backend->contextMessage, QStringLiteral("42"));
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Чаты задач"));
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(chats && chats->currentItem());
        QVERIFY(history);
        QCOMPARE(chats->currentItem()->data(workspace::Id).toString(), QStringLiteral("chat42"));
        Message stale;
        stale.id = QStringLiteral("98");
        emit backend->messageContextLoaded(QStringLiteral("chat42"), stale.id, {stale});
        QCOMPARE(history->count(), 0);
        Message wanted;
        wanted.id = QStringLiteral("42");
        wanted.text = QStringLiteral("Сообщение по ссылке");
        emit backend->messageContextLoaded(QStringLiteral("chat42"), wanted.id, {wanted});
        QVERIFY(history->currentItem());
        QCOMPARE(history->currentItem()->data(workspace::Id).toString(), wanted.id);
        emit backend->chatMessagesLoaded(QStringLiteral("chat42"), {stale});
        QCOMPARE(history->currentItem()->data(workspace::Id).toString(), wanted.id);
        QTest::qWait(230);
        QVERIFY(std::none_of(backend->reads.cbegin(), backend->reads.cend(), [](const auto &read) {
            return read.scope == QStringLiteral("chat42") && read.visible.contains(QStringLiteral("42"));
        }));

        auto activateNotice = [&](int row) {
            sections->setCurrentRow(2);
            notices->setCurrentRow(row);
            notices->setFocus();
            QTest::keyClick(notices, Qt::Key_Return);
        };
        auto *searchToggle = findAccessible<QPushButton>(&window, QStringLiteral("Найти в чате"));
        auto *searchField = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск по сообщениям"));
        QVERIFY(searchToggle && searchField);
        searchToggle->click();
        searchField->setText(QStringLiteral("старый запрос"));
        QTest::keyClick(searchField, Qt::Key_Return);
        Message latest;
        latest.id = QStringLiteral("99");
        latest.text = QStringLiteral("Последнее сообщение");
        backend->linkedMessages = {latest};
        activateNotice(4);
        QTRY_COMPARE(sections->currentRow(), 0);
        QTRY_COMPARE(history->count(), 1);
        QCOMPARE(history->item(0)->data(workspace::Id).toString(), latest.id);
        emit backend->messageContextLoaded(QStringLiteral("chat42"), wanted.id, {wanted});
        QCOMPARE(history->item(0)->data(workspace::Id).toString(), latest.id);

        activateNotice(1);
        auto *task = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(task);
        auto *copyLink = findAccessible<QPushButton>(task, QStringLiteral("Скопировать ссылку"));
        QVERIFY(copyLink && copyLink->isEnabled());
        task->close();
        activateNotice(2);
        QCOMPARE(backend->requestedPerson, QStringLiteral("19"));
        auto *person = window.findChild<QDialog *>(QStringLiteral("personProfileDialog"));
        QVERIFY(person);
        QVERIFY(!person->isModal());
        emit backend->personLoaded({QStringLiteral("18"), QStringLiteral("Чужой профиль"), {}, {}});
        auto *name = findAccessible<QLabel>(person, QStringLiteral("Имя сотрудника"));
        QVERIFY(name);
        QCOMPARE(name->text(), QStringLiteral("Загружаем профиль…"));
        emit backend->personLoaded({QStringLiteral("19"), QStringLiteral("Анна Лебедева"),
                                     QStringLiteral("Инженер"), {}});
        QCOMPARE(name->text(), QStringLiteral("Анна Лебедева"));
        person->close();
        activateNotice(3);
        QCOMPARE(sections->currentRow(), 1);
        auto *projects = findAccessible<QListWidget>(&window, QStringLiteral("Проекты"));
        QVERIFY(projects && projects->currentItem());
        QCOMPARE(projects->currentItem()->data(workspace::Id).toString(), QStringLiteral("101"));
    }

    void forwardedVideoShowsPosterAndOpensLocalOriginal_data()
    {
        QTest::addColumn<bool>("clickWhileDownloading");
        QTest::newRow("poster-only-until-click") << false;
        QTest::newRow("click-before-poster") << true;
    }

    void forwardedVideoShowsPosterAndOpensLocalOriginal()
    {
        QFETCH(bool, clickWhileDownloading);
        const auto ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        if (ffmpeg.isEmpty())
            QSKIP("FFmpeg is optional for automatic video posters");
        QTemporaryDir sample;
        QVERIFY(sample.isValid());
        const auto original = sample.filePath(QStringLiteral("sample.mp4"));
        QProcess generate;
        generate.start(ffmpeg, {QStringLiteral("-nostdin"), QStringLiteral("-v"), QStringLiteral("error"),
            QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
            QStringLiteral("color=c=magenta:s=64x48:r=4"), QStringLiteral("-frames:v"), QStringLiteral("1"),
            QStringLiteral("-c:v"), QStringLiteral("mpeg4"), QStringLiteral("-threads"), QStringLiteral("1"), original});
        QVERIFY(generate.waitForFinished(5000));
        QCOMPARE(generate.exitStatus(), QProcess::NormalExit);
        QCOMPARE(generate.exitCode(), 0);
        const QFileInfo fixture(original);
        QVERIFY(fixture.isFile() && fixture.size() > 0);

        std::unique_ptr<FixtureBackend> backend(makeFixture());
        MessageFile video;
        video.id = QStringLiteral("98");
        video.name = QStringLiteral("forwarded.mp4");
        video.size = fixture.size();
        video.downloadUrl = backend->portalOrigin().resolved(QUrl(QStringLiteral("/disk/downloadFile/98/")));
        video.isVideo = true;
        auto &message = backend->messages.first();
        message.files = {video};
        message.forwarded = true;
        message.forwardedAuthor = QStringLiteral("Сотрудник");

        LocalFileOpener player;
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        QTRY_COMPARE(backend->videoRequests.size(), 1);
        QCOMPARE(player.opened.size(), 0);
        const auto [url, path] = backend->videoRequests.first();
        QCOMPARE(url, video.downloadUrl);
        QVERIFY(QFileInfo(path).isAbsolute());
        QCOMPARE(QFileInfo(path).fileName(), QStringLiteral("clip.mp4"));

        const auto findPixel = [&](const std::function<bool(const QColor &)> &match) {
            const auto pixels = history->viewport()->grab().toImage();
            const auto bounds = history->visualItemRect(history->item(0)).intersected(history->viewport()->rect());
            for (int y = bounds.top(); y <= bounds.bottom(); ++y) {
                for (int x = bounds.left(); x <= bounds.right(); ++x) {
                    if (match(pixels.pixelColor(x, y)))
                        return QPoint(x, y);
                }
            }
            return QPoint();
        };
        if (clickWhileDownloading) {
            const auto tile = [&] { return findPixel([](const QColor &color) { return color == QColor("#f6faf8"); }); };
            QTRY_VERIFY(!tile().isNull());
            QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, tile());
            QCOMPARE(backend->videoRequests.size(), 1);
            QCOMPARE(player.opened.size(), 0);
        }
        QVERIFY(QFile::copy(original, path));
        emit backend->messageVideoDownloaded(url, path);
        const auto poster = [&] {
            return findPixel([](const QColor &color) { return color.red() > 220 && color.green() < 35 && color.blue() > 220; });
        };
        QTRY_VERIFY_WITH_TIMEOUT(!poster().isNull(), 10000);
        if (!clickWhileDownloading) {
            QCOMPARE(player.opened.size(), 0);
            QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, poster());
        }
        QTRY_COMPARE(player.opened.size(), 1);
        QCOMPARE(player.opened.first(), QUrl::fromLocalFile(path));
        QCOMPARE(backend->videoRequests.size(), 1);
    }

    void messageGalleryUsesOneBubbleAndCompactClickableQuote()
    {
        QListWidget list;
        auto *delegate = workspace::configureRows(&list, workspace::Row::Message);
        list.resize(650, 660);
        const QColor firstColor("#ce4261");
        const QColor secondColor("#3bad77");
        QPixmap first(160, 92);
        QPixmap second(160, 92);
        first.fill(firstColor);
        second.fill(secondColor);
        const QUrl firstUrl(QStringLiteral("https://example.bitrix24.ru/one.png"));
        const QUrl secondUrl(QStringLiteral("https://example.bitrix24.ru/two.png"));
        delegate->setMessagePreviewSource([&](const QUrl &url) -> const QPixmap * {
            return url == firstUrl ? &first : url == secondUrl ? &second : nullptr;
        });
        QStringList opened;
        delegate->setMessagePhotoHandler([&](const MessageFile &file) { opened.append(file.id); });
        delegate->setReplyNavigation([&](const QString &id) { opened.append(QStringLiteral("reply:") + id); });
        delegate->setMessagePhotoMenuHandler([](const QModelIndex &, const MessageFile &, const QRect &) {});

        Message gallery;
        gallery.id = QStringLiteral("101");
        gallery.author = QStringLiteral("Анна");
        gallery.text = QStringLiteral("[URL=https://example.bitrix24.ru/attachment-note]Описание галереи[/URL]");
        gallery.files = {{QStringLiteral("11"), QStringLiteral("Первая.png"), 3000, {}, firstUrl},
                         {QStringLiteral("12"), QStringLiteral("Вторая.png"), 3200, {}, secondUrl}};
        Message reply;
        reply.id = QStringLiteral("102");
        reply.replyId = gallery.id;
        reply.text = QStringLiteral("Принято");
        QLabel state;
        ui::presentMessages(&list, &state, {gallery, reply});
        list.show();
        QTRY_COMPARE(list.count(), 2);
        QTRY_VERIFY(list.visualItemRect(list.item(1)).isValid());

        const auto grab = list.viewport()->grab();
        const auto capture = qEnvironmentVariable("B24_CHAT_GALLERY_SCREENSHOT");
        if (!capture.isEmpty())
            QVERIFY(grab.save(capture));
        auto pixels = grab.toImage();
        const auto colorBounds = [&](int row, const QColor &color) {
            const auto bounds = list.visualItemRect(list.item(row)).intersected(list.viewport()->rect());
            QRect painted;
            for (int y = bounds.top(); y <= bounds.bottom(); ++y) {
                for (int x = bounds.left(); x <= bounds.right(); ++x) {
                    if (pixels.pixelColor(x, y) == color)
                        painted |= QRect(x, y, 1, 1);
                }
            }
            return painted;
        };
        const auto firstTile = colorBounds(0, firstColor);
        const auto secondTile = colorBounds(0, secondColor);
        QVERIFY(firstTile.isValid() && secondTile.isValid());
        QVERIFY(firstTile.right() < secondTile.left());
        QVERIFY(qAbs(firstTile.top() - secondTile.top()) <= 4);

        const auto original = list.model()->index(0, 0);
        const auto row = list.visualRect(original);
        const auto bubble = delegate->receiptArea(original, row);
        QPoint link;
        for (int y = bubble.top(); y < bubble.bottom() && link.isNull(); ++y) {
            const QPoint candidate(bubble.left() + 22, y);
            if (delegate->anchorAt(original, row, candidate).endsWith(QStringLiteral("/attachment-note")))
                link = candidate;
        }
        QVERIFY(!link.isNull());
        QVERIFY(link.y() > qMax(firstTile.bottom(), secondTile.bottom()));

        const auto quotePreview = colorBounds(1, firstColor);
        QVERIFY(quotePreview.isValid());
        QVERIFY(quotePreview.width() <= 44 && quotePreview.height() <= 44);
        QVERIFY(colorBounds(1, secondColor).isEmpty());
        const auto quoted = list.model()->index(1, 0);
        const auto quoteRow = list.visualRect(quoted);
        QVERIFY(quoteRow.height() < 135);
        QVERIFY(delegate->receiptArea(quoted, quoteRow).width() >= 285);
        QVERIFY(!delegate->showPhotoMenuAt(quoted, quotePreview.center()));

        for (const auto &point : {firstTile.center(), secondTile.center(), quotePreview.center()})
            QTest::mouseClick(list.viewport(), Qt::LeftButton, Qt::NoModifier, point);
        QCOMPARE(opened, (QStringList{QStringLiteral("11"), QStringLiteral("12"), QStringLiteral("reply:101")}));

        list.resize(215, 660);
        list.doItemsLayout();
        const auto narrowGrab = list.viewport()->grab();
        const auto narrowCapture = qEnvironmentVariable("B24_CHAT_GALLERY_NARROW_SCREENSHOT");
        if (!narrowCapture.isEmpty())
            QVERIFY(narrowGrab.save(narrowCapture));
        pixels = narrowGrab.toImage();
        const auto firstNarrow = colorBounds(0, firstColor);
        const auto secondNarrow = colorBounds(0, secondColor);
        QVERIFY(firstNarrow.isValid() && secondNarrow.isValid());
        QVERIFY(firstNarrow.bottom() < secondNarrow.top());
        QVERIFY(qAbs(firstNarrow.left() - secondNarrow.left()) <= 4);
        QVERIFY(delegate->receiptArea(quoted, list.visualRect(quoted)).right() < list.viewport()->width());
    }

    void chatAttachmentTooltipsShowEscapedFullNames()
    {
        QListWidget list;
        auto *delegate = workspace::configureRows(&list, workspace::Row::Message);
        list.resize(420, 420);
        const QUrl preview(QStringLiteral("https://example.bitrix24.ru/preview.png"));
        const QColor photoColor("#518faf");
        QPixmap photo(120, 80);
        photo.fill(photoColor);
        delegate->setMessagePreviewSource([&](const QUrl &url) { return url == preview ? &photo : nullptr; });
        delegate->setMessagePhotoMenuHandler([](const QModelIndex &, const MessageFile &, const QRect &) {});

        Message document;
        document.id = QStringLiteral("document");
        const auto docName = QStringLiteral("Подробный отчёт для бухгалтерии за  целый год <b>Q&А</b>.xlsx");
        document.files = {{QStringLiteral("1"), docName, 36152, {}}};
        Message quote;
        quote.id = QStringLiteral("quote");
        quote.replyId = document.id;
        quote.text = QStringLiteral("Получила файл");
        Message picture;
        picture.id = QStringLiteral("picture");
        const auto photoName = QStringLiteral("Очень длинное имя снимка <i>корпус  & этаж</i>.png");
        picture.files = {{QStringLiteral("2"), photoName, 41000, {}, preview}};
        QLabel state;
        ui::presentMessages(&list, &state, {document, quote, picture});
        list.show();
        QTRY_COMPARE(list.count(), 3);
        QTRY_VERIFY(list.visualItemRect(list.item(2)).isValid());
        const auto pixels = list.viewport()->grab().toImage();
        const auto locate = [&](int row, const QColor &color) {
            const auto bounds = list.visualItemRect(list.item(row)).intersected(list.viewport()->rect());
            for (int y = bounds.top(); y <= bounds.bottom(); ++y) {
                for (int x = bounds.left(); x <= bounds.right(); ++x) {
                    if (pixels.pixelColor(x, y) == color)
                        return QPoint(x, y);
                }
            }
            return QPoint();
        };
        const auto hint = [&](const QPoint &point) {
            QHelpEvent event(QEvent::ToolTip, point, list.viewport()->mapToGlobal(point));
            QApplication::sendEvent(list.viewport(), &event);
            QTextDocument text;
            text.setHtml(QToolTip::text());
            return text.toPlainText();
        };
        const auto documentPoint = locate(0, QColor("#f6faf8"));
        const auto quotePoint = locate(1, QColor("#e8f1ee"));
        const auto photoPoint = locate(2, photoColor);
        QVERIFY(!documentPoint.isNull());
        QVERIFY(!quotePoint.isNull());
        QVERIFY(!photoPoint.isNull());
        QCOMPARE(hint(documentPoint), docName);
        QCOMPARE(hint(quotePoint), docName);
        QCOMPARE(hint(photoPoint), photoName);
        QToolTip::hideText();
    }

    void fileCardsSaveToExplicitPathAndReuseQuotedFiles()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdMessageFileDownload = true;
        LocalFileOpener opener;
        const QUrl url(QStringLiteral("https://example.bitrix24.ru/company/personal/file/download/1/"));
        Message fileMessage;
        fileMessage.id = QStringLiteral("file");
        fileMessage.outgoing = true;
        fileMessage.attachments = {QStringLiteral("Смета.xlsx")};
        fileMessage.files = {{QStringLiteral("1"), QStringLiteral("Смета.xlsx"), 36152, url}};
        Message reply;
        reply.id = QStringLiteral("reply");
        reply.replyId = fileMessage.id;
        reply.author = QStringLiteral("Анна");
        reply.text = QStringLiteral("Получила файл");
        backend->messages = {fileMessage, reply};
        backend->taskMessages = backend->messages;
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        QTRY_COMPARE(history->count(), 2);
        const auto *item = history->item(0);
        QCOMPARE(item->data(workspace::MessageFiles).value<QVector<MessageFile>>().size(), 1);
        QVERIFY(!item->data(workspace::Detail).toString().contains(QStringLiteral("Смета.xlsx")));
        QCOMPARE(history->item(1)->data(workspace::ReplyFiles).value<QVector<MessageFile>>().size(), 1);
        QCOMPARE(history->item(1)->data(workspace::ReplyPreview).toString(), QStringLiteral("Вложение"));
        const auto capture = qEnvironmentVariable("B24_CHAT_FILE_SCREENSHOT");
        if (!capture.isEmpty()) {
            QCoreApplication::processEvents();
            QVERIFY(window.grab().save(capture));
        }
        const auto row = history->visualItemRect(item);
        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(row.left() + 130, row.bottom() - 20));
        QTRY_VERIFY(window.findChild<QFileDialog *>(QStringLiteral("messageFileSaveDialog")));
        auto *dialog = window.findChild<QFileDialog *>(QStringLiteral("messageFileSaveDialog"));
        QCOMPARE(dialog->acceptMode(), QFileDialog::AcceptSave);
        QVERIFY(backend->downloadedPath.isEmpty());
        QTemporaryDir destination;
        QVERIFY(destination.isValid());
        const auto path = destination.filePath(QStringLiteral("confirmed.xlsx"));
        dialog->selectFile(path);
        QVERIFY(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection));
        QTRY_COMPARE(backend->downloadedPath, path);
        QCOMPARE(backend->downloadedUrl, url);

        const auto offerName = QStringLiteral("savedFileOffer");
        const auto oldDialog = QPointer<QFileDialog>(dialog);
        emit backend->messageFileDownloaded(url, destination.filePath(QStringLiteral("unexpected.xlsx")));
        QVERIFY(!window.findChild<QMessageBox *>(offerName));
        emit backend->failed(QStringLiteral("message-file-download"), QStringLiteral("Ошибка фикстуры"));
        QFile saved(path);
        QVERIFY(saved.open(QIODevice::WriteOnly));
        QCOMPARE(saved.write("fixture"), 7);
        saved.close();
        emit backend->messageFileDownloaded(url, path);
        QVERIFY(!window.findChild<QMessageBox *>(offerName));
        QVERIFY(opener.opened.isEmpty());
        QVERIFY(saved.remove());

        QTRY_VERIFY(!oldDialog);
        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(row.left() + 130, row.bottom() - 20));
        QTRY_VERIFY(window.findChild<QFileDialog *>(QStringLiteral("messageFileSaveDialog")));
        dialog = window.findChild<QFileDialog *>(QStringLiteral("messageFileSaveDialog"));
        dialog->selectFile(path);
        backend->downloadedPath.clear();
        QVERIFY(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection));
        QTRY_COMPARE(backend->downloadedPath, path);
        QVERIFY(saved.open(QIODevice::WriteOnly));
        QCOMPARE(saved.write("fixture"), 7);
        saved.close();
        emit backend->messageFileDownloaded(url, path);
        auto *offer = window.findChild<QMessageBox *>(offerName);
        QVERIFY(offer && offer->isVisible());
        QVERIFY(opener.opened.isEmpty());
        auto *open = findAccessible<QPushButton>(offer, QStringLiteral("Открыть"));
        QVERIFY(open);
        open->click();
        QCOMPARE(opener.opened, QList<QUrl>{QUrl::fromLocalFile(path)});

        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        sections->setCurrentRow(1);
        window.openTaskCard(QStringLiteral("501"));
        auto *task = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(task);
        auto *taskHistory = findAccessible<QListWidget>(task, QStringLiteral("Чат карточки"));
        QVERIFY(taskHistory);
        QTRY_COMPARE(taskHistory->count(), 2);
        QCOMPARE(taskHistory->item(1)->data(workspace::ReplyFiles).value<QVector<MessageFile>>().size(), 1);
    }

    void createsTaskFromConfirmedMessageInBothHistories()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        backend->accountScope = QStringLiteral("fixture/account");
        backend->chats.first().id = QStringLiteral("chat91");
        backend->messages.first().id = QStringLiteral("701");
        backend->messages.first().text = QStringLiteral("Исходное сообщение");
        backend->taskMessages = backend->messages;
        backend->taskMessages.first().id = QStringLiteral("801");
        backend->stages.first().tasks.first().chatDialogId = QStringLiteral("chat51");
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        emit backend->connectionChanged(true);
        auto *mainHistory = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(mainHistory);
        QTRY_COMPARE(mainHistory->count(), 1);

        const auto offerAction = [](QListWidget *history) -> QAction * {
            history->setCurrentRow(0);
            const auto point = history->visualItemRect(history->item(0)).center();
            QContextMenuEvent event(QContextMenuEvent::Keyboard, point, history->viewport()->mapToGlobal(point));
            QApplication::sendEvent(history->viewport(), &event);
            auto *menu = visibleMessageMenu();
            if (!menu)
                return nullptr;
            for (auto *action : menu->actions())
                if (action->text() == QStringLiteral("Создать задачу из сообщения…"))
                    return action;
            menu->close();
            return nullptr;
        };
        const auto confirm = [&](QListWidget *history, QWidget *parent, bool accept, bool replaceSource = false) {
            auto *action = offerAction(history);
            QVERIFY(action);
            auto *menu = visibleMessageMenu();
            menu->setActiveAction(action);
            QTest::keyClick(menu, Qt::Key_Return);
            auto *box = parent->findChild<QMessageBox *>(QStringLiteral("createTaskFromMessageConfirm"));
            QVERIFY(box && box->isVisible());
            QCOMPARE(box->parentWidget(), parent);
            QVERIFY(box->text().contains(QStringLiteral("Исходное сообщение")));
            if (accept) {
                auto *create = findAccessible<QPushButton>(box, QStringLiteral("Создать задачу"));
                QVERIFY(create);
                if (replaceSource)
                    history->item(0)->setData(workspace::ReceiptMessageId, QStringLiteral("799"));
                create->click();
            } else {
                QTest::keyClick(box, Qt::Key_Escape);
            }
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        };
        confirm(mainHistory, &window, false);
        QVERIFY(backend->messageActionId.isEmpty());
        confirm(mainHistory, &window, true, true);
        QVERIFY(backend->messageActionId.isEmpty());
        mainHistory->item(0)->setData(workspace::ReceiptMessageId, QStringLiteral("701"));
        confirm(mainHistory, &window, true);
        QCOMPARE(backend->messageActionDialog, QStringLiteral("chat91"));
        QCOMPARE(backend->messageActionId, QStringLiteral("701"));
        QCOMPARE(backend->messageAction, MessageAction::CreateTask);
        QCOMPARE(mainHistory->count(), 1); // The portal, not the client, inserts the new record.

        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        auto *taskHistory = findAccessible<QListWidget>(card, QStringLiteral("Чат карточки"));
        QVERIFY(taskHistory);
        QTRY_COMPARE(taskHistory->count(), 1);
        confirm(taskHistory, card, true);
        QCOMPARE(backend->messageActionDialog, QStringLiteral("chat51"));
        QCOMPARE(backend->messageActionId, QStringLiteral("801"));
        QCOMPARE(backend->messageAction, MessageAction::CreateTask);
        taskHistory->item(0)->setData(workspace::SystemMessage, true);
        QVERIFY(!offerAction(taskHistory));
        taskHistory->item(0)->setData(workspace::SystemMessage, false);
        emit backend->connectionChanged(false);
        QVERIFY(!offerAction(taskHistory));
    }

    void offersToOpenSavedWorktimeSheetAndEscapeDismisses()
    {
        class WorktimeBackend final : public FixtureBackend {
        public:
            void requestWorktimeSheet(const QDate &month) override
            {
                emit worktimeSheetLoaded({month, {}, true, {}});
            }
            void exportWorktimeSheet(const QDate &month, const QString &path) override
            {
                exportedMonth = month;
                exportedPath = path;
            }
            QDate exportedMonth;
            QString exportedPath;
        } backend;
        LocalFileOpener opener;
        QTemporaryDir destination;
        QVERIFY(destination.isValid());
        const auto path = destination.filePath(QStringLiteral("Табель.xls"));
        ui::WorktimePanel panel(&backend);
        panel.show();
        panel.refresh();
        auto *save = findAccessible<QPushButton>(&panel, QStringLiteral("Скачать табель Excel"));
        QVERIFY(save && save->isEnabled());
        QTimer::singleShot(0, &panel, [&] {
            if (auto *dialog = panel.findChild<QFileDialog *>()) {
                dialog->selectFile(path);
                QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
            }
        });
        save->click();
        QCOMPARE(backend.exportedPath, path);
        const auto offerName = QStringLiteral("savedFileOffer");
        emit backend.worktimeSheetExported(backend.exportedMonth.addMonths(1), path);
        QVERIFY(!panel.findChild<QMessageBox *>(offerName));
        QFile saved(path);
        QVERIFY(saved.open(QIODevice::WriteOnly));
        QCOMPARE(saved.write("fixture"), 7);
        saved.close();
        emit backend.worktimeSheetExported(backend.exportedMonth, path);
        QPointer<QMessageBox> offer = panel.findChild<QMessageBox *>(offerName);
        QVERIFY(offer && offer->isVisible());
        QVERIFY(opener.opened.isEmpty());
        QTest::keyClick(offer, Qt::Key_Escape);
        QTRY_VERIFY(!offer || !offer->isVisible());
        QVERIFY(opener.opened.isEmpty());
    }

    void showsExternalLinkCardInBothHistories()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        Message message;
        message.author = QStringLiteral("Анна");
        message.id = QStringLiteral("701");
        message.text = QStringLiteral("[URL=https://example.test/page]Ссылка[/URL]");
        const auto linked = message;
        message.id = QStringLiteral("702");
        message.text = QStringLiteral("Ссылка");
        backend->messages = {linked, message};
        backend->taskMessages = backend->messages;

        MainWindow window(backend.get());
        window.resize(960, 790);
        window.show();
        const auto check = [](QListWidget *history) {
            QVERIFY(history);
            QTRY_COMPARE(history->count(), 2);
            QTRY_VERIFY(history->sizeHintForRow(0) > history->sizeHintForRow(1) + 20);
            const auto row = history->visualItemRect(history->item(0));
            const QPoint preview(row.left() + 65, row.bottom() - 18);
            auto *viewport = history->viewport();
            QVERIFY(viewport->rect().contains(preview));
            QMouseEvent move(QEvent::MouseMove, preview, viewport->mapToGlobal(preview),
                             Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(viewport, &move);
            QCOMPARE(viewport->cursor().shape(), Qt::PointingHandCursor);
        };
        check(findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата")));
        window.openTaskCard(QStringLiteral("501"));
        auto *task = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(task);
        check(findAccessible<QListWidget>(task, QStringLiteral("Чат карточки")));
    }

    void fetchesOnlyMissingPublicLinksFromTheVisibleOutgoingRowOrAnExplicitAction()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        backend->chats.first().id = QStringLiteral("chat91");
        backend->holdExternalLinkPreviews = true;
        const auto make = [](const QString &id, const QUrl &url, bool outgoing) {
            Message message;
            message.id = id;
            message.outgoing = outgoing;
            message.author = outgoing ? QStringLiteral("Вы") : QStringLiteral("Анна");
            message.text = url.toString();
            return message;
        };
        const QUrl portalRich(QStringLiteral("https://public-preview.example.com/rich"));
        const QUrl outgoing(QStringLiteral("https://public-preview.example.com/first"));
        const QUrl unavailable(QStringLiteral("https://public-preview.example.com/unavailable"));
        const QUrl incoming(QStringLiteral("https://public-preview.example.com/incoming"));
        const QUrl forwarded(QStringLiteral("https://public-preview.example.com/forwarded"));
        auto duplicate = outgoing;
        duplicate.setFragment(QStringLiteral("same-page"));
        auto forwardedMessage = make(QStringLiteral("716"), forwarded, true);
        forwardedMessage.forwarded = true;
        forwardedMessage.forwardedAuthor = QStringLiteral("Другой автор");
        backend->messages = {make(QStringLiteral("711"), portalRich, true), make(QStringLiteral("712"), outgoing, true),
            make(QStringLiteral("713"), unavailable, true), make(QStringLiteral("714"), incoming, false),
            make(QStringLiteral("715"), duplicate, true), forwardedMessage};
        QVector<QPair<QUrl, ui::PublicLinkPreviewFetcher::Completion>> requests;
        MainWindow window(backend.get(), false, nullptr, [&](const QUrl &url, auto complete) {
            requests.append({url, std::move(complete)});
            return true;
        });
        window.resize(1000, 780);
        window.show();
        QApplication::setActiveWindow(&window);
        emit backend->connectionChanged(true);
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        QTRY_COMPARE(history->count(), 6);
        QTRY_COMPARE(backend->externalLinkPreviewRequests.size(), 1);
        QVERIFY(requests.isEmpty());
        const auto showMessage = [history](int row) {
            history->scrollToItem(history->item(row), QAbstractItemView::PositionAtCenter);
            QTRY_VERIFY(history->viewport()->rect().contains(history->visualItemRect(history->item(row)).center()));
        };
        showMessage(1);
        ChatAsset portal;
        portal.messageId = QStringLiteral("711");
        portal.link = portalRich;
        portal.name = QStringLiteral("Предпросмотр портала");
        portal.description = QStringLiteral("Данные портала");
        auto deliverPortal = std::move(backend->externalLinkPreviewRequests.first().second);
        deliverPortal({portal});
        QTRY_COMPARE(requests.size(), 1);
        QCOMPARE(requests.constFirst().first, outgoing);
        const int before = history->sizeHintForRow(1);
        auto deliverPublic = std::move(requests.first().second);
        QImage thumbnail(116, 100, QImage::Format_ARGB32_Premultiplied);
        const QColor thumbnailColor("#994064");
        thumbnail.fill(thumbnailColor);
        {
            QPainter painter(&thumbnail);
            painter.fillRect(QRect(24, 20, 66, 48), QColor("#fbe2ed"));
            painter.fillRect(QRect(32, 30, 50, 5), thumbnailColor);
            painter.fillRect(QRect(32, 44, 22, 15), thumbnailColor);
        }
        const auto thumbnailDrawn = [thumbnailColor](QListWidget *list, int row) {
            const auto image = list->viewport()->grab().toImage();
            const auto bounds = list->visualItemRect(list->item(row)).intersected(image.rect());
            int pixels = 0;
            for (int y = bounds.top(); y <= bounds.bottom(); ++y)
                for (int x = bounds.left(); x <= bounds.right(); ++x)
                    pixels += image.pixelColor(x, y) == thumbnailColor ? 1 : 0;
            return pixels > 100;
        };
        deliverPublic(ui::PublicLinkMetadata{QStringLiteral("Заголовок сайта"), QStringLiteral("Описание сайта"), thumbnail});
        QTRY_VERIFY(history->sizeHintForRow(1) > before);
        showMessage(1);
        QTRY_VERIFY(thumbnailDrawn(history, 1));
        const auto screenshot = qEnvironmentVariable("B24_PUBLIC_PREVIEW_IMAGE_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(history->viewport()->grab().save(screenshot));
        QVERIFY(backend->imageRequests.isEmpty());
        showMessage(2);
        QTRY_COMPARE(requests.size(), 2);
        QCOMPARE(requests.last().first, unavailable);
        auto deliverUnavailable = std::move(requests.last().second);
        deliverUnavailable(std::nullopt);
        emit backend->chatMessagesLoaded(QStringLiteral("chat91"), backend->messages);
        QTest::qWait(100);
        QCOMPARE(requests.size(), 2);

        history->setCurrentRow(3);
        showMessage(3);
        const auto point = history->visualItemRect(history->item(3)).center();
        QContextMenuEvent event(QContextMenuEvent::Keyboard, point, history->viewport()->mapToGlobal(point));
        QApplication::sendEvent(history->viewport(), &event);
        auto *menu = visibleMessageMenu();
        QVERIFY(menu);
        QAction *preview = nullptr;
        for (auto *action : menu->actions())
            if (action->text() == QStringLiteral("Загрузить предпросмотр ссылки"))
                preview = action;
        QVERIFY(preview && preview->isEnabled());
        menu->setActiveAction(preview);
        QTest::keyClick(menu, Qt::Key_Return);
        QTRY_COMPARE(requests.size(), 3);
        QCOMPARE(requests.last().first, incoming);
        auto deliverIncoming = std::move(requests.last().second);
        deliverIncoming(ui::PublicLinkMetadata{{}, {}, thumbnail});
        QTRY_VERIFY(thumbnailDrawn(history, 3));
        QVERIFY(backend->imageRequests.isEmpty());

        backend->taskMessages = {make(QStringLiteral("721"), duplicate, true)};
        window.openTaskCard(QStringLiteral("501"));
        auto *task = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(task);
        auto *taskHistory = findAccessible<QListWidget>(task, QStringLiteral("Чат карточки"));
        QVERIFY(taskHistory);
        QTRY_COMPARE(taskHistory->count(), 1);
        QTRY_VERIFY(thumbnailDrawn(taskHistory, 0));
        QCOMPARE(requests.size(), 3);
    }

    void drawsAuthorizedLinkAvatar()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdLinkPreviews = true;
        const QUrl avatar(QStringLiteral("https://example.bitrix24.ru/upload/preview-person.png"));
        const QColor avatarColor("#8d3c58");
        const QColor projectColor("#286f62");
        QByteArray bytes;
        QBuffer buffer(&bytes);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QImage photo(64, 64, QImage::Format_RGB32);
        photo.fill(avatarColor);
        {
            QPainter painter(&photo);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setBrush(QColor("#f7d9cd"));
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(QRect(22, 11, 20, 21));
            painter.drawEllipse(QRect(13, 37, 38, 33));
        }
        QVERIFY(photo.save(&buffer, "PNG"));
        backend->avatarImages.insert(avatar, bytes);
        Message task;
        task.author = QStringLiteral("Поддержка");
        task.id = QStringLiteral("701");
        task.text = QStringLiteral("Создана [URL=/tasks/task/view/42/]задача[/URL]");
        auto person = task;
        person.id = QStringLiteral("702");
        person.text = QStringLiteral("[URL=/company/personal/user/19/]Анна Волкова[/URL]");
        backend->messages = {task, person};

        MainWindow window(backend.get());
        window.resize(950, 760);
        window.show();
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        QTRY_COMPARE(backend->linkPreviewRequests.size(), 2);
        QVERIFY(!backend->avatarRequests.contains(avatar));
        for (auto &[url, complete] : backend->linkPreviewRequests) {
            if (url.path().contains(QStringLiteral("/tasks/"))) {
                MessageLinkPreview preview{QStringLiteral("Восстановить принтер и кнопку печати"),
                    {QStringLiteral("Статус · Ждёт выполнения"), QStringLiteral("Исполнитель · Анна Волкова"),
                     QStringLiteral("Проект · IT разработка")}, MessageLinkPreviewKind::Task};
                Project project{QStringLiteral("7"), QStringLiteral("IT разработка"), {}};
                project.avatarType = QStringLiteral("folder");
                project.avatarColor = projectColor.name();
                preview.project = std::move(project);
                complete(std::move(preview));
            } else {
                complete({QStringLiteral("Анна Волкова"), {QStringLiteral("Инженер поддержки")},
                          MessageLinkPreviewKind::Person, avatar});
            }
        }
        QTRY_COMPARE(backend->avatarRequests.count(avatar), 1);
        const auto image = history->viewport()->grab().toImage();
        const auto pixelsInRow = [&image, history](int index, QColor color) {
            const auto row = history->visualItemRect(history->item(index)).intersected(image.rect());
            int pixels = 0;
            for (int y = row.top(); y <= row.bottom(); ++y)
                for (int x = row.left(); x <= row.right(); ++x)
                    pixels += image.pixelColor(x, y) == color ? 1 : 0;
            return pixels;
        };
        QVERIFY2(pixelsInRow(0, projectColor) > 60, "The task should draw its project's configured icon");
        QVERIFY2(pixelsInRow(1, avatarColor) > 60, "The authorized profile picture should be drawn inside the link card");
        const auto screenshot = qEnvironmentVariable("B24_LINK_CARD_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(history->viewport()->grab().save(screenshot));
    }

    void rechecksPortalLinkPermissionWhenSwitchingHistories()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdLinkPreviews = true;
        Message message;
        message.author = QStringLiteral("Анна");
        message.id = QStringLiteral("701");
        message.text = QStringLiteral("[URL=/tasks/task/view/42/]Задача[/URL]");
        const auto linked = message;
        message.id = QStringLiteral("702");
        message.text = QStringLiteral("Задача");
        backend->messages = {linked, message};
        backend->taskMessages = backend->messages;

        MainWindow window(backend.get());
        window.resize(960, 790);
        window.show();
        QTRY_COMPARE(backend->linkPreviewRequests.size(), 1);
        window.openTaskCard(QStringLiteral("501"));
        auto *task = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(task);
        auto *history = findAccessible<QListWidget>(task, QStringLiteral("Чат карточки"));
        QVERIFY(history);
        QTRY_COMPARE(backend->linkPreviewRequests.size(), 2);
        QCOMPARE(backend->linkPreviewRequests.at(0).first, backend->linkPreviewRequests.at(1).first);
        auto earlier = std::move(backend->linkPreviewRequests[0].second);
        earlier({QStringLiteral("Доступ в прежнем диалоге"), {}});
        QCoreApplication::processEvents();
        QCOMPARE(history->sizeHintForRow(0), history->sizeHintForRow(1));
        auto current = std::move(backend->linkPreviewRequests[1].second);
        current({});
        QCoreApplication::processEvents();
        QCOMPARE(history->sizeHintForRow(0), history->sizeHintForRow(1));
        QCOMPARE(backend->linkPreviewRequests.size(), 2);
        emit backend->taskUpdated(QStringLiteral("42"));
        QTRY_COMPARE(backend->linkPreviewRequests.size(), 3);
        emit backend->taskLinkPreviewInvalidated(QStringLiteral("701"));
        QCoreApplication::processEvents();
        QCOMPARE(backend->linkPreviewRequests.size(), 3);
        emit backend->taskLinkPreviewInvalidated(QStringLiteral("42"));
        QTRY_COMPARE(backend->linkPreviewRequests.size(), 4);
    }

    void hidesOnlyTheCurrentTaskPreviewInBothTaskHistories()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdLinkPreviews = true;
        const auto self = QStringLiteral("/tasks/task/view/501/");
        const auto moved = QStringLiteral("https://example.bitrix24.ru/workgroups/group/70/tasks/task/view/501/?from=chat");
        const auto other = QStringLiteral("/tasks/task/view/502/");
        const auto external = QStringLiteral("https://elsewhere.example/tasks/task/view/501/");
        const auto author = QStringLiteral("Анна");
        const auto caption = QStringLiteral("Ссылка на задачу");
        backend->taskMessages.clear();
        for (const auto &url : {self, moved, other, external, QString()}) {
            Message message;
            message.id = QString::number(700 + backend->taskMessages.size());
            message.author = author;
            message.text = url.isEmpty() ? caption : QStringLiteral("[URL=%1]%2[/URL]").arg(url, caption);
            backend->taskMessages.append(message);
        }
        backend->messages = backend->taskMessages;
        Chat taskChat{QStringLiteral("task-dialog"), QStringLiteral("Чат задачи"), QStringLiteral("tasksTask")};
        taskChat.taskChat = true;
        taskChat.taskId = QStringLiteral("501");
        backend->taskChats = {taskChat};

        MainWindow window(backend.get());
        window.resize(1020, 850);
        window.show();
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("chatScopeTabs"));
        QVERIFY(history && tabs);
        const auto complete = [&backend] {
            for (auto &request : backend->linkPreviewRequests) {
                auto completion = std::exchange(request.second, {});
                if (completion)
                    completion({QStringLiteral("Карточка задачи"), {QStringLiteral("Описание")}, MessageLinkPreviewKind::Task});
            }
            backend->linkPreviewRequests.clear();
        };
        QTRY_COMPARE(backend->linkPreviewRequests.size(), 3);
        complete();
        QTRY_VERIFY(history->sizeHintForRow(0) > history->sizeHintForRow(4));
        tabs->setCurrentIndex(1);
        QTRY_VERIFY(!backend->linkPreviewRequests.isEmpty());
        QCOMPARE(backend->linkPreviewRequests.size(), 1);
        QCOMPARE(backend->linkPreviewRequests.first().first.path(), other);
        complete();
        QTRY_COMPARE(history->sizeHintForRow(0), history->sizeHintForRow(4));
        QCOMPARE(history->sizeHintForRow(1), history->sizeHintForRow(4));
        QVERIFY(history->sizeHintForRow(2) > history->sizeHintForRow(4));
        QVERIFY(history->sizeHintForRow(3) > history->sizeHintForRow(4));

        // Scope metadata can arrive on the current chat without resetting its existing cache.
        backend->taskChats[0].taskId.clear();
        emit backend->taskChatsLoaded(backend->taskChats);
        QTRY_COMPARE(backend->linkPreviewRequests.size(), 2);
        complete();
        QTRY_VERIFY(history->sizeHintForRow(0) > history->sizeHintForRow(4));
        backend->taskChats[0].taskId = taskChat.taskId;
        emit backend->taskChatsLoaded(backend->taskChats);
        QTRY_COMPARE(history->sizeHintForRow(0), history->sizeHintForRow(4));
        QCOMPARE(history->sizeHintForRow(1), history->sizeHintForRow(4));
        QVERIFY(backend->linkPreviewRequests.isEmpty());

        auto *delegate = static_cast<workspace::RowPainter *>(history->itemDelegate());
        const auto index = history->model()->index(0, 0);
        const auto row = history->visualItemRect(history->item(0));
        const auto bubble = delegate->receiptArea(index, row);
        std::optional<QPoint> anchor;
        for (int y = bubble.top(); y <= bubble.bottom() && !anchor; y += 2) {
            for (int x = bubble.left(); x <= bubble.right(); x += 2) {
                if (delegate->anchorAt(index, row, QPoint(x, y)) == self) {
                    anchor = QPoint(x, y);
                    break;
                }
            }
        }
        QVERIFY(anchor);
        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, *anchor);
        auto *card = dynamic_cast<ui::TaskCardDialog *>(
            window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        QVERIFY(card);
        QCOMPARE(card->taskId(), taskChat.taskId);
        auto *cardHistory = findAccessible<QListWidget>(card, QStringLiteral("Чат карточки"));
        QVERIFY(cardHistory);
        QTRY_VERIFY(!backend->linkPreviewRequests.isEmpty());
        QCOMPARE(backend->linkPreviewRequests.size(), 1);
        QCOMPARE(backend->linkPreviewRequests.first().first.path(), other);
        complete();
        QTRY_COMPARE(cardHistory->sizeHintForRow(0), cardHistory->sizeHintForRow(4));
        QCOMPARE(cardHistory->sizeHintForRow(1), cardHistory->sizeHintForRow(4));
        QVERIFY(cardHistory->sizeHintForRow(2) > cardHistory->sizeHintForRow(4));
        QVERIFY(cardHistory->sizeHintForRow(3) > cardHistory->sizeHintForRow(4));
    }

    void refreshesOnlyTheChangedTaskLinkPreviews()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdLinkPreviews = true;
        const auto cached = QStringLiteral("/tasks/task/view/500/");
        const auto alias = QStringLiteral("/workgroups/group/7/tasks/task/view/42/");
        const auto changed = QStringLiteral("/tasks/task/view/42/");
        const auto pending = QStringLiteral("/company/personal/user/19/");
        backend->messages.clear();
        for (const auto &path : {cached, alias, changed, pending}) {
            Message message;
            message.id = QString::number(701 + backend->messages.size());
            message.author = QStringLiteral("Анна");
            message.text = QStringLiteral("[URL=%1]Ссылка[/URL]").arg(path);
            backend->messages.append(std::move(message));
        }

        MainWindow window(backend.get());
        window.resize(960, 790);
        window.show();
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        const auto count = [&backend](const QString &path) {
            return std::count_if(backend->linkPreviewRequests.cbegin(), backend->linkPreviewRequests.cend(),
                                 [&path](const auto &entry) { return entry.first.path() == path; });
        };
        const auto takeCallback = [&backend](const QString &path) -> Backend::LinkPreviewHandler {
            for (auto &entry : backend->linkPreviewRequests)
                if (entry.first.path() == path && entry.second)
                    return std::exchange(entry.second, {});
            return {};
        };
        QTRY_COMPARE(backend->linkPreviewRequests.size(), 3);
        auto completeCached = takeCallback(cached);
        auto completeAlias = takeCallback(alias);
        QVERIFY(completeCached && completeAlias);
        completeCached({QStringLiteral("Other task"), {QStringLiteral("Known project")}});
        completeAlias({QStringLiteral("Old alias"), {}});
        QTRY_COMPARE(backend->linkPreviewRequests.size(), 4);
        auto oldChanged = takeCallback(changed);
        auto completePending = takeCallback(pending);
        QVERIFY(oldChanged && completePending);
        const int cachedHeight = history->sizeHintForRow(0);
        const int pendingHeight = history->sizeHintForRow(3);

        emit backend->taskLinkPreviewInvalidated(QStringLiteral("42"));
        QTRY_COMPARE(backend->linkPreviewRequests.size(), 5);
        QCOMPARE(count(cached), 1);
        QCOMPARE(history->sizeHintForRow(0), cachedHeight);
        completePending({QStringLiteral("Other profile"), {QStringLiteral("Authorized details")}});
        QTRY_COMPARE(backend->linkPreviewRequests.size(), 6);
        QCOMPARE(count(pending), 1);
        QTRY_VERIFY(history->sizeHintForRow(3) > pendingHeight);
        QCOMPARE(count(alias), 2);
        QCOMPARE(count(changed), 2);
        auto newChanged = takeCallback(changed);
        auto newAlias = takeCallback(alias);
        QVERIFY(newChanged && newAlias);
        newChanged({QStringLiteral("New project"), {QStringLiteral("Current details")}});
        newAlias({QStringLiteral("New project alias"), {}});
        const int updatedHeight = history->sizeHintForRow(2);
        oldChanged({QStringLiteral("Outdated project"), {}});
        QCoreApplication::processEvents();
        QCOMPARE(history->sizeHintForRow(2), updatedHeight);
        QCOMPARE(backend->linkPreviewRequests.size(), 6);
    }

    void keepsMessageViewersRecoverableAndOnScreen_data()
    {
        QTest::addColumn<bool>("recover");
        QTest::newRow("retry-and-scope") << true;
        QTest::newRow("asynchronous-growth-at-edge") << false;
    }

    void keepsMessageViewersRecoverableAndOnScreen()
    {
        QFETCH(bool, recover);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdViewerResponses = true;
        backend->accountScope = QStringLiteral("fixture/viewer-first");
        Message message;
        message.id = QStringLiteral("701");
        message.author = QStringLiteral("Я");
        message.text = QStringLiteral("Отправленное сообщение");
        message.outgoing = true;
        message.seenByOthers = true;
        backend->messages = {message};
        Chat other;
        other.id = QStringLiteral("chat42");
        other.title = QStringLiteral("Другой чат");
        backend->chats.append(other);
        MainWindow window(backend.get());
        window.resize(880, 600);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
        auto *footer = findAccessible<QLabel>(&window, QStringLiteral("Состояние"));
        QVERIFY(history && chats && footer && window.screen());
        QTRY_COMPARE(history->count(), 1);
        QVERIFY(chats->count() >= 2);
        const auto available = window.screen()->availableGeometry();
        auto *delegate = static_cast<const workspace::RowPainter *>(history->itemDelegate());
        const auto receipt = [&] {
            return delegate->receiptArea(history->model()->index(0, 0), history->visualItemRect(history->item(0)));
        };
        QVERIFY(!receipt().isEmpty());
        if (!recover) {
            const auto target = available.bottomRight() - QPoint(30, 30);
            QTRY_VERIFY(history->viewport()->rect().contains(receipt().center()));
            for (int pass = 0; pass < 2; ++pass) {
                QCoreApplication::processEvents();
                window.move(window.pos() + target - history->viewport()->mapToGlobal(receipt().bottomLeft()));
            }
            QCoreApplication::processEvents();
            const auto nearEdge = history->viewport()->mapToGlobal(receipt().bottomLeft());
            QVERIFY2(available.contains(nearEdge),
                     qPrintable(QStringLiteral("available %1x%2, actual %3,%4").arg(available.width())
                       .arg(available.height()).arg(nearEdge.x()).arg(nearEdge.y())));
            QVERIFY(nearEdge.y() > available.bottom() - 50);
            QVERIFY(nearEdge.x() > available.right() - 50);
        }
        const auto clickReceipt = [&] {
            const auto box = receipt();
            QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier,
                              QPoint(box.right() - 14, box.bottom() - 9));
        };
        clickReceipt();
        QCOMPARE(backend->viewerReplies.size(), 1);
        QPointer<QFrame> popup = history->findChild<QFrame *>(QStringLiteral("messageViewersMenu"));
        QVERIFY(popup && popup->isVisible());
        const auto takeReply = [&] { return std::exchange(backend->viewerReplies.last(), {}); };
        const auto findRetry = [&] {
            return findAccessible<QPushButton>(popup, QStringLiteral("Повторить загрузку списка прочитавших"));
        };
        auto *rows = popup->findChild<QListWidget *>(QStringLiteral("messageViewersList"));
        QPushButton *close = nullptr;
        for (auto *button : popup->findChildren<QPushButton *>())
            if (button->text() == QStringLiteral("Закрыть"))
                close = button;
        QVERIFY(rows && close);
        MessageViewers result;
        result.count = 33;
        for (int i = 0; i < result.count; ++i)
            result.people.append({QString::number(100 + i), QStringLiteral("Сотрудник %1").arg(i), {}, {}});
        if (recover) {
            const auto initial = QStringLiteral("Текущий статус");
            emit backend->failed(QStringLiteral("fixture"), initial);
            takeReply()(std::nullopt);
            QCOMPARE(footer->text(), initial);
            auto *retry = findRetry();
            QVERIFY(retry && retry->isVisible() && retry->isEnabled());
            retry->click();
            QCOMPARE(backend->viewerReplies.size(), 2);
            QVERIFY(!retry->isVisible() || !retry->isEnabled());
            retry->click();
            QCOMPARE(backend->viewerReplies.size(), 2);
            takeReply()(result);
            QCOMPARE(rows->count(), 33);
            QCOMPARE(rows->item(0)->text(), QStringLiteral("Сотрудник 0"));
            QVERIFY(!retry->isVisible());
            popup->close();
            QTRY_VERIFY(popup.isNull());

            clickReceipt();
            QCOMPARE(backend->viewerReplies.size(), 3);
            popup = history->findChild<QFrame *>(QStringLiteral("messageViewersMenu"));
            QVERIFY(popup && popup->isVisible());
            auto late = takeReply();
            chats->setCurrentRow(1);
            QTRY_VERIFY(popup.isNull());
            late(result);

            chats->setCurrentRow(0);
            QTRY_COMPARE(history->count(), 1);
            clickReceipt();
            QCOMPARE(backend->viewerReplies.size(), 4);
            popup = history->findChild<QFrame *>(QStringLiteral("messageViewersMenu"));
            QVERIFY(popup && popup->isVisible());
            late = takeReply();
            backend->accountScope = QStringLiteral("fixture/viewer-second");
            emit backend->accountScopeChanged();
            QTRY_VERIFY(popup.isNull());
            late(result);
            return;
        }

        const auto anchor = history->viewport()->mapToGlobal(receipt().bottomLeft());
        takeReply()(result);
        QTRY_COMPARE(rows->count(), 33);
        QVERIFY(rows->isVisible() && close->isVisible());
        QTRY_VERIFY(rows->verticalScrollBar()->maximum() > 0);
        const auto inside = [popup](const QWidget *widget) {
            return popup->rect().contains(QRect(widget->mapTo(popup, QPoint()), widget->size()));
        };
        QTRY_VERIFY(inside(rows) && inside(close));
        QTRY_VERIFY(available.contains(popup->frameGeometry()));
        QVERIFY(popup->frameGeometry().bottom() < anchor.y());
        if (const auto path = qEnvironmentVariable("B24_VIEWERS_EDGE_SCREENSHOT"); !path.isEmpty())
            QVERIFY(popup->grab().save(path));
    }

    void opensViewersFromReadReceiptsInBothHistories()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        Message message;
        message.id = QStringLiteral("701");
        message.author = QStringLiteral("Я");
        message.text = QStringLiteral("Отправленное сообщение");
        message.outgoing = true;
        message.seenByOthers = true;
        backend->messages = {message};
        backend->taskMessages = backend->messages;
        MainWindow window(backend.get());
        window.resize(960, 790);
        window.show();
        QVERIFY(backend->viewedReceipts.isEmpty());
        const auto check = [&backend](QListWidget *history, bool task, const QString &scope) {
            QVERIFY(history);
            QTRY_COMPARE(history->count(), 1);
            const auto *delegate = static_cast<const workspace::RowPainter *>(history->itemDelegate());
            const auto box = delegate->receiptArea(history->model()->index(0, 0), history->visualItemRect(history->item(0)));
            QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier,
                              QPoint(box.right() - 14, box.bottom() - 9));
            QTRY_VERIFY(history->findChild<QFrame *>(QStringLiteral("messageViewersMenu")));
            auto *menu = history->findChild<QFrame *>(QStringLiteral("messageViewersMenu"));
            QVERIFY(menu->isVisible());
            const auto *rows = menu->findChild<QListWidget *>(QStringLiteral("messageViewersList"));
            QVERIFY(rows);
            QCOMPARE(rows->count(), 1);
            QCOMPARE(rows->item(0)->text(), QStringLiteral("Анна Волкова"));
            QVERIFY(rows->isVisible());
            QVERIFY(menu->rect().contains(QRect(rows->mapTo(menu, QPoint{}), rows->size())));
            QCOMPARE(backend->viewedReceipts.last().scope, scope);
            QCOMPARE(backend->viewedReceipts.last().id, QStringLiteral("701"));
            QCOMPARE(backend->viewedReceipts.last().task, task);
            QTRY_VERIFY(backend->avatarRequests.contains(backend->viewerAvatar));
            const auto capture = qEnvironmentVariable("B24_VIEWERS_SCREENSHOT");
            if (!capture.isEmpty() && !task)
                QVERIFY(menu->grab().save(capture));
            menu->close();
        };
        check(findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата")), false, QStringLiteral("c-1"));
        window.openTaskCard(QStringLiteral("501"));
        auto *task = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(task);
        check(findAccessible<QListWidget>(task, QStringLiteral("Чат карточки")), true, QStringLiteral("501"));
        QCOMPARE(backend->viewedReceipts.size(), 2);
    }

    void rendersExternalMetadataOnlyForTheMatchingHistoryMessage()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdExternalLinkPreviews = true;
        const QUrl link(QStringLiteral("https://example.test/article"));
        const QUrl internal(QStringLiteral("https://example.bitrix24.ru/unknown"));
        const QUrl thumbnail(QStringLiteral("https://example.bitrix24.ru/upload/link-preview.png"));
        const QColor color("#83326d");
        QByteArray png;
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QImage photo(100, 70, QImage::Format_RGB32);
        photo.fill(color);
        QVERIFY(photo.save(&buffer, "PNG"));
        backend->messageImages.insert(thumbnail, png);
        const auto message = [](const QString &id, const QUrl &url) {
            Message value;
            value.id = id;
            value.author = QStringLiteral("Анна");
            value.text = QStringLiteral("[URL=%1]Ссылка[/URL]").arg(url.toString());
            return value;
        };
        backend->messages = {message(QStringLiteral("701"), link)};
        backend->taskMessages = {message(QStringLiteral("701"), link), message(QStringLiteral("702"), link),
                                 message(QStringLiteral("703"), internal)};

        MainWindow window(backend.get());
        window.resize(960, 790);
        window.show();
        QTRY_COMPARE(backend->externalLinkPreviewRequests.size(), 1);
        QVERIFY(!backend->externalLinkPreviewRequests.first().first.second);
        window.openTaskCard(QStringLiteral("501"));
        auto *task = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(task);
        auto *history = findAccessible<QListWidget>(task, QStringLiteral("Чат карточки"));
        QVERIFY(history);
        QTRY_COMPARE(history->count(), 3);
        const auto fallbackHeight = history->sizeHintForRow(0);
        const auto internalHeight = history->sizeHintForRow(2);
        ChatAsset asset;
        asset.messageId = QStringLiteral("701");
        asset.link = link;
        asset.name = QStringLiteral("Заголовок страницы");
        asset.description = QStringLiteral("Описание страницы");
        asset.thumbnailUrl = thumbnail;
        auto stale = std::exchange(backend->externalLinkPreviewRequests[0].second, {});
        stale({asset});
        QTRY_COMPARE(backend->externalLinkPreviewRequests.size(), 2);
        QVERIFY(backend->externalLinkPreviewRequests.last().first.second);
        QCOMPARE(history->sizeHintForRow(0), fallbackHeight);
        QVERIFY(backend->imageRequests.isEmpty());
        auto intranet = asset;
        intranet.messageId = QStringLiteral("703");
        intranet.link = internal;
        auto complete = std::exchange(backend->externalLinkPreviewRequests[1].second, {});
        complete({asset, intranet});
        QTRY_COMPARE(backend->imageRequests.count(thumbnail), 1);
        QTRY_VERIFY(history->sizeHintForRow(0) > fallbackHeight);
        QCOMPARE(history->sizeHintForRow(1), fallbackHeight);
        QCOMPARE(history->sizeHintForRow(2), internalHeight);
        const auto thumbnailDrawn = [history, color] {
            const auto image = history->viewport()->grab().toImage();
            const auto bounds = history->visualItemRect(history->item(0)).intersected(image.rect());
            int pixels = 0;
            for (int y = bounds.top(); y <= bounds.bottom(); ++y)
                for (int x = bounds.left(); x <= bounds.right(); ++x)
                    pixels += image.pixelColor(x, y) == color ? 1 : 0;
            return pixels > 100;
        };
        QTRY_VERIFY(thumbnailDrawn());
        const auto screenshot = qEnvironmentVariable("B24_EXTERNAL_RICH_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(history->viewport()->grab().save(screenshot));
        backend->taskMessages.append(message(QStringLiteral("704"), link));
        emit backend->taskMessagesLoaded(QStringLiteral("501"), backend->taskMessages);
        QTRY_COMPARE(backend->externalLinkPreviewRequests.size(), 3);
    }

    void inlineChatPreviewAnimatesUntilTerminalResult_data()
    {
        QTest::addColumn<bool>("fallback");
        QTest::addColumn<bool>("succeeds");
        QTest::newRow("preferred succeeds") << false << true;
        QTest::newRow("preferred fails") << false << false;
        QTest::newRow("original succeeds") << true << true;
    }

    void inlineChatPreviewAnimatesUntilTerminalResult()
    {
        QFETCH(bool, fallback);
        QFETCH(bool, succeeds);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdMessageImages = true;
        MessageFile file;
        file.id = QStringLiteral("800");
        file.name = QStringLiteral("image (800).png");
        file.isImage = true;
        file.size = 8611;
        file.downloadUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/disk/downloadFile/800/"));
        file.previewUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/disk/showFile/800/"));
        if (fallback)
            file.originalPreviewUrl = file.downloadUrl;
        backend->messages.first().files = {file};

        MainWindow window(backend.get());
        window.resize(900, 600);
        window.show();
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        QTRY_COMPARE(backend->imageRequests.count(file.previewUrl), 1);
        if (fallback) {
            emit backend->messageImageLoaded(file.previewUrl, {});
            QTRY_COMPARE_WITH_TIMEOUT(backend->imageRequests.count(file.downloadUrl), 1, 800);
        }

        const auto rowImage = [history] {
            const auto row = history->visualItemRect(history->item(0));
            return history->viewport()->grab(row).toImage();
        };
        const auto firstFrame = rowImage();
        QVERIFY(!firstFrame.isNull());
        QTRY_VERIFY_WITH_TIMEOUT(rowImage() != firstFrame, 500);

        QByteArray bytes;
        QBuffer buffer(&bytes);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        const QColor thumbnailColor("#749fb1");
        QImage image(634, 60, QImage::Format_RGB32);
        image.fill(thumbnailColor);
        if (succeeds)
            QVERIFY(image.save(&buffer, "PNG"));
        emit backend->messageImageLoaded(fallback ? file.downloadUrl : file.previewUrl, bytes);
        QCoreApplication::processEvents();
        const auto finalFrame = rowImage();
        bool hasImage = false;
        for (int y = 0; y < finalFrame.height() && !hasImage; ++y)
            for (int x = 0; x < finalFrame.width() && !hasImage; ++x)
                hasImage = finalFrame.pixelColor(x, y) == thumbnailColor;
        QCOMPARE(hasImage, succeeds);
        QTest::qWait(210);
        QCOMPARE(rowImage(), finalFrame);
        QCOMPARE(backend->imageRequests.size(), fallback ? 2 : 1);
    }

    void chatPreviewRequestsRestartOnlyAfterAnAccountChange()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdMessageImages = true;
        MessageFile file;
        file.id = QStringLiteral("800");
        file.name = QStringLiteral("image.png");
        file.isImage = true;
        file.previewUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/disk/showFile/800/"));
        auto other = file;
        other.id = QStringLiteral("801");
        other.previewUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/disk/showFile/801/"));
        backend->messages.first().files = {file, other};

        MainWindow window(backend.get());
        window.resize(900, 600);
        window.show();
        QTRY_COMPARE(backend->imageRequests.size(), 2);

        backend->accountScope = QStringLiteral("fixture/preview-account-one");
        emit backend->accountScopeChanged();
        QCoreApplication::processEvents();
        QCOMPARE(backend->imageRequests.size(), 2);

        // Account changes cancel raster requests without delivering a completion.
        backend->accountScope = QStringLiteral("fixture/preview-account-two");
        emit backend->accountScopeChanged();
        QTRY_COMPARE_WITH_TIMEOUT(backend->imageRequests.count(file.previewUrl), 2, 800);
        QCOMPARE(backend->imageRequests.count(other.previewUrl), 2);
    }

    void imageContextMenuUsesTheClickedPhoto_data()
    {
        QTest::addColumn<bool>("viewer");
        QTest::addColumn<bool>("hasOriginal");
        QTest::newRow("inline original") << false << true;
        QTest::newRow("original viewer") << true << true;
        QTest::newRow("inline preview only") << false << false;
        QTest::newRow("preview only viewer") << true << false;
    }

    void imageContextMenuUsesTheClickedPhoto()
    {
        QFETCH(bool, viewer);
        QFETCH(bool, hasOriginal);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto &message = backend->messages.first();
        message.id = QStringLiteral("92");
        const QColor previewColor("#518faf");
        const QColor viewerColor("#294563");
        const QSize previewSize(120, 75);
        const QSize originalSize(600, 375);
        for (const int id : {1, 2}) {
            MessageFile file;
            file.id = QString::number(id);
            file.name = QStringLiteral("Фото %1.jpg").arg(id);
            file.previewUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/preview/%1").arg(id));
            if (id == 1 || hasOriginal)
                file.downloadUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/download/%1").arg(id));
            file.previewSize = previewSize;
            file.isImage = true;
            message.files.append(file);
            QByteArray bytes;
            QBuffer buffer(&bytes);
            QVERIFY(buffer.open(QIODevice::WriteOnly));
            QImage image(previewSize, QImage::Format_RGB32);
            image.fill(id == 1 ? QColor("#87593b") : previewColor);
            QVERIFY(image.save(&buffer, "PNG"));
            backend->messageImages.insert(file.previewUrl, bytes);
        }
        const auto photo = message.files.last();
        MainWindow window(backend.get());
        window.resize(1000, 760);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        QTRY_COMPARE(backend->imageRequests.count(photo.previewUrl), 1);
        const auto pointInPreview = [&] {
            const auto pixels = history->viewport()->grab().toImage();
            for (int y = 0; y < pixels.height(); ++y)
                for (int x = 0; x < pixels.width(); ++x)
                    if (pixels.pixelColor(x, y) == previewColor)
                        return QPoint(x, y);
            return QPoint();
        };
        QTRY_VERIFY(!pointInPreview().isNull());
        QWidget *target = history->viewport();
        auto point = pointInPreview();
        const auto openContext = [](QWidget *widget, const QPoint &pos) {
            QContextMenuEvent event(QContextMenuEvent::Mouse, pos, widget->mapToGlobal(pos));
            QCoreApplication::sendEvent(widget, &event);
        };
        if (viewer) {
            QTest::mouseClick(target, Qt::LeftButton, Qt::NoModifier, point);
            auto *dialog = window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer"));
            QVERIFY(dialog);
            if (hasOriginal) {
                auto *state = findAccessible<QLabel>(dialog, QStringLiteral("Состояние загрузки фотографии"));
                QVERIFY(state && state->isVisible());
                openContext(state, state->rect().center());
                QTRY_VERIFY(visibleMessageMenu(QStringLiteral("imageContextMenu")));
                auto *loadingMenu = visibleMessageMenu(QStringLiteral("imageContextMenu"));
                QVERIFY(loadingMenu->actions().first()->isEnabled());
                QVERIFY(!loadingMenu->actions().last()->isEnabled());
                loadingMenu->close();
                QCOMPARE(backend->photoUrl, photo.downloadUrl);
                QImage original(originalSize, QImage::Format_RGB32);
                original.fill(viewerColor);
                QVERIFY(original.save(backend->photoPath, "PNG"));
                emit backend->messageFileDownloaded(backend->photoUrl, backend->photoPath);
            }
            target = dialog->findChild<QWidget *>(QStringLiteral("chatPhotoCanvas"));
            QTRY_VERIFY(target && target->isVisible());
            auto *zoom = findAccessible<QPushButton>(dialog, QStringLiteral("Увеличить фотографию"));
            QVERIFY(zoom && zoom->isEnabled());
            zoom->click();
            point = target->rect().center();
        } else if (hasOriginal) {
            const auto row = history->visualItemRect(history->item(0));
            openContext(target, QPoint(row.right() - 5, row.top() + 12));
            QTRY_VERIFY(visibleMessageMenu());
            visibleMessageMenu()->close();
        }
        auto *clipboard = QGuiApplication::clipboard();
        clipboard->setText(QStringLiteral("clipboard sentinel"));
        openContext(target, point);
        QTRY_VERIFY(visibleMessageMenu(QStringLiteral("imageContextMenu")));
        auto *menu = visibleMessageMenu(QStringLiteral("imageContextMenu"));
        QCOMPARE(menu->actions().size(), 2);
        QCOMPARE(menu->actions().first()->text(), QStringLiteral("Скачать изображение"));
        QCOMPARE(menu->actions().last()->text(), QStringLiteral("Скопировать изображение"));
        QVERIFY(menu->actions().last()->isEnabled());
        menu->actions().last()->trigger();
        QTRY_VERIFY(!visibleMessageMenu(QStringLiteral("imageContextMenu")));
        const auto pixels = clipboard->image();
        const bool showOriginal = viewer && hasOriginal;
        QCOMPARE(pixels.size(), showOriginal ? originalSize : previewSize);
        QCOMPARE(pixels.pixelColor(3, 3), showOriginal ? viewerColor : previewColor);
        openContext(target, point);
        QTRY_VERIFY(visibleMessageMenu(QStringLiteral("imageContextMenu")));
        menu = visibleMessageMenu(QStringLiteral("imageContextMenu"));
        QVERIFY(menu->actions().first()->isEnabled());
        menu->actions().first()->trigger();
        auto *save = window.findChild<QFileDialog *>(QStringLiteral("messageFileSaveDialog"));
        QTRY_VERIFY(save && save->isVisible());
        QCOMPARE(QFileInfo(save->selectedFiles().value(0)).fileName(),
                 hasOriginal ? photo.name : QStringLiteral("Фото 2.png"));
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
#ifdef Q_OS_UNIX
        if (!hasOriginal) {
            QFile protectedFile(folder.filePath(QStringLiteral("protected.txt")));
            QVERIFY(protectedFile.open(QIODevice::WriteOnly));
            QCOMPARE(protectedFile.write("keep this file"), qint64(14));
            protectedFile.close();
            const auto link = folder.filePath(QStringLiteral("symlink.png"));
            QVERIFY(QFile::link(protectedFile.fileName(), link));
            QVERIFY(QFileInfo(link).isSymLink());
            QVERIFY(QMetaObject::invokeMethod(save, "fileSelected", Qt::DirectConnection, Q_ARG(QString, link)));
            QVERIFY(QFileInfo(link).isSymLink());
            QVERIFY(protectedFile.open(QIODevice::ReadOnly));
            QCOMPARE(protectedFile.readAll(), QByteArray("keep this file"));
        }
#endif
        const auto path = folder.filePath(hasOriginal ? QStringLiteral("original.jpg") : QStringLiteral("preview.png"));
        save->selectFile(path);
        QVERIFY(QMetaObject::invokeMethod(save, "accept"));
        if (hasOriginal) {
            QTRY_COMPARE(backend->downloadedUrl, photo.downloadUrl);
            QCOMPARE(backend->downloadedPath, path);
        } else {
            QTRY_VERIFY(QFileInfo::exists(path));
            QVERIFY(backend->downloadedUrl.isEmpty());
            const QImage saved(path);
            QCOMPARE(saved.size(), previewSize);
            QCOMPARE(saved.pixelColor(3, 3), previewColor);
        }
    }

    void chatPreviewsLoadOnlyNearVisibleRowsAndShowInsideQuotes()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        const QUrl visible(QStringLiteral("https://example.bitrix24.ru/preview/2"));
        const QUrl distant(QStringLiteral("https://example.bitrix24.ru/preview/1"));
        const QUrl unavailable(QStringLiteral("https://example.bitrix24.ru/preview/3"));
        const QUrl omittedQuotePreview(QStringLiteral("https://example.bitrix24.ru/preview/4"));
        const QColor photoColor("#b85038");
        QByteArray photoBytes;
        {
            QBuffer buffer(&photoBytes);
            QVERIFY(buffer.open(QIODevice::WriteOnly));
            QImage photo(240, 145, QImage::Format_ARGB32);
            photo.fill(photoColor);
            QVERIFY(photo.save(&buffer, "PNG"));
        }
        backend->messageImages.insert(visible, photoBytes);
        backend->messageImages.insert(distant, photoBytes);

        auto makeFile = [](QString id, const QUrl &preview) {
            MessageFile file;
            file.id = id;
            file.name = QStringLiteral("Образец %1.png").arg(id);
            file.size = 3500;
            file.downloadUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/download/%1").arg(id));
            file.previewUrl = preview;
            file.previewSize = QSize(240, 145);
            return file;
        };
        auto makeMessage = [](QString id, QString text, bool outgoing = false) {
            Message message;
            message.id = id;
            message.author = outgoing ? QStringLiteral("Вы") : QStringLiteral("Анна");
            message.text = text;
            message.outgoing = outgoing;
            return message;
        };
        auto top = makeMessage(QStringLiteral("1"), QStringLiteral("Старая фотография"));
        top.files = {makeFile(QStringLiteral("1"), distant)};
        backend->messages = {top};
        for (int row = 0; row < 28; ++row)
            backend->messages.append(makeMessage(QString::number(10 + row), QStringLiteral("Сообщение в истории")));
        auto noPreview = makeMessage(QStringLiteral("90"), QStringLiteral("Файл остаётся доступен"));
        noPreview.files = {makeFile(QStringLiteral("3"), unavailable)};
        backend->messages.append(noPreview);
        auto own = makeMessage(QStringLiteral("91"), QStringLiteral("Мой снимок"), true);
        own.files = {makeFile(QStringLiteral("2"), visible)};
        backend->messages.append(own);
        auto quoted = makeMessage(QStringLiteral("92"), QStringLiteral("Спасибо за снимок"));
        quoted.replyId = own.id;
        quoted.replyFiles = {own.files.front(), makeFile(QStringLiteral("4"), omittedQuotePreview)};
        quoted.forwarded = true;
        quoted.forwardedAuthor = QStringLiteral("Михаил");
        backend->messages.append(quoted);

        MainWindow window(backend.get());
        window.resize(960, 790);
        window.show();
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        QTRY_COMPARE(backend->imageRequests.count(visible), 1);
        QTRY_COMPARE(backend->imageRequests.count(unavailable), 1);
        QCOMPARE(backend->imageRequests.count(distant), 0);
        QCOMPARE(backend->imageRequests.count(omittedQuotePreview), 0);
        const auto *last = history->item(history->count() - 1);
        const auto *previous = history->item(history->count() - 2);
        QTRY_VERIFY(history->visualItemRect(last).height() < 165);
        QTRY_VERIFY(history->visualItemRect(previous).height() > 180);
        QCOMPARE(last->data(workspace::ReplyFiles).value<QVector<MessageFile>>().front().previewUrl, visible);

        const auto capture = qEnvironmentVariable("B24_CHAT_PREVIEW_SCREENSHOT");
        if (!capture.isEmpty())
            QVERIFY(window.grab().save(capture));
        const auto photoPixel = [photoColor, history](const QRect &row) {
            const auto screenshot = history->viewport()->grab().toImage();
            const auto area = row.intersected(screenshot.rect());
            for (int y = area.top(); y <= area.bottom(); ++y)
                for (int x = area.left(); x <= area.right(); ++x)
                    if (screenshot.pixelColor(x, y) == photoColor)
                        return QPoint(x, y);
            return QPoint();
        };
        const auto quotedPhoto = photoPixel(history->visualItemRect(last));
        QVERIFY(!quotedPhoto.isNull());
        QCOMPARE(backend->photoDownloads, 0);
        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, quotedPhoto);
        QTRY_COMPARE(history->currentItem(), previous);
        QVERIFY(!window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));
        QCOMPARE(backend->photoDownloads, 0);
        const auto ownPhoto = photoPixel(history->visualItemRect(previous));
        QVERIFY(!ownPhoto.isNull());
        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, ownPhoto);
        QTRY_VERIFY(window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));
        auto *viewer = window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer"));
        QCOMPARE(viewer->parentWidget(), static_cast<QWidget *>(&window));
        const auto *selectedBeforeViewerKey = history->currentItem();
        QTest::keyClick(history, Qt::Key_Up);
        QCOMPARE(history->currentItem(), selectedBeforeViewerKey);
        QVERIFY(viewer->isVisible());
        auto *originalState = findAccessible<QLabel>(viewer, QStringLiteral("Состояние загрузки фотографии"));
        QVERIFY(originalState && originalState->isVisible());
        QCOMPARE(originalState->text(), QStringLiteral("Загружаем оригинал фотографии…"));
        QCOMPARE(backend->photoDownloads, 1);
        QCOMPARE(backend->photoUrl, own.files.front().downloadUrl);
        auto *zoom = findAccessible<QPushButton>(viewer, QStringLiteral("Увеличить фотографию"));
        auto *fit = findAccessible<QPushButton>(viewer, QStringLiteral("Вписать фотографию в окно"));
        auto *scale = findAccessible<QLabel>(viewer, QStringLiteral("Масштаб фотографии"));
        auto *canvas = viewer->findChild<QWidget *>(QStringLiteral("chatPhotoCanvas"));
        QVERIFY(zoom && fit && scale && canvas);
        QVERIFY(!canvas->isVisible() && !zoom->isEnabled());
        const auto firstTarget = backend->photoPath;
        const auto originalDir = QFileInfo(firstTarget).absolutePath();
        QVERIFY(QFileInfo::exists(originalDir));
        const QColor originalColor("#33688c");
        const QSize originalSize(1440, 1000);
        {
            QFile file(firstTarget);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QImage original(originalSize, QImage::Format_RGB32);
            original.fill(originalColor);
            QVERIFY(original.save(&file, "PNG"));
        }
        emit backend->messageFileDownloaded(backend->photoUrl, firstTarget);
        QTRY_VERIFY(canvas->isVisible() && !originalState->isVisible());
        QCOMPARE(static_cast<ui::ChatPhotoCanvas *>(canvas)->imageSize(), originalSize);
        const auto originalScreenshot = canvas->grab().toImage();
        QCOMPARE(originalScreenshot.pixelColor(originalScreenshot.rect().center()), originalColor);
        QTRY_VERIFY(!QFileInfo::exists(originalDir));
        QCOMPARE(scale->text(), QStringLiteral("×1.00"));
        QTest::mouseClick(zoom, Qt::LeftButton);
        QCOMPARE(scale->text(), QStringLiteral("×1.25"));
        const auto center = canvas->rect().center();
        QWheelEvent wheel(center, canvas->mapToGlobal(center), {}, QPoint(0, 120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(canvas, &wheel);
        QCOMPARE(scale->text(), QStringLiteral("×1.56"));
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, center);
        QCOMPARE(canvas->cursor().shape(), Qt::ClosedHandCursor);
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, center);
        QTest::mouseClick(fit, Qt::LeftButton);
        QCOMPARE(scale->text(), QStringLiteral("×1.00"));
        const auto viewerCapture = qEnvironmentVariable("B24_CHAT_PHOTO_VIEWER_SCREENSHOT");
        if (!viewerCapture.isEmpty())
            QVERIFY(viewer->grab().save(viewerCapture));
        QTest::keyClick(viewer, Qt::Key_Escape);
        QTRY_VERIFY(!window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));
        QVERIFY(backend->downloadedUrl.isEmpty());
        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, ownPhoto);
        QTRY_VERIFY(window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));
        viewer = window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer"));
        QCOMPARE(viewer->windowTitle(), own.files.front().name);
        QCOMPARE(backend->photoDownloads, 2);
        QCOMPARE(backend->photoUrl, own.files.front().downloadUrl);
        const auto canceledPath = backend->photoPath;
        auto *save = findAccessible<QPushButton>(viewer, QStringLiteral("Сохранить оригинал фотографии"));
        QVERIFY(save);
        QTest::mouseClick(save, Qt::LeftButton);
        QTRY_VERIFY(viewer->findChild<QFileDialog *>(QStringLiteral("messageFileSaveDialog")));
        viewer->findChild<QFileDialog *>(QStringLiteral("messageFileSaveDialog"))->reject();
        QTest::keyClick(viewer, Qt::Key_Escape);
        QTRY_VERIFY(!window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));
        QVERIFY(backend->downloadedUrl.isEmpty());
        QCOMPARE(backend->photoCancels, QStringList{canceledPath});
        QVERIFY(!QFileInfo::exists(QFileInfo(canceledPath).absolutePath()));
        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, ownPhoto);
        QTRY_VERIFY(window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));
        viewer = window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer"));
        originalState = findAccessible<QLabel>(viewer, QStringLiteral("Состояние загрузки фотографии"));
        QVERIFY(originalState && originalState->isVisible());
        emit backend->messagePhotoDownloadFailed(canceledPath, QStringLiteral("Устаревший ответ"));
        QCOMPARE(originalState->text(), QStringLiteral("Загружаем оригинал фотографии…"));
        emit backend->messagePhotoDownloadFailed(backend->photoPath, QStringLiteral("Сбой загрузки оригинала"));
        QCOMPARE(originalState->text(), QStringLiteral("Сбой загрузки оригинала"));
        QVERIFY(!viewer->findChild<QWidget *>(QStringLiteral("chatPhotoCanvas"))->isVisible());
        QVERIFY(findAccessible<QPushButton>(viewer, QStringLiteral("Сохранить оригинал фотографии")));
        QVERIFY(!QFileInfo::exists(QFileInfo(backend->photoPath).absolutePath()));
        QTest::keyClick(viewer, Qt::Key_Escape);
        QTRY_VERIFY(!window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));

        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, ownPhoto);
        QTRY_VERIFY(window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));
        viewer = window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer"));
        auto *previousPhoto = findAccessible<QPushButton>(viewer, QStringLiteral("Предыдущая фотография"));
        auto *nextPhoto = findAccessible<QPushButton>(viewer, QStringLiteral("Следующая фотография"));
        auto *position = findAccessible<QLabel>(viewer, QStringLiteral("Номер фотографии"));
        QVERIFY(previousPhoto && nextPhoto && position);
        QCOMPARE(position->text(), QStringLiteral("3 из 3"));
        QVERIFY(previousPhoto->isEnabled() && !nextPhoto->isEnabled());
        QCOMPARE(backend->photoDownloads, 4);
        const auto quoteTarget = backend->photoPath;

        QVERIFY(QTest::qWaitForWindowExposed(viewer));
        QApplication::setActiveWindow(viewer);
        QTest::keyClick(viewer, Qt::Key_Left);
        QTRY_COMPARE(viewer->windowTitle(), noPreview.files.front().name);
        QCOMPARE(position->text(), QStringLiteral("2 из 3"));
        QCOMPARE(backend->photoUrl, noPreview.files.front().downloadUrl);
        QCOMPARE(backend->photoDownloads, 5);
        QVERIFY(backend->photoCancels.contains(quoteTarget));
        originalState = findAccessible<QLabel>(viewer, QStringLiteral("Состояние загрузки фотографии"));
        emit backend->messagePhotoDownloadFailed(quoteTarget, QStringLiteral("Устаревший ответ"));
        QCOMPARE(originalState->text(), QStringLiteral("Загружаем оригинал фотографии…"));

        QTest::mouseClick(previousPhoto, Qt::LeftButton);
        QCOMPARE(position->text(), QStringLiteral("1 из 3"));
        QCOMPARE(backend->photoUrl, top.files.front().downloadUrl);
        QCOMPARE(backend->photoDownloads, 6);
        QVERIFY(!previousPhoto->isEnabled() && nextPhoto->isEnabled());
        QTest::keyClick(viewer, Qt::Key_Left);
        QCOMPARE(backend->photoDownloads, 6);
        QTest::mouseClick(nextPhoto, Qt::LeftButton);
        QCOMPARE(backend->photoUrl, noPreview.files.front().downloadUrl);
        QCOMPARE(backend->photoDownloads, 7);
        QTest::keyClick(viewer, Qt::Key_Escape);
        QTRY_VERIFY(!window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));

        history->scrollToTop();
        QTRY_COMPARE(backend->imageRequests.count(distant), 1);
        emit backend->chatMessagesLoaded(QStringLiteral("c-1"), backend->messages);
        QCOMPARE(backend->imageRequests.count(visible), 1);
        QCOMPARE(backend->imageRequests.count(unavailable), 1);

        auto taskMessage = makeMessage(QStringLiteral("601"), QStringLiteral("Фото задачи"));
        taskMessage.files = {makeFile(QStringLiteral("task-1"), distant),
                             makeFile(QStringLiteral("task-2"), visible)};
        for (auto &file : taskMessage.files)
            file.isImage = true;
        backend->taskMessages = {taskMessage};
        window.openTaskCard(QStringLiteral("501"));
        auto *task = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(task);
        auto *taskHistory = findAccessible<QListWidget>(task, QStringLiteral("Чат карточки"));
        QVERIFY(taskHistory);
        QTRY_COMPARE(taskHistory->count(), 1);
        taskHistory->setCurrentRow(0);
        QTest::keyClick(taskHistory, Qt::Key_Return);
        QTRY_VERIFY(task->findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));
        viewer = task->findChild<QDialog *>(QStringLiteral("chatPhotoViewer"));
        position = findAccessible<QLabel>(viewer, QStringLiteral("Номер фотографии"));
        QVERIFY(position);
        QCOMPARE(position->text(), QStringLiteral("1 из 2"));
        QCOMPARE(backend->photoUrl, taskMessage.files.front().downloadUrl);
        QVERIFY(QTest::qWaitForWindowExposed(viewer));
        QApplication::setActiveWindow(viewer);
        QTest::keyClick(viewer, Qt::Key_Right);
        QTRY_COMPARE(position->text(), QStringLiteral("2 из 2"));
        QCOMPARE(backend->photoUrl, taskMessage.files.back().downloadUrl);
    }

    void opensMediaPhotosInDisplayedOrderAndSavesTheCurrentOriginal()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        const auto makeAsset = [](const QString &id, const QString &name) {
            ChatAsset asset;
            asset.id = id;
            asset.name = name;
            asset.file.id = id;
            asset.file.name = name;
            asset.file.isImage = true;
            asset.file.previewUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/preview/shared"));
            asset.file.downloadUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/download/%1").arg(id));
            return asset;
        };
        const auto first = makeAsset(QStringLiteral("7"), QStringLiteral("Первое.png"));
        const auto second = makeAsset(QStringLiteral("1"), QStringLiteral("Второе.png"));
        const auto last = makeAsset(QStringLiteral("3"), QStringLiteral("Третье.png"));
        auto preview = makeAsset(QStringLiteral("4"), QStringLiteral("Миниатюра.png"));
        preview.file.downloadUrl.clear();
        preview.file.previewUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/preview/4"));
        QByteArray bytes;
        QBuffer buffer(&bytes);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QImage thumbnail(60, 40, QImage::Format_RGB32);
        thumbnail.fill(Qt::blue);
        QVERIFY(thumbnail.save(&buffer, "PNG"));
        backend->messageImages.insert(preview.file.previewUrl, bytes);
        backend->mediaAssets = {first, second, last, preview};
        MainWindow window(backend.get());
        window.show();
        QApplication::setActiveWindow(&window);
        emit backend->connectionChanged(true);
        auto *assets = findAccessible<QPushButton>(&window, QStringLiteral("Медиа, файлы и ссылки чата"));
        QVERIFY(assets);
        assets->click();
        auto *media = findAccessible<QListWidget>(&window, QStringLiteral("Медиа"));
        QVERIFY(media);
        QTRY_COMPARE(media->count(), 4);
        media->setCurrentRow(1);
        QTest::keyClick(media, Qt::Key_Return);
        QTRY_VERIFY(window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));
        auto *viewer = window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer"));
        auto *position = findAccessible<QLabel>(viewer, QStringLiteral("Номер фотографии"));
        auto *next = findAccessible<QPushButton>(viewer, QStringLiteral("Следующая фотография"));
        auto *save = findAccessible<QPushButton>(viewer, QStringLiteral("Сохранить оригинал фотографии"));
        QVERIFY(position && next && save);
        QCOMPARE(position->text(), QStringLiteral("2 из 4"));
        QCOMPARE(backend->photoUrl, second.file.downloadUrl);
        next->click();
        QCOMPARE(position->text(), QStringLiteral("3 из 4"));
        QCOMPARE(viewer->windowTitle(), last.file.name);
        QCOMPARE(backend->photoUrl, last.file.downloadUrl);
        QVERIFY(next->isEnabled());
        save->click();
        auto *fileDialog = viewer->findChild<QFileDialog *>(QStringLiteral("messageFileSaveDialog"));
        QVERIFY(fileDialog);
        QCOMPARE(QFileInfo(fileDialog->selectedFiles().value(0)).fileName(), last.file.name);
        fileDialog->reject();
        QVERIFY(!backend->imageRequests.contains(preview.file.previewUrl));
        const auto downloads = backend->photoDownloads;
        next->click();
        QCOMPARE(position->text(), QStringLiteral("4 из 4"));
        QCOMPARE(backend->photoDownloads, downloads);
        QVERIFY(!next->isEnabled() && !save->isVisible());
        auto *canvas = viewer->findChild<QWidget *>(QStringLiteral("chatPhotoCanvas"));
        QVERIFY(canvas);
        QTRY_VERIFY(canvas->isVisible());
        QCOMPARE(backend->imageRequests.count(preview.file.previewUrl), 1);
        auto *previous = findAccessible<QPushButton>(viewer, QStringLiteral("Предыдущая фотография"));
        QVERIFY(previous);
        const auto center = canvas->rect().center();
        QContextMenuEvent photoMenuEvent(QContextMenuEvent::Mouse, center, canvas->mapToGlobal(center));
        QCoreApplication::sendEvent(canvas, &photoMenuEvent);
        QTRY_VERIFY(visibleMessageMenu(QStringLiteral("imageContextMenu")));
        const QPointer<QAction> oldCopy(visibleMessageMenu(QStringLiteral("imageContextMenu"))->actions().last());
        auto *clipboard = QGuiApplication::clipboard();
        clipboard->setText(QStringLiteral("previous frame sentinel"));
        previous->click();
        QVERIFY(!visibleMessageMenu(QStringLiteral("imageContextMenu")));
        if (oldCopy)
            oldCopy->trigger();
        QCOMPARE(clipboard->text(), QStringLiteral("previous frame sentinel"));
        QCOMPARE(backend->photoUrl, last.file.downloadUrl);
        const auto canceled = backend->photoPath;
        backend->accountScope = QStringLiteral("fixture/photo-account-changed");
        emit backend->accountScopeChanged();
        QTRY_VERIFY(!window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));
        QVERIFY(backend->photoCancels.contains(canceled));
    }

    void ownAndIncomingMessagesShareLeftAlignmentAndKeepDifferentColors()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto own = backend->messages.front();
        own.id = QStringLiteral("2");
        own.outgoing = true;
        backend->messages.append(own);
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        QTRY_COMPARE(history->count(), 2);
        QCoreApplication::processEvents();
        const auto screenshot = history->viewport()->grab().toImage();
        const auto firstBubblePixel = [&screenshot](const QRect &row, const QColor &color) {
            const auto area = row.intersected(screenshot.rect());
            for (int x = area.left(); x <= area.right(); ++x)
                for (int y = area.top(); y <= area.bottom(); ++y)
                    if (screenshot.pixelColor(x, y) == color)
                        return x;
            return -1;
        };
        const int incoming = firstBubblePixel(history->visualItemRect(history->item(0)), workspace::Surface);
        const int outgoing = firstBubblePixel(history->visualItemRect(history->item(1)), QColor("#c8e7d8"));
        QVERIFY(incoming >= 0 && outgoing >= 0);
        QVERIFY(qAbs(incoming - outgoing) <= 1);
        QVERIFY(outgoing < history->viewport()->width() / 2);
    }

    void unreadBoundaryAnchorsHistoryAndStaysOutOfSearch_data()
    {
        QTest::addColumn<bool>("task");
        QTest::newRow("chat") << false;
        QTest::newRow("task-chat") << true;
    }

    void messageActionsUseTheAdjacentGutter_data()
    {
        QTest::addColumn<bool>("task");
        QTest::newRow("chat") << false;
        QTest::newRow("task-chat") << true;
    }

    void messageActionsUseTheAdjacentGutter()
    {
        QFETCH(bool, task);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        Message message{QStringLiteral("701"), QStringLiteral("Анна"), QStringLiteral("Короткое сообщение"),
                        QDateTime::currentDateTime()};
        backend->messages = backend->taskMessages = {message};
        backend->stages.first().tasks.first().chatDialogId = QStringLiteral("chat73");
        MainWindow window(backend.get());
        window.resize(810, 620);
        window.show();
        QWidget *owner = &window;
        if (task) {
            window.openTaskCard(QStringLiteral("501"));
            owner = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
            QVERIFY(owner);
            owner->resize(680, 500);
        }
        auto *history = findAccessible<QListWidget>(owner, task ? QStringLiteral("Чат карточки")
                                                               : QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        auto *more = findAccessible<QPushButton>(history, QStringLiteral("Действия сообщения"));
        QVERIFY(more);
        QTRY_COMPARE(history->count(), 1);
        QCoreApplication::processEvents();
        const auto index = history->model()->index(0, 0);
        const auto *painter = static_cast<const workspace::RowPainter *>(history->itemDelegate());
        const auto bubble = painter->receiptArea(index, history->visualRect(index));
        const auto gutter = QPoint(bubble.right() + 43, bubble.top() + 10);
        QVERIFY(history->viewport()->rect().contains(gutter));
        const auto move = [history](const QPoint &point) {
            auto *viewport = history->viewport();
            QMouseEvent event(QEvent::MouseMove, point, viewport->mapToGlobal(point),
                              Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(viewport, &event);
        };
        move(gutter);
        QTRY_VERIFY(more->isVisible());
        QVERIFY(history->viewport()->rect().contains(more->geometry()));
        QVERIFY(!bubble.intersects(more->geometry()));
        move(QPoint(gutter.x(), bubble.top() - 1));
        QVERIFY(more->isHidden());
    }

    void messageMenuKeepsTheTargetThroughRefresh()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        const auto dialog = backend->chats.first().id;
        const Message target{QStringLiteral("701"), QStringLiteral("Анна"), QStringLiteral("Первая строка"),
                             QDateTime::currentDateTime()};
        const Message next{QStringLiteral("702"), QStringLiteral("Михаил"), QStringLiteral("Новое сообщение"),
                           QDateTime::currentDateTime().addSecs(1)};
        backend->messages = {target};
        MainWindow window(backend.get());
        window.resize(800, 620);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *quote = findAccessible<QLabel>(&window, QStringLiteral("Цитата в черновике"));
        QVERIFY(history && quote);
        QTRY_COMPARE(history->count(), 1);
        const auto open = [history] {
            history->setCurrentRow(0);
            const auto point = history->visualItemRect(history->item(0)).center();
            QContextMenuEvent event(QContextMenuEvent::Keyboard, point, history->viewport()->mapToGlobal(point));
            QApplication::sendEvent(history->viewport(), &event);
            return QPointer<QMenu>(visibleMessageMenu());
        };
        auto menu = open();
        QVERIFY(menu && menu->isVisible());
        emit backend->chatMessagesLoaded(dialog, QVector<Message>{target, next});
        QTRY_COMPARE(history->count(), 2);
        QVERIFY(menu && menu->isVisible());
        QCOMPARE(menu->actions().first()->text(), QStringLiteral("Ответить"));
        menu->setActiveAction(menu->actions().first());
        QTest::keyClick(menu, Qt::Key_Return);
        QTRY_VERIFY(quote->isVisible());
        QVERIFY(quote->text().contains(target.text));
        QTRY_VERIFY(!menu || !menu->isVisible());

        menu = open();
        QVERIFY(menu && menu->isVisible());
        emit backend->chatMessagesLoaded(dialog, QVector<Message>{next});
        QTRY_VERIFY(!menu || !menu->isVisible());
    }

    void messageMenuOpensAboveOutsideThePointerAndReleasesAppSwitch()
    {
        QWidget window;
        auto *layout = new QVBoxLayout(&window);
        auto *history = new ui::HistoryList(&window);
        workspace::configureRows(history, workspace::Row::Message);
        layout->addWidget(history);
        QLabel state(&window);
        int selected = 0;
        history->setMessageMenu([&selected](QMenu &menu, const QPersistentModelIndex &, const QString &) {
            menu.addAction(QStringLiteral("Действие"), [&selected] { ++selected; });
            auto *submenu = menu.addMenu(QStringLiteral("Ещё"));
            submenu->addAction(QStringLiteral("Выбор"), [&selected] { ++selected; });
        });
        const auto screen = QGuiApplication::primaryScreen()->availableGeometry();
        window.resize(350, 220);
        window.move(screen.left() + 20, screen.bottom() - window.height() - 3);
        window.show();
        QApplication::setActiveWindow(&window);
        history->showHistory(QStringLiteral("chat73"), &state, {{QStringLiteral("701"), QStringLiteral("Анна"),
                             QStringLiteral("Сообщение"), QDateTime::currentDateTime()}});
        QCoreApplication::processEvents();
        const auto point = history->visualItemRect(history->item(0)).bottomLeft() + QPoint(40, -2);
        const auto global = history->viewport()->mapToGlobal(point);
        QContextMenuEvent event(QContextMenuEvent::Mouse, point, global);
        QApplication::sendEvent(history->viewport(), &event);
        const auto menu = QPointer<QMenu>(history->findChild<QMenu *>(QStringLiteral("messageContextMenu")));
        QVERIFY(menu && menu->isVisible());
        QVERIFY(menu->geometry().bottom() < global.y());
        QVERIFY(screen.contains(menu->geometry()));
        QVERIFY(menu->windowType() != Qt::Popup);
        QVERIFY(menu->actions().last()->menu()->windowType() != Qt::Popup);
        QVERIFY(!QApplication::activePopupWidget());
        const auto local = menu->mapFromGlobal(global);
        for (const auto kind : {QEvent::MouseMove, QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            const auto button = kind == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
            QMouseEvent mouse(kind, local, global, button, button, Qt::NoModifier);
            QApplication::sendEvent(menu, &mouse);
        }
        QVERIFY(!menu->activeAction());
        QCOMPARE(selected, 0);
        QTest::keyClick(menu, Qt::Key_Escape);
        QTRY_VERIFY(!menu || !menu->isVisible());
    }

    void copiesTheExactClickedMessageLink_data()
    {
        QTest::addColumn<bool>("task");
        QTest::newRow("ordinary-chat") << false;
        QTest::newRow("task-chat") << true;
    }

    void copiesTheExactClickedMessageLink()
    {
        QFETCH(bool, task);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        const auto relative = QStringLiteral("/tasks/task/view/41/?probe=x%2Fy#fragment");
        const auto local = QStringLiteral("https://example.bitrix24.ru") + relative;
        const auto publicUrl = QStringLiteral("https://public.example.com/path?token=first%2Fkeep&display=full#target");
        Message message{QStringLiteral("701"), QStringLiteral("Анна"),
                        QStringLiteral("[URL=%1]Задача[/URL] и [URL=%2]Внешняя ссылка[/URL]").arg(relative, publicUrl),
                        QDateTime::currentDateTime()};
        backend->messages = backend->taskMessages = {message};
        const auto taskDialog = QStringLiteral("chat73");
        backend->stages.first().tasks.first().chatDialogId = taskDialog;
        MainWindow window(backend.get(), true);
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QWidget *pane = &window;
        if (task) {
            window.openTaskCard(QStringLiteral("501"));
            pane = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
            QVERIFY(pane);
        }
        auto *list = findAccessible<QListWidget>(pane, task ? QStringLiteral("Чат карточки")
                                                           : QStringLiteral("Сообщения чата"));
        QVERIFY(list);
        QTRY_COMPARE(list->count(), 1);
        const auto index = list->model()->index(0, 0);
        const auto *painter = static_cast<const workspace::RowPainter *>(list->itemDelegate());
        const auto findPoint = [list, painter, index](const QString &href, bool card) {
            const auto row = list->visualRect(index);
            const auto visible = row.intersected(list->viewport()->rect());
            for (int y = visible.top(); y <= visible.bottom(); y += 2)
                for (int x = visible.left(); x <= visible.right(); x += 2) {
                    const QPoint point(x, y);
                    const auto anchor = painter->anchorAt(index, row, point);
                    if (card ? anchor.isEmpty() && painter->linkAt(index, row, point) == href : anchor == href)
                        return point;
                }
            return QPoint();
        };
        const auto findAction = [](QMenu *menu, const QString &name) -> QAction * {
            for (auto *action : menu->actions())
                if (action->text() == name)
                    return action;
            return nullptr;
        };
        const auto menuName = QStringLiteral("messageContextMenu");
        const auto copyName = QStringLiteral("Скопировать ссылку");
        const auto openMenu = [list](const QPoint &point, QContextMenuEvent::Reason reason) {
            QContextMenuEvent event(reason, point, list->viewport()->mapToGlobal(point));
            QCoreApplication::sendEvent(list->viewport(), &event);
        };
        for (const auto &[href, expected, card] : {std::tuple{relative, local, false}, std::tuple{publicUrl, publicUrl, true}}) {
            const auto point = findPoint(href, card);
            QVERIFY(!point.isNull());
            openMenu(point, QContextMenuEvent::Mouse);
            auto *menu = list->findChild<QMenu *>(menuName);
            QVERIFY(menu);
            auto *copy = findAction(menu, copyName);
            QVERIFY(copy && !copy->menu() && copy->isEnabled());
            copy->trigger();
            QCOMPARE(QGuiApplication::clipboard()->text(), expected);
            menu->close();
            QTRY_VERIFY(!list->findChild<QMenu *>(menuName));
        }

        list->setCurrentRow(0);
        openMenu(list->visualRect(index).center(), QContextMenuEvent::Keyboard);
        auto *menu = list->findChild<QMenu *>(menuName);
        QVERIFY(menu);
        auto *links = findAction(menu, copyName);
        QVERIFY(links && links->menu());
        QCOMPARE(links->menu()->actions().size(), 2);
        links->menu()->actions().at(0)->trigger();
        QCOMPARE(QGuiApplication::clipboard()->text(), local);
        links->menu()->actions().at(1)->trigger();
        QCOMPARE(QGuiApplication::clipboard()->text(), publicUrl);
        auto *permalink = findAction(menu, QStringLiteral("Скопировать ссылку на сообщение"));
        QVERIFY(permalink);
        permalink->trigger();
        const QUrl reference(QGuiApplication::clipboard()->text());
        QCOMPARE(reference.path(), QStringLiteral("/online/"));
        QCOMPARE(QUrlQuery(reference).queryItemValue(QStringLiteral("IM_MESSAGE")), message.id);
        QCOMPARE(QUrlQuery(reference).queryItemValue(task ? QStringLiteral("IM_TASK") : QStringLiteral("IM_DIALOG")),
                 task ? taskDialog : backend->chats.first().id);
        menu->close();
    }

    void historyKeepsTheViewportWhenLinkPreviewsGrow_data()
    {
        QTest::addColumn<bool>("task");
        QTest::newRow("ordinary-chat") << false;
        QTest::newRow("task-chat") << true;
    }

    void historyKeepsTheViewportWhenLinkPreviewsGrow()
    {
        QFETCH(bool, task);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdLinkPreviews = true;
        auto &messages = task ? backend->taskMessages : backend->messages;
        messages.clear();
        const auto now = QDateTime::currentDateTime();
        for (int i = 0; i < 20; ++i) {
            Message message{QString::number(701 + i), QStringLiteral("Сергей"),
                            QStringLiteral("Сообщение %1").arg(i), now.addSecs((i - 20) * 360)};
            if (i == 3 || i == 16)
                message.text = QStringLiteral("https://example.bitrix24.ru/tasks/task/view/%1/").arg(i == 3 ? 52 : 53);
            messages.append(message);
        }
        MainWindow window(backend.get(), true);
        window.resize(960, 600);
        window.show();
        QApplication::setActiveWindow(&window);
        QWidget *pane = &window;
        if (task) {
            window.openTaskCard(QStringLiteral("501"));
            pane = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
            QVERIFY(pane);
        }
        auto *list = findAccessible<QListWidget>(pane, task ? QStringLiteral("Чат карточки")
                                                           : QStringLiteral("Сообщения чата"));
        QVERIFY(list);
        QTRY_COMPARE(list->count(), 20);
        const auto take = [&backend](const QString &path) {
            for (auto &request : backend->linkPreviewRequests)
                if (request.first.path() == path && request.second)
                    return std::exchange(request.second, {});
            return Backend::LinkPreviewHandler{};
        };
        Backend::LinkPreviewHandler latestPreview;
        QTRY_VERIFY(latestPreview || (latestPreview = take(QStringLiteral("/tasks/task/view/53/"))));
        auto *scroll = list->verticalScrollBar();
        QVERIFY(scroll->maximum() > 0);
        QCOMPARE(scroll->value(), scroll->maximum());
        const auto preview = MessageLinkPreview{QStringLiteral("Карточка задачи"),
            {QStringLiteral("Статус"), QStringLiteral("Постановщик"), QStringLiteral("Исполнитель"),
             QStringLiteral("Описание")}, MessageLinkPreviewKind::Task};
        const int before = scroll->maximum();
        latestPreview(preview);
        QTRY_VERIFY(scroll->maximum() > before);
        QCOMPARE(scroll->value(), scroll->maximum());
        QVERIFY(list->viewport()->rect().contains(list->visualItemRect(list->item(list->count() - 1)).bottomLeft()));

        const auto *olderRow = list->item(5);
        list->scrollToItem(olderRow, QAbstractItemView::PositionAtTop);
        Backend::LinkPreviewHandler olderPreview;
        QTRY_VERIFY(olderPreview || (olderPreview = take(QStringLiteral("/tasks/task/view/52/"))));
        const int olderOffset = list->visualItemRect(olderRow).top();
        const int olderMaximum = scroll->maximum();
        olderPreview(preview);
        QTRY_VERIFY(scroll->maximum() > olderMaximum);
        QCOMPARE(list->visualItemRect(olderRow).top(), olderOffset);
        QVERIFY(scroll->value() < scroll->maximum());
    }

    void unreadBoundaryAnchorsHistoryAndStaysOutOfSearch()
    {
        QFETCH(bool, task);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        const auto now = QDateTime::currentDateTime();
        auto &history = task ? backend->taskMessages : backend->messages;
        constexpr int firstUnread = 8;
        constexpr int total = 32;
        history.clear();
        for (int i = 0; i < total; ++i) {
            auto message = Message{QString::number(i + 1), QStringLiteral("Анна"),
                QStringLiteral("Сообщение %1").arg(i + 1), now.addSecs((i - total) * 30)};
            message.unread = i >= firstUnread || i == 6;
            message.outgoing = i == 6;
            history.append(message);
        }

        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QWidget *pane = &window;
        if (task) {
            QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
            window.openTaskCard(QStringLiteral("501"));
            pane = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
            QVERIFY(pane);
        }

        auto *messages = findAccessible<QListWidget>(pane, task ? QStringLiteral("Чат карточки")
                                                               : QStringLiteral("Сообщения чата"));
        QVERIFY(messages);
        auto *scroll = messages->verticalScrollBar();
        const auto anchored = [messages, scroll, firstUnread] {
            return messages->visualItemRect(messages->item(firstUnread)).top() >= 0 &&
                   messages->visualItemRect(messages->item(firstUnread)).top() < 10 &&
                   scroll->value() < scroll->maximum();
        };
        QTRY_COMPARE(messages->count(), total);
        QTRY_VERIFY2(anchored(), qPrintable(QStringLiteral("boundary top: %1; scroll: %2/%3")
            .arg(messages->visualItemRect(messages->item(firstUnread)).top())
            .arg(scroll->value()).arg(scroll->maximum())));
        for (int i = 0; i < total; ++i)
            QCOMPARE(messages->item(i)->data(workspace::UnreadDivider).toBool(), i == firstUnread);
        QVERIFY(messages->item(firstUnread)->data(Qt::AccessibleTextRole).toString()
            .startsWith(workspace::UnreadMessages));
        QCOMPARE(messages->item(firstUnread)->data(workspace::Title).toString(), QStringLiteral("Анна"));

        const auto scope = task ? QStringLiteral("501") : QStringLiteral("c-1");
        const auto updateHistory = [&] {
            if (task)
                emit backend->taskMessagesLoaded(scope, history);
            else
                emit backend->chatMessagesLoaded(scope, history);
        };
        const int previous = scroll->value();
        auto newest = history.back();
        newest.id = QStringLiteral("33");
        history.append(newest);
        updateHistory();
        QTRY_COMPARE(messages->count(), total + 1);
        QCOMPARE(scroll->value(), previous);
        messages->scrollToBottom();
        newest.id = QStringLiteral("34");
        history.append(newest);
        updateHistory();
        QTRY_COMPARE(messages->count(), total + 2);
        QTRY_COMPARE(scroll->value(), scroll->maximum());
        QVERIFY(messages->item(firstUnread)->data(workspace::UnreadDivider).toBool());
        QVERIFY(!messages->item(total + 1)->data(workspace::UnreadDivider).toBool());

        auto *showSearch = findAccessible<QPushButton>(pane, task ? QStringLiteral("Найти в чате задачи")
                                                                   : QStringLiteral("Найти в чате"));
        auto *search = findAccessible<QLineEdit>(pane, task ? QStringLiteral("Поиск по чату карточки")
                                                           : QStringLiteral("Поиск по сообщениям"));
        QVERIFY(showSearch);
        QVERIFY(search);
        QTest::mouseClick(showSearch, Qt::LeftButton);
        QTRY_VERIFY(search->isVisible() && search->hasFocus());
        const auto query = QStringLiteral("Сообщение 9");
        search->setText(query);
        QTest::keyClick(search, Qt::Key_Return);
        emit backend->messagesFound(scope, query, task, {history[firstUnread]});
        updateHistory();
        QTRY_COMPARE(messages->count(), 1);
        QVERIFY(!messages->item(0)->data(workspace::UnreadDivider).toBool());
        QTest::mouseClick(showSearch, Qt::LeftButton);
        QTRY_COMPARE(messages->count(), total + 2);
        QTRY_VERIFY(anchored());
    }

    void kanbanStageColorsPaintAndSaveWithoutRenaming()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto &stage = backend->stages.first();
        stage.color = QStringLiteral("11c4e9");
        backend->stages.last().color = QStringLiteral("254763");
        const QString stageId = stage.id;
        const QString name = stage.name;
        const QString darkName = backend->stages.last().name;
        backend->stages.append({QStringLiteral("603"), QStringLiteral("Без цвета"), {}});
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);

        const auto colorIn = [](const QListWidget *list) {
            auto *header = list ? list->parentWidget()->findChild<QWidget *>(QStringLiteral("kanbanStageHeader")) : nullptr;
            if (!header || !header->isVisible())
                return QColor();
            const auto image = header->grab().toImage();
            return image.pixelColor(qRound(12 * image.devicePixelRatio()), qRound(4 * image.devicePixelRatio()));
        };
        QTRY_COMPARE(colorIn(findAccessible<QListWidget>(&window, name)), QColor("#11c4e9"));
        QTRY_COMPARE(colorIn(findAccessible<QListWidget>(&window, darkName)), QColor("#254763"));
        QTRY_COMPARE(colorIn(findAccessible<QListWidget>(&window, QStringLiteral("Без цвета"))), QColor("#dce5e4"));
        const auto screenshot = qEnvironmentVariable("B24_STAGE_COLORS_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(window.grab().save(screenshot + QStringLiteral("-wide.png")));

        auto *stageTools = findAccessible<QPushButton>(&window, QStringLiteral("Управление этапами"));
        QVERIFY(stageTools && stageTools->isEnabled());
        stageTools->click();
        auto *manager = window.findChild<QDialog *>(QStringLiteral("stageManagerDialog"));
        QVERIFY(manager);
        auto *stages = findAccessible<QListWidget>(manager, QStringLiteral("Список этапов"));
        auto *title = findAccessible<QLineEdit>(manager, QStringLiteral("Название этапа"));
        auto *color = findAccessible<QPushButton>(manager, QStringLiteral("Изменить цвет этапа"));
        QVERIFY(stages && title && color);
        stages->setCurrentRow(0);
        title->setText(QStringLiteral("Несохранённое название"));
        color->setFocus();
        QTest::keyClick(color, Qt::Key_Space);
        auto *picker = manager->findChild<QColorDialog *>();
        QVERIFY(picker && picker->isVisible());
        QCOMPARE(picker->currentColor(), QColor("#11c4e9"));
        picker->setCurrentColor(QColor("#7b47d1"));
        picker->accept();
        QCOMPARE(backend->savedStage, stageId);
        QVERIFY(backend->savedStageTitle.isEmpty());
        QCOMPARE(backend->savedAfterStage, QString());
        QCOMPARE(backend->savedStageColor, QStringLiteral("7b47d1"));
        QTRY_COMPARE(colorIn(findAccessible<QListWidget>(&window, name)), QColor("#7b47d1"));
        QCOMPARE(title->text(), QStringLiteral("Несохранённое название"));
        manager->reject();

        MainWindow readOnly(backend.get(), true);
        readOnly.resize(720, 500);
        readOnly.show();
        QApplication::setActiveWindow(&readOnly);
        QTest::keyClick(&readOnly, Qt::Key_2, Qt::AltModifier);
        const auto *readOnlyStages = findAccessible<QPushButton>(&readOnly, QStringLiteral("Управление этапами"));
        QVERIFY(readOnlyStages && !readOnlyStages->isEnabled());
        QTRY_COMPARE(colorIn(findAccessible<QListWidget>(&readOnly, name)), QColor("#7b47d1"));
        if (!screenshot.isEmpty())
            QVERIFY(readOnly.grab().save(screenshot + QStringLiteral("-compact.png")));
    }

    void queuesNewTaskFilesUntilTheMatchingCreationAcknowledgement_data()
    {
        QTest::addColumn<bool>("subtask");
        QTest::addColumn<bool>("cancel");
        QTest::addColumn<bool>("changeAccount");
        QTest::newRow("task") << false << false << false;
        QTest::newRow("subtask") << true << false << false;
        QTest::newRow("cancel before acknowledgement") << false << true << false;
        QTest::newRow("account changes during upload") << false << false << true;
    }

    void queuesNewTaskFilesUntilTheMatchingCreationAcknowledgement()
    {
        QFETCH(bool, subtask);
        QFETCH(bool, cancel);
        QFETCH(bool, changeAccount);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->accountScope = QStringLiteral("fixture/task-attachment-first");
        backend->holdTaskCreation = true;
        backend->holdTaskAttachment = true;
        MainWindow window(backend.get());
        window.resize(960, 740);
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        if (subtask) {
            window.openTaskCard(QStringLiteral("501"));
            auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
            QVERIFY(card);
            QPushButton *create = nullptr;
            for (auto *button : card->findChildren<QPushButton *>()) {
                if (button->text() == QStringLiteral("Новая подзадача"))
                    create = button;
            }
            QVERIFY(create);
            QTRY_VERIFY(create->isEnabled());
            create->click();
        } else {
            auto *create = findAccessible<QPushButton>(&window, QStringLiteral("Создать задачу"));
            QVERIFY(create);
            create->click();
        }
        auto *form = findAccessible<QDialog>(&window, subtask ? QStringLiteral("Форма новой подзадачи")
                                                            : QStringLiteral("Форма новой задачи"));
        QVERIFY(form);
        auto *title = findAccessible<QLineEdit>(form, QStringLiteral("Заголовок задачи"));
        auto *editor = findAccessible<QTextEdit>(form, QStringLiteral("Описание задачи"));
        auto *attach = findAccessible<QToolButton>(form, QStringLiteral("Прикрепить файл"));
        auto *queue = findAccessible<QListWidget>(form, QStringLiteral("Файлы к новой задаче"));
        auto *remove = findAccessible<QPushButton>(form, QStringLiteral("Убрать выбранный файл"));
        auto *save = findAccessible<QPushButton>(form, QStringLiteral("Сохранить задачу"));
        QVERIFY(title && editor && attach && queue && remove && save);
        QVERIFY(attach->isEnabled());
        title->setText(QStringLiteral("Новая задача с файлами"));
        const auto description = QStringLiteral("Описание сохраняется без подстановок");
        editor->insertPlainText(description);
        QTemporaryDir files;
        QVERIFY(files.isValid());
        const auto first = files.filePath(QStringLiteral("первый.txt"));
        const auto second = files.filePath(QStringLiteral("второй.txt"));
        const auto unused = files.filePath(QStringLiteral("лишний.txt"));
        for (const auto &path : {first, second, unused}) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("file"), 4);
        }
        attach->click();
        auto *chooser = form->findChild<QFileDialog *>();
        QVERIFY(chooser);
        chooser->selectFile(first);
        QVERIFY(QMetaObject::invokeMethod(chooser, "accept"));
        QTRY_COMPARE(queue->count(), 1);
        auto *clipboard = new QMimeData;
        clipboard->setUrls({QUrl::fromLocalFile(first), QUrl::fromLocalFile(unused), QUrl::fromLocalFile(second),
                            QUrl::fromLocalFile(first)});
        QGuiApplication::clipboard()->setMimeData(clipboard);
        editor->paste();
        QCOMPARE(queue->count(), 3);
        QCOMPARE(queue->item(0)->text(), QFileInfo(first).fileName());
        queue->setCurrentRow(1);
        remove->click();
        QCOMPARE(queue->count(), 2);
        QCOMPARE(queue->item(1)->text(), QFileInfo(second).fileName());
        QVERIFY(backend->attachmentRequests.isEmpty());
        save->click();
        QCOMPARE(backend->createTaskCalls, 1);
        QCOMPARE(backend->lastCreate.description, std::optional<QString>(description));
        QVERIFY(backend->attachmentRequests.isEmpty());
        emit backend->taskCreated(QStringLiteral("999"), QStringLiteral("503"));
        QVERIFY(backend->attachmentRequests.isEmpty());

        if (cancel)
            form->reject();
        if (subtask)
            emit backend->subtaskCreated(backend->createdParent, backend->createdProject, QStringLiteral("502"));
        else
            emit backend->taskCreated(backend->createdProject, QStringLiteral("502"));
        if (cancel) {
            QVERIFY(backend->attachmentRequests.isEmpty());
            return;
        }
        QCOMPARE(backend->attachmentRequests.size(), 1);
        QCOMPARE(backend->attachmentRequests.last(), qMakePair(QStringLiteral("502"), first));
        if (changeAccount) {
            backend->accountScope = QStringLiteral("fixture/task-attachment-second");
            emit backend->accountScopeChanged();
            QVERIFY(!form->isVisible());
            emit backend->fileUploaded(QStringLiteral("502"), QStringLiteral("task"));
            QCOMPARE(backend->attachmentRequests.size(), 1);
            return;
        }
        emit backend->fileUploaded(QStringLiteral("999"), QStringLiteral("task"));
        QCOMPARE(backend->attachmentRequests.size(), 1);
        emit backend->fileUploaded(QStringLiteral("502"), QStringLiteral("task"));
        QCOMPARE(backend->attachmentRequests.size(), 2);
        QCOMPARE(backend->attachmentRequests.last(), qMakePair(QStringLiteral("502"), second));
        emit backend->failed(QStringLiteral("attach-task:502"), QStringLiteral("Не удалось загрузить файл"));
        QCOMPARE(queue->count(), 1);
        QVERIFY(form->isVisible());
        QCOMPARE(save->text(), QStringLiteral("Повторить загрузку"));
        QVERIFY(save->isEnabled());
        save->click();
        QCOMPARE(backend->createTaskCalls, 1);
        QCOMPARE(backend->attachmentRequests.size(), 3);
        QCOMPARE(backend->attachmentRequests.last(), qMakePair(QStringLiteral("502"), second));
        emit backend->fileUploaded(QStringLiteral("502"), QStringLiteral("task"));
        QVERIFY(!form->isVisible());
    }

    void createsTaskWithNamedDefaults_data()
    {
        QTest::addColumn<QString>("profileState");
        QTest::addColumn<bool>("subtask");
        QTest::newRow("confirmed task") << QStringLiteral("confirmed") << false;
        QTest::newRow("confirmed subtask") << QStringLiteral("confirmed") << true;
        QTest::newRow("profile unavailable") << QStringLiteral("unavailable") << false;
        QTest::newRow("profile still loading") << QStringLiteral("loading") << false;
    }

    void createsTaskWithNamedDefaults()
    {
        QFETCH(QString, profileState);
        QFETCH(bool, subtask);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdCurrentPerson = true;
        backend->holdTaskCreation = true;
        backend->accountScope = QStringLiteral("fixture/task-defaults-first");
        backend->stages.first().tasks.first().projectId = QStringLiteral("101");
        backend->stages.prepend({QStringLiteral("0"), QStringLiteral("Без стадии"), {}});
        MainWindow window(backend.get());
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        if (subtask) {
            window.openTaskCard(QStringLiteral("501"));
            auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
            QVERIFY(card);
            QPushButton *create = nullptr;
            for (auto *button : card->findChildren<QPushButton *>()) {
                if (button->text() == QStringLiteral("Новая подзадача"))
                    create = button;
            }
            QVERIFY(create);
            QTRY_VERIFY(create->isEnabled());
            create->click();
        } else {
            auto *create = findAccessible<QPushButton>(&window, QStringLiteral("Создать задачу"));
            QVERIFY(create && create->isEnabled());
            create->click();
        }
        auto *form = findAccessible<QDialog>(&window, subtask ? QStringLiteral("Форма новой подзадачи")
                                                            : QStringLiteral("Форма новой задачи"));
        QVERIFY(form);
        auto *stage = findAccessible<QComboBox>(form, QStringLiteral("Стадия"));
        auto *creator = findAccessible<QPushButton>(form, QStringLiteral("Постановщик"));
        auto *assignee = findAccessible<QPushButton>(form, QStringLiteral("Ответственный"));
        auto *title = findAccessible<QLineEdit>(form, QStringLiteral("Заголовок задачи"));
        auto *save = findAccessible<QPushButton>(form, QStringLiteral("Сохранить задачу"));
        QVERIFY(stage && creator && assignee && title && save);
        QCOMPARE(stage->currentText(), QStringLiteral("В работе"));
        QCOMPARE(stage->currentData().toString(), QStringLiteral("601"));
        QCOMPARE(stage->findData(QStringLiteral("0")), -1);
        QCOMPARE(assignee->text(), QStringLiteral("Загружаем профиль…"));
        QCOMPARE(creator->text(), assignee->text());
        QCOMPARE(backend->currentPersonRequests.size(), 1);
        const bool confirmed = profileState == QLatin1String("confirmed");
        if (profileState != QLatin1String("loading")) {
            backend->currentPersonRequests.takeFirst()(confirmed ? backend->currentPerson : Person());
            const auto expectedName = confirmed ? backend->currentPerson.name : QStringLiteral("Профиль недоступен · Выбрать");
            QCOMPARE(assignee->text(), expectedName);
            QCOMPARE(creator->text(), expectedName);
        }
        QVERIFY(assignee->isEnabled() && creator->isEnabled());
        title->setText(QStringLiteral("Задача со стартовыми реквизитами"));
        QVERIFY(save->isEnabled());
        save->click();
        QCOMPARE(backend->createTaskCalls, 1);
        QCOMPARE(backend->createdParent, subtask ? QStringLiteral("501") : QString());
        QCOMPARE(backend->lastCreate.stageId, std::optional<QString>(QStringLiteral("601")));
        const auto expectedUser = confirmed ? std::optional<QString>(backend->currentPerson.id) : std::nullopt;
        QCOMPARE(backend->lastCreate.creatorId, expectedUser);
        QCOMPARE(backend->lastCreate.assigneeId, expectedUser);

        if (profileState == QLatin1String("loading")) {
            auto staleResponse = backend->currentPersonRequests.takeFirst();
            const QPointer<QDialog> oldForm(form);
            backend->accountScope = QStringLiteral("fixture/task-defaults-second");
            emit backend->accountScopeChanged();
            QTRY_VERIFY(oldForm.isNull());
            auto *create = findAccessible<QPushButton>(&window, QStringLiteral("Создать задачу"));
            QVERIFY(create && create->isEnabled());
            create->click();
            auto *next = findAccessible<QDialog>(&window, QStringLiteral("Форма новой задачи"));
            QVERIFY(next);
            auto *nextCreator = findAccessible<QPushButton>(next, QStringLiteral("Постановщик"));
            QVERIFY(nextCreator);
            staleResponse(backend->currentPerson);
            QCOMPARE(nextCreator->text(), QStringLiteral("Загружаем профиль…"));
            QCOMPARE(backend->currentPersonRequests.size(), 1);
            backend->currentPersonRequests.takeFirst()({QStringLiteral("20"), QStringLiteral("Илья Власов"), {}, {}});
            QCOMPARE(nextCreator->text(), QStringLiteral("Илья Власов"));
        }
    }

    void createsTaskWithSelectedRequisites()
    {
        const auto applicationFont = QApplication::font();
        const auto restoreFont = qScopeGuard([applicationFont] { QApplication::setFont(applicationFont); });
        ui::installApplicationTypography(*qApp);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdProjectStages = true;
        backend->holdTaskCreation = true;
        backend->holdCurrentPerson = true;
        backend->projects.append({QStringLiteral("102"), QStringLiteral("Проект два"), {}});
        backend->stages.append({QStringLiteral("0"), QStringLiteral("Без стадии"), {}});
        backend->people = {{QStringLiteral("12"), QStringLiteral("Анна"), {}, {}},
                           {QStringLiteral("13"), QStringLiteral("Вера"), {}, {}},
                           {QStringLiteral("14"), QStringLiteral("Борис"), {}, {}},
                           {QStringLiteral("15"), QStringLiteral("Ольга"), {}, {}}};
        MainWindow window(backend.get());
        window.resize(860, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        auto *create = findAccessible<QPushButton>(&window, QStringLiteral("Создать задачу"));
        QVERIFY(create && create->isEnabled());
        create->click();
        auto *form = findAccessible<QDialog>(&window, QStringLiteral("Форма новой задачи"));
        QVERIFY(form);
        auto *project = findAccessible<QPushButton>(form, QStringLiteral("Проект"));
        auto *stage = findAccessible<QComboBox>(form, QStringLiteral("Стадия"));
        auto *creator = findAccessible<QPushButton>(form, QStringLiteral("Постановщик"));
        auto *assignee = findAccessible<QPushButton>(form, QStringLiteral("Ответственный"));
        auto *observers = findAccessible<QPushButton>(form, QStringLiteral("Наблюдатели"));
        QVERIFY(project && stage && creator && assignee && observers);
        QVERIFY(!observers->visibleRegion().isEmpty());
        QVERIFY(project->text().contains(QStringLiteral("Проект один")));
        QTRY_COMPARE(stage->currentData().toString(), QStringLiteral("601"));
        QCOMPARE(stage->findData(QStringLiteral("0")), -1);
        stage->setCurrentIndex(stage->findData(QStringLiteral("602")));
        emit backend->projectStagesLoaded(QStringLiteral("101"), backend->stages);
        QCOMPARE(stage->currentData().toString(), QStringLiteral("602"));
        project->setFocus();
        QTest::keyClick(project, Qt::Key_Space);
        auto *projects = findAccessible<QListWidget>(form, QStringLiteral("Проект задачи"));
        QVERIFY(projects && projects->isVisible());
        const auto destination = projects->findItems(QStringLiteral("Проект два"), Qt::MatchExactly);
        QCOMPARE(destination.size(), 1);
        projects->setCurrentItem(destination.first());
        QTest::keyClick(projects, Qt::Key_Return);
        QCOMPARE(backend->projectStageRequests.last(), QStringLiteral("102"));
        QVERIFY(project->text().contains(QStringLiteral("Проект два")));
        QVERIFY(!stage->isEnabled());
        QCOMPARE(stage->findData(QStringLiteral("601")), -1);
        emit backend->projectStagesLoaded(QStringLiteral("101"), backend->stages);
        QVERIFY(!stage->isEnabled());
        emit backend->failed(QStringLiteral("project-stages:102"), QStringLiteral("Временно недоступно"));
        auto *retryStage = findAccessible<QPushButton>(form, QStringLiteral("Повторить загрузку стадий"));
        QVERIFY(retryStage && retryStage->isVisible());
        retryStage->click();
        QCOMPARE(backend->projectStageRequests.count(QStringLiteral("102")), 2);
        emit backend->projectStagesLoaded(QStringLiteral("102"), {});
        QCOMPARE(stage->currentText(), QStringLiteral("Нет стадий"));
        QVERIFY(!stage->isEnabled() && !stage->currentData().isValid());
        emit backend->projectStagesLoaded(QStringLiteral("102"), {{QStringLiteral("611"), QStringLiteral("Проверка"), {}}});
        QVERIFY(stage->isEnabled());
        QCOMPARE(stage->currentText(), QStringLiteral("Проверка"));
        QCOMPARE(stage->currentData().toString(), QStringLiteral("611"));

        const auto pickPeople = [&](QPushButton *button, const QString &inputName, const QStringList &names) {
            button->click();
            auto *input = findAccessible<QLineEdit>(form, QStringLiteral("Поиск сотрудника · ") + inputName);
            QTRY_VERIFY(input && input->isVisible());
            for (const auto &name : names) {
                QListWidget *results = nullptr;
                QTRY_VERIFY([&] {
                    for (auto *list : form->findChildren<QListWidget *>()) {
                        if (list->accessibleName() == QStringLiteral("Найденные сотрудники")
                            && list->isVisible() && !list->findItems(name, Qt::MatchExactly).isEmpty()) {
                            results = list;
                            return true;
                        }
                    }
                    return false;
                }());
                auto *item = results->findItems(name, Qt::MatchExactly).first();
                results->scrollToItem(item);
                QTest::mouseClick(results->viewport(), Qt::LeftButton, Qt::NoModifier, results->visualItemRect(item).center());
            }
            if (button == creator) {
                QCOMPARE(backend->currentPersonRequests.size(), 1);
                backend->currentPersonRequests.takeFirst()(backend->currentPerson);
            }
            auto *apply = findAccessible<QPushButton>(form, QStringLiteral("Применить изменение реквизита"));
            if (apply && apply->isVisible())
                apply->click();
        };
        pickPeople(assignee, QStringLiteral("Ответственный"), {QStringLiteral("Анна")});
        pickPeople(creator, QStringLiteral("Постановщик"), {QStringLiteral("Вера")});
        pickPeople(observers, QStringLiteral("Наблюдатели"), {QStringLiteral("Борис"), QStringLiteral("Ольга")});
        auto *title = findAccessible<QLineEdit>(form, QStringLiteral("Заголовок задачи"));
        QVERIFY(title);
        title->setText(QStringLiteral("Проверить реквизиты"));
        auto *save = findAccessible<QPushButton>(form, QStringLiteral("Сохранить задачу"));
        QVERIFY(save && save->isEnabled());
        save->click();
        QCOMPARE(backend->createdProject, QStringLiteral("102"));
        QCOMPARE(backend->lastCreate.stageId, std::optional<QString>(QStringLiteral("611")));
        QCOMPARE(backend->lastCreate.assigneeId, std::optional<QString>(QStringLiteral("12")));
        QCOMPARE(backend->lastCreate.creatorId, std::optional<QString>(QStringLiteral("13")));
        QCOMPARE(backend->lastCreate.observerIds, std::optional<QStringList>({QStringLiteral("14"), QStringLiteral("15")}));
        QCOMPARE(backend->createTaskCalls, 1);
        emit backend->taskCreated(QStringLiteral("101"), QStringLiteral("999"));
        QVERIFY(form->isVisible());
        emit backend->taskCreated(QStringLiteral("102"), QStringLiteral("502"));
        QVERIFY(!form->isVisible());
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card && card->isVisible());
        QCOMPARE(static_cast<ui::TaskCardDialog *>(card)->taskId(), QStringLiteral("502"));
        QCOMPARE(static_cast<ui::TaskCardDialog *>(card)->projectId(), QStringLiteral("102"));
    }

    void createsTaskProjectAndManagesStages()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);

        auto *newTask = findAccessible<QPushButton>(&window, QStringLiteral("Создать задачу"));
        QVERIFY(newTask);
        newTask->click();
        auto *form = findAccessible<QDialog>(&window, QStringLiteral("Форма новой задачи"));
        QVERIFY(form);
        auto *title = findAccessible<QLineEdit>(form, QStringLiteral("Заголовок задачи"));
        QVERIFY(title);
        title->setText(QStringLiteral("Новая задача"));
        auto *save = findAccessible<QPushButton>(form, QStringLiteral("Сохранить задачу"));
        QVERIFY(save);
        save->click();
        QCOMPARE(backend->createdProject, QStringLiteral("101"));
        QVERIFY(backend->lastCreate.title.has_value());
        QCOMPARE(*backend->lastCreate.title, QStringLiteral("Новая задача"));
        if (auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog")))
            card->reject();

        auto *newProject = findAccessible<QPushButton>(&window, QStringLiteral("Создать проект"));
        QVERIFY(newProject);
        newProject->click();
        auto *nameDialog = window.findChild<QDialog *>(QStringLiteral("nameDialog"));
        QVERIFY(nameDialog);
        auto *name = findAccessible<QLineEdit>(nameDialog, QStringLiteral("Название"));
        QVERIFY(name);
        name->setText(QStringLiteral("Новый проект"));
        auto *confirm = findAccessible<QPushButton>(nameDialog, QStringLiteral("Подтвердить название"));
        QVERIFY(confirm);
        confirm->click();
        QCOMPARE(backend->createdProjectName, QStringLiteral("Новый проект"));
        auto *projects = findAccessible<QListWidget>(&window, QStringLiteral("Проекты"));
        QVERIFY(projects);
        QCOMPARE(projects->currentItem()->data(workspace::Id).toString(), QStringLiteral("102"));
        projects->setCurrentRow(0);

        auto *stagesButton = findAccessible<QPushButton>(&window, QStringLiteral("Управление этапами"));
        QVERIFY(stagesButton);
        stagesButton->click();
        auto *manager = window.findChild<QDialog *>(QStringLiteral("stageManagerDialog"));
        QVERIFY(manager);
        auto *stageList = findAccessible<QListWidget>(manager, QStringLiteral("Список этапов"));
        QVERIFY(stageList);
        QTRY_COMPARE(stageList->count(), 2);
        stageList->setCurrentRow(0);
        auto *stageTitle = findAccessible<QLineEdit>(manager, QStringLiteral("Название этапа"));
        QVERIFY(stageTitle);
        stageTitle->setText(QStringLiteral("В работе 2"));
        auto *rename = findAccessible<QPushButton>(manager, QStringLiteral("Переименовать этап"));
        QVERIFY(rename);
        rename->click();
        QCOMPARE(backend->savedStage, QStringLiteral("601"));
        QCOMPARE(backend->savedStageTitle, QStringLiteral("В работе 2"));

        auto *remove = findAccessible<QPushButton>(manager, QStringLiteral("Удалить этап"));
        QVERIFY(remove);
        QVERIFY(!remove->isEnabled());
        remove->click();
        QVERIFY(backend->deletedStage.isEmpty());

        stageList->setCurrentRow(1);
        auto *moveUp = findAccessible<QPushButton>(manager, QStringLiteral("Переместить этап выше"));
        QVERIFY(moveUp);
        moveUp->click();
        QCOMPARE(backend->savedStage, QStringLiteral("602"));
        QCOMPARE(backend->savedAfterStage, QStringLiteral("0"));

        backend->stages[1].taskCount.reset();
        emit backend->boardLoaded(QStringLiteral("101"), backend->stages);
        QVERIFY(!remove->isEnabled());
        backend->stages[1].taskCount = 0;
        emit backend->boardLoaded(QStringLiteral("101"), backend->stages);
        QVERIFY(remove->isEnabled());
        QTimer::singleShot(0, [] {
            auto *confirm = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (confirm)
                confirm->button(QMessageBox::Yes)->click();
        });
        remove->click();
        QCOMPARE(backend->deletedStage, QStringLiteral("602"));
    }

    void markupAndSearchInReadOnlyTaskCard()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->chats.first().lastMessage = QStringLiteral("[B]Предпросмотр[/B]");
        auto &message = backend->messages.first();
        message.text = QStringLiteral("[USER=1 REPLACE]Анна[/USER] [B]проверила[/B]");
        message.forwarded = true;
        message.forwardedAuthor = QStringLiteral("[B]Иван[/B]");
        message.replyId = QStringLiteral("m-source");
        message.replyAuthor = QStringLiteral("Алексей");
        message.replyText = QStringLiteral("[B]Проверить кран[/B]");
        message.attachments = {QStringLiteral("C:\\temp\\<file>.txt")};
        backend->taskMessages = {message};
        backend->stages.first().tasks.first().description = QStringLiteral("[B]Описание рабочей задачи[/B]");
        backend->notifications = {{QStringLiteral("n-1"), {}, QStringLiteral("[B]Новая задача[/B]"),
            QStringLiteral("[I]Описание[/I]"), {}, false}};
        MainWindow window(backend.get(), true);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *chatList = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
        auto *messages = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(chatList);
        QVERIFY(messages);
        QTRY_COMPARE(chatList->item(0)->data(workspace::Detail).toString(), QStringLiteral("Предпросмотр"));
        QCOMPARE(messages->count(), 1);
        QCOMPARE(messages->item(0)->data(workspace::Detail).toString(),
                 QStringLiteral("Анна проверила\nВложения: <file>.txt"));
        QVERIFY(messages->item(0)->data(workspace::MarkupHtml).toString().contains(QStringLiteral("&lt;file&gt;")));
        QCOMPARE(messages->item(0)->data(workspace::ForwardByline).toString(), QStringLiteral("Переслано · Иван"));
        QCOMPARE(messages->item(0)->data(workspace::ReplyPreview).toString(), QStringLiteral("Проверить кран"));
        QVERIFY(messages->item(0)->data(Qt::AccessibleTextRole).toString().contains(QStringLiteral("Ответ · Алексей")));
        QVERIFY(messages->item(0)->toolTip().isEmpty());
        QTest::keyClick(&window, Qt::Key_3, Qt::AltModifier);
        auto *notices = findAccessible<QListWidget>(&window, QStringLiteral("Уведомления"));
        QVERIFY(notices);
        QCOMPARE(notices->item(0)->data(workspace::Title).toString(), QStringLiteral("Новая задача"));
        QVERIFY(!notices->item(0)->data(workspace::MarkupHtml).toString().isEmpty());
        QVERIFY(notices->item(0)->toolTip().isEmpty());

        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        auto *description = findAccessible<QListWidget>(card, QStringLiteral("Описание и встроенные файлы"));
        QVERIFY(description && description->count() == 1);
        QCOMPARE(description->item(0)->text(), QStringLiteral("Описание рабочей задачи"));
        QVERIFY(description->item(0)->toolTip().isEmpty());
        QVERIFY(!findAccessible<QPushButton>(card, QStringLiteral("Сохранить задачу"))->isEnabled());
        QVERIFY(!findAccessible<QPushButton>(card, QStringLiteral("Повторить перенос задачи"))->isEnabled());
        QVERIFY(!findAccessible<QPushButton>(card, QStringLiteral("Отправить файл в чат задачи"))->isEnabled());
        auto *search = findAccessible<QLineEdit>(card, QStringLiteral("Поиск по чату карточки"));
        auto *taskMessages = findAccessible<QListWidget>(card, QStringLiteral("Чат карточки"));
        auto *showSearch = findAccessible<QPushButton>(card, QStringLiteral("Найти в чате задачи"));
        QVERIFY(search);
        QVERIFY(taskMessages);
        QVERIFY(showSearch);
        QVERIFY(!search->isVisible());
        QTest::mouseClick(showSearch, Qt::LeftButton);
        QTRY_VERIFY(search->isVisible() && search->hasFocus());
        QCOMPARE(taskMessages->item(0)->data(workspace::Detail).toString(), messages->item(0)->data(workspace::Detail).toString());
        QVERIFY(taskMessages->item(0)->toolTip().isEmpty());
        QCOMPARE(taskMessages->item(0)->data(workspace::ReplyPreview).toString(), QStringLiteral("Проверить кран"));
        search->setText(QStringLiteral("проверила"));
        QTest::keyClick(search, Qt::Key_Return);
        QVERIFY(backend->searchedTask);
        QCOMPARE(backend->searchedScope, QStringLiteral("501"));
        emit backend->messagesFound(QStringLiteral("501"), QStringLiteral("проверила"), true, backend->taskMessages);
        emit backend->taskMessagesLoaded(QStringLiteral("501"), {});
        QCOMPARE(taskMessages->count(), 1);
        QVERIFY(!taskMessages->item(0)->data(workspace::MarkupHtml).toString().isEmpty());
        QTest::mouseClick(showSearch, Qt::LeftButton);
        QVERIFY(!search->isVisible());
        QCOMPARE(taskMessages->item(0)->data(workspace::Detail).toString(), messages->item(0)->data(workspace::Detail).toString());
    }

    void emojiPanelAnchorsToBothComposers()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        const auto checkPanel = [](QWidget *host, const QString &inputName, const QString &toggleName,
                                   const char *screenshotVariable) {
            auto *input = findAccessible<QPlainTextEdit>(host, inputName);
            auto *toggle = findAccessible<QPushButton>(host, toggleName);
            QVERIFY(input);
            QVERIFY(toggle);
            input->setPlainText(QStringLiteral("До встречи"));
            auto cursor = input->textCursor();
            cursor.setPosition(2);
            input->setTextCursor(cursor);
            toggle->click();
            auto *panel = host->findChild<QWidget *>(QStringLiteral("emojiPanel"));
            QVERIFY(panel);
            QVERIFY(panel->isVisible());
            QVERIFY(!panel->isWindow());
            QVERIFY(panel->parentWidget()->rect().contains(panel->geometry()));
            const auto screenshot = qEnvironmentVariable(screenshotVariable);
            if (!screenshot.isEmpty()) {
                QTest::qWait(40);
                QVERIFY(host->grab().save(screenshot));
            }

            auto *tile = findAccessible<QPushButton>(panel, QStringLiteral("Улыбка"));
            QVERIFY(tile);
            QVERIFY(tile->width() >= 40);
            QVERIFY(tile->height() >= 38);
            tile->click();
            QCOMPARE(input->toPlainText(), QStringLiteral("До😀 встречи"));
            QTRY_VERIFY(input->hasFocus());
            if (!panel->isVisible())
                toggle->click();
            QTest::keyClick(input, Qt::Key_Escape);
            QVERIFY(!panel->isVisible());
            QVERIFY(host->isVisible());
            QCOMPARE(input->toPlainText(), QStringLiteral("До😀 встречи"));

            toggle->click();
            QVERIFY(panel->isVisible());
            QTest::mouseClick(input->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(8, 8));
            QVERIFY(!panel->isVisible());
        };

        checkPanel(&window, QStringLiteral("Текст сообщения"), QStringLiteral("Эмодзи в чате"),
                   "B24_CHAT_EMOJI_SCREENSHOT");
        window.resize(720, 500);
        checkPanel(&window, QStringLiteral("Текст сообщения"), QStringLiteral("Эмодзи в чате"),
                   "B24_COMPACT_CHAT_SCREENSHOT");

        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        checkPanel(card, QStringLiteral("Текст в чате карточки"), QStringLiteral("Эмодзи в карточке"),
                   "B24_TASK_EMOJI_SCREENSHOT");
    }

    void samePortalTaskLinksOpenNativeCard()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto &message = backend->messages.first();
        message.system = true;
        message.text = QStringLiteral("Создана [URL=/workgroups/group/101/tasks/task/view/501/]задача[/URL]");
        MainWindow window(backend.get());
        window.resize(930, 630);
        window.show();
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        QTRY_COMPARE(history->count(), 1);
        const auto *item = history->item(0);
        QVERIFY(item->data(workspace::SystemMessage).toBool());
        QVERIFY(item->data(Qt::AccessibleTextRole).toString().startsWith(QStringLiteral("Системное сообщение")));
        QCoreApplication::processEvents();
        const auto row = history->visualItemRect(item);
        const auto index = history->model()->index(0, 0);
        const auto *painter = static_cast<workspace::RowPainter *>(history->itemDelegate());
        const auto shot = history->viewport()->grab().toImage();
        QPoint target;
        bool foundSystemColor = false;
        const auto visible = row.intersected(shot.rect());
        for (int y = visible.top(); y <= visible.bottom(); ++y) {
            for (int x = visible.left(); x <= visible.right(); ++x) {
                if (shot.pixelColor(x, y) == QColor("#e2ebe6"))
                    foundSystemColor = true;
                if (target.isNull() && !painter->anchorAt(index, row, QPoint(x, y)).isEmpty())
                    target = QPoint(x, y);
            }
        }
        QVERIFY(foundSystemColor);
        QVERIFY(!target.isNull());
        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, target);
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        QVERIFY(card->isVisible());
        auto *copyLink = findAccessible<QPushButton>(card, QStringLiteral("Скопировать ссылку"));
        QVERIFY(copyLink && copyLink->isEnabled());
        card->close();
        QTRY_VERIFY(!window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        window.openMessageLink(QStringLiteral("/extranet/company/personal/user/19/tasks/task/view/501/"));
        card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card && card->isVisible());
    }

    void samePortalChatMessageLinksOpenNativeContext()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        const auto taskDialog = QStringLiteral("chat154729");
        const auto linkedMessage = QStringLiteral("5381055");
        const auto address = QStringLiteral("https://example.bitrix24.ru/online/?IM_TASK=chat154729&IM_MESSAGE=5381055");
        backend->messages.first().text = address;
        backend->stages.first().tasks.first().chatDialogId = taskDialog;
        LocalFileOpener intercepted;
        QDesktopServices::setUrlHandler(QStringLiteral("https"), &intercepted, "open");
        const auto restoreBrowser = qScopeGuard([] { QDesktopServices::unsetUrlHandler(QStringLiteral("https")); });
        MainWindow window(backend.get());
        window.resize(960, 630);
        window.show();
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("chatScopeTabs"));
        QVERIFY(history && tabs);
        QTRY_COMPARE(history->count(), 1);
        const auto row = history->visualItemRect(history->item(0));
        const auto index = history->model()->index(0, 0);
        const auto *painter = static_cast<const workspace::RowPainter *>(history->itemDelegate());
        QPoint anchor;
        for (int y = row.top(); y <= row.bottom() && anchor.isNull(); ++y) {
            for (int x = row.left(); x <= row.right(); ++x) {
                const auto link = painter->anchorAt(index, row, QPoint(x, y));
                if (link.contains(QStringLiteral("IM_MESSAGE=5381055"))) {
                    anchor = QPoint(x, y);
                    break;
                }
            }
        }
        QVERIFY(!anchor.isNull());
        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, anchor);
        QVERIFY(intercepted.opened.isEmpty());
        QCOMPARE(tabs->currentIndex(), 1);
        QCOMPARE(backend->contextDialog, taskDialog);
        QCOMPARE(backend->contextMessage, linkedMessage);
        Message selected;
        selected.id = linkedMessage;
        selected.text = QStringLiteral("Нужное сообщение");
        emit backend->messageContextLoaded(taskDialog, selected.id, {selected});
        QVERIFY(history->currentItem());
        QCOMPARE(history->currentItem()->data(workspace::Id).toString(), selected.id);

        window.openTaskCard(QStringLiteral("501"));
        auto *card = static_cast<ui::TaskCardDialog *>(window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        QVERIFY(card && card->isVisible());
        QCOMPARE(card->taskChatDialogId(), taskDialog);
        card->messageInput()->setPlainText(QStringLiteral("Черновик задачи"));
        int closeAttempts = 0;
        card->setDiscardConfirmation([&closeAttempts] { ++closeAttempts; return false; });
        window.openMessageLink(QStringLiteral("/online/?IM_TASK=chat154729&IM_MESSAGE=5381056"), card->messageList());
        QCOMPARE(closeAttempts, 0);
        QVERIFY(card->isVisible());
        QCOMPARE(backend->contextDialog, taskDialog);
        QCOMPARE(backend->contextMessage, QStringLiteral("5381056"));
        selected.id = backend->contextMessage;
        emit backend->messageContextLoaded(taskDialog, selected.id, {selected});
        QVERIFY(card->messageList()->currentItem());
        QCOMPARE(card->messageList()->currentItem()->data(workspace::Id).toString(), selected.id);

        const auto other = QStringLiteral("/online/?IM_DIALOG=chat42&IM_MESSAGE=555");
        window.openMessageLink(other, card->messageList());
        QCOMPARE(closeAttempts, 1);
        QVERIFY(card->isVisible());
        QCOMPARE(card->messageInput()->toPlainText(), QStringLiteral("Черновик задачи"));
        QCOMPARE(backend->contextDialog, taskDialog);
        QVERIFY(intercepted.opened.isEmpty());
        card->setDiscardConfirmation([] { return true; });
        window.openMessageLink(other, card->messageList());
        QTRY_VERIFY(!window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        QCOMPARE(tabs->currentIndex(), 0);
        QCOMPARE(backend->contextDialog, QStringLiteral("chat42"));
        QCOMPARE(backend->contextMessage, QStringLiteral("555"));

        const auto external = QUrl(QStringLiteral("https://example.test/online/?IM_TASK=chat154729&IM_MESSAGE=5381055"));
        window.openMessageLink(external.toString());
        QCOMPARE(intercepted.opened, QList<QUrl>{external});
    }

    void mentionInHistoryOpensNativeProfile()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->messages.first().text = QStringLiteral("[USER=19 REPLACE]Анна[/USER] проверила задачу");
        MainWindow window(backend.get());
        window.resize(930, 630);
        window.show();
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        QTRY_COMPARE(history->count(), 1);
        QCoreApplication::processEvents();
        const auto row = history->visualItemRect(history->item(0));
        const auto index = history->model()->index(0, 0);
        const auto *painter = static_cast<workspace::RowPainter *>(history->itemDelegate());
        QPoint target;
        for (int y = row.top(); y <= row.bottom() && target.isNull(); ++y) {
            for (int x = row.left(); x <= row.right(); ++x) {
                if (painter->anchorAt(index, row, QPoint(x, y)) == QLatin1String("/company/personal/user/19/")) {
                    target = QPoint(x, y);
                    break;
                }
            }
        }
        QVERIFY(!target.isNull());
        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, target);
        QCOMPARE(backend->requestedPerson, QStringLiteral("19"));
        auto *profile = window.findChild<QDialog *>(QStringLiteral("personProfileDialog"));
        QVERIFY(profile && profile->isVisible());
    }

    void employeeProfileClosesAboveTaskCard()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        Message message;
        message.id = QStringLiteral("502");
        message.text = QStringLiteral("[USER=19 REPLACE]Анна[/USER] проверила задачу");
        backend->taskMessages = {message};
        MainWindow window(backend.get());
        window.resize(970, 650);
        window.show();
        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card && card->isVisible());
        QCOMPARE(card->windowModality(), Qt::WindowModal);
        auto *history = findAccessible<QListWidget>(card, QStringLiteral("Чат карточки"));
        QVERIFY(history);
        QTRY_COMPARE(history->count(), 1);
        const auto openProfile = [&]() -> bool {
            const auto row = history->visualItemRect(history->item(0));
            const auto index = history->model()->index(0, 0);
            const auto *painter = static_cast<workspace::RowPainter *>(history->itemDelegate());
            for (int y = row.top(); y <= row.bottom(); ++y) {
                for (int x = row.left(); x <= row.right(); ++x) {
                    const QPoint link(x, y);
                    if (painter->anchorAt(index, row, link) == QLatin1String("/company/personal/user/19/")) {
                        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, link);
                        return true;
                    }
                }
            }
            return false;
        };
        QVERIFY(openProfile());
        QPointer<QDialog> profile = window.findChild<QDialog *>(QStringLiteral("personProfileDialog"));
        QVERIFY(profile && profile->isVisible() && profile->windowHandle());
        QCOMPARE(backend->requestedPerson, QStringLiteral("19"));
        auto *buttons = profile->findChild<QDialogButtonBox *>();
        QVERIFY(buttons && buttons->button(QDialogButtonBox::Close));
        auto *closeButton = buttons->button(QDialogButtonBox::Close);
        // The QWindow route applies Qt modality before a button can receive input.
        QTest::mouseClick(profile->windowHandle(), Qt::LeftButton, Qt::NoModifier,
                          closeButton->mapTo(profile, closeButton->rect().center()));
        QVERIFY(!profile || !profile->isVisible());
        QVERIFY(card->isVisible());
        QTRY_VERIFY(profile.isNull());

        QVERIFY(openProfile());
        profile = window.findChild<QDialog *>(QStringLiteral("personProfileDialog"));
        QVERIFY(profile && profile->isVisible() && profile->windowHandle());
        QCOMPARE(profile->windowHandle()->transientParent(), card->windowHandle());
        QVERIFY(profile->windowHandle()->close());
        QVERIFY(!profile || !profile->isVisible());
        QVERIFY(card->isVisible());
        QTRY_VERIFY(profile.isNull());

        QVERIFY(openProfile());
        profile = window.findChild<QDialog *>(QStringLiteral("personProfileDialog"));
        QVERIFY(profile && profile->isVisible());
        card->close();
        QTRY_VERIFY(profile.isNull());
        emit backend->personLoaded({QStringLiteral("19"), QStringLiteral("Старая карточка"), {}, {}});
        window.openMessageLink(QStringLiteral("/company/personal/user/20/"));
        QCOMPARE(backend->requestedPerson, QStringLiteral("20"));
        profile = window.findChild<QDialog *>(QStringLiteral("personProfileDialog"));
        QVERIFY(profile && profile->parentWidget() == &window);
        auto *name = findAccessible<QLabel>(profile, QStringLiteral("Имя сотрудника"));
        QVERIFY(name);
        emit backend->personLoaded({QStringLiteral("19"), QStringLiteral("Старый ответ"), {}, {}});
        QCOMPARE(name->text(), QStringLiteral("Загружаем профиль…"));
    }

    void invalidTaskLinksAreSuppressed()
    {
        const auto portal = QUrl(QStringLiteral("https://example.bitrix24.ru"));
        QVERIFY(ui::taskLink(portal, QStringLiteral("101"), QStringLiteral("../22")).isEmpty());
        QVERIFY(ui::taskLink(portal, QStringLiteral("0"), QStringLiteral("501")).isEmpty());
        QVERIFY(ui::taskLink(QUrl(QStringLiteral("https://name@example.bitrix24.ru")),
                             QStringLiteral("101"), QStringLiteral("501")).isEmpty());
        QVERIFY(ui::taskLink(QUrl(QStringLiteral("http://example.bitrix24.ru")),
                             QStringLiteral("101"), QStringLiteral("501")).isEmpty());
    }

    void uploadsKeepDraftAndCorrelateReplies()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdUpload = true;
        MainWindow window(backend.get());
        window.show();
        QApplication::setActiveWindow(&window);
        auto *fileButton = findAccessible<QPushButton>(&window, QStringLiteral("Прикрепить файл в чат"));
        auto *composer = findAccessible<QPlainTextEdit>(&window, QStringLiteral("Текст сообщения"));
        QVERIFY(fileButton);
        QVERIFY(composer);
        composer->setPlainText(QStringLiteral("Черновик"));

        QTemporaryFile file;
        QVERIFY(file.open());
        const auto path = file.fileName();
        file.close();
        QTimer::singleShot(0, &window, [path] {
            if (auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                dialog->selectFile(path);
                QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
            }
        });
        QTimer::singleShot(1500, &window, [] {
            if (auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget()))
                dialog->reject();
        });
        fileButton->click();
        QCOMPARE(backend->uploadedPath, path);
        QVERIFY(!fileButton->isEnabled());
        emit backend->fileUploaded(QStringLiteral("c-1"), QStringLiteral("task"));
        emit backend->failed(QStringLiteral("upload-chat:c-9"), QStringLiteral("Другой запрос"));
        QVERIFY(!fileButton->isEnabled());
        emit backend->failed(QStringLiteral("upload-chat:c-1"), QStringLiteral("Файл не загружен"));
        QVERIFY(fileButton->isEnabled());
        QVERIFY(backend->sentChat.isEmpty());
        QCOMPARE(composer->toPlainText(), QStringLiteral("Черновик"));
    }

    void exposesDistinctBadgesInChatRows()
    {
        QListWidget chats;
        Chat ordinary;
        ordinary.unread = 127;
        ordinary.pinned = true;
        ordinary.hasMention = true;
        auto *first = ui::addChatRow(&chats, ordinary);
        Chat task;
        task.taskChat = true;
        task.unread = 7;
        task.hasReaction = true;
        auto *second = ui::addChatRow(&chats, task);

        QCOMPARE(first->data(workspace::Count).toInt(), 127);
        QVERIFY(first->data(workspace::Pinned).toBool());
        QVERIFY(first->data(workspace::HasMention).toBool());
        QVERIFY(!first->data(workspace::HasReaction).toBool());
        QCOMPARE(second->data(workspace::Count).toInt(), 7);
        QVERIFY(!second->data(workspace::HasMention).toBool());
        QVERIFY(second->data(workspace::HasReaction).toBool());
        QVERIFY(first->data(Qt::AccessibleDescriptionRole).toString().contains(QStringLiteral("упоминание")));
        QVERIFY(second->data(Qt::AccessibleDescriptionRole).toString().contains(QStringLiteral("реакция")));
    }

    void dismissedChatsStayHiddenUnlessEnabledForThisAccount()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        backend->accountScope = QStringLiteral("example.bitrix24.ru/first-user");
        Chat dismissed;
        dismissed.id = QStringLiteral("453");
        dismissed.title = QStringLiteral("Бывший сотрудник");
        dismissed.kind = QStringLiteral("im");
        dismissed.pinned = true;
        dismissed.dismissed = true;
        backend->chats.append(dismissed);
        Chat group;
        group.id = QStringLiteral("chat475");
        group.title = QStringLiteral("Группа");
        group.kind = QStringLiteral("group");
        backend->chats.append(group);
        Notification notice;
        notice.id = QStringLiteral("n-dismissed");
        notice.title = QStringLiteral("Событие в чате");
        notice.targets = {{NotificationTargetKind::Chat, dismissed.id, QStringLiteral("421"),
                           {}, QStringLiteral("Открыть сообщение")}};
        backend->notifications = {notice};
        const auto contains = [](QListWidget *list, const QString &id) {
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->data(workspace::Id).toString() == id)
                    return true;
            return false;
        };

        {
            MainWindow window(backend.get());
            window.show();
            auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
            auto *profile = findAccessible<QPushButton>(&window, QStringLiteral("Настройки профиля"));
            auto *query = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск чатов"));
            auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
            auto *notices = findAccessible<QListWidget>(&window, QStringLiteral("Уведомления"));
            QVERIFY(chats && profile && query && sections && notices);
            QVERIFY(profile->isVisible());
            QVERIFY(contains(chats, group.id));
            QVERIFY(!contains(chats, dismissed.id));

            sections->setCurrentRow(2);
            notices->setCurrentRow(0);
            QTest::keyClick(notices, Qt::Key_Return);
            QCOMPARE(backend->contextDialog, dismissed.id);
            QCOMPARE(backend->contextMessage, QStringLiteral("421"));
            QVERIFY(!contains(chats, dismissed.id));

            query->setText(QStringLiteral("сотрудник"));
            QTest::keyClick(query, Qt::Key_Return);
            Chat external;
            external.id = QStringLiteral("765");
            external.title = QStringLiteral("Внешний участник");
            emit backend->chatsFound(query->text(), {dismissed, external});
            QVERIFY(!contains(chats, dismissed.id));
            QVERIFY(contains(chats, external.id));

            profile->click();
            auto *check = findAccessible<QCheckBox>(
                &window, QStringLiteral("Показывать чаты с уволенными сотрудниками"));
            QVERIFY(check && check->isVisible() && !check->isChecked());
            check->click();
            QVERIFY(contains(chats, dismissed.id));
            QCOMPARE(chats->item(0)->data(workspace::Id).toString(), dismissed.id);
            query->clear();
            QVERIFY(contains(chats, dismissed.id));
            QVERIFY(contains(chats, group.id));
        }

        MainWindow restored(backend.get());
        restored.show();
        auto *chats = findAccessible<QListWidget>(&restored, QStringLiteral("Недавние чаты"));
        auto *profile = findAccessible<QPushButton>(&restored, QStringLiteral("Настройки профиля"));
        QVERIFY(chats && profile);
        QVERIFY(contains(chats, dismissed.id));
        profile->click();
        auto *check = findAccessible<QCheckBox>(
            &restored, QStringLiteral("Показывать чаты с уволенными сотрудниками"));
        QVERIFY(check && check->isChecked());
        backend->accountScope = QStringLiteral("example.bitrix24.ru/second-user");
        emit backend->accountScopeChanged();
        QVERIFY(check->isEnabled());
        QVERIFY(!check->isChecked());
        QVERIFY(!contains(chats, dismissed.id));
        backend->accountScope.clear();
        emit backend->accountScopeChanged();
        QVERIFY(!check->isEnabled());
        QVERIFY(!check->isChecked());
        check->click();
        QVERIFY(!contains(chats, dismissed.id));
        MainWindow unscoped(backend.get());
        auto *unscopedChats = findAccessible<QListWidget>(&unscoped, QStringLiteral("Недавние чаты"));
        QVERIFY(unscopedChats);
        QVERIFY(!contains(unscopedChats, dismissed.id));
        backend->accountScope = QStringLiteral("example.bitrix24.ru/first-user");
        emit backend->accountScopeChanged();
        QVERIFY(check->isEnabled());
        QVERIFY(check->isChecked());
        QVERIFY(contains(chats, dismissed.id));
    }

    void logoutIsVisibleAtCompactWidthAndWorksFromKeyboard()
    {
        DemoBackend demo;
        MainWindow window(&demo);
        window.resize(720, 500);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *logout = findAccessible<QPushButton>(&window, QStringLiteral("Выйти из аккаунта"));
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(logout);
        QVERIFY(sections);
        QVERIFY(!logout->isVisible());
        QVERIFY(!logout->isEnabled());
        window.setLogoutAvailable(true);
        QCOMPARE(logout->text(), QStringLiteral("Выйти"));
        for (int row = 0; row < sections->count(); ++row) {
            sections->setCurrentRow(row);
            QVERIFY(logout->isVisible());
            QVERIFY(logout->isEnabled());
            QVERIFY(logout->parentWidget()->rect().contains(logout->geometry()));
        }
        sections->setCurrentRow(0);
        logout->setFocus(Qt::TabFocusReason);
        QTRY_VERIFY(logout->hasFocus());
        const auto screenshot = qEnvironmentVariable("B24_LOGOUT_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(window.grab().save(screenshot));

        QSignalSpy requests(&window, &MainWindow::logoutRequested);
        QTest::keyClick(logout, Qt::Key_Space);
        QCOMPARE(requests.size(), 1);
        QVERIFY(window.isVisible());
        window.setLogoutAvailable(false);
        logout->click();
        QCOMPARE(requests.size(), 1);
    }

    void pastedChatPictureWaitsInComposer()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        auto *input = findAccessible<QPlainTextEdit>(&window, QStringLiteral("Текст сообщения"));
        QVERIFY(input);

        QImage image(80, 40, QImage::Format_ARGB32);
        image.fill(QColor("#42788f"));
        auto *clipboard = QApplication::clipboard();
        auto *mime = new QMimeData;
        mime->setImageData(image);
        clipboard->setMimeData(mime);
        const auto clearClipboard = qScopeGuard([clipboard] { clipboard->clear(); });
        QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);

        auto *preview = findAccessible<QLabel>(&window, QStringLiteral("Изображение из буфера"));
        QVERIFY(preview && preview->isVisible());
        QVERIFY(!preview->pixmap().isNull());
        QVERIFY(backend->uploadedPath.isEmpty());
        QVERIFY(input->toPlainText().isEmpty());
    }

    void pastedChatPictureUploadsWithItsOwnDraft_data()
    {
        QTest::addColumn<bool>("taskChat");
        QTest::newRow("ordinary chat with caption") << false;
        QTest::newRow("task chat without caption") << true;
    }

    void pastedChatPictureUploadsWithItsOwnDraft()
    {
        QFETCH(bool, taskChat);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdUpload = true;
        backend->chats.append({QStringLiteral("c-2"), QStringLiteral("Другой чат")});
        auto secondTask = backend->stages.first().tasks.first();
        secondTask.id = QStringLiteral("502");
        backend->stages.first().tasks.append(secondTask);
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        const auto activeTaskCard = [&]() -> ui::TaskCardDialog * {
            const auto dialogs = window.findChildren<QDialog *>(QStringLiteral("taskCardDialog"));
            for (auto *dialog : dialogs) {
                auto *card = dynamic_cast<ui::TaskCardDialog *>(dialog);
                if (card && card->isVisible())
                    return card;
            }
            return nullptr;
        };
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
        QVERIFY(chats);
        QTRY_COMPARE(chats->count(), 2);

        QWidget *owner = &window;
        if (taskChat) {
            window.openTaskCard(QStringLiteral("501"));
            QTRY_VERIFY(activeTaskCard());
            owner = activeTaskCard();
        }
        const auto inputName = taskChat ? QStringLiteral("Текст в чате карточки") : QStringLiteral("Текст сообщения");
        auto *input = findAccessible<QPlainTextEdit>(owner, inputName);
        QVERIFY(input);
        auto *clipboard = QApplication::clipboard();
        const auto clearClipboard = qScopeGuard([clipboard] { clipboard->clear(); });
        auto *html = new QMimeData;
        html->setHtml(QStringLiteral("<b>Подпись</b>"));
        html->setText(QStringLiteral("Подпись"));
        html->setData(QStringLiteral("application/x-qt-image"), QByteArray("invalid optional image representation"));
        clipboard->setMimeData(html);
        QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(input->toPlainText(), QStringLiteral("Подпись"));
        if (taskChat)
            input->clear();
        const auto caption = input->toPlainText();

        const QColor color("#477f9d");
        QImage image(80, 40, QImage::Format_ARGB32);
        image.fill(color);
        if (taskChat) {
            QByteArray png;
            QBuffer buffer(&png);
            QVERIFY(buffer.open(QIODevice::WriteOnly));
            QVERIFY(image.save(&buffer, "PNG"));
            auto *raw = new QMimeData;
            raw->setData(QStringLiteral("image/png"), png);
            clipboard->setMimeData(raw);
        } else {
            clipboard->setImage(image);
        }
        QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
        auto *preview = findAccessible<QLabel>(owner, QStringLiteral("Изображение из буфера"));
        QVERIFY(preview && preview->isVisible());
        QCOMPARE(backend->imageUploadRequests, 0);

        if (taskChat) {
            auto *card = activeTaskCard();
            QVERIFY(card);
            card->setDiscardConfirmation([] { return true; });
            QVERIFY(card->close());
            window.openTaskCard(QStringLiteral("502"));
            QTRY_VERIFY(activeTaskCard());
            QCOMPARE(activeTaskCard()->taskId(), QStringLiteral("502"));
            auto *otherPreview = findAccessible<QLabel>(activeTaskCard(), QStringLiteral("Изображение из буфера"));
            QVERIFY(otherPreview && !otherPreview->isVisible());
            QVERIFY(activeTaskCard()->close());
            window.openTaskCard(QStringLiteral("501"));
            QTRY_VERIFY(activeTaskCard());
            owner = activeTaskCard();
            input = findAccessible<QPlainTextEdit>(owner, inputName);
            preview = findAccessible<QLabel>(owner, QStringLiteral("Изображение из буфера"));
        } else {
            chats->setCurrentRow(1);
            QVERIFY(!preview->isVisible());
            QVERIFY(input->toPlainText().isEmpty());
            chats->setCurrentRow(0);
        }
        QVERIFY(input && preview && preview->isVisible());
        QCOMPARE(input->toPlainText(), caption);

        const auto capture = qEnvironmentVariable("B24_CHAT_PASTE_SCREENSHOT");
        if (!capture.isEmpty() && !taskChat)
            QVERIFY(input->parentWidget()->grab().save(capture));
        auto *composer = static_cast<ui::ChatComposer *>(input->parentWidget());
        const auto restoredPath = composer->draft().image->path;
        auto *remove = findAccessible<QPushButton>(owner, QStringLiteral("Удалить изображение из черновика"));
        QVERIFY(remove && remove->isVisible());
        remove->click();
        QVERIFY(!preview->isVisible());
        QVERIFY(!QFileInfo::exists(restoredPath));
        QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
        QVERIFY(preview->isVisible());
        const auto replyId = QStringLiteral("999");
        composer->setReply({replyId, QStringLiteral("Анна"), QStringLiteral("Исходное сообщение")});
        QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(backend->imageUploadRequests, 0);
        QCOMPARE(composer->reply().id, replyId);
        QVERIFY(preview->isVisible());
        composer->setReply({});

        const auto scope = taskChat ? QStringLiteral("501") : QStringLiteral("c-1");
        const auto kind = taskChat ? QStringLiteral("task-chat") : QStringLiteral("chat");
        QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(backend->imageUploadRequests, 1);
        QCOMPARE(backend->imageUploadForTask, taskChat);
        QCOMPARE(backend->uploadedScope, scope);
        QCOMPARE(backend->uploadedCaption, caption);
        const auto uploaded = backend->uploadedPath;
        QVERIFY(QFileInfo::exists(uploaded));
        QVERIFY(uploaded.endsWith(QStringLiteral(".png")));
        const QImage stored(uploaded);
        QCOMPARE(stored.size(), image.size());
        QCOMPARE(stored.pixelColor(20, 20), color);
        QVERIFY(preview->isVisible());

        emit backend->failed((taskChat ? QStringLiteral("upload-task-chat:") : QStringLiteral("upload-chat:")) + scope,
                             QStringLiteral("Ошибка отправки"));
        QVERIFY(preview->isVisible());
        QVERIFY(QFileInfo::exists(uploaded));
        QCOMPARE(input->toPlainText(), caption);
        QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(backend->imageUploadRequests, 2);
        QCOMPARE(backend->uploadedPath, uploaded);
        const auto newerDraft = QStringLiteral("Новый текст");
        input->setPlainText(newerDraft);
        emit backend->fileUploaded(QStringLiteral("other"), kind);
        QVERIFY(preview->isVisible());
        emit backend->fileUploaded(scope, kind);
        QVERIFY(!preview->isVisible());
        QCOMPARE(input->toPlainText(), newerDraft);
        QVERIFY(!QFileInfo::exists(uploaded));

        QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
        QVERIFY(preview->isVisible());
        QVERIFY(remove->isVisible());
        remove->click();
        QVERIFY(!preview->isVisible());
        QCOMPARE(input->toPlainText(), newerDraft);
        QCOMPARE(backend->imageUploadRequests, 2);

        input->clear();
        QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
        QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(backend->imageUploadRequests, 3);
        emit backend->fileUploaded(scope, kind);
        QVERIFY(!preview->isVisible());
        QVERIFY(input->toPlainText().isEmpty());

        const auto waitCaption = QStringLiteral("Подпись уже отправляется");
        input->setPlainText(waitCaption);
        QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
        QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(backend->imageUploadRequests, 4);
        QVERIFY(remove->isVisible());
        remove->click();
        QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(backend->chatSendRequests, 0);
        QCOMPARE(backend->taskSendRequests, 0);
        QCOMPARE(input->toPlainText(), waitCaption);
        emit backend->fileUploaded(scope, kind);
        QVERIFY(input->toPlainText().isEmpty());
        QVERIFY(!QFileInfo::exists(backend->uploadedPath));
    }

    void taskCardCanCloseImmediatelyAfterSendAcknowledgement()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdMessageSends = true;
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        const auto taskId = QStringLiteral("501");
        window.openTaskCard(taskId);
        auto *dialog = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        const QPointer card(dynamic_cast<ui::TaskCardDialog *>(dialog));
        QVERIFY(card);
        QTRY_VERIFY(card->isVisible());
        QVERIFY(!card->hasUnsavedChanges());

        int confirmations = 0;
        card->setDiscardConfirmation([&confirmations] { ++confirmations; return false; });
        auto *input = card->messageInput();
        const auto sent = QStringLiteral("Ответ по задаче");
        input->setPlainText(sent);
        QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(backend->taskSendRequests, 1);
        QCOMPARE(backend->sentTask, sent);
        QVERIFY(card->hasUnsavedChanges());
        QVERIFY(!card->close());
        QCOMPARE(confirmations, 1);

        const auto historyRequests = backend->taskChatReadRequests.size();
        emit backend->messageSent(QStringLiteral("task:") + taskId, QStringLiteral("900"));
        QCOMPARE(backend->taskChatReadRequests.size(), historyRequests);
        QVERIFY(input->toPlainText().isEmpty());
        QVERIFY(!card->hasUnsavedChanges());

        input->setPlainText(QStringLiteral("Новый черновик"));
        QVERIFY(card->hasUnsavedChanges());
        QVERIFY(!card->close());
        QCOMPARE(confirmations, 2);

        input->clear();
        QVERIFY(card->close());
        QCOMPARE(confirmations, 2);
        QVERIFY(!card || !card->isVisible());
    }

    void taskCardWarnsBeforeClosingWithUnsavedChanges()
    {
        DemoBackend demo;
        demo.start();
        MainWindow window(&demo);
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        QTRY_COMPARE(sections->currentRow(), 1);
        auto *board = window.findChild<BoardWidget *>();
        QVERIFY(board);
        QTRY_VERIFY(board->findChildren<QListWidget *>().size() > 1);
        QListWidget *work = nullptr;
        QTRY_VERIFY((work = findAccessible<QListWidget>(board, QStringLiteral("В работе"))));
        QVERIFY(work->count() > 0);
        work->setFocus();
        QTest::keyClick(work, Qt::Key_Down);
        QTest::keyClick(work, Qt::Key_Return);
        auto *card = static_cast<ui::TaskCardDialog *>(window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        QVERIFY(card);
        QTRY_VERIFY(card->isVisible());
        auto *input = card->messageInput();
        QVERIFY(input);
        input->setPlainText(QStringLiteral("черновик сообщения"));
        QVERIFY(card->hasUnsavedChanges());

        int asked = 0;
        card->setDiscardConfirmation([&asked] { ++asked; return false; });
        QTest::keyClick(card, Qt::Key_Escape);
        QCOMPARE(asked, 1);
        QVERIFY(card->isVisible());
        card->close();
        QCOMPARE(asked, 2);
        QVERIFY(card->isVisible());
        QVERIFY(!window.confirmWorkspaceClose());
        QVERIFY(!window.close());
        QCOMPARE(asked, 4);
        QVERIFY(window.isVisible());
        QCOMPARE(input->toPlainText(), QStringLiteral("черновик сообщения"));

        bool nestedCloseRejected = false;
        card->setDiscardConfirmation([&] {
            ++asked;
            nestedCloseRejected = !window.closeWorkspace();
            return true;
        });
        QVERIFY(window.confirmWorkspaceClose());
        QVERIFY(card->isVisible());
        QVERIFY(window.closeWorkspace());
        QVERIFY(nestedCloseRejected);
        QCOMPARE(asked, 6);
        QVERIFY(!card->isVisible());
        demo.stop();
    }

    void taskCardReplacementKeepsCardWhenDiscardIsDeclined()
    {
        DemoBackend demo;
        demo.start();
        MainWindow window(&demo);
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        QTRY_COMPARE(sections->currentRow(), 1);
        auto *board = window.findChild<BoardWidget *>();
        QVERIFY(board);
        QTRY_VERIFY(board->findChildren<QListWidget *>().size() > 1);
        QListWidget *work = nullptr;
        QTRY_VERIFY((work = findAccessible<QListWidget>(board, QStringLiteral("В работе"))));
        QVERIFY(work->count() > 0);
        work->setFocus();
        QTest::keyClick(work, Qt::Key_Down);
        QTest::keyClick(work, Qt::Key_Return);
        auto *card = static_cast<ui::TaskCardDialog *>(window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        QVERIFY(card);
        QTRY_VERIFY(card->isVisible());
        const auto taskId = card->taskId();
        card->messageInput()->setPlainText(QStringLiteral("черновик"));
        int asked = 0;
        card->setDiscardConfirmation([&asked] { ++asked; return false; });

        window.openTaskCard(taskId == QStringLiteral("t-1") ? QStringLiteral("t-2") : QStringLiteral("t-1"));
        QCOMPARE(asked, 1);
        auto *current = static_cast<ui::TaskCardDialog *>(window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        QVERIFY(current);
        QCOMPARE(current->taskId(), taskId);
        QVERIFY(current->isVisible());
        QCOMPARE(current->messageInput()->toPlainText(), QStringLiteral("черновик"));
        demo.stop();
    }

    void historyEnterOpensPhotoLikeMouse()
    {
        ui::HistoryList list(nullptr);
        workspace::configureRows(&list, workspace::Row::Message);
        list.resize(420, 320);
        list.show();
        auto *item = workspace::addMarkupRow(&list, {}, QStringLiteral("Фотография"), {}, QStringLiteral("m-1"));
        MessageFile file;
        file.id = QStringLiteral("f-1");
        file.name = QStringLiteral("photo.jpg");
        file.isImage = true;
        file.downloadUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/photo.jpg"));
        item->setData(workspace::MessageFiles, QVariant::fromValue(QVector<MessageFile>{file}));
        list.setCurrentRow(0);
        list.setFocus();
        QTRY_VERIFY(list.hasFocus());

        int opened = 0;
        QString openedId;
        auto *delegate = static_cast<workspace::RowPainter *>(list.itemDelegate());
        QVERIFY(delegate);
        delegate->setMessagePhotoHandler([&opened, &openedId](const MessageFile &entry) {
            ++opened;
            openedId = entry.id;
        });
        QTest::keyClick(&list, Qt::Key_Return);
        QCOMPARE(opened, 1);
        QCOMPARE(openedId, QStringLiteral("f-1"));
    }

    void keepsSelectedTaskChatAtTheSameScrollPositionAfterRead_data()
    {
        QTest::addColumn<bool>("unreadOnly");
        QTest::newRow("all-task-chats") << false;
        QTest::newRow("unread-task-chats") << true;
    }

    void keepsSelectedTaskChatAtTheSameScrollPositionAfterRead()
    {
        QFETCH(bool, unreadOnly);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->accountScope = QStringLiteral("fixture/task-chat-before-read");
        for (int i = 0; i < 30; ++i) {
            Chat chat;
            chat.id = i == 11 ? QStringLiteral("task-dialog") : QStringLiteral("chat%1").arg(100 + i);
            chat.title = QStringLiteral("Чат задачи %1").arg(i);
            chat.taskId = QString::number(200 + i);
            chat.taskChat = true;
            chat.unread = 2;
            backend->taskChats.append(chat);
        }
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("chatScopeTabs"));
        auto *filter = findAccessible<QPushButton>(&window, QStringLiteral("Только непрочитанные чаты"));
        QVERIFY(tabs && filter);
        tabs->setCurrentIndex(1);
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Чаты задач"));
        QVERIFY(chats);
        if (unreadOnly)
            filter->click();
        QTRY_COMPARE(chats->count(), 30);

        const auto rowId = [](const QListWidgetItem *row) {
            return row ? row->data(workspace::Id).toString() : QString{};
        };
        const auto topRow = [chats] { return chats->itemAt(QPoint(4, 0)); };
        const auto anchorId = backend->taskChats[10].id;
        const auto activeId = backend->taskChats[11].id;
        const auto nextId = backend->taskChats[12].id;
        chats->setCurrentRow(11);
        chats->scrollToItem(chats->item(10), QAbstractItemView::PositionAtTop);
        auto *scroll = chats->verticalScrollBar();
        QTRY_COMPARE(rowId(topRow()), anchorId);
        scroll->setValue(scroll->value() + 7);
        QVERIFY(scroll->value() > 0);
        QCOMPARE(rowId(topRow()), anchorId);
        QVERIFY(chats->visualItemRect(topRow()).top() < 0);
        const int selectedOffset = chats->visualItemRect(chats->currentItem()).top();
        QSignalSpy historyReloaded(backend.get(), &Backend::chatMessagesLoaded);

        emit backend->chatReadConfirmed(activeId, {QStringLiteral("701")}, 701);
        backend->taskChats[1].unread = 0;
        backend->taskChats[11].unread = 0;
        emit backend->taskChatsLoaded(backend->taskChats);
        QTRY_COMPARE(rowId(chats->currentItem()), activeId);
        QCOMPARE(chats->currentItem()->data(workspace::Count).toInt(), 0);
        QCOMPARE(chats->currentRow(), 11);
        QVERIFY(chats->viewport()->rect().intersects(chats->visualItemRect(chats->currentItem())));
        QCOMPARE(chats->visualItemRect(chats->currentItem()).top(), selectedOffset);
        QCOMPARE(chats->count(), unreadOnly ? 29 : 30);
        QCOMPARE(historyReloaded.count(), 0);

        backend->taskChats.removeAt(11);
        emit backend->taskChatsLoaded(backend->taskChats);
        QTRY_COMPARE(rowId(chats->currentItem()), activeId);
        QCOMPARE(chats->visualItemRect(chats->currentItem()).top(), selectedOffset);
        QCOMPARE(historyReloaded.count(), 0);

        emit backend->taskChatsLoaded(backend->taskChats);
        QTRY_COMPARE(rowId(chats->currentItem()), activeId);
        QCOMPARE(chats->visualItemRect(chats->currentItem()).top(), selectedOffset);

        const auto lookup = [chats, &rowId](const QString &id) -> QListWidgetItem * {
            for (int row = 0; row < chats->count(); ++row) {
                if (rowId(chats->item(row)) == id)
                    return chats->item(row);
            }
            return nullptr;
        };
        auto *next = lookup(nextId);
        QVERIFY(next);
        chats->setCurrentItem(next);
        QTRY_COMPARE(rowId(chats->currentItem()), nextId);
        QTRY_VERIFY(!lookup(activeId));

        backend->taskChats[11].unread = 0;
        emit backend->taskChatsLoaded(backend->taskChats);
        QCOMPARE(rowId(chats->currentItem()), nextId);
        if (unreadOnly)
            filter->click();
        filter->click();
        QVERIFY(!lookup(nextId));

        const auto otherId = backend->taskChats[12].id;
        auto *other = lookup(otherId);
        QVERIFY(other);
        chats->setCurrentItem(other);
        backend->taskChats[12].unread = 0;
        emit backend->taskChatsLoaded(backend->taskChats);
        QCOMPARE(rowId(chats->currentItem()), otherId);
        backend->accountScope = QStringLiteral("fixture/task-chat-another-account");
        emit backend->accountScopeChanged();
        QVERIFY(!lookup(otherId));
    }

    void keepsTaskChatSelectionThroughChangingRecentSnapshots_data()
    {
        QTest::addColumn<bool>("initiallyRead");
        QTest::newRow("intermediate-unread") << false;
        QTest::newRow("already-read-on-selection") << true;
    }

    void keepsTaskChatSelectionThroughChangingRecentSnapshots()
    {
        QFETCH(bool, initiallyRead);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        const auto activeId = QStringLiteral("task-dialog");
        const int selectedRow = 9;
        for (int i = 0; i < 30; ++i) {
            Chat chat;
            chat.id = i == selectedRow ? activeId : QStringLiteral("chat%1").arg(100 + i);
            chat.title = QStringLiteral("Чат задачи %1").arg(i);
            chat.taskId = QString::number(200 + i);
            chat.taskChat = true;
            chat.unread = i < selectedRow || (i == selectedRow && !initiallyRead) ? 2 : 0;
            backend->taskChats.append(chat);
        }
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("chatScopeTabs"));
        QVERIFY(tabs);
        tabs->setCurrentIndex(1);
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Чаты задач"));
        QVERIFY(chats);
        QTRY_COMPARE(chats->count(), 30);
        const auto rowId = [](const QListWidgetItem *row) {
            return row ? row->data(workspace::Id).toString() : QString{};
        };
        chats->setCurrentRow(selectedRow);
        QCOMPARE(rowId(chats->currentItem()), activeId);
        chats->scrollToItem(chats->item(selectedRow - 1), QAbstractItemView::PositionAtTop);
        const int selectedOffset = chats->visualItemRect(chats->currentItem()).top();

        if (!initiallyRead) {
            emit backend->chatReadConfirmed(activeId, {QStringLiteral("701")}, 701);
            backend->taskChats[selectedRow].unread = 0;
            emit backend->taskChatsLoaded(backend->taskChats);
            QCOMPARE(chats->currentRow(), selectedRow);
        }
        backend->taskChats.move(selectedRow, 28);
        backend->taskChats[28].unread = initiallyRead ? 0 : 2;
        for (int row = 10; row < 15; ++row)
            backend->taskChats[row].unread = 2;
        emit backend->taskChatsLoaded(backend->taskChats);
        QCOMPARE(rowId(chats->currentItem()), activeId);
        QCOMPARE(chats->currentRow(), selectedRow);
        QCOMPARE(chats->visualItemRect(chats->currentItem()).top(), selectedOffset);
        QCOMPARE(chats->currentItem()->data(workspace::Count).toInt(), initiallyRead ? 0 : 2);

        auto *scroll = chats->verticalScrollBar();
        scroll->setValue(scroll->maximum());
        auto *top = chats->itemAt(QPoint(4, 0));
        QVERIFY(top);
        const auto topOffset = chats->visualItemRect(top).top();
        const auto topId = rowId(top);
        QVERIFY(!chats->viewport()->rect().intersects(chats->visualItemRect(chats->currentItem())));
        backend->taskChats[28].unread = 0;
        emit backend->taskChatsLoaded(backend->taskChats);
        QCOMPARE(rowId(chats->currentItem()), activeId);
        QCOMPARE(chats->currentRow(), selectedRow);
        QCOMPARE(rowId(chats->itemAt(QPoint(4, 0))), topId);
        QCOMPARE(chats->visualItemRect(chats->itemAt(QPoint(4, 0))).top(), topOffset);

        emit backend->taskChatsLoaded({backend->taskChats[28]});
        QCOMPARE(rowId(chats->currentItem()), activeId);
        QCOMPARE(chats->currentRow(), 0);
        emit backend->taskChatsLoaded(backend->taskChats);
        QCOMPARE(rowId(chats->currentItem()), activeId);
        QCOMPARE(chats->currentRow(), selectedRow);

        chats->setCurrentRow(selectedRow + 1);
        QTRY_VERIFY(rowId(chats->item(selectedRow)) != activeId);
    }

    void taskChatRowsStayInPlaceUntilMouseRelease_data()
    {
        QTest::addColumn<bool>("ungrab");
        QTest::addColumn<bool>("leaveSection");
        QTest::newRow("release") << false << false;
        QTest::newRow("ungrab") << true << false;
        QTest::newRow("switch-section") << false << true;
    }

    void taskChatRowsStayInPlaceUntilMouseRelease()
    {
        QFETCH(bool, ungrab);
        QFETCH(bool, leaveSection);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        const auto activeId = QStringLiteral("task-dialog");
        const auto targetId = QStringLiteral("chat201");
        for (const auto &id : {activeId, targetId, QStringLiteral("chat202"), QStringLiteral("chat203")}) {
            Chat chat;
            chat.id = id;
            chat.title = id;
            chat.taskChat = true;
            chat.unread = 2;
            backend->taskChats.append(chat);
        }
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("chatScopeTabs"));
        QVERIFY(tabs);
        tabs->setCurrentIndex(1);
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Чаты задач"));
        QVERIFY(chats);
        QTRY_COMPARE(chats->count(), 4);
        const auto idOf = [](const QListWidgetItem *item) {
            return item ? item->data(workspace::Id).toString() : QString{};
        };
        QCOMPARE(idOf(chats->currentItem()), activeId);
        backend->taskChats.removeFirst();
        emit backend->taskChatsLoaded(backend->taskChats);
        QCOMPARE(idOf(chats->item(0)), activeId);
        auto *target = chats->item(1);
        QCOMPARE(idOf(target), targetId);
        const auto point = chats->visualItemRect(target).center();
        QVERIFY(chats->viewport()->rect().contains(point));
        QString clicked;
        connect(chats, &QListWidget::itemClicked, &window, [&](const QListWidgetItem *item) { clicked = idOf(item); });

        QTest::mousePress(chats->viewport(), Qt::LeftButton, {}, point);
        QCOMPARE(idOf(chats->currentItem()), targetId);
        QCoreApplication::processEvents();
        QCOMPARE(idOf(chats->item(0)), activeId);
        QCOMPARE(chats->itemAt(point), target);

        emit backend->chatReadConfirmed(targetId, {QStringLiteral("701")}, 701);
        backend->taskChats[0].unread = 0;
        backend->taskChats[1].pinned = true;
        emit backend->taskChatsLoaded(backend->taskChats);
        backend->taskChats[2].pinned = true;
        emit backend->taskChatsLoaded(backend->taskChats);
        QCOMPARE(idOf(chats->itemAt(point)), targetId);
        QCOMPARE(chats->itemAt(point), target);

        if (ungrab) {
            QEvent cancelled(QEvent::UngrabMouse);
            QCoreApplication::sendEvent(chats->viewport(), &cancelled);
            QTRY_COMPARE(chats->count(), 3);
        }
        if (leaveSection) {
            auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
            QVERIFY(sections);
            backend->holdTaskChatDirectory = true;
            sections->setCurrentRow(1);
            QVERIFY(!chats->isVisible());
            QTest::mouseRelease(sections->viewport(), Qt::LeftButton, {}, sections->visualItemRect(sections->currentItem()).center());
            QCoreApplication::processEvents();
            QCOMPARE(chats->count(), 4);
            sections->setCurrentRow(0);
        } else {
            QTest::mouseRelease(chats->viewport(), Qt::LeftButton, {}, point);
        }
        QCOMPARE(clicked, ungrab || leaveSection ? QString{} : targetId);
        QTRY_COMPARE(chats->count(), 3);
        QCOMPARE(idOf(chats->currentItem()), targetId);
        QCOMPARE(idOf(chats->item(1)), QStringLiteral("chat203"));
        QCOMPARE(chats->currentItem()->data(workspace::Count).toInt(), 0);
        QCOMPARE(backend->chatRefreshCalls.last(), targetId);
    }

    void chatUnreadFilterWorksTogetherWithSearch()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        Chat unreadChat;
        unreadChat.id = QStringLiteral("c-2");
        unreadChat.title = QStringLiteral("Непрочитанный чат");
        unreadChat.unread = 3;
        backend->chats = {backend->chats.first(), unreadChat};
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
        QVERIFY(chats);
        QTRY_COMPARE(chats->count(), 2);

        auto *button = findAccessible<QPushButton>(&window, QStringLiteral("Только непрочитанные чаты"));
        QVERIFY(button);
        QVERIFY(button->isCheckable());
        button->click();
        QTRY_COMPARE(chats->count(), 1);
        QCOMPARE(chats->item(0)->data(workspace::Id).toString(), QStringLiteral("c-2"));
        auto *search = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск чатов"));
        QVERIFY(search);
        search->setText(QStringLiteral("чат"));
        QTest::keyClick(search, Qt::Key_Return);
        emit backend->chatsFound(QStringLiteral("чат"), backend->chats);
        QCOMPARE(chats->count(), 1);
        QCOMPARE(chats->item(0)->data(workspace::Id).toString(), unreadChat.id);

        button->click();
        QTRY_COMPARE(chats->count(), 2);
    }

    void projectSearchFiltersLoadedCatalogAndKeepsTaskFilter()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        Project second;
        second.id = QStringLiteral("102");
        second.name = QStringLiteral("Второй проект");
        backend->projects = {backend->projects.first(), second};
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        auto *projects = findAccessible<QListWidget>(&window, QStringLiteral("Проекты"));
        QVERIFY(projects);
        QTRY_COMPARE(projects->count(), 2);

        auto *search = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск по названию проектов"));
        QVERIFY(search);
        auto *taskFilter = findAccessible<QLineEdit>(&window, QStringLiteral("Фильтр задач"));
        QVERIFY(taskFilter);
        QVERIFY(taskFilter->text().isEmpty());
        search->setText(QStringLiteral("Второй"));
        QTRY_COMPARE(projects->count(), 1);
        QCOMPARE(projects->item(0)->data(workspace::Id).toString(), QStringLiteral("102"));
        QVERIFY(taskFilter->text().isEmpty());
        search->clear();
        QTRY_COMPARE(projects->count(), 2);
    }

    void projectAvatarUsesSystemIconUnlessCustomPhotoIsAvailable()
    {
        const auto render = [](const QString &type, const QPixmap &photo = {}) {
            QImage image(48, 48, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing);
            workspace::paintAvatar(painter, image.rect(), QStringLiteral("Проект"),
                                   workspace::AvatarKind::Project, photo, {}, {}, type);
            return image;
        };
        const auto initials = render(QString());
        QCOMPARE(render(QStringLiteral("unknown")), initials);
        const QStringList types{QStringLiteral("folder"), QStringLiteral("checks"), QStringLiteral("pie"),
                                QStringLiteral("bag"), QStringLiteral("members")};
        const QStringList aliases{QStringLiteral("folder"), QStringLiteral("tasks"), QStringLiteral("chart"),
                                  QStringLiteral("briefcase"), QStringLiteral("group")};
        QVector<QImage> icons;
        for (qsizetype i = 0; i < types.size(); ++i) {
            const auto system = render(types.at(i));
            QVERIFY(system != initials);
            QVERIFY(!icons.contains(system));
            QCOMPARE(render(aliases.at(i)), system);
            icons.append(system);
        }
        QPixmap custom(32, 32);
        custom.fill(QColor("#429360"));
        QCOMPARE(render(QStringLiteral("folder"), custom), render(QString(), custom));
        QCOMPARE(render(QStringLiteral("folder"), custom).pixelColor(24, 24), QColor("#429360"));
    }

    void displaysProjectUnreadCountsBeforeOpeningChats()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->accountScope = QStringLiteral("fixture/project-unread-first");
        backend->holdProjectTaskChats = true;
        backend->projects.first().pinned = true;
        backend->projects.append({QStringLiteral("102"), QStringLiteral("Склад"), {}});
        backend->projects[1].pinned = true;
        backend->projects.append({QStringLiteral("103"), QStringLiteral("Проверка"), {}});
        MainWindow window(backend.get());
        window.resize(850, 630);
        window.show();
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        auto *projects = findAccessible<QListWidget>(&window, QStringLiteral("Проекты"));
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("projectScopeTabs"));
        QVERIFY(sections && projects && tabs);
        sections->setCurrentRow(1);
        QTRY_COMPARE(projects->count(), 3);
        QCOMPARE(tabs->currentIndex(), 0);
        QVERIFY(backend->projectTaskChatRequests.isEmpty());

        auto *first = projects->item(0);
        auto *second = projects->item(1);
        const auto directoryIndex = projects->currentRow();
        emit backend->projectUnreadCountsChanged({
            {QStringLiteral("101"), 310}, {QStringLiteral("102"), 1021}, {QStringLiteral("104"), 6}}, true);
        QCOMPARE(projects->item(0), first);
        QCOMPARE(projects->item(1), second);
        QCOMPARE(projects->currentRow(), directoryIndex);
        QCOMPARE(first->data(workspace::Count).toLongLong(), 310);
        QCOMPARE(second->data(workspace::Count).toLongLong(), 1021);
        QCOMPARE(projects->item(2)->data(workspace::Count).toLongLong(), 0);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты (310)"));
        QVERIFY(first->data(Qt::AccessibleDescriptionRole).toString().contains(QStringLiteral("Закреплён")));
        QVERIFY(first->data(Qt::AccessibleDescriptionRole).toString().contains(QStringLiteral("310")));
        QVERIFY(backend->projectTaskChatRequests.isEmpty());
        if (const auto capture = qEnvironmentVariable("B24_PROJECT_UNREAD_SCREENSHOT"); !capture.isEmpty())
            QVERIFY(window.grab().save(capture));

        projects->setCurrentRow(1);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты (1021)"));
        QVERIFY(backend->projectTaskChatRequests.isEmpty());

        backend->projects[1].name = QStringLiteral("Склад материалов");
        backend->projects.append({QStringLiteral("104"), QStringLiteral("Следующая страница"), {}});
        emit backend->projectsLoaded(backend->projects);
        QCOMPARE(projects->currentItem()->data(workspace::Title).toString(), backend->projects[1].name);
        QCOMPARE(projects->currentItem()->data(workspace::Count).toLongLong(), 1021);
        QCOMPARE(projects->item(3)->data(workspace::Count).toLongLong(), 6);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты (1021)"));

        emit backend->projectPinChanged(QStringLiteral("102"), false);
        QCOMPARE(projects->currentItem()->data(workspace::Count).toLongLong(), 1021);
        QVERIFY(!projects->currentItem()->data(Qt::AccessibleDescriptionRole).toString().contains(QStringLiteral("Закреплён")));
        emit backend->projectPinChanged(QStringLiteral("102"), true);
        QVERIFY(projects->currentItem()->data(Qt::AccessibleDescriptionRole).toString().contains(QStringLiteral("Закреплён")));
        emit backend->projectUnreadCountsChanged({}, true);
        QCOMPARE(projects->item(0)->data(workspace::Count).toLongLong(), 0);
        QCOMPARE(projects->item(1)->data(workspace::Count).toLongLong(), 0);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты"));
        QVERIFY(backend->projectTaskChatRequests.isEmpty());

        emit backend->projectUnreadCountsChanged({{QStringLiteral("102"), 2}}, false);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты (2+)"));
        QVERIFY(projects->currentItem()->data(workspace::CountPartial).toBool());
        QVERIFY(projects->currentItem()->data(Qt::AccessibleDescriptionRole).toString().contains(QStringLiteral("не менее 2")));
        tabs->setCurrentIndex(1);
        QCOMPARE(backend->projectTaskChatRequests.last(), qMakePair(QStringLiteral("102"), 0));
        Chat chat;
        chat.id = QStringLiteral("chat42");
        chat.taskId = QStringLiteral("501");
        chat.title = QStringLiteral("Задача");
        chat.taskChat = true;
        chat.unread = 5;
        emit backend->projectTaskChatsLoaded(QStringLiteral("102"), 0, {chat}, 50);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты (5+)"));
        emit backend->projectUnreadCountsChanged({{QStringLiteral("102"), 9}}, false);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты (9+)"));
        emit backend->projectUnreadCountsChanged({}, true);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты"));
        QCOMPARE(projects->currentItem()->data(workspace::Count).toLongLong(), 0);
        emit backend->projectUnreadCountsChanged({{QStringLiteral("102"), 4}}, true);
        tabs->setCurrentIndex(0);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты (4)"));

        backend->accountScope = QStringLiteral("fixture/project-unread-second");
        emit backend->accountScopeChanged();
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты"));
        QCOMPARE(projects->currentItem()->data(workspace::Count).toLongLong(), 0);
    }

    void projectTaskChatPaneStartsOnTheTaskView()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        sections->setCurrentRow(1);
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("projectScopeTabs"));
        QVERIFY(tabs);
        QCOMPARE(tabs->tabText(0), QStringLiteral("Задачи"));
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты"));
        QCOMPARE(tabs->currentIndex(), 0);
        auto *tasks = findAccessible<QLineEdit>(&window, QStringLiteral("Фильтр задач"));
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Чаты задач проекта"));
        QVERIFY(tasks && chats);
        QVERIFY(tasks->isVisible());
        QVERIFY(!chats->isVisible());
        tabs->setCurrentIndex(1);
        QVERIFY(!tasks->isVisible());
        tabs->setCurrentIndex(0);
        QVERIFY(tasks->isVisible());
    }

    void projectTaskChatsPageAndUpdateOnlyTheOpenProject()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->accountScope = QStringLiteral("fixture/project-chat-first");
        backend->holdProjectTaskChats = true;
        backend->projects.first().pinned = true;
        backend->projects.append({QStringLiteral("102"), QStringLiteral("Склад"), {}});
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        auto *projects = findAccessible<QListWidget>(&window, QStringLiteral("Проекты"));
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("projectScopeTabs"));
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Чаты задач проекта"));
        auto *more = window.findChild<QPushButton *>(QStringLiteral("moreProjectTaskChats"));
        auto *state = findAccessible<QLabel>(&window, QStringLiteral("Состояние чатов проекта"));
        QVERIFY(sections && projects && tabs && chats && more && state);
        sections->setCurrentRow(1);
        QTRY_COMPARE(projects->currentItem()->data(workspace::Id).toString(), QStringLiteral("101"));
        QVERIFY(backend->projectTaskChatRequests.isEmpty());
        tabs->setCurrentIndex(1);
        QCOMPARE(backend->projectTaskChatRequests.size(), 1);
        QCOMPARE(backend->projectTaskChatRequests.last(), qMakePair(QStringLiteral("101"), 0));
        QVERIFY(state->isVisible());
        QVERIFY(!more->isVisible());
        QVERIFY(!chats->isVisible());

        Chat first;
        first.id = QStringLiteral("chat42");
        first.title = QStringLiteral("Монтаж");
        first.taskId = QStringLiteral("501");
        first.taskChat = true;
        first.unread = 3;
        Chat second = first;
        second.id = QStringLiteral("chat43");
        second.taskId = QStringLiteral("502");
        second.title = QStringLiteral("Доставка");
        second.unread = 2;
        emit backend->projectTaskChatsLoaded(QStringLiteral("102"), 0, {second}, -1);
        emit backend->projectTaskChatsLoaded(QStringLiteral("101"), 50, {second}, -1);
        QCOMPARE(chats->count(), 0);
        emit backend->projectTaskChatsLoaded(QStringLiteral("101"), 0, {first}, 50);
        QTRY_VERIFY(chats->isVisible() && more->isVisible());
        QCOMPARE(chats->item(0)->data(workspace::Count).toInt(), 3);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты (3+)"));
        QVERIFY(tabs->tabToolTip(1).contains(QStringLiteral("в загруженных чатах")));
        if (const auto capture = qEnvironmentVariable("B24_PROJECT_CHATS_SCREENSHOT"); !capture.isEmpty())
            QVERIFY(window.grab().save(capture));

        first.unread = 0;
        Chat badge;
        badge.id = first.id;
        badge.serverChatId = 42;
        emit backend->projectTaskChatBadgesChanged(QStringLiteral("102"), {badge});
        QCOMPARE(chats->item(0)->data(workspace::Count).toInt(), 3);
        emit backend->projectTaskChatBadgesChanged(QStringLiteral("101"), {badge, second});
        QCOMPARE(chats->count(), 1);
        QCOMPARE(chats->item(0)->data(workspace::Count).toInt(), 0);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты"));
        more->click();
        more->click();
        QCOMPARE(backend->projectTaskChatRequests.size(), 2);
        QCOMPARE(backend->projectTaskChatRequests.last(), qMakePair(QStringLiteral("101"), 50));
        QVERIFY(!more->isEnabled());
        emit backend->failed(QStringLiteral("project-task-chats:102"), QStringLiteral("Чужая ошибка"));
        QVERIFY(!more->isEnabled());
        emit backend->failed(QStringLiteral("project-task-chats:101"), QStringLiteral("Не удалось загрузить"));
        QTRY_VERIFY(more->isEnabled());
        QCOMPARE(more->text(), QStringLiteral("Повторить"));
        QCOMPARE(chats->count(), 1);
        more->click();
        QCOMPARE(backend->projectTaskChatRequests.last(), qMakePair(QStringLiteral("101"), 50));
        emit backend->projectTaskChatsLoaded(QStringLiteral("101"), 50, {first, second}, -1);
        QCOMPARE(chats->count(), 2);
        QVERIFY(!more->isVisible());
        QCOMPARE(chats->item(1)->data(workspace::Count).toInt(), 2);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты (2)"));

        emit chats->itemActivated(chats->item(0));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        QCOMPARE(backend->taskReadRequests.last(), first.taskId);
        card->close();
        QTRY_VERIFY(!window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        projects->setCurrentRow(1);
        QCOMPARE(projects->currentItem()->data(workspace::Id).toString(), QStringLiteral("102"));
        QCOMPARE(backend->projectTaskChatRequests[backend->projectTaskChatRequests.size() - 2], qMakePair(QString(), 0));
        QCOMPARE(backend->projectTaskChatRequests.last(), qMakePair(QStringLiteral("102"), 0));
        emit backend->projectTaskChatsLoaded(QStringLiteral("101"), 0, {first}, -1);
        QCOMPARE(chats->count(), 0);
        emit backend->projectTaskChatsLoaded(QStringLiteral("102"), 0, {second}, -1);
        QCOMPARE(chats->item(0)->data(workspace::TaskId).toString(), second.taskId);

        const auto requests = backend->projectTaskChatRequests.size();
        emit backend->taskCreated(QStringLiteral("101"), QStringLiteral("503"));
        QCOMPARE(backend->projectTaskChatRequests.size(), requests);
        emit backend->taskCreated(QStringLiteral("102"), QStringLiteral("504"));
        QCOMPARE(backend->projectTaskChatRequests.size(), requests + 2);
        QCOMPARE(backend->projectTaskChatRequests.last(), qMakePair(QStringLiteral("102"), 0));
        emit backend->projectTaskChatsLoaded(QStringLiteral("102"), 0, {second}, -1);
        QCOMPARE(chats->count(), 1);

        backend->accountScope = QStringLiteral("fixture/project-chat-second");
        emit backend->accountScopeChanged();
        QCOMPARE(backend->projectTaskChatRequests.last(), qMakePair(QString(), 0));
        emit backend->projectTaskChatsLoaded(QStringLiteral("102"), 0, {second}, -1);
        QCOMPARE(chats->count(), 0);
    }

    void projectTaskChatsRefreshAfterMovingATask_data()
    {
        QTest::addColumn<bool>("showSource");
        QTest::newRow("moving-out") << true;
        QTest::newRow("moving-in") << false;
    }

    void projectTaskChatsRefreshAfterMovingATask()
    {
        QFETCH(bool, showSource);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        const auto source = QStringLiteral("101");
        const auto target = QStringLiteral("102");
        const auto selected = showSource ? source : target;
        auto &task = backend->stages.first().tasks.first();
        task.projectId = source;
        task.editRights.project = true;
        backend->projects.append({target, QStringLiteral("Склад"), {}});
        backend->holdTaskUpdate = true;
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        window.onConnectionChanged(true);
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        auto *projects = findAccessible<QListWidget>(&window, QStringLiteral("Проекты"));
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("projectScopeTabs"));
        QVERIFY(sections && projects && tabs);
        sections->setCurrentRow(1);
        QTRY_COMPARE(projects->count(), 2);
        projects->setCurrentRow(showSource ? 0 : 1);
        tabs->setCurrentIndex(1);
        QCOMPARE(backend->projectTaskChatRequests.last(), qMakePair(selected, 0));
        window.openMessageLink(ui::taskLink(backend->portalOrigin(), source, task.id));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        auto *project = findAccessible<QPushButton>(card, QStringLiteral("Проект"));
        QVERIFY(project && project->isEnabled());
        project->click();
        auto *choices = findAccessible<QListWidget>(card, QStringLiteral("Проект задачи"));
        QVERIFY(choices);
        const auto targetRows = choices->findItems(QStringLiteral("Склад"), Qt::MatchExactly);
        QCOMPARE(targetRows.size(), 1);
        choices->setCurrentItem(targetRows.first());
        QVERIFY(backend->updatedTask.isEmpty());
        const auto boardRequests = backend->boardRequests;
        const auto requests = backend->projectTaskChatRequests.size();
        QTest::keyClick(choices, Qt::Key_Return);
        QCOMPARE(backend->lastUpdate.projectId, std::optional<QString>(target));
        QCOMPARE(backend->projectTaskChatRequests.size(), requests);
        task.projectId = target;
        emit backend->taskUpdated(task.id);
        QCOMPARE(backend->projectTaskChatRequests.size(), requests + 2);
        QCOMPARE(backend->projectTaskChatRequests[requests], qMakePair(QString(), 0));
        QCOMPARE(backend->projectTaskChatRequests.last(), qMakePair(selected, 0));
        QCOMPARE(backend->boardRequests, boardRequests);
    }

    void projectDirectoryDrawsThePinBesideTheTitle()
    {
        QListWidget list;
        auto *painter = workspace::configureRows(&list, workspace::Row::Directory);
        auto *row = workspace::addRow(&list, QStringLiteral("Проект"), {}, {}, QStringLiteral("101"));
        row->setData(workspace::AvatarType, int(workspace::AvatarKind::Project));
        const auto paintRow = [&] {
            QImage image(QSize(190, 52), QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter output(&image);
            QStyleOptionViewItem option;
            option.rect = image.rect();
            painter->paint(&output, option, list.indexFromItem(row));
            return image.copy(QRect(158, 7, 22, 18));
        };
        const auto withoutPin = paintRow();
        row->setData(workspace::Pinned, true);
        QVERIFY(paintRow() != withoutPin);
    }

    void projectRailKeepsPagesAndShowsIncompleteSearch()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        Project second;
        second.id = QStringLiteral("102");
        second.name = QStringLiteral("Проект со второй страницы");
        backend->moreProjects = {second};
        Notification notice;
        notice.id = QStringLiteral("71");
        notice.title = QStringLiteral("Проект");
        notice.targets = {{NotificationTargetKind::Project, second.id, {}, {}, QStringLiteral("Открыть проект")}};
        backend->notifications = {notice};

        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        auto *projects = findAccessible<QListWidget>(&window, QStringLiteral("Проекты"));
        auto *search = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск по названию проектов"));
        auto *more = window.findChild<QPushButton *>(QStringLiteral("moreProjects"));
        QVERIFY(sections && projects && search && more);
        sections->setCurrentRow(1);
        QCOMPARE(backend->projectRequests, 1);
        QVERIFY(more->isVisible() && more->isEnabled());
        search->setText(QStringLiteral("второй"));
        QCOMPARE(projects->item(0)->data(workspace::Detail).toString(),
                 QStringLiteral("В загруженных проектах совпадений нет"));
        QCOMPARE(backend->projectMoreRequests, 0);
        QTest::keyClick(more, Qt::Key_Space);
        QCOMPARE(backend->projectMoreRequests, 1);
        QCOMPARE(projects->item(0)->data(workspace::Id).toString(), second.id);
        QVERIFY(more->isHidden());

        sections->setCurrentRow(0);
        sections->setCurrentRow(1);
        QCOMPARE(backend->projectRequests, 1);
        QCOMPARE(projects->count(), 1);
        QCOMPARE(projects->item(0)->data(workspace::Id).toString(), second.id);
        search->setText(QStringLiteral("Нет совпадений"));
        QCOMPARE(projects->item(0)->data(workspace::Detail).toString(), QStringLiteral("Проектов по запросу нет"));
        sections->setCurrentRow(2);
        auto *notices = findAccessible<QListWidget>(&window, QStringLiteral("Уведомления"));
        QVERIFY(notices && notices->count() == 1);
        notices->setCurrentRow(0);
        QTest::keyClick(notices, Qt::Key_Return);
        QCOMPARE(sections->currentRow(), 1);
        QCOMPARE(backend->projectRequests, 1);
        QVERIFY(search->text().isEmpty());
        QVERIFY(projects->currentItem());
        QCOMPARE(projects->currentItem()->data(workspace::Id).toString(), second.id);

        QTest::keyClick(&window, Qt::Key_F5);
        QCOMPARE(backend->projectRequests, 2);
        emit backend->projectCreated(QStringLiteral("103"));
        QCOMPARE(backend->projectRequests, 3);
    }

    void projectRailLoadsOnForwardScrollAndRetriesExactlyOnce()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->projects.clear();
        for (int i = 101; i < 128; ++i)
            backend->projects.append({QString::number(i), QStringLiteral("Проект %1").arg(i), {}});
        backend->moreProjects = {{QStringLiteral("128"), QStringLiteral("Следующая страница"), {}}};
        backend->holdProjectMore = true;
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        auto *projects = findAccessible<QListWidget>(&window, QStringLiteral("Проекты"));
        auto *more = window.findChild<QPushButton *>(QStringLiteral("moreProjects"));
        QVERIFY(sections && projects && more);
        sections->setCurrentRow(1);
        auto *scroll = projects->verticalScrollBar();
        QTRY_VERIFY(scroll->maximum() > 10);
        QTRY_VERIFY(more->isHidden());
        QCOMPARE(backend->projectMoreRequests, 0);
        projects->setCurrentRow(4);
        const auto selection = projects->currentItem()->data(workspace::Id).toString();
        const auto threshold = [&] { return scroll->minimum() + ((scroll->maximum() - scroll->minimum()) * 2 + 2) / 3; };
        scroll->setValue(threshold() / 2);
        scroll->triggerAction(QAbstractSlider::SliderSingleStepAdd);
        QTest::qWait(40);
        QVERIFY(scroll->value() < threshold());
        QCOMPARE(backend->projectMoreRequests, 0);
        const auto boundary = threshold();
        scroll->setTracking(false);
        scroll->setSliderDown(true);
        scroll->setSliderPosition(boundary);
        scroll->triggerAction(QAbstractSlider::SliderMove);
        scroll->setSliderDown(false);
        scroll->setTracking(true);
        QCOMPARE(scroll->value(), boundary);
        QTRY_COMPARE(backend->projectMoreRequests, 1);
        emit backend->projectsMoreAvailable(true); // This signal precedes the fetched page.
        scroll->triggerAction(QAbstractSlider::SliderToMaximum);
        QTest::qWait(40);
        QCOMPARE(backend->projectMoreRequests, 1);

        emit backend->failed(QStringLiteral("projects"), QStringLiteral("Страница недоступна"));
        QTRY_VERIFY(more->isVisible() && more->isEnabled());
        QVERIFY(more->text().contains(QStringLiteral("Повторить")));
        scroll->setValue(threshold() - 1);
        scroll->triggerAction(QAbstractSlider::SliderToMaximum);
        QTest::qWait(40);
        QCOMPARE(backend->projectMoreRequests, 1);
        more->click();
        QCOMPARE(backend->projectMoreRequests, 2);
        QVERIFY(!more->isEnabled());

        const auto position = scroll->value();
        auto extended = backend->projects + backend->moreProjects;
        for (int i = 129; i < 140; ++i)
            extended.append({QString::number(i), QStringLiteral("Проект %1").arg(i), {}});
        emit backend->projectsMoreAvailable(true);
        emit backend->projectsLoaded(extended);
        QTRY_COMPARE(projects->count(), extended.size());
        QTRY_COMPARE(scroll->value(), position);
        QCOMPARE(projects->currentItem()->data(workspace::Id).toString(), selection);
        QTRY_VERIFY(more->isHidden());
        QTest::qWait(40);
        QCOMPARE(backend->projectMoreRequests, 2);
        scroll->triggerAction(QAbstractSlider::SliderToMaximum);
        QTRY_COMPARE(backend->projectMoreRequests, 3);
        emit backend->projectsMoreAvailable(false);
        emit backend->projectsLoaded(extended);
        scroll->setValue(scroll->minimum());
        scroll->triggerAction(QAbstractSlider::SliderToMaximum);
        QTest::qWait(40);
        QCOMPARE(backend->projectMoreRequests, 3);
        QVERIFY(more->isHidden());
    }

    void narrowPanelShowsFullNameTooltips()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        Project second;
        second.id = QStringLiteral("102");
        second.name = QStringLiteral("Очень длинное название второго проекта для узкой панели");
        backend->projects = {backend->projects.first(), second};
        MainWindow window(backend.get());
        window.resize(720, 500);
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        auto *projects = findAccessible<QListWidget>(&window, QStringLiteral("Проекты"));
        QVERIFY(projects);
        QTRY_COMPARE(projects->count(), 2);
        bool found = false;
        for (int row = 0; row < projects->count(); ++row) {
            const auto tooltip = projects->item(row)->toolTip();
            if (tooltip.contains(QStringLiteral("Очень длинное название второго проекта для узкой панели")))
                found = true;
        }
        QVERIFY(found);
        for (int i = 0; i < projects->count(); ++i)
            if (projects->item(i)->data(workspace::Id).toString() == second.id)
                projects->setCurrentRow(i);
        auto *title = findAccessible<QLabel>(&window, QStringLiteral("Открытый проект"));
        QVERIFY(title);
        QVERIFY(title->toolTip().contains(QStringLiteral("Очень длинное название второго проекта для узкой панели")));
        QVERIFY(title->width() >= title->fontMetrics().horizontalAdvance(QStringLiteral("название")));
    }
private:
    QTemporaryDir m_settings;
};

QTEST_MAIN(PilotUiTest)
#include "tst_pilot_ui.moc"
