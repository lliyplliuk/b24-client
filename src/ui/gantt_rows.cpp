#include "ui/gantt_rows.h"

#include "ui/workspace_style.h"
#include <QPainter>

namespace bx::ui {

GanttRows::GanttRows(QObject *parent) : QStyledItemDelegate(parent)
{}

QSize GanttRows::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const
{
    return {0, 50};
}

void GanttRows::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    p->save();
    const auto rect = option.rect.adjusted(5, 1, -5, -1);
    if (option.state & QStyle::State_Selected)
        p->fillRect(rect, workspace::Selected);
    const int labelWidth = qBound(110, rect.width() / 3, 280);
    const auto title = index.data(workspace::Title).toString();
    const auto subtitle = index.data(workspace::Detail).toString();
    const QRect heading(rect.left() + 7, rect.top() + 4, labelWidth - 16, 22);
    p->setFont(workspace::rowFont(bodyTextPixels, true));
    p->setPen(workspace::Ink);
    p->drawText(heading, Qt::AlignVCenter, QFontMetrics(p->font()).elidedText(title, Qt::ElideRight, heading.width()));
    p->setFont(workspace::rowFont(secondaryTextPixels));
    p->setPen(workspace::Muted);
    const auto sub = heading.translated(0, 21);
    p->drawText(sub, Qt::AlignVCenter, QFontMetrics(p->font()).elidedText(subtitle, Qt::ElideRight, sub.width()));
    const QRect track(rect.left() + labelWidth, rect.top() + 17, std::max(1, rect.width() - labelWidth), 13);
    p->fillRect(track, QColor(QStringLiteral("#eef2ed")));
    const auto start = index.data(Qt::UserRole + 50).toDate();
    const auto end = index.data(Qt::UserRole + 51).toDate();
    if (m_first.isValid() && start.isValid() && end.isValid() && end >= start) {
        const auto width = std::max<qint64>(1, m_first.daysTo(m_last) + 1);
        const auto x = track.left() + int(double(m_first.daysTo(start)) / width * track.width());
        const auto right = track.left() + int(double(m_first.daysTo(end) + 1) / width * track.width());
        p->fillRect(QRect(x, track.top(), std::max(2, right - x), track.height()), workspace::Accent);
    }
    p->restore();
}

} // namespace bx::ui
