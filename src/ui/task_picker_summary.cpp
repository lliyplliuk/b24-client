#include "ui/task_picker_summary.h"

#include <QPixmap>

namespace bx::ui {

TaskPickerSummary::TaskPickerSummary(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto *currentLine = addLine(layout, "taskPickerCurrent");
    addCaption(currentLine, QStringLiteral("Сейчас"));
    m_icon = new QLabel(currentLine->parentWidget());
    m_icon->setFixedSize(22, 22);
    currentLine->addWidget(m_icon);
    m_current = addValue(currentLine, QStringLiteral("Текущее значение"));
    auto *pendingLine = addLine(layout, "taskPickerPending");
    m_pendingCaption = addCaption(pendingLine, QStringLiteral("Будет"));
    m_pending = addValue(pendingLine, QStringLiteral("Новый выбор"));
    m_pending->parentWidget()->hide();
}

void TaskPickerSummary::setCurrent(const QString &text, const QIcon &icon)
{
    m_current->setFullText(text.isEmpty() ? QStringLiteral("—") : text);
    m_icon->setVisible(!icon.isNull());
    m_icon->setPixmap(icon.isNull() ? QPixmap() : icon.pixmap(22, 22));
}

void TaskPickerSummary::setPending(const QString &text, const QString &caption)
{
    m_pendingCaption->setText(caption);
    m_pending->setFullText(text);
    m_pending->parentWidget()->setVisible(!text.isEmpty());
}

QHBoxLayout *TaskPickerSummary::addLine(QVBoxLayout *layout, const char *name)
{
    auto *line = new QWidget(this);
    line->setObjectName(QString::fromLatin1(name));
    auto *content = new QHBoxLayout(line);
    content->setContentsMargins(8, 5, 8, 5);
    content->setSpacing(7);
    layout->addWidget(line);
    return content;
}

QLabel *TaskPickerSummary::addCaption(QHBoxLayout *line, const QString &caption)
{
    auto *label = new QLabel(caption, line->parentWidget());
    label->setObjectName(QStringLiteral("taskPickerCaption"));
    line->addWidget(label);
    return label;
}

TaskPickerValueLabel *TaskPickerSummary::addValue(QHBoxLayout *line, const QString &accessible)
{
    auto *value = new TaskPickerValueLabel(line->parentWidget());
    value->setObjectName(QStringLiteral("taskPickerValue"));
    value->setAccessibleName(accessible);
    line->addWidget(value, 1);
    return value;
}

} // namespace bx::ui
