#include "cloud/cloud_backend.h"

#include "core/notification_targets.h"
#include "core/origin_guard.h"

#include <QDir>
#include <QJsonArray>
#include <QRegularExpression>
#include <QUrlQuery>
#include <algorithm>
#include <utility>

namespace bx {
namespace {

constexpr qsizetype maxMarkup = 24 * 1024;
constexpr qsizetype maxTargets = 8;
constexpr int maxLinkCandidates = 64;
const auto authorPlaceholder = QLatin1String("#AUTHOR#");

QString integerId(const QString &value)
{
    if (value.isEmpty() || value.size() > 19
        || std::any_of(value.cbegin(), value.cend(), [](QChar c) { return c < QLatin1Char('0') || c > QLatin1Char('9'); }))
        return {};
    const auto number = value.toLongLong();
    return number > 0 ? QString::number(number) : QString();
}

QString valueId(const QJsonValue &value)
{
    return integerId(value.isDouble() ? QString::number(value.toInteger()) : value.toString());
}

QString dialogIdentifier(const QString &value)
{
    const bool group = value.startsWith(QLatin1String("chat"));
    const auto id = integerId(group ? value.mid(4) : value);
    return id.isEmpty() || !group ? id : QStringLiteral("chat") + id;
}

QString jsonString(const QJsonObject &object, const QString &key, const QString &alias = {})
{
    const auto value = object.value(key).toString();
    return value.isEmpty() && !alias.isEmpty() ? object.value(alias).toString() : value;
}

QString decodeEntities(const QString &text)
{
    static const QRegularExpression entity(
        QStringLiteral("&(amp|lt|gt|quot|apos|nbsp|#[0-9]{1,8}|#[Xx][0-9A-Fa-f]{1,8});"));
    QString result;
    qsizetype offset = 0;
    auto matches = entity.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        const auto name = match.capturedView(1);
        result += text.sliced(offset, match.capturedStart() - offset);
        if (name == QLatin1String("amp"))
            result += QLatin1Char('&');
        else if (name == QLatin1String("lt"))
            result += QLatin1Char('<');
        else if (name == QLatin1String("gt"))
            result += QLatin1Char('>');
        else if (name == QLatin1String("quot"))
            result += QLatin1Char('"');
        else if (name == QLatin1String("apos"))
            result += QLatin1Char('\'');
        else if (name == QLatin1String("nbsp"))
            result += QLatin1Char(' ');
        else {
            const bool hex = name.size() > 1 && name.at(1).toLower() == QLatin1Char('x');
            const char32_t cp = name.sliced(hex ? 2 : 1).toUInt(nullptr, hex ? 16 : 10);
            if (cp > 0 && cp <= 0x10ffff && (cp < 0xd800 || cp > 0xdfff))
                result += QString::fromUcs4(&cp, 1);
        }
        offset = match.capturedEnd();
    }
    return result + text.sliced(offset);
}

QString visibleMarkup(QString text)
{
    static const QRegularExpression hidden(QString::fromLatin1(
        R"hidden(<!--.*?(?:-->|$)|<(script|style)(?=\s|>)[^>]*>.*?(?:</\1\s*>|$))hidden"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    return text.remove(hidden);
}

struct HtmlTag {
    QStringView name;
    qsizetype start;
    qsizetype attributesStart;
    qsizetype end;
    bool closing;
    bool complete;
};

std::optional<HtmlTag> nextHtmlTag(const QString &markup, qsizetype &cursor)
{
    static const QRegularExpression begin(QString::fromLatin1(
        R"tag(<(/?)([A-Za-z][A-Za-z0-9:_-]*)(?=[\s/>]))tag"));
    const auto match = begin.match(markup, cursor);
    if (!match.hasMatch())
        return std::nullopt;

    QChar quote;
    for (cursor = match.capturedEnd(); cursor < markup.size(); ++cursor) {
        const auto ch = markup.at(cursor);
        if (!quote.isNull()) {
            if (ch == quote)
                quote = {};
        } else if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) {
            quote = ch;
        } else if (ch == QLatin1Char('>')) {
            return HtmlTag{match.capturedView(2), match.capturedStart(), match.capturedEnd(),
                           ++cursor, !match.capturedView(1).isEmpty(), true};
        }
    }
    // An unterminated tag consumes the rest of the source. Its quoted attributes are not text.
    return HtmlTag{match.capturedView(2), match.capturedStart(), match.capturedEnd(),
                   cursor, !match.capturedView(1).isEmpty(), false};
}

QString plainText(QString text)
{
    static const QRegularExpression bbTags(QStringLiteral("\\[/?[a-zA-Z]+(?:=[^\\]]*)?\\]"));
    static const QRegularExpression invisible(QStringLiteral("[<>\\p{Cc}\\p{Cf}]"));
    if (text.contains(QLatin1Char('<'))) {
        QString stripped;
        qsizetype cursor = 0;
        qsizetype previousEnd = 0;
        while (const auto tag = nextHtmlTag(text, cursor)) {
            stripped += QStringView(text).sliced(previousEnd, tag->start - previousEnd);
            previousEnd = tag->end;
        }
        stripped += QStringView(text).sliced(previousEnd);
        text = std::move(stripped);
    }
    text.remove(bbTags);
    return decodeEntities(text).remove(invisible).simplified();
}

struct RawLink {
    qsizetype offset;
    qsizetype end;
    QString url;
    QString label;
};

QString hrefAttribute(const QString &attributes)
{
    static const QRegularExpression pattern(
        QString::fromLatin1(R"attr((?:^|\s+)([A-Za-z_:][A-Za-z0-9_:.-]*)(?:\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'=<>]+)))?)attr"));
    auto matches = pattern.globalMatch(attributes);
    QString result;
    bool found = false;
    while (matches.hasNext()) {
        const auto match = matches.next();
        if (match.capturedView(1).compare(QLatin1String("href"), Qt::CaseInsensitive) != 0)
            continue;
        if (found)
            return {};
        found = true;
        for (int field : {2, 3, 4}) {
            if (match.capturedStart(field) >= 0) {
                result = match.captured(field);
                break;
            }
        }
    }
    return result;
}

QVector<RawLink> anchors(const QString &markup)
{
    static const QRegularExpression bbBegin(QStringLiteral("\\[url(?:=([^\\]\\r\\n]+))?\\]"),
                                            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression bbEnd(QStringLiteral("\\[/url\\]"), QRegularExpression::CaseInsensitiveOption);
    QVector<RawLink> links;
    QString bbMarkup = markup;
    const bool hasBbCode = markup.contains(QLatin1Char('['));
    qsizetype cursor = 0;
    qsizetype anchorStart = -1;
    qsizetype anchorBodyStart = 0;
    QString href;
    while (const auto tag = nextHtmlTag(markup, cursor)) {
        if (hasBbCode) {
            const auto length = tag->end - tag->start;
            bbMarkup.replace(tag->start, length, QString(length, QLatin1Char(' ')));
        }
        if (!tag->complete || tag->name.compare(QLatin1String("a"), Qt::CaseInsensitive) != 0)
            continue;
        if (tag->closing) {
            if (anchorStart >= 0 && !href.isEmpty() && href.size() <= 2048 && links.size() < maxLinkCandidates)
                links.append({anchorStart, tag->end, href, markup.mid(anchorBodyStart, tag->start - anchorBodyStart)});
            anchorStart = -1;
        } else {
            anchorStart = tag->start;
            anchorBodyStart = tag->end;
            href = hrefAttribute(markup.mid(tag->attributesStart, tag->end - tag->attributesStart - 1));
        }
    }
    if (hasBbCode) {
        qsizetype position = 0;
        for (int i = 0; i < maxLinkCandidates; ++i) {
            const auto begin = bbBegin.match(bbMarkup, position);
            if (!begin.hasMatch())
                break;
            const auto end = bbEnd.match(bbMarkup, begin.capturedEnd());
            if (!end.hasMatch())
                break;
            position = end.capturedEnd();
            const auto attributes = begin.captured(1);
            const auto label = bbMarkup.mid(begin.capturedEnd(), end.capturedStart() - begin.capturedEnd());
            const auto address = attributes.isEmpty() ? label : attributes;
            if (!address.isEmpty() && address.size() <= 2048)
                links.append({begin.capturedStart(), position, address, attributes.isEmpty() ? QString() : label});
        }
    }
    std::stable_sort(links.begin(), links.end(), [](const auto &left, const auto &right) {
        return left.offset < right.offset;
    });
    return links;
}

std::optional<NotificationTarget> onlineTarget(const QUrl &url)
{
    const QUrlQuery query(url);
    const auto dialogs = query.allQueryItemValues(QStringLiteral("IM_DIALOG"), QUrl::FullyDecoded);
    const auto tasks = query.allQueryItemValues(QStringLiteral("IM_TASK"), QUrl::FullyDecoded);
    const auto messages = query.allQueryItemValues(QStringLiteral("IM_MESSAGE"), QUrl::FullyDecoded);
    if (dialogs.size() + tasks.size() != 1 || messages.size() > 1)
        return std::nullopt;
    const bool taskChat = !tasks.isEmpty();
    const auto dialog = dialogIdentifier(taskChat ? tasks.first() : dialogs.first());
    const auto message = messages.isEmpty() ? QString() : integerId(messages.first());
    if (dialog.isEmpty() || (taskChat && !dialog.startsWith(QLatin1String("chat")))
        || (!messages.isEmpty() && message.isEmpty()))
        return std::nullopt;
    return NotificationTarget{NotificationTargetKind::Chat, dialog, message, {},
        message.isEmpty() ? (taskChat ? QStringLiteral("Чат задачи") : QStringLiteral("Чат"))
                          : QStringLiteral("Сообщение"), taskChat};
}

QUrl linkFromMarkup(QString address)
{
    address = decodeEntities(address.trimmed());
    if (address.size() >= 2 && ((address.front() == QLatin1Char('"') && address.back() == QLatin1Char('"'))
        || (address.front() == QLatin1Char('\'') && address.back() == QLatin1Char('\'')))) {
        address = address.mid(1, address.size() - 2).trimmed();
    }
    return QUrl(address, QUrl::StrictMode);
}

bool sameObject(const NotificationTarget &left, const NotificationTarget &right)
{
    return left.kind == right.kind && left.id == right.id && left.messageId == right.messageId
        && left.projectId == right.projectId && left.taskChat == right.taskChat;
}

QString literalNoticeText(QString text)
{
    // Decoded entities can introduce brackets after stripping the source markup.
    return text.replace(QLatin1Char('['), QChar(0xFF3B)).replace(QLatin1Char(']'), QChar(0xFF3D));
}

QString noticeLinkMarkup(const QUrl &portal, const QString &source, QString label)
{
    if (source.size() > 2048)
        return literalNoticeText(std::move(label));
    auto url = linkFromMarkup(source);
    if (url.isRelative()) {
        const auto path = url.path(QUrl::FullyDecoded);
        if (!url.authority().isEmpty() || !path.startsWith(QLatin1Char('/'))
            || path.startsWith(QLatin1String("//")) || path.contains(QLatin1String("..")))
            return literalNoticeText(std::move(label));
        url = portal.resolved(url);
    }
    const auto encoded = url.toString(QUrl::FullyEncoded);
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty()
        || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))
        || encoded.size() > 2048 || encoded.contains(QLatin1Char(']')))
        return literalNoticeText(std::move(label));
    if (label.isEmpty())
        label = url.toDisplayString();
    return QStringLiteral("[URL=%1]%2[/URL]").arg(encoded, literalNoticeText(std::move(label)));
}

