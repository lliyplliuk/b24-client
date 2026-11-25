#include "ui/message_markup.h"
#include "core/notification_targets.h"

#include <QDateTime>
#include <QHash>
#include <QLocale>
#include <QRegularExpression>

#include <algorithm>

namespace bx::workspace {
namespace {

using namespace Qt::StringLiterals;

constexpr qsizetype maxMarkupCharacters = 65536;
const auto replyDelimiter = u"------------------------------------------------------"_s;

enum class Tag { Unknown, Inline, Block, Label, Leaf, Code, List, Item, Table, Row, Cell, Quote, Arrow, Break };

const QHash<QString, Tag> Tags = {
    {u"b"_s, Tag::Inline}, {u"i"_s, Tag::Inline}, {u"u"_s, Tag::Inline}, {u"s"_s, Tag::Inline},
    {u"size"_s, Tag::Inline}, {u"color"_s, Tag::Inline}, {u"br"_s, Tag::Break}, {u"hr"_s, Tag::Leaf},
    {u"h1"_s, Tag::Block}, {u"h2"_s, Tag::Block}, {u"quote"_s, Tag::Block},
    {u"url"_s, Tag::Label}, {u"user"_s, Tag::Label},
    {u"chat"_s, Tag::Label}, {u"context"_s, Tag::Label}, {u"source"_s, Tag::Label},
    {u"send"_s, Tag::Label}, {u"put"_s, Tag::Label}, {u"call"_s, Tag::Label}, {u"pch"_s, Tag::Label},
    {u"icon"_s, Tag::Leaf}, {u"img"_s, Tag::Label}, {u"disk"_s, Tag::Leaf}, {u"attach"_s, Tag::Leaf},
    {u"timestamp"_s, Tag::Leaf},
    {u"like"_s, Tag::Leaf}, {u"dislike"_s, Tag::Leaf}, {u"rating"_s, Tag::Leaf}, {u"icode"_s, Tag::Code},
    {u"code"_s, Tag::Code}, {u"list"_s, Tag::List}, {u"*"_s, Tag::Item},
    {u"table"_s, Tag::Table}, {u"tr"_s, Tag::Row}, {u"th"_s, Tag::Cell}, {u"td"_s, Tag::Cell}
};

struct Token {
    qsizetype begin;
    qsizetype end;
    Tag tag;
    QString name;
    QString argument;
    bool closing = false;
    qsizetype pair = -1;
};

bool standalone(Tag tag)
{
    return tag == Tag::Break || tag == Tag::Leaf || tag == Tag::Item || tag == Tag::Arrow;
}

bool validArgument(const Token &token)
{
    static const QHash<QString, QRegularExpression> rules = [] {
        QHash<QString, QRegularExpression> result;
        const QHash<QString, QString> patterns = {
            {u"size"_s, uR"(\d+(?:pt|px)?)"_s},
            {u"color"_s, uR"(#[0-9a-f]{3}(?:[0-9a-f]{3})?)"_s},
            {u"user"_s, uR"((?:all|\d+)(?: REPLACE)?)"_s},
            {u"chat"_s, uR"((?:imol\|)?\d+)"_s},
            {u"context"_s, uR"((?:chat\d+|\d+:\d+)/\d+)"_s},
            {u"call"_s, uR"([\d #()+./-]*)"_s},
            {u"source"_s, uR"(\d+)"_s}, {u"pch"_s, uR"(\d+)"_s}, {u"disk"_s, uR"(\d+)"_s},
            {u"attach"_s, uR"(\d+)"_s},
            {u"timestamp"_s, uR"(\d+\s+format=[_a-z]+)"_s},
            {u"list"_s, uR"(1?(?:\s+start=\d+)?)"_s},
            {u"img"_s, uR"((?:\s+[a-z]+=\S+)*)"_s},
            {u"icon"_s, u".+"_s}, {u"rating"_s, u"[1-5]"_s},
            {u"url"_s, u".*"_s}, {u"send"_s, u".*"_s}, {u"put"_s, u".*"_s}
        };
        for (auto it = patterns.cbegin(); it != patterns.cend(); ++it)
            result.insert(it.key(), QRegularExpression(QRegularExpression::anchoredPattern(it.value()),
                QRegularExpression::CaseInsensitiveOption));
        return result;
    }();
    const auto rule = rules.constFind(token.name);
    return rule == rules.cend() ? token.argument.isEmpty() : rule->match(token.argument).hasMatch();
}

QList<Token> tokenize(const QString &source)
{
    static const QRegularExpression pattern(
        uR"(\[(/?)([a-z][a-z0-9]*|\*)(?:=([^\[\]\r\n]{0,2048})|( [^\[\]\r\n]{0,2048}))?\]|\r\n|\r|\n|-{54}|>>)"_s,
        QRegularExpression::CaseInsensitiveOption);
    QList<Token> tokens;
    auto matches = pattern.globalMatch(source);
    while (matches.hasNext() && tokens.size() < 4096) {
        const auto match = matches.next();
        const auto begin = match.capturedStart();
        const auto end = match.capturedEnd();
        const auto literal = match.capturedView();
        const auto name = match.captured(2).toLower();
        if (literal == u">>") {
            if (begin == 0 || (!tokens.isEmpty() && tokens.constLast().end == begin && tokens.constLast().tag == Tag::Break))
                tokens.append({begin, end, Tag::Arrow, {}, {}});
        } else if (literal[0] == u'-') {
            if ((begin == 0 || source[begin - 1] != u'-') && (end == source.size() || source[end] != u'-'))
                tokens.append({begin, end, Tag::Quote, u"quote"_s, {}});
        } else if (name.isEmpty()) {
            tokens.append({begin, end, Tag::Break, {}, {}});
        } else {
            Token token{begin, end, Tags.value(name), name, match.capturedStart(3) >= 0
                ? match.captured(3) : match.captured(4), match.capturedLength(1) != 0};
            if (token.tag != Tag::Unknown && (token.closing ? !standalone(token.tag) && token.argument.isEmpty()
                : validArgument(token)))
                tokens.append(std::move(token));
        }
    }

    // Code consumes raw source through its first matching close, even if it contains other tags.
    qsizetype nextCode = -1;
    qsizetype nextInline = -1;
    for (qsizetype i = tokens.size(); i > 0; --i) {
        auto &token = tokens[i - 1];
        if (token.tag != Tag::Code)
            continue;
        auto &next = token.name == u"code" ? nextCode : nextInline;
        if (token.closing)
            next = i - 1;
        else
            token.pair = next;
    }

    QList<qsizetype> stack;
    for (qsizetype i = 0; i < tokens.size(); ++i) {
        auto &token = tokens[i];
        if (token.tag == Tag::Code) {
            if (token.pair >= 0)
                i = token.pair;
            continue;
        }
        if (standalone(token.tag))
            continue;
        if (token.tag == Tag::Quote) {
            if (!stack.isEmpty() && tokens[stack.constLast()].tag == Tag::Quote)
                token.closing = true;
            else if (i + 1 == tokens.size() || tokens[i + 1].tag != Tag::Break || tokens[i + 1].begin != token.end)
                continue;
        }
        if (!token.closing)
            stack.append(i);
        else if (!stack.isEmpty() && tokens[stack.constLast()].tag == token.tag
                 && tokens[stack.constLast()].name == token.name)
            tokens[stack.takeLast()].pair = i;
    }
    return tokens;
}

QString escaped(QString text)
{
    return text.toHtmlEscaped().replace(u"\r\n"_s, u"\n"_s).replace(u'\r', u'\n')
        .replace(u'\t', u"\u00a0 \u00a0 "_s).replace(u'\n', u"<br>"_s);
}

QString timeLabel(const QString &argument)
{
    static const QHash<QString, QString> formats = {
        {u"SHORT_TIME_FORMAT"_s, u"HH:mm"_s}, {u"LONG_TIME_FORMAT"_s, u"HH:mm:ss"_s},
        {u"SHORT_DATE_FORMAT"_s, u"dd.MM.yyyy"_s}, {u"DAY_MONTH_FORMAT"_s, u"d MMMM"_s},
        {u"LONG_DATE_FORMAT"_s, u"d MMMM yyyy"_s}, {u"DAY_OF_WEEK_MONTH_FORMAT"_s, u"dddd, d MMMM"_s},
        {u"SHORT_DAY_OF_WEEK_MONTH_FORMAT"_s, u"ddd, d MMMM"_s},
        {u"SHORT_DAY_OF_WEEK_SHORT_MONTH_FORMAT"_s, u"ddd, d MMM"_s},
        {u"FULL_DATE_FORMAT"_s, u"dddd, d MMMM yyyy"_s}, {u"DAY_SHORT_MONTH_FORMAT"_s, u"d MMM"_s},
        {u"MEDIUM_DATE_FORMAT"_s, u"d MMM yyyy"_s}, {u"FORMAT_DATETIME"_s, u"dd.MM.yyyy HH:mm:ss"_s},
        {u"FORMAT_DATE"_s, u"dd.MM.yyyy"_s}
    };
    const auto format = formats.value(argument.section(u'=', 1).toUpper());
    bool ok = false;
    static const QRegularExpression secondsPattern(uR"(^\d+)"_s);
    const auto seconds = secondsPattern.match(argument).captured().toLongLong(&ok);
    return ok && seconds <= 253402300799LL && !format.isEmpty()
        ? QLocale().toString(QDateTime::fromSecsSinceEpoch(seconds), format) : QString{};
}

struct Part {
    QString plain;
    QString html;
    bool changed = false;
    bool block = false;
    QStringList links{};
};

void appendLinks(QStringList &links, const QStringList &added)
{
    constexpr qsizetype previewsPerMessage = 3;
    for (const auto &link : added) {
        if (links.size() >= previewsPerMessage)
            break;
        if (link.size() <= 2048 && !links.contains(link))
            links.append(link);
    }
}

const QRegularExpression &webLinks()
{
    static const QRegularExpression urls(uR"((?<![\w@])https?://[^\s<>"']+)"_s,
                                         QRegularExpression::CaseInsensitiveOption);
    return urls;
}

QString trimLinkPunctuation(QString candidate)
{
    auto unmatchedParens = candidate.count(u')') - candidate.count(u'(');
    auto unmatchedBrackets = candidate.count(u']') - candidate.count(u'[');
    auto unmatchedBraces = candidate.count(u'}') - candidate.count(u'{');
    while (!candidate.isEmpty()) {
        const auto end = candidate.back();
        if (!QStringView(u".,;:!?…»").contains(end) && !(end == u')' && unmatchedParens-- > 0)
            && !(end == u']' && unmatchedBrackets-- > 0) && !(end == u'}' && unmatchedBraces-- > 0))
            break;
        candidate.chop(1);
    }
    return candidate;
}

void boundary(QString &text)
{
    if (!text.isEmpty() && !text.endsWith(u'\n'))
        text += u'\n';
}

Part wrapped(Part part, const QString &start, const QString &end, bool block = false)
{
    part.html = start + part.html + end;
    part.changed = true;
    part.block |= block;
    if (block)
        boundary(part.plain);
    return part;
}

Part label(const QString &text)
{
    return wrapped({text, escaped(text)}, u"<span style=\"color:#215c70;font-weight:600;\">"_s, u"</span>"_s);
}

class Parser {
public:
    explicit Parser(const QString &source) : m_source(source), m_tokens(tokenize(source)) {}

