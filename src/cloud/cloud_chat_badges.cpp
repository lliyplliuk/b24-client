#include "cloud_backend.h"

#include "core/origin_guard.h"
#include "core/tunables.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUuid>

#include <algorithm>
#include <limits>
#include <utility>

namespace bx {
namespace {

constexpr qint64 badgeRefreshIntervalMs = 15 * 1000;
constexpr qsizetype maxBadgeEntries = 4096;
constexpr qsizetype maxPendingAnchorEvents = 64;
constexpr qsizetype maxVisibleReadIds = 25;
constexpr qsizetype projectUnreadBatchSize = 50;
constexpr qsizetype projectUnreadAttemptLimit = 100;
const QString snapshotOperation = operationKey(OperationKind::ChatBadgesSnapshot);

int visibleUnreadCount(int unread, bool marked)
{
    return std::max(unread, static_cast<int>(marked));
}

qint64 positiveId(const QJsonValue &value)
{
    bool ok = false;
    const auto id = value.isString() ? value.toString().toLongLong(&ok) : value.toInteger();
    return id > 0 && (!value.isString() || ok) ? id : 0;
}

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

QJsonObject launchConfig(QStringView source)
{
    static const QRegularExpression launcher(
        QStringLiteral(R"(^\s*BX\.Messenger\.v2\.Application\.Launch\(\s*(['"])(messenger|quickAccess)\1\s*,\s*\{)"),
        QRegularExpression::MultilineOption);
    const auto candidate = launcher.match(source.toString());
    if (!candidate.hasMatch())
        return {};
    const auto start = candidate.capturedEnd() - 1;
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (auto end = start; end < source.size(); ++end) {
        const auto ch = source.at(end);
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (ch == QLatin1Char('\\'))
                escaped = true;
            else if (ch == QLatin1Char('"'))
                quoted = false;
        } else if (ch == QLatin1Char('"')) {
            quoted = true;
        } else if (ch == QLatin1Char('{')) {
            ++depth;
        } else if (ch == QLatin1Char('}') && --depth == 0) {
            if (source.mid(end + 1).trimmed().startsWith(QLatin1Char(')')))
                return QJsonDocument::fromJson(source.mid(start, end - start + 1).toUtf8()).object();
            return {};
        }
    }
    return {};
}

