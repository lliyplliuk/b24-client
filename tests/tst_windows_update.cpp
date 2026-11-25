#include "app/windows_update.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace bx::update;

namespace {

QString assetName(const QString &tag)
{
    return QStringLiteral("bitrix24-client-%1-windows-x64.zip").arg(tag);
}

QString assetUrl(const QString &tag)
{
    return QStringLiteral("https://github.com/lliyplliuk/b24-client/releases/download/%1/%2")
        .arg(tag, assetName(tag));
}

QJsonObject release(const QString &tag)
{
    return {{QStringLiteral("tag_name"), tag}, {QStringLiteral("draft"), false},
            {QStringLiteral("prerelease"), false},
            {QStringLiteral("assets"), QJsonArray{QJsonObject{
                {QStringLiteral("name"), assetName(tag)}, {QStringLiteral("browser_download_url"), assetUrl(tag)},
                {QStringLiteral("state"), QStringLiteral("uploaded")}, {QStringLiteral("size"), 2048}}}}};
}

QByteArray json(const QJsonObject &obj)
{
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

} // namespace

class WindowsUpdateTest : public QObject {
    Q_OBJECT
private slots:
    void startupAndPeriodicChecksUseTheirOwnDestinations()
    {
        QStringList startup, periodic;
        QString tag = QStringLiteral("v1.2.3");
        WindowsUpdateChecker checker(QStringLiteral("1.0.0"),
            [&](const WindowsRelease &r) { startup.append(r.tag); },
            [&](const WindowsRelease &r) { periodic.append(r.tag); },
            [&](auto complete) { complete(json(release(tag))); });
        checker.start();
        QCOMPARE(startup, QStringList{tag});
        QVERIFY(periodic.isEmpty());
        auto *timer = checker.findChild<QTimer *>(QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(timer && timer->isActive());
        QCOMPARE(timer->interval(), 12 * 60 * 60 * 1000);
        QVERIFY(QMetaObject::invokeMethod(timer, "timeout"));
        QVERIFY(periodic.isEmpty());
        tag = QStringLiteral("v1.2.4");
        QVERIFY(QMetaObject::invokeMethod(timer, "timeout"));
        QCOMPARE(startup.size(), 1);
        QCOMPARE(periodic, QStringList{tag});
        QVERIFY(QMetaObject::invokeMethod(timer, "timeout"));
        QCOMPARE(periodic.size(), 1);
    }

    void readsExactPackageVersion_data()
    {
        QTest::addColumn<QByteArray>("marker");
        QTest::addColumn<QString>("expected");
        QTest::newRow("LF") << QByteArray("0.0.2\n") << QStringLiteral("0.0.2");
        QTest::newRow("CRLF with optional tag prefix") << QByteArray("v0.0.2\r\n") << QStringLiteral("v0.0.2");
        QTest::newRow("no final newline") << QByteArray("0.0.2") << QStringLiteral("0.0.2");
        QTest::newRow("development version") << QByteArray("0.1.0-pilot\n") << QString();
        QTest::newRow("multiple lines") << QByteArray("0.0.2\n0.0.3\n") << QString();
        QTest::newRow("trailing whitespace") << QByteArray("0.0.2 \n") << QString();
        QTest::newRow("oversized") << QByteArray(256, '0') << QString();
        QTest::newRow("invalid prefix") << QByteArray("V0.0.2\n") << QString();
    }

    void readsExactPackageVersion()
    {
        QFETCH(QByteArray, marker);
        QFETCH(QString, expected);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("bitrix24-version.txt"));
        QCOMPARE(readWindowsPackageVersion(path), QString());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(marker), marker.size());
        file.close();
        QCOMPARE(readWindowsPackageVersion(path), expected);
    }