    Part render() const
    {
        auto result = range(0, m_tokens.size(), 0, m_source.size(), 0);
        if (result.block && result.plain.endsWith(u'\n') && !m_source.endsWith(u'\n'))
            result.plain.chop(1);
        return result;
    }

    std::optional<AttributedMessageQuote> leadingAttributedQuote() const
    {
        if (m_tokens.isEmpty() || m_tokens.first().tag != Tag::Quote || m_tokens.first().begin != 0)
            return std::nullopt;
        const auto close = m_tokens.first().pair;
        if (close <= 2 || m_tokens[close - 1].tag != Tag::Break || m_tokens[close - 1].end != m_tokens[close].begin)
            return std::nullopt;
        if (close + 1 == m_tokens.size() || m_tokens[close + 1].tag != Tag::Break
            || m_tokens[close + 1].begin != m_tokens[close].end)
            return std::nullopt;
        const auto bodyBegin = m_tokens[close + 1].end;
        const auto header = replyHeader(0);
        if (!header.match.hasMatch())
            return std::nullopt;
        static const QRegularExpression clock(uR"((?:^|[ ,])(?:[01]?[0-9]|2[0-3]):[0-5][0-9](?::[0-5][0-9])?\s*$)"_s);
        if (!clock.match(header.match.captured(3)).hasMatch())
            return std::nullopt;
        if (std::any_of(m_tokens.cbegin() + header.first, m_tokens.cbegin() + close, [](const Token &token) {
                return token.tag == Tag::Quote || token.tag == Tag::Arrow || token.name == u"quote";
            }))
            return std::nullopt;
        const auto author = markupPlainText(header.match.captured(2).left(512)).simplified();
        auto excerpt = range(header.first, close, header.begin, m_tokens[close].begin, 1).plain.simplified();
        constexpr qsizetype maxExcerpt = 240;
        if (excerpt.size() > maxExcerpt)
            excerpt.truncate(excerpt[maxExcerpt - 1].isHighSurrogate() ? maxExcerpt - 1 : maxExcerpt);
        if (author.isEmpty() || excerpt.isEmpty())
            return std::nullopt;
        if (range(close + 2, m_tokens.size(), bodyBegin, m_source.size(), 0).plain.trimmed().isEmpty())
            return std::nullopt;
        return AttributedMessageQuote{author, excerpt, m_source.mid(bodyBegin)};
    }

private:
    const QString &m_source;
    const QList<Token> m_tokens;

