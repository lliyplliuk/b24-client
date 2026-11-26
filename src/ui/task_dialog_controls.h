#pragma once

#include <QString>

class QWidget;
class QLabel;
class QPushButton;

namespace bx::ui {

QString editorStyles();
void editor(QWidget *widget);
QPushButton *button(const QString &title, const QString &name, QWidget *parent);
void setStatusLabel(QLabel *label, const QString &text, bool loading = false);

} // namespace bx::ui
