#include "cloud/portal_http.h"

#include "cloud/cloud_backend.h"
#include "core/origin_guard.h"
#include "signin/session_capture.h"
#include "signin/session_restore.h"

#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QPointer>
#include <QTimer>
#include <QUrlQuery>
#include <algorithm>
#include <utility>

namespace bx {
namespace {

bool shareableRead(OperationKind kind)
{
    // A read step inside a mutation still belongs to that mutation.
    switch (kind) {
    case OperationKind::Chats:
    case OperationKind::TaskChats:
    case OperationKind::ProjectPageFallback:
    case OperationKind::ProjectPinIdentity:
    case OperationKind::SettingsIdentity:
    case OperationKind::RecentUserStatus:
    case OperationKind::ChatSoundSettings:
    case OperationKind::ChatHistoryIdentity:
    case OperationKind::SessionKeepalive:
    case OperationKind::Notifications:
    case OperationKind::NotificationsOlder:
    case OperationKind::ChatHistoryRead:
    case OperationKind::ChatHistorySnapshot:
    case OperationKind::TaskMessages:
    case OperationKind::TaskChatTitles:
    case OperationKind::TaskChatTitlesRequest:
    case OperationKind::ProjectTaskChats:
    case OperationKind::ChatBadgesSnapshot:
    case OperationKind::ProjectUnreadDialogs:
    case OperationKind::ProjectUnreadTasks:
    case OperationKind::PortalStatus:
    case OperationKind::PortalStatusBootstrap:
    case OperationKind::Person:
    case OperationKind::CurrentPerson:
    case OperationKind::SearchPeople:
    case OperationKind::ProjectFiles:
    case OperationKind::ProjectMembers:
    case OperationKind::ProjectStages:
    case OperationKind::Board:
    case OperationKind::BoardColumn:
    case OperationKind::SearchProjectTasks:
    case OperationKind::Task:
    case OperationKind::TaskDetails:
    case OperationKind::TaskFiles:
    case OperationKind::Checklist:
    case OperationKind::Subtasks:
    case OperationKind::GanttLinks:
    case OperationKind::TaskHistory:
    case OperationKind::Timeline:
    case OperationKind::LinkPreview:
    case OperationKind::LinkPreviewStage:
    case OperationKind::LinkPreviewChatAssets:
    case OperationKind::MessageContext:
    case OperationKind::MessageViewers:
    case OperationKind::Pins:
    case OperationKind::ChatAssets:
    case OperationKind::OlderChat:
    case OperationKind::SearchChat:
    case OperationKind::SearchTaskChat:
    case OperationKind::SearchChats:
    case OperationKind::SearchTaskChats:
    case OperationKind::SearchNotifications:
    case OperationKind::NotificationSettings:
        return true;
    default:
        return false;
    }
}

bool safeHeader(const QByteArray &header)
{
    return !header.isEmpty() && header.size() < 16384
        && std::none_of(header.cbegin(), header.cend(), [](char character) {
            const auto byte = static_cast<unsigned char>(character);
            return byte < 32 || byte == 127;
        });
}

} // namespace

PortalHttp::PortalHttp(CloudBackend &backend)
    : QObject(&backend), m_backend(backend)
{
}

bool PortalHttp::sessionUsable(const Session &session, bool allowLoopback)
{
    if (!session.isValid() || !session.origin.userInfo().isEmpty()
        || !safeHeader(session.cookie) || !safeHeader(session.csrf))
        return false;
    return session.origin.scheme() == QLatin1String("https")
        || (allowLoopback && OriginGuard::isLoopback(session.origin)
            && session.origin.scheme() == QLatin1String("http"));
}

void PortalHttp::webRequest(QUrl url, const Fields &fields, const QString &operation,
                            const QString &envelope, Handler onData, const QString &requestKey,
                            const std::shared_ptr<Session> &candidate, bool includeResponseMeta,
                            std::optional<RequestPolicy> policyOverride)
{
    QByteArray encoded;
    const auto appendField = [&encoded](const QString &key, const QString &value) {
        if (!encoded.isEmpty())
            encoded += '&';
        encoded += QUrl::toPercentEncoding(key);
        encoded += '=';
        encoded += QUrl::toPercentEncoding(value);
    };
    appendField(QStringLiteral("sessid"), QString::fromLatin1(candidate ? candidate->csrf : m_backend.m_session.csrf));
    for (const auto &field : fields)
        appendField(field.first, field.second);
    auto *buffer = new QBuffer;
    buffer->setData(std::move(encoded));
    buffer->open(QIODevice::ReadOnly);
    webRequestRaw(std::move(url), buffer,
                  {{"Content-Type", "application/x-www-form-urlencoded; charset=UTF-8"}},
                  operation, envelope, std::move(onData), requestKey, candidate, includeResponseMeta, policyOverride);
}

QString PortalHttp::scopedRequestKey(const QString &operation, RequestPolicy policy, const QString &requestKey) const
{
    if (policy == RequestPolicy::AccountScoped)
        return operation + QLatin1Char(':') + QString::number(m_backend.m_accountScopeRevision);
    return requestKey.isEmpty() ? operation : requestKey;
}

struct PortalHttp::InflightRequest {
    QUrl url;
    Headers headers;
    std::optional<QByteArray> bufferedBody;
    QString operation;
    RequestPolicy policy;
    QString envelope;
    quint64 scope;
    quint64 revision;
    std::shared_ptr<Session> candidate;
    bool includeResponseMeta;
    QList<Handler> handlers;
    bool waitingForHistoryRetry = false;

