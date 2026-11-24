#include "cloud/cloud_backend.h"
#include "core/origin_guard.h"

#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrlQuery>

using namespace Qt::StringLiterals;

namespace bx {
namespace {

constexpr qsizetype statusLimit = 128 * 1024;
constexpr qsizetype exportLimit = 16 * 1024 * 1024;
constexpr int exportPollLimit = 40;
const auto workdayRead = u"workday"_s;
const auto workdayWrite = u"workday-change"_s;
const auto sheetPath = u"/timeman/timeman.php"_s;

bool actionConfirmed(QStringView action, WorkdayState state)
{
    if (action == u"open" || action == u"reopen")
        return state == WorkdayState::Running;
    if (action == u"pause")
        return state == WorkdayState::Paused;
    return action == u"close" && state == WorkdayState::Closed;
}

QJsonObject readLegacyObject(const QByteArray &bytes)
{
    if (auto doc = QJsonDocument::fromJson(bytes); doc.isObject())
        return doc.object();

    // The web endpoint uses a PHP-generated JavaScript object with single-quoted strings.
    // Translate only string syntax and validate the entire result as JSON before reading it.
    QByteArray json;
    json.reserve(bytes.size());
    char quote = 0;
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        const char c = bytes[i];
        if (!quote) {
            if (c == '\'' || c == '"') {
                quote = c;
                json += '"';
            } else {
                json += c;
            }
        } else if (c == quote) {
            json += '"';
            quote = 0;
        } else if (c == '\\' && i + 1 < bytes.size()) {
            const char escaped = bytes[++i];
            if (escaped == '\'') {
                json += escaped;
            } else if (escaped == 'x' && i + 2 < bytes.size()) {
                json += "\\u00";
                json += bytes[i + 1];
                json += bytes[i + 2];
                i += 2;
            } else {
                json += '\\';
                json += escaped;
            }
        } else {
            if (c == '"')
                json += '\\';
            json += c;
        }
    }
    return quote ? QJsonObject{} : QJsonDocument::fromJson(json).object();
}

qint64 fieldNumber(const QJsonObject &source, QStringView key)
{
    const auto value = source.value(key);
    const auto result = value.isString() ? value.toString().toLongLong() : value.toInteger();
    return result > 0 && result <= 9999999999999999LL ? result : 0;
}

qint64 nonnegativeNumber(const QJsonObject &info, QStringView key, qint64 max)
{
    const auto value = info.value(key);
    bool valid = false;
    const auto number = value.isString() ? value.toString().toLongLong(&valid) : value.toInteger(-1);
    return (value.isDouble() || valid) && number >= 0 && number <= max ? number : -1;
}

bool flag(const QJsonObject &source, QStringView key)
{
    const auto value = source.value(key);
    const auto text = value.toString();
    return value.toBool() || text == u"Y" || text == u"1" || value.toInt() == 1;
}

bool grantedWhenSpecified(const QJsonObject &source, QStringView key)
{
    return !source.contains(key) || flag(source, key);
}

QString plain(QString text)
{
    static const QRegularExpression tag(uR"(<(?:[^>"']++|"[^"]*"|'[^']*')*>)"_s);
    static const QRegularExpression entity(uR"(&(?:#([0-9]{1,7})|#[xX]([a-fA-F0-9]{1,6})|(amp|lt|gt|quot|apos|nbsp));)"_s);
    text.remove(tag);
    QString result;
    qsizetype end = 0;
    auto matches = entity.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        result += QStringView(text).mid(end, match.capturedStart() - end);
        if (match.hasCaptured(1) || match.hasCaptured(2)) {
            const auto n = match.hasCaptured(1) ? match.capturedView(1).toUInt() : match.capturedView(2).toUInt(nullptr, 16);
            if (n >= 32 && n <= 0x10ffff && (n < 0xd800 || n > 0xdfff)) {
                const auto character = static_cast<char32_t>(n);
                result += QString::fromUcs4(&character, 1);
            }
        } else {
            const auto name = match.capturedView(3);
            result += name == u"amp" ? u'&' : name == u"lt" ? u'<' : name == u"gt" ? u'>'
                : name == u"quot" ? u'"' : name == u"apos" ? u'\'' : u' ';
        }
        end = match.capturedEnd();
    }
    result += QStringView(text).mid(end);
    return result.simplified().left(380);
}

