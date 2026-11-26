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
class QNetworkReply;
class TestCloudBackend;

namespace bx {

class CloudBackend;

class PortalHttp final : public QObject {
    Q_OBJECT

public:
    explicit PortalHttp(CloudBackend &backend);
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
    bool atCapacity() const { return m_requests.size() >= 12; }
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

signals:
    void failed(const QString &operation, const QString &detail);

private:
    friend class ::TestCloudBackend;
    CloudBackend &m_backend;
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
