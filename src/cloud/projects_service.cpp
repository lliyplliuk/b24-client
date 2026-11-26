#include "cloud/projects_service.h"
#include "cloud/cloud_rows.h"
#include "core/origin_guard.h"

#include <QMap>
#include <QRegularExpression>
#include <QSettings>
#include <QUrlQuery>
#include <algorithm>
#include <limits>
#include <utility>

namespace bx {
using namespace cloud_rows;

namespace {

const QString projectsOp = operationKey(OperationKind::Projects);
const QString projectFallbackOp = operationKey(OperationKind::ProjectPageFallback);
const QString projectPinIdentityOp = operationKey(OperationKind::ProjectPinIdentity);

QJsonObject newProjectDto(const QString &name, qint64 ownerId)
{
    // Defaults from the cloud Project Wizard's Pinia store, mapped to Project.add field names.
    const QString participants = QStringLiteral("K");
    const QString moderators = QStringLiteral("E");
    const QString owner = QStringLiteral("A");
    const QString enabled = QStringLiteral("Y");
    const QJsonObject tasks{{QStringLiteral("view"), participants}, {QStringLiteral("view_all"), participants},
                            {QStringLiteral("sort"), participants}, {QStringLiteral("create_tasks"), participants},
                            {QStringLiteral("edit_tasks"), moderators}, {QStringLiteral("delete_tasks"), moderators}};
    const QJsonObject blog{{QStringLiteral("view_post"), participants}, {QStringLiteral("premoderate_post"), participants},
                           {QStringLiteral("write_post"), participants}, {QStringLiteral("moderate_post"), moderators},
                           {QStringLiteral("full_post"), owner}, {QStringLiteral("view_comment"), participants},
                           {QStringLiteral("premoderate_comment"), participants}, {QStringLiteral("write_comment"), participants},
                           {QStringLiteral("moderate_comment"), moderators}, {QStringLiteral("full_comment"), moderators}};
    const QJsonObject knowledge{{QStringLiteral("read"), participants}, {QStringLiteral("edit"), participants},
                                {QStringLiteral("sett"), participants}, {QStringLiteral("delete"), participants}};
    const QJsonObject options{{QStringLiteral("whoCanInvite"), participants}, {QStringLiteral("manageMessages"), participants},
                              {QStringLiteral("manageMessagesAutoDelete"), moderators}, {QStringLiteral("messagesAutoDeleteDelay"), QString()},
                              {QStringLiteral("showHistory"), enabled}, {QStringLiteral("canGuestCopyText"), enabled},
                              {QStringLiteral("canGuestScreenshot"), enabled}, {QStringLiteral("allowGuestsInvitation"), enabled}};
    const auto emptyDate = QJsonValue(QJsonValue::Null);
    return {{QStringLiteral("name"), name}, {QStringLiteral("description"), QString()}, {QStringLiteral("goal"), QString()},
            {QStringLiteral("privacyType"), QStringLiteral("closed")}, {QStringLiteral("ownerId"), ownerId},
            {QStringLiteral("members"), QJsonArray{}}, {QStringLiteral("moderatorMembers"), QJsonArray{}},
            {QStringLiteral("permissions"), QJsonObject{{QStringLiteral("tasks"), tasks}, {QStringLiteral("blog"), blog},
                                                       {QStringLiteral("landingKnowledge"), knowledge}}},
            {QStringLiteral("options"), options}, {QStringLiteral("publication"), false},
            {QStringLiteral("dates"), QJsonObject{{QStringLiteral("startTs"), emptyDate}, {QStringLiteral("finishTs"), emptyDate}}},
            {QStringLiteral("tags"), QJsonArray{}}, {QStringLiteral("baseFeatureId"), QStringLiteral("chat")}};
}

QString projectHtmlText(QString text)
{
    return text.replace(QLatin1String("&quot;"), QLatin1String("\""))
        .replace(QLatin1String("&#39;"), QLatin1String("'"))
        .replace(QLatin1String("&lt;"), QLatin1String("<"))
        .replace(QLatin1String("&gt;"), QLatin1String(">"))
        .replace(QLatin1String("&nbsp;"), QLatin1String(" "))
        .replace(QLatin1String("&amp;"), QLatin1String("&"));
}

QHash<QString, QString> projectHtmlAttributes(const QString &attrs)
{
    static const QRegularExpression attribute(QString::fromLatin1(
        R"attribute(([a-zA-Z][\w:-]*)\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+)))attribute"));
    QHash<QString, QString> values;
    for (auto matches = attribute.globalMatch(attrs); matches.hasNext();) {
        const auto match = matches.next();
        values.insert(match.captured(1).toLower(), projectHtmlText(match.captured(match.lastCapturedIndex())));
    }
    return values;
}

bool hasHtmlClass(const QHash<QString, QString> &attrs, const QString &name)
{
    static const QRegularExpression spacing(QStringLiteral("\\s+"));
    return attrs.value(QStringLiteral("class")).split(spacing, Qt::SkipEmptyParts).contains(name);
}

QUrl safeProjectHref(const QUrl &base, const QString &href)
{
    if (href.isEmpty() || href.size() > 2048)
        return {};
    auto url = base.resolved(QUrl(href, QUrl::StrictMode));
    if (!url.isValid() || !url.userInfo().isEmpty() || url.hasFragment()
        || !OriginGuard(base).isSameOrigin(url))
        return {};
    QUrlQuery query(url);
    for (const auto &item : query.queryItems(QUrl::FullyDecoded)) {
        const auto key = item.first.toLower();
        if (key.contains(QLatin1String("token")) || key == QLatin1String("sessid")
            || key == QLatin1String("auth") || key == QLatin1String("logout") || key == QLatin1String("action"))
            return {};
    }
    // Composite cache selection belongs to the request, not to the grid scope or cursor.
    if (query.hasQueryItem(QStringLiteral("ncc"))) {
        query.removeAllQueryItems(QStringLiteral("ncc"));
        if (query.isEmpty())
            url.setQuery(QString());
        else
            url.setQuery(query);
    }
    return url;
}

const QRegularExpression projectAnchors(QString::fromLatin1(R"(<a\b([^<>]*)>([\s\S]*?)</a\s*>)"),
                                        QRegularExpression::CaseInsensitiveOption);

QString cleanProjectPath(const QUrl &url)
{
    auto path = url.path(QUrl::FullyDecoded);
    if (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return path;
}

QUrl projectGridHref(const QString &html, const QUrl &tasksPage)
{
    static const QRegularExpression projectTab(QStringLiteral("^/(project|projects|group|groups)$"));
    const auto personalPath = cleanProjectPath(tasksPage);
    for (auto it = projectAnchors.globalMatch(html); it.hasNext();) {
        const auto match = it.next();
        const auto attrs = projectHtmlAttributes(match.captured(1));
        const auto url = safeProjectHref(tasksPage, attrs.value(QStringLiteral("href")));
        const auto path = cleanProjectPath(url);
        for (const auto &prefix : {personalPath, QStringLiteral("/tasks")}) {
            if (path.startsWith(prefix) && projectTab.match(path.mid(prefix.size())).hasMatch())
                return url;
        }
    }
    return {};
}

struct ProjectPageCursor
{
    QString gridPath;
    QString name;
    qint64 number = 0;
    bool pathBased = false;
};

ProjectPageCursor projectPageCursor(const QUrl &url)
{
    static const QRegularExpression namedPage(QStringLiteral("^page-([1-9][0-9]{0,8})(?:-size-[1-9][0-9]{0,8})?$"));
    static const QRegularExpression classicPage(QStringLiteral("^PAGEN_[1-9][0-9]*$"));
    static const QRegularExpression pathPage(
        QStringLiteral("^(.+)/([A-Za-z0-9_-]+)/page-([1-9][0-9]{0,8})(?:/size-[1-9][0-9]{0,8})?$"));
    ProjectPageCursor cursor{cleanProjectPath(url), {}};
    const auto pathMatch = pathPage.match(cursor.gridPath);
    if (pathMatch.hasMatch())
        cursor = {pathMatch.captured(1), pathMatch.captured(2), pathMatch.captured(3).toLongLong(), true};

    const auto queryItems = QUrlQuery(url).queryItems(QUrl::FullyDecoded);
    for (const auto &[key, value] : queryItems) {
        const auto named = namedPage.match(value).captured(1);
        const auto number = !named.isEmpty() ? named.toLongLong()
            : classicPage.match(key).hasMatch() ? numericId(value).toLongLong() : 0;
        if (number <= 0)
            continue;
        if (cursor.number != 0)
            return {};
        cursor.name = key;
        cursor.number = number;
    }
    if (cursor.number && !cursor.pathBased
        && std::count_if(queryItems.cbegin(), queryItems.cend(), [&](const auto &item) {
            return item.first == cursor.name;
        }) != 1)
        return {};
    return cursor;
}

QMap<QString, QStringList> projectPageScope(const QUrl &url, const ProjectPageCursor &cursor)
{
    QMap<QString, QStringList> scope;
    for (const auto &[key, value] : QUrlQuery(url).queryItems(QUrl::FullyDecoded)) {
        if (cursor.pathBased || !cursor.number || key != cursor.name)
            scope[key].append(value);
    }
    return scope;
}

QUrl nextProjectGridHref(const QString &html, const QUrl &current)
{
    static const QRegularExpression pagerAnchors(QString::fromLatin1(
        R"((?:<li\b([^<>]*)>\s*)?<a\b([^<>]*)>[\s\S]*?</a\s*>)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression nextRelation(QStringLiteral("(?:^|\\s)next(?:\\s|$)"),
        QRegularExpression::CaseInsensitiveOption);
    const auto cursor = projectPageCursor(current);
    if (cursor.gridPath.isEmpty() || cursor.number == std::numeric_limits<qint64>::max())
        return {};
    const auto nextNumber = std::max<qint64>(cursor.number, 1) + 1;
    const auto scope = projectPageScope(current, cursor);
    QUrl marked;
    QUrl bare;
    bool conflictingMarked = false;
    bool conflictingBare = false;
    for (auto it = pagerAnchors.globalMatch(html); it.hasNext();) {
        const auto match = it.next();
        const auto attrs = projectHtmlAttributes(match.captured(2));
        const auto candidate = safeProjectHref(current, attrs.value(QStringLiteral("href")));
        const auto next = projectPageCursor(candidate);
        if (next.gridPath != cursor.gridPath || next.number != nextNumber)
            continue;
        const auto nextScope = projectPageScope(candidate, next);
        if (cursor.number) {
            if (next.name != cursor.name || next.pathBased != cursor.pathBased || nextScope != scope)
                continue;
        } else {
            const auto scopeItems = scope.asKeyValueRange();
            if (!std::all_of(scopeItems.begin(), scopeItems.end(), [&](const auto &item) {
                return nextScope.value(item.first) == item.second;
            }))
                continue;
        }

        const bool explicitNext = nextRelation.match(attrs.value(QStringLiteral("rel"))).hasMatch()
            || hasHtmlClass(attrs, QStringLiteral("bx-pag-next"))
            || hasHtmlClass(projectHtmlAttributes(match.captured(1)), QStringLiteral("bx-pag-next"));
        auto &selected = explicitNext ? marked : bare;
        auto &conflicting = explicitNext ? conflictingMarked : conflictingBare;
        if (selected.isEmpty())
            selected = candidate;
        else if (selected != candidate)
            conflicting = true;
    }
    if (!marked.isEmpty())
        return conflictingMarked ? QUrl{} : marked;
    return conflictingBare ? QUrl{} : bare;
}

QString projectAvatarColor(const QString &value)
{
    static const QRegularExpression hex(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
    return hex.match(value).hasMatch() ? value : QString();
}

} // namespace

QString ProjectsService::projectPinSettingsKey() const
{
    const auto scope = m_host.settingsScope();
    return scope.isEmpty() ? QString() : (m_taskProjectScope ? QStringLiteral("task-project-pins/")
                                                             : QStringLiteral("project-pins/")) + scope;
}

void ProjectsService::requestProjects()
{
    if (m_loadingProjects)
        return;
    m_loadingProjects = true;
    m_loadedProjects.clear();
    m_nextProjectGridPage.clear();
    m_nextProjectOffset = -1;
    m_emptyProjectPages = 0;
    m_host.withCurrentUser(projectPinIdentityOp, [this](qint64 userId) {
        if (userId <= 0) {
            loadProjectFallback(0);
            return;
        }
        const auto taskPage = OriginGuard(m_host.portalOrigin()).resolve(
            QStringLiteral("/company/personal/user/%1/tasks/").arg(userId));
        m_portalHttp.fetchPortalHtml(taskPage, [this, taskPage](const QString &html) {
            const auto grid = projectGridHref(html, taskPage);
            if (grid.isEmpty())
                loadProjectFallback(0);
            else
                loadProjectGrid(grid, true);
        });
    });
}

void ProjectsService::requestMoreProjects()
{
    if (m_loadingProjects || (m_nextProjectGridPage.isEmpty() && m_nextProjectOffset < 0))
        return;
    m_loadingProjects = true;
    if (!m_nextProjectGridPage.isEmpty())
        loadProjectGrid(m_nextProjectGridPage, false);
    else
        loadProjectFallback(m_nextProjectOffset);
}

void ProjectsService::loadProjectGrid(const QUrl &url, bool discovery)
{
    auto requestUrl = url;
    QUrlQuery query(requestUrl);
    // The cached page can contain only the Composite frame scaffold without project rows.
    query.addQueryItem(QStringLiteral("ncc"), QStringLiteral("1"));
    requestUrl.setQuery(query);
    m_portalHttp.fetchPortalHtml(requestUrl, [this, url, discovery](const QString &html) {
        static const QRegularExpression taskScope(QStringLiteral(
            "[\"']?pinMode[\"']?\\s*:\\s*[\"']tasks_project[\"']"));
        if (!html.contains(QLatin1String("BX.Socialnetwork.Project.List.Controller.init")) || !taskScope.match(html).hasMatch()) {
            if (discovery) {
                loadProjectFallback(0);
                return;
            }
            m_loadingProjects = false;
            emit projectsMoreAvailable(true);
            m_host.failShape(projectsOp);
            return;
        }
        m_taskProjectScope = true;
        static const QRegularExpression gridRows(QString::fromLatin1(R"(<tr\b([^<>]*)>([\s\S]*?)</tr\s*>)"),
                                                  QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression rowElements(QString::fromLatin1(R"(<(?:span|div|button|a)\b([^<>]*)>)"),
                                                    QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression tags(QStringLiteral("<[^>]*>"));
        QVector<Project> projects;
        for (auto it = gridRows.globalMatch(html); it.hasNext();) {
            const auto match = it.next();
            const auto attrs = projectHtmlAttributes(match.captured(1));
            if (!hasHtmlClass(attrs, QStringLiteral("main-grid-row")))
                continue;
            Project project;
            project.id = numericId(attrs.value(QStringLiteral("data-id")));
            if (project.id.isEmpty())
                continue;
            project.pinned = hasHtmlClass(attrs, QStringLiteral("sonet-ui-grid-row-pinned"));
            const auto row = match.captured(2);
            for (auto links = projectAnchors.globalMatch(row); links.hasNext();) {
                const auto link = links.next();
                const auto linkAttrs = projectHtmlAttributes(link.captured(1));
                if (hasHtmlClass(linkAttrs, QStringLiteral("socialnetwork-project-list-name-link")))
                    project.name = projectHtmlText(link.captured(2).remove(tags)).trimmed();
            }
            for (auto elements = rowElements.globalMatch(row); elements.hasNext();) {
                const auto avatar = projectHtmlAttributes(elements.next().captured(1));
                if (hasHtmlClass(avatar, QStringLiteral("main-grid-cell-content-action-pin"))
                    && (hasHtmlClass(avatar, QStringLiteral("main-grid-cell-content-action-active"))
                        || avatar.value(QStringLiteral("aria-pressed")) == QLatin1String("true")))
                    project.pinned = true;
                if (!hasHtmlClass(avatar, QStringLiteral("socialnetwork-project-list-avatar")))
                    continue;
                if (project.name.isEmpty())
                    project.name = avatar.value(QStringLiteral("data-title")).trimmed();
                project.avatarVariant = avatar.value(QStringLiteral("data-border-variant")) == QLatin1String("guest")
                    ? QStringLiteral("guest") : QStringLiteral("project");
                project.avatarColor = projectAvatarColor(avatar.value(QStringLiteral("data-color")));
                project.avatarUrl = m_host.avatarUrlFromPortal(avatar.value(QStringLiteral("data-pic")));
            }
            if (!project.name.isEmpty())
                projects.append(std::move(project));
        }
        m_nextProjectGridPage = nextProjectGridHref(html, url);
        publishProjectPage(std::move(projects));
    });
}

void ProjectsService::loadProjectFallback(qint64 offset)
{
    const auto offsetField = offset > 0 ? QString::number(offset) : QStringLiteral("0");
    m_portalHttp.ajax(QStringLiteral("socialnetwork.api.workgroup.list"),
         {{QStringLiteral("filter[PROJECT]"), QStringLiteral("Y")}, {QStringLiteral("filter[ACTIVE]"), QStringLiteral("Y")},
          {QStringLiteral("filter[CLOSED]"), QStringLiteral("N")}, {QStringLiteral("order[DATE_ACTIVITY]"), QStringLiteral("DESC")},
          {QStringLiteral("order[ID]"), QStringLiteral("ASC")}, {QStringLiteral("params[mode]"), QStringLiteral("mobile")},
          {QStringLiteral("start"), offsetField}, {QStringLiteral("select[0]"), QStringLiteral("ID")},
          {QStringLiteral("select[1]"), QStringLiteral("NAME")}, {QStringLiteral("select[2]"), QStringLiteral("AVATAR")},
          {QStringLiteral("select[3]"), QStringLiteral("TYPE")}, {QStringLiteral("select[4]"), QStringLiteral("AVATAR_TYPE")}},
         projectFallbackOp, [this, offset](QJsonValue data) {
            const auto rows = list(data, {"items", "groups", "workgroups", "result"});
            if (!rows.isArray()) {
                m_loadingProjects = false;
                emit projectsMoreAvailable(offset > 0);
                m_host.failShape(projectsOp);
                return;
            }
            const auto items = rows.toArray();
            if (offset == 0) {
                m_taskProjectScope = std::any_of(items.cbegin(), items.cend(), [](const QJsonValue &item) {
                    return item.toObject().value(QStringLiteral("additionalData")).toObject()
                        .value(QStringLiteral("role")).isString();
                });
            }
            const auto cacheKey = projectPinSettingsKey();
            QStringList cached;
            if (!cacheKey.isEmpty()) {
                QSettings settings(QStringLiteral("bitrix24-client"), QStringLiteral("bitrix24-client"));
                cached = settings.value(cacheKey).toStringList();
            }
            QVector<Project> projects;
            for (const auto row : items) {
                const auto item = row.toObject();
                const auto id = numericId(field(item, {"ID", "id"}));
                const auto name = field(item, {"NAME", "name"});
                if (id.isEmpty() || name.isEmpty())
                    continue;
                const auto extra = item.value(QStringLiteral("additionalData")).toObject();
                const auto role = field(extra, {"role"});
                if (m_taskProjectScope && role != QLatin1String("A") && role != QLatin1String("E") && role != QLatin1String("K"))
                    continue;
                const auto remotePin = notificationFlag(oneOf(item, {"pinned", "isPinned", "IS_PINNED"}));
                auto project = projectFromApiRow(item);
                project.pinned = remotePin.value_or(cached.contains(id));
                projects.append(std::move(project));
            }
            const auto explicitNext = numericId(field(data.toObject(), {"next"})).toLongLong();
            constexpr auto lastOffset = std::numeric_limits<int>::max() - tunables::kPageSizeProjects;
            m_nextProjectOffset = explicitNext > offset && explicitNext <= lastOffset ? explicitNext
                : items.size() == tunables::kPageSizeProjects && offset <= lastOffset
                ? offset + tunables::kPageSizeProjects : -1;
            publishProjectPage(std::move(projects));
    });
}

void ProjectsService::publishProjectPage(QVector<Project> rows)
{
    QSet<QString> ids;
    for (const auto &project : std::as_const(m_loadedProjects))
        ids.insert(project.id);
    for (auto &project : rows) {
        if (!ids.contains(project.id)) {
            ids.insert(project.id);
            m_loadedProjects.append(std::move(project));
        }
    }
    m_loadingProjects = false;
    const bool more = !m_nextProjectGridPage.isEmpty() || m_nextProjectOffset >= 0;
    if (m_loadedProjects.isEmpty() && more && ++m_emptyProjectPages <= 3) {
        requestMoreProjects();
        return;
    }
    emit projectsScopeLoaded(m_taskProjectScope);
    emit projectsMoreAvailable(more);
    emit projectsLoaded(m_loadedProjects);
}

void ProjectsService::requestProjectMembers(const QString &projectId)
{
    const auto operation = operationKey(OperationKind::ProjectMembers, projectId);
    if (numericId(projectId).isEmpty()) {
        emit failed(operation, QStringLiteral("Некорректный номер проекта"));
        return;
    }
    const QJsonArray fields{QStringLiteral("OWNER_DATA"), QStringLiteral("LIST_OF_MEMBERS"),
        QStringLiteral("LIST_OF_MEMBERS_AWAITING_INVITE"), QStringLiteral("GROUP_MEMBERS_LIST")};
    m_portalHttp.ajaxJson(QStringLiteral("socialnetwork.api.workgroup.get"),
        {{QStringLiteral("params"), QJsonObject{{QStringLiteral("groupId"), projectId.toLongLong()},
                                                {QStringLiteral("select"), fields}}}},
        operation, Handler::checked(QStringLiteral("Не удалось получить участников проекта"),
        [projectId](const QJsonValue &data) {
            const auto payload = data.toObject();
            const auto list = payload.value(QStringLiteral("LIST_OF_MEMBERS"));
            return field(payload, {"ID", "id"}) == projectId
                && (list.isObject() || list.isArray() || payload.value(QStringLiteral("MEMBERS")).isArray());
        }, [this, projectId](QJsonValue data) {
        const auto payload = data.toObject();
        QHash<QString, ProjectMember> entries;
        const auto append = [&entries](const QString &id, ProjectMemberRole role, const QJsonObject &row = {}) {
            if (id.isEmpty() || (entries.size() >= 200 && !entries.contains(id)))
                return;
            auto *entry = &entries[id];
            if (entry->id.isEmpty() || role < entry->role)
                entry->role = role;
            entry->id = id;
            auto name = field(row, {"FORMATTED_NAME"});
            if (name.isEmpty())
                name = (field(row, {"name"}) + QLatin1Char(' ') + field(row, {"lastName"})).trimmed();
            if (!name.isEmpty())
                entry->name = name.left(256);
            const auto position = field(row, {"position"});
            if (!position.isEmpty())
                entry->position = position.left(256);
        };
        const auto owner = numericId(field(payload, {"OWNER_ID"}));
        const auto ownerInfo = payload.value(QStringLiteral("OWNER_DATA")).toObject();
        append(owner, ProjectMemberRole::Owner, field(ownerInfo, {"ID", "id"}) == owner ? ownerInfo : QJsonObject());
        const QPair<const char *, ProjectMemberRole> roleLists[] = {
            {"MODERATOR_MEMBERS", ProjectMemberRole::Moderator}, {"ORDINARY_MEMBERS", ProjectMemberRole::Member},
            {"MEMBERS", ProjectMemberRole::Member}, {"INVITED_MEMBERS", ProjectMemberRole::Invited}};
        for (const auto &[key, role] : roleLists) {
            for (const auto value : payload.value(QLatin1String(key)).toArray())
                append(numericId(string(value)), role);
        }
        const auto visit = [&payload](const QString &key, const auto &consumer) {
            const auto values = payload.value(key);
            for (const auto value : values.toArray())
                consumer(value.toObject());
            for (const auto value : values.toObject())
                consumer(value.toObject());
        };
        visit(QStringLiteral("LIST_OF_MEMBERS"), [&append](const QJsonObject &row) {
            const auto role = row.value(QStringLiteral("isOwner")) == true ? ProjectMemberRole::Owner
                : row.value(QStringLiteral("isModerator")) == true ? ProjectMemberRole::Moderator : ProjectMemberRole::Member;
            append(numericId(field(row, {"id"})), role, row);
        });
        visit(QStringLiteral("LIST_OF_MEMBERS_AWAITING_INVITE"), [&append](const QJsonObject &row) {
            append(numericId(field(row, {"id"})), ProjectMemberRole::Awaiting, row);
        });
        visit(QStringLiteral("GROUP_MEMBERS_LIST"), [&append](const QJsonObject &row) {
            const auto id = numericId(field(row, {"id"}));
            if (row.value(QStringLiteral("isMember")) == true)
                append(id, ProjectMemberRole::Member);
            else if (row.value(QStringLiteral("invited")) == true)
                append(id, ProjectMemberRole::Invited);
            else if (row.value(QStringLiteral("isAwaiting")) == true)
                append(id, ProjectMemberRole::Awaiting);
        });
        auto roster = entries.values();
        for (auto &entry : roster) {
            if (entry.name.isEmpty())
                entry.name = QStringLiteral("Пользователь №%1").arg(entry.id);
        }
        std::sort(roster.begin(), roster.end(), [](const ProjectMember &a, const ProjectMember &b) {
            return a.role != b.role ? a.role < b.role : QString::localeAwareCompare(a.name, b.name) < 0;
        });
        emit projectMembersLoaded(projectId, roster);
    }));
}

void ProjectsService::requestProjectFiles(const QString &projectId, const QString &folderId, int offset)
{
    const auto operation = operationKey(OperationKind::ProjectFiles, projectId, folderId);
    if (numericId(projectId).isEmpty() || offset < 0 || offset > std::numeric_limits<int>::max() - 50) {
        emit failed(operation, QStringLiteral("Некорректная папка проекта"));
        return;
    }
    if (!folderId.isEmpty() || offset > 0) {
        const auto disk = m_projectDisks.constFind(projectId);
        if (disk == m_projectDisks.cend() || (!folderId.isEmpty() && !disk->folders.contains(folderId))) {
            emit failed(operation, QStringLiteral("Папка недоступна в этом проекте"));
            return;
        }
        loadProjectDiskItems(projectId, folderId, offset);
        return;
    }
    const auto url = OriginGuard(m_host.portalOrigin()).resolve(QStringLiteral("/rest/disk.storage.getList.json"));
    m_portalHttp.webRequest(url, {{QStringLiteral("filter[ENTITY_TYPE]"), QStringLiteral("group")},
                     {QStringLiteral("filter[ENTITY_ID]"), projectId}}, operation,
               QStringLiteral("result"), Handler::checked(QStringLiteral("Диск проекта недоступен"),
               [](const QJsonValue &data) { return data.isArray(); },
               [this, projectId, operation](QJsonValue data) {
        for (const auto value : data.toArray()) {
            const auto entry = value.toObject();
            if (field(entry, {"ENTITY_TYPE"}) != QLatin1String("group") || field(entry, {"ENTITY_ID"}) != projectId)
                continue;
            const auto id = numericId(field(entry, {"ID"}));
            const auto root = numericId(field(entry, {"ROOT_OBJECT_ID"}));
            if (id.isEmpty() || root.isEmpty())
                continue;
            if (m_projectDisks.size() >= 8 && !m_projectDisks.contains(projectId))
                m_projectDisks.erase(m_projectDisks.begin());
            m_projectDisks.insert(projectId, {id, root, {}});
            loadProjectDiskItems(projectId, {}, 0);
            return;
        }
        emit failed(operation, QStringLiteral("Диск проекта недоступен"));
    }));
}

void ProjectsService::loadProjectDiskItems(const QString &projectId, const QString &folderId, int offset)
{
    const auto disk = m_projectDisks.value(projectId);
    const auto operation = operationKey(OperationKind::ProjectFiles, projectId, folderId);
    const auto endpoint = folderId.isEmpty() ? QStringLiteral("/rest/disk.storage.getChildren.json")
                                            : QStringLiteral("/rest/disk.folder.getChildren.json");
    const auto url = OriginGuard(m_host.portalOrigin()).resolve(endpoint);
    m_portalHttp.webRequest(url, {{QStringLiteral("id"), folderId.isEmpty() ? disk.id : folderId},
                     {QStringLiteral("start"), QString::number(offset)}}, operation, QStringLiteral("result"),
               Handler::checked(QStringLiteral("Не удалось получить файлы проекта"),
               [](const QJsonValue &data) { return data.isArray(); },
               [this, projectId, folderId, operation, storage = disk.id, root = disk.root, offset](QJsonValue data) {
        auto current = m_projectDisks.find(projectId);
        if (current == m_projectDisks.end() || current->id != storage || current->root != root) {
            emit failed(operation, QStringLiteral("Папка больше недоступна"));
            return;
        }
        const auto expectedParent = folderId.isEmpty() ? root : folderId;
        QVector<ProjectDiskItem> items;
        const auto rows = data.toArray();
        QSet<QString> seen;
        for (const auto value : rows) {
            const auto entry = value.toObject();
            if (field(entry, {"STORAGE_ID"}) != storage || field(entry, {"PARENT_ID"}) != expectedParent)
                continue;
            const auto id = numericId(field(entry, {"ID"}));
            const auto name = field(entry, {"NAME"}).left(255);
            const auto type = field(entry, {"TYPE"});
            if (id.isEmpty() || name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")
                || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) || seen.contains(id)
                || (type != QLatin1String("folder") && type != QLatin1String("file")))
                continue;
            seen.insert(id);
            ProjectDiskItem item{{id, name, -1, {}}, type == QLatin1String("folder")};
            if (item.folder) {
                if (current->folders.size() >= 500)
                    current->folders.erase(current->folders.begin());
                current->folders.insert(id);
            } else {
                bool ok = false;
                const auto size = field(entry, {"SIZE"}).toLongLong(&ok);
                item.file.size = ok && size >= 0 ? size : -1;
                item.file.downloadUrl = m_host.projectFileUrl(field(entry, {"DOWNLOAD_URL"}), item.file.size);
            }
            items.append(std::move(item));
            if (items.size() == 50)
                break;
        }
        const auto next = rows.size() >= 50 ? offset + 50 : -1;
        emit projectFilesLoaded(projectId, folderId, items, next);
    }));
}

void ProjectsService::setProjectPinned(const QString &projectId, bool pinned)
{
    Q_UNUSED(pinned); // The official project controller changes the current pin and returns its new state.
    const auto operation = operationKey(OperationKind::ProjectPin, projectId);
    const auto id = numericId(projectId);
    if (id.isEmpty()) {
        emit failed(operation, QStringLiteral("Некорректный номер проекта"));
        return;
    }
    m_host.withCurrentUser(operation, [this, id, projectId, operation](qint64) {
        m_portalHttp.ajaxJson(QStringLiteral("socialnetwork.v2.Project.changePin"),
                 {{QStringLiteral("projectId"), id.toLongLong()},
                  {QStringLiteral("mode"), m_taskProjectScope ? QStringLiteral("tasks_project") : QString()}},
                 operation, [this, id, projectId, operation, key = projectPinSettingsKey(), taskScope = m_taskProjectScope](QJsonValue result) {
            const auto actual = notificationFlag(result.toObject().value(QStringLiteral("pinned")));
            if (!actual)
                return m_host.failShape(operation);
            if (!key.isEmpty()) {
                QSettings settings(QStringLiteral("bitrix24-client"), QStringLiteral("bitrix24-client"));
                auto ids = settings.value(key).toStringList();
                ids.removeAll(id);
                if (*actual)
                    ids.prepend(id);
                if (ids.size() > tunables::kPageSizeBoardTasks)
                    ids = ids.first(tunables::kPageSizeBoardTasks);
                settings.setValue(key, ids);
            }
            if (m_taskProjectScope == taskScope) {
                const auto project = std::find_if(m_loadedProjects.begin(), m_loadedProjects.end(),
                                                  [id](const Project &row) { return row.id == id; });
                if (project != m_loadedProjects.end())
                    project->pinned = *actual;
            }
            emit projectPinChanged(projectId, *actual);
        });
    });
}

void ProjectsService::createProject(const QString &name)
{
    const auto op = operationKey(OperationKind::CreateProject);
    const auto title = name.trimmed();
    if (title.isEmpty()) {
        emit failed(op, QStringLiteral("Введите название проекта"));
        return;
    }
    m_host.withCurrentUser(op, [this, title, op](qint64 userId) {
        m_portalHttp.ajaxJson(QStringLiteral("socialnetwork.V2.Project.add"),
                 {{QStringLiteral("project"), newProjectDto(title, userId)}},
                 op, [this, op](QJsonValue result) {
            const auto id = numericId(field(result.toObject(), {"id"}));
            if (id.isEmpty())
                return m_host.failShape(op);
            emit projectCreated(id);
        });
    });
}

Project ProjectsService::projectFromApiRow(const QJsonObject &row) const
{
    Project project{numericId(field(row, {"ID", "id"})), field(row, {"NAME", "name"}), {}};
    project.avatarType = field(row, {"AVATAR_TYPE", "avatarType"});
    auto avatar = field(row, {"AVATAR", "avatar"});
    const auto data = row.value(QStringLiteral("AVATAR_DATA")).toObject();
    const auto kind = field(data, {"type"});
    const auto icon = field(data, {"id"});
    if (kind == QLatin1String("icon") && !icon.isEmpty())
        project.avatarType = icon;
    if (kind == QLatin1String("image") && avatar.isEmpty())
        avatar = icon;
    project.avatarUrl = m_host.avatarUrlFromPortal(avatar);
    return project;
}

void ProjectsService::loadLinkProject(const QString &id, const QString &operation,
                                  std::function<void(Project, QString)> complete)
{
    const auto scope = m_host.settingsScope();
    m_portalHttp.ajax(QStringLiteral("socialnetwork.api.workgroup.list"),
         {{QStringLiteral("filter[ID]"), id}, {QStringLiteral("params[mode]"), QStringLiteral("mobile")},
          {QStringLiteral("select[0]"), QStringLiteral("ID")}, {QStringLiteral("select[1]"), QStringLiteral("NAME")},
          {QStringLiteral("select[2]"), QStringLiteral("DESCRIPTION")}, {QStringLiteral("select[3]"), QStringLiteral("AVATAR")},
          {QStringLiteral("select[4]"), QStringLiteral("AVATAR_TYPE")}},
         operation, [this, id, scope, complete = std::move(complete)](QJsonValue value) {
        if (m_host.authExpired() || m_host.settingsScope() != scope) {
            complete({}, {});
            return;
        }
        const auto rows = list(value, {"workgroups", "items", "groups", "result"}).toArray();
        const auto match = std::find_if(rows.cbegin(), rows.cend(), [&id](const QJsonValue &entry) {
            const auto row = entry.toObject();
            const auto upper = row.value(QStringLiteral("ID"));
            const auto lower = row.value(QStringLiteral("id"));
            return (!upper.isUndefined() || !lower.isUndefined())
                && (upper.isUndefined() || numericId(string(upper)) == id)
                && (lower.isUndefined() || numericId(string(lower)) == id);
        });
        if (match == rows.cend()) {
            complete({}, {});
            return;
        }
        const auto row = (*match).toObject();
        auto project = projectFromApiRow(row);
        const auto known = std::find_if(m_loadedProjects.cbegin(), m_loadedProjects.cend(), [&project](const Project &entry) {
            return entry.id == project.id && entry.name == project.name
                && (entry.avatarType.isEmpty() || entry.avatarType == project.avatarType);
        });
        if (known != m_loadedProjects.cend())
            project.avatarColor = known->avatarColor;
        complete(std::move(project), field(row, {"DESCRIPTION", "description"}));
    });
}

bool ProjectsService::reset()
{
    m_loadingProjects = false;
    ++m_boardPageGeneration;
    m_boardColumnCursors.clear();
    m_linkStageSnapshots.clear();
    m_pendingLinkStages.clear();
    m_openBoard.clear();
    m_loadedProjects.clear();
    m_projectDisks.clear();
    m_nextProjectGridPage.clear();
    m_nextProjectOffset = -1;
    m_taskProjectScope = false;
    m_emptyProjectPages = 0;
    m_boardDirty = false;

    resetProjectTaskChats();
    m_projectUnreadParentsByChat.clear();
    m_projectUnreadNonTaskChats.clear();
    m_projectUnreadTasksByChat.clear();
    m_projectUnreadGroupsByParent.clear();
    m_projectUnreadParentAttempts.clear();
    m_projectUnreadTaskChatAttempts.clear();
    m_projectUnreadMovedTasks.clear();
    m_projectUnreadMoveAttempts.clear();
    m_projectUnreadPending = false;
    const bool countsChanged = !m_projectUnreadCounts.isEmpty() || m_projectUnreadComplete;
    m_projectUnreadCounts.clear();
    m_projectUnreadComplete = false;
    return countsChanged;
}

} // namespace bx
