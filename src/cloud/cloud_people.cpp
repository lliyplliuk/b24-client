#include "cloud/cloud_backend.h"

#include "core/notification_targets.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QUrlQuery>

namespace bx {
namespace {

using namespace Qt::StringLiterals;

QString idFromJson(const QJsonValue &value)
{
    if (value.isString())
        return portalTaskIdentifier(value.toString());
    const auto number = value.toInteger();
    return number > 0 ? QString::number(number) : QString{};
}

QString label(const QJsonValue &value)
{
    return value.isObject() ? value.toObject().value(u"text"_s).toString() : value.toString();
}

QString personName(const QJsonObject &user)
{
    auto name = user.value(u"name"_s).toString();
    if (name.isEmpty()) {
        const auto first = user.value(u"first_name"_s).toString(user.value(u"firstName"_s).toString());
        const auto last = user.value(u"last_name"_s).toString(user.value(u"lastName"_s).toString());
        name = first + u' ' + last;
    }
    return name.simplified().left(96);
}

constexpr int peopleLimit = 20;

QJsonArray recentTaskPeople(const QJsonObject &dialog)
{
    constexpr qsizetype scanLimit = 256;
    const auto items = dialog.value(u"items"_s).toArray();
    QHash<QString, QJsonObject> directory;
    QJsonArray fallback;
    for (qsizetype i = 0; i < items.size() && i < scanLimit; ++i) {
        const auto row = items.at(i).toObject();
        const auto id = idFromJson(row.value(u"id"_s));
        if (row.value(u"entityId"_s).toString() == u"user" && !id.isEmpty() && !directory.contains(id)) {
            directory.insert(id, row);
            fallback.append(row);
        }
    }

    const auto recent = dialog.value(u"recentItems"_s).toArray();
    QJsonArray people;
    for (qsizetype i = 0; i < recent.size() && i < scanLimit; ++i) {
        const auto key = recent.at(i).toArray();
        if (key.size() != 2 || key.first().toString() != u"user")
            continue;
        const auto row = directory.take(idFromJson(key.at(1)));
        if (!row.isEmpty())
            people.append(row);
    }
    return people.isEmpty() ? fallback : people;
}

} // namespace

void CloudBackend::searchPeople(const QString &requestId, const QString &query, const QString &dialogId)
{
    const auto trimmed = query.trimmed();
    const auto groupId = dialogId.startsWith(u"chat") ? portalTaskIdentifier(dialogId.mid(4)) : QString{};
    const auto userId = portalTaskIdentifier(dialogId);
    if (requestId.isEmpty() || requestId.size() > 96 || trimmed.size() > 120 ||
        (!dialogId.isEmpty() && groupId.isEmpty() && userId.isEmpty())) {
        emit peopleFound(requestId, {});
        return;
    }
    const auto operation = u"search-people:"_s + requestId;
    const bool mention = !dialogId.isEmpty();
    const auto deliver = [this, requestId, scope = m_accountScopeRevision]
        (const QJsonArray &rows, bool selector, bool mention) {
        if (scope != m_accountScopeRevision)
            return;
        QVector<Person> people;
        QSet<QString> seen;
        for (const auto &value : rows) {
            const auto row = value.toObject();
            const auto id = idFromJson(row.value(u"id"_s));
            if (id.isEmpty() || seen.contains(id))
                continue;
            if (selector) {
                const auto entity = row.value(u"entityId"_s).toString();
                const auto type = row.value(u"entityType"_s).toString();
                if ((!mention && entity != u"user") || (mention && type != u"im-user"))
                    continue;
            }
            const auto user = selector ? row.value(u"customData"_s).toObject().value(u"user"_s).toObject() : row;
            const auto nestedId = user.value(u"id"_s);
            if (selector && mention && !nestedId.isUndefined() && idFromJson(nestedId) != id)
                continue;
            auto name = personName(user);
            if (name.isEmpty())
                name = label(row.value(u"title"_s)).simplified().left(96);
            if (name.isEmpty())
                continue;
            auto position = user.value(u"workPosition"_s).toString().simplified();
            if (position.isEmpty())
                position = user.value(u"work_position"_s).toString().simplified();
            if (position.isEmpty())
                position = label(row.value(u"subtitle"_s)).simplified();
            auto avatar = avatarUrlFromPortal(user.value(u"avatar"_s).toString());
            if (avatar.isEmpty())
                avatar = avatarUrlFromPortal(user.value(u"avatarUrl"_s).toString());
            if (selector && avatar.isEmpty())
                avatar = avatarUrlFromPortal(row.value(u"avatar"_s).toString());
            people.append({id, name, position.left(160), avatar});
            seen.insert(id);
            if (people.size() == peopleLimit)
                break;
        }
        emit peopleFound(requestId, people);
    };

    if (trimmed.isEmpty() && mention) {
        const auto dialog = groupId.isEmpty() ? userId : u"chat"_s + groupId;
        ajax(u"im.v2.Chat.Mention.list"_s,
             {{u"dialogId"_s, dialog}, {u"order[lastSendMessageId]"_s, u"desc"_s}, {u"limit"_s, u"50"_s}},
             operation, [deliver](QJsonValue data) {
            deliver(data.toObject().value(u"users"_s).toArray(), false, true);
        });
        return;
    }

    const auto selectorId = mention ? u"im-chat-search"_s : requestId;
    const auto context = mention ? u"IM_CHAT_SEARCH"_s : u"tasks-card"_s;
    QJsonObject options;
    if (mention) {
        const qint64 chat = groupId.isEmpty() ? m_dialogChatIds.value(userId) : groupId.toLongLong();
        options = {{u"exclude"_s, QJsonArray{u"chats"_s}}, {u"contextChatId"_s, chat}, {u"mentionSearch"_s, true}};
    } else {
        options = {{u"emailUsers"_s, true}, {u"inviteGuestLink"_s, true}, {u"analyticsSource"_s, u"tasks"_s}};
    }
    const QJsonObject entity{{u"id"_s, mention ? u"im-recent-v2"_s : u"user"_s}, {u"dynamicLoad"_s, true},
        {u"dynamicSearch"_s, true}, {u"options"_s, options}};
    QJsonArray entities{entity};
    if (!mention)
        entities.append(QJsonObject{{u"id"_s, u"department"_s}});
    QJsonObject dialog{{u"entities"_s, entities}, {u"preselectedItems"_s, QJsonArray{}},
        {u"clearUnavailableItems"_s, false}, {u"context"_s, context}, {u"id"_s, selectorId}};
    auto url = actionUrl(trimmed.isEmpty() ? u"ui.entityselector.load"_s : u"ui.entityselector.doSearch"_s);
    if (!mention) {
        QUrlQuery parameters(url);
        parameters.addQueryItem(u"context"_s, context);
        url.setQuery(parameters);
    }
    if (trimmed.isEmpty()) {
        dialog.insert(u"recentItemsLimit"_s, peopleLimit);
        ajaxJson(url, {{u"dialog"_s, dialog}}, operation,
                Handler::checked(u"Не удалось загрузить недавних сотрудников."_s,
                [](const QJsonValue &data) { return data.toObject().value(u"dialog"_s).isObject(); },
                [deliver](QJsonValue data) {
            deliver(recentTaskPeople(data.toObject().value(u"dialog"_s).toObject()), true, false);
        }));
        return;
    }

    static const QRegularExpression separator(uR"([\s()[\]{}<>#№"'-]+)"_s);
    QJsonArray words;
    for (const auto &word : trimmed.split(separator, Qt::SkipEmptyParts))
        words.append(word);
    QJsonObject search{{u"queryWords"_s, words}, {u"query"_s, trimmed}};
    if (!mention)
        search.insert(u"dynamicSearchEntities"_s, QJsonArray{});
    ajaxJson(url, {{u"dialog"_s, dialog}, {u"searchQuery"_s, search}}, operation,
             [deliver, mention](QJsonValue data) {
        const auto items = data.toObject().value(u"dialog"_s).toObject().value(u"items"_s).toArray();
        deliver(items, true, mention);
    });
}

} // namespace bx
