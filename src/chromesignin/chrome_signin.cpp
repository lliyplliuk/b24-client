#include "chromesignin/chrome_signin.h"
#include "chromesignin/chrome_signin_internal.h"
#include "signin/session_capture.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QPointer>
#include <QProcess>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTimer>
#include <utility>

#ifdef Q_OS_LINUX
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace bx::chromesignin {

QString ensureChromeProfile(const QString &applicationData)
{
#ifdef Q_OS_LINUX
    const auto directory = QDir::cleanPath(applicationData);
    if (!QDir::isAbsolutePath(directory) || !QDir().mkpath(QFileInfo(directory).absolutePath()))
        return {};
    const auto encoded = QFile::encodeName(directory);
    if (::mkdir(encoded.constData(), 0700) != 0 && errno != EEXIST)
        return {};
    const int parent = ::open(encoded.constData(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (parent == -1)
        return {};
    struct stat parentStatus {};
    const bool privateParent = ::fstat(parent, &parentStatus) == 0 && parentStatus.st_uid == ::geteuid()
        && (::fchmod(parent, 0700) == 0);
    const bool created = privateParent && (::mkdirat(parent, "Chrome", 0700) == 0 || errno == EEXIST);
    const int fd = created ? ::openat(parent, "Chrome", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW) : -1;
    ::close(parent);
    if (fd == -1)
        return {};
    struct stat status {};
    const bool owned = ::fstat(fd, &status) == 0 && status.st_uid == ::geteuid();
    const bool restricted = owned && ::fchmod(fd, 0700) == 0;
    ::close(fd);
    return restricted ? QDir(directory).filePath(QStringLiteral("Chrome")) : QString();
#else
    Q_UNUSED(applicationData)
    return {};
#endif
}

bool Frames::append(const QByteArray &bytes)
{
    constexpr qsizetype limit = 2 * 1024 * 1024;
    if (bytes.size() > limit - m_pending.size()) {
        clear();
        return false;
    }
    m_pending += bytes;
    return true;
}

std::optional<QByteArray> Frames::take()
{
    const auto end = m_pending.indexOf(char(0));
    if (end < 0)
        return std::nullopt;
    auto message = m_pending.left(end);
    m_pending.remove(0, end + 1);
    return message;
}

void Frames::clear() { m_pending.clear(); }

QString csrfProbeExpression(const QUrl &portal)
{
    // Encode the trusted origin as a JSON string before using it in JavaScript.
    auto origin = QJsonDocument(QJsonArray{portal.toString(QUrl::FullyEncoded)})
                      .toJson(QJsonDocument::Compact);
    origin = origin.mid(1, origin.size() - 2);
    return QStringLiteral("(location.origin === %1 && typeof window.BX?.bitrix_sessid === \"function\")"
                          " ? window.BX.bitrix_sessid() : null")
        .arg(QString::fromUtf8(origin));
}

QByteArray portalCookieHeader(const QUrl &portal, const QJsonArray &cookies)
{
    signin::CookieCapture capture(portal);
    for (const auto &entry : cookies) {
        const auto fields = entry.toObject();
        if (fields.contains(QStringLiteral("partitionKey"))
            || fields.value(QStringLiteral("partitionKeyOpaque")).toBool())
            continue;
        const auto name = fields.value(QStringLiteral("name")).toString();
        const auto value = fields.value(QStringLiteral("value")).toString();
        if (QString::fromLatin1(name.toLatin1()) != name
            || QString::fromLatin1(value.toLatin1()) != value)
            continue;

        QNetworkCookie cookie(name.toLatin1(), value.toLatin1());
        cookie.setDomain(fields.value(QStringLiteral("domain")).toString());
        cookie.setPath(fields.value(QStringLiteral("path")).toString());
        cookie.setSecure(fields.value(QStringLiteral("secure")).toBool());
        const double expires = fields.value(QStringLiteral("expires")).toDouble(-1);
        if (expires > 0 && expires < 253402300800.0)
            cookie.setExpirationDate(QDateTime::fromSecsSinceEpoch(static_cast<qint64>(expires), Qt::UTC));
        capture.add(cookie);
    }
    return capture.header();
}

} // namespace bx::chromesignin

namespace bx {

#ifdef Q_OS_LINUX
namespace {

constexpr qsizetype maxProbeSize = 256 * 1024;
constexpr qint64 probeRetryDelay = 10000;

void closeFd(int &fd)
{
    if (fd != -1)
        ::close(std::exchange(fd, -1));
}

bool openBridge(int &parent, int &child)
{
    int pair[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) < 0)
        return false;
    parent = pair[0];
    child = ::fcntl(pair[1], F_DUPFD_CLOEXEC, 5);
    ::close(pair[1]);
    if (child == -1 || ::fcntl(parent, F_SETFL, O_NONBLOCK) < 0) {
        closeFd(parent);
        closeFd(child);
        return false;
    }
    return true;
}

QString chromeExecutable()
{
    for (const auto *name : {"google-chrome-stable", "google-chrome", "chromium", "chromium-browser"}) {
        const auto found = QStandardPaths::findExecutable(QString::fromLatin1(name));
        if (!found.isEmpty())
            return found;
    }
    return {};
}

} // namespace
#endif

class ChromeSignin::Impl {
public:
    explicit Impl(ChromeSignin &owner);
    ~Impl();
    bool start(const QUrl &portal, bool background);
    void cancel();

private:
    ChromeSignin &m_owner;

#ifdef Q_OS_LINUX
    enum class Command { None, FindPage, Attach, Navigate, Inspect, Cookies };

    void launched();
    void pump();
    void receive();
    void flush();
    void dispatch(const QJsonObject &message);
    void send(Command command, const QString &method, const QJsonObject &params = {}, bool page = false);
    void writeCommand(const QJsonObject &command);
    void verify(Session candidate);
    void stop(QString error = {}, std::optional<Session> session = std::nullopt);
    void forceClose();
    void finish();
    void closeChannels();

    QProcess m_browser;
    QNetworkAccessManager m_http;
    QTimer m_poll;
    QTimer m_killTimer;
    QTimer m_backgroundDeadline;
    QElapsedTimer m_startedAt;
    QElapsedTimer m_commandAt;
    QElapsedTimer m_rejectedAt;
    std::unique_ptr<QLockFile> m_lock;
    std::unique_ptr<QSocketNotifier> m_reader;
    std::unique_ptr<QSocketNotifier> m_writer;
    QPointer<QNetworkReply> m_probe;
    chromesignin::Frames m_frames;
    QByteArray m_outgoing;
    QByteArray m_csrf;
    QByteArray m_rejectedProbe;
    QUrl m_portal;
    QUrl m_restart;
    bool m_restartBackground = false;
    QString m_pageSession;
    QString m_error;
    std::optional<Session> m_authorized;
    Command m_pending = Command::None;
    int m_nextId = 0;
    int m_readFd = -1;
    int m_writeFd = -1;
    int m_childReadFd = -1;
    int m_childWriteFd = -1;
    bool m_closing = false;
#endif
};

ChromeSignin::Impl::Impl(ChromeSignin &owner) : m_owner(owner)
{
#ifdef Q_OS_LINUX
    m_http.setProxy(QNetworkProxy::NoProxy);
    m_poll.setInterval(900);
    m_killTimer.setInterval(3500);
    m_killTimer.setSingleShot(true);
    m_backgroundDeadline.setSingleShot(true);
    QObject::connect(&m_poll, &QTimer::timeout, &m_owner, [this] { pump(); });
    QObject::connect(&m_killTimer, &QTimer::timeout, &m_owner, [this] { forceClose(); });
    QObject::connect(&m_backgroundDeadline, &QTimer::timeout, &m_owner, [this] {
        stop(QStringLiteral("Время фонового восстановления входа истекло."));
    });
    QObject::connect(&m_browser, &QProcess::started, &m_owner, [this] { launched(); });
    QObject::connect(&m_browser, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &m_owner, [this] {
        if (!m_closing)
            m_error = m_pageSession.isEmpty()
                ? QStringLiteral("Chrome не открыл окно входа. Если оно уже открыто, закройте его и повторите.")
                : QStringLiteral("Окно Chrome было закрыто до завершения входа.");
        finish();
    });
    QObject::connect(&m_browser, &QProcess::errorOccurred, &m_owner, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            if (!m_closing)
                m_error = QStringLiteral("Не удалось запустить Chrome.");
            finish();
        }
    });
#endif
}

