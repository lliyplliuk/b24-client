#include "cloud/projects_service.h"
#include "cloud/cloud_rows.h"
#include "cloud/cloud_badge_data.h"

#include "core/origin_guard.h"
#include <algorithm>
#include <limits>
#include <utility>

namespace bx {
using namespace cloud_rows;

namespace {

using badge_data::maxBadgeEntries;
using badge_data::positiveId;
constexpr qsizetype projectUnreadBatchSize = 50;
constexpr qsizetype projectUnreadAttemptLimit = 100;
constexpr int projectTaskChatsPageSize = 50;

std::optional<qint64> nonnegativeId(const QJsonValue &value)
{
    bool parsed = true;
    const auto id = value.isString() ? value.toString().toLongLong(&parsed) : value.toInteger(-1);
    return parsed && id >= 0 ? std::optional<qint64>(id) : std::nullopt;
}

std::optional<qint64> matchingIds(const QJsonValue &first, const QJsonValue &second)
{
    if (first.isUndefined())
        return nonnegativeId(second);
    const auto id = nonnegativeId(first);
    return second.isUndefined() || id == nonnegativeId(second) ? id : std::nullopt;
}

} // namespace

void ProjectsService::requestProjectTaskChats(const QString &projectId, int offset)
{
    if (projectId.isEmpty()) {
        resetProjectTaskChats();
        return;
    }
    const auto operation = operationKey(OperationKind::ProjectTaskChats, projectId);
    if (!m_host.started() || numericId(projectId) != projectId || offset < 0
        || offset > std::numeric_limits<int>::max() - projectTaskChatsPageSize
        || (offset > 0 && (projectId != m_projectTaskChatsProject || offset != m_projectTaskChatsNext)))
        return m_host.failShape(operation);
    if (offset == 0) {
        resetProjectTaskChats();
        m_projectTaskChatsProject = projectId;
    } else if (m_portalHttp.contains(m_projectTaskChatsRequest)) {
        return;
    }
    const auto request = operation + QLatin1Char(':') + QString::number(++m_projectTaskChatsSerial);
    const auto bindingRevision = m_projectUnreadBindingRevision;
    m_projectTaskChatsRequest = request;
    Fields fields{{QStringLiteral("filter[GROUP_ID]"), projectId},
                  {QStringLiteral("order[ID]"), QStringLiteral("DESC")},
                  {QStringLiteral("start"), QString::number(offset)}};
    for (const auto *name : {"ID", "TITLE", "GROUP_ID", "CHAT_ID", "ACTIVITY_DATE"})
        fields.append({QStringLiteral("select[]"), QLatin1String(name)});
    const auto url = OriginGuard(m_host.portalOrigin()).resolve(QStringLiteral("/rest/tasks.task.list.json"));
    m_portalHttp.webRequest(url, fields, operation, QStringLiteral("result"),
        [this, projectId, offset, request, operation, bindingRevision](const QJsonValue &response) {
            if (m_projectTaskChatsRequest != request || m_host.authExpired())
                return;
            const auto result = response.toObject();
            const auto value = result.value(QStringLiteral("result")).toObject().value(QStringLiteral("tasks"));
            const auto rows = value.toArray();
            const auto cursor = result.value(QStringLiteral("next"));
            const auto next = cursor.isUndefined() || cursor.isNull() ? -1 : cursor.toInteger(-2);
            if (!value.isArray() || rows.size() > projectTaskChatsPageSize
                || (next != -1 && (next != offset + projectTaskChatsPageSize || rows.size() != projectTaskChatsPageSize)))
                return m_host.failShape(operation);
            QVector<Chat> chats;
            for (const auto &entry : rows) {
                const auto row = entry.toObject();
                const auto taskId = numericId(field(row, {"id", "ID"}));
                const auto chatId = explicitMessageChatId(row);
                if (!matchesTaskIdentity(row, taskId) || chatId <= 0
                    || m_projectTaskIds.contains(taskId) || m_projectTaskChatIds.contains(chatId))
                    continue;
                const auto group = row.value(QStringLiteral("group")).toObject();
                bool matched = false;
                for (const auto &candidate : {row.value("groupId"), row.value("GROUP_ID"), group.value("id"), group.value("ID")}) {
                    if (candidate.isUndefined())
                        continue;
                    matched = numericId(string(candidate)) == projectId;
                    if (!matched)
                        break;
                }
                if (!matched)
                    continue;
                m_projectTaskIds.insert(taskId);
                m_projectTaskChatIds.insert(chatId);
                if (bindingRevision == m_projectUnreadBindingRevision)
                    rememberProjectUnreadTask(chatId, taskId.toLongLong(), projectId.toLongLong());
                Chat chat;
                chat.id = chatDialog(QString::number(chatId));
                chat.serverChatId = chatId;
                chat.taskId = taskId;
                chat.taskChat = true;
                chat.kind = QStringLiteral("chat");
                chat.title = field(row, {"title", "TITLE"}).left(500);
                if (chat.title.isEmpty())
                    chat.title = QStringLiteral("Задача №%1").arg(taskId);
                chat.updated = date(oneOf(row, {"activityDate", "ACTIVITY_DATE"}));
                m_host.decorateProjectTaskChat(chat);
                chats.append(std::move(chat));
            }
            m_projectTaskChatsNext = static_cast<int>(next);
            publishProjectUnreadCounts();
            emit projectTaskChatsLoaded(projectId, offset, chats, m_projectTaskChatsNext);
        }, request, {}, true);
    m_host.loadChatBadges();
}

void ProjectsService::publishProjectUnreadCounts(bool force)
{
    if (!force && !m_host.projectsVisible())
        return;
    auto candidates = m_host.chatMessageCounters();
    for (const auto chat : std::as_const(m_projectUnreadNonTaskChats))
        candidates.remove(chat);
    QHash<QString, qint64> counts;
    bool complete = m_host.hasCounterSnapshot();
    for (auto it = candidates.cbegin(); it != candidates.cend(); ++it) {
        const auto direct = m_projectUnreadTasksByChat.value(it.key());
        const auto project = direct.project >= 0 ? direct.project
            : m_projectUnreadGroupsByParent.value(m_projectUnreadParentsByChat.value(it.key()), -1);
        if (project > 0)
            counts[QString::number(project)] += it.value();
        else if (project < 0)
            complete = false;
    }
    if (force || counts != m_projectUnreadCounts || complete != m_projectUnreadComplete) {
        m_projectUnreadCounts = std::move(counts);
        m_projectUnreadComplete = complete;
        emit projectUnreadCountsChanged(m_projectUnreadCounts, m_projectUnreadComplete);
    }
    if ((!complete || !m_projectUnreadMovedTasks.isEmpty()) && m_host.hasCounterSnapshot() && m_host.started()
        && m_host.projectsVisible() && !m_host.authExpired() && !m_projectUnreadPending)
        resolveProjectUnreadChats(candidates);
}

void ProjectsService::rememberProjectUnreadTask(qint64 chat, qint64 task, qint64 project)
{
    if (chat <= 0 || task <= 0 || project < -1 || m_projectUnreadNonTaskChats.contains(chat))
        return;
    if (m_projectUnreadTasksByChat.size() >= maxBadgeEntries && !m_projectUnreadTasksByChat.contains(chat))
        return;
    auto &known = m_projectUnreadTasksByChat[chat];
    if (known.task > 0 && known.task != task)
        return;
    known.task = task;
    if (project >= 0)
        known.project = project;
}

void ProjectsService::rememberProjectUnreadTaskChat(const Chat &chat)
{
    if (chat.taskChat && chat.serverChatId > 0 && chat.id == QStringLiteral("chat") + QString::number(chat.serverChatId))
        rememberProjectUnreadTask(chat.serverChatId, chat.taskId.toLongLong(), -1);
}

void ProjectsService::updateProjectUnreadTask(qint64 task, qint64 chat, qint64 project)
{
    if (task <= 0 || project < 0)
        return;
    ++m_projectUnreadBindingRevision;
    if (chat <= 0) {
        for (auto it = m_projectUnreadTasksByChat.cbegin(); it != m_projectUnreadTasksByChat.cend(); ++it) {
            if (it->task == task) {
                chat = it.key();
                break;
            }
        }
    }
    if (chat > 0) {
        m_projectUnreadMovedTasks.remove(task);
        rememberProjectUnreadTask(chat, task, project);
    } else if (m_host.hasCounterSnapshot() && (m_projectUnreadMovedTasks.contains(task)
               || (!m_projectUnreadMoveAttempts.contains(task)
                   && m_projectUnreadMoveAttempts.size() + m_projectUnreadMovedTasks.size() < projectUnreadBatchSize))) {
        m_projectUnreadMovedTasks.insert(task, project);
    }
    publishProjectUnreadCounts();
}

void ProjectsService::resolveProjectUnreadChats(const QHash<qint64, int> &candidates)
{
    if (!m_projectUnreadMovedTasks.isEmpty()) {
        QHash<qint64, qint64> changes;
        for (auto it = m_projectUnreadMovedTasks.cbegin(); it != m_projectUnreadMovedTasks.cend(); ++it) {
            if (!m_projectUnreadMoveAttempts.contains(it.key())) {
                m_projectUnreadMoveAttempts.insert(it.key());
                changes.insert(it.key(), 0);
            }
        }
        if (!changes.isEmpty()) {
            resolveProjectUnreadTasks(changes);
            return;
        }
    }
    auto ids = candidates.keys();
    std::sort(ids.begin(), ids.end());
    QSet<qint64> parents, directChats;
    QHash<qint64, qint64> tasks;
    for (const auto chat : ids) {
        if (parents.size() + directChats.size() + tasks.size() >= projectUnreadBatchSize
            || m_projectUnreadParentAttempts.size() + m_projectUnreadTaskChatAttempts.size() >= projectUnreadAttemptLimit)
            break;
        const auto known = m_projectUnreadTasksByChat.value(chat);
        if (known.project >= 0)
            continue;
        const auto parent = m_projectUnreadParentsByChat.value(chat);
        if (m_projectUnreadGroupsByParent.contains(parent) || parents.contains(parent))
            continue;
        if (parent > 0 && !m_projectUnreadParentAttempts.contains(parent)) {
            parents.insert(parent);
            m_projectUnreadParentAttempts.insert(parent);
        } else if (!m_projectUnreadTaskChatAttempts.contains(chat)) {
            m_projectUnreadTaskChatAttempts.insert(chat);
            if (known.task > 0)
                tasks.insert(known.task, chat);
            else
                directChats.insert(chat);
        }
    }
    if (parents.isEmpty() && directChats.isEmpty()) {
        if (!tasks.isEmpty())
            resolveProjectUnreadTasks(tasks);
        return;
    }
    const auto commandKey = [](qint64 chat) { return QStringLiteral("lookup_") + QString::number(chat); };
    Fields fields{{QStringLiteral("halt"), QStringLiteral("0")}};
    auto lookups = parents + directChats;
    for (const auto chat : std::as_const(lookups)) {
        fields.append({QStringLiteral("cmd[%1]").arg(commandKey(chat)),
                       QStringLiteral("im.dialog.get?DIALOG_ID=chat%1").arg(chat)});
    }
    const auto scope = m_host.accountScopeRevision();
    const auto operation = operationKey(OperationKind::ProjectUnreadDialogs);
    m_projectUnreadPending = true;
    const auto url = OriginGuard(m_host.portalOrigin()).resolve(QStringLiteral("/rest/batch.json"));
    m_portalHttp.webRequest(url, std::move(fields), operation, QStringLiteral("result"),
        [this, scope, parents, directChats, tasks = std::move(tasks), commandKey](QJsonValue response) mutable {
        if (scope != m_host.accountScopeRevision())
            return;
        m_projectUnreadPending = false;
        const auto rows = response.toObject().value(QStringLiteral("result")).toObject();
        const auto read = [&rows, &commandKey](qint64 id) {
            const auto row = rows.value(commandKey(id)).toObject();
            if (positiveId(row.value(QStringLiteral("id"))) != id
                || row.value(QStringLiteral("dialog_id")).toString() != QStringLiteral("chat") + QString::number(id))
                return QJsonObject{};
            return row;
        };
        for (const auto parent : parents) {
            const auto row = read(parent);
            const auto link = row.value(QStringLiteral("entity_link")).toObject();
            const auto project = positiveId(row.value(QStringLiteral("entity_id")));
            if (m_projectUnreadGroupsByParent.size() < maxBadgeEntries
                && row.value(QStringLiteral("type")) == QLatin1String("collab")
                && row.value(QStringLiteral("entity_type")) == QLatin1String("SONET_GROUP")
                && link.value(QStringLiteral("type")) == QLatin1String("SONET_GROUP")
                && project > 0 && positiveId(link.value(QStringLiteral("id"))) == project)
                m_projectUnreadGroupsByParent.insert(parent, project);
        }
        for (const auto chat : directChats) {
            const auto row = read(chat);
            const auto link = row.value(QStringLiteral("entity_link")).toObject();
            const auto type = row.value(QStringLiteral("entity_type"));
            const auto linkType = link.value(QStringLiteral("type")).toString();
            const auto task = positiveId(row.value(QStringLiteral("entity_id")));
            if ((type == QLatin1String("TASKS_TASK") || type == QLatin1String("TASKS"))
                && (linkType.isEmpty() || linkType == QLatin1String("TASKS_TASK") || linkType == QLatin1String("TASKS"))
                && task > 0 && positiveId(link.value(QStringLiteral("id"))) == task) {
                rememberProjectUnreadTask(chat, task, -1);
                if (m_projectUnreadTasksByChat.value(chat).task == task)
                    tasks.insert(task, chat);
            } else if (type == QLatin1String("SONET_GROUP") && !m_projectUnreadParentsByChat.contains(chat)
                       && !m_projectUnreadTasksByChat.contains(chat)) {
                // A typeless counter can only be excluded after its exact chat metadata is seen.
                m_projectUnreadNonTaskChats.insert(chat);
            }
        }
        if (!tasks.isEmpty() && m_host.projectsVisible() && !m_host.authExpired())
            resolveProjectUnreadTasks(tasks);
        publishProjectUnreadCounts();
    }, operation + QLatin1Char(':') + QString::number(scope));
}

void ProjectsService::resolveProjectUnreadTasks(const QHash<qint64, qint64> &tasks)
{
    Fields fields;
    int index = 0;
    for (auto it = tasks.cbegin(); it != tasks.cend(); ++it)
        fields.append({QStringLiteral("filter[ID][%1]").arg(index++), QString::number(it.key())});
    for (const auto *name : {"ID", "GROUP_ID", "CHAT_ID"})
        fields.append({QStringLiteral("select[]"), QLatin1String(name)});
    const auto scope = m_host.accountScopeRevision();
    const auto operation = operationKey(OperationKind::ProjectUnreadTasks);
    m_projectUnreadPending = true;
    const auto url = OriginGuard(m_host.portalOrigin()).resolve(QStringLiteral("/rest/tasks.task.list.json"));
    m_portalHttp.webRequest(url, std::move(fields), operation, QStringLiteral("result"), [this, scope, tasks](QJsonValue response) {
        if (scope != m_host.accountScopeRevision())
            return;
        m_projectUnreadPending = false;
        const auto rows = response.toObject().value(QStringLiteral("tasks")).toArray();
        if (rows.size() <= projectUnreadBatchSize) {
            for (const auto &value : rows) {
                const auto row = value.toObject();
                const auto task = matchingIds(row.value(QStringLiteral("id")), row.value(QStringLiteral("ID")));
                const auto chat = matchingIds(row.value(QStringLiteral("chatId")), row.value(QStringLiteral("CHAT_ID")));
                const auto group = matchingIds(row.value(QStringLiteral("groupId")), row.value(QStringLiteral("GROUP_ID")));
                const auto groupObject = row.value(QStringLiteral("group")).toObject();
                const auto nestedGroup = matchingIds(groupObject.value(QStringLiteral("id")), groupObject.value(QStringLiteral("ID")));
                if (!task || !chat || *task <= 0 || *chat <= 0 || !tasks.contains(*task)
                    || !group || (!groupObject.isEmpty() && group != nestedGroup))
                    continue;
                const auto expected = tasks.value(*task);
                const auto known = m_projectUnreadTasksByChat.value(*chat);
                if (expected == 0 && known.project < 0
                    && (m_projectUnreadParentsByChat.contains(*chat) || known.task == *task)) {
                    const auto move = m_projectUnreadMovedTasks.constFind(*task);
                    if (move != m_projectUnreadMovedTasks.cend())
                        rememberProjectUnreadTask(*chat, *task, move.value());
                } else if (expected == *chat && known.project < 0) {
                    rememberProjectUnreadTask(*chat, *task, *group);
                }
            }
        }
        for (auto it = tasks.cbegin(); it != tasks.cend(); ++it) {
            if (it.value() == 0)
                m_projectUnreadMovedTasks.remove(it.key());
        }
        publishProjectUnreadCounts();
    }, operation + QLatin1Char(':') + QString::number(scope));
}

void ProjectsService::resetProjectTaskChats()
{
    m_projectTaskChatsProject.clear();
    m_projectTaskChatsRequest.clear();
    m_projectTaskChatsNext = -1;
    m_projectTaskIds.clear();
    m_projectTaskChatIds.clear();
}

void ProjectsService::publishProjectTaskChatBadges(qint64 chatId)
{
    if (m_projectTaskChatsProject.isEmpty() || m_projectTaskChatIds.isEmpty()
        || (chatId > 0 && !m_projectTaskChatIds.contains(chatId)))
        return;
    QVector<Chat> badges;
    const auto append = [this, &badges](qint64 id) {
        Chat chat;
        chat.id = QStringLiteral("chat") + QString::number(id);
        chat.serverChatId = id;
        m_host.decorateProjectTaskChat(chat);
        badges.append(std::move(chat));
    };
    if (chatId > 0)
        append(chatId);
    else
        for (auto id : std::as_const(m_projectTaskChatIds))
            append(id);
    emit projectTaskChatBadgesChanged(m_projectTaskChatsProject, badges);
}

void ProjectsService::setChatClassification(QHash<qint64, qint64> taskParents, QSet<qint64> nonTaskChats)
{
    m_projectUnreadParentsByChat = std::move(taskParents);
    m_projectUnreadNonTaskChats = std::move(nonTaskChats);
}

void ProjectsService::refreshUnreadCounts()
{
    m_projectUnreadParentAttempts.clear();
    m_projectUnreadTaskChatAttempts.clear();
    m_projectUnreadMoveAttempts.clear();
    publishProjectUnreadCounts(true);
}

bool ProjectsService::containsChat(qint64 chat) const
{
    return m_projectTaskChatIds.contains(chat) || hasTaskParent(chat) || m_projectUnreadTasksByChat.contains(chat);
}

} // namespace bx
