#include "signin/session_store.h"

#include "signin/session_capture.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <QSettings>
#include <QThread>
#include <algorithm>
#include <cstring>

namespace bx::signin {
namespace {

constexpr qsizetype payloadLimit = 64 * 1024;
constexpr int keyringDeadlineMs = 3000;

QString restoreBlockKey(const QUrl &origin)
{
    if (origin.isEmpty())
        return {};
    const auto hash = QCryptographicHash::hash(origin.toEncoded(), QCryptographicHash::Sha256);
    return QStringLiteral("connection/blockedRestore/") + QString::fromLatin1(hash.toHex());
}

bool safeCookieHeader(const QByteArray &cookie)
{
    return !cookie.isEmpty() && cookie.size() < 16384
        && std::all_of(cookie.cbegin(), cookie.cend(), [](unsigned char character) {
            return character >= 32 && character <= 126;
        });
}

#if defined(Q_OS_LINUX)
constexpr auto schemaName = "local.bitrix24client.PortalSession";
constexpr auto keyName = "origin";

class SecretApi {
public:
    SecretApi()
    {
        // Use distribution libraries, never search the application or working directory.
        m_library.setLoadHints(QLibrary::PreventUnloadHint);
        for (const auto *root : {"/usr/lib", "/usr/lib64", "/lib", "/lib64"}) {
            QDir dir(QString::fromLatin1(root));
            const auto multiarchDirs = dir.entryList({QStringLiteral("*-linux-*")}, QDir::Dirs);
            QStringList paths{dir.filePath(QStringLiteral("libsecret-1.so.0"))};
            for (const auto &arch : multiarchDirs)
                paths.append(dir.filePath(arch + QStringLiteral("/libsecret-1.so.0")));
            for (const auto &path : paths) {
                if (!QFileInfo::exists(path))
                    continue;
                m_library.setFileName(path);
                if (m_library.load())
                    break;
            }
            if (m_library.isLoaded())
                break;
        }
        if (!m_library.isLoaded())
            return;

        schemaNew = resolve<decltype(schemaNew)>("secret_schema_new");
        schemaUnref = resolve<decltype(schemaUnref)>("secret_schema_unref");
        lookup = resolve<decltype(lookup)>("secret_password_lookup_sync");
        store = resolve<decltype(store)>("secret_password_store_sync");
        clear = resolve<decltype(clear)>("secret_password_clear_sync");
        search = resolve<decltype(search)>("secret_password_search_sync");
        listFree = resolve<decltype(listFree)>("g_list_free_full");
        passwordFree = resolve<decltype(passwordFree)>("secret_password_free");
        cancellableNew = resolve<decltype(cancellableNew)>("g_cancellable_new");
        cancel = resolve<decltype(cancel)>("g_cancellable_cancel");
        errorFree = resolve<decltype(errorFree)>("g_error_free");
        objectUnref = resolve<decltype(objectUnref)>("g_object_unref");
    }

    bool available() const
    {
        return schemaNew && schemaUnref && lookup && store && passwordFree && cancellableNew
            && cancel && errorFree && objectUnref;
    }

    bool canForget() const { return clear && search && listFree; }

    void *(*schemaNew)(const char *, int, ...) = nullptr;
    void (*schemaUnref)(void *) = nullptr;
    char *(*lookup)(const void *, void *, void **, ...) = nullptr;
    int (*store)(const void *, const char *, const char *, const char *, void *, void **, ...) = nullptr;
    int (*clear)(const void *, void *, void **, ...) = nullptr;
    void *(*search)(const void *, int, void *, void **, ...) = nullptr;
    void (*listFree)(void *, void (*)(void *)) = nullptr;
    void (*passwordFree)(char *) = nullptr;
    void *(*cancellableNew)() = nullptr;
    void (*cancel)(void *) = nullptr;
    void (*errorFree)(void *) = nullptr;
    void (*objectUnref)(void *) = nullptr;

private:
    template <typename Function>
    Function resolve(const char *name)
    {
        return reinterpret_cast<Function>(m_library.resolve(name));
    }

