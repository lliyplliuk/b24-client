#include "ui/board_widget.h"
#include "ui/loading_indicator.h"
#include "ui/workspace_style.h"

#include <QCursor>
#include <QDrag>
#include <QEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTimer>
#include <QWheelEvent>

#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace bx {

namespace {
constexpr int CardTags = Qt::UserRole + 53;
constexpr int CardCreator = Qt::UserRole + 54;
constexpr int CardAssignee = Qt::UserRole + 55;
constexpr int CardCreatorAvatar = Qt::UserRole + 56;
constexpr int CardAssigneeAvatar = Qt::UserRole + 57;
constexpr int InsetX = 2;
constexpr int InsetY = 3;
constexpr int Padding = 9;
constexpr int DefaultColumnWidth = 232;
constexpr int MinimumColumnWidth = 150;
constexpr int MaximumColumnWidth = 640;
constexpr int ColumnGap = 12;

QString stageHeaderStyle(const QString &color)
{
    const QColor supplied(QLatin1Char('#') + color);
    const QColor background = supplied.isValid() ? supplied : QColor("#dce5e4");
    const auto linear = [](qreal channel) {
        return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
    };
    const qreal luminance = 0.2126 * linear(background.redF())
        + 0.7152 * linear(background.greenF()) + 0.0722 * linear(background.blueF());
    const bool light = luminance >= 0.179;
    const auto text = light ? QStringLiteral("#000000") : QStringLiteral("#ffffff");
    const auto outline = light ? QStringLiteral("rgba(0,0,0,75)") : QStringLiteral("rgba(255,255,255,120)");
    return QStringLiteral(
        "QWidget#kanbanStageHeader { background: %1; border-radius: 7px; }"
        "QLabel#columnTitle { color: %2; }"
        "QLabel#stageCount { color: %2; background: transparent; border: 1px solid %3; }")
        .arg(background.name(), text, outline);
}

QSettings boardPreferences()
{
    return QSettings(QSettings::defaultFormat(), QSettings::UserScope, QStringLiteral("bitrix24-client"),
                     QStringLiteral("bitrix24-client"));
}

void setStageCount(QLabel *count, qsizetype total)
{
    count->setText(QString::number(total));
    count->setAccessibleName(QStringLiteral("Задач в колонке: %1").arg(total));
}

QListWidgetItem *addListedTask(QListWidget *list, const Task &task, const Stage &stage)
{
    return workspace::addRow(list, task.name, stage.name.isEmpty() ? task.assignee : stage.name, {}, task.id);
}

class ColumnWidthGrip final : public QWidget {
public:
    ColumnWidthGrip(QWidget *column, const QString &title, std::function<void(int, bool)> resize)
        : QWidget(column), m_resize(std::move(resize))
    {
        setObjectName(QStringLiteral("kanbanColumnGrip"));
        setCursor(Qt::SplitHCursor);
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(QStringLiteral("Ширина колонки %1").arg(title));
        setAccessibleDescription(QStringLiteral("Перетащите границу или используйте стрелки влево и вправо"));
        setToolTip(accessibleDescription());
        setMouseTracking(true);
        column->installEventFilter(this);
        positionGrip();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == parentWidget() && event->type() == QEvent::Resize)
            positionGrip();
        return QWidget::eventFilter(watched, event);
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::UngrabMouse || event->type() == QEvent::Hide)
            cancelDrag();
        if (event->type() == QEvent::Enter || event->type() == QEvent::Leave
            || event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)
            update();
        return QWidget::event(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const bool active = underMouse() || hasFocus() || m_dragging;
        painter.setPen(QPen(active ? QColor("#397b77") : QColor("#bdcecc"), active ? 2 : 1));
        painter.drawLine(rect().center().x(), 10, rect().center().x(), 34);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            event->ignore();
            return;
        }
        m_dragging = true;
        m_startX = event->globalPosition().x();
        m_initialWidth = parentWidget()->width();
        update();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_dragging && event->buttons().testFlag(Qt::LeftButton))
            m_resize(m_initialWidth + qRound(event->globalPosition().x() - m_startX), false);
        else
            cancelDrag();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_dragging) {
            m_dragging = false;
            m_resize(parentWidget()->width(), true);
            update();
        }
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
            const int direction = event->key() == Qt::Key_Left ? -1 : 1;
            m_resize(parentWidget()->width() + direction * 16, true);
            return;
        }
        QWidget::keyPressEvent(event);
    }

