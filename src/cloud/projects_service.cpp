#include "cloud/projects_service.h"
#include "cloud/cloud_rows.h"
#include "cloud/cloud_badge_data.h"
#include "core/origin_guard.h"
#include "core/stage_color.h"

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

QString boardReadKey(const QString &operation, quint64 generation)
{
    return operation + QLatin1Char('@') + QString::number(generation);
}

std::optional<qsizetype> stageTaskCount(const QJsonObject &column)
{
    // The web grid keeps the server total separately from the loaded cards.
    const auto total = column.value(QStringLiteral("total")).toInteger(-1);
    if (total < 0 || total > std::numeric_limits<qsizetype>::max())
        return std::nullopt;
    return static_cast<qsizetype>(total);
}

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

QString stageColorFrom(const QJsonObject &column)
{
    const auto color = normalizedStageColor(field(column, {"color"}));
    return color.isEmpty() ? normalizedStageColor(field(column, {"COLOR"})) : color;
}

QString boardItemId(const QJsonObject &item, const QString &projectId, const QString &stageId)
{
    const auto data = item.value(QStringLiteral("data")).toObject();
    if (data.isEmpty() || field(item, {"columnId"}) != stageId)
        return {};
    const auto rowProject = field(data, {"groupId", "GROUP_ID"});
    if (!rowProject.isEmpty() && numericId(rowProject) != projectId)
        return {};
    return numericId(field(item, {"id"}));
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

void ProjectsService::searchProjectTasks(const QString &projectId, const QString &query)
{
    const auto operation = operationKey(OperationKind::SearchProjectTasks, projectId);
    if (numericId(projectId).isEmpty()) {
        emit failed(operation, QStringLiteral("Некорректный номер проекта"));
        return;
    }
    const auto needle = query.trimmed();
    if (needle.isEmpty()) {
        emit projectTasksFound(projectId, query, {});
        return;
    }
    Fields fields{{QStringLiteral("filter[GROUP_ID]"), projectId},
                  {QStringLiteral("order[ID]"), QStringLiteral("DESC")},
                  {QStringLiteral("start"), QStringLiteral("0")}};
    const bool numberPrefix = needle.startsWith(QLatin1Char('#')) || needle.startsWith(QChar(u'№'));
    const auto matchId = numericId(numberPrefix ? needle.mid(1).trimmed() : needle);
    if (matchId.isEmpty()) {
        QString pattern = needle;
        pattern.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        pattern.replace(QLatin1Char('%'), QStringLiteral("\\%"));
        pattern.replace(QLatin1Char('_'), QStringLiteral("\\_"));
        fields.append({QStringLiteral("filter[TITLE]"), QStringLiteral("%") + pattern + QLatin1Char('%')});
    } else {
        fields.append({QStringLiteral("filter[ID]"), matchId});
    }
    for (const auto *name : {"ID", "TITLE", "GROUP_ID", "STAGE_ID", "DEADLINE", "PRIORITY", "RESPONSIBLE_ID"})
        fields.append({QStringLiteral("select[]"), QLatin1String(name)});
    m_portalHttp.ajax(QStringLiteral("tasks.task.list"), fields, operation,
         Handler::checked(QStringLiteral("Портал вернул неподдерживаемый формат данных"),
         [](const QJsonValue &data) { return data.toObject().value(QStringLiteral("tasks")).isArray(); },
         [this, projectId, query, matchId](QJsonValue data) {
        const auto rows = data.toObject().value(QStringLiteral("tasks"));
        QVector<Task> matches;
        for (const auto &row : rows.toArray()) {
            const auto item = row.toObject();
            auto groupId = field(item, {"groupId", "GROUP_ID"});
            if (groupId.isEmpty())
                groupId = field(item.value(QStringLiteral("group")).toObject(), {"id", "ID"});
            if (groupId != projectId)
                continue;
            auto task = taskFromRow(item);
            if (numericId(task.id).isEmpty() || (!matchId.isEmpty() && task.id != matchId))
                continue;
            if (task.stageName.isEmpty())
                task.stageName = stageLabel(task.stageId);
            matches.append(std::move(task));
            if (matches.size() == tunables::kPageSizeBoardTasks)
                break;
        }
        emit projectTasksFound(projectId, query, matches);
    }), operation + QLatin1Char(':') + query);
}

Task ProjectsService::boardTask(const QJsonObject &item, const QString &id, const QString &projectId,
                            const QString &stageId, const QString &stageName) const
{
    const auto data = item.value(QStringLiteral("data")).toObject();
    auto task = taskFromRow(data);
    task.id = id;
    task.projectId = projectId;
    task.stageId = stageId;
    task.stageName = stageName;
    task.name = field(data, {"name", "title", "TITLE"});
    const auto fillPerson = [this](const QJsonObject &person, QString &personId, QString &name, QUrl &avatar) {
        const auto id = numericId(field(person, {"id"}));
        if (id.isEmpty())
            return;
        personId = id;
        name = field(person, {"name"});
        avatar = m_host.avatarUrlFromPortal(field(person.value(QStringLiteral("photo")).toObject(), {"src"}));
    };
    fillPerson(data.value(QStringLiteral("author")).toObject(), task.creatorId, task.createdBy, task.creatorAvatarUrl);
    fillPerson(data.value(QStringLiteral("responsible")).toObject(), task.assigneeId, task.assignee, task.assigneeAvatarUrl);
    return task;
}

void ProjectsService::requestProjectBoard(const QString &projectId)
{
    const auto op = operationKey(OperationKind::Board, projectId);
    if (numericId(projectId).isEmpty()) {
        emit failed(op, QStringLiteral("Некорректный номер проекта"));
        return;
    }
    if (m_openBoard == projectId && m_portalHttp.contains(boardReadKey(op, m_boardPageGeneration)))
        return;
    if (m_openBoard != projectId) {
        m_openBoard = projectId;
        m_boardColumnCursors.clear();
        m_boardDirty = false;
    }
    const auto component = QStringLiteral("bitrix:tasks.kanban");
    const auto generation = ++m_boardPageGeneration;
    m_portalHttp.componentAction(QStringLiteral("applyFilter"), boardFields(projectId), op, [this, projectId, op, generation](QJsonValue data) {
        if (m_openBoard != projectId || m_boardPageGeneration != generation)
            return;
        const auto board = data.toObject();
        const auto columns = board.value(QStringLiteral("columns"));
        const auto items = board.value(QStringLiteral("items"));
        if (!columns.isArray() || !items.isArray())
            return m_host.failShape(op);
        QVector<Stage> stages;
        QHash<QString, qsizetype> byId;
        QHash<QString, BoardColumnCursor> cursors;
        for (const auto value : columns.toArray()) {
            const auto column = value.toObject();
            const auto id = field(column, {"id", "ID"});
            if (id.isEmpty() || byId.contains(id))
                continue;
            auto label = field(column, {"name", "TITLE"});
            if (label.isEmpty())
                label = stageLabel(id);
            byId.insert(id, stages.size());
            stages.append({id, label, {}, stageTaskCount(column), false, stageColorFrom(column)});
            auto &cursor = cursors[id];
            cursor.stageName = label;
            cursor.stageColor = stages.last().color;
            cursor.total = stages.last().taskCount;
            if (stages.size() == tunables::kPageSizeBoardTasks)
                break;
        }
        int tasks = 0;
        for (const auto value : items.toArray()) {
            const auto item = value.toObject();
            const auto stageId = field(item, {"columnId"});
            if (!byId.contains(stageId))
                continue;
            auto &cursor = cursors[stageId];
            const auto id = boardItemId(item, projectId, stageId);
            if (id.isEmpty() || cursor.seen.contains(id))
                continue;
            cursor.seen.insert(id);
            if (tasks < tunables::kPageSizeBoardTasks) {
                stages[byId.value(stageId)].tasks.append(boardTask(item, id, projectId, stageId, cursor.stageName));
                ++tasks;
            } else {
                // Do not skip unpainted page-one cards in later columns.
                cursor.deferred.append(item);
            }
        }
        for (auto &stage : stages) {
            auto &cursor = cursors[stage.id];
            if (cursor.total && *cursor.total < cursor.seen.size()) {
                cursor.total.reset();
                stage.taskCount.reset();
            }
            stage.hasMoreTasks = cursor.hasMore();
        }
        m_boardColumnCursors = std::move(cursors);
        emit boardLoaded(projectId, stages);
    }, component, boardReadKey(op, generation));
}

void ProjectsService::requestMoreBoardTasks(const QString &projectId, const QString &stageId)
{
    const auto operation = operationKey(OperationKind::BoardColumn, projectId, stageId);
    auto cursor = m_boardColumnCursors.find(stageId);
    if (projectId != m_openBoard || cursor == m_boardColumnCursors.end() || !boardStageId(stageId)) {
        emit failed(operation, QStringLiteral("Эта колонка сейчас недоступна"));
        return;
    }
    const auto generation = m_boardPageGeneration;
    if (m_portalHttp.contains(boardReadKey(operationKey(OperationKind::Board, projectId), generation))) {
        emit failed(operation, QStringLiteral("Дождитесь обновления доски"));
        return;
    }
    const auto key = boardReadKey(operation, generation);
    if (m_portalHttp.contains(key))
        return;
    if (!cursor->deferred.isEmpty()) {
        publishDeferredBoardTasks(projectId, stageId, *cursor);
        return;
    }
    if (!cursor->hasMore()) {
        emit boardTasksPageLoaded(projectId, stageId, {}, false);
        return;
    }

    auto fields = boardFields(projectId);
    fields.append({QStringLiteral("pageId"), QString::number(cursor->nextPage)});
    fields.append({QStringLiteral("columnId"), stageId});
    m_portalHttp.componentAction(QStringLiteral("getColumnItems"), fields, operation,
        [this, projectId, stageId, operation, generation](QJsonValue data) {
        auto current = m_boardColumnCursors.find(stageId);
        if (generation != m_boardPageGeneration || projectId != m_openBoard || current == m_boardColumnCursors.end())
            return;
        const auto rows = data.isArray() ? data : data.toObject().value(QStringLiteral("items"));
        if (!rows.isArray())
            return m_host.failShape(operation);
        const auto items = rows.toArray();
        if (items.isEmpty()) {
            current->serverExhausted = true;
            emit boardTasksPageLoaded(projectId, stageId, {}, false);
            return;
        }
        bool scoped = false;
        for (const auto value : items) {
            const auto item = value.toObject();
            const auto id = boardItemId(item, projectId, stageId);
            if (id.isEmpty())
                continue;
            scoped = true;
            if (current->seen.contains(id))
                continue;
            current->seen.insert(id);
            current->deferred.append(item);
        }
        if (!scoped)
            return m_host.failShape(operation);
        if (current->nextPage < std::numeric_limits<int>::max())
            ++current->nextPage;
        else
            current->serverExhausted = true;
        publishDeferredBoardTasks(projectId, stageId, *current);
    }, QStringLiteral("bitrix:tasks.kanban"), key);
}

void ProjectsService::publishDeferredBoardTasks(const QString &projectId, const QString &stageId, BoardColumnCursor &cursor)
{
    QVector<Task> tasks;
    const auto end = std::min(cursor.deferredOffset + tunables::kPageSizeBoardTasks, cursor.deferred.size());
    tasks.reserve(end - cursor.deferredOffset);
    for (; cursor.deferredOffset < end; ++cursor.deferredOffset) {
        const auto item = cursor.deferred.at(cursor.deferredOffset).toObject();
        tasks.append(boardTask(item, numericId(field(item, {"id"})), projectId, stageId, cursor.stageName));
    }
    if (cursor.deferredOffset == cursor.deferred.size()) {
        cursor.deferred = {};
        cursor.deferredOffset = 0;
    }
    emit boardTasksPageLoaded(projectId, stageId, tasks, cursor.hasMore());
}

void ProjectsService::requestProjectStages(const QString &projectId)
{
    const auto operation = operationKey(OperationKind::ProjectStages, projectId);
    if (numericId(projectId).isEmpty()) {
        emit failed(operation, QStringLiteral("Некорректный номер проекта"));
        return;
    }
    loadProjectStages(projectId, operation, [this, operation, projectId](std::optional<QVector<Stage>> stages) {
        if (!stages)
            return m_host.failShape(operation);
        emit projectStagesLoaded(projectId, *stages);
    });
}

void ProjectsService::loadProjectStages(const QString &projectId, const QString &operation, ProjectStagesHandler complete)
{
    const QJsonObject group{{QStringLiteral("id"), projectId.toLongLong()}};
    m_portalHttp.ajaxJson(QStringLiteral("tasks.v2.Group.Stage.list"), {{QStringLiteral("group"), group}},
             operation, [projectId, complete = std::move(complete)](QJsonValue data) {
        if (!data.isArray())
            return complete(std::nullopt);
        QSet<QString> seen;
        QVector<QPair<qint64, Stage>> ordered;
        for (const auto value : data.toArray()) {
            const auto row = value.toObject();
            const auto id = numericId(field(row, {"id"}));
            const auto title = field(row, {"title", "name"});
            const auto groupId = field(row, {"groupId", "GROUP_ID"});
            if (id.isEmpty() || title.isEmpty() || seen.contains(id)
                || (!groupId.isEmpty() && numericId(groupId) != projectId))
                continue;
            seen.insert(id);
            ordered.append({row.value(QStringLiteral("sort")).toInteger(),
                            {id, title, {}, std::nullopt, false, stageColorFrom(row)}});
            if (ordered.size() >= tunables::kPageSizeBoardTasks)
                break;
        }
        std::stable_sort(ordered.begin(), ordered.end(), [](const auto &left, const auto &right) {
            return left.first < right.first;
        });
        QVector<Stage> stages;
        stages.reserve(ordered.size());
        for (auto &entry : ordered)
            stages.append(std::move(entry.second));
        complete(std::move(stages));
    });
}

void ProjectsService::loadLinkStage(const QString &projectId, const QString &stageId, std::function<void(Stage)> complete)
{
    if (numericId(stageId).isEmpty() || m_host.authExpired()) {
        complete({});
        return;
    }
    if (projectId == m_openBoard) {
        const auto stage = m_boardColumnCursors.constFind(stageId);
        if (stage != m_boardColumnCursors.cend() && !stage->stageColor.isEmpty()) {
            complete({stageId, stage->stageName, {}, std::nullopt, false, stage->stageColor});
            return;
        }
    }
    const auto scope = m_host.settingsScope();
    const auto key = QString::number(m_host.sessionRevision()) + QLatin1Char(':') + scope + QLatin1Char(':') + projectId;
    auto deliver = [stageId, complete = std::move(complete)](const QVector<Stage> &stages) {
        const auto stage = std::find_if(stages.cbegin(), stages.cend(), [&stageId](const Stage &entry) {
            return entry.id == stageId;
        });
        complete(stage == stages.cend() ? Stage{} : *stage);
    };
    if (const auto *snapshot = m_linkStageSnapshots.object(key);
        snapshot && snapshot->expiresAt > QDateTime::currentMSecsSinceEpoch()) {
        deliver(snapshot->stages);
        return;
    }
    auto &pending = m_pendingLinkStages[key];
    if (pending.size() >= tunables::kPageSizeProjects) {
        deliver({});
        return;
    }
    pending.append(std::move(deliver));
    if (pending.size() > 1)
        return;
    loadProjectStages(projectId, operationKey(OperationKind::LinkPreviewStage, key),
        [this, key, scope](std::optional<QVector<Stage>> stages) {
            const auto pending = m_pendingLinkStages.take(key);
            if (m_host.authExpired() || m_host.settingsScope() != scope)
                stages.reset();
            const auto entries = stages.value_or(QVector<Stage>{});
            constexpr int stageSnapshotTtlMs = 60 * 1000;
            if (stages)
                m_linkStageSnapshots.insert(key, new LinkStageSnapshot{
                    entries, QDateTime::currentMSecsSinceEpoch() + stageSnapshotTtlMs});
            for (const auto &complete : pending)
                complete(entries);
        });
}

ProjectsService::Fields ProjectsService::boardFields(const QString &projectId) const
{
    Fields fields{{QStringLiteral("params[GROUP_ID]"), projectId},
                  {QStringLiteral("params[GROUP_ID_FORCED]"), projectId},
                  {QStringLiteral("params[PERSONAL]"), QStringLiteral("N")},
                  {QStringLiteral("personal"), QStringLiteral("N")}};
    if (!m_host.currentUserId().isEmpty())
        fields.append({QStringLiteral("params[USER_ID]"), m_host.currentUserId()});
    return fields;
}

void ProjectsService::boardMutation(const QString &action, const QString &projectId, Fields fields,
                                 Handler onData)
{
    fields.append(boardFields(projectId));
    if (!onData.onData)
        onData = [this, projectId](QJsonValue) { emit boardUpdated(projectId); };
    m_portalHttp.componentAction(action, fields, operationKey(OperationKind::BoardEdit, projectId), std::move(onData));
}

void ProjectsService::moveTask(const QString &projectId, const QString &taskId, const QString &stageId)
{
    const auto operation = operationKey(OperationKind::BoardEdit, projectId);
    if (numericId(projectId).isEmpty() || numericId(taskId).isEmpty() || numericId(stageId).isEmpty()) {
        emit failed(operation, QStringLiteral("Проверьте проект, задачу и стадию"));
        return;
    }
    boardMutation(QStringLiteral("moveTask"), projectId,
                  {{QStringLiteral("itemId"), taskId}, {QStringLiteral("columnId"), stageId},
                   {QStringLiteral("beforeItemId"), QStringLiteral("0")},
                   {QStringLiteral("afterItemId"), QStringLiteral("0")}},
                  [this, projectId, operation](const QJsonValue &result) {
        const bool moved = result.toBool() || result.isObject() || result.isArray()
            || result.toDouble() != 0 || !result.toString().isEmpty();
        if (!moved) {
            emit failed(operation, QStringLiteral("Портал отклонил перенос задачи"));
            return;
        }
        if (projectId == m_openBoard) {
            // The board refresh requested on confirmation needs a new snapshot and deduplication key.
            ++m_boardPageGeneration;
            m_boardColumnCursors.clear();
        }
        emit boardUpdated(projectId);
    });
}

void ProjectsService::saveStage(const QString &projectId, const QString &stageId, const QString &title,
                             const QString &afterStageId, const QString &color)
{
    const auto operation = operationKey(OperationKind::BoardEdit, projectId);
    const auto cleanTitle = title.trimmed();
    const auto requestedColor = normalizedStageColor(color);
    if (!color.isEmpty() && requestedColor.isEmpty()) {
        emit failed(operation, QStringLiteral("Укажите цвет стадии в формате RRGGBB"));
        return;
    }
    const bool colorOnly = title.isEmpty() && !stageId.isEmpty() && !requestedColor.isEmpty();
    if (numericId(projectId).isEmpty() || (cleanTitle.isEmpty() && !colorOnly)
        || (!stageId.isEmpty() && numericId(stageId).isEmpty())
        || (!afterStageId.isEmpty() && (!boardStageId(afterStageId) || afterStageId == stageId))) {
        emit failed(operation, QStringLiteral("Проверьте название и положение стадии"));
        return;
    }
    m_portalHttp.componentAction(QStringLiteral("applyFilter"), boardFields(projectId), operation,
                    [this, projectId, stageId, cleanTitle, afterStageId, requestedColor, colorOnly, operation](QJsonValue data) {
        const auto columns = data.toObject().value(QStringLiteral("columns"));
        if (!columns.isArray())
            return m_host.failShape(operation);
        QString previous = QStringLiteral("0");
        QString currentPrevious;
        QJsonObject current;
        bool predecessorExists = afterStageId.isEmpty() || afterStageId == QLatin1String("0");
        for (const auto &value : columns.toArray()) {
            const auto column = value.toObject();
            const auto id = field(column, {"id", "ID"});
            if (!boardStageId(id))
                continue;
            if (id == stageId) {
                current = column;
                currentPrevious = previous;
            }
            predecessorExists = predecessorExists || id == afterStageId;
            previous = id;
        }
        if ((!stageId.isEmpty() && current.isEmpty()) || !predecessorExists) {
            emit failed(operation, QStringLiteral("Стадия больше не доступна на этой доске"));
            return;
        }
        const auto currentTitle = field(current, {"name", "TITLE"});
        const auto nextTitle = colorOnly ? currentTitle : cleanTitle;
        if (nextTitle.isEmpty())
            return m_host.failShape(operation);
        const auto currentColor = stageColorFrom(current);
        const auto nextColor = requestedColor.isEmpty() ? currentColor : requestedColor;
        Fields fields{{QStringLiteral("fields[columnName]"), nextTitle},
                      {QStringLiteral("fields[columnColor]"), nextColor.isEmpty() ? QStringLiteral("ace9fb") : nextColor}};
        if (stageId.isEmpty()) {
            fields.append({QStringLiteral("fields[afterColumnId]"), afterStageId.isEmpty() ? previous : afterStageId});
            boardMutation(QStringLiteral("modifyColumn"), projectId, fields);
            return;
        }
        fields.append({QStringLiteral("fields[id]"), stageId});
        auto finish = [this, projectId, stageId, afterStageId, currentPrevious](QJsonValue) {
            if (!afterStageId.isEmpty() && afterStageId != currentPrevious) {
                boardMutation(QStringLiteral("moveColumn"), projectId,
                              {{QStringLiteral("columnId"), stageId},
                               {QStringLiteral("afterColumnId"), afterStageId}});
            } else {
                emit boardUpdated(projectId);
            }
        };
        if (nextTitle == currentTitle && nextColor == currentColor)
            finish({});
        else
            boardMutation(QStringLiteral("modifyColumn"), projectId, fields, std::move(finish));
    });
}

void ProjectsService::deleteStage(const QString &projectId, const QString &stageId)
{
    const auto operation = operationKey(OperationKind::BoardEdit, projectId);
    if (numericId(projectId).isEmpty() || numericId(stageId).isEmpty()) {
        emit failed(operation, QStringLiteral("Проверьте проект и стадию"));
        return;
    }
    m_portalHttp.componentAction(QStringLiteral("applyFilter"), boardFields(projectId), operation,
                    [this, projectId, stageId, operation](QJsonValue data) {
        const auto board = data.toObject();
        const auto columns = board.value(QStringLiteral("columns"));
        const auto items = board.value(QStringLiteral("items"));
        if (!columns.isArray() || !items.isArray())
            return m_host.failShape(operation);
        const auto stageRows = columns.toArray();
        const auto stage = std::find_if(stageRows.cbegin(), stageRows.cend(), [&stageId](const QJsonValue &value) {
            return field(value.toObject(), {"id", "ID"}) == stageId;
        });
        const auto taskRows = items.toArray();
        const bool listedTask = std::any_of(taskRows.cbegin(), taskRows.cend(), [&stageId](const QJsonValue &value) {
            return field(value.toObject(), {"columnId"}) == stageId;
        });
        if (stage == stageRows.cend() || stageTaskCount((*stage).toObject()) != 0 || listedTask
            || !(*stage).toObject().value(QStringLiteral("canRemove")).toBool(true)) {
            emit failed(operation, QStringLiteral("Портал не подтвердил, что стадию можно удалить без переноса задач"));
            return;
        }

        // The portal can retain a personal board filter. Check the stage again by project and stage alone.
        m_portalHttp.ajax(QStringLiteral("tasks.task.list"),
             {{QStringLiteral("filter[GROUP_ID]"), projectId}, {QStringLiteral("filter[STAGE_ID]"), stageId},
              {QStringLiteral("select[]"), QStringLiteral("ID")}, {QStringLiteral("start"), QStringLiteral("0")}},
             operation, [this, projectId, stageId, operation](QJsonValue result) {
            const auto tasks = result.toObject().value(QStringLiteral("tasks"));
            if (!tasks.isArray())
                return m_host.failShape(operation);
            if (!tasks.toArray().isEmpty()) {
                emit failed(operation, QStringLiteral("На этой стадии ещё есть задачи"));
                return;
            }
            boardMutation(QStringLiteral("modifyColumn"), projectId,
                          {{QStringLiteral("fields[id]"), stageId}, {QStringLiteral("fields[delete]"), QStringLiteral("1")}});
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

void ProjectsService::requestProjectTimeline(const QString &projectId)
{
    if (numericId(projectId).isEmpty()) {
        emit failed(operationKey(OperationKind::Timeline, projectId), QStringLiteral("Некорректный номер проекта"));
        return;
    }
    loadProjectTimeline(projectId, 0, {});
}

void ProjectsService::loadProjectTimeline(const QString &projectId, int offset, QVector<Task> tasks)
{
    const auto operation = operationKey(OperationKind::Timeline, projectId);
    Fields fields{{QStringLiteral("filter[GROUP_ID]"), projectId},
                  {QStringLiteral("order[ID]"), QStringLiteral("DESC")},
                  {QStringLiteral("start"), QString::number(offset)}};
    for (const auto *name : {"ID", "TITLE", "GROUP_ID", "START_DATE_PLAN", "END_DATE_PLAN"})
        fields.append({QStringLiteral("select[]"), QLatin1String(name)});
    m_portalHttp.ajax(QStringLiteral("tasks.task.list"), fields, operation,
         [this, projectId, offset, operation, tasks = std::move(tasks)](QJsonValue data) mutable {
        const auto rows = data.toObject().value(QStringLiteral("tasks"));
        if (!rows.isArray())
            return m_host.failShape(operation);
        const auto page = rows.toArray();
        const auto previousLength = tasks.size();
        for (const auto value : page) {
            auto task = taskFromRow(value.toObject());
            if (!task.id.isEmpty() && task.projectId == projectId &&
                std::none_of(tasks.cbegin(), tasks.cend(), [&task](const Task &other) { return other.id == task.id; }))
                tasks.append(std::move(task));
            if (tasks.size() == tunables::kPageSizeBoardTasks)
                break;
        }
        constexpr int pageSize = 50;
        const bool full = page.size() >= pageSize;
        if (full && tasks.size() > previousLength && offset + pageSize < tunables::kPageSizeBoardTasks) {
            loadProjectTimeline(projectId, offset + pageSize, std::move(tasks));
            return;
        }
        emit projectTimelineLoaded(projectId, tasks, full);
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

void ProjectsService::refreshDirtyBoard()
{
    if (m_openBoard.isEmpty() || !m_boardDirty || m_portalHttp.atCapacity()
        || m_portalHttp.contains(boardReadKey(operationKey(OperationKind::Board, m_openBoard), m_boardPageGeneration)))
        return;
    m_boardDirty = false;
    requestProjectBoard(m_openBoard);
}

void ProjectsService::handleTaskPull(const QJsonObject &fields, const QString &groupId, const QString &taskId, qint64 remembered)
{
    const auto changedProject = field(fields, {"GROUP_ID", "groupId"});
    bool hasProject = false;
    const auto project = changedProject.toLongLong(&hasProject);
    if (hasProject && !taskId.isEmpty() && project >= 0) {
        const auto chat = explicitMessageChatId(fields);
        updateProjectUnreadTask(taskId.toLongLong(), remembered > 0 ? remembered
            : hasTaskParent(chat) ? chat : 0, project);
    }
    invalidateBoardForTask(groupId, taskId);
}

void ProjectsService::invalidateBoardForTask(const QString &groupId, const QString &taskId)
{
    // After moving away, the event may identify only the destination board.
    const bool shownOnBoard = !taskId.isEmpty()
        && std::any_of(m_boardColumnCursors.cbegin(), m_boardColumnCursors.cend(),
            [&taskId](const BoardColumnCursor &cursor) { return cursor.seen.contains(taskId); });
    if (!m_openBoard.isEmpty() && (groupId.isEmpty() || groupId == m_openBoard || shownOnBoard))
        m_boardDirty = true;
}

bool ProjectsService::isCurrentResponse(OperationKind kind, const QString &key, const QString &operation) const
{
    if (kind == OperationKind::ProjectTaskChats)
        return key == m_projectTaskChatsRequest;
    if (key != operation && (kind == OperationKind::Board || kind == OperationKind::BoardColumn))
        return key == boardReadKey(operation, m_boardPageGeneration);
    return true;
}

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
