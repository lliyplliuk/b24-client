#include "cloud/cloud_backend.h"

#include "cloud/cloud_avatars.h"
#include "cloud/cloud_chat_fields.h"
#include "cloud/cloud_chat_metadata.h"
#include "cloud/cloud_rows.h"
#include "core/notification_targets.h"
#include "core/origin_guard.h"

#include <QJsonArray>
#include <QUuid>

namespace bx {
using chat_fields::positiveId;
using chat_fields::groupChatId;
using cloud_rows::field;
using cloud_rows::responseFiles;

namespace {

constexpr qsizetype chatAssetPageLimit = 50;
constexpr qint64 chatLinkMaxOffset = 10000;

const QString resultEnvelope = QStringLiteral("result");

} // namespace

void CloudBackend::requestChatAssets(const QString &dialog, ChatAssetKind kind, const QString &cursor)
{
    const auto operation = operationKey(OperationKind::ChatAssets, dialog, QString::number(int(kind)), cursor);
    const bool links = kind == ChatAssetKind::Links;
    if (kind != ChatAssetKind::Media && kind != ChatAssetKind::Files && !links)
        return;
    const auto boundary = positiveId(cursor);
    if ((!cursor.isEmpty() && boundary <= 0) || (links && boundary > chatLinkMaxOffset)) {
        emit failed(operation, QStringLiteral("Некорректная граница списка вложений"));
        return;
    }
    resolveDialog(dialog, false, operation, operation,
                  [this, dialog, kind, cursor, operation, boundary](const QString &, qint64 chatId) {
        loadChatAssets(chatId, kind, boundary, operation, [this, dialog, kind, cursor](QVector<ChatAsset> items, QString next) {
            emit chatAssetsLoaded(dialog, kind, cursor, items, next);
        });
    });
}

void CloudBackend::requestChatLinkPreviews(const QString &scopeId, bool task, ChatLinkPreviewHandler complete)
{
    const auto dialog = task ? m_timeline.taskDialog(scopeId) : validDialog(scopeId);
    const auto chatId = badgeChatId(dialog);
    if (chatId <= 0 || m_authExpired) {
        complete({});
        return;
    }
    const auto operation = operationKey(OperationKind::LinkPreviewChatAssets, QUuid::createUuid().toString(QUuid::WithoutBraces));
    loadChatAssets(chatId, ChatAssetKind::Links, 0, operation,
                   [complete = std::move(complete)](QVector<ChatAsset> items, QString) {
        complete(std::move(items));
    });
}

void CloudBackend::loadChatAssets(qint64 chatId, ChatAssetKind kind, qint64 boundary, const QString &operation,
                                  std::function<void(QVector<ChatAsset>, QString)> complete)
{
    const bool links = kind == ChatAssetKind::Links;
    const bool preview = parseOperation(operation).kind == OperationKind::LinkPreviewChatAssets;
    const auto scope = preview ? settingsScope() : QString{};
    Fields fields{{QStringLiteral("CHAT_ID"), QString::number(chatId)}, {QStringLiteral("LIMIT"), QString::number(chatAssetPageLimit)}};
    if (!links)
        fields.append({QStringLiteral("GROUP"), kind == ChatAssetKind::Media ? QStringLiteral("media") : QStringLiteral("file")});
    if (boundary > 0)
        fields.append({links ? QStringLiteral("OFFSET") : QStringLiteral("LAST_ID"), QString::number(boundary)});
    const auto url = OriginGuard(m_session.origin).resolve(links ? QStringLiteral("/rest/im.chat.url.get.json")
                                                                 : QStringLiteral("/rest/im.chat.file.get.json"));
    m_portalHttp.webRequest(url, fields, operation, resultEnvelope,
               [this, operation, links, boundary, chatId, preview, scope,
                complete = std::move(complete)](const QJsonValue &value) {
        if (preview && (m_authExpired || settingsScope() != scope)) {
            complete({}, {});
            return;
        }
        const auto result = value.toObject();
        const auto rawRows = result.value(QStringLiteral("list"));
        if (!rawRows.isArray() && preview) {
            complete({}, {});
            return;
        }
        if (!rawRows.isArray())
            return failShape(operation);
        const auto rows = rawRows.toArray();
        const auto files = responseFiles(result.value(QStringLiteral("files")));
        const auto count = std::min(chatAssetPageLimit, rows.size());
        QVector<ChatAsset> decoded;
        QSet<QString> seen;
        qint64 last = 0;
        for (qsizetype i = 0; i < count; ++i) {
            const auto row = rows.at(i).toObject();
            const auto numeric = positiveId(field(row, {"id"}));
            const auto rawChat = row.value(QStringLiteral("chatId"));
            if (numeric <= 0 || (!rawChat.isUndefined() && positiveId(cloud_rows::string(rawChat)) != chatId))
                continue;
            last = last > 0 ? std::min(last, numeric) : numeric;
            ChatAsset asset;
            asset.id = QString::number(numeric);
            const auto messageId = positiveId(field(row, {"messageId"}));
            if (messageId > 0)
                asset.messageId = QString::number(messageId);
            if (seen.contains(asset.id))
                continue;
            seen.insert(asset.id);
            if (links) {
                const auto link = row.value(QStringLiteral("url")).toObject();
                const auto raw = field(link, {"source"});
                if (raw.isEmpty() || raw.size() > 4096)
                    continue;
                const QUrl parsed(raw, QUrl::StrictMode);
                if (!parsed.isValid() || parsed.host().isEmpty() || !parsed.userInfo().isEmpty()
                    || (parsed.scheme() != QLatin1String("https") && parsed.scheme() != QLatin1String("http")))
                    continue;
                asset.link = parsed;
                const auto rich = link.value(QStringLiteral("richData")).toObject();
                asset.name = notificationDisplayText(field(rich, {"name"})).left(250);
                if (preview && messageId > 0 && !OriginGuard(m_session.origin).isSameOrigin(parsed)) {
                    asset.description = notificationDisplayText(field(rich, {"description"})).simplified().left(350);
                    asset.thumbnailUrl = messageImageUrlFromPortal(field(rich, {"previewUrl"}), {}, false);
                }
                if (asset.name.isEmpty())
                    asset.name = parsed.toDisplayString();
            } else {
                const auto fileId = field(row, {"fileId"});
                asset.file = decodeChatFile(fileId, files.value(fileId));
                if (asset.file.name.isEmpty())
                    continue;
                asset.name = asset.file.name;
            }
            decoded.append(std::move(asset));
        }
        QString next;
        if (count == chatAssetPageLimit) {
            if (links && boundary + count <= chatLinkMaxOffset)
                next = QString::number(boundary + count);
            else if (!links && last > 0 && (boundary == 0 || last < boundary))
                next = QString::number(last);
        }
        complete(std::move(decoded), std::move(next));
    });
}

void CloudBackend::requestMessageViewers(const QString &scopeId, const QString &messageId, bool task,
                                        MessageViewersHandler complete)
{
    const auto scope = settingsScope();
    const auto dialog = task ? m_timeline.taskDialog(scopeId) : validDialog(scopeId);
    const auto id = positiveId(messageId);
    const auto chatId = badgeChatId(dialog);
    const auto group = groupChatId(dialog);
    const auto viewerDialog = scope + QLatin1Char(':') + dialog;
    const auto key = viewerDialog + QLatin1Char(':') + QString::number(id);
    const auto *knownCount = m_messageViewerCounts.object(key);
    const auto *member = m_messageViewerMembership.object(viewerDialog);
    if (m_authExpired || scope.isEmpty() || id <= 0 || chatId <= 0 || !knownCount
        || (group > 0 && (group != chatId || !member || !*member))
        || (task ? m_timeline.activeTask() != scopeId : m_timeline.activeDialog() != dialog)) {
        complete(std::nullopt);
        return;
    }
    const auto operation = operationKey(OperationKind::MessageViewers, QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto url = OriginGuard(m_session.origin).resolve(QStringLiteral("/rest/im.v2.Chat.Message.tailViewers.json"));
    m_portalHttp.webRequest(url, {{QStringLiteral("id"), QString::number(id)},
                     {QStringLiteral("limit"), QString::number(tunables::kPageSizeChats)}},
               operation, resultEnvelope,
               [this, scope, dialog, task, scopeId, id, chatId, count = *knownCount, group, viewerDialog, key,
                complete = std::move(complete)](QJsonValue response) {
        const auto result = response.toObject();
        const auto views = result.value(QStringLiteral("views"));
        const auto *memberNow = m_messageViewerMembership.object(viewerDialog);
        if (m_authExpired || settingsScope() != scope || !views.isArray()
            || !m_messageViewerCounts.contains(key) || badgeChatId(dialog) != chatId
            || (group > 0 && (!memberNow || !*memberNow))
            || (task ? m_timeline.activeTask() != scopeId : m_timeline.activeDialog() != dialog)) {
            complete(std::nullopt);
            return;
        }
        QHash<qint64, QJsonObject> users;
        const auto profiles = result.value(QStringLiteral("users")).toArray();
        for (qsizetype index = 0; index < qMin<qsizetype>(profiles.size(), tunables::kPageSizeChats); ++index) {
            const auto user = profiles[index].toObject();
            const auto uid = positiveId(field(user, {"id", "ID"}));
            if (uid > 0)
                users.insert(uid, user);
        }
        MessageViewers decoded;
        const auto array = views.toArray();
        const auto length = qMin<qsizetype>(array.size(), tunables::kPageSizeChats);
        QSet<qint64> seen;
        for (qsizetype index = 0; index < length; ++index) {
            const auto view = array[index].toObject();
            const auto explicitChat = view.value(QStringLiteral("chatId"));
            if (positiveId(field(view, {"messageId"})) != id
                || (!explicitChat.isUndefined() && positiveId(cloud_rows::string(explicitChat)) != chatId)) {
                complete(std::nullopt);
                return;
            }
            const auto uid = positiveId(field(view, {"userId"}));
            if (uid <= 0 || QString::number(uid) == m_userId || seen.contains(uid))
                continue;
            seen.insert(uid);
            const auto user = users.value(uid);
            auto name = userName(user);
            if (name.isEmpty())
                name = QStringLiteral("Пользователь №%1").arg(uid);
            decoded.people.append({QString::number(uid), name.left(160), {},
                                   avatarUrlFromPortal(field(user, {"avatar", "avatarUrl"}))});
        }
        const auto reported = result.value(QStringLiteral("viewedCount")).toInteger(count);
        const auto more = result.value(QStringLiteral("hasNextPage"));
        decoded.hasMore = reported > decoded.people.size()
            || (more.isBool() ? more.toBool() : array.size() >= tunables::kPageSizeChats);
        decoded.count = reported >= decoded.people.size() ? reported : decoded.hasMore ? -1 : decoded.people.size();
        complete(std::move(decoded));
    });
}

} // namespace bx
