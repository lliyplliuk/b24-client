#include "ui/task_content_list.h"

#include "ui/workspace_style.h"

#include <QResizeEvent>
#include <QTimer>

namespace bx::ui {

TaskContentList::TaskContentList(const QString &name, QWidget *parent)
    : QListWidget(parent)
{
    setObjectName(QStringLiteral("taskContent"));
    setAccessibleName(name);
    workspace::configureRows(this, workspace::Row::TaskContent);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(0);
    connect(model(), &QAbstractItemModel::rowsInserted, this, [this] { requestHeight(); });
    connect(model(), &QAbstractItemModel::rowsRemoved, this, [this] { requestHeight(); });
    connect(model(), &QAbstractItemModel::modelReset, this, [this] { requestHeight(); });
}

void TaskContentList::requestHeight()
{
    if (m_queued)
        return;
    m_queued = true;
    QTimer::singleShot(0, this, [this] {
        m_queued = false;
        int height = 0;
        for (int row = 0; row < count(); ++row)
            height += qMax(0, sizeHintForRow(row));
        if (minimumHeight() != height || maximumHeight() != height)
            setFixedHeight(height);
        doItemsLayout();
        if (m_onLayout)
            m_onLayout();
    });
}

void TaskContentList::setOnLayout(std::function<void()> handler)
{
    m_onLayout = std::move(handler);
}

void TaskContentList::resizeEvent(QResizeEvent *event)
{
    QListWidget::resizeEvent(event);
    requestHeight();
}

} // namespace bx::ui
