#include "ui/photo_viewer_input.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QWindow>
#include <QTimer>

namespace bx::ui {

PhotoViewerInput::PhotoViewerInput(QDialog &viewer)
    : QObject(qApp), m_viewer(&viewer), m_owner(viewer.parentWidget()->window())
{
    qApp->installEventFilter(this);
    connect(&viewer, &QObject::destroyed, this, [this] {
        m_viewer.clear();
        retireIfIdle();
    });
}

bool PhotoViewerInput::eventFilter(QObject *watched, QEvent *event)
{
    const auto type = event->type();
    const bool press = type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick;
    const bool release = type == QEvent::MouseButtonRelease;
    const auto button = press || release ? static_cast<QMouseEvent *>(event)->button() : Qt::NoButton;
    if (m_dismissedContextPending) {
        // Qt sends the mouse context event separately, on press or release depending on the platform.
        const bool dismissedContext = type == QEvent::ContextMenu
            && static_cast<QContextMenuEvent *>(event)->reason() == QContextMenuEvent::Mouse
            && m_owner && belongsTo(watched, *m_owner);
        if (press || dismissedContext) {
            m_dismissedContextPending = false;
            retireIfIdle();
            if (dismissedContext)
                return true;
        }
    }
    // A chat row can act on release alone, after the viewer has already been destroyed.
    if (m_dismissedButton != Qt::NoButton && button == m_dismissedButton) {
        m_dismissedButton = Qt::NoButton;
        retireIfIdle();
        return release && m_owner && belongsTo(watched, *m_owner);
    }
    if (!press && !release && type != QEvent::Wheel && type != QEvent::ContextMenu
        && type != QEvent::KeyPress && type != QEvent::KeyRelease
        && type != QEvent::Shortcut && type != QEvent::ShortcutOverride)
        return false;
    if (!m_viewer || !m_viewer->isVisible() || !m_owner || belongsTo(watched, *m_viewer)
        || !belongsTo(watched, *m_owner))
        return false;
    const auto *modal = QApplication::activeModalWidget();
    if (press && (!modal || !belongsTo(modal, *m_viewer))) {
        m_dismissedButton = button;
        m_dismissedContextPending = button == Qt::RightButton;
        // A release outside this application might never reach Qt.
        QTimer::singleShot(30000, this, [this] {
            m_dismissedButton = Qt::NoButton;
            m_dismissedContextPending = false;
            retireIfIdle();
        });
        m_viewer->reject();
    }
    return true;
}

void PhotoViewerInput::retireIfIdle()
{
    if (!m_viewer && m_dismissedButton == Qt::NoButton && !m_dismissedContextPending)
        deleteLater();
}

bool PhotoViewerInput::belongsTo(const QObject *target, const QWidget &owner)
{
    const auto *ownerWindow = owner.windowHandle();
    for (auto *object = target; object; object = object->parent()) {
        if (object == &owner)
            return true;
        // Native mouse events can reach a QWindow before being delivered to a widget.
        for (auto *window = qobject_cast<const QWindow *>(object); window; window = window->transientParent()) {
            if (window == ownerWindow)
                return true;
        }
    }
    return false;
}

} // namespace bx::ui
