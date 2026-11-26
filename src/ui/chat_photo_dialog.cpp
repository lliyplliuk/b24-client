#include "ui/chat_photo_dialog.h"

#include "ui/photo_viewer_input.h"
#include "ui/loading_indicator.h"
#include "ui/message_menu_window.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QScreen>
#include <QShortcut>
#include <QTimer>

namespace bx::ui {

ChatPhotoDialog::ChatPhotoDialog(QWidget *parent, const QString &name, SaveAction download)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("chatPhotoViewer"));
    setWindowTitle(name.isEmpty() ? QStringLiteral("Фотография") : name);
    setAttribute(Qt::WA_DeleteOnClose);
    if (parent)
        new PhotoViewerInput(*this);
    const auto *display = parent ? parent->screen() : screen();
    const auto available = display ? display->availableGeometry().size() : QSize(960, 620);
    resize(qMin(920, available.width() - 44), qMin(690, available.height() - 60));
    setStyleSheet(QStringLiteral(R"(
            QDialog#chatPhotoViewer { background: #21333b; color: #edf3f0; }
            QDialog#chatPhotoViewer QLabel { color: #edf3f0; }
            QDialog#chatPhotoViewer QPushButton {
                background: #2a434c; color: #edf3f0; border: 1px solid #4d6670;
                border-radius: 6px; padding: 6px 9px;
            }
            QDialog#chatPhotoViewer QPushButton:hover { background: #365662; }
            QDialog#chatPhotoViewer QPushButton:focus { border-color: #a4ccbc; }
            QDialog#chatPhotoViewer QPushButton:disabled { color: #9baeb1; }
        )"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(9, 9, 9, 9);
    m_viewport = new QStackedWidget(this);
    m_canvas = new ChatPhotoCanvas(m_viewport);
    m_state = new QLabel(QStringLiteral("Загружаем оригинал фотографии…"), m_viewport);
    m_state->setAccessibleName(QStringLiteral("Состояние загрузки фотографии"));
    m_state->setTextFormat(Qt::PlainText);
    m_state->setWordWrap(true);
    m_state->setAlignment(Qt::AlignCenter);
    m_state->setStyleSheet(QStringLiteral("background: #142329; padding: 16px;"));
    setLoadingIndicator(m_state, true);
    m_viewport->addWidget(m_canvas);
    m_viewport->addWidget(m_state);
    m_viewport->setCurrentWidget(m_state);
    for (auto *surface : {static_cast<QWidget *>(m_canvas), static_cast<QWidget *>(m_state)}) {
        surface->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(surface, &QWidget::customContextMenuRequested, this, [this, surface](const QPoint &point) {
            showImageMenu(surface->mapToGlobal(point));
        });
    }
    auto *photoRow = new QHBoxLayout;
    photoRow->setSpacing(6);
    m_previous = new QPushButton(QStringLiteral("‹"), this);
    m_previous->setAccessibleName(QStringLiteral("Предыдущая фотография"));
    m_previous->setAutoDefault(false);
    m_next = new QPushButton(QStringLiteral("›"), this);
    m_next->setAccessibleName(QStringLiteral("Следующая фотография"));
    m_next->setAutoDefault(false);
    photoRow->addWidget(m_previous);
    photoRow->addWidget(m_viewport, 1);
    photoRow->addWidget(m_next);
    layout->addLayout(photoRow, 1);
    const auto addNavigation = [this](QPushButton *button, Qt::Key key, int offset) {
        auto *shortcut = new QShortcut(QKeySequence(key), this);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        const auto navigate = [this, button, offset] {
            if (button->isEnabled() && m_navigate)
                m_navigate(offset);
        };
        QObject::connect(shortcut, &QShortcut::activated, this, navigate);
        QObject::connect(button, &QPushButton::clicked, this, navigate);
    };
    addNavigation(m_previous, Qt::Key_Left, -1);
    addNavigation(m_next, Qt::Key_Right, 1);
    auto *photo = m_canvas;
    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(6);
    auto *out = new QPushButton(QStringLiteral("−"), this);
    out->setAccessibleName(QStringLiteral("Уменьшить фотографию"));
    auto *in = new QPushButton(QStringLiteral("+"), this);
    in->setAccessibleName(QStringLiteral("Увеличить фотографию"));
    auto *fit = new QPushButton(QStringLiteral("По размеру"), this);
    fit->setAccessibleName(QStringLiteral("Вписать фотографию в окно"));
    auto *scale = new QLabel(this);
    scale->setAccessibleName(QStringLiteral("Масштаб фотографии"));
    toolbar->addWidget(out);
    toolbar->addWidget(in);
    toolbar->addWidget(fit);
    toolbar->addWidget(scale);
    toolbar->addStretch(1);
    m_position = new QLabel(this);
    m_position->setAccessibleName(QStringLiteral("Номер фотографии"));
    toolbar->addWidget(m_position);
    QObject::connect(out, &QPushButton::clicked, photo, [photo] { photo->zoomOut(); });
    QObject::connect(in, &QPushButton::clicked, photo, [photo] { photo->zoomIn(); });
    QObject::connect(fit, &QPushButton::clicked, photo, [photo] { photo->fit(); });
    m_refreshControls = [photo, out, in, fit, scale] {
        const bool ready = !photo->imageSize().isEmpty();
        out->setEnabled(ready && photo->canZoomOut());
        fit->setEnabled(ready && photo->canZoomOut());
        in->setEnabled(ready && photo->canZoomIn());
        scale->setText(ready ? QStringLiteral("×%1").arg(photo->zoomFactor(), 0, 'f', 2) : QString());
    };
    photo->setZoomChanged(m_refreshControls);
    m_refreshControls();
    m_save = new QPushButton(QStringLiteral("Сохранить оригинал"), this);
    m_save->setAccessibleName(QStringLiteral("Сохранить оригинал фотографии"));
    QObject::connect(m_save, &QPushButton::clicked, this, [this] {
        if (m_download)
            m_download({});
    });
    toolbar->addWidget(m_save);
    auto *close = new QPushButton(QStringLiteral("Закрыть"), this);
    QObject::connect(close, &QPushButton::clicked, this, &QDialog::reject);
    toolbar->addWidget(close);
    layout->addLayout(toolbar);
    setSaveAction(std::move(download));
    setPhoto(name, 0, 1, bool(m_download));
}