    void notify(PortalHttp *http, const QJsonValue &value, const QString &error = {}) const
    {
        QPointer<PortalHttp> alive(http);
        for (const auto &handler : handlers) {
            if (!alive || !http->m_backend.m_started || scope != http->m_backend.m_accountScopeRevision)
                return;
            if (error.isEmpty())
                handler.onData(value);
            else
                handler.reject(error, policy == RequestPolicy::Optional);
        }
    }
};

void PortalHttp::webRequestRaw(QUrl url, QIODevice *body, const Headers &headers,
                               const QString &operation, const QString &envelope, Handler onData,
                               const QString &requestKey, const std::shared_ptr<Session> &candidate,
                               bool includeResponseMeta, std::optional<RequestPolicy> policyOverride)
{
    std::unique_ptr<QIODevice> ownedBody(body);
    const auto accountScope = m_backend.m_accountScopeRevision;
    const auto kind = parseOperation(operation).kind;
    const auto policy = policyOverride.value_or(defaultRequestPolicy(kind));
    const auto key = scopedRequestKey(operation, policy, requestKey);
    const bool historySnapshot = policy == RequestPolicy::HistorySnapshot;
    const bool latestHistory = historySnapshot || policy == RequestPolicy::HistoryRead;
    const auto historyRevision = (latestHistory && parseOperation(key).kind == OperationKind::TaskMessages
        ? m_taskHistoryRetry : m_chatHistoryRetry).revision;
    const auto failedOperation = latestHistory ? key : operation;
    const auto rejectRequest = [&](const QString &reason) {
        const bool optional = policy == RequestPolicy::Optional;
        QPointer<PortalHttp> alive(this);
        const auto detail = optional && onData.accepts ? onData.invalidMessage : reason;
        if (!optional || onData.accepts)
            emit failed(failedOperation, detail);
        if (alive && accountScope == m_backend.m_accountScopeRevision)
            onData.reject(detail, optional);
    };
    auto *buffer = qobject_cast<QBuffer *>(body);
    const auto revision = m_backend.m_sessionRevision;
    if (m_inflightOperations.contains(key)) {
        const auto pending = m_inflightOperations.value(key);
        if (buffer && pending && shareableRead(kind) && pending->scope == accountScope
            && pending->revision == revision && pending->policy == policy && pending->candidate == candidate
            && pending->operation == operation && pending->url == url && pending->headers == headers && pending->bufferedBody
            && *pending->bufferedBody == buffer->data() && pending->envelope == envelope
            && pending->includeResponseMeta == includeResponseMeta)
            pending->handlers.append(std::move(onData));
        return;
    }
    const auto &credentials = candidate ? *candidate : m_backend.m_session;
    if (m_backend.m_authExpired && !candidate) {
        rejectRequest(QStringLiteral("Вход в портал истёк. Обновите подключение."));
        return;
    }
    if (!sessionUsable(credentials, m_backend.m_allowLoopback) || credentials.origin != m_backend.m_session.origin
        || !url.userInfo().isEmpty() || url.hasFragment() || !OriginGuard(m_backend.m_session.origin).isSameOrigin(url)) {
        rejectRequest(QStringLiteral("Импортируйте действующую веб-сессию портала"));
        return;
    }
    if (atCapacity()) {
        rejectRequest(QStringLiteral("Дождитесь завершения текущих запросов"));
        if (kind == OperationKind::Push)
            m_backend.scheduleReconnect();
        return;
    }
    if (!body || !body->isReadable()) {
        rejectRequest(QStringLiteral("Не удалось прочитать данные запроса"));
        return;
    }
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    bool upload = false;
    for (const auto &[name, value] : headers) {
        const auto normalized = name.toLower();
        if ((normalized != "content-type" && normalized != "content-range"
             && !normalized.startsWith("x-upload-")) || !safeHeader(value)) {
            rejectRequest(QStringLiteral("Некорректные параметры запроса"));
            return;
        }
        upload = upload || normalized == "x-upload-content-name";
        request.setRawHeader(name, value);
    }
    request.setTransferTimeout(upload ? 60000 : 12000);
    request.setRawHeader("Cookie", credentials.cookie);
    request.setRawHeader("X-Bitrix-Csrf-Token", credentials.csrf);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("X-Requested-With", "XMLHttpRequest");
    request.setRawHeader("BX-Ajax", "true");
    const auto online = OriginGuard(m_backend.m_session.origin).resolve(QStringLiteral("/online/"));
    request.setRawHeader("Origin", online.toEncoded(QUrl::RemovePath));
    request.setRawHeader("Referer", online.toEncoded());

    auto *reply = m_backend.m_http.post(request, body);
    ownedBody.release()->setParent(reply);
    m_requests.insert(reply, key);
    auto pending = std::make_shared<InflightRequest>(InflightRequest{
        url, headers, buffer ? std::optional<QByteArray>(buffer->data()) : std::nullopt, operation, policy, envelope,
        accountScope, revision, candidate, includeResponseMeta, {std::move(onData)}});
    m_inflightOperations.insert(key, pending);
    if (kind == OperationKind::SessionRecovery)
        m_sessionRecoveryReply = reply;
    reply->setReadBufferSize(maxResponse + 1);
    auto *deadline = new QTimer(reply);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, reply, &QNetworkReply::abort);
    deadline->start(upload ? 300000 : 15000);
    connect(reply, &QIODevice::readyRead, this, [reply] {
        if (reply->bytesAvailable() > maxResponse)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, operation, kind, policy, key, envelope, revision, candidate, historySnapshot, latestHistory, historyRevision, failedOperation,
             accountScope, includeResponseMeta, pending] {
        m_requests.remove(reply);
        reply->deleteLater();
        if (kind == OperationKind::SessionRecovery) {
            if (m_sessionRecoveryReply != reply)
                return;
            m_sessionRecoveryReply = nullptr;
        }
        m_inflightOperations.remove(key, pending);
        if (!m_backend.m_started)
            return;
        if (accountScope != m_backend.m_accountScopeRevision) {
            // These operations leave controls busy until they receive a terminal signal.
            if (key == operation && policy == RequestPolicy::NotifyAccountChange)
                emit failed(operation, QStringLiteral("Аккаунт изменился. Результат запроса не подтверждён."));
            return;
        }
        m_backend.scheduleRefresh();
        if (!m_backend.isCurrentResponse(kind, key, operation))
            return;

        const QPointer<PortalHttp> alive(this);
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool redirect = status >= 300 && status < 400;
        const bool complete = reply->isOpen() && reply->bytesAvailable() <= maxResponse && !redirect;
        const auto document = complete ? QJsonDocument::fromJson(reply->readAll()) : QJsonDocument();
        const auto object = document.object();
        const auto location = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        const bool denied = signin::isAuthorizationRejection(status, object, reply->url(), location);
        const auto result = object.value(envelope);
        const auto errors = object.value(QStringLiteral("errors"));
        const bool ajaxRejected = envelope == QLatin1String("data")
            && (object.value(QStringLiteral("status")) != QLatin1String("success")
                || (!errors.isUndefined() && !errors.isNull() && (!errors.isArray() || !errors.toArray().isEmpty())));
        const bool failed = reply->error() != QNetworkReply::NoError || redirect || !document.isObject()
            || object.contains(QStringLiteral("error")) || ajaxRejected || result.isUndefined();

        if (failed) {
            const auto error = reply->error();
            const bool transientConnection = status == 0
                && (error == QNetworkReply::TimeoutError || error == QNetworkReply::OperationCanceledError
                    || error == QNetworkReply::RemoteHostClosedError || error == QNetworkReply::TemporaryNetworkFailureError
                    || error == QNetworkReply::ConnectionRefusedError || error == QNetworkReply::NetworkSessionFailedError);
            const bool temporary = transientConnection || (status >= 500 && status < 600);
            if (historySnapshot && !denied && !m_backend.m_authExpired
                && (temporary || (status >= 200 && status < 300))) {
                // The separate message list can still be usable if the full chat snapshot fails.
                pending->notify(this, {});
                return;
            }
            if (latestHistory && !denied && temporary && scheduleHistoryRetry(key, historyRevision, pending))
                return;
            if (kind == OperationKind::CsrfRefresh) {
                if (denied && revision == m_backend.m_sessionRevision)
                    m_backend.rejectSession();
                return;
            }
            // Some optional endpoints may deny access while the main routes are still usable.
            const bool lookup = policy == RequestPolicy::Optional;
            bool refreshing = false;
            if (denied && (!lookup || kind == OperationKind::SessionKeepalive)
                && !candidate && revision == m_backend.m_sessionRevision) {
                auto retry = signin::csrfRetrySession(m_backend.m_session, *reply, object);
                refreshing = retry.isValid() && m_backend.refreshCsrf(std::move(retry), revision);
                if (!refreshing) {
                    if (kind == OperationKind::SessionKeepalive) {
                        m_backend.rejectSession();
                    } else {
                        m_backend.confirmSession(revision);
                        refreshing = true;
                    }
                }
            }
            QString detail = refreshing ? QStringLiteral("Клиент проверяет вход в портал. Повторите действие после проверки")
                : denied && revision != m_backend.m_sessionRevision && !m_backend.m_authExpired
                    ? QStringLiteral("Параметры входа обновлены. Повторите действие")
                : denied ? QStringLiteral("Действие требует новой веб-сессии портала")
                : status == 403 ? QStringLiteral("Портал запретил действие")
                : status > 0 ? QStringLiteral("Портал отклонил действие или вернул другой формат (HTTP %1)").arg(status)
                : QStringLiteral("Ошибка сети или истекло время ожидания");
            detail += operationErrorHint(kind);
            const auto &first = pending->handlers.first();
            if (lookup && first.accepts)
                detail = first.invalidMessage;
            if (!lookup || first.accepts)
                emit this->failed(failedOperation, detail);
            if (alive)
                pending->notify(this, {}, detail);
            if (kind == OperationKind::Push && alive && !m_backend.m_authExpired)
                m_backend.scheduleReconnect();
            return;
        }
        if (candidate) {
            *candidate = signin::updatedSession(*candidate, *reply);
        } else if (revision == m_backend.m_sessionRevision) {
            const auto renewed = signin::updatedSession(m_backend.m_session, *reply);
            if (!renewed.isValid()) {
                m_backend.rejectSession();
            } else if (renewed.cookie != m_backend.m_session.cookie || renewed.csrf != m_backend.m_session.csrf) {
                m_backend.m_session = renewed;
                ++m_backend.m_sessionRevision;
                emit m_backend.sessionUpdated(m_backend.m_session);
            }
        }
        if (!alive || !m_backend.m_started || accountScope != m_backend.m_accountScopeRevision)
            return;
        const auto value = includeResponseMeta ? QJsonValue(object) : result;
        const auto &first = pending->handlers.first();
        if (first.accepts && !first.accepts(value)) {
            emit this->failed(failedOperation, first.invalidMessage);
            if (alive)
                pending->notify(this, {}, first.invalidMessage);
            return;
        }
        pending->notify(this, value);
    });
}

