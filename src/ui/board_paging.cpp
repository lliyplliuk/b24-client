#include "ui/board_widget.h"
#include "ui/board_card_painter.h"
#include "ui/board_column.h"
#include "ui/loading_indicator.h"
#include "ui/workspace_rows.h"

#include <QListWidget>
#include <QKeyEvent>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>

namespace bx {

bool BoardWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Show || event->type() == QEvent::Resize)
        emit visibleCardsChanged();
    if (event->type() == QEvent::Wheel) {
        const auto *wheel = static_cast<QWheelEvent *>(event);
        const auto delta = wheel->pixelDelta().isNull() ? wheel->angleDelta() : wheel->pixelDelta();
        if (delta.y() < 0 && -qint64(delta.y()) >= qAbs(qint64(delta.x()))) {
            if (auto *list = qobject_cast<QListWidget *>(watched->parent()))
                queuePageFromScroll(list);
        }
    } else if (event->type() == QEvent::KeyPress) {
        const auto key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Down || key == Qt::Key_PageDown || key == Qt::Key_End) {
            if (auto *list = qobject_cast<QListWidget *>(watched))
                queuePageFromScroll(list);
        }
    }
    return QWidget::eventFilter(watched, event);
}

void BoardWidget::setStagePageLoading(const QString &stage, bool loading)
{
    m_loadingStage = loading ? stage : QString();
    updatePageStatus();
}

bool BoardWidget::finishStagePage(const QString &stage, bool progressed)
{
    if (!progressed && m_unchangedPages.contains(stage)) {
        failStagePage(stage);
        return false;
    }
    if (progressed)
        m_unchangedPages.remove(stage);
    else
        m_unchangedPages.insert(stage);
    setStagePageLoading(stage, false);
    return true;
}

void BoardWidget::failStagePage(const QString &stage)
{
    m_failedPageStages.insert(stage);
    setStagePageLoading(stage, false);
}

void BoardWidget::resetStagePageLoads(bool enabled)
{
    m_loadingStage.clear();
    m_unchangedPages.clear();
    m_failedPageStages.clear();
    m_pageLoadsEnabled = enabled;
    updatePageStatus();
}

bool BoardWidget::canPageStages() const
{
    return m_pageLoadsEnabled && !m_alreadyFiltered && m_filter.trimmed().isEmpty();
}

bool BoardWidget::canPageStage(const Stage &stage) const
{
    return stage.hasMoreTasks && !stage.id.isEmpty() && !m_failedPageStages.contains(stage.id);
}

bool BoardWidget::hasMoreStagePages() const
{
    return canPageStages() && std::any_of(m_stages.cbegin(), m_stages.cend(),
        [this](const Stage &stage) { return canPageStage(stage); });
}

void BoardWidget::watchPageScroll(QListWidget *list)
{
    list->installEventFilter(this);
    list->viewport()->installEventFilter(this);
    auto *bar = list->verticalScrollBar();
    connect(bar, &QScrollBar::actionTriggered, this, [this, list, bar](int action) {
        if (action == QAbstractSlider::SliderSingleStepAdd || action == QAbstractSlider::SliderPageStepAdd
            || action == QAbstractSlider::SliderToMaximum
            || (action == QAbstractSlider::SliderMove && bar->isSliderDown() && bar->sliderPosition() > bar->value()))
            queuePageFromScroll(list);
    });
}

void BoardWidget::queuePageFromScroll(QListWidget *list)
{
    if (m_pageScrollQueued || !canPageStages() || !m_loadingStage.isEmpty() || m_stages.isEmpty())
        return;
    m_pageScrollQueued = true;
    // Evaluate the new position after Qt has handled the input and committed the scroll.
    QTimer::singleShot(0, this, [this, source = QPointer<QListWidget>(list), revision = m_columnRevision] {
        m_pageScrollQueued = false;
        if (!source || revision != m_columnRevision || !source->isVisible() || !canPageStages() || !m_loadingStage.isEmpty())
            return;

        if (source == m_list) {
            const auto stageId = listStageAtPageThreshold();
            if (!stageId.isEmpty())
                emit loadMoreRequested(stageId);
            return;
        }

        const auto *viewport = m_scroll->viewport();
        const QRect area(source->viewport()->mapTo(viewport, QPoint()), source->viewport()->size());
        if (!viewport->rect().intersects(area))
            return;
        const auto *bar = source->verticalScrollBar();
        if (3 * qint64(bar->value() - bar->minimum()) < 2 * qint64(bar->maximum() - bar->minimum()))
            return;
        const auto stageId = source->property("stageId").toString();
        const auto stage = std::find_if(m_stages.cbegin(), m_stages.cend(),
            [&stageId](const Stage &candidate) { return candidate.id == stageId; });
        if (stage != m_stages.cend() && canPageStage(*stage))
            emit loadMoreRequested(stageId);
    });
}

