#include "cloud/projects_service.h"
#include "cloud/cloud_rows.h"
#include "core/stage_color.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace bx {
using namespace cloud_rows;

namespace {

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

} // namespace

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

} // namespace bx
