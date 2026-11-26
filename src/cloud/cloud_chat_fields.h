#pragma once

#include <QString>

namespace bx::chat_fields {

qint64 positiveId(const QString &candidate);
qint64 groupChatId(const QString &dialog);

} // namespace bx::chat_fields
