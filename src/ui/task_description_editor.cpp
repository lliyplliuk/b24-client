#include "ui/task_description_editor.h"

#include "ui/task_description/input.h"
#include "ui/task_description/bb_code.h"
#include "ui/loading_indicator.h"
#include "ui/message_markup.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShortcut>
#include <QTextList>
#include <QToolButton>
#include <QVBoxLayout>

#include <utility>

namespace bx::ui {

TaskDescriptionEditor::TaskDescriptionEditor(QWidget *parent)
    : QWidget(parent)
{
    using namespace Qt::StringLiterals;
    setObjectName(u"taskDescriptionEditor"_s);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    m_tools = new QWidget(this);
    m_tools->setObjectName(u"taskDescriptionTools"_s);
    m_tools->setStyleSheet(u"#taskDescriptionTools {background:#edf3f2;border:1px solid #dce4e4;border-radius:8px;}"
        "QToolButton {padding:4px 8px;color:#215c70;background:transparent;border-radius:5px;border:1px solid transparent;}"
        "QToolButton:hover {background:#e1eff0;} QToolButton:checked {background:#dcecf0;border-color:#aec9ce;}"
        "QToolButton:disabled {color:#81929b;}"_s);
    m_toolRow = new QHBoxLayout(m_tools);
    m_toolRow->setContentsMargins(5, 4, 5, 4);
    m_toolRow->setSpacing(3);
    layout->addWidget(m_tools);
    m_input = new task_description::Input(this);
    m_input->setObjectName(u"taskDescriptionInput"_s);
    m_input->setAccessibleName(tr("Описание задачи"));
    m_input->setPlaceholderText(tr("Добавьте описание задачи"));
    m_input->setMinimumHeight(140);
    layout->addWidget(m_input, 1);
    m_upload = new QLabel(this);
    m_upload->setTextFormat(Qt::PlainText);
    m_upload->setWordWrap(true);
    m_upload->setObjectName(u"taskDescriptionUploadStatus"_s);
    m_upload->hide();
    layout->addWidget(m_upload);

    m_bold = tool(u"B"_s, tr("Полужирный"), true);
    m_italic = tool(u"I"_s, tr("Курсив"), true);
    m_underline = tool(u"U"_s, tr("Подчёркнутый"), true);
    m_strike = tool(u"S"_s, tr("Зачёркнутый"), true);
    auto font = m_bold->font();
    font.setBold(true);
    m_bold->setFont(font);
    font = m_italic->font();
    font.setItalic(true);
    m_italic->setFont(font);
    font = m_underline->font();
    font.setUnderline(true);
    m_underline->setFont(font);
    font = m_strike->font();
    font.setStrikeOut(true);
    m_strike->setFont(font);

    connect(m_bold, &QToolButton::clicked, this, [this](bool active) {
        QTextCharFormat format;
        format.setFontWeight(active ? QFont::Bold : QFont::Normal);
        apply(format);
    });
    connect(m_italic, &QToolButton::clicked, this, [this](bool active) {
        QTextCharFormat format;
        format.setFontItalic(active);
        apply(format);
    });
    connect(m_underline, &QToolButton::clicked, this, [this](bool active) {
        QTextCharFormat format;
        format.setFontUnderline(active);
        apply(format);
    });
    connect(m_strike, &QToolButton::clicked, this, [this](bool active) {
        QTextCharFormat format;
        format.setFontStrikeOut(active);
        apply(format);
    });
    bindShortcut(QKeySequence::Bold, m_bold);
    bindShortcut(QKeySequence::Italic, m_italic);
    bindShortcut(QKeySequence::Underline, m_underline);
    m_bullets = tool(u"•"_s, tr("Маркированный список"), true);
    m_numbers = tool(u"1."_s, tr("Нумерованный список"), true);
    connect(m_bullets, &QToolButton::clicked, this, [this](bool active) { formatList(QTextListFormat::ListDisc, active); });
    connect(m_numbers, &QToolButton::clicked, this, [this](bool active) { formatList(QTextListFormat::ListDecimal, active); });
    m_toolRow->addStretch();
    auto *link = tool(tr("Ссылка"), tr("Вставить ссылку"));
    connect(link, &QToolButton::clicked, this, [this] { insertLink(); });
    m_attach = tool(tr("Файл"), tr("Прикрепить файл"));
    connect(m_attach, &QToolButton::clicked, this, [this] {
        auto *dialog = new QFileDialog(this, tr("Выберите файлы"));
        dialog->setFileMode(QFileDialog::ExistingFiles);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(dialog, &QFileDialog::filesSelected, m_input, &task_description::Input::requestFiles);
        dialog->open();
    });

    connect(m_input, &QTextEdit::currentCharFormatChanged, this, [this](const QTextCharFormat &format) {
        m_bold->setChecked(format.fontWeight() >= QFont::DemiBold);
        m_italic->setChecked(format.fontItalic());
        m_underline->setChecked(format.fontUnderline() && !format.isAnchor());
        m_strike->setChecked(format.fontStrikeOut());
    });
    connect(m_input, &QTextEdit::cursorPositionChanged, this, [this] {
        const auto *list = m_input->textCursor().currentList();
        const auto style = list ? list->format().style() : QTextListFormat::ListStyleUndefined;
        m_bullets->setChecked(style == QTextListFormat::ListDisc);
        m_numbers->setChecked(style == QTextListFormat::ListDecimal);
    });
    refreshUpload();
}

void TaskDescriptionEditor::setBbCode(const QString &description)
{
    m_original = description;
    m_input->load(description);
}

void TaskDescriptionEditor::setCompactHeight()
{
    m_input->setMinimumHeight(64);
    m_input->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
}

QString TaskDescriptionEditor::bbCode() const
{
    return m_input->document()->isModified() ? m_input->bbCode() : m_original;
}

void TaskDescriptionEditor::insertAttachment(const QString &diskId, const QString &name, const QSize &imageSize)
{
    if (task_description::isDiskId(diskId))
        m_input->attach(task_description::diskReference(diskId, imageSize),
                        name.isEmpty() ? tr("Файл %1").arg(diskId) : name.simplified());
}

void TaskDescriptionEditor::setEditEnabled(bool enabled)
{
    m_input->setReadOnly(!enabled);
    m_tools->setEnabled(enabled);
    refreshUpload();
}

void TaskDescriptionEditor::setUploadStatus(const QString &status, bool loading)
{
    m_upload->setText(status);
    setLoadingIndicator(m_upload, loading);
    m_upload->setVisible(!status.isEmpty());
}

void TaskDescriptionEditor::setUploadEnabled(bool enabled)
{
    m_allowUpload = enabled;
    refreshUpload();
}

void TaskDescriptionEditor::setAttachHandler(std::function<void(const QStringList &)> handler)
{
    m_input->filesPasted = std::move(handler);
    refreshUpload();
}

void TaskDescriptionEditor::setImagePasteHandler(std::function<void(const QImage &)> handler)
{
    m_input->imagePasted = std::move(handler);
    refreshUpload();
}

QToolButton *TaskDescriptionEditor::tool(const QString &title, const QString &accessible, bool checkable)
{
    auto *button = new QToolButton(m_tools);
    button->setText(title);
    button->setAccessibleName(accessible);
    button->setToolTip(accessible);
    button->setCheckable(checkable);
    m_toolRow->addWidget(button);
    return button;
}

void TaskDescriptionEditor::bindShortcut(QKeySequence::StandardKey key, QToolButton *button)
{
    auto *shortcut = new QShortcut(QKeySequence(key), m_input);
    shortcut->setContext(Qt::WidgetShortcut);
    connect(shortcut, &QShortcut::activated, button, &QToolButton::click);
}

void TaskDescriptionEditor::refreshUpload()
{
    const bool allowed = m_allowUpload && !m_input->isReadOnly();
    m_input->uploadEnabled = allowed && (m_input->filesPasted || m_input->imagePasted);
    m_attach->setEnabled(allowed && bool(m_input->filesPasted));
}

void TaskDescriptionEditor::apply(const QTextCharFormat &format)
{
    m_input->mergeCurrentCharFormat(format);
    m_input->setFocus();
}

void TaskDescriptionEditor::formatList(QTextListFormat::Style style, bool active)
{
    auto cursor = m_input->textCursor();
    if (active) {
        QTextListFormat format;
        format.setStyle(style);
        format.setIndent(1);
        cursor.createList(format);
    } else {
        const int end = cursor.selectionEnd();
        const bool hasSelection = cursor.hasSelection();
        for (auto block = m_input->document()->findBlock(cursor.selectionStart());
             block.isValid() && (hasSelection ? block.position() < end : block.position() <= end); block = block.next()) {
            if (auto *list = block.textList())
                list->remove(block);
        }
    }
    m_input->setFocus();
}

void TaskDescriptionEditor::insertLink()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Вставить ссылку"));
    auto *form = new QVBoxLayout(&dialog);
    auto *label = new QLabel(tr("Адрес ссылки"), &dialog);
    auto *address = new QLineEdit(&dialog);
    address->setPlaceholderText(QStringLiteral("https://"));
    label->setBuddy(address);
    form->addWidget(label);
    form->addWidget(address);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    const auto current = m_input->textCursor().charFormat().anchorHref();
    address->setText(current);
    auto *accept = buttons->button(QDialogButtonBox::Ok);
    accept->setEnabled(!workspace::messageLinkTarget(current).isEmpty());
    connect(address, &QLineEdit::textChanged, accept, [accept](const QString &value) {
        accept->setEnabled(!workspace::messageLinkTarget(value).isEmpty());
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addWidget(buttons);
    dialog.setMinimumWidth(360);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto target = workspace::messageLinkTarget(address->text());
    QTextCharFormat format;
    format.setAnchor(true);
    format.setAnchorHref(target);
    format.setFontUnderline(true);
    format.setForeground(QColor(QStringLiteral("#215c70")));
    auto cursor = m_input->textCursor();
    if (cursor.hasSelection()) {
        cursor.mergeCharFormat(format);
    } else {
        cursor.insertText(target, format);
        m_input->setTextCursor(cursor);
        m_input->setCurrentCharFormat(QTextCharFormat{});
    }
    m_input->setFocus();
}

} // namespace bx::ui
