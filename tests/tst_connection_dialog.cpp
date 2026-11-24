#include "app/silent_session_restore.h"
#include "ui/chat_widgets.h"
#include "ui/connection_dialog.h"
#include "ui/loading_indicator.h"
#include "ui/typography.h"
#include "ui/workspace_style.h"

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProxyStyle>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

using bx::ConnectionDialog;

class ConnectionDialogTest final : public QObject {
    Q_OBJECT

private slots:
    void usesReadableApplicationText()
    {
        bx::ui::installApplicationTypography(*qApp);
        ConnectionDialog dialog(QUrl(QStringLiteral("https://test.bitrix24.ru")));
        dialog.show();
        auto *address = dialog.findChild<QLineEdit *>(QStringLiteral("connectionAddress"));
        auto *continueButton = dialog.findChild<QPushButton *>(QStringLiteral("connectionContinue"));
        auto *hint = dialog.findChild<QLabel *>(QStringLiteral("connectionHint"));
        QVERIFY(address && continueButton && hint);
        QCOMPARE(address->font().pixelSize(), 16);
        QCOMPARE(continueButton->font().pixelSize(), 16);
        QCOMPARE(hint->font().pixelSize(), 16);
        dialog.resize(dialog.minimumSize().expandedTo(dialog.minimumSizeHint()));
        QCoreApplication::processEvents();
        const auto fits = [&dialog](const QWidget *widget) {
            return dialog.rect().contains(QRect(widget->mapTo(&dialog, QPoint()), widget->size()));
        };
        QVERIFY(fits(address));
        QVERIFY(fits(continueButton));
        QVERIFY(continueButton->width() >= continueButton->sizeHint().width());

        QWidget workspace;
        workspace.setStyleSheet(bx::workspace::styleSheet());
        workspace.resize(640, 180);
        bx::ui::ChatComposer composer(&workspace, {}, {}, {}, {});
        composer.resize(285, composer.sizeHint().height());
        workspace.show();
        auto *editor = composer.input();
        composer.ensurePolished();
        QCOMPARE(editor->font().pixelSize(), 16);
        const auto fullPlaceholder = QStringLiteral("Напишите сообщение");
        const auto textWidth = editor->viewport()->width() - 2 * editor->document()->documentMargin();
        QVERIFY(editor->fontMetrics().horizontalAdvance(fullPlaceholder) > textWidth);
        QVERIFY(editor->fontMetrics().horizontalAdvance(editor->placeholderText()) <= textWidth);
        QVERIFY(!editor->placeholderText().isEmpty());
        composer.resize(530, composer.height());
        QCoreApplication::processEvents();
        QCOMPARE(editor->placeholderText(), fullPlaceholder);
        QCOMPARE(editor->lineWrapMode(), QPlainTextEdit::WidgetWidth);
        QMenu messageMenu(&workspace);
        QCOMPARE(messageMenu.font().pixelSize(), 16);
    }

    void savedSessionFailureOffersOneRetryBeforeBrowser()
    {
        using bx::RestoreFailureAction;
        using bx::signin::RestoreStatus;
        QCOMPARE(bx::restoreFailureAction(RestoreStatus::Unavailable, false), RestoreFailureAction::OfferRetry);
        QCOMPARE(bx::restoreFailureAction(RestoreStatus::Unavailable, true), RestoreFailureAction::OpenBrowser);
        QCOMPARE(bx::restoreFailureAction(RestoreStatus::Rejected, false), RestoreFailureAction::OfferBrowser);
        QCOMPARE(bx::restoreFailureAction(RestoreStatus::Rejected, true), RestoreFailureAction::OpenBrowser);
    }

    void loadingAnimationRunsOnlyWhileVisibleAndPending()
    {
        class AnimationStyle final : public QProxyStyle {
        public:
            int styleHint(StyleHint hint, const QStyleOption *option, const QWidget *widget,
                          QStyleHintReturn *result) const override
            {
                if (hint == SH_Widget_Animation_Duration)
                    return 200;
                return QProxyStyle::styleHint(hint, option, widget, result);
            }
        } style;
        QWidget window;
        QLabel first(QStringLiteral("Загружаем чаты…"), &window);
        QLabel second(QStringLiteral("Ищем сообщения…"), &window);
        first.setStyle(&style);
        second.setStyle(&style);
        first.resize(210, 24);
        second.resize(210, 24);
        second.move(0, 30);
        first.setContentsMargins(4, 2, 3, 1);
        const auto margins = first.contentsMargins();
        auto *clock = bx::ui::LoadingClock::shared();
        bx::ui::setLoadingIndicator(&first, true);
        QVERIFY(!clock->isRunning());

        window.show();
        QTRY_VERIFY(clock->isRunning());
        const auto angle = clock->angle();
        QTRY_VERIFY(clock->angle() != angle);
        QCOMPARE(first.text(), QStringLiteral("Загружаем чаты…"));
        bx::ui::setLoadingIndicator(&second, true);
        bx::ui::setLoadingIndicator(&first, false);
        QCOMPARE(first.contentsMargins(), margins);
        QVERIFY(clock->isRunning());
        window.hide();
        QVERIFY(!clock->isRunning());
        window.show();
        QTRY_VERIFY(clock->isRunning());
        bx::ui::setLoadingIndicator(&second, false);
        QVERIFY(!clock->isRunning());
    }

