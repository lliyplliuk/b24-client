#include "pilot_fixture.h"

namespace bx::test {

void fireTimer(QTimer &timer)
{
    QVERIFY(timer.isActive());
    QTimerEvent event(timer.timerId());
    QCoreApplication::sendEvent(&timer, &event);
}

void deliverPendingTimer(MainWindow &window, const QString &name)
{
    QCoreApplication::sendPostedEvents();
    auto *timer = window.findChild<QTimer *>(name);
    QVERIFY(timer);
    QCOMPARE(timer->interval(), 160);
    QVERIFY(timer->isSingleShot());
    if (timer->isActive())
        fireTimer(*timer);
}

QMenu *visibleMessageMenu(const QString &name)
{
    for (auto *window : QApplication::topLevelWidgets()) {
        if (window->isVisible() && window->objectName() == name)
            return qobject_cast<QMenu *>(window);
    }
    return nullptr;
}

void FixtureBackend::requestRecentChats()
{
    chatRefreshCalls.append(QStringLiteral("directory"));
    emit recentChatsLoaded(chats);
}

void FixtureBackend::requestTaskChats()
{
    ++taskChatRequests;
    if (!holdTaskChatDirectory)
        emit taskChatsLoaded(taskChats);
}

void FixtureBackend::requestTaskChatBadges()
{
    ++taskChatBadgeRequests;
    if (provideTaskBadges)
        emit taskChatsLoaded(taskChats);
}

void FixtureBackend::requestAvatar(const QUrl &url)
{
    avatarRequests.append(url);
    if (!holdAvatarResponses)
        emit avatarLoaded(url, avatarImages.value(url, avatarBytes));
}

void FixtureBackend::requestMessageImage(const QUrl &url)
{
    imageRequests.append(url);
    if (holdMessageImages)
        return;
    const auto bytes = messageImages.value(url);
    QTimer::singleShot(0, this, [this, url, bytes] { emit messageImageLoaded(url, bytes); });
}

void FixtureBackend::downloadMessageVideo(const QUrl &url, const QString &path)
{
    videoRequests.append({url, path});
}

void FixtureBackend::cancelMessageVideo(const QString &path)
{
    cancelledVideos.append(path);
}

void FixtureBackend::requestMessageLinkPreview(const QUrl &url, LinkPreviewHandler complete)
{
    if (holdLinkPreviews)
        linkPreviewRequests.append({url, std::move(complete)});
    else
        complete({});
}

void FixtureBackend::requestMessageViewers(const QString &scope, const QString &id, bool task, MessageViewersHandler complete)
{
    viewedReceipts.append({scope, id, task});
    if (holdViewerResponses)
        viewerReplies.append(std::move(complete));
    else
        complete(MessageViewers{{{QStringLiteral("19"), QStringLiteral("Анна Волкова"), {}, viewerAvatar}}, 1, false});
}

void FixtureBackend::requestChatLinkPreviews(const QString &scope, bool task, ChatLinkPreviewHandler complete)
{
    if (holdExternalLinkPreviews)
        externalLinkPreviewRequests.append({qMakePair(scope, task), std::move(complete)});
    else
        complete({});
}

void FixtureBackend::setChatPinned(const QString &id, bool pinned)
{
    pinTarget = id;
    pinValue = pinned;
}

void FixtureBackend::setProjectPinned(const QString &id, bool pinned)
{
    pinTarget = id;
    pinValue = pinned;
}

void FixtureBackend::requestChatMessages(const QString &chatId)
{
    chatRefreshCalls.append(chatId);
    if (chatId == QStringLiteral("c-1") || chatId == QStringLiteral("chat91"))
        emit chatMessagesLoaded(chatId, messages);
    else if (chatId == QStringLiteral("task-dialog"))
        emit chatMessagesLoaded(chatId, taskMessages);
    else if (chatId == QStringLiteral("chat42"))
        emit chatMessagesLoaded(chatId, linkedMessages);
}

void FixtureBackend::requestOlderChatMessages(const QString &scope, const QString &before, bool task)
{
    olderScope = scope;
    olderBefore = before;
    olderTask = task;
}

void FixtureBackend::readChatMessages(const QString &dialog, const QStringList &incoming, const QStringList &visible)
{
    reads.append({dialog, incoming, visible, false});
}

void FixtureBackend::readTaskChatMessages(const QString &task, const QStringList &incoming, const QStringList &visible)
{
    reads.append({task, incoming, visible, true});
}

void FixtureBackend::requestPinnedMessages(const QString &dialog)
{
    pinnedRequests.append(dialog);
    emit pinnedMessagesLoaded(dialog, pinnedMessages.value(dialog));
}

void FixtureBackend::requestMessageContext(const QString &dialogId, const QString &messageId)
{
    contextDialog = dialogId;
    contextMessage = messageId;
}

void FixtureBackend::requestPerson(const QString &id)
{
    requestedPerson = id;
    personRequests.append(id);
}

void FixtureBackend::requestCurrentPerson(PersonHandler complete)
{
    if (holdCurrentPerson)
        currentPersonRequests.append(std::move(complete));
    else
        complete(currentPerson);
}

void FixtureBackend::downloadMessageFile(const QUrl &url, const QString &path)
{
    downloadedUrl = url;
    downloadedPath = path;
    if (!holdMessageFileDownload)
        emit messageFileDownloaded(url, path);
}

void FixtureBackend::downloadMessagePhoto(const QUrl &url, const QString &path)
{
    photoUrl = url;
    photoPath = path;
    ++photoDownloads;
}

void FixtureBackend::requestChatAssets(const QString &scope, ChatAssetKind kind, const QString &cursor)
{
    emit chatAssetsLoaded(scope, kind, cursor, kind == ChatAssetKind::Media ? mediaAssets : QVector<ChatAsset>{}, {});
}

void FixtureBackend::requestProjects()
{
    ++projectRequests;
    emit projectsMoreAvailable(!moreProjects.isEmpty());
    emit projectsLoaded(projects);
}

void FixtureBackend::requestMoreProjects()
{
    ++projectMoreRequests;
    if (holdProjectMore)
        return;
    emit projectsMoreAvailable(false);
    emit projectsLoaded(projects + moreProjects);
}

void FixtureBackend::requestProjectTaskChats(const QString &projectId, int offset)
{
    projectTaskChatRequests.append({projectId, offset});
    if (!projectId.isEmpty() && !holdProjectTaskChats)
        emit projectTaskChatsLoaded(projectId, offset, {}, -1);
}

void FixtureBackend::requestProjectStages(const QString &projectId)
{
    projectStageRequests.append(projectId);
    if (!holdProjectStages)
        emit projectStagesLoaded(projectId, projectStageChoices);
}

void FixtureBackend::requestProjectTimeline(const QString &projectId)
{
    timelineRequests.append(projectId);
    emit projectTimelineLoaded(projectId, timelineTasks, timelineLimited);
}

void FixtureBackend::requestMoreBoardTasks(const QString &projectId, const QString &stageId)
{
    boardPageRequests.append({projectId, stageId});
}

void FixtureBackend::requestProjectMembers(const QString &projectId)
{
    emit projectMembersLoaded(projectId, projectToolsFixture
        ? QVector<ProjectMember>{{QStringLiteral("11"), QStringLiteral("Анна"), {}, ProjectMemberRole::Owner}}
        : QVector<ProjectMember>{});
}

void FixtureBackend::requestProjectFiles(const QString &projectId, const QString &folderId, int)
{
    if (!projectToolsFixture) {
        emit projectFilesLoaded(projectId, folderId, {}, -1);
    } else if (folderId.isEmpty()) {
        const ProjectDiskItem folder{{QStringLiteral("77"), QStringLiteral("Общие материалы"), -1, {}}, true};
        emit projectFilesLoaded(projectId, folderId, {folder}, -1);
    } else {
        emit failed(QStringLiteral("project-files:%1:%2").arg(projectId, folderId), QStringLiteral("Доступ запрещён"));
    }
}

void FixtureBackend::requestProjectBoard(const QString &projectId)
{
    ++boardRequests;
    if (projectId == QStringLiteral("101") && !holdBoardRead)
        emit boardLoaded(projectId, stages);
}

void FixtureBackend::requestTask(const QString &taskId)
{
    taskReadRequests.append(taskId);
    if (holdTaskDetails)
        return;
    for (const auto &stage : stages) {
        for (const auto &task : stage.tasks) {
            if (task.id == taskId) {
                emit taskLoaded(task);
                return;
            }
        }
    }
}

void FixtureBackend::requestTaskMessages(const QString &taskId)
{
    taskChatReadRequests.append(taskId);
    if (taskId == QStringLiteral("501"))
        emit taskMessagesLoaded(taskId, taskMessages);
}

void FixtureBackend::searchPeople(const QString &requestId, const QString &query, const QString &dialogId)
{
    lastPeopleRequestId = requestId;
    peopleRequests.append({query, dialogId});
    emit peopleFound(requestId, people);
}

void FixtureBackend::requestNotifications()
{
    ++noticeHistoryRequests;
    if (!holdNotifications)
        emit notificationsLoaded(notifications);
}

void FixtureBackend::requestOlderNotifications()
{
    ++olderNoticeRequests;
    emit notificationHistoryPageStateChanged(true, true);
}

void FixtureBackend::requestWorkday()
{
    ++workdayRequests;
    if (!holdWorkday)
        emit workdayLoaded(workdayStatus);
}

void FixtureBackend::requestNotificationSettings()
{
    ++noticeSettingsReads;
    if (noticeSettingsReadFails)
        emit failed(QStringLiteral("notification-settings"), QStringLiteral("Портал временно недоступен"));
    else
        emit notificationSettingsLoaded(noticeSettings);
}

void FixtureBackend::updateNotificationSetting(const QString &name, int value)
{
    noticeSettingWrites.append({name, value});
}

void FixtureBackend::sendChatMessage(const QString &chatId, const QString &text)
{
    ++chatSendRequests;
    sentChat = text;
    if (!holdMessageSends)
        emit messageSent(QStringLiteral("chat:") + chatId, QStringLiteral("m-1"));
}

void FixtureBackend::sendTaskMessage(const QString &taskId, const QString &text)
{
    ++taskSendRequests;
    sentTask = text;
    if (!holdMessageSends)
        emit messageSent(QStringLiteral("task:") + taskId, QStringLiteral("m-2"));
}

void FixtureBackend::sendChatReply(const QString &chatId, const QString &text, const QString &replyId)
{
    sentReplyScope = QStringLiteral("chat:") + chatId;
    sentReplyText = text;
    sentReplyId = replyId;
}

void FixtureBackend::sendTaskReply(const QString &taskId, const QString &text, const QString &replyId)
{
    sentReplyScope = QStringLiteral("task:") + taskId;
    sentReplyText = text;
    sentReplyId = replyId;
}

void FixtureBackend::updateTask(const QString &taskId, const TaskEdit &edit)
{
    ++taskUpdateRequests;
    lastUpdate = edit;
    updatedTask = taskId;
    if (!holdTaskUpdate)
        emit taskUpdated(taskId);
}

void FixtureBackend::createTask(const QString &projectId, const TaskEdit &edit)
{
    createdProject = projectId;
    lastCreate = edit;
    ++createTaskCalls;
    if (!holdTaskCreation)
        emit taskCreated(projectId, QStringLiteral("502"));
}

void FixtureBackend::createSubtask(const QString &parentId, const QString &projectId, const TaskEdit &edit)
{
    createdParent = parentId;
    createdProject = projectId;
    lastCreate = edit;
    ++createTaskCalls;
    if (!holdTaskCreation)
        emit subtaskCreated(parentId, projectId, QStringLiteral("502"));
}

void FixtureBackend::createProject(const QString &name)
{
    createdProjectName = name;
    projects.append({QStringLiteral("102"), name, {}});
    emit projectCreated(QStringLiteral("102"));
}

void FixtureBackend::moveTask(const QString &projectId, const QString &taskId, const QString &stageId)
{
    movedProject = projectId;
    movedStage = stageId;
    movedTask = taskId;
}

void FixtureBackend::saveStage(const QString &projectId, const QString &stageId, const QString &title,
               const QString &afterStageId, const QString &color)
{
    savedStage = stageId;
    savedStageTitle = title;
    savedAfterStage = afterStageId;
    savedStageColor = color;
    if (!color.isEmpty()) {
        for (auto &stage : stages) {
            if (stage.id == stageId)
                stage.color = color;
        }
    }
    emit boardUpdated(projectId);
}

void FixtureBackend::deleteStage(const QString &projectId, const QString &stageId)
{
    deletedStage = stageId;
    emit boardUpdated(projectId);
}

void FixtureBackend::searchMessages(const QString &scopeId, const QString &query, bool task)
{
    searchedScope = scopeId;
    searchedQuery = query;
    searchedTask = task;
}

void FixtureBackend::searchProjectTasks(const QString &projectId, const QString &query)
{
    searchedProject = projectId;
    searchedTaskQuery = query;
}

void FixtureBackend::uploadChatFile(const QString &scopeId, const QString &localPath, bool task)
{
    uploadedPath = localPath;
    uploadedScope = scopeId;
    if (!holdUpload)
        emit fileUploaded(scopeId, task ? QStringLiteral("task-chat") : QStringLiteral("chat"));
}

void FixtureBackend::uploadChatImage(const QString &scopeId, const QString &localPath, bool task, const QString &caption,
                     std::shared_ptr<QTemporaryDir>)
{
    ++imageUploadRequests;
    uploadedCaption = caption;
    imageUploadForTask = task;
    uploadChatFile(scopeId, localPath, task);
}

void FixtureBackend::attachTaskFile(const QString &taskId, const QString &localPath)
{
    attachedPath = localPath;
    attachedTask = taskId;
    attachmentRequests.append({taskId, localPath});
    if (holdTaskAttachment)
        return;
    if (!attachmentEmbedId.isEmpty())
        emit taskFileAttached(taskId, attachmentEmbedId);
    emit fileUploaded(taskId, QStringLiteral("task"));
}

void FixtureBackend::changeMessage(const QString &dialog, const QString &message, MessageAction action, const QString &value)
{
    messageActionDialog = dialog;
    messageActionId = message;
    messageAction = action;
    messageActionValue = value;
}

FixtureBackend *makeFixture()
{
    auto *backend = new FixtureBackend;
    Chat chat;
    chat.id = QStringLiteral("c-1");
    chat.title = QStringLiteral("Рабочий чат");
    backend->chats = {chat};
    Message message;
    message.id = QStringLiteral("m-1");
    message.author = QStringLiteral("Иван");
    message.text = QStringLiteral("Привет");
    backend->messages = {message};
    backend->taskMessages = {message};
    Project project;
    project.id = QStringLiteral("101");
    project.name = QStringLiteral("Проект один");
    backend->projects = {project};
    Stage stage;
    stage.id = QStringLiteral("601");
    stage.name = QStringLiteral("В работе");
    Task task;
    task.id = QStringLiteral("501");
    task.name = QStringLiteral("Починить кран");
    task.description = QStringLiteral("Течёт");
    task.due = QStringLiteral("2026-01-02");
    task.priority = QStringLiteral("Обычный");
    task.stageId = stage.id;
    task.stageName = stage.name;
    task.editRights.title = true;
    task.editRights.description = true;
    task.editRights.priority = true;
    task.editRights.stage = true;
    stage.tasks = {task};
    stage.taskCount = 1;
    Stage empty;
    empty.id = QStringLiteral("602");
    empty.name = QStringLiteral("Готово");
    empty.taskCount = 0;
    backend->stages = {stage, empty};
    return backend;
}

void PilotFixture::initTestCase()
{
    QVERIFY(m_settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
}

} // namespace bx::test