ChromeSignin::Impl::~Impl()
{
#ifdef Q_OS_LINUX
    cancel();
    if (m_browser.state() != QProcess::NotRunning && !m_browser.waitForFinished(1200)) {
        QObject::disconnect(&m_browser, nullptr, &m_owner, nullptr);
        forceClose();
        m_browser.waitForFinished(500);
    }
    closeChannels();
#endif
}

bool ChromeSignin::Impl::start(const QUrl &address, bool background)
{
    const auto portal = signin::portalOrigin(address);
    if (portal.isEmpty()) {
        emit m_owner.failed(QStringLiteral("Укажите HTTPS-адрес портала."));
        return false;
    }
#ifdef Q_OS_LINUX
    if (m_lock) {
        m_restart = portal;
        m_restartBackground = background;
        stop();
        return true;
    }
    const auto executable = chromeExecutable();
    if (executable.isEmpty()) {
        emit m_owner.failed(QStringLiteral("Установите Google Chrome или Chromium для входа."));
        return false;
    }
    const auto data = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const auto profile = chromesignin::ensureChromeProfile(data);
    if (profile.isEmpty()) {
        emit m_owner.failed(QStringLiteral("Не удалось открыть отдельный профиль Chrome для Битрикс24."));
        return false;
    }
    auto lock = std::make_unique<QLockFile>(QDir(profile).filePath(QStringLiteral(".bitrix24-signin.lock")));
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0)) {
        emit m_owner.failed(QStringLiteral("Профиль Chrome уже используется другим окном входа."));
        return false;
    }
    if (!openBridge(m_writeFd, m_childReadFd) || !openBridge(m_readFd, m_childWriteFd)) {
        closeChannels();
        emit m_owner.failed(QStringLiteral("Не удалось подключить отдельное окно Chrome."));
        return false;
    }

    m_portal = portal;
    m_lock = std::move(lock);
    m_nextId = 0;
    m_startedAt.start();
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto *name : {"CHROME_USER_DATA_DIR", "CHROMIUM_FLAGS", "CHROME_FLAGS", "CHROME_LOG_FILE",
                             "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy", "https_proxy", "all_proxy"})
        environment.remove(QString::fromLatin1(name));
    m_browser.setProcessEnvironment(environment);
    m_browser.setStandardOutputFile(QProcess::nullDevice());
    m_browser.setStandardErrorFile(QProcess::nullDevice());
    const int browserInput = m_childReadFd;
    const int browserOutput = m_childWriteFd;
    m_browser.setChildProcessModifier([browserInput, browserOutput] {
        if (::setsid() < 0 || ::dup2(browserInput, 3) < 0 || ::dup2(browserOutput, 4) < 0)
            ::_exit(126);
        ::close(browserInput);
        ::close(browserOutput);
    });
    if (background)
        m_backgroundDeadline.start(ChromeSignin::backgroundDeadlineMs);
    m_browser.start(executable,
                    {QStringLiteral("--user-data-dir=%1").arg(profile),
                     QStringLiteral("--remote-debugging-pipe"),
                     QStringLiteral("--no-first-run"),
                     QStringLiteral("--no-default-browser-check"),
                     QStringLiteral("--disable-background-mode"),
                     background ? QStringLiteral("--headless") : QStringLiteral("--new-window"),
                     QStringLiteral("about:blank")});
    return true;
