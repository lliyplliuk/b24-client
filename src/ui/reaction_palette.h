#pragma once

#include "ui/reaction_catalog.h"
#include <QListWidget>
#include <QMenu>
#include <QVariantMap>
#include <functional>

namespace bx::ui {

QVector<const ReactionChoice *> rankedReactionChoices(const QVariantMap &frequency);

class ReactionPalette final : public QMenu {
public:
    ReactionPalette(const QStringList &own, const QVector<const ReactionChoice *> &choices, QWidget *parent);
    void setOnChosen(std::function<void(const QString &)> handler);

private:
    void choose(QListWidgetItem *item);
    QListWidget *m_list = nullptr;
    std::function<void(const QString &)> m_chosen;
};

} // namespace bx::ui