void PortalHttp::resetHistoryRetries()
{
    // Waiting reads have no live reply for the account reset to abort.
    m_inflightOperations.removeIf([](decltype(m_inflightOperations)::iterator it) {
        return it.value() && it.value()->waitingForHistoryRetry;
    });
    m_chatHistoryRetry = {};
    m_taskHistoryRetry = {};
}

bool PortalHttp::scheduleHistoryRetry(const QString &operation, quint64 revision,
                                      const std::shared_ptr<InflightRequest> &pending)
{
    const auto parsed = parseOperation(operation);
    const bool task = parsed.kind == OperationKind::TaskMessages;
    const auto key = parsed.ids.value(0);
    auto &attempt = task ? m_taskHistoryRetry : m_chatHistoryRetry;
    const auto dialog = attempt.dialog;
    const auto scope = m_backend.m_accountScopeRevision;
    const auto current = [this, task, key, dialog, revision, scope] {
        return m_backend.m_started && !m_backend.m_authExpired && scope == m_backend.m_accountScopeRevision
            && m_backend.isCurrentHistory(task, key, dialog)
            && (task ? m_taskHistoryRetry : m_chatHistoryRetry).revision == revision;
    };
    if (attempt.used || !current())
        return false;
    attempt.used = true;
    pending->waitingForHistoryRetry = true;
    m_inflightOperations.insert(operation, pending);
    QTimer::singleShot(350, this, [this, key, dialog, task, current, operation, pending] {
        m_inflightOperations.remove(operation, pending);
        if (!current()) {
            // A newer view may have deferred its refresh while this key was occupied.
            if (pending->scope == m_backend.m_accountScopeRevision)
                m_backend.scheduleRefresh();
            pending->notify(this, {}, QStringLiteral("Чтение истории больше не актуально"));
            return;
        }
        if (pending->handlers.size() == 1) {
            m_backend.loadMessages(key, dialog, task, true);
            return;
        }
        // Subscribers need the failed stage's response. A fresh chat snapshot could
        // contain messages inline and skip the message-list request they subscribed to.
        QUrlQuery form(QString::fromUtf8(*pending->bufferedBody));
        form.removeAllQueryItems(QStringLiteral("sessid"));
        Handler subscribers{
            [this, pending](QJsonValue result) { pending->notify(this, result); },
            [this, pending](const QString &error) { pending->notify(this, {}, error); }
        };
        // A replay must retain the effective policy of the request its subscribers joined.
        webRequest(pending->url, form.queryItems(QUrl::FullyDecoded), pending->operation,
                   pending->envelope, std::move(subscribers), operation, pending->candidate,
                   pending->includeResponseMeta, pending->policy);
    });
    return true;
}

