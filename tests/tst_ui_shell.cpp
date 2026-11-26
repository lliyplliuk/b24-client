#include "pilot_fixture.h"

using namespace bx;
using namespace bx::test;

class UiShellTest : public PilotFixture {
    Q_OBJECT

private slots:
    void showsUnreadAndWorkdayWithoutOpeningTheirSections()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->chats.first().unread = 3;
        backend->chats.first().serverChatId = 67;
        auto duplicate = backend->chats.first();
        duplicate.id = QStringLiteral("chat67");
        Chat other;
        other.id = QStringLiteral("chat68");
        other.serverChatId = 68;
        other.unread = 4;
        backend->taskChats = {duplicate, other};
        Notification unread;
        unread.id = QStringLiteral("notice-1");
        auto read = unread;
        read.id = QStringLiteral("notice-2");
        read.read = true;
        backend->notifications = {unread, read};
        backend->workdayStatus.state = WorkdayState::Running;
        backend->workdayStatus.elapsedSeconds = 71 * 60 + 59;

        MainWindow window(backend.get());
        window.show();
        QApplication::setActiveWindow(&window);
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        const auto detail = [sections](int row) {
            return sections->item(row)->data(Qt::AccessibleDescriptionRole).toString();
        };
        QCOMPARE(sections->currentRow(), 0);
        QTRY_COMPARE(backend->workdayRequests, 1);
        QCOMPARE(backend->noticeHistoryRequests, 1);
        QVERIFY(detail(0).contains(QStringLiteral("3")));
        emit backend->taskChatsLoaded(backend->taskChats);
        QVERIFY(detail(0).contains(QStringLiteral("7")));
        QVERIFY(detail(2).contains(QStringLiteral("1")));
        QTRY_VERIFY(detail(3).contains(QStringLiteral("01:11")));
        const auto screenshot = qEnvironmentVariable("B24_RAIL_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(sections->grab().save(screenshot));
        QTRY_VERIFY(detail(3).contains(QStringLiteral("01:12")));
        QCOMPARE(backend->workdayRequests, 1);
        sections->setCurrentRow(3);
        QCOMPARE(backend->workdayRequests, 1);
        backend->railUnread = 21;
        emit backend->unreadChatCountChanged(21);
        emit backend->recentChatsLoaded(backend->chats);
        QVERIFY(detail(0).contains(QStringLiteral("7")));
        emit backend->notificationReadStateChanged(QStringList{unread.id}, true);
        QVERIFY(detail(2).isEmpty());
        sections->setCurrentRow(2);
        QCOMPARE(backend->noticeHistoryRequests, 1);

        window.onConnectionChanged(false);
        QVERIFY(detail(3).isEmpty());
        backend->holdWorkday = true;
        window.onConnectionChanged(true);
        QCOMPARE(backend->workdayRequests, 2);
        QVERIFY(detail(3).isEmpty());
        window.onConnectionChanged(false);
        backend->holdWorkday = false;
        backend->workdayStatus.state = WorkdayState::Paused;
        backend->workdayStatus.elapsedSeconds = 89 * 60;
        window.onConnectionChanged(true);
        QCOMPARE(backend->workdayRequests, 3);
        QVERIFY(detail(3).contains(QStringLiteral("01:29")));
        auto *clock = window.findChild<QTimer *>(QStringLiteral("railWorkdayTimer"));
        QVERIFY(clock && !clock->isActive());
    }

    void requiresAnExplicitStatusAfterPortalDnd()
    {
        class StatusFixture final : public FixtureBackend {
        public:
            void requestPortalStatus() override { emit portalStatusLoaded(QStringLiteral("dnd")); }
            void updatePortalStatus(const QString &expected, const QString &status) override
            {
                previous = expected;
                requested = status;
                emit portalStatusLoaded(status);
                emit portalStatusUpdated(status);
            }
            QString previous;
            QString requested;
        } backend;
        backend.demoMode = false;
        QWidget panel;
        QVBoxLayout layout(&panel);
        ui::addPortalDndControl(&layout, &backend, [] { return true; });
        auto *status = findAccessible<QComboBox>(&panel, QStringLiteral("Новый статус на портале"));
        auto *apply = findAccessible<QPushButton>(&panel, QStringLiteral("Установить статус на портале"));
        QVERIFY(status && apply);
        QVERIFY(status->currentData().toString().isEmpty());
        QVERIFY(!apply->isEnabled());
        status->setCurrentIndex(status->findData(QStringLiteral("away")));
        QVERIFY(apply->isEnabled());
        apply->click();
        QCOMPARE(backend.previous, QStringLiteral("dnd"));
        QCOMPARE(backend.requested, QStringLiteral("away"));
        QVERIFY(!apply->isEnabled());
    }

