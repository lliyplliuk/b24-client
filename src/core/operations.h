#pragma once

#include <QStringList>

namespace bx {

enum class OperationKind {
    Unknown,
    AttachTask,
    Board,
    BoardColumn,
    BoardEdit,
    Chat,
    ChatAssets,
    ChatBadgesSnapshot,
    ChatHistoryIdentity,
    ChatHistoryRead,
    ChatHistorySnapshot,
    ChatImageUpload,
    ChatMember,
    ChatPin,
    ChatSoundSettings,
    ChatTyping,
    Chats,
    Checklist,
    ChecklistWrite,
    CreateChat,
    CreateProject,
    CreateSubtask,
    CreateTask,
    CreateTaskFromMessage,
    CsrfRefresh,
    CurrentPerson,
    DeleteNotification,
    DeleteTask,
    ForwardMessage,
    GanttLinks,
    InvalidMessageReaction,
    LinkPreview,
    LinkPreviewChatAssets,
    LinkPreviewStage,
    MessageAction,
    MessageContext,
    MessageEdit,
    MessageFileDownload,
    MessageReaction,
    MessageViewers,
    NotificationBatch,
    NotificationSettings,
    NotificationSettingsUpdate,
    Notifications,
    NotificationsOlder,
    OlderChat,
    Person,
    Pins,
    PortalStatus,
    PortalStatusBootstrap,
    PortalStatusUpdate,
    ProjectFiles,
    ProjectMembers,
    ProjectPageFallback,
    ProjectPin,
    ProjectPinIdentity,
    ProjectStages,
    ProjectTaskChats,
    ProjectUnreadDialogs,
    ProjectUnreadTasks,
    Projects,
    Push,
    ReadAllNotifications,
    ReadNotification,
    ReadNotifications,
    ReadVisibleAnchors,
    ReadVisibleMessages,
    RecentUserStatus,
    SearchChat,
    SearchChats,
    SearchNotifications,
    SearchPeople,
    SearchProjectTasks,
    SearchTaskChat,
    SearchTaskChats,
    SendChat,
    SendTask,
    SessionKeepalive,
    SessionRecovery,
    SettingsIdentity,
    Subtasks,
    Task,
    TaskCardAction,
    TaskCardRead,
    TaskChatTitles,
    TaskChatTitlesRequest,
    TaskChats,
    TaskDetails,
    TaskFiles,
    TaskHistory,
    TaskMessages,
    Timeline,
    UpdateTask,
    UploadChat,
    UploadTaskChat,
    Workday,
    WorkdayChange,
    WorktimeExport,
    WorktimeSheet,
    Count
};

struct ParsedOperation {
    OperationKind kind = OperationKind::Unknown;
    QStringList ids;
};

QString operationKey(OperationKind kind, const QStringList &ids = {});
inline QString operationKey(OperationKind kind, const QString &id)
{
    return operationKey(kind, QStringList{id});
}
inline QString operationKey(OperationKind kind, const QString &first, const QString &second)
{
    return operationKey(kind, QStringList{first, second});
}
inline QString operationKey(OperationKind kind, const QString &first, const QString &second, const QString &third)
{
    return operationKey(kind, QStringList{first, second, third});
}
ParsedOperation parseOperation(const QString &key);
QString operationErrorHint(OperationKind kind);

} // namespace bx
