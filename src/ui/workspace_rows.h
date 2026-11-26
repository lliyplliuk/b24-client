#pragma once

#include "ui/workspace_row_style.h"

class QLabel;
class QListWidget;
class QListWidgetItem;
class QWidget;

namespace bx::workspace {

class RowPainter;

QLabel *label(const QString &text, const char *name, QWidget *parent);

QListWidgetItem *addRow(QListWidget *list, const QString &title, const QString &detail = {},
                       const QString &meta = {}, const QString &id = {}, bool showTextToolTip = true);

QListWidgetItem *addMarkupRow(QListWidget *list, const QString &title, const QString &raw,
                             const QString &meta = {}, const QString &id = {});

void emptyRow(QListWidget *list, const QString &message);

void loadingRow(QListWidget *list, const QString &message);

RowPainter *configureRows(QListWidget *list, Row style);

} // namespace bx::workspace