    QString text(qsizetype start, qsizetype end) const { return m_source.mid(start, end - start); }

    void literal(Part &result, qsizetype start, qsizetype end) const
    {
        const auto value = text(start, end);
        result.plain += value;
        qsizetype offset = 0;
        auto matches = webLinks().globalMatch(value);
        while (matches.hasNext()) {
            const auto match = matches.next();
            const auto address = trimLinkPunctuation(match.captured());
            if (address.isEmpty() || address.size() > 2048 || messageWebLink(address).isEmpty())
                continue;
            const auto linked = link({address, escaped(address)}, address);
            result.html += escaped(value.mid(offset, match.capturedStart() - offset)) + linked.html;
            offset = match.capturedStart() + address.size();
            result.changed = true;
            appendLinks(result.links, linked.links);
        }
        result.html += escaped(value.mid(offset));
    }

    Part range(qsizetype first, qsizetype last, qsizetype offset, qsizetype end, int depth) const
    {
        Part result;
        if (depth >= 16) {
            literal(result, offset, end);
            return result;
        }
        for (qsizetype i = first; i < last; ++i) {
            const auto &token = m_tokens[i];
            literal(result, offset, token.begin);
            offset = token.end;
            Part value;
            if (token.tag == Tag::Arrow) {
                auto lineEnd = i + 1;
                while (lineEnd < last && m_tokens[lineEnd].tag != Tag::Break)
                    ++lineEnd;
                const auto finish = lineEnd < last ? m_tokens[lineEnd].begin : end;
                value = arrowQuote(i + 1, lineEnd, token.end, finish, depth);
                offset = lineEnd < last ? m_tokens[lineEnd].end : end;
                i = lineEnd;
            } else if (standalone(token.tag)) {
                value = leaf(token);
                if (token.tag == Tag::Break && i > first && m_tokens[i - 1].closing
                    && m_tokens[i - 1].tag == Tag::Quote && m_tokens[i - 1].end == token.begin)
                    continue;
            } else if (!token.closing && token.pair > i && token.pair < last) {
                value = pair(i, depth);
                if (value.changed) {
                    i = token.pair;
                    offset = m_tokens[i].end;
                }
            }
            if (!value.changed) {
                literal(result, token.begin, offset);
                continue;
            }
            if (value.block)
                boundary(result.plain);
            result.plain += value.plain;
            result.html += value.html;
            result.changed = true;
            result.block |= value.block;
            appendLinks(result.links, value.links);
        }
        literal(result, offset, end);
        return result;
    }

