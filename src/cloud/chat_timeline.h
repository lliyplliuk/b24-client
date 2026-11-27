#pragma once

#include "core/models.h"

#include <QCache>
#include <QJsonValue>
#include <QQueue>
#include <QSet>
#include <functional>

namespace bx {

// CloudBackend owns this account's timeline and resets it when the account changes.
// Chat updates address one dialog at a time. Looking up a dialog must not create an
// entry; insertion protects the two open histories when evicting message state.
// History, recent rows and receipts compare revisions from the same badge clock.
class ChatTimeline {
public:
    ChatTimeline() = default;
    void resetAccount();

    struct RecentOverlay {
        Chat preview;
        struct HistoryPreview {
            qint64 chatId = 0;
            qint64 messageId = 0;
            QString text;
            QDateTime updated;
        } historyPreview;
        qint64 rejectedRecentPreviewChatId = 0;
        qint64 rejectedTaskPreviewChatId = 0;
        qint64 chatId = 0;
        qint64 messageId = 0;
        qint64 incomingId = 0;
        qint64 eventTime = 0;
        qint64 confirmedCounterCursor = 0;
        quint64 counterRevision = 0;
        std::optional<int> counter;
        std::optional<bool> markedUnread;
    };
    RecentOverlay &recentOverlay(const QString &dialog);
    struct DialogMessageState {
        qint64 lastReadId = -1;
        qint64 confirmedReadThrough = 0;
        quint64 historyGeneration = 0;
        quint64 readReceiptRevision = 0;
        std::optional<int> unreadCounter;
        std::optional<bool> muted;
        bool muteFromPull = false;
        QJsonValue muteList;
        QSet<qint64> exactlyRead;
        QSet<qint64> historyUnread;
        QSet<qint64> seenByOthers;
        QStringList pendingReadIds;
        QString pendingReadTask;
    };
    DialogMessageState &dialogMessageState(const QString &dialog);
    const DialogMessageState *findMessageState(const QString &dialog) const;
    quint64 historyGeneration(const QString &dialog) const;
    qint64 lastReadId(const QString &dialog) const;
    const QHash<QString, RecentOverlay> &overlays() const { return recentOverlays; }

    const QString &activeDialog() const { return openDialog; }
    const QString &activeTask() const { return openTask; }
    QString activeTaskDialog() const { return taskDialogs.value(openTask); }
    void openHistory(const QString &scope, bool task);
    void invalidateHistory(bool task);
    void invalidateRecent();
    void invalidateOpenViews(bool chatSection);
    bool isRecentDirty() const { return recentDirty; }
    bool isHistoryDirty(bool task) const { return task ? taskMessagesDirty : chatDirty; }
    bool takeRecentDirty();
    bool takeHistoryDirty(bool task);
    void resetPullMuteFlags();
    void rememberTaskDialog(const QString &task, const QString &dialog);
    void forgetTaskDialog(const QString &task);
    void rememberChatId(const QString &dialog, qint64 id);
    void updateRecentChat(const QString &dialog, const std::function<void(Chat &)> &update);
    QString taskDialog(const QString &task) const { return taskDialogs.value(task); }
    bool hasTaskForDialog(const QString &dialog) const;
    QStringList tasksForDialog(const QString &dialog) const { return taskDialogs.keys(dialog); }
    qint64 knownChatId(const QString &dialog) const { return dialogChatIds.value(dialog); }
    QString dialogForChat(qint64 id, const QString &fallback) const { return dialogChatIds.key(id, fallback); }

    const QVector<Chat> &recentChats(bool task) const { return task ? recentTaskChatItems : recentChatItems; }
    void replaceRecentChats(QVector<Chat> chats, bool task);
    const QVector<Chat> &legacyTasks() const { return legacyTaskChats; }
    void replaceLegacyTasks(QVector<Chat> chats);
    bool showingTaskChats() const { return showTaskChats; }
    void selectRecentSection(bool task);

    quint64 badgeRevision() const { return chatBadgeRevision; }
    quint64 nextBadgeRevision();
    quint64 snapshotRevision() const { return chatCounterSnapshotRevision; }
    bool wasSentTemplate(const QString &id) const { return sentTemplates.contains(id); }
    void rememberSentTemplate(const QString &id);
    bool hasReplyPreview(const QString &dialog, const QString &message) const;
    void forgetReplyPreview(const QString &dialog, const QString &message) const;

    struct ReplyPreview { QString author; QString text; QVector<MessageFile> files; };
    const ReplyPreview *replyPreview(const QString &dialog, const QString &message) const;
    void rememberReplyPreview(const QString &dialog, const QString &message, ReplyPreview preview) const;
    void applyCounterSnapshotRevision(quint64 revision);

private:
    Q_DISABLE_COPY_MOVE(ChatTimeline)

    mutable QCache<QString, ReplyPreview> replyPreviews{128};
    static QString replyPreviewKey(const QString &dialog, const QString &message);
    QHash<QString, RecentOverlay> recentOverlays;
    bool recentDirty = false;
    QString openDialog;
    QString openTask;
    bool chatDirty = false;
    bool taskMessagesDirty = false;
    QHash<QString, QString> taskDialogs;
    QHash<QString, qint64> dialogChatIds;
    QVector<Chat> recentChatItems;
    QVector<Chat> recentTaskChatItems;
    QHash<QString, DialogMessageState> dialogMessageStates;
    QQueue<QString> sentTemplates;
    bool showTaskChats = false;
    QVector<Chat> legacyTaskChats;
    quint64 chatBadgeRevision = 0;
    quint64 chatCounterSnapshotRevision = 0;
};

} // namespace bx
