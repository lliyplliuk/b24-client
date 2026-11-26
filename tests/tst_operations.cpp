#include "core/operations.h"

#include <QtTest>
#include <iterator>

using namespace bx;

class OperationsTest : public QObject {
    Q_OBJECT
private slots:
    void roundTrip_data()
    {
        QTest::addColumn<OperationKind>("kind");
        QTest::addColumn<QString>("prefix");
        QTest::addColumn<int>("arity");
        using K = OperationKind;
        struct Row { K kind; const char *prefix; int arity; };
        constexpr Row rows[] = {
            {K::AttachTask, "attach-task:", 1},
            {K::Board, "board:", 1},
            {K::BoardColumn, "board-column:", 2},
            {K::BoardEdit, "board-edit:", 1},
            {K::Chat, "chat:", 1},
            {K::ChatAssets, "chat-assets:", 3},
            {K::ChatBadgesSnapshot, "chat-badges-snapshot", 0},
            {K::ChatHistoryIdentity, "chat-history-identity", 0},
            {K::ChatHistoryRead, "chat-history-read:", 1},
            {K::ChatHistorySnapshot, "chat-history-snapshot:", 1},
            {K::ChatImageUpload, "chat-image-upload:", 2},
            {K::ChatMember, "chat-member:", 1},
            {K::ChatPin, "chat-pin:", 1},
            {K::ChatSoundSettings, "chat-sound-settings", 0},
            {K::ChatTyping, "chat-typing:", 1},
            {K::Chats, "chats", 0},
            {K::Checklist, "checklist:", 1},
            {K::ChecklistWrite, "checklist-write:", 1},
            {K::CreateChat, "create-chat", 0},
            {K::CreateProject, "create-project", 0},
            {K::CreateSubtask, "create-subtask:", 1},
            {K::CreateTask, "create-task:", 1},
            {K::CreateTaskFromMessage, "create-task-from-message:", 2},
            {K::CsrfRefresh, "csrf-refresh", 0},
            {K::CurrentPerson, "current-person:", 1},
            {K::DeleteNotification, "delete-notification", 0},
            {K::DeleteTask, "delete-task:", 1},
            {K::ForwardMessage, "forward-message:", 2},
            {K::GanttLinks, "gantt-links:", 1},
            {K::InvalidMessageReaction, "message-reaction", 0},
            {K::LinkPreview, "link-preview:", 1},
            {K::LinkPreviewChatAssets, "link-preview:chat-assets:", 1},
            {K::LinkPreviewStage, "link-preview:stage:", 1},
            {K::MessageAction, "message-action:", 2},
            {K::MessageContext, "message-context:", 2},
            {K::MessageEdit, "message-edit:", 2},
            {K::MessageFileDownload, "message-file-download", 0},
            {K::MessageReaction, "message-reaction:", 3},
            {K::MessageViewers, "message-viewers:", 1},
            {K::NotificationBatch, "notification-batch:", 2},
            {K::NotificationSettings, "notification-settings", 0},
            {K::NotificationSettingsUpdate, "notification-settings-update", 0},
            {K::Notifications, "notifications", 0},
            {K::NotificationsOlder, "notifications-older", 0},
            {K::OlderChat, "older-chat:", 3},
            {K::Person, "person:", 1},
            {K::Pins, "pins:", 1},
            {K::PortalStatus, "portal-status", 0},
            {K::PortalStatusBootstrap, "portal-status-bootstrap", 0},
            {K::PortalStatusUpdate, "portal-status-update", 0},
            {K::ProjectFiles, "project-files:", 2},
            {K::ProjectMembers, "project-members:", 1},
            {K::ProjectPageFallback, "project-page-fallback", 0},
            {K::ProjectPin, "project-pin:", 1},
            {K::ProjectPinIdentity, "project-pin-identity", 0},
            {K::ProjectStages, "project-stages:", 1},
            {K::ProjectTaskChats, "project-task-chats:", 1},
            {K::ProjectUnreadDialogs, "project-unread:dialogs", 0},
            {K::ProjectUnreadTasks, "project-unread:tasks", 0},
            {K::Projects, "projects", 0},
            {K::Push, "push", 0},
            {K::ReadAllNotifications, "read-all-notifications", 0},
            {K::ReadNotification, "read-notification", 0},
            {K::ReadNotifications, "read-notifications", 0},
            {K::ReadVisibleAnchors, "read-visible:anchors:", 1},
            {K::ReadVisibleMessages, "read-visible:messages:", 1},
            {K::RecentUserStatus, "recent-user-status", 0},
            {K::SearchChat, "search-chat:", 1},
            {K::SearchChats, "search-chats:", 1},
            {K::SearchNotifications, "search-notifications:", 1},
            {K::SearchPeople, "search-people:", 1},
            {K::SearchProjectTasks, "search-project-tasks:", 1},
            {K::SearchTaskChat, "search-task-chat:", 1},
            {K::SearchTaskChats, "search-task-chats:", 1},
            {K::SendChat, "send:chat:", 1},
            {K::SendTask, "send:task:", 1},
            {K::SessionKeepalive, "session-keepalive", 0},
            {K::SessionRecovery, "session-recovery", 0},
            {K::SettingsIdentity, "settings-identity", 0},
            {K::Subtasks, "subtasks:", 1},
            {K::Task, "task:", 1},
            {K::TaskCardAction, "task-card-action:", 2},
            {K::TaskCardRead, "task-card-read:", 2},
            {K::TaskChatTitles, "task-chat-titles", 0},
            {K::TaskChatTitlesRequest, "task-chat-titles:", 2},
            {K::TaskChats, "task-chats", 0},
            {K::TaskDetails, "task-details:", 1},
            {K::TaskFiles, "task-files:", 1},
            {K::TaskHistory, "task-history:", 1},
            {K::TaskMessages, "task-messages:", 1},
            {K::Timeline, "timeline:", 1},
            {K::UpdateTask, "update-task:", 1},
            {K::UploadChat, "upload-chat:", 1},
            {K::UploadTaskChat, "upload-task-chat:", 1},
            {K::Workday, "workday", 0},
            {K::WorkdayChange, "workday-change", 0},
            {K::WorktimeExport, "worktime-export:", 1},
            {K::WorktimeSheet, "worktime-sheet:", 1},
        };
        static_assert(std::size(rows) == int(K::Count) - 1);
        for (const auto &row : rows)
            QTest::newRow(row.prefix) << row.kind << QString::fromLatin1(row.prefix) << row.arity;
    }

