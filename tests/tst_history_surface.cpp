#include "core/demo_backend.h"
#include "ui/history_list.h"
#include "ui/history_surface.h"

#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>

using namespace bx;

class HistorySurfaceTest final : public QObject {
    Q_OBJECT

private slots:
    void pagesFollowTheActiveSurface_data()
    {
        QTest::addColumn<bool>("task");
        QTest::newRow("chat") << false;
        QTest::newRow("task card") << true;
    }

    void pagesFollowTheActiveSurface()
    {
        QFETCH(bool, task);
        DemoBackend backend;
        QWidget parent;
        QLabel status(&parent);
        ui::HistoryList history(&parent);
        ui::HistorySurface::View view{&history, QStringLiteral("chat42"),
                                      task ? QStringLiteral("7") : QStringLiteral("chat42"), true};
        ui::HistorySurface surface(&backend, task, [&view] { return view; }, &parent);
        QSignalSpy added(&surface, &ui::HistorySurface::olderPageAdded);
        QString cursor;
        history.setOnOlder([&](const QString &before) { cursor = before; });
        auto *button = history.olderButton(&parent)->findChild<QPushButton *>();
        QVERIFY(button);
        Message latest;
        latest.id = QStringLiteral("42");
        latest.text = QStringLiteral("latest");
        history.showHistory(view.pageScope, &status, {latest});
        button->click();
        QCOMPARE(cursor, latest.id);

        Message older;
        older.id = QStringLiteral("41");
        older.text = QStringLiteral("older");
        const auto deliver = [&](const QString &scope, bool taskPage) {
            emit backend.olderChatMessagesLoaded(scope, cursor, taskPage, {older}, false);
        };
        deliver(view.pageScope, !task);
        deliver(QStringLiteral("other"), task);
        view.active = false;
        deliver(view.pageScope, task);
        view.active = true;
        view.searching = true;
        deliver(view.pageScope, task);
        QCOMPARE(added.count(), 0);
        view.searching = false;
        deliver(view.pageScope, task);
        QCOMPARE(added.count(), 1);
        QVERIFY(history.focusOnMessage(older.id));
    }

    void dialogEventsFollowCardReplacement()
    {
        DemoBackend backend;
        ui::HistoryList history(nullptr);
        ui::HistorySurface::View view{&history, QStringLiteral("chat42"), QStringLiteral("7"), true};
        ui::HistorySurface surface(&backend, true, [&view] { return view; }, this);
        QSignalSpy pins(&surface, &ui::HistorySurface::pinsChanged);
        QSignalSpy cleared(&surface, &ui::HistorySurface::historyCleared);
        emit backend.pinnedMessagesLoaded(view.pageScope, {});
        emit backend.pinnedMessagesLoaded(view.dialogId, {});
        QCOMPARE(pins.count(), 1);

        const auto oldDialog = view.dialogId;
        view = {};
        emit backend.pinnedMessagesLoaded(oldDialog, {});
        emit backend.chatHistoryRemoved({}, {}, 0);
        view = {&history, QStringLiteral("chat99"), QStringLiteral("9"), true};
        emit backend.pinnedMessagesLoaded(oldDialog, {});
        emit backend.chatHistoryRemoved(oldDialog, {}, 0);
        view.searching = true;
        emit backend.chatHistoryRemoved(view.dialogId, {}, 0);
        QCOMPARE(cleared.count(), 0);
        view.searching = false;
        emit backend.chatHistoryRemoved(view.dialogId, {}, 0);
        emit backend.pinnedMessagesLoaded(view.dialogId, {});
        QCOMPARE(pins.count(), 2);
        QCOMPARE(cleared.count(), 1);
    }
};

QTEST_MAIN(HistorySurfaceTest)
#include "tst_history_surface.moc"
