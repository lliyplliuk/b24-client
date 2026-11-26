#include "ui/task_description/bb_code.h"

#include <QRegularExpression>

namespace bx::ui::task_description {

QString htmlText(QString text)
{
    return text.toHtmlEscaped().replace(u"\r\n"_s, u"\n"_s).replace(u'\r', u'\n')
        .replace(u'\n', u"<br>"_s);
}

bool isDiskId(const QString &id)
{
    static const QRegularExpression pattern(u"^n?[1-9][0-9]{0,18}$"_s, QRegularExpression::CaseInsensitiveOption);
    return pattern.match(id).hasMatch();
}

QString diskReference(const QString &id, const QSize &size)
{
    auto result = u"[disk file id=%1"_s.arg(id);
    if (size.isValid() && !size.isEmpty())
        result += u" width=%1 height=%2"_s.arg(size.width()).arg(size.height());
    return result + u']';
}

QString literalText(QString text)
{
    return text.replace(QChar::LineSeparator, u'\n').replace(QChar::Nbsp, u' ');
}

} // namespace bx::ui::task_description
