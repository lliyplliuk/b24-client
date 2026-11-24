#include "cloud/cloud_backend.h"

#include <QDateTime>
#include <QFile>
#include <QHostAddress>
#include <QNetworkReply>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>
#include <functional>

using namespace bx;
using namespace Qt::StringLiterals;

namespace bx::worktime {
QNetworkRequest networkRequest(const QUrl &portal, const QUrl &url, const QByteArray &cookie,
                               bool allowLoopback, bool exportDownload);
bool isExportWorkbook(const QUrl &url, const QByteArray &type, const QByteArray &attachment, const QByteArray &data);
}

namespace {

struct Request {
    QByteArray method;
    QUrl url;
    QUrlQuery form;
    QByteArray cookie;
    bool sensitiveHeaders = false;
};

struct Response {
    QByteArray body;
    QByteArray type = "application/json";
    QByteArray status = "200 OK";
    QByteArray headers;
};

class Fixture {
public:
    Fixture()
    {
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (auto *socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, bytes = QByteArray()]() mutable {
                    bytes += socket->readAll();
                    const auto end = bytes.indexOf("\r\n\r\n");
                    if (end < 0)
                        return;
                    const auto lines = bytes.left(end).split('\n');
                    const auto requestLine = lines.first().trimmed().split(' ');
                    QByteArray cookie;
                    int length = 0;
                    bool sensitiveHeaders = false;
                    for (const auto &raw : lines) {
                        const auto line = raw.trimmed();
                        const auto name = line.left(line.indexOf(':')).toLower();
                        sensitiveHeaders |= name == "cookie" || name == "authorization" || name == "referer"
                            || name == "origin" || name == "x-bitrix-csrf-token";
                        if (line.toLower().startsWith("content-length:"))
                            length = line.mid(15).trimmed().toInt();
                        if (line.toLower().startsWith("cookie:"))
                            cookie = line.mid(7).trimmed();
                    }
                    if (bytes.size() < end + 4 + length)
                        return;
                    Request req{requestLine.value(0), QUrl::fromEncoded(requestLine.value(1)),
                        QUrlQuery(QString::fromUtf8(bytes.mid(end + 4, length))), cookie, sensitiveHeaders};
                    requests.append(req);
                    const auto response = respond(req);
                    socket->write("HTTP/1.1 " + response.status + "\r\nConnection: close\r\nContent-Type: "
                                  + response.type + "\r\nContent-Length: " + QByteArray::number(response.body.size())
                                  + "\r\n" + response.headers + "\r\n" + response.body);
                    socket->disconnectFromHost();
                });
            }
        });
    }

    bool start() { return server.listen(QHostAddress::LocalHost); }
    Session session() const
    {
        return {QUrl(u"http://127.0.0.1:"_s + QString::number(server.serverPort())), "b24=fake", "fake-csrf"};
    }

    QList<Request> requests;
    std::function<Response(const Request &)> respond;
private:
    QTcpServer server;
};

QByteArray status(const QByteArray &state, const QByteArray &report = "A")
{
    return "{'ID':'91','STATE':'" + state + "','CAN_OPEN':'OPEN','REPORT_REQ':'" + report
        + "','TM_FREE':false,'INFO':{'DATE_START':'1769558400','DATE_FINISH':1769559000,'TIME_LEAKS':100}}";
}

QByteArray sheet(const QDate &month)
{
    QByteArray html = "<html><div class='timeman-report-container'><table class='main-grid-table'><tr>";
    for (int day = 1; day <= month.daysInMonth(); ++day)
        html += "<th data-name='" + month.addDays(day - 1).toString(Qt::ISODate).toLatin1() + "'></th>";
    html += "</tr><tr class='main-grid-row main-grid-row-body'>"
        "<td data-column-id='USER_NAME'><a class='timeman-grid-user-name'>User &amp; Partner</a></td><td data-column-id='"
        + month.toString(Qt::ISODate).toLatin1()
        + "'><span class='timeman-grid-worktime-duration-value'>07:35</span> <span>09:00–17:00</span></td></tr>"
        "</table></div><script>var exportManager=new BX.Timeman.Export({'componentName':'bitrix:timeman.worktime.export.excel',"
        "'siteId':'s1','stExportId':'test','managerId':'test2','sToken':'opaque','signedParameters':'signed.123'});"
        "</script></html>";
    return html;
}

} // namespace