WorkdayStatus statusFrom(const QJsonObject &data)
{
    WorkdayStatus status;
    const auto state = data.value(u"STATE").toString();
    const auto open = data.value(u"CAN_OPEN").toString();
    if (state == u"CLOSED") {
        status.state = WorkdayState::Closed;
        status.canStart = open == u"OPEN";
        status.canResume = open == u"REOPEN";
    } else if (state == u"OPENED") {
        status.state = WorkdayState::Running;
        status.canPause = grantedWhenSpecified(data, u"CAN_PAUSE");
        status.canFinish = grantedWhenSpecified(data, u"CAN_CLOSE");
    } else if (state == u"PAUSED") {
        status.state = WorkdayState::Paused;
        status.canResume = grantedWhenSpecified(data, u"CAN_RESUME");
        status.canFinish = grantedWhenSpecified(data, u"CAN_CLOSE");
    } else if (state == u"EXPIRED") {
        status.state = WorkdayState::Expired;
        status.details = u"Укажите время окончания просроченного рабочего дня."_s;
    }
    const auto info = data.value(u"INFO").toObject();
    const bool canEdit = flag(data, u"CAN_EDIT") || flag(info, u"CAN_EDIT");
    const bool hasRecord = fieldNumber(data, u"ID") > 0 || fieldNumber(data, u"RECORD_ID") > 0;
    const auto start = fieldNumber(info, u"DATE_START");
    const auto today = QDate::currentDate();
    const bool previousRecord = start && QDateTime::fromSecsSinceEpoch(start).date() < today;
    status.canStartAt = status.canStart && (canEdit || !hasRecord || previousRecord);
    status.canFinishAt = (status.state == WorkdayState::Running && status.canFinish && canEdit)
        || (status.state == WorkdayState::Expired && hasRecord && grantedWhenSpecified(data, u"CAN_CLOSE"));
    status.requiresTimeReason = !flag(data, u"TM_FREE");
    status.requiresReport = (status.canFinish || status.canFinishAt) && status.requiresTimeReason
        && data.value(u"REPORT_REQ").toString() == u"Y";
    if (status.canStartAt)
        status.startDate = today;
    if (status.canFinishAt) {
        qint64 suggested = fieldNumber(info, u"RECOMMENDED_CLOSE_TIMESTAMP");
        if (!suggested)
            suggested = fieldNumber(info, u"DATE_FINISH");
        if (!suggested)
            suggested = fieldNumber(info, u"DATE_START");
        if (suggested)
            status.finishDate = QDateTime::fromSecsSinceEpoch(suggested).date();
    }
    if (status.canFinishAt && status.state == WorkdayState::Expired) {
        auto seconds = nonnegativeNumber(data, u"EXPIRED_DATE", 86399);
        if (seconds < 0)
            seconds = nonnegativeNumber(info, u"TIME_FINISH", 86399);
        if (seconds >= 0)
            status.suggestedFinishTime = QTime(0, 0).addSecs(static_cast<int>(seconds));
    }

    const auto displayStart = fieldNumber(info, u"DISPLAY_START_TIMESTAMP");
    const auto elapsedStart = status.state == WorkdayState::Running && displayStart ? displayStart : start;
    const auto finish = status.state == WorkdayState::Running
        ? QDateTime::currentSecsSinceEpoch() : fieldNumber(info, u"DATE_FINISH");
    if (status.state == WorkdayState::Paused || status.state == WorkdayState::Closed)
        status.elapsedSeconds = nonnegativeNumber(info, u"DURATION", 7 * 86400);
    if (status.elapsedSeconds < 0 && elapsedStart && finish >= elapsedStart && finish - elapsedStart <= 7 * 86400)
        status.elapsedSeconds = qMax<qint64>(0, finish - elapsedStart - fieldNumber(info, u"TIME_LEAKS"));
    return status;
}

QByteArray formEncode(const QList<QPair<QString, QString>> &fields)
{
    QByteArray result;
    for (const auto &[key, value] : fields) {
        if (!result.isEmpty())
            result += '&';
        result += QUrl::toPercentEncoding(key) + '=' + QUrl::toPercentEncoding(value);
    }
    return result;
}

