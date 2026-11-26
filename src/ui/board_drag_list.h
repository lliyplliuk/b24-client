#pragma once

#include <QListWidget>

namespace bx {

class BoardWidget;

class BoardDragList final : public QListWidget {
public:
    BoardDragList(BoardWidget *board, const QString &stageId, QWidget *parent);
    bool retireIfDragging();
    bool canDrop(const QDropEvent *event, const BoardDragList *target) const;
    void setDropStage(const QString &stageId);
    const QString &stageId() const { return m_stageId; }

protected:
    void startDrag(Qt::DropActions) override;
    bool viewportEvent(QEvent *event) override;

private:
    BoardWidget *const m_board;
    const QString m_stageId;
    QString m_activeTask;
    QString m_dropStage;
    bool m_retired = false;
};

} // namespace bx
