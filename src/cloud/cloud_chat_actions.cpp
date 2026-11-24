#include "cloud_backend.h"
#include "reaction_code.h"

#include "core/origin_guard.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QUuid>

namespace bx {
namespace {

QString portalId(const QString &raw)
{
    static const QRegularExpression digits(QStringLiteral("^[1-9][0-9]{0,18}$"));
    return digits.match(raw).hasMatch() && raw.toLongLong() > 0 ? raw : QString();
}

QString portalDialog(const QString &raw)
{
    const bool group = raw.startsWith(QLatin1String("chat"));
    const auto id = portalId(group ? raw.mid(4) : raw);
    return id.isEmpty() ? QString() : group ? QStringLiteral("chat") + id : id;
}

QString jsonId(const QJsonValue &value)
{
    return portalId(value.isString() ? value.toString() : QString::number(value.toInteger()));
}

bool commandAccepted(const QJsonValue &data)
{
    if (data.isBool())
        return data.toBool();
    if (data.isObject()) {
        const auto object = data.toObject();
        return !object.contains(QStringLiteral("error"))
            && object.value(QStringLiteral("result")) != QJsonValue(false)
            && object.value(QStringLiteral("success")) != QJsonValue(false);
    }
    return data.isArray();
}

} // namespace

void CloudBackend::notifyChatTyping(const QString &dialog)
{
    const auto scope = portalDialog(dialog);
    if (!m_started || !m_foreground || !m_online || scope.isEmpty() || m_authExpired
        || (m_userId.isEmpty() && !scope.startsWith(QLatin1String("chat"))) || scope == m_userId
        || (scope != m_openDialog && scope != m_taskDialogs.value(m_openTask)))
        return;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto previous = m_lastTypingRequest.value(scope, 0);
    if (previous > 0 && now >= previous && now - previous < 15000)
        return;
    if (!m_lastTypingRequest.contains(scope) && m_lastTypingRequest.size() >= 32)
        m_lastTypingRequest.erase(m_lastTypingRequest.begin());
    m_lastTypingRequest.insert(scope, now);
    ajaxJson(QStringLiteral("im.v2.Chat.InputAction.notify"),
             {{QStringLiteral("dialogId"), scope}, {QStringLiteral("type"), QStringLiteral("writing")}},
             QStringLiteral("chat-typing:") + scope, [](const QJsonValue &) {});
}

void CloudBackend::refreshChangedChat(const QString &dialog, bool pins)
{
    if (m_openDialog == dialog)
        m_chatDirty = true;
    if (m_taskDialogs.value(m_openTask) == dialog) {
        if (m_foreground)
            requestTaskMessages(m_openTask);
        else
            m_taskMessagesDirty = true;
    }
    if (pins && m_pinnedDialog == dialog)
        requestPinnedMessages(dialog);
    if (m_activeSection == 0)
        m_recentDirty = true;
    scheduleRefresh();
}

void CloudBackend::changeMessage(const QString &dialog, const QString &id, MessageAction action, const QString &value)
{
    const auto scope = portalDialog(dialog);
    const auto message = portalId(id);
    const bool reaction = action == MessageAction::React || action == MessageAction::RemoveReaction;
    if (reaction && !isReactionCode(value)) {
        emit failed(QStringLiteral("message-reaction"), QStringLiteral("Некорректный код реакции"));
        return;
    }
    const auto prefix = action == MessageAction::CreateTask ? QStringLiteral("create-task-from-message:")
        : action == MessageAction::Edit ? QStringLiteral("message-edit:") : QStringLiteral("message-action:");
    const auto operation = reaction ? reactionOperationKey(scope, message, value) : prefix + scope + QLatin1Char(':') + message;
    if (scope.isEmpty() || message.isEmpty()) {
        emit failed(operation, QStringLiteral("Некорректный диалог или сообщение"));
        return;
    }
    const auto messageNumber = message.toLongLong();
    const auto acknowledge = [this, scope, message, operation, action, value](const QJsonValue &data) {
        if (!commandAccepted(data)) {
            emit failed(operation, QStringLiteral("Портал отклонил действие с сообщением"));
            return;
        }
        const bool pins = action == MessageAction::Pin || action == MessageAction::Unpin;
        if (action != MessageAction::Favorite && action != MessageAction::Unfavorite)
            refreshChangedChat(scope, pins);
        QString feedback = QStringLiteral("Портал подтвердил действие");
        if (action == MessageAction::Favorite)
            feedback = QStringLiteral("Добавлено в избранное");
        else if (action == MessageAction::Unfavorite)
            feedback = QStringLiteral("Удалено из избранного");
        if (action == MessageAction::Edit)
            emit messageEditAccepted(scope, message, value);
        if (action == MessageAction::Delete)
            emit chatHistoryRemoved(scope, {message});
        if (action == MessageAction::React || action == MessageAction::RemoveReaction)
            emit chatOwnReactionChanged(scope, message, value, action == MessageAction::React);
        emit chatActionSucceeded(scope, feedback);
    };
    const auto rest = [this, &operation, &acknowledge](const QString &method, const Fields &fields) {
        const auto url = OriginGuard(m_session.origin).resolve(QStringLiteral("/rest/") + method + QStringLiteral(".json"));
        webRequest(url, fields, operation, QStringLiteral("result"), acknowledge);
    };

    switch (action) {
    case MessageAction::React:
    case MessageAction::RemoveReaction: {
        const auto method = action == MessageAction::React ? QStringLiteral("im.v2.Chat.Message.Reaction.add")
                                                           : QStringLiteral("im.v2.Chat.Message.Reaction.delete");
        ajax(method, {{QStringLiteral("messageId"), QString::number(messageNumber)},
                      {QStringLiteral("reaction"), value}}, operation, acknowledge);
        break;
    }
    case MessageAction::Edit:
        if (value.trimmed().isEmpty() || value.size() > 20000) {
            emit failed(operation, QStringLiteral("Введите сообщение длиной до 20 000 символов"));
            return;
        }
        ajax(QStringLiteral("im.v2.Chat.Message.update"),
             {{QStringLiteral("id"), QString::number(messageNumber)},
              {QStringLiteral("fields[message]"), value}}, operation, acknowledge);
        break;
    case MessageAction::Delete:
        ajaxJson(QStringLiteral("im.v2.Chat.Message.delete"),
                 {{QStringLiteral("messageIds"), QJsonArray{messageNumber}}}, operation, acknowledge);
        break;
    case MessageAction::CreateTask: {
        if (!m_started || !m_online) {
            emit failed(operation, QStringLiteral("Создание задачи доступно при подключении к порталу"));
            break;
        }
        if ((m_activeSection != 0 || m_openDialog != scope) &&
            (m_openTask.isEmpty() || m_taskDialogs.value(m_openTask) != scope)) {
            emit failed(operation, QStringLiteral("Откройте исходный чат сообщения"));
            break;
        }
        const auto url = OriginGuard(m_session.origin).resolve(QStringLiteral("/rest/im.message.share.json"));
        const auto account = m_accountScopeRevision;
        webRequest(url, {{QStringLiteral("MESSAGE_ID"), message}, {QStringLiteral("DIALOG_ID"), scope},
                         {QStringLiteral("TYPE"), QStringLiteral("TASK")}},
                   operation, QStringLiteral("result"), [this, operation, scope, account](const QJsonValue &result) {
            if (account != m_accountScopeRevision)
                return;
            if (result != QJsonValue(true)) {
                emit failed(operation, QStringLiteral("Портал не подтвердил создание задачи"));
                return;
            }
            if (m_activeSection == 0 && m_openDialog == scope)
                m_chatDirty = true;
            if (!m_openTask.isEmpty() && m_taskDialogs.value(m_openTask) == scope)
                m_taskMessagesDirty = true;
            scheduleRefresh();
            emit chatActionSucceeded(scope, QStringLiteral("Портал принял запрос на создание задачи"));
        });
        break;
    }
    case MessageAction::Pin:
    case MessageAction::Unpin:
        rest(action == MessageAction::Pin ? QStringLiteral("im.v2.Chat.Message.pin")
                                          : QStringLiteral("im.v2.Chat.Message.unpin"),
             {{QStringLiteral("id"), message}});
        break;
    case MessageAction::Favorite:
    case MessageAction::Unfavorite:
        rest(action == MessageAction::Favorite ? QStringLiteral("im.chat.favorite.add")
                                               : QStringLiteral("im.chat.favorite.delete"),
             {{QStringLiteral("MESSAGE_ID"), message}});
        break;
    }
}

void CloudBackend::forwardMessage(const QString &source, const QString &id, const QString &target)
{
    const auto origin = portalDialog(source);
    const auto destination = portalDialog(target);
    const auto message = portalId(id);
    const auto operation = QStringLiteral("forward-message:") + origin + QLatin1Char(':') + message;
    if (origin.isEmpty() || destination.isEmpty() || message.isEmpty()) {
        emit failed(operation, QStringLiteral("Некорректный диалог или сообщение для пересылки"));
        return;
    }
    if (m_inflightOperations.contains(operation))
        return;
    const auto uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_sentTemplates.enqueue(uuid);
    if (m_sentTemplates.size() > 64)
        m_sentTemplates.dequeue();
    const QJsonObject fields{{QStringLiteral("forwardIds"), QJsonObject{{uuid, message.toLongLong()}}}};
    ajaxJson(QStringLiteral("im.v2.Chat.Message.send"),
             {{QStringLiteral("dialogId"), destination}, {QStringLiteral("fields"), fields}},
             operation, [this, operation, origin, destination, uuid](const QJsonValue &value) {
        const auto result = value.toObject();
        const auto failures = result.value(QStringLiteral("failureMap")).toObject();
        const auto id = jsonId(result.value(QStringLiteral("uuidMap")).toObject().value(uuid));
        if (failures.contains(uuid) || id.isEmpty()) {
            emit failed(operation, QStringLiteral("Портал не подтвердил пересылку"));
            return;
        }
        refreshChangedChat(destination);
        emit chatActionSucceeded(origin, QStringLiteral("Сообщение переслано"));
    });
}

void CloudBackend::createGroupChat(const QString &title, const QStringList &members)
{
    const auto op = QStringLiteral("create-chat");
    const auto name = title.trimmed();
    if (name.isEmpty() || name.size() > 255 || members.isEmpty() || members.size() > 50) {
        emit failed(op, QStringLiteral("Укажите название и от 1 до 50 участников"));
        return;
    }
    const auto url = OriginGuard(m_session.origin).resolve(QStringLiteral("/rest/im.v2.Chat.add.json"));
    Fields fields{{QStringLiteral("fields[title]"), name}, {QStringLiteral("fields[type]"), QStringLiteral("CHAT")},
                  {QStringLiteral("fields[searchable]"), QStringLiteral("N")}};
    QSet<QString> seen;
    for (const auto &member : members) {
        const auto id = portalId(member);
        if (id.isEmpty()) {
            emit failed(op, QStringLiteral("Идентификаторы участников должны быть положительными числами"));
            return;
        }
        if (seen.contains(id))
            continue;
        seen.insert(id);
        fields.append({QStringLiteral("fields[users][%1]").arg(seen.size() - 1), id});
    }
    webRequest(url, fields, op, QStringLiteral("result"), [this, op](const QJsonValue &value) {
        const auto id = jsonId(value.toObject().value(QStringLiteral("chatId")));
        if (id.isEmpty()) {
            emit failed(op, QStringLiteral("Портал не подтвердил создание чата"));
            return;
        }
        emit groupChatCreated(QStringLiteral("chat") + id);
    });
}

void CloudBackend::changeChatMember(const QString &dialog, const QString &user, bool add)
{
    const auto scope = portalDialog(dialog);
    const auto id = portalId(user);
    const auto op = QStringLiteral("chat-member:") + scope;
    if (!scope.startsWith(QLatin1String("chat")) || id.isEmpty()) {
        emit failed(op, QStringLiteral("Нужен групповой чат и числовой ID участника"));
        return;
    }
    const auto url = OriginGuard(m_session.origin).resolve(add ? QStringLiteral("/rest/im.chat.user.add.json")
                                                               : QStringLiteral("/rest/im.v2.Chat.deleteUser.json"));
    const auto fields = add ? Fields{{QStringLiteral("CHAT_ID"), scope.mid(4)},
                                     {QStringLiteral("USERS[0]"), id}, {QStringLiteral("HIDE_HISTORY"), QStringLiteral("Y")}}
                            : Fields{{QStringLiteral("dialogId"), scope}, {QStringLiteral("userId"), id}};
    webRequest(url, fields, op, QStringLiteral("result"), [this, scope, op, add](const QJsonValue &result) {
        if (!commandAccepted(result)) {
            emit failed(op, QStringLiteral("Портал отклонил изменение состава чата"));
            return;
        }
        refreshChangedChat(scope);
        emit chatActionSucceeded(scope, add ? QStringLiteral("Участник добавлен") : QStringLiteral("Участник удалён"));
    });
}

} // namespace bx
