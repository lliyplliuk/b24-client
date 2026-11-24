#include "core/demo_backend.h"
#include "ui/main_window.h"
#include "ui/worktime_panel.h"

#include <QFileDialog>
#include <QDateEdit>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace bx;
using namespace bx::ui;

namespace {

class WorktimeFixture final : public Backend {
public:
    QString displayName() const override { return QStringLiteral("Тестовый портал"); }
    bool isDemo() const override { return true; }
    QUrl portalOrigin() const override { return {}; }
    void start() override {}
    void stop() override {}
    void requestRecentChats() override { emit recentChatsLoaded({}); }
    void requestChatMessages(const QString &) override {}
    void requestProjects() override { emit projectsLoaded({}); }
    void requestProjectBoard(const QString &) override {}
    void requestTask(const QString &) override {}
    void requestTaskMessages(const QString &) override {}
    void requestNotifications() override { emit notificationsLoaded({}); }
    void sendChatMessage(const QString &, const QString &) override {}
    void sendTaskMessage(const QString &, const QString &) override {}
    void updateTask(const QString &, const TaskEdit &) override {}
    void createTask(const QString &, const TaskEdit &) override {}
    void createProject(const QString &) override {}
    void moveTask(const QString &, const QString &, const QString &) override {}
    void saveStage(const QString &, const QString &, const QString &, const QString &, const QString &) override {}
    void deleteStage(const QString &, const QString &) override {}
    void searchChats(const QString &) override {}
    void searchMessages(const QString &, const QString &, bool) override {}
    void uploadChatFile(const QString &, const QString &, bool) override {}
    void attachTaskFile(const QString &, const QString &) override {}

    void requestWorkday() override { ++workdayRequests; }
    void requestWorktimeSheet(const QDate &month) override { months.append(month); }
    void changeWorkday(WorkdayAction action, const QString &report) override { changes.append({action, report}); }
    void changeWorkdayAt(WorkdayAction action, const QDate &date, const QTime &time,
                         const QString &reason, const QString &report) override
    {
        manualChanges.append({action, date, time, reason, report});
    }
    void exportWorktimeSheet(const QDate &month, const QString &path) override { exports.append({month, path}); }

    int workdayRequests = 0;
    QVector<QDate> months;
    QVector<QPair<WorkdayAction, QString>> changes;
    struct ManualChange { WorkdayAction action; QDate date; QTime time; QString reason; QString report; };
    QVector<ManualChange> manualChanges;
    QVector<QPair<QDate, QString>> exports;
};

template<typename Widget>
Widget *named(QWidget &parent, const QString &name)
{
    for (auto *widget : parent.findChildren<Widget *>()) {
        if (widget->accessibleName() == name)
            return widget;
    }
    return nullptr;
}

WorkdayStatus closed()
{
    WorkdayStatus status;
    status.state = WorkdayState::Closed;
    status.canStart = true;
    return status;
}

WorkdayStatus running()
{
    WorkdayStatus status;
    status.state = WorkdayState::Running;
    status.canPause = true;
    status.canFinish = true;
    status.requiresReport = true;
    status.elapsedSeconds = 2 * 3600 + 38 * 60;
    return status;
}

WorktimeSheet populated(const QDate &month)
{
    return {month, {
        {QStringLiteral("Ирина Котова"), {{month, QStringLiteral("08:05"), QStringLiteral("09:00–17:05")}}},
        {QStringLiteral("Алексей Волков"), {{month, QStringLiteral("07:30"), QStringLiteral("09:30–17:00")}}},
        {QStringLiteral("Анна Миронова"), {{month, QStringLiteral("08:00"), QStringLiteral("09:00–17:00")}}},
    }, true, {}};
}

} // namespace