#else
    Q_UNUSED(background)
    emit m_owner.failed(QStringLiteral("Вход через Chrome пока доступен только на Linux."));
    return false;
#endif
}

void ChromeSignin::Impl::cancel()
{
#ifdef Q_OS_LINUX
    m_restart.clear();
    stop();
#endif
}

#ifdef Q_OS_LINUX
void ChromeSignin::Impl::launched()
{
    closeFd(m_childReadFd);
    closeFd(m_childWriteFd);
    if (m_closing) {
        forceClose();
        return;
    }
    m_reader = std::make_unique<QSocketNotifier>(m_readFd, QSocketNotifier::Read);
    m_writer = std::make_unique<QSocketNotifier>(m_writeFd, QSocketNotifier::Write);
    m_writer->setEnabled(false);
    QObject::connect(m_reader.get(), &QSocketNotifier::activated, &m_owner, [this] { receive(); });
    QObject::connect(m_writer.get(), &QSocketNotifier::activated, &m_owner, [this] { flush(); });
    m_poll.start();
    pump();
}

void ChromeSignin::Impl::pump()
{
    if (m_closing)
        return;
    if (m_startedAt.elapsed() > 8 * 60 * 1000 || (m_pageSession.isEmpty() && m_startedAt.elapsed() > 20000)
        || (m_pending != Command::None && m_commandAt.elapsed() > 25000)) {
        stop(QStringLiteral("Время ожидания входа в Chrome истекло."));
        return;
    }
    if (m_pending != Command::None || m_probe)
        return;
    if (m_pageSession.isEmpty()) {
        send(Command::FindPage, QStringLiteral("Target.getTargets"));
        return;
    }
    send(Command::Inspect, QStringLiteral("Runtime.evaluate"),
         {{QStringLiteral("expression"), chromesignin::csrfProbeExpression(m_portal)},
          {QStringLiteral("returnByValue"), true}}, true);
}