QString attr(QStringView tag, const QRegularExpression &expression)
{
    return expression.match(tag).captured(2);
}

QString textWithClass(QStringView markup, const QRegularExpression &expression)
{
    const auto result = expression.match(markup);
    return result.hasMatch() ? plain(result.captured(2)) : QString{};
}

WorktimeSheet parseSheet(const QDate &month, const QString &html)
{
    WorktimeSheet sheet;
    sheet.month = month;
    const auto container = html.indexOf(u"timeman-report-container"_s);
    const auto start = html.indexOf(u"main-grid-table"_s, qMax<qsizetype>(0, container));
    const auto end = html.indexOf(u"</table>"_s, qMax<qsizetype>(0, start));
    if (container < 0 || start < 0 || end < 0) {
        sheet.notice = u"Табель недоступен для этой учётной записи."_s;
        return sheet;
    }

    const auto table = QStringView(html).mid(start, end - start);
    static const QRegularExpression heading(uR"(<th\b[^>]*\bdata-name\s*=\s*(['"])(\d{4}-\d{2}-\d{2})\1)"_s);
    QSet<QDate> columns;
    auto headers = heading.globalMatch(table);
    while (headers.hasNext()) {
        const auto day = QDate::fromString(headers.next().capturedView(2), Qt::ISODate);
        if (day.isValid() && day.month() == month.month() && day.year() == month.year())
            columns.insert(day);
    }
    if (columns.size() != month.daysInMonth()) {
        sheet.notice = u"Портал вернул табель за другой период или скрыл часть дней."_s;
        return sheet;
    }

    static const QRegularExpression row(uR"(<tr\b((?:[^>"']++|"[^"]*"|'[^']*')*)>([\s\S]*?)</tr\s*>)"_s);
    static const QRegularExpression cell(uR"(<td\b((?:[^>"']++|"[^"]*"|'[^']*')*)>([\s\S]*?)</td\s*>)"_s);
    static const QRegularExpression className(uR"((?:^|\s)class\s*=\s*(['"])([^'"]*)\1)"_s);
    static const QRegularExpression column(uR"((?:^|\s)data-column-id\s*=\s*(['"])([^'"]*)\1)"_s);
    static const QRegularExpression user(uR"(<a\b[^>]*class\s*=\s*(['"])[^'"]*\btimeman-grid-user-name\b[^'"]*\1[^>]*>([\s\S]*?)</a\s*>)"_s);
    static const QRegularExpression duration(uR"(<(span|div)\b[^>]*class\s*=\s*['"][^'"]*\btimeman-grid-worktime-duration-value\b[^'"]*['"][^>]*>([\s\S]*?)</\1\s*>)"_s);
    auto rows = row.globalMatch(table);
    while (rows.hasNext() && sheet.employees.size() < 300) {
        const auto match = rows.next();
        if (!attr(match.capturedView(1), className).split(u' ', Qt::SkipEmptyParts).contains(u"main-grid-row-body"_s))
            continue;
        WorktimeEmployee employee;
        auto cells = cell.globalMatch(match.capturedView(2));
        while (cells.hasNext()) {
            const auto item = cells.next();
            const auto id = attr(item.capturedView(1), column);
            if (id == u"USER_NAME") {
                employee.name = textWithClass(item.capturedView(2), user);
                continue;
            }
            const auto day = QDate::fromString(id, Qt::ISODate);
            if (!columns.contains(day))
                continue;
            const auto content = item.capturedView(2);
            const auto dayValue = textWithClass(content, duration);
            const auto details = plain(content.left(4096).toString());
            const auto value = dayValue.isEmpty() ? details.left(40) : dayValue;
            employee.days.append({day, value, value == details ? QString{} : details});
        }
        if (!employee.name.isEmpty())
            sheet.employees.append(std::move(employee));
    }
    if (sheet.employees.isEmpty())
        sheet.notice = u"За выбранный период нет строк, доступных на портале."_s;
    return sheet;
}

bool isOfficialExportCdn(const QUrl &url)
{
    static const QRegularExpression path(uR"(\A/b[0-9]{1,20}/export/[A-Za-z0-9_-]{1,128}\.xls\z)"_s);
    return url.isValid() && url.scheme() == u"https" && url.host() == u"cdn-ru.bitrix24.ru"
        && url.port(443) == 443 && !url.authority(QUrl::FullyEncoded).contains(u'@')
        && !url.hasQuery() && !url.hasFragment() && path.match(url.path(QUrl::FullyEncoded)).hasMatch();
}

} // namespace

