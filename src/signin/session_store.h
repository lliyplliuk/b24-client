#pragma once

#include "core/session.h"

#include <QObject>
#include <QTimer>
#include <memory>

class QSettings;

namespace bx::signin {

inline constexpr char lastPortalPreferenceKey[] = "connection/lastPortal";

bool blockSessionRestore(QSettings &settings, const QUrl &portal);
bool isSessionRestoreBlocked(QSettings &settings, const QUrl &portal);
void clearSessionRestoreBlock(QSettings &settings, const QUrl &portal);

QByteArray storedSessionPayload(const Session &session);
Session sessionFromStoredPayload(const QByteArray &payload, const QUrl &requestedPortal);

class SessionStore final : public QObject {
    Q_OBJECT
public:
    explicit SessionStore(QObject *parent = nullptr);
    ~SessionStore() override;

    void load(const QUrl &portal);
    void save(const Session &session);
    void forget(const QUrl &portal);
    void cancel();

signals:
    void loaded(const QByteArray &payload);
    void saved(bool persisted);
    void forgotten(bool removed);

private:
    enum class Operation { Load, Save, Forget };
    struct Job;
    void start(const QUrl &portal, Operation operation, QByteArray payload = {});
    void finish(const std::shared_ptr<Job> &job);
    void report(Operation operation, bool succeeded, const QByteArray &payload = {});

    std::shared_ptr<Job> m_job;
    QTimer m_deadline;
};

} // namespace bx::signin
