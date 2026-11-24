#pragma once

#include "core/session.h"

#include <QObject>
#include <memory>

namespace bx {

class ChromeSignin final : public QObject {
    Q_OBJECT
public:
    explicit ChromeSignin(QObject *parent = nullptr);
    ~ChromeSignin() override;

    bool start(const QUrl &portal);
    bool restoreInBackground(const QUrl &portal);
    void cancel();

    static constexpr int backgroundDeadlineMs = 12000;

signals:
    void sessionReady(const bx::Session &session);
    void failed(const QString &message);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace bx