namespace worktime {

QNetworkRequest networkRequest(const QUrl &portal, const QUrl &url, const QByteArray &cookie,
                               bool allowLoopback, bool exportDownload)
{
    const bool publicExport = exportDownload && isOfficialExportCdn(url);
    if (!publicExport && (!OriginGuard(portal).isSameOrigin(url) || !url.userInfo().isEmpty() || url.hasFragment()
        || (url.scheme() != u"https" && !(allowLoopback && OriginGuard::isLoopback(url)))))
        return {};

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    if (!publicExport)
        request.setRawHeader("Cookie", cookie);
    request.setTransferTimeout(12000);
    return request;
}

bool isExportWorkbook(const QUrl &url, const QByteArray &type, const QByteArray &attachment, const QByteArray &data)
{
    const bool binary = data.startsWith(QByteArray::fromHex("d0cf11e0a1b11ae1")) || data.startsWith("PK\x03\x04");
    const auto heading = data.left(4096).toLower();
    const bool htmlWorkbook = type == "application/vnd.ms-excel"
        && (attachment.contains(".xls") || isOfficialExportCdn(url))
        && (heading.contains("<table") || heading.contains("schemas-microsoft-com:office:excel"));
    const bool spreadsheet = type == "application/vnd.ms-excel"
        || type == "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"
        || (type == "application/octet-stream" && attachment.contains(".xls"));
    return spreadsheet && (binary || htmlWorkbook);
}

} // namespace worktime

struct CloudBackend::WorktimeExport {
    QDate month;
    QString target;
    QString operation;
    Fields fields;
    quint64 revision = 0;
    int attempts = 0;
    QDeadlineTimer deadline{60000};
};

void CloudBackend::resetWorktime()
{
    ++m_worktimeRevision;
    if (m_workdayReply)
        m_workdayReply->abort();
    m_inflightOperations.remove(workdayRead);
    m_workday = {};
    m_worktimeSheetMonth = {};
    m_loadingWorktimeSheet = false;
    m_worktimeExportMonth = {};
    m_worktimeExportFields.clear();
    m_worktimeExport.reset();
}

QNetworkReply *CloudBackend::startWorktimeRequest(const QUrl &url, const QByteArray &form, qsizetype limit)
{
    const OriginGuard guard(m_session.origin);
    if (!m_started || m_authExpired || !m_session.isValid() || m_requests.size() >= 12
        || m_session.cookie.contains('\r') || m_session.cookie.contains('\n'))
        return nullptr;

    auto request = worktime::networkRequest(m_session.origin, url, m_session.cookie, m_allowLoopback,
                                           form.isEmpty() && limit == exportLimit);
    if (request.url().isEmpty())
        return nullptr;
    if (!form.isEmpty()) {
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded; charset=UTF-8");
        request.setRawHeader("Accept", "application/json");
        request.setRawHeader("X-Requested-With", "XMLHttpRequest");
        request.setRawHeader("BX-Ajax", "true");
        request.setRawHeader("Origin", m_session.origin.toEncoded(QUrl::RemovePath));
        request.setRawHeader("Referer", guard.resolve(sheetPath).toEncoded());
    }

    auto *reply = form.isEmpty() ? m_http.get(request) : m_http.post(request, form);
    m_requests.insert(reply);
    reply->setReadBufferSize(limit + 1);
    QTimer::singleShot(15000, reply, [reply] {
        if (!reply->isFinished())
            reply->abort();
    });
    connect(reply, &QIODevice::readyRead, this, [reply, limit] {
        if (reply->bytesAvailable() > limit)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        m_requests.remove(reply);
        reply->deleteLater();
        scheduleRefresh();
    });
    return reply;
}

void CloudBackend::requestWorkday()
{
    if (!m_inflightOperations.contains(workdayWrite))
        queryWorkday(u"update"_s, {});
}

