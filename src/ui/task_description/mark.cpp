#include "ui/task_description/mark.h"

#include <QRegularExpression>

#include <utility>

namespace bx::ui::task_description {

using namespace Qt::StringLiterals;

QList<Mark> marks(const QString &source)
{
    static const QRegularExpression syntax(uR"(\[(/?)([a-z][a-z0-9]*|\*)([ =][^\[\]\r\n]{0,2048})?\])"_s,
        QRegularExpression::CaseInsensitiveOption);
    QList<Mark> result;
    QList<qsizetype> stack;
    auto matches = syntax.globalMatch(source);
    while (matches.hasNext() && result.size() < 16384) {
        const auto match = matches.next();
        Mark mark{match.capturedStart(), match.capturedEnd(), match.captured(2).toLower(),
            match.captured(3), !match.capturedView(1).isEmpty()};
        const auto &name = mark.name;
        const auto &arg = mark.argument;
        const bool simple = name == u"b" || name == u"i" || name == u"u" || name == u"s" || name == u"p"
            || name == u"code" || name == u"icode";
        const bool paired = (simple && arg.isEmpty()) || (name == u"url" && (arg.isEmpty() || arg.startsWith(u'=')))
            || name == u"list";
        const bool standalone = !mark.close && (((name == u"*" || name == u"br") && arg.isEmpty()) || name == u"disk");
        if ((!standalone && !paired) || (mark.close && !arg.isEmpty()))
            continue;
        const bool insideCode = !stack.isEmpty()
            && (result[stack.constLast()].name == u"code" || result[stack.constLast()].name == u"icode");
        if (insideCode && (!mark.close || result[stack.constLast()].name != name))
            continue;
        if (mark.close) {
            if (!stack.isEmpty() && result[stack.constLast()].name == name)
                result[stack.takeLast()].pair = result.size();
        } else if (paired) {
            stack.append(result.size());
        }
        result.append(std::move(mark));
    }
    return result;
}

} // namespace bx::ui::task_description