private:
    void cancelDrag()
    {
        if (m_dragging) {
            m_dragging = false;
            m_resize(m_initialWidth, false);
            update();
        }
    }

    void positionGrip()
    {
        setGeometry(parentWidget()->width() - 10, 0, 9, parentWidget()->height());
    }

    std::function<void(int, bool)> m_resize;
    qreal m_startX = 0;
    int m_initialWidth = 0;
    bool m_dragging = false;
};

using AvatarSource = std::function<const QPixmap *(const QUrl &)>;

constexpr auto CardDragType = "application/x-bitrix24-client-kanban-task";
} // namespace

class KanbanTaskList final : public QListWidget {
public:
    KanbanTaskList(BoardWidget *board, const QString &stageId, QWidget *parent)
        : QListWidget(parent), m_board(board), m_stageId(stageId)
    {
        setDragDropMode(QAbstractItemView::DragOnly);
    }

    bool retireIfDragging()
    {
        if (m_activeTask.isEmpty())
            return false;
        m_retired = true;
        return true;
    }

    bool canDrop(const QDropEvent *event, const KanbanTaskList *target) const
    {
        return !m_retired && m_board == target->m_board && target != this
            && !m_activeTask.isEmpty() && !target->m_stageId.isEmpty()
            && (event->possibleActions() & Qt::MoveAction)
            && event->mimeData()->data(CardDragType) == m_activeTask.toUtf8()
            && m_board->m_taskMoveGuard && m_board->m_taskMoveGuard(m_activeTask, m_stageId, target->m_stageId);
    }

    void setDropStage(const QString &stageId) { m_dropStage = stageId; }
    const QString &stageId() const { return m_stageId; }

protected:
    void startDrag(Qt::DropActions) override
    {
        const auto *task = currentItem();
        if (QGuiApplication::mouseButtons() != Qt::LeftButton || !task || !m_board->m_taskMoveGuard)
            return;
        const auto taskId = task->data(workspace::Id).toString();
        if (taskId.isEmpty() || !m_board->m_taskMoveGuard(taskId, m_stageId, {}))
            return;

        auto *mime = new QMimeData;
        mime->setData(CardDragType, taskId.toUtf8());
        auto *drag = new QDrag(this);
        drag->setMimeData(mime);
        const auto card = visualItemRect(task).intersected(viewport()->visibleRegion().boundingRect());
        if (!card.isEmpty()) {
            drag->setPixmap(viewport()->grab(card));
            const auto cursor = viewport()->mapFromGlobal(QCursor::pos());
            drag->setHotSpot(card.contains(cursor) ? cursor - card.topLeft() : card.center() - card.topLeft());
        }

        // Scroll across wide boards only while the pointer is at a visible edge during a drag.
        QTimer edgeScroll;
        edgeScroll.setInterval(55);
        connect(&edgeScroll, &QTimer::timeout, this, [this] {
            const auto *viewport = m_board->m_scroll->viewport();
            const auto point = viewport->mapFromGlobal(QCursor::pos());
            if (m_retired || !viewport->isVisible() || !viewport->rect().contains(point))
                return;
            constexpr int edge = 32;
            constexpr int step = 16;
            auto *scroll = m_board->m_scroll->horizontalScrollBar();
            if (point.x() < edge)
                scroll->setValue(scroll->value() - step);
            else if (point.x() >= viewport->width() - edge)
                scroll->setValue(scroll->value() + step);
        });
        edgeScroll.start();
        const auto revision = m_board->m_columnRevision;
        const QPointer<KanbanTaskList> source(this);
        m_activeTask = taskId;
        m_dropStage.clear();
        const auto action = drag->exec(Qt::MoveAction);
        if (!source)
            return;
        m_activeTask.clear();
        if (m_retired) {
            parentWidget()->deleteLater();
            return;
        }
        if (action != Qt::MoveAction || m_dropStage.isEmpty())
            return;

        // The server can synchronously rebuild columns. Finish Qt's source drag event first.
        QMetaObject::invokeMethod(m_board, [board = m_board, revision, taskId, from = m_stageId, to = m_dropStage] {
            if (revision == board->m_columnRevision && board->m_taskMoveGuard && board->m_taskMoveGuard(taskId, from, to))
                emit board->taskMoveRequested(taskId, from, to);
        }, Qt::QueuedConnection);
    }