class TestCloudWorktime : public QObject {
    Q_OBJECT
private slots:
    void readsAndChangesWorkdayByOfficialActions();
    void changesSelectedTimeAndKeepsReasonSeparateFromDailyReport();
    void customTimeRespectsRightsAndRepairsExpiredDay();
    void usesServerDisplayStartAndConfirmedDuration();
    void refusesUnknownAndHandlesPortalErrors();
    void checksCanonicalSessionBeforeRejectingLegacyExpiry_data();
    void checksCanonicalSessionBeforeRejectingLegacyExpiry();
    void readsExactMonthAndDownloadsExcel();
    void rejectsOffOriginWorkbook();
    void downloadsOfficialCdnWorkbookWithoutWebSessionHeaders();
};

void TestCloudWorktime::readsAndChangesWorkdayByOfficialActions()
{
    Fixture fixture;
    QList<Request> timeman;
    fixture.respond = [&](const Request &request) -> Response {
        if (request.url.path() != u"/bitrix/tools/timeman.php")
            return {"{}", "text/html"};
        timeman.append(request);
        const auto action = QUrlQuery(request.url).queryItemValue(u"action"_s);
        if (action == u"update")
            return {status("CLOSED")};
        if (action == u"pause")
            return {status("PAUSED", "Y")};
        if (action == u"open" || action == u"reopen")
            return {status("OPENED", "Y")};
        return {status("CLOSED")};
    };
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QVector<WorkdayStatus> loaded;
    QVector<WorkdayStatus> changed;
    QStringList failures;
    connect(&backend, &Backend::workdayLoaded, this, [&](const WorkdayStatus &s) { loaded.append(s); });
    connect(&backend, &Backend::workdayChanged, this, [&](const WorkdayStatus &s) { changed.append(s); });
    connect(&backend, &Backend::failed, this, [&](const QString &op, const QString &) {
        if (op.startsWith(u"workday"_s))
            failures.append(op);
    });
    backend.start();
    backend.requestWorkday();
    QTRY_COMPARE(loaded.size(), 1);
    QCOMPARE(loaded.last().state, WorkdayState::Closed);
    QVERIFY(loaded.last().canStart);
    backend.changeWorkday(WorkdayAction::Start);
    QTRY_COMPARE(changed.size(), 1);
    QCOMPARE(changed.last().state, WorkdayState::Running);
    QVERIFY(changed.last().canPause);
    backend.changeWorkday(WorkdayAction::Pause);
    QTRY_COMPARE(changed.size(), 2);
    QCOMPARE(changed.last().state, WorkdayState::Paused);
    QCOMPARE(changed.last().elapsedSeconds, 500);
    backend.changeWorkday(WorkdayAction::Resume);
    QTRY_COMPARE(changed.size(), 3);
    QVERIFY(changed.last().requiresReport);
    backend.changeWorkday(WorkdayAction::Finish);
    QCOMPARE(failures, QStringList{u"workday-change"_s});
    QCOMPARE(timeman.size(), 4);
    backend.changeWorkday(WorkdayAction::Finish, u"  Line & note  "_s);
    QTRY_COMPARE(changed.size(), 4);
    QCOMPARE(changed.last().state, WorkdayState::Closed);
    QCOMPARE(timeman.size(), 5);

    const QStringList expected{u"update"_s, u"open"_s, u"pause"_s, u"reopen"_s, u"close"_s};
    for (qsizetype i = 0; i < timeman.size(); ++i) {
        const auto &request = timeman.at(i);
        const QUrlQuery query(request.url);
        QCOMPARE(request.method, "POST");
        QCOMPARE(request.cookie, "b24=fake");
        QCOMPARE(query.queryItemValue(u"action"_s), expected.at(i));
        QCOMPARE(query.queryItemValue(u"sessid"_s), u"fake-csrf"_s);
        QCOMPARE(query.queryItemValue(u"site_id"_s), u"s1"_s);
        QCOMPARE(request.form.queryItemValue(u"device"_s), u"browser"_s);
        if (i > 0)
            QCOMPARE(request.form.queryItemValue(u"recordId"_s), u"91"_s);
    }
    QCOMPARE(timeman.at(1).form.queryItemValue(u"timestamp"_s), u"0"_s);
    QCOMPARE(timeman.at(3).form.queryItemValue(u"newActionName"_s), u"continue"_s);
    QCOMPARE(timeman.at(4).form.queryItemValue(u"ready"_s), u"Y"_s);
    QCOMPARE(timeman.at(4).form.queryItemValue(u"REPORT"_s, QUrl::FullyDecoded), u"Line & note"_s);
}

