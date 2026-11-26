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
        QTest::addColumn<RequestPolicy>("policy");
        using K = OperationKind;
        using P = RequestPolicy;
        struct Row { K kind; const char *prefix; int arity; P policy; };
        constexpr Row rows[] = {
            {K::AttachTask, "attach-task:", 1, P::AccountScoped},
            {K::Board, "board:", 1, P::Required},
            {K::BoardColumn, "board-column:", 2, P::Required},
            {K::BoardEdit, "board-edit:", 1, P::NotifyAccountChange},
            {K::Chat, "chat:", 1, P::Required},
            {K::ChatAssets, "chat-assets:", 3, P::Required},
            {K::ChatBadgesSnapshot, "chat-badges-snapshot", 0, P::Optional},
            {K::ChatHistoryIdentity, "chat-history-identity", 0, P::Required},
            {K::ChatHistoryRead, "chat-history-read:", 1, P::HistoryRead},
            {K::ChatHistorySnapshot, "chat-history-snapshot:", 1, P::HistorySnapshot},
            {K::ChatImageUpload, "chat-image-upload:", 2, P::Required},
            {K::ChatMember, "chat-member:", 1, P::Required},
            {K::ChatPin, "chat-pin:", 1, P::NotifyAccountChange},
            {K::ChatSoundSettings, "chat-sound-settings", 0, P::Required},
            {K::ChatTyping, "chat-typing:", 1, P::Optional},
            {K::Chats, "chats", 0, P::Required},
            {K::Checklist, "checklist:", 1, P::Required},
            {K::ChecklistWrite, "checklist-write:", 1, P::Required},
            {K::CreateChat, "create-chat", 0, P::Required},
            {K::CreateProject, "create-project", 0, P::NotifyAccountChange},
            {K::CreateSubtask, "create-subtask:", 1, P::AccountScoped},
            {K::CreateTask, "create-task:", 1, P::AccountScoped},
            {K::CreateTaskFromMessage, "create-task-from-message:", 2, P::Required},
            {K::CsrfRefresh, "csrf-refresh", 0, P::Required},
            {K::CurrentPerson, "current-person:", 1, P::Optional},
            {K::DeleteNotification, "delete-notification", 0, P::Required},
            {K::DeleteTask, "delete-task:", 1, P::Required},
            {K::ForwardMessage, "forward-message:", 2, P::Required},
            {K::GanttLinks, "gantt-links:", 1, P::Required},
            {K::InvalidMessageReaction, "message-reaction", 0, P::Required},
            {K::LinkPreview, "link-preview:", 1, P::Optional},
            {K::LinkPreviewChatAssets, "link-preview:chat-assets:", 1, P::Optional},
            {K::LinkPreviewStage, "link-preview:stage:", 1, P::Optional},
            {K::MessageAction, "message-action:", 2, P::Required},
            {K::MessageContext, "message-context:", 2, P::Required},
            {K::MessageEdit, "message-edit:", 2, P::Required},
            {K::MessageFileDownload, "message-file-download", 0, P::Required},
            {K::MessageReaction, "message-reaction:", 3, P::Required},
            {K::MessageViewers, "message-viewers:", 1, P::Optional},
            {K::NotificationBatch, "notification-batch:", 2, P::Required},
            {K::NotificationSettings, "notification-settings", 0, P::Required},
            {K::NotificationSettingsUpdate, "notification-settings-update", 0, P::Required},
            {K::Notifications, "notifications", 0, P::Required},
            {K::NotificationsOlder, "notifications-older", 0, P::Required},
            {K::OlderChat, "older-chat:", 3, P::Required},
            {K::Person, "person:", 1, P::AccountScoped},
            {K::Pins, "pins:", 1, P::Required},
            {K::PortalStatus, "portal-status", 0, P::Required},
            {K::PortalStatusBootstrap, "portal-status-bootstrap", 0, P::Optional},
            {K::PortalStatusUpdate, "portal-status-update", 0, P::Required},
            {K::ProjectFiles, "project-files:", 2, P::Optional},
            {K::ProjectMembers, "project-members:", 1, P::Optional},
            {K::ProjectPageFallback, "project-page-fallback", 0, P::Optional},
            {K::ProjectPin, "project-pin:", 1, P::NotifyAccountChange},
            {K::ProjectPinIdentity, "project-pin-identity", 0, P::Optional},
            {K::ProjectStages, "project-stages:", 1, P::Required},
            {K::ProjectTaskChats, "project-task-chats:", 1, P::Required},
            {K::ProjectUnreadDialogs, "project-unread:dialogs", 0, P::Optional},
            {K::ProjectUnreadTasks, "project-unread:tasks", 0, P::Optional},
            {K::Projects, "projects", 0, P::Required},
            {K::Push, "push", 0, P::Required},
            {K::ReadAllNotifications, "read-all-notifications", 0, P::Required},
            {K::ReadNotification, "read-notification", 0, P::Required},
            {K::ReadNotifications, "read-notifications", 0, P::Required},
            {K::ReadVisibleAnchors, "read-visible:anchors:", 1, P::Optional},
            {K::ReadVisibleMessages, "read-visible:messages:", 1, P::Optional},
            {K::RecentUserStatus, "recent-user-status", 0, P::Optional},
            {K::SearchChat, "search-chat:", 1, P::Required},
            {K::SearchChats, "search-chats:", 1, P::Required},
            {K::SearchNotifications, "search-notifications:", 1, P::Required},
            {K::SearchPeople, "search-people:", 1, P::Optional},
            {K::SearchProjectTasks, "search-project-tasks:", 1, P::NotifyAccountChange},
            {K::SearchTaskChat, "search-task-chat:", 1, P::Required},
            {K::SearchTaskChats, "search-task-chats:", 1, P::Required},
            {K::SendChat, "send:chat:", 1, P::NotifyAccountChange},
            {K::SendTask, "send:task:", 1, P::NotifyAccountChange},
            {K::SessionKeepalive, "session-keepalive", 0, P::Optional},
            {K::SessionRecovery, "session-recovery", 0, P::Required},
            {K::SettingsIdentity, "settings-identity", 0, P::Optional},
            {K::Subtasks, "subtasks:", 1, P::Required},
            {K::Task, "task:", 1, P::Required},
            {K::TaskCardAction, "task-card-action:", 2, P::Required},
            {K::TaskCardRead, "task-card-read:", 2, P::Required},
            {K::TaskChatTitles, "task-chat-titles", 0, P::Required},
            {K::TaskChatTitlesRequest, "task-chat-titles:", 2, P::Required},
            {K::TaskChats, "task-chats", 0, P::Required},
            {K::TaskDetails, "task-details:", 1, P::Optional},
            {K::TaskFiles, "task-files:", 1, P::Required},
            {K::TaskHistory, "task-history:", 1, P::Optional},
            {K::TaskMessages, "task-messages:", 1, P::Required},
            {K::Timeline, "timeline:", 1, P::Required},
            {K::UpdateTask, "update-task:", 1, P::Required},
            {K::UploadChat, "upload-chat:", 1, P::NotifyAccountChange},
            {K::UploadTaskChat, "upload-task-chat:", 1, P::NotifyAccountChange},
            {K::Workday, "workday", 0, P::Required},
            {K::WorkdayChange, "workday-change", 0, P::Required},
            {K::WorktimeExport, "worktime-export:", 1, P::Required},
            {K::WorktimeSheet, "worktime-sheet:", 1, P::Required},
        };
        static_assert(std::size(rows) == int(K::Count) - 1);
        for (const auto &row : rows)
            QTest::newRow(row.prefix) << row.kind << QString::fromLatin1(row.prefix) << row.arity << row.policy;
    }

    void roundTrip()
    {
        QFETCH(OperationKind, kind);
        QFETCH(QString, prefix);
        QFETCH(int, arity);
        QFETCH(RequestPolicy, policy);
        QCOMPARE(defaultRequestPolicy(kind), policy);
        QCOMPARE(operationName(kind), prefix.section(QLatin1Char(':'), 0, 0));
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
        QCOMPARE(defaultRequestPolicy(OperationKind::Unknown), RequestPolicy::Required);
        QVERIFY(operationName(OperationKind::Unknown).isEmpty());
        QCOMPARE(parseOperation(QStringLiteral("board-column:only-one")).kind, OperationKind::Unknown);
        QCOMPARE(parseOperation(QStringLiteral("workday:extra")).kind, OperationKind::Unknown);
    }

    void wrongArityProducesDistinctOpaqueKeys()
    {
#if defined(QT_NO_DEBUG) && !defined(QT_FORCE_ASSERTS)
        const auto wrongKey = [] {
            QTest::ignoreMessage(QtWarningMsg, "operationKey: board-column: expected 2 ids, got 1");
            return operationKey(OperationKind::BoardColumn, QStringLiteral("only-one"));
        };
        const auto first = wrongKey();
        const auto second = wrongKey();
        QVERIFY(!first.isEmpty());
        QVERIFY(!second.isEmpty());
        QVERIFY(first != second);
        QVERIFY(!first.contains(QStringLiteral("only-one")));
        QCOMPARE(parseOperation(first).kind, OperationKind::Unknown);
#else
        QSKIP("A wrong argument count asserts in debug builds");
#endif
    }
};

QTEST_APPLESS_MAIN(OperationsTest)
#include "tst_operations.moc"