QJsonObject messengerConfig(const QString &html)
{
    // Each match consumes the entire tag, including quoted attributes. A launch inside an
    // attribute or HTML comment cannot be a script. Script content is consumed as a single token.
    static const QRegularExpression tokens(
        QStringLiteral(R"regex(<!--[\s\S]*?-->|<(script|style|textarea|title)(?:\s(?:[^>"']|"[^"]*"|'[^']*')*)?>([\s\S]*?)</\1\s*>|<(?=[!/a-zA-Z])(?:[^>"']|"[^"]*"|'[^']*')*>)regex"),
        QRegularExpression::CaseInsensitiveOption);
    QJsonObject snapshot;
    auto next = tokens.globalMatch(html);
    while (next.hasNext()) {
        const auto tag = next.next();
        if (tag.captured(1).compare(QLatin1String("script"), Qt::CaseInsensitive) != 0)
            continue;
        const auto config = launchConfig(QStringView(html).mid(tag.capturedStart(2), tag.capturedLength(2)));
        for (const auto *key : {"counters", "anchors"}) {
            const auto value = config.value(QLatin1String(key));
            if (value.isArray())
                snapshot.insert(QLatin1String(key), value);
        }
        const auto status = config.value(QStringLiteral("settings")).toObject().value(QStringLiteral("status"));
        if (status.isString())
            snapshot.insert(QStringLiteral("portalStatus"), status);
        const auto user = config.value(QStringLiteral("currentUser")).toObject();
        const auto id = positiveId(user.value(QStringLiteral("id")));
        if (id > 0)
            snapshot.insert(QStringLiteral("currentUserId"), id);
        if (snapshot.contains(QStringLiteral("counters")) && snapshot.contains(QStringLiteral("anchors"))
            && snapshot.contains(QStringLiteral("currentUserId")))
            break;
    }
    return snapshot;
}

} // namespace

void CloudBackend::loadChatBadges()
{
    if (!m_started || m_authExpired || m_badgePagePending || m_portalHttp.contains(snapshotOperation)
        || (m_chatBadgeRefresh.isValid() && !m_chatBadgeRefresh.hasExpired(badgeRefreshIntervalMs)))
        return;
    const auto revision = ++m_chatBadgeRevision;
    const auto scope = m_accountScopeRevision;
    const auto statusRevision = m_portalStatusRevision;
    const auto anchorRevision = m_anchorRevision;
    m_badgePagePending = true;
    m_badgePageOvertaken = false;
    m_chatBadgeRefresh.start();
    const auto url = OriginGuard(m_session.origin).resolve(QStringLiteral("/online/"));
    fetchPortalHtml(url, [this, revision, scope, statusRevision, anchorRevision](const QString &html) {
        if (scope != m_accountScopeRevision)
            return;
        m_badgePagePending = false;
        auto pending = std::exchange(m_pendingAnchorEvents, {});
        const auto config = messengerConfig(html);
        const auto userId = positiveId(config.value(QStringLiteral("currentUserId")));
        if (m_userId.isEmpty() && userId > 0)
            rememberCurrentUser(QString::number(userId));
        if (userId > 0 && QString::number(userId) != m_userId)
            return;
        if (statusRevision == m_portalStatusRevision)
            rememberPortalStatus(config.value(QStringLiteral("portalStatus")).toString());
        loadInitialPortalStatus();
        const auto anchors = config.value(QStringLiteral("anchors"));
        if (!m_badgePageOvertaken && anchors.isArray() && anchors.toArray().size() <= maxBadgeEntries) {
            m_chatAnchors.clear();
            m_chatAnchorCount = 0;
            for (const auto &anchor : anchors.toArray())
                updateAnchors(QStringLiteral("addAnchor"), anchor.toObject(), anchorRevision);
            for (const auto &[command, params, observed] : std::as_const(pending))
                updateAnchors(command, params, observed);
            republishChatBadges();
            notifyAnchorAvailable();
        }
        const auto counters = config.value(QStringLiteral("counters"));
        if (counters.isArray()) {
            applyChatCounterSnapshot(counters.toArray(), revision);
        } else {
            ajax(QStringLiteral("im.v2.Counter.get"), {}, snapshotOperation, [this, revision, scope](const QJsonValue &data) {
                if (scope != m_accountScopeRevision)
                    return;
                const auto rows = data.toObject().value(QStringLiteral("userCounters"));
                if (rows.isArray())
                    applyChatCounterSnapshot(rows.toArray(), revision);
            });
        }
    });
}

void CloudBackend::applyChatCounterSnapshot(const QJsonArray &rows, quint64 revision)
{
    if (rows.size() > maxBadgeEntries)
        return;
    QHash<qint64, ChatCounterBadge> counters;
    QHash<qint64, qint64> taskParents;
    QSet<qint64> nonTaskChats;
    for (const auto &value : rows) {
        const auto row = value.toObject();
        const auto id = positiveId(row.value(QStringLiteral("chatId")));
        const auto count = row.value(QStringLiteral("counter")).toInteger(-1);
        if (id > 0 && count >= 0 && count <= std::numeric_limits<int>::max()) {
            const auto displayCount = count == 0 && row.value(QStringLiteral("isNonAnswered")).toBool() ? 1 : int(count);
            counters.insert(id, {displayCount, row.value(QStringLiteral("isMarkedAsUnread")).toBool(), int(count)});
            const auto type = row.value(QStringLiteral("type")).toString();
            const auto sections = row.value(QStringLiteral("recentSections")).toArray();
            if (type == QLatin1String("tasksTask") || (type.isEmpty() && sections.contains(QStringLiteral("tasksTask"))))
                taskParents.insert(id, positiveId(row.value(QStringLiteral("parentChatId"))));
            else if (!type.isEmpty() || !sections.isEmpty())
                nonTaskChats.insert(id);
        }
    }
    m_haveCounterSnapshot = true;
    m_chatCounterSnapshotRevision = revision;
    m_chatCounterSnapshotTotal = 0;
    for (const auto &badge : std::as_const(counters))
        m_chatCounterSnapshotTotal += visibleUnreadCount(badge.unread, badge.marked);
    m_chatCounterBadges = std::move(counters);
    m_projectUnreadParentsByChat = std::move(taskParents);
    m_projectUnreadNonTaskChats = std::move(nonTaskChats);
    for (auto it = m_recentOverlays.begin(); it != m_recentOverlays.end(); ++it) {
        if (it->counterRevision <= revision) {
            it->counter.reset();
            it->markedUnread.reset();
        }
    }
    publishUnreadChatCount();
    republishChatBadges();
}

void CloudBackend::publishUnreadChatCount()
{
    // A negative key represents a personal dialog whose portal chat ID is still unknown.
    const auto keyFor = [this](const QString &dialog, qint64 chatId) {
        if (chatId <= 0)
            chatId = badgeChatId(dialog);
        return chatId > 0 ? chatId : -std::max<qint64>(0, dialog.toLongLong());
    };
    struct Entry {
        ChatCounterBadge baseline;
        std::optional<int> counter;
        std::optional<bool> marked;
        quint64 counterRevision = 0;
        quint64 markedRevision = 0;
    };
    QHash<qint64, Entry> changes;
    if (!m_haveCounterSnapshot) {
        for (const auto *rows : {&m_recentChatItems, &m_recentTaskChatItems, &m_legacyTaskChats}) {
            for (const auto &chat : *rows) {
                const auto key = keyFor(chat.id, chat.serverChatId);
                if (key == 0 || (chat.unread <= 0 && !chat.markedUnread))
                    continue;
                auto &base = changes[key].baseline;
                base.unread = std::max(base.unread, chat.unread);
                base.marked |= chat.markedUnread;
            }
        }
    }
    for (auto it = m_recentOverlays.cbegin(); it != m_recentOverlays.cend(); ++it) {
        if (!it->counter && !it->markedUnread)
            continue;
        const auto key = keyFor(it.key(), it->chatId);
        if (key == 0 || (m_haveCounterSnapshot && key < 0))
            continue;
        auto &change = changes[key];
        if (m_haveCounterSnapshot)
            change.baseline = m_chatCounterBadges.value(key);
        if (it->counter && it->counterRevision >= change.counterRevision) {
            change.counter = it->counter;
            change.counterRevision = it->counterRevision;
        }
        if (it->markedUnread && it->counterRevision >= change.markedRevision) {
            change.marked = it->markedUnread;
            change.markedRevision = it->counterRevision;
        }
    }
    qint64 total = m_chatCounterSnapshotTotal;
    for (const auto &change : std::as_const(changes)) {
        if (m_haveCounterSnapshot)
            total -= visibleUnreadCount(change.baseline.unread, change.baseline.marked);
        total += visibleUnreadCount(change.counter.value_or(change.baseline.unread),
                                    change.marked.value_or(change.baseline.marked));
    }
    if (m_unreadChatCount != total) {
        m_unreadChatCount = total;
        emit unreadChatCountChanged(total);
    }
    publishProjectUnreadCounts();
}

QHash<qint64, int> CloudBackend::projectUnreadCandidates() const
{
    QHash<qint64, int> candidates;
    for (auto it = m_chatCounterBadges.cbegin(); it != m_chatCounterBadges.cend(); ++it) {
        if (it->messageCount > 0 && !m_projectUnreadNonTaskChats.contains(it.key()))
            candidates.insert(it.key(), it->messageCount);
    }
    for (auto it = m_recentOverlays.cbegin(); it != m_recentOverlays.cend(); ++it) {
        if (!it->counter || it->chatId <= 0 || it.key() != QStringLiteral("chat") + QString::number(it->chatId)
            || m_projectUnreadNonTaskChats.contains(it->chatId))
            continue;
        if (*it->counter > 0)
            candidates.insert(it->chatId, *it->counter);
        else
            candidates.remove(it->chatId);
    }
    return candidates;
}

void CloudBackend::publishProjectUnreadCounts(bool force)
{
    if (!force && (m_activeSection != 1 || !m_foreground))
        return;
    const auto candidates = projectUnreadCandidates();
    QHash<QString, qint64> counts;
    bool complete = m_haveCounterSnapshot;
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
    if ((!complete || !m_projectUnreadMovedTasks.isEmpty()) && m_haveCounterSnapshot && m_started && m_foreground && !m_authExpired
        && m_activeSection == 1 && !m_projectUnreadPending)
        resolveProjectUnreadChats(candidates);
}

void CloudBackend::rememberProjectUnreadTask(qint64 chat, qint64 task, qint64 project)
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

void CloudBackend::rememberProjectUnreadTaskChat(const Chat &chat)
{
    if (chat.taskChat && chat.serverChatId > 0 && chat.id == QStringLiteral("chat") + QString::number(chat.serverChatId))
        rememberProjectUnreadTask(chat.serverChatId, chat.taskId.toLongLong(), -1);
}

void CloudBackend::updateProjectUnreadTask(qint64 task, qint64 chat, qint64 project)
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
    } else if (m_haveCounterSnapshot && (m_projectUnreadMovedTasks.contains(task)
               || (!m_projectUnreadMoveAttempts.contains(task)
                   && m_projectUnreadMoveAttempts.size() + m_projectUnreadMovedTasks.size() < projectUnreadBatchSize))) {
        m_projectUnreadMovedTasks.insert(task, project);
    }
    publishProjectUnreadCounts();
}