QString noticeLineMarkup(const QUrl &portal, const QString &source, const QString &authorLink)
{
    if (source.isEmpty())
        return {};
    const auto visible = visibleMarkup(source.left(maxMarkup));
    QStringList parts;
    const auto append = [&parts](QString part) {
        if (!part.isEmpty())
            parts.append(std::move(part));
    };
    const auto appendText = [&append, &authorLink](const QString &text) {
        auto plain = literalNoticeText(plainText(text));
        if (plain.contains(authorPlaceholder))
            plain = plain.replace(authorPlaceholder, authorLink).trimmed();
        append(std::move(plain));
    };
    qsizetype cursor = 0;
    for (const auto &link : anchors(visible)) {
        if (link.offset < cursor)
            continue;
        appendText(visible.mid(cursor, link.offset - cursor));
        auto label = plainText(link.label);
        if (label.contains(authorPlaceholder))
            appendText(label);
        else if (link.url.contains(authorPlaceholder))
            append(literalNoticeText(std::move(label)));
        else
            append(noticeLinkMarkup(portal, link.url, std::move(label)));
        cursor = link.end;
    }
    appendText(visible.mid(cursor));
    return parts.join(QLatin1Char(' '));
}

} // namespace

std::optional<NotificationTarget> portalLinkTarget(const QUrl &portal, const QUrl &supplied)
{
    if (!supplied.isValid() || (supplied.isRelative() && supplied.path().isEmpty() && supplied.authority().isEmpty()))
        return std::nullopt;

    const OriginGuard guard(portal);
    const auto url = guard.resolve(QStringLiteral("/")).resolved(supplied);
    if (!guard.isSameOrigin(url) || !url.userInfo().isEmpty()
        || (url.scheme() != QLatin1String("https") && url.scheme() != QLatin1String("http")))
        return std::nullopt;

    auto path = url.path(QUrl::FullyDecoded);
    const auto cleanPath = QDir::cleanPath(path);
    if (cleanPath != path && path != cleanPath + QLatin1Char('/'))
        return std::nullopt;
    if (path.startsWith(QLatin1String("/extranet/")))
        path.remove(0, 9);
    if (path == QLatin1String("/online/") || path == QLatin1String("/online") || path == QLatin1String("/"))
        return onlineTarget(url);

    const auto task = portalTaskUrl(url);
    if (!task.taskId.isEmpty())
        return NotificationTarget{NotificationTargetKind::Task, task.taskId, {}, task.projectId, QStringLiteral("Задача")};

    if (const auto id = portalPersonPath(path); !id.isEmpty())
        return NotificationTarget{NotificationTargetKind::Person, id, {}, {}, QStringLiteral("Профиль")};

    static const QRegularExpression project(QStringLiteral("^/workgroups/group/([0-9]+)(?:/tasks(?:/kanban)?)?/?$"));
    if (const auto id = integerId(project.match(path).captured(1)); !id.isEmpty())
        return NotificationTarget{NotificationTargetKind::Project, id, {}, id, QStringLiteral("Проект")};
    return std::nullopt;
}

