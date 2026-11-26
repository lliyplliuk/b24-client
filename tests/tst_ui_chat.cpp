#include "pilot_fixture.h"

using namespace bx;
using namespace bx::test;

class UiChatTest : public PilotFixture {
    Q_OBJECT

private slots:
    void showsSeparateUnreadBadgesOnChatTabs()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->railUnread = 150;
        backend->provideTaskBadges = true;
        auto ordinary = backend->chats.first();
        ordinary.serverChatId = 67;
        ordinary.unread = 3;
        auto duplicate = ordinary;
        duplicate.id = QStringLiteral("legacy-task");
        duplicate.serverChatId = 71;
        duplicate.unread = 7;
        auto task = duplicate;
        task.id = QStringLiteral("chat71");
        task.serverChatId = 0;
        task.taskChat = true;
        task.taskId = QStringLiteral("501");
        task.unread = 2;
        auto otherTask = task;
        otherTask.id = QStringLiteral("chat72");
        otherTask.taskId = QStringLiteral("502");
        otherTask.unread = 4;
        backend->chats = {ordinary, duplicate};
        backend->taskChats = {task, otherTask};

        MainWindow window(backend.get());
        window.show();
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("chatScopeTabs"));
        auto *rail = window.findChild<QListWidget *>(QStringLiteral("sections"));
        QVERIFY(tabs && rail);
        QCOMPARE(backend->taskChatBadgeRequests, 1);
        QCOMPARE(backend->taskChatRequests, 0);
        QCOMPARE(tabs->currentIndex(), 0);
        QCOMPARE(tabs->tabData(0).toLongLong(), 3);
        QCOMPARE(tabs->tabData(1).toLongLong(), 6);
        QCOMPARE(tabs->tabText(0), QStringLiteral("Чаты"));
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты задач"));
        const auto railCount = [rail] { return rail->item(0)->data(ui::SectionUnreadCount).toLongLong(); };
        QCOMPARE(railCount(), 9);
        for (int scope = 0; scope < 2; ++scope) {
            auto *badge = qobject_cast<QLabel *>(tabs->tabButton(scope, QTabBar::RightSide));
            QVERIFY(badge);
            QCOMPARE(badge->text(), scope == 0 ? QStringLiteral("3") : QStringLiteral("6"));
        }
        const auto screenshot = qEnvironmentVariable("B24_CHAT_TABS_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(tabs->grab().save(screenshot));

        backend->taskChats[0].unread = 0;
        emit backend->taskChatsLoaded(backend->taskChats);
        QCOMPARE(tabs->tabData(0).toLongLong(), 3);
        QCOMPARE(tabs->tabData(1).toLongLong(), 4);
        QCOMPARE(railCount(), 7);
        backend->chats[0].unread = 0;
        backend->taskChats[1].unread = 95;
        emit backend->recentChatsLoaded(backend->chats);
        emit backend->taskChatsLoaded(backend->taskChats);
        QCOMPARE(tabs->tabData(0).toLongLong(), 0);
        QCOMPARE(tabs->tabData(1).toLongLong(), 95);
        QCOMPARE(railCount(), 95);
        emit backend->unreadChatCountChanged(150);
        QCOMPARE(railCount(), 95);
        backend->chats[0].unread = 2;
        backend->taskChats[1].unread = 96;
        emit backend->taskChatsLoaded(backend->taskChats);
        emit backend->recentChatsLoaded(backend->chats);
        QCOMPARE(tabs->tabData(0).toLongLong(), 2);
        QCOMPARE(tabs->tabData(1).toLongLong(), 96);
        QCOMPARE(railCount(), 98);
        tabs->setCurrentIndex(1);
        QCOMPARE(backend->taskChatRequests, 1);
        QCOMPARE(railCount(), 98);
        backend->chats[0].unread = 0;
        emit backend->recentChatsLoaded(backend->chats);
        backend->taskChats[1].unread = 0;
        emit backend->taskChatsLoaded(backend->taskChats);
        QCOMPARE(tabs->tabData(1).toLongLong(), 0);
        const auto *blankBadge = qobject_cast<QLabel *>(tabs->tabButton(1, QTabBar::RightSide));
        QVERIFY(!blankBadge || (!blankBadge->isVisible() && blankBadge->text().isEmpty()));
        QCOMPARE(railCount(), 0);
        emit backend->recentChatsLoaded(backend->chats);
        emit backend->unreadChatCountChanged(150);
        QCOMPARE(backend->taskChatBadgeRequests, 1);
        QCOMPARE(tabs->tabData(0).toLongLong(), 0);
        QCOMPARE(railCount(), 0);

        backend->taskChats[1].unread = 95;
        emit backend->taskChatsLoaded(backend->taskChats);
        backend->accountScope = QStringLiteral("fixture/chat-badges-first");
        emit backend->accountScopeChanged();
        QCOMPARE(railCount(), 95);
        backend->accountScope = QStringLiteral("fixture/chat-badges-second");
        emit backend->accountScopeChanged();
        QCOMPARE(tabs->tabData(0).toLongLong(), 0);
        QCOMPARE(tabs->tabData(1).toLongLong(), 0);
        QCOMPARE(railCount(), 0);
    }

    void chatTabUnreadBadgesStayInsideTheirTabsOnResize()
    {
        const auto applicationFont = QApplication::font();
        const auto restoreFont = qScopeGuard([applicationFont] { QApplication::setFont(applicationFont); });
        ui::installApplicationTypography(*qApp);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->provideTaskBadges = true;
        auto task = backend->chats.first();
        task.id = QStringLiteral("chat72");
        task.taskChat = true;
        task.taskId = QStringLiteral("502");
        task.unread = 1;
        backend->taskChats = {task};
        MainWindow window(backend.get());
        window.resize(1400, 690);
        window.show();
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("chatScopeTabs"));
        auto *splitter = window.findChild<QSplitter *>(QStringLiteral("chatDirectorySplitter"));
        QVERIFY(tabs && splitter);
        QTRY_VERIFY(tabs->isVisible());
        const int compactWidth = splitter->widget(0)->minimumWidth();
        const int wideWidth = compactWidth + 130;
        const auto setDirectoryWidth = [splitter](int width) {
            const int available = splitter->contentsRect().width() - splitter->handle(1)->width();
            splitter->setSizes({width, available - width});
            QTRY_COMPARE(splitter->sizes().first(), width);
        };
        setDirectoryWidth(wideWidth);
        const int wideTabWidth = tabs->width();
        const auto assertBadge = [tabs](int scope, const QString &text) {
            const auto *badge = qobject_cast<QLabel *>(tabs->tabButton(scope, QTabBar::RightSide));
            if (text.isEmpty()) {
                QVERIFY(!badge || !badge->isVisible());
                return;
            }
            QVERIFY(badge && badge->isVisible());
            QCOMPARE(badge->text(), text);
            const auto message = QStringLiteral("scope %1, tab %2×%3 at %4, badge %5×%6 at %7, tabs width %8")
                .arg(scope).arg(tabs->tabRect(scope).width()).arg(tabs->tabRect(scope).height())
                .arg(tabs->tabRect(scope).x()).arg(badge->width()).arg(badge->height())
                .arg(badge->x()).arg(tabs->width());
            QVERIFY2(tabs->rect().contains(badge->geometry()), qPrintable(message));
            QVERIFY2(tabs->tabRect(scope).contains(badge->geometry()), qPrintable(message));
            QCOMPARE(badge->visibleRegion().boundingRect(), badge->rect());
        };
        assertBadge(0, {});
        assertBadge(1, QStringLiteral("1"));

        for (const auto width : {compactWidth, wideWidth}) {
            window.resize(width == compactWidth ? 900 : 1400, 690);
            QCoreApplication::processEvents();
            setDirectoryWidth(width);
            QTRY_VERIFY(width == compactWidth ? tabs->width() < wideTabWidth : tabs->width() == wideTabWidth);
            const QRect first = tabs->tabRect(0);
            const QRect second = tabs->tabRect(1);
            for (int scope : {0, 1}) {
                tabs->setCurrentIndex(scope);
                for (const int count : {1, 10, 127, 0}) {
                    backend->chats[0].unread = scope == 0 ? 0 : count;
                    backend->taskChats[0].unread = scope == 0 ? count : 0;
                    emit backend->recentChatsLoaded(backend->chats);
                    emit backend->taskChatsLoaded(backend->taskChats);
                    QCoreApplication::processEvents();
                    const auto label = count > 99 ? QStringLiteral("99+")
                        : count > 0 ? QString::number(count) : QString();
                    assertBadge(scope, {});
                    assertBadge(1 - scope, label);
                    QCOMPARE(tabs->tabData(1 - scope).toLongLong(), qint64(count));
                    QCOMPARE(tabs->tabRect(0), first);
                    QCOMPARE(tabs->tabRect(1), second);
                    const auto screenshot = qEnvironmentVariable("B24_CHAT_TAB_GEOMETRY_SCREENSHOT");
                    if (!screenshot.isEmpty() && count == 127) {
                        QVERIFY(tabs->grab().save(QStringLiteral("%1-%2-%3.png").arg(screenshot).arg(width).arg(scope)));
                    }
                }
            }
        }
    }

    void upEditsLatestEligibleMessageOnlyFromEmptyComposer()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        backend->chats.first().id = QStringLiteral("chat91");
        backend->stages.first().tasks.first().chatDialogId = QStringLiteral("chat67");
        const auto now = QDateTime::currentDateTime();
        Message earlier{QStringLiteral("701"), QStringLiteral("Я"), QStringLiteral("Раннее"), now.addSecs(-40), true};
        Message latest{QStringLiteral("702"), QStringLiteral("Я"), QStringLiteral("Последнее своё"), now.addSecs(-30), true};
        Message incoming{QStringLiteral("703"), QStringLiteral("Другой"), QStringLiteral("Входящее"), now.addSecs(-20)};
        Message system{QStringLiteral("704"), {}, QStringLiteral("Системное"), now.addSecs(-10), true};
        system.system = true;
        backend->messages = {earlier, latest, incoming, system};
        backend->taskMessages = {earlier, latest, incoming};
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        emit backend->connectionChanged(true);

        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *composer = dynamic_cast<ui::ChatComposer *>(
            window.findChild<QFrame *>(QStringLiteral("chatComposer")));
        QVERIFY(history && composer);
        QTRY_COMPARE(history->count(), 4);
        auto *input = composer->input();
        history->setCurrentRow(3);
        QTest::keyClick(history, Qt::Key_Up);
        QCOMPARE(history->currentRow(), 2);
        QVERIFY(!composer->editingMessage());
        input->setFocus();
        QTest::keyClick(input, Qt::Key_Up, Qt::ShiftModifier);
        QVERIFY(!composer->editingMessage());
        input->setPlainText(QStringLiteral("Черновик"));
        QTest::keyClick(input, Qt::Key_Up);
        QCOMPARE(input->toPlainText(), QStringLiteral("Черновик"));
        QVERIFY(!composer->editingMessage());
        input->clear();
        ui::ChatReply reply;
        reply.id = incoming.id;
        reply.preview = incoming.text;
        composer->setReply(reply);
        QTest::keyClick(input, Qt::Key_Up);
        QVERIFY(!composer->editingMessage());
        QCOMPARE(composer->reply().id, reply.id);
        composer->setReply({});
        QInputMethodEvent preedit(QStringLiteral("か"), {});
        QApplication::sendEvent(input, &preedit);
        QTest::keyClick(input, Qt::Key_Up);
        QVERIFY(!composer->editingMessage());
        QInputMethodEvent endPreedit;
        QApplication::sendEvent(input, &endPreedit);
        QTest::keyClick(input, Qt::Key_Up);
        QVERIFY(composer->matchesEdit(QStringLiteral("chat91"), latest.id));
        QCOMPARE(input->toPlainText(), latest.text);
        QTest::keyClick(input, Qt::Key_Escape);
        QVERIFY(input->toPlainText().isEmpty());
        QVERIFY(backend->messageActionId.isEmpty());

        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        QApplication::setActiveWindow(card);
        auto *taskHistory = findAccessible<QListWidget>(card, QStringLiteral("Чат карточки"));
        auto *taskComposer = dynamic_cast<ui::ChatComposer *>(
            card->findChild<QFrame *>(QStringLiteral("chatComposer")));
        QVERIFY(taskHistory && taskComposer);
        QTRY_COMPARE(taskHistory->count(), 3);
        taskComposer->input()->setFocus();
        QTest::keyClick(taskComposer->input(), Qt::Key_Up);
        QVERIFY(taskComposer->matchesEdit(QStringLiteral("chat67"), latest.id));
        QCOMPARE(taskComposer->input()->toPlainText(), latest.text);
    }

    void growsComposerForVisualLinesAndShrinksAfterResize()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        auto *composer = new ui::ChatComposer(&host, QStringLiteral("Input"), QStringLiteral("Emoji"),
                                               QStringLiteral("File"), QStringLiteral("Send"));
        layout->addWidget(composer);
        layout->addStretch();
        host.resize(420, 440);
        host.show();
        auto *input = composer->input();
        QVERIFY(input->isVisible());
        const auto minimum = input->height();
        QTest::keyClick(input, Qt::Key_Return, Qt::ShiftModifier);
        QTRY_VERIFY(input->height() > minimum);
        input->clear();
        QTRY_COMPARE(input->height(), minimum);
        input->setPlainText(QStringLiteral("wrapped word ").repeated(9));
        QCOMPARE(input->document()->blockCount(), 1);
        QTRY_VERIFY(input->height() > minimum);
        const auto wrapped = input->height();
        host.resize(1200, 440);
        QTRY_VERIFY(input->height() < wrapped);
        input->setPlainText(QStringLiteral("line\n").repeated(35));
        QTRY_VERIFY(input->verticalScrollBar()->maximum() > 0);
        const auto capped = input->height();
        input->appendPlainText(QStringLiteral("line\n").repeated(20));
        QTRY_COMPARE(input->height(), capped);
        input->clear();
        QTRY_COMPARE(input->height(), minimum);
    }

    void reportsOnlyCommittedUserTextToTypingIndicator()
    {
        ui::ChatTextEdit input(nullptr);
        int edits = 0;
        input.userEdited = [&edits] { ++edits; };
        input.setPlainText(QStringLiteral("Черновик"));
        QCOMPARE(edits, 0);
        QInputMethodEvent preedit(QStringLiteral("か"), {});
        QApplication::sendEvent(&input, &preedit);
        QVERIFY(input.composing());
        QCOMPARE(edits, 0);
        QInputMethodEvent commit;
        commit.setCommitString(QStringLiteral("か"));
        QApplication::sendEvent(&input, &commit);
        QVERIFY(!input.composing());
        QCOMPARE(edits, 1);
        QTest::keyClicks(&input, QStringLiteral("x"));
        QCOMPARE(edits, 2);
    }

    void chatSendsFromKeyboardAndRestoresDraft()
    {
        DemoBackend demo;
        demo.start();
        MainWindow window(&demo);
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);

        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
        auto *messages = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *composer = findAccessible<QPlainTextEdit>(&window, QStringLiteral("Текст сообщения"));
        QVERIFY(chats);
        QVERIFY(messages);
        QVERIFY(composer);
        QTRY_COMPARE(messages->count(), 3);
        QTRY_VERIFY(chats->currentItem());
        composer->setFocus();

        const auto screenshotPath = qEnvironmentVariable("B24_CHAT_SCREENSHOT");
        if (!screenshotPath.isEmpty())
            QVERIFY(window.grab().save(screenshotPath));

        QTest::keyClicks(composer, QStringLiteral("First line"));
        QTest::keyClick(composer, Qt::Key_Return, Qt::ShiftModifier);
        QTest::keyClicks(composer, QStringLiteral("Second line"));
        QCOMPARE(composer->toPlainText(), QStringLiteral("First line\nSecond line"));
        chats->setCurrentRow(1);
        QVERIFY(composer->toPlainText().isEmpty());
        chats->setCurrentRow(0);
        QCOMPARE(composer->toPlainText(), QStringLiteral("First line\nSecond line"));

        QSignalSpy sent(&demo, &Backend::messageSent);
        QTest::keyClick(composer, Qt::Key_Return);
        QTRY_COMPARE(sent.count(), 1);
        QCOMPARE(sent.first().first().toString(), QStringLiteral("chat:c-1"));
        QTRY_VERIFY(composer->toPlainText().isEmpty());

        QTest::keyClick(&window, Qt::Key_3, Qt::AltModifier);
        auto *notifications = findAccessible<QListWidget>(&window, QStringLiteral("Уведомления"));
        QVERIFY(notifications);
        QTRY_COMPARE(notifications->count(), 1);
        QCOMPARE(notifications->item(0)->data(workspace::Id).toString(), QStringLiteral("n-2"));
    }

    void taskChatSearchUsesTaskPromptAndHidesGroupCreation()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        backend->accountScope = QStringLiteral("fixture/task-chat-search-first");
        MainWindow window(backend.get());
        window.resize(960, 720);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *tabs = findAccessible<QTabBar>(&window, QStringLiteral("Список чатов"));
        auto *query = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск чатов"));
        auto *create = findAccessible<QPushButton>(&window, QStringLiteral("Создать групповой чат"));
        QVERIFY(tabs && query && create);
        QVERIFY(create->isVisible());
        tabs->setCurrentIndex(1);
        QVERIFY(create->isHidden());
        QCOMPARE(query->placeholderText(), QStringLiteral("Название или номер задачи"));
        const auto taskNumber = QStringLiteral("№ 99123");
        query->setText(taskNumber);
        QTest::keyClick(query, Qt::Key_Return);
        QCOMPARE(backend->searchedTaskChats, taskNumber);
        QVERIFY(backend->searchedChats.isEmpty());
        Chat result;
        result.id = QStringLiteral("chat555");
        result.title = QStringLiteral("Сборка наличия");
        result.taskChat = true;
        result.taskId = QStringLiteral("99123");
        emit backend->taskChatsFound(taskNumber, {result});
        auto *rows = findAccessible<QListWidget>(&window, QStringLiteral("Чаты задач"));
        QVERIFY(rows);
        QCOMPARE(rows->count(), 1);
        QCOMPARE(rows->item(0)->data(workspace::Id).toString(), result.id);

        backend->accountScope = QStringLiteral("fixture/task-chat-search-second");
        emit backend->accountScopeChanged();
        QVERIFY(query->text().isEmpty());
        QCOMPARE(rows->count(), 0);
        emit backend->taskChatsFound(taskNumber, {result});
        QCOMPARE(rows->count(), 0);

        tabs->setCurrentIndex(0);
        QVERIFY(create->isVisible());
        QCOMPARE(query->placeholderText(), QStringLiteral("Поиск чатов"));
        query->setText(QStringLiteral("Команда"));
        QTest::keyClick(query, Qt::Key_Return);
        QCOMPARE(backend->searchedChats, QStringLiteral("Команда"));
        QCOMPARE(backend->searchedTaskChats, taskNumber);
    }

    void switchingChatTabsClosesHistoryEvenWithoutASelectedChat()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->chats.clear();
        MainWindow window(backend.get());
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("chatScopeTabs"));
        QVERIFY(tabs);
        QSignalSpy closed(&window, &MainWindow::historyClosed);
        tabs->setCurrentIndex(1);
        QCOMPARE(closed.count(), 1);
        QCOMPARE(closed.first().first().toBool(), false);
    }

    void replyDraftsStayScopedAndOpenExactOriginal()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto second = backend->chats.first();
        second.id = QStringLiteral("chat42");
        second.title = QStringLiteral("Другой чат");
        backend->chats.append(second);
        Message photo;
        photo.id = QStringLiteral("871");
        photo.outgoing = true;
        photo.files = {{QStringLiteral("88"), QStringLiteral("Снимок.jpg")}};
        Message reply;
        reply.id = QStringLiteral("872");
        reply.author = QStringLiteral("Анна");
        reply.text = QStringLiteral("Ответ");
        reply.replyId = QStringLiteral("999");
        backend->messages = {photo, reply};
        backend->taskMessages = {photo, reply};
        backend->stages[0].tasks[0].chatDialogId = QStringLiteral("chat51");
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
        auto *input = findAccessible<QPlainTextEdit>(&window, QStringLiteral("Текст сообщения"));
        auto *strip = findAccessible<QLabel>(&window, QStringLiteral("Цитата в черновике"));
        QVERIFY(history && chats && input && strip);
        QTRY_COMPARE(history->count(), 2);
        QCOMPARE(history->item(1)->data(workspace::ReplyPreview).toString(), QStringLiteral("Сообщение №999"));

        const auto chooseReply = [](QListWidget *list, int row) {
            list->setCurrentRow(row);
            const auto point = list->visualItemRect(list->item(row)).center();
            QContextMenuEvent event(QContextMenuEvent::Keyboard, point, list->viewport()->mapToGlobal(point));
            QApplication::sendEvent(list->viewport(), &event);
            auto *menu = visibleMessageMenu();
            QVERIFY(menu);
            QCOMPARE(menu->actions().first()->text(), QStringLiteral("Ответить"));
            menu->setActiveAction(menu->actions().first());
            QTest::keyClick(menu, Qt::Key_Return);
        };

        chooseReply(history, 0);
        QTRY_VERIFY(strip->isVisible());
        QVERIFY(strip->text().contains(QStringLiteral("Снимок.jpg")));
        input->setPlainText(QStringLiteral("Черновик"));
        chats->setCurrentRow(1);
        QVERIFY(input->toPlainText().isEmpty());
        QVERIFY(!strip->isVisible());
        chats->setCurrentRow(0);
        QCOMPARE(input->toPlainText(), QStringLiteral("Черновик"));
        QVERIFY(strip->isVisible());
        QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(backend->sentReplyScope, QStringLiteral("chat:c-1"));
        QCOMPARE(backend->sentReplyId, QStringLiteral("871"));
        QCOMPARE(backend->sentReplyText, QStringLiteral("Черновик"));
        emit backend->failed(QStringLiteral("send:chat:c-1"), QStringLiteral("Нет сети"));
        QCOMPARE(input->toPlainText(), QStringLiteral("Черновик"));
        QVERIFY(strip->isVisible());
        QTest::keyClick(input, Qt::Key_Return);
        chats->setCurrentRow(1);
        input->setPlainText(QStringLiteral("Другой черновик"));
        emit backend->messageSent(QStringLiteral("chat:c-1"), QStringLiteral("900"));
        QCOMPARE(input->toPlainText(), QStringLiteral("Другой черновик"));
        chats->setCurrentRow(0);
        QVERIFY(input->toPlainText().isEmpty());
        QVERIFY(!strip->isVisible());

        const auto clickQuote = [](QListWidget *list) {
            const auto row = list->visualItemRect(list->item(1));
            const auto *painter = static_cast<const workspace::RowPainter *>(list->itemDelegate());
            const auto bubble = painter->receiptArea(list->model()->index(1, 0), row);
            QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, bubble.topLeft() + QPoint(30, 30));
        };
        clickQuote(history);
        QCOMPARE(backend->contextDialog, QStringLiteral("c-1"));
        QCOMPARE(backend->contextMessage, QStringLiteral("999"));

        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        sections->setCurrentRow(1);
        window.openTaskCard(QStringLiteral("501"));
        auto *task = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(task);
        auto *taskHistory = findAccessible<QListWidget>(task, QStringLiteral("Чат карточки"));
        auto *taskInput = findAccessible<QPlainTextEdit>(task, QStringLiteral("Текст в чате карточки"));
        auto *taskStrip = findAccessible<QLabel>(task, QStringLiteral("Цитата в черновике"));
        QVERIFY(taskHistory && taskInput && taskStrip);
        QTRY_COMPARE(taskHistory->count(), 2);
        chooseReply(taskHistory, 1);
        QVERIFY(taskStrip->isVisible());
        taskInput->setPlainText(QStringLiteral("Ответ в задаче"));
        QTest::keyClick(taskInput, Qt::Key_Return);
        QCOMPARE(backend->sentReplyScope, QStringLiteral("task:501"));
        QCOMPARE(backend->sentReplyId, QStringLiteral("872"));
        QCOMPARE(backend->sentReplyText, QStringLiteral("Ответ в задаче"));
        emit backend->failed(QStringLiteral("send:task:501"), QStringLiteral("Нет сети"));
        auto *cancel = findAccessible<QPushButton>(task, QStringLiteral("Отменить ответ"));
        QVERIFY(cancel && cancel->isVisible());
        QTest::keyClick(cancel, Qt::Key_Space);
        QVERIFY(!taskStrip->isVisible());
        QCOMPARE(taskInput->toPlainText(), QStringLiteral("Ответ в задаче"));
        clickQuote(taskHistory);
        QCOMPARE(backend->contextDialog, QStringLiteral("chat51"));
        QCOMPARE(backend->contextMessage, QStringLiteral("999"));
    }

    void namesVisibleTypersWithoutRestartingTheExpiry()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto &personal = backend->chats.first();
        personal.id = QStringLiteral("22");
        personal.kind = QStringLiteral("im");
        personal.title = QStringLiteral("Вера Смирнова");
        const auto taskDialog = QStringLiteral("chat51");
        backend->stages.first().tasks.first().chatDialogId = taskDialog;
        MainWindow window(backend.get());
        window.show();
        QApplication::setActiveWindow(&window);
        auto *typing = findAccessible<QLabel>(&window, QStringLiteral("Индикатор набора сообщения"));
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
        QVERIFY(typing && chats);
        QTRY_VERIFY(chats->currentItem() && chats->currentItem()->data(workspace::Id).toString() == personal.id);
        emit backend->chatTypingChanged(QStringLiteral("chat999"), QStringLiteral("99"), 1000);
        emit backend->chatTypingChanged(personal.id, QStringLiteral("99"), 0);
        QVERIFY(backend->personRequests.isEmpty());

        emit backend->chatTypingChanged(personal.id, personal.id, 1000);
        QVERIFY(typing->isVisible());
        QCOMPARE(typing->text(), QStringLiteral("Вера Смирнова печатает…"));
        QCOMPARE(typing->textFormat(), Qt::PlainText);
        QVERIFY(backend->personRequests.isEmpty());

        const auto another = QStringLiteral("23");
        emit backend->chatTypingChanged(personal.id, another, 1000);
        emit backend->chatTypingChanged(personal.id, another, 1000);
        QCOMPARE(typing->text(), QStringLiteral("Печатает…"));
        QCOMPARE(backend->personRequests, QStringList{another});
        auto *composer = dynamic_cast<ui::ChatComposer *>(typing->parentWidget());
        QVERIFY(composer);
        auto &expiry = testClock(*composer);
        QVERIFY(expiry.isActive());
        QCOMPARE(expiry.interval(), 1000);
        // Qt can reuse a stopped timer's ID. Mark the next registration so a
        // restart during name resolution is detectable even on those versions.
        const auto registeredType = expiry.timerType();
        expiry.setTimerType(registeredType == Qt::PreciseTimer ? Qt::CoarseTimer : Qt::PreciseTimer);
        emit backend->personLoaded(Person{another, QStringLiteral("Павел <дежурный>"), {}, {}});
        QCOMPARE(typing->text(), QStringLiteral("Павел <дежурный> печатает…"));
        const auto registrations = QAbstractEventDispatcher::instance()->registeredTimers(&expiry);
        QCOMPARE(registrations.size(), 1);
        QCOMPARE(registrations.first().timerType, registeredType);
        fireTimer(expiry);
        QVERIFY(!typing->isVisible());
        emit backend->chatTypingChanged(personal.id, another, 1000);
        QCOMPARE(typing->text(), QStringLiteral("Павел <дежурный> печатает…"));
        QCOMPARE(backend->personRequests.size(), 1);

        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card && card->isVisible());
        auto *taskTyping = findAccessible<QLabel>(card, QStringLiteral("Индикатор набора сообщения"));
        QVERIFY(taskTyping);
        emit backend->chatTypingChanged(taskDialog, another, 1000);
        QCOMPARE(taskTyping->text(), QStringLiteral("Павел <дежурный> печатает…"));
        const auto next = QStringLiteral("24");
        emit backend->chatTypingChanged(taskDialog, next, 1000);
        QCOMPARE(backend->personRequests.last(), next);
        emit backend->chatTypingChanged(taskDialog, next, 0);
        emit backend->personLoaded(Person{next, QStringLiteral("Поздний ответ"), {}, {}});
        QVERIFY(!taskTyping->isVisible());
    }

    void chatHistoryTypingUsesComposerAndPreservesNavigation_data()
    {
        QTest::addColumn<bool>("readOnly");
        QTest::newRow("writable") << false;
        QTest::newRow("read-only") << true;
    }

    void chatHistoryTypingUsesComposerAndPreservesNavigation()
    {
        QFETCH(bool, readOnly);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->messages.first().text = QStringLiteral("Alpha");
        Message second;
        second.id = QStringLiteral("2");
        second.text = QStringLiteral("Beta");
        backend->messages.append(second);
        backend->taskMessages = backend->messages;
        Chat taskChat;
        taskChat.id = QStringLiteral("task-dialog");
        taskChat.taskChat = true;
        taskChat.title = QStringLiteral("Чат задачи");
        taskChat.taskId = QStringLiteral("501");
        backend->taskChats = {taskChat};
        MainWindow window(backend.get(), readOnly);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *input = findAccessible<QPlainTextEdit>(&window, QStringLiteral("Текст сообщения"));
        auto *search = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск по сообщениям"));
        QVERIFY(chats && history && input && search);
        QTRY_COMPARE(history->count(), 2);
        QTest::mouseClick(chats->viewport(), Qt::LeftButton, Qt::NoModifier, chats->visualItemRect(chats->item(0)).center());
        QTRY_COMPARE(input->hasFocus(), !readOnly);

        history->setFocus();
        history->setCurrentRow(0);
        QTest::keyClick(history, Qt::Key_Down);
        QCOMPARE(history->currentRow(), 1);
        history->setCurrentRow(0);
        QTest::keyClick(history, Qt::Key_B);
        QCOMPARE(history->currentRow(), 0);
        QCOMPARE(input->toPlainText(), readOnly ? QString() : QStringLiteral("b"));
        QTRY_COMPARE(input->hasFocus(), !readOnly);
        if (!readOnly) {
            history->setFocus();
            QInputMethodEvent ime;
            ime.setCommitString(QStringLiteral("текст"));
            QCoreApplication::sendEvent(history, &ime);
            QCOMPARE(input->toPlainText(), QStringLiteral("bтекст"));
        }

        history->setFocus();
        QTest::keyClick(history, Qt::Key_F, Qt::ControlModifier);
        QTRY_VERIFY(search->hasFocus());
        QTest::keyClicks(search, QStringLiteral("Beta"));
        QCOMPARE(search->text(), QStringLiteral("Beta"));
        QCOMPARE(input->toPlainText(), readOnly ? QString() : QStringLiteral("bтекст"));

        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("chatScopeTabs"));
        QVERIFY(tabs);
        tabs->setCurrentIndex(1);
        QTRY_COMPARE(chats->count(), 1);
        chats->setFocus();
        QTest::keyClick(chats, Qt::Key_Return);
        QTRY_COMPARE(input->hasFocus(), !readOnly);
        QVERIFY(input->toPlainText().isEmpty());
        history->setFocus();
        QTest::keyClick(history, Qt::Key_T);
        QCOMPARE(input->toPlainText(), readOnly ? QString() : QStringLiteral("t"));
        tabs->setCurrentIndex(0);
        QCOMPARE(input->toPlainText(), readOnly ? QString() : QStringLiteral("bтекст"));

        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        sections->setCurrentRow(1);
        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        auto *taskInput = findAccessible<QPlainTextEdit>(card, QStringLiteral("Текст в чате карточки"));
        auto *taskHistory = findAccessible<QListWidget>(card, QStringLiteral("Чат карточки"));
        QVERIFY(taskInput && taskHistory);
        QTRY_COMPARE(taskInput->hasFocus(), !readOnly);
        QTRY_COMPARE(taskHistory->count(), 2);
        taskHistory->setFocus();
        QTest::keyClick(taskHistory, Qt::Key_C);
        QCOMPARE(taskInput->toPlainText(), readOnly ? QString() : QStringLiteral("c"));
    }

    void mentionSuggestionsLoadVisiblePhotosAndKeepKeyboardSelection()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdAvatarResponses = true;
        const QUrl portrait(QStringLiteral("https://example.bitrix24.ru/upload/mention-anna.png"));
        const QUrl offscreen(QStringLiteral("https://example.bitrix24.ru/upload/mention-olga.png"));
        const auto name = QStringLiteral("Анна Орлова");
        const auto position = QStringLiteral("Руководитель отдела по работе с клиентами");
        backend->people = {{QStringLiteral("42"), name, position, portrait},
                           {QStringLiteral("43"), QStringLiteral("Михаил Новиков"), {}, {}}};
        for (int i = 0; i < 8; ++i)
            backend->people.append({QString::number(50 + i), QStringLiteral("Сотрудник %1").arg(i), {}, {}});
        backend->people.append({QStringLiteral("60"), QStringLiteral("Ольга Волкова"), {}, offscreen});

        MainWindow window(backend.get());
        window.resize(900, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *input = findAccessible<QPlainTextEdit>(&window, QStringLiteral("Текст сообщения"));
        auto *suggestions = findAccessible<QListWidget>(&window, QStringLiteral("Найденные сотрудники"));
        QVERIFY(input && suggestions);
        input->setFocus();
        QTest::keyClicks(input, "@");
        QTRY_VERIFY(suggestions->isVisible());
        QTRY_COMPARE(suggestions->count(), backend->people.size());
        QTRY_COMPARE(backend->avatarRequests.count(portrait), 1);
        QVERIFY(!backend->avatarRequests.contains(offscreen));
        QCOMPARE(suggestions->item(0)->font().weight(), QFont::DemiBold);
        QVERIFY(suggestions->item(0)->data(Qt::AccessibleTextRole).toString().contains(position));
        QVERIFY(!suggestions->item(1)->icon().isNull());

        QImage photo(36, 36, QImage::Format_RGB32);
        const QColor red(QStringLiteral("#ef392b"));
        photo.fill(red);
        QByteArray png;
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(photo.save(&buffer, "PNG"));
        emit backend->avatarLoaded(portrait, png);
        const auto photoColor = [suggestions] {
            const auto icon = suggestions->item(0)->icon().pixmap(22, 22).toImage();
            return icon.isNull() ? QColor() : icon.pixelColor(10, 10);
        };
        QTRY_COMPARE(photoColor(), red);

        suggestions->scrollToItem(suggestions->item(suggestions->count() - 1));
        QTRY_COMPARE(backend->avatarRequests.count(offscreen), 1);
        QTest::keyClick(input, Qt::Key_Down);
        QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(input->toPlainText().trimmed(), QStringLiteral("@") + name);
        QVERIFY(!suggestions->isVisible());
        QVERIFY(backend->sentChat.isEmpty());
    }

    void scopedMentionsSendFromChatAndTaskCardWithLazyTaskChoices()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto &task = backend->stages.first().tasks.first();
        task.projectId = QStringLiteral("101");
        task.project = QStringLiteral("Проект один");
        task.chatDialogId = QStringLiteral("chat451");
        task.editRights.project = true;
        backend->people = {{QStringLiteral("42"), QStringLiteral("Анна"), {}, {}}};
        backend->projects.append({QStringLiteral("102"), QStringLiteral("Проект два"), {}});
        backend->projectStageChoices = {{QStringLiteral("601"), QStringLiteral("Работа"), {}, {}},
                                        {QStringLiteral("602"), QStringLiteral("Готово"), {}, {}}};
        backend->moreProjects = {{QStringLiteral("103"), QStringLiteral("Проект с новой страницы"), {}}};
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *input = findAccessible<QPlainTextEdit>(&window, QStringLiteral("Текст сообщения"));
        QVERIFY(input);
        input->setFocus();
        QTest::keyClicks(input, "@");
        QTRY_COMPARE(backend->peopleRequests.size(), 1);
        QCOMPARE(backend->peopleRequests.last().first, QString());
        QCOMPARE(backend->peopleRequests.last().second, QStringLiteral("c-1"));
        QTest::keyClick(input, Qt::Key_Down);
        QTest::keyClick(input, Qt::Key_Return);
        QVERIFY(backend->sentChat.isEmpty());
        QCOMPARE(input->toPlainText().trimmed(), QStringLiteral("@Анна"));
        QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(backend->sentChat, QStringLiteral("[USER=42]Анна[/USER]"));

        QCOMPARE(backend->projectRequests, 0);
        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        QApplication::setActiveWindow(card);
        QCOMPARE(backend->projectRequests, 1);
        QCOMPARE(backend->projectStageRequests, QStringList{QStringLiteral("101")});
        auto *stage = findAccessible<QListWidget>(card, QStringLiteral("Этап задачи"));
        auto *move = findAccessible<QPushButton>(card, QStringLiteral("Повторить перенос задачи"));
        QVERIFY(stage && move && stage->count() == 2);
        auto *currentStage = findAccessible<QLabel>(card, QStringLiteral("Этап"));
        QVERIFY(currentStage);
        QCOMPARE(currentStage->text(), QStringLiteral("Работа"));
        auto *requisite = findAccessible<QPushButton>(card, QStringLiteral("Проект"));
        QVERIFY(requisite && requisite->isEnabled());
        requisite->click();
        auto *moreProjects = findAccessible<QPushButton>(card, QStringLiteral("Загрузить следующие проекты"));
        auto *projectChoices = findAccessible<QListWidget>(card, QStringLiteral("Проект задачи"));
        QVERIFY(moreProjects && moreProjects->isVisible() && projectChoices);
        QVERIFY(projectChoices->findItems(QStringLiteral("Проект с новой страницы"), Qt::MatchExactly).isEmpty());
        moreProjects->click();
        QCOMPARE(backend->projectMoreRequests, 1);
        QVERIFY(!projectChoices->findItems(QStringLiteral("Проект с новой страницы"), Qt::MatchExactly).isEmpty());
        auto *cancelRequisite = findAccessible<QPushButton>(card, QStringLiteral("Отменить изменение реквизита"));
        QVERIFY(cancelRequisite);
        cancelRequisite->click();
        auto *taskInput = findAccessible<QPlainTextEdit>(card, QStringLiteral("Текст в чате карточки"));
        QVERIFY(taskInput);
        taskInput->setFocus();
        QTest::keyClicks(taskInput, "+");
        QTRY_COMPARE(backend->peopleRequests.size(), 2);
        QCOMPARE(backend->peopleRequests.last().first, QString());
        QCOMPARE(backend->peopleRequests.last().second, QStringLiteral("chat451"));
        QTest::keyClick(taskInput, Qt::Key_Down);
        QTest::keyClick(taskInput, Qt::Key_Return);
        QVERIFY(backend->sentTask.isEmpty());
        QCOMPARE(taskInput->toPlainText().trimmed(), QStringLiteral("@Анна"));
        QTest::keyClick(taskInput, Qt::Key_Return);
        QCOMPARE(backend->sentTask, QStringLiteral("[USER=42]Анна[/USER]"));

        backend->holdTaskUpdate = true;
        auto *stageRequisite = findAccessible<QPushButton>(card, QStringLiteral("Этап"));
        QVERIFY(stageRequisite && stageRequisite->isEnabled());
        stageRequisite->click();
        QVERIFY(stage->isVisible());
        stage->setCurrentRow(1);
        QVERIFY(!move->isVisible());
        QTest::keyClick(stage, Qt::Key_Return);
        QCOMPARE(backend->lastUpdate.stageId, std::optional<QString>(QStringLiteral("602")));
        QCOMPARE(backend->updatedTask, QStringLiteral("501"));
        QVERIFY(backend->movedTask.isEmpty());
    }

    void refreshesTheSelectedHistoryFirstAndKeepsItsDraft()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        MainWindow window(backend.get());
        window.show();
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *input = findAccessible<QPlainTextEdit>(&window, QStringLiteral("Текст сообщения"));
        auto *refresh = findAccessible<QPushButton>(&window, QStringLiteral("Обновить чат (F5)"));
        if (!refresh)
            refresh = findAccessible<QPushButton>(&window, QStringLiteral("Обновить чаты (F5)"));
        QVERIFY(history && input && refresh);
        QTRY_VERIFY(history->count() > 0);
        input->setPlainText(QStringLiteral("Черновик"));

        const auto verifyRefresh = [&] {
            QCOMPARE(backend->chatRefreshCalls, QStringList({QStringLiteral("c-1"), QStringLiteral("directory")}));
            QCOMPARE(input->toPlainText(), QStringLiteral("Черновик"));
            QVERIFY(history->count() > 0);
        };
        backend->chatRefreshCalls.clear();
        refresh->click();
        verifyRefresh();
        backend->chatRefreshCalls.clear();
        QApplication::setActiveWindow(&window);
        input->setFocus();
        QTest::keyClick(input, Qt::Key_F5);
        verifyRefresh();
    }

    void searchesChatsAndMessagesWithoutPushOverwrite()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);

        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
        auto *messages = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(chats);
        QVERIFY(messages);
        QTRY_COMPARE(messages->count(), 1);

        auto *chatSearch = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск чатов"));
        QVERIFY(chatSearch);
        chatSearch->setText(QStringLiteral("работ"));
        QTest::keyClick(chatSearch, Qt::Key_Return);
        QCOMPARE(backend->searchedChats, QStringLiteral("работ"));

        Chat found;
        found.id = QStringLiteral("c-9");
        found.title = QStringLiteral("Найденный чат");
        found.lastMessage = QStringLiteral("[B]Из поиска[/B]");
        emit backend->chatsFound(QStringLiteral("работ"), QVector<Chat>{found});
        emit backend->recentChatsLoaded(backend->chats);
        QTRY_COMPARE(chats->count(), 1);
        QCOMPARE(chats->item(0)->data(workspace::Title).toString(), QStringLiteral("Найденный чат"));
        QCOMPARE(chats->item(0)->data(workspace::Detail).toString(), QStringLiteral("Из поиска"));

        auto *messageSearch = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск по сообщениям"));
        QVERIFY(messageSearch);
        auto *showMessageSearch = findAccessible<QPushButton>(&window, QStringLiteral("Найти в чате"));
        QVERIFY(showMessageSearch);
        QVERIFY(!messageSearch->isVisible());
        QTest::mouseClick(showMessageSearch, Qt::LeftButton);
        QTRY_VERIFY(messageSearch->isVisible() && messageSearch->hasFocus());
        messageSearch->setText(QStringLiteral("Привет"));
        QTest::keyClick(messageSearch, Qt::Key_Return);
        QCOMPARE(backend->searchedScope, QStringLiteral("c-1"));
        QCOMPARE(backend->searchedQuery, QStringLiteral("Привет"));
        QCOMPARE(backend->searchedTask, false);

        Message hit;
        hit.id = QStringLiteral("m-9");
        hit.author = QStringLiteral("Иван");
        hit.text = QStringLiteral("Привет [B]из поиска[/B]");
        emit backend->messagesFound(QStringLiteral("c-1"), QStringLiteral("Привет"), false,
                                    QVector<Message>{hit});
        QTRY_COMPARE(messages->count(), 1);

        Message push;
        push.id = QStringLiteral("m-10");
        push.text = QStringLiteral("Свежее сообщение");
        emit backend->chatMessagesLoaded(QStringLiteral("c-1"), QVector<Message>{push, push});
        QCoreApplication::processEvents();
        QCOMPARE(messages->count(), 1);
        QCOMPARE(messages->item(0)->data(workspace::Detail).toString(), QStringLiteral("Привет из поиска"));

        auto *clear = findAccessible<QPushButton>(&window, QStringLiteral("Очистить поиск по сообщениям"));
        QVERIFY(clear);
        clear->click();
        QTRY_COMPARE(messages->count(), 1);
        QTest::mouseClick(showMessageSearch, Qt::LeftButton);
        QVERIFY(!messageSearch->isVisible());
        QCOMPARE(messages->item(0)->data(workspace::Detail).toString(), backend->messages.front().text);

        QTest::mouseClick(showMessageSearch, Qt::LeftButton);
        QTRY_VERIFY(messageSearch->hasFocus());
        messageSearch->setText(QStringLiteral("Нет такого сообщения"));
        QTest::keyClick(messageSearch, Qt::Key_Return);
        const auto query = backend->searchedQuery;
        QCOMPARE(query, QStringLiteral("Нет такого сообщения"));
        emit backend->messagesFound(QStringLiteral("c-1"), query, false, {});
        QTest::keyClick(messageSearch, Qt::Key_Escape);
        QVERIFY(!messageSearch->isVisible());
        QVERIFY(showMessageSearch->hasFocus());
        QCOMPARE(messages->item(0)->data(workspace::Detail).toString(), backend->messages.front().text);
    }

    void chatDirectoriesKeepDraftSearchSelectionAndConfirmedPin()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto makeChat = [](const char *id, const QString &title, int unread = 0, bool pinned = false) {
            Chat chat;
            chat.id = QString::fromLatin1(id);
            chat.title = title;
            chat.unread = unread;
            chat.pinned = pinned;
            chat.kind = QStringLiteral("im");
            return chat;
        };
        const QUrl photo(QStringLiteral("https://example.bitrix24.ru/avatar.png"));
        {
            QBuffer buffer(&backend->avatarBytes);
            QVERIFY(buffer.open(QIODevice::WriteOnly));
            QImage image(180, 150, QImage::Format_ARGB32);
            image.fill(QColor("#53938a"));
            QVERIFY(image.save(&buffer, "PNG"));
        }
        const auto regular = makeChat("second", QStringLiteral("Анна Лебедева"));
        auto unread = makeChat("unread", QStringLiteral("Поддержка"), 4);
        unread.avatarUrl = photo;
        auto manuallyUnread = makeChat("marked", QStringLiteral("Документы"));
        manuallyUnread.markedUnread = true;
        backend->chats += {regular, unread, manuallyUnread, makeChat("pinned", QStringLiteral("Команда"), 0, true)};
        auto task = makeChat("task-dialog", QStringLiteral("Починить кран"), 2);
        task.taskChat = true;
        task.taskId = QStringLiteral("501");
        backend->taskChats = {makeChat("another-task", QStringLiteral("Обновить справку")), task};
        backend->taskMessages.first().author = QStringLiteral("Анна Лебедева");
        backend->taskMessages.first().authorAvatarUrl = photo;
        backend->taskMessages.first().text = QStringLiteral("Смета уже у поставщика, можно начинать.");
        backend->taskMessages.first().forwarded = true;
        backend->taskMessages.first().forwardedAuthor = QStringLiteral("Иван Соколов");
        Message own;
        own.id = QStringLiteral("my-message");
        own.outgoing = true;
        own.text = QStringLiteral("Начинаем сегодня.");
        own.replyId = backend->taskMessages.first().id;
        backend->taskMessages.append(own);
        backend->projects.first().avatarUrl = photo;
        Project pinnedProject;
        pinnedProject.id = QStringLiteral("102");
        pinnedProject.name = QStringLiteral("Поддержка");
        pinnedProject.pinned = true;
        backend->projects.append(pinnedProject);
        MainWindow window(backend.get());
        window.resize(720, 500);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("chatScopeTabs"));
        auto *list = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
        auto *search = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск чатов"));
        auto *input = findAccessible<QPlainTextEdit>(&window, QStringLiteral("Текст сообщения"));
        QVERIFY(tabs);
        QVERIFY(list);
        QVERIFY(search);
        QVERIFY(input);
        const auto idAt = [](QListWidget *rows, int row) {
            const auto *item = rows->item(row);
            return item ? item->data(workspace::Id).toString() : QString();
        };
        QCOMPARE(idAt(list, 0), QStringLiteral("pinned"));
        QCOMPARE(idAt(list, 1), unread.id);
        QCOMPARE(idAt(list, 2), manuallyUnread.id);
        QVERIFY(list->item(2)->data(workspace::MarkedUnread).toBool());
        QTRY_VERIFY(!list->item(1)->data(workspace::AvatarImage).value<QPixmap>().isNull());
        QCOMPARE(backend->avatarRequests.count(photo), 1);
        list->setCurrentRow(4);
        QCOMPARE(idAt(list, 4), regular.id);
        input->setPlainText(QStringLiteral("Обычный черновик"));
        search->setText(QStringLiteral("Анна"));
        QTest::keyClick(search, Qt::Key_Return);
        emit backend->chatsFound(QStringLiteral("Анна"), {regular});
        tabs->setCurrentIndex(1);
        QVERIFY(search->text().isEmpty());
        QCOMPARE(idAt(list, 0), task.id);
        QCOMPARE(list->item(0)->data(workspace::Title).toString(), task.title);
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        QCOMPARE(history->item(1)->data(workspace::ReplyPreview).toString(), backend->taskMessages.first().text);
        QVERIFY(input->toPlainText().isEmpty());
        input->setPlainText(QStringLiteral("Черновик задачи"));
        search->setText(QStringLiteral("кран"));
        QTest::keyClick(search, Qt::Key_Return);
        QCOMPARE(backend->searchedTaskChats, QStringLiteral("кран"));
        emit backend->chatsFound(QStringLiteral("Анна"), {regular});
        emit backend->taskChatsFound(QStringLiteral("кран"), {task});
        QCOMPARE(idAt(list, 0), task.id);
        auto *openTask = findAccessible<QPushButton>(&window, QStringLiteral("Открыть карточку задачи"));
        QVERIFY(openTask && openTask->isVisible());
        openTask->click();
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        card->close();
        tabs->setCurrentIndex(0);
        QCOMPARE(search->text(), QStringLiteral("Анна"));
        QCOMPARE(idAt(list, 0), regular.id);
        QCOMPARE(input->toPlainText(), QStringLiteral("Обычный черновик"));
        QVERIFY(!openTask->isVisible());
        tabs->setCurrentIndex(1);
        QCOMPARE(search->text(), QStringLiteral("кран"));
        QCOMPARE(input->toPlainText(), QStringLiteral("Черновик задачи"));
        auto *clear = findAccessible<QPushButton>(&window, QStringLiteral("Очистить поиск чатов"));
        QVERIFY(clear);
        clear->click();

        auto openPin = [](QListWidget *rows, int row, const QString &label) {
            const auto point = rows->visualItemRect(rows->item(row)).center();
            emit rows->customContextMenuRequested(point);
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            QVERIFY(menu);
            QCOMPARE(menu->actions().front()->text(), label);
            menu->actions().front()->trigger();
            menu->close();
        };
        openPin(list, 1, QStringLiteral("Закрепить"));
        QCOMPARE(backend->pinTarget, QStringLiteral("another-task"));
        QVERIFY(backend->pinValue);
        QCOMPARE(idAt(list, 0), task.id);
        emit backend->chatPinChanged(backend->pinTarget, true);
        QCOMPARE(idAt(list, 0), QStringLiteral("another-task"));
        QCOMPARE(list->currentItem()->data(workspace::Id).toString(), task.id);
        QCOMPARE(input->toPlainText(), QStringLiteral("Черновик задачи"));
        const auto capture = qEnvironmentVariable("B24_DIRECTORY_COMPACT_SCREENSHOT");
        if (!capture.isEmpty()) {
            QCoreApplication::processEvents();
            QVERIFY(window.grab().save(capture));
        }

        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        sections->setCurrentRow(1);
        auto *projects = findAccessible<QListWidget>(&window, QStringLiteral("Проекты"));
        QVERIFY(projects);
        QCOMPARE(idAt(projects, 0), pinnedProject.id);
        QTRY_VERIFY(!projects->item(1)->data(workspace::AvatarImage).value<QPixmap>().isNull());
        QCOMPARE(backend->avatarRequests.count(photo), 1);
        const auto projectCapture = qEnvironmentVariable("B24_PROJECT_AVATAR_SCREENSHOT");
        if (!projectCapture.isEmpty()) {
            projects->setCurrentRow(1);
            QCoreApplication::processEvents();
            QVERIFY(window.grab().save(projectCapture));
            projects->setCurrentRow(0);
        }
        openPin(projects, 0, QStringLiteral("Открепить"));
        QCOMPARE(backend->pinTarget, pinnedProject.id);
        QVERIFY(!backend->pinValue);
        emit backend->failed(QStringLiteral("project-pin:102"), QStringLiteral("Ошибка записи"));
        QCOMPARE(idAt(projects, 0), pinnedProject.id);
        openPin(projects, 0, QStringLiteral("Открепить"));
        emit backend->projectPinChanged(pinnedProject.id, false);
        QCOMPARE(idAt(projects, 0), QStringLiteral("101"));
        QCOMPARE(projects->currentItem()->data(workspace::Id).toString(), pinnedProject.id);
    }

    void chatDirectoryWidthPersistsAfterDragging()
    {
        QSettings settings(QStringLiteral("bitrix24-client"), QStringLiteral("bitrix24-client"));
        const auto key = QStringLiteral("ui/chatDirectoryWidth");
        settings.remove(key);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        int savedWidth = 0;
        {
            MainWindow window(backend.get());
            window.resize(960, 650);
            window.show();
            auto *splitter = window.findChild<QSplitter *>(QStringLiteral("chatDirectorySplitter"));
            QVERIFY(splitter);
            QVERIFY(splitter->sizes().first() >= 196);
            auto *handle = splitter->handle(1);
            const auto start = handle->rect().center();
            const int old = splitter->sizes().first();
            QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, start);
            QTest::mouseMove(handle, start + QPoint(63, 0));
            QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier, start + QPoint(63, 0));
            QTRY_VERIFY(splitter->sizes().first() > old + 30);
            savedWidth = splitter->sizes().first();
            QCOMPARE(settings.value(key).toInt(), savedWidth);
        }
        MainWindow restored(backend.get());
        restored.resize(960, 650);
        restored.show();
        auto *splitter = restored.findChild<QSplitter *>(QStringLiteral("chatDirectorySplitter"));
        QVERIFY(splitter);
        QTRY_COMPARE(splitter->sizes().first(), savedWidth);
        restored.resize(720, 500);
        QTRY_VERIFY(splitter->sizes().at(1) >= 325);
        restored.resize(960, 650);
        QTRY_COMPARE(splitter->sizes().first(), savedWidth);
        settings.remove(key);
    }

    void emojiPanelAnchorsToBothComposers()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        const auto checkPanel = [](QWidget *host, const QString &inputName, const QString &toggleName,
                                   const char *screenshotVariable) {
            auto *input = findAccessible<QPlainTextEdit>(host, inputName);
            auto *toggle = findAccessible<QPushButton>(host, toggleName);
            QVERIFY(input);
            QVERIFY(toggle);
            input->setPlainText(QStringLiteral("До встречи"));
            auto cursor = input->textCursor();
            cursor.setPosition(2);
            input->setTextCursor(cursor);
            toggle->click();
            auto *panel = host->findChild<QWidget *>(QStringLiteral("emojiPanel"));
            QVERIFY(panel);
            QVERIFY(panel->isVisible());
            QVERIFY(!panel->isWindow());
            QVERIFY(panel->parentWidget()->rect().contains(panel->geometry()));
            const auto screenshot = qEnvironmentVariable(screenshotVariable);
            if (!screenshot.isEmpty()) {
                QCoreApplication::processEvents();
                QVERIFY(host->grab().save(screenshot));
            }

            auto *tile = findAccessible<QPushButton>(panel, QStringLiteral("Улыбка"));
            QVERIFY(tile);
            QVERIFY(tile->width() >= 40);
            QVERIFY(tile->height() >= 38);
            tile->click();
            QCOMPARE(input->toPlainText(), QStringLiteral("До😀 встречи"));
            QTRY_VERIFY(input->hasFocus());
            if (!panel->isVisible())
                toggle->click();
            QTest::keyClick(input, Qt::Key_Escape);
            QVERIFY(!panel->isVisible());
            QVERIFY(host->isVisible());
            QCOMPARE(input->toPlainText(), QStringLiteral("До😀 встречи"));

            toggle->click();
            QVERIFY(panel->isVisible());
            QTest::mouseClick(input->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(8, 8));
            QVERIFY(!panel->isVisible());
        };

        checkPanel(&window, QStringLiteral("Текст сообщения"), QStringLiteral("Эмодзи в чате"),
                   "B24_CHAT_EMOJI_SCREENSHOT");
        window.resize(720, 500);
        checkPanel(&window, QStringLiteral("Текст сообщения"), QStringLiteral("Эмодзи в чате"),
                   "B24_COMPACT_CHAT_SCREENSHOT");

        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        checkPanel(card, QStringLiteral("Текст в чате карточки"), QStringLiteral("Эмодзи в карточке"),
                   "B24_TASK_EMOJI_SCREENSHOT");
    }

    void uploadsKeepDraftAndCorrelateReplies_data()
    {
        QTest::addColumn<bool>("changeAccount");
        QTest::newRow("same account") << false;
        QTest::newRow("late reply after account switch") << true;
    }

    void uploadsKeepDraftAndCorrelateReplies()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        QFETCH(bool, changeAccount);
        backend->accountScope = QStringLiteral("fixture/upload-A");
        backend->holdUpload = true;
        MainWindow window(backend.get());
        window.show();
        QApplication::setActiveWindow(&window);
        auto *fileButton = findAccessible<QPushButton>(&window, QStringLiteral("Прикрепить файл в чат"));
        auto *composer = findAccessible<QPlainTextEdit>(&window, QStringLiteral("Текст сообщения"));
        QVERIFY(fileButton);
        QVERIFY(composer);
        composer->setPlainText(QStringLiteral("Черновик"));

        QTemporaryFile file;
        QVERIFY(file.open());
        const auto path = file.fileName();
        file.close();
        QTimer::singleShot(0, &window, [path] {
            if (auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                dialog->selectFile(path);
                QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
            }
        });
        QTimer::singleShot(1500, &window, [] {
            if (auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget()))
                dialog->reject();
        });
        fileButton->click();
        QCOMPARE(backend->uploadedPath, path);
        QVERIFY(!fileButton->isEnabled());
        if (changeAccount) {
            backend->accountScope = QStringLiteral("fixture/upload-B");
            emit backend->accountScopeChanged();
            emit backend->recentChatsLoaded(backend->chats);
            QVERIFY(fileButton->isEnabled());
            composer->setPlainText(QStringLiteral("Черновик B"));
            auto *status = findAccessible<QLabel>(&window, QStringLiteral("Состояние"));
            QVERIFY(status);
            const auto previousStatus = status->text();
            const auto requests = backend->chatRefreshCalls;
            emit backend->fileUploaded(backend->uploadedScope, QStringLiteral("chat"));
            QCOMPARE(status->text(), previousStatus);
            QCOMPARE(backend->chatRefreshCalls, requests);
            emit backend->failed(QStringLiteral("upload-chat:") + backend->uploadedScope, QStringLiteral("Ошибка A"));
            QCOMPARE(status->text(), previousStatus);
            QCOMPARE(composer->toPlainText(), QStringLiteral("Черновик B"));
            QVERIFY(fileButton->isEnabled());
            return;
        }
        emit backend->fileUploaded(QStringLiteral("c-1"), QStringLiteral("task"));
        emit backend->failed(QStringLiteral("upload-chat:c-9"), QStringLiteral("Другой запрос"));
        QVERIFY(!fileButton->isEnabled());
        emit backend->failed(QStringLiteral("upload-chat:c-1"), QStringLiteral("Файл не загружен"));
        QVERIFY(fileButton->isEnabled());
        QVERIFY(backend->sentChat.isEmpty());
        QCOMPARE(composer->toPlainText(), QStringLiteral("Черновик"));
    }

    void paintsEmojiInChatListPreviews()
    {
        if (!QFontDatabase::hasFamily(QStringLiteral("Noto Color Emoji")))
            QSKIP("Noto Color Emoji is required for the color rendering check");
        const auto applicationFont = QApplication::font();
        const auto restoreFont = qScopeGuard([applicationFont] { QApplication::setFont(applicationFont); });
        auto textFont = applicationFont;
        textFont.setStyleStrategy(QFont::NoFontMerging);
        QApplication::setFont(textFont);

        QListWidget chats;
        auto *delegate = workspace::configureRows(&chats, workspace::Row::Directory);
        const auto smile = QString::fromUtf8("😀");
        Chat chat;
        chat.title = QStringLiteral("Messages");
        chat.lastMessage = smile;
        ui::addChatRow(&chats, chat);
        chat.taskChat = true;
        ui::addChatRow(&chats, chat);

        for (int row = 0; row < chats.count(); ++row) {
            QImage pixels(220, 56, QImage::Format_RGB32);
            pixels.fill(Qt::white);
            QStyleOptionViewItem option;
            option.rect = pixels.rect();
            QPainter painter(&pixels);
            delegate->paint(&painter, option, chats.model()->index(row, 0));
            painter.end();

            bool hasYellow = false;
            for (int y = 29; y < 46; ++y) {
                for (int x = 54; x < 85; ++x) {
                    const auto pixel = pixels.pixelColor(x, y);
                    hasYellow |= pixel.red() > 200 && pixel.green() > 130 && pixel.blue() < 110;
                }
            }
            QVERIFY2(hasYellow, row == 0 ? "Ordinary chat preview" : "Task chat preview");
        }
    }

    void exposesDistinctBadgesInChatRows()
    {
        QListWidget chats;
        Chat ordinary;
        ordinary.unread = 127;
        ordinary.pinned = true;
        ordinary.hasMention = true;
        auto *first = ui::addChatRow(&chats, ordinary);
        Chat task;
        task.taskChat = true;
        task.unread = 7;
        task.hasReaction = true;
        auto *second = ui::addChatRow(&chats, task);

        QCOMPARE(first->data(workspace::Count).toInt(), 127);
        QVERIFY(first->data(workspace::Pinned).toBool());
        QVERIFY(first->data(workspace::HasMention).toBool());
        QVERIFY(!first->data(workspace::HasReaction).toBool());
        QCOMPARE(second->data(workspace::Count).toInt(), 7);
        QVERIFY(!second->data(workspace::HasMention).toBool());
        QVERIFY(second->data(workspace::HasReaction).toBool());
        QVERIFY(first->data(Qt::AccessibleDescriptionRole).toString().contains(QStringLiteral("упоминание")));
        QVERIFY(second->data(Qt::AccessibleDescriptionRole).toString().contains(QStringLiteral("реакция")));
    }

    void dismissedChatsStayHiddenUnlessEnabledForThisAccount()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        backend->accountScope = QStringLiteral("example.bitrix24.ru/first-user");
        Chat dismissed;
        dismissed.id = QStringLiteral("453");
        dismissed.title = QStringLiteral("Бывший сотрудник");
        dismissed.kind = QStringLiteral("im");
        dismissed.pinned = true;
        dismissed.dismissed = true;
        backend->chats.append(dismissed);
        Chat group;
        group.id = QStringLiteral("chat475");
        group.title = QStringLiteral("Группа");
        group.kind = QStringLiteral("group");
        backend->chats.append(group);
        Notification notice;
        notice.id = QStringLiteral("n-dismissed");
        notice.title = QStringLiteral("Событие в чате");
        notice.targets = {{NotificationTargetKind::Chat, dismissed.id, QStringLiteral("421"),
                           {}, QStringLiteral("Открыть сообщение")}};
        backend->notifications = {notice};
        const auto contains = [](QListWidget *list, const QString &id) {
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->data(workspace::Id).toString() == id)
                    return true;
            return false;
        };

        {
            MainWindow window(backend.get());
            window.show();
            auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
            auto *profile = findAccessible<QPushButton>(&window, QStringLiteral("Настройки профиля"));
            auto *query = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск чатов"));
            auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
            auto *notices = findAccessible<QListWidget>(&window, QStringLiteral("Уведомления"));
            QVERIFY(chats && profile && query && sections && notices);
            QVERIFY(profile->isVisible());
            QVERIFY(contains(chats, group.id));
            QVERIFY(!contains(chats, dismissed.id));

            sections->setCurrentRow(2);
            notices->setCurrentRow(0);
            QApplication::setActiveWindow(&window);
            notices->setFocus();
            QTest::keyClick(notices, Qt::Key_Return, Qt::ControlModifier);
            QCOMPARE(backend->contextDialog, dismissed.id);
            QCOMPARE(backend->contextMessage, QStringLiteral("421"));
            QVERIFY(!contains(chats, dismissed.id));

            query->setText(QStringLiteral("сотрудник"));
            QTest::keyClick(query, Qt::Key_Return);
            Chat external;
            external.id = QStringLiteral("765");
            external.title = QStringLiteral("Внешний участник");
            emit backend->chatsFound(query->text(), {dismissed, external});
            QVERIFY(!contains(chats, dismissed.id));
            QVERIFY(contains(chats, external.id));

            profile->click();
            auto *check = findAccessible<QCheckBox>(
                &window, QStringLiteral("Показывать чаты с уволенными сотрудниками"));
            QVERIFY(check && check->isVisible() && !check->isChecked());
            check->click();
            QVERIFY(contains(chats, dismissed.id));
            QCOMPARE(chats->item(0)->data(workspace::Id).toString(), dismissed.id);
            query->clear();
            QVERIFY(contains(chats, dismissed.id));
            QVERIFY(contains(chats, group.id));
        }

        MainWindow restored(backend.get());
        restored.show();
        auto *chats = findAccessible<QListWidget>(&restored, QStringLiteral("Недавние чаты"));
        auto *profile = findAccessible<QPushButton>(&restored, QStringLiteral("Настройки профиля"));
        QVERIFY(chats && profile);
        QVERIFY(contains(chats, dismissed.id));
        profile->click();
        auto *check = findAccessible<QCheckBox>(
            &restored, QStringLiteral("Показывать чаты с уволенными сотрудниками"));
        QVERIFY(check && check->isChecked());
        backend->accountScope = QStringLiteral("example.bitrix24.ru/second-user");
        emit backend->accountScopeChanged();
        emit backend->recentChatsLoaded(backend->chats);
        QVERIFY(check->isEnabled());
        QVERIFY(!check->isChecked());
        QVERIFY(!contains(chats, dismissed.id));
        backend->accountScope.clear();
        emit backend->accountScopeChanged();
        QVERIFY(!check->isEnabled());
        QVERIFY(!check->isChecked());
        check->click();
        QVERIFY(!contains(chats, dismissed.id));
        MainWindow unscoped(backend.get());
        auto *unscopedChats = findAccessible<QListWidget>(&unscoped, QStringLiteral("Недавние чаты"));
        QVERIFY(unscopedChats);
        QVERIFY(!contains(unscopedChats, dismissed.id));
        backend->accountScope = QStringLiteral("example.bitrix24.ru/first-user");
        emit backend->accountScopeChanged();
        emit backend->recentChatsLoaded(backend->chats);
        QVERIFY(check->isEnabled());
        QVERIFY(check->isChecked());
        QVERIFY(contains(chats, dismissed.id));
    }

    void pastedChatPictureWaitsInComposer()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        auto *input = findAccessible<QPlainTextEdit>(&window, QStringLiteral("Текст сообщения"));
        QVERIFY(input);

        QImage image(80, 40, QImage::Format_ARGB32);
        image.fill(QColor("#42788f"));
        auto *clipboard = QApplication::clipboard();
        auto *mime = new QMimeData;
        mime->setImageData(image);
        clipboard->setMimeData(mime);
        const auto clearClipboard = qScopeGuard([clipboard] { clipboard->clear(); });
        QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);

        auto *preview = findAccessible<QLabel>(&window, QStringLiteral("Изображение из буфера"));
        QVERIFY(preview && preview->isVisible());
        QVERIFY(!preview->pixmap().isNull());
        QVERIFY(backend->uploadedPath.isEmpty());
        QVERIFY(input->toPlainText().isEmpty());
    }

    void pastedChatPictureUploadsWithItsOwnDraft_data()
    {
        QTest::addColumn<bool>("taskChat");
        QTest::newRow("ordinary chat with caption") << false;
        QTest::newRow("task chat without caption") << true;
    }

    void pastedChatPictureUploadsWithItsOwnDraft()
    {
        QFETCH(bool, taskChat);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdUpload = true;
        backend->chats.append({QStringLiteral("c-2"), QStringLiteral("Другой чат")});
        auto secondTask = backend->stages.first().tasks.first();
        secondTask.id = QStringLiteral("502");
        backend->stages.first().tasks.append(secondTask);
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        const auto activeTaskCard = [&]() -> ui::TaskCardDialog * {
            const auto dialogs = window.findChildren<QDialog *>(QStringLiteral("taskCardDialog"));
            for (auto *dialog : dialogs) {
                auto *card = dynamic_cast<ui::TaskCardDialog *>(dialog);
                if (card && card->isVisible())
                    return card;
            }
            return nullptr;
        };
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
        QVERIFY(chats);
        QTRY_COMPARE(chats->count(), 2);

        QWidget *owner = &window;
        if (taskChat) {
            window.openTaskCard(QStringLiteral("501"));
            QTRY_VERIFY(activeTaskCard());
            owner = activeTaskCard();
        }
        const auto inputName = taskChat ? QStringLiteral("Текст в чате карточки") : QStringLiteral("Текст сообщения");
        auto *input = findAccessible<QPlainTextEdit>(owner, inputName);
        QVERIFY(input);
        auto *clipboard = QApplication::clipboard();
        const auto clearClipboard = qScopeGuard([clipboard] { clipboard->clear(); });
        auto *html = new QMimeData;
        html->setHtml(QStringLiteral("<b>Подпись</b>"));
        html->setText(QStringLiteral("Подпись"));
        html->setData(QStringLiteral("application/x-qt-image"), QByteArray("invalid optional image representation"));
        clipboard->setMimeData(html);
        QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(input->toPlainText(), QStringLiteral("Подпись"));
        if (taskChat)
            input->clear();
        const auto caption = input->toPlainText();

        const QColor color("#477f9d");
        QImage image(80, 40, QImage::Format_ARGB32);
        image.fill(color);
        if (taskChat) {
            QByteArray png;
            QBuffer buffer(&png);
            QVERIFY(buffer.open(QIODevice::WriteOnly));
            QVERIFY(image.save(&buffer, "PNG"));
            auto *raw = new QMimeData;
            raw->setData(QStringLiteral("image/png"), png);
            clipboard->setMimeData(raw);
        } else {
            clipboard->setImage(image);
        }
        QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
        auto *preview = findAccessible<QLabel>(owner, QStringLiteral("Изображение из буфера"));
        QVERIFY(preview && preview->isVisible());
        QCOMPARE(backend->imageUploadRequests, 0);

        if (taskChat) {
            auto *card = activeTaskCard();
            QVERIFY(card);
            card->setDiscardConfirmation([] { return true; });
            QVERIFY(card->close());
            window.openTaskCard(QStringLiteral("502"));
            QTRY_VERIFY(activeTaskCard());
            QCOMPARE(activeTaskCard()->taskId(), QStringLiteral("502"));
            auto *otherPreview = findAccessible<QLabel>(activeTaskCard(), QStringLiteral("Изображение из буфера"));
            QVERIFY(otherPreview && !otherPreview->isVisible());
            QVERIFY(activeTaskCard()->close());
            window.openTaskCard(QStringLiteral("501"));
            QTRY_VERIFY(activeTaskCard());
            owner = activeTaskCard();
            input = findAccessible<QPlainTextEdit>(owner, inputName);
            preview = findAccessible<QLabel>(owner, QStringLiteral("Изображение из буфера"));
        } else {
            chats->setCurrentRow(1);
            QVERIFY(!preview->isVisible());
            QVERIFY(input->toPlainText().isEmpty());
            chats->setCurrentRow(0);
        }
        QVERIFY(input && preview && preview->isVisible());
        QCOMPARE(input->toPlainText(), caption);

        const auto capture = qEnvironmentVariable("B24_CHAT_PASTE_SCREENSHOT");
        if (!capture.isEmpty() && !taskChat)
            QVERIFY(input->parentWidget()->grab().save(capture));
        auto *composer = static_cast<ui::ChatComposer *>(input->parentWidget());
        const auto restoredPath = composer->draft().image->path;
        auto *remove = findAccessible<QPushButton>(owner, QStringLiteral("Удалить изображение из черновика"));
        QVERIFY(remove && remove->isVisible());
        remove->click();
        QVERIFY(!preview->isVisible());
        QVERIFY(!QFileInfo::exists(restoredPath));
        QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
        QVERIFY(preview->isVisible());
        const auto replyId = QStringLiteral("999");
        composer->setReply({replyId, QStringLiteral("Анна"), QStringLiteral("Исходное сообщение")});
        auto &typingDelay = testClock(*composer, TypingDelay{});
        typingDelay.start();
        QTest::keyClick(input, Qt::Key_Return);
        QVERIFY(!typingDelay.isActive());
        QCOMPARE(backend->imageUploadRequests, 0);
        QCOMPARE(composer->reply().id, replyId);
        QVERIFY(preview->isVisible());
        composer->setReply({});

        const auto scope = taskChat ? QStringLiteral("501") : QStringLiteral("c-1");
        const auto kind = taskChat ? QStringLiteral("task-chat") : QStringLiteral("chat");
        QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(backend->imageUploadRequests, 1);
        QCOMPARE(backend->imageUploadForTask, taskChat);
        QCOMPARE(backend->uploadedScope, scope);
        QCOMPARE(backend->uploadedCaption, caption);
        const auto uploaded = backend->uploadedPath;
        QVERIFY(QFileInfo::exists(uploaded));
        QVERIFY(uploaded.endsWith(QStringLiteral(".png")));
        const QImage stored(uploaded);
        QCOMPARE(stored.size(), image.size());
        QCOMPARE(stored.pixelColor(20, 20), color);
        QVERIFY(preview->isVisible());

        emit backend->failed((taskChat ? QStringLiteral("upload-task-chat:") : QStringLiteral("upload-chat:")) + scope,
                             QStringLiteral("Ошибка отправки"));
        QVERIFY(preview->isVisible());
        QVERIFY(QFileInfo::exists(uploaded));
        QCOMPARE(input->toPlainText(), caption);
        QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(backend->imageUploadRequests, 2);
        QCOMPARE(backend->uploadedPath, uploaded);
        const auto newerDraft = QStringLiteral("Новый текст");
        input->setPlainText(newerDraft);
        emit backend->fileUploaded(QStringLiteral("other"), kind);
        QVERIFY(preview->isVisible());
        emit backend->fileUploaded(scope, kind);
        QVERIFY(!preview->isVisible());
        QCOMPARE(input->toPlainText(), newerDraft);
        QVERIFY(!QFileInfo::exists(uploaded));

        QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
        QVERIFY(preview->isVisible());
        QVERIFY(remove->isVisible());
        remove->click();
        QVERIFY(!preview->isVisible());
        QCOMPARE(input->toPlainText(), newerDraft);
        QCOMPARE(backend->imageUploadRequests, 2);

        input->clear();
        QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
        QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(backend->imageUploadRequests, 3);
        emit backend->fileUploaded(scope, kind);
        QVERIFY(!preview->isVisible());
        QVERIFY(input->toPlainText().isEmpty());

        const auto waitCaption = QStringLiteral("Подпись уже отправляется");
        input->setPlainText(waitCaption);
        QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
        QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(backend->imageUploadRequests, 4);
        QVERIFY(remove->isVisible());
        remove->click();
        QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(backend->chatSendRequests, 0);
        QCOMPARE(backend->taskSendRequests, 0);
        QCOMPARE(input->toPlainText(), waitCaption);
        emit backend->fileUploaded(scope, kind);
        QVERIFY(input->toPlainText().isEmpty());
        QVERIFY(!QFileInfo::exists(backend->uploadedPath));
    }

    void keepsSelectedTaskChatAtTheSameScrollPositionAfterRead_data()
    {
        QTest::addColumn<bool>("unreadOnly");
        QTest::newRow("all-task-chats") << false;
        QTest::newRow("unread-task-chats") << true;
    }

    void keepsSelectedTaskChatAtTheSameScrollPositionAfterRead()
    {
        QFETCH(bool, unreadOnly);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->accountScope = QStringLiteral("fixture/task-chat-before-read");
        for (int i = 0; i < 30; ++i) {
            Chat chat;
            chat.id = i == 11 ? QStringLiteral("task-dialog") : QStringLiteral("chat%1").arg(100 + i);
            chat.title = QStringLiteral("Чат задачи %1").arg(i);
            chat.taskId = QString::number(200 + i);
            chat.taskChat = true;
            chat.unread = 2;
            backend->taskChats.append(chat);
        }
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("chatScopeTabs"));
        auto *filter = findAccessible<QPushButton>(&window, QStringLiteral("Только непрочитанные чаты"));
        QVERIFY(tabs && filter);
        tabs->setCurrentIndex(1);
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Чаты задач"));
        QVERIFY(chats);
        if (unreadOnly)
            filter->click();
        QTRY_COMPARE(chats->count(), 30);

        const auto rowId = [](const QListWidgetItem *row) {
            return row ? row->data(workspace::Id).toString() : QString{};
        };
        const auto topRow = [chats] { return chats->itemAt(QPoint(4, 0)); };
        const auto anchorId = backend->taskChats[10].id;
        const auto activeId = backend->taskChats[11].id;
        const auto nextId = backend->taskChats[12].id;
        chats->setCurrentRow(11);
        chats->scrollToItem(chats->item(10), QAbstractItemView::PositionAtTop);
        auto *scroll = chats->verticalScrollBar();
        QTRY_COMPARE(rowId(topRow()), anchorId);
        scroll->setValue(scroll->value() + 7);
        QVERIFY(scroll->value() > 0);
        QCOMPARE(rowId(topRow()), anchorId);
        QVERIFY(chats->visualItemRect(topRow()).top() < 0);
        const int selectedOffset = chats->visualItemRect(chats->currentItem()).top();
        QSignalSpy historyReloaded(backend.get(), &Backend::chatMessagesLoaded);

        emit backend->chatReadConfirmed(activeId, {QStringLiteral("701")}, 701);
        backend->taskChats[1].unread = 0;
        backend->taskChats[11].unread = 0;
        emit backend->taskChatsLoaded(backend->taskChats);
        QTRY_COMPARE(rowId(chats->currentItem()), activeId);
        QCOMPARE(chats->currentItem()->data(workspace::Count).toInt(), 0);
        QCOMPARE(chats->currentRow(), 11);
        QVERIFY(chats->viewport()->rect().intersects(chats->visualItemRect(chats->currentItem())));
        QCOMPARE(chats->visualItemRect(chats->currentItem()).top(), selectedOffset);
        QCOMPARE(chats->count(), unreadOnly ? 29 : 30);
        QCOMPARE(historyReloaded.count(), 0);

        backend->taskChats.removeAt(11);
        emit backend->taskChatsLoaded(backend->taskChats);
        QTRY_COMPARE(rowId(chats->currentItem()), activeId);
        QCOMPARE(chats->visualItemRect(chats->currentItem()).top(), selectedOffset);
        QCOMPARE(historyReloaded.count(), 0);

        emit backend->taskChatsLoaded(backend->taskChats);
        QTRY_COMPARE(rowId(chats->currentItem()), activeId);
        QCOMPARE(chats->visualItemRect(chats->currentItem()).top(), selectedOffset);

        const auto lookup = [chats, &rowId](const QString &id) -> QListWidgetItem * {
            for (int row = 0; row < chats->count(); ++row) {
                if (rowId(chats->item(row)) == id)
                    return chats->item(row);
            }
            return nullptr;
        };
        auto *next = lookup(nextId);
        QVERIFY(next);
        chats->setCurrentItem(next);
        QTRY_COMPARE(rowId(chats->currentItem()), nextId);
        QTRY_VERIFY(!lookup(activeId));

        backend->taskChats[11].unread = 0;
        emit backend->taskChatsLoaded(backend->taskChats);
        QCOMPARE(rowId(chats->currentItem()), nextId);
        if (unreadOnly)
            filter->click();
        filter->click();
        QVERIFY(!lookup(nextId));

        const auto otherId = backend->taskChats[12].id;
        auto *other = lookup(otherId);
        QVERIFY(other);
        chats->setCurrentItem(other);
        backend->taskChats[12].unread = 0;
        emit backend->taskChatsLoaded(backend->taskChats);
        QCOMPARE(rowId(chats->currentItem()), otherId);
        backend->accountScope = QStringLiteral("fixture/task-chat-another-account");
        emit backend->accountScopeChanged();
        QVERIFY(!lookup(otherId));
    }

    void keepsTaskChatSelectionThroughChangingRecentSnapshots_data()
    {
        QTest::addColumn<bool>("initiallyRead");
        QTest::newRow("intermediate-unread") << false;
        QTest::newRow("already-read-on-selection") << true;
    }

    void keepsTaskChatSelectionThroughChangingRecentSnapshots()
    {
        QFETCH(bool, initiallyRead);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        const auto activeId = QStringLiteral("task-dialog");
        const int selectedRow = 9;
        for (int i = 0; i < 30; ++i) {
            Chat chat;
            chat.id = i == selectedRow ? activeId : QStringLiteral("chat%1").arg(100 + i);
            chat.title = QStringLiteral("Чат задачи %1").arg(i);
            chat.taskId = QString::number(200 + i);
            chat.taskChat = true;
            chat.unread = i < selectedRow || (i == selectedRow && !initiallyRead) ? 2 : 0;
            backend->taskChats.append(chat);
        }
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("chatScopeTabs"));
        QVERIFY(tabs);
        tabs->setCurrentIndex(1);
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Чаты задач"));
        QVERIFY(chats);
        QTRY_COMPARE(chats->count(), 30);
        const auto rowId = [](const QListWidgetItem *row) {
            return row ? row->data(workspace::Id).toString() : QString{};
        };
        chats->setCurrentRow(selectedRow);
        QCOMPARE(rowId(chats->currentItem()), activeId);
        chats->scrollToItem(chats->item(selectedRow - 1), QAbstractItemView::PositionAtTop);
        const int selectedOffset = chats->visualItemRect(chats->currentItem()).top();

        if (!initiallyRead) {
            emit backend->chatReadConfirmed(activeId, {QStringLiteral("701")}, 701);
            backend->taskChats[selectedRow].unread = 0;
            emit backend->taskChatsLoaded(backend->taskChats);
            QCOMPARE(chats->currentRow(), selectedRow);
        }
        backend->taskChats.move(selectedRow, 28);
        backend->taskChats[28].unread = initiallyRead ? 0 : 2;
        for (int row = 10; row < 15; ++row)
            backend->taskChats[row].unread = 2;
        emit backend->taskChatsLoaded(backend->taskChats);
        QCOMPARE(rowId(chats->currentItem()), activeId);
        QCOMPARE(chats->currentRow(), selectedRow);
        QCOMPARE(chats->visualItemRect(chats->currentItem()).top(), selectedOffset);
        QCOMPARE(chats->currentItem()->data(workspace::Count).toInt(), initiallyRead ? 0 : 2);

        auto *scroll = chats->verticalScrollBar();
        scroll->setValue(scroll->maximum());
        auto *top = chats->itemAt(QPoint(4, 0));
        QVERIFY(top);
        const auto topOffset = chats->visualItemRect(top).top();
        const auto topId = rowId(top);
        QVERIFY(!chats->viewport()->rect().intersects(chats->visualItemRect(chats->currentItem())));
        backend->taskChats[28].unread = 0;
        emit backend->taskChatsLoaded(backend->taskChats);
        QCOMPARE(rowId(chats->currentItem()), activeId);
        QCOMPARE(chats->currentRow(), selectedRow);
        QCOMPARE(rowId(chats->itemAt(QPoint(4, 0))), topId);
        QCOMPARE(chats->visualItemRect(chats->itemAt(QPoint(4, 0))).top(), topOffset);

        emit backend->taskChatsLoaded({backend->taskChats[28]});
        QCOMPARE(rowId(chats->currentItem()), activeId);
        QCOMPARE(chats->currentRow(), 0);
        emit backend->taskChatsLoaded(backend->taskChats);
        QCOMPARE(rowId(chats->currentItem()), activeId);
        QCOMPARE(chats->currentRow(), selectedRow);

        chats->setCurrentRow(selectedRow + 1);
        QTRY_VERIFY(rowId(chats->item(selectedRow)) != activeId);
    }

    void taskChatRowsStayInPlaceUntilMouseRelease_data()
    {
        QTest::addColumn<bool>("ungrab");
        QTest::addColumn<bool>("leaveSection");
        QTest::newRow("release") << false << false;
        QTest::newRow("ungrab") << true << false;
        QTest::newRow("switch-section") << false << true;
    }

    void taskChatRowsStayInPlaceUntilMouseRelease()
    {
        QFETCH(bool, ungrab);
        QFETCH(bool, leaveSection);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        const auto activeId = QStringLiteral("task-dialog");
        const auto targetId = QStringLiteral("chat201");
        for (const auto &id : {activeId, targetId, QStringLiteral("chat202"), QStringLiteral("chat203")}) {
            Chat chat;
            chat.id = id;
            chat.title = id;
            chat.taskChat = true;
            chat.unread = 2;
            backend->taskChats.append(chat);
        }
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("chatScopeTabs"));
        QVERIFY(tabs);
        tabs->setCurrentIndex(1);
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Чаты задач"));
        QVERIFY(chats);
        QTRY_COMPARE(chats->count(), 4);
        const auto idOf = [](const QListWidgetItem *item) {
            return item ? item->data(workspace::Id).toString() : QString{};
        };
        QCOMPARE(idOf(chats->currentItem()), activeId);
        backend->taskChats.removeFirst();
        emit backend->taskChatsLoaded(backend->taskChats);
        QCOMPARE(idOf(chats->item(0)), activeId);
        auto *target = chats->item(1);
        QCOMPARE(idOf(target), targetId);
        const auto point = chats->visualItemRect(target).center();
        QVERIFY(chats->viewport()->rect().contains(point));
        QString clicked;
        connect(chats, &QListWidget::itemClicked, &window, [&](const QListWidgetItem *item) { clicked = idOf(item); });

        QTest::mousePress(chats->viewport(), Qt::LeftButton, {}, point);
        QCOMPARE(idOf(chats->currentItem()), targetId);
        QCoreApplication::processEvents();
        QCOMPARE(idOf(chats->item(0)), activeId);
        QCOMPARE(chats->itemAt(point), target);

        emit backend->chatReadConfirmed(targetId, {QStringLiteral("701")}, 701);
        backend->taskChats[0].unread = 0;
        backend->taskChats[1].pinned = true;
        emit backend->taskChatsLoaded(backend->taskChats);
        backend->taskChats[2].pinned = true;
        emit backend->taskChatsLoaded(backend->taskChats);
        QCOMPARE(idOf(chats->itemAt(point)), targetId);
        QCOMPARE(chats->itemAt(point), target);

        if (ungrab) {
            QEvent cancelled(QEvent::UngrabMouse);
            QCoreApplication::sendEvent(chats->viewport(), &cancelled);
            QTRY_COMPARE(chats->count(), 3);
        }
        if (leaveSection) {
            auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
            QVERIFY(sections);
            backend->holdTaskChatDirectory = true;
            sections->setCurrentRow(1);
            QVERIFY(!chats->isVisible());
            QTest::mouseRelease(sections->viewport(), Qt::LeftButton, {}, sections->visualItemRect(sections->currentItem()).center());
            QCoreApplication::processEvents();
            QCOMPARE(chats->count(), 4);
            sections->setCurrentRow(0);
        } else {
            QTest::mouseRelease(chats->viewport(), Qt::LeftButton, {}, point);
        }
        QCOMPARE(clicked, ungrab || leaveSection ? QString{} : targetId);
        QTRY_COMPARE(chats->count(), 3);
        QCOMPARE(idOf(chats->currentItem()), targetId);
        QCOMPARE(idOf(chats->item(1)), QStringLiteral("chat203"));
        QCOMPARE(chats->currentItem()->data(workspace::Count).toInt(), 0);
        QCOMPARE(backend->chatRefreshCalls.last(), targetId);
    }

    void chatUnreadFilterWorksTogetherWithSearch()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        Chat unreadChat;
        unreadChat.id = QStringLiteral("c-2");
        unreadChat.title = QStringLiteral("Непрочитанный чат");
        unreadChat.unread = 3;
        backend->chats = {backend->chats.first(), unreadChat};
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
        QVERIFY(chats);
        QTRY_COMPARE(chats->count(), 2);

        auto *button = findAccessible<QPushButton>(&window, QStringLiteral("Только непрочитанные чаты"));
        QVERIFY(button);
        QVERIFY(button->isCheckable());
        button->click();
        QTRY_COMPARE(chats->count(), 1);
        QCOMPARE(chats->item(0)->data(workspace::Id).toString(), QStringLiteral("c-2"));
        auto *search = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск чатов"));
        QVERIFY(search);
        search->setText(QStringLiteral("чат"));
        QTest::keyClick(search, Qt::Key_Return);
        emit backend->chatsFound(QStringLiteral("чат"), backend->chats);
        QCOMPARE(chats->count(), 1);
        QCOMPARE(chats->item(0)->data(workspace::Id).toString(), unreadChat.id);

        button->click();
        QTRY_COMPARE(chats->count(), 2);
    }
};

QTEST_MAIN(UiChatTest)
#include "tst_ui_chat.moc"