    void scopesTimeoutRecoveryToTheAffectedOperation_data()
    {
        QTest::addColumn<QString>("kind");
        for (const auto *kind : {"push", "history", "send-chat", "task-history", "send-task", "task-update"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }

    void scopesTimeoutRecoveryToTheAffectedOperation()
    {
        QFETCH(QString, kind);
        const auto timeout = QStringLiteral("Ошибка сети или истекло время ожидания");
        const auto initial = QStringLiteral("Портал вернул неизвестный ответ");
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdMessageSends = true;
        backend->holdTaskUpdate = true;
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *status = findAccessible<QLabel>(&window, QStringLiteral("Состояние"));
        auto *messages = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *input = findAccessible<QPlainTextEdit>(&window, QStringLiteral("Текст сообщения"));
        QVERIFY(status && messages && input);
        QTRY_VERIFY(messages->count() > 0);
        emit backend->failed(QStringLiteral("unclassified"), initial);
        QCOMPARE(status->text(), initial);
        if (kind == QLatin1String("push")) {
            for (const auto *operation : {"push", "pins:c-1", "chat-history-identity", "task-chat-titles"}) {
                emit backend->failed(QString::fromLatin1(operation), timeout);
                QCOMPARE(status->text(), initial);
            }
            emit backend->failed(QStringLiteral("chat:previous-dialog"), timeout);
            QCOMPARE(status->text(), initial);
            const auto access = QStringLiteral("Действие требует новой веб-сессии портала");
            emit backend->failed(QStringLiteral("push"), access);
            QCOMPARE(status->text(), access);
            const auto diagnostic = QStringLiteral("Портал не предоставил допустимый защищённый канал уведомлений");
            emit backend->failed(QStringLiteral("push"), diagnostic);
            QCOMPARE(status->text(), diagnostic);
            emit backend->failed(QStringLiteral("unclassified"), timeout);
            QCOMPARE(status->text(), timeout);
            return;
        }
        if (kind == QLatin1String("history")) {
            const auto count = messages->count();
            emit backend->failed(QStringLiteral("chat:c-1"), timeout);
            QCOMPARE(messages->count(), count);
            QVERIFY(status->text().contains(QStringLiteral("сообщения")));
            QVERIFY(status->text().contains(QStringLiteral("«Обновить»")));
            return;
        }
        if (kind == QLatin1String("send-chat")) {
            input->setPlainText(QStringLiteral("Черновик сообщения"));
            QTest::keyClick(input, Qt::Key_Return);
            QCOMPARE(backend->chatSendRequests, 1);
            emit backend->failed(QStringLiteral("send:chat:c-1"),
                                 timeout + QStringLiteral(". Проверьте историю перед повторной отправкой"));
            QCOMPARE(input->toPlainText(), QStringLiteral("Черновик сообщения"));
            QVERIFY(status->text().contains(QStringLiteral("подтвердить отправку")));
            QVERIFY(status->text().contains(QStringLiteral("историю перед повторной отправкой")));
            QCOMPARE(backend->chatSendRequests, 1);
            return;
        }

        window.openTaskCard(QStringLiteral("501"));
        auto *card = dynamic_cast<ui::TaskCardDialog *>(
            window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        QVERIFY(card);
        auto *cardStatus = findAccessible<QLabel>(card, QStringLiteral("Состояние карточки"));
        QVERIFY(cardStatus);
        emit backend->failed(QStringLiteral("unclassified"), initial);
        if (kind == QLatin1String("task-history")) {
            emit backend->taskMessagesLoaded(QStringLiteral("501"), backend->messages);
            const auto count = card->messageList()->count();
            QVERIFY(count > 0);
            emit backend->failed(QStringLiteral("task-messages:501"), timeout);
            QCOMPARE(card->messageList()->count(), count);
            QVERIFY(cardStatus->text().contains(QStringLiteral("чат задачи")));
            QVERIFY(cardStatus->text().contains(QStringLiteral("карточку заново")));
        } else if (kind == QLatin1String("send-task")) {
            card->messageInput()->setPlainText(QStringLiteral("Ответ по задаче"));
            QTest::keyClick(card->messageInput(), Qt::Key_Return);
            QCOMPARE(backend->taskSendRequests, 1);
            emit backend->failed(QStringLiteral("send:task:501"),
                                 timeout + QStringLiteral(". Проверьте историю перед повторной отправкой"));
            QCOMPARE(card->messageInput()->toPlainText(), QStringLiteral("Ответ по задаче"));
            QVERIFY(cardStatus->text().contains(QStringLiteral("подтвердить отправку")));
            QVERIFY(cardStatus->text().contains(QStringLiteral("историю перед повторной отправкой")));
            QCOMPARE(backend->taskSendRequests, 1);
        } else {
            auto *edit = findAccessible<QPushButton>(card, QStringLiteral("Изменить задачу"));
            QVERIFY(edit && edit->isEnabled());
            edit->click();
            auto *title = findAccessible<QLineEdit>(card, QStringLiteral("Заголовок задачи"));
            auto *save = findAccessible<QPushButton>(card, QStringLiteral("Сохранить задачу"));
            QVERIFY(title && save);
            title->setText(QStringLiteral("Изменённая задача"));
            save->click();
            QCOMPARE(backend->taskUpdateRequests, 1);
            emit backend->failed(QStringLiteral("update-task:501"),
                                 timeout + QStringLiteral(". Проверьте сохранённые изменения перед повтором"));
            QCOMPARE(title->text(), QStringLiteral("Изменённая задача"));
            QVERIFY(save->isEnabled());
            QVERIFY(cardStatus->text().contains(QStringLiteral("подтвердить сохранение")));
            QVERIFY(cardStatus->text().contains(QStringLiteral("перед повтором")));
            QCOMPARE(backend->taskUpdateRequests, 1);
        }
        QCOMPARE(status->text(), initial);
    }

    void accountChangeClosesChatAndCardAndClearsDrafts()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->accountScope = QStringLiteral("fixture/drafts-first");
        auto other = backend->chats.first();
        other.id = QStringLiteral("chat42");
        backend->chats.append(other);
        MainWindow window(backend.get());
        window.show();
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *input = findAccessible<QPlainTextEdit>(&window, QStringLiteral("Текст сообщения"));
        QVERIFY(chats && history && input);
        QTRY_COMPARE(chats->count(), 2);
        chats->setCurrentRow(0);
        input->setPlainText(QStringLiteral("Сохранённый черновик A"));
        chats->setCurrentRow(1);
        input->setPlainText(QStringLiteral("Открытый черновик A"));
        auto *search = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск по сообщениям"));
        QVERIFY(search);
        search->setText(QStringLiteral("Запрос A"));
        emit search->returnPressed();
        QCOMPARE(backend->searchedQuery, search->text());
        const auto taskId = backend->stages.first().tasks.first().id;
        window.openTaskCard(taskId);
        QPointer<QDialog> card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card && card->isVisible());
        auto *taskInput = findAccessible<QPlainTextEdit>(card, QStringLiteral("Текст в чате карточки"));
        QVERIFY(taskInput);
        taskInput->setPlainText(QStringLiteral("Комментарий A"));

        emit backend->accountScopeChanged();
        QCOMPARE(input->toPlainText(), QStringLiteral("Открытый черновик A"));
        QVERIFY(card && card->isVisible());
        backend->accountScope = QStringLiteral("fixture/drafts-second");
        emit backend->accountScopeChanged();
        QVERIFY(input->toPlainText().isEmpty());
        QVERIFY(!input->isVisible());
        QCOMPARE(history->count(), 0);
        QVERIFY(!chats->currentItem());
        QTRY_VERIFY(card.isNull());

        emit backend->recentChatsLoaded(backend->chats);
        for (int row = 0; row < 2; ++row) {
            chats->setCurrentRow(row);
            QVERIFY(input->isVisible());
            QVERIFY(input->toPlainText().isEmpty());
        }
        search->setText(QStringLiteral("Запрос B"));
        emit search->returnPressed();
        QCOMPARE(backend->searchedQuery, search->text());
        window.openTaskCard(taskId);
        card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card && card->isVisible());
        taskInput = findAccessible<QPlainTextEdit>(card, QStringLiteral("Текст в чате карточки"));
        QVERIFY(taskInput && taskInput->toPlainText().isEmpty());
    }

