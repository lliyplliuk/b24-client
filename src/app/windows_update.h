#pragma once

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <functional>
#include <optional>

class QNetworkReply;

namespace bx::update {

struct WindowsRelease {
    QString tag;
    QUrl downloadUrl;
};

QString readWindowsPackageVersion(const QString &filePath);
std::optional<WindowsRelease> newerWindowsRelease(const QString &installedVersion, const QByteArray &response);

class WindowsUpdateChecker final : public QObject {
public:
    using Notice = std::function<void(const WindowsRelease &)>;
    using Response = std::function<void(const QByteArray &)>;
    using Fetch = std::function<void(Response)>;

    WindowsUpdateChecker(QString installedVersion, Notice startup, Notice periodic, Fetch fetch = {});
    void start();

private:
    void check(const Notice &notify);
    void fetchLatest(Response complete);

    QString m_minimumVersion;
    Notice m_startup;
    Notice m_periodic;
    Fetch m_fetch;
    QNetworkAccessManager m_network;
    QTimer m_period;
    QPointer<QNetworkReply> m_pending;
};

} // namespace bx::update