QVector<NotificationTarget> notificationTargets(const QUrl &portal, const QString &notificationMarkup)
{
    QVector<NotificationTarget> targets;
    if (!portal.isValid() || (!notificationMarkup.contains(QLatin1Char('<')) && !notificationMarkup.contains(QLatin1Char('['))))
        return targets;

    for (const auto &link : anchors(visibleMarkup(notificationMarkup.left(maxMarkup)))) {
        auto target = portalLinkTarget(portal, linkFromMarkup(link.url));
        if (!target || std::any_of(targets.cbegin(), targets.cend(), [&](const auto &previous) {
            return sameObject(previous, *target);
        }))
            continue;
        if (const auto label = plainText(link.label).left(96); !label.isEmpty())
            target->label = label;
        targets.append(std::move(*target));
        if (targets.size() == maxTargets)
            break;
    }
    return targets;
}

std::optional<NotificationTarget> portalLinkPreviewTarget(const QUrl &portal, const QUrl &url)
{
    auto target = portalLinkTarget(portal, url);
    if (target && target->kind == NotificationTargetKind::Chat
        && (!target->messageId.isEmpty() || !target->id.startsWith(QLatin1String("chat"))))
        return std::nullopt;
    return target;
}

QString notificationDisplayText(const QString &notificationMarkup)
{
    return plainText(visibleMarkup(notificationMarkup.left(maxMarkup))).left(2000);
}

