#include "ui/message_files.h"

#include "core/backend.h"
#include "core/download_target.h"
#include "ui/chat_photo_viewer.h"
#include "ui/chat_widgets.h"
#include "ui/message_menu_window.h"
#include "ui/saved_file_offer.h"
#include "ui/workspace_presentation.h"
#include "ui/workspace_style.h"

#include <QFileDialog>
#include <QFutureWatcher>
#include <QImageReader>
#include <QMenu>
#include <QPromise>
#include <QSaveFile>
#include <QTemporaryDir>

#include <algorithm>
#include <utility>

namespace bx::ui {
namespace {

struct PhotoLoad {
    std::shared_ptr<QTemporaryDir> directory;
    QString target;
    QUrl source;
    QFuture<QImage> decode;
    bool downloading = true;
};

struct PhotoGallery {
    QVector<MessageFile> files;
    qsizetype index = 0;
    std::shared_ptr<PhotoLoad> load;
    QUrl pendingPreview;
};

QStringList photoKeys(const MessageFile &file)
{
    QStringList keys;
    if (!file.diskObjectId.isEmpty())
        keys.append(QStringLiteral("disk:") + file.diskObjectId);
    if (!file.downloadUrl.isEmpty())
        keys.append(QStringLiteral("download:") + file.downloadUrl.toString(QUrl::FullyEncoded));
    if (!file.id.isEmpty())
        keys.append(QStringLiteral("file:") + file.id);
    if (keys.isEmpty() && !file.previewUrl.isEmpty())
        keys.append(QStringLiteral("preview:") + file.previewUrl.toString(QUrl::FullyEncoded));
    return keys;
}

PhotoGallery photoGallery(const MessageFile &opened, const QVector<MessageFile> &loaded)
{
    PhotoGallery gallery;
    QHash<QString, qsizetype> indexes;
    const auto indexOf = [&indexes](const QStringList &keys) {
        for (const auto &key : keys)
            if (const auto it = indexes.constFind(key); it != indexes.cend())
                return it.value();
        return qsizetype(-1);
    };
    for (const auto &file : loaded) {
        if (file.isVideo || (file.downloadUrl.isEmpty() && file.previewUrl.isEmpty())
            || (!file.isImage && file.previewUrl.isEmpty()))
            continue;
        const auto keys = photoKeys(file);
        auto index = indexOf(keys);
        if (index < 0) {
            index = gallery.files.size();
            gallery.files.append(file);
        } else if (gallery.files[index].downloadUrl.isEmpty() && !file.downloadUrl.isEmpty()) {
            gallery.files[index] = file;
        }
        for (const auto &key : keys)
            if (!indexes.contains(key))
                indexes.insert(key, index);
    }
    gallery.index = indexOf(photoKeys(opened));
    if (gallery.index < 0) {
        gallery.index = gallery.files.size();
        gallery.files.append(opened);
    }
    return gallery;
}

QImage readMessagePhoto(const QString &path)
{
    constexpr qint64 maxEncodedSize = 24 * 1024 * 1024;
    constexpr qint64 maxPixels = 16 * 1024 * 1024;
    const auto bytes = QFileInfo(path).size();
    if (bytes <= 0 || bytes > maxEncodedSize)
        return {};
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);
    const auto format = reader.format();
    if (format != "jpeg" && format != "png" && format != "webp" && format != "gif")
        return {};
    const auto size = reader.size();
    if (size.isEmpty() || size.width() > 8192 || size.height() > 8192 ||
        qint64(size.width()) * size.height() > maxPixels)
        return {};
    return reader.read();
}

} // namespace

MessageFiles::MessageFiles(Backend *backend, ChatVideoPreviews::Changed changed,
                           ChatVideoPreviews::ShowError videoError, DownloadStatus downloadStatus)
    : m_backend(backend), m_downloadStatus(std::move(downloadStatus)),
      m_chatVideoPreviews(backend, changed, std::move(videoError))
{
    m_chatPhotoDecodePool.setMaxThreadCount(1);
    connect(m_backend, &Backend::messageFileDownloaded, this, [this](const QUrl &url, const QString &path) {
        if (url != m_messageDownloadUrl || path != m_messageDownloadPath)
            return;
        m_messageDownloadUrl.clear();
        m_messageDownloadPath.clear();
        auto parent = std::exchange(m_messageDownloadParent, {});
        const auto notice = ui::plainTooltip(QStringLiteral("Файл сохранён: %1").arg(QFileInfo(path).fileName()));
        m_downloadStatus(parent, notice, false);
        ui::offerToOpenSavedFile(parent, path);
    });
    connect(m_backend, &Backend::messageImageLoaded, this, [this, changed](const QUrl &url, const QByteArray &bytes) {
        if (!m_chatImagePreviews.complete(url, bytes))
            return;
        changed(m_chatImagePreviews.photo(url) != nullptr);
    });
}

