#include "ui/chat_video_previews.h"

#include "core/backend.h"
#include "core/origin_guard.h"
#include "core/tunables.h"
#include "ui/chat_image_previews.h"

#include <QDesktopServices>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>

#include <utility>

namespace bx::ui {
namespace {

constexpr int posterTimeoutMs = 8000;
constexpr qsizetype posterLimit = 512 * 1024;
constexpr qsizetype cacheKib = 96 * 1024;
const QString playbackFailed = QStringLiteral("Не удалось открыть видео");

} // namespace

struct ChatVideoPreviews::Entry {
    std::shared_ptr<QTemporaryDir> directory;
    QString path;
    QPixmap poster;
    qint64 size = 0;
};

struct ChatVideoPreviews::Job {
    QUrl url;
    Entry entry;
    Playback playback;
    QPointer<QProcess> process;
    QByteArray png;
    qint64 expectedSize = -1;
    bool downloading = true;
};

ChatVideoPreviews::ChatVideoPreviews(Backend *backend, Changed changed, ShowError showError)
    : m_backend(backend), m_changed(std::move(changed)), m_showError(std::move(showError)),
      m_ffmpeg(QStandardPaths::findExecutable(QStringLiteral("ffmpeg"))), m_entries(cacheKib)
{
    connect(backend, &Backend::messageImageLoaded, this, [this](const QUrl &url, const QByteArray &bytes) {
        if (m_serverPosters.complete(url, bytes))
            m_changed(m_serverPosters.photo(url) != nullptr);
    });
    connect(backend, &Backend::messageVideoDownloaded, this, [this](const QUrl &url, const QString &path) {
        const auto job = m_active;
        if (!job || !job->downloading || job->url != url || job->entry.path != path)
            return;
        job->downloading = false;
        const QFileInfo local(path);
        const auto size = local.size();
        if (!local.isFile() || local.isSymLink() || size <= 0 || size > tunables::kChatVideoPlaybackBytes
            || (job->expectedSize >= 0 && size != job->expectedSize)) {
            finish(job, playbackFailed);
            return;
        }
        job->entry.size = size;
        dispatch(job->entry, std::exchange(job->playback, {}));
        if (m_active != job)
            return;
        if (size > tunables::kChatVideoPreviewBytes || m_ffmpeg.isEmpty()) {
            finish(job);
            return;
        }
        startPoster(job);
    });
    connect(backend, &Backend::messageVideoDownloadFailed, this, [this](const QString &path, const QString &error) {
        const auto job = m_active;
        if (job && job->downloading && job->entry.path == path)
            finish(job, error.isEmpty() ? playbackFailed : error);
    });
    connect(backend, &Backend::accountScopeChanged, this, &ChatVideoPreviews::reset);
    connect(backend, &Backend::messageLinkPreviewAccessChanged, this, [this](bool allowed) {
        if (!allowed)
            reset();
    });
}

ChatVideoPreviews::~ChatVideoPreviews()
{
    cancel();
}

bool ChatVideoPreviews::authorized(const QUrl &url) const
{
    return OriginGuard(m_backend->portalOrigin()).isAllowed(url);
}

void ChatVideoPreviews::request(const MessageFile &file)
{
    if (!file.isVideo)
        return;
    if (m_serverPosters.begin(file.previewUrl))
        m_backend->requestMessageImage(file.previewUrl);
    if ((!file.previewUrl.isEmpty() && !m_serverPosters.failed(file.previewUrl)) ||
        m_active || m_ffmpeg.isEmpty() || file.size <= 0 ||
        file.size > tunables::kChatVideoPreviewBytes || !authorized(file.downloadUrl) ||
        m_entries.contains(file.downloadUrl) || m_attempted.contains(file.downloadUrl))
        return;
    m_attempted.insert(file.downloadUrl, new bool(true));
    begin(file, {});
}

void ChatVideoPreviews::play(const MessageFile &file, QWidget *window, std::function<bool()> stillVisible)
{
    Playback playback{window, std::move(stillVisible)};
    if (!file.isVideo || !authorized(file.downloadUrl)) {
        m_showError(window, playbackFailed);
        return;
    }
    if (const auto *entry = m_entries.object(file.downloadUrl); entry && QFileInfo::exists(entry->path)) {
        dispatch(*entry, playback);
        return;
    }
    if (m_active && m_active->url == file.downloadUrl) {
        if (m_active->downloading)
            m_active->playback = std::move(playback);
        else
            dispatch(m_active->entry, playback);
        return;
    }
    cancel();
    begin(file, std::move(playback));
}

const QPixmap *ChatVideoPreviews::poster(const MessageFile &file) const
{
    if (const auto *server = m_serverPosters.photo(file.previewUrl))
        return server;
    const auto *entry = m_entries.object(file.downloadUrl);
    return entry && !entry->poster.isNull() ? &entry->poster : nullptr;
}

bool ChatVideoPreviews::posterPending(const MessageFile &file) const
{
    return m_serverPosters.pending(file.previewUrl)
        || (m_active && m_active->url == file.downloadUrl && !m_ffmpeg.isEmpty()
            && file.size > 0 && file.size <= tunables::kChatVideoPreviewBytes);
}

void ChatVideoPreviews::begin(const MessageFile &file, Playback playback)
{
    auto directory = std::make_shared<QTemporaryDir>();
    if (!directory->isValid()) {
        if (playback.window && playback.stillVisible && playback.stillVisible())
            m_showError(playback.window, playbackFailed);
        m_changed(false);
        return;
    }
    m_active = std::make_shared<Job>();
    m_active->url = file.downloadUrl;
    m_active->expectedSize = file.size;
    m_active->entry.path = directory->filePath(QStringLiteral("clip.mp4"));
    m_active->entry.directory = std::move(directory);
    m_active->playback = std::move(playback);
    const auto job = m_active;
    m_backend->downloadMessageVideo(job->url, job->entry.path);
}

void ChatVideoPreviews::startPoster(const std::shared_ptr<Job> &job)
{
    auto *process = new QProcess(this);
    job->process = process;
    process->setStandardErrorFile(QProcess::nullDevice());
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("FFREPORT"));
    process->setProcessEnvironment(environment);
    connect(process, &QProcess::readyReadStandardOutput, this, [this, job, process] {
        if (m_active != job)
            return;
        job->png.append(process->readAllStandardOutput());
        if (job->png.size() > posterLimit)
            process->kill();
    });
    connect(process, &QProcess::errorOccurred, this, [this, job](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finishPoster(job, false);
    });
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, job](int code, QProcess::ExitStatus status) {
        finishPoster(job, code == 0 && status == QProcess::NormalExit);
    });
    QTimer::singleShot(posterTimeoutMs, process, [process] {
        if (process->state() != QProcess::NotRunning)
            process->kill();
    });
    process->start(m_ffmpeg, {QStringLiteral("-nostdin"), QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-max_alloc"), QStringLiteral("67108864"), QStringLiteral("-filter_threads"), QStringLiteral("1"),
        QStringLiteral("-protocol_whitelist"), QStringLiteral("file"), QStringLiteral("-f"), QStringLiteral("mov"),
        QStringLiteral("-enable_drefs"), QStringLiteral("0"), QStringLiteral("-use_absolute_path"), QStringLiteral("0"),
        QStringLiteral("-threads"), QStringLiteral("1"), QStringLiteral("-max_pixels"), QStringLiteral("16777216"),
        QStringLiteral("-i"), job->entry.path, QStringLiteral("-map"), QStringLiteral("0:v:0"),
        QStringLiteral("-an"), QStringLiteral("-sn"), QStringLiteral("-dn"),
        QStringLiteral("-vf"), QStringLiteral("scale=480:270:force_original_aspect_ratio=decrease"),
        QStringLiteral("-frames:v"), QStringLiteral("1"), QStringLiteral("-c:v"), QStringLiteral("png"),
        QStringLiteral("-threads"), QStringLiteral("1"), QStringLiteral("-f"), QStringLiteral("image2pipe"),
        QStringLiteral("pipe:1")}, QIODevice::ReadOnly);
}

