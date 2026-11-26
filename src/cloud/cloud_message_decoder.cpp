#include "cloud/cloud_backend.h"

#include "cloud/cloud_chat_fields.h"
#include "cloud/cloud_chat_metadata.h"
#include "cloud/cloud_rows.h"
#include "cloud/reaction_code.h"
#include "core/notification_targets.h"
#include "core/origin_guard.h"

#include <QJsonArray>
#include <algorithm>

namespace bx {
using chat_fields::positiveId;
using chat_fields::groupChatId;
using cloud_rows::field;
using cloud_rows::date;
using cloud_rows::maxAttachmentNames;
using cloud_rows::responseFiles;

namespace {

constexpr qsizetype maxMessageReactionKinds = 64;

QJsonObject messageParams(const QJsonObject &message)
{
    const auto params = message.value(QStringLiteral("params")).toObject();
    return params.isEmpty() ? message.value(QStringLiteral("PARAMS")).toObject() : params;
}

QString replyMessageId(const QJsonObject &message)
{
    const auto direct = positiveId(field(message, {"replyId"}));
    const auto id = direct > 0 ? direct : positiveId(field(messageParams(message), {"replyId", "REPLY_ID"}));
    return id > 0 ? QString::number(id) : QString();
}

bool deletedMessage(const QJsonObject &message)
{
    const auto params = messageParams(message);
    return message.value(QStringLiteral("isDeleted")).toBool()
        || params.value(QStringLiteral("IS_DELETED")) == QLatin1String("Y");
}

QJsonArray messageFileIds(const QJsonObject &message)
{
    auto ids = message.value(QStringLiteral("files")).toArray();
    if (ids.isEmpty())
        ids = message.value(QStringLiteral("fileIds")).toArray();
    if (ids.isEmpty()) {
        const auto params = messageParams(message);
        auto value = params.value(QStringLiteral("FILE_ID"));
        if (value.isUndefined())
            value = params.value(QStringLiteral("fileId"));
        ids = value.isArray() ? value.toArray() : QJsonArray{value};
    }
    return ids;
}

} // namespace

QVector<Message> CloudBackend::decodeMessages(const QJsonObject &data, bool sortChronologically, const QString &dialog) const
{
    QHash<QString, QString> authors;
    QHash<QString, QString> authorPictures;
    for (const auto *key : {"usersShort", "users"}) {
        for (const auto row : data.value(QLatin1String(key)).toArray()) {
            const auto user = row.toObject();
            const auto userId = field(user, {"id", "ID"});
            if (const auto name = userName(user); !name.isEmpty())
                authors.insert(userId, name);
            if (const auto picture = field(user, {"avatar", "avatarUrl"}); !picture.isEmpty())
                authorPictures.insert(userId, picture);
        }
    }
    const auto authorOf = [](const QJsonObject &message) { return field(message, {"senderId", "authorId", "author_id", "AUTHOR_ID"}); };
    const auto authorLabel = [&authors](const QString &id) {
        const auto fallback = positiveId(id) == 0 ? QStringLiteral("Системное сообщение")
                                                 : QStringLiteral("Пользователь №%1").arg(id);
        return authors.value(id, fallback);
    };
    const auto rows = data.value(QStringLiteral("messages")).toArray();
    const auto explicitChat = positiveId(field(data, {"chatId", "CHAT_ID"}));
    const auto resolvedChat = badgeChatId(dialog);
    const auto expectedChat = resolvedChat > 0 ? resolvedChat : explicitChat;
    const auto sameChat = [expectedChat](const QJsonObject &message) {
        for (const auto *name : {"chatId", "chat_id", "CHAT_ID"}) {
            const auto value = message.value(QLatin1String(name));
            if (!value.isUndefined() && (expectedChat <= 0 || positiveId(cloud_rows::string(value)) != expectedChat))
                return false;
        }
        return true;
    };
    const auto viewerScope = settingsScope();
    const auto viewerDialog = viewerScope + QLatin1Char(':') + dialog;
    const auto chat = data.value(QStringLiteral("chat")).toObject();
    const auto role = field(chat, {"role"});
    const auto dialogId = chat.value(QStringLiteral("dialogId"));
    const auto componentId = chat.value(QStringLiteral("id"));
    const auto group = groupChatId(dialog);
    const bool matchingViewerHistory = sameChat(data) && sameChat(chat)
        && (componentId.isUndefined() || positiveId(cloud_rows::string(componentId)) == expectedChat)
        && (dialogId.isUndefined() || cloud_rows::string(dialogId) == dialog)
        && (group == 0 || group == expectedChat);
    if (group > 0 && !matchingViewerHistory) {
        m_messageViewerMembership.remove(viewerDialog);
    } else if (!viewerScope.isEmpty() && group > 0
               && positiveId(field(chat, {"id", "chatId"})) == group && !role.isEmpty()) {
        m_messageViewerMembership.insert(viewerDialog, new bool(role == QLatin1String("member")
            || role == QLatin1String("manager") || role == QLatin1String("owner")));
    }
    const auto *groupMember = m_messageViewerMembership.object(viewerDialog);
    const bool canRequestViewers = matchingViewerHistory && !viewerScope.isEmpty() && expectedChat > 0
        && (group > 0 ? (groupMember && *groupMember) : positiveId(dialog) > 0);
    const auto lastViews = chat.value(QStringLiteral("lastMessageViews")).toObject();
    const auto lastViewedMessage = positiveId(field(lastViews, {"messageId"}));
    const auto lastViewerCount = lastViews.value(QStringLiteral("countOfViewers")).toInteger(-1);
    QHash<QString, QJsonObject> replySources;
    QSet<QString> replies;
    for (const auto &row : rows) {
        const auto id = replyMessageId(row.toObject());
        if (!id.isEmpty())
            replies.insert(id);
        if (replies.size() >= tunables::kPageSizeMessages)
            break;
    }
    const auto rememberSource = [&replies, &replySources, &sameChat](const QJsonValue &row, const QString &key = {}) {
        const auto source = row.toObject();
        const auto id = field(source, {"id", "ID"});
        const auto sourceId = positiveId(id.isEmpty() ? key : id);
        const auto canonicalId = QString::number(sourceId);
        if (sourceId > 0 && (key.isEmpty() || positiveId(key) == sourceId)
            && replies.contains(canonicalId) && sameChat(source)) {
            replySources.insert(canonicalId, source);
        }
    };
    const auto additional = data.value(QStringLiteral("additionalMessages"));
    if (additional.isArray()) {
        for (const auto &row : additional.toArray())
            rememberSource(row);
    } else {
        const auto indexed = additional.toObject();
        for (auto it = indexed.constBegin(); it != indexed.constEnd(); ++it)
            rememberSource(it.value(), it.key());
    }
    for (const auto &row : rows)
        rememberSource(row);
    QHash<QString, QVector<MessageReaction>> reactions;
    for (const auto &value : data.value(QStringLiteral("reactions")).toArray()) {
        const auto row = value.toObject();
        const auto id = positiveId(field(row, {"messageId"}));
        if (id <= 0 || reactions.size() >= tunables::kPageSizeMessages)
            continue;
        QSet<QString> own;
        for (const auto &item : row.value(QStringLiteral("ownReactions")).toArray()) {
            const auto code = item.toString();
            if (isReactionCode(code))
                own.insert(code);
        }
        QVector<MessageReaction> decoded;
        const auto counters = row.value(QStringLiteral("reactionCounters")).toObject();
        for (auto it = counters.constBegin(); it != counters.constEnd() && decoded.size() < maxMessageReactionKinds; ++it) {
            const auto count = it.value().toInt();
            if (count > 0 && isReactionCode(it.key()))
                decoded.append({it.key(), count, own.remove(it.key())});
        }
        for (const auto &code : std::as_const(own)) {
            if (decoded.size() >= maxMessageReactionKinds)
                break;
            decoded.append({code, 0, true});
        }
        reactions.insert(QString::number(id), std::move(decoded));
    }
    const auto files = responseFiles(data.value(QStringLiteral("files")));
    const auto filesFor = [this, &files](const QJsonObject &message) {
        QVector<MessageFile> result;
        for (const auto &id : messageFileIds(message)) {
            const auto fileId = cloud_rows::string(id);
            const auto file = files.value(fileId);
            auto entry = decodeChatFile(fileId, file);
            if (entry.name.isEmpty())
                continue;
            result.append(std::move(entry));
            if (result.size() >= maxAttachmentNames)
                break;
        }
        return result;
    };
    const auto state = m_timeline.dialogMessageStates.constFind(dialog);
    QVector<Message> messages;
    for (const auto row : rows) {
        if (messages.size() >= tunables::kPageSizeMessages)
            break;
        const auto message = row.toObject();
        const auto id = field(message, {"id", "ID"});
        if (id.isEmpty())
            continue;
        const auto authorId = authorOf(message);
        const auto timestamp = message.value(QStringLiteral("date"));
        const bool system = authorId == QLatin1String("0");
        const bool outgoing = !system && !m_userId.isEmpty() && m_userId == authorId;
        const auto numericId = positiveId(id);
        const auto explicitUnread = message.value(QStringLiteral("unread"));
        const bool readByCursor = state != m_timeline.dialogMessageStates.cend() && numericId > 0
            && (numericId <= state->confirmedReadThrough || state->exactlyRead.contains(numericId)
                || (numericId <= state->lastReadId
                    && !(explicitUnread.toBool() && state->historyUnread.contains(numericId))));
        const bool unread = !dialog.isEmpty() && !outgoing && !readByCursor && (explicitUnread.isBool()
            ? explicitUnread.toBool()
            : state != m_timeline.dialogMessageStates.cend() && !m_userId.isEmpty() && positiveId(authorId) > 0
                && state->lastReadId >= 0 && numericId > state->lastReadId && state->unreadCounter != 0);
        Message entry;
        entry.id = id;
        entry.author = authorLabel(authorId);
        entry.text = field(message, {"text", "TEXT"});
        entry.createdAt = date(timestamp.isUndefined() ? message.value(QStringLiteral("DATE_CREATE")) : timestamp);
        entry.outgoing = outgoing;
        entry.seenByOthers = outgoing && (message.value(QStringLiteral("viewedByOthers")).toBool()
            || (state != m_timeline.dialogMessageStates.cend() && state->seenByOthers.contains(numericId)));
        if (canRequestViewers && outgoing && numericId > 0 && sameChat(message)) {
            const auto key = viewerDialog + QLatin1Char(':') + QString::number(numericId);
            if (deletedMessage(message)) {
                m_messageViewerCounts.remove(key);
            } else if (lastViewedMessage == numericId || !m_messageViewerCounts.contains(key)) {
                m_messageViewerCounts.insert(key,
                    new qint64(lastViewedMessage == numericId ? lastViewerCount : -1));
            }
        }
        entry.system = system;
        entry.reactions = reactions.value(QString::number(numericId));
        entry.files = filesFor(message);
        for (const auto &file : entry.files)
            entry.attachments.append(file.name);
        entry.unread = unread;
        entry.authorAvatarUrl = avatarUrlFromPortal(authorPictures.value(authorId));
        const auto forward = message.value(QStringLiteral("forward")).toObject();
        const auto originalAuthor = field(forward, {"userId"});
        const auto originalMessage = field(forward, {"id"});
        entry.forwarded = (!originalMessage.isEmpty() && originalMessage != QLatin1String("0")) || positiveId(originalAuthor) > 0;
        if (entry.forwarded)
            entry.forwardedAuthor = authors.value(originalAuthor);
        entry.replyId = replyMessageId(message);
        if (!entry.replyId.isEmpty()) {
            const auto reply = replySources.value(entry.replyId);
            const auto previewKey = ChatTimeline::replyPreviewKey(dialog, entry.replyId);
            if (!reply.isEmpty() && deletedMessage(reply)) {
                m_timeline.replyPreviews.remove(previewKey);
            } else if (!reply.isEmpty()) {
                entry.replyAuthor = authorLabel(authorOf(reply)).left(512);
                entry.replyText = field(reply, {"text", "TEXT"}).left(2000);
                entry.replyFiles = filesFor(reply);
                if (!dialog.isEmpty()) {
                    m_timeline.replyPreviews.insert(previewKey,
                        new ChatTimeline::ReplyPreview{entry.replyAuthor, entry.replyText, entry.replyFiles},
                        1 + entry.replyFiles.size());
                }
            } else if (!dialog.isEmpty()) {
                if (const auto *cached = m_timeline.replyPreviews.object(previewKey)) {
                    entry.replyAuthor = cached->author;
                    entry.replyText = cached->text;
                    entry.replyFiles = cached->files;
                }
            }
        }
        if (!dialog.isEmpty() && numericId > 0 && sameChat(message)) {
            const auto key = ChatTimeline::replyPreviewKey(dialog, QString::number(numericId));
            if (deletedMessage(message))
                m_timeline.replyPreviews.remove(key);
            else
                m_timeline.replyPreviews.insert(key, new ChatTimeline::ReplyPreview{entry.author.left(512), entry.text.left(2000), entry.files},
                                       1 + entry.files.size());
        }
        messages.append(std::move(entry));
    }
    if (sortChronologically) {
        std::stable_sort(messages.begin(), messages.end(), [](const Message &left, const Message &right) {
            if (left.createdAt != right.createdAt)
                return left.createdAt < right.createdAt;
            return left.id.toLongLong() < right.id.toLongLong();
        });
    }
    return messages;
}

void CloudBackend::requestMessageContext(const QString &dialogId, const QString &messageId)
{
    const auto operation = operationKey(OperationKind::MessageContext, dialogId, messageId);
    const auto expectedMessage = portalTaskIdentifier(messageId);
    if (expectedMessage.isEmpty()) {
        emit failed(operation, QStringLiteral("Некорректный идентификатор сообщения"));
        return;
    }
    resolveDialog(dialogId, false, operation, operation,
                  [this, dialogId, messageId, expectedMessage, operation](const QString &canonicalDialog, qint64 chatId) {
        // The browser uses this batch for context and marks the message read in a separate command.
        // Loading a context here only needs the read-only command in that batch.
        const auto url = OriginGuard(m_session.origin).resolve(QStringLiteral("/rest/batch.json"));
        const auto method = QStringLiteral("im.v2.Chat.Message.getContext?id=%1&range=25").arg(expectedMessage);
        m_portalHttp.webRequest(url, {{QStringLiteral("halt"), QStringLiteral("0")}, {QStringLiteral("cmd[context]"), method}},
                   operation, QStringLiteral("result"),
                   [this, dialogId, messageId, canonicalDialog, expectedMessage, chatId, operation](QJsonValue batch) {
            const auto result = batch.toObject().value(QStringLiteral("result")).toObject();
            const auto context = result.value(QStringLiteral("context")).toObject();
            const auto rows = context.value(QStringLiteral("messages"));
            if (!rows.isArray())
                return failShape(operation);
            const auto expectedChat = QString::number(chatId);
            const auto rootChatId = context.value(QStringLiteral("chatId"));
            const auto contextChatId = context.value(QStringLiteral("chat")).toObject().value(QStringLiteral("id"));
            if ((!rootChatId.isUndefined() && portalTaskIdentifier(cloud_rows::string(rootChatId)) != expectedChat)
                || (!contextChatId.isUndefined() && portalTaskIdentifier(cloud_rows::string(contextChatId)) != expectedChat))
                return failShape(operation);
            bool found = false;
            for (const auto value : rows.toArray()) {
                const auto row = value.toObject();
                const auto camelChat = row.value(QStringLiteral("chatId"));
                const auto snakeChat = row.value(QStringLiteral("chat_id"));
                if ((camelChat.isUndefined() && snakeChat.isUndefined())
                    || (!camelChat.isUndefined() && portalTaskIdentifier(cloud_rows::string(camelChat)) != expectedChat)
                    || (!snakeChat.isUndefined() && portalTaskIdentifier(cloud_rows::string(snakeChat)) != expectedChat)) {
                    emit failed(operation, QStringLiteral("Полученное сообщение относится к другому чату"));
                    return;
                }
                if (portalTaskIdentifier(cloud_rows::string(row.value(QStringLiteral("id")))) == expectedMessage)
                    found = true;
            }
            if (!found) {
                emit failed(operation, QStringLiteral("Сообщение отсутствует в истории чата"));
                return;
            }
            const auto messages = decodeMessages(context, true, canonicalDialog);
            if (std::none_of(messages.cbegin(), messages.cend(), [&](const auto &message) { return message.id == expectedMessage; }))
                return failShape(operation);
            emit messageContextLoaded(dialogId, messageId, messages);
        });
    });
}

} // namespace bx