    bool viewportEvent(QEvent *event) override
    {
        if (event->type() == QEvent::Resize) {
            const auto *resize = static_cast<QResizeEvent *>(event);
            if (resize->size().width() != resize->oldSize().width())
                scheduleDelayedItemsLayout();
        }
        return QListWidget::viewportEvent(event);
    }

private:
    BoardWidget *const m_board;
    const QString m_stageId;
    QString m_activeTask;
    QString m_dropStage;
    bool m_retired = false;
};

namespace {
class KanbanColumn final : public QWidget {
public:
    KanbanColumn(BoardWidget *board, const QString &stageId, QWidget *parent)
        : QWidget(parent), m_list(new KanbanTaskList(board, stageId, this))
    {
        setAcceptDrops(true);
    }

    KanbanTaskList *list() const { return m_list; }

protected:
    void dragEnterEvent(QDragEnterEvent *event) override { updateDrop(event); }
    void dragMoveEvent(QDragMoveEvent *event) override { updateDrop(event); }

    void dragLeaveEvent(QDragLeaveEvent *event) override
    {
        setDropHighlight(false);
        event->accept();
    }

    void dropEvent(QDropEvent *event) override
    {
        setDropHighlight(false);
        if (auto *source = acceptedSource(event)) {
            source->setDropStage(m_list->stageId());
            event->setDropAction(Qt::MoveAction);
            event->accept();
        } else {
            event->ignore();
        }
    }

    void paintEvent(QPaintEvent *event) override
    {
        QWidget::paintEvent(event);
        if (!m_dropHighlight)
            return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(workspace::Accent, 2));
        p.drawRoundedRect(rect().adjusted(1, 1, -2, -2), 10, 10);
    }

private:
    KanbanTaskList *acceptedSource(const QDropEvent *event) const
    {
        auto *source = dynamic_cast<KanbanTaskList *>(event->source());
        return source && source->canDrop(event, m_list) ? source : nullptr;
    }

    void updateDrop(QDropEvent *event)
    {
        const bool allowed = acceptedSource(event);
        setDropHighlight(allowed);
        if (allowed) {
            event->setDropAction(Qt::MoveAction);
            event->accept();
        } else {
            event->ignore();
        }
    }

    void setDropHighlight(bool highlighted)
    {
        if (m_dropHighlight == highlighted)
            return;
        m_dropHighlight = highlighted;
        update();
    }

    KanbanTaskList *const m_list;
    bool m_dropHighlight = false;
};