void ChatVideoPreviews::finishPoster(const std::shared_ptr<Job> &job, bool success)
{
    if (m_active != job)
        return;
    if (job->process) {
        if (success) {
            job->png.append(job->process->readAllStandardOutput());
            if (job->png.size() <= posterLimit)
                job->entry.poster = QPixmap::fromImage(decodeChatPreviewRaster(job->png, QSize(480, 270), "image/png"));
        }
        job->process->deleteLater();
    }
    finish(job);
}

void ChatVideoPreviews::dispatch(const Entry &entry, const Playback &playback)
{
    if (!playback.window || !playback.stillVisible || !playback.stillVisible())
        return;
    // The desktop player may open the local file asynchronously after dispatch.
    if (m_lastPlaybacks.front() != entry.directory) {
        m_lastPlaybacks.back() = std::move(m_lastPlaybacks.front());
        m_lastPlaybacks.front() = entry.directory;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(entry.path)))
        m_showError(playback.window, QStringLiteral("Не удалось запустить проигрыватель видео"));
}

void ChatVideoPreviews::finish(const std::shared_ptr<Job> &job, const QString &error)
{
    m_active.reset();
    if (!error.isEmpty() && job->playback.window && job->playback.stillVisible && job->playback.stillVisible())
        m_showError(job->playback.window, error);
    const bool cacheChanged = job->entry.size > 0;
    if (cacheChanged) {
        const auto pixels = qint64(job->entry.poster.width()) * job->entry.poster.height() * 4;
        m_entries.insert(job->url, new Entry(job->entry), (job->entry.size + pixels + 1023) / 1024);
    }
    m_changed(cacheChanged);
}

void ChatVideoPreviews::cancel()
{
    const auto job = std::exchange(m_active, {});
    if (!job)
        return;
    if (job->downloading)
        m_backend->cancelMessageVideo(job->entry.path);
    if (job->process) {
        job->process->disconnect(this);
        job->process->kill();
        job->process->deleteLater();
    }
}

void ChatVideoPreviews::reset()
{
    cancel();
    m_entries.clear();
    m_serverPosters.clear();
    m_attempted.clear();
    m_lastPlaybacks = {};
    m_changed(true);
}

} // namespace bx::ui
