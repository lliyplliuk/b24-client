#pragma once

#include "core/operations.h"
#include "core/models.h"
#include "core/worktime.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>
#include <QVector>
#include <functional>
#include <memory>
#include <optional>

class QTemporaryDir;

namespace bx {

// Transport-agnostic contract used by the UI. Both DemoBackend (synthetic)
// and HttpBackend (real portal endpoints) implement it.
class Backend : public QObject {
    Q_OBJECT
public:
    explicit Backend(QObject *parent = nullptr) : QObject(parent) {}
    ~Backend() override = default;

    virtual QString displayName() const = 0;
    virtual bool isDemo() const = 0;
    virtual QUrl portalOrigin() const = 0;
    virtual QString settingsScope() const { return {}; }
    static QString reactionOperationKey(const QString &dialogId, const QString &messageId, const QString &code)
    {
        return operationKey(OperationKind::MessageReaction, dialogId, messageId, code);
    }
    virtual void start() = 0;
    virtual void stop() = 0;

    // Bounded, on-demand reads (see core/tunables.h for the bound values).
    virtual void requestRecentChats() = 0;
    virtual void requestTaskChats() { emit taskChatsLoaded({}); }
    virtual void requestTaskChatBadges() {}
    virtual void requestProjectTaskChats(const QString &projectId, int offset = 0)
    {
        if (!projectId.isEmpty())
            emit projectTaskChatsLoaded(projectId, offset, {}, -1);
    }
    virtual std::optional<qint64> unreadChatCount() const { return std::nullopt; }
    virtual void requestAvatar(const QUrl &) {}
    using MessageViewersHandler = std::function<void(std::optional<MessageViewers>)>;
    virtual void requestMessageViewers(const QString &, const QString &, bool, MessageViewersHandler complete)
    {
        complete(std::nullopt);
    }
    virtual void requestMessageImage(const QUrl &url) { emit messageImageLoaded(url, {}); }
    using LinkPreviewHandler = std::function<void(MessageLinkPreview)>;
    virtual void requestMessageLinkPreview(const QUrl &, LinkPreviewHandler complete) { complete({}); }
    using ChatLinkPreviewHandler = std::function<void(QVector<ChatAsset>)>;
    virtual void requestChatLinkPreviews(const QString &, bool, ChatLinkPreviewHandler complete) { complete({}); }
    virtual void downloadMessageFile(const QUrl &, const QString &)
    {
        emit failed(operationKey(OperationKind::MessageFileDownload), QStringLiteral("Скачивание файла недоступно"));
    }
    virtual void downloadMessagePhoto(const QUrl &, const QString &targetPath)
    {
        emit messagePhotoDownloadFailed(targetPath, QStringLiteral("Оригинал изображения недоступен"));
    }
    virtual void cancelMessagePhoto(const QString &) {}
    virtual void downloadMessageVideo(const QUrl &, const QString &targetPath)
    {
        emit messageVideoDownloadFailed(targetPath, QStringLiteral("Видео недоступно"));
    }
    virtual void cancelMessageVideo(const QString &) {}
    virtual void requestChatMessages(const QString &chatId) = 0;
    virtual void requestOlderChatMessages(const QString &scopeId, const QString &beforeId, bool task)
    {
        emit olderChatMessagesLoaded(scopeId, beforeId, task, {}, false);
    }
    virtual void requestPinnedMessages(const QString &dialogId) { emit pinnedMessagesLoaded(dialogId, {}); }
    virtual void readChatMessages(const QString &, const QStringList &, const QStringList & = {}) {}
    virtual void readTaskChatMessages(const QString &, const QStringList &, const QStringList & = {}) {}
    virtual void requestProjects() = 0;
    virtual void requestProjectMembers(const QString &projectId) { emit projectMembersLoaded(projectId, {}); }
    virtual void requestProjectFiles(const QString &projectId, const QString &folderId, int)
    {
        emit projectFilesLoaded(projectId, folderId, {}, -1);
    }
    virtual void requestMoreProjects() {}
    virtual void requestProjectBoard(const QString &projectId) = 0;
    virtual void requestMoreBoardTasks(const QString &, const QString &) {}
    virtual void requestProjectStages(const QString &projectId) { emit projectStagesLoaded(projectId, {}); }
    virtual void requestTask(const QString &taskId) = 0;
    virtual void requestTaskChecklist(const QString &taskId) { emit taskChecklistLoaded(taskId, {}); }
    virtual void addTaskChecklistItem(const QString &, const QString &, const QString &) {}
    virtual void setTaskChecklistComplete(const QString &, const QString &, bool) {}
    virtual void requestSubtasks(const QString &taskId) { emit subtasksLoaded(taskId, {}); }
    virtual void createSubtask(const QString &, const QString &, const TaskEdit &) {}
    virtual void requestTaskDependencies(const QString &taskId) { emit taskDependenciesLoaded(taskId, {}); }
    virtual void requestTaskHistory(const QString &taskId) { emit taskHistoryLoaded(taskId, {}, false); }
    virtual void requestProjectTimeline(const QString &projectId) { emit projectTimelineLoaded(projectId, {}, false); }
    virtual void requestTaskMessages(const QString &taskId) = 0;
    virtual void notifyChatTyping(const QString &) {}
    virtual void requestChatAssets(const QString &, ChatAssetKind, const QString & = {}) {}
    virtual void changeMessage(const QString &, const QString &, MessageAction, const QString & = {}) {}
    virtual void forwardMessage(const QString &, const QString &, const QString &) {}
    virtual void createGroupChat(const QString &, const QStringList &) {}
    virtual void changeChatMember(const QString &, const QString &, bool) {}
    virtual void requestNotifications() = 0;
    virtual void requestOlderNotifications() {}
    virtual void searchNotifications(const QString &query)
    {
        emit failed(operationKey(OperationKind::SearchNotifications, query), QStringLiteral("Поиск уведомлений недоступен"));
    }
    virtual void markNotificationRead(const QString &)
    {
        emit failed(operationKey(OperationKind::ReadNotification), QStringLiteral("Изменение уведомлений недоступно"));
    }
    virtual void markNotificationsRead(const QStringList &)
    {
        emit failed(operationKey(OperationKind::ReadNotifications), QStringLiteral("Изменение уведомлений недоступно"));
    }
    virtual void markAllNotificationsRead()
    {
        emit failed(operationKey(OperationKind::ReadAllNotifications), QStringLiteral("Изменение уведомлений недоступно"));
    }
    virtual void deleteNotification(const QString &)
    {
        emit failed(operationKey(OperationKind::DeleteNotification), QStringLiteral("Удаление уведомлений недоступно"));
    }
    virtual void requestPortalStatus() {}
    virtual void updatePortalStatus(const QString &, const QString &) {}
    virtual void requestWorkday() { emit workdayLoaded({}); }
    virtual void changeWorkday(WorkdayAction, const QString & = {})
    {
        emit failed(operationKey(OperationKind::WorkdayChange), QStringLiteral("Рабочий день недоступен"));
    }
    virtual void changeWorkdayAt(WorkdayAction, const QDate &, const QTime &, const QString &,
                                 const QString & = {})
    {
        emit failed(operationKey(OperationKind::WorkdayChange), QStringLiteral("Изменение времени рабочего дня недоступно"));
    }
    virtual void requestWorktimeSheet(const QDate &month)
    {
        emit worktimeSheetLoaded({month, {}, false, QStringLiteral("Табель недоступен")});
    }
    virtual void exportWorktimeSheet(const QDate &month, const QString &)
    {
        emit failed(operationKey(OperationKind::WorktimeExport, month.toString(QStringLiteral("yyyy-MM"))),
                    QStringLiteral("Экспорт табеля недоступен"));
    }
    virtual void requestNotificationSettings()
    {
        emit failed(operationKey(OperationKind::NotificationSettings), QStringLiteral("Настройки уведомлений недоступны"));
    }
    virtual void updateNotificationSetting(const QString &, int)
    {
        emit failed(operationKey(OperationKind::NotificationSettingsUpdate), QStringLiteral("Настройки уведомлений недоступны"));
    }
    virtual void requestMessageContext(const QString &dialogId, const QString &messageId)
    {
        emit failed(operationKey(OperationKind::MessageContext, dialogId, messageId),
                    QStringLiteral("История сообщения недоступна"));
    }
    virtual void requestPerson(const QString &id)
    {
        emit failed(operationKey(OperationKind::Person, id), QStringLiteral("Профиль недоступен"));
    }
    using PersonHandler = std::function<void(Person)>;
    virtual void requestCurrentPerson(PersonHandler complete) { complete({}); }
    virtual void searchPeople(const QString &requestId, const QString &, const QString & = {})
    {
        emit peopleFound(requestId, {});
    }

