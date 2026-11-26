#pragma once

#include <QDialog>
#include <functional>

class QLabel;
class QLineEdit;
class QPushButton;

namespace bx::ui {

class NameDialog final : public QDialog {
public:
    explicit NameDialog(QWidget *parent);

    void setOnCreate(std::function<void(const QString &)> handler);
    void setStatus(const QString &text, bool loading = false);
    void setWritable(bool writable);

private:
    void updateButton();

    QLineEdit *m_input = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_create = nullptr;
    bool m_writable = true;
    std::function<void(const QString &)> m_onCreate;
};

} // namespace bx::ui
