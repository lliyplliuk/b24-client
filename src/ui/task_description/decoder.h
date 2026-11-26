#pragma once

#include "ui/task_description/mark.h"

#include <functional>

namespace bx::ui::task_description {

class Decoder {
public:
    using TokenHtml = std::function<QString(const QString &raw, const QString &label)>;

    Decoder(const QString &source, TokenHtml renderToken);

    QString html() const;

private:
    const QString &m_source;
    const QList<Mark> m_marks;
    const TokenHtml m_token;

    QString text(qsizetype start, qsizetype end) const;

    QString range(qsizetype first, qsizetype last, qsizetype start, qsizetype end, int depth) const;

    QString pair(qsizetype index, int depth) const;

    QString list(qsizetype index, int depth) const;

    QString disk(const QString &raw, const QString &argument) const;
};

} // namespace bx::ui::task_description
