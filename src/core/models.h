#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>
#include <optional>

namespace bx {

// Value types exchanged between backends and UI. Deliberately plain structs:
// the demo backend and the HTTP transport must produce identical structures so
// the UI layer is transport agnostic.
struct Chat {
    QString id;
    QString title;
    QString kind;          // "chat" | "im" | "group" ...
    int unread = 0;
    QString lastMessage;
    QDateTime updated;
    bool taskChat = false;
    QString taskId{};
    QUrl avatarUrl{};
    bool pinned = false;
    bool markedUnread = false;
    bool dismissed = false;
    QDateTime onlineUntil{};
    QString position{};
    bool hasMention = false;
    bool hasReaction = false;
    qint64 serverChatId = 0;
};

struct MessageFile {
    QString id;
    QString name;
    qint64 size = -1;
    QUrl downloadUrl;
    QUrl previewUrl{};
    QSize previewSize{};
    QString diskObjectId{};
    bool isImage = false;
    QUrl originalPreviewUrl{};
    bool isVideo = false;
};

enum class ChatAssetKind { Media, Files, Links };

struct ChatAsset {
    QString id;
    QString messageId;
    QString name;
    QUrl link;
    MessageFile file;
    QString description{};
    QUrl thumbnailUrl{};
};

enum class MessageAction { React, RemoveReaction, Edit, Delete, Pin, Unpin, Favorite, Unfavorite, CreateTask };

struct MessageReaction {
    QString code;
    int count = 0;
    bool own = false;
};

struct Message {
    QString id;
    QString author;
    QString text;
    QDateTime createdAt;
    bool outgoing = false;
    QStringList attachments;
    bool unread = false;
    bool forwarded = false;
    QString forwardedAuthor{};
    QString replyId{};
    QString replyAuthor{};
    QString replyText{};
    QUrl authorAvatarUrl{};
    QVector<MessageFile> files{};
    QVector<MessageFile> replyFiles{};
    bool system = false;
    QVector<MessageReaction> reactions{};
    bool seenByOthers = false;
};

struct Project {
    QString id;
    QString name;
    QString group;
    QUrl avatarUrl{};
    bool pinned = false;
    QString avatarVariant{};
    QString avatarColor{};
    QString avatarType{};
};

enum class MessageLinkPreviewKind { External, Task, Project, Person, Chat };

struct MessageLinkPreview {
    QString title;
    QStringList details;
    MessageLinkPreviewKind kind = MessageLinkPreviewKind::External;
    QUrl avatarUrl{};
    std::optional<Project> project{};
    QUrl thumbnailUrl{};

    const QUrl &imageUrl() const { return project ? project->avatarUrl : avatarUrl; }
    QUrl creatorAvatarUrl{};
    QUrl assigneeAvatarUrl{};
    QString stageColor{};
};

enum class ProjectMemberRole { Owner, Moderator, Member, Invited, Awaiting };

struct ProjectMember {
    QString id;
    QString name;
    QString position;
    ProjectMemberRole role = ProjectMemberRole::Member;
};

struct ProjectDiskItem {
    MessageFile file;
    bool folder = false;
};

struct TaskPerson {
    QString id;
    QString name;
    QUrl avatarUrl{};
};

struct TaskEditRights {
    bool title = false;
    bool description = false;
    bool priority = false;
    bool creator = false;
    bool assignee = false;
    bool observers = false;
    bool deadline = false;
    bool project = false;
    bool stage = false;
    bool rate = false;
    bool remove = false;
    bool delegate = false;
};

enum class TaskStatusAction { Start, Pause, Complete, Renew, Defer, Approve, Disapprove, Take };
enum class TaskRating { Unrated, Positive, Negative };

struct Task {
    QString id;
    QString name;
    QString stageId;
    QString stageName;
    QString assignee;
    QString description;
    QString due;
    QString priority;
    QStringList attachments;
    QVector<MessageFile> files{};
    QString projectId{};
    QString project{};
    QString status{};
    QString createdBy{};
    QStringList observers{};
    QString descriptionChecksum{};
    QDateTime createdAt{};
    QString chatDialogId{};
    QDateTime planStart{};
    QDateTime planEnd{};
    QString creatorId{};
    QString assigneeId{};
    QVector<TaskPerson> observerPeople{};
    QString statusCode{};
    TaskEditRights editRights{};
    QVector<TaskStatusAction> statusActions{};
    bool deadlineReasonRequired = false;
    QStringList tags{};
    QUrl creatorAvatarUrl{};
    QUrl assigneeAvatarUrl{};
    TaskRating rating = TaskRating::Unrated;
};

struct TaskEdit {
    std::optional<QString> title;
    std::optional<QString> description;
    std::optional<QString> deadline;
    std::optional<bool> highPriority;
    std::optional<QString> descriptionChecksum;
    std::optional<QString> originalDescription;
    std::optional<QString> creatorId{};
    std::optional<QString> assigneeId{};
    std::optional<QStringList> observerIds{};
    std::optional<QStringList> originalObserverIds{};
    std::optional<QString> projectId{};
    std::optional<QString> stageId{};
    std::optional<TaskStatusAction> statusAction{};
    std::optional<QString> deadlineReason{};
    std::optional<TaskRating> rating{};
    bool requireDelegate = false;

