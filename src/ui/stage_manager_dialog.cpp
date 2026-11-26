#include "ui/stage_manager_dialog.h"

#include "ui/task_dialog_controls.h"
#include "ui/workspace_presentation.h"

#include <QColorDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace bx::ui {

StageManagerDialog::StageManagerDialog(const QString &projectId, QWidget *parent)
    : QDialog(parent), m_projectId(projectId)
{
    setProperty("workspaceDialog", true);
    setWindowTitle(QStringLiteral("Этапы доски"));
    setObjectName(QStringLiteral("stageManagerDialog"));
    setAccessibleName(QStringLiteral("Управление этапами"));
    resize(525, 400);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 10);
    layout->setSpacing(7);
    m_stages = new QListWidget(this);
    m_stages->setAccessibleName(QStringLiteral("Список этапов"));
    workspace::configureRows(m_stages, workspace::Row::Card);
    layout->addWidget(m_stages, 1);
    auto *row = new QHBoxLayout;
    m_title = new QLineEdit(this);
    editor(m_title);
    m_title->setAccessibleName(QStringLiteral("Название этапа"));
    m_title->setPlaceholderText(QStringLiteral("Название этапа"));
    row->addWidget(m_title, 1);
    m_color = button(QStringLiteral("Цвет…"), QStringLiteral("Изменить цвет этапа"), this);
    connect(m_color, &QPushButton::clicked, this, [this] { chooseColor(); });
    row->addWidget(m_color);
    m_add = button(QStringLiteral("Добавить"), QStringLiteral("Добавить этап"), this);
    connect(m_add, &QPushButton::clicked, this, [this] {
        if (m_onSave)
            m_onSave({}, m_title->text().trimmed(), {}, {});
    });
    row->addWidget(m_add);
    layout->addLayout(row);
    auto *actions = new QHBoxLayout;
    m_rename = button(QStringLiteral("Переименовать"), QStringLiteral("Переименовать этап"), this);
    connect(m_rename, &QPushButton::clicked, this, [this] {
        if (m_onSave)
            m_onSave(currentStage(), m_title->text().trimmed(), {}, {});
    });
    actions->addWidget(m_rename);
    m_up = button(QStringLiteral("Выше"), QStringLiteral("Переместить этап выше"), this);
    connect(m_up, &QPushButton::clicked, this, [this] { move(-1); });
    actions->addWidget(m_up);
    m_down = button(QStringLiteral("Ниже"), QStringLiteral("Переместить этап ниже"), this);
    connect(m_down, &QPushButton::clicked, this, [this] { move(1); });
    actions->addWidget(m_down);
    m_remove = button(QStringLiteral("Удалить"), QStringLiteral("Удалить этап"), this);
    connect(m_remove, &QPushButton::clicked, this, [this] {
        if (m_onDelete && m_remove->isEnabled())
            m_onDelete(currentStage());
    });
    actions->addWidget(m_remove);
    actions->addStretch(1);
    auto *close = button(QStringLiteral("Готово"), QStringLiteral("Закрыть управление этапами"), this);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    actions->addWidget(close);
    layout->addLayout(actions);
    m_status = workspace::label(QString(), "quiet", this);
    m_status->setAccessibleName(QStringLiteral("Состояние этапов"));
    layout->addWidget(m_status);
    m_status->hide();
    connect(m_stages, &QListWidget::currentRowChanged, this, [this](int index) {
        if (validRow(index))
            m_title->setText(m_data.at(index).name);
        updateColorSwatch();
        updateButtons();
    });
    connect(m_title, &QLineEdit::textChanged, this, [this] { updateButtons(); });
    updateButtons();
}

void StageManagerDialog::setStages(const QVector<Stage> &stages)
{
    const auto selected = currentStage();
    const QSignalBlocker blocker(m_stages);
    m_data = stages;
    m_stages->clear();
    for (const auto &stage : stages) {
        const auto count = stage.taskCount ? QStringLiteral("Задач: %1").arg(*stage.taskCount)
            : QStringLiteral("Загружено: %1").arg(stage.tasks.size());
        auto *item = workspace::addRow(m_stages, stage.name, count, {}, stage.id);
        if (stage.id == selected)
            m_stages->setCurrentItem(item);
    }
    updateColorSwatch();
    updateButtons();
}