class WorktimePanelTest final : public QObject {
    Q_OBJECT
private slots:
    void explainsUnavailableDemoWithoutOfferingTimesheetActions()
    {
        DemoBackend fixture;
        WorktimePanel panel(&fixture);
        panel.resize(680, 550);
        panel.show();
        panel.refresh();
        auto *heading = panel.findChild<QLabel *>(QStringLiteral("pageTitle"));
        auto *details = named<QLabel>(panel, QStringLiteral("Подробности рабочего дня"));
        auto *previous = named<QPushButton>(panel, QStringLiteral("Предыдущий месяц"));
        auto *exportSheet = named<QPushButton>(panel, QStringLiteral("Скачать табель Excel"));
        QVERIFY(heading && details && previous && exportSheet);
        QTRY_VERIFY(details->isVisible());
        QVERIFY(details->text().contains(QStringLiteral("деморежиме")));
        QVERIFY(!previous->isVisible());
        QVERIFY(!exportSheet->isVisible());
        QTRY_VERIFY(heading->geometry().top() < 42);
    }

    void followsConfirmedWorkdayAndRequiresReports()
    {
        WorktimeFixture fixture;
        WorktimePanel panel(&fixture);
        panel.resize(680, 480);
        panel.show();
        panel.refresh();
        QCOMPARE(fixture.workdayRequests, 1);
        auto *status = named<QLabel>(panel, QStringLiteral("Статус рабочего дня"));
        auto *start = named<QPushButton>(panel, QStringLiteral("Начать день"));
        auto *timer = panel.findChild<QTimer *>(QStringLiteral("worktimeElapsedTimer"));
        auto *poll = panel.findChild<QTimer *>(QStringLiteral("worktimeStatusRefreshTimer"));
        QVERIFY(status && start && timer && poll);
        QVERIFY(!start->isEnabled());
        QVERIFY(!timer->isActive());
        QCOMPARE(poll->interval(), 60000);
        QVERIFY(poll->isActive());

        emit fixture.workdayLoaded(closed());
        QVERIFY(start->isEnabled());
        start->click();
        QVERIFY(!start->isEnabled());
        start->click();
        QCOMPARE(fixture.changes.size(), 1);
        QCOMPARE(fixture.changes.first().first, WorkdayAction::Start);
        QCOMPARE(status->text(), QStringLiteral("Рабочий день закрыт"));
        emit fixture.failed(QStringLiteral("workday-change"), QStringLiteral("Портал отказал"));
        QVERIFY(start->isEnabled());
        QCOMPARE(named<QLabel>(panel, QStringLiteral("Ошибка рабочего дня"))->text(), QStringLiteral("Портал отказал"));

        start->click();
        emit fixture.workdayChanged(running());
        QCOMPARE(status->text(), QStringLiteral("Рабочий день идёт"));
        QVERIFY(timer->isActive());
        const auto requestedSheets = fixture.months.size();
        QVERIFY(QMetaObject::invokeMethod(poll, "timeout"));
        QCOMPARE(fixture.workdayRequests, 2);
        QCOMPARE(fixture.months.size(), requestedSheets);
        emit fixture.workdayLoaded(running());
        auto *finish = named<QPushButton>(panel, QStringLiteral("Завершить рабочий день"));
        QVERIFY(finish && finish->isEnabled());
        QTimer::singleShot(0, &panel, [] {
            auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->setTextValue(QStringLiteral("Проверил табель"));
            dialog->accept();
        });
        finish->click();
        QCOMPARE(fixture.changes.size(), 3);
        QCOMPARE(fixture.changes.last().first, WorkdayAction::Finish);
        QCOMPARE(fixture.changes.last().second, QStringLiteral("Проверил табель"));
        QVERIFY(!finish->isEnabled());
        panel.hide();
        QVERIFY(!timer->isActive());
        QVERIFY(!poll->isActive());
        emit fixture.workdayChanged(closed());
        QVERIFY(!timer->isActive());
    }

