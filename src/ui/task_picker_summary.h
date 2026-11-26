#pragma once

#include "ui/task_picker_value_label.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QIcon>

namespace bx::ui {

class TaskPickerSummary final : public QWidget {
public:
    explicit TaskPickerSummary(QWidget *parent);
    void setCurrent(const QString &text, const QIcon &icon = {});
    void setPending(const QString &text, const QString &caption = QStringLiteral("Будет"));

private:
    QHBoxLayout *addLine(QVBoxLayout *layout, const char *name);
    static QLabel *addCaption(QHBoxLayout *line, const QString &caption);
    static TaskPickerValueLabel *addValue(QHBoxLayout *line, const QString &accessible);
    QLabel *m_icon = nullptr;
    QLabel *m_pendingCaption = nullptr;
    TaskPickerValueLabel *m_current = nullptr;
    TaskPickerValueLabel *m_pending = nullptr;
};

} // namespace bx::ui
