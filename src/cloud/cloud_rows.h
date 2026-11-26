#pragma once

#include "core/models.h"

#include <QJsonObject>

namespace bx::cloud_rows {

inline constexpr qsizetype maxAttachmentNames = 100;
QString plainFileName(QString path);
QHash<QString, QJsonObject> responseFiles(const QJsonValue &value);
QString taskFileId(const QString &raw);
QString string(const QJsonValue &value);
QString field(const QJsonObject &object, std::initializer_list<const char *> keys);
QString numericId(const QString &candidate);
QString chatDialog(const QString &chatId);
qint64 explicitMessageChatId(const QJsonObject &message);
std::optional<bool> notificationFlag(const QJsonValue &value);
bool boardStageId(const QString &id);
QJsonValue oneOf(const QJsonObject &object, std::initializer_list<const char *> keys);
QJsonValue list(const QJsonValue &data, std::initializer_list<const char *> keys);
QDateTime date(const QJsonValue &value);
QString stageLabel(const QString &id);
QStringList taskTags(const QJsonValue &value);
void appendTaskObservers(Task &task, const QJsonValue &rows);
QHash<QString, QJsonObject> taskObserversById(const QJsonValue &rows);
bool matchesTaskIdentity(const QJsonObject &row, const QString &taskId);
std::optional<TaskRating> taskRating(const QJsonObject &row);
Task taskFromRow(const QJsonObject &row);

} // namespace bx::cloud_rows
