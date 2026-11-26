#pragma once

#include <QLabel>

namespace bx::ui {

class TaskPickerValueLabel final : public QLabel {
public:
    explicit TaskPickerValueLabel(QWidget *parent);
    void setFullText(const QString &text);
    void setWrapLongText(bool wrap);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void updateElision();
    QString m_fullText;
};

} // namespace bx::ui