    // Writes against the user session only.
    virtual void sendChatMessage(const QString &chatId, const QString &text) = 0;
    virtual void sendTaskMessage(const QString &taskId, const QString &text) = 0;
    virtual void sendChatReply(const QString &dialogId, const QString &, const QString &)
    {
        emit failed(operationKey(OperationKind::SendChat, dialogId), QStringLiteral("Ответ с цитатой недоступен"));
    }
    virtual void sendTaskReply(const QString &taskId, const QString &, const QString &)
    {
        emit failed(operationKey(OperationKind::SendTask, taskId), QStringLiteral("Ответ с цитатой недоступен"));
    }
    virtual void updateTask(const QString &taskId, const TaskEdit &edit) = 0;
    virtual void deleteTask(const QString &taskId)
    {
        emit failed(operationKey(OperationKind::DeleteTask, taskId), QStringLiteral("Удаление задачи недоступно"));
    }
    virtual void createTask(const QString &projectId, const TaskEdit &edit) = 0;
    virtual void createProject(const QString &name) = 0;
    virtual void moveTask(const QString &projectId, const QString &taskId, const QString &stageId) = 0;
    virtual void saveStage(const QString &projectId, const QString &stageId, const QString &title,
                           const QString &afterStageId, const QString &color = {}) = 0;
    virtual void deleteStage(const QString &projectId, const QString &stageId) = 0;
    virtual void searchChats(const QString &query) = 0;
    virtual void searchTaskChats(const QString &query) { emit taskChatsFound(query, {}); }
    virtual void setChatPinned(const QString &dialogId, bool pinned)
    {
        Q_UNUSED(pinned);
        emit failed(operationKey(OperationKind::ChatPin, dialogId), QStringLiteral("Закрепление чатов недоступно"));
    }
    virtual void setProjectPinned(const QString &projectId, bool pinned)
    {
        Q_UNUSED(pinned);
        emit failed(operationKey(OperationKind::ProjectPin, projectId), QStringLiteral("Закрепление проектов недоступно"));
    }
    virtual void searchProjectTasks(const QString &projectId, const QString &query)
    {
        Q_UNUSED(query);
        emit failed(operationKey(OperationKind::SearchProjectTasks, projectId), QStringLiteral("Поиск задач недоступен"));
    }
    virtual void searchMessages(const QString &scopeId, const QString &query, bool task) = 0;
    virtual void uploadChatFile(const QString &scopeId, const QString &localPath, bool task) = 0;
    virtual void attachTaskFile(const QString &taskId, const QString &localPath) = 0;
    virtual void uploadChatImage(const QString &scopeId, const QString &localPath, bool task, const QString &caption,
                                 std::shared_ptr<QTemporaryDir> = {})
    {
        if (caption.isEmpty()) {
            uploadChatFile(scopeId, localPath, task);
            return;
        }
        emit failed(operationKey(task ? OperationKind::UploadTaskChat : OperationKind::UploadChat, scopeId),
                    QStringLiteral("Отправка изображения с подписью недоступна"));
    }

signals:
    void workdayLoaded(const WorkdayStatus &status);
    void workdayChanged(const WorkdayStatus &status);
    void worktimeSheetLoaded(const WorktimeSheet &sheet);
    void worktimeSheetExported(const QDate &month, const QString &targetPath);
    void recentChatsLoaded(const QVector<Chat> &chats);
    void taskChatsLoaded(const QVector<Chat> &chats);
    void projectTaskChatsLoaded(const QString &projectId, int offset, const QVector<Chat> &chats, int nextOffset);
    void projectTaskChatBadgesChanged(const QString &projectId, const QVector<Chat> &chats);
    void projectUnreadCountsChanged(const QHash<QString, qint64> &counts, bool complete);
    void unreadChatCountChanged(qint64 count);
    void avatarLoaded(const QUrl &url, const QByteArray &bytes);
    void messageImageLoaded(const QUrl &url, const QByteArray &bytes);
    void messageLinkPreviewAccessChanged(bool available);
    void messageFileDownloaded(const QUrl &url, const QString &targetPath);
    void messagePhotoDownloadFailed(const QString &targetPath, const QString &reason);
    void messageVideoDownloaded(const QUrl &url, const QString &targetPath);
    void messageVideoDownloadFailed(const QString &targetPath, const QString &reason);
    void chatMessagesLoaded(const QString &chatId, const QVector<Message> &messages);
    void pinnedMessagesLoaded(const QString &dialogId, const QVector<Message> &messages);
    void chatContactChanged(const QString &dialogId, const QDateTime &onlineUntil, const QString &position);
    void projectsLoaded(const QVector<Project> &projects);
    void projectMembersLoaded(const QString &projectId, const QVector<ProjectMember> &members);
    void projectFilesLoaded(const QString &projectId, const QString &folderId, const QVector<ProjectDiskItem> &files,
                            int nextOffset);
    void projectsScopeLoaded(bool membershipVerified);
    void projectsMoreAvailable(bool available);
    void boardLoaded(const QString &projectId, const QVector<Stage> &stages);
    void boardTasksPageLoaded(const QString &projectId, const QString &stageId,
                              const QVector<Task> &tasks, bool hasMore);
    void projectStagesLoaded(const QString &projectId, const QVector<Stage> &stages);
    void taskSummaryLoaded(const Task &task);
    void taskLoaded(const Task &task);
    void taskChecklistLoaded(const QString &taskId, const QVector<TaskChecklistItem> &items);
    void taskChecklistChanged(const QString &taskId);
    void subtasksLoaded(const QString &taskId, const QVector<Task> &tasks);
    void subtaskCreated(const QString &parentId, const QString &projectId, const QString &taskId);
    void taskDependenciesLoaded(const QString &taskId, const QVector<TaskDependency> &dependencies);
    void taskHistoryLoaded(const QString &taskId, const QVector<TaskHistoryEntry> &history, bool limited);
    void projectTimelineLoaded(const QString &projectId, const QVector<Task> &tasks, bool limited);
    void taskMessagesLoaded(const QString &taskId, const QVector<Message> &messages);
    void notificationsLoaded(const QVector<Notification> &notifications);
    void notificationHistoryPageStateChanged(bool hasMore, bool loading);
    void notificationsFound(const QString &query, const QVector<Notification> &notifications);
    void notificationReadStateChanged(const QStringList &ids, bool read);
    void notificationRemoved(const QString &id);
    void notificationActionFinished(const QString &operation);
    void portalStatusLoaded(const QString &status);
    void portalStatusUpdated(const QString &status);
    void notificationSettingsLoaded(const QVariantMap &settings, const QString &updated = {});
    void messageContextLoaded(const QString &dialogId, const QString &messageId, const QVector<Message> &messages);
    void personLoaded(const Person &person);
    void chatReadConfirmed(const QString &dialogId, const QStringList &exactlyRead, qint64 readThrough);
    void olderChatMessagesLoaded(const QString &scopeId, const QString &beforeId, bool task,
                                const QVector<Message> &messages, bool hasMore);
    void chatHistoryRemoved(const QString &dialogId, const QStringList &messageIds, qint64 through = 0);
    void chatBadgeAnchorAvailable(const QString &dialogId);
    void peopleFound(const QString &requestId, const QVector<Person> &people);
    void messageSent(const QString &scopeId, const QString &messageId);
    void chatMessagesSeen(const QString &dialogId, const QStringList &messageIds);
    void chatTypingChanged(const QString &dialogId, const QString &userId, int durationMs);
    void chatAssetsLoaded(const QString &dialogId, ChatAssetKind kind, const QString &cursor,
                          const QVector<ChatAsset> &assets, const QString &nextCursor);
    void chatActionSucceeded(const QString &dialogId, const QString &message);
    void messageEditAccepted(const QString &dialogId, const QString &messageId, const QString &text);
    void chatOwnReactionChanged(const QString &dialogId, const QString &messageId, const QString &code, bool present);
    void groupChatCreated(const QString &dialogId);
    void taskUpdated(const QString &taskId);
    void taskDeleted(const QString &taskId);
    void taskLinkPreviewInvalidated(const QString &taskId);
    void taskCreated(const QString &projectId, const QString &taskId);
    void projectCreated(const QString &projectId);
    void boardUpdated(const QString &projectId);
    void chatsFound(const QString &query, const QVector<Chat> &chats);
    void taskChatsFound(const QString &query, const QVector<Chat> &chats);
    void chatPinChanged(const QString &dialogId, bool pinned);
    void projectPinChanged(const QString &projectId, bool pinned);
    void projectTasksFound(const QString &projectId, const QString &query, const QVector<Task> &tasks);
    void messagesFound(const QString &scopeId, const QString &query, bool task,
                       const QVector<Message> &messages);
    void fileUploaded(const QString &scopeId, const QString &kind);
    void taskFileAttached(const QString &taskId, const QString &fileId);
    void failed(const QString &operation, const QString &reason);
    void connectionChanged(bool online);
    void accountScopeChanged();
};

} // namespace bx