void CloudBackend::queryWorkday(const QString &action, Fields fields, bool allowSessionRetry)
{
    const bool writing = action != u"update";
    const auto operation = writing ? workdayWrite : workdayRead;
    if (m_inflightOperations.contains(operation))
        return;
    auto url = OriginGuard(m_session.origin).resolve(u"/bitrix/tools/timeman.php"_s);
    QUrlQuery query;
    query.addQueryItem(u"action"_s, action);
    query.addQueryItem(u"site_id"_s, u"s1"_s);
    query.addQueryItem(u"sessid"_s, QString::fromLatin1(m_session.csrf));
    url.setQuery(query);
    fields.append({u"device"_s, u"browser"_s});
    fields.append({u"full"_s, u"Y"_s});
    if (const auto id = fieldNumber(m_workday, u"ID"); id > 0)
        fields.append({u"recordId"_s, QString::number(id)});
    if (m_session.csrf.isEmpty()) {
        emit failed(operation, u"Для учёта времени требуется новая веб-сессия."_s);
        return;
    }
    auto *reply = startWorktimeRequest(url, formEncode(fields), statusLimit);
    if (!reply) {
        emit failed(operation, u"Не удалось выполнить запрос рабочего дня."_s);
        return;
    }
    m_inflightOperations.insert(operation);
    m_workdayReply = reply;
    m_workday = {};
    const auto revision = m_worktimeRevision;
    const auto sessionRevision = m_sessionRevision;
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, action, operation, revision, sessionRevision, writing, allowSessionRetry] {
        m_inflightOperations.remove(operation);
        m_workdayReply = nullptr;
        if (!m_started || revision != m_worktimeRevision)
            return;
        const auto unauthorized = [&] {
            if (writing || !allowSessionRetry) {
                if (writing)
                    confirmSession(sessionRevision);
                emit failed(operation, writing
                    ? u"Портал отклонил действие. Обновите состояние рабочего дня перед повтором."_s
                    : u"Состояние рабочего дня недоступно. Обновите его вручную."_s);
                return;
            }
            m_inflightOperations.insert(operation);
            confirmSession(sessionRevision, [this, operation, revision, sessionRevision](bool accepted) {
                if (!m_started || revision != m_worktimeRevision)
                    return;
                m_inflightOperations.remove(operation);
                if (accepted && m_sessionRevision != sessionRevision)
                    queryWorkday(u"update"_s, {}, false);
                else
                    emit failed(operation, u"Состояние рабочего дня недоступно. Обновите его вручную."_s);
            });
        };
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (code == 401) {
            unauthorized();
            return;
        }
        if (reply->error() != QNetworkReply::NoError || code != 200 || reply->bytesAvailable() > statusLimit) {
            const auto message = writing
                ? u"Не удалось подтвердить изменение. Обновите состояние рабочего дня перед повтором."_s
                : u"Не удалось получить состояние рабочего дня."_s;
            emit failed(operation, message);
            return;
        }
        const auto bytes = reply->readAll();
        if (bytes.size() < 256 && QString::fromUtf8(bytes).trimmed().startsWith(u"Ваша сессия истекла")) {
            unauthorized();
            return;
        }
        const auto data = readLegacyObject(bytes);
        const auto error = data.value(u"error");
        if (!error.isUndefined() && !error.isNull()
            && !(error.isBool() && !error.toBool()) && !(error.isString() && error.toString().isEmpty())) {
            auto reason = plain(error.toString());
            if (reason.isEmpty())
                reason = u"Портал запретил действие с рабочим временем."_s;
            if (writing)
                emit failed(operation, reason);
            else {
                WorkdayStatus unavailable;
                unavailable.details = reason;
                emit workdayLoaded(unavailable);
            }
            return;
        }
        const auto status = statusFrom(data);
        if (status.state == WorkdayState::Unavailable
            || (writing && (!actionConfirmed(action, status.state) || fieldNumber(data, u"CLOSE_TIMESTAMP")))) {
            emit failed(operation, u"Портал не подтвердил действие. Проверьте рабочий день на портале."_s);
            return;
        }
        m_workday = data;
        if (writing)
            emit workdayChanged(status);
        else
            emit workdayLoaded(status);
    });
}