    Part leaf(const Token &token) const
    {
        const auto &name = token.name;
        if (token.tag == Tag::Break)
            return {u"\n"_s, u"<br>"_s, !name.isEmpty()};
        if (name == u"hr")
            return {{}, u"<hr>"_s, true, true};
        if (name == u"icon")
            return label(QObject::tr("Иконка"));
        if (name == u"disk")
            return label(QObject::tr("Файл"));
        if (name == u"attach")
            return label(QObject::tr("Вложение"));
        if (name == u"like")
            return label(u"👍"_s);
        if (name == u"dislike")
            return label(u"👎"_s);
        if (name == u"rating") {
            const int score = token.argument.toInt();
            return {QObject::tr("Оценка %1 из 5").arg(score),
                label(QString(score, u'★') + QString(5 - score, u'☆')).html, true};
        }
        if (name == u"timestamp") {
            const auto time = timeLabel(token.argument);
            if (!time.isEmpty())
                return label(time);
        }
        return {};
    }

    Part pair(qsizetype index, int depth) const
    {
        const auto &token = m_tokens[index];
        const auto &close = m_tokens[token.pair];
        if (token.tag == Tag::Code) {
            const bool block = token.name == u"code";
            auto raw = text(token.end, close.begin);
            if (block && index + 1 < token.pair && m_tokens[index + 1].tag == Tag::Break
                && m_tokens[index + 1].begin == token.end)
                raw.remove(0, m_tokens[index + 1].end - token.end);
            auto html = escaped(raw).replace(u' ', u'\u00a0');
            return wrapped({raw, html}, block ? u"<pre style=\"background-color:#e8edeb;\">"_s
                : u"<code style=\"background-color:#e8edeb;\">"_s, block ? u"</pre>"_s : u"</code>"_s, block);
        }
        if (token.tag == Tag::List)
            return list(index, depth);
        if (token.tag == Tag::Table)
            return table(index, depth);
        if (token.tag == Tag::Quote)
            return replyQuote(index, depth);
        if (token.tag == Tag::Row || token.tag == Tag::Cell)
            return {};

        const auto &name = token.name;
        if (name == u"img") {
            static const QRegularExpression alt(uR"((?:^|\s)alt=(\S+))"_s, QRegularExpression::CaseInsensitiveOption);
            const auto caption = QUrl::fromPercentEncoding(alt.match(token.argument).captured(1).toUtf8()).simplified();
            return label(caption.isEmpty() ? QObject::tr("Изображение")
                : QObject::tr("Изображение · %1").arg(caption));
        }
        auto body = range(index + 1, token.pair, token.end, close.begin, depth + 1);
        if (name == u"quote")
            return quote(body);
        if (name == u"size") {
            static const QRegularExpression digits(uR"(^\d+)"_s);
            bool ok = false;
            const int size = digits.match(token.argument).captured().toInt(&ok);
            return wrapped(body, u"<span style=\"font-size:%1px;\">"_s.arg(ok ? qBound(8, size, 30) : 30), u"</span>"_s);
        }
        if (name == u"color")
            return wrapped(body, u"<span style=\"color:%1;\">"_s.arg(token.argument), u"</span>"_s);
        if (token.tag == Tag::Inline || token.tag == Tag::Block)
            return wrapped(body, u"<"_s + name + u">"_s, u"</"_s + name + u">"_s, token.tag == Tag::Block);
        if (name == u"url")
            return link(body, token.argument);
        if (name == u"user") {
            const auto id = token.argument.section(u' ', 0, 0);
            const auto person = portalTaskIdentifier(id);
            if (body.plain.isEmpty()) {
                body.plain = id.compare(u"all"_s, Qt::CaseInsensitive) == 0 ? QObject::tr("Все участники")
                    : QObject::tr("Пользователь %1").arg(id);
            }
            return person.isEmpty() ? label(body.plain)
                : link(body, u"/company/personal/user/%1/"_s.arg(person), false);
        }
        if (!body.plain.isEmpty())
            return label(body.plain);
        if (name == u"chat")
            return label(QObject::tr("Чат %1").arg(token.argument.section(u'|', -1)));
        if (name == u"context")
            return label(QObject::tr("Сообщение"));
        if (name == u"source")
            return label(QObject::tr("Источник %1").arg(token.argument));
        return label(token.argument);
    }

