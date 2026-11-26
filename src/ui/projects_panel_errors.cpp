#include "ui/projects_panel.h"

#include "ui/board_widget.h"
#include "ui/chat_widgets.h"
#include "ui/project_tools.h"
#include "ui/task_dialogs.h"
#include "ui/task_gantt.h"
#include "ui/workspace_directory.h"
#include "ui/workspace_presentation.h"

#include <QDesktopServices>
#include <QLineEdit>
#include <QPushButton>
#include <utility>

namespace bx {

bool ProjectsPanel::handleFailure(const QString &operation, OperationKind kind, const QStringList &ids, const QString &reason, QString &footer)
{
    switch (kind) {
    case OperationKind::ProjectTaskChats:
        if (!m_projectTaskChatScope.isEmpty() && m_projectTaskChatPending >= 0
            && ids.value(0) == m_projectTaskChatScope) {
            m_projectTaskChatPending = -1;
            m_projectTaskChatError = reason.isEmpty() ? QStringLiteral("Не удалось загрузить чаты задач") : reason;
            updateProjectTaskChatControls();
        }
        return true;
    case OperationKind::BoardColumn: {
        const auto stage = ids.value(1);
        if (ids.value(0) == m_currentProjectId &&
            m_board->stagePageLoading(stage)) {
            m_board->failStagePage(stage);
            emit statusChanged(QStringLiteral("%1 · F5 — обновить доску").arg(reason));
        }
        return true;
    }
    case OperationKind::ProjectMembers:
        if (m_projectTools && ids.value(0) == m_projectTools->projectId())
            m_projectTools->membersFailed(reason);
        return true;
    case OperationKind::ProjectFiles:
        if (m_projectTools && ids.value(0) == m_projectTools->projectId())
            m_projectTools->filesFailed(ids.value(1), reason);
        return true;
    case OperationKind::Timeline:
        if (!m_ganttMode || m_currentProjectId.isEmpty() || ids.value(0) != m_currentProjectId)
            break;
        m_projectTimeline->clear(reason);
        return true;
    case OperationKind::SearchProjectTasks: {
        const auto projectId = ids.value(0);
        const auto query = m_pendingProjectSearches.take(projectId);
        if (m_active && !m_ganttMode && projectId == m_currentProjectId && !query.isEmpty()) {
            if (query == m_filter->text().trimmed())
                ui::setStatusLabel(m_projectTaskSearchStatus, QStringLiteral("Поиск по проекту недоступен. %1").arg(reason));
            else
                runProjectTaskSearch();
        }
        return true;
    }
    case OperationKind::ProjectPin:
        if (m_pendingPins.remove(operation))
            emit statusChanged(reason);
        return true;
    case OperationKind::CreateProject:
        if (!m_pendingCreateProject)
            break;
        m_pendingCreateProject = false;
        if (m_projectForm)
            m_projectForm->setStatus(reason);
        break;
    case OperationKind::BoardEdit:
        if (m_pendingBoardEdit.isEmpty() || ids.value(0) != m_pendingBoardEdit)
            break;
        if (m_stageManager && m_stageManager->projectId() == m_pendingBoardEdit)
            m_stageManager->setStatus(reason);
        emit boardMoveFailed(m_pendingMoveTask, reason);
        m_pendingBoardEdit.clear();
        m_pendingMoveTask.clear();
        break;
    case OperationKind::Board:
        if (m_currentProjectId.isEmpty() || ids.value(0) != m_currentProjectId)
            break;
        m_board->resetStagePageLoads(false);
        if (m_stages.isEmpty())
            m_board->clearBoard(QStringLiteral("Не удалось загрузить доску. Нажмите «Обновить»"));
        else if (!m_projectSearchDisplayed)
            m_board->setStages(m_stages);
        if (m_currentProjectId == m_boardMoveRefreshProject)
            footer = QStringLiteral("Не удалось обновить доску после переноса. Нажмите F5");
        break;
    case OperationKind::Projects:
    case OperationKind::ProjectPageFallback: {
        const auto link = std::exchange(m_pendingMessageProject, {});
        if (!link.url.isEmpty())
            QDesktopServices::openUrl(link.url);
        m_linkedProjectId.clear();
        emit directoryLoadFailed();
        if (m_projectsPagePending) {
            m_projectsPagePending = false;
            m_projectsPageFailed = true;
            updateProjectsRailPagination();
        }
        if (kind == OperationKind::Projects && m_active && !m_projectDirectoryLoaded) {
            m_projects->clear();
            workspace::emptyRow(m_projects, QStringLiteral("Не удалось загрузить проекты"));
        }
        break;
    }
    default:
        break;
    }
    return false;
}

} // namespace bx
