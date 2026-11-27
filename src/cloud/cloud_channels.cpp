#include "cloud/cloud_backend.h"

#include "cloud/cloud_avatars.h"
#include "cloud/cloud_chat_fields.h"
#include "cloud/cloud_chat_metadata.h"
#include "cloud/cloud_rows.h"

#include <QJsonArray>
#include <algorithm>

namespace bx {
using chat_fields::positiveId;
using chat_fields::groupChatId;
using cloud_rows::field;
using cloud_rows::date;

namespace {

constexpr int onlineLimitSeconds = 15 * 60;

} // namespace

void CloudBackend::rememberChatContact(const QString &dialog, const QJsonObject &user, bool publish)
{
    if (positiveId(dialog) == 0 || user.isEmpty())
        return;
    const auto userId = field(user, {"id", "ID"});
    if (!userId.isEmpty() && userId != dialog)
        return;
    auto activity = user.value(QStringLiteral("lastActivityDate"));
    if (activity.isUndefined())
        activity = user.value(QStringLiteral("last_activity_date"));
    const auto lastActivity = date(activity);
    const auto position = user.value(QStringLiteral("workPosition"));
    if (!lastActivity.isValid() && !position.isString())
        return;
    if (!m_chatContacts.contains(dialog) && m_chatContacts.size() >= 2 * tunables::kPageSizeChats)
        m_chatContacts.erase(m_chatContacts.begin());
    auto &contact = m_chatContacts[dialog];
    const auto previous = contact;
    if (lastActivity.isValid() && lastActivity <= QDateTime::currentDateTimeUtc().addSecs(300)) {
        const auto until = lastActivity.addSecs(onlineLimitSeconds);
        if (!contact.onlineUntil.isValid() || until > contact.onlineUntil)
            contact.onlineUntil = until;
    }
    if (position.isString())
        contact.position = position.toString().trimmed().left(256);
    if (!publish || (contact.position == previous.position && contact.onlineUntil == previous.onlineUntil))
        return;
    m_timeline.updateRecentChat(dialog, [this](Chat &chat) { applyChatContact(chat); });
    const auto current = contact;
    emit chatContactChanged(dialog, current.onlineUntil, current.position);
}

void CloudBackend::applyChatContact(Chat &chat) const
{
    if (chat.taskChat || positiveId(chat.id) == 0)
        return;
    const auto contact = m_chatContacts.constFind(chat.id);
    if (contact != m_chatContacts.cend()) {
        chat.position = contact->position;
        chat.onlineUntil = contact->onlineUntil;
    }
}

void CloudBackend::rememberDialogContact(const QString &dialog, const QJsonObject &payload)
{
    if (positiveId(dialog) == 0)
        return;
    for (const auto *fieldName : {"usersShort", "users", "additionalUsers"}) {
        const auto users = payload.value(QLatin1String(fieldName));
        const auto update = [this, &dialog](const QJsonObject &user) {
            if (field(user, {"id", "ID"}) == dialog)
                rememberChatContact(dialog, user, true);
        };
        if (users.isObject())
            update(users.toObject());
        else {
            for (const auto user : users.toArray())
                update(user.toObject());
        }
    }
}

void CloudBackend::handleOnlinePull(const QJsonObject &params)
{
    const auto users = params.value(QStringLiteral("users"));
    const auto update = [this](const QJsonObject &user) {
        const auto id = field(user, {"id"});
        if (positiveId(id) == 0 || (!m_chatContacts.contains(id) && id != m_timeline.activeDialog()
            && std::none_of(m_timeline.recentChats(false).cbegin(), m_timeline.recentChats(false).cend(), [&id](const Chat &chat) { return chat.id == id; })))
            return;
        rememberChatContact(id, user, true);
    };
    if (users.isArray()) {
        for (const auto user : users.toArray())
            update(user.toObject());
    } else {
        for (const auto user : users.toObject())
            update(user.toObject());
    }
}

void CloudBackend::rememberDialog(const QString &dialog, const QJsonObject &object, bool rememberCounter)
{
    auto chat = object.value(QStringLiteral("chat")).toObject();
    if (chat.isEmpty()) {
        for (const auto &item : object.value(QStringLiteral("chats")).toArray()) {
            const auto candidate = item.toObject();
            if (field(candidate, {"dialogId", "DIALOG_ID"}) == dialog) {
                chat = candidate;
                break;
            }
        }
    }
    if (!chat.isEmpty()) {
        auto &state = m_timeline.dialogMessageState(dialog);
        const auto readId = chat.value(QStringLiteral("lastId")).toInteger(-1);
        if (readId >= state.lastReadId) {
            state.lastReadId = readId;
            const auto unreadIdValue = chat.value(QStringLiteral("unreadId"));
            bool hasUnreadId = true;
            const auto unreadId = unreadIdValue.isString() ? unreadIdValue.toString().toLongLong(&hasUnreadId)
                : unreadIdValue.toInteger(-1);
            if (rememberCounter && hasUnreadId && unreadId >= 0)
                state.unreadCounter = unreadId == 0 ? std::optional<int>(0) : std::nullopt;
        }
        const auto muted = chat.value(QStringLiteral("isMuted"));
        const auto muteList = chat.value(QStringLiteral("muteList"));
        if (!state.muteFromPull && muted.isBool()) {
            state.muted = muted.toBool();
            state.muteList = {};
        } else if (!state.muteFromPull && (muteList.isArray() || muteList.isObject())) {
            state.muteList = muteList;
            state.muted.reset();
        }
    }
    auto id = positiveId(field(object, {"chatId", "CHAT_ID"}));
    if (id == 0)
        id = positiveId(field(chat, {"id", "chatId"}));
    if (id == 0)
        return;
    m_timeline.rememberChatId(dialog, id);
}

void CloudBackend::resolveDialog(const QString &scopeId, bool task, const QString &operation,
                                 const QString &requestKey, ResolvedDialog onDialog)
{
    if (task) {
        const auto known = m_timeline.taskDialog(scopeId);
        if (const auto id = groupChatId(known); id > 0) {
            onDialog(known, id);
            return;
        }
        m_tasks.loadTask(scopeId, operation, [this, scopeId, operation, onDialog](const QJsonObject &) {
            const auto dialog = m_timeline.taskDialog(scopeId);
            const auto chatId = groupChatId(dialog);
            if (chatId == 0) {
                emit failed(operation, QStringLiteral("У задачи нет чата"));
                return;
            }
            onDialog(dialog, chatId);
        }, requestKey);
        return;
    }

    const auto dialog = validDialog(scopeId);
    if (dialog.isEmpty()) {
        emit failed(operation, QStringLiteral("Некорректный номер диалога"));
        return;
    }
    if (auto chatId = groupChatId(dialog); chatId > 0 || (chatId = m_timeline.knownChatId(dialog)) > 0) {
        onDialog(dialog, chatId);
        return;
    }
    ajax(QStringLiteral("im.v2.Chat.load"),
         {{QStringLiteral("dialogId"), dialog}, {QStringLiteral("messageLimit"), QStringLiteral("1")}},
         operation, [this, operation, dialog, onDialog, scope = m_accountScopeRevision](QJsonValue data) {
        if (scope != m_accountScopeRevision)
            return;
        rememberDialog(dialog, data.toObject());
        const auto chatId = m_timeline.knownChatId(dialog);
        if (chatId == 0) {
            failShape(operation);
            return;
        }
        onDialog(dialog, chatId);
    }, requestKey);
}

} // namespace bx
