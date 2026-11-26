#include "ui/task_dialog_controls.h"

#include "ui/workspace_presentation.h"

#include <QPushButton>

namespace bx::ui {

QString editorStyles()
{
    return QStringLiteral(R"(
        QDialog[workspaceDialog="true"] { background: #f4f4ef; }
        QLineEdit#workspaceEditor, QPlainTextEdit#workspaceEditor, QComboBox#workspaceEditor, QDateTimeEdit#workspaceEditor {
            color: #182d3b; background: #fffefa; border: 1px solid #cad7d7;
            border-radius: 7px; padding: 5px 7px;
            selection-background-color: #215c70; selection-color: #fffefa;
        }
        QLineEdit#workspaceEditor:focus, QPlainTextEdit#workspaceEditor:focus,
        QComboBox#workspaceEditor:focus { border-color: #215c70; }
        QLineEdit#workspaceEditor:read-only, QPlainTextEdit#workspaceEditor:read-only { background: #f5f5ef; }
        QLabel#fieldCaption { color: #435d68; font-size: 14px; font-weight: 600; }
        QWidget#taskSection { background: #fffefa; border: 1px solid #dfe7e0; border-radius: 9px; }
        QWidget#taskDetailsPane, QScrollArea#taskDetailsScroll { background: #f4f4ef; border: 0; }
        QLabel#taskCardHeading { color: #182d3b; font-size: 20px; font-weight: 650; }
        QLabel#taskSectionHeading { color: #215c70; font-size: 17px; font-weight: 650; }
        QPushButton#taskRequisiteRow { background: #f4f8f5; border: 1px solid #dce7e3; border-radius: 6px; }
        QPushButton#taskRequisiteRow:hover { background: #e7f1ed; border-color: #96b6ad; }
        QPushButton#taskRequisiteRow:focus { border-color: #215c70; }
        QPushButton#taskRequisiteRow:disabled { background: transparent; border-color: transparent; }
        QLabel#taskRequisiteValue { color: #182d3b; }
        QPushButton#taskRequisiteRow[stageField="true"]:enabled QLabel#taskRequisiteValue { color: #215c70; font-weight: 600; }
        QWidget#taskRequisiteDivider { background: #dfe7e0; }
        QFrame#taskRequisitePopup { background: #fffefa; border: 1px solid #9fbdb4; border-radius: 8px; }
        QListWidget#taskRequisiteChoices { background: #fffefa; border: 0; outline: none; color: #182d3b; }
        QWidget#taskPickerCurrent { background: #e8f2ed; border: 1px solid #cfe1d9; border-radius: 7px; }
        QWidget#taskPickerPending { background: #edf3f6; border: 1px solid #d5e2e8; border-radius: 7px; }
        QLabel#taskPickerCaption { color: #435d68; font-size: 14px; font-weight: 600; }
        QLabel#taskPickerValue { color: #182d3b; font-weight: 600; }
        QLabel#taskCopyFeedback { color: #215c70; font-size: 14px; }
        QFrame#taskSelectionField { background: #fffefa; border: 1px solid #8fb3a9; border-radius: 7px; }
        QLineEdit#taskSelectionSearch { background: transparent; border: 0; color: #182d3b; }
        QScrollArea#taskInlineSelectionScroll, QWidget#taskInlineSelectionItems { background: transparent; border: 0; }
        QWidget#taskInlineSelectionPill { background: #e3f0e9; border-radius: 5px; }
        QWidget#taskInlineSelectionPill[pending="true"] { background: #dfedf4; }
        QLabel#taskInlineSelectionValue { color: #173b43; font-weight: 600; }
        QWidget#taskInlineDraftDot { background: #28657b; border-radius: 3px; }
        QPushButton#taskInlineSelectionRemove { padding: 0; background: transparent; border: 0;
                                               color: #2f5b63; font-size: 14px; }
        QPushButton#taskInlineSelectionRemove:hover, QPushButton#taskInlineSelectionRemove:focus {
            background: #c8e0d6; border: 1px solid #759f92; }
        QListWidget#taskContent { background: transparent; }
        QSplitter#taskCardSplitter::handle:hover { background: #83aba5; }
        QPushButton#viewToggle:checked { border-color: #215c70; background: #dcecf0; }
    )");
}

void editor(QWidget *widget)
{
    widget->setObjectName(QStringLiteral("workspaceEditor"));
    auto palette = widget->palette();
    palette.setColor(QPalette::PlaceholderText, workspace::Muted);
    widget->setPalette(palette);
}

QPushButton *button(const QString &title, const QString &name, QWidget *parent)
{
    auto *result = new QPushButton(title, parent);
    result->setAccessibleName(name);
    result->setAutoDefault(false);
    return result;
}

void setStatusLabel(QLabel *label, const QString &text, bool loading)
{
    label->setText(text);
    setLoadingIndicator(label, loading && !text.isEmpty());
    label->setVisible(!text.isEmpty());
}

} // namespace bx::ui