void TestCloudWorktime::changesSelectedTimeAndKeepsReasonSeparateFromDailyReport()
{
    Fixture fixture;
    QList<Request> writes;
    fixture.respond = [&](const Request &request) -> Response {
        if (request.url.path() != u"/bitrix/tools/timeman.php")
            return {"{}", "text/html"};
        const auto action = QUrlQuery(request.url).queryItemValue(u"action"_s);
        if (action == u"update")
            return {"{'STATE':'CLOSED','CAN_OPEN':'OPEN','CAN_EDIT':'Y','REPORT_REQ':'A','TM_FREE':false}"};
        writes.append(request);
        if (action == u"open")
            return {"{'ID':'91','STATE':'OPENED','CAN_EDIT':'Y','REPORT_REQ':'Y','TM_FREE':false,'INFO':{}}"};
        return {"{'STATE':'CLOSED','CAN_OPEN':'REOPEN','CAN_EDIT':'Y','REPORT_REQ':'Y',"
            "'INFO':{'DURATION':'420','TIME_LEAKS':'180'}}"};
    };
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QVector<WorkdayStatus> loaded;
    QVector<WorkdayStatus> changed;
    QStringList failures;
    connect(&backend, &Backend::workdayLoaded, this, [&](const WorkdayStatus &s) { loaded.append(s); });
    connect(&backend, &Backend::workdayChanged, this, [&](const WorkdayStatus &s) { changed.append(s); });
    connect(&backend, &Backend::failed, this, [&](const QString &op, const QString &) {
        if (op == u"workday-change")
            failures.append(op);
    });
    backend.start();
    backend.requestWorkday();
    QTRY_COMPARE(loaded.size(), 1);
    QVERIFY(loaded.first().canStartAt);
    backend.changeWorkdayAt(WorkdayAction::Start, QDate(2026, 4, 15), QTime(8, 10), u" "_s);
    QCOMPARE(failures.size(), 1);
    QCOMPARE(writes.size(), 0);
    backend.changeWorkdayAt(WorkdayAction::Start, QDate(2026, 4, 15), QTime(8, 10), u" Reason & start "_s);
    QTRY_COMPARE(changed.size(), 1);
    QVERIFY(changed.last().canFinishAt);
    backend.changeWorkdayAt(WorkdayAction::Finish, QDate(2026, 4, 15), QTime(16, 20), u"Reason & finish"_s);
    QCOMPARE(failures.size(), 2);
    QCOMPARE(writes.size(), 1);
    backend.changeWorkdayAt(WorkdayAction::Finish, QDate(2026, 4, 15), QTime(16, 20),
                            u"Reason & finish"_s, u"Daily report"_s);
    QTRY_COMPARE(changed.size(), 2);
    QCOMPARE(changed.last().elapsedSeconds, 420);
    QCOMPARE(writes.size(), 2);
    const auto start = writes.at(0).form;
    QCOMPARE(start.queryItemValue(u"timestamp"_s), u"29400"_s);
    QCOMPARE(start.queryItemValue(u"customUserDate"_s), u"15.04.2026"_s);
    QCOMPARE(start.queryItemValue(u"report"_s, QUrl::FullyDecoded), u"Reason & start"_s);
    const auto finish = writes.at(1).form;
    QCOMPARE(finish.queryItemValue(u"ready"_s), u"Y"_s);
    QCOMPARE(finish.queryItemValue(u"timeman_edit_to"_s), u"58800"_s);
    QCOMPARE(finish.queryItemValue(u"endUserDate"_s), u"15.04.2026"_s);
    QCOMPARE(finish.queryItemValue(u"recordId"_s), u"91"_s);
    QCOMPARE(finish.queryItemValue(u"REPORT"_s, QUrl::FullyDecoded), u"Daily report"_s);
    QCOMPARE(finish.queryItemValue(u"report"_s, QUrl::FullyDecoded), u"Reason & finish"_s);
    QVERIFY(!finish.hasQueryItem(u"timestamp"_s));
}

