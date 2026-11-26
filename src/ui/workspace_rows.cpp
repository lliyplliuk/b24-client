#include "ui/workspace_rows.h"

#include "ui/loading_indicator.h"
#include "ui/message_markup.h"
#include "ui/row_painter.h"
#include "ui/typography.h"

#include <QLabel>
#include <QListWidget>

namespace bx::workspace {

QLabel *label(const QString &text, const char *name, QWidget *parent)
{
    auto *result = new QLabel(text, parent);
    result->setObjectName(QString::fromLatin1(name));
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    return result;
}

QListWidgetItem *addRow(QListWidget *list, const QString &title, const QString &detail,
                       const QString &meta, const QString &id, bool showTextToolTip)
{
    auto *item = new QListWidgetItem(list);
    QStringList accessible;
    for (const auto &part : {title, detail, meta}) {
        if (!part.isEmpty())
            accessible.append(part);
    }
    item->setText(accessible.join(QStringLiteral("\n")));
    item->setData(Id, id);
    item->setData(Title, title);
    item->setData(Detail, detail);
    item->setData(Meta, meta);
    if (showTextToolTip)
        item->setToolTip(ui::plainTooltip(item->text()).replace(QLatin1Char('\n'), QStringLiteral("<br>")));
    return item;
}

QListWidgetItem *addMarkupRow(QListWidget *list, const QString &title, const QString &raw,
                             const QString &meta, const QString &id)
{
    const auto markup = parseMessageMarkup(raw);
    auto *item = addRow(list, title, markup.plain, meta, id, false);
    if (!markup.html.isEmpty()) {
        item->setData(MarkupHtml, markup.html);
        item->setData(MarkupBlock, markup.block);
    }
    if (!markup.links.isEmpty())
        item->setData(MessageLinks, markup.links);
    return item;
}

void emptyRow(QListWidget *list, const QString &message)
{
    auto *item = addRow(list, QString(), message);
    item->setFlags(Qt::ItemIsEnabled);
}

void loadingRow(QListWidget *list, const QString &message)
{
    auto *item = new QListWidgetItem(list);
    item->setFlags(Qt::ItemIsEnabled);
    item->setData(Qt::AccessibleTextRole, message);
    auto *caption = label(message, "quiet", list);
    caption->setContentsMargins(10, 2, 8, 2);
    ui::setLoadingIndicator(caption, true);
    item->setSizeHint(caption->sizeHint().expandedTo(QSize(0, 36)));
    list->setItemWidget(item, caption);
}

RowPainter *configureRows(QListWidget *list, Row style)
{
    list->setFrameShape(QFrame::NoFrame);
    list->setMouseTracking(true);
    list->setResizeMode(QListView::Adjust);
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *delegate = new RowPainter(list, style);
    list->setItemDelegate(delegate);
    return delegate;
}

} // namespace bx::workspace
