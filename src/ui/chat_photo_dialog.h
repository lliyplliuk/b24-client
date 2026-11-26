#pragma once

#include "ui/chat_photo_canvas.h"
#include <QDialog>
#include <QPointer>
#include <QMenu>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <functional>

namespace bx::ui {

class ChatPhotoDialog final : public QDialog {
public:
    using SaveAction = std::function<void(const QImage &)>;
    ChatPhotoDialog(QWidget *parent, const QString &name, SaveAction download);
    void setNavigationAction(std::function<void(int)> navigate);
    void setSaveAction(SaveAction download);
    void setPhoto(const QString &name, qsizetype index, qsizetype count, bool hasOriginal);
    void showOriginal(QImage image);
    void showFailure(const QString &reason);

private:
    void showImageMenu(const QPoint &point);
    QStackedWidget *m_viewport = nullptr;
    ChatPhotoCanvas *m_canvas = nullptr;
    QLabel *m_state = nullptr;
    QLabel *m_position = nullptr;
    QPushButton *m_previous = nullptr;
    QPushButton *m_next = nullptr;
    QPushButton *m_save = nullptr;
    QPointer<QMenu> m_imageMenu;
    quint64 m_photoRevision = 0;
    bool m_hasOriginal = false;
    std::function<void(int)> m_navigate;
    SaveAction m_download;
    std::function<void()> m_refreshControls;
};

} // namespace bx::ui