MessageFiles::~MessageFiles()
{
    if (m_chatPhoto)
        m_chatPhoto->close();
}

void MessageFiles::downloadFailed(const QString &reason)
{
    if (m_messageDownloadUrl.isEmpty())
        return;
    m_messageDownloadUrl.clear();
    m_messageDownloadPath.clear();
    m_downloadStatus(std::exchange(m_messageDownloadParent, {}), reason, false);
}

void MessageFiles::bindMessageFiles(QListWidget *list)
{
    auto *delegate = static_cast<workspace::RowPainter *>(list->itemDelegate());
    delegate->setMessagePreviewSource([this](const QUrl &url) { return m_chatImagePreviews.photo(url); });
    delegate->setVideoPreviewSource([this](const MessageFile &file) { return m_chatVideoPreviews.poster(file); });
    delegate->setPreviewPendingSource([this](const MessageFile &file) {
        if (file.isVideo)
            return m_chatVideoPreviews.posterPending(file);
        return m_chatImagePreviews.pending(m_chatImagePreviews.requestUrl(file.previewUrl, file.originalPreviewUrl));
    });
    delegate->setMessageVideoHandler([this, list](const QModelIndex &index, const MessageFile &file) {
        playMessageVideo(file, list->window(), [list = QPointer<QListWidget>(list),
                         current = QPersistentModelIndex(index), file] {
            return list && list->isVisible() && current.isValid()
                && workspace::RowPainter::hasOriginal(current, file);
        });
    });
    delegate->setMessagePhotoHandler([this, list](const MessageFile &file) {
        QVector<MessageFile> loaded;
        if (dynamic_cast<ui::HistoryList *>(list)) {
            for (int row = 0; row < list->count(); ++row) {
                const auto *item = list->item(row);
                if (item->isHidden())
                    continue;
                loaded += item->data(workspace::MessageFiles).value<QVector<MessageFile>>();
            }
        }
        openMessagePhoto(file, list->window(), loaded);
    });
    const auto open = [this, list](const MessageFile &file) { saveMessageFile(file, list->window()); };
    delegate->setMessageDownloadHandler(open);
    delegate->setMessagePhotoMenuHandler([this, list, delegate](const QModelIndex &index, const MessageFile &file,
                                                      const QRect &anchor) {
        const auto *thumb = delegate->filePreview(file);
        const auto pixels = thumb && !file.isVideo ? thumb->toImage() : QImage();
        const auto samePhoto = [this, list, current = QPersistentModelIndex(index), file,
                                account = m_backend->settingsScope()] {
            return account == m_backend->settingsScope() && list->isVisible() && current.isValid()
                && workspace::RowPainter::hasPhotoPreview(current, file);
        };
        std::function<void()> download;
        if (workspace::RowPainter::downloadable(file) || !pixels.isNull()) {
            download = [this, list, samePhoto, file, pixels] {
                if (samePhoto())
                    saveMessageFile(file, list->window(), pixels);
            };
        }
        std::function<QImage()> copy;
        if (!pixels.isNull())
            copy = [samePhoto, pixels] { return samePhoto() ? pixels : QImage(); };
        QMenu *menu = nullptr;
        if (file.isVideo) {
            menu = new QMenu(list);
            connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
            auto *save = menu->addAction(QStringLiteral("Сохранить видео"));
            save->setEnabled(bool(download));
            if (download)
                connect(save, &QAction::triggered, menu, std::move(download));
        } else {
            menu = ui::createImageContextMenu(list, std::move(download), std::move(copy));
        }
        connect(m_backend, &Backend::accountScopeChanged, menu, &QMenu::close);
        connect(list->model(), &QAbstractItemModel::modelReset, menu, &QMenu::close);
        const auto closeIfStale = [menu, samePhoto] {
            if (!samePhoto())
                menu->close();
        };
        connect(list->model(), &QAbstractItemModel::rowsRemoved, menu, closeIfStale);
        connect(list->model(), &QAbstractItemModel::dataChanged, menu, closeIfStale);
        ui::showMessageMenuWindow(*menu, anchor);
    });
    connect(list, &QListWidget::itemActivated, this, [open, list, delegate](QListWidgetItem *item) {
        auto files = item->data(workspace::MessageFiles).value<QVector<MessageFile>>();
        const auto index = list->model()->index(list->row(item), 0);
        const auto photos = delegate->photoOriginals(index);
        files.erase(std::remove_if(files.begin(), files.end(), [&photos](const MessageFile &file) {
            return file.isVideo || file.downloadUrl.isEmpty() || std::any_of(photos.cbegin(), photos.cend(), [&file](const auto &photo) {
                return photo.downloadUrl == file.downloadUrl;
            });
        }), files.end());
        if (files.size() == 1) {
            open(files.front());
        } else if (!files.isEmpty()) {
            auto *menu = new QMenu(list);
            connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
            for (const auto &file : files)
                connect(menu->addAction(file.name), &QAction::triggered, list, [open, file] { open(file); });
            menu->popup(list->viewport()->mapToGlobal(list->visualItemRect(item).center()));
        } else if (!photos.isEmpty()) {
            delegate->activateMedia(index);
        }
    });
}

