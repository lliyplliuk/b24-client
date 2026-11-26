#include "ui/task_picker_value_label.h"

#include "ui/typography.h"
#include <QEvent>

namespace bx::ui {

TaskPickerValueLabel::TaskPickerValueLabel(QWidget *parent) : QLabel(parent)
{
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setTextFormat(Qt::PlainText);
}

void TaskPickerValueLabel::setFullText(const QString &text)
{
    if (m_fullText == text)
        return;
    m_fullText = text;
    setAccessibleDescription(text);
    updateElision();
}

void TaskPickerValueLabel::setWrapLongText(bool wrap)
{
    if (wordWrap() == wrap)
        return;
    setWordWrap(wrap);
    updateElision();
}

void TaskPickerValueLabel::resizeEvent(QResizeEvent *event)
{
    QLabel::resizeEvent(event);
    updateElision();
}

void TaskPickerValueLabel::changeEvent(QEvent *event)
{
    QLabel::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
        updateElision();
}

void TaskPickerValueLabel::updateElision()
{
    auto line = m_fullText;
    line.replace(QLatin1Char('\n'), QLatin1Char(' '));
    line.replace(QLatin1Char('\r'), QLatin1Char(' '));
    const auto width = contentsRect().width();
    const auto visible = !wordWrap() && width > 0 ? fontMetrics().elidedText(line, Qt::ElideRight, width) : line;
    setText(visible);
    setToolTip(visible == line ? QString()
        : plainTooltip(m_fullText));
}

} // namespace bx::ui
