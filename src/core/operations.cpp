#include "operations.h"

#include <iterator>

namespace bx {
namespace {

struct OperationSpec {
    OperationKind kind;
    QLatin1String prefix;
    int idCount;
};

constexpr OperationSpec operations[] = {
    {OperationKind::Unknown, {}, 0},
    {OperationKind::AttachTask, QLatin1String("attach-task:"), 1},
    {OperationKind::Board, QLatin1String("board:"), 1},
    {OperationKind::BoardColumn, QLatin1String("board-column:"), 2},
    {OperationKind::BoardEdit, QLatin1String("board-edit:"), 1},
    {OperationKind::Chat, QLatin1String("chat:"), 1},
    {OperationKind::ChatAssets, QLatin1String("chat-assets:"), 3},
    {OperationKind::ChatBadgesSnapshot, QLatin1String("chat-badges-snapshot"), 0},
    {OperationKind::ChatHistoryIdentity, QLatin1String("chat-history-identity"), 0},
    {OperationKind::ChatHistoryRead, QLatin1String("chat-history-read:"), 1},
    {OperationKind::ChatHistorySnapshot, QLatin1String("chat-history-snapshot:"), 1},
    {OperationKind::ChatImageUpload, QLatin1String("chat-image-upload:"), 2},
    {OperationKind::ChatMember, QLatin1String("chat-member:"), 1},
    {OperationKind::ChatPin, QLatin1String("chat-pin:"), 1},
    {OperationKind::ChatSoundSettings, QLatin1String("chat-sound-settings"), 0},
    {OperationKind::ChatTyping, QLatin1String("chat-typing:"), 1},
    {OperationKind::Chats, QLatin1String("chats"), 0},
    {OperationKind::Checklist, QLatin1String("checklist:"), 1},
    {OperationKind::ChecklistWrite, QLatin1String("checklist-write:"), 1},
    {OperationKind::CreateChat, QLatin1String("create-chat"), 0},
    {OperationKind::CreateProject, QLatin1String("create-project"), 0},
    {OperationKind::CreateSubtask, QLatin1String("create-subtask:"), 1},
    {OperationKind::CreateTask, QLatin1String("create-task:"), 1},
    {OperationKind::CreateTaskFromMessage, QLatin1String("create-task-from-message:"), 2},
    {OperationKind::CsrfRefresh, QLatin1String("csrf-refresh"), 0},
    {OperationKind::CurrentPerson, QLatin1String("current-person:"), 1},
    {OperationKind::DeleteNotification, QLatin1String("delete-notification"), 0},
    {OperationKind::DeleteTask, QLatin1String("delete-task:"), 1},
    {OperationKind::ForwardMessage, QLatin1String("forward-message:"), 2},
    {OperationKind::GanttLinks, QLatin1String("gantt-links:"), 1},
    {OperationKind::InvalidMessageReaction, QLatin1String("message-reaction"), 0},
    {OperationKind::LinkPreview, QLatin1String("link-preview:"), 1},
    {OperationKind::LinkPreviewChatAssets, QLatin1String("link-preview:chat-assets:"), 1},
    {OperationKind::LinkPreviewStage, QLatin1String("link-preview:stage:"), 1},
    {OperationKind::MessageAction, QLatin1String("message-action:"), 2},
    {OperationKind::MessageContext, QLatin1String("message-context:"), 2},
    {OperationKind::MessageEdit, QLatin1String("message-edit:"), 2},
    {OperationKind::MessageFileDownload, QLatin1String("message-file-download"), 0},
    {OperationKind::MessageReaction, QLatin1String("message-reaction:"), 3},
    {OperationKind::MessageViewers, QLatin1String("message-viewers:"), 1},
    {OperationKind::NotificationBatch, QLatin1String("notification-batch:"), 2},
    {OperationKind::NotificationSettings, QLatin1String("notification-settings"), 0},
    {OperationKind::NotificationSettingsUpdate, QLatin1String("notification-settings-update"), 0},
    {OperationKind::Notifications, QLatin1String("notifications"), 0},
    {OperationKind::NotificationsOlder, QLatin1String("notifications-older"), 0},
    {OperationKind::OlderChat, QLatin1String("older-chat:"), 3},
    {OperationKind::Person, QLatin1String("person:"), 1},
    {OperationKind::Pins, QLatin1String("pins:"), 1},
    {OperationKind::PortalStatus, QLatin1String("portal-status"), 0},
    {OperationKind::PortalStatusBootstrap, QLatin1String("portal-status-bootstrap"), 0},
    {OperationKind::PortalStatusUpdate, QLatin1String("portal-status-update"), 0},
    {OperationKind::ProjectFiles, QLatin1String("project-files:"), 2},
    {OperationKind::ProjectMembers, QLatin1String("project-members:"), 1},
    {OperationKind::ProjectPageFallback, QLatin1String("project-page-fallback"), 0},
    {OperationKind::ProjectPin, QLatin1String("project-pin:"), 1},
    {OperationKind::ProjectPinIdentity, QLatin1String("project-pin-identity"), 0},
    {OperationKind::ProjectStages, QLatin1String("project-stages:"), 1},
    {OperationKind::ProjectTaskChats, QLatin1String("project-task-chats:"), 1},
    {OperationKind::ProjectUnreadDialogs, QLatin1String("project-unread:dialogs"), 0},
    {OperationKind::ProjectUnreadTasks, QLatin1String("project-unread:tasks"), 0},
    {OperationKind::Projects, QLatin1String("projects"), 0},
    {OperationKind::Push, QLatin1String("push"), 0},
    {OperationKind::ReadAllNotifications, QLatin1String("read-all-notifications"), 0},
    {OperationKind::ReadNotification, QLatin1String("read-notification"), 0},
    {OperationKind::ReadNotifications, QLatin1String("read-notifications"), 0},
    {OperationKind::ReadVisibleAnchors, QLatin1String("read-visible:anchors:"), 1},
    {OperationKind::ReadVisibleMessages, QLatin1String("read-visible:messages:"), 1},
    {OperationKind::RecentUserStatus, QLatin1String("recent-user-status"), 0},
    {OperationKind::SearchChat, QLatin1String("search-chat:"), 1},
    {OperationKind::SearchChats, QLatin1String("search-chats:"), 1},
    {OperationKind::SearchNotifications, QLatin1String("search-notifications:"), 1},
    {OperationKind::SearchPeople, QLatin1String("search-people:"), 1},
    {OperationKind::SearchProjectTasks, QLatin1String("search-project-tasks:"), 1},
    {OperationKind::SearchTaskChat, QLatin1String("search-task-chat:"), 1},
    {OperationKind::SearchTaskChats, QLatin1String("search-task-chats:"), 1},
    {OperationKind::SendChat, QLatin1String("send:chat:"), 1},
    {OperationKind::SendTask, QLatin1String("send:task:"), 1},
    {OperationKind::SessionKeepalive, QLatin1String("session-keepalive"), 0},
    {OperationKind::SessionRecovery, QLatin1String("session-recovery"), 0},
    {OperationKind::SettingsIdentity, QLatin1String("settings-identity"), 0},
    {OperationKind::Subtasks, QLatin1String("subtasks:"), 1},
    {OperationKind::Task, QLatin1String("task:"), 1},
    {OperationKind::TaskCardAction, QLatin1String("task-card-action:"), 2},
    {OperationKind::TaskCardRead, QLatin1String("task-card-read:"), 2},
    {OperationKind::TaskChatTitles, QLatin1String("task-chat-titles"), 0},
    {OperationKind::TaskChatTitlesRequest, QLatin1String("task-chat-titles:"), 2},
    {OperationKind::TaskChats, QLatin1String("task-chats"), 0},
    {OperationKind::TaskDetails, QLatin1String("task-details:"), 1},
    {OperationKind::TaskFiles, QLatin1String("task-files:"), 1},
    {OperationKind::TaskHistory, QLatin1String("task-history:"), 1},
    {OperationKind::TaskMessages, QLatin1String("task-messages:"), 1},
    {OperationKind::Timeline, QLatin1String("timeline:"), 1},
    {OperationKind::UpdateTask, QLatin1String("update-task:"), 1},
    {OperationKind::UploadChat, QLatin1String("upload-chat:"), 1},
    {OperationKind::UploadTaskChat, QLatin1String("upload-task-chat:"), 1},
    {OperationKind::Workday, QLatin1String("workday"), 0},
    {OperationKind::WorkdayChange, QLatin1String("workday-change"), 0},
    {OperationKind::WorktimeExport, QLatin1String("worktime-export:"), 1},
    {OperationKind::WorktimeSheet, QLatin1String("worktime-sheet:"), 1},
};

constexpr bool completeCatalog()
{
    if (std::size(operations) != static_cast<size_t>(OperationKind::Count))
        return false;
    for (size_t i = 0; i < std::size(operations); ++i) {
        if (static_cast<size_t>(operations[i].kind) != i)
            return false;
    }
    return true;
}
static_assert(completeCatalog());

// A final id consumes the rest of the key. Escape interior ids, and the generic
// link-preview id whose first segment could otherwise select a more specific kind.
bool needsEscaping(const OperationSpec &spec, int index)
{
    return index + 1 < spec.idCount || spec.kind == OperationKind::LinkPreview;
}

QString escapeId(QString id)
{
    return id.replace(QLatin1Char('%'), QLatin1String("%25"))
             .replace(QLatin1Char(':'), QLatin1String("%3A"));
}

QString unescapeId(QString id)
{
    return id.replace(QLatin1String("%3A"), QLatin1String(":"))
             .replace(QLatin1String("%25"), QLatin1String("%"));
}

} // namespace

QString operationKey(OperationKind kind, const QStringList &ids)
{
    if (kind <= OperationKind::Unknown || kind >= OperationKind::Count)
        return {};
    const auto &spec = operations[static_cast<size_t>(kind)];
    if (ids.size() != spec.idCount)
        return {};

    QString key(spec.prefix);
    for (int i = 0; i < spec.idCount; ++i) {
        if (i > 0)
            key += QLatin1Char(':');
        key += needsEscaping(spec, i) ? escapeId(ids.at(i)) : ids.at(i);
    }
    return key;
}

ParsedOperation parseOperation(const QString &key)
{
    const OperationSpec *match = nullptr;
    for (const auto &spec : operations) {
        if (spec.kind == OperationKind::Unknown)
            continue;
        if (spec.idCount == 0) {
            if (key == spec.prefix)
                return {spec.kind, {}};
        } else if ((!match || spec.prefix.size() > match->prefix.size()) && key.startsWith(spec.prefix)) {
            match = &spec;
        }
    }
    if (!match)
        return {};

    QStringList ids;
    qsizetype start = match->prefix.size();
    for (int i = 0; i < match->idCount; ++i) {
        const auto end = i + 1 == match->idCount ? key.size() : key.indexOf(QLatin1Char(':'), start);
        if (end < 0)
            return {};
        const auto id = key.mid(start, end - start);
        ids.append(needsEscaping(*match, i) ? unescapeId(id) : id);
        start = end + 1;
    }
    return {match->kind, ids};
}

QString operationErrorHint(OperationKind kind)
{
    switch (kind) {
    case OperationKind::SendChat:
    case OperationKind::SendTask:
    case OperationKind::UploadChat:
    case OperationKind::UploadTaskChat:
        return QStringLiteral(". Проверьте историю перед повторной отправкой");
    case OperationKind::AttachTask:
        return QStringLiteral(". Проверьте файлы задачи перед повторным прикреплением");
    case OperationKind::UpdateTask:
    case OperationKind::BoardEdit:
        return QStringLiteral(". Проверьте сохранённые изменения перед повтором");
    default:
        return {};
    }
}

} // namespace bx
