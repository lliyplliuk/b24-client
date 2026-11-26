#include "ui/task_relations_panel.h"

#include "ui/loading_indicator.h"
#include "ui/workspace_style.h"
#include <QHash>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace bx::ui {

TaskRelationsPanel::TaskRelationsPanel(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    m_checklist = new QTreeWidget(this);
    m_checklist->setObjectName(QStringLiteral("taskChecklist"));
    m_checklist->setAccessibleName(QStringLiteral("Чек-листы задачи"));
    m_checklist->setHeaderHidden(true);
    m_checklist->setIndentation(15);
    m_checklist->setMinimumHeight(78);
    m_checklist->setMaximumHeight(185);
    auto *initialChecklists = new QTreeWidgetItem(m_checklist);
    auto *checklistState = workspace::label(QStringLiteral("Загружаем чек-листы…"), "quiet", m_checklist);
    checklistState->setContentsMargins(6, 1, 2, 1);
    setLoadingIndicator(checklistState, true);
    initialChecklists->setData(0, Qt::AccessibleTextRole, checklistState->text());
    m_checklist->setItemWidget(initialChecklists, 0, checklistState);
    layout->addWidget(m_checklist);
    m_group = new QComboBox(this);
    m_group->setObjectName(QStringLiteral("workspaceEditor"));
    m_group->setAccessibleName(QStringLiteral("Чек-лист для нового пункта"));
    layout->addWidget(m_group);
    auto *newItem = new QHBoxLayout;
    m_title = new QLineEdit(this);
    m_title->setObjectName(QStringLiteral("workspaceEditor"));
    m_title->setPlaceholderText(QStringLiteral("Новый пункт"));
    m_title->setAccessibleName(QStringLiteral("Название пункта"));
    m_title->setMaxLength(300);
    newItem->addWidget(m_title, 1);
    m_add = new QPushButton(QStringLiteral("Добавить"), this);
    newItem->addWidget(m_add);
    layout->addLayout(newItem);

    layout->addWidget(workspace::label(QStringLiteral("Подзадачи"), "taskSectionHeading", this));
    m_children = new QListWidget(this);
    m_children->setObjectName(QStringLiteral("taskSubtasks"));
    m_children->setAccessibleName(QStringLiteral("Подзадачи"));
    m_children->setMaximumHeight(140);
    workspace::configureRows(m_children, workspace::Row::Directory);
    layout->addWidget(m_children);
    m_newChild = new QPushButton(QStringLiteral("Новая подзадача"), this);
    layout->addWidget(m_newChild);
    layout->addWidget(workspace::label(QStringLiteral("Зависимости Ганта"), "taskSectionHeading", this));
    m_dependencies = new QListWidget(this);
    m_dependencies->setAccessibleName(QStringLiteral("Зависимости Ганта"));
    m_dependencies->setMaximumHeight(140);
    workspace::configureRows(m_dependencies, workspace::Row::Directory);
    layout->addWidget(m_dependencies);
    auto *historyButton = new QPushButton(QStringLiteral("История задачи"), this);
    historyButton->setCheckable(true);
    layout->addWidget(historyButton);
    m_history = new QListWidget(this);
    m_history->setAccessibleName(QStringLiteral("История изменений задачи"));
    m_history->setMaximumHeight(230);
    workspace::configureRows(m_history, workspace::Row::Directory);
    m_history->hide();
    layout->addWidget(m_history);
    connect(historyButton, &QPushButton::toggled, this, [this](bool visible) {
        m_history->setVisible(visible);
        if (visible && !m_historyRequested && onHistory) {
            m_historyRequested = true;
            m_history->clear();
            workspace::loadingRow(m_history, QStringLiteral("Загружаем историю…"));
            onHistory();
        }
    });

    connect(m_title, &QLineEdit::textChanged, this, [this] { updateControls(); });
    connect(m_title, &QLineEdit::returnPressed, m_add, &QPushButton::click);
    connect(m_add, &QPushButton::clicked, this, [this] {
        if (!onAdd || !m_add->isEnabled())
            return;
        m_adding = true;
        setChecklistPending(true);
        onAdd(m_group->currentData().toString(), m_title->text().trimmed());
    });
    connect(m_checklist, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int) {
        const auto id = item->data(0, Qt::UserRole).toString();
        if (id.isEmpty())
            return;
        const bool original = item->data(0, Qt::UserRole + 1).toBool();
        const bool desired = item->checkState(0) == Qt::Checked;
        if (original == desired)
            return;
        const QSignalBlocker blocker(m_checklist);
        item->setCheckState(0, original ? Qt::Checked : Qt::Unchecked);
        if (!m_writable || m_checklistPending || !onToggle)
            return;
        setChecklistPending(true);
        onToggle(id, desired);
    });
    connect(m_children, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        const auto id = item->data(workspace::Id).toString();
        if (!id.isEmpty() && onOpenChild)
            onOpenChild(id);
    });
    connect(m_dependencies, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        const auto id = item->data(workspace::Id).toString();
        if (!id.isEmpty() && onOpenChild)
            onOpenChild(id);
    });
    connect(m_newChild, &QPushButton::clicked, this, [this] {
        if (onCreateChild)
            onCreateChild();
    });
    workspace::loadingRow(m_children, QStringLiteral("Загружаем подзадачи…"));
    workspace::loadingRow(m_dependencies, QStringLiteral("Загружаем связи…"));
    updateControls();
}

