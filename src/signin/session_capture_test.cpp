#include "signin/session_capture.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

namespace {

QNetworkCookie cookie(const QByteArray &name, const QByteArray &value, const QString &domain,
                      const QString &path = QStringLiteral("/"))
{
    QNetworkCookie result(name, value);
    result.setDomain(domain);
    result.setPath(path);
    result.setSecure(true);
    return result;
}

class SessionCaptureTest : public QObject {
    Q_OBJECT

private slots:
    void checksPortalAndIdentityOrigins()
    {
        using namespace bx::signin;
        const auto portal = portalOrigin(QUrl(QStringLiteral("https://portal.bitrix24.ru/online/?state=hidden")));
        QCOMPARE(portal.toString(), QStringLiteral("https://portal.bitrix24.ru"));
        QVERIFY(portalOrigin(QUrl(QStringLiteral("http://portal.bitrix24.ru"))).isEmpty());
        QVERIFY(portalOrigin(QUrl(QStringLiteral("https://name@portal.bitrix24.ru"))).isEmpty());
        QVERIFY(mayNavigate(portal, QUrl(QStringLiteral("https://auth2.bitrix24.net/oauth/authorize/"))));
        QVERIFY(!mayNavigate(portal, QUrl(QStringLiteral("https://auth2.bitrix24.net.evil.test/"))));
        QVERIFY(!mayNavigate(portal, QUrl(QStringLiteral("http://auth2.bitrix24.net/"))));
        QVERIFY(!mayNavigate(portal, QUrl(QStringLiteral("https://portal.bitrix24.ru:444/"))));
        QVERIFY(!mayNavigate(portal, QUrl(QStringLiteral("https://another.bitrix24.ru/"))));
    }

    void handsOffOnlyPortalCookiesUsableOnBothRoutes()
    {
        using namespace bx::signin;
        const QUrl portal(QStringLiteral("https://portal.bitrix24.ru"));
        CookieCapture captured(portal);
        const auto session = cookie("PHPSESSID", "mine", QStringLiteral("portal.bitrix24.ru"));
        captured.add(session);
        captured.add(cookie("common", "on", QStringLiteral(".bitrix24.ru")));
        captured.add(cookie("other", "no", QStringLiteral("auth2.bitrix24.net")));
        captured.add(cookie("not-a-suffix", "no", QStringLiteral(".24.ru")));
        captured.add(cookie("host-only", "no", QStringLiteral("bitrix24.ru")));
        captured.add(cookie("ajax-only", "no", QStringLiteral("portal.bitrix24.ru"), QStringLiteral("/bitrix")));
        captured.add(cookie("login-only", "no", QStringLiteral("portal.bitrix24.ru"), QStringLiteral("/online/")));
        captured.add(cookie("invalid", "one; injected=one", QStringLiteral("portal.bitrix24.ru")));
        auto expired = cookie("old", "no", QStringLiteral("portal.bitrix24.ru"));
        expired.setExpirationDate(QDateTime::currentDateTimeUtc().addSecs(-2));
        captured.add(expired);

        QCOMPARE(captured.header(), QByteArray("PHPSESSID=mine; common=on"));
        captured.add(cookie("PHPSESSID", "renewed", QStringLiteral("portal.bitrix24.ru")));
        captured.remove(session); // a stale removal should leave a newer value intact
        QCOMPARE(captured.header(), QByteArray("PHPSESSID=renewed; common=on"));
        captured.remove(cookie("PHPSESSID", "renewed", QStringLiteral("portal.bitrix24.ru")));
        QCOMPARE(captured.header(), QByteArray("common=on"));
    }

    void constructsReadOnlyOriginBoundProbe()
    {
        using namespace bx::signin;
        const QUrl portal(QStringLiteral("https://portal.bitrix24.ru"));
        const auto request = authorizationProbe(portal, "PHPSESSID=mine", QByteArray(32, 'a'));
        QCOMPARE(request.url().toString(),
                 QStringLiteral("https://portal.bitrix24.ru/bitrix/services/main/ajax.php?action=im.v2.Recent.load"));
        QCOMPARE(request.rawHeader("Origin"), QByteArray("https://portal.bitrix24.ru"));
        QCOMPARE(request.rawHeader("Cookie"), QByteArray("PHPSESSID=mine"));
        QCOMPARE(request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(),
                 static_cast<int>(QNetworkRequest::ManualRedirectPolicy));
        QVERIFY(validCsrf(QByteArray(32, 'a')));
        QVERIFY(!validCsrf(QByteArray(32, '\r')));
        QVERIFY(!validCsrf("guest"));
    }

    void requiresAuthenticatedResponseShape()
    {
        using bx::signin::authenticatedResponse;
        QVERIFY(authenticatedResponse(200, R"({"status":"success","data":{"recentItems":[]}})"));
        QVERIFY(!authenticatedResponse(200, R"({"status":"success","data":{"recentItems":"invalid"}})"));
        QVERIFY(!authenticatedResponse(200, R"({"status":"error","data":{"recentItems":[]}})"));
        QVERIFY(!authenticatedResponse(200, R"({"status":"success","data":{"recentItems":[]},"errors":[{"code":"x"}]})"));
        QVERIFY(!authenticatedResponse(302, R"({"status":"success","data":{"recentItems":[]}})"));
        QVERIFY(!authenticatedResponse(200, "<html>Sign in</html>"));
    }

    void encodesSinglePrivateProtocolLine()
    {
        using bx::signin::sessionLine;
        const auto line = sessionLine(QUrl(QStringLiteral("https://portal.bitrix24.ru")),
                                      "PHPSESSID=mine", QByteArray(32, 'a'));
        QCOMPARE(line.count('\n'), 1);
        const auto fields = QJsonDocument::fromJson(line).object();
        QCOMPARE(fields.value(QStringLiteral("origin")).toString(), QStringLiteral("https://portal.bitrix24.ru"));
        QCOMPARE(fields.value(QStringLiteral("cookie")).toString(), QStringLiteral("PHPSESSID=mine"));
        QCOMPARE(fields.value(QStringLiteral("csrf")).toString(), QString(32, QLatin1Char('a')));
        QCOMPARE(fields.size(), 3);
    }
};

} // namespace

QTEST_APPLESS_MAIN(SessionCaptureTest)
#include "session_capture_test.moc"