void StageManagerDialog::setStatus(const QString &text, bool loading)
{
    setStatusLabel(m_status, text, loading);
}

void StageManagerDialog::setWritable(bool writable)
{
    m_writable = writable;
    updateButtons();
}

void StageManagerDialog::setOnSave(std::function<void(const QString &, const QString &, const QString &, const QString &)> handler)
{
    m_onSave = std::move(handler);
}

void StageManagerDialog::setOnDelete(std::function<void(const QString &)> handler)
{
    m_onDelete = std::move(handler);
}

bool StageManagerDialog::validRow(int index) const
{
    return index >= 0 && index < m_data.size();
}

QString StageManagerDialog::currentStage() const
{
    const auto *item = m_stages->currentItem();
    return item ? item->data(workspace::Id).toString() : QString();
}

void StageManagerDialog::chooseColor()
{
    const int row = m_stages->currentRow();
    if (!m_writable || !m_onSave || !validRow(row))
        return;
    const auto &stage = m_data.at(row);
    const QColor initial(QLatin1Char('#') + stage.color);
    auto *picker = new QColorDialog(initial.isValid() ? initial : QColor("#dce5e4"), this);
    picker->setAttribute(Qt::WA_DeleteOnClose);
    picker->setOption(QColorDialog::DontUseNativeDialog);
    picker->setWindowTitle(QStringLiteral("Цвет этапа"));
    connect(picker, &QColorDialog::colorSelected, this, [this, id = stage.id](const QColor &color) {
        const int selected = m_stages->currentRow();
        if (!m_writable || !m_onSave || !validRow(selected) || !color.isValid())
            return;
        const auto &current = m_data.at(selected);
        const auto hex = color.name(QColor::HexRgb).mid(1);
        if (current.id == id && current.color != hex)
            m_onSave(id, {}, {}, hex);
    });
    picker->open();
}

void StageManagerDialog::move(int offset)
{
    const int current = m_stages->currentRow();
    if (!m_onSave || !validRow(current) || !validRow(current + offset))
        return;
    const auto &stage = m_data.at(current);
    const int predecessor = offset < 0 ? current - 2 : current + 1;
    m_onSave(stage.id, stage.name, predecessor < 0 ? QStringLiteral("0") : m_data.at(predecessor).id, {});
}

void StageManagerDialog::updateColorSwatch()
{
    const int row = m_stages->currentRow();
    const QColor color(validRow(row) ? QLatin1Char('#') + m_data.at(row).color : QString());
    if (color.isValid()) {
        QPixmap swatch(13, 13);
        swatch.fill(color);
        m_color->setIcon(QIcon(swatch));
    } else {
        m_color->setIcon({});
    }
}

void StageManagerDialog::updateButtons()
{
    const int row = m_stages->currentRow();
    const bool selected = validRow(row);
    const bool title = !m_title->text().trimmed().isEmpty();
    m_title->setReadOnly(!m_writable);
    m_add->setEnabled(m_writable && title);
    m_color->setEnabled(m_writable && selected);
    m_rename->setEnabled(m_writable && selected && title);
    m_up->setEnabled(m_writable && selected && row > 0);
    m_down->setEnabled(m_writable && selected && row + 1 < m_data.size());
    const auto count = selected ? m_data.at(row).taskCount : std::optional<qsizetype>();
    m_remove->setEnabled(m_writable && count && *count == 0 && m_data.at(row).tasks.isEmpty());
    m_remove->setToolTip(!selected ? QString() : (!count
        ? QStringLiteral("Портал не подтвердил, что этап пуст") : (*count > 0
        ? QStringLiteral("Чтобы удалить этап, сначала перенесите из него задачи") : QString())));
}

} // namespace bx::ui
