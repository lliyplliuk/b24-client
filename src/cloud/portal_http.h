#pragma once

#include "core/operations.h"
#include "core/session.h"

#include <QHash>
#include <QJsonValue>
#include <QObject>
#include <functional>
#include <memory>
#include <optional>

class QIODevice;
class QNetworkAccessManager;
class QNetworkReply;

namespace bx {

class SessionStateHost {
public:
    virtual ~SessionStateHost() = default;
    virtual quint64 sessionRevision() const = 0;
    virtual quint64 accountScopeRevision() const = 0;
    virtual bool started() const = 0;
    virtual bool authExpired() const = 0;
};

class PortalSession : public virtual SessionStateHost {
public:
    virtual const Session &currentSession() const = 0;
    virtual bool allowLoopback() const = 0;
    virtual bool isCurrentResponse(OperationKind kind, const QString &key, const QString &operation) const = 0;
    virtual bool isCurrentHistory(bool task, const QString &key, const QString &dialog) const = 0;
    virtual void rejectSession() = 0;
    virtual bool refreshCsrf(Session candidate, quint64 revision) = 0;
    virtual void scheduleRefresh() = 0;
    virtual void confirmPortalSession(quint64 revision) = 0;
    virtual void reconnectPortal() = 0;
    virtual void retryHistory(const QString &key, const QString &dialog, bool task) = 0;
    virtual void applyRenewedSession(const Session &renewed) = 0;
    virtual void portalRequestFailed(const QString &operation, const QString &detail) = 0;
};

class PortalHttp final : public QObject {
    Q_OBJECT

public:
    PortalHttp(PortalSession &session, QNetworkAccessManager &network, QObject *parent);
    using Fields = QList<QPair<QString, QString>>;
    using Headers = QList<QPair<QByteArray, QByteArray>>;
    struct Handler {
        std::function<void(QJsonValue)> onData;
        std::function<void(const QString &)> onError;

        QString invalidMessage;
        std::function<bool(const QJsonValue &)> accepts;

        Handler() = default;
        template<typename Callback>
        Handler(Callback callback, std::function<void(const QString &)> error = {})
            : onData(std::move(callback)), onError(std::move(error)) {}

        static Handler checked(QString message, std::function<bool(const QJsonValue &)> validate,
                               std::function<void(QJsonValue)> callback)
        {
            Handler handler(std::move(callback));
            handler.invalidMessage = std::move(message);
            handler.accepts = std::move(validate);
            return handler;
        }

        void reject(const QString &reason, bool emptyFallback) const
        {
            if (onError)
                onError(reason);
            else if (emptyFallback && !accepts)
                onData({});
        }
    };

    void ajax(const QString &action, Fields fields, const QString &operation, Handler onData,
              const QString &requestKey = {});
    void ajaxJson(const QString &action, const QJsonObject &body, const QString &operation, Handler onData,
                  const QString &requestKey = {});
    void ajaxJson(QUrl url, const QJsonObject &body, const QString &operation, Handler onData,
                  const QString &requestKey = {});
    void componentAction(const QString &action, const Fields &fields, const QString &operation, Handler onData,
                         const QString &component = QStringLiteral("bitrix:tasks.kanban"), const QString &requestKey = {});
    QUrl actionUrl(const QString &action) const;
    void fetchPortalHtml(const QUrl &url, std::function<void(QString)> handler);

    // An occupied key admits only matching subscribers to an idempotent buffered read.
    // Other duplicates, including mutations and uploads, are dropped without another POST.
    // Use distinct keys for distinct parameters that must run concurrently. A shared read
    // delivers data or a private error to each subscriber. Required reads emit failed once
    // per request. Optional lookups can deliver an empty fallback. Stopped backends and
    // stale account scopes discard replies, except for existing busy-control failures.
    void webRequest(QUrl url, const Fields &fields, const QString &operation,
                    const QString &envelope, Handler onData, const QString &requestKey = {},
                    const std::shared_ptr<Session> &candidate = {}, bool includeResponseMeta = false,
                    std::optional<RequestPolicy> policyOverride = {});
    void webRequestRaw(QUrl url, QIODevice *body, const Headers &headers, const QString &operation,
                       const QString &envelope, Handler onData, const QString &requestKey = {},
                       const std::shared_ptr<Session> &candidate = {}, bool includeResponseMeta = false,
                       std::optional<RequestPolicy> policyOverride = {});
    QString scopedRequestKey(const QString &operation, RequestPolicy policy, const QString &requestKey = {}) const;

    static bool sessionUsable(const Session &session, bool allowLoopback);
    static constexpr qsizetype maxResponse = 2 * 1024 * 1024;
    static constexpr qsizetype maxRequests = 12;
    bool atCapacity() const { return m_requests.size() >= maxRequests; }
    bool contains(const QString &key) const { return m_inflightOperations.contains(key); }
    void reserve(const QString &key) { m_inflightOperations.insert(key, nullptr); }
    void release(const QString &key) { m_inflightOperations.remove(key); }
    void track(QNetworkReply *reply) { m_requests.insert(reply, {}); }
    void untrack(QNetworkReply *reply) { m_requests.remove(reply); }

    using Requests = QHash<QNetworkReply *, QString>;
    Requests pendingRequests() const { return m_requests; }
    void reset(const Requests &pending);
    void abortAll();
    void cancelSessionRecovery();
    void resetHistoryRetries();
    void invalidateHistory(bool task);
    void beginHistory(const QString &dialog, bool task, bool retry);

private:
    PortalSession &m_session;
    QNetworkAccessManager &m_network;
    struct InflightRequest;
    bool scheduleHistoryRetry(const QString &operation, quint64 revision, const std::shared_ptr<InflightRequest> &pending);
    QHash<QNetworkReply *, QString> m_requests;
    QNetworkReply *m_sessionRecoveryReply = nullptr;
    QMultiHash<QString, std::shared_ptr<InflightRequest>> m_inflightOperations;
    struct HistoryRetry {
        QString dialog;
        quint64 revision = 0;
        bool used = false;
    };
    HistoryRetry m_chatHistoryRetry;
    HistoryRetry m_taskHistoryRetry;
};

} // namespace bx