int wrapped(QString text, const QFont &font, int width, QPainter *painter, const QPoint &origin)
{
    text.replace(QLatin1Char('\n'), QChar::LineSeparator);
    QTextLayout layout(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.beginLayout();
    qreal height = 0;
    for (auto line = layout.createLine(); line.isValid(); line = layout.createLine()) {
        line.setLineWidth(qMax(1, width));
        line.setPosition(QPointF(0, height));
        height += line.height();
    }
    layout.endLayout();
    if (painter)
        layout.draw(painter, origin);
    return qCeil(height);
}

class KanbanCardPainter final : public QStyledItemDelegate {
public:
    KanbanCardPainter(QListWidget *list, AvatarSource avatar)
        : QStyledItemDelegate(list), m_list(list), m_avatar(std::move(avatar)) {}

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const override
    {
        const int width = m_list->viewport()->width();
        if (m_width != width) {
            m_width = width;
            m_heights.clear();
        }
        auto it = m_heights.constFind(index.row());
        if (it == m_heights.cend())
            it = m_heights.insert(index.row(), 2 * (InsetY + Padding) + content(index, width - 2 * (InsetX + Padding)));
        return {0, it.value()};
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const auto box = option.rect.adjusted(InsetX, InsetY, -InsetX, -InsetY);
        const bool selected = option.state & QStyle::State_Selected;
        p->save();
        p->setClipRect(option.rect);
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(option.state & QStyle::State_HasFocus ? workspace::Accent
                                                        : selected ? QColor("#8bb6b2") : workspace::Border);
        p->setBrush(selected ? workspace::Selected : workspace::Surface);
        p->drawRoundedRect(box, 8, 8);
        content(index, box.width() - 2 * Padding, p, box.topLeft() + QPoint(Padding, Padding));
        p->restore();
    }

private:
    static int pills(const QStringList &labels, int width, QPainter *p, const QPoint &origin, bool deadline = false)
    {
        constexpr int horizontalPadding = 4;
        constexpr int verticalPadding = 2;
        constexpr int gap = 4;
        const auto font = workspace::rowFont(ui::secondaryTextPixels);
        const QFontMetrics metrics(font);
        int x = 0, y = 0, lineHeight = 0;
        for (const auto &label : labels) {
            const int chipWidth = qMin(width, metrics.horizontalAdvance(label) + 2 * horizontalPadding);
            if (x != 0 && x + chipWidth > width) {
                x = 0;
                y += lineHeight + gap;
                lineHeight = 0;
            }
            const int textWidth = chipWidth - 2 * horizontalPadding;
            const bool fits = metrics.horizontalAdvance(label) <= textWidth;
            const int textHeight = fits ? metrics.height() : wrapped(label, font, textWidth, nullptr, {});
            const int height = textHeight + 2 * verticalPadding;
            const auto start = origin + QPoint(x, y);
            if (p) {
                p->setPen(deadline ? QPen(QColor("#b5c6cb")) : QPen(Qt::NoPen));
                p->setBrush(deadline ? QColor("#f8faf8") : QColor("#e0ede8"));
                const int radius = fits ? height / 2 : 6;
                p->drawRoundedRect(QRect(start, QSize(chipWidth, height)), radius, radius);
                p->setPen(deadline ? workspace::Muted : QColor("#28584f"));
                const auto textAt = start + QPoint(horizontalPadding, verticalPadding);
                if (fits) {
                    p->setFont(font);
                    p->drawText(textAt + QPoint(0, metrics.ascent()), label);
                } else {
                    wrapped(label, font, textWidth, p, textAt);
                }
            }
            x += chipWidth + gap;
            lineHeight = qMax(lineHeight, height);
        }
        return y + lineHeight;
    }

    void avatar(QPainter &p, const QRect &box, const QString &name, const QUrl &url) const
    {
        const auto *photo = m_avatar(url);
        workspace::paintAvatar(p, box, name, workspace::AvatarKind::Person, photo ? *photo : QPixmap());
        if (!name.simplified().isEmpty() || (photo && !photo->isNull()))
            return;
        p.setPen(QPen(workspace::Muted, 1.1, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        const QPoint center(box.center().x(), box.top() + 7);
        p.drawEllipse(QPointF(center), 2.4, 2.4);
        QPainterPath shoulders;
        shoulders.moveTo(center + QPoint(-5, 8));
        shoulders.quadTo(center + QPoint(0, 1), center + QPoint(5, 8));
        p.drawPath(shoulders);
    }

    int content(const QModelIndex &index, int width, QPainter *p = nullptr, const QPoint &origin = {}) const
    {
        width = qMax(1, width);
        const bool task = !index.data(workspace::Id).toString().isEmpty();
        if (p)
            p->setPen(task ? workspace::Ink : workspace::Muted);
        int height = wrapped(index.data(workspace::Title).toString(), workspace::rowFont(ui::bodyTextPixels, task), width, p, origin);
        if (!task)
            return height;
        const auto tags = index.data(CardTags).toStringList();
        if (!tags.isEmpty()) {
            height += 6;
            height += pills(tags, width, p, origin + QPoint(0, height));
        }
        const auto due = index.data(workspace::Meta).toString();
        if (!due.isEmpty()) {
            height += 6;
            height += pills({due}, width, p, origin + QPoint(0, height), true);
        }
        constexpr int diameter = 22;
        height += 8;
        if (p) {
            const auto start = origin + QPoint(0, height);
            avatar(*p, QRect(start, QSize(diameter, diameter)), index.data(CardCreator).toString(),
                   index.data(CardCreatorAvatar).toUrl());
            const auto arrow = start + QPoint(27, diameter / 2);
            p->setPen(QPen(QColor("#627d80"), 1.2, Qt::SolidLine, Qt::RoundCap));
            p->drawLine(arrow + QPoint(0, -3), arrow + QPoint(3, 0));
            p->drawLine(arrow + QPoint(3, 0), arrow + QPoint(0, 3));
            avatar(*p, QRect(start + QPoint(36, 0), QSize(diameter, diameter)), index.data(CardAssignee).toString(),
                   index.data(CardAssigneeAvatar).toUrl());
        }
        return height + diameter;
    }

    QListWidget *const m_list;
    const AvatarSource m_avatar;
    mutable int m_width = -1;
    // The board rebuilds each column when its rows change.
    mutable QHash<int, int> m_heights;
};

QListWidgetItem *addKanbanTask(QListWidget *list, const Task &task)
{
    QStringList tags;
    for (auto tag : task.tags) {
        tag = tag.trimmed();
        if (!tag.isEmpty() && tag != QLatin1String("#"))
            tags.append(tag.startsWith(QLatin1Char('#')) ? tag : QLatin1Char('#') + tag);
    }
    QStringList information{
        QStringLiteral("Постановщик: %1").arg(task.createdBy.isEmpty() ? QStringLiteral("не указан") : task.createdBy),
        QStringLiteral("Исполнитель: %1").arg(task.assignee.isEmpty() ? QStringLiteral("не указан") : task.assignee),
    };
    if (!tags.isEmpty())
        information.append(QStringLiteral("Теги: %1").arg(tags.join(QStringLiteral(", "))));
    if (!task.due.isEmpty())
        information.append(QStringLiteral("Срок: %1").arg(task.due));
    auto *item = workspace::addRow(list, task.name, information.join(QLatin1Char('\n')), {}, task.id);
    item->setData(Qt::AccessibleTextRole, task.name);
    item->setData(Qt::AccessibleDescriptionRole, information.join(QStringLiteral(". ")));
    item->setData(workspace::Meta, task.due);
    item->setData(CardTags, tags);
    item->setData(CardCreator, task.createdBy);
    item->setData(CardAssignee, task.assignee);
    item->setData(CardCreatorAvatar, task.creatorAvatarUrl);
    item->setData(CardAssigneeAvatar, task.assigneeAvatarUrl);
    return item;
}
}

BoardWidget::BoardWidget(QWidget *parent) : QWidget(parent)
{
    setAccessibleName(QStringLiteral("Доска проекта"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_empty = workspace::label(QStringLiteral("Выберите проект, чтобы открыть доску"), "empty", this);
    m_empty->setAlignment(Qt::AlignCenter);
    root->addWidget(m_empty, 1);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_container = new QWidget(m_scroll);
    m_columnsLayout = new QHBoxLayout(m_container);
    m_columnsLayout->setContentsMargins(0, 0, 0, 0);
    m_columnsLayout->setSpacing(ColumnGap);
    m_scroll->setWidget(m_container);
    m_scroll->viewport()->installEventFilter(this);
    connect(m_scroll->horizontalScrollBar(), &QScrollBar::valueChanged, this, &BoardWidget::visibleCardsChanged);
    m_scroll->hide();
    root->addWidget(m_scroll, 1);

    m_list = new QListWidget(this);
    m_list->setAccessibleName(QStringLiteral("Список задач"));
    workspace::configureRows(m_list, workspace::Row::Card);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    watchPageScroll(m_list);
    m_list->hide();
    const auto openTask = [this](QListWidgetItem *item) {
        const auto taskId = item->data(workspace::Id).toString();
        if (taskId.isEmpty())
            return;
        m_selectedTaskId = taskId;
        emit taskClicked(taskId);
    };
    connect(m_list, &QListWidget::itemClicked, this, openTask);
    connect(m_list, &QListWidget::itemActivated, this, openTask);
    root->addWidget(m_list, 1);
    m_pageStatus = workspace::label({}, "quiet", this);
    m_pageStatus->setMinimumHeight(m_pageStatus->fontMetrics().height() + 4);
    m_pageStatus->hide();
    root->addWidget(m_pageStatus);
}

void BoardWidget::clearColumns()
{
    ++m_columnRevision;
    while (auto *item = m_columnsLayout->takeAt(0)) {
        auto *column = dynamic_cast<KanbanColumn *>(item->widget());
        // A refresh can arrive inside QDrag's event loop. Keep its source alive until it unwinds.
        if (column && column->list()->retireIfDragging())
            column->hide();
        else
            delete item->widget();
        delete item;
    }
}

void BoardWidget::setAvatarSource(AvatarSource source)
{
    m_avatarSource = std::move(source);
    refreshAvatars();
}

void BoardWidget::setTaskMoveGuard(TaskMoveGuard guard)
{
    m_taskMoveGuard = std::move(guard);
}

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

void BoardWidget::setColumnWidthContext(const QString &projectId, const QString &accountPreference)
{
    QString group;
    if (!projectId.isEmpty() && !accountPreference.isEmpty())
        group = accountPreference + QLatin1Char('/') + QString::fromLatin1(QUrl::toPercentEncoding(projectId));
    if (m_widthProject == projectId && m_widthGroup == group)
        return;

    m_widthProject = projectId;
    m_widthGroup = group;
    m_columnWidths.clear();
    if (!m_stages.isEmpty() && m_view == View::Kanban)
        setStages(m_stages, m_alreadyFiltered);
}

QList<QUrl> BoardWidget::visibleAvatarUrls() const
{
    QList<QUrl> urls;
    if (m_view != View::Kanban || !m_scroll->isVisible())
        return urls;
    for (auto *column : m_container->findChildren<QListWidget *>()) {
        auto *viewport = column->viewport();
        const auto *outer = m_scroll->viewport();
        const QRect outerRect(viewport->mapFrom(outer, QPoint()), outer->size());
        const auto visible = viewport->rect().intersected(outerRect);
        if (!viewport->isVisible() || visible.isEmpty())
            continue;
        const auto first = column->indexAt(QPoint(visible.center().x(), visible.top()));
        for (int row = first.isValid() ? first.row() : 0; row < column->count(); ++row) {
            const auto *item = column->item(row);
            const auto rowRect = column->visualItemRect(item);
            if (rowRect.top() > visible.bottom())
                break;
            if (!rowRect.intersects(visible))
                continue;
            for (const int role : {CardCreatorAvatar, CardAssigneeAvatar}) {
                const auto url = item->data(role).toUrl();
                if (!url.isEmpty() && !urls.contains(url))
                    urls.append(url);
            }
        }
    }
    return urls;
}

void BoardWidget::refreshAvatars()
{
    if (m_view == View::Kanban && m_scroll->isVisible()) {
        for (auto *column : m_container->findChildren<QListWidget *>())
            column->viewport()->update();
    }
}

bool BoardWidget::matches(const Task &task) const
{
    return m_alreadyFiltered || matchesTaskFilter(task, m_filter);
}

bool BoardWidget::matchesTaskFilter(const Task &task, const QString &query)
{
    const auto title = query.trimmed();
    if (title.isEmpty())
        return true;
    auto id = title;
    if (id.startsWith(QLatin1Char('#')) || id.startsWith(QChar(u'№')))
        id.remove(0, 1);
    id = id.trimmed();
    return task.name.contains(title, Qt::CaseInsensitive) || (!id.isEmpty() && task.id.contains(id));
}

QString BoardWidget::noMatchesText() const
{
    return m_alreadyFiltered ? QStringLiteral("Ничего не найдено")
                             : QStringLiteral("В загруженных задачах совпадений нет");
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

void BoardWidget::setFilter(const QString &text)
{
    if (m_filter == text)
        return;
    m_filter = text;
    setStages(m_stages);
}

void BoardWidget::setView(View view)
{
    if (m_view == view)
        return;
    m_view = view;
    setStages(m_stages, m_alreadyFiltered);
    emit viewChanged(view);
}

void BoardWidget::rebuildList()
{
    const int position = m_list->verticalScrollBar()->value();
    m_list->clear();
    for (const auto &stage : m_stages) {
        for (const auto &task : stage.tasks) {
            if (!matches(task))
                continue;
            auto *item = addListedTask(m_list, task, stage);
            if (!task.id.isEmpty() && task.id == m_selectedTaskId)
                m_list->setCurrentItem(item);
        }
    }
    if (m_list->count() == 0 && !hasMoreStagePages())
        workspace::emptyRow(m_list, m_filter.trimmed().isEmpty() ? QStringLiteral("Задач пока нет")
                                                                 : noMatchesText());
    m_list->verticalScrollBar()->setValue(position);
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
            if (list->property("stageId").toString() != stage.id || list->parentWidget()->isHidden())
                continue;
            auto *count = list->parentWidget()->findChild<QLabel *>(QStringLiteral("stageCount"));
            setStageCount(count, stage.taskCount.value_or(stage.tasks.size()));
            for (auto n = firstNew; n < stage.tasks.size(); ++n)
                addKanbanTask(list, stage.tasks.at(n));
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

void BoardWidget::setStages(const QVector<Stage> &stages, bool alreadyFiltered)
{
    m_stages = stages;
    m_alreadyFiltered = alreadyFiltered;
    ui::setLoadingIndicator(m_empty, false);
    updatePageStatus();
    const int previousScroll = m_scroll->horizontalScrollBar()->value();
    const auto *focused = QApplication::focusWidget();
    const auto focusedStage = focused && m_container->isAncestorOf(focused)
        ? focused->property("stageId").toString() : QString();
    QHash<QString, int> columnScrolls;
    for (const auto *column : m_container->findChildren<QListWidget *>())
        columnScrolls.insert(column->property("stageId").toString(), column->verticalScrollBar()->value());
    clearColumns();
    if (m_view == View::List) {
        rebuildList();
        m_scroll->hide();
        const bool showList = !m_stages.isEmpty() || !m_filter.trimmed().isEmpty();
        m_list->setVisible(showList);
        if (m_stages.isEmpty()) {
            m_empty->setText(QStringLiteral("На этой доске пока нет этапов"));
            m_empty->setVisible(!showList);
        } else {
            m_empty->hide();
        }
        return;
    }
    m_list->hide();
    if (stages.isEmpty()) {
        m_scroll->hide();
        m_empty->setText(m_filter.trimmed().isEmpty() ? QStringLiteral("На этой доске пока нет этапов")
                                                    : noMatchesText());
        m_empty->show();
        return;
    }
    QWidget *focus = nullptr;
    auto settings = boardPreferences();
    for (const auto &stage : stages) {
        auto *column = new KanbanColumn(this, stage.id, m_container);
        column->setObjectName(QStringLiteral("column"));
        const auto widthKey = m_widthGroup.isEmpty() || stage.id.isEmpty() ? QString()
            : m_widthGroup + QLatin1Char('/') + QString::fromLatin1(QUrl::toPercentEncoding(stage.id));
        const int savedWidth = m_columnWidths.value(stage.id, widthKey.isEmpty() ? DefaultColumnWidth
            : settings.value(widthKey, DefaultColumnWidth).toInt());
        const int initialWidth = qBound(MinimumColumnWidth, savedWidth, MaximumColumnWidth);
        column->setFixedWidth(initialWidth);
        auto *layout = new QVBoxLayout(column);
        layout->setContentsMargins(7, 7, 13, 7);
        layout->setSpacing(6);

        auto *header = new QWidget(column);
        header->setObjectName(QStringLiteral("kanbanStageHeader"));
        header->setStyleSheet(stageHeaderStyle(stage.color));
        auto *heading = new QHBoxLayout(header);
        heading->setContentsMargins(8, 5, 6, 5);
        heading->setSpacing(6);
        const auto title = stage.name.isEmpty() ? QStringLiteral("Без названия") : stage.name;
        auto *name = workspace::label(title, "columnTitle", header);
        heading->addWidget(name, 1);
        auto *count = workspace::label(QString(), "stageCount", header);
        count->setMinimumHeight(21);
        count->setMinimumWidth(23);
        count->setContentsMargins(3, 0, 3, 0);
        count->setAlignment(Qt::AlignCenter);
        heading->addWidget(count, 0, Qt::AlignVCenter);
        layout->addWidget(header);

        auto *list = column->list();
        list->setAccessibleName(title);
        list->setProperty("stageId", stage.id);
        list->setFrameShape(QFrame::NoFrame);
        list->setMouseTracking(true);
        list->setResizeMode(QListView::Adjust);
        list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->setItemDelegate(new KanbanCardPainter(list, [this](const QUrl &url) {
            return m_avatarSource && !url.isEmpty() ? m_avatarSource(url) : nullptr;
        }));
        watchPageScroll(list);
        connect(list->verticalScrollBar(), &QScrollBar::valueChanged, this, &BoardWidget::visibleCardsChanged);
        list->setSelectionMode(QAbstractItemView::SingleSelection);
        int shown = 0;
        for (const auto &task : stage.tasks) {
            if (!matches(task))
                continue;
            ++shown;
            auto *item = addKanbanTask(list, task);
            if (!task.id.isEmpty() && task.id == m_selectedTaskId)
                list->setCurrentItem(item);
        }
        const auto total = m_filter.trimmed().isEmpty() ? stage.taskCount.value_or(stage.tasks.size()) : shown;
        setStageCount(count, total);
        const bool moreAvailable = m_filter.trimmed().isEmpty() && !m_alreadyFiltered && stage.hasMoreTasks;
        if (shown == 0 && !moreAvailable)
            workspace::emptyRow(list, m_filter.trimmed().isEmpty() ? QStringLiteral("Нет задач") : noMatchesText());
        if (stage.id == focusedStage)
            focus = list;

        const auto openTask = [this, list](QListWidgetItem *item) {
            const auto taskId = item->data(workspace::Id).toString();
            if (taskId.isEmpty())
                return;
            m_selectedTaskId = taskId;
            for (auto *columnList : m_container->findChildren<QListWidget *>()) {
                if (columnList != list)
                    columnList->clearSelection();
            }
            emit taskClicked(taskId);
        };
        connect(list, &QListWidget::itemClicked, this, openTask);
        connect(list, &QListWidget::itemActivated, this, openTask);
        layout->addWidget(list, 1);
        auto *grip = new ColumnWidthGrip(column, title, [this, column, id = stage.id, widthKey, initialWidth](int proposed, bool finished) {
            const int width = qBound(MinimumColumnWidth, proposed, MaximumColumnWidth);
            if (column->width() != width)
                column->setFixedWidth(width);
            if (finished && !id.isEmpty() && m_columnWidths.value(id, initialWidth) != width) {
                m_columnWidths.insert(id, width);
                if (!widthKey.isEmpty()) {
                    auto settings = boardPreferences();
                    settings.setValue(widthKey, width);
                }
            }
        });
        grip->show();
        m_columnsLayout->addWidget(column);
        if (const int previous = columnScrolls.value(stage.id); previous > 0) {
            list->doItemsLayout();
            list->verticalScrollBar()->setValue(previous);
        }
    }
    m_columnsLayout->addStretch(1);
    m_empty->hide();
    m_scroll->show();
    m_scroll->horizontalScrollBar()->setValue(previousScroll);
    if (focus)
        focus->setFocus(Qt::OtherFocusReason);
    emit visibleCardsChanged();
}

void BoardWidget::clearBoard(const QString &emptyText, bool loading)
{
    m_selectedTaskId.clear();
    m_stages.clear();
    resetStagePageLoads();
    m_alreadyFiltered = false;
    clearColumns();
    m_list->clear();
    m_scroll->hide();
    m_list->hide();
    m_empty->setText(emptyText);
    m_empty->show();
    ui::setLoadingIndicator(m_empty, loading);
}

} // namespace bx