void TestCloudWorktime::customTimeRespectsRightsAndRepairsExpiredDay()
{
    Fixture fixture;
    int reads = 0;
    QList<Request> writes;
    const auto today = QDate::currentDate();
    const auto beforeToday = QDateTime(today.addDays(-1), QTime(12, 0)).toSecsSinceEpoch();
    fixture.respond = [&](const Request &request) -> Response {
        if (request.url.path() != u"/bitrix/tools/timeman.php")
            return {"{}", "text/html"};
        if (QUrlQuery(request.url).queryItemValue(u"action"_s) == u"update") {
            if (++reads == 1)
                return {"{'STATE':'CLOSED','CAN_OPEN':'OPEN','CAN_EDIT':'N','REPORT_REQ':'A','TM_FREE':true}"};
            if (reads == 2)
                return {"{'ID':'91','STATE':'CLOSED','CAN_OPEN':'OPEN','CAN_EDIT':'N','REPORT_REQ':'A',"
                    "'INFO':{'DATE_START':" + QByteArray::number(beforeToday) + "}}"};
            if (reads == 3)
                return {"{'ID':'91','STATE':'OPENED','CAN_EDIT':'N','REPORT_REQ':'A','TM_FREE':false}"};
            return {"{'RECORD_ID':91,'STATE':'EXPIRED','CAN_EDIT':'N','REPORT_REQ':'A','TM_FREE':false,"
                "'EXPIRED_DATE':'0','INFO':{'TIME_FINISH':64800}}"};
        }
        writes.append(request);
        return {status("CLOSED")};
    };
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QVector<WorkdayStatus> loaded;
    QVector<WorkdayStatus> changed;
    connect(&backend, &Backend::workdayLoaded, this, [&](const WorkdayStatus &s) { loaded.append(s); });
    connect(&backend, &Backend::workdayChanged, this, [&](const WorkdayStatus &s) { changed.append(s); });
    backend.start();
    backend.requestWorkday();
    QTRY_COMPARE(loaded.size(), 1);
    QVERIFY(loaded.last().canStartAt);
    QVERIFY(!loaded.last().requiresTimeReason);
    backend.requestWorkday();
    QTRY_COMPARE(loaded.size(), 2);
    QVERIFY(loaded.last().canStartAt);
    QCOMPARE(loaded.last().startDate, today);
    backend.requestWorkday();
    QTRY_COMPARE(loaded.size(), 3);
    QVERIFY(loaded.last().canFinish);
    QVERIFY(!loaded.last().canFinishAt);
    backend.changeWorkdayAt(WorkdayAction::Finish, QDate(2026, 4, 15), QTime(18, 5), u"Denied"_s);
    QCOMPARE(writes.size(), 0);
    backend.requestWorkday();
    QTRY_COMPARE(loaded.size(), 4);
    QVERIFY(loaded.last().canFinishAt);
    QVERIFY(!loaded.last().canFinish);
    QCOMPARE(loaded.last().suggestedFinishTime, QTime(0, 0));
    backend.changeWorkdayAt(WorkdayAction::Finish, QDate(2026, 4, 15), QTime(0, 0), u"Midnight"_s);
    QCOMPARE(writes.size(), 0);
    backend.changeWorkdayAt(WorkdayAction::Finish, QDate(2026, 4, 15), QTime(18, 5), u"End explanation"_s);
    QTRY_COMPARE(changed.size(), 1);
    QCOMPARE(writes.size(), 1);
    const auto finish = writes.first().form;
    QCOMPARE(finish.queryItemValue(u"timestamp"_s), u"65100"_s);
    QCOMPARE(finish.queryItemValue(u"customUserDate"_s), u"15.04.2026"_s);
    QCOMPARE(finish.queryItemValue(u"report"_s, QUrl::FullyDecoded), u"End explanation"_s);
    QCOMPARE(finish.queryItemValue(u"recordId"_s), u"91"_s);
}