    void savedSessionKeepsSignInHidden_data()
    {
        QTest::addColumn<int>("waitTurns");
        QTest::addColumn<bool>("authenticated");
        QTest::addColumn<QString>("error");
        QTest::newRow("authenticated") << 1 << true << QString();
        QTest::newRow("authenticated-with-rotated-save") << 2 << true << QString();
        QTest::newRow("missing-session") << 1 << false << QString();
        QTest::newRow("rejected-session") << 1 << false << QStringLiteral("Портал отклонил сохранённый вход");
        QTest::newRow("unavailable") << 1 << false << QStringLiteral("Не удалось проверить сохранённый вход");
        QTest::newRow("blocked") << 0 << false << QStringLiteral("Войдите через браузер");
        QTest::newRow("synchronous-authentication") << 0 << true << QString();
    }

    void savedSessionKeepsSignInHidden()
    {
        QFETCH(int, waitTurns);
        QFETCH(bool, authenticated);
        QFETCH(QString, error);
        ConnectionDialog dialog(QUrl(QStringLiteral("https://test.bitrix24.ru")));
        class ShowCounter final : public QObject {
        public:
            int count = 0;
            bool eventFilter(QObject *watched, QEvent *event) override
            {
                if (event->type() == QEvent::Show)
                    ++count;
                return QObject::eventFilter(watched, event);
            }
        } shown;
        dialog.installEventFilter(&shown);
        bx::SilentSessionRestore startup;
        int starts = 0;
        bool visibleAtStart = true;
        bool visibleAtCompletion = true;
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        QSignalSpy rejected(&dialog, &QDialog::rejected);
        auto *feedback = dialog.findChild<QLabel *>(QStringLiteral("connectionFeedback"));
        auto *submit = dialog.findChild<QPushButton *>(QStringLiteral("connectionContinue"));
        QVERIFY(feedback && submit);
        const auto complete = [&] {
            visibleAtCompletion = dialog.isVisible();
            if (authenticated) {
                dialog.accept();
            } else {
                if (error.isEmpty())
                    dialog.resetFeedback();
                else
                    dialog.setError(error);
                QTimer::singleShot(0, &dialog, [&] {
                    const bool visible = dialog.isVisible();
                    const bool feedbackVisible = feedback->isVisible();
                    const bool submitEnabled = submit->isEnabled();
                    dialog.reject();
                    QVERIFY(visible);
                    QCOMPARE(feedbackVisible, !error.isEmpty());
                    QVERIFY(submitEnabled);
                });
            }
            startup.finish();
        };
        startup.run(dialog, [&] {
            ++starts;
            visibleAtStart = dialog.isVisible();
            if (waitTurns) {
                dialog.setCheckingSavedSession();
                QTimer::singleShot(0, &dialog, [&] {
                    if (waitTurns == 2)
                        QTimer::singleShot(0, &dialog, complete);
                    else
                        complete();
                });
            } else {
                complete();
            }
        });
        QCOMPARE(starts, 1);
        QVERIFY(!visibleAtStart);
        QVERIFY(!visibleAtCompletion);
        QCOMPARE(shown.count, authenticated ? 0 : 1);
        QCOMPARE(accepted.size(), authenticated ? 1 : 0);
        QCOMPARE(rejected.size(), authenticated ? 0 : 1);
    }

    void normalizesPastedPortalAddress()
    {
        QCOMPARE(ConnectionDialog::normalizePortal(
                     QStringLiteral(" HTTPS://Workspace.Bitrix24.ru:443/online/?next=1#top ")),
                 QUrl(QStringLiteral("https://workspace.bitrix24.ru")));
        QCOMPARE(ConnectionDialog::normalizePortal(QStringLiteral("portal.example.org:8443/work")),
                 QUrl(QStringLiteral("https://portal.example.org:8443")));
        for (const auto &invalid : {QString(), QStringLiteral("http://portal.example.org"),
                                     QStringLiteral("file:///etc/hosts"), QStringLiteral("https://@portal.example.org"),
                                     QStringLiteral("https://user:secret@portal.example.org"),
                                     QStringLiteral("portal.example.org\nother.example.org"),
                                     QStringLiteral("https://portal.example.org:0")})
            QVERIFY2(ConnectionDialog::normalizePortal(invalid).isEmpty(), qPrintable(invalid));
    }

