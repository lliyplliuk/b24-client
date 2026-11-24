#pragma once

#include "core/session.h"

#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QTimer>

class QNetworkReply;
class QJsonObject;

namespace bx::signin {

enum class RestoreStatus { Authenticated, Rejected, Unavailable };

RestoreStatus restoreStatus(int status, const QByteArray &body, const QUrl &portal,
                            const QUrl &redirect = {});
bool isAuthorizationRejection(int status, const QJsonObject &response, const QUrl &portal,
                              const QUrl &redirect);
Session csrfRetrySession(const Session &current, const QNetworkReply &reply, const QJsonObject &response);

class SessionRestore final : public QObject {
    Q_OBJECT
public:
    explicit SessionRestore(QObject *parent = nullptr);
    ~SessionRestore() override;

    void verify(Session session, bool allowLoopback = false);
    void cancel();

signals:
    void verified(bx::signin::RestoreStatus status, const bx::Session &session);

private:
    void probe();
    void drain();

    QNetworkAccessManager m_network;
    QTimer m_deadline;
    QPointer<QNetworkReply> m_reply;
    Session m_session;
    QByteArray m_body;
    bool m_aborted = false;
    bool m_retriedCsrf = false;
};

} // namespace bx::signin