void CloudBackend::resolveProjectUnreadChats(const QHash<qint64, int> &candidates)
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
    const auto scope = m_accountScopeRevision;
    const auto operation = operationKey(OperationKind::ProjectUnreadDialogs);
    m_projectUnreadPending = true;
    const auto url = OriginGuard(m_session.origin).resolve(QStringLiteral("/rest/batch.json"));
    m_portalHttp.webRequest(url, std::move(fields), operation, QStringLiteral("result"),
        [this, scope, parents, directChats, tasks = std::move(tasks), commandKey](QJsonValue response) mutable {
        if (scope != m_accountScopeRevision)
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
        if (!tasks.isEmpty() && m_activeSection == 1 && m_foreground && !m_authExpired)
            resolveProjectUnreadTasks(tasks);
        publishProjectUnreadCounts();
    }, operation + QLatin1Char(':') + QString::number(scope));
}

void CloudBackend::resolveProjectUnreadTasks(const QHash<qint64, qint64> &tasks)
{
    Fields fields;
    int index = 0;
    for (auto it = tasks.cbegin(); it != tasks.cend(); ++it)
        fields.append({QStringLiteral("filter[ID][%1]").arg(index++), QString::number(it.key())});
    for (const auto *name : {"ID", "GROUP_ID", "CHAT_ID"})
        fields.append({QStringLiteral("select[]"), QLatin1String(name)});
    const auto scope = m_accountScopeRevision;
    const auto operation = operationKey(OperationKind::ProjectUnreadTasks);
    m_projectUnreadPending = true;
    const auto url = OriginGuard(m_session.origin).resolve(QStringLiteral("/rest/tasks.task.list.json"));
    m_portalHttp.webRequest(url, std::move(fields), operation, QStringLiteral("result"), [this, scope, tasks](QJsonValue response) {
        if (scope != m_accountScopeRevision)
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

void CloudBackend::resetChatCounters()
{
    resetProjectTaskChats();
    ++m_recentStatusGeneration;
    m_chatBadgeRevision = 0;
    m_chatCounterSnapshotRevision = 0;
    m_badgePagePending = false;
    m_badgePageOvertaken = false;
    m_chatBadgeRefresh.invalidate();
    m_haveCounterSnapshot = false;
    m_chatCounterSnapshotTotal = 0;
    m_chatCounterBadges.clear();
    m_projectUnreadParentsByChat.clear();
    m_projectUnreadNonTaskChats.clear();
    m_projectUnreadTasksByChat.clear();
    m_projectUnreadGroupsByParent.clear();
    m_projectUnreadParentAttempts.clear();
    m_projectUnreadTaskChatAttempts.clear();
    m_projectUnreadMovedTasks.clear();
    m_projectUnreadMoveAttempts.clear();
    m_projectUnreadPending = false;
    m_recentOverlays.clear();
    m_recentChatItems.clear();
    m_recentTaskChatItems.clear();
    m_legacyTaskChats.clear();
    if (!m_projectUnreadCounts.isEmpty() || m_projectUnreadComplete) {
        m_projectUnreadCounts.clear();
        m_projectUnreadComplete = false;
        emit projectUnreadCountsChanged({}, false);
    }
    publishUnreadChatCount();
}

qint64 CloudBackend::badgeChatId(const QString &dialog) const
{
    const auto id = m_dialogChatIds.value(dialog);
    if (id > 0)
        return id;
    if (dialog.startsWith(QLatin1String("chat")))
        return std::max<qint64>(0, dialog.sliced(4).toLongLong());
    return 0;
}

void CloudBackend::decorateChatBadges(Chat &chat) const
{
    const auto id = chat.serverChatId > 0 ? chat.serverChatId : badgeChatId(chat.id);
    if (id <= 0)
        return;
    if (m_haveCounterSnapshot) {
        const auto badge = m_chatCounterBadges.value(id);
        chat.unread = badge.unread;
        chat.markedUnread = badge.marked;
    }
    chat.hasMention = false;
    chat.hasReaction = false;
    const auto anchors = m_chatAnchors.constFind(id);
    if (anchors == m_chatAnchors.cend())
        return;
    for (const auto &anchor : *anchors) {
        chat.hasMention |= !anchor.reaction;
        chat.hasReaction |= anchor.reaction;
        if (chat.hasMention && chat.hasReaction)
            break;
    }
}

bool CloudBackend::updateAnchors(const QString &command, const QJsonObject &params, quint64 revision)
{
    const auto id = positiveId(params.value(QStringLiteral("chatId")));
    const auto clearChat = [this](qint64 chatId) {
        auto it = m_chatAnchors.find(chatId);
        if (it == m_chatAnchors.end())
            return false;
        m_chatAnchorCount -= it->size();
        m_chatAnchors.erase(it);
        return true;
    };
    if (command == QLatin1String("deleteAnchors")) {
        bool changed = false;
        for (const auto &value : params.value(QStringLiteral("chatIds")).toArray())
            changed |= clearChat(positiveId(value));
        return changed;
    }
    if (command == QLatin1String("deleteChatAnchors"))
        return clearChat(id);
    const auto message = positiveId(params.value(QStringLiteral("messageId")));
    const auto type = params.value(QStringLiteral("type")).toString().toLower();
    if (id == 0 || message == 0 || (type != QLatin1String("mention") && type != QLatin1String("reaction")))
        return false;
    // The IM anchor commands carry the receiving user's own marks. fromUserId identifies the actor.
    const auto recipient = positiveId(params.value(QStringLiteral("userId")));
    if (recipient > 0 && QString::number(recipient) != m_userId)
        return false;
    const auto actor = positiveId(params.value(QStringLiteral("fromUserId")));
    if (actor > 0 && QString::number(actor) == m_userId)
        return false;
    const ChatBadgeAnchor anchor{message, actor, type == QLatin1String("reaction"), revision};
    if (command == QLatin1String("addAnchor")) {
        if (m_chatAnchorCount >= maxBadgeEntries && !m_chatAnchors.contains(id))
            return false;
        auto &items = m_chatAnchors[id];
        auto existing = std::find(items.begin(), items.end(), anchor);
        if (existing != items.end()) {
            if (existing->revision >= revision)
                return false;
            existing->revision = revision;
            return true;
        }
        if (m_chatAnchorCount >= maxBadgeEntries)
            return false;
        items.append(anchor);
        ++m_chatAnchorCount;
        return true;
    }
    auto items = m_chatAnchors.find(id);
    if (items == m_chatAnchors.end())
        return false;
    const auto removed = items->removeAll(anchor);
    m_chatAnchorCount -= removed;
    if (items->isEmpty())
        m_chatAnchors.erase(items);
    return removed > 0;
}

bool CloudBackend::handleAnchorPull(const QString &command, const QJsonObject &params)
{
    if (command != QLatin1String("addAnchor") && command != QLatin1String("deleteAnchor")
        && command != QLatin1String("deleteAnchors") && command != QLatin1String("deleteChatAnchors"))
        return false;
    const auto revision = ++m_anchorRevision;
    if (m_badgePagePending) {
        if (m_pendingAnchorEvents.size() >= maxPendingAnchorEvents) {
            m_badgePageOvertaken = true;
        } else {
            QJsonObject delta;
            for (const auto *field : {"chatId", "messageId", "userId", "fromUserId"})
                delta.insert(QLatin1String(field), positiveId(params.value(QLatin1String(field))));
            const auto type = params.value(QStringLiteral("type")).toString();
            if (type.size() <= 8)
                delta.insert(QStringLiteral("type"), type);
            const auto chatIds = params.value(QStringLiteral("chatIds")).toArray();
            if (chatIds.size() > maxBadgeEntries) {
                m_badgePageOvertaken = true;
            } else {
                QJsonArray ids;
                for (const auto &value : chatIds)
                    ids.append(positiveId(value));
                delta.insert(QStringLiteral("chatIds"), ids);
                m_pendingAnchorEvents.append({command, std::move(delta), revision});
            }
        }
    }
    if (updateAnchors(command, params, revision)) {
        const auto chatId = positiveId(params.value(QStringLiteral("chatId")));
        republishChatBadges(chatId);
        if (command == QLatin1String("addAnchor"))
            notifyAnchorAvailable(chatId);
    }
    return true;
}

void CloudBackend::notifyAnchorAvailable(qint64 chatId)
{
    const auto ordinary = m_activeSection == 0 ? m_openDialog : QString{};
    const auto task = m_taskDialogs.value(m_openTask);
    for (const auto &dialog : {ordinary, task}) {
        if (dialog.isEmpty())
            continue;
        const auto id = badgeChatId(dialog);
        if ((chatId == 0 || id == chatId) && m_chatAnchors.contains(id))
            emit chatBadgeAnchorAvailable(dialog);
        if (ordinary == task)
            break;
    }
}

void CloudBackend::republishChatBadges(qint64 changedChatId)
{
    const auto affected = [this, changedChatId](const QVector<Chat> &rows) {
        return !rows.isEmpty() && (changedChatId == 0 || std::any_of(rows.cbegin(), rows.cend(),
            [this, changedChatId](const Chat &row) {
                return (row.serverChatId > 0 ? row.serverChatId : badgeChatId(row.id)) == changedChatId;
            }));
    };
    if (affected(m_recentChatItems))
        publishRecentChats(m_recentChatItems, false);
    if (affected(m_recentTaskChatItems))
        publishRecentChats(m_recentTaskChatItems, true);
    publishProjectTaskChatBadges(changedChatId);
}

void CloudBackend::resetProjectTaskChats()
{
    m_projectTaskChatsProject.clear();
    m_projectTaskChatsRequest.clear();
    m_projectTaskChatsNext = -1;
    m_projectTaskIds.clear();
    m_projectTaskChatIds.clear();
}

void CloudBackend::decorateProjectTaskChat(Chat &chat) const
{
    decorateChatBadges(chat);
    const auto live = m_recentOverlays.constFind(chat.id);
    if (live == m_recentOverlays.cend() || live->chatId != chat.serverChatId)
        return;
    if (live->counter)
        chat.unread = *live->counter;
    if (live->markedUnread)
        chat.markedUnread = *live->markedUnread;
}

void CloudBackend::publishProjectTaskChatBadges(qint64 chatId)
{
    if (m_projectTaskChatsProject.isEmpty() || m_projectTaskChatIds.isEmpty()
        || (chatId > 0 && !m_projectTaskChatIds.contains(chatId)))
        return;
    QVector<Chat> badges;
    const auto append = [this, &badges](qint64 id) {
        Chat chat;
        chat.id = QStringLiteral("chat") + QString::number(id);
        chat.serverChatId = id;
        decorateProjectTaskChat(chat);
        badges.append(std::move(chat));
    };
    if (chatId > 0)
        append(chatId);
    else
        for (auto id : std::as_const(m_projectTaskChatIds))
            append(id);
    emit projectTaskChatBadgesChanged(m_projectTaskChatsProject, badges);
}

void CloudBackend::readChatMessages(const QString &dialog, const QStringList &incoming, const QStringList &visible)
{
    readVisibleChat(dialog, incoming, visible);
}

void CloudBackend::readTaskChatMessages(const QString &task, const QStringList &incoming, const QStringList &visible)
{
    if (!task.isEmpty())
        readVisibleChat(m_taskDialogs.value(task), incoming, visible, task);
}

std::optional<int> CloudBackend::unreadCount(const QJsonValue &value)
{
    bool parsed = true;
    const auto count = value.isString() ? value.toString().toLongLong(&parsed) : value.toInteger(-1);
    return parsed && count >= 0 && count <= std::numeric_limits<int>::max()
        ? std::optional<int>(int(count)) : std::nullopt;
}

void CloudBackend::applyReadHistory(const QString &dialog, const QJsonObject &receipt, const QStringList &submitted)
{
    auto &state = dialogMessageState(dialog);
    const auto exact = receipt.value(QStringLiteral("exact"));
    const bool fromPush = submitted.isEmpty();
    // A scoped HTTP acknowledgement only confirms its submitted IDs. Only an accepted
    // Push can advance the cumulative cursor, which can affect previously unseen rows.
    const auto through = !fromPush || exact.toBool() || exact == QLatin1String("Y")
        ? 0 : positiveId(receipt.value(QStringLiteral("lastId")));
    if (through > state.confirmedReadThrough) {
        state.confirmedReadThrough = through;
        state.lastReadId = qMax(state.lastReadId, through);
        state.exactlyRead.removeIf([through](qint64 id) { return id <= through; });
        state.historyUnread.removeIf([through](qint64 id) { return id <= through; });
    }

    QSet<qint64> confirmed;
    const auto append = [fromPush, &submitted, &confirmed](qint64 id) {
        if (id <= 0)
            return;
        if (fromPush || submitted.contains(QString::number(id)))
            confirmed.insert(id);
    };
    const auto viewed = receipt.value(QStringLiteral("viewedMessages"));
    if (viewed.isUndefined()) {
        // A successful HTTP read may return only chatId and counter. It confirms the submitted IDs.
        for (const auto &id : submitted)
            append(positiveId(id));
    } else {
        const auto ids = viewed.toArray();
        for (qsizetype index = 0; index < std::min<qsizetype>(ids.size(), tunables::kPageSizeMessages); ++index)
            append(positiveId(ids.at(index)));
    }

    auto ordered = confirmed.values();
    std::sort(ordered.begin(), ordered.end());
    QStringList confirmedIds;
    for (const auto id : ordered) {
        confirmedIds.append(QString::number(id));
        state.historyUnread.remove(id);
        if (id <= state.confirmedReadThrough || state.exactlyRead.contains(id))
            continue;
        if (state.exactlyRead.size() >= tunables::kRetainedChatMessages)
            state.exactlyRead.erase(std::min_element(state.exactlyRead.cbegin(), state.exactlyRead.cend()));
        state.exactlyRead.insert(id);
    }
    if (through > 0 || !confirmedIds.isEmpty()) {
        state.readReceiptRevision = ++m_chatBadgeRevision;
        emit chatReadConfirmed(dialog, confirmedIds, through);
    }
}

void CloudBackend::readVisibleChat(const QString &dialog, const QStringList &incoming, const QStringList &visible,
                                    const QString &task)
{
    const bool selected = task.isEmpty() ? m_activeSection == 0 && dialog == m_openDialog
        : task == m_openTask && dialog == m_taskDialogs.value(task);
    if (!m_started || !m_foreground || m_authExpired || dialog.isEmpty() || !selected)
        return;
    const auto chatId = badgeChatId(dialog);
    if (chatId <= 0)
        return;
    readVisibleAnchors(chatId, visible.isEmpty() ? incoming : visible);

    const auto operation = operationKey(OperationKind::ReadVisibleMessages, dialog);
    auto &state = dialogMessageState(dialog);
    if (m_portalHttp.contains(operation)) {
        state.pendingReadIds = incoming.mid(0, maxVisibleReadIds);
        state.pendingReadTask = task;
        return;
    }
    QStringList ids;
    for (const auto &value : incoming) {
        const auto id = positiveId(value);
        const auto canonical = QString::number(id);
        if (id > state.confirmedReadThrough && (id > state.lastReadId || state.historyUnread.contains(id))
            && !state.exactlyRead.contains(id) && !ids.contains(canonical)) {
            ids.append(canonical);
            if (ids.size() >= maxVisibleReadIds)
                break;
        }
    }
    if (ids.isEmpty())
        return;
    const auto revision = m_chatBadgeRevision;
    const auto scope = m_accountScopeRevision;
    Fields fields{{QStringLiteral("chatId"), QString::number(chatId)},
                  {QStringLiteral("actionUuid"), QUuid::createUuid().toString(QUuid::WithoutBraces)}};
    for (qsizetype index = 0; index < ids.size(); ++index)
        fields.append({QStringLiteral("ids[%1]").arg(index), ids.at(index)});
    ajax(QStringLiteral("im.v2.Chat.Message.read"), fields, operation,
         [this, dialog, chatId, revision, scope, ids](const QJsonValue &value) {
        if (scope != m_accountScopeRevision)
            return;
        auto &updated = dialogMessageState(dialog);
        const auto pending = std::exchange(updated.pendingReadIds, {});
        const auto pendingTask = std::exchange(updated.pendingReadTask, {});
        const auto result = value.toObject();
        const auto counter = unreadCount(result.value(QStringLiteral("counter")));
        if (positiveId(result.value(QStringLiteral("chatId"))) == chatId && counter) {
            auto &live = recentOverlay(dialog);
            if (live.counterRevision <= revision) {
                updated.unreadCounter = counter;
                live.chatId = chatId;
                live.counterRevision = ++m_chatBadgeRevision;
                live.counter = counter;
                live.confirmedCounterCursor = qMax(live.confirmedCounterCursor, positiveId(result.value(QStringLiteral("lastId"))));
                publishUnreadChatCount();
                republishChatBadges(chatId);
            }
            applyReadHistory(dialog, result, ids);
        }
        if (!pending.isEmpty())
            readVisibleChat(dialog, pending, {}, pendingTask);
    });
}

void CloudBackend::readVisibleAnchors(qint64 chatId, const QStringList &visible)
{
    const auto known = m_chatAnchors.constFind(chatId);
    if (known == m_chatAnchors.cend())
        return;
    QSet<qint64> seen;
    for (const auto &value : visible.mid(0, maxVisibleReadIds))
        seen.insert(positiveId(value));
    QSet<qint64> ids;
    for (const auto &anchor : *known) {
        if (seen.contains(anchor.message))
            ids.insert(anchor.message);
    }
    if (ids.isEmpty())
        return;
    auto ordered = ids.values();
    std::sort(ordered.begin(), ordered.end());
    Fields fields;
    for (qsizetype index = 0; index < ordered.size(); ++index)
        fields.append({QStringLiteral("messageIds[%1]").arg(index), QString::number(ordered.at(index))});
    const auto revision = m_anchorRevision;
    ajax(QStringLiteral("im.v2.Anchor.read"), fields, operationKey(OperationKind::ReadVisibleAnchors, QString::number(chatId)),
         [this, chatId, ids, revision](const QJsonValue &data) {
        if (!data.toBool())
            return;
        const auto items = m_chatAnchors.find(chatId);
        if (items == m_chatAnchors.end())
            return;
        const auto removed = items->removeIf([&ids, revision](const ChatBadgeAnchor &anchor) {
            return anchor.revision <= revision && ids.contains(anchor.message);
        });
        const bool newer = std::any_of(items->cbegin(), items->cend(), [revision](const ChatBadgeAnchor &anchor) {
            return anchor.revision > revision;
        });
        if (items->isEmpty())
            m_chatAnchors.erase(items);
        if (removed) {
            m_chatAnchorCount -= removed;
            // A fetched snapshot started before the acknowledgement can still contain these marks.
            m_badgePageOvertaken |= m_badgePagePending;
            republishChatBadges(chatId);
        }
        if (newer)
            notifyAnchorAvailable(chatId);
    });
}

} // namespace bx
