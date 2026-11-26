#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <optional>

namespace bx {

inline constexpr int freshPopupSeconds = 10;

QString validDialog(const QString &candidate);
bool matchesRequestedHistory(const QJsonObject &history, const QString &dialog, qint64 expectedChatId);
qint64 recentMessageId(const QJsonObject &message);
std::optional<qint64> explicitMessageAuthorId(const QJsonObject &message);
qint64 groupDialogChatId(const QString &dialog);
bool isTaskChatType(const QString &type);
bool isTaskChatMetadata(const QString &kind, const QJsonObject &entity);
bool isRecentTaskChat(bool metadataTaskChat, bool listedTask, const QJsonArray &sections);
bool mutedListContains(const QJsonValue &list, const QString &user);
std::optional<bool> dismissedPersonalUser(const QString &dialog, const QJsonObject &row, const QJsonObject &user);
bool isPersonalMetadata(const QString &dialog, const QString &type);
QString userName(const QJsonObject &user);

std::optional<int> chatUnreadCount(const QJsonValue &value);

} // namespace bx
