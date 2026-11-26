#pragma once

#include "core/models.h"

#include <QCache>
#include <QJsonValue>
#include <QQueue>
#include <QSet>

namespace bx {

// Shared by recent rows, history and the badge/read-receipt reconciliation code.
struct ChatTimeline {
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
    QHash<QString, RecentOverlay> recentOverlays;
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
    bool recentDirty = false;
    struct ReplyPreview { QString author; QString text; QVector<MessageFile> files; };
    mutable QCache<QString, ReplyPreview> replyPreviews{128};
    static QString replyPreviewKey(const QString &dialog, const QString &message);
    void rememberSentTemplate(const QString &id);
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