void TaskRelationsPanel::setChecklist(const QVector<TaskChecklistItem> &items)
{
    const QSignalBlocker blocker(m_checklist);
    const auto selected = m_group->currentData().toString();
    m_checklist->clear();
    m_group->clear();
    QHash<QString, QTreeWidgetItem *> groups;
    for (const auto &entry : items) {
        if (!entry.parentId.isEmpty())
            continue;
        auto *item = new QTreeWidgetItem(m_checklist, QStringList{entry.title});
        groups.insert(entry.id, item);
        m_group->addItem(entry.title, entry.id);
    }
    m_group->addItem(QStringLiteral("Новый чек-лист"), QString());
    if (!selected.isEmpty() && m_group->findData(selected) >= 0)
        m_group->setCurrentIndex(m_group->findData(selected));
    for (const auto &entry : items) {
        if (entry.parentId.isEmpty())
            continue;
        auto *group = groups.value(entry.parentId);
        auto *item = group ? new QTreeWidgetItem(group, QStringList{entry.title})
                           : new QTreeWidgetItem(m_checklist, QStringList{entry.title});
        item->setFlags(entry.canToggle ? item->flags() | Qt::ItemIsUserCheckable
                                       : item->flags() & ~Qt::ItemIsUserCheckable);
        item->setData(0, Qt::UserRole, entry.id);
        item->setData(0, Qt::UserRole + 1, entry.complete);
        item->setCheckState(0, entry.complete ? Qt::Checked : Qt::Unchecked);
    }
    if (items.isEmpty())
        new QTreeWidgetItem(m_checklist, QStringList{QStringLiteral("Пунктов пока нет")});
    m_checklist->expandAll();
    m_checklistLoaded = true;
    setChecklistPending(false);
}

void TaskRelationsPanel::setSubtasks(const QVector<Task> &tasks)
{
    m_children->clear();
    for (const auto &task : tasks)
        workspace::addRow(m_children, task.name, task.status, QStringLiteral("№%1").arg(task.id), task.id);
    if (tasks.isEmpty())
        workspace::emptyRow(m_children, QStringLiteral("Подзадач пока нет"));
}

void TaskRelationsPanel::setDependencies(const QVector<TaskDependency> &dependencies)
{
    m_dependencies->clear();
    for (const auto &entry : dependencies) {
        auto detail = entry.prerequisite ? QStringLiteral("Предшественник") : QStringLiteral("Последователь");
        const auto type = entry.type.split(QLatin1Char('_'));
        if (type.size() == 2) {
            const auto endpoint = [](const QString &value) {
                if (value == QLatin1String("start"))
                    return QStringLiteral("начало");
                if (value == QLatin1String("finish"))
                    return QStringLiteral("окончание");
                return QString();
            };
            const auto first = endpoint(type.at(0));
            const auto second = endpoint(type.at(1));
            if (!first.isEmpty() && !second.isEmpty())
                detail += QStringLiteral(" · %1–%2").arg(first, second);
        }
        const auto number = QStringLiteral("№%1").arg(entry.taskId);
        workspace::addRow(m_dependencies, entry.title.isEmpty() ? QStringLiteral("Задача %1").arg(number) : entry.title,
                          detail, number, entry.taskId);
    }
    if (dependencies.isEmpty())
        workspace::emptyRow(m_dependencies, QStringLiteral("Связей пока нет"));
}

void TaskRelationsPanel::setHistory(const QVector<TaskHistoryEntry> &history, bool limited)
{
    m_historyRequested = false;
    m_history->clear();
    for (const auto &entry : history) {
        auto detail = entry.author;
        if (!entry.detail.isEmpty())
            detail = detail.isEmpty() ? entry.detail : detail + QStringLiteral(" · ") + entry.detail;
        workspace::addRow(m_history, entry.event, detail,
                          entry.at.toLocalTime().toString(QStringLiteral("dd.MM.yyyy HH:mm")), entry.id);
    }
    if (history.isEmpty())
        workspace::emptyRow(m_history, QStringLiteral("Записей пока нет"));
    else if (limited)
        workspace::emptyRow(m_history, QStringLiteral("Показаны последние записи"));
}

void TaskRelationsPanel::historyFailed()
{
    m_historyRequested = false;
    m_history->clear();
    workspace::emptyRow(m_history, QStringLiteral("История недоступна"));
}

void TaskRelationsPanel::setWritable(bool writable)
{
    m_writable = writable;
    updateControls();
}

void TaskRelationsPanel::checklistReadFailed()
{
    if (!m_checklistLoaded) {
        m_checklist->clear();
        new QTreeWidgetItem(m_checklist, QStringList{QStringLiteral("Чек-листы недоступны")});
    }
    setChecklistPending(false);
}

void TaskRelationsPanel::subtasksReadFailed()
{
    m_children->clear();
    workspace::emptyRow(m_children, QStringLiteral("Подзадачи недоступны"));
}

void TaskRelationsPanel::dependenciesReadFailed()
{
    m_dependencies->clear();
    workspace::emptyRow(m_dependencies, QStringLiteral("Связи недоступны"));
}

void TaskRelationsPanel::setChecklistPending(bool pending)
{
    m_checklistPending = pending;
    if (!pending)
        m_adding = false;
    updateControls();
}

void TaskRelationsPanel::confirmChecklistChange()
{
    if (m_adding)
        m_title->clear();
    m_adding = false;
}

void TaskRelationsPanel::updateControls()
{
    const bool editing = m_writable && m_checklistLoaded && !m_checklistPending;
    m_checklist->setEnabled(editing || !m_writable);
    m_title->setEnabled(editing);
    m_group->setEnabled(editing);
    m_add->setEnabled(editing && !m_title->text().trimmed().isEmpty());
    m_newChild->setEnabled(m_writable);
}

} // namespace bx::ui
