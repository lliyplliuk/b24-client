#pragma once

#include <QHash>
#include <QStyledItemDelegate>

#include <functional>

class QListWidget;
class QListWidgetItem;

namespace bx {

struct Task;

class BoardCardPainter final : public QStyledItemDelegate {
public:
    using AvatarSource = std::function<const QPixmap *(const QUrl &)>;
    static constexpr int CardCreatorAvatar = Qt::UserRole + 56;
    static constexpr int CardAssigneeAvatar = Qt::UserRole + 57;
    static QListWidgetItem *addTask(QListWidget *list, const Task &task);

    BoardCardPainter(QListWidget *list, AvatarSource avatar);
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const override;
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    static int pills(const QStringList &labels, int width, QPainter *p, const QPoint &origin, bool deadline = false);
    void avatar(QPainter &p, const QRect &box, const QString &name, const QUrl &url) const;
    int content(const QModelIndex &index, int width, QPainter *p = nullptr, const QPoint &origin = {}) const;

    QListWidget *const m_list;
    const AvatarSource m_avatar;
    mutable int m_width = -1;
    // The board rebuilds each column when its rows change.
    mutable QHash<int, int> m_heights;
};

} // namespace bx