QString BoardWidget::listStageAtPageThreshold() const
{
    const int height = m_list->viewport()->height();
    int firstRow = 0;
    for (const auto &stage : m_stages) {
        const int endRow = firstRow + int(stage.tasks.size());
        if (canPageStage(stage)) {
            const auto *firstItem = m_list->item(firstRow);
            const auto *lastItem = endRow > 0 ? m_list->item(endRow - 1) : nullptr;
            const int top = firstItem ? m_list->visualItemRect(firstItem).top() : 0;
            const int bottom = lastItem ? m_list->visualItemRect(lastItem).bottom() + 1 : 0;
            if (endRow == firstRow) {
                const int boundary = firstItem ? top : bottom;
                if (boundary >= 0 && boundary <= height)
                    return stage.id;
            } else {
                const int range = qMax(0, bottom - top - height);
                if (top < height && bottom > 0 && (range == 0 || 3 * -qint64(top) >= 2 * qint64(range)))
                    return stage.id;
            }
        }
        firstRow = endRow;
    }
    return {};
}

void BoardWidget::updatePageStatus()
{
    const bool active = canPageStages() && !m_loadingStage.isEmpty();
    m_pageStatus->setVisible(active);
    m_pageStatus->setText(active ? QStringLiteral("Загружаем задачи…") : QString());
    ui::setLoadingIndicator(m_pageStatus, active);
}

void BoardWidget::applyStagePage(const Stage &stage)
{
    const auto existing = std::find_if(m_stages.begin(), m_stages.end(),
        [&stage](const Stage &column) { return column.id == stage.id; });
    if (existing == m_stages.end())
        return;
    const auto firstNew = existing->tasks.size();
    *existing = stage;
    if (!m_filter.trimmed().isEmpty() || m_alreadyFiltered) {
        setStages(m_stages, m_alreadyFiltered);
        return;
    }
    if (m_view == View::List && firstNew < stage.tasks.size()) {
        int row = int(firstNew);
        for (auto before = m_stages.begin(); before != existing; ++before)
            row += int(before->tasks.size());
        auto *anchor = m_list->itemAt(QPoint(2, 2));
        if (m_list->count() == 1 && m_list->item(0)->data(workspace::Id).toString().isEmpty()) {
            anchor = nullptr;
            delete m_list->takeItem(0);
        }
        const int anchorTop = anchor ? m_list->visualItemRect(anchor).top() : 0;
        for (auto n = firstNew; n < stage.tasks.size(); ++n, ++row) {
            addListedTask(m_list, stage.tasks.at(n), stage);
            if (row < m_list->count() - 1)
                m_list->insertItem(row, m_list->takeItem(m_list->count() - 1));
        }
        if (anchor) {
            m_list->doItemsLayout();
            auto *scrollbar = m_list->verticalScrollBar();
            scrollbar->setValue(scrollbar->value() + m_list->visualItemRect(anchor).top() - anchorTop);
        }
    } else if (m_view == View::Kanban) {
        for (auto *list : m_container->findChildren<QListWidget *>()) {
            auto *column = dynamic_cast<BoardColumn *>(list->parentWidget());
            if (!column || column->isHidden() || list->property("stageId").toString() != stage.id)
                continue;
            column->setTaskCount(stage.taskCount.value_or(stage.tasks.size()));
            for (auto n = firstNew; n < stage.tasks.size(); ++n)
                BoardCardPainter::addTask(list, stage.tasks.at(n));
            if (list->count() == 0 && !stage.hasMoreTasks)
                workspace::emptyRow(list, QStringLiteral("Нет задач"));
            break;
        }
    }
    updatePageStatus();
    if (m_view == View::List && m_list->count() == 0 && !hasMoreStagePages())
        workspace::emptyRow(m_list, QStringLiteral("Задач пока нет"));
    emit visibleCardsChanged();
}

} // namespace bx
