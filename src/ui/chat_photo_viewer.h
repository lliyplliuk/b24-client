#pragma once

#include "ui/loading_indicator.h"

#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QShortcut>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <functional>

namespace bx::ui {

class ChatPhotoCanvas final : public QWidget {
public:
    explicit ChatPhotoCanvas(QWidget *parent) : QWidget(parent)
    {
        setObjectName(QStringLiteral("chatPhotoCanvas"));
        setAccessibleName(QStringLiteral("Фотография"));
        setMinimumHeight(180);
    }

    void setOriginal(QImage image)
    {
        m_photo = QPixmap::fromImage(std::move(image));
        m_zoom = 1.0;
        m_pan = {};
        m_dragging = false;
        unsetCursor();
        update();
    }

    QSize imageSize() const { return m_photo.size(); }
    qreal zoomFactor() const { return m_zoom; }
    bool canZoomOut() const { return m_zoom > 1.0; }
    bool canZoomIn() const { return m_zoom < maximumZoom(); }
    void setZoomChanged(std::function<void()> handler) { m_zoomChanged = std::move(handler); }
    void zoomIn() { changeZoom(m_zoom * 1.25, rect().center()); }
    void zoomOut() { changeZoom(m_zoom / 1.25, rect().center()); }
    void fit() { changeZoom(1.0, rect().center()); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor("#142329"));
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        QRectF photo(QPointF{}, photoSize());
        photo.moveCenter(QRectF(rect()).center() + m_pan);
        painter.drawPixmap(photo, m_photo, QRectF(m_photo.rect()));
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        clampPan();
        if (m_zoomChanged)
            m_zoomChanged();
    }

    void wheelEvent(QWheelEvent *event) override
    {
        const auto delta = !event->angleDelta().isNull() ? event->angleDelta().y() : event->pixelDelta().y();
        if (delta != 0) {
            changeZoom(delta > 0 ? m_zoom * 1.25 : m_zoom / 1.25, event->position());
            event->accept();
        } else {
            QWidget::wheelEvent(event);
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && canZoomOut()) {
            m_dragging = true;
            m_dragPoint = event->position();
            setCursor(Qt::ClosedHandCursor);
            event->accept();
        } else {
            QWidget::mousePressEvent(event);
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_dragging) {
            QWidget::mouseMoveEvent(event);
            return;
        }
        m_pan += event->position() - m_dragPoint;
        m_dragPoint = event->position();
        clampPan();
        update();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (m_dragging && event->button() == Qt::LeftButton) {
            m_dragging = false;
            setCursor(canZoomOut() ? Qt::OpenHandCursor : Qt::ArrowCursor);
            event->accept();
        } else {
            QWidget::mouseReleaseEvent(event);
        }
    }

private:
    QSize fittedSize() const
    {
        return m_photo.size().scaled(size(), Qt::KeepAspectRatio);
    }

    qreal maximumZoom() const
    {
        const auto fitted = fittedSize();
        const qreal nativeWidth = qreal(m_photo.width()) / qMax(1, fitted.width());
        const qreal nativeHeight = qreal(m_photo.height()) / qMax(1, fitted.height());
        return qMax(5.0, qMax(nativeWidth, nativeHeight));
    }

    QSizeF photoSize() const
    {
        return QSizeF(fittedSize()) * m_zoom;
    }

    void clampPan()
    {
        const auto actual = photoSize();
        const auto horizontal = qMax(0.0, (actual.width() - width()) / 2);
        const auto vertical = qMax(0.0, (actual.height() - height()) / 2);
        m_pan.setX(qBound(-horizontal, m_pan.x(), horizontal));
        m_pan.setY(qBound(-vertical, m_pan.y(), vertical));
    }

    void changeZoom(qreal level, const QPointF &anchor)
    {
        const auto previous = m_zoom;
        m_zoom = qBound(1.0, level, maximumZoom());
        if (m_zoom == previous)
            return;
        m_pan += (anchor - QRectF(rect()).center() - m_pan) * (1.0 - m_zoom / previous);
        clampPan();
        setCursor(canZoomOut() ? Qt::OpenHandCursor : Qt::ArrowCursor);
        if (m_zoomChanged)
            m_zoomChanged();
        update();
    }

    QPixmap m_photo;
    qreal m_zoom = 1.0;
    QPointF m_pan;
    QPointF m_dragPoint;
    bool m_dragging = false;
    std::function<void()> m_zoomChanged;
};

class ChatPhotoDialog final : public QDialog {
public:
    ChatPhotoDialog(QWidget *parent, const QString &name, std::function<void()> download)
        : QDialog(parent)
    {
        setObjectName(QStringLiteral("chatPhotoViewer"));
        setWindowTitle(name.isEmpty() ? QStringLiteral("Фотография") : name);
        setWindowModality(Qt::WindowModal);
        setAttribute(Qt::WA_DeleteOnClose);
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
                m_download();
        });
        toolbar->addWidget(m_save);
        auto *close = new QPushButton(QStringLiteral("Закрыть"), this);
        QObject::connect(close, &QPushButton::clicked, this, &QDialog::reject);
        toolbar->addWidget(close);
        layout->addLayout(toolbar);
        setSaveAction(std::move(download));
        setPhoto(name, 0, 1, bool(m_download));
    }

    void setNavigationAction(std::function<void(int)> navigate) { m_navigate = std::move(navigate); }
    void setSaveAction(std::function<void()> download) { m_download = std::move(download); }

    void setPhoto(const QString &name, qsizetype index, qsizetype count, bool hasOriginal)
    {
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

    void showOriginal(QImage image)
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

    void showFailure(const QString &reason)
    {
        m_state->setText(reason);
        setLoadingIndicator(m_state, false);
        m_viewport->setCurrentWidget(m_state);
    }

private:
    QStackedWidget *m_viewport = nullptr;
    ChatPhotoCanvas *m_canvas = nullptr;
    QLabel *m_state = nullptr;
    QLabel *m_position = nullptr;
    QPushButton *m_previous = nullptr;
    QPushButton *m_next = nullptr;
    QPushButton *m_save = nullptr;
    std::function<void(int)> m_navigate;
    std::function<void()> m_download;
    std::function<void()> m_refreshControls;
};

} // namespace bx::ui
