#include "ui/workspace_presentation.h"

#include <QMenu>
#include <utility>

namespace bx::ui {

QMenu *directoryPinMenu(QWidget *parent, bool pinned, std::function<bool()> canChange,
                        std::function<void()> togglePin)
{
    auto *menu = new QMenu(parent);
    QObject::connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    auto *action = menu->addAction(pinned ? QStringLiteral("Открепить") : QStringLiteral("Закрепить"));
    action->setEnabled(canChange());
    QObject::connect(action, &QAction::triggered, parent,
                     [canChange = std::move(canChange), togglePin = std::move(togglePin)] {
        if (canChange())
            togglePin();
    });
    return menu;
}

QLabel *emptyState(const QString &text, QWidget *parent)
{
    auto *result = workspace::label(text, "empty", parent);
    result->setAlignment(Qt::AlignCenter);
    return result;
}

ListViewportAnchor listViewportAnchor(const QListWidget *list)
{
    ListViewportAnchor anchor;
    anchor.scroll = list->verticalScrollBar()->value();
    const auto first = list->indexAt(QPoint(4, 0));
    if (first.isValid()) {
        anchor.offset = list->visualRect(first).top();
        for (int row = first.row(); row < list->count(); ++row)
            anchor.successors.append(list->item(row)->data(workspace::Id).toString());
    }
    return anchor;
}

void restoreListViewport(QListWidget *list, const ListViewportAnchor &anchor)
{
    list->doItemsLayout();
    auto *scroll = list->verticalScrollBar();
    QHash<QString, QListWidgetItem *> byId;
    for (int row = 0; row < list->count(); ++row) {
        auto *item = list->item(row);
        byId.insert(item->data(workspace::Id).toString(), item);
    }
    for (const auto &id : anchor.successors) {
        if (const auto *item = byId.value(id)) {
            scroll->setValue(scroll->value() + list->visualItemRect(item).top() - anchor.offset);
            return;
        }
    }
    scroll->setValue(anchor.scroll);
}

} // namespace bx::ui