void TestCloudWorktime::usesServerDisplayStartAndConfirmedDuration()
{
    Fixture fixture;
    int reads = 0;
    const auto now = QDateTime::currentSecsSinceEpoch();
    fixture.respond = [&](const Request &request) -> Response {
        if (request.url.path() != u"/bitrix/tools/timeman.php")
            return {"{}", "text/html"};
        if (++reads == 1) {
            return {"{'STATE':'OPENED','INFO':{'DATE_START':" + QByteArray::number(now - 1800)
                + ",'DISPLAY_START_TIMESTAMP':" + QByteArray::number(now - 800) + ",'TIME_LEAKS':'200'}}"};
        }
        return {"{'STATE':'PAUSED','INFO':{'DATE_START':" + QByteArray::number(now - 1800)
            + ",'DATE_FINISH':" + QByteArray::number(now - 400) + ",'TIME_LEAKS':'100','DURATION':'0'}}"};
    };
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QVector<WorkdayStatus> loaded;
    connect(&backend, &Backend::workdayLoaded, this, [&](const WorkdayStatus &s) { loaded.append(s); });
    backend.start();
    backend.requestWorkday();
    QTRY_COMPARE(loaded.size(), 1);
    QVERIFY(loaded.last().elapsedSeconds >= 600 && loaded.last().elapsedSeconds < 610);
    backend.requestWorkday();
    QTRY_COMPARE(loaded.size(), 2);
    QCOMPARE(loaded.last().elapsedSeconds, 0);
}

void TestCloudWorktime::refusesUnknownAndHandlesPortalErrors()
{
    Fixture fixture;
    int received = 0;
    fixture.respond = [&](const Request &request) -> Response {
        if (request.url.path() != u"/bitrix/tools/timeman.php")
            return {"{}", "text/html"};
        ++received;
        return {"{'type':'deny_query','error':'<b>No access</b>','error_id':'USER_RESTRICTION'}"};
    };
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    QVector<WorkdayStatus> loaded;
    QStringList failures;
    connect(&backend, &Backend::workdayLoaded, this, [&](const WorkdayStatus &s) { loaded.append(s); });
    connect(&backend, &Backend::failed, this, [&](const QString &op, const QString &) {
        if (op.startsWith(u"workday"_s))
            failures.append(op);
    });
    backend.start();
    backend.changeWorkday(WorkdayAction::Start);
    QCOMPARE(received, 0);
    QCOMPARE(failures.size(), 1);
    backend.requestWorkday();
    QTRY_COMPARE(loaded.size(), 1);
    QCOMPARE(loaded.last().state, WorkdayState::Unavailable);
    QCOMPARE(loaded.last().details, u"No access"_s);
    backend.changeWorkday(WorkdayAction::Start);
    QCOMPARE(received, 1);
    QCOMPARE(failures.size(), 2);
}

void TestCloudWorktime::checksCanonicalSessionBeforeRejectingLegacyExpiry_data()
{
    QTest::addColumn<QString>("decision");
    QTest::newRow("valid") << u"valid"_s;
    QTest::newRow("expired") << u"expired"_s;
    QTest::newRow("unavailable") << u"unavailable"_s;
    QTest::newRow("rotated-csrf") << u"renew"_s;
    QTest::newRow("rotated-cookie") << u"cookie"_s;
}