    [[nodiscard]] bool hasChanges() const
    {
        return title || description || deadline || highPriority || creatorId || assigneeId
            || observerIds || projectId || stageId || statusAction || rating;
    }
};

struct TaskChecklistItem {
    QString id;
    QString parentId;
    QString title;
    bool complete = false;
    bool canToggle = false;
};

struct TaskDependency {
    QString taskId;
    QString title;
    QString type;
    bool prerequisite = false;
};

struct TaskHistoryEntry {
    QString id;
    QString event;
    QString author;
    QDateTime at;
    QString detail;
};

struct Stage {
    QString id;
    QString name;
    QVector<Task> tasks;
    std::optional<qsizetype> taskCount = std::nullopt;
    bool hasMoreTasks = false;
    QString color{};
};

enum class NotificationTargetKind { Chat, Task, Person, Project };

struct NotificationTarget {
    NotificationTargetKind kind = NotificationTargetKind::Chat;
    QString id;
    QString messageId;
    QString projectId;
    QString label;
    bool taskChat = false;
};

struct Person {
    QString id;
    QString name;
    QString position;
    QUrl avatarUrl;
};

struct MessageViewers {
    QVector<Person> people;
    qint64 count = -1;
    bool hasMore = false;
};

struct Notification {
    QString id;
    QString type;
    QString title;
    QString text;
    QDateTime createdAt;
    bool read = false;
    QVector<NotificationTarget> targets{};
    QString markup{};
    bool ordinaryChatMessage = false;
};

} // namespace bx

Q_DECLARE_METATYPE(bx::Chat)
Q_DECLARE_METATYPE(bx::Message)
Q_DECLARE_METATYPE(bx::Project)
Q_DECLARE_METATYPE(bx::ChatAssetKind)
Q_DECLARE_METATYPE(QVector<bx::ChatAsset>)
Q_DECLARE_METATYPE(bx::Task)
Q_DECLARE_METATYPE(bx::Stage)
Q_DECLARE_METATYPE(bx::Notification)
Q_DECLARE_METATYPE(bx::Person)
Q_DECLARE_METATYPE(QVector<bx::Chat>)
Q_DECLARE_METATYPE(QVector<bx::Message>)
Q_DECLARE_METATYPE(QVector<bx::Project>)
Q_DECLARE_METATYPE(QVector<bx::ProjectMember>)
Q_DECLARE_METATYPE(QVector<bx::ProjectDiskItem>)
Q_DECLARE_METATYPE(QVector<bx::Task>)
Q_DECLARE_METATYPE(QVector<bx::TaskChecklistItem>)
Q_DECLARE_METATYPE(QVector<bx::TaskDependency>)
Q_DECLARE_METATYPE(QVector<bx::TaskHistoryEntry>)
Q_DECLARE_METATYPE(QVector<bx::Stage>)
Q_DECLARE_METATYPE(QVector<bx::Notification>)
