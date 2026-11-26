#pragma once

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

#include <QAbstractEventDispatcher>
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
#include <QFontDatabase>
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

namespace bx::test {

namespace {

// These clocks are value members without a QObject parent. Explicit instantiation
// gives the tests access without changing widget ownership or exposing a UI API.
template<typename Owner, typename Clock, Clock Owner::*member, typename... Tags>
struct WidgetClock {
    friend Clock &testClock(Owner &owner, Tags...) { return owner.*member; }
};

QTimer &testClock(ui::ChatComposer &);
template struct WidgetClock<ui::ChatComposer, QTimer, &ui::ChatComposer::m_typingExpiry>;
struct TypingDelay {};
QTimer &testClock(ui::ChatComposer &, TypingDelay);
template struct WidgetClock<ui::ChatComposer, QTimer, &ui::ChatComposer::m_typingDelay, TypingDelay>;
QTimer &testClock(ui::TaskCardDialog &);
template struct WidgetClock<ui::TaskCardDialog, QTimer, &ui::TaskCardDialog::m_copyFeedbackTimer>;
QVariantAnimation &testClock(ui::HistoryList &);
template struct WidgetClock<ui::HistoryList, QVariantAnimation, &ui::HistoryList::m_wheelAnimation>;
QTimer &testClock(ui::LoadingClock &);
template struct WidgetClock<ui::LoadingClock, QTimer, &ui::LoadingClock::m_timer>;

} // namespace

void fireTimer(QTimer &timer);
void deliverPendingTimer(MainWindow &window, const QString &name);


template<typename Widget>
Widget *findAccessible(const QWidget *window, const QString &name)
{
    for (auto *child : window->findChildren<Widget *>()) {
        if (child->accessibleName() == name)
            return child;
    }
    return nullptr;
}

QMenu *visibleMessageMenu(const QString &name = QStringLiteral("messageContextMenu"));

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

    void requestRecentChats() override;
    QStringList chatRefreshCalls;
    void requestTaskChats() override;
    void requestTaskChatBadges() override;
    int taskChatRequests = 0;
    bool holdTaskChatDirectory = false;
    int taskChatBadgeRequests = 0;
    bool provideTaskBadges = false;
    std::optional<qint64> unreadChatCount() const override { return railUnread; }
    std::optional<qint64> railUnread;
    void requestAvatar(const QUrl &url) override;
    void requestMessageImage(const QUrl &url) override;
    bool holdMessageImages = false;
    void downloadMessageVideo(const QUrl &url, const QString &path) override;
    void cancelMessageVideo(const QString &path) override;
    QVector<QPair<QUrl, QString>> videoRequests;
    QStringList cancelledVideos;

    void requestMessageLinkPreview(const QUrl &url, LinkPreviewHandler complete) override;
    bool holdLinkPreviews = false;
    QVector<QPair<QUrl, LinkPreviewHandler>> linkPreviewRequests;
    void requestMessageViewers(const QString &scope, const QString &id, bool task, MessageViewersHandler complete) override;
    bool holdViewerResponses = false;
    QVector<MessageViewersHandler> viewerReplies;
    struct ViewedReceipt {
        QString scope;
        QString id;
        bool task;
    };
    QVector<ViewedReceipt> viewedReceipts;
    QUrl viewerAvatar{QStringLiteral("https://example.bitrix24.ru/upload/viewer.png")};
    void requestChatLinkPreviews(const QString &scope, bool task, ChatLinkPreviewHandler complete) override;
    bool holdExternalLinkPreviews = false;
    QVector<QPair<QPair<QString, bool>, ChatLinkPreviewHandler>> externalLinkPreviewRequests;
    void setChatPinned(const QString &id, bool pinned) override;
    void setProjectPinned(const QString &id, bool pinned) override;
    void searchTaskChats(const QString &query) override { searchedTaskChats = query; }
    void requestChatMessages(const QString &chatId) override;
    void requestOlderChatMessages(const QString &scope, const QString &before, bool task) override;
    QString olderScope;
    QString olderBefore;
    bool olderTask = false;
    struct ReadCall {
        QString scope;
        QStringList incoming;
        QStringList visible;
        bool task;
    };
    void readChatMessages(const QString &dialog, const QStringList &incoming, const QStringList &visible) override;
    void readTaskChatMessages(const QString &task, const QStringList &incoming, const QStringList &visible) override;

