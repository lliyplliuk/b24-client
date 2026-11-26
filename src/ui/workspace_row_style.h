#pragma once

#include <QColor>
#include <QFont>
#include <QMetaType>
#include <QString>

namespace bx::workspace {

inline const QColor Ink("#182d3b");
inline const QColor Muted("#516674");
inline const QColor Accent("#215c70");
inline const QColor ConversationMetaInk("#3e5663");
inline const QColor Surface("#fffefa");
inline const QColor Selected("#dcecf0");
inline const QColor Border("#dce4e4");

constexpr int Id = Qt::UserRole;
constexpr int Title = Qt::UserRole + 1;
constexpr int Detail = Qt::UserRole + 2;
constexpr int Meta = Qt::UserRole + 3;
constexpr int Count = Qt::UserRole + 4;
constexpr int Outgoing = Qt::UserRole + 5;
constexpr int MarkupHtml = Qt::UserRole + 6;
constexpr int MarkupBlock = Qt::UserRole + 7;
constexpr int DayDivider = Qt::UserRole + 8;
constexpr int UnreadDivider = Qt::UserRole + 9;
constexpr int AvatarUrl = Qt::UserRole + 10;
constexpr int AvatarImage = Qt::UserRole + 11;
constexpr int AvatarType = Qt::UserRole + 12;
constexpr int Pinned = Qt::UserRole + 13;
constexpr int TaskId = Qt::UserRole + 14;
constexpr int MarkedUnread = Qt::UserRole + 15;
constexpr int ForwardByline = Qt::UserRole + 16;
constexpr int ReplyByline = Qt::UserRole + 17;
constexpr int ReplyPreview = Qt::UserRole + 18;
constexpr int NoticePresentation = Qt::UserRole + 19;
constexpr int MessageFiles = Qt::UserRole + 20;
constexpr int ReplyFiles = Qt::UserRole + 21;
constexpr int SystemMessage = Qt::UserRole + 22;
constexpr int HasMention = Qt::UserRole + 23;
constexpr int HasReaction = Qt::UserRole + 24;
constexpr int ProjectAvatarColor = Qt::UserRole + 25;
constexpr int ProjectAvatarVariant = Qt::UserRole + 26;
constexpr int ProjectAvatarType = Qt::UserRole + 27;
constexpr int ReceiptMessageId = Qt::UserRole + 28;
constexpr int ContactOnlineUntil = Qt::UserRole + 29;
constexpr int ContactPosition = Qt::UserRole + 30;
constexpr int ReplyTargetId = Qt::UserRole + 31;
constexpr int MessageAuthor = Qt::UserRole + 32;
constexpr int RawMessageBody = Qt::UserRole + 37;
constexpr int MessageReactionCounts = Qt::UserRole + 38;
constexpr int OwnMessageReactions = Qt::UserRole + 39;
constexpr int MessageSeen = Qt::UserRole + 40;
constexpr int MessageUnread = Qt::UserRole + 41;
constexpr int MessageCalendarDay = Qt::UserRole + 42;
constexpr int MessageLinks = Qt::UserRole + 43;
constexpr int CountPartial = Qt::UserRole + 44;
constexpr int ChatMessagePreview = Qt::UserRole + 45;
constexpr int NoticeTargets = Qt::UserRole + 46;

struct NoticeRow {
    QString day;
    QString excerpt;
    QString primaryLabel;
    int primaryTarget = -1;
    bool expanded = false;
};

inline const QString UnreadMessages = QStringLiteral("Непрочитанные сообщения");
enum class Row { Directory, Message, Notice, Card, TaskContent };

QString reactionGlyph(const QString &code);

QFont reactionFont(int pixels);

QFont rowFont(int pixels, bool bold = false);

} // namespace bx::workspace

Q_DECLARE_METATYPE(bx::workspace::NoticeRow)