void TestCloudWorktime::checksCanonicalSessionBeforeRejectingLegacyExpiry()
{
    QFETCH(QString, decision);
    Fixture fixture;
    int workdayRequests = 0;
    int canonicalRequests = 0;
    const auto fresh = QByteArray(32, 'b');
    const auto renewedCookie = QByteArray("b24=renewed");
    fixture.respond = [&](const Request &request) -> Response {
        const QUrlQuery query(request.url);
        if (request.url.path() == u"/bitrix/tools/timeman.php") {
            ++workdayRequests;
            if (workdayRequests == 1 || (decision == u"renew" && query.queryItemValue(u"sessid"_s).toLatin1() != fresh)
                || (decision == u"cookie" && renewedCookie != request.cookie))
                return {"Ваша сессия истекла, повторите попытку авторизации.", "text/html"};
            return {status("CLOSED")};
        }
        if (query.queryItemValue(u"action"_s) == u"im.v2.Recent.load") {
            ++canonicalRequests;
            if (decision == u"expired")
                return {"{}", "application/json", "401 Unauthorized"};
            if (decision == u"unavailable")
                return {"{}", "application/json", "503 Service Unavailable"};
            if (decision == u"renew" && request.form.queryItemValue(u"sessid"_s).toLatin1() != fresh)
                return {R"({"status":"error","errors":[{"code":"invalid_csrf","customData":{"csrf":")"
                    + fresh + R"("}}]})"};
            if (decision == u"cookie")
                return {R"({"status":"success","data":{"recentItems":[]}})", "application/json", "200 OK",
                        "Set-Cookie: " + renewedCookie + "; Path=/; HttpOnly\r\n"};
            return {R"({"status":"success","data":{"recentItems":[]}})"};
        }
        return {"{}", "text/html"};
    };
    QVERIFY(fixture.start());

    CloudBackend backend(fixture.session(), true);
    int rejected = 0;
    int workdayFailed = 0;
    Session saved;
    connect(&backend, &CloudBackend::sessionRejected, this, [&] { ++rejected; });
    connect(&backend, &CloudBackend::sessionUpdated, this, [&](const Session &session) { saved = session; });
    connect(&backend, &Backend::failed, this, [&](const QString &operation, const QString &) {
        workdayFailed += operation == u"workday";
    });
    QVector<WorkdayStatus> statuses;
    connect(&backend, &Backend::workdayLoaded, this, [&](const WorkdayStatus &status) { statuses.append(status); });
    backend.start();
    backend.requestWorkday();
    QTRY_VERIFY_WITH_TIMEOUT(canonicalRequests > 0, 1000);
    QTRY_COMPARE(canonicalRequests, decision == u"renew" ? 2 : 1);
    if (decision == u"renew" || decision == u"cookie") {
        if (decision == u"renew")
            QTRY_COMPARE(saved.csrf, fresh);
        else
            QTRY_COMPARE(saved.cookie, renewedCookie);
        QTRY_COMPARE(statuses.size(), 1);
        QCOMPARE(workdayRequests, 2);
        QCOMPARE(workdayFailed, 0);
        QCOMPARE(rejected, 0);
        return;
    }
    QTRY_COMPARE(workdayFailed, 1);
    if (decision == u"expired") {
        QTRY_COMPARE(rejected, 1);
        QCOMPARE(workdayRequests, 1);
        return;
    }
    backend.requestWorkday();
    QTRY_COMPARE(statuses.size(), 1);
    QCOMPARE(workdayRequests, 2);
    QCOMPARE(rejected, 0);
}