    QLibrary m_library;
};
#endif

} // namespace

bool blockSessionRestore(QSettings &settings, const QUrl &portal)
{
    const auto origin = portalOrigin(portal);
    const auto marker = restoreBlockKey(origin);
    settings.sync();
    if (marker.isEmpty() || settings.status() != QSettings::NoError)
        return false;

    settings.setValue(marker, true);
    const auto lastPortal = QLatin1String(lastPortalPreferenceKey);
    const auto remembered = QUrl(settings.value(lastPortal).toString(), QUrl::StrictMode);
    if (portalOrigin(remembered) == origin)
        settings.remove(lastPortal);
    settings.sync();
    return settings.status() == QSettings::NoError;
}

bool isSessionRestoreBlocked(QSettings &settings, const QUrl &portal)
{
    const auto marker = restoreBlockKey(portalOrigin(portal));
    settings.sync();
    return marker.isEmpty() || settings.status() != QSettings::NoError || settings.contains(marker);
}

void clearSessionRestoreBlock(QSettings &settings, const QUrl &portal)
{
    const auto marker = restoreBlockKey(portalOrigin(portal));
    if (!marker.isEmpty()) {
        settings.remove(marker);
        settings.sync();
    }
}

QByteArray storedSessionPayload(const Session &session)
{
    const auto origin = portalOrigin(session.origin);
    if (origin.isEmpty() || !safeCookieHeader(session.cookie) || !validCsrf(session.csrf))
        return {};

    return QJsonDocument(QJsonObject{
        {QStringLiteral("version"), 1},
        {QStringLiteral("origin"), origin.toString(QUrl::FullyEncoded)},
        {QStringLiteral("cookie"), QString::fromLatin1(session.cookie.toBase64())},
        {QStringLiteral("csrf"), QString::fromLatin1(session.csrf.toBase64())}
    }).toJson(QJsonDocument::Compact);
}

Session sessionFromStoredPayload(const QByteArray &payload, const QUrl &requestedPortal)
{
    const auto origin = portalOrigin(requestedPortal);
    if (origin.isEmpty() || payload.size() > payloadLimit)
        return {};

    const auto object = QJsonDocument::fromJson(payload).object();
    if (object.size() != 4 || object.value(QStringLiteral("version")) != QJsonValue(1)
        || object.value(QStringLiteral("origin")).toString() != origin.toString(QUrl::FullyEncoded)) {
        return {};
    }
    const auto cookie = QByteArray::fromBase64Encoding(object.value(QStringLiteral("cookie")).toString().toLatin1(),
                                                      QByteArray::AbortOnBase64DecodingErrors);
    const auto csrf = QByteArray::fromBase64Encoding(object.value(QStringLiteral("csrf")).toString().toLatin1(),
                                                    QByteArray::AbortOnBase64DecodingErrors);
    if (!cookie || !csrf || !safeCookieHeader(cookie.decoded) || !validCsrf(csrf.decoded))
        return {};
    return {origin, cookie.decoded, csrf.decoded};
}

struct SessionStore::Job {
    Operation operation = Operation::Load;
    bool succeeded = false;
    QByteArray result;
#if defined(Q_OS_LINUX)
    std::shared_ptr<SecretApi> api;
    std::shared_ptr<void> cancellable;
#endif
};

SessionStore::SessionStore(QObject *parent) : QObject(parent)
{
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        if (!m_job)
            return;
        const auto operation = m_job->operation;
        cancel();
        report(operation, false);
    });
}

SessionStore::~SessionStore()
{
    cancel();
}

void SessionStore::load(const QUrl &portal)
{
    start(portalOrigin(portal), Operation::Load);
}

