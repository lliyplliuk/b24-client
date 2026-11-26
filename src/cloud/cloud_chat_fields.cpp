#include "cloud/cloud_chat_fields.h"

namespace bx::chat_fields {

qint64 positiveId(const QString &candidate)
{
    const auto id = candidate.toLongLong();
    return id > 0 ? id : 0;
}

qint64 groupChatId(const QString &dialog)
{
    return dialog.startsWith(QLatin1String("chat")) ? positiveId(dialog.mid(4)) : 0;
}

} // namespace bx::chat_fields