    static Part link(Part body, const QString &argument, bool preview = true)
    {
        const auto address = argument.isEmpty() ? body.plain.trimmed() : argument.trimmed();
        if (body.plain.isEmpty())
            body = {address, escaped(address)};
        const auto target = messageLinkTarget(address);
        if (!target.isEmpty()) {
            auto result = wrapped({body.plain, escaped(body.plain)}, u"<a style=\"color:#215c70;\" href=\"%1\">"_s.arg(
                target.toHtmlEscaped()), u"</a>"_s);
            if (preview)
                appendLinks(result.links, {target});
            return result;
        }
        return label(body.plain);
    }

    Part list(qsizetype index, int depth) const
    {
        const auto &opening = m_tokens[index];
        const auto &closing = m_tokens[opening.pair];
        const bool ordered = opening.argument.startsWith(u'1');
        bool hasStart = false;
        const auto start = opening.argument.section(u'=', -1).toInt(&hasStart);
        int number = hasStart ? qMin(start, 999999999) : 1;
        QList<qsizetype> items;
        constexpr int maxItems = 200;
        for (qsizetype i = index + 1; i < opening.pair && items.size() <= maxItems; ++i) {
            if (m_tokens[i].tag == Tag::Item)
                items.append(i);
            else if (m_tokens[i].pair > i)
                i = m_tokens[i].pair;
        }
        if (items.isEmpty())
            return {};
        Part result{{}, {}, true, true};
        for (qsizetype i = 0; i < qMin<qsizetype>(items.size(), maxItems); ++i) {
            const auto itemIndex = items[i];
            const auto next = i + 1 < items.size() ? items[i + 1] : opening.pair;
            const auto item = range(itemIndex + 1, next, m_tokens[itemIndex].end,
                next == opening.pair ? closing.begin : m_tokens[next].begin, depth + 1);
            const auto prefix = ordered ? u"%1. "_s.arg(number++) : u"• "_s;
            result.plain += prefix + item.plain.trimmed() + u'\n';
            appendLinks(result.links, item.links);
            result.html += u"<p style=\"margin:0 0 3px %1px;\">%2%3</p>"_s.arg(
                QString::number(qMin(depth + 1, 8) * 12), prefix, item.html.trimmed());
        }
        return result;
    }

