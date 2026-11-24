#include "signin/session_restore.h"
#include "signin/session_store.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace bx;
using namespace bx::signin;

class SessionRestoreTest final : public QObject {
    Q_OBJECT

private slots:
    void onlyRestoresExactCanonicalPortal()
    {
        const Session input{QUrl(QStringLiteral("https://Fixture.Example:443/online/")),
                            QByteArray("PHPSESSID=synthetic"), QByteArray(32, 'a')};
        const auto payload = storedSessionPayload(input);
        QVERIFY(!payload.isEmpty());
        QVERIFY(!payload.contains(input.cookie));
        const auto restored = sessionFromStoredPayload(payload, input.origin);
        QCOMPARE(restored.origin, QUrl(QStringLiteral("https://fixture.example")));
        QCOMPARE(restored.cookie, input.cookie);
        QCOMPARE(restored.csrf, input.csrf);
        for (const auto *other : {"http://fixture.example", "https://fixture.example:8443",
                                 "https://organization.fixture.example", "https://elsewhere.example"}) {
            QVERIFY(!sessionFromStoredPayload(payload, QUrl(QString::fromLatin1(other))).isValid());
        }
    }

    void rejectsMalformedPayload()
    {
        const QUrl portal(QStringLiteral("https://fixture.example"));
        QVERIFY(storedSessionPayload({portal, "x\nFoo: y", QByteArray(32, 'a')}).isEmpty());
        QVERIFY(storedSessionPayload({portal, "X=1", "not-a-valid-csrf"}).isEmpty());
        auto data = QJsonDocument::fromJson(storedSessionPayload({portal, "X=1", QByteArray(32, 'a')})).object();
        data.insert(QStringLiteral("version"), 2);
        QVERIFY(!sessionFromStoredPayload(QJsonDocument(data).toJson(), portal).isValid());
        data.insert(QStringLiteral("version"), 1);
        data.insert(QStringLiteral("cookie"), QStringLiteral("!@@"));
        QVERIFY(!sessionFromStoredPayload(QJsonDocument(data).toJson(), portal).isValid());
    }

    void distinguishesExpiryAndTemporaryErrors()
    {
        const QUrl portal(QStringLiteral("https://fixture.example"));
        QCOMPARE(restoreStatus(200, R"({"status":"success","data":{"recentItems":[]}})", portal),
                 RestoreStatus::Authenticated);
        QCOMPARE(restoreStatus(401, {}, portal), RestoreStatus::Rejected);
        QCOMPARE(restoreStatus(403, R"({"status":"error","errors":[{"code":"invalid_csrf"}]})", portal),
                 RestoreStatus::Rejected);
        QCOMPARE(restoreStatus(302, {}, portal, QUrl(QStringLiteral("https://auth2.bitrix24.net/login/"))),
                 RestoreStatus::Rejected);
        QCOMPARE(restoreStatus(302, {}, portal, QUrl(QStringLiteral("/auth/"))), RestoreStatus::Rejected);
        QCOMPARE(restoreStatus(302, {}, portal, QUrl(QStringLiteral("https://other.example/login/"))),
                 RestoreStatus::Unavailable);
        QCOMPARE(restoreStatus(503, {}, portal), RestoreStatus::Unavailable);
        QCOMPARE(restoreStatus(429, {}, portal), RestoreStatus::Unavailable);
        QCOMPARE(restoreStatus(0, {}, portal), RestoreStatus::Unavailable);
        QCOMPARE(restoreStatus(403, "<html>blocked</html>", portal), RestoreStatus::Unavailable);
        QCOMPARE(restoreStatus(200, "<html>login</html>", portal), RestoreStatus::Unavailable);
    }

