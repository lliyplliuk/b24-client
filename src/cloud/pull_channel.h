#pragma once

#include "cloud/portal_http.h"

#include <QJsonObject>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <QWebSocket>

namespace bx {

class PullHost : public virtual SessionStateHost {
public:
    virtual QUrl portalOrigin() const = 0;
    virtual bool allowLoopback() const = 0;
};

class PullChannel final : public QObject {
    Q_OBJECT

public:
    PullChannel(PullHost &host, PortalHttp &http, QObject *parent);

    void connectPull();
    void scheduleReconnect(bool immediately = false);
    void reset();
    void resetBackoff();
    void cancelRetry();
    void stop();

signals:
    void connected();
    void offline();
    void failed(const QString &operation, const QString &detail);
    void taskEvent(const QJsonObject &fields, const QString &groupId, const QString &taskId, bool invalidatePreview);
    void presenceEvent(const QJsonObject &params, qint64 timestamp);
    void imEvent(const QString &command, const QJsonObject &params, const QJsonObject &extra, const QString &mid);

private:
    void handleTaskPull(const QString &command, const QJsonObject &params);
    void receivePull(const QString &frame);
    void handlePullEvent(const QJsonObject &event);

    PullHost &m_host;
    PortalHttp &m_portalHttp;
    QWebSocket m_socket;
    QTimer m_retry;
    QTimer m_watchdog;
    QTimer m_heartbeat;
    QTimer m_configExpiry;
    int m_attempt = 0;
    QQueue<QString> m_seenEvents;
    QSet<QString> m_seenEventIds;
    QString m_mid;
    QString m_tag;
    QString m_time;
};

} // namespace bx
