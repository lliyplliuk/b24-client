#pragma once

#include <QList>
#include <QString>

namespace bx::ui::task_description {

struct Mark {
    qsizetype start;
    qsizetype end;
    QString name;
    QString argument;
    bool close;
    qsizetype pair = -1;
};

QList<Mark> marks(const QString &source);

} // namespace bx::ui::task_description
