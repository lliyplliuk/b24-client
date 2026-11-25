#include "app/desktop_notification.h"
#include "ui/chat_photo_viewer.h"
#include "ui/dialog_chrome.h"
#include "ui/message_menu_window.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDialog>
#include <QFileDialog>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QSignalSpy>
#include <QTest>
#include <QToolButton>
#include <QVBoxLayout>

using bx::ui::installDialogChrome;

class DialogChromeTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { installDialogChrome(*qApp); }

    void desktopNotificationsRequireInactiveApplication_data()
    {
        QTest::addColumn<Qt::ApplicationState>("state");
        QTest::addColumn<bool>("loggedOut");
        QTest::addColumn<bool>("allowed");
        QTest::newRow("focused workspace or modal dialog") << Qt::ApplicationActive << false << false;
        QTest::newRow("inactive workspace") << Qt::ApplicationInactive << false << true;
        QTest::newRow("hidden in tray") << Qt::ApplicationHidden << false << true;
        QTest::newRow("logged out") << Qt::ApplicationInactive << true << false;
    }

    void desktopNotificationsRequireInactiveApplication()
    {
        QFETCH(Qt::ApplicationState, state);
        QFETCH(bool, loggedOut);
        QFETCH(bool, allowed);
        QCOMPARE(bx::desktop::shouldShowNotification(state, loggedOut), allowed);
    }

    void addsSharedChromeWithoutChangingDialogActions()
    {
        QWidget workspace;
        workspace.show();
        QDialog dialog(&workspace);
        dialog.setWindowTitle(QStringLiteral("Этапы доски"));
        auto *layout = new QVBoxLayout(&dialog);
        auto *input = new QLineEdit(&dialog);
        layout->addWidget(input);
        const auto flags = dialog.windowFlags();
        dialog.resize(300, 140);
        dialog.show();
        auto *heading = qobject_cast<QLabel *>(layout->menuBar());
        QVERIFY(heading);
        QCOMPARE(heading->accessibleName(), dialog.windowTitle());
        QCOMPARE(dialog.windowFlags(), flags);
        QVERIFY(!workspace.findChild<QWidget *>(QStringLiteral("dialogContrastFrame"), Qt::FindDirectChildrenOnly));
        auto *frame = dialog.findChild<QWidget *>(QStringLiteral("dialogContrastFrame"));
        QVERIFY(frame && frame->testAttribute(Qt::WA_TransparentForMouseEvents));
        QCOMPARE(frame->geometry(), dialog.rect());
        QCOMPARE(dialog.grab().toImage().pixelColor(1, dialog.height() / 2), QColor(QStringLiteral("#294d59")));
        dialog.setWindowTitle(QStringLiteral("Настройки этапов"));
        QCOMPARE(heading->accessibleName(), dialog.windowTitle());
        dialog.resize(360, 160);
        QCOMPARE(frame->geometry(), dialog.rect());
        input->setFocus();
        QTest::keyClicks(input, "stage");
        QCOMPARE(input->text(), QStringLiteral("stage"));
        QTest::keyClick(input, Qt::Key_Escape);
        QVERIFY(!dialog.isVisible());
        dialog.show();
        QCOMPARE(layout->menuBar(), heading);
        QCOMPARE(dialog.findChildren<QWidget *>(QStringLiteral("dialogContrastFrame")).size(), 1);
    }

    void strengthensAnExistingContentTitleOnce()
    {
        QDialog dialog;
        dialog.setWindowTitle(QStringLiteral("Задача №42"));
        auto *layout = new QVBoxLayout(&dialog);
        auto *title = new QLabel(QStringLiteral("Согласовать правки"), &dialog);
        title->setObjectName(QStringLiteral("taskCardHeading"));
        layout->addWidget(title);
        dialog.show();
        QVERIFY(!layout->menuBar());
        QCOMPARE(title->text(), QStringLiteral("Согласовать правки"));
        QCOMPARE(title->grab().toImage().pixelColor(title->width() - 12, 5), QColor(QStringLiteral("#193742")));
    }

    void framesTheActualTaskWindowTitle()
    {
        QWidget workspace;
        workspace.show();
        QDialog dialog(&workspace, Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
                                      Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);
        dialog.setObjectName(QStringLiteral("taskCardDialog"));
        dialog.setWindowTitle(QStringLiteral("Задача №120111"));
        dialog.setWindowModality(Qt::WindowModal);
        auto *layout = new QVBoxLayout(&dialog);
        auto *input = new QLineEdit(&dialog);
        layout->addWidget(input);
        dialog.resize(660, 420);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));

        QVERIFY(dialog.windowFlags().testFlag(Qt::FramelessWindowHint));
        QCOMPARE(dialog.windowType(), Qt::Window);
        QCOMPARE(dialog.windowModality(), Qt::WindowModal);
        auto *bar = layout->menuBar();
        QVERIFY(bar);
        QCOMPARE(bar->geometry().top(), 0);
        auto *heading = bar->findChild<QLabel *>(QStringLiteral("dialogContrastHeading"));
        QVERIFY(heading);
        QCOMPARE(heading->text(), dialog.windowTitle());
        QCOMPARE(heading->accessibleName(), dialog.windowTitle());
        QCOMPARE(heading->palette().color(QPalette::WindowText), QColor(QStringLiteral("#fffefa")));

        const auto image = dialog.grab().toImage();
        const auto pixel = [&image](int x, int y) {
            return image.pixelColor(qRound(x * image.devicePixelRatio()), qRound(y * image.devicePixelRatio()));
        };
        QCOMPARE(pixel(100, 1), QColor(QStringLiteral("#294d59")));
        QCOMPARE(pixel(1, bar->height() / 2), QColor(QStringLiteral("#294d59")));
        QCOMPARE(pixel(8, bar->height() / 2), QColor(QStringLiteral("#193742")));

        auto *minimize = bar->findChild<QToolButton *>(QStringLiteral("dialogWindowMinimize"));
        auto *maximize = bar->findChild<QToolButton *>(QStringLiteral("dialogWindowMaximize"));
        auto *close = bar->findChild<QToolButton *>(QStringLiteral("dialogWindowClose"));
        QVERIFY(minimize && maximize && close);
        const auto movePointer = [&dialog](QPoint point, Qt::CursorShape expected) {
            QMouseEvent move(QEvent::MouseMove, point, dialog.mapToGlobal(point), Qt::NoButton,
                             Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&dialog, &move);
            QCOMPARE(dialog.cursor().shape(), expected);
        };
        movePointer({1, 1}, Qt::SizeBDiagCursor);
        movePointer({dialog.width() - 2, 1}, Qt::SizeFDiagCursor);
        movePointer(dialog.rect().center(), Qt::ArrowCursor);

        auto *systemMenu = bar->findChild<QMenu *>();
        QVERIFY(systemMenu);
        const QPoint titlePoint(30, bar->height() / 2);
        QContextMenuEvent context(QContextMenuEvent::Mouse, titlePoint, bar->mapToGlobal(titlePoint));
        QApplication::sendEvent(bar, &context);
        QTRY_VERIFY(systemMenu->isVisible());
        QCOMPARE(systemMenu->actions().size(), 3);
        QTest::keyClick(systemMenu, Qt::Key_Escape);
        QTRY_VERIFY(!systemMenu->isVisible());
        QTRY_VERIFY(dialog.isActiveWindow());
        QTest::keyClick(input, Qt::Key_Space, Qt::AltModifier);
        QTRY_VERIFY(systemMenu->isVisible());
        QTest::keyClick(systemMenu, Qt::Key_Escape);
        QTRY_VERIFY(!systemMenu->isVisible());

        dialog.setWindowTitle(QStringLiteral("Задача №42"));
        QCOMPARE(heading->text(), dialog.windowTitle());
        QTest::mouseClick(maximize, Qt::LeftButton);
        QTRY_VERIFY(dialog.isMaximized());
        QTest::mouseClick(maximize, Qt::LeftButton);
        QTRY_VERIFY(!dialog.isMaximized());
        QTest::mouseDClick(bar, Qt::LeftButton, Qt::NoModifier, QPoint(30, bar->height() / 2));
        QTRY_VERIFY(dialog.isMaximized());
        QTest::mouseDClick(bar, Qt::LeftButton, Qt::NoModifier, QPoint(30, bar->height() / 2));
        QTRY_VERIFY(!dialog.isMaximized());
        QTest::keyClick(input, Qt::Key_Escape);
        QVERIFY(!dialog.isVisible());
        dialog.show();
        QTest::mouseClick(close, Qt::LeftButton);
        QVERIFY(!dialog.isVisible());
    }

    void framesThePhotoViewerFileName()
    {
        QWidget workspace;
        workspace.show();
        const auto fileName = QStringLiteral("Фото 20261124_113355.jpg");
        const QPointer<bx::ui::ChatPhotoDialog> viewer = new bx::ui::ChatPhotoDialog(&workspace, fileName, {});
        viewer->resize(660, 420);
        viewer->show();
        QVERIFY(QTest::qWaitForWindowExposed(viewer));

        QVERIFY(viewer->windowFlags().testFlag(Qt::FramelessWindowHint));
        QCOMPARE(viewer->windowType(), Qt::Dialog);
        QCOMPARE(viewer->windowModality(), Qt::NonModal);
        auto *bar = viewer->layout()->menuBar();
        QVERIFY(bar);
        QCOMPARE(bar->geometry().top(), 0);
        const auto headings = viewer->findChildren<QLabel *>(QStringLiteral("dialogContrastHeading"));
        QCOMPARE(headings.size(), 1);
        QCOMPARE(headings.front()->parentWidget(), bar);
        QCOMPARE(headings.front()->text(), fileName);
        QCOMPARE(headings.front()->palette().color(QPalette::WindowText), QColor(QStringLiteral("#fffefa")));

        const auto image = viewer->grab().toImage();
        const auto pixel = [&image](int x, int y) {
            return image.pixelColor(qRound(x * image.devicePixelRatio()), qRound(y * image.devicePixelRatio()));
        };
        QCOMPARE(pixel(100, 1), QColor(QStringLiteral("#294d59")));
        QCOMPARE(pixel(1, bar->height() / 2), QColor(QStringLiteral("#294d59")));
        QCOMPARE(pixel(8, bar->height() / 2), QColor(QStringLiteral("#193742")));

        viewer->setWindowTitle(QStringLiteral("Второе фото.jpg"));
        QCOMPARE(headings.front()->text(), viewer->windowTitle());
        auto *close = bar->findChild<QToolButton *>(QStringLiteral("dialogWindowClose"));
        QVERIFY(close);
        QTest::mouseClick(close, Qt::LeftButton);
        QTRY_VERIFY(viewer.isNull());
    }

    void photoViewerDismissesOutsidePressWithoutClickThrough_data()
    {
        QTest::addColumn<bool>("throughWindow");
        QTest::addColumn<bool>("releaseInOwner");
        QTest::newRow("widget input") << false << true;
        QTest::newRow("native window input") << true << true;
        QTest::newRow("release in an unrelated window") << false << false;
    }

    void photoViewerDismissesOutsidePressWithoutClickThrough()
    {
        QFETCH(bool, throughWindow);
        QFETCH(bool, releaseInOwner);
        class BackgroundButton final : public QPushButton {
        public:
            using QPushButton::QPushButton;
            int presses = 0;
            int releases = 0;

        protected:
            void mousePressEvent(QMouseEvent *event) override
            {
                ++presses;
                QPushButton::mousePressEvent(event);
            }

            void mouseReleaseEvent(QMouseEvent *event) override
            {
                ++releases;
                QPushButton::mouseReleaseEvent(event);
            }
        };
        QWidget workspace;
        auto *layout = new QVBoxLayout(&workspace);
        auto *background = new BackgroundButton(QStringLiteral("Сообщение"), &workspace);
        layout->addWidget(background);
        workspace.show();
        const QPointer<bx::ui::ChatPhotoDialog> viewer =
            new bx::ui::ChatPhotoDialog(&workspace, QStringLiteral("Фото.jpg"), {});
        QSignalSpy finished(viewer, &QDialog::finished);
        QSignalSpy backgroundClicks(background, &QPushButton::clicked);
        viewer->show();
        QVERIFY(QTest::qWaitForWindowExposed(viewer));

        QImage photo(40, 30, QImage::Format_RGB32);
        photo.fill(Qt::darkRed);
        viewer->showOriginal(photo);
        auto *canvas = viewer->findChild<QWidget *>(QStringLiteral("chatPhotoCanvas"));
        QVERIFY(canvas && canvas->isVisible());
        QTest::mouseClick(canvas, Qt::LeftButton);
        QVERIFY(viewer && viewer->isVisible());
        QEvent deactivate(QEvent::WindowDeactivate);
        QApplication::sendEvent(viewer, &deactivate);
        QVERIFY(viewer && viewer->isVisible());

        {
            QDialog child(viewer);
            child.setWindowModality(Qt::WindowModal);
            auto *childLayout = new QVBoxLayout(&child);
            auto *button = new QPushButton(QStringLiteral("Сохранить"), &child);
            childLayout->addWidget(button);
            child.show();
            QVERIFY(QTest::qWaitForWindowExposed(&child));
            QSignalSpy childClicks(button, &QPushButton::clicked);
            QTest::mouseClick(button, Qt::LeftButton);
            QCOMPARE(childClicks.size(), 1);
            QVERIFY(viewer && viewer->isVisible());
            QTest::keyClick(&child, Qt::Key_Escape);
            QVERIFY(!child.isVisible() && viewer->isVisible());
        }

        const auto point = canvas->rect().center();
        QContextMenuEvent context(QContextMenuEvent::Mouse, point, canvas->mapToGlobal(point));
        QApplication::sendEvent(canvas, &context);
        const auto menu = viewer->findChild<QMenu *>(QStringLiteral("imageContextMenu"));
        QVERIFY(menu);
        QTRY_VERIFY(menu->isVisible());
        auto *copy = menu->actions().last();
        QSignalSpy copied(copy, &QAction::triggered);
        QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(copy).center());
        QCOMPARE(copied.size(), 1);
        QVERIFY(viewer && viewer->isVisible());

        auto *outsideWindow = workspace.windowHandle();
        QVERIFY(outsideWindow);
        const auto outsidePoint = background->mapTo(&workspace, background->rect().center());
        if (throughWindow)
            QTest::mousePress(outsideWindow, Qt::LeftButton, {}, outsidePoint);
        else
            QTest::mousePress(background, Qt::LeftButton);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().first().toInt(), int(QDialog::Rejected));
        QTRY_VERIFY(viewer.isNull());
        if (!releaseInOwner) {
            BackgroundButton unrelated(QStringLiteral("Другое окно"));
            unrelated.show();
            QTest::mouseRelease(&unrelated, Qt::LeftButton);
            QCOMPARE(unrelated.releases, 1);
        } else if (throughWindow) {
            QTest::mouseRelease(outsideWindow, Qt::LeftButton, {}, outsidePoint);
        } else {
            QTest::mouseRelease(background, Qt::LeftButton);
        }
        QCOMPARE(background->presses, 0);
        QCOMPARE(background->releases, 0);
        QCOMPARE(backgroundClicks.size(), 0);

        QTest::mouseClick(background, Qt::LeftButton);
        QCOMPARE(backgroundClicks.size(), 1);
    }

    void rightClickDismissalDoesNotOpenBackgroundContextMenu_data()
    {
        QTest::addColumn<bool>("contextBeforeRelease");
        QTest::newRow("context on press") << true;
        QTest::newRow("context on release") << false;
    }

    void rightClickDismissalDoesNotOpenBackgroundContextMenu()
    {
        QFETCH(bool, contextBeforeRelease);
        QWidget workspace;
        workspace.setContextMenuPolicy(Qt::CustomContextMenu);
        workspace.show();
        QSignalSpy workspaceMenus(&workspace, &QWidget::customContextMenuRequested);
        QWidget unrelated;
        unrelated.setContextMenuPolicy(Qt::CustomContextMenu);
        unrelated.show();
        QSignalSpy unrelatedMenus(&unrelated, &QWidget::customContextMenuRequested);
        const QPointer<bx::ui::ChatPhotoDialog> viewer =
            new bx::ui::ChatPhotoDialog(&workspace, QStringLiteral("Фото.jpg"), {});
        viewer->show();
        QVERIFY(QTest::qWaitForWindowExposed(viewer));

        const auto context = [](QWidget &target, QContextMenuEvent::Reason reason) {
            const auto point = target.rect().center();
            QContextMenuEvent event(reason, point, target.mapToGlobal(point));
            QApplication::sendEvent(&target, &event);
        };
        QTest::mousePress(&workspace, Qt::RightButton);
        QTRY_VERIFY(viewer.isNull());
        context(workspace, QContextMenuEvent::Keyboard);
        QCOMPARE(workspaceMenus.size(), 1);
        context(unrelated, QContextMenuEvent::Mouse);
        QCOMPARE(unrelatedMenus.size(), 1);
        if (contextBeforeRelease)
            context(workspace, QContextMenuEvent::Mouse);
        QTest::mouseRelease(&workspace, Qt::RightButton);
        if (!contextBeforeRelease)
            context(workspace, QContextMenuEvent::Mouse);
        QCOMPARE(workspaceMenus.size(), 1);

        QTest::mouseClick(&workspace, Qt::RightButton);
        context(workspace, QContextMenuEvent::Mouse);
        QCOMPARE(workspaceMenus.size(), 2);
    }

    void keepsStandardConfirmationAndInputBehavior()
    {
        QMessageBox confirm(QMessageBox::Warning, QStringLiteral("Удаление задачи"),
                           QStringLiteral("Удалить задачу №42?"), QMessageBox::Yes | QMessageBox::Cancel);
        confirm.setDefaultButton(QMessageBox::Cancel);
        confirm.setEscapeButton(QMessageBox::Cancel);
        QSignalSpy finished(&confirm, &QDialog::finished);
        confirm.open();
        QVERIFY(confirm.layout()->menuBar() && confirm.layout()->menuBar()->isVisible());
        QCOMPARE(confirm.defaultButton(), confirm.button(QMessageBox::Cancel));
        QTest::keyClick(&confirm, Qt::Key_Escape);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().first().toInt(), int(QMessageBox::Cancel));

        QInputDialog input;
        input.setWindowTitle(QStringLiteral("Новый групповой чат"));
        input.setLabelText(QStringLiteral("Название"));
        input.show();
        QVERIFY(input.layout()->menuBar() && input.layout()->menuBar()->isVisible());
        auto *field = input.findChild<QLineEdit *>();
        QVERIFY(field);
        QTest::keyClicks(field, "team");
        QTest::keyClick(field, Qt::Key_Return);
        QCOMPARE(input.result(), int(QDialog::Accepted));
        QCOMPARE(input.textValue(), QStringLiteral("team"));
    }

    void leavesSystemFilePickerUnstyled()
    {
        QFileDialog dialog;
        dialog.setOption(QFileDialog::DontUseNativeDialog); // Offscreen fallback has a Qt layout.
        dialog.show();
        QVERIFY(!dialog.findChild<QWidget *>(QStringLiteral("dialogContrastFrame")));
        QVERIFY(!dialog.layout()->menuBar());
    }

    void positionsMessageMenuByItsAnchor_data()
    {
        QTest::addColumn<bool>("nearBottomRight");
        QTest::newRow("below-anchor") << false;
        QTest::newRow("above-anchor-at-screen-edge") << true;
    }

    void positionsMessageMenuByItsAnchor()
    {
        QFETCH(bool, nearBottomRight);
        QWidget workspace;
        workspace.showMaximized();
        QVERIFY(QTest::qWaitForWindowExposed(&workspace));
        const QRect available = workspace.screen()->availableGeometry();
        const QPoint anchorPosition = nearBottomRight ? available.bottomRight() - QPoint(25, 25)
                                                      : workspace.mapToGlobal(QPoint(80, 60));
        const QRect anchor(anchorPosition, QSize(22, 22));
        QMenu menu(&workspace);
        auto *subMenu = menu.addMenu(QStringLiteral("Реакция"));
        auto *action = subMenu->addAction(QStringLiteral("Нравится"));
        QSignalSpy triggered(action, &QAction::triggered);
        bx::ui::showMessageMenuWindow(menu, anchor);

        const bool wayland = QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
        const auto expectedType = wayland ? Qt::Popup : Qt::Tool;
        QCOMPARE(menu.windowType(), expectedType);
        QCOMPARE(subMenu->windowType(), expectedType);
        QVERIFY(QTest::qWaitForWindowExposed(&menu));
        const int x = nearBottomRight ? available.right() - menu.width() + 1 : anchor.left();
        const int y = nearBottomRight ? anchor.top() - menu.height() - 4 : anchor.bottom() + 1 + 4;
        QTRY_COMPARE(menu.geometry().topLeft(), QPoint(x, y));

        menu.setActiveAction(subMenu->menuAction());
        QTest::keyClick(&menu, Qt::Key_Right);
        QTRY_VERIFY(subMenu->isVisible());
        subMenu->setActiveAction(action);
        QTest::keyClick(subMenu, Qt::Key_Return);
        QCOMPARE(triggered.size(), 1);
        QVERIFY(!menu.isVisible());
    }
};

QTEST_MAIN(DialogChromeTest)
#include "tst_dialog_chrome.moc"
