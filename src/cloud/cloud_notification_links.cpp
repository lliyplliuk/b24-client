#include "cloud/cloud_rows.h"
#include "cloud/cloud_notification_links.h"

#include "core/notification_targets.h"
#include "core/origin_guard.h"
#include "core/message_markup.h"

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

QString dialogIdentifier(const QString &value)
{
    const bool group = value.startsWith(QLatin1String("chat"));
    const auto id = portalTaskIdentifier(group ? value.mid(4) : value);
    return id.isEmpty() || !group ? id : QStringLiteral("chat") + id;
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
    while (const auto tag = markup::nextHtmlTag(markup, cursor)) {
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
    const auto message = messages.isEmpty() ? QString() : portalTaskIdentifier(messages.first());
    if (dialog.isEmpty() || (taskChat && !dialog.startsWith(QLatin1String("chat")))
        || (!messages.isEmpty() && message.isEmpty()))
        return std::nullopt;
    return NotificationTarget{NotificationTargetKind::Chat, dialog, message, {},
        message.isEmpty() ? (taskChat ? QStringLiteral("Чат задачи") : QStringLiteral("Чат"))
                          : QStringLiteral("Сообщение"), taskChat};
}

QUrl linkFromMarkup(QString address)
{
    address = markup::decodeHtmlEntities(address.trimmed());
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
    const auto visible = markup::visibleHtml(source.left(maxMarkup));
    QStringList parts;
    const auto append = [&parts](QString part) {
        if (!part.isEmpty())
            parts.append(std::move(part));
    };
    const auto appendText = [&append, &authorLink](const QString &text) {
        auto plain = literalNoticeText(markup::plainText(text, markup::TextFormat::PortalNotification));
        if (plain.contains(authorPlaceholder))
            plain = plain.replace(authorPlaceholder, authorLink).trimmed();
        append(std::move(plain));
    };
    qsizetype cursor = 0;
    for (const auto &link : anchors(visible)) {
        if (link.offset < cursor)
            continue;
        appendText(visible.mid(cursor, link.offset - cursor));
        auto label = markup::plainText(link.label, markup::TextFormat::PortalNotification);
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
    if (!guard.isSameOrigin(url) || url.port() == 0 || portal.port() == 0 || !url.userInfo().isEmpty()
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
    if (const auto id = portalTaskIdentifier(project.match(path).captured(1)); !id.isEmpty())
        return NotificationTarget{NotificationTargetKind::Project, id, {}, id, QStringLiteral("Проект")};
    return std::nullopt;
}

QVector<NotificationTarget> notificationTargets(const QUrl &portal, const QString &notificationMarkup)
{
    QVector<NotificationTarget> targets;
    if (!portal.isValid() || (!notificationMarkup.contains(QLatin1Char('<')) && !notificationMarkup.contains(QLatin1Char('['))))
        return targets;

    for (const auto &link : anchors(markup::visibleHtml(notificationMarkup.left(maxMarkup)))) {
        auto target = portalLinkTarget(portal, linkFromMarkup(link.url));
        if (!target || std::any_of(targets.cbegin(), targets.cend(), [&](const auto &previous) {
            return sameObject(previous, *target);
        }))
            continue;
        if (const auto label = markup::plainText(link.label, markup::TextFormat::PortalNotification).left(96); !label.isEmpty())
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
    const auto visible = markup::visibleHtml(notificationMarkup.left(maxMarkup));
    return markup::plainText(visible, markup::TextFormat::PortalNotification).left(2000);
}

void prepareNotificationDisplay(Notification &notice)
{
    const auto &content = notice.markup.isEmpty() ? notice.text : notice.markup;
    auto plainTitle = markup::plainText(notice.title);
    auto plainBody = markup::plainText(content);
    if (!content.trimmed().isEmpty()
        && (plainTitle == QStringLiteral("Уведомление Битрикс24") || plainTitle == notice.text.trimmed()))
        plainTitle.clear();
    notice.displayTitle = std::move(plainTitle);

    static const QRegularExpression quotedReply(QStringLiteral(R"(-{8,}[^\n]*\[.+?\]\s*#chat\d+/\d+.*?-{8,}\s*(?=\S))"),
                                               QRegularExpression::DotMatchesEverythingOption);
    const auto quote = quotedReply.match(plainBody);
    if (quote.hasMatch())
        plainBody.remove(quote.capturedStart(), quote.capturedLength());
    const auto message = std::find_if(notice.targets.cbegin(), notice.targets.cend(), [](const auto &target) {
        return target.kind == NotificationTargetKind::Chat && !target.messageId.isEmpty();
    });
    const bool taskChat = message != notice.targets.cend() && message->taskChat;
    static const QRegularExpression taskPrefix(QStringLiteral(R"(^Задача #\d+\s+)"));
    if (taskChat || (!notice.displayTitle.isEmpty() && notice.displayTitle != QStringLiteral("Новое сообщение")))
        plainBody.remove(taskPrefix);
    notice.excerpt = plainBody.simplified();
    if (taskChat)
        notice.excerpt.prepend(QStringLiteral("Сообщение в чате задачи: "));
    notice.excerpt.truncate(512);
}

void decodeNotificationContent(Notification &notification, const QUrl &portal, const QString &text,
                               const QJsonObject &params, const QString &authorId, const QString &authorName)
{
    const auto component = params.value(QStringLiteral("COMPONENT_PARAMS")).toObject();
    const auto userId = portalTaskIdentifier(authorId);
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
    if (notification.type == QLatin1String("tasks") &&
        (notification.title.isEmpty() || notification.title == QStringLiteral("Уведомление Битрикс24"))) {
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
    prepareNotificationDisplay(notification);
}

std::optional<NotificationTarget> incomingMessageTarget(const QJsonObject &params, bool groupEvent,
                                                        bool allowMissingMessageId)
{
    const auto message = params.value(QStringLiteral("message")).toObject();
    const auto messageId = portalTaskIdentifier(cloud_rows::string(message.value(QStringLiteral("id"))));
    const auto rawDialog = params.value(QStringLiteral("dialogId"));
    auto dialog = rawDialog.isDouble() ? portalTaskIdentifier(cloud_rows::string(rawDialog)) : dialogIdentifier(rawDialog.toString());
    auto chatId = portalTaskIdentifier(cloud_rows::string(params.value(QStringLiteral("chatId"))));
    auto messageChat = portalTaskIdentifier(cloud_rows::string(message.value(QStringLiteral("chatId"))));
    if (messageChat.isEmpty())
        messageChat = portalTaskIdentifier(cloud_rows::string(message.value(QStringLiteral("chat_id"))));
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

} // namespace bx
