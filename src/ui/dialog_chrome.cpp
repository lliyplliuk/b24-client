#include "ui/dialog_chrome.h"

#include <QApplication>
#include <QDialog>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QShortcut>
#include <QToolButton>
#include <QWindow>

namespace bx::ui {
namespace {

const auto frameName = QStringLiteral("dialogContrastFrame");
const auto headingName = QStringLiteral("dialogContrastHeading");
const auto headingStyle = QStringLiteral("QLabel { background: #193742; color: #fffefa; font-weight: 700; }");

class DialogFrame final : public QWidget {
public:
    explicit DialogFrame(QDialog *dialog) : QWidget(dialog)
    {
        setObjectName(frameName);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setPen(QPen(QColor(QStringLiteral("#294d59")), 2));
        painter.drawRect(rect().adjusted(1, 1, -1, -1));
    }
};

class DialogHeading final : public QLabel {
public:
    explicit DialogHeading(QDialog *dialog, QWidget *parent = nullptr) : QLabel(parent ? parent : dialog)
    {
        setObjectName(headingName);
        setStyleSheet(headingStyle);
        setTextFormat(Qt::PlainText);
        setMargin(7);
        setIndent(4);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        updateTitle(dialog->windowTitle());
        connect(dialog, &QWidget::windowTitleChanged, this, &DialogHeading::updateTitle);
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QLabel::resizeEvent(event);
        elideTitle();
    }

private:
    void updateTitle(const QString &title)
    {
        setAccessibleName(title);
        setToolTip(title.toHtmlEscaped());
        elideTitle();
        setVisible(!title.isEmpty());
    }

