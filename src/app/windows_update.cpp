#include "app/windows_update.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>

#include <array>
#include <utility>

namespace bx::update {
namespace {

constexpr qsizetype markerLimit = 128;
constexpr qsizetype responseLimit = 256 * 1024;
constexpr int checkPeriodMs = 12 * 60 * 60 * 1000;
constexpr int deadlineMs = 10000;
using Version = std::array<quint64, 3>;

std::optional<Version> stableVersion(const QString &tag)
{
    static const QRegularExpression pattern(
        QStringLiteral("\\Av?(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\z"));
    const auto match = pattern.match(tag);
    if (!match.hasMatch())
        return std::nullopt;
    Version version{};
    for (int i = 0; i < 3; ++i) {
        bool ok = false;
        version[i] = match.capturedView(i + 1).toULongLong(&ok);
        if (!ok)
            return std::nullopt;
    }
    return version;
}

} // namespace

QString readWindowsPackageVersion(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    auto bytes = file.read(markerLimit + 1);
    if (bytes.size() > markerLimit)
        return {};
    if (bytes.endsWith("\r\n"))
        bytes.chop(2);
    else if (bytes.endsWith('\n'))
        bytes.chop(1);
    const auto tag = QString::fromLatin1(bytes);
    return stableVersion(tag) ? tag : QString();
}

std::optional<WindowsRelease> newerWindowsRelease(const QString &installedVersion, const QByteArray &response)
{
    const auto installed = stableVersion(installedVersion);
    if (!installed || response.size() > responseLimit)
        return std::nullopt;
    const auto release = QJsonDocument::fromJson(response).object();
    if (release.value(QStringLiteral("draft")) != QJsonValue(false)
        || release.value(QStringLiteral("prerelease")) != QJsonValue(false))
        return std::nullopt;
    const auto tag = release.value(QStringLiteral("tag_name")).toString();
    const auto available = stableVersion(tag);
    if (!available || *available <= *installed)
        return std::nullopt;

    const auto name = QStringLiteral("bitrix24-client-%1-windows-x64.zip").arg(tag);
    const auto url = QStringLiteral("https://github.com/lliyplliuk/b24-client/releases/download/%1/%2").arg(tag, name);
    for (const auto item : release.value(QStringLiteral("assets")).toArray()) {
        const auto asset = item.toObject();
        if (asset.value(QStringLiteral("name")).toString() == name
            && asset.value(QStringLiteral("browser_download_url")).toString() == url
            && asset.value(QStringLiteral("state")) == QLatin1String("uploaded")
            && asset.value(QStringLiteral("size")).toInteger() > 0)
            return WindowsRelease{tag, QUrl(url)};
    }
    return std::nullopt;
}

WindowsUpdateChecker::WindowsUpdateChecker(QString installedVersion, Notice startup, Notice periodic, Fetch fetch)
    : m_minimumVersion(std::move(installedVersion)), m_startup(std::move(startup)),
      m_periodic(std::move(periodic)), m_fetch(std::move(fetch)), m_period(this)
{
    m_network.setProxy(QNetworkProxy::NoProxy);
    m_period.setInterval(checkPeriodMs);
    m_period.setTimerType(Qt::VeryCoarseTimer);
    connect(&m_period, &QTimer::timeout, this, [this] { check(m_periodic); });
}

void WindowsUpdateChecker::start()
{
    if (m_period.isActive() || !stableVersion(m_minimumVersion) || !m_startup || !m_periodic)
        return;
    m_period.start();
    check(m_startup);
}

void WindowsUpdateChecker::check(const Notice &notify)
{
    const auto complete = [this, notify](const QByteArray &response) {
        if (const auto release = newerWindowsRelease(m_minimumVersion, response)) {
            m_minimumVersion = release->tag;
            notify(*release);
        }
    };
    if (m_fetch)
        m_fetch(complete);
    else
        fetchLatest(complete);
}

void WindowsUpdateChecker::fetchLatest(Response complete)
{
    if (m_pending)
        return;
    const auto endpoint = QUrl(QStringLiteral("https://api.github.com/repos/lliyplliuk/b24-client/releases/latest"));
    QNetworkRequest request(endpoint);
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("Accept-Encoding", "identity");
    request.setRawHeader("User-Agent", "bitrix24-client");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    auto *reply = m_network.get(request);
    m_pending = reply;
    reply->setReadBufferSize(responseLimit + 1);
    connect(reply, &QIODevice::readyRead, reply, [reply] {
        if (reply->bytesAvailable() > responseLimit)
            reply->abort();
    });
    QTimer::singleShot(deadlineMs, reply, [reply] {
        if (reply->isRunning())
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, complete = std::move(complete)] {
        m_pending.clear();
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError || reply->bytesAvailable() > responseLimit
            || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200)
            return;
        complete(reply->readAll());
    });
}

} // namespace bx::update
