#include "ui/reaction_palette.h"

#include "ui/workspace_style.h"
#include "ui/typography.h"
#include <QWidgetAction>
#include <algorithm>
#include <utility>

namespace bx::ui {

QVector<const ReactionChoice *> rankedReactionChoices(const QVariantMap &frequency)
{
    QVector<const ReactionChoice *> choices;
    choices.reserve(std::size(reactionChoices));
    for (const auto &choice : reactionChoices)
        choices.append(&choice);
    if (!frequency.isEmpty()) {
        std::stable_sort(choices.begin(), choices.end(), [&](const auto *left, const auto *right) {
            return frequency.value(QString::fromLatin1(left->code)).toULongLong()
                > frequency.value(QString::fromLatin1(right->code)).toULongLong();
        });
    }
    return choices;
}

ReactionPalette::ReactionPalette(const QStringList &own, const QVector<const ReactionChoice *> &choices, QWidget *parent)
    : QMenu(parent)
{
    setObjectName(QStringLiteral("reactionPalette"));
    setAccessibleName(QStringLiteral("Все реакции"));
    setStyleSheet(QStringLiteral(R"(
            QMenu#reactionPalette { background:#fffefa; border:1px solid #ccdbd5; border-radius:9px; padding:5px; }
            QListWidget#reactionChoices { background:#fffefa; color:#182d3b; border:none; outline:none; }
            QListWidget#reactionChoices::item { border:1px solid transparent; border-radius:7px; }
            QListWidget#reactionChoices::item:hover { background:#e7f1ed; }
            QListWidget#reactionChoices::item:selected { border-color:#54877a; background:#dcece6; }
            QListWidget#reactionChoices QScrollBar:vertical { width:9px; background:#edf3ee; border-radius:4px; }
            QListWidget#reactionChoices QScrollBar::handle:vertical { background:#a4bfb7; min-height:22px; border-radius:4px; }
            QListWidget#reactionChoices QScrollBar::add-line:vertical,
            QListWidget#reactionChoices QScrollBar::sub-line:vertical { height:0; }
            QListWidget#reactionChoices QScrollBar::add-page:vertical,
            QListWidget#reactionChoices QScrollBar::sub-page:vertical { background:transparent; }
        )"));
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("reactionChoices"));
    m_list->setAccessibleName(QStringLiteral("Выберите реакцию"));
    m_list->setViewMode(QListView::IconMode);
    m_list->setMovement(QListView::Static);
    m_list->setFlow(QListView::LeftToRight);
    m_list->setWrapping(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    constexpr int cell = 40;
    constexpr int columns = 10;
    constexpr int layoutMargin = 4;
    m_list->setGridSize(QSize(cell, cell));
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setUniformItemSizes(true);
    m_list->setMouseTracking(true);
    const int rows = (int(choices.size()) + columns - 1) / columns;
    m_list->setFixedSize(columns * cell + layoutMargin, rows * cell + layoutMargin);
    m_list->setFont(workspace::reactionFont(24));
    for (const auto *reaction : choices) {
        const auto code = QString::fromLatin1(reaction->code);
        const auto caption = QString::fromUtf8(reaction->caption);
        const bool selected = own.contains(code);
        auto *item = new QListWidgetItem(QString::fromUtf8(reaction->emoji), m_list);
        const auto action = selected ? QStringLiteral("Снять реакцию «%1»").arg(caption)
                                     : QStringLiteral("Поставить реакцию «%1»").arg(caption);
        item->setData(Qt::UserRole, code);
        item->setData(Qt::AccessibleTextRole, action);
        item->setToolTip(plainTooltip(action));
        item->setTextAlignment(Qt::AlignCenter);
        if (selected)
            item->setBackground(QColor("#e0e1fa"));
    }
    m_list->setCurrentRow(0);
    auto *action = new QWidgetAction(this);
    action->setDefaultWidget(m_list);
    addAction(action);
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) { choose(item); });
    connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) { choose(item); });
    connect(this, &QMenu::aboutToShow, this, [this] { m_list->setFocus(Qt::PopupFocusReason); });
}

void ReactionPalette::setOnChosen(std::function<void(const QString &)> handler)
{
    m_chosen = std::move(handler);
}

void ReactionPalette::choose(QListWidgetItem *item)
{
    if (!m_chosen || !item)
        return;
    auto handler = std::exchange(m_chosen, {});
    const auto code = item->data(Qt::UserRole).toString();
    hide();
    handler(code);
}

} // namespace bx::ui