void ChromeSignin::Impl::send(Command command, const QString &method, const QJsonObject &params, bool page)
{
    m_pending = command;
    m_commandAt.start();
    QJsonObject message{{QStringLiteral("id"), ++m_nextId}, {QStringLiteral("method"), method},
                        {QStringLiteral("params"), params}};
    if (page)
        message.insert(QStringLiteral("sessionId"), m_pageSession);
    writeCommand(message);
}

void ChromeSignin::Impl::writeCommand(const QJsonObject &command)
{
    m_outgoing += QJsonDocument(command).toJson(QJsonDocument::Compact);
    m_outgoing += char(0);
    flush();
}

void ChromeSignin::Impl::flush()
{
    while (!m_outgoing.isEmpty() && m_writeFd != -1) {
        const auto written = ::send(m_writeFd, m_outgoing.constData(),
                                    static_cast<size_t>(m_outgoing.size()), MSG_NOSIGNAL);
        if (written > 0) {
            m_outgoing.remove(0, static_cast<qsizetype>(written));
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            m_writer->setEnabled(true);
            return;
        } else {
            m_writer->setEnabled(false);
            closeFd(m_writeFd);
            if (!m_closing)
                stop(QStringLiteral("Соединение с окном Chrome прервано."));
            return;
        }
    }
    if (m_writer)
        m_writer->setEnabled(false);
}

void ChromeSignin::Impl::receive()
{
    char data[16384];
    while (!m_closing && m_readFd != -1) {
        const auto count = ::read(m_readFd, data, sizeof(data));
        if (count > 0) {
            if (!m_frames.append(QByteArray(data, static_cast<qsizetype>(count)))) {
                stop(QStringLiteral("Chrome вернул слишком большой ответ."));
                return;
            }
            while (const auto frame = m_frames.take())
                dispatch(QJsonDocument::fromJson(*frame).object());
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        } else {
            m_reader->setEnabled(false);
            closeFd(m_readFd);
            stop(QStringLiteral("Chrome закрыл канал входа. Если окно уже открыто, закройте его и повторите."));
        }
    }
}

void ChromeSignin::Impl::dispatch(const QJsonObject &message)
{
    if (m_closing || m_pending == Command::None || message.value(QStringLiteral("id")).toInt() != m_nextId)
        return;
    const auto command = std::exchange(m_pending, Command::None);
    const auto result = message.value(QStringLiteral("result")).toObject();
    if (const auto error = message.value(QStringLiteral("error")); !error.isUndefined()) {
        if (command == Command::Navigate || command == Command::Attach)
            stop(QStringLiteral("Chrome не смог открыть портал."));
        else if (error.toObject().value(QStringLiteral("code")).toInt() == -32001)
            stop(QStringLiteral("Вкладка портала была закрыта до завершения входа."));
        return;
    }

    switch (command) {
    case Command::FindPage:
        for (const auto &entry : result.value(QStringLiteral("targetInfos")).toArray()) {
            const auto target = entry.toObject();
            if (target.value(QStringLiteral("type")) != QLatin1String("page")
                || target.value(QStringLiteral("url")) != QLatin1String("about:blank"))
                continue;
            send(Command::Attach, QStringLiteral("Target.attachToTarget"),
                 {{QStringLiteral("targetId"), target.value(QStringLiteral("targetId"))},
                  {QStringLiteral("flatten"), true}});
            break;
        }
        break;
    case Command::Attach: {
        m_pageSession = result.value(QStringLiteral("sessionId")).toString();
        if (m_pageSession.isEmpty()) {
            stop(QStringLiteral("Chrome не смог открыть портал."));
            break;
        }
        auto address = m_portal;
        address.setPath(QStringLiteral("/online/"));
        send(Command::Navigate, QStringLiteral("Page.navigate"),
             {{QStringLiteral("url"), address.toString(QUrl::FullyEncoded)}}, true);
        break;
    }
    case Command::Navigate:
        if (!result.value(QStringLiteral("errorText")).toString().isEmpty())
            stop(QStringLiteral("Chrome не смог загрузить портал."));
        break;
    case Command::Inspect: {
        m_csrf = result.value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("value")).toString().toLatin1();
        if (!signin::validCsrf(m_csrf))
            break;
        auto ajax = m_portal;
        auto pull = m_portal;
        ajax.setPath(QStringLiteral("/bitrix/services/main/ajax.php"));
        pull.setPath(QStringLiteral("/rest/pull.config.get.json"));
        send(Command::Cookies, QStringLiteral("Network.getCookies"),
             {{QStringLiteral("urls"), QJsonArray{ajax.toString(QUrl::FullyEncoded),
                                                  pull.toString(QUrl::FullyEncoded)}}}, true);
        break;
    }
    case Command::Cookies: {
        auto cookie = chromesignin::portalCookieHeader(m_portal, result.value(QStringLiteral("cookies")).toArray());
        if (!cookie.isEmpty())
            verify({m_portal, std::move(cookie), std::exchange(m_csrf, {})});
        break;
    }
    case Command::None:
        break;
    }
}

