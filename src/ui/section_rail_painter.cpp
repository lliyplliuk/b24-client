#include "ui/section_rail_painter.h"

#include "core/worktime.h"
#include "ui/glyphs.h"
#include "ui/workspace_style.h"

namespace bx::ui {

SectionRailPainter::SectionRailPainter(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

QSize SectionRailPainter::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const
{
    return {79, 65};
}

void SectionRailPainter::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    if (index.row() < 0 || index.row() > 3)
        return;
    static constexpr Glyph glyphs[] = {Glyph::Chat, Glyph::Projects, Glyph::Notices, Glyph::Clock};
    auto caption = index.row() == 2 ? QStringLiteral("События") : index.data().toString();
    const auto row = option.rect.adjusted(2, 2, -2, -2);
    const bool active = option.state & QStyle::State_Selected;
    const bool focused = option.state & QStyle::State_HasFocus;
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setPen(focused ? QPen(QColor("#97cbbf"), 1.2) : Qt::NoPen);
    p->setBrush(active ? QColor("#2b5660") : (option.state & QStyle::State_MouseOver
        ? QColor("#244954") : QColor(Qt::transparent)));
    p->drawRoundedRect(row, 9, 9);
    const auto color = active ? QColor("#dff6ed") : QColor("#b1caCD");
    const auto elapsed = index.data(SectionWorkdayElapsed).toString();
    const bool running = WorkdayState(index.data(SectionWorkdayStatus).toInt()) == WorkdayState::Running;
    const auto clockColor = running ? QColor("#a6e3cb") : color;
    const int iconTop = row.top() + (elapsed.isEmpty() ? 7 : 3);
    paintGlyph(*p, glyphs[index.row()], QRectF(row.center().x() - 10, iconTop, 21, 21), clockColor);
    p->setPen(color);
    p->setFont(workspace::rowFont(bodyTextPixels, active));
    const int captionTop = elapsed.isEmpty() ? 33 : 25;
    p->drawText(row.adjusted(2, captionTop, -2, -4), Qt::AlignHCenter | Qt::AlignTop, caption);
    if (!elapsed.isEmpty()) {
        p->setPen(clockColor);
        p->setFont(workspace::rowFont(secondaryTextPixels, true));
        p->drawText(row.adjusted(2, 43, -2, -1), Qt::AlignHCenter | Qt::AlignTop, elapsed);
    }
    if (running) {
        p->setPen(Qt::NoPen);
        p->setBrush(clockColor);
        p->drawEllipse(QRect(row.center().x() + 10, iconTop + 1, 5, 5));
    }
    const auto count = index.data(SectionUnreadCount).toLongLong();
    if (count > 0) {
        const auto text = count > 99 ? QStringLiteral("99+") : QString::number(count);
        p->setFont(workspace::rowFont(secondaryTextPixels, true));
        const int width = qMax(19, p->fontMetrics().horizontalAdvance(text) + 6);
        const QRect badge(row.right() - width, row.top(), width, 19);
        p->setPen(Qt::NoPen);
        p->setBrush(QColor("#aa4c58"));
        p->drawRoundedRect(badge, 7, 7);
        p->setPen(Qt::white);
        p->drawText(badge, Qt::AlignCenter, text);
    }
    p->restore();
}

} // namespace bx::ui
