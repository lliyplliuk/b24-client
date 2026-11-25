#pragma once

#include "core/models.h"

#include <QRegularExpression>
#include <QUrlQuery>

class QJsonObject;

namespace bx {

struct PortalTaskReference {
    QString projectId;
    QString taskId;
};

inline QString portalTaskIdentifier(const QString &value)
{
    if (value.isEmpty() || value.size() > 19)
        return {};
    for (const auto character : value) {
        if (character < QLatin1Char('0') || character > QLatin1Char('9'))
            return {};
    }
    const auto number = value.toLongLong();
    return number > 0 ? QString::number(number) : QString();
}

inline QString portalPersonPath(const QString &path)
{
    static const QRegularExpression user(
        QStringLiteral("^/(?:extranet/)?company/personal/user/([0-9]+)/?\\z"));
    return portalTaskIdentifier(user.match(path).captured(1));
}

inline PortalTaskReference portalTaskPath(const QString &path)
{
    static const QRegularExpression group(
        QStringLiteral("^/(?:extranet/)?workgroups/group/([0-9]+)/tasks/task/view/([0-9]+)/?\\z"));
    const auto groupMatch = group.match(path);
    if (groupMatch.hasMatch()) {
        const auto projectId = portalTaskIdentifier(groupMatch.captured(1));
        const auto taskId = portalTaskIdentifier(groupMatch.captured(2));
        if (!projectId.isEmpty() && !taskId.isEmpty())
            return {projectId, taskId};
        return {};
    }
    static const QRegularExpression personal(
        QStringLiteral("^/(?:extranet/)?(?:company/personal/user/[0-9]+/)?tasks/task/view/([0-9]+)/?\\z"));
    return {{}, portalTaskIdentifier(personal.match(path).captured(1))};
}

inline PortalTaskReference portalTaskUrl(const QUrl &url)
{
    if (!url.isValid())
        return {};
    const auto path = url.path(QUrl::FullyDecoded);
    if (auto reference = portalTaskPath(path); !reference.taskId.isEmpty())
        return reference;
    if (path == QLatin1String("/tasks") || path == QLatin1String("/tasks/") ||
        path == QLatin1String("/extranet/tasks") || path == QLatin1String("/extranet/tasks/")) {
        const auto ids = QUrlQuery(url).allQueryItemValues(QStringLiteral("task_id"));
        if (ids.size() == 1)
            return {{}, portalTaskIdentifier(ids.front())};
    }
    return {};
}

QVector<NotificationTarget> notificationTargets(const QUrl &portal, const QString &notificationMarkup);
std::optional<NotificationTarget> portalLinkTarget(const QUrl &portal, const QUrl &url);
std::optional<NotificationTarget> portalLinkPreviewTarget(const QUrl &portal, const QUrl &url);
QString notificationDisplayText(const QString &notificationMarkup);
void decodeNotificationContent(Notification &notification, const QUrl &portal, const QString &text,
                               const QJsonObject &params, const QString &authorId = {}, const QString &authorName = {});
inline bool isLocalNotificationId(const QString &id)
{
    if (id.startsWith(QLatin1String("message:"))) {
        const auto message = id.sliced(8);
        return !message.isEmpty() && portalTaskIdentifier(message) == message;
    }
    return id.startsWith(QLatin1String("push:")) && id.size() > 5 && id.size() <= 133;
}
std::optional<NotificationTarget> incomingMessageTarget(const QJsonObject &params, bool groupEvent,
                                                        bool allowMissingMessageId = false);

} // namespace bx