void PortalHttp::cancelSessionRecovery()
{
    if (auto *reply = std::exchange(m_sessionRecoveryReply, nullptr)) {
        m_requests.remove(reply);
        m_inflightOperations.remove(operationKey(OperationKind::SessionRecovery));
        reply->abort();
    }
}

void PortalHttp::abortAll()
{
    const auto pending = m_requests.keys();
    for (auto *reply : pending)
        reply->abort();
}

void PortalHttp::reset(const Requests &pending)
{
    // abort() can synchronously notify consumers. Only cancel requests from before the reset.
    for (const auto &[reply, key] : pending.asKeyValueRange()) {
        // The pending clipboard upload owns the staged file, which a new upload may still open.
        // Its account-specific key allows the new request, and its stale response cannot advance the upload.
        if (m_backend.m_started && parseOperation(key).kind == OperationKind::ChatImageUpload)
            continue;
        if (m_requests.contains(reply))
            reply->abort();
    }
}

void PortalHttp::invalidateHistory(bool task)
{
    ++(task ? m_taskHistoryRetry : m_chatHistoryRetry).revision;
}

void PortalHttp::beginHistory(const QString &dialog, bool task, bool retry)
{
    auto &attempt = task ? m_taskHistoryRetry : m_chatHistoryRetry;
    attempt.dialog = dialog;
    ++attempt.revision;
    attempt.used = retry;
}

} // namespace bx