void SessionStore::save(const Session &session)
{
    start(portalOrigin(session.origin), Operation::Save, storedSessionPayload(session));
}

void SessionStore::forget(const QUrl &portal)
{
    start(portalOrigin(portal), Operation::Forget);
}

void SessionStore::cancel()
{
    m_deadline.stop();
#if defined(Q_OS_LINUX)
    if (m_job && m_job->cancellable)
        m_job->api->cancel(m_job->cancellable.get());
#endif
    m_job.reset();
}

void SessionStore::start(const QUrl &portal, Operation operation, QByteArray payload)
{
    cancel();
    const auto job = std::make_shared<Job>();
    job->operation = operation;
    m_job = job;

#if defined(Q_OS_LINUX)
    if (!portal.isEmpty() && (operation != Operation::Save || !payload.isEmpty())) {
        static const auto api = std::make_shared<SecretApi>();
        if (api->available() && (operation != Operation::Forget || api->canForget())) {
            job->api = api;
            job->cancellable.reset(api->cancellableNew(), api->objectUnref);
            const auto key = portal.toEncoded();
            auto *thread = QThread::create([job, key, payload = std::move(payload)] {
                const auto &secret = *job->api;
                // SecretSchemaFlags::NONE and SecretSchemaAttributeType::STRING are zero.
                auto *schema = secret.schemaNew(schemaName, 0, keyName, 0, static_cast<const char *>(nullptr));
                if (!schema)
                    return;
                void *error = nullptr;
                auto *cancellable = job->cancellable.get();
                if (job->operation == Operation::Save) {
                    job->succeeded = secret.store(schema, nullptr, "Bitrix24 client session", payload.constData(),
                                                  cancellable, &error, keyName, key.constData(),
                                                  static_cast<const char *>(nullptr)) != 0;
                } else if (job->operation == Operation::Forget) {
                    secret.clear(schema, cancellable, &error, keyName, key.constData(),
                                 static_cast<const char *>(nullptr));
                    if (!error) {
                        // clear removes unlocked items only. Search metadata without unlocking
                        // or loading the password to tell whether any items remain.
                        auto *remaining = secret.search(schema, 0, cancellable, &error, keyName,
                                                        key.constData(), static_cast<const char *>(nullptr));
                        job->succeeded = !error && !remaining;
                        if (remaining)
                            secret.listFree(remaining, secret.objectUnref);
                    }
                } else {
                    auto *value = secret.lookup(schema, cancellable, &error, keyName, key.constData(),
                                                static_cast<const char *>(nullptr));
                    if (value) {
                        const auto size = std::strlen(value);
                        if (size <= static_cast<size_t>(payloadLimit))
                            job->result = QByteArray(value, static_cast<qsizetype>(size));
                        secret.passwordFree(value);
                    }
                }
                if (error)
                    secret.errorFree(error);
                secret.schemaUnref(schema);
            });
            connect(thread, &QThread::finished, this, [this, job] { finish(job); });
            connect(thread, &QThread::finished, thread, &QObject::deleteLater);
            m_deadline.start(keyringDeadlineMs);
            thread->start();
            return;
        }
    }
#else
    Q_UNUSED(payload);
    // This platform never persists portal sessions.
    job->succeeded = operation == Operation::Forget && !portal.isEmpty();
#endif
    QTimer::singleShot(0, this, [this, job] { finish(job); });
}

void SessionStore::finish(const std::shared_ptr<Job> &job)
{
    if (m_job != job)
        return;
    m_deadline.stop();
    m_job.reset();
    report(job->operation, job->succeeded, job->result);
}

void SessionStore::report(Operation operation, bool succeeded, const QByteArray &payload)
{
    switch (operation) {
    case Operation::Load: emit loaded(payload); break;
    case Operation::Save: emit saved(succeeded); break;
    case Operation::Forget: emit forgotten(succeeded); break;
    }
}

} // namespace bx::signin
