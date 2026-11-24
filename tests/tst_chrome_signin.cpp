#include "chromesignin/chrome_signin_internal.h"
#include "chromesignin/chrome_signin.h"

#include <QJsonArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

#ifdef Q_OS_LINUX
#include <sys/stat.h>
#endif

using namespace bx::chromesignin;

class ChromeSigninTest : public QObject {
    Q_OBJECT
private slots:
    void passiveModeUsesOwnedProfileWithoutOpeningBrowserWindow()
    {
#ifdef Q_OS_LINUX
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto trace = fixture.filePath(QStringLiteral("args"));
        QFile mock(fixture.filePath(QStringLiteral("google-chrome-stable")));
        QVERIFY(mock.open(QIODevice::WriteOnly));
        QVERIFY(mock.write("#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$B24_TEST_CHROME_ARGS\"\nexit 42\n") > 0);
        mock.close();
        QVERIFY(mock.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));

        const QList<QByteArray> names{"PATH", "XDG_DATA_HOME", "B24_TEST_CHROME_ARGS"};
        const QList<QByteArray> previous{qgetenv("PATH"), qgetenv("XDG_DATA_HOME"), qgetenv("B24_TEST_CHROME_ARGS")};
        const auto restoreEnvironment = qScopeGuard([&] {
            for (qsizetype i = 0; i < names.size(); ++i) {
                if (previous.at(i).isNull())
                    qunsetenv(names.at(i));
                else
                    qputenv(names.at(i), previous.at(i));
            }
        });
        qputenv("PATH", QFile::encodeName(fixture.path()));
        qputenv("XDG_DATA_HOME", QFile::encodeName(fixture.path()));
        qputenv("B24_TEST_CHROME_ARGS", QFile::encodeName(trace));
        bx::ChromeSignin signin;
        QSignalSpy failed(&signin, &bx::ChromeSignin::failed);
        bool accepted = false;
        connect(&signin, &bx::ChromeSignin::sessionReady, this, [&](const bx::Session &) { accepted = true; });
        const QUrl portal(QStringLiteral("https://fixture.invalid"));

        const auto argsWritten = [&] {
            QFile args(trace);
            return args.open(QIODevice::ReadOnly) ? args.readAll().split('\n') : QList<QByteArray>{};
        };
        QVERIFY(signin.restoreInBackground(portal));
        QTRY_COMPARE(failed.size(), 1);
        auto args = argsWritten();
        QVERIFY(args.contains("--headless"));
        QVERIFY(!args.contains("--new-window"));
        QVERIFY(args.contains("--remote-debugging-pipe"));
        QVERIFY(std::any_of(args.cbegin(), args.cend(), [&](const QByteArray &argument) {
            return argument.startsWith("--user-data-dir=" + QFile::encodeName(fixture.path()) + '/')
                && argument.endsWith("/Chrome");
        }));
        QVERIFY(!accepted);

        QVERIFY(signin.start(portal));
        QTRY_COMPARE(failed.size(), 2);
        args = argsWritten();
        QVERIFY(args.contains("--new-window"));
        QVERIFY(!args.contains("--headless"));
#endif
    }

    void chromeProfileRemainsInAppData()
    {
#ifdef Q_OS_LINUX
        QTemporaryDir applicationData;
        QVERIFY(applicationData.isValid());
        const auto profile = ensureChromeProfile(applicationData.path());
        QCOMPARE(profile, applicationData.filePath(QStringLiteral("Chrome")));
        QFile marker(QDir(profile).filePath(QStringLiteral("fixture")));
        QVERIFY(marker.open(QIODevice::WriteOnly));
        marker.write("persistent");
        marker.close();
        QCOMPARE(ensureChromeProfile(applicationData.path()), profile);
        QVERIFY(marker.exists());
        const auto permissions = QFileInfo(profile).permissions();
        QVERIFY(!(permissions & (QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                                  QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                  QFileDevice::WriteOther | QFileDevice::ExeOther)));
        QVERIFY(ensureChromeProfile(QStringLiteral("relative")).isEmpty());
        QTemporaryDir unrelated;
        QVERIFY(unrelated.isValid());
        const auto alias = applicationData.filePath(QStringLiteral("symlink"));
        QVERIFY(QFile::link(unrelated.path(), alias));
        QVERIFY(ensureChromeProfile(alias + QLatin1Char('/')).isEmpty());
        QVERIFY(!QDir(unrelated.path()).exists(QStringLiteral("Chrome")));

        const auto firstRun = applicationData.filePath(QStringLiteral("first-run"));
        const auto previousMask = ::umask(0002);
        const auto firstProfile = ensureChromeProfile(firstRun);
        ::umask(previousMask);
        QCOMPARE(firstProfile, QDir(firstRun).filePath(QStringLiteral("Chrome")));
        QCOMPARE(QFileInfo(firstRun).permissions() & (QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                                                       QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                                       QFileDevice::WriteOther | QFileDevice::ExeOther),
                 QFileDevice::Permissions());
        QVERIFY(::chmod(QFile::encodeName(firstRun).constData(), 0775) == 0);
        QCOMPARE(ensureChromeProfile(firstRun), firstProfile);
        QVERIFY(!(QFileInfo(firstRun).permissions() & QFileDevice::WriteGroup));
#endif
    }

    void framesHandleFragmentedMessagesAndRejectExcess()
    {
        Frames frames;
        QVERIFY(frames.append(R"({"id":)"));
        QVERIFY(!frames.take().has_value());
        QVERIFY(frames.append(QByteArray("1}\0{\"id\":2}\0", 12)));
        QCOMPARE(frames.take().value_or(QByteArray()), QByteArray(R"({"id":1})"));
        QCOMPARE(frames.take().value_or(QByteArray()), QByteArray(R"({"id":2})"));
        QVERIFY(!frames.append(QByteArray(2 * 1024 * 1024 + 1, 'x')));
        QVERIFY(!frames.take().has_value());
    }

    void csrfExpressionChecksOriginFirst()
    {
        const auto expression = csrfProbeExpression(QUrl(QStringLiteral("https://portal.example")));
        QVERIFY(expression.contains(QStringLiteral("location.origin === \"https://portal.example\"")));
        QVERIFY(expression.indexOf(QStringLiteral("location.origin")) < expression.indexOf(QStringLiteral("bitrix_sessid")));
    }

    void cookieHeaderConfinesPortalAndRequests()
    {
        auto cookie = [](const QString &name, const QString &domain, const QString &path) {
            return QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("value"), QStringLiteral("test")},
                               {QStringLiteral("domain"), domain}, {QStringLiteral("path"), path}};
        };
        auto partitioned = cookie(QStringLiteral("partitioned"), QStringLiteral("portal.example"), QStringLiteral("/"));
        partitioned.insert(QStringLiteral("partitionKey"), QJsonObject{});
        const QJsonArray cookies{
            cookie(QStringLiteral("PHPSESSID"), QStringLiteral("portal.example"), QStringLiteral("/")),
            cookie(QStringLiteral("yandex"), QStringLiteral(".yandex.ru"), QStringLiteral("/")),
            cookie(QStringLiteral("ajaxOnly"), QStringLiteral("portal.example"), QStringLiteral("/bitrix")),
            cookie(QStringLiteral("subdomain"), QStringLiteral("login.portal.example"), QStringLiteral("/")),
            partitioned};
        QCOMPARE(portalCookieHeader(QUrl(QStringLiteral("https://portal.example")), cookies),
                 QByteArray("PHPSESSID=test"));
    }
};

QTEST_GUILESS_MAIN(ChromeSigninTest)
#include "tst_chrome_signin.moc"
