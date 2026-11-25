#pragma once

#include "core/backend.h"

#include <QHash>

#include <functional>

namespace bx {

// Synthetic backend used by the demo mode. No network, no account, no portal.
class DemoBackend final : public Backend {
    Q_OBJECT
public:
    explicit DemoBackend(QObject *parent = nullptr);

    QString displayName() const override { return QStringLiteral("Демо-режим (синтетические данные)"); }
    bool isDemo() const override { return true; }
    QUrl portalOrigin() const override { return {}; }
    void start() override;
    void stop() override;

    void requestRecentChats() override;
    void requestTaskChats() override;
    void requestTaskChatBadges() override { requestTaskChats(); }
    void requestChatMessages(const QString &chatId) override;
    void requestProjects() override;
    void requestProjectBoard(const QString &projectId) override;
    void requestProjectTimeline(const QString &projectId) override;
    void requestProjectStages(const QString &projectId) override;
    void requestTask(const QString &taskId) override;
    void requestSubtasks(const QString &parentId) override;
    void requestTaskMessages(const QString &taskId) override;
    void requestNotifications() override;
    void sendChatMessage(const QString &chatId, const QString &text) override;
    void sendTaskMessage(const QString &taskId, const QString &text) override;
    void updateTask(const QString &taskId, const TaskEdit &edit) override;
    void createTask(const QString &projectId, const TaskEdit &edit) override;
    void createSubtask(const QString &parentId, const QString &projectId, const TaskEdit &edit) override;
    void createProject(const QString &name) override;
    void moveTask(const QString &projectId, const QString &taskId, const QString &stageId) override;
    void saveStage(const QString &projectId, const QString &stageId, const QString &title,
                   const QString &afterStageId, const QString &color = {}) override;
    void deleteStage(const QString &projectId, const QString &stageId) override;
    void searchChats(const QString &query) override;
    void searchTaskChats(const QString &query) override;
    void requestCurrentPerson(PersonHandler complete) override;
    void searchPeople(const QString &requestId, const QString &query, const QString &dialogId = {}) override;
    void setChatPinned(const QString &dialogId, bool pinned) override;
    void setProjectPinned(const QString &projectId, bool pinned) override;
    void searchProjectTasks(const QString &projectId, const QString &query) override;
    void searchMessages(const QString &scopeId, const QString &query, bool task) override;
    void uploadChatFile(const QString &scopeId, const QString &localPath, bool task) override;
    void uploadChatImage(const QString &scopeId, const QString &localPath, bool task, const QString &caption,
                         std::shared_ptr<QTemporaryDir> temporaryDirectory = {}) override;
    void attachTaskFile(const QString &taskId, const QString &localPath) override;

private:
    void uploadChatAttachment(const QString &scopeId, const QString &localPath, bool task, const QString &caption,
                              std::shared_ptr<QTemporaryDir> temporaryDirectory = {});
    void emitLater(const std::function<void()> &fn);
    Task *findTask(const QString &taskId);
    void createTaskInBoard(const QString &projectId, const QString &parentId, const TaskEdit &edit);
    void sendMessage(const QString &scopeId, const QString &text, bool task);
    int m_nextMessageId = 1000;
    int m_nextProjectId = 1000;
    int m_nextTaskId = 1000;
    int m_nextStageId = 1000;
    QVector<Project> m_projects;
    QHash<QString, QVector<Stage>> m_boards;
    QHash<QString, QStringList> m_subtaskIds;
    QVector<Chat> m_chats;
    QVector<Chat> m_taskChats;
    QHash<QString, QVector<Message>> m_chatMessages;
    QHash<QString, QVector<Message>> m_taskMessages;
};

} // namespace bx