    void comparesNumericStableVersions_data()
    {
        QTest::addColumn<QString>("local");
        QTest::addColumn<QString>("remote");
        QTest::addColumn<bool>("offer");
        QTest::newRow("patch is numeric") << QStringLiteral("0.0.9") << QStringLiteral("0.0.10") << true;
        QTest::newRow("minor is numeric") << QStringLiteral("v1.9.999") << QStringLiteral("1.10.0") << true;
        QTest::newRow("major is numeric") << QStringLiteral("9.21.0") << QStringLiteral("v10.0.0") << true;
        QTest::newRow("same with prefixed tag") << QStringLiteral("0.0.2") << QStringLiteral("v0.0.2") << false;
        QTest::newRow("older release") << QStringLiteral("0.0.10") << QStringLiteral("v0.0.9") << false;
        QTest::newRow("no package version") << QString() << QStringLiteral("0.0.2") << false;
        QTest::newRow("development") << QStringLiteral("0.1.0-pilot") << QStringLiteral("0.1.1") << false;
        QTest::newRow("rejected RC tag") << QStringLiteral("0.0.1") << QStringLiteral("v0.0.2-rc.1") << false;
        QTest::newRow("rejected leading zero") << QStringLiteral("0.0.1") << QStringLiteral("0.0.02") << false;
    }

    void comparesNumericStableVersions()
    {
        QFETCH(QString, local);
        QFETCH(QString, remote);
        QFETCH(bool, offer);
        const auto update = newerWindowsRelease(local, json(release(remote)));
        QCOMPARE(update.has_value(), offer);
        if (update) {
            QCOMPARE(update->tag, remote);
            QCOMPARE(update->downloadUrl.toString(), assetUrl(remote));
        }
    }

    void requiresOfficialAsset_data()
    {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QString>("value");
        QTest::newRow("lookalike host") << QStringLiteral("browser_download_url")
            << QStringLiteral("https://github.com.evil.example/lliyplliuk/b24-client/releases/download/0.0.2/bitrix24-client-0.0.2-windows-x64.zip");
        QTest::newRow("HTTP") << QStringLiteral("browser_download_url")
            << QString(assetUrl(QStringLiteral("0.0.2"))).replace(QStringLiteral("https:"), QStringLiteral("http:"));
        QTest::newRow("query") << QStringLiteral("browser_download_url")
            << assetUrl(QStringLiteral("0.0.2")) + QStringLiteral("?extra=1");
        QTest::newRow("other repo") << QStringLiteral("browser_download_url")
            << QString(assetUrl(QStringLiteral("0.0.2"))).replace(QStringLiteral("lliyplliuk"), QStringLiteral("someone"));
        QTest::newRow("other version") << QStringLiteral("browser_download_url") << assetUrl(QStringLiteral("0.0.3"));
        QTest::newRow("source archive name") << QStringLiteral("name") << QStringLiteral("Source code.zip");
        QTest::newRow("upload incomplete") << QStringLiteral("state") << QStringLiteral("starter");
    }

    void requiresOfficialAsset()
    {
        QFETCH(QString, field);
        QFETCH(QString, value);
        auto candidate = release(QStringLiteral("0.0.2"));
        auto asset = candidate.value(QStringLiteral("assets")).toArray().first().toObject();
        asset.insert(field, value);
        candidate.insert(QStringLiteral("assets"), QJsonArray{asset});
        QVERIFY(!newerWindowsRelease(QStringLiteral("0.0.1"), json(candidate)));
    }

    void ignoresUnpublishedAndInvalidResponses()
    {
        const auto installed = QStringLiteral("0.0.1");
        auto candidate = release(QStringLiteral("0.0.2"));
        candidate.insert(QStringLiteral("draft"), true);
        QVERIFY(!newerWindowsRelease(installed, json(candidate)));
        candidate.insert(QStringLiteral("draft"), false);
        candidate.insert(QStringLiteral("prerelease"), true);
        QVERIFY(!newerWindowsRelease(installed, json(candidate)));
        auto oversized = json(release(QStringLiteral("0.0.2")));
        oversized.append(QByteArray(256 * 1024, ' '));
        QVERIFY(!newerWindowsRelease(installed, oversized));
        QVERIFY(!newerWindowsRelease(installed, "{malformed"));
        QVERIFY(!newerWindowsRelease(installed, "[]"));
    }
};

QTEST_GUILESS_MAIN(WindowsUpdateTest)
#include "tst_windows_update.moc"