void MessageFiles::openMessagePhoto(const MessageFile &file, QWidget *parent, const QVector<MessageFile> &loaded)
{
    if (m_chatPhoto)
        return;
    const auto gallery = std::make_shared<PhotoGallery>(photoGallery(file, loaded));
    const auto scope = m_backend->settingsScope();
    auto *viewer = new ui::ChatPhotoDialog(parent, file.name, {});
    m_chatPhoto = viewer;
    const auto cancel = [backend = QPointer<Backend>(m_backend), gallery] {
        gallery->pendingPreview.clear();
        const auto load = std::exchange(gallery->load, {});
        if (!load)
            return;
        if (std::exchange(load->downloading, false) && backend)
            backend->cancelMessagePhoto(load->target);
        if (load->decode.isRunning())
            load->decode.cancel();
        load->directory.reset();
    };
    connect(viewer, &QDialog::finished, this, [this, viewer, cancel] {
        cancel();
        m_chatPhotoDecodePool.clear();
        if (m_chatPhoto == viewer)
            m_chatPhoto.clear();
    });
    connect(viewer, &QObject::destroyed, m_backend, cancel);
    connect(m_backend, &Backend::accountScopeChanged, viewer, &QDialog::reject);
    connect(m_backend, &Backend::messagePhotoDownloadFailed, viewer,
            [viewer, gallery](const QString &path, const QString &reason) {
        const auto load = gallery->load;
        if (!load || path != load->target || !std::exchange(load->downloading, false))
            return;
        load->directory.reset();
        viewer->showFailure(reason);
    });
    connect(m_backend, &Backend::messageFileDownloaded, viewer,
            [this, viewer, gallery, scope](const QUrl &url, const QString &path) {
        const auto load = gallery->load;
        if (!load || url != load->source || path != load->target || !std::exchange(load->downloading, false))
            return;
        auto promise = std::make_shared<QPromise<QImage>>();
        promise->start();
        load->decode = promise->future();
        auto *watcher = new QFutureWatcher<QImage>(viewer);
        connect(watcher, &QFutureWatcher<QImage>::finished, viewer,
                [this, viewer, watcher, load, gallery, scope] {
            const auto future = watcher->future();
            load->decode = QFuture<QImage>();
            if (m_chatPhoto == viewer && gallery->load == load &&
                m_backend->settingsScope() == scope && !future.isCanceled()) {
                const auto image = future.resultCount() > 0 ? future.result() : QImage();
                if (image.isNull())
                    viewer->showFailure(QStringLiteral("Формат или размер оригинала не поддерживается просмотром"));
                else
                    viewer->showOriginal(image);
            }
            watcher->deleteLater();
        });
        watcher->setFuture(load->decode);
        m_chatPhotoDecodePool.start([promise, directory = std::exchange(load->directory, {}), path]() mutable {
            if (!promise->isCanceled())
                promise->addResult(readMessagePhoto(path));
            directory.reset();
            promise->finish();
        });
    });
    const auto showPreview = [this, viewer, gallery, scope] {
        const auto url = gallery->pendingPreview;
        if (url.isEmpty() || m_chatPhoto != viewer || m_backend->settingsScope() != scope)
            return;
        if (const auto *preview = m_chatImagePreviews.photo(url)) {
            gallery->pendingPreview.clear();
            viewer->showOriginal(preview->toImage());
        } else if (m_chatImagePreviews.failed(url)) {
            gallery->pendingPreview.clear();
            viewer->showFailure(QStringLiteral("Не удалось загрузить миниатюру фотографии"));
        } else if (m_chatImagePreviews.begin(url)) {
            m_backend->requestMessageImage(url);
        }
    };
    // MessageFiles has already updated the shared preview cache when this handler runs.
    // Completions for other images can free a request slot for the selected preview.
    connect(m_backend, &Backend::messageImageLoaded, viewer, showPreview);
    const auto show = [this, viewer, gallery, scope, cancel, showPreview] {
        if (m_chatPhoto != viewer || m_backend->settingsScope() != scope)
            return;
        cancel();
        m_chatPhotoDecodePool.clear();
        const auto &file = gallery->files.at(gallery->index);
        const bool hasOriginal = workspace::RowPainter::downloadable(file);
        viewer->setPhoto(file.name, gallery->index, gallery->files.size(), hasOriginal);
        if (!hasOriginal) {
            const auto &url = file.previewUrl;
            if (!url.isValid() || url.scheme() != QStringLiteral("https") || url.host().isEmpty() ||
                !url.userInfo().isEmpty() || url.hasFragment()) {
                viewer->showFailure(QStringLiteral("Оригинал фотографии недоступен"));
            } else {
                gallery->pendingPreview = url;
                showPreview();
            }
            return;
        }
        auto load = std::make_shared<PhotoLoad>();
        load->directory = std::make_shared<QTemporaryDir>(QDir::temp().filePath(QStringLiteral("bitrix24-photo-XXXXXX")));
        if (!load->directory->isValid()) {
            viewer->showFailure(QStringLiteral("Не удалось подготовить временный файл для фотографии"));
            return;
        }
        load->target = load->directory->filePath(QStringLiteral("original"));
        load->source = file.downloadUrl;
        gallery->load = load;
        m_backend->downloadMessagePhoto(load->source, load->target);
    };
    viewer->setNavigationAction([gallery, show](int offset) {
        const auto index = gallery->index + offset;
        if (index >= 0 && index < gallery->files.size()) {
            gallery->index = index;
            show();
        }
    });
    viewer->setSaveAction([this, viewer, gallery, scope](const QImage &preview) {
        if (m_chatPhoto == viewer && m_backend->settingsScope() == scope)
            saveMessageFile(gallery->files.at(gallery->index), viewer, preview);
    });
    viewer->show();
    show();
}