void ChromeSignin::Impl::verify(Session candidate)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(candidate.csrf);
    hash.addData(candidate.cookie);
    const auto fingerprint = hash.result();
    if (m_rejectedProbe == fingerprint && m_rejectedAt.elapsed() < probeRetryDelay)
        return;

    auto *reply = m_http.post(signin::authorizationProbe(candidate.origin, candidate.cookie, candidate.csrf),
                              signin::authorizationProbeBody(candidate.csrf));
    m_probe = reply;
    reply->setReadBufferSize(maxProbeSize + 1);
    QObject::connect(reply, &QNetworkReply::readyRead, &m_owner, [reply] {
        if (reply->bytesAvailable() > maxProbeSize)
            reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::finished, &m_owner,
                     [this, reply, fingerprint, candidate = std::move(candidate)] {
        reply->deleteLater();
        if (m_probe != reply)
            return;
        m_probe.clear();
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() == QNetworkReply::NoError && reply->bytesAvailable() <= maxProbeSize
            && signin::authenticatedResponse(status, reply->readAll())) {
            const auto verified = signin::updatedSession(candidate, *reply);
            if (verified.isValid()) {
                stop({}, verified);
                return;
            }
        }
        m_rejectedProbe = fingerprint;
        m_rejectedAt.start();
    });
}

void ChromeSignin::Impl::stop(QString error, std::optional<Session> session)
{
    if (!m_lock)
        return;
    m_error = std::move(error);
    m_authorized = std::move(session);
    if (m_closing)
        return;
    m_closing = true;
    m_poll.stop();
    m_backgroundDeadline.stop();
    if (m_reader)
        m_reader->setEnabled(false);
    auto *request = m_probe.data();
    m_probe.clear();
    if (request)
        request->abort();
    if (m_browser.state() == QProcess::NotRunning) {
        finish();
        return;
    }
    if (m_browser.state() == QProcess::Running && m_writeFd != -1)
        writeCommand({{QStringLiteral("id"), ++m_nextId}, {QStringLiteral("method"), QStringLiteral("Browser.close")}});
    m_killTimer.start();
}

void ChromeSignin::Impl::forceClose()
{
    if (m_browser.state() != QProcess::NotRunning) {
        const auto pid = static_cast<pid_t>(m_browser.processId());
        // The process modifier created an independent session for this window.
        if (pid > 0)
            ::kill(-pid, SIGKILL);
        m_browser.kill();
    }
}

void ChromeSignin::Impl::finish()
{
    m_poll.stop();
    m_killTimer.stop();
    m_backgroundDeadline.stop();
    closeChannels();
    if (m_probe) {
        auto *request = m_probe.data();
        m_probe.clear();
        request->abort();
    }
    // Chrome itself retains its profile. Release our exclusive lock only
    // after the process exits and flushes the browser’s data.
    m_lock.reset();
    m_pageSession.clear();
    m_csrf.clear();
    m_rejectedProbe.clear();
    m_rejectedAt.invalidate();
    m_pending = Command::None;
    m_closing = false;

    if (!m_restart.isEmpty()) {
        m_authorized.reset();
        m_error.clear();
        start(std::exchange(m_restart, {}), m_restartBackground);
    } else if (const auto session = std::exchange(m_authorized, std::nullopt)) {
        emit m_owner.sessionReady(*session);
    } else if (const auto error = std::exchange(m_error, {}); !error.isEmpty()) {
        emit m_owner.failed(error);
    }
}

void ChromeSignin::Impl::closeChannels()
{
    m_reader.reset();
    m_writer.reset();
    closeFd(m_readFd);
    closeFd(m_writeFd);
    closeFd(m_childReadFd);
    closeFd(m_childWriteFd);
    m_frames.clear();
    m_outgoing.clear();
}
#endif

ChromeSignin::ChromeSignin(QObject *parent) : QObject(parent), m_impl(std::make_unique<Impl>(*this)) {}
ChromeSignin::~ChromeSignin() = default;

bool ChromeSignin::start(const QUrl &portal) { return m_impl->start(portal, false); }
bool ChromeSignin::restoreInBackground(const QUrl &portal) { return m_impl->start(portal, true); }
void ChromeSignin::cancel() { m_impl->cancel(); }

} // namespace bx