    void submitsAnotherTimeOnlyAfterConfirmationAndWithRequiredReport()
    {
        WorktimeFixture fixture;
        WorktimePanel panel(&fixture);
        panel.resize(650, 450);
        panel.show();
        panel.refresh();
        auto *start = named<QPushButton>(panel, QStringLiteral("Начать с другим временем"));
        auto *finish = named<QPushButton>(panel, QStringLiteral("Завершить с другим временем"));
        QVERIFY(start && finish);
        QVERIFY(!start->isVisible() && !finish->isVisible());
        auto closedDay = closed();
        closedDay.canStartAt = true;
        closedDay.requiresTimeReason = true;
        emit fixture.workdayLoaded(closedDay);
        QVERIFY(start->isVisible() && start->isEnabled());
        QVERIFY(!finish->isVisible());
        auto *details = named<QLabel>(panel, QStringLiteral("Подробности рабочего дня"));
        QVERIFY(details && details->isHidden());
        QTimer::singleShot(0, &panel, [] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->reject();
        });
        start->click();
        QVERIFY(fixture.manualChanges.isEmpty());

        const auto fill = [](bool needsReport, QTime suggested = {}) {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto *date = named<QDateEdit>(*dialog, QStringLiteral("Дата"));
            auto *time = named<QTimeEdit>(*dialog, QStringLiteral("Время"));
            auto *reason = named<QLineEdit>(*dialog, QStringLiteral("Причина"));
            auto *report = named<QPlainTextEdit>(*dialog, QStringLiteral("Отчёт за рабочий день"));
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            QVERIFY(date && time && reason && buttons);
            auto *confirm = buttons->button(QDialogButtonBox::Ok);
            QVERIFY(confirm && !confirm->isEnabled());
            if (suggested.isValid())
                QCOMPARE(time->time(), suggested);
            time->setTime(QTime(8, 41));
            reason->setText(QStringLiteral("Уточнил время"));
            QCOMPARE(report != nullptr, needsReport);
            if (needsReport) {
                QVERIFY(!confirm->isEnabled());
                report->setPlainText(QStringLiteral("Закрыл задачи"));
            }
            QVERIFY(confirm->isEnabled());
            confirm->click();
        };
        QTimer::singleShot(0, &panel, [fill] { fill(false); });
        start->click();
        QCOMPARE(fixture.manualChanges.size(), 1);
        QCOMPARE(fixture.manualChanges.first().action, WorkdayAction::Start);
        QCOMPARE(fixture.manualChanges.first().time, QTime(8, 41));
        QVERIFY(fixture.manualChanges.first().date.isValid());
        QVERIFY(!start->isEnabled());
        start->click();
        QCOMPARE(fixture.manualChanges.size(), 1);

        auto active = running();
        active.state = WorkdayState::Expired;
        active.canFinishAt = true;
        active.requiresTimeReason = true;
        active.finishDate = QDate::currentDate().addDays(-1);
        active.suggestedFinishTime = QTime(17, 26);
        emit fixture.workdayChanged(active);
        QVERIFY(!start->isVisible());
        QVERIFY(finish->isVisible() && finish->isEnabled());
        QTimer::singleShot(0, &panel, [fill] { fill(true, QTime(17, 26)); });
        finish->click();
        QCOMPARE(fixture.manualChanges.size(), 2);
        QCOMPARE(fixture.manualChanges.last().action, WorkdayAction::Finish);
        QCOMPARE(fixture.manualChanges.last().date, active.finishDate);
        QCOMPARE(fixture.manualChanges.last().reason, QStringLiteral("Уточнил время"));
        QCOMPARE(fixture.manualChanges.last().report, QStringLiteral("Закрыл задачи"));
        emit fixture.failed(QStringLiteral("workday-change"), QStringLiteral("Время отклонено порталом"));
        QVERIFY(finish->isEnabled());