void CloudBackend::changeWorkday(WorkdayAction action, const QString &report)
{
    if (m_inflightOperations.contains(workdayWrite) || m_inflightOperations.contains(workdayRead)) {
        emit failed(workdayWrite, u"Дождитесь ответа портала о рабочем дне."_s);
        return;
    }
    const auto status = statusFrom(m_workday);
    const auto text = report.trimmed();
    if (text.size() > 16000) {
        emit failed(workdayWrite, u"Сократите отчёт о рабочем дне."_s);
        return;
    }
    if (action == WorkdayAction::Start && status.canStart) {
        queryWorkday(u"open"_s, {{u"timestamp"_s, u"0"_s}, {u"report"_s, {}}});
    } else if (action == WorkdayAction::Pause && status.canPause) {
        queryWorkday(u"pause"_s, {});
    } else if (action == WorkdayAction::Resume && status.canResume) {
        const auto next = status.state == WorkdayState::Paused ? u"continue"_s : u"reopen"_s;
        queryWorkday(u"reopen"_s, {{u"newActionName"_s, next}});
    } else if (action == WorkdayAction::Finish && status.canFinish) {
        if (status.requiresReport && text.isEmpty()) {
            emit failed(workdayWrite, u"Заполните отчёт перед завершением рабочего дня."_s);
            return;
        }
        const bool reportForm = !flag(m_workday, u"TM_FREE") && m_workday.value(u"REPORT_REQ").toString() != u"A";
        if (reportForm)
            queryWorkday(u"close"_s, {{u"ready"_s, u"Y"_s}, {u"REPORT"_s, text}});
        else {
            Fields fields{{u"timestamp"_s, u"0"_s}, {u"report"_s, {}}};
            if (!fieldNumber(m_workday, u"ID")) {
                if (const auto id = fieldNumber(m_workday, u"RECORD_ID"); id > 0)
                    fields.append({u"recordId"_s, QString::number(id)});
            }
            queryWorkday(u"close"_s, std::move(fields));
        }
    } else {
        emit failed(workdayWrite, u"Действие недоступно в текущем состоянии рабочего дня. Обновите состояние."_s);
    }
}

void CloudBackend::changeWorkdayAt(WorkdayAction action, const QDate &date, const QTime &time,
                                  const QString &reason, const QString &report)
{
    if (m_inflightOperations.contains(workdayWrite) || m_inflightOperations.contains(workdayRead)) {
        emit failed(workdayWrite, u"Дождитесь ответа портала о рабочем дне."_s);
        return;
    }
    const auto status = statusFrom(m_workday);
    const bool starting = action == WorkdayAction::Start && status.canStartAt;
    const bool finishing = action == WorkdayAction::Finish && status.canFinishAt;
    if (!starting && !finishing) {
        emit failed(workdayWrite, u"Изменение времени недоступно в текущем состоянии рабочего дня."_s);
        return;
    }
    if (!date.isValid() || !time.isValid()) {
        emit failed(workdayWrite, u"Укажите дату и время рабочего дня."_s);
        return;
    }
    const auto explanation = status.requiresTimeReason ? reason.trimmed() : QString{};
    const auto dailyReport = report.trimmed();
    if ((status.requiresTimeReason && explanation.isEmpty()) || explanation.size() > 16000
        || dailyReport.size() > 16000) {
        emit failed(workdayWrite, u"Укажите краткую причину изменения времени и проверьте объём отчёта."_s);
        return;
    }
    if (finishing && status.requiresReport && dailyReport.isEmpty()) {
        emit failed(workdayWrite, u"Заполните отчёт перед завершением рабочего дня."_s);
        return;
    }

    const auto localDate = date.toString(u"dd.MM.yyyy"_s);
    const auto seconds = time.hour() * 3600 + time.minute() * 60;
    const bool reportForm = finishing && status.requiresTimeReason && m_workday.value(u"REPORT_REQ").toString() != u"A";
    if (!reportForm && seconds == 0) {
        // The legacy selector represents the regular current-time action as timestamp=0.
        emit failed(workdayWrite, u"Для времени 00:00 воспользуйтесь порталом: этот запрос означает текущее время."_s);
        return;
    }
    Fields fields{{u"report"_s, explanation}};
    if (reportForm) {
        fields.append({u"ready"_s, u"Y"_s});
        fields.append({u"REPORT"_s, dailyReport});
        fields.append({u"timeman_edit_to"_s, QString::number(seconds)});
        fields.append({u"endUserDate"_s, localDate});
    } else {
        fields.append({u"timestamp"_s, QString::number(seconds)});
        fields.append({u"customUserDate"_s, localDate});
    }
    if (finishing && !fieldNumber(m_workday, u"ID")) {
        if (const auto id = fieldNumber(m_workday, u"RECORD_ID"); id > 0)
            fields.append({u"recordId"_s, QString::number(id)});
    }
    queryWorkday(starting ? u"open"_s : u"close"_s, std::move(fields));
}