void TestCloudWorktime::readsExactMonthAndDownloadsExcel()
{
    Fixture fixture;
    const QDate month(2026, 2, 1);
    const auto workbook = QByteArray::fromHex("d0cf11e0a1b11ae1") + "workbook";
    QList<Request> grids;
    QList<Request> jobs;
    QList<Request> downloads;
    fixture.respond = [&](const Request &request) -> Response {
        if (request.url.path() == u"/timeman/timeman.php") {
            grids.append(request);
            return {sheet(month), "text/html"};
        }
        if (QUrlQuery(request.url).queryItemValue(u"action"_s) == u"timeman.api.export.dispatcher") {
            jobs.append(request);
            if (jobs.size() == 1)
                return {R"({"status":"success","data":{"STATUS":"PROGRESS","TOTAL_ITEMS":3,"PROCESSED_ITEMS":1}})"};
            return {R"({"status":"success","data":{"STATUS":"COMPLETED","DOWNLOAD_LINK":"/bitrix/tools/timeman/export.xls"}})"};
        }
        if (request.url.path() == u"/bitrix/tools/timeman/export.xls") {
            downloads.append(request);
            return {workbook, "application/vnd.ms-excel", "200 OK", "Content-Disposition: attachment; filename=\"export.xls\"\r\n"};
        }
        return {"{}", "text/html"};
    };
    QVERIFY(fixture.start());

    CloudBackend backend(fixture.session(), true);
    QVector<WorktimeSheet> loaded;
    QStringList exported;
    connect(&backend, &Backend::worktimeSheetLoaded, this, [&](const WorktimeSheet &s) { loaded.append(s); });
    connect(&backend, &Backend::worktimeSheetExported, this, [&](const QDate &, const QString &p) { exported.append(p); });
    backend.start();
    backend.requestWorktimeSheet(QDate(2026, 2, 18));
    QTRY_COMPARE(loaded.size(), 1);
    QCOMPARE(grids.size(), 1);
    const QUrlQuery q(grids.first().url);
    QCOMPARE(grids.first().method, "GET");
    QCOMPARE(q.queryItemValue(u"ncc"_s), u"1"_s);
    QCOMPARE(q.queryItemValue(u"apply_filter"_s), u"Y"_s);
    QCOMPARE(q.queryItemValue(u"REPORT_PERIOD_datesel"_s), u"RANGE"_s);
    QCOMPARE(q.queryItemValue(u"REPORT_PERIOD_from"_s, QUrl::FullyDecoded), u"01.02.2026 00:00:00"_s);
    QCOMPARE(q.queryItemValue(u"REPORT_PERIOD_to"_s, QUrl::FullyDecoded), u"28.02.2026 00:00:00"_s);
    const auto row = loaded.first();
    QCOMPARE(row.month, month);
    QCOMPARE(row.employees.size(), 1);
    QCOMPARE(row.employees.first().name, u"User & Partner"_s);
    QCOMPARE(row.employees.first().days.first().value, u"07:35"_s);
    QVERIFY(row.canExport);

    QTemporaryDir dir;
    const auto output = dir.filePath(u"timesheet.xls"_s);
    backend.exportWorktimeSheet(month, output);
    QTRY_COMPARE(exported, QStringList{output});
    QCOMPARE(jobs.size(), 2);
    QCOMPARE(jobs.at(0).form.queryItemValue(u"EXPORT_TYPE"_s), u"excel"_s);
    QVERIFY(jobs.at(0).form.queryItemValue(u"PROCESS_TOKEN"_s).startsWith(u"opaquec"_s));
    QCOMPARE(jobs.at(0).form.queryItemValue(u"PROCESS_TOKEN"_s), jobs.at(1).form.queryItemValue(u"PROCESS_TOKEN"_s));
    QCOMPARE(jobs.at(0).form.queryItemValue(u"signedParameters"_s), u"signed.123"_s);
    QCOMPARE(jobs.at(0).form.queryItemValue(u"COMPONENT_NAME"_s, QUrl::FullyDecoded),
             u"bitrix:timeman.worktime.export.excel"_s);
    QCOMPARE(downloads.size(), 1);
    QFile saved(output);
    QVERIFY(saved.open(QIODevice::ReadOnly));
    QCOMPARE(saved.readAll(), workbook);
}

void TestCloudWorktime::rejectsOffOriginWorkbook()
{
    Fixture external;
    external.respond = [](const Request &) -> Response { return {"{}"}; };
    QVERIFY(external.start());
    const QDate month(2026, 2, 1);
    Fixture fixture;
    fixture.respond = [&](const Request &request) -> Response {
        if (request.url.path() == u"/timeman/timeman.php")
            return {sheet(month), "text/html"};
        if (QUrlQuery(request.url).queryItemValue(u"action"_s) == u"timeman.api.export.dispatcher")
            return {"{\"status\":\"success\",\"data\":{\"STATUS\":\"COMPLETED\",\"DOWNLOAD_LINK\":\""
                + external.session().origin.toEncoded() + "/export.xls\"}}"};
        return {"{}", "text/html"};
    };
    QVERIFY(fixture.start());
    CloudBackend backend(fixture.session(), true);
    bool allowed = false;
    QStringList failures;
    connect(&backend, &Backend::worktimeSheetLoaded, this, [&](const WorktimeSheet &s) { allowed = s.canExport; });
    connect(&backend, &Backend::failed, this, [&](const QString &op, const QString &) {
        if (op.startsWith(u"worktime-export:"_s))
            failures.append(op);
    });
    backend.start();
    backend.requestWorktimeSheet(month);
    QTRY_VERIFY(allowed);
    QTemporaryDir dir;
    const auto output = dir.filePath(u"timesheet.xls"_s);
    backend.exportWorktimeSheet(month, output);
    QTRY_COMPARE(failures, QStringList{u"worktime-export:2026-02"_s});
    QCOMPARE(external.requests.size(), 0);
    QVERIFY(!QFile::exists(output));
}