    Part table(qsizetype index, int depth) const
    {
        const auto &opening = m_tokens[index];
        Part result{{}, u"<table border=\"1\" bordercolor=\"#dce4e4\" cellspacing=\"0\" cellpadding=\"5\">"_s, true, true};
        int rows = 0;
        for (qsizetype i = index + 1; i < opening.pair && rows < 200; ++i) {
            const auto &row = m_tokens[i];
            if (row.tag != Tag::Row || row.pair <= i)
                continue;
            ++rows;
            result.html += u"<tr>";
            int cells = 0;
            for (qsizetype j = i + 1; j < row.pair && cells < 24; ++j) {
                const auto &cell = m_tokens[j];
                if (cell.tag != Tag::Cell || cell.pair <= j)
                    continue;
                const auto item = range(j + 1, cell.pair, cell.end, m_tokens[cell.pair].begin, depth + 1);
                if (cells++)
                    result.plain += u'\t';
                result.plain += item.plain.trimmed();
                appendLinks(result.links, item.links);
                result.html += u"<"_s + cell.name + u">"_s + item.html.trimmed() + u"</"_s + cell.name + u">"_s;
                j = cell.pair;
            }
            result.plain += u'\n';
            result.html += u"</tr>";
            i = row.pair;
        }
        result.html += u"</table>";
        return rows > 0 ? result : Part{};
    }

    static Part quote(Part body)
    {
        return wrapped(body, u"<blockquote style=\"margin:3px 0 3px 12px;color:#516674;\">"_s,
            u"</blockquote>"_s, true);
    }

    Part arrowQuote(qsizetype first, qsizetype last, qsizetype begin, qsizetype end, int depth) const
    {
        int count = 1;
        for (; begin < end && count < 8 && QStringView(m_source).sliced(begin, end - begin).startsWith(u">>"); ++count)
            begin += 2;
        while (begin < end && m_source[begin] == u' ')
            ++begin;
        auto body = range(first, last, begin, end, depth + 1);
        for (int i = 0; i < count; ++i)
            body = quote(body);
        return body;
    }

