#pragma once

#include "ui/chat_video_previews.h"

#include <QImage>
#include <QThreadPool>

class QDialog;
class QFileDialog;
class QListWidget;

namespace bx::ui {

// The chat, task card and notifications share the same in-flight downloads and previews.
class MessageFiles final : public QObject {
public:
    using DownloadStatus = std::function<void(QWidget *, const QString &, bool)>;

    MessageFiles(Backend *backend, ChatVideoPreviews::Changed changed,
                 ChatVideoPreviews::ShowError videoError, DownloadStatus downloadStatus);
    ~MessageFiles() override;

    void bindMessageFiles(QListWidget *list);
    void openMessagePhoto(const MessageFile &file, QWidget *parent, const QVector<MessageFile> &loaded);
    void playMessageVideo(const MessageFile &file, QWidget *parent, std::function<bool()> stillVisible);
    void saveMessageFile(const MessageFile &file, QWidget *parent, const QImage &preview = {});
    void downloadFailed(const QString &reason);
    ChatImagePreviews &imagePreviews() { return m_chatImagePreviews; }
    void requestVideo(const MessageFile &file) { m_chatVideoPreviews.request(file); }

private:
    Backend *m_backend;
    DownloadStatus m_downloadStatus;
    ChatImagePreviews m_chatImagePreviews;
    ChatVideoPreviews m_chatVideoPreviews;
    QPointer<QFileDialog> m_messageSaveDialog;
    QUrl m_messageDownloadUrl;
    QString m_messageDownloadPath;
    QPointer<QWidget> m_messageDownloadParent;
    QPointer<QDialog> m_chatPhoto;
    QThreadPool m_chatPhotoDecodePool;
};

} // namespace bx::ui
