#include "ui/board_card_painter.h"
#include "core/models.h"
#include "ui/avatar_painter.h"
#include "ui/typography.h"
#include "ui/workspace_rows.h"

#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>

#include <utility>

namespace bx {

namespace {
constexpr int CardTags = Qt::UserRole + 53;
constexpr int CardCreator = Qt::UserRole + 54;
constexpr int CardAssignee = Qt::UserRole + 55;
constexpr int InsetX = 2;
constexpr int InsetY = 3;
constexpr int Padding = 9;

int wrapped(QString text, const QFont &font, int width, QPainter *painter, const QPoint &origin)
{
    text.replace(QLatin1Char('\n'), QChar::LineSeparator);
    QTextLayout layout(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.beginLayout();
    qreal height = 0;
    for (auto line = layout.createLine(); line.isValid(); line = layout.createLine()) {
        line.setLineWidth(qMax(1, width));
        line.setPosition(QPointF(0, height));
        height += line.height();
    }
    layout.endLayout();
    if (painter)
        layout.draw(painter, origin);
    return qCeil(height);
}
} // namespace

BoardCardPainter::BoardCardPainter(QListWidget *list, AvatarSource avatar)
    : QStyledItemDelegate(list), m_list(list), m_avatar(std::move(avatar)) {}

QSize BoardCardPainter::sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const
{
    const int width = m_list->viewport()->width();
    if (m_width != width) {
        m_width = width;
        m_heights.clear();
    }
    auto it = m_heights.constFind(index.row());
    if (it == m_heights.cend())
        it = m_heights.insert(index.row(), 2 * (InsetY + Padding) + content(index, width - 2 * (InsetX + Padding)));
    return {0, it.value()};
}

void BoardCardPainter::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const auto box = option.rect.adjusted(InsetX, InsetY, -InsetX, -InsetY);
    const bool selected = option.state & QStyle::State_Selected;
    p->save();
    p->setClipRect(option.rect);
    p->setRenderHint(QPainter::Antialiasing);
    p->setPen(option.state & QStyle::State_HasFocus ? workspace::Accent
                                                    : selected ? QColor("#8bb6b2") : workspace::Border);
    p->setBrush(selected ? workspace::Selected : workspace::Surface);
    p->drawRoundedRect(box, 8, 8);
    content(index, box.width() - 2 * Padding, p, box.topLeft() + QPoint(Padding, Padding));
    p->restore();
}

int BoardCardPainter::pills(const QStringList &labels, int width, QPainter *p, const QPoint &origin, bool deadline)
{
    constexpr int horizontalPadding = 4;
    constexpr int verticalPadding = 2;
    constexpr int gap = 4;
    const auto font = workspace::rowFont(ui::secondaryTextPixels);
    const QFontMetrics metrics(font);
    int x = 0, y = 0, lineHeight = 0;
    for (const auto &label : labels) {
        const int chipWidth = qMin(width, metrics.horizontalAdvance(label) + 2 * horizontalPadding);
        if (x != 0 && x + chipWidth > width) {
            x = 0;
            y += lineHeight + gap;
            lineHeight = 0;
        }
        const int textWidth = chipWidth - 2 * horizontalPadding;
        const bool fits = metrics.horizontalAdvance(label) <= textWidth;
        const int textHeight = fits ? metrics.height() : wrapped(label, font, textWidth, nullptr, {});
        const int height = textHeight + 2 * verticalPadding;
        const auto start = origin + QPoint(x, y);
        if (p) {
            p->setPen(deadline ? QPen(QColor("#b5c6cb")) : QPen(Qt::NoPen));
            p->setBrush(deadline ? QColor("#f8faf8") : QColor("#e0ede8"));
            const int radius = fits ? height / 2 : 6;
            p->drawRoundedRect(QRect(start, QSize(chipWidth, height)), radius, radius);
            p->setPen(deadline ? workspace::Muted : QColor("#28584f"));
            const auto textAt = start + QPoint(horizontalPadding, verticalPadding);
            if (fits) {
                p->setFont(font);
                p->drawText(textAt + QPoint(0, metrics.ascent()), label);
            } else {
                wrapped(label, font, textWidth, p, textAt);
            }
        }
        x += chipWidth + gap;
        lineHeight = qMax(lineHeight, height);
    }
    return y + lineHeight;
}

void BoardCardPainter::avatar(QPainter &p, const QRect &box, const QString &name, const QUrl &url) const
{
    const auto *photo = m_avatar(url);
    workspace::paintAvatar(p, box, name, workspace::AvatarKind::Person, photo ? *photo : QPixmap());
    if (!name.simplified().isEmpty() || (photo && !photo->isNull()))
        return;
    p.setPen(QPen(workspace::Muted, 1.1, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    const QPoint center(box.center().x(), box.top() + 7);
    p.drawEllipse(QPointF(center), 2.4, 2.4);
    QPainterPath shoulders;
    shoulders.moveTo(center + QPoint(-5, 8));
    shoulders.quadTo(center + QPoint(0, 1), center + QPoint(5, 8));
    p.drawPath(shoulders);
}

int BoardCardPainter::content(const QModelIndex &index, int width, QPainter *p, const QPoint &origin) const
{
    width = qMax(1, width);
    const bool task = !index.data(workspace::Id).toString().isEmpty();
    if (p)
        p->setPen(task ? workspace::Ink : workspace::Muted);
    int height = wrapped(index.data(workspace::Title).toString(), workspace::rowFont(ui::bodyTextPixels, task), width, p, origin);
    if (!task)
        return height;
    const auto tags = index.data(CardTags).toStringList();
    if (!tags.isEmpty()) {
        height += 6;
        height += pills(tags, width, p, origin + QPoint(0, height));
    }
    const auto due = index.data(workspace::Meta).toString();
    if (!due.isEmpty()) {
        height += 6;
        height += pills({due}, width, p, origin + QPoint(0, height), true);
    }
    constexpr int diameter = 22;
    height += 8;
    if (p) {
        const auto start = origin + QPoint(0, height);
        avatar(*p, QRect(start, QSize(diameter, diameter)), index.data(CardCreator).toString(),
               index.data(CardCreatorAvatar).toUrl());
        const auto arrow = start + QPoint(27, diameter / 2);
        p->setPen(QPen(QColor("#627d80"), 1.2, Qt::SolidLine, Qt::RoundCap));
        p->drawLine(arrow + QPoint(0, -3), arrow + QPoint(3, 0));
        p->drawLine(arrow + QPoint(3, 0), arrow + QPoint(0, 3));
        avatar(*p, QRect(start + QPoint(36, 0), QSize(diameter, diameter)), index.data(CardAssignee).toString(),
               index.data(CardAssigneeAvatar).toUrl());
    }
    return height + diameter;
}

QListWidgetItem *BoardCardPainter::addTask(QListWidget *list, const Task &task)
{
    QStringList tags;
    for (auto tag : task.tags) {
        tag = tag.trimmed();
        if (!tag.isEmpty() && tag != QLatin1String("#"))
            tags.append(tag.startsWith(QLatin1Char('#')) ? tag : QLatin1Char('#') + tag);
    }
    QStringList information{
        QStringLiteral("Постановщик: %1").arg(task.createdBy.isEmpty() ? QStringLiteral("не указан") : task.createdBy),
        QStringLiteral("Исполнитель: %1").arg(task.assignee.isEmpty() ? QStringLiteral("не указан") : task.assignee),
    };
    if (!tags.isEmpty())
        information.append(QStringLiteral("Теги: %1").arg(tags.join(QStringLiteral(", "))));
    if (!task.due.isEmpty())
        information.append(QStringLiteral("Срок: %1").arg(task.due));
    auto *item = workspace::addRow(list, task.name, information.join(QLatin1Char('\n')), {}, task.id);
    item->setData(Qt::AccessibleTextRole, task.name);
    item->setData(Qt::AccessibleDescriptionRole, information.join(QStringLiteral(". ")));
    item->setData(workspace::Meta, task.due);
    item->setData(CardTags, tags);
    item->setData(CardCreator, task.createdBy);
    item->setData(CardAssignee, task.assignee);
    item->setData(CardCreatorAvatar, task.creatorAvatarUrl);
    item->setData(CardAssigneeAvatar, task.assigneeAvatarUrl);
    return item;
}

} // namespace bx