void TestCloudWorktime::downloadsOfficialCdnWorkbookWithoutWebSessionHeaders()
{
    Fixture receiver;
    const QByteArray workbook = "<html><body><table><tr><td>час</td></tr></table></body></html>";
    receiver.respond = [&](const Request &) -> Response {
        return {workbook, "application/vnd.ms-excel"};
    };
    QVERIFY(receiver.start());
    const auto portal = receiver.session().origin;
    const auto cdn = QUrl(u"https://cdn-ru.bitrix24.ru/b1234567/export/worktime_0123456789.xls"_s);
    auto request = worktime::networkRequest(portal, cdn, "b24=synthetic", true, true);
    QCOMPARE(request.url(), cdn);
    QCOMPARE(request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(), int(QNetworkRequest::ManualRedirectPolicy));
    QCOMPARE(request.attribute(QNetworkRequest::CookieLoadControlAttribute).toInt(), int(QNetworkRequest::Manual));
    QCOMPARE(request.attribute(QNetworkRequest::CookieSaveControlAttribute).toInt(), int(QNetworkRequest::Manual));
    QCOMPARE(request.attribute(QNetworkRequest::AuthenticationReuseAttribute).toInt(), int(QNetworkRequest::Manual));
    QVERIFY(worktime::networkRequest(portal, cdn, "b24=synthetic", true, false).url().isEmpty());

    // Reroute the already validated request inside this fixture only.
    request.setUrl(portal.resolved(QUrl(cdn.path())));
    QNetworkAccessManager transport;
    auto *reply = transport.get(request);
    QTRY_VERIFY(reply->isFinished());
    QCOMPARE(reply->error(), QNetworkReply::NoError);
    QCOMPARE(receiver.requests.size(), 1);
    QCOMPARE(receiver.requests.first().method, QByteArray("GET"));
    QVERIFY(!receiver.requests.first().sensitiveHeaders);
    const auto bytes = reply->readAll();
    QCOMPARE(bytes, workbook);
    QVERIFY(worktime::isExportWorkbook(cdn, reply->rawHeader("Content-Type"), reply->rawHeader("Content-Disposition"), bytes));
    QVERIFY(!worktime::isExportWorkbook(cdn, "text/html", {}, bytes));
    QVERIFY(!worktime::isExportWorkbook(request.url(), "application/vnd.ms-excel", {}, bytes));

    for (const auto &invalid : {u"https://cdn-ru.bitrix24.ru.invalid/b123/export/test.xls"_s,
        u"//cdn-ru.bitrix24.ru/b123/export/test.xls"_s, u"https://cdn-ru.bitrix24.ru/b123/export/./test.xls"_s,
        u"http://cdn-ru.bitrix24.ru/b123/export/test.xls"_s, u"https://cdn-ru.bitrix24.ru:444/b123/export/test.xls"_s,
        u"https://user@cdn-ru.bitrix24.ru/b123/export/test.xls"_s, u"https://cdn-ru.bitrix24.ru/b123/private/test.xls"_s,
        u"https://cdn-ru.bitrix24.ru/b123/export/nested/test.xls"_s, u"https://cdn-ru.bitrix24.ru/b123/export/test.xls?token=x"_s,
        u"https://cdn-ru.bitrix24.ru/b123/export/test.xls#x"_s, u"https://cdn-ru.bitrix24.ru/b123/export/%2E%2E/test.xls"_s})
        QVERIFY(worktime::networkRequest(portal, QUrl(invalid), "b24=synthetic", true, true).url().isEmpty());
}

QTEST_GUILESS_MAIN(TestCloudWorktime)
#include "tst_cloud_worktime.moc"
