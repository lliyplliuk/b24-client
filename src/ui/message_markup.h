#pragma once
#include <QString>
#include <QStringList>
#include <QUrl>

#include <optional>

namespace bx::workspace {

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

MessageMarkup parseMessageMarkup(const QString &source);
std::optional<AttributedMessageQuote> leadingAttributedMessageQuote(const QString &source);
QString markupPlainText(const QString &source);
QUrl messageWebLink(const QString &address);
// Safe link target: absolute http(s) URL or portal-relative path ("/tasks/?task_id=1").
// Unsafe schemes and foreign hosts stay out of the rendered markup.
QString messageLinkTarget(const QString &address);

} // namespace bx::workspace
