#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QUrl>
#include <optional>

namespace bx::chromesignin {

class Frames {
public:
    bool append(const QByteArray &bytes);
    std::optional<QByteArray> take();
    void clear();

private:
    QByteArray m_pending;
};

QString ensureChromeProfile(const QString &applicationData);
QString csrfProbeExpression(const QUrl &portal);
QByteArray portalCookieHeader(const QUrl &portal, const QJsonArray &cookies);

} // namespace bx::chromesignin
