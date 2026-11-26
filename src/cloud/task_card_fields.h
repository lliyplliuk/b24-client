#pragma once

#include "core/models.h"

#include <QJsonObject>

namespace bx::task_card_fields {

std::optional<qint64> taskDeadline(const QString &value);
QString encodedTaskDescription(QString text);
QString taskStatusCommand(TaskStatusAction action);
std::optional<QStringList> taskObserverIds(const QJsonValue &value);
bool validTaskObserverIds(const QStringList &ids);
void applyTaskCardDetails(Task &task, const QJsonObject &card);

} // namespace bx::task_card_fields
