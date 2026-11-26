#include "ui/name_dialog.h"

#include "ui/task_dialog_controls.h"
#include "ui/workspace_presentation.h"

#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace bx::ui {

NameDialog::NameDialog(QWidget *parent)
    : QDialog(parent)
{
    setProperty("workspaceDialog", true);
    setWindowTitle(QStringLiteral("Новый проект"));
    setObjectName(QStringLiteral("nameDialog"));
    setAccessibleName(QStringLiteral("Новый проект"));
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(workspace::label(QStringLiteral("Название"), "fieldCaption", this));
    m_input = new QLineEdit(this);
    editor(m_input);
    m_input->setAccessibleName(QStringLiteral("Название"));
    layout->addWidget(m_input);
    m_status = workspace::label(QString(), "quiet", this);
    layout->addWidget(m_status);
    m_status->hide();
    auto *actions = new QHBoxLayout;
    actions->addStretch(1);
    auto *close = button(QStringLiteral("Отмена"), QStringLiteral("Отмена"), this);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    actions->addWidget(close);
    m_create = button(QStringLiteral("Создать"), QStringLiteral("Подтвердить название"), this);
    m_create->setObjectName(QStringLiteral("primary"));
    const auto submit = [this] {
        if (m_create->isEnabled() && m_onCreate)
            m_onCreate(m_input->text().trimmed());
    };
    connect(m_create, &QPushButton::clicked, this, submit);
    connect(m_input, &QLineEdit::returnPressed, this, submit);
    connect(m_input, &QLineEdit::textChanged, this, [this] { updateButton(); });
    actions->addWidget(m_create);
    layout->addLayout(actions);
    updateButton();
}

void NameDialog::setOnCreate(std::function<void(const QString &)> handler)
{
    m_onCreate = std::move(handler);
}

void NameDialog::setStatus(const QString &text, bool loading)
{
    setStatusLabel(m_status, text, loading);
}

void NameDialog::setWritable(bool writable)
{
    m_writable = writable;
    m_input->setReadOnly(!writable);
    updateButton();
}

void NameDialog::updateButton()
{
    m_create->setEnabled(m_writable && !m_input->text().trimmed().isEmpty());
}

} // namespace bx::ui