void CloudBackend::requestWorktimeSheet(const QDate &selected)
{
    if (!selected.isValid() || selected.year() < 2000 || selected.year() > 2200) {
        emit failed(u"worktime-sheet:"_s + selected.toString(u"yyyy-MM"_s), u"Укажите корректный месяц."_s);
        return;
    }
    const QDate month(selected.year(), selected.month(), 1);
    m_worktimeSheetMonth = month;
    if (!m_loadingWorktimeSheet)
        loadWorktimeSheet(month);
}

void CloudBackend::loadWorktimeSheet(const QDate &month)
{
    m_loadingWorktimeSheet = true;
    const auto operation = u"worktime-sheet:"_s + month.toString(u"yyyy-MM"_s);
    auto url = OriginGuard(m_session.origin).resolve(sheetPath);
    QUrlQuery query;
    query.addQueryItem(u"ncc"_s, u"1"_s);
    query.addQueryItem(u"apply_filter"_s, u"Y"_s);
    query.addQueryItem(u"REPORT_PERIOD_datesel"_s, u"RANGE"_s);
    query.addQueryItem(u"REPORT_PERIOD_from"_s, month.toString(u"dd.MM.yyyy"_s) + u" 00:00:00"_s);
    query.addQueryItem(u"REPORT_PERIOD_to"_s, month.addMonths(1).addDays(-1).toString(u"dd.MM.yyyy"_s) + u" 00:00:00"_s);
    url.setQuery(query);
    if (m_worktimeExportMonth == month) {
        m_worktimeExportMonth = {};
        m_worktimeExportFields.clear();
    }
    const auto revision = m_worktimeRevision;
    fetchPortalHtml(url, [this, operation, month, revision](const QString &html) {
        if (!m_started || revision != m_worktimeRevision)
            return;
        m_loadingWorktimeSheet = false;
        if (m_worktimeSheetMonth != month) {
            loadWorktimeSheet(m_worktimeSheetMonth);
            return;
        }
        if (html.isEmpty()) {
            emit failed(operation, u"Не удалось загрузить табель с портала."_s);
            return;
        }
        auto sheet = parseSheet(month, html);
        static const QRegularExpression exportOptions(uR"(\bnew\s+BX\.Timeman\.Export\s*\(\s*(\{[^{}]{1,8192}\}))"_s);
        const auto expression = exportOptions.match(html).captured(1);
        if (sheet.notice.isEmpty() && !expression.isEmpty()) {
            const auto options = readLegacyObject(expression.toUtf8());
            const auto siteId = options.value(u"siteId").toString();
            const auto component = options.value(u"componentName").toString();
            const auto token = options.value(u"sToken").toString();
            const auto signature = options.value(u"signedParameters").toString();
            static const QRegularExpression site(uR"(^[A-Za-z0-9_-]{1,20}$)"_s);
            static const QRegularExpression safeValue(uR"(^[A-Za-z0-9._~:/+=-]{1,4096}$)"_s);
            if (site.match(siteId).hasMatch() && component == u"bitrix:timeman.worktime.export.excel"
                && safeValue.match(token).hasMatch() && safeValue.match(signature).hasMatch()) {
                m_worktimeExportMonth = month;
                m_worktimeExportFields = {{u"SITE_ID"_s, siteId}, {u"COMPONENT_NAME"_s, component},
                    {u"PROCESS_TOKEN"_s, token}, {u"signedParameters"_s, signature}, {u"EXPORT_TYPE"_s, u"excel"_s}};
                sheet.canExport = true;
            }
        }
        if (sheet.notice.isEmpty() && html.contains(u"main-grid-more-btn"_s))
            sheet.notice = u"Строк на открытой странице портала: %1."_s.arg(sheet.employees.size());
        emit worktimeSheetLoaded(sheet);
    });
}

