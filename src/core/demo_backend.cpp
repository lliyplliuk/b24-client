#include "core/demo_backend.h"
#include "core/stage_color.h"
#include "core/tunables.h"

#include <QFileInfo>
#include <QTimer>
#include <algorithm>

namespace bx {

namespace {
QDateTime ago(int minutes) { return QDateTime::currentDateTimeUtc().addSecs(-60 * minutes); }

const QVector<Person> &demoPeople()
{
    static const QVector<Person> people{
        {QStringLiteral("1"), QStringLiteral("Алексей Морозов"), {}, {}},
        {QStringLiteral("2"), QStringLiteral("Ирина Соколова"), {}, {}},
        {QStringLiteral("3"), QStringLiteral("Тимлид"), {}, {}}
    };
    return people;
}

QVector<Stage> demoBoard()
{
    QVector<Stage> stages;
    Stage todo{QStringLiteral("s-1"), QStringLiteral("К выполнению"), {}};
    todo.tasks.append({QStringLiteral("t-1"), QStringLiteral("Согласовать схему интеграции"),
                       QStringLiteral("s-1"), QStringLiteral("К выполнению"), QStringLiteral("Ирина Соколова"),
                       QStringLiteral("Описать обмен данными между порталом и пилотом."),
                       QStringLiteral("2026-11-25"), QStringLiteral("Обычный"), {}});
    Stage work{QStringLiteral("s-2"), QStringLiteral("В работе"), {}};
    work.tasks.append({QStringLiteral("t-2"), QStringLiteral("Собрать тестовый стенд"),
                       QStringLiteral("s-2"), QStringLiteral("В работе"), demoPeople().first().name,
                       QStringLiteral("Локальный HTTP/WS-фикстур с синтетическими данными."),
                       QStringLiteral("2026-11-22"), QStringLiteral("Высокий"), {}});
    Stage done{QStringLiteral("s-3"), QStringLiteral("Готово"), {}};
    done.tasks.append({QStringLiteral("t-3"), QStringLiteral("Описать web-контракты"),
                       QStringLiteral("s-3"), QStringLiteral("Готово"), QStringLiteral("Тимлид"),
                       QStringLiteral("Зафиксировать известные контракты и их источники."),
                       QStringLiteral("2026-11-18"), QStringLiteral("Обычный"), {}});
    stages.append(todo);
    stages.append(work);
    stages.append(done);
    return stages;
}
void applyEdit(Task &task, const TaskEdit &edit)
{
    if (edit.title)
        task.name = edit.title->trimmed();
    if (edit.description)
        task.description = *edit.description;
    if (edit.deadline)
        task.due = *edit.deadline;
    if (edit.highPriority)
        task.priority = *edit.highPriority ? QStringLiteral("Высокий") : QStringLiteral("Обычный");
}

QString demoPersonName(const QString &id)
{
    const auto &people = demoPeople();
    const auto person = std::find_if(people.cbegin(), people.cend(), [&id](const Person &entry) { return entry.id == id; });
    return person == people.cend() ? QString() : person->name;
}

std::optional<QVector<TaskPerson>> demoObservers(const QStringList &ids)
{
    QVector<TaskPerson> observers;
    for (const auto &id : ids) {
        const auto name = demoPersonName(id);
        if (name.isEmpty())
            return std::nullopt;
        observers.append({id, name});
    }
    return observers;
}

void setDemoObservers(Task &task, QVector<TaskPerson> observers)
{
    task.observerPeople = std::move(observers);
    task.observers.clear();
    for (const auto &person : task.observerPeople)
        task.observers.append(person.name);
}

void setDemoTaskRights(Task &task)
{
    task.editRights = {true, true, true, true, true, true, true, true, true};
    if (task.statusCode == QLatin1String("completed") || task.statusCode == QLatin1String("deferred")) {
        task.statusActions = {TaskStatusAction::Renew};
    } else {
        task.statusActions = {task.statusCode == QLatin1String("in_progress") ? TaskStatusAction::Pause : TaskStatusAction::Start,
                              TaskStatusAction::Complete, TaskStatusAction::Defer};
    }
}

auto stageAt(QVector<Stage> &stages, const QString &id)
{
    return std::find_if(stages.begin(), stages.end(), [&id](const Stage &stage) { return stage.id == id; });
}

template<typename T>
QVector<T> latest(const QVector<T> &items, qsizetype limit)
{
    return items.size() > limit ? items.last(limit) : items;
}
} // namespace

DemoBackend::DemoBackend(QObject *parent) : Backend(parent)
{
    m_projects = {{QStringLiteral("p-1"), QStringLiteral("Внедрение CRM"), QStringLiteral("Клиенты")},
                  {QStringLiteral("p-2"), QStringLiteral("Портал поддержки"), QStringLiteral("Внутренние")}};
    m_boards.insert(m_projects.first().id, demoBoard());
    for (auto &stage : m_boards[m_projects.first().id]) {
        for (auto &task : stage.tasks) {
            task.projectId = m_projects.first().id;
            task.project = m_projects.first().name;
            task.creatorId = demoPeople().first().id;
            task.createdBy = demoPeople().first().name;
            task.createdAt = ago(180);
            for (const auto &person : demoPeople()) {
                if (task.assignee == person.name)
                    task.assigneeId = person.id;
            }
            task.statusCode = QStringLiteral("pending");
            task.status = QStringLiteral("Ждёт выполнения");
            setDemoTaskRights(task);
        }
    }
    auto emptyBoard = demoBoard();
    for (auto &stage : emptyBoard)
        stage.tasks.clear();
    m_boards.insert(m_projects.last().id, emptyBoard);
    m_boards.insert(QString(), {Stage{}}); // A subtask without a project still has a card and chat.
    m_chats = {{QStringLiteral("c-1"), QStringLiteral("Отдел продаж"), QStringLiteral("chat"), 2,
                QStringLiteral("Клиент просит счёт"), ago(3)},
               {QStringLiteral("c-2"), QStringLiteral("Разработка"), QStringLiteral("group"), 0,
                QStringLiteral("Сборка прошла успешно"), ago(28)},
               {QStringLiteral("c-3"), QStringLiteral("Ирина Соколова"), QStringLiteral("im"), 1,
                QStringLiteral("Посмотри, пожалуйста, ТЗ"), ago(95)}};
    const QVector<Message> chatMessages{
        {QStringLiteral("m-1"), QStringLiteral("Ирина Соколова"),
         QStringLiteral("Добрый день! Отправьте, пожалуйста, счёт за август."), ago(12), false, {}},
        {QStringLiteral("m-2"), QStringLiteral("Вы"),
         QStringLiteral("Добрый день! Счёт подготовлю до конца дня."), ago(9), true, {}},
        {QStringLiteral("m-3"), QStringLiteral("Ирина Соколова"), QStringLiteral("Спасибо, жду."), ago(4), false, {}}};
    for (const auto &chat : m_chats)
        m_chatMessages.insert(chat.id, chatMessages);
    const auto addTaskMessages = [this](const QString &id, const QString &first, const QString &second) {
        m_taskMessages.insert(id, {{id + QStringLiteral("-m1"), QStringLiteral("Ирина Соколова"), first, ago(50), false, {}},
                                   {id + QStringLiteral("-m2"), QStringLiteral("Вы"), second, ago(40), true, {}}});
    };
    addTaskMessages(QStringLiteral("t-1"), QStringLiteral("Схема готова. Проверь, пожалуйста, какие данные нужны отделу продаж."),
                    QStringLiteral("Проверю и напишу здесь, если чего-то не хватает."));
    addTaskMessages(QStringLiteral("t-2"), QStringLiteral("На стенде нужно проверить чаты и доску проекта."),
                    QStringLiteral("Стенд уже поднят. Можно начинать проверку."));
    addTaskMessages(QStringLiteral("t-3"), QStringLiteral("Добавила в описание ответы сервера и примеры запросов."),
                    QStringLiteral("Посмотрел. Теперь можно закрыть задачу."));
    for (const auto &stage : m_boards.value(m_projects.first().id)) {
        for (const auto &task : stage.tasks) {
            const auto history = m_taskMessages.value(task.id);
            m_taskChats.append({task.id, task.name, QStringLiteral("tasksTask"), 0,
                                history.last().text, history.last().createdAt, true, task.id, {}});
        }
    }
}

void DemoBackend::emitLater(const std::function<void()> &fn) {
    QTimer::singleShot(0, this, fn);
}

void DemoBackend::start() {
    emitLater([this] { emit connectionChanged(true); });
}

void DemoBackend::stop() {
    emitLater([this] { emit connectionChanged(false); });
}

void DemoBackend::requestRecentChats() {
    emitLater([this] { emit recentChatsLoaded(latest(m_chats, tunables::kPageSizeChats)); });
}

void DemoBackend::requestTaskChats()
{
    emitLater([this] { emit taskChatsLoaded(latest(m_taskChats, tunables::kPageSizeChats)); });
}

void DemoBackend::requestChatMessages(const QString &chatId)
{
    emitLater([this, chatId] {
        const auto &history = m_taskMessages.contains(chatId) ? m_taskMessages : m_chatMessages;
        emit chatMessagesLoaded(chatId, latest(history.value(chatId), tunables::kPageSizeMessages));
    });
}

void DemoBackend::requestProjects()
{
    emitLater([this] { emit projectsLoaded(latest(m_projects, tunables::kPageSizeProjects)); });
}

void DemoBackend::requestProjectBoard(const QString &projectId)
{
    emitLater([this, projectId] {
        if (projectId.isEmpty() || !m_boards.contains(projectId)) {
            emit failed(QStringLiteral("board:") + projectId, QStringLiteral("Проект не найден"));
            return;
        }
        auto stages = m_boards.value(projectId);
        for (auto &stage : stages)
            stage.taskCount = stage.tasks.size();
        emit boardLoaded(projectId, stages);
    });
}

void DemoBackend::requestProjectTimeline(const QString &projectId)
{
    emitLater([this, projectId] {
        const auto board = m_boards.constFind(projectId);
        if (projectId.isEmpty() || board == m_boards.cend()) {
            emit failed(QStringLiteral("timeline:") + projectId, QStringLiteral("Проект не найден"));
            return;
        }
        QVector<Task> tasks;
        for (const auto &stage : board.value())
            tasks.append(stage.tasks);
        emit projectTimelineLoaded(projectId, tasks, false);
    });
}

void DemoBackend::requestTask(const QString &taskId)
{
    emitLater([this, taskId] {
        if (auto *task = findTask(taskId)) {
            emit taskLoaded(*task);
            return;
        }
        emit failed(QStringLiteral("task:") + taskId, QStringLiteral("Задача не найдена"));
    });
}

void DemoBackend::requestSubtasks(const QString &parentId)
{
    emitLater([this, parentId] {
        QVector<Task> subtasks;
        for (const auto &id : m_subtaskIds.value(parentId)) {
            if (const auto *task = findTask(id))
                subtasks.append(*task);
        }
        emit subtasksLoaded(parentId, subtasks);
    });
}

void DemoBackend::requestProjectStages(const QString &projectId)
{
    emitLater([this, projectId] {
        QVector<Stage> stages;
        for (const auto &stage : m_boards.value(projectId)) {
            if (!stage.id.isEmpty())
                stages.append({stage.id, stage.name, {}});
        }
        emit projectStagesLoaded(projectId, stages);
    });
}

void DemoBackend::requestTaskMessages(const QString &taskId)
{
    emitLater([this, taskId] {
        if (!m_taskMessages.contains(taskId)) {
            emit failed(QStringLiteral("task-messages:") + taskId, QStringLiteral("Чат задачи не найден"));
            return;
        }
        emit taskMessagesLoaded(taskId, latest(m_taskMessages.value(taskId), tunables::kPageSizeMessages));
    });
}

void DemoBackend::requestNotifications() {
    emitLater([this] {
        QVector<Notification> notifications;
        notifications.append({QStringLiteral("n-1"), QStringLiteral("message"),
                             QStringLiteral("Новое сообщение"),
                             QStringLiteral("Ирина Соколова написала в чат «Отдел продаж»."), ago(3), false});
        notifications.append({QStringLiteral("n-2"), QStringLiteral("task"),
                             QStringLiteral("Задача переведена в работу"),
                             QStringLiteral("«Собрать тестовый стенд» переведена в «В работе»."), ago(60), true});
        if (notifications.size() > tunables::kPageSizeNotifications)
            notifications.resize(tunables::kPageSizeNotifications);
        emit notificationsLoaded(notifications);
    });
}

Task *DemoBackend::findTask(const QString &taskId)
{
    for (auto &stages : m_boards) {
        for (auto &stage : stages) {
            for (auto &task : stage.tasks) {
                if (task.id == taskId)
                    return &task;
            }
        }
    }
    return nullptr;
}

void DemoBackend::sendMessage(const QString &scopeId, const QString &text, bool task)
{
    emitLater([this, scopeId, text, task] {
        const bool taskChat = task || m_taskMessages.contains(scopeId);
        auto &history = taskChat ? m_taskMessages : m_chatMessages;
        const auto scope = (task ? QStringLiteral("task:") : QStringLiteral("chat:")) + scopeId;
        if (text.trimmed().isEmpty() || !history.contains(scopeId)) {
            emit failed(QStringLiteral("send:") + scope, QStringLiteral("Сообщение не отправлено"));
            return;
        }
        const auto id = QStringLiteral("demo-%1").arg(++m_nextMessageId);
        const auto now = QDateTime::currentDateTimeUtc();
        auto &messages = history[scopeId];
        messages.append({id, QStringLiteral("Вы"), text, now, true, {}});
        auto &chats = taskChat ? m_taskChats : m_chats;
        for (auto &chat : chats) {
            if (chat.id == scopeId) {
                chat.lastMessage = text;
                chat.updated = now;
                break;
            }
        }
        emit messageSent(scope, id);
        const auto page = latest(messages, tunables::kPageSizeMessages);
        emit chatMessagesLoaded(scopeId, page);
        if (taskChat) {
            emit taskMessagesLoaded(scopeId, page);
            emit taskChatsLoaded(latest(chats, tunables::kPageSizeChats));
        } else {
            emit recentChatsLoaded(latest(chats, tunables::kPageSizeChats));
        }
    });
}

void DemoBackend::sendChatMessage(const QString &chatId, const QString &text)
{
    sendMessage(chatId, text, false);
}

void DemoBackend::sendTaskMessage(const QString &taskId, const QString &text)
{
    sendMessage(taskId, text, true);
}

void DemoBackend::updateTask(const QString &taskId, const TaskEdit &edit)
{
    emitLater([this, taskId, edit] {
        auto *task = findTask(taskId);
        if (!task || (edit.title && edit.title->trimmed().isEmpty())) {
            emit failed(QStringLiteral("update-task:") + taskId, QStringLiteral("Проверьте задачу и её название"));
            return;
        }
        const auto operation = QStringLiteral("update-task:") + taskId;
        const auto creator = edit.creatorId ? demoPersonName(*edit.creatorId) : QString();
        const auto assignee = edit.assigneeId ? demoPersonName(*edit.assigneeId) : QString();
        if ((edit.creatorId && creator.isEmpty()) || (edit.assigneeId && assignee.isEmpty())
            || (edit.statusAction && !task->statusActions.contains(*edit.statusAction))) {
            emit failed(operation, QStringLiteral("Реквизит задачи недоступен"));
            return;
        }
        auto observers = demoObservers(edit.observerIds.value_or(QStringList()));
        if (!observers) {
            emit failed(operation, QStringLiteral("Наблюдатель не найден"));
            return;
        }
        if ((edit.projectId && *edit.projectId != task->projectId) || edit.stageId) {
            const auto projectId = edit.projectId.value_or(task->projectId);
            auto sourceBoard = m_boards.find(task->projectId);
            auto targetBoard = m_boards.find(projectId);
            if (sourceBoard == m_boards.end() || targetBoard == m_boards.end() || targetBoard->isEmpty()) {
                emit failed(operation, QStringLiteral("Проект не найден"));
                return;
            }
            auto source = stageAt(*sourceBoard, task->stageId);
            auto target = edit.stageId ? stageAt(*targetBoard, *edit.stageId) : targetBoard->begin();
            if (source == sourceBoard->end() || target == targetBoard->end()) {
                emit failed(operation, QStringLiteral("Стадия не найдена"));
                return;
            }
            if (projectId != task->projectId || source->id != target->id) {
                const auto position = std::find_if(source->tasks.cbegin(), source->tasks.cend(), [&taskId](const Task &entry) {
                    return entry.id == taskId;
                });
                if (position == source->tasks.cend()) {
                    emit failed(operation, QStringLiteral("Задача не найдена в стадии"));
                    return;
                }
                auto relocated = source->tasks.takeAt(position - source->tasks.cbegin());
                relocated.projectId = projectId;
                relocated.project.clear();
                for (const auto &project : m_projects) {
                    if (project.id == projectId)
                        relocated.project = project.name;
                }
                relocated.stageId = target->id;
                relocated.stageName = target->name;
                target->tasks.append(std::move(relocated));
                task = &target->tasks.last();
            }
        }
        if (edit.creatorId) {
            task->creatorId = *edit.creatorId;
            task->createdBy = creator;
        }
        if (edit.assigneeId) {
            task->assigneeId = *edit.assigneeId;
            task->assignee = assignee;
        }
        if (edit.observerIds)
            setDemoObservers(*task, std::move(*observers));
        if (edit.statusAction) {
            switch (*edit.statusAction) {
            case TaskStatusAction::Start:
                task->statusCode = QStringLiteral("in_progress");
                task->status = QStringLiteral("Выполняется");
                break;
            case TaskStatusAction::Complete:
                task->statusCode = QStringLiteral("completed");
                task->status = QStringLiteral("Завершена");
                break;
            case TaskStatusAction::Defer:
                task->statusCode = QStringLiteral("deferred");
                task->status = QStringLiteral("Отложена");
                break;
            default:
                task->statusCode = QStringLiteral("pending");
                task->status = QStringLiteral("Ждёт выполнения");
                break;
            }
            setDemoTaskRights(*task);
        }
        applyEdit(*task, edit);
        for (auto &chat : m_taskChats) {
            if (chat.taskId == taskId)
                chat.title = task->name;
        }
        emit taskUpdated(taskId);
    });
}

void DemoBackend::requestCurrentPerson(PersonHandler complete)
{
    emitLater([complete = std::move(complete)] { complete(demoPeople().first()); });
}

void DemoBackend::searchPeople(const QString &requestId, const QString &query, const QString &)
{
    emitLater([this, requestId, query] {
        QVector<Person> matches;
        for (const auto &person : demoPeople()) {
            if (person.name.contains(query, Qt::CaseInsensitive))
                matches.append(person);
        }
        emit peopleFound(requestId, matches);
    });
}

void DemoBackend::createTask(const QString &projectId, const TaskEdit &edit)
{
    createTaskInBoard(projectId, {}, edit);
}

void DemoBackend::createSubtask(const QString &parentId, const QString &projectId, const TaskEdit &edit)
{
    if (parentId.isEmpty()) {
        emitLater([this] { emit failed(QStringLiteral("create-subtask:"), QStringLiteral("Родительская задача не найдена")); });
        return;
    }
    createTaskInBoard(projectId, parentId, edit);
}

void DemoBackend::createTaskInBoard(const QString &projectId, const QString &parentId, const TaskEdit &edit)
{
    emitLater([this, projectId, parentId, edit] {
        const bool child = !parentId.isEmpty();
        const auto operation = child ? QStringLiteral("create-subtask:") + parentId : QStringLiteral("create-task:") + projectId;
        if (child && !findTask(parentId)) {
            emit failed(operation, QStringLiteral("Родительская задача не найдена"));
            return;
        }
        auto board = m_boards.find(projectId);
        if ((!child && projectId.isEmpty()) || board == m_boards.end() || board->isEmpty()
            || !edit.title || edit.title->trimmed().isEmpty()) {
            emit failed(operation, QStringLiteral("Проверьте проект и название задачи"));
            return;
        }
        const auto stage = edit.stageId ? stageAt(*board, *edit.stageId) : board->begin();
        if (stage == board->end() || (edit.stageId && projectId.isEmpty())) {
            emit failed(operation, QStringLiteral("Стадия не найдена"));
            return;
        }
        Task task;
        const auto currentUser = demoPeople().first().id;
        task.creatorId = edit.creatorId.value_or(currentUser);
        task.assigneeId = edit.assigneeId.value_or(currentUser);
        task.createdBy = demoPersonName(task.creatorId);
        task.assignee = demoPersonName(task.assigneeId);
        auto observers = demoObservers(edit.observerIds.value_or(QStringList()));
        if (task.createdBy.isEmpty() || task.assignee.isEmpty() || !observers) {
            emit failed(operation, QStringLiteral("Участник задачи не найден"));
            return;
        }
        setDemoObservers(task, std::move(*observers));
        task.id = QStringLiteral("t-%1").arg(++m_nextTaskId);
        task.stageId = stage->id;
        task.stageName = stage->name;
        task.createdAt = QDateTime::currentDateTimeUtc();
        task.projectId = projectId;
        for (const auto &project : m_projects) {
            if (project.id == projectId)
                task.project = project.name;
        }
        task.statusCode = QStringLiteral("pending");
        task.status = QStringLiteral("Ждёт выполнения");
        setDemoTaskRights(task);
        task.priority = QStringLiteral("Обычный");
        applyEdit(task, edit);
        stage->tasks.append(task);
        m_taskMessages.insert(task.id, {});
        m_taskChats.append({task.id, task.name, QStringLiteral("tasksTask"), 0, {}, {}, true, task.id, {}});
        if (child) {
            m_subtaskIds[parentId].append(task.id);
            emit subtaskCreated(parentId, projectId, task.id);
        } else {
            emit taskCreated(projectId, task.id);
        }
    });
}

void DemoBackend::createProject(const QString &name)
{
    emitLater([this, name] {
        if (name.trimmed().isEmpty()) {
            emit failed(QStringLiteral("create-project"), QStringLiteral("Введите название проекта"));
            return;
        }
        const auto id = QStringLiteral("p-%1").arg(++m_nextProjectId);
        m_projects.append({id, name.trimmed(), {}});
        m_boards.insert(id, {{QStringLiteral("s-%1").arg(++m_nextStageId), QStringLiteral("К выполнению"), {}}});
        emit projectCreated(id);
    });
}

void DemoBackend::moveTask(const QString &projectId, const QString &taskId, const QString &stageId)
{
    emitLater([this, projectId, taskId, stageId] {
        auto board = m_boards.find(projectId);
        if (board != m_boards.end()) {
            const auto target = stageAt(*board, stageId);
            if (target != board->end()) {
                for (auto &source : *board) {
                    for (qsizetype i = 0; i < source.tasks.size(); ++i) {
                        if (source.tasks.at(i).id != taskId)
                            continue;
                        if (source.id != stageId) {
                            auto task = source.tasks.takeAt(i);
                            task.stageId = target->id;
                            task.stageName = target->name;
                            target->tasks.append(std::move(task));
                        }
                        emit boardUpdated(projectId);
                        return;
                    }
                }
            }
        }
        emit failed(QStringLiteral("board-edit:") + projectId, QStringLiteral("Задача или стадия не найдена"));
    });
}

void DemoBackend::saveStage(const QString &projectId, const QString &stageId, const QString &title,
                            const QString &afterStageId, const QString &color)
{
    emitLater([this, projectId, stageId, title, afterStageId, color] {
        auto board = m_boards.find(projectId);
        const auto op = QStringLiteral("board-edit:") + projectId;
        const auto requestedColor = normalizedStageColor(color);
        if (!color.isEmpty() && requestedColor.isEmpty()) {
            emit failed(op, QStringLiteral("Укажите цвет стадии в формате RRGGBB"));
            return;
        }
        const auto cleanTitle = title.trimmed();
        const bool colorOnly = title.isEmpty() && !stageId.isEmpty() && !requestedColor.isEmpty();
        if (board == m_boards.end() || (cleanTitle.isEmpty() && !colorOnly)) {
            emit failed(op, QStringLiteral("Проверьте проект и название стадии"));
            return;
        }
        auto source = stageAt(*board, stageId);
        const bool hasPredecessor = !afterStageId.isEmpty() && afterStageId != QLatin1String("0");
        if ((!stageId.isEmpty() && source == board->end())
            || (hasPredecessor && (afterStageId == stageId || stageAt(*board, afterStageId) == board->end()))) {
            emit failed(op, QStringLiteral("Стадия не найдена"));
            return;
        }
        if (source == board->end()) {
            const auto target = hasPredecessor ? stageAt(*board, afterStageId) + 1
                : afterStageId.isEmpty() ? board->end() : board->begin();
            const auto newColor = requestedColor.isEmpty() ? QStringLiteral("ace9fb") : requestedColor;
            board->insert(target, {QStringLiteral("s-%1").arg(++m_nextStageId), cleanTitle, {},
                                   std::nullopt, false, newColor});
        } else {
            if (!colorOnly) {
                source->name = cleanTitle;
                for (auto &task : source->tasks)
                    task.stageName = source->name;
            }
            if (!requestedColor.isEmpty())
                source->color = requestedColor;
            if (!afterStageId.isEmpty()) {
                auto stage = std::move(*source);
                board->erase(source);
                const auto target = hasPredecessor ? stageAt(*board, afterStageId) + 1 : board->begin();
                board->insert(target, std::move(stage));
            }
        }
        emit boardUpdated(projectId);
    });
}

void DemoBackend::deleteStage(const QString &projectId, const QString &stageId)
{
    emitLater([this, projectId, stageId] {
        auto board = m_boards.find(projectId);
        if (board != m_boards.end()) {
            const auto stage = stageAt(*board, stageId);
            if (stage != board->end() && stage->tasks.isEmpty() && board->size() > 1) {
                board->erase(stage);
                emit boardUpdated(projectId);
                return;
            }
        }
        emit failed(QStringLiteral("board-edit:") + projectId, QStringLiteral("Стадию нельзя удалить"));
    });
}

void DemoBackend::searchChats(const QString &query)
{
    emitLater([this, query] {
        QVector<Chat> result;
        for (const auto &chat : m_chats) {
            if (chat.title.contains(query.trimmed(), Qt::CaseInsensitive))
                result.append(chat);
        }
        emit chatsFound(query, latest(result, tunables::kPageSizeChats));
    });
}

void DemoBackend::searchTaskChats(const QString &query)
{
    emitLater([this, query] {
        QVector<Chat> result;
        for (const auto &chat : m_taskChats) {
            if (chat.title.contains(query.trimmed(), Qt::CaseInsensitive))
                result.append(chat);
        }
        emit taskChatsFound(query, latest(result, tunables::kPageSizeChats));
    });
}

void DemoBackend::setChatPinned(const QString &dialogId, bool pinned)
{
    emitLater([this, dialogId, pinned] {
        for (auto *chats : {&m_chats, &m_taskChats}) {
            for (auto &chat : *chats) {
                if (chat.id == dialogId) {
                    chat.pinned = pinned;
                    emit chatPinChanged(dialogId, pinned);
                    return;
                }
            }
        }
        emit failed(QStringLiteral("chat-pin:") + dialogId, QStringLiteral("Чат не найден"));
    });
}

void DemoBackend::setProjectPinned(const QString &projectId, bool pinned)
{
    emitLater([this, projectId, pinned] {
        for (auto &project : m_projects) {
            if (project.id == projectId) {
                project.pinned = pinned;
                emit projectPinChanged(projectId, pinned);
                return;
            }
        }
        emit failed(QStringLiteral("project-pin:") + projectId, QStringLiteral("Проект не найден"));
    });
}

void DemoBackend::searchProjectTasks(const QString &projectId, const QString &query)
{
    emitLater([this, projectId, query] {
        const auto board = m_boards.constFind(projectId);
        if (board == m_boards.cend()) {
            emit failed(QStringLiteral("search-project-tasks:") + projectId, QStringLiteral("Проект не найден"));
            return;
        }
        const auto needle = query.trimmed();
        QVector<Task> result;
        if (!needle.isEmpty()) {
            const auto id = needle.startsWith(QLatin1Char('#')) ? needle.mid(1) : QString();
            for (const auto &stage : *board) {
                for (const auto &task : stage.tasks) {
                    if ((!id.isEmpty() && task.id == id) || (id.isEmpty() && task.name.contains(needle, Qt::CaseInsensitive)))
                        result.append(task);
                    if (result.size() == tunables::kPageSizeBoardTasks) {
                        emit projectTasksFound(projectId, query, result);
                        return;
                    }
                }
            }
        }
        emit projectTasksFound(projectId, query, result);
    });
}

void DemoBackend::searchMessages(const QString &scopeId, const QString &query, bool task)
{
    emitLater([this, scopeId, query, task] {
        const auto &history = task ? m_taskMessages : m_chatMessages;
        const auto items = history.constFind(scopeId);
        if (items == history.cend()) {
            emit failed((task ? QStringLiteral("search-task-chat:") : QStringLiteral("search-chat:")) + scopeId,
                        QStringLiteral("Чат не найден"));
            return;
        }
        const auto needle = query.trimmed();
        QVector<Message> result;
        for (const auto &message : *items) {
            if (message.text.contains(needle, Qt::CaseInsensitive)
                || std::any_of(message.attachments.cbegin(), message.attachments.cend(),
                               [&needle](const QString &name) { return name.contains(needle, Qt::CaseInsensitive); }))
                result.append(message);
        }
        emit messagesFound(scopeId, query, task, latest(result, tunables::kPageSizeMessages));
    });
}

void DemoBackend::uploadChatFile(const QString &scopeId, const QString &localPath, bool task)
{
    emitLater([this, scopeId, localPath, task] {
        auto &history = task ? m_taskMessages : m_chatMessages;
        const QFileInfo file(localPath);
        if (!file.isFile() || !file.isReadable() || !history.contains(scopeId)) {
            emit failed((task ? QStringLiteral("upload-task-chat:") : QStringLiteral("upload-chat:")) + scopeId,
                        QStringLiteral("Файл или чат недоступен"));
            return;
        }
        const auto id = QStringLiteral("demo-%1").arg(++m_nextMessageId);
        history[scopeId].append({id, QStringLiteral("Вы"), {}, QDateTime::currentDateTimeUtc(), true, {file.fileName()}});
        emit fileUploaded(scopeId, task ? QStringLiteral("task-chat") : QStringLiteral("chat"));
    });
}

void DemoBackend::attachTaskFile(const QString &taskId, const QString &localPath)
{
    emitLater([this, taskId, localPath] {
        const QFileInfo file(localPath);
        auto *task = findTask(taskId);
        if (!file.isFile() || !file.isReadable() || !task) {
            emit failed(QStringLiteral("attach-task:") + taskId, QStringLiteral("Файл или задача недоступны"));
            return;
        }
        task->attachments.append(file.fileName());
        emit fileUploaded(taskId, QStringLiteral("task"));
    });
}

} // namespace bx
