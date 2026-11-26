#include "chats_service.h"
#include "cloud_chat_fields.h"
#include "cloud_chat_metadata.h"
#include "cloud_rows.h"
#include "core/notification_targets.h"
#include "core/origin_guard.h"

#include <QRegularExpression>
#include <algorithm>

namespace bx {
using chat_fields::positiveId;
using chat_fields::groupChatId;
using cloud_rows::field;
using cloud_rows::date;

namespace {

constexpr qsizetype maxQueryLength = 512;

constexpr qsizetype maxPinnedMessages = 64;

constexpr qsizetype initialPinBlockSize = 50;

const QString resultEnvelope = QStringLiteral("result");

std::optional<qint64> taskSearchId(const QJsonObject &row, std::initializer_list<const char *> aliases,
                                   bool allowZero = false)
{
    std::optional<qint64> result;
    for (const auto *alias : aliases) {
        const auto value = row.value(QLatin1String(alias));
        if (value.isUndefined())
            continue;
        const auto raw = value.isDouble() ? QString::number(value.toInteger(-1)) : value.toString();
        const auto id = portalTaskIdentifier(raw);
        if (id.isEmpty() && !(allowZero && raw == QLatin1String("0")))
            return std::nullopt;
        const auto number = id.toLongLong();
        if (result && *result != number)
            return std::nullopt;
        result = number;
    }
    return result;
}

} // namespace

QVector<Message> ChatsService::decodePinnedMessages(const QJsonObject &payload) const
{
    QStringList keys;
    QSet<QString> seen;
    for (const auto record : payload.value(QStringLiteral("pins")).toArray()) {
        const auto key = field(record.toObject(), {"messageId"});
        if (positiveId(key) <= 0 || seen.contains(key))
            continue;
        keys.append(key);
        seen.insert(key);
        if (keys.size() == maxPinnedMessages)
            break;
    }
    QHash<QString, QJsonObject> rows;
    for (const auto *fieldName : {"additionalMessages", "messages"}) {
        for (const auto record : payload.value(QLatin1String(fieldName)).toArray()) {
            const auto row = record.toObject();
            const auto key = field(row, {"id", "ID"});
            if (seen.contains(key))
                rows.insert(key, row);
        }
    }
    QJsonArray messages;
    for (const auto &key : keys) {
        if (rows.contains(key))
            messages.append(rows.value(key));
        else
            messages.append(QJsonObject{{QStringLiteral("id"), key},
                                        {QStringLiteral("text"), QStringLiteral("Сообщение недоступно")}});
    }
    auto data = payload;
    data.insert(QStringLiteral("messages"), messages);
    return m_history.decodeMessages(data, false, m_pinnedDialog);
}

void ChatsService::publishPinnedMessages()
{
    auto rows = m_pinnedMessages;
    for (auto it = m_pinnedChanges.cbegin(); it != m_pinnedChanges.cend(); ++it) {
        rows.erase(std::remove_if(rows.begin(), rows.end(), [&it](const Message &row) {
            return row.id == it.key();
        }), rows.end());
        if (it.value())
            rows.append(*it.value());
    }
    std::sort(rows.begin(), rows.end(), [](const Message &a, const Message &b) {
        return positiveId(a.id) > positiveId(b.id);
    });
    if (rows.size() > maxPinnedMessages)
        rows.resize(maxPinnedMessages);
    const auto dialog = m_pinnedDialog;
    emit pinnedMessagesLoaded(dialog, rows);
}

void ChatsService::acceptPinnedSnapshot(const QString &dialog, const QJsonObject &payload)
{
    if (dialog.isEmpty() || dialog != m_pinnedDialog)
        return;
    const auto generation = m_pinnedGeneration;
    m_havePinnedSnapshot = true;
    m_pinnedMessages = decodePinnedMessages(payload);
    publishPinnedMessages();
    if (m_pinnedGeneration != generation)
        return;
    const auto pins = payload.value(QStringLiteral("pins")).toArray();
    if (pins.size() < initialPinBlockSize || m_pinnedMessages.isEmpty())
        return;
    const auto last = field(pins.last().toObject(), {"messageId"});
    if (positiveId(last) == 0)
        return;
    const auto operation = operationKey(OperationKind::Pins, dialog);
    m_portalHttp.ajax(QStringLiteral("im.v2.Chat.Pin.tail"),
         {{QStringLiteral("dialogId"), dialog}, {QStringLiteral("filter[lastId]"), last},
          {QStringLiteral("order[id]"), QStringLiteral("DESC")}, {QStringLiteral("limit"), QStringLiteral("25")}},
         operation, [this, generation, dialog, last](QJsonValue data) {
        if (m_pinnedGeneration != generation || m_pinnedDialog != dialog)
            return;
        auto next = decodePinnedMessages(data.toObject());
        for (auto &row : next) {
            if (m_pinnedMessages.size() == maxPinnedMessages)
                break;
            if (positiveId(row.id) < positiveId(last)
                && std::none_of(m_pinnedMessages.cbegin(), m_pinnedMessages.cend(), [&row](const Message &old) { return old.id == row.id; }))
                m_pinnedMessages.append(std::move(row));
        }
        publishPinnedMessages();
    }, operation + QLatin1Char(':') + QString::number(generation) + QStringLiteral(":tail"));
}

void ChatsService::requestPinnedMessages(const QString &dialogId)
{
    const auto dialog = validDialog(dialogId);
    if (dialog == m_pinnedDialog) {
        if (m_havePinnedSnapshot)
            publishPinnedMessages();
        return;
    }
    m_pinnedDialog = dialog;
    m_havePinnedSnapshot = false;
    m_pinnedMessages.clear();
    m_pinnedChanges.clear();
    const auto generation = ++m_pinnedGeneration;
    if (dialog.isEmpty())
        return;
    QTimer::singleShot(0, this, [this, dialog, generation] {
        if (!m_session.started() || m_pinnedGeneration != generation || m_havePinnedSnapshot)
            return;
        const auto historyOperation = operationKey(OperationKind::Chat, dialog);
        const auto taskOperation = operationKey(OperationKind::TaskMessages, m_timeline.activeTask());
        const bool pendingHistory = m_portalHttp.contains(historyOperation)
            || (m_timeline.activeTaskDialog() == dialog && m_portalHttp.contains(taskOperation))
            || std::any_of(m_historiesWaitingForIdentity.cbegin(), m_historiesWaitingForIdentity.cend(),
                           [&dialog](const auto &history) { return history.dialog == dialog; });
        if (pendingHistory)
            return;
        const auto operation = operationKey(OperationKind::Pins, dialog);
        m_portalHttp.ajax(QStringLiteral("im.v2.Chat.load"),
             {{QStringLiteral("dialogId"), dialog}, {QStringLiteral("messageLimit"), QStringLiteral("1")}},
             operation, [this, dialog, generation](QJsonValue data) {
            if (m_pinnedGeneration != generation)
                return;
            const auto payload = data.toObject();
            m_directory.rememberDialog(dialog, payload);
            m_directory.rememberDialogContact(dialog, payload);
            acceptPinnedSnapshot(dialog, payload);
        }, operation + QLatin1Char(':') + QString::number(generation));
    });
}

void ChatsService::handlePinnedPull(const QString &command, const QJsonObject &params)
{
    if (m_pinnedDialog.isEmpty())
        return;
    const bool add = command == QLatin1String("pinAdd");
    const auto record = add ? params.value(QStringLiteral("pin")).toObject() : params;
    const auto id = field(record, {"messageId"});
    const auto chat = positiveId(field(record, {"chatId"}));
    if (positiveId(id) == 0 || chat <= 0
        || (chat != m_timeline.knownChatId(m_pinnedDialog) && chat != groupChatId(m_pinnedDialog)))
        return;
    std::optional<Message> refreshed;
    if (add) {
        auto payload = params;
        payload.insert(QStringLiteral("pins"), QJsonArray{record});
        auto rows = decodePinnedMessages(payload);
        if (!rows.isEmpty())
            refreshed = std::move(rows.first());
    }
    if (!m_pinnedChanges.contains(id) && m_pinnedChanges.size() >= 2 * maxPinnedMessages)
        m_pinnedChanges.erase(m_pinnedChanges.begin());
    m_pinnedChanges.insert(id, std::move(refreshed));
    publishPinnedMessages();
}

void ChatsService::searchTaskChats(const QString &query)
{
    const auto operation = operationKey(OperationKind::SearchTaskChats, query);
    const auto needle = query.trimmed();
    if (needle.isEmpty()) {
        emit taskChatsFound(query, {});
        return;
    }
    if (needle.size() > maxQueryLength) {
        emit failed(operation, QStringLiteral("Слишком длинный запрос поиска"));
        return;
    }
    const auto number = needle.startsWith(QLatin1Char('#')) || needle.startsWith(QChar(u'№'))
        ? needle.mid(1).trimmed() : needle;
    const auto id = portalTaskIdentifier(number);
    Fields fields{{id.isEmpty() ? QStringLiteral("filter[%TITLE]") : QStringLiteral("filter[ID]"),
                   id.isEmpty() ? needle : id}, {QStringLiteral("start"), QStringLiteral("0")}};
    for (const auto *name : {"ID", "TITLE", "CHAT_ID", "GROUP_ID"})
        fields.append({QStringLiteral("select[]"), QLatin1String(name)});
    const auto scope = m_session.accountScopeRevision();
    const auto url = OriginGuard(m_session.portalOrigin()).resolve(QStringLiteral("/rest/tasks.task.list.json"));
    m_portalHttp.webRequest(url, std::move(fields), operation, resultEnvelope,
               [this, scope, query, needle, id, operation](QJsonValue data) {
        if (scope != m_session.accountScopeRevision() || m_session.authExpired())
            return;
        const auto rows = data.toObject().value(QStringLiteral("tasks"));
        if (!rows.isArray())
            return m_session.failShape(operation);
        QSet<qint64> seenTasks, seenChats;
        QVector<Chat> matches;
        const auto page = rows.toArray();
        const auto limit = qMin(qsizetype(tunables::kPageSizeChats), page.size());
        for (qsizetype index = 0; index < limit; ++index) {
            const auto row = page.at(index).toObject();
            const auto taskId = taskSearchId(row, {"id", "ID"});
            const auto chatId = taskSearchId(row, {"chatId", "CHAT_ID", "chat_id"});
            const auto group = taskSearchId(row, {"groupId", "GROUP_ID"}, true);
            if (!taskId || !chatId || !group || seenTasks.contains(*taskId) || seenChats.contains(*chatId))
                continue;
            const auto groupRow = row.value(QStringLiteral("group")).toObject();
            if (!groupRow.isEmpty() && taskSearchId(groupRow, {"id", "ID"}, true) != group)
                continue;
            const auto title = row.value(QStringLiteral("title"));
            const auto titleAlias = row.value(QStringLiteral("TITLE"));
            if (!title.isUndefined() && (!title.isString() || (!titleAlias.isUndefined() && title != titleAlias)))
                continue;
            const auto name = (title.isUndefined() ? titleAlias : title).toString();
            const auto task = QString::number(*taskId);
            if (name.trimmed().isEmpty() || (id.isEmpty() ? !name.contains(needle, Qt::CaseInsensitive) : task != id))
                continue;
            const auto dialog = QStringLiteral("chat") + QString::number(*chatId);
            const auto recent = std::find_if(m_timeline.recentChats(true).cbegin(), m_timeline.recentChats(true).cend(),
                [&dialog, &task](const Chat &chat) { return chat.id == dialog && chat.taskId == task; });
            Chat chat = recent == m_timeline.recentChats(true).cend() ? Chat{} : *recent;
            chat.id = dialog;
            chat.taskId = task;
            chat.serverChatId = *chatId;
            chat.taskChat = true;
            chat.kind = QStringLiteral("tasksTask");
            chat.title = name.simplified().left(500);
            m_badges.decorateProjectTaskChat(chat);
            seenTasks.insert(*taskId);
            seenChats.insert(*chatId);
            matches.append(std::move(chat));
        }
        emit taskChatsFound(query, matches);
    }, operation + QLatin1Char(':') + QString::number(scope));
}

void ChatsService::searchChats(const QString &query)
{
    const auto operation = operationKey(OperationKind::SearchChats, query);
    const auto trimmed = query.trimmed();
    if (trimmed.isEmpty()) {
        emit chatsFound(query, {});
        return;
    }
    if (trimmed.size() > maxQueryLength) {
        emit failed(operation, QStringLiteral("Слишком длинный запрос поиска"));
        return;
    }
    static const QRegularExpression separator(QStringLiteral("[\\s()\\[\\]{}<>#\\-\"']+"));
    QJsonArray words;
    for (const auto &word : trimmed.split(separator, Qt::SkipEmptyParts))
        words.append(word);
    const QJsonObject entity{{QStringLiteral("id"), QStringLiteral("im-recent-v2")},
                             {QStringLiteral("dynamicLoad"), true}, {QStringLiteral("dynamicSearch"), true},
                             {QStringLiteral("options"), QJsonObject{}}};
    const QJsonObject dialog{{QStringLiteral("entities"), QJsonArray{entity}},
                             {QStringLiteral("preselectedItems"), QJsonArray{}},
                             {QStringLiteral("clearUnavailableItems"), false},
                             {QStringLiteral("context"), QStringLiteral("IM_CHAT_SEARCH")},
                             {QStringLiteral("id"), QStringLiteral("im-chat-search")}};
    const QJsonObject search{{QStringLiteral("queryWords"), words}, {QStringLiteral("query"), trimmed}};
    m_portalHttp.ajaxJson(QStringLiteral("ui.entityselector.doSearch"),
             {{QStringLiteral("dialog"), dialog}, {QStringLiteral("searchQuery"), search}},
             operation, [this, query, operation](QJsonValue data) {
        const auto items = data.toObject().value(QStringLiteral("dialog")).toObject().value(QStringLiteral("items"));
        if (!items.isArray())
            return m_session.failShape(operation);
        QVector<Chat> matches;
        QSet<QString> seen;
        for (const auto row : items.toArray()) {
            const auto item = row.toObject();
            const auto id = validDialog(field(item, {"id"}));
            if (id.isEmpty() || seen.contains(id))
                continue;
            const auto custom = item.value(QStringLiteral("customData")).toObject();
            const auto chat = custom.value(QStringLiteral("chat")).toObject();
            auto result = chatFromMetadata(id, item, chat, custom.value(QStringLiteral("user")).toObject());
            if (result.title.isEmpty() || result.taskChat)
                continue;
            seen.insert(id);
            m_directory.rememberDialog(id, QJsonObject{{QStringLiteral("chat"), chat}});
            result.updated = date(custom.value(QStringLiteral("dateMessage")));
            matches.append(std::move(result));
            if (matches.size() >= tunables::kPageSizeChats)
                break;
        }
        emit chatsFound(query, matches);
    });
}

void ChatsService::requestOlderChatMessages(const QString &scopeId, const QString &beforeId, bool task)
{
    const auto operation = operationKey(OperationKind::OlderChat,
        task ? QStringLiteral("task") : QStringLiteral("chat"), scopeId, beforeId);
    const auto before = positiveId(beforeId);
    if (before == 0)
        return m_session.failShape(operation);
    const auto scope = m_session.accountScopeRevision();
    m_directory.resolveDialog(scopeId, task, operation, operation,
                  [this, scopeId, beforeId, before, task, operation, scope](const QString &dialog, qint64 chatId) {
        if (scope != m_session.accountScopeRevision())
            return;
        constexpr int limit = 25;
        const auto generation = m_timeline.historyGeneration(dialog);
        const auto revision = m_timeline.badgeRevision();
        m_portalHttp.ajax(QStringLiteral("im.v2.Chat.Message.tail"),
             {{QStringLiteral("chatId"), QString::number(chatId)}, {QStringLiteral("filter[lastId]"), QString::number(before)},
              {QStringLiteral("order[id]"), QStringLiteral("DESC")}, {QStringLiteral("limit"), QString::number(limit)}},
             operation, [this, scopeId, beforeId, before, task, operation, dialog, chatId, generation, scope, revision](QJsonValue data) {
            if (scope != m_session.accountScopeRevision() || m_timeline.historyGeneration(dialog) != generation)
                return;
            auto page = data.toObject();
            const auto rawRows = page.value(QStringLiteral("messages"));
            const auto responseChat = field(page, {"chatId", "CHAT_ID"});
            if (!rawRows.isArray() || (!responseChat.isEmpty() && positiveId(responseChat) != chatId))
                return m_session.failShape(operation);
            QSet<qint64> seen;
            QJsonArray rows;
            for (const auto &value : rawRows.toArray()) {
                const auto row = value.toObject();
                const auto id = positiveId(field(row, {"id", "ID"}));
                const auto rowChat = field(row, {"chatId", "chat_id", "CHAT_ID"});
                if (id <= 0 || id >= before || (!rowChat.isEmpty() && positiveId(rowChat) != chatId) || seen.contains(id))
                    continue;
                seen.insert(id);
                rows.append(row);
                if (rows.size() >= limit)
                    break;
            }
            page.insert(QStringLiteral("chatId"), chatId);
            page.insert(QStringLiteral("messages"), rows);
            if (task ? m_timeline.activeTask() == scopeId : m_timeline.activeDialog() == dialog)
                rememberUnreadHistoryRows(dialog, chatId, rows, revision);
            const auto messages = m_history.decodeMessages(page, true, dialog);
            const auto hasMore = !messages.isEmpty() &&
                page.value(QStringLiteral("hasNextPage")).toBool(rows.size() == limit);
            emit olderChatMessagesLoaded(scopeId, beforeId, task, messages, hasMore);
        });
    });
}

void ChatsService::searchMessages(const QString &scopeId, const QString &query, bool task)
{
    const auto operation = operationKey(task ? OperationKind::SearchTaskChat : OperationKind::SearchChat, scopeId);
    const auto trimmed = query.trimmed();
    if (trimmed.isEmpty()) {
        emit messagesFound(scopeId, query, task, {});
        return;
    }
    if (trimmed.size() > maxQueryLength) {
        emit failed(operation, QStringLiteral("Слишком длинный запрос поиска"));
        return;
    }
    const auto key = operation + QLatin1Char('\n') + trimmed;
    m_directory.resolveDialog(scopeId, task, operation, key,
                  [this, scopeId, query, trimmed, task, operation, key](const QString &dialog, qint64 chatId) {
        const auto url = OriginGuard(m_session.portalOrigin()).resolve(QStringLiteral("/rest/im.dialog.messages.search.json"));
        m_portalHttp.webRequest(url, {{QStringLiteral("CHAT_ID"), QString::number(chatId)},
                         {QStringLiteral("SEARCH_MESSAGE"), trimmed}}, operation, resultEnvelope,
                   [this, scopeId, query, task, operation, dialog](QJsonValue result) {
            const auto obj = result.toObject();
            if (!obj.value(QStringLiteral("messages")).isArray())
                return m_session.failShape(operation);
            auto messages = m_history.decodeMessages(obj, false, dialog);
            for (auto &message : messages)
                message.unread = false;
            emit messagesFound(scopeId, query, task, messages);
        }, key);
    });
}

} // namespace bx