void ChatPhotoDialog::setNavigationAction(std::function<void(int)> navigate)
{
    m_navigate = std::move(navigate);
}

void ChatPhotoDialog::setSaveAction(SaveAction download)
{
    m_download = std::move(download);
}

void ChatPhotoDialog::setPhoto(const QString &name, qsizetype index, qsizetype count, bool hasOriginal)
{
    ++m_photoRevision;
    if (m_imageMenu)
        m_imageMenu->close();
    m_hasOriginal = hasOriginal;
    setWindowTitle(name.isEmpty() ? QStringLiteral("Фотография") : name);
    m_previous->setVisible(count > 1);
    m_next->setVisible(count > 1);
    m_position->setVisible(count > 1);
    m_previous->setEnabled(index > 0);
    m_next->setEnabled(index + 1 < count);
    m_position->setText(QStringLiteral("%1 из %2").arg(index + 1).arg(count));
    m_save->setVisible(hasOriginal && bool(m_download));
    m_canvas->setOriginal({});
    m_state->setText(QStringLiteral("Загружаем оригинал фотографии…"));
    setLoadingIndicator(m_state, true);
    m_viewport->setCurrentWidget(m_state);
    m_refreshControls();
}

void ChatPhotoDialog::showOriginal(QImage image)
{
    m_canvas->setOriginal(std::move(image));
    if (m_canvas->imageSize().isEmpty()) {
        showFailure(QStringLiteral("Не удалось отобразить оригинал фотографии"));
        return;
    }
    m_viewport->setCurrentWidget(m_canvas);
    setLoadingIndicator(m_state, false);
    m_refreshControls();
}

void ChatPhotoDialog::showFailure(const QString &reason)
{
    m_state->setText(reason);
    setLoadingIndicator(m_state, false);
    m_viewport->setCurrentWidget(m_state);
}

void ChatPhotoDialog::showImageMenu(const QPoint &point)
{
    if (m_imageMenu)
        m_imageMenu->close();
    const auto samePhoto = [this, revision = m_photoRevision] { return isVisible() && revision == m_photoRevision; };
    const bool ready = m_canvas->isVisible() && !m_canvas->image().isNull();
    std::function<void()> download;
    if (m_download && (m_hasOriginal || ready))
        download = [this, samePhoto] {
            if (samePhoto())
                m_download(m_canvas->image());
        };
    std::function<QImage()> copy;
    if (ready)
        copy = [this, samePhoto] { return samePhoto() && m_canvas->isVisible() ? m_canvas->image() : QImage(); };
    auto *menu = createImageContextMenu(this, std::move(download), std::move(copy));
    m_imageMenu = menu;
    QObject::connect(this, &QDialog::finished, menu, &QMenu::close);
    showMessageMenuWindow(*menu, QRect(point, QSize(1, 1)));
}

} // namespace bx::ui
