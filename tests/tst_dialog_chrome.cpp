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
        QCOMPARE(viewer->windowModality(), Qt::WindowModal);
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