    void elideTitle()
    {
        setText(fontMetrics().elidedText(accessibleName(), Qt::ElideRight, qMax(0, width() - 2 * margin() - indent())));
    }
};

class DialogTitleBar final : public QWidget {
public:
    explicit DialogTitleBar(QDialog &dialog) : QWidget(&dialog), m_dialog(dialog)
    {
        setObjectName(QStringLiteral("dialogWindowTitleBar"));
        setAttribute(Qt::WA_StyledBackground);
        setStyleSheet(QStringLiteral(R"(
            QWidget#dialogWindowTitleBar { background: #193742; }
            QToolButton { background: transparent; color: #fffefa; border: 0; border-radius: 4px; font-size: 20px; }
            QToolButton:hover, QToolButton:focus { background: #345763; }
            QToolButton#dialogWindowClose:hover { background: #a23543; }
        )"));
        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(5, 4, 5, 4);
        row->setSpacing(2);
        auto *heading = new DialogHeading(&dialog, this);
        heading->setMargin(3);
        heading->setStyleSheet(headingStyle + QStringLiteral("QLabel { font-size: 16px; }"));
        heading->setAttribute(Qt::WA_TransparentForMouseEvents);
        row->addWidget(heading, 1);
        auto *minimize = addControl(*row, "dialogWindowMinimize", QStringLiteral("−"), tr("Свернуть"));
        m_maximize = addControl(*row, "dialogWindowMaximize", {}, {});
        auto *close = addControl(*row, "dialogWindowClose", QStringLiteral("×"), tr("Закрыть"));
        connect(minimize, &QToolButton::clicked, &dialog, &QWidget::showMinimized);
        connect(m_maximize, &QToolButton::clicked, this, &DialogTitleBar::toggleMaximized);
        connect(close, &QToolButton::clicked, &dialog, &QWidget::close);

        auto *menu = new QMenu(this);
        menu->addAction(tr("Свернуть"), &dialog, &QWidget::showMinimized);
        m_maximizeAction = menu->addAction(QString(), this, &DialogTitleBar::toggleMaximized);
        menu->addAction(tr("Закрыть"), &dialog, &QWidget::close);
        setContextMenuPolicy(Qt::CustomContextMenu);
        connect(this, &QWidget::customContextMenuRequested, menu,
                [this, menu](const QPoint &position) { menu->popup(mapToGlobal(position)); });
        auto *systemMenu = new QShortcut(QKeySequence(QStringLiteral("Alt+Space")), &dialog);
        connect(systemMenu, &QShortcut::activated, menu,
                [this, menu] { menu->popup(mapToGlobal(rect().bottomLeft())); });
        updateMaximizeControl();

        setMouseTracking(true);
        dialog.setMouseTracking(true);
        installEventFilter(this);
        dialog.installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == &m_dialog && event->type() == QEvent::WindowStateChange)
            updateMaximizeControl();
        if (event->type() != QEvent::MouseMove && event->type() != QEvent::MouseButtonPress
                && event->type() != QEvent::MouseButtonDblClick && event->type() != QEvent::Leave)
            return false;
        auto *surface = static_cast<QWidget *>(watched);
        if (event->type() == QEvent::Leave) {
            surface->unsetCursor();
            return false;
        }
        auto *mouse = static_cast<QMouseEvent *>(event);
        const auto point = surface->mapTo(&m_dialog, mouse->position().toPoint());
        const auto edges = resizeEdges(point);
        if (event->type() == QEvent::MouseMove) {
            if (edges)
                surface->setCursor(resizeCursor(edges));
            else
                surface->unsetCursor();
            return false;
        }
        if (mouse->button() != Qt::LeftButton)
            return false;
        if (event->type() == QEvent::MouseButtonDblClick && watched == this && !edges) {
            toggleMaximized();
            return true;
        }
        if (auto *window = m_dialog.windowHandle()) {
            if (edges)
                return window->startSystemResize(edges);
            if (watched == this)
                return window->startSystemMove();
        }
        return false;
    }

private:
    static QToolButton *addControl(QHBoxLayout &row, const char *name, const QString &symbol, const QString &label)
    {
        auto *button = new QToolButton(row.parentWidget());
        button->setObjectName(QString::fromLatin1(name));
        button->setFixedSize(32, 28);
        button->setText(symbol);
        button->setAccessibleName(label);
        button->setToolTip(label);
        row.addWidget(button);
        return button;
    }

    void toggleMaximized()
    {
        if (m_dialog.isMaximized())
            m_dialog.showNormal();
        else
            m_dialog.showMaximized();
    }

    void updateMaximizeControl()
    {
        const bool maximized = m_dialog.isMaximized();
        const auto label = maximized ? tr("Восстановить размер") : tr("Развернуть");
        m_maximize->setText(maximized ? QStringLiteral("❐") : QStringLiteral("□"));
        m_maximize->setAccessibleName(label);
        m_maximize->setToolTip(label);
        m_maximizeAction->setText(label);
    }

    Qt::Edges resizeEdges(const QPoint &point) const
    {
        if (m_dialog.isMaximized() || m_dialog.isFullScreen())
            return {};
        constexpr int grip = 5;
        Qt::Edges edges;
        if (point.x() < grip)
            edges |= Qt::LeftEdge;
        else if (point.x() >= m_dialog.width() - grip)
            edges |= Qt::RightEdge;
        if (point.y() < grip)
            edges |= Qt::TopEdge;
        else if (point.y() >= m_dialog.height() - grip)
            edges |= Qt::BottomEdge;
        return edges;
    }

    static Qt::CursorShape resizeCursor(Qt::Edges edges)
    {
        if (!(edges & (Qt::TopEdge | Qt::BottomEdge)))
            return Qt::SizeHorCursor;
        if (!(edges & (Qt::LeftEdge | Qt::RightEdge)))
            return Qt::SizeVerCursor;
        return edges.testFlag(Qt::LeftEdge) == edges.testFlag(Qt::TopEdge)
            ? Qt::SizeBDiagCursor : Qt::SizeFDiagCursor;
    }

    QDialog &m_dialog;
    QToolButton *m_maximize = nullptr;
    QAction *m_maximizeAction = nullptr;
};

void applyDialogHeading(QDialog *dialog)
{
    auto *layout = dialog->layout();
    if (!layout || dialog->windowTitle().isEmpty())
        return;

    // These dialogs already have a content title unique to the whole dialog.
    for (const auto &name : {"taskCardHeading", "connectionTitle", "pageTitle", "conversationTitle"}) {
        if (auto *heading = dialog->findChild<QLabel *>(QLatin1String(name), Qt::FindDirectChildrenOnly)) {
            if (!heading->property("dialogContrastApplied").toBool()) {
                heading->setProperty("dialogContrastApplied", true);
                heading->setStyleSheet(heading->styleSheet() + headingStyle + QStringLiteral(
                    "QLabel { border-radius: 4px; padding: 4px 9px; }"));
            }
            return;
        }
    }
    if (layout->menuBar())
        return;
    auto *heading = dialog->findChild<QLabel *>(headingName, Qt::FindDirectChildrenOnly);
    if (!heading)
        heading = new DialogHeading(dialog);
    layout->setMenuBar(heading);
    heading->show();
}

class DialogChrome final : public QObject {
public:
    using QObject::QObject;

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        const auto type = event->type();
        if (type != QEvent::Polish && type != QEvent::Show && type != QEvent::Resize && type != QEvent::LayoutRequest)
            return false;
        auto *dialog = qobject_cast<QDialog *>(watched);
        if (!dialog || !dialog->isWindow() || qobject_cast<QFileDialog *>(dialog))
            return false;
        if (type == QEvent::Polish && (dialog->objectName() == QLatin1String("taskCardDialog") ||
                                     dialog->objectName() == QLatin1String("chatPhotoViewer"))) {
            if (auto *layout = dialog->layout(); layout && !layout->menuBar()) {
                // On Wayland the platform decoration is outside the painted client frame.
                dialog->setWindowFlag(Qt::FramelessWindowHint);
                layout->setMenuBar(new DialogTitleBar(*dialog));
            }
            return false;
        }
        if (type == QEvent::LayoutRequest) {
            if (dialog->isVisible())
                applyDialogHeading(dialog);
            return false;
        }
        auto *frame = dialog->findChild<QWidget *>(frameName, Qt::FindDirectChildrenOnly);
        if (type == QEvent::Show) {
            applyDialogHeading(dialog);
            if (!frame)
                frame = new DialogFrame(dialog);
            frame->show();
        }
        if (frame) {
            frame->setGeometry(dialog->rect());
            frame->raise();
        }
        return false;
    }
};

} // namespace

void installDialogChrome(QApplication &app)
{
    app.installEventFilter(new DialogChrome(&app));
}

} // namespace bx::ui