    void requestPinnedMessages(const QString &dialog) override;

    void requestMessageContext(const QString &dialogId, const QString &messageId) override;
    void requestPerson(const QString &id) override;
    QStringList personRequests;
    void requestCurrentPerson(PersonHandler complete) override;
    Person currentPerson{QStringLiteral("19"), QStringLiteral("Виктория Сергеева"), {}, {}};
    bool holdCurrentPerson = false;
    QList<PersonHandler> currentPersonRequests;
    void downloadMessageFile(const QUrl &url, const QString &path) override;
    void downloadMessagePhoto(const QUrl &url, const QString &path) override;
    void cancelMessagePhoto(const QString &path) override { photoCancels.append(path); }
    void requestChatAssets(const QString &scope, ChatAssetKind kind, const QString &cursor) override;
    QVector<ChatAsset> mediaAssets;
    void requestProjects() override;
    void requestMoreProjects() override;
    void requestProjectTaskChats(const QString &projectId, int offset) override;
    QList<QPair<QString, int>> projectTaskChatRequests;
    bool holdProjectTaskChats = false;
    void requestProjectStages(const QString &projectId) override;
    void requestProjectTimeline(const QString &projectId) override;
    void requestMoreBoardTasks(const QString &projectId, const QString &stageId) override;
    void requestProjectMembers(const QString &projectId) override;
    void requestProjectFiles(const QString &projectId, const QString &folderId, int) override;
    void requestProjectBoard(const QString &projectId) override;
    void requestTask(const QString &taskId) override;
    void requestTaskMessages(const QString &taskId) override;
    bool holdTaskDetails = false;
    QStringList taskChatReadRequests;
    void searchPeople(const QString &requestId, const QString &query, const QString &dialogId) override;
    void requestNotifications() override;
    void requestOlderNotifications() override;
    void requestWorkday() override;
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
    void requestNotificationSettings() override;
    void updateNotificationSetting(const QString &name, int value) override;

    void sendChatMessage(const QString &chatId, const QString &text) override;
    void sendTaskMessage(const QString &taskId, const QString &text) override;
    void sendChatReply(const QString &chatId, const QString &text, const QString &replyId) override;
    void sendTaskReply(const QString &taskId, const QString &text, const QString &replyId) override;

    void updateTask(const QString &taskId, const TaskEdit &edit) override;
    void deleteTask(const QString &taskId) override { deletedTasks.append(taskId); }
    QStringList deletedTasks;
    void createTask(const QString &projectId, const TaskEdit &edit) override;
    void createSubtask(const QString &parentId, const QString &projectId, const TaskEdit &edit) override;
    void createProject(const QString &name) override;
    void moveTask(const QString &projectId, const QString &taskId, const QString &stageId) override;
    void saveStage(const QString &projectId, const QString &stageId, const QString &title,
                   const QString &afterStageId, const QString &color) override;
    void deleteStage(const QString &projectId, const QString &stageId) override;
    void searchChats(const QString &query) override { searchedChats = query; }
    void searchMessages(const QString &scopeId, const QString &query, bool task) override;
    void searchProjectTasks(const QString &projectId, const QString &query) override;
    void uploadChatFile(const QString &scopeId, const QString &localPath, bool task) override;
    void uploadChatImage(const QString &scopeId, const QString &localPath, bool task, const QString &caption,
                         std::shared_ptr<QTemporaryDir>) override;
    void attachTaskFile(const QString &taskId, const QString &localPath) override;

    void changeMessage(const QString &dialog, const QString &message, MessageAction action, const QString &value) override;

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

FixtureBackend *makeFixture();

class PilotFixture : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

private:
    QTemporaryDir m_settings;
};

} // namespace bx::test