        panel.refresh();
        active.suggestedFinishTime = {};
        emit fixture.workdayLoaded(active);
        QTimer::singleShot(0, &panel, [] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto *time = named<QLineEdit>(*dialog, QStringLiteral("Время"));
            auto *reason = named<QLineEdit>(*dialog, QStringLiteral("Причина"));
            auto *report = named<QPlainTextEdit>(*dialog, QStringLiteral("Отчёт за рабочий день"));
            QVERIFY(time && reason && report);
            auto *confirm = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
            reason->setText(QStringLiteral("Уточнил время"));
            report->setPlainText(QStringLiteral("Закрыл задачи"));
            QVERIFY(!confirm->isEnabled());
            time->setText(QStringLiteral("00:00"));
            QVERIFY(confirm->isEnabled());
            confirm->click();
        });
        finish->click();
        QCOMPARE(fixture.manualChanges.size(), 3);
        QCOMPARE(fixture.manualChanges.last().time, QTime(0, 0));
    }

    void elapsedTicksOnlyWhileRunning()
    {
        WorktimeFixture fixture;
        WorktimePanel panel(&fixture);
        panel.show();
        panel.refresh();
        emit fixture.workdayLoaded(running());
        auto *elapsed = named<QLabel>(panel, QStringLiteral("Отработано"));
        QVERIFY(elapsed && elapsed->isVisible());
        const auto initial = elapsed->text();
        QVERIFY(initial.startsWith(QStringLiteral("Отработано 02:38:")));
        QTRY_VERIFY_WITH_TIMEOUT(elapsed->text() != initial, 2800);
        QVERIFY(QMetaObject::invokeMethod(panel.findChild<QTimer *>(QStringLiteral("worktimeStatusRefreshTimer")), "timeout"));
        auto paused = running();
        paused.state = WorkdayState::Paused;
        paused.elapsedSeconds += 11;
        emit fixture.workdayLoaded(paused);
        QCOMPARE(elapsed->text(), QStringLiteral("Отработано 02:38:11"));
        auto *timer = panel.findChild<QTimer *>(QStringLiteral("worktimeElapsedTimer"));
        QVERIFY(!timer->isActive());
        QVERIFY(QMetaObject::invokeMethod(timer, "timeout"));
        QCOMPARE(elapsed->text(), QStringLiteral("Отработано 02:38:11"));
    }

    void reopensClosedDayWhenThePortalAllowsIt()
    {
        WorktimeFixture fixture;
        WorktimePanel panel(&fixture);
        panel.show();
        panel.refresh();
        auto reopen = closed();
        reopen.canStart = false;
        reopen.canResume = true;
        emit fixture.workdayLoaded(reopen);
        auto *action = named<QPushButton>(panel, QStringLiteral("Открыть снова"));
        auto *manual = named<QPushButton>(panel, QStringLiteral("Начать с другим временем"));
        auto *details = named<QLabel>(panel, QStringLiteral("Подробности рабочего дня"));
        QVERIFY(action && action->isEnabled());
        QVERIFY(manual && manual->isVisible() && !manual->isEnabled());
        QVERIFY(details && details->isVisible());
        QVERIFY(details->text().contains(QStringLiteral("с прежним временем начала")));
        manual->click();
        QVERIFY(fixture.manualChanges.isEmpty());
        QVERIFY(fixture.changes.isEmpty());
        action->click();
        QVERIFY(!action->isEnabled());
        QCOMPARE(fixture.changes.size(), 1);
        QCOMPARE(fixture.changes.last().first, WorkdayAction::Resume);
        emit fixture.failed(QStringLiteral("workday-change"), QStringLiteral("Повторите попытку"));
        QVERIFY(action->isEnabled());
        action->click();
        QCOMPARE(fixture.changes.size(), 2);
        QCOMPARE(fixture.changes.last().first, WorkdayAction::Resume);
        auto active = running();
        active.canFinishAt = true;
        emit fixture.workdayChanged(active);
        QCOMPARE(action->text(), QStringLiteral("Пауза"));
        QVERIFY(manual->isVisible() && !manual->isEnabled());
        QVERIFY(details->text().contains(QStringLiteral("Рабочий день уже начат")));
        auto *finish = named<QPushButton>(panel, QStringLiteral("Завершить с другим временем"));
        QVERIFY(finish && finish->isVisible() && finish->isEnabled());

        panel.refresh();
        active.state = WorkdayState::Paused;
        active.canFinishAt = false;
        emit fixture.workdayLoaded(active);
        QVERIFY(manual->isVisible() && !manual->isEnabled());
        QVERIFY(!finish->isVisible());
    }

    void canReturnToAMonthSkippedByThePortal()
    {
        WorktimeFixture fixture;
        WorktimePanel panel(&fixture);
        panel.show();
        panel.refresh();
        QCOMPARE(fixture.months.size(), 1);
        const auto current = fixture.months.first();
        auto *previous = named<QPushButton>(panel, QStringLiteral("Предыдущий месяц"));
        auto *next = named<QPushButton>(panel, QStringLiteral("Следующий месяц"));
        QVERIFY(previous && next);
        previous->click();
        previous->click();
        next->click();
        QCOMPARE(fixture.months, QVector<QDate>({current, current.addMonths(-1), current.addMonths(-2), current.addMonths(-1)}));
        emit fixture.worktimeSheetLoaded(populated(current.addMonths(-2)));
        auto *table = named<QTableView>(panel, QStringLiteral("Табель по сотрудникам и дням"));
        QVERIFY(table);
        QCOMPARE(table->model()->rowCount(), 0);
        emit fixture.worktimeSheetLoaded(populated(current.addMonths(-1)));
        QCOMPARE(table->model()->rowCount(), 3);
        next->click();
        next->click();
        QCOMPARE(fixture.months.last(), current.addMonths(1));
    }

    void keepsMonthsAndExportsScopedAndClearsOnAccountChange()
    {
        WorktimeFixture fixture;
        WorktimePanel panel(&fixture);
        panel.resize(680, 470);
        panel.show();
        panel.refresh();
        auto *table = named<QTableView>(panel, QStringLiteral("Табель по сотрудникам и дням"));
        auto *previous = named<QPushButton>(panel, QStringLiteral("Предыдущий месяц"));
        auto *next = named<QPushButton>(panel, QStringLiteral("Следующий месяц"));
        auto *save = named<QPushButton>(panel, QStringLiteral("Скачать табель Excel"));
        auto *notice = named<QLabel>(panel, QStringLiteral("Состояние табеля"));
        QVERIFY(table && previous && next && save && notice);
        QCOMPARE(fixture.months.size(), 1);
        const auto current = fixture.months.first();
        previous->click();
        QCOMPARE(fixture.months.size(), 2);
        const auto older = current.addMonths(-1);
        QCOMPARE(fixture.months.last(), older);
        emit fixture.worktimeSheetLoaded(populated(current));
        QCOMPARE(table->model()->rowCount(), 0);
        QVERIFY(!save->isEnabled());
        emit fixture.failed(QStringLiteral("worktime-sheet:") + older.toString(QStringLiteral("yyyy-MM")),
                            QStringLiteral("Нет доступа к табелю"));
        QCOMPARE(notice->text(), QStringLiteral("Нет доступа к табелю"));

        panel.refresh();
        emit fixture.worktimeSheetLoaded({older, {}, false, QStringLiteral("Модуль рабочего времени отключён")});
        QCOMPARE(notice->text(), QStringLiteral("Модуль рабочего времени отключён"));
        panel.refresh();
        emit fixture.worktimeSheetLoaded({older, {}, false, {}});
        QCOMPARE(notice->text(), QStringLiteral("За выбранный месяц записей нет."));

        next->click();
        emit fixture.worktimeSheetLoaded(populated(current));
        QCOMPARE(table->model()->columnCount(), current.daysInMonth());
        QCOMPARE(table->model()->rowCount(), 3);
        table->setCurrentIndex(table->model()->index(0, 0));
        auto *selected = named<QLabel>(panel, QStringLiteral("Подробности выбранного дня"));
        QVERIFY(selected->text().contains(QStringLiteral("Ирина Котова")));
        QVERIFY(selected->text().contains(QStringLiteral("09:00–17:05")));
        QVERIFY(save->isEnabled());
        emit fixture.connectionChanged(false);
        QVERIFY(!previous->isEnabled());
        QVERIFY(!next->isEnabled());
        previous->click();
        QCOMPARE(table->model()->rowCount(), 3);
        emit fixture.connectionChanged(true);
        emit fixture.worktimeSheetLoaded(populated(current));
        QTemporaryDir destination;
        QVERIFY(destination.isValid());
        const auto path = destination.filePath(QStringLiteral("табель.xls"));
        QTimer::singleShot(0, &panel, [path] {
            auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->selectFile(path);
            QVERIFY(QMetaObject::invokeMethod(dialog, "accept"));
        });
        save->click();
        QCOMPARE(fixture.exports.size(), 1);
        QCOMPARE(fixture.exports.first().first, current);
        QCOMPARE(fixture.exports.first().second, path);
        QVERIFY(!save->isEnabled());
        emit fixture.worktimeSheetExported(current, path + QStringLiteral(".other"));
        QVERIFY(!save->isEnabled());
        emit fixture.worktimeSheetExported(current, path);
        QVERIFY(save->isEnabled());

        emit fixture.accountScopeChanged();
        QCOMPARE(table->model()->rowCount(), 0);
        QVERIFY(!save->isEnabled());
        emit fixture.connectionChanged(false);
        QCOMPARE(notice->text(), QStringLiteral("Нет связи с порталом."));
    }

    void opensFromRailWithAltFourAndHasCompactLayout()
    {
        WorktimeFixture fixture;
        MainWindow window(&fixture);
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *rail = named<QListWidget>(window, QStringLiteral("Разделы"));
        QVERIFY(rail);
        QCOMPARE(fixture.workdayRequests, 0);
        QTest::keyClick(&window, Qt::Key_4, Qt::AltModifier);
        QTRY_COMPARE(rail->currentRow(), 3);
        QCOMPARE(fixture.workdayRequests, 1);
        auto active = running();
        active.canFinishAt = true;
        emit fixture.workdayLoaded(active);
        QVERIFY(!fixture.months.isEmpty());
        emit fixture.worktimeSheetLoaded(populated(fixture.months.last()));
        auto *panel = window.findChild<QWidget *>(QStringLiteral("worktimePanel"));
        QVERIFY(panel && panel->isVisible());
        auto *table = named<QTableView>(*panel, QStringLiteral("Табель по сотрудникам и дням"));
        QVERIFY(table);
        table->setCurrentIndex(table->model()->index(0, 0));
        QCoreApplication::processEvents();
        const auto wide = qEnvironmentVariable("B24_WORKTIME_WIDE_SCREENSHOT");
        if (!wide.isEmpty())
            QVERIFY(window.grab().save(wide));
        window.resize(720, 500);
        QCoreApplication::processEvents();
        QCOMPARE(window.width(), 720);
        QVERIFY(table->viewport()->width() >= 300);
        const auto compact = qEnvironmentVariable("B24_WORKTIME_COMPACT_SCREENSHOT");
        if (!compact.isEmpty())
            QVERIFY(window.grab().save(compact));
        const auto manual = qEnvironmentVariable("B24_WORKTIME_MANUAL_SCREENSHOT");
        if (!manual.isEmpty()) {
            auto *finish = named<QPushButton>(*panel, QStringLiteral("Завершить с другим временем"));
            QVERIFY(finish && finish->isEnabled());
            QTimer::singleShot(0, &window, [manual] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                QVERIFY(dialog->grab().save(manual));
                dialog->reject();
            });
            finish->click();
            QApplication::setActiveWindow(&window);
            QCoreApplication::processEvents();
        }
        const auto reopenCapture = qEnvironmentVariable("B24_WORKTIME_REOPEN_SCREENSHOT");
        if (!reopenCapture.isEmpty()) {
            QTest::keyClick(&window, Qt::Key_F5);
            auto reopen = closed();
            reopen.canStart = false;
            reopen.canResume = true;
            reopen.elapsedSeconds = 2 * 3600 + 38 * 60;
            emit fixture.workdayLoaded(reopen);
            emit fixture.worktimeSheetLoaded(populated(fixture.months.last()));
            table->setCurrentIndex(table->model()->index(0, 0));
            QCoreApplication::processEvents();
            QVERIFY(window.grab().save(reopenCapture));
        }
        QTest::keyClick(&window, Qt::Key_1, Qt::AltModifier);
        QTRY_COMPARE(rail->currentRow(), 0);
        QVERIFY(!panel->isVisible());
        QVERIFY(!panel->findChild<QTimer *>(QStringLiteral("worktimeStatusRefreshTimer"))->isActive());
    }
};

QTEST_MAIN(WorktimePanelTest)
#include "tst_worktime_panel.moc"
