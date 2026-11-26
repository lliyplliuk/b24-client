#include "chats_service.h"
#include "cloud_chat_metadata.h"
#include "cloud_rows.h"
#include "core/origin_guard.h"

#include <QUuid>
#include <algorithm>
#include <utility>

namespace bx {
using namespace cloud_rows;

namespace {
const QString recentOp = operationKey(OperationKind::Chats);
const QString taskRecentOp = operationKey(OperationKind::TaskChats);
const QString settingsIdentityOp = operationKey(OperationKind::SettingsIdentity);
const QString recentUserStatusOp = operationKey(OperationKind::RecentUserStatus);

template<typename T>
void storeChatMetadata(QCache<QString, T> &cache, const QString &id, const T &value)
{
    // QCache trims before replacing a key, so update a hit in place at capacity.
    if (auto *cached = cache.object(id))
        *cached = value;
    else
        cache.insert(id, new T(value));
}

} // namespace

ChatsService::ChatsService(ChatsHost &host, ChatTimeline &timeline, PortalHttp &http, QObject *parent)
    : QObject(parent), m_host(host), m_timeline(timeline), m_portalHttp(http)
{
}

void ChatsService::invalidateRecentStatuses()
{
    ++m_recentStatusGeneration;
}

void ChatsService::forgetTaskTitle(const QString &task)
{
    m_taskChatTitles.remove(task);
}

void ChatsService::resumeRecentWithIdentity()
{
    const auto waiting = std::exchange(m_recentPullsAwaitingIdentity, {});
    for (const auto &event : waiting)
        applyRecentPull(event.command, event.dialog, event.chatId, event.params, event.extra);
}

void ChatsService::reset()
{
    m_personalChatStatuses.clear();
    m_recentPullsAwaitingIdentity.clear();
    m_pinnedDialog.clear();
    ++m_pinnedGeneration;
    m_havePinnedSnapshot = false;
    m_pinnedMessages.clear();
    m_pinnedChanges.clear();
    m_lastTypingRequest.clear();
    m_historiesWaitingForIdentity.clear();
    m_triedHistoryIdentity = false;
    m_recentTitles.clear();
    m_taskChatTitles.clear();
}

void ChatsService::rememberDismissalStatus(const QString &id, std::optional<bool> dismissed)
{
    const auto ttl = dismissed.has_value() ? 300 : 5;
    const PersonalChatStatus status{dismissed, QDateTime::currentSecsSinceEpoch() + ttl};
    storeChatMetadata(m_personalChatStatuses, id, status);
}

std::optional<bool> ChatsService::cachedDismissalStatus(const QString &id) const
{
    const auto *cached = m_personalChatStatuses.object(id);
    if (cached && cached->expiresAt > QDateTime::currentSecsSinceEpoch())
        return cached->dismissed;
    return std::nullopt;
}

Chat ChatsService::chatFromMetadata(const QString &dialog, const QJsonObject &row, const QJsonObject &chat,
                                    const QJsonObject &user)
{
    Chat result;
    result.id = dialog;
    auto type = field(chat, {"type", "kind"});
    if (type.isEmpty())
        type = field(row, {"type", "kind"});
    // The search selector calls every result a chat, including direct user dialogs.
    result.kind = type.isEmpty() ? field(row, {"entityType"}) : type;
    auto entity = chat.value(QStringLiteral("entityLink")).toObject();
    if (entity.isEmpty())
        entity = row.value(QStringLiteral("entityLink")).toObject();
    result.taskChat = isTaskChatMetadata(result.kind, entity);
    if (result.taskChat)
        result.taskId = numericId(field(entity, {"id"}));
    QString avatar;
    if (!dialog.startsWith(QLatin1String("chat"))) {
        result.title = userName(user);
        avatar = field(user, {"avatar", "avatarUrl"});
    }
    if (result.title.isEmpty())
        result.title = field(chat, {"name", "title"});
    if (result.title.isEmpty())
        result.title = field(row, {"title", "name"});
    if (avatar.isEmpty())
        avatar = field(chat, {"avatar", "avatarUrl"});
    if (avatar.isEmpty())
        avatar = field(row, {"avatar", "avatarUrl"});
    result.avatarUrl = m_host.avatarUrlFromPortal(avatar);
    result.pinned = notificationFlag(row.value(QStringLiteral("pinned"))).value_or(false);
    if (!result.taskChat && isPersonalMetadata(dialog, type)) {
        result.kind = QStringLiteral("im");
        const auto dismissed = dismissedPersonalUser(dialog, row, user);
        result.dismissed = dismissed.value_or(cachedDismissalStatus(dialog).value_or(false));
        if (dismissed)
            rememberDismissalStatus(dialog, dismissed);
        m_host.rememberChatContact(dialog, user, false);
        m_host.applyChatContact(result);
    }
    return result;
}

void ChatsService::requestRecentChats()
{
    m_timeline.showTaskChats = false;
    loadRecentChats(false);
    m_host.loadChatBadges();
}

void ChatsService::requestTaskChats()
{
    m_timeline.showTaskChats = true;
    loadRecentChats(true);
    m_host.loadChatBadges();
}

void ChatsService::requestTaskChatBadges()
{
    loadRecentChats(true, true);
}

void ChatsService::loadRecentChats(bool task, bool badgesOnly)
{
    const auto operation = task ? taskRecentOp : recentOp;
    m_portalHttp.ajax(QStringLiteral("im.v2.Recent.load"),
         {{QStringLiteral("filter[recentSection]"), task ? QStringLiteral("tasksTask") : QStringLiteral("default")},
          {QStringLiteral("limit"), QString::number(tunables::kPageSizeChats)}},
         operation, [this, task, badgesOnly, operation, scope = m_host.accountScopeRevision(),
                     counterRevision = m_timeline.chatBadgeRevision](QJsonValue data) {
        if (scope != m_host.accountScopeRevision())
            return;
        const auto rows = list(data, {"recentItems", "items", "recent"});
        if (!rows.isArray())
            return m_host.failShape(operation);

        const auto root = data.toObject();
        const auto userId = numericId(field(root, {"userId", "currentUserId"}));
        m_host.rememberCurrentUser(userId);
        const auto currentUser = numericId(m_host.currentUserId()).toLongLong();
        QHash<qint64, QJsonObject> messages, latestMessagesByChat;
        QHash<QString, QJsonObject> users, chatById, chatByDialog, counters;
        for (const auto value : root.value(QStringLiteral("messages")).toArray()) {
            const auto message = value.toObject();
            const auto id = recentMessageId(message);
            if (id <= 0)
                continue;
            messages.insert(id, message);
            const auto messageChatId = explicitMessageChatId(message);
            if (messageChatId > 0 && id > recentMessageId(latestMessagesByChat.value(messageChatId)))
                latestMessagesByChat.insert(messageChatId, message);
        }
        for (const auto *key : {"usersShort", "users"}) {
            for (const auto value : root.value(QLatin1String(key)).toArray()) {
                const auto user = value.toObject();
                const auto id = numericId(field(user, {"id", "ID"}));
                if (id.isEmpty())
                    continue;
                auto merged = users.value(id);
                for (auto it = user.constBegin(); it != user.constEnd(); ++it) {
                    if (!it.value().isNull() && (!it.value().isString() || !it.value().toString().isEmpty()))
                        merged.insert(it.key(), it.value());
                }
                users.insert(id, merged);
            }
        }
        for (const auto value : root.value(QStringLiteral("chats")).toArray()) {
            const auto chat = value.toObject();
            chatById.insert(field(chat, {"id", "chatId"}), chat);
            chatByDialog.insert(validDialog(field(chat, {"dialogId"})), chat);
        }
        const auto rawCounters = root.value(QStringLiteral("counters"));
        for (const auto value : rawCounters.toArray()) {
            const auto count = value.toObject();
            counters.insert(field(count, {"chatId"}), count);
        }
        const auto countersObject = rawCounters.toObject();
        for (auto it = countersObject.constBegin(); it != countersObject.constEnd(); ++it)
            counters.insert(it.key(), it.value().isObject() ? it.value().toObject()
                : QJsonObject{{QStringLiteral("counter"), it.value()}});

        QVector<Chat> chats;
        QVector<Chat> legacyTasks;
        QSet<QString> taskDialogs;
        QStringList usersWithoutStatus;
        for (const auto row : rows.toArray()) {
            const auto object = row.toObject();
            const auto recentChatId = field(object, {"chatId", "CHAT_ID"});
            auto dialogId = validDialog(field(object, {"dialogId", "DIALOG_ID", "id"}));
            auto metadata = object.value(QStringLiteral("chat")).toObject();
            if (metadata.isEmpty() && !recentChatId.isEmpty())
                metadata = chatById.value(recentChatId);
            if (metadata.isEmpty() && !dialogId.isEmpty())
                metadata = chatByDialog.value(dialogId);
            if (dialogId.isEmpty())
                dialogId = validDialog(field(metadata, {"dialogId"}));
            if (dialogId.isEmpty())
                dialogId = chatDialog(recentChatId);
            if (dialogId.isEmpty())
                continue;
            auto user = users.value(dialogId);
            if (user.isEmpty())
                user = object.value(QStringLiteral("user")).toObject();
            auto chat = chatFromMetadata(dialogId, object, metadata, user);
            if (chat.title.isEmpty())
                chat.title = QStringLiteral("Диалог %1").arg(dialogId);
            if (task && chat.kind.isEmpty())
                chat.taskChat = true; // A row in the dedicated server section may omit its type.

            auto rowChatId = recentChatId;
            if (rowChatId.isEmpty())
                rowChatId = field(metadata, {"id", "chatId"});
            chat.serverChatId = numericId(rowChatId).toLongLong();
            m_host.trackProjectChat(chat);
            const auto counter = counters.value(rowChatId);
            bool hasCounterSnapshot = false;
            const auto snapshotCount = counter.value(QStringLiteral("counter")).toVariant().toInt(&hasCounterSnapshot);
            chat.unread = hasCounterSnapshot ? std::max(0, snapshotCount)
                : std::max({0, oneOf(object, {"counter", "unreadCount"}).toVariant().toInt(),
                             metadata.value(QStringLiteral("counter")).toVariant().toInt()});
            const auto snapshotMarked = notificationFlag(counter.value(QStringLiteral("isMarkedAsUnread")));
            chat.markedUnread = snapshotMarked.value_or(notificationFlag(object.value(QStringLiteral("unread"))).value_or(false)
                || notificationFlag(metadata.value(QStringLiteral("isMarkedAsUnread"))).value_or(false));
            auto messageId = numericId(field(object, {"messageId"})).toLongLong();
            auto preview = object.value(QStringLiteral("message")).toObject();
            if (preview.isEmpty())
                preview = messages.value(messageId);
            const auto groupChatId = groupDialogChatId(dialogId);
            const auto expectedChatId = groupChatId > 0 ? groupChatId : chat.serverChatId;
            const auto previewChatId = explicitMessageChatId(preview);
            const bool foreignPreview = previewChatId != 0 && previewChatId != expectedChatId;
            // Recent rows can point into another chat's message in the shared list.
            const bool useChatMessage = foreignPreview || preview.isEmpty();
            if (useChatMessage) {
                preview = latestMessagesByChat.value(expectedChatId);
                if (foreignPreview || !preview.isEmpty())
                    messageId = recentMessageId(preview);
            }
            chat.lastMessage = field(preview, {"text"});
            const auto updated = oneOf(object, {"dateLastActivity", "dateUpdate", "updated"});
            const auto previewDate = date(preview.value(QStringLiteral("date")));
            chat.updated = (useChatMessage && previewDate.isValid()) || updated.isUndefined()
                ? previewDate : date(updated);
            bool hasReadId = false;
            const auto readId = field(metadata, {"lastId"}).toLongLong(&hasReadId);
            auto &live = m_timeline.recentOverlay(dialogId);
            const auto state = m_timeline.dialogMessageStates.constFind(dialogId);
            const auto unreadId = numericId(field(metadata, {"unreadId"})).toLongLong();
            const bool alreadyRead = chat.unread == 1 && unreadId > 0 && state != m_timeline.dialogMessageStates.cend()
                && (unreadId <= state->confirmedReadThrough || state->exactlyRead.contains(unreadId));
            const bool olderCursor = hasReadId && readId >= 0 && readId < live.confirmedCounterCursor;
            // A newer own preview alone does not supersede a confirmed read.
            const bool staleOwnCounter = live.counter && (alreadyRead || olderCursor) && currentUser > 0
                && expectedChatId > 0 && explicitMessageChatId(preview) == expectedChatId
                && explicitMessageAuthorId(preview) == currentUser;
            const bool counterCurrent = m_timeline.chatCounterSnapshotRevision <= counterRevision
                && live.counterRevision <= counterRevision && !staleOwnCounter;
            if (!metadata.isEmpty() || !recentChatId.isEmpty())
                m_host.rememberDialog(dialogId, {{QStringLiteral("chat"), metadata}, {QStringLiteral("chatId"), recentChatId}},
                               counterCurrent);
            auto &rejectedPreviewChatId = chat.taskChat ? live.rejectedTaskPreviewChatId : live.rejectedRecentPreviewChatId;
            rejectedPreviewChatId = foreignPreview && preview.isEmpty() ? expectedChatId : 0;
            const auto lastRead = m_timeline.dialogMessageStates.value(dialogId).lastReadId;
            if (messageId > live.messageId && messageId > lastRead) {
                live.messageId = messageId;
                // A newer preview can arrive in a response carrying an older unread count.
                if (counterCurrent) {
                    live.incomingId = 0;
                    if (!m_host.hasCounterSnapshot())
                        live.counter.reset();
                }
                live.preview = {};
            }
            if (counterCurrent && !m_host.hasCounterSnapshot() && live.incomingId > 0 && messageId >= live.messageId
                && readId >= live.incomingId && readId >= lastRead
                && (hasCounterSnapshot || chatUnreadCount(oneOf(object, {"counter", "unreadCount"})))) {
                live.counter = chat.unread;
            }

            storeChatMetadata(m_recentTitles, dialogId, chat.title);
            if (!task && chat.taskChat && legacyTasks.size() < tunables::kPageSizeChats)
                legacyTasks.append(chat);
            if (chat.taskChat == task && chats.size() < tunables::kPageSizeChats) {
                const auto *status = m_personalChatStatuses.object(dialogId);
                if (!task && chat.kind == QLatin1String("im")
                    && (!status || status->expiresAt <= QDateTime::currentSecsSinceEpoch())
                    && !usersWithoutStatus.contains(dialogId)) {
                    usersWithoutStatus.append(dialogId);
                }
                taskDialogs.insert(dialogId);
                chats.append(std::move(chat));
            }
        }
        m_host.publishProjectChatCounts();
        if (!task) {
            m_timeline.legacyTaskChats = std::move(legacyTasks);
            const auto generation = ++m_recentStatusGeneration;
            publishRecentChats(chats, false);
            if (!usersWithoutStatus.isEmpty())
                hydrateRecentUserStatuses(chats, usersWithoutStatus, generation);
            if (m_host.currentUserId().isEmpty() && std::any_of(chats.cbegin(), chats.cend(), [](const Chat &chat) { return chat.dismissed; }))
                m_host.withCurrentUser(settingsIdentityOp, [](qint64) {});
            return;
        }
        for (const auto &legacy : std::as_const(m_timeline.legacyTaskChats)) {
            if (chats.size() >= tunables::kPageSizeChats)
                break;
            if (!taskDialogs.contains(legacy.id))
                chats.append(legacy);
        }
        ++m_taskTitleGeneration;
        if (badgesOnly && !m_timeline.showTaskChats) {
            publishRecentChats(std::move(chats), true);
            return;
        }
        resolveTaskChatTitles(std::move(chats), taskRecentOp, [this](QVector<Chat> result) {
            for (const auto &chat : result)
                storeChatMetadata(m_recentTitles, chat.id, chat.title);
            publishRecentChats(std::move(result), true);
        });
    });
}

void ChatsService::hydrateRecentUserStatuses(QVector<Chat> chats, const QStringList &ids, quint64 generation)
{
    Fields fields;
    QSet<QString> pending;
    for (const auto &id : ids) {
        fields.append({QStringLiteral("ID[%1]").arg(fields.size()), id});
        pending.insert(id);
        rememberDismissalStatus(id, std::nullopt);
    }
    const auto url = OriginGuard(m_host.portalOrigin()).resolve(QStringLiteral("/rest/im.user.list.get.json"));
    m_portalHttp.webRequest(url, fields, recentUserStatusOp, QStringLiteral("result"),
               [this, pending = std::move(pending), chats = std::move(chats), generation](QJsonValue data) mutable {
        if (data.isNull() || generation != m_recentStatusGeneration)
            return;
        const auto record = [this, &pending](const QJsonObject &user, const QString &id) {
            if (pending.contains(id))
                rememberDismissalStatus(id, dismissedPersonalUser(id, {}, user));
        };
        if (data.isArray()) {
            for (const auto item : data.toArray()) {
                const auto user = item.toObject();
                record(user, numericId(field(user, {"id", "ID"})));
            }
        } else {
            const auto keyed = data.toObject();
            for (auto it = keyed.constBegin(); it != keyed.constEnd(); ++it) {
                const auto id = numericId(it.key());
                const auto user = it.value().toObject();
                const auto embedded = oneOf(user, {"id", "ID"});
                if (embedded.isUndefined() || numericId(string(embedded)) == id)
                    record(user, id);
            }
        }
        bool changed = false;
        for (auto &chat : chats) {
            if (pending.contains(chat.id) && cachedDismissalStatus(chat.id) == true && !chat.dismissed) {
                chat.dismissed = true;
                changed = true;
            }
        }
        if (changed) {
            publishRecentChats(std::move(chats), false);
            if (m_host.currentUserId().isEmpty())
                m_host.withCurrentUser(settingsIdentityOp, [](qint64) {});
        }
    }, recentUserStatusOp + QLatin1Char(':') + QString::number(generation));
}

void ChatsService::publishRecentChats(QVector<Chat> chats, bool task)
{
    QSet<QString> listed;
    bool hasLivePreview = false;
    for (auto &chat : chats) {
        listed.insert(chat.id);
        m_host.decorateChatBadges(chat);
        m_host.applyChatContact(chat);
        const auto live = m_timeline.recentOverlays.constFind(chat.id);
        if (live == m_timeline.recentOverlays.cend())
            continue;
        if (live->counter)
            chat.unread = *live->counter;
        if (live->markedUnread)
            chat.markedUnread = *live->markedUnread;
        const auto rejectedChatId = task ? live->rejectedTaskPreviewChatId : live->rejectedRecentPreviewChatId;
        const auto &history = live->historyPreview;
        const bool recoveredPreview = rejectedChatId > 0 && rejectedChatId == history.chatId;
        if (recoveredPreview) {
            chat.lastMessage = history.text;
            if (history.updated.isValid())
                chat.updated = history.updated;
            hasLivePreview = true;
        }
        if (live->preview.updated.isValid() && (!chat.updated.isValid() || live->preview.updated >= chat.updated)
            && (!recoveredPreview || live->messageId >= history.messageId)) {
            chat.lastMessage = live->preview.lastMessage;
            chat.updated = live->preview.updated;
            hasLivePreview = true;
        }
    }
    for (const auto &live : std::as_const(m_timeline.recentOverlays)) {
        const auto &preview = live.preview;
        if (preview.id.isEmpty() || preview.taskChat != task || listed.contains(preview.id)
            || (live.counter.value_or(0) == 0 && !live.markedUnread.value_or(false)))
            continue;
        auto chat = preview;
        m_host.decorateChatBadges(chat);
        m_host.applyChatContact(chat);
        chat.unread = live.counter.value_or(0);
        chat.markedUnread = live.markedUnread.value_or(false);
        chats.append(std::move(chat));
        hasLivePreview = true;
    }
    if (hasLivePreview) {
        std::stable_sort(chats.begin(), chats.end(), [this](const Chat &left, const Chat &right) {
            if (left.pinned != right.pinned)
                return left.pinned;
            if (left.pinned)
                return false;
            if (left.updated != right.updated)
                return left.updated > right.updated;
            return m_timeline.recentOverlays.value(left.id).messageId > m_timeline.recentOverlays.value(right.id).messageId;
        });
    }
    if (chats.size() > tunables::kPageSizeChats)
        chats.resize(tunables::kPageSizeChats);
    if (task) {
        m_timeline.recentTaskChatItems = std::move(chats);
        emit taskChatsLoaded(m_timeline.recentTaskChatItems);
    } else {
        m_timeline.recentChatItems = std::move(chats);
        emit recentChatsLoaded(m_timeline.recentChatItems);
    }
    if (!m_host.hasCounterSnapshot())
        m_host.publishUnreadChatCount();
}

void ChatsService::rememberTaskTitle(const QString &id, const QString &title)
{
    if (id.isEmpty() || title.isEmpty())
        return;
    storeChatMetadata(m_taskChatTitles, id, title);
}

QString ChatsService::recentTitle(const QString &dialog, const QString &fallback) const
{
    const auto *title = m_recentTitles.object(dialog);
    return title ? *title : fallback;
}

void ChatsService::resolveTaskChatTitles(QVector<Chat> chats, const QString &key, std::function<void(QVector<Chat>)> deliver)
{
    Fields fields{{QStringLiteral("select[]"), QStringLiteral("ID")},
                  {QStringLiteral("select[]"), QStringLiteral("TITLE")},
                  {QStringLiteral("start"), QStringLiteral("-1")}};
    QSet<QString> missing;
    for (auto &chat : chats) {
        if (chat.taskId.isEmpty())
            continue;
        if (const auto *title = m_taskChatTitles.object(chat.taskId)) {
            chat.title = *title;
        } else {
            missing.insert(chat.taskId);
        }
    }
    const auto generation = m_taskTitleGeneration;
    const auto scope = m_host.accountScopeRevision();
    // Show available rows even if the optional title batch is unavailable.
    deliver(chats);
    if (missing.isEmpty() || scope != m_host.accountScopeRevision() || generation != m_taskTitleGeneration)
        return;
    auto ids = missing.values();
    std::sort(ids.begin(), ids.end());
    for (qsizetype i = 0; i < ids.size(); ++i)
        fields.append({QStringLiteral("filter[ID][%1]").arg(i), ids.at(i)});
    m_portalHttp.ajax(QStringLiteral("tasks.task.list"), fields, operationKey(OperationKind::TaskChatTitles),
         [this, generation, chats = std::move(chats), missing = std::move(missing),
          deliver = std::move(deliver)](QJsonValue data) mutable {
        if (generation != m_taskTitleGeneration)
            return;
        for (const auto row : data.toObject().value(QStringLiteral("tasks")).toArray()) {
            const auto item = row.toObject();
            const auto id = numericId(field(item, {"id", "ID"}));
            if (missing.contains(id))
                rememberTaskTitle(id, field(item, {"title", "TITLE"}));
        }
        bool changed = false;
        for (auto &chat : chats) {
            const auto *title = m_taskChatTitles.object(chat.taskId);
            if (title && chat.title != *title) {
                chat.title = *title;
                changed = true;
            }
        }
        if (changed)
            deliver(std::move(chats));
    }, operationKey(OperationKind::TaskChatTitlesRequest, key, ids.join(QLatin1Char(','))));
}

void ChatsService::applyRecentPull(const QString &command, const QString &dialog, const QString &chatId,
                                  const QJsonObject &params, const QJsonObject &extra)
{
    if (dialog.isEmpty())
        return;
    const bool messageEvent = command == QLatin1String("message") || command == QLatin1String("messageChat");
    const bool manualRead = command == QLatin1String("chatUnread");
    const bool readEvent = command == QLatin1String("readMessage") || command == QLatin1String("readMessageChat");
    if (!messageEvent && !manualRead && !readEvent)
        return;

    const auto counter = chatUnreadCount(params.value(QStringLiteral("counter")));
    const auto marked = manualRead ? std::optional<bool>(params.value(QStringLiteral("active")).toBool())
        : readEvent ? notificationFlag(params.value(QStringLiteral("unread"))) : std::nullopt;
    if (!counter && !marked)
        return;
    const auto message = params.value(QStringLiteral("message")).toObject();
    const auto messageId = numericId(field(message, {"id"})).toLongLong();
    const auto author = numericId(field(message, {"senderId", "authorId"}));
    const auto state = m_timeline.dialogMessageStates.constFind(dialog);
    if (messageEvent) {
        const auto groupChatId = groupDialogChatId(dialog);
        const auto knownChatId = groupChatId > 0 ? groupChatId : m_host.badgeChatId(dialog);
        const auto eventChatId = chatId.toLongLong();
        const auto expectedChatId = knownChatId > 0 ? knownChatId : eventChatId;
        const auto messageChatId = explicitMessageChatId(message);
        if ((knownChatId > 0 && eventChatId > 0 && eventChatId != knownChatId)
            || (messageChatId != 0 && messageChatId != expectedChatId))
            return;
        if (!counter || messageId <= 0 || author.isEmpty() || author == m_host.currentUserId()
            || m_timeline.sentTemplates.contains(field(message, {"templateId", "uuid"})))
            return;
        if (messageId <= m_timeline.recentOverlays.value(dialog).messageId
            || (state != m_timeline.dialogMessageStates.cend()
                && (messageId <= state->lastReadId || state->exactlyRead.contains(messageId))))
            return;
        const auto sent = date(extra.value(QStringLiteral("server_time_unix")));
        if (extra.value(QStringLiteral("server_time_ago")).toInteger() > freshPopupSeconds
            || (sent.isValid() && sent.secsTo(QDateTime::currentDateTimeUtc()) > freshPopupSeconds))
            return;
        if (m_host.currentUserId().isEmpty()) {
            if (m_recentPullsAwaitingIdentity.size() >= 8)
                m_recentPullsAwaitingIdentity.removeFirst();
            m_recentPullsAwaitingIdentity.append({command, dialog, chatId, params, extra});
            m_host.withCurrentUser(settingsIdentityOp, [this](qint64 id) {
                if (id == 0)
                    m_recentPullsAwaitingIdentity.clear();
            });
            return;
        }
        const auto members = params.value(QStringLiteral("userInChat")).toObject().value(chatId);
        if (members.isArray()) {
            const auto users = members.toArray();
            if (std::none_of(users.cbegin(), users.cend(), [this](const QJsonValue &user) { return string(user) == m_host.currentUserId(); }))
                return;
        }
    }

    const auto sections = params.value(QStringLiteral("recentConfig")).toObject().value(QStringLiteral("sections")).toArray();
    const auto lookup = [&dialog](const QVector<Chat> &chats) {
        return std::find_if(chats.cbegin(), chats.cend(), [&dialog](const Chat &chat) { return chat.id == dialog; });
    };
    const auto ordinary = lookup(m_timeline.recentChatItems);
    const auto task = lookup(m_timeline.recentTaskChatItems);
    const bool listedOrdinary = ordinary != m_timeline.recentChatItems.cend();
    const bool listedTask = task != m_timeline.recentTaskChatItems.cend();
    const auto chat = params.value(QStringLiteral("chat")).toObject().value(chatId).toObject();
    const auto user = params.value(QStringLiteral("users")).toObject().value(dialog).toObject();
    auto candidate = chatFromMetadata(dialog, {}, chat, user);
    candidate.serverChatId = chatId.toLongLong();
    candidate.taskChat = isRecentTaskChat(candidate.taskChat, listedTask, sections);
    const bool listed = candidate.taskChat ? listedTask : listedOrdinary;
    const auto previous = m_timeline.recentOverlays.constFind(dialog);
    const bool knownRow = listed || (previous != m_timeline.recentOverlays.cend() && !previous->preview.id.isEmpty());
    if (candidate.taskChat && listedTask)
        candidate = *task;
    else if (!candidate.taskChat && listedOrdinary)
        candidate = *ordinary;

    const auto eventChatId = chatId.toLongLong();
    const auto canonicalChatId = eventChatId > 0 ? eventChatId
        : candidate.serverChatId > 0 ? candidate.serverChatId : m_host.badgeChatId(dialog);
    m_host.trackProjectChat(candidate);
    const bool inRecentScope = knownRow || (candidate.taskChat ? !sections.isEmpty()
                                                               : sections.contains(QStringLiteral("default")));
    const bool inProjectScope = m_host.hasProjectChat(canonicalChatId)
        && dialog == chatDialog(QString::number(canonicalChatId));
    // Reads, manual marks and updates for snapshot chats also affect the global count off page.
    if (!inRecentScope && !inProjectScope
        && (canonicalChatId <= 0 || (messageEvent && !m_host.hasChatCounter(canonicalChatId))))
        return;
    auto &live = m_timeline.recentOverlay(dialog);
    live.chatId = canonicalChatId;
    live.counterRevision = ++m_timeline.chatBadgeRevision;
    if (counter)
        live.counter = counter;
    if (marked)
        live.markedUnread = marked;
    live.eventTime = std::max(live.eventTime, field(extra, {"server_time_unix"}).toLongLong());
    if (messageEvent) {
        live.messageId = messageId;
        live.incomingId = messageId;
        if (inRecentScope && (!params.value(QStringLiteral("skipRecentUpdate")).toBool() || !knownRow)) {
            if (candidate.title.isEmpty())
                candidate.title = recentTitle(dialog, QStringLiteral("Диалог %1").arg(dialog));
            candidate.lastMessage = field(message, {"text", "message"}).left(2000);
            candidate.updated = date(message.value(QStringLiteral("date")));
            if (!candidate.updated.isValid())
                candidate.updated = date(extra.value(QStringLiteral("server_time_unix")));
            if (!candidate.updated.isValid())
                candidate.updated = QDateTime::currentDateTimeUtc();
            live.preview = candidate;
        }
    }
    if (inRecentScope && (knownRow || live.counter.value_or(0) > 0 || live.markedUnread.value_or(false)))
        publishRecentChats(candidate.taskChat ? m_timeline.recentTaskChatItems : m_timeline.recentChatItems, candidate.taskChat);
    if (inProjectScope)
        m_host.publishProjectChatBadges(canonicalChatId);
    m_host.publishUnreadChatCount();
}

bool ChatsService::acceptsRecentRead(const QString &dialog, const QJsonObject &params, const QJsonObject &extra) const
{
    const auto live = m_timeline.recentOverlays.constFind(dialog);
    const auto state = m_timeline.dialogMessageStates.constFind(dialog);
    const auto currentRead = state == m_timeline.dialogMessageStates.cend() ? -1 : state->lastReadId;
    const auto eventTime = field(extra, {"server_time_unix"}).toLongLong();
    bool hasRead = false;
    const auto lastRead = field(params, {"lastId"}).toLongLong(&hasRead);
    const bool exact = notificationFlag(params.value(QStringLiteral("exact"))).value_or(false);
    if (hasRead && !exact && lastRead < currentRead)
        return false;
    if (live == m_timeline.recentOverlays.cend())
        return true;
    if (eventTime > 0 && eventTime < live->eventTime)
        return false;
    if (!hasRead || live->incomingId <= lastRead || live->counter.value_or(0) <= 0)
        return true;

    const auto viewed = params.value(QStringLiteral("viewedMessages")).toArray();
    if (std::any_of(viewed.cbegin(), viewed.cend(), [id = live->incomingId](const QJsonValue &value) {
            return string(value).toLongLong() == id;
        }))
        return true;
    // A read below the latest unread message can clear earlier messages. It cannot clear the
    // whole chat. Require a current server event or advancing cursor before adopting a partial count.
    const auto counter = chatUnreadCount(params.value(QStringLiteral("counter")));
    return counter != 0 && (lastRead > currentRead || (eventTime > 0 && eventTime > live->eventTime));
}

} // namespace bx
