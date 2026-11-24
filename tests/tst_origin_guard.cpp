#include "core/origin_guard.h"

#include <QTest>

using namespace bx;

class TestOriginGuard : public QObject {
    Q_OBJECT
private slots:
    void sameOriginAllowed();
    void foreignOriginRejected();
    void plainHttpExternalRejected();
    void loopbackAllowedWhenExplicitlyEnabled();
    void resolveKeepsOrigin();
};

void TestOriginGuard::sameOriginAllowed() {
    OriginGuard guard(QUrl(QStringLiteral("https://acme.bitrix24.ru")));
    QVERIFY(guard.isSameOrigin(QUrl(QStringLiteral("https://acme.bitrix24.ru/bitrix/im/chat/recent/"))));
    QVERIFY(guard.isAllowed(QUrl(QStringLiteral("https://acme.bitrix24.ru:443/x"))));
    QVERIFY(!guard.isSameOrigin(QUrl(QStringLiteral("https://other.bitrix24.ru/x"))));
    QVERIFY(!guard.isSameOrigin(QUrl(QStringLiteral("http://acme.bitrix24.ru/x"))));
}

void TestOriginGuard::foreignOriginRejected() {
    OriginGuard guard(QUrl(QStringLiteral("https://acme.bitrix24.ru")));
    const QUrl foreign(QStringLiteral("https://collector.example.com/"));
    QVERIFY(!guard.isAllowed(foreign));
    QVERIFY(guard.rejectionReason(foreign).contains(QStringLiteral("origin")));
}

void TestOriginGuard::plainHttpExternalRejected() {
    OriginGuard guard(QUrl(QStringLiteral("https://acme.bitrix24.ru")));
    const QUrl plainHttp(QStringLiteral("http://acme.bitrix24.ru/x"));
    QVERIFY(!guard.isAllowed(plainHttp));
    QVERIFY(guard.rejectionReason(plainHttp).contains(QStringLiteral("HTTPS")));
}

void TestOriginGuard::loopbackAllowedWhenExplicitlyEnabled() {
    OriginGuard guard(QUrl(QStringLiteral("http://127.0.0.1:8080")));
    const QUrl loopback(QStringLiteral("http://127.0.0.1:8080/bitrix/im/chat/recent/"));
    QVERIFY(guard.isAllowed(loopback, true));                       // explicit flag
    // The explicit loopback flag is a test-only escape hatch for local fixtures.
    QVERIFY(guard.isAllowed(QUrl(QStringLiteral("http://127.0.0.1:9090/x")), true));
    // Without the explicit flag only the portal origin itself is reachable.
    OriginGuard external(QUrl(QStringLiteral("https://acme.bitrix24.ru")));
    QVERIFY(!external.isAllowed(QUrl(QStringLiteral("http://127.0.0.1:8080/x")), false));
}

void TestOriginGuard::resolveKeepsOrigin() {
    OriginGuard guard(QUrl(QStringLiteral("https://acme.bitrix24.ru")));
    const QUrl url = guard.resolve(QStringLiteral("/bitrix/groups/projects/"));
    QCOMPARE(url.host(), QStringLiteral("acme.bitrix24.ru"));
    QCOMPARE(url.path(), QStringLiteral("/bitrix/groups/projects/"));
    QCOMPARE(url.scheme(), QStringLiteral("https"));
}

QTEST_GUILESS_MAIN(TestOriginGuard)
#include "tst_origin_guard.moc"