void decodeNotificationContent(Notification &notification, const QUrl &portal, const QString &text,
                               const QJsonObject &params, const QString &authorId, const QString &authorName)
{
    const auto component = params.value(QStringLiteral("COMPONENT_PARAMS")).toObject();
    const auto userId = integerId(authorId);
    const auto name = userId.isEmpty() ? QString()
        : notificationDisplayText(authorName.left(256)).remove(authorPlaceholder).simplified();
    const auto authorLink = name.isEmpty() ? QString()
        : noticeLinkMarkup(portal, QStringLiteral("/company/personal/user/%1/").arg(userId), name);
    const auto markupOf = [&](const QString &source) { return noticeLineMarkup(portal, source, authorLink); };
    const auto replaceAuthor = [&name](QString value) {
        return value.contains(authorPlaceholder) ? value.replace(authorPlaceholder, name).simplified() : value;
    };
    QStringList lines;
    qsizetype remaining = maxMarkup;
    const auto append = [&lines, &remaining](QString line) {
        if (line.isEmpty() || line.size() >= remaining)
            return;
        remaining -= line.size() + 1;
        lines.append(std::move(line));
    };
    const auto subject = markupOf(component.value(QStringLiteral("SUBJECT")).toString());
    append(subject.isEmpty() ? markupOf(text) : subject);

    int attachmentBudget = 32;
    for (const auto &value : params.value(QStringLiteral("ATTACH")).toArray()) {
        if (attachmentBudget <= 0)
            break;
        --attachmentBudget;
        const auto attachment = value.toObject();
        append(markupOf(attachment.value(QStringLiteral("DESCRIPTION")).toString()));
        for (const auto &rawBlock : attachment.value(QStringLiteral("BLOCKS")).toArray()) {
            if (attachmentBudget <= 0)
                break;
            --attachmentBudget;
            const auto block = rawBlock.toObject();
            append(markupOf(block.value(QStringLiteral("MESSAGE")).toString()));
            for (const auto &rawCell : block.value(QStringLiteral("GRID")).toArray()) {
                if (attachmentBudget <= 0)
                    break;
                --attachmentBudget;
                const auto cell = rawCell.toObject();
                const auto column = literalNoticeText(replaceAuthor(notificationDisplayText(cell.value(QStringLiteral("NAME")).toString())));
                const auto label = replaceAuthor(notificationDisplayText(cell.value(QStringLiteral("VALUE")).toString()));
                const auto link = cell.value(QStringLiteral("LINK")).toString();
                const auto entry = link.isEmpty() ? literalNoticeText(label) : noticeLinkMarkup(portal, link, label);
                append(column.isEmpty() ? entry : entry.isEmpty() ? column : column + QStringLiteral(" · ") + entry);
            }
        }
    }
    if (lines.isEmpty())
        append(markupOf(component.value(QStringLiteral("PLAIN_TEXT")).toString()));

    notification.title = replaceAuthor(notification.title);
    notification.text = replaceAuthor(notificationDisplayText(text));
    if (notification.text.isEmpty())
        notification.text = notificationDisplayText(lines.join(QLatin1Char(' ')));
    const auto markup = lines.isEmpty() ? literalNoticeText(notification.text) : lines.join(QLatin1Char('\n'));
    if (markup != notification.text)
        notification.markup = markup;
    notification.targets = notificationTargets(portal, markup);
    const auto fallbackMarkup = text.contains(authorPlaceholder) ? markupOf(text) : text;
    for (const auto &target : notificationTargets(portal, fallbackMarkup)) {
        if (notification.targets.size() == maxTargets)
            break;
        if (std::none_of(notification.targets.cbegin(), notification.targets.cend(), [&target](const auto &existing) {
            return sameObject(existing, target);
        }))
            notification.targets.append(target);
    }
    if (notification.type != QLatin1String("tasks") ||
        (!notification.title.isEmpty() && notification.title != QStringLiteral("Уведомление Битрикс24")))
        return;
    static const QRegularExpression createdTask(QStringLiteral("(?:\\A|\\s)создал(?:а|\\(а\\))?\\s+задачу\\s*:?\\s*\\z"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
    for (const auto &link : anchors(markup)) {
        const auto target = portalLinkTarget(portal, linkFromMarkup(link.url));
        if (!target || target->kind != NotificationTargetKind::Task)
            continue;
        if (createdTask.match(notificationDisplayText(markup.left(link.offset))).hasMatch())
            notification.title = QStringLiteral("Новая задача");
        break;
    }
}

std::optional<NotificationTarget> incomingMessageTarget(const QJsonObject &params, bool groupEvent,
                                                        bool allowMissingMessageId)
{
    const auto message = params.value(QStringLiteral("message")).toObject();
    const auto messageId = valueId(message.value(QStringLiteral("id")));
    const auto rawDialog = params.value(QStringLiteral("dialogId"));
    auto dialog = rawDialog.isDouble() ? valueId(rawDialog) : dialogIdentifier(rawDialog.toString());
    auto chatId = valueId(params.value(QStringLiteral("chatId")));
    auto messageChat = valueId(message.value(QStringLiteral("chatId")));
    if (messageChat.isEmpty())
        messageChat = valueId(message.value(QStringLiteral("chat_id")));
    if (!messageChat.isEmpty() && !chatId.isEmpty() && messageChat != chatId)
        return std::nullopt;
    if (chatId.isEmpty())
        chatId = messageChat;
    if (dialog.isEmpty() && !params.contains(QStringLiteral("dialogId")) && groupEvent && !chatId.isEmpty())
        dialog = QStringLiteral("chat") + chatId;
    if (dialog.isEmpty() || (!allowMissingMessageId && messageId.isEmpty())
        || (groupEvent && !dialog.startsWith(QLatin1String("chat")))
        || (dialog.startsWith(QLatin1String("chat")) && !chatId.isEmpty() && dialog.mid(4) != chatId))
        return std::nullopt;
    return NotificationTarget{NotificationTargetKind::Chat, dialog, messageId, {}, QStringLiteral("Открыть сообщение")};
}

void CloudBackend::requestMessageContext(const QString &dialogId, const QString &messageId)
{
    const auto operation = operationKey(OperationKind::MessageContext, dialogId, messageId);
    const auto expectedMessage = integerId(messageId);
    if (expectedMessage.isEmpty()) {
        emit failed(operation, QStringLiteral("Некорректный идентификатор сообщения"));
        return;
    }
    resolveDialog(dialogId, false, operation, operation,
                  [this, dialogId, messageId, expectedMessage, operation](const QString &canonicalDialog, qint64 chatId) {
        // The browser uses this batch for context and marks the message read in a separate command.
        // Loading a context here only needs the read-only command in that batch.
        const auto url = OriginGuard(m_session.origin).resolve(QStringLiteral("/rest/batch.json"));
        const auto method = QStringLiteral("im.v2.Chat.Message.getContext?id=%1&range=25").arg(expectedMessage);
        webRequest(url, {{QStringLiteral("halt"), QStringLiteral("0")}, {QStringLiteral("cmd[context]"), method}},
                   operation, QStringLiteral("result"),
                   [this, dialogId, messageId, canonicalDialog, expectedMessage, chatId, operation](QJsonValue batch) {
            const auto result = batch.toObject().value(QStringLiteral("result")).toObject();
            const auto context = result.value(QStringLiteral("context")).toObject();
            const auto rows = context.value(QStringLiteral("messages"));
            if (!rows.isArray())
                return failShape(operation);
            const auto expectedChat = QString::number(chatId);
            const auto rootChatId = context.value(QStringLiteral("chatId"));
            const auto contextChatId = context.value(QStringLiteral("chat")).toObject().value(QStringLiteral("id"));
            if ((!rootChatId.isUndefined() && valueId(rootChatId) != expectedChat)
                || (!contextChatId.isUndefined() && valueId(contextChatId) != expectedChat))
                return failShape(operation);
            bool found = false;
            for (const auto value : rows.toArray()) {
                const auto row = value.toObject();
                const auto camelChat = row.value(QStringLiteral("chatId"));
                const auto snakeChat = row.value(QStringLiteral("chat_id"));
                if ((camelChat.isUndefined() && snakeChat.isUndefined())
                    || (!camelChat.isUndefined() && valueId(camelChat) != expectedChat)
                    || (!snakeChat.isUndefined() && valueId(snakeChat) != expectedChat)) {
                    emit failed(operation, QStringLiteral("Полученное сообщение относится к другому чату"));
                    return;
                }
                if (valueId(row.value(QStringLiteral("id"))) == expectedMessage)
                    found = true;
            }
            if (!found) {
                emit failed(operation, QStringLiteral("Сообщение отсутствует в истории чата"));
                return;
            }
            const auto messages = decodeMessages(context, true, canonicalDialog);
            if (std::none_of(messages.cbegin(), messages.cend(), [&](const auto &message) { return message.id == expectedMessage; }))
                return failShape(operation);
            emit messageContextLoaded(dialogId, messageId, messages);
        });
    });
}

void CloudBackend::requestPerson(const QString &id)
{
    const auto operation = operationKey(OperationKind::Person, id);
    if (integerId(id).isEmpty()) {
        emit failed(operation, QStringLiteral("Некорректный идентификатор пользователя"));
        return;
    }
    loadPerson(id, operation, [this, operation](const Person &person) {
        if (person.id.isEmpty())
            return failShape(operation);
        emit personLoaded(person);
    });
}

void CloudBackend::loadPerson(const QString &id, const QString &operation, PersonHandler onPerson, bool requireName)
{
    const auto expected = integerId(id);
    const auto url = OriginGuard(m_session.origin).resolve(QStringLiteral("/rest/im.user.get.json"));
    webRequest(url, {{QStringLiteral("ID"), expected}}, operation, QStringLiteral("result"),
               [this, expected, requireName, onPerson = std::move(onPerson)](QJsonValue response) {
        const auto profile = response.toObject();
        if (valueId(profile.value(QStringLiteral("id"))) != expected)
            return onPerson({});
        auto name = profile.value(QStringLiteral("name")).toString();
        if (name.isEmpty()) {
            name = (jsonString(profile, QStringLiteral("first_name"), QStringLiteral("firstName")) + QLatin1Char(' ')
                    + jsonString(profile, QStringLiteral("last_name"), QStringLiteral("lastName"))).trimmed();
        }
        name = plainText(name).left(96);
        if (requireName && name.isEmpty())
            return onPerson({});
        if (name.isEmpty())
            name = QStringLiteral("Пользователь №%1").arg(expected);
        const auto position = plainText(jsonString(profile, QStringLiteral("work_position"), QStringLiteral("workPosition"))).left(160);
        const auto avatar = avatarUrlFromPortal(jsonString(profile, QStringLiteral("avatar"), QStringLiteral("avatarUrl")));
        onPerson({expected, name, position, avatar});
    });
}

} // namespace bx
