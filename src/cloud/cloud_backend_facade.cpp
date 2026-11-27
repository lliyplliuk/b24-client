#include "cloud_backend.h"
#include "cloud_chat_metadata.h"

#include <utility>

namespace bx {

bool CloudBackend::isCurrentResponse(OperationKind kind, const QString &key, const QString &operation) const
{
    return m_projects.isCurrentResponse(kind, key, operation);
}

void CloudBackend::ajax(const QString &action, Fields fields, const QString &operation, Handler handler,
                        const QString &requestKey)
{
    m_portalHttp.ajax(action, std::move(fields), operation, std::move(handler), requestKey);
}

QUrl CloudBackend::actionUrl(const QString &action) const
{
    return m_portalHttp.actionUrl(action);
}

void CloudBackend::ajaxJson(const QString &action, const QJsonObject &body, const QString &operation,
                           Handler handler, const QString &requestKey)
{
    m_portalHttp.ajaxJson(action, body, operation, std::move(handler), requestKey);
}

void CloudBackend::ajaxJson(QUrl url, const QJsonObject &body, const QString &operation,
                           Handler handler, const QString &requestKey)
{
    m_portalHttp.ajaxJson(std::move(url), body, operation, std::move(handler), requestKey);
}

void CloudBackend::componentAction(const QString &action, const Fields &fields,
                                   const QString &operation, Handler handler, const QString &component,
                                   const QString &requestKey)
{
    m_portalHttp.componentAction(action, fields, operation, std::move(handler), component, requestKey);
}

void CloudBackend::cancelSessionRecovery()
{
    m_portalHttp.cancelSessionRecovery();
}

void CloudBackend::requestRecentChats()
{
    m_chats.requestRecentChats();
}

void CloudBackend::requestTaskChats()
{
    m_chats.requestTaskChats();
}

void CloudBackend::requestTaskChatBadges()
{
    m_chats.requestTaskChatBadges();
}

void CloudBackend::requestProjectTaskChats(const QString &projectId, int offset)
{
    m_projects.requestProjectTaskChats(projectId, offset);
}

void CloudBackend::rememberTaskTitle(const QString &id, const QString &title)
{
    m_chats.rememberTaskTitle(id, title);
}

QString CloudBackend::recentTitle(const QString &dialog, const QString &fallback) const
{
    return m_chats.recentTitle(dialog, fallback);
}

void CloudBackend::notifyChatTyping(const QString &dialog)
{
    m_chats.notifyChatTyping(dialog);
}

void CloudBackend::changeMessage(const QString &dialog, const QString &id, MessageAction action, const QString &value)
{
    m_chats.changeMessage(dialog, id, action, value);
}

void CloudBackend::forwardMessage(const QString &source, const QString &id, const QString &target)
{
    m_chats.forwardMessage(source, id, target);
}

void CloudBackend::createGroupChat(const QString &title, const QStringList &members)
{
    m_chats.createGroupChat(title, members);
}

void CloudBackend::changeChatMember(const QString &dialog, const QString &user, bool add)
{
    m_chats.changeChatMember(dialog, user, add);
}

void CloudBackend::requestPinnedMessages(const QString &dialogId)
{
    m_chats.requestPinnedMessages(dialogId);
}

void CloudBackend::searchTaskChats(const QString &query)
{
    m_chats.searchTaskChats(query);
}

void CloudBackend::searchChats(const QString &query)
{
    m_chats.searchChats(query);
}

void CloudBackend::requestOlderChatMessages(const QString &scopeId, const QString &beforeId, bool task)
{
    m_chats.requestOlderChatMessages(scopeId, beforeId, task);
}

void CloudBackend::searchMessages(const QString &scopeId, const QString &query, bool task)
{
    m_chats.searchMessages(scopeId, query, task);
}

void CloudBackend::fetchPortalHtml(const QUrl &url, std::function<void(QString)> handler)
{
    m_portalHttp.fetchPortalHtml(url, std::move(handler));
}

void CloudBackend::requestProjects()
{
    m_projects.requestProjects();
}

void CloudBackend::requestMoreProjects()
{
    m_projects.requestMoreProjects();
}

void CloudBackend::requestProjectMembers(const QString &projectId)
{
    m_projects.requestProjectMembers(projectId);
}

void CloudBackend::requestProjectFiles(const QString &projectId, const QString &folderId, int offset)
{
    m_projects.requestProjectFiles(projectId, folderId, offset);
}

void CloudBackend::setChatPinned(const QString &dialogId, bool pinned)
{
    m_chats.setChatPinned(dialogId, pinned);
}

void CloudBackend::setProjectPinned(const QString &projectId, bool pinned)
{
    m_projects.setProjectPinned(projectId, pinned);
}

void CloudBackend::createProject(const QString &name)
{
    m_projects.createProject(name);
}

void CloudBackend::searchProjectTasks(const QString &projectId, const QString &query)
{
    m_projects.searchProjectTasks(projectId, query);
}

void CloudBackend::requestProjectBoard(const QString &projectId)
{
    m_projects.requestProjectBoard(projectId);
}

void CloudBackend::requestMoreBoardTasks(const QString &projectId, const QString &stageId)
{
    m_projects.requestMoreBoardTasks(projectId, stageId);
}

void CloudBackend::requestProjectStages(const QString &projectId)
{
    m_projects.requestProjectStages(projectId);
}

void CloudBackend::moveTask(const QString &projectId, const QString &taskId, const QString &stageId)
{
    m_projects.moveTask(projectId, taskId, stageId);
}

void CloudBackend::saveStage(const QString &projectId, const QString &stageId, const QString &title,
                             const QString &afterStageId, const QString &color)
{
    m_projects.saveStage(projectId, stageId, title, afterStageId, color);
}

void CloudBackend::deleteStage(const QString &projectId, const QString &stageId)
{
    m_projects.deleteStage(projectId, stageId);
}

void CloudBackend::createTask(const QString &projectId, const TaskEdit &edit)
{
    m_tasks.createTask(projectId, edit);
}

void CloudBackend::createSubtask(const QString &parentId, const QString &projectId, const TaskEdit &edit)
{
    m_tasks.createSubtask(parentId, projectId, edit);
}

void CloudBackend::updateTask(const QString &taskId, const TaskEdit &edit)
{
    m_tasks.updateTask(taskId, edit);
}

void CloudBackend::deleteTask(const QString &taskId)
{
    m_tasks.deleteTask(taskId);
}

void CloudBackend::requestTask(const QString &taskId)
{
    m_tasks.requestTask(taskId);
}

void CloudBackend::requestTaskChecklist(const QString &taskId)
{
    m_tasks.requestTaskChecklist(taskId);
}

void CloudBackend::addTaskChecklistItem(const QString &taskId, const QString &parentId, const QString &title)
{
    m_tasks.addTaskChecklistItem(taskId, parentId, title);
}

void CloudBackend::setTaskChecklistComplete(const QString &taskId, const QString &itemId, bool complete)
{
    m_tasks.setTaskChecklistComplete(taskId, itemId, complete);
}

void CloudBackend::requestSubtasks(const QString &taskId)
{
    m_tasks.requestSubtasks(taskId);
}

void CloudBackend::requestTaskDependencies(const QString &taskId)
{
    m_tasks.requestTaskDependencies(taskId);
}

void CloudBackend::requestTaskHistory(const QString &taskId)
{
    m_tasks.requestTaskHistory(taskId);
}

void CloudBackend::requestProjectTimeline(const QString &projectId)
{
    m_projects.requestProjectTimeline(projectId);
}

void CloudBackend::updateTaskChatProject(const QString &taskId, qint64 project)
{
    m_projects.updateProjectUnreadTask(taskId.toLongLong(), groupDialogChatId(m_timeline.taskDialog(taskId)), project);
}

void CloudBackend::sendChatMessage(const QString &dialogId, const QString &text)
{
    m_chats.sendChatMessage(dialogId, text);
}

void CloudBackend::sendChatReply(const QString &dialog, const QString &text, const QString &replyId)
{
    m_chats.sendChatReply(dialog, text, replyId);
}

void CloudBackend::sendTaskMessage(const QString &taskId, const QString &text)
{
    m_chats.sendTaskMessage(taskId, text);
}

void CloudBackend::sendTaskReply(const QString &taskId, const QString &text, const QString &replyId)
{
    m_chats.sendTaskReply(taskId, text, replyId);
}

void CloudBackend::requestNotifications()
{
    m_notices.requestNotifications();
}

void CloudBackend::requestOlderNotifications()
{
    m_notices.requestOlderNotifications();
}

void CloudBackend::searchNotifications(const QString &query)
{
    m_notices.searchNotifications(query);
}

void CloudBackend::markNotificationRead(const QString &id)
{
    m_notices.markNotificationRead(id);
}

void CloudBackend::markNotificationsRead(const QStringList &ids)
{
    m_notices.markNotificationsRead(ids);
}

void CloudBackend::markAllNotificationsRead()
{
    m_notices.markAllNotificationsRead();
}

void CloudBackend::deleteNotification(const QString &id)
{
    m_notices.deleteNotification(id);
}

void CloudBackend::handleTaskPull(const QJsonObject &fields, const QString &groupId, const QString &taskId, bool invalidatePreview)
{
    m_projects.handleTaskPull(fields, groupId, taskId, groupDialogChatId(m_timeline.taskDialog(taskId)));
    if (invalidatePreview)
        emit taskLinkPreviewInvalidated(taskId);
    invalidateTaskChat(taskId);
}

void CloudBackend::handlePresencePull(const QJsonObject &params, qint64 timestamp)
{
    handlePortalStatusPush(params.value(QStringLiteral("users")).toObject(), timestamp);
    handleOnlinePull(params);
}

void CloudBackend::reconcileHistoryPopups(const QString &dialog, qint64 chatId, bool task, const QJsonObject &history,
                                         const QVector<Message> &messages)
{
    m_notices.reconcileHistoryPopups(dialog, chatId, task, history, messages);
}

void CloudBackend::settleInitialPortalStatus()
{
    if (std::exchange(m_initialPortalStatusPending, false))
        m_notices.settleInitialPortalStatus();
}

void CloudBackend::deliverChatPopups()
{
    m_notices.deliverChatPopups();
}

void CloudBackend::requestNotificationSettings()
{
    m_notices.requestNotificationSettings();
}

void CloudBackend::updateNotificationSetting(const QString &name, int value)
{
    m_notices.updateNotificationSetting(name, value);
}

void CloudBackend::loadChatPopupSettings()
{
    m_notices.loadChatPopupSettings();
}

bool CloudBackend::popupWorkspaceVisible() const
{
    return m_workspaceVisible;
}

QString CloudBackend::popupPortalStatus() const
{
    return m_portalStatus;
}

bool CloudBackend::initialStatusPending() const
{
    return m_initialPortalStatusPending;
}

void CloudBackend::popupIdentityResolved()
{
    if (!m_timeline.activeDialog().isEmpty())
        m_timeline.invalidateHistory(false);
    if (!m_timeline.activeTask().isEmpty())
        m_timeline.invalidateHistory(true);
    scheduleRefresh();
}

void CloudBackend::requestChatMessages(const QString &dialogId)
{
    m_chats.requestChatMessages(dialogId);
}

void CloudBackend::requestTaskMessages(const QString &taskId)
{
    m_chats.requestTaskMessages(taskId);
}

void CloudBackend::loadChatTask(const QString &taskId, const QString &operation, std::function<void(QJsonObject)> onTask)
{
    m_tasks.loadTask(taskId, operation, std::move(onTask));
}

} // namespace bx
