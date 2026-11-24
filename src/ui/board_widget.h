#pragma once

#include "core/models.h"

#include <QHash>
#include <QHBoxLayout>
#include <QSet>
#include <QWidget>

#include <functional>

class QLabel;
class QListWidget;
class QPixmap;
class QScrollArea;
class QVBoxLayout;

namespace bx {

class KanbanTaskList;

// Kanban board of one project: one column per stage, tasks listed inside.
// The same model (QVector<Stage>) feeds both the kanban columns and the flat
// task list.
class BoardWidget final : public QWidget {
    Q_OBJECT
public:
    enum class View { Kanban, List };

    explicit BoardWidget(QWidget *parent = nullptr);

    void setStages(const QVector<Stage> &stages, bool alreadyFiltered = false);
    void clearBoard(const QString &emptyText, bool loading = false);
    void setView(View view);
    View view() const { return m_view; }
    void setFilter(const QString &text);
    void setColumnWidthContext(const QString &projectId, const QString &accountPreference);
    void setAvatarSource(std::function<const QPixmap *(const QUrl &)> source);
    using TaskMoveGuard = std::function<bool(const QString &, const QString &, const QString &)>;
    void setTaskMoveGuard(TaskMoveGuard guard);
    QList<QUrl> visibleAvatarUrls() const;
    void refreshAvatars();
    QString filter() const { return m_filter; }
    static bool matchesTaskFilter(const Task &task, const QString &query);
    bool stagePageLoading(const QString &stage) const { return !stage.isEmpty() && m_loadingStage == stage; }
    void setStagePageLoading(const QString &stage, bool loading);
    bool finishStagePage(const QString &stage, bool progressed);
    void failStagePage(const QString &stage);
    void applyStagePage(const Stage &stage);
    void resetStagePageLoads(bool enabled = true);

signals:
    void taskClicked(const QString &taskId);
    void taskMoveRequested(const QString &taskId, const QString &sourceStage, const QString &targetStage);
    void viewChanged(View view);
    void loadMoreRequested(const QString &stageId);
    void visibleCardsChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    friend class KanbanTaskList;

    void clearColumns();
    bool matches(const Task &task) const;
    QString noMatchesText() const;
    void rebuildList();
    bool canPageStages() const;
    bool canPageStage(const Stage &stage) const;
    bool hasMoreStagePages() const;
    void watchPageScroll(QListWidget *list);
    void queuePageFromScroll(QListWidget *list);
    QString listStageAtPageThreshold() const;
    void updatePageStatus();

    QScrollArea *m_scroll = nullptr;
    QWidget *m_container = nullptr;
    QHBoxLayout *m_columnsLayout = nullptr;
    QLabel *m_empty = nullptr;
    QLabel *m_pageStatus = nullptr;
    QListWidget *m_list = nullptr;
    QString m_selectedTaskId;
    QString m_filter;
    QVector<Stage> m_stages;
    QString m_loadingStage;
    QSet<QString> m_unchangedPages;
    QSet<QString> m_failedPageStages;
    bool m_pageScrollQueued = false;
    bool m_pageLoadsEnabled = true;
    std::function<const QPixmap *(const QUrl &)> m_avatarSource;
    TaskMoveGuard m_taskMoveGuard;
    quint64 m_columnRevision = 0;
    View m_view = View::Kanban;
    QString m_widthProject;
    QString m_widthGroup;
    QHash<QString, int> m_columnWidths;
    bool m_alreadyFiltered = false;
};

} // namespace bx