    void directoryWidthSurvivesRestartAtManualWidth_data()
    {
        QTest::addColumn<QString>("key");
        QTest::addColumn<QString>("splitterName");
        QTest::addColumn<int>("section");
        QTest::addColumn<int>("bodyMinimum");
        QTest::newRow("chats") << QStringLiteral("ui/chatDirectoryWidth")
            << QStringLiteral("chatDirectorySplitter") << 0 << 325;
        QTest::newRow("projects") << QStringLiteral("ui/projectDirectoryWidth")
            << QStringLiteral("projectDirectorySplitter") << 1 << 430;
    }

    void directoryWidthSurvivesRestartAtManualWidth()
    {
        QFETCH(QString, key);
        QFETCH(QString, splitterName);
        QFETCH(int, section);
        QFETCH(int, bodyMinimum);
        QSettings settings(QStringLiteral("bitrix24-client"), QStringLiteral("bitrix24-client"));
        settings.remove(key);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        int savedWidth = 0;
        {
            MainWindow window(backend.get());
            window.resize(1750, 700);
            window.show();
            auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
            auto *splitter = window.findChild<QSplitter *>(splitterName);
            QVERIFY(sections && splitter);
            sections->setCurrentRow(section);
            QTRY_VERIFY(splitter->isVisible());
            auto *handle = splitter->handle(1);
            QVERIFY(handle);
            const auto start = handle->rect().center();
            QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, start);
            QTest::mouseMove(handle, start + QPoint(880, 0));
            QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier, start + QPoint(880, 0));
            QTRY_VERIFY(splitter->widget(0)->width() > 1000);
            savedWidth = splitter->widget(0)->width();
            QCOMPARE(settings.value(key).toInt(), savedWidth);
        }
        {
            MainWindow restored(backend.get());
            restored.resize(1750, 700);
            restored.show();
            auto *sections = findAccessible<QListWidget>(&restored, QStringLiteral("Разделы"));
            auto *splitter = restored.findChild<QSplitter *>(splitterName);
            QVERIFY(sections && splitter);
            sections->setCurrentRow(section);
            QTRY_COMPARE(splitter->widget(0)->width(), savedWidth);

            restored.resize(900, 600);
            QTRY_VERIFY(splitter->widget(0)->width() < savedWidth);
            QTRY_VERIFY(splitter->widget(1)->width() >= bodyMinimum);
            QCOMPARE(settings.value(key).toInt(), savedWidth);

            sections->setCurrentRow(1 - section);
            restored.resize(1750, 700);
            sections->setCurrentRow(section);
            QTRY_COMPARE(splitter->widget(0)->width(), savedWidth);
        }
        settings.remove(key);
    }

    void periodicUpdateStaysLocalAcrossPortalSnapshots()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdNotifications = true;
        backend->accountScope = QStringLiteral("fixture/first-account");
        ClientUpdateFeed feed;
        MainWindow window(backend.get());
        window.setClientUpdateFeed(&feed);
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        auto *notices = findAccessible<QListWidget>(&window, QStringLiteral("Уведомления"));
        auto *search = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск уведомлений"));
        auto *read = window.findChild<QAction *>(QStringLiteral("readNotice"));
        QVERIFY(sections && notices && search && read);
        const QUrl url(QStringLiteral("https://github.com/lliyplliuk/b24-client/releases/download/v1.2.3/bitrix24-client-v1.2.3-windows-x64.zip"));
        feed.publish(QStringLiteral("v1.2.3"), url);
        QVERIFY(window.isHidden());
        QCOMPARE(sections->currentRow(), 0);
        QVERIFY(sections->item(2)->data(Qt::AccessibleDescriptionRole).toString().contains(QStringLiteral("1")));
        window.show();
        sections->setCurrentRow(2);
        emit backend->notificationsLoaded({});
        QCOMPARE(notices->count(), 1);
        QVERIFY(notices->item(0)->data(workspace::MarkupHtml).toString().contains(url.toString()));
        search->setText(QStringLiteral("1.2.3"));
        QTest::keyClick(search, Qt::Key_Return);
        emit backend->notificationsFound(search->text(), {});
        QCOMPARE(notices->count(), 1);
        notices->setCurrentRow(0);
        read->trigger();
        QVERIFY(feed.notice().read);
        QVERIFY(backend->noticeReadId.isEmpty());
        QVERIFY(backend->noticeReadBatches.isEmpty());
        feed.publish(QStringLiteral("v1.2.3"), url);
        QVERIFY(feed.notice().read);
        search->setText(QStringLiteral("другое"));
        QTest::keyClick(search, Qt::Key_Return);
        emit backend->notificationsFound(search->text(), {});
        QCOMPARE(notices->count(), 0);
        backend->accountScope = QStringLiteral("fixture/another-account");
        emit backend->accountScopeChanged();
        QCOMPARE(notices->count(), 1);
        QVERIFY(!notices->item(0)->data(workspace::Count).toBool());
        QVERIFY(sections->item(2)->data(Qt::AccessibleDescriptionRole).toString().isEmpty());
        QApplication::setActiveWindow(&window);
        window.onConnectionChanged(true);
        feed.publish(QStringLiteral("v1.2.4"), QUrl(url.toString().replace(QStringLiteral("1.2.3"), QStringLiteral("1.2.4"))));
        QTRY_VERIFY(feed.notice().read);
        QCOMPARE(notices->count(), 1);
        QVERIFY(backend->noticeReadBatches.isEmpty());
    }

    void previewWindowHideDoesNotCommitAnEditor()
    {
        QWidget window;
        QListWidget list(&window);
        auto *delegate = workspace::configureRows(&list, workspace::Row::Message);
        QSignalSpy commits(delegate, &QAbstractItemDelegate::commitData);
        QSignalSpy closes(delegate, &QAbstractItemDelegate::closeEditor);
        window.show();
        delegate->setPreviewAnimation(true);
        window.hide();
        QCOMPARE(commits.count(), 0);
        QCOMPARE(closes.count(), 0);
    }

    void offersToOpenSavedWorktimeSheetAndEscapeDismisses()
    {
        class WorktimeBackend final : public FixtureBackend {
        public:
            void requestWorktimeSheet(const QDate &month) override
            {
                emit worktimeSheetLoaded({month, {}, true, {}});
            }
            void exportWorktimeSheet(const QDate &month, const QString &path) override
            {
                exportedMonth = month;
                exportedPath = path;
            }
            QDate exportedMonth;
            QString exportedPath;
        } backend;
        LocalFileOpener opener;
        QTemporaryDir destination;
        QVERIFY(destination.isValid());
        const auto path = destination.filePath(QStringLiteral("Табель.xls"));
        ui::WorktimePanel panel(&backend);
        panel.show();
        panel.refresh();
        auto *save = findAccessible<QPushButton>(&panel, QStringLiteral("Скачать табель Excel"));
        QVERIFY(save && save->isEnabled());
        QTimer::singleShot(0, &panel, [&] {
            if (auto *dialog = panel.findChild<QFileDialog *>()) {
                dialog->selectFile(path);
                QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
            }
        });
        save->click();
        QCOMPARE(backend.exportedPath, path);
        const auto offerName = QStringLiteral("savedFileOffer");
        emit backend.worktimeSheetExported(backend.exportedMonth.addMonths(1), path);
        QVERIFY(!panel.findChild<QMessageBox *>(offerName));
        QFile saved(path);
        QVERIFY(saved.open(QIODevice::WriteOnly));
        QCOMPARE(saved.write("fixture"), 7);
        saved.close();
        emit backend.worktimeSheetExported(backend.exportedMonth, path);
        QPointer<QMessageBox> offer = panel.findChild<QMessageBox *>(offerName);
        QVERIFY(offer && offer->isVisible());
        QVERIFY(opener.opened.isEmpty());
        QTest::keyClick(offer, Qt::Key_Escape);
        QTRY_VERIFY(!offer || !offer->isVisible());
        QVERIFY(opener.opened.isEmpty());
    }

    void samePortalTaskLinksOpenNativeCard()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto &message = backend->messages.first();
        message.system = true;
        message.text = QStringLiteral("Создана [URL=/workgroups/group/101/tasks/task/view/501/]задача[/URL]");
        MainWindow window(backend.get());
        window.resize(930, 630);
        window.show();
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        QTRY_COMPARE(history->count(), 1);
        const auto *item = history->item(0);
        QVERIFY(item->data(workspace::SystemMessage).toBool());
        QVERIFY(item->data(Qt::AccessibleTextRole).toString().startsWith(QStringLiteral("Системное сообщение")));
        QCoreApplication::processEvents();
        const auto row = history->visualItemRect(item);
        const auto index = history->model()->index(0, 0);
        const auto *painter = static_cast<workspace::RowPainter *>(history->itemDelegate());
        const auto shot = history->viewport()->grab().toImage();
        QPoint target;
        bool foundSystemColor = false;
        const auto visible = row.intersected(shot.rect());
        for (int y = visible.top(); y <= visible.bottom(); ++y) {
            for (int x = visible.left(); x <= visible.right(); ++x) {
                if (shot.pixelColor(x, y) == QColor("#e2ebe6"))
                    foundSystemColor = true;
                if (target.isNull() && !painter->anchorAt(index, row, QPoint(x, y)).isEmpty())
                    target = QPoint(x, y);
            }
        }
        QVERIFY(foundSystemColor);
        QVERIFY(!target.isNull());
        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, target);
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        QVERIFY(card->isVisible());
        auto *copyLink = findAccessible<QPushButton>(card, QStringLiteral("Скопировать ссылку"));
        QVERIFY(copyLink && copyLink->isEnabled());
        card->close();
        QTRY_VERIFY(!window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        window.openMessageLink(QStringLiteral("/extranet/company/personal/user/19/tasks/task/view/501/"));
        card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card && card->isVisible());
    }

    void projectMessageLinksOpenNativeProject()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        Project linked;
        linked.id = QStringLiteral("5");
        linked.name = QStringLiteral("Проект по ссылке");
        backend->moreProjects = {linked};
        LocalFileOpener browser;
        QDesktopServices::setUrlHandler(QStringLiteral("https"), &browser, "open");
        QDesktopServices::setUrlHandler(QStringLiteral("http"), &browser, "open");
        QDesktopServices::setUrlHandler(QStringLiteral("javascript"), &browser, "open");
        const auto restore = qScopeGuard([] {
            QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
            QDesktopServices::unsetUrlHandler(QStringLiteral("http"));
            QDesktopServices::unsetUrlHandler(QStringLiteral("javascript"));
        });
        MainWindow window(backend.get());
        window.show();
        auto *projects = findAccessible<QListWidget>(&window, QStringLiteral("Проекты"));
        QVERIFY(projects);
        const auto external = QUrl(QStringLiteral("http://example.bitrix24.ru/workgroups/group/5/"));
        window.openMessageLink(external.toString());
        const auto zeroPort = QUrl(QStringLiteral("https://example.bitrix24.ru:0/workgroups/group/5/"));
        window.openMessageLink(zeroPort.toString());
        const QList<QUrl> externalLinks{external, zeroPort};
        QCOMPARE(browser.opened, externalLinks);
        window.openMessageLink(QStringLiteral("javascript:alert(1)"));
        window.openMessageLink(QStringLiteral("/workgroups/group/5/"));
        QCOMPARE(browser.opened, externalLinks);
        QVERIFY(projects->isVisible() && projects->currentItem());
        QCOMPARE(backend->projectMoreRequests, 1);
        QCOMPARE(projects->currentItem()->data(workspace::Id).toString(), linked.id);

        window.openMessageLink(QStringLiteral("/workgroups/group/101/"));
        QCOMPARE(projects->currentItem()->data(workspace::Id).toString(), QStringLiteral("101"));
        window.openMessageLink(QStringLiteral("/workgroups/group/5/"));
        QCOMPARE(projects->currentItem()->data(workspace::Id).toString(), linked.id);
    }

    void missingProjectMessageLinkFallsBackToOriginalUrl_data()
    {
        QTest::addColumn<bool>("pageError");
        QTest::newRow("missing") << false;
        QTest::newRow("page-error") << true;
    }

    void projectMessageLinkUsesCurrentAccountDirectory()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->accountScope = QStringLiteral("fixture/project-links-first");
        LocalFileOpener browser;
        QDesktopServices::setUrlHandler(QStringLiteral("https"), &browser, "open");
        const auto restore = qScopeGuard([] { QDesktopServices::unsetUrlHandler(QStringLiteral("https")); });
        MainWindow window(backend.get());
        window.show();
        backend->requestProjects();

        backend->accountScope = QStringLiteral("fixture/project-links-second");
        Project project;
        project.id = QStringLiteral("72");
        project.name = QStringLiteral("Проект второго аккаунта");
        backend->projects = {project};
        emit backend->accountScopeChanged();
        window.openMessageLink(QStringLiteral("/workgroups/group/72/"));
        QVERIFY(browser.opened.isEmpty());
        auto *projects = findAccessible<QListWidget>(&window, QStringLiteral("Проекты"));
        QVERIFY(projects && projects->isVisible() && projects->currentItem());
        QCOMPARE(projects->currentItem()->data(workspace::Id).toString(), project.id);
    }

    void missingProjectMessageLinkFallsBackToOriginalUrl()
    {
        QFETCH(bool, pageError);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        Project later;
        later.id = QStringLiteral("5");
        later.name = QStringLiteral("Дальняя страница");
        backend->moreProjects = {later};
        backend->holdProjectMore = true;
        LocalFileOpener browser;
        QDesktopServices::setUrlHandler(QStringLiteral("https"), &browser, "open");
        const auto restore = qScopeGuard([] { QDesktopServices::unsetUrlHandler(QStringLiteral("https")); });
        MainWindow window(backend.get());
        window.show();
        const auto href = QStringLiteral("/extranet/workgroups/group/5/?view=details#activity");
        const auto url = backend->portalOrigin().resolved(QUrl(href));
        window.openMessageLink(href);
        QVERIFY(browser.opened.isEmpty());
        QCOMPARE(backend->projectMoreRequests, 1);
        if (pageError) {
            emit backend->failed(QStringLiteral("projects"), QStringLiteral("Страница недоступна"));
        } else {
            emit backend->projectsMoreAvailable(false);
            emit backend->projectsLoaded(backend->projects);
        }
        QCOMPARE(browser.opened, QList<QUrl>{url});
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        QCOMPARE(sections->currentRow(), 0);
        emit backend->projectsMoreAvailable(false);
        emit backend->projectsLoaded(backend->projects + backend->moreProjects);
        QCOMPARE(browser.opened, QList<QUrl>{url});
        QCOMPARE(sections->currentRow(), 0);
    }

    void samePortalChatMessageLinksOpenNativeContext()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        const auto taskDialog = QStringLiteral("chat154729");
        const auto linkedMessage = QStringLiteral("5381055");
        const auto address = QStringLiteral("https://example.bitrix24.ru/online/?IM_TASK=chat154729&IM_MESSAGE=5381055");
        backend->messages.first().text = address;
        backend->stages.first().tasks.first().chatDialogId = taskDialog;
        LocalFileOpener intercepted;
        QDesktopServices::setUrlHandler(QStringLiteral("https"), &intercepted, "open");
        const auto restoreBrowser = qScopeGuard([] { QDesktopServices::unsetUrlHandler(QStringLiteral("https")); });
        MainWindow window(backend.get());
        window.resize(960, 630);
        window.show();
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("chatScopeTabs"));
        QVERIFY(history && tabs);
        QTRY_COMPARE(history->count(), 1);
        const auto row = history->visualItemRect(history->item(0));
        const auto index = history->model()->index(0, 0);
        const auto *painter = static_cast<const workspace::RowPainter *>(history->itemDelegate());
        QPoint anchor;
        for (int y = row.top(); y <= row.bottom() && anchor.isNull(); ++y) {
            for (int x = row.left(); x <= row.right(); ++x) {
                const auto link = painter->anchorAt(index, row, QPoint(x, y));
                if (link.contains(QStringLiteral("IM_MESSAGE=5381055"))) {
                    anchor = QPoint(x, y);
                    break;
                }
            }
        }
        QVERIFY(!anchor.isNull());
        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, anchor);
        QVERIFY(intercepted.opened.isEmpty());
        QCOMPARE(tabs->currentIndex(), 1);
        QCOMPARE(backend->contextDialog, taskDialog);
        QCOMPARE(backend->contextMessage, linkedMessage);
        Message selected;
        selected.id = linkedMessage;
        selected.text = QStringLiteral("Нужное сообщение");
        emit backend->messageContextLoaded(taskDialog, selected.id, {selected});
        QVERIFY(history->currentItem());
        QCOMPARE(history->currentItem()->data(workspace::Id).toString(), selected.id);

        window.openTaskCard(QStringLiteral("501"));
        auto *card = static_cast<ui::TaskCardDialog *>(window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        QVERIFY(card && card->isVisible());
        QCOMPARE(card->taskChatDialogId(), taskDialog);
        card->messageInput()->setPlainText(QStringLiteral("Черновик задачи"));
        int closeAttempts = 0;
        card->setDiscardConfirmation([&closeAttempts] { ++closeAttempts; return false; });
        window.openMessageLink(QStringLiteral("/online/?IM_TASK=chat154729&IM_MESSAGE=5381056"), card->messageList());
        QCOMPARE(closeAttempts, 0);
        QVERIFY(card->isVisible());
        QCOMPARE(backend->contextDialog, taskDialog);
        QCOMPARE(backend->contextMessage, QStringLiteral("5381056"));
        selected.id = backend->contextMessage;
        emit backend->messageContextLoaded(taskDialog, selected.id, {selected});
        QVERIFY(card->messageList()->currentItem());
        QCOMPARE(card->messageList()->currentItem()->data(workspace::Id).toString(), selected.id);

        const auto other = QStringLiteral("/online/?IM_DIALOG=chat42&IM_MESSAGE=555");
        window.openMessageLink(other, card->messageList());
        QCOMPARE(closeAttempts, 1);
        QVERIFY(card->isVisible());
        QCOMPARE(card->messageInput()->toPlainText(), QStringLiteral("Черновик задачи"));
        QCOMPARE(backend->contextDialog, taskDialog);
        QVERIFY(intercepted.opened.isEmpty());
        card->setDiscardConfirmation([] { return true; });
        window.openMessageLink(other, card->messageList());
        QTRY_VERIFY(!window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        QCOMPARE(tabs->currentIndex(), 0);
        QCOMPARE(backend->contextDialog, QStringLiteral("chat42"));
        QCOMPARE(backend->contextMessage, QStringLiteral("555"));

        const auto external = QUrl(QStringLiteral("https://example.test/online/?IM_TASK=chat154729&IM_MESSAGE=5381055"));
        window.openMessageLink(external.toString());
        QCOMPARE(intercepted.opened, QList<QUrl>{external});
    }

    void mentionInHistoryOpensNativeProfile()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->messages.first().text = QStringLiteral("[USER=19 REPLACE]Анна[/USER] проверила задачу");
        MainWindow window(backend.get());
        window.resize(930, 630);
        window.show();
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        QTRY_COMPARE(history->count(), 1);
        QCoreApplication::processEvents();
        const auto row = history->visualItemRect(history->item(0));
        const auto index = history->model()->index(0, 0);
        const auto *painter = static_cast<workspace::RowPainter *>(history->itemDelegate());
        QPoint target;
        for (int y = row.top(); y <= row.bottom() && target.isNull(); ++y) {
            for (int x = row.left(); x <= row.right(); ++x) {
                if (painter->anchorAt(index, row, QPoint(x, y)) == QLatin1String("/company/personal/user/19/")) {
                    target = QPoint(x, y);
                    break;
                }
            }
        }
        QVERIFY(!target.isNull());
        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, target);
        QCOMPARE(backend->requestedPerson, QStringLiteral("19"));
        auto *profile = window.findChild<QDialog *>(QStringLiteral("personProfileDialog"));
        QVERIFY(profile && profile->isVisible());
    }

    void employeeProfileClosesAboveTaskCard()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        Message message;
        message.id = QStringLiteral("502");
        message.text = QStringLiteral("[USER=19 REPLACE]Анна[/USER] проверила задачу");
        backend->taskMessages = {message};
        MainWindow window(backend.get());
        window.resize(970, 650);
        window.show();
        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card && card->isVisible());
        QCOMPARE(card->windowModality(), Qt::WindowModal);
        auto *history = findAccessible<QListWidget>(card, QStringLiteral("Чат карточки"));
        QVERIFY(history);
        QTRY_COMPARE(history->count(), 1);
        const auto openProfile = [&]() -> bool {
            const auto row = history->visualItemRect(history->item(0));
            const auto index = history->model()->index(0, 0);
            const auto *painter = static_cast<workspace::RowPainter *>(history->itemDelegate());
            for (int y = row.top(); y <= row.bottom(); ++y) {
                for (int x = row.left(); x <= row.right(); ++x) {
                    const QPoint link(x, y);
                    if (painter->anchorAt(index, row, link) == QLatin1String("/company/personal/user/19/")) {
                        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, link);
                        return true;
                    }
                }
            }
            return false;
        };
        QVERIFY(openProfile());
        QPointer<QDialog> profile = window.findChild<QDialog *>(QStringLiteral("personProfileDialog"));
        QVERIFY(profile && profile->isVisible() && profile->windowHandle());
        QCOMPARE(backend->requestedPerson, QStringLiteral("19"));
        auto *buttons = profile->findChild<QDialogButtonBox *>();
        QVERIFY(buttons && buttons->button(QDialogButtonBox::Close));
        auto *closeButton = buttons->button(QDialogButtonBox::Close);
        // The QWindow route applies Qt modality before a button can receive input.
        QTest::mouseClick(profile->windowHandle(), Qt::LeftButton, Qt::NoModifier,
                          closeButton->mapTo(profile, closeButton->rect().center()));
        QVERIFY(!profile || !profile->isVisible());
        QVERIFY(card->isVisible());
        QTRY_VERIFY(profile.isNull());

        QVERIFY(openProfile());
        profile = window.findChild<QDialog *>(QStringLiteral("personProfileDialog"));
        QVERIFY(profile && profile->isVisible() && profile->windowHandle());
        QCOMPARE(profile->windowHandle()->transientParent(), card->windowHandle());
        QVERIFY(profile->windowHandle()->close());
        QVERIFY(!profile || !profile->isVisible());
        QVERIFY(card->isVisible());
        QTRY_VERIFY(profile.isNull());

        QVERIFY(openProfile());
        profile = window.findChild<QDialog *>(QStringLiteral("personProfileDialog"));
        QVERIFY(profile && profile->isVisible());
        card->close();
        QTRY_VERIFY(profile.isNull());
        emit backend->personLoaded({QStringLiteral("19"), QStringLiteral("Старая карточка"), {}, {}});
        window.openMessageLink(QStringLiteral("/company/personal/user/20/"));
        QCOMPARE(backend->requestedPerson, QStringLiteral("20"));
        profile = window.findChild<QDialog *>(QStringLiteral("personProfileDialog"));
        QVERIFY(profile && profile->parentWidget() == &window);
        auto *name = findAccessible<QLabel>(profile, QStringLiteral("Имя сотрудника"));
        QVERIFY(name);
        emit backend->personLoaded({QStringLiteral("19"), QStringLiteral("Старый ответ"), {}, {}});
        QCOMPARE(name->text(), QStringLiteral("Загружаем профиль…"));
    }

    void invalidTaskLinksAreSuppressed()
    {
        const auto portal = QUrl(QStringLiteral("https://example.bitrix24.ru"));
        QVERIFY(ui::taskLink(portal, QStringLiteral("101"), QStringLiteral("../22")).isEmpty());
        QVERIFY(ui::taskLink(portal, QStringLiteral("0"), QStringLiteral("501")).isEmpty());
        QVERIFY(ui::taskLink(QUrl(QStringLiteral("https://name@example.bitrix24.ru")),
                             QStringLiteral("101"), QStringLiteral("501")).isEmpty());
        QVERIFY(ui::taskLink(QUrl(QStringLiteral("http://example.bitrix24.ru")),
                             QStringLiteral("101"), QStringLiteral("501")).isEmpty());
    }

    void logoutIsVisibleAtCompactWidthAndWorksFromKeyboard()
    {
        DemoBackend demo;
        MainWindow window(&demo);
        window.resize(720, 500);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *logout = findAccessible<QPushButton>(&window, QStringLiteral("Выйти из аккаунта"));
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(logout);
        QVERIFY(sections);
        QVERIFY(!logout->isVisible());
        QVERIFY(!logout->isEnabled());
        window.setLogoutAvailable(true);
        QCOMPARE(logout->text(), QStringLiteral("Выйти"));
        for (int row = 0; row < sections->count(); ++row) {
            sections->setCurrentRow(row);
            QVERIFY(logout->isVisible());
            QVERIFY(logout->isEnabled());
            QVERIFY(logout->parentWidget()->rect().contains(logout->geometry()));
        }
        sections->setCurrentRow(0);
        logout->setFocus(Qt::TabFocusReason);
        QTRY_VERIFY(logout->hasFocus());
        const auto screenshot = qEnvironmentVariable("B24_LOGOUT_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(window.grab().save(screenshot));

        QSignalSpy requests(&window, &MainWindow::logoutRequested);
        QTest::keyClick(logout, Qt::Key_Space);
        QCOMPARE(requests.size(), 1);
        QVERIFY(window.isVisible());
        window.setLogoutAvailable(false);
        logout->click();
        QCOMPARE(requests.size(), 1);
    }
};

QTEST_MAIN(UiShellTest)
#include "tst_ui_shell.moc"