    void logoutBlocksOnlySelectedOriginAcrossRestarts()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto file = directory.filePath(QStringLiteral("config.ini"));
        const QUrl first(QStringLiteral("https://portal.example"));
        const QUrl other(QStringLiteral("https://portal.example:8443"));
        {
            QSettings settings(file, QSettings::IniFormat);
            settings.setValue(QLatin1String(lastPortalPreferenceKey), first.toString());
            QVERIFY(blockSessionRestore(settings, QUrl(QStringLiteral("https://PORTAL.example:443/online/"))));
        }
        {
            QSettings afterRestart(file, QSettings::IniFormat);
            QVERIFY(isSessionRestoreBlocked(afterRestart, first));
            QVERIFY(!isSessionRestoreBlocked(afterRestart, other));
            QVERIFY(!afterRestart.contains(QLatin1String(lastPortalPreferenceKey)));
            afterRestart.setValue(QLatin1String(lastPortalPreferenceKey), other.toString());
            QVERIFY(blockSessionRestore(afterRestart, first));
            QCOMPARE(afterRestart.value(QLatin1String(lastPortalPreferenceKey)).toString(), other.toString());
            clearSessionRestoreBlock(afterRestart, first);
        }
        QSettings afterForget(file, QSettings::IniFormat);
        QVERIFY(!isSessionRestoreBlocked(afterForget, first));
        QCOMPARE(afterForget.value(QLatin1String(lastPortalPreferenceKey)).toString(), other.toString());
    }

    void privateKeyringRoundTrip()
    {
        if (!qEnvironmentVariableIsSet("B24_TEST_PRIVATE_KEYRING"))
            QSKIP("Run with B24_TEST_PRIVATE_KEYRING in a disposable Secret Service session.");
        const Session input{QUrl(QStringLiteral("https://restore-fixture.invalid")),
                            QByteArray("PHPSESSID=synthetic"), QByteArray(32, 'e')};
        SessionStore store;
        QSignalSpy saved(&store, &SessionStore::saved);
        QSignalSpy loaded(&store, &SessionStore::loaded);
        store.save(input);
        QTRY_COMPARE(saved.size(), 1);
        QVERIFY(saved.first().first().toBool());

        // A fresh store instance represents a subsequent connection attempt.
        SessionStore fresh;
        QSignalSpy recovered(&fresh, &SessionStore::loaded);
        fresh.load(input.origin);
        QTRY_COMPARE(recovered.size(), 1);
        const auto value = sessionFromStoredPayload(recovered.first().first().toByteArray(), input.origin);
        QCOMPARE(value.cookie, input.cookie);
        QCOMPARE(value.csrf, input.csrf);
        store.load(QUrl(QStringLiteral("https://restore-fixture.invalid:8443")));
        QTRY_COMPARE(loaded.size(), 1);
        QVERIFY(loaded.first().first().toByteArray().isEmpty());

        const Session other{QUrl(QStringLiteral("https://restore-fixture.invalid:8443")),
                            QByteArray("PHPSESSID=other-synthetic"), QByteArray(32, 'f')};
        store.save(other);
        QTRY_COMPARE(saved.size(), 2);
        QVERIFY(saved.last().first().toBool());

        QSignalSpy forgotten(&store, &SessionStore::forgotten);
        store.forget(QUrl(QStringLiteral("https://RESTORE-FIXTURE.invalid:443/online/")));
        QTRY_COMPARE(forgotten.size(), 1);
        QVERIFY(forgotten.last().first().toBool());

        fresh.load(input.origin);
        QTRY_COMPARE(recovered.size(), 2);
        QVERIFY(recovered.last().first().toByteArray().isEmpty());
        fresh.load(other.origin);
        QTRY_COMPARE(recovered.size(), 3);
        QCOMPARE(sessionFromStoredPayload(recovered.last().first().toByteArray(), other.origin).cookie, other.cookie);

        store.forget(input.origin);
        QTRY_COMPARE(forgotten.size(), 2);
        QVERIFY(forgotten.last().first().toBool());
    }
};

QTEST_GUILESS_MAIN(SessionRestoreTest)
#include "tst_session_restore.moc"
