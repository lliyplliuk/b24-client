#pragma once

#include <QWidget>

class QLabel;

namespace bx {

class BoardWidget;
class BoardDragList;
struct Stage;

class BoardColumn final : public QWidget {
public:
    BoardColumn(BoardWidget *board, const Stage &stage, QWidget *parent);
    void setTaskCount(qsizetype total);
    QString title() const;
    BoardDragList *list() const { return m_list; }

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    BoardDragList *acceptedSource(const QDropEvent *event) const;
    void updateDrop(QDropEvent *event);
    void setDropHighlight(bool highlighted);

    QLabel *m_count;
    BoardDragList *const m_list;
    bool m_dropHighlight = false;
};

} // namespace bx
