#include "cloud/cloud_backend.h"

#include "cloud/cloud_rows.h"
#include "core/notification_targets.h"

#include <QUuid>
#include <utility>

namespace bx {
using namespace cloud_rows;

void CloudBackend::requestMessageLinkPreview(const QUrl &url, LinkPreviewHandler complete)
{
    const auto target = portalLinkPreviewTarget(m_session.origin, url);
    if (m_authExpired || !target) {
        complete({});
        return;
    }
    const auto operation = operationKey(OperationKind::LinkPreview, QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto scope = settingsScope();
    const auto finish = [this, scope, complete = std::move(complete)](MessageLinkPreview preview) {
        if (m_authExpired || settingsScope() != scope)
            preview = {};
        complete(std::move(preview));
    };
    switch (target->kind) {
    case NotificationTargetKind::Task:
        m_tasks.loadTask(target->id, operation, [this, finish, scope, operation](const QJsonObject &row) {
            if (m_authExpired || settingsScope() != scope) {
                finish({});
                return;
            }
            const auto task = taskFromRow(row);
            MessageLinkPreview preview{notificationDisplayText(task.name).left(160), {},
                                       MessageLinkPreviewKind::Task};
            const auto creator = row.value(QStringLiteral("creator")).toObject();
            const auto responsible = row.value(QStringLiteral("responsible")).toObject();
            preview.creatorAvatarUrl = m_tasks.taskRoleAvatar({}, creator, task.creatorId);
            preview.assigneeAvatarUrl = m_tasks.taskRoleAvatar({}, responsible, task.assigneeId);
            if (!preview.title.isEmpty()) {
                const auto append = [&preview](const QString &label, const QString &value) {
                    const auto display = notificationDisplayText(value).left(180);
                    if (!display.isEmpty())
                        preview.details.append(label.arg(display));
                };
                append(QStringLiteral("Статус · %1"), task.status);
                append(QStringLiteral("Постановщик · %1"), task.createdBy);
                append(QStringLiteral("Исполнитель · %1"), task.assignee);
                append(QStringLiteral("Описание · %1"), task.description);
            }
            const auto explicitProject = field(row, {"groupId", "GROUP_ID"});
            const auto projectId = explicitProject.isEmpty() ? task.projectId : numericId(explicitProject);
            const auto group = row.value(QStringLiteral("group")).toObject();
            const auto nestedProject = numericId(field(group, {"id", "ID"}));
            const auto fallbackName = explicitProject.isEmpty()
                || (!projectId.isEmpty() && (nestedProject.isEmpty() || nestedProject == projectId))
                ? task.project : QString{};
            const bool canLoadProject = !preview.title.isEmpty() && !projectId.isEmpty()
                && !m_authExpired && settingsScope() == scope;
            auto applyProject = [this, finish, fallbackName, stageId = task.stageId,
                                 stageName = task.stageName, hasStatus = !task.status.isEmpty(),
                                 preview = std::move(preview)](Project project, QString) mutable {
                if (!preview.title.isEmpty()) {
                    const auto name = notificationDisplayText(project.name.isEmpty() ? fallbackName : project.name).left(180);
                    if (!name.isEmpty())
                        preview.details.append(QStringLiteral("Проект · %1").arg(name));
                    if (!project.name.isEmpty())
                        preview.project = std::move(project);
                }
                const auto group = preview.project ? preview.project->id : QString{};
                auto applyStage = [finish, stageName, hasStatus, preview = std::move(preview)](Stage stage) mutable {
                    const auto name = notificationDisplayText(stage.name.isEmpty() ? stageName : stage.name).left(180);
                    if (!preview.title.isEmpty() && !name.isEmpty()) {
                        preview.details.insert(hasStatus ? 1 : 0, QStringLiteral("Стадия · %1").arg(name));
                        preview.stageColor = stage.color;
                    }
                    finish(std::move(preview));
                };
                if (group.isEmpty() || numericId(stageId).isEmpty())
                    applyStage({});
                else
                    m_projects.loadLinkStage(group, stageId, std::move(applyStage));
            };
            if (canLoadProject)
                m_projects.loadLinkProject(projectId, operation, std::move(applyProject));
            else
                applyProject({}, {});
        }, {}, false);
        break;
    case NotificationTargetKind::Project:
        m_projects.loadLinkProject(target->id, operation, [finish](Project project, const QString &text) {
            MessageLinkPreview preview{notificationDisplayText(project.name).left(160), {}, MessageLinkPreviewKind::Project};
            const auto description = notificationDisplayText(text).left(180);
            if (!description.isEmpty())
                preview.details.append(description);
            if (!preview.title.isEmpty())
                preview.project = std::move(project);
            finish(std::move(preview));
        });
        break;
    case NotificationTargetKind::Person:
        loadPerson(target->id, operation, [finish](const Person &person) {
            finish({person.name, person.position.isEmpty() ? QStringList{} : QStringList{person.position},
                    MessageLinkPreviewKind::Person, person.avatarUrl});
        });
        break;
    case NotificationTargetKind::Chat:
        ajax(QStringLiteral("im.v2.Chat.load"),
             {{QStringLiteral("dialogId"), target->id}, {QStringLiteral("messageLimit"), QStringLiteral("1")}},
             operation, [this, dialogId = target->id, finish](QJsonValue value) {
            const auto chat = value.toObject().value(QStringLiteral("chat")).toObject();
            const auto supplied = field(chat, {"dialogId", "DIALOG_ID"});
            if (chatDialog(field(chat, {"id", "chatId", "ID"})) != dialogId
                || (!supplied.isEmpty() && supplied != dialogId)) {
                finish({});
                return;
            }
            finish({notificationDisplayText(field(chat, {"name", "title", "TITLE"})).left(160), {},
                    MessageLinkPreviewKind::Chat, avatarUrlFromPortal(field(chat, {"avatar", "avatarUrl"}))});
        });
        break;
    }
}

} // namespace bx
