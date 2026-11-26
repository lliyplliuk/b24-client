#pragma once
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QUrl>

#include <optional>

namespace bx::markup {

struct MessageMarkup {
    QString plain;
    QString html;
    bool block = false;
    QStringList links{};
};

struct AttributedMessageQuote {
    QString author;
    QString excerpt;
    QString body;
};

// Portal fragments can contain HTML and entities before conversion to message markup.
enum class TextFormat { Message, PortalNotification };

MessageMarkup parseMessageMarkup(const QString &source);
std::optional<AttributedMessageQuote> leadingAttributedMessageQuote(const QString &source);
QString plainText(const QString &source, TextFormat format = TextFormat::Message);

struct HtmlTag {
    QStringView name;
    qsizetype start;
    qsizetype attributesStart;
    qsizetype end;
    bool closing;
    bool complete;
};

QString visibleHtml(QString source);
std::optional<HtmlTag> nextHtmlTag(const QString &source, qsizetype &cursor);
QString decodeHtmlEntities(const QString &source);

QUrl messageWebLink(const QString &address);
// Safe link target: absolute http(s) URL or portal-relative path ("/tasks/?task_id=1").
QString messageLinkTarget(const QString &address);

} // namespace bx::markup
