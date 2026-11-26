#include "ui/task_description/decoder.h"

#include "ui/task_description/bb_code.h"
#include "ui/message_markup.h"

#include <QRegularExpression>

#include <utility>

namespace bx::ui::task_description {

Decoder::Decoder(const QString &source, TokenHtml renderToken)
    : m_source(source), m_marks(marks(source)), m_token(std::move(renderToken)) {}

QString Decoder::html() const
{
    return u"<body style=\"white-space:pre-wrap;\">"_s + range(0, m_marks.size(), 0, m_source.size(), 0)
        + u"</body>"_s;
}

QString Decoder::text(qsizetype start, qsizetype end) const
{
    return m_source.mid(start, end - start);
}

QString Decoder::range(qsizetype first, qsizetype last, qsizetype start, qsizetype end, int depth) const
{
    if (depth >= 16)
        return htmlText(text(start, end));
    QString result;
    for (auto i = first; i < last; ++i) {
        const auto &mark = m_marks[i];
        result += htmlText(text(start, mark.start));
        start = mark.end;
        const auto raw = text(mark.start, mark.end);
        if (!mark.close && mark.name == u"disk") {
            result += disk(raw, mark.argument);
        } else if (!mark.close && mark.name == u"br") {
            result += u"<br>";
        } else if (!mark.close && mark.pair > i && mark.pair < last) {
            const auto inner = pair(i, depth + 1);
            if (inner.isNull()) {
                result += htmlText(raw);
                continue;
            }
            result += inner;
            i = mark.pair;
            start = m_marks[i].end;
        } else {
            result += htmlText(raw);
        }
    }
    return result + htmlText(text(start, end));
}

QString Decoder::pair(qsizetype index, int depth) const
{
    const auto &open = m_marks[index];
    const auto &close = m_marks[open.pair];
    if (open.name == u"code" || open.name == u"icode"
        || (open.name == u"list" && !open.argument.isEmpty() && open.argument != u"=1"))
        return htmlText(text(open.start, close.end));
    if (open.name == u"list")
        return list(index, depth);
    const auto body = range(index + 1, open.pair, open.end, close.start, depth);
    if (open.name == u"url") {
        const auto address = open.argument.isEmpty() ? text(open.end, close.start).trimmed() : open.argument.mid(1);
        const auto target = workspace::messageLinkTarget(address);
        if (target.isEmpty())
            return {};
        return u"<a href=\"%1\">%2</a>"_s.arg(target.toHtmlEscaped(), body.isEmpty() ? htmlText(address) : body);
    }
    return u"<"_s + open.name + u">"_s + body + u"</"_s + open.name + u">"_s;
}

QString Decoder::list(qsizetype index, int depth) const
{
    const auto &open = m_marks[index];
    QList<qsizetype> items;
    for (auto i = index + 1; i < open.pair; ++i) {
        if (m_marks[i].name == u"*")
            items.append(i);
        else if (m_marks[i].pair > i)
            i = m_marks[i].pair;
    }
    if (items.isEmpty())
        return {};
    const auto lead = text(open.end, m_marks[items.constFirst()].start);
    if (!lead.trimmed().isEmpty())
        return {};
    const auto tag = open.argument == u"=1" ? u"ol"_s : u"ul"_s;
    QString result = u"<"_s + tag + u">"_s;
    for (qsizetype i = 0; i < items.size(); ++i) {
        const auto itemIndex = items[i];
        const auto stop = i + 1 < items.size() ? items[i + 1] : open.pair;
        result += u"<li>"_s + range(itemIndex + 1, stop, m_marks[itemIndex].end, m_marks[stop].start, depth) + u"</li>"_s;
    }
    return result + u"</"_s + tag + u">"_s;
}

QString Decoder::disk(const QString &raw, const QString &argument) const
{
    static const QRegularExpression fileId(
        uR"(^\s+file\s+id=(n?[1-9][0-9]{0,18})(?:\s|$))"_s, QRegularExpression::CaseInsensitiveOption);
    const auto id = argument.startsWith(u'=') ? argument.mid(1) : fileId.match(argument).captured(1);
    return isDiskId(id) ? m_token(raw, QObject::tr("Файл %1").arg(id)) : htmlText(raw);
}

} // namespace bx::ui::task_description