    void submitAndEditWhileWaiting()
    {
        ConnectionDialog dialog(QUrl(QStringLiteral("https://work.bitrix24.ru")));
        dialog.show();
        auto *address = dialog.findChild<QLineEdit *>(QStringLiteral("connectionAddress"));
        auto *submit = dialog.findChild<QPushButton *>(QStringLiteral("connectionContinue"));
        auto *feedback = dialog.findChild<QLabel *>(QStringLiteral("connectionFeedback"));
        QVERIFY(address);
        QVERIFY(submit);
        QVERIFY(feedback);
        QCOMPARE(address->text(), QStringLiteral("work.bitrix24.ru"));

        QSignalSpy launched(&dialog, &ConnectionDialog::connectRequested);
        QSignalSpy edited(&dialog, &ConnectionDialog::addressEdited);
        address->setText(QStringLiteral("https://"));
        QTest::keyClick(address, Qt::Key_Return);
        QCOMPARE(launched.count(), 0);
        QVERIFY(feedback->isVisible());
        QVERIFY(!feedback->text().isEmpty());

        address->setText(QStringLiteral("next.bitrix24.ru/work"));
        QTest::keyClick(address, Qt::Key_Return);
        QCOMPARE(launched.count(), 1);
        QCOMPARE(launched.takeFirst().first().toUrl(), QUrl(QStringLiteral("https://next.bitrix24.ru")));
        dialog.setWaiting({});
        QVERIFY(address->isEnabled());
        QVERIFY(!address->isReadOnly());
        QCOMPARE(submit->text(), QStringLiteral("Открыть браузер снова"));
        QVERIFY(feedback->text().contains(QStringLiteral("Битрикс24")));
        QVERIFY(feedback->text().contains(QStringLiteral("автоматически")));

        QTest::keyClicks(address, QStringLiteral("a"));
        QCOMPARE(edited.count(), 1);
        QVERIFY(!feedback->isVisible());
        QCOMPARE(submit->text(), QStringLiteral("Войти"));

        QSignalSpy rejected(&dialog, &QDialog::rejected);
        QTest::keyClick(address, Qt::Key_Escape);
        QCOMPARE(rejected.count(), 1);
    }

    void checkingSavedSessionDoesNotStartBrowser()
    {
        ConnectionDialog dialog(QUrl(QStringLiteral("https://work.bitrix24.ru")));
        dialog.show();
        auto *address = dialog.findChild<QLineEdit *>(QStringLiteral("connectionAddress"));
        auto *submit = dialog.findChild<QPushButton *>(QStringLiteral("connectionContinue"));
        QVERIFY(address);
        QVERIFY(submit);
        QSignalSpy launched(&dialog, &ConnectionDialog::connectRequested);
        dialog.setCheckingSavedSession();
        QVERIFY(!submit->isEnabled());
        QVERIFY(address->isEnabled());
        QTest::keyClick(address, Qt::Key_Return);
        submit->click();
        QCOMPARE(launched.count(), 0);
        QTest::keyClicks(address, QStringLiteral("a"));
        QVERIFY(submit->isEnabled());
        dialog.setCheckingSavedSession();
        dialog.setError(QStringLiteral("Временная ошибка"));
        QVERIFY(submit->isEnabled());
        QTest::keyClick(address, Qt::Key_Return);
        QCOMPARE(launched.count(), 1);
    }

    void screenshot()
    {
        const auto output = qEnvironmentVariable("B24_CONNECTION_SCREENSHOT");
        if (output.isEmpty())
            return;
        ConnectionDialog dialog(QUrl(QStringLiteral("https://company.bitrix24.ru")));
        dialog.show();
        QTest::qWait(25);
        QVERIFY(dialog.grab().save(output));
        dialog.setWaiting({});
        QTest::qWait(25);
        QVERIFY(dialog.grab().save(output + QStringLiteral(".waiting.png")));
        dialog.resize(470, 414);
        QTest::qWait(25);
        QVERIFY(dialog.grab().save(output + QStringLiteral(".compact.png")));
    }
};

QTEST_MAIN(ConnectionDialogTest)
#include "tst_connection_dialog.moc"