void CloudBackend::exportWorktimeSheet(const QDate &selected, const QString &targetPath)
{
    const QDate month(selected.year(), selected.month(), 1);
    const auto operation = u"worktime-export:"_s + month.toString(u"yyyy-MM"_s);
    if (!m_started || !month.isValid() || month != m_worktimeSheetMonth || month != m_worktimeExportMonth
        || m_worktimeExportFields.isEmpty()
        || !QDir::isAbsolutePath(targetPath) || targetPath.size() > 4096 || targetPath.contains(QChar::Null)) {
        emit failed(operation, u"Откройте доступный табель и выберите файл для экспорта."_s);
        return;
    }
    if (!m_worktimeExport.expired()) {
        emit failed(operation, u"Дождитесь завершения текущего экспорта."_s);
        return;
    }
    auto job = std::make_shared<WorktimeExport>();
    job->month = month;
    job->target = targetPath;
    job->operation = operation;
    job->fields = m_worktimeExportFields;
    job->revision = m_worktimeRevision;
    for (auto &field : job->fields) {
        if (field.first == u"PROCESS_TOKEN")
            field.second += u'c' + QString::number(QDateTime::currentMSecsSinceEpoch());
    }
    m_worktimeExport = job;
    pollWorktimeExport(job);
}

void CloudBackend::pollWorktimeExport(const std::shared_ptr<WorktimeExport> &job)
{
    if (!m_started || job->revision != m_worktimeRevision)
        return;
    if (++job->attempts > exportPollLimit || job->deadline.hasExpired()) {
        emit failed(job->operation, u"Портал не завершил формирование файла вовремя."_s);
        return;
    }
    ajax(u"timeman.api.export.dispatcher"_s, job->fields, job->operation, [this, job](const QJsonValue &result) {
        if (!m_started || job->revision != m_worktimeRevision)
            return;
        const auto data = result.toObject();
        const auto state = data.value(u"STATUS").toString();
        if (state == u"PROGRESS") {
            QTimer::singleShot(750, this, [this, job] { pollWorktimeExport(job); });
            return;
        }
        const auto link = data.value(u"DOWNLOAD_LINK").toString();
        const auto url = QUrl(link, QUrl::StrictMode);
        if ((state != u"COMPLETED" && state != u"NOT_REQUIRED") || link.isEmpty() || link.size() > 4096
            || !url.isValid()) {
            emit failed(job->operation, u"Портал не предоставил файл табеля."_s);
            return;
        }
        const OriginGuard guard(m_session.origin);
        if (url.host().isEmpty() || url.host() == m_session.origin.host()) {
            downloadWorktimeExport(job, guard.resolve(sheetPath).resolved(url));
        } else if (isOfficialExportCdn(url) && link == QString::fromLatin1(url.toEncoded())) {
            downloadWorktimeExport(job, url);
        } else {
            emit failed(job->operation, u"Портал вернул недоступную ссылку на файл табеля."_s);
        }
    });
}

void CloudBackend::downloadWorktimeExport(const std::shared_ptr<WorktimeExport> &job, const QUrl &url)
{
    // The dispatcher can return a same-portal file or an anonymous CDN export.
    auto *reply = startWorktimeRequest(url, {}, exportLimit);
    if (!reply) {
        emit failed(job->operation, u"Портал вернул недоступную ссылку на файл табеля."_s);
        return;
    }
    connect(reply, &QNetworkReply::finished, this, [this, reply, job, url] {
        if (!m_started || job->revision != m_worktimeRevision)
            return;
        const auto type = reply->rawHeader("Content-Type").split(';').first().trimmed().toLower();
        const auto attachment = reply->rawHeader("Content-Disposition").toLower();
        if (reply->error() != QNetworkReply::NoError
            || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200
            || reply->bytesAvailable() > exportLimit) {
            emit failed(job->operation, u"Не удалось скачать файл табеля."_s);
            return;
        }
        const auto data = reply->readAll();
        if (!worktime::isExportWorkbook(url, type, attachment, data)) {
            emit failed(job->operation, u"Ссылка портала вернула данные другого формата."_s);
            return;
        }
        QSaveFile file(job->target);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
            emit failed(job->operation, u"Не удалось сохранить файл табеля."_s);
            return;
        }
        emit worktimeSheetExported(job->month, job->target);
    });
}

} // namespace bx
