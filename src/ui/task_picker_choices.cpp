#include "ui/task_picker_choices.h"

#include <QPainter>

namespace bx::ui {

QIcon taskStageColorIcon(const QString &color)
{
    const QColor supplied(QLatin1Char('#') + color);
    QPixmap dot(22, 22);
    dot.fill(Qt::transparent);
    QPainter painter(&dot);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(supplied.isValid() ? supplied : QColor("#bccbc7"));
    painter.drawEllipse(QRect(5, 5, 12, 12));
    painter.end();
    return QIcon(dot);
}

} // namespace bx::ui