    void roundTrip()
    {
        QFETCH(OperationKind, kind);
        QFETCH(QString, prefix);
        QFETCH(int, arity);
        QStringList ids;
        for (int i = 0; i < arity; ++i)
            ids.append(QString::number(i + 1));
        QCOMPARE(operationKey(kind, ids), prefix + ids.join(QLatin1Char(':')));
        const auto check = [&](const QStringList &args) {
            const auto parsed = parseOperation(operationKey(kind, args));
            QCOMPARE(parsed.kind, kind);
            QCOMPARE(parsed.ids, args);
        };
        check(ids);
        for (QString &id : ids)
            id = QStringLiteral("stage:ю:%3A:") + id;
        check(ids);
        ids.fill(QString{});
        check(ids);
    }

    void failureHints()
    {
        using K = OperationKind;
        for (const auto kind : {K::SendChat, K::SendTask, K::UploadChat, K::UploadTaskChat})
            QCOMPARE(operationErrorHint(kind), QStringLiteral(". Проверьте историю перед повторной отправкой"));
        QCOMPARE(operationErrorHint(K::AttachTask),
                 QStringLiteral(". Проверьте файлы задачи перед повторным прикреплением"));
        for (const auto kind : {K::UpdateTask, K::BoardEdit})
            QCOMPARE(operationErrorHint(kind), QStringLiteral(". Проверьте сохранённые изменения перед повтором"));
        QVERIFY(operationErrorHint(K::Task).isEmpty());
    }

    void unrecognizedKeys()
    {
        QCOMPARE(parseOperation(QStringLiteral("missing:key")).kind, OperationKind::Unknown);
        QCOMPARE(parseOperation(QStringLiteral("board-column:only-one")).kind, OperationKind::Unknown);
        QCOMPARE(parseOperation(QStringLiteral("workday:extra")).kind, OperationKind::Unknown);
        QVERIFY(operationKey(OperationKind::BoardColumn, QStringLiteral("only-one")).isEmpty());
    }
};

QTEST_APPLESS_MAIN(OperationsTest)
#include "tst_operations.moc"