    struct ReplyHeader {
        QRegularExpressionMatch match;
        qsizetype first;
        qsizetype begin;
    };

    ReplyHeader replyHeader(qsizetype index) const
    {
        static const QRegularExpression author(uR"(^((.*) \[([^\]\r\n]+)\])(?: #(?:chat\d+|\d+:\d+)/\d+)?$)"_s);
        const auto &opening = m_tokens[index];
        const auto first = index + 2; // The tokenizer admits a reply delimiter only before a line break.
        const auto begin = m_tokens[index + 1].end;
        for (auto i = first; i < opening.pair; ++i) {
            if (m_tokens[i].tag != Tag::Break)
                continue;
            auto match = author.match(text(begin, m_tokens[i].begin));
            if (match.hasMatch())
                return {std::move(match), i + 1, m_tokens[i].end};
            break;
        }
        return {{}, first, begin};
    }

    Part replyQuote(qsizetype index, int depth) const
    {
        const auto close = m_tokens[index].pair;
        const auto header = replyHeader(index);
        auto body = range(header.first, close, header.begin, m_tokens[close].begin, depth + 1);
        if (header.match.hasMatch()) {
            const auto caption = header.match.captured(1);
            body.plain.prepend(caption + u'\n');
            body.html.prepend(u"<b>"_s + escaped(caption) + u"</b><br>"_s);
        }
        return quote(body);
    }
};

} // namespace

MessageMarkup parseMessageMarkup(const QString &source)
{
    const auto message = source.size() <= maxMarkupCharacters ? source
        : source.left(source[maxMarkupCharacters - 1].isHighSurrogate() ? maxMarkupCharacters - 1 : maxMarkupCharacters) + u'…';
    const bool action = message.startsWith(u"/me ");
    const bool loud = message.startsWith(u"/loud ");
    if (!action && !loud && !message.contains(u'[') && !message.contains(u">>")
        && !message.contains(replyDelimiter) && !webLinks().match(message).hasMatch())
        return {message, {}, false};
    if (action || loud) {
        const auto body = message.mid(action ? 4 : 6);
        auto wrappedBody = Parser(body).render();
        wrappedBody = action ? wrapped(wrappedBody, u"<i>"_s, u"</i>"_s)
            : wrapped(wrappedBody, u"<span style=\"font-size:20px;\">"_s, u"</span>"_s);
        return {wrappedBody.plain, wrappedBody.html, wrappedBody.block, wrappedBody.links};
    }
    auto result = Parser(message).render();
    return {result.plain, result.changed ? result.html : QString{}, result.block, result.links};
}

std::optional<AttributedMessageQuote> leadingAttributedMessageQuote(const QString &source)
{
    if (!source.startsWith(replyDelimiter) || source.size() > maxMarkupCharacters)
        return std::nullopt;
    return Parser(source).leadingAttributedQuote();
}

QString markupPlainText(const QString &source)
{
    return parseMessageMarkup(source).plain;
}

QUrl messageWebLink(const QString &address)
{
    const QUrl url(address, QUrl::StrictMode);
    if (!url.isValid() || (url.scheme() != u"https" && url.scheme() != u"http")
        || url.host().isEmpty() || !url.userInfo().isEmpty())
        return {};
    return url;
}

QString messageLinkTarget(const QString &address)
{
    const auto text = address.trimmed();
    if (text.isEmpty())
        return {};
    const QUrl url(text, QUrl::StrictMode);
    if (!url.isValid())
        return {};
    if (url.scheme().isEmpty()) {
        // Portal-relative link: kept clickable, resolved against the portal on click.
        if (!text.startsWith(QLatin1Char('/')) || text.startsWith(QLatin1String("//"))
            || text.contains(QLatin1String("..")))
            return {};
        return text;
    }
    if (url.scheme() != u"https" && url.scheme() != u"http")
        return {};
    if (url.host().isEmpty() || !url.userInfo().isEmpty())
        return {};
    return url.toString(QUrl::FullyEncoded);
}

} // namespace bx::workspace
