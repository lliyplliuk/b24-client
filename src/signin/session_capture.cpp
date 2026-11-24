#include "signin/session_capture.h"

#include "core/origin_guard.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QUrlQuery>
#include <algorithm>
#include <utility>

namespace bx::signin {

namespace {

constexpr auto ajaxPath = "/bitrix/services/main/ajax.php";
constexpr auto pullPath = "/rest/pull.config.get.json";

bool hasAuthority(const QUrl &url)
{
    return url.isValid() && !url.host().isEmpty() && !url.authority().contains(QLatin1Char('@'))
        && url.port() != 0;
}

bool domainMatches(QString host, QString domain)
{
    host = host.toLower();
    domain = domain.toLower();
    if (!domain.startsWith(QLatin1Char('.')))
        return host == domain;

    domain.remove(0, 1);
    return !domain.isEmpty() && (host == domain || host.endsWith(QLatin1Char('.') + domain));
}

bool pathMatches(const QString &path, const QString &cookiePath)
{
    if (cookiePath.isEmpty() || !path.startsWith(cookiePath))
        return false;
    return path.size() == cookiePath.size() || cookiePath.endsWith(QLatin1Char('/'))
        || path.at(cookiePath.size()) == QLatin1Char('/');
}

bool cookieBytesAreSafe(const QNetworkCookie &cookie)
{
    const auto name = cookie.name();
    const QByteArray separators("()<>@,;:\\\"/[]?={} \t");
    if (name.isEmpty() || !std::all_of(name.cbegin(), name.cend(), [&separators](unsigned char c) {
        return c >= 33 && c <= 126 && !separators.contains(static_cast<char>(c));
    }))
        return false;

    auto value = cookie.value();
    if (value.size() >= 2 && value.startsWith('"') && value.endsWith('"'))
        value = value.mid(1, value.size() - 2);
    return std::all_of(value.cbegin(), value.cend(), [](unsigned char c) {
        return c >= 33 && c <= 126 && c != '"' && c != ',' && c != ';' && c != '\\';
    });
}

bool includeCookie(const QNetworkCookie &cookie, const QUrl &portal, QDateTime now)
{
    return (!cookie.isSecure() || portal.scheme() == QLatin1String("https"))
        && (cookie.isSessionCookie() || cookie.expirationDate() > now)
        && domainMatches(portal.host(), cookie.domain()) && cookieBytesAreSafe(cookie)
        // The desktop transport uses one header on the portal's AJAX and Pull routes.
        && pathMatches(QString::fromLatin1(ajaxPath), cookie.path())
        && pathMatches(QString::fromLatin1(pullPath), cookie.path());
}

} // namespace

QUrl portalOrigin(const QUrl &url)
{
    if (url.scheme() != QLatin1String("https") || !hasAuthority(url))
        return {};

    QUrl origin = url.adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment);
    if (origin.port() == 443)
        origin.setPort(-1);
    return origin;
}

bool mayNavigate(const QUrl &portal, const QUrl &destination)
{
    if (!hasAuthority(destination) || destination.scheme() != QLatin1String("https"))
        return false;
    if (OriginGuard(portal).isSameOrigin(destination))
        return true;
    if (destination.port(443) != 443)
        return false;

    const auto host = destination.host();
    return host == QLatin1String("auth2.bitrix24.net")
        || host == QLatin1String("auth.bitrix24.net")
        || host == QLatin1String("bitrix24.net")
        || host == QLatin1String("www.bitrix24.net")
        || host == QLatin1String("oauth.bitrix.info");
}

CookieCapture::CookieCapture(QUrl portal) : m_portal(std::move(portal)) {}

void CookieCapture::add(const QNetworkCookie &cookie)
{
    if (!includeCookie(cookie, m_portal, QDateTime::currentDateTimeUtc()))
        return;
    const auto existing = std::find_if(m_cookies.begin(), m_cookies.end(), [&cookie](const QNetworkCookie &old) {
        return old.hasSameIdentifier(cookie);
    });
    if (existing == m_cookies.end())
        m_cookies.append(cookie);
    else
        *existing = cookie;
}

void CookieCapture::remove(const QNetworkCookie &cookie)
{
    auto existing = std::find_if(m_cookies.begin(), m_cookies.end(), [&cookie](const QNetworkCookie &old) {
        return old.hasSameIdentifier(cookie) && old.value() == cookie.value();
    });
    if (existing != m_cookies.end())
        m_cookies.erase(existing);
}

QByteArray CookieCapture::header() const
{
    QByteArray result;
    auto eligible = m_cookies;
    const auto now = QDateTime::currentDateTimeUtc();
    std::stable_sort(eligible.begin(), eligible.end(), [](const QNetworkCookie &a, const QNetworkCookie &b) {
        return a.path().size() > b.path().size();
    });
    for (const auto &cookie : eligible) {
        if (!includeCookie(cookie, m_portal, now))
            continue;
        if (!result.isEmpty())
            result += "; ";
        result += cookie.name() + '=' + cookie.value();
        if (result.size() >= 16384)
            return {};
    }
    return result;
}

