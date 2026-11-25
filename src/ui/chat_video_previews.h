#pragma once

#include "ui/chat_image_previews.h"

#include <QCache>
#include <QObject>
#include <QPointer>
#include <QUrl>

#include <array>
#include <functional>
#include <memory>

class QPixmap;
class QTemporaryDir;
class QWidget;

namespace bx {
class Backend;
struct MessageFile;

namespace ui {

class ChatVideoPreviews final : public QObject {
public:
    using Changed = std::function<void(bool posterChanged)>;
    using ShowError = std::function<void(QWidget *, const QString &)>;
    ChatVideoPreviews(Backend *backend, Changed changed, ShowError showError);
    ~ChatVideoPreviews() override;

    void request(const MessageFile &file);
    void play(const MessageFile &file, QWidget *window, std::function<bool()> stillVisible);
    const QPixmap *poster(const MessageFile &file) const;
    bool posterPending(const MessageFile &file) const;

private:
    struct Entry;
    struct Job;
    struct Playback {
        QPointer<QWidget> window;
        std::function<bool()> stillVisible;
    };

    bool authorized(const QUrl &url) const;
    void begin(const MessageFile &file, Playback playback);
    void startPoster(const std::shared_ptr<Job> &job);
    void finishPoster(const std::shared_ptr<Job> &job, bool success);
    void dispatch(const Entry &entry, const Playback &playback);
    void finish(const std::shared_ptr<Job> &job, const QString &error = {});
    void cancel();
    void reset();

    Backend *m_backend;
    Changed m_changed;
    ShowError m_showError;
    QString m_ffmpeg;
    QCache<QUrl, Entry> m_entries;
    ChatImagePreviews m_serverPosters;
    QCache<QUrl, bool> m_attempted{128};
    std::shared_ptr<Job> m_active;
    std::array<std::shared_ptr<QTemporaryDir>, 2> m_lastPlaybacks;
};

} // namespace ui
} // namespace bx
