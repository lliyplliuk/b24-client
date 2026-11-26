#include "cloud/notifications_service.h"
#include "cloud/cloud_chat_metadata.h"
#include "cloud/chat_timeline.h"
#include "cloud/cloud_rows.h"
#include "cloud/cloud_notification_links.h"
#include "core/notification_targets.h"
#include "core/tunables.h"

#include <QUuid>
#include <algorithm>
#include <utility>

namespace bx {
using namespace cloud_rows;

const QString soundsOp = operationKey(OperationKind::ChatSoundSettings);
const QString soundMuted = QStringLiteral("soundMuted");
const QString volumeGeneral = QStringLiteral("soundVolumeGeneral");
const QString volumeMessages = QStringLiteral("soundVolumeMessages");
const QString notifyScheme = QStringLiteral("notifyScheme");
const QString notifyOnComputer = QStringLiteral("notifySchemeSendSite");
const QString simpleNotifyScheme = QStringLiteral("simple");
const QStringList simpleChannels{notifyOnComputer, QStringLiteral("notifySchemeSendEmail"),
                                 QStringLiteral("notifySchemeSendPush")};
constexpr int maxPendingChatPopups = 12;
constexpr int freshHistoryPopupSeconds = 30;

void NotificationsService::handleIncomingNoticePull(const QString &command, const QString &dialog, const QString &chatId, const QJsonObject &params, const QJsonObject &extra, const QString &mid)
{
    const auto message = params.value(QStringLiteral("message")).toObject();
    auto notification = params.value(QStringLiteral("notify")).toObject();
    const auto author = numericId(field(message, {"senderId", "authorId"}));
    const auto templateId = field(message, {"templateId", "uuid"});
    if ((!m_host.currentUserId().isEmpty() && m_host.currentUserId() == author)
        || (!templateId.isEmpty() && m_timeline.wasSentTemplate(templateId)))
        return;
    const bool isMessage = command.startsWith(QLatin1String("message"));
    if (!isMessage && params.value(QStringLiteral("onlyFlash")).toBool())
        return;
    const auto outerId = field(params, {"id"});
    const auto nestedId = field(notification, {"id"});
    if (!outerId.isEmpty() && !nestedId.isEmpty() && outerId != nestedId)
        notification = {};
    auto notifyTitle = notificationDisplayText(field(params, {"notify_title", "title"}));
    if (notifyTitle.isEmpty())
        notifyTitle = notificationDisplayText(field(notification, {"notify_title", "title"}));
    const auto title = isMessage ? m_host.recentTitle(dialog, QStringLiteral("Новое сообщение"))
        : notifyTitle.isEmpty() ? QStringLiteral("Уведомление Битрикс24") : notifyTitle;
    QString content = field(message, {"text", "message"});
    if (content.isEmpty())
        content = field(notification, {"text", "message"});
    if (content.isEmpty())
        content = field(params, {"text", "message", "notify"});
    if (content.isEmpty() && isMessage)
        content = title;
    auto id = numericId(isMessage ? field(message, {"id"}) : outerId.isEmpty() ? nestedId : outerId);
    if (id.isEmpty())
        id = QStringLiteral("push:") + (mid.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : mid);
    else if (isMessage)
        id.prepend(QStringLiteral("message:"));
    const auto notify = notificationFlag(params.value(QStringLiteral("notify")));
    const auto messageNotify = notificationFlag(message.value(QStringLiteral("params")).toObject().value(QStringLiteral("NOTIFY")));
    const auto blockedUsers = params.value(QStringLiteral("userBlockChat")).toObject().value(chatId);
    if (isMessage && (notify == false || messageNotify == false || mutedListContains(blockedUsers, m_host.currentUserId())
                      || m_host.popupPortalStatus() == QLatin1String("dnd"))) {
        rememberChatPopup(dialog, id);
        m_pendingChatPopups.removeIf([&](const ChatPopup &popup) {
            return popup.dialog == dialog && popup.entry.id == id;
        });
    }
    if (std::any_of(m_notifications.cbegin(), m_notifications.cend(), [&id](const auto &item) { return item.id == id; }))
        return;
    const auto serverTime = extra.value(QStringLiteral("server_time_unix"));
    auto published = date(serverTime);
    if (!published.isValid()) {
        const auto seconds = numericId(string(serverTime)).toLongLong();
        if (seconds > 0)
            published = QDateTime::fromSecsSinceEpoch(seconds);
    }
    const auto now = QDateTime::currentDateTime();
    bool hasAge = false;
    const auto age = extra.value(QStringLiteral("server_time_ago")).toVariant().toInt(&hasAge);
    auto noticeModule = field(notification, {"notify_module", "module"});
    if (noticeModule.isEmpty())
        noticeModule = field(params, {"notify_module", "module"});
    Notification entry{id, isMessage || noticeModule.isEmpty() ? QStringLiteral("im") : noticeModule, title, {},
                       published.isValid() ? published : now.addSecs(hasAge ? -age : 0)};
    if (isLocalNotificationId(id)) {
        const auto previous = m_notificationMutations.constFind(id);
        if (previous != m_notificationMutations.cend())
            entry.read = previous->read;
    }
    if (isMessage)
        prepareMessagePullNotice(entry, command, content, params);
    else
        preparePortalPullNotice(entry, content, notification, params);
    m_notifications.prepend(entry);
    trimNotificationHistory(m_notifications, m_notificationOlderPages);
    emit notificationsLoaded(m_notifications);
    if (m_host.popupPortalStatus() == QLatin1String("dnd"))
        return;
    const bool fresh = (!hasAge || age <= freshPopupSeconds)
        && (!published.isValid() || published.secsTo(now) <= freshPopupSeconds);
    if (!isMessage) {
        if (fresh && field(params, {"silent"}) != QLatin1String("Y")) {
            if (m_host.initialStatusPending()) {
                if (m_pendingInitialAlerts.size() == maxPendingChatPopups)
                    m_pendingInitialAlerts.dequeue();
                m_pendingInitialAlerts.enqueue(entry);
            } else {
                emit notificationReceived(entry);
            }
        }
        return;
    }
    const bool important = message.value(QStringLiteral("isImportant")).toBool();
    if (!fresh || (!published.isValid() && !hasAge) || author.isEmpty() || dialog.isEmpty()
        || notify == false || messageNotify == false || (!important && notify != true))
        return;
    const auto pullChats = params.value(QStringLiteral("chat")).toObject();
    auto eventChat = pullChats.value(chatId).toObject();
    if (eventChat.isEmpty() && !field(pullChats, {"id", "chatId"}).isEmpty())
        eventChat = pullChats;
    if (!eventChat.isEmpty())
        m_host.rememberDialog(dialog, {{QStringLiteral("chat"), eventChat}}, true);
    queueChatPopup({entry, dialog, author, blockedUsers});
}

void NotificationsService::prepareMessagePullNotice(Notification &entry, const QString &command, const QString &content, const QJsonObject &params)
{
    entry.text = notificationDisplayText(content);
    auto &targets = entry.targets;
    targets = notificationTargets(m_host.portalOrigin(), content);
    auto target = incomingMessageTarget(params, command == QLatin1String("messageChat"), true);
    QString taskId;
    if (target) {
        const auto chatMap = params.value(QStringLiteral("chat")).toObject();
        const auto targetChatId = QString::number(groupDialogChatId(target->id));
        auto eventChat = chatMap.value(targetChatId).toObject();
        if (eventChat.isEmpty() && numericId(field(chatMap, {"id", "chatId", "CHAT_ID"})) == targetChatId)
            eventChat = chatMap;
        const auto knownDialog = oneOf(eventChat, {"dialogId"});
        if (!knownDialog.isUndefined() && validDialog(string(knownDialog)) != target->id)
            eventChat = {};
        for (const auto *key : {"id", "chatId", "CHAT_ID"}) {
            const auto reportedId = eventChat.value(QLatin1String(key));
            if (!reportedId.isUndefined() && numericId(string(reportedId)) != targetChatId) {
                eventChat = {};
                break;
            }
        }
        taskId = target->id.startsWith(QLatin1String("chat"))
            ? notificationTaskId(target->id, eventChat) : QString();
        const bool listedTask = std::any_of(m_timeline.recentChats(true).cbegin(), m_timeline.recentChats(true).cend(),
                                           [&target](const Chat &chat) { return chat.id == target->id; });
        const auto sections = params.value(QStringLiteral("recentConfig")).toObject().value(QStringLiteral("sections")).toArray();
        const bool metadataTaskChat = isTaskChatMetadata(field(eventChat, {"type", "kind"}),
            eventChat.value(QStringLiteral("entityLink")).toObject())
            || isTaskChatType(field(eventChat, {"entityType", "entity_type"}));
        target->taskChat = !taskId.isEmpty() || isRecentTaskChat(metadataTaskChat, listedTask, sections)
            || m_timeline.hasTaskForDialog(target->id);
        entry.ordinaryChatMessage = !target->taskChat;
    }
    if (target && !target->messageId.isEmpty()) {
        if (!taskId.isEmpty()) {
            const auto taskLink = QStringLiteral("[URL=/tasks/task/view/%1/]Задача #%1[/URL]").arg(taskId);
            const auto plainMessage = entry.text;
            decodeNotificationContent(entry, m_host.portalOrigin(), taskLink + QLatin1Char('\n') + content, {});
            entry.text = plainMessage;
        }
        const auto duplicate = std::find_if(targets.begin(), targets.end(), [&target](const auto &link) {
            return link.kind == NotificationTargetKind::Chat && link.id == target->id
                && link.messageId == target->messageId;
        });
        if (duplicate == targets.end()) {
            if (targets.size() >= 8)
                targets.removeLast();
            targets.prepend(std::move(*target));
        } else if (target->taskChat) {
            duplicate->taskChat = true;
        }
    }
    prepareNotificationDisplay(entry);
}

void NotificationsService::preparePortalPullNotice(Notification &entry, const QString &content, const QJsonObject &notification, const QJsonObject &params)
{
    auto noticeParams = notification.value(QStringLiteral("params")).toObject();
    if (noticeParams.isEmpty())
        noticeParams = params.value(QStringLiteral("params")).toObject();
    if (noticeParams.isEmpty())
        noticeParams = notification.isEmpty() ? params : notification;
    const auto authorId = noticeAuthorId(notification, params);
    QString authorName;
    for (const auto *source : {&params, &notification}) {
        for (const auto *key : {"users", "user", "author"}) {
            authorName = noticeAuthorName(authorId, source->value(QLatin1String(key)));
            if (!authorName.isEmpty())
                break;
        }
        if (!authorName.isEmpty())
            break;
    }
    decodeNotificationContent(entry, m_host.portalOrigin(), content, noticeParams, authorId, authorName);
    if (entry.text.isEmpty()) {
        entry.text = entry.title;
        prepareNotificationDisplay(entry);
    }
}

void NotificationsService::deliverChatPopups()
{
    if (m_host.initialStatusPending())
        return;
    if (m_host.authExpired() || m_host.popupPortalStatus() == QLatin1String("dnd")) {
        m_pendingChatPopups.clear();
        return;
    }
    const auto enabled = chatPopupsEnabled();
    if (enabled == false) {
        m_pendingChatPopups.clear();
        return;
    }
    const auto now = QDateTime::currentDateTime();
    auto waiting = std::exchange(m_pendingChatPopups, {});
    for (const auto &popup : waiting) {
        if (!m_host.started())
            return;
        const bool watching = watchingPopupDialog(popup.dialog);
        if ((!m_host.chatOnline() && (!popup.fromHistory || m_host.popupPortalStatus().isEmpty()))
            || (popup.fromHistory && (m_host.chatForeground() || !m_host.popupWorkspaceVisible() || !watching)))
            continue;
        const auto maxAge = popup.fromHistory ? freshHistoryPopupSeconds : freshPopupSeconds;
        const auto id = popup.entry.id.section(QLatin1Char(':'), -1).toLongLong();
        const bool read = popup.fromHistory && noticeConfirmedRead(popup.dialog, id);
        if (read || popup.author == m_host.currentUserId() || popup.entry.createdAt.secsTo(now) > maxAge || chatMuted(popup))
            continue;
        if (enabled == true && !m_host.currentUserId().isEmpty())
            emit notificationReceived(popup.entry);
        else
            m_pendingChatPopups.enqueue(popup);
    }
}

bool NotificationsService::chatMuted(const ChatPopup &popup) const
{
    const auto *state = m_timeline.findMessageState(popup.dialog);
    const auto user = m_host.currentUserId();
    return mutedListContains(popup.muteList, user)
        || (state
            && (state->muted.value_or(false) || mutedListContains(state->muteList, user)));
}

bool NotificationsService::rememberChatPopup(const QString &dialog, const QString &noticeId)
{
    if (dialog.isEmpty() || noticeId.isEmpty())
        return false;
    const auto key = dialog + QLatin1Char('\n') + noticeId;
    if (m_seenChatPopups.contains(key))
        return false;
    m_seenChatPopups.insert(key, new bool(true));
    return true;
}

void NotificationsService::queueChatPopup(ChatPopup popup)
{
    if (!rememberChatPopup(popup.dialog, popup.entry.id) || chatMuted(popup) || chatPopupsEnabled() == false)
        return;
    if (m_pendingChatPopups.size() == maxPendingChatPopups)
        m_pendingChatPopups.dequeue();
    m_pendingChatPopups.enqueue(std::move(popup));
    deliverChatPopups();
    if (!m_pendingChatPopups.isEmpty())
        loadChatPopupSettings();
}

void NotificationsService::reconcileHistoryPopups(const QString &dialog, qint64 chatId, bool task, const QJsonObject &history,
                                         const QVector<Message> &messages)
{
    if (chatId <= 0)
        return;

    const auto snapshot = history.value(QStringLiteral("messages")).toArray();
    QHash<qint64, QJsonObject> rows;
    qint64 newest = 0;
    for (qsizetype i = 0; i < qMin<qsizetype>(snapshot.size(), tunables::kPageSizeMessages); ++i) {
        const auto row = snapshot.at(i).toObject();
        const auto id = recentMessageId(row);
        const auto rowChat = explicitMessageChatId(row);
        if (id > 0 && (rowChat == 0 || rowChat == chatId)) {
            rows.insert(id, row);
            newest = qMax(newest, id);
        }
    }
    if (!snapshot.isEmpty() && rows.isEmpty())
        return;
    auto &scope = task ? m_taskPopupHistory : m_chatPopupHistory;
    const auto previous = scope.dialog == dialog ? scope.through.value_or(-1) : -1;
    scope = {dialog, qMax(newest, previous)};
    m_pendingChatPopups.removeIf([&](const ChatPopup &popup) {
        if (!popup.fromHistory || popup.dialog != dialog)
            return false;
        const auto id = popup.entry.id.section(QLatin1Char(':'), -1).toLongLong();
        const auto unread = rows.value(id).value(QStringLiteral("unread"));
        if (unread.isBool())
            return !unread.toBool();
        return noticeHistoryRead(dialog, id);
    });
    if (previous < 0 || m_host.chatForeground() || !m_host.popupWorkspaceVisible() || (!task && !m_host.chatsSectionActive())
        || m_host.authExpired() || m_host.currentUserId().isEmpty() || m_host.popupPortalStatus() == QLatin1String("dnd"))
        return;
    if (!m_host.chatOnline() && m_host.popupPortalStatus().isEmpty())
        return;

    const auto now = QDateTime::currentDateTime();
    const auto title = m_host.recentTitle(dialog, QStringLiteral("Новое сообщение"));
    const auto chat = history.value(QStringLiteral("chat")).toObject();
    const bool taskChat = task || !notificationTaskId(dialog, chat).isEmpty();
    for (const auto &message : messages) {
        const auto id = numericId(message.id).toLongLong();
        if (id <= previous || !message.unread || message.outgoing || message.system || !message.createdAt.isValid())
            continue;
        const auto age = message.createdAt.secsTo(now);
        if (age < -5 || age > freshHistoryPopupSeconds)
            continue;
        const auto row = rows.value(id);
        const auto author = explicitMessageAuthorId(row);
        const auto params = oneOf(row, {"params", "PARAMS"}).toObject();
        if (!author || *author <= 0 || QString::number(*author) == m_host.currentUserId()
            || notificationFlag(oneOf(row, {"notify", "NOTIFY"})) == false
            || notificationFlag(oneOf(params, {"NOTIFY", "notify"})) == false
            || notificationFlag(row.value(QStringLiteral("silent"))) == true
            || notificationFlag(row.value(QStringLiteral("isDeleted"))) == true
            || notificationFlag(params.value(QStringLiteral("IS_DELETED"))) == true)
            continue;
        auto text = notificationDisplayText(message.text);
        if (text.isEmpty())
            text = message.attachments.isEmpty() ? QStringLiteral("Новое сообщение") : QStringLiteral("Вложение");
        Notification entry{QStringLiteral("message:%1").arg(id), QStringLiteral("im"), title, text, message.createdAt};
        entry.ordinaryChatMessage = !taskChat;
        entry.targets.append({NotificationTargetKind::Chat, dialog, QString::number(id), {}, title, taskChat});
        prepareNotificationDisplay(entry);
        queueChatPopup({std::move(entry), dialog, QString::number(*author), {}, true});
    }
}

void NotificationsService::settleInitialPortalStatus()
{
    const auto pending = std::exchange(m_pendingInitialAlerts, {});
    const auto now = QDateTime::currentDateTime();
    for (const auto &entry : pending) {
        if (!m_host.started() || m_host.authExpired() || m_host.popupPortalStatus() == QLatin1String("dnd"))
            return;
        const auto current = std::find_if(m_notifications.cbegin(), m_notifications.cend(), [&entry](const auto &item) {
            return item.id == entry.id;
        });
        if (current != m_notifications.cend() && !current->read
            && entry.createdAt.secsTo(now) <= freshPopupSeconds)
            emit notificationReceived(entry);
    }
    deliverChatPopups();
}

void NotificationsService::handleSettingsPull(const QJsonObject &params)
{
    const auto revision = ++m_notificationSettingsRevision;
    const auto remember = [this, &params, revision](const QString &key) {
        if (params.contains(key))
            m_notificationSettingChanges.insert(key, {revision, params.value(key)});
    };
    for (const auto &key : {soundMuted, volumeGeneral, volumeMessages, notifyScheme})
        remember(key);
    for (const auto &key : simpleChannels)
        remember(key);
    if (params.contains(notifyScheme))
        m_notificationSettings.clear();
    applyChatPopupSettings(params, ChatPopupSettingsSource::Push);
}

void NotificationsService::loadChatPopupSettings()
{
    const auto now = QDateTime::currentMSecsSinceEpoch();
    if (m_portalHttp.contains(soundsOp) || now < m_nextChatPopupSettingsRequest)
        return;
    m_nextChatPopupSettingsRequest = now + 3000;
    const bool hadUser = !m_host.currentUserId().isEmpty();
    const auto generation = m_chatPopupSettingsGeneration;
    m_host.withCurrentUser(soundsOp, [this, hadUser, generation](qint64 userId) {
        if (generation != m_chatPopupSettingsGeneration) {
            loadChatPopupSettings();
            return;
        }
        if (!hadUser)
            m_host.popupIdentityResolved();
        m_portalHttp.ajax(QStringLiteral("im.v2.Settings.General.list"), {{QStringLiteral("userId"), QString::number(userId)}},
             soundsOp, [this, generation](QJsonValue data) {
            if (generation != m_chatPopupSettingsGeneration) {
                loadChatPopupSettings();
                return;
            }
            applyChatPopupSettings(data.toObject(), ChatPopupSettingsSource::InitialSnapshot);
        });
    });
}

void NotificationsService::applyChatPopupSettings(const QJsonObject &settings, ChatPopupSettingsSource source)
{
    const bool push = source == ChatPopupSettingsSource::Push;
    const bool initial = source == ChatPopupSettingsSource::InitialSnapshot;
    if (settings.value(notifyScheme) == simpleNotifyScheme && !notificationFlag(settings.value(notifyOnComputer))) {
        if (push && m_chatPopupSettings.value(notifyScheme) != simpleNotifyScheme) {
            // A scheme change can omit the computer setting. Refresh it before displaying a banner.
            m_chatPopupSettings.remove(notifyOnComputer);
            m_chatPopupSettingsKnown = false;
            ++m_chatPopupSettingsGeneration;
            m_nextChatPopupSettingsRequest = 0;
        } else if (source == ChatPopupSettingsSource::RefreshSnapshot) {
            m_chatPopupSettings.remove(notifyOnComputer);
        }
    }
    if (!push && !settings.isEmpty())
        m_chatPopupSettingsKnown = true;
    for (const auto *key : {&notifyScheme, &notifyOnComputer, &soundMuted, &volumeGeneral, &volumeMessages}) {
        const auto setting = settings.value(*key);
        if (!setting.isUndefined() && (!initial || !m_chatPopupSettings.contains(*key)))
            m_chatPopupSettings.insert(*key, setting);
    }
    deliverChatPopups();
}

std::optional<bool> NotificationsService::chatPopupsEnabled() const
{
    const auto scheme = m_chatPopupSettings.value(notifyScheme);
    if (scheme == simpleNotifyScheme) {
        if (const auto desktop = notificationFlag(m_chatPopupSettings.value(notifyOnComputer)))
            return desktop;
    }
    if (!m_chatPopupSettingsKnown && scheme != QLatin1String("expert"))
        return std::nullopt;

    const auto isMuted = notificationFlag(m_chatPopupSettings.value(soundMuted));
    bool generalKnown = false;
    bool messagesKnown = false;
    const auto general = m_chatPopupSettings.value(volumeGeneral).toVariant().toDouble(&generalKnown);
    const auto messages = m_chatPopupSettings.value(volumeMessages).toVariant().toDouble(&messagesKnown);
    if (isMuted == true || (generalKnown && general <= 0) || (messagesKnown && messages <= 0))
        return false;
    if (isMuted == false && generalKnown && messagesKnown)
        return true;
    return std::nullopt;
}

void NotificationsService::requestNotificationSettings()
{
    requestNotificationSettings(operationKey(OperationKind::NotificationSettings));
}

void NotificationsService::updateNotificationSetting(const QString &name, int value)
{
    const auto operation = operationKey(OperationKind::NotificationSettingsUpdate);
    const bool toggle = name == soundMuted || simpleChannels.contains(name);
    if (!m_notificationSettings.contains(name) || (!toggle && name != volumeGeneral && name != volumeMessages))
        return m_host.failShape(operation);
    if (value < 0 || value > (toggle ? 1 : 100))
        return m_host.failShape(operation);
    m_host.withCurrentUser(operation, [this, name, value, toggle, operation, started = m_notificationSettingsRevision](qint64 userId) {
        const auto setting = toggle ? QJsonValue(value != 0) : QJsonValue(value);
        m_portalHttp.ajaxJson(QStringLiteral("im.v2.Settings.General.update"),
                 {{QStringLiteral("userId"), userId}, {QStringLiteral("name"), name}, {QStringLiteral("value"), setting},
                  {QStringLiteral("actionUuid"), QUuid::createUuid().toString(QUuid::WithoutBraces)}},
                 operation, [this, name, value, operation, started](QJsonValue response) {
            if (response == false)
                return m_host.failShape(operation);
            requestNotificationSettings(operation, name, value, started);
        });
    });
}

void NotificationsService::requestNotificationSettings(const QString &operation, const QString &updated, int expected,
                                               std::optional<quint64> started)
{
    const auto before = started.value_or(m_notificationSettingsRevision);
    const auto popupGeneration = m_chatPopupSettingsGeneration;
    const auto accountScope = m_host.accountScopeRevision();
    m_host.withCurrentUser(operation, [this, operation, updated, expected, before, popupGeneration, accountScope](qint64 userId) {
        if (accountScope != m_host.accountScopeRevision())
            return;
        m_portalHttp.ajax(QStringLiteral("im.v2.Settings.General.list"), {{QStringLiteral("userId"), QString::number(userId)}},
             operation, [this, operation, updated, expected, before, popupGeneration, accountScope](QJsonValue result) {
            if (accountScope != m_host.accountScopeRevision())
                return;
            auto data = result.toObject();
            for (auto entry = m_notificationSettingChanges.cbegin(); entry != m_notificationSettingChanges.cend(); ++entry)
                if (entry.value().first > before)
                    data.insert(entry.key(), entry.value().second);
            QVariantMap settings;
            if (const auto muted = notificationFlag(data.value(soundMuted)))
                settings.insert(soundMuted, *muted);
            for (const auto &name : {volumeGeneral, volumeMessages}) {
                bool ok = false;
                const auto value = data.value(name).toVariant().toInt(&ok);
                if (ok && value >= 0 && value <= 100)
                    settings.insert(name, value);
            }
            if (data.value(notifyScheme) == simpleNotifyScheme) {
                for (const auto &channel : simpleChannels)
                    if (const auto enabled = notificationFlag(data.value(channel)))
                        settings.insert(channel, *enabled);
            }
            m_notificationSettings = settings;
            if (popupGeneration == m_chatPopupSettingsGeneration)
                applyChatPopupSettings(data, ChatPopupSettingsSource::RefreshSnapshot);
            if (settings.isEmpty())
                return m_host.failShape(operation);
            if (!updated.isEmpty() && settings.contains(updated) && data.contains(updated))
                m_notificationSettingChanges.insert(updated, {++m_notificationSettingsRevision, data.value(updated)});
            emit notificationSettingsLoaded(settings, updated);
            if (!updated.isEmpty() && (!settings.contains(updated) || settings.value(updated).toInt() != expected)) {
                emit failed(operation, QStringLiteral("Портал не подтвердил изменение настройки"));
                return;
            }
        });
    });
}

void NotificationsService::resetSettings()
{
    ++m_chatPopupSettingsGeneration;
    m_chatPopupSettings = {};
    m_chatPopupSettingsKnown = false;
    m_notificationSettings.clear();
    m_notificationSettingChanges.clear();
    m_notificationSettingsRevision = 0;
    m_nextChatPopupSettingsRequest = 0;
}

void NotificationsService::resetReconnectSettings()
{
    ++m_chatPopupSettingsGeneration;
    m_chatPopupSettings = {};
    m_chatPopupSettingsKnown = false;
    m_pendingChatPopups.removeIf([](const ChatPopup &popup) { return !popup.fromHistory; });
    m_nextChatPopupSettingsRequest = 0;
}

bool NotificationsService::reconnectSettingsNeeded() const
{
    return !m_chatPopupSettings.isEmpty() || m_portalHttp.contains(soundsOp);
}

void NotificationsService::clearPendingPopups()
{
    m_pendingChatPopups.clear();
}

void NotificationsService::clearHistory(bool task)
{
    (task ? m_taskPopupHistory : m_chatPopupHistory) = {};
}

bool NotificationsService::noticeHistoryRead(const QString &dialog, qint64 id) const
{
    const auto *state = m_timeline.findMessageState(dialog);
    return state && id <= state->lastReadId && !state->historyUnread.contains(id);
}

bool NotificationsService::noticeConfirmedRead(const QString &dialog, qint64 id) const
{
    const auto *state = m_timeline.findMessageState(dialog);
    return state && (id <= state->confirmedReadThrough || state->exactlyRead.contains(id));
}

bool NotificationsService::watchingPopupDialog(const QString &dialog) const
{
    return (m_host.chatsSectionActive() && dialog == m_timeline.activeDialog()) || (!m_timeline.activeTask().isEmpty() && dialog == m_timeline.activeTaskDialog());
}

QString NotificationsService::notificationTaskId(const QString &dialog, const QJsonObject &eventChat) const
{
    QSet<QString> matching;
    const auto add = [&matching](const QString &id) {
        if (const auto valid = numericId(id); !valid.isEmpty())
            matching.insert(valid);
    };
    const auto taskChat = isTaskChatType(field(eventChat, {"type", "kind"}));
    const auto entity = eventChat.value(QStringLiteral("entityLink")).toObject();
    const auto linkType = field(entity, {"type"});
    if (isTaskChatType(linkType) || (linkType.isEmpty() && taskChat))
        add(field(entity, {"id"}));
    const auto entityType = field(eventChat, {"entityType", "entity_type"});
    if (isTaskChatType(entityType) || (entityType.isEmpty() && taskChat))
        add(field(eventChat, {"entityId", "entity_id"}));
    for (const auto &id : m_timeline.tasksForDialog(dialog))
        add(id);
    for (const auto &known : m_timeline.recentChats(true))
        if (known.id == dialog && known.taskChat)
            add(known.taskId);
    return matching.size() == 1 ? *matching.cbegin() : QString();
}

} // namespace bx