void MessageFiles::playMessageVideo(const MessageFile &file, QWidget *parent, std::function<bool()> stillVisible)
{
    m_chatVideoPreviews.play(file, parent, std::move(stillVisible));
}

void MessageFiles::saveMessageFile(const MessageFile &file, QWidget *parent, const QImage &preview)
{
    const bool hasOriginal = workspace::RowPainter::downloadable(file);
    if ((!hasOriginal && preview.isNull()) || m_messageSaveDialog || !m_messageDownloadUrl.isEmpty())
        return;
    auto *dialog = new QFileDialog(parent, QStringLiteral("Сохранить вложение"));
    dialog->setObjectName(QStringLiteral("messageFileSaveDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->setFileMode(QFileDialog::AnyFile);
    auto name = ui::attachmentNames({file.name}).value(0);
    if (!hasOriginal) {
        dialog->setDefaultSuffix(QStringLiteral("png"));
        dialog->setNameFilter(QStringLiteral("Изображения PNG (*.png)"));
        const auto stem = QFileInfo(name).completeBaseName();
        name = (stem.isEmpty() || stem == QStringLiteral(".") || stem == QStringLiteral("..")
                ? QStringLiteral("Изображение") : stem) + QStringLiteral(".png");
    }
    if (!name.isEmpty() && name != QStringLiteral(".") && name != QStringLiteral(".."))
        dialog->selectFile(name);
    m_messageSaveDialog = dialog;
    connect(m_backend, &Backend::accountScopeChanged, dialog, &QDialog::reject);
    connect(dialog, &QDialog::finished, this, [this, dialog] {
        if (m_messageSaveDialog == dialog)
            m_messageSaveDialog = nullptr;
    });
    connect(dialog, &QFileDialog::fileSelected, this,
            [this, url = file.downloadUrl, parent = QPointer<QWidget>(parent),
             account = m_backend->settingsScope(), preview = hasOriginal ? QImage() : preview](const QString &path) {
        if (path.isEmpty() || account != m_backend->settingsScope())
            return;
        if (!preview.isNull()) {
            const auto target = QDir::cleanPath(path);
            QSaveFile output(target);
            output.setDirectWriteFallback(false);
            const bool opened = safeDownloadTarget(target) && output.open(QIODevice::WriteOnly);
            const bool encoded = opened && safeDownloadTarget(target) && preview.save(&output, "PNG");
            const bool saved = encoded && safeDownloadTarget(target) && output.commit();
            const auto status = saved ? QStringLiteral("Изображение сохранено")
                                      : QStringLiteral("Не удалось сохранить изображение");
            m_downloadStatus(parent, status, false);
            return;
        }
        m_messageDownloadUrl = url;
        m_messageDownloadPath = path;
        m_messageDownloadParent = parent;
        m_downloadStatus(parent, QStringLiteral("Скачиваем файл…"), true);
        m_backend->downloadMessageFile(url, path);
    });
    dialog->open();
}

} // namespace bx::ui