bool validCsrf(const QByteArray &csrf)
{
    return csrf.size() == 32 && std::all_of(csrf.cbegin(), csrf.cend(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

Session updatedSession(Session session, const QNetworkReply &reply)
{
    const auto csrf = reply.rawHeader("X-Bitrix-Csrf-Token");
    const bool refreshCsrf = validCsrf(csrf);
    const bool refreshCookies = reply.hasRawHeader("Set-Cookie");
    if (!refreshCsrf && !refreshCookies)
        return session;
    if (!OriginGuard(session.origin).isSameOrigin(reply.url()))
        return session;
    if (refreshCsrf)
        session.csrf = csrf;
    if (!refreshCookies)
        return session;

    auto pairs = session.cookie.split(';');
    bool changed = false;
    const auto now = QDateTime::currentDateTimeUtc();
    for (const auto &header : reply.rawHeaderPairs()) {
        if (header.first.compare("Set-Cookie", Qt::CaseInsensitive) != 0)
            continue;
        for (const auto &cookie : QNetworkCookie::parseCookies(header.second)) {
            // The persisted header holds only cookies that work on every portal route.
            const auto domain = cookie.domain().isEmpty() ? session.origin.host() : cookie.domain();
            if (cookie.path() != QLatin1String("/") || !domainMatches(session.origin.host(), domain)
                || !cookieBytesAreSafe(cookie)
                || (cookie.isSecure() && session.origin.scheme() != QLatin1String("https")))
                continue;
            const auto prefix = cookie.name() + '=';
            const auto match = [&prefix](const QByteArray &pair) { return pair.trimmed().startsWith(prefix); };
            const auto existing = std::find_if(pairs.begin(), pairs.end(), match);
            if (!cookie.isSessionCookie() && cookie.expirationDate() <= now) {
                if (existing == pairs.end())
                    continue;
                pairs.erase(std::remove_if(pairs.begin(), pairs.end(), match), pairs.end());
            } else if (existing == pairs.end()) {
                pairs.append(prefix + cookie.value());
            } else {
                if (existing->trimmed() == prefix + cookie.value())
                    continue;
                *existing = prefix + cookie.value();
            }
            changed = true;
        }
    }
    if (changed) {
        QByteArray cookie;
        for (const auto &pair : pairs) {
            if (!cookie.isEmpty())
                cookie += "; ";
            cookie += pair.trimmed();
        }
        if (cookie.size() < 16384)
            session.cookie = cookie;
    }
    return session;
}

QNetworkRequest authorizationProbe(const QUrl &origin, const QByteArray &cookie, const QByteArray &csrf)
{
    const OriginGuard guard(origin);
    auto url = guard.resolve(QString::fromLatin1(ajaxPath));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("action"), QStringLiteral("im.v2.Recent.load"));
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setTransferTimeout(10000);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded; charset=UTF-8"));
    request.setRawHeader("Cookie", cookie);
    request.setRawHeader("X-Bitrix-Csrf-Token", csrf);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("X-Requested-With", "XMLHttpRequest");
    request.setRawHeader("BX-Ajax", "true");
    request.setRawHeader("Origin", origin.toEncoded());
    request.setRawHeader("Referer", guard.resolve(QStringLiteral("/online/")).toEncoded());
    return request;
}

QByteArray authorizationProbeBody(const QByteArray &csrf)
{
    return QByteArray("sessid=") + csrf + "&filter%5BrecentSection%5D=default&limit=1";
}

bool authenticatedResponse(int status, const QByteArray &body)
{
    if (status != 200)
        return false;
    const auto parsed = QJsonDocument::fromJson(body).object();
    const auto errors = parsed.value(QStringLiteral("errors"));
    return parsed.value(QStringLiteral("status")) == QLatin1String("success")
        && (errors.isUndefined() || (errors.isArray() && errors.toArray().isEmpty()))
        && parsed.value(QStringLiteral("data")).toObject().value(QStringLiteral("recentItems")).isArray();
}

QByteArray sessionLine(const QUrl &portal, const QByteArray &cookie, const QByteArray &csrf)
{
    const QJsonObject session{
        {QStringLiteral("origin"), portal.toString(QUrl::FullyEncoded)},
        {QStringLiteral("cookie"), QString::fromLatin1(cookie)},
        {QStringLiteral("csrf"), QString::fromLatin1(csrf)}
    };
    return QJsonDocument(session).toJson(QJsonDocument::Compact) + '\n';
}

} // namespace bx::signin
