#include "pilot_fixture.h"

using namespace bx;
using namespace bx::test;

class UiNotificationsTest : public PilotFixture {
    Q_OBJECT

private slots:
    void notificationFiltersKeepPendingAndFailedHistoryVisible()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdNotifications = true;
        MainWindow window(backend.get());
        window.show();
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        auto *status = findAccessible<QLabel>(&window, QStringLiteral("Состояние уведомлений"));
        auto *search = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск уведомлений"));
        auto *filter = findAccessible<QCheckBox>(&window, QStringLiteral("Только непрочитанные уведомления"));
        auto *sort = findAccessible<QComboBox>(&window, QStringLiteral("Порядок уведомлений"));
        QVERIFY(sections && status && search && filter && sort);
        sections->setCurrentRow(2);
        filter->setChecked(true);
        sort->setCurrentIndex(1);
        QVERIFY(status->isVisible());
        QCOMPARE(status->text(), QStringLiteral("Загружаем уведомления…"));
        auto *indicator = status->findChild<QWidget *>(QStringLiteral("sharedLoadingIndicator"));
        QVERIFY(indicator && indicator->isVisible());

        search->setText(QStringLiteral("задача"));
        QTest::keyClick(search, Qt::Key_Return);
        QCOMPARE(status->text(), QStringLiteral("Ищем уведомления…"));
        emit backend->failed(QStringLiteral("notifications"), QStringLiteral("Нет ответа"));
        QCOMPARE(status->text(), QStringLiteral("Ищем уведомления…"));
        QVERIFY(indicator->isVisible());
        search->clear();
        QTest::keyClick(search, Qt::Key_Return);
        QCOMPARE(status->text(), QStringLiteral("Не удалось загрузить уведомления"));
        QVERIFY(indicator->isHidden());
        sort->setCurrentIndex(0);
        QCOMPARE(status->text(), QStringLiteral("Не удалось загрузить уведомления"));

        emit backend->notificationsLoaded({});
        QCOMPARE(status->text(), QStringLiteral("Непрочитанных уведомлений нет"));
        QVERIFY(indicator->isHidden());
    }

    void labelsTaskChatMessageNotice()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        Notification taskNotice;
        taskNotice.id = QStringLiteral("801");
        taskNotice.title = QStringLiteral("Ошибка маршрутизации");
        taskNotice.text = QStringLiteral("Задача #123727 Не открывается ни один маршрут");
        taskNotice.createdAt = QDateTime::currentDateTime();
        taskNotice.targets = {{NotificationTargetKind::Task, QStringLiteral("123727")},
                              {NotificationTargetKind::Chat, QStringLiteral("chat42"), QStringLiteral("502")}};
        taskNotice.targets.last().taskChat = true;
        auto chatNotice = taskNotice;
        chatNotice.id = QStringLiteral("800");
        chatNotice.createdAt = taskNotice.createdAt.addSecs(-1);
        chatNotice.targets.removeFirst();
        chatNotice.targets.first().taskChat = false;
        backend->notifications = {taskNotice, chatNotice};

        MainWindow window(backend.get(), true);
        window.show();
        auto *sections = window.findChild<QListWidget *>(QStringLiteral("sections"));
        QVERIFY(sections);
        sections->setCurrentRow(2);
        auto *list = findAccessible<QListWidget>(&window, QStringLiteral("Уведомления"));
        QVERIFY(list);
        QTRY_COMPARE(list->count(), 2);
        const auto chat = list->item(1)->data(workspace::NoticePresentation).value<workspace::NoticeRow>();
        QCOMPARE(chat.excerpt, QStringLiteral("Не открывается ни один маршрут"));
        QCOMPARE(chat.primaryLabel, QStringLiteral("Открыть сообщение"));
        const auto task = list->item(0)->data(workspace::NoticePresentation).value<workspace::NoticeRow>();
        QCOMPARE(task.primaryTarget, 1);
        QCOMPARE(task.excerpt, QStringLiteral("Сообщение в чате задачи: Не открывается ни один маршрут"));
        QCOMPARE(task.primaryLabel, QStringLiteral("Открыть в чате задачи"));
    }

    void rendersNoticeBodyLinksAndKeepsDistinctTitle()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdLinkPreviews = true;
        Notification structured;
        structured.id = QStringLiteral("801");
        structured.title = QStringLiteral("Уведомление Битрикс24");
        structured.text = QStringLiteral("Заявка на печать. Задача");
        structured.markup = QStringLiteral("Заявка на печать\n[URL=/workgroups/group/7/tasks/task/view/501/]Задача[/URL]");
        structured.createdAt = QDateTime(QDate::currentDate(), QTime(15, 15));
        structured.read = true;
        Notification titled;
        titled.id = QStringLiteral("800");
        titled.title = QStringLiteral("Возвраты клиентам");
        titled.text = QStringLiteral("Задача #122889 После закрытия бокса не появляется отметка «Закрыт».\n")
            + QStringLiteral("При выборе клиента открывается документ «Сбор возврата». Детали собираются в бокс, "
                             "после чего бокс размещается в ячейке. ").repeated(12);
        titled.createdAt = structured.createdAt.addSecs(-60);
        titled.targets = {{NotificationTargetKind::Task, QStringLiteral("122889")},
                          {NotificationTargetKind::Chat, QStringLiteral("chat42"), QStringLiteral("502")}};
        backend->notifications = {structured, titled};
        for (int i = 0; i < 16; ++i) {
            auto archived = titled;
            archived.id = QString::number(799 - i);
            archived.title = QStringLiteral("Ошибка картриджа с тонером на принтере");
            archived.text = i % 2 == 0 ? QStringLiteral("Иван Привезенцев стал наблюдателем")
                : QStringLiteral("Закончились картриджи, информацию передали в ИТ");
            archived.createdAt = titled.createdAt.addDays(-1).addSecs(-i * 124);
            archived.read = i > 0;
            if (i == 15)
                archived.markup = QStringLiteral("[URL=/tasks/task/view/999/]Старая задача[/URL]");
            backend->notifications.append(std::move(archived));
        }
        MainWindow window(backend.get(), true);
        window.resize(1000, 750);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *sections = window.findChild<QListWidget *>(QStringLiteral("sections"));
        QVERIFY(sections);
        sections->setCurrentRow(2);
        auto *list = findAccessible<QListWidget>(&window, QStringLiteral("Уведомления"));
        QVERIFY(list);
        QTRY_COMPARE(list->count(), 18);
        QVERIFY(list->item(0)->data(workspace::Title).toString().isEmpty());
        QCOMPARE(list->item(1)->data(workspace::Title).toString(), titled.title);
        QVERIFY(list->item(0)->data(workspace::MarkupHtml).toString().contains(QStringLiteral("href=")));
        QVERIFY(list->sizeHintForRow(1) < 100);
        const auto presentation = [](const QListWidgetItem *item) {
            return item->data(workspace::NoticePresentation).value<workspace::NoticeRow>();
        };
        QCOMPARE(presentation(list->item(0)).day, QStringLiteral("Сегодня"));
        QCOMPARE(presentation(list->item(2)).day, QStringLiteral("Вчера"));
        QCOMPARE(presentation(list->item(1)).primaryTarget, 1);
        QVERIFY(presentation(list->item(1)).excerpt.startsWith(QStringLiteral("После закрытия")));
        emit backend->connectionChanged(true);
        emit backend->notificationHistoryPageStateChanged(true, false);
        QCoreApplication::processEvents();
        QVERIFY(!findAccessible<QPushButton>(&window, QStringLiteral("Более ранние уведомления")));
        QCOMPARE(backend->olderNoticeRequests, 0);
        const auto screenshot = qEnvironmentVariable("B24_NOTICE_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(window.grab().save(screenshot));
        QVERIFY(backend->linkPreviewRequests.isEmpty());
        const auto rowGap = list->visualItemRect(list->item(1)).top() -
            static_cast<workspace::RowPainter *>(list->itemDelegate())
                ->receiptArea(list->model()->index(0, 0), list->visualItemRect(list->item(0))).bottom();
        QVERIFY(rowGap >= 10);
        const auto collapsedHeight = list->sizeHintForRow(1);
        list->setCurrentRow(1);
        list->setFocus();
        QTest::keyClick(list, Qt::Key_Return);
        QTRY_VERIFY(list->sizeHintForRow(1) > collapsedHeight);
        const auto expandedScreenshot = qEnvironmentVariable("B24_NOTICE_EXPANDED_SCREENSHOT");
        if (!expandedScreenshot.isEmpty())
            QVERIFY(window.grab().save(expandedScreenshot));
        list->setCurrentRow(0);
        QTest::keyClick(list, Qt::Key_Return);
        QCOMPARE(list->sizeHintForRow(1), collapsedHeight);
        QTRY_COMPARE(backend->linkPreviewRequests.size(), 1);
        QCOMPARE(backend->linkPreviewRequests.constFirst().first.path(),
                 QStringLiteral("/workgroups/group/7/tasks/task/view/501/"));
        auto complete = std::move(backend->linkPreviewRequests.first().second);
        const auto withoutPreview = list->visualItemRect(list->item(0)).height();
        const auto withoutPreviewHint = list->sizeHintForRow(0);
        complete({QStringLiteral("Восстановить печать"), {QStringLiteral("Проект · IT")}, MessageLinkPreviewKind::Task});
        QCoreApplication::processEvents();
        list->doItemsLayout();
        QCOMPARE(list->sizeHintForRow(0), withoutPreviewHint);
        QCOMPARE(list->visualItemRect(list->item(0)).height(), withoutPreview);
        QTRY_VERIFY(list->visualItemRect(list->item(1)).top() > list->visualItemRect(list->item(0)).bottom());
        auto *painter = static_cast<workspace::RowPainter *>(list->itemDelegate());
        const auto index = list->model()->index(0, 0);
        const auto rect = list->visualRect(index);
        QPoint link;
        for (int y = rect.top(); y < rect.bottom() && link.isNull(); ++y)
            for (int x = rect.left(); x < rect.right(); ++x)
                if (!painter->anchorAt(index, rect, QPoint(x, y)).isEmpty()) {
                    link = QPoint(x, y);
                    break;
                }
        QVERIFY(!link.isNull());
        QMouseEvent move(QEvent::MouseMove, link, list->viewport()->mapToGlobal(link),
                         Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(list->viewport(), &move);
        QCOMPARE(list->viewport()->cursor().shape(), Qt::PointingHandCursor);
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, link);
        QTRY_VERIFY(window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        QVERIFY(backend->noticeReadBatches.isEmpty());
        window.findChild<QDialog *>(QStringLiteral("taskCardDialog"))->close();
        auto *unread = findAccessible<QCheckBox>(&window, QStringLiteral("Только непрочитанные уведомления"));
        QVERIFY(unread);
        unread->setChecked(true);
        QCOMPARE(list->item(0)->data(workspace::Id).toString(), titled.id);
        list->setCurrentRow(0);
        QApplication::setActiveWindow(&window);
        list->setFocus();
        QTest::keyClick(list, Qt::Key_Return);
        QTRY_VERIFY(presentation(list->item(0)).expanded);
        emit backend->notificationReadStateChanged({titled.id}, true);
        QCOMPARE(list->item(0)->data(workspace::Id).toString(), titled.id);
        QVERIFY(presentation(list->item(0)).expanded);
        QTest::keyClick(list, Qt::Key_Return);
        QTRY_COMPARE(list->count(), 1);
        QVERIFY(list->item(0)->data(workspace::Id).toString() != titled.id);
    }

    void marksOnlyVisibleNotificationsAndWaitsForTheNextScroll()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        for (int id = 950; id > 934; --id) {
            Notification notice;
            notice.id = QString::number(id);
            notice.title = QStringLiteral("Событие");
            notice.text = QString(5, QLatin1Char('\n')) + QStringLiteral("Подробности");
            backend->notifications.append(notice);
        }
        MainWindow window(backend.get());
        window.resize(900, 540);
        window.show();
        QApplication::setActiveWindow(&window);
        QTRY_COMPARE(QGuiApplication::applicationState(), Qt::ApplicationActive);
        auto *sections = window.findChild<QListWidget *>(QStringLiteral("sections"));
        auto *filter = findAccessible<QCheckBox>(&window, QStringLiteral("Только непрочитанные уведомления"));
        QVERIFY(sections && filter);
        QVERIFY(backend->noticeReadBatches.isEmpty());
        filter->setChecked(true);
        sections->setCurrentRow(2);
        auto *list = findAccessible<QListWidget>(&window, QStringLiteral("Уведомления"));
        QVERIFY(list);
        QTRY_COMPARE(list->count(), 16);
        auto *readTimer = window.findChild<QTimer *>(QStringLiteral("noticeReadTimer"));
        QVERIFY(readTimer && readTimer->isActive());
        QDialog options(&window);
        options.setWindowModality(Qt::WindowModal);
        options.show();
        QApplication::setActiveWindow(&options);
        fireTimer(*readTimer);
        QVERIFY(backend->noticeReadBatches.isEmpty());
        options.close();
        QApplication::setActiveWindow(&window);
        QCoreApplication::processEvents();
        QTRY_VERIFY(window.isActiveWindow() && !QApplication::activeModalWidget());
        list->verticalScrollBar()->setValue(1);
        list->verticalScrollBar()->setValue(0);
        deliverPendingTimer(window, QStringLiteral("noticeReadTimer"));
        QCOMPARE(backend->noticeReadBatches.size(), 1);
        const auto first = backend->noticeReadBatches.constFirst();
        QVERIFY(!first.isEmpty());
        QVERIFY(first.contains(QStringLiteral("950")));
        QVERIFY(!first.contains(QStringLiteral("935")));
        for (const auto &id : first) {
            const auto row = 950 - id.toInt();
            QVERIFY(row >= 0 && row < list->count());
            QVERIFY(list->visualItemRect(list->item(row)).intersects(list->viewport()->rect()));
        }
        emit backend->notificationReadStateChanged(first, true);
        emit backend->notificationActionFinished(QStringLiteral("read-notifications"));
        QTRY_COMPARE(list->count(), 16 - first.size());
        QCOMPARE(list->indexAt(QPoint(4, 4)).data(workspace::Id).toString(), QString::number(950 - first.size()));
        deliverPendingTimer(window, QStringLiteral("noticeReadTimer"));
        QCOMPARE(backend->noticeReadBatches.size(), 1);

        list->verticalScrollBar()->setValue(list->verticalScrollBar()->maximum());
        deliverPendingTimer(window, QStringLiteral("noticeReadTimer"));
        QCOMPARE(backend->noticeReadBatches.size(), 2);
        QVERIFY(backend->noticeReadBatches.last().contains(QStringLiteral("935")));
        emit backend->failed(QStringLiteral("read-notifications"), QStringLiteral("Ошибка фикстуры"));
        deliverPendingTimer(window, QStringLiteral("noticeReadTimer"));
        QCOMPARE(backend->noticeReadBatches.size(), 2);
    }

    void ordinaryChatPopupsDoNotAppearInEventsOrSearch()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->chats.first().unread = 4;
        Notification ordinary;
        ordinary.id = QStringLiteral("push:ordinary-message");
        ordinary.title = QStringLiteral("Техническая поддержка");
        ordinary.text = QStringLiteral("Событие разговора с поддержкой");
        ordinary.targets = {{NotificationTargetKind::Chat, QStringLiteral("chat95"), QStringLiteral("801")},
                            {NotificationTargetKind::Task, QStringLiteral("501")}};
        ordinary.ordinaryChatMessage = true;
        Notification task;
        task.id = QStringLiteral("message:802");
        task.title = QStringLiteral("Чат задачи");
        task.text = QStringLiteral("Событие чата задачи");
        task.targets = {{NotificationTargetKind::Chat, QStringLiteral("chat96"), QStringLiteral("802"), {}, {}, true}};
        Notification notice;
        notice.id = QStringLiteral("803");
        notice.type = QStringLiteral("im");
        notice.title = QStringLiteral("Уведомление портала");
        notice.text = QStringLiteral("Событие портала");
        notice.targets = {ordinary.targets.first()};
        backend->notifications = {ordinary, task, notice};
        MainWindow window(backend.get());
        window.resize(900, 700);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *sections = window.findChild<QListWidget *>(QStringLiteral("sections"));
        auto *list = findAccessible<QListWidget>(&window, QStringLiteral("Уведомления"));
        auto *search = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск уведомлений"));
        QVERIFY(sections && list && search);
        QTRY_COMPARE(sections->item(0)->data(ui::SectionUnreadCount).toLongLong(), 4);
        QTRY_COMPARE(sections->item(2)->data(ui::SectionUnreadCount).toLongLong(), 2);
        sections->setCurrentRow(2);
        const auto listedIds = [list] {
            QStringList ids;
            for (int row = 0; row < list->count(); ++row)
                ids.append(list->item(row)->data(workspace::Id).toString());
            ids.sort();
            return ids;
        };
        const QStringList expected{notice.id, task.id};
        QCOMPARE(listedIds(), expected);
        QTRY_COMPARE(backend->noticeReadBatches.size(), 1);
        auto read = backend->noticeReadBatches.first();
        read.sort();
        QCOMPARE(read, expected);

        search->setText(QStringLiteral("т"));
        QTRY_COMPARE(listedIds(), expected);
        search->setText(QStringLiteral("событие"));
        QTRY_COMPARE(backend->noticeQuery, search->text());
        emit backend->notificationsFound(search->text(), {ordinary, task, notice});
        QCOMPARE(listedIds(), expected);
        emit backend->notificationsFound(search->text(), {ordinary});
        QVERIFY(listedIds().isEmpty());
        QCOMPARE(sections->item(2)->data(ui::SectionUnreadCount).toLongLong(), 2);
    }

    void marksVisibleSyntheticNoticeThroughTheSameViewportBatch()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        Notification synthetic;
        synthetic.id = QStringLiteral("message:951");
        synthetic.text = QStringLiteral("Новое событие в чате задачи");
        synthetic.createdAt = QDateTime::currentDateTime().addDays(1);
        backend->notifications = {synthetic};
        MainWindow window(backend.get());
        window.resize(900, 540);
        window.show();
        QApplication::setActiveWindow(&window);
        window.findChild<QListWidget *>(QStringLiteral("sections"))->setCurrentRow(2);
        QTRY_COMPARE(backend->noticeReadBatches.size(), 1);
        QCOMPARE(backend->noticeReadBatches.first(), QStringList{synthetic.id});
    }

    void resetsVisibleNoticesWhenTheAccountChanges()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->accountScope = QStringLiteral("fixture/first-account");
        Notification oldNotice;
        oldNotice.id = QStringLiteral("971");
        oldNotice.text = QStringLiteral("Событие первого аккаунта");
        backend->notifications = {oldNotice};
        MainWindow window(backend.get());
        window.show();
        QApplication::setActiveWindow(&window);
        auto *sections = window.findChild<QListWidget *>(QStringLiteral("sections"));
        auto *list = findAccessible<QListWidget>(&window, QStringLiteral("Уведомления"));
        auto *search = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск уведомлений"));
        QVERIFY(sections && list && search);
        sections->setCurrentRow(2);
        QTRY_COMPARE(backend->noticeReadBatches, QVector<QStringList>{{oldNotice.id}});
        search->setText(QStringLiteral("старый запрос"));

        backend->holdNotifications = true;
        backend->accountScope = QStringLiteral("fixture/second-account");
        emit backend->accountScopeChanged();
        QCOMPARE(list->count(), 0);
        QVERIFY(search->text().isEmpty());
        QVERIFY(sections->item(2)->data(Qt::AccessibleDescriptionRole).toString().isEmpty());
        QTRY_COMPARE(backend->noticeHistoryRequests, 2);
        Notification newNotice;
        newNotice.id = QStringLiteral("972");
        newNotice.text = QStringLiteral("Событие второго аккаунта");
        emit backend->notificationsLoaded({newNotice});
        QTRY_COMPARE(backend->noticeReadBatches.size(), 2);
        QCOMPARE(backend->noticeReadBatches.last(), QStringList{newNotice.id});
        QVERIFY(backend->noticeQuery.isEmpty());

        emit list->customContextMenuRequested(list->visualItemRect(list->item(0)).center());
        QPointer<QMenu> menu = list->findChild<QMenu *>(QStringLiteral("noticeMenu"));
        QVERIFY(menu && menu->isVisible());
        backend->accountScope = QStringLiteral("fixture/first-account");
        emit backend->accountScopeChanged();
        QVERIFY(!menu || !menu->isVisible());
    }

    void returnsToTheTopWhenReenteringNotifications()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        const auto newest = QDateTime::currentDateTimeUtc();
        for (int row = 0; row < 30; ++row) {
            Notification notice;
            notice.id = QString::number(1000 - row);
            notice.text = QStringLiteral("Уведомление %1").arg(row);
            notice.createdAt = newest.addSecs(-row);
            notice.read = true;
            backend->notifications.append(notice);
        }
        MainWindow window(backend.get(), true);
        window.resize(780, 500);
        window.show();
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        auto *list = findAccessible<QListWidget>(&window, QStringLiteral("Уведомления"));
        QVERIFY(sections && list);
        sections->setCurrentRow(2);
        QTRY_COMPARE(list->count(), 30);

        auto *scroll = list->verticalScrollBar();
        list->scrollToBottom();
        QVERIFY(scroll->value() > scroll->minimum());
        const auto position = scroll->value();
        emit backend->notificationsLoaded(backend->notifications);
        QCOMPARE(scroll->value(), position);

        sections->setCurrentRow(0);
        sections->setCurrentRow(2);
        QCOMPARE(scroll->value(), scroll->minimum());
        QCOMPARE(list->indexAt(QPoint(4, 0)).row(), 0);
    }

    void loadsEarlierNotificationsAtTheHistoryEdge()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->accountScope = QStringLiteral("fixture/first-account");
        const auto newest = QDateTime::currentDateTimeUtc();
        for (int id = 970; id >= 955; --id) {
            Notification item;
            item.id = QString::number(id);
            item.text = QStringLiteral("Задача %1").arg(id);
            item.createdAt = newest.addSecs(id - 970);
            item.read = id == 970;
            backend->notifications.append(item);
        }
        MainWindow window(backend.get(), true);
        window.resize(780, 500);
        window.show();
        findAccessible<QListWidget>(&window, QStringLiteral("Разделы"))->setCurrentRow(2);
        auto *list = findAccessible<QListWidget>(&window, QStringLiteral("Уведомления"));
        auto *search = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск уведомлений"));
        auto *filter = findAccessible<QCheckBox>(&window, QStringLiteral("Только непрочитанные уведомления"));
        auto *sort = findAccessible<QComboBox>(&window, QStringLiteral("Порядок уведомлений"));
        QVERIFY(list && search && filter && sort);
        QTRY_COMPARE(list->count(), 16);
        auto *scroll = list->verticalScrollBar();
        QVERIFY(scroll->maximum() > 0);
        window.onConnectionChanged(false);
        list->scrollToBottom();
        emit backend->notificationHistoryPageStateChanged(true, false);
        QCoreApplication::processEvents();
        QCOMPARE(backend->olderNoticeRequests, 0);
        list->scrollToTop();
        window.onConnectionChanged(true);
        search->setText(QStringLiteral("задача"));
        list->scrollToBottom();
        QCoreApplication::processEvents();
        QCOMPARE(backend->olderNoticeRequests, 0);
        QTRY_COMPARE(backend->noticeQuery, search->text());
        search->clear();
        QTRY_COMPARE(list->count(), 16);
        scroll->setValue(scroll->maximum() / 2);
        QCoreApplication::processEvents();
        QCOMPARE(backend->olderNoticeRequests, 0);
        list->scrollToBottom();
        QTRY_COMPARE(backend->olderNoticeRequests, 1);
        QVERIFY(!findAccessible<QPushButton>(&window, QStringLiteral("Более ранние уведомления")));
        list->scrollToTop();
        list->scrollToBottom();
        QCoreApplication::processEvents();
        QCOMPARE(backend->olderNoticeRequests, 1);
        const auto top = list->indexAt(QPoint(4, 0)).data(workspace::Id).toString();
        QVERIFY(!top.isEmpty());

        Notification earlier;
        earlier.id = QStringLiteral("954");
        earlier.text = QStringLiteral("Ранняя задача");
        earlier.createdAt = newest.addSecs(-16);
        backend->notifications.append(earlier);
        emit backend->notificationsLoaded(backend->notifications);
        QCOMPARE(list->count(), 17);
        QCOMPARE(list->item(16)->data(workspace::Id).toString(), earlier.id);
        QCOMPARE(list->indexAt(QPoint(4, 0)).data(workspace::Id).toString(), top);
        emit backend->notificationHistoryPageStateChanged(true, false);
        QCoreApplication::processEvents();
        QCOMPARE(backend->olderNoticeRequests, 1);
        filter->setChecked(true);
        sort->setCurrentIndex(1);
        QVERIFY(filter->isChecked());
        QCOMPARE(sort->currentIndex(), 1);
        QTRY_COMPARE(backend->olderNoticeRequests, 2);
        emit backend->notificationHistoryPageStateChanged(true, false);
        emit backend->failed(QStringLiteral("notifications-older"), QStringLiteral("Нет ответа"));
        QCoreApplication::processEvents();
        QCOMPARE(backend->olderNoticeRequests, 2);
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_F5);
        QTRY_COMPARE(backend->olderNoticeRequests, 3);
        backend->notifications.clear();
        backend->accountScope = QStringLiteral("fixture/second-account");
        emit backend->accountScopeChanged();
        QCoreApplication::processEvents();
        QCOMPARE(backend->olderNoticeRequests, 3);
        emit backend->notificationsLoaded({earlier});
        emit backend->notificationHistoryPageStateChanged(true, false);
        QTRY_COMPARE(backend->olderNoticeRequests, 4);
        QVERIFY(backend->noticeReadBatches.isEmpty());
    }

    void notificationActionsWaitForAcknowledgement()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        Notification old;
        old.id = QStringLiteral("51");
        old.title = QStringLiteral("Старая задача");
        old.createdAt = QDateTime::currentDateTimeUtc().addSecs(-60);
        old.read = true;
        auto recent = old;
        recent.id = QStringLiteral("52");
        recent.title = QStringLiteral("Новая задача");
        recent.createdAt = old.createdAt.addSecs(30);
        recent.read = false;
        backend->notifications = {old, recent};
        MainWindow window(backend.get());
        window.show();
        findAccessible<QListWidget>(&window, QStringLiteral("Разделы"))->setCurrentRow(2);
        auto *rows = findAccessible<QListWidget>(&window, QStringLiteral("Уведомления"));
        auto *search = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск уведомлений"));
        auto *filter = findAccessible<QCheckBox>(&window, QStringLiteral("Только непрочитанные уведомления"));
        auto *sort = findAccessible<QComboBox>(&window, QStringLiteral("Порядок уведомлений"));
        auto *read = window.findChild<QAction *>(QStringLiteral("readNotice"));
        auto *readAll = findAccessible<QPushButton>(&window, QStringLiteral("Прочитать все уведомления"));
        auto *remove = window.findChild<QAction *>(QStringLiteral("deleteNotice"));
        QVERIFY(rows && search && filter && sort && read && readAll && remove);
        QCOMPARE(rows->count(), 2);
        rows->setCurrentRow(0);
        QCOMPARE(rows->currentItem()->data(workspace::Id).toString(), recent.id);
        sort->setCurrentIndex(1);
        QCOMPARE(rows->currentRow(), 1);
        filter->setChecked(true);
        QCOMPARE(rows->count(), 1);
        search->setText(QStringLiteral("задача"));
        QTRY_COMPARE(backend->noticeQuery, search->text());
        emit backend->notificationsFound(QStringLiteral("другой запрос"), {recent});
        QCOMPARE(rows->count(), 0);
        emit backend->notificationsFound(search->text(), {old, recent});
        QCOMPARE(rows->count(), 1);
        QVERIFY(backend->noticeReadId.isEmpty());
        QApplication::setActiveWindow(&window);
        rows->setFocus();
        QTest::keyClick(rows, Qt::Key_F10, Qt::ShiftModifier);
        auto *menu = rows->findChild<QMenu *>(QStringLiteral("noticeMenu"));
        QTRY_VERIFY(menu && menu->isVisible());
        QVERIFY(menu->actions().contains(read));
        menu->setActiveAction(read);
        QTest::keyClick(menu, Qt::Key_Return);
        QCOMPARE(backend->noticeReadId, recent.id);
        QCOMPARE(rows->count(), 1);
        QVERIFY(!remove->isEnabled());
        emit backend->failed(QStringLiteral("read-notification"), QStringLiteral("Не удалось"));
        QVERIFY(read->isEnabled());
        read->trigger();
        emit backend->notificationReadStateChanged({recent.id}, true);
        emit backend->notificationActionFinished(QStringLiteral("read-notification"));
        QCOMPARE(rows->count(), 0);
        filter->setChecked(false);
        QCOMPARE(rows->currentItem()->data(workspace::Id).toString(), recent.id);
        readAll->click();
        QCOMPARE(backend->noticeReadAll, 1);
        emit backend->notificationActionFinished(QStringLiteral("read-all-notifications"));
        QTimer::singleShot(0, &window, [] {
            auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (dialog)
                dialog->button(QMessageBox::Yes)->click();
        });
        remove->trigger();
        QCOMPARE(backend->noticeDeleteId, recent.id);
        QCOMPARE(rows->count(), 2);
        emit backend->notificationRemoved(recent.id);
        emit backend->notificationActionFinished(QStringLiteral("delete-notification"));
        QCOMPARE(rows->count(), 1);
    }

    void notificationSettingsKeepPendingWriteAcrossSnapshots()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        const auto muted = QStringLiteral("soundMuted");
        backend->noticeSettings = {{muted, false}};
        MainWindow window(backend.get());
        window.show();
        emit backend->connectionChanged(true);
        findAccessible<QListWidget>(&window, QStringLiteral("Разделы"))->setCurrentRow(2);
        auto *settings = findAccessible<QPushButton>(&window, QStringLiteral("Настройки уведомлений портала"));
        QVERIFY(settings);
        settings->click();
        auto *save = findAccessible<QPushButton>(&window, QStringLiteral("Сохранить настройку «Без звука»"));
        QVERIFY(save);
        auto *toggle = save->parentWidget()->findChild<QCheckBox *>();
        QVERIFY(toggle);
        toggle->setChecked(true);
        save->click();
        QCOMPARE(backend->noticeSettingWrites.size(), 1);
        emit backend->notificationSettingsLoaded({{muted, false}});
        QVERIFY(toggle->isChecked());
        save->click();
        QCOMPARE(backend->noticeSettingWrites.size(), 1);
        emit backend->notificationSettingsLoaded({{muted, true}}, muted);
        toggle->setChecked(false);
        save->click();
        QCOMPARE(backend->noticeSettingWrites.size(), 2);
        QCOMPARE(backend->noticeSettingWrites.last().second, 0);
    }

    void notificationSettingsOfferRecoveryAndAreOmittedInDemo()
    {
        const auto caption = QStringLiteral("Настройки уведомлений портала");
        DemoBackend demo;
        MainWindow demoWindow(&demo);
        demoWindow.show();
        findAccessible<QListWidget>(&demoWindow, QStringLiteral("Разделы"))->setCurrentRow(2);
        auto *demoSettings = findAccessible<QPushButton>(&demoWindow, caption);
        QVERIFY(!demoSettings || !demoSettings->isVisible());
        demoWindow.hide();

        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        backend->noticeSettingsReadFails = true;
        backend->noticeSettings = {{QStringLiteral("soundMuted"), false}};
        MainWindow window(backend.get());
        window.show();
        emit backend->connectionChanged(true);
        findAccessible<QListWidget>(&window, QStringLiteral("Разделы"))->setCurrentRow(2);
        auto *settings = findAccessible<QPushButton>(&window, caption);
        QVERIFY(settings && settings->isVisible());
        settings->click();
        QCOMPARE(backend->noticeSettingsReads, 1);
        QPointer<QDialog> dialog = window.findChild<QDialog *>(QStringLiteral("notificationSettingsDialog"));
        QVERIFY(dialog && dialog->isVisible());
        auto *buttons = dialog->findChild<QDialogButtonBox *>();
        QVERIFY(buttons);
        const auto *close = buttons->button(QDialogButtonBox::Close);
        QVERIFY(close && close->text() == QStringLiteral("Закрыть"));
        auto *retry = findAccessible<QPushButton>(dialog, QStringLiteral("Повторить загрузку настроек"));
        QVERIFY(retry && retry->isVisible());
        const auto *workspaceStatus = findAccessible<QLabel>(&window, QStringLiteral("Состояние"));
        QVERIFY(workspaceStatus && !workspaceStatus->isVisible());
        backend->noticeSettingsReadFails = false;
        retry->click();
        QCOMPARE(backend->noticeSettingsReads, 2);
        QVERIFY(!retry->isVisible());
        const auto *save = findAccessible<QPushButton>(dialog, QStringLiteral("Сохранить настройку «Без звука»"));
        QVERIFY(save && save->isVisible() && save->isEnabled());
        QTest::keyClick(dialog, Qt::Key_Escape);
        QTRY_VERIFY(!dialog || !dialog->isVisible());
    }

    void queuedNoticeClickHonorsActiveSection_data()
    {
        QTest::addColumn<bool>("leaveSection");
        QTest::newRow("stay in notifications") << false;
        QTest::newRow("leave before delivery") << true;
    }

    void queuedNoticeClickHonorsActiveSection()
    {
        QFETCH(bool, leaveSection);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        Notification notice;
        notice.id = QStringLiteral("1");
        notice.title = QStringLiteral("Сотрудник");
        notice.read = true;
        notice.targets = {{NotificationTargetKind::Person, QStringLiteral("19")}};
        backend->notifications = {notice};
        MainWindow window(backend.get());
        window.show();
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        auto *rows = findAccessible<QListWidget>(&window, QStringLiteral("Уведомления"));
        QVERIFY(sections && rows);
        sections->setCurrentRow(2);
        QTRY_COMPARE(rows->count(), 1);
        const auto clickRow = [&](const QPoint &offset) {
            QTest::mouseClick(rows->viewport(), Qt::LeftButton, Qt::NoModifier,
                              rows->visualItemRect(rows->item(0)).topLeft() + offset);
        };
        clickRow(QPoint(50, 15));
        QTRY_VERIFY(rows->item(0)->data(workspace::NoticePresentation).value<workspace::NoticeRow>().expanded);
        clickRow(QPoint(50, 54));
        QVERIFY(backend->requestedPerson.isEmpty());
        if (leaveSection)
            sections->setCurrentRow(1);
        QCoreApplication::processEvents();
        QCOMPARE(backend->requestedPerson, leaveSection ? QString() : QStringLiteral("19"));
        QCOMPARE(sections->currentRow(), leaveSection ? 1 : 2);
    }

    void reorderedNoticeOpensOwnTarget_data()
    {
        QTest::addColumn<bool>("useMenu");
        QTest::newRow("shortcut") << false;
        QTest::newRow("context menu") << true;
    }

    void reorderedNoticeOpensOwnTarget()
    {
        QFETCH(bool, useMenu);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        Notification first, second;
        first.id = QStringLiteral("2");
        first.targets = {{NotificationTargetKind::Person, QStringLiteral("18")}};
        second.id = QStringLiteral("1");
        second.targets = {{NotificationTargetKind::Person, QStringLiteral("19")}};
        backend->notifications = {first, second};
        MainWindow window(backend.get());
        window.show();
        QApplication::setActiveWindow(&window);
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        auto *rows = findAccessible<QListWidget>(&window, QStringLiteral("Уведомления"));
        QVERIFY(sections && rows);
        sections->setCurrentRow(2);
        QTRY_COMPARE(rows->count(), 2);
        auto *moved = rows->takeItem(1);
        rows->insertItem(0, moved);
        QCOMPARE(moved->data(workspace::Id).toString(), second.id);
        rows->setCurrentItem(moved);
        rows->setFocus();
        if (useMenu) {
            emit rows->customContextMenuRequested(rows->visualItemRect(moved).center());
            auto *menu = rows->findChild<QMenu *>(QStringLiteral("noticeMenu"));
            QVERIFY(menu && !menu->actions().isEmpty());
            menu->actions().first()->trigger();
        } else {
            QTest::keyClick(rows, Qt::Key_Return, Qt::ControlModifier);
        }
        QCOMPARE(backend->requestedPerson, QStringLiteral("19"));
    }

    void noticesOpenTypedObjectsAndContext()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto makeNotice = [](QString id, NotificationTarget target) {
            Notification notice;
            notice.id = id;
            notice.title = QStringLiteral("Событие в рабочей группе");
            notice.text = QStringLiteral("[URL=https://example.invalid/test]Текст события[/URL]");
            notice.targets = {target};
            return notice;
        };
        backend->notifications = {
            makeNotice(QStringLiteral("n-1"), {NotificationTargetKind::Chat, QStringLiteral("chat42"),
                QStringLiteral("42"), {}, QStringLiteral("Открыть сообщение"), true}),
            makeNotice(QStringLiteral("n-2"), {NotificationTargetKind::Task, QStringLiteral("501"),
                {}, QStringLiteral("101"), QStringLiteral("Открыть задачу")}),
            makeNotice(QStringLiteral("n-3"), {NotificationTargetKind::Person, QStringLiteral("19"),
                {}, {}, QStringLiteral("Анна Лебедева")}),
            makeNotice(QStringLiteral("n-4"), {NotificationTargetKind::Project, QStringLiteral("101"),
                {}, {}, QStringLiteral("Открыть проект")}),
            makeNotice(QStringLiteral("n-5"), {NotificationTargetKind::Chat, QStringLiteral("chat42"),
                {}, {}, QStringLiteral("Открыть чат"), true}),
        };
        backend->notifications.first().text += QStringLiteral(" [URL=javascript:alert(1)]Небезопасная ссылка[/URL]");
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        auto *notices = findAccessible<QListWidget>(&window, QStringLiteral("Уведомления"));
        QVERIFY(sections);
        QVERIFY(notices);
        sections->setCurrentRow(2);
        QTRY_COMPARE(notices->count(), 5);
        const auto *firstNotice = notices->item(0);
        const auto html = firstNotice->data(workspace::MarkupHtml).toString();
        QVERIFY(html.contains(QStringLiteral("href=\"https://example.invalid/test\"")));
        QVERIFY(!html.contains(QStringLiteral("href=\"javascript:")));
        QCOMPARE(firstNotice->data(workspace::MessageLinks).toStringList(),
                 QStringList{QStringLiteral("https://example.invalid/test")});
        const auto capture = qEnvironmentVariable("B24_NOTICE_OBJECT_SCREENSHOT");
        if (!capture.isEmpty())
            QVERIFY(window.grab().save(capture));
        const auto first = notices->visualItemRect(notices->item(0));
        QTest::mouseClick(notices->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(first.left() + 50, first.top() + 15));
        QTRY_VERIFY(notices->item(0)->data(workspace::NoticePresentation).value<workspace::NoticeRow>().expanded);
        const auto expanded = notices->visualItemRect(notices->item(0));
        QTest::mouseClick(notices->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(expanded.left() + 50, expanded.top() + 54));
        QTRY_COMPARE(backend->contextDialog, QStringLiteral("chat42"));
        QCOMPARE(backend->contextMessage, QStringLiteral("42"));
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Чаты задач"));
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(chats && chats->currentItem());
        QVERIFY(history);
        QCOMPARE(chats->currentItem()->data(workspace::Id).toString(), QStringLiteral("chat42"));
        Message stale;
        stale.id = QStringLiteral("98");
        emit backend->messageContextLoaded(QStringLiteral("chat42"), stale.id, {stale});
        QCOMPARE(history->count(), 0);
        Message wanted;
        wanted.id = QStringLiteral("42");
        wanted.text = QStringLiteral("Сообщение по ссылке");
        emit backend->messageContextLoaded(QStringLiteral("chat42"), wanted.id, {wanted});
        QVERIFY(history->currentItem());
        QCOMPARE(history->currentItem()->data(workspace::Id).toString(), wanted.id);
        emit backend->chatMessagesLoaded(QStringLiteral("chat42"), {stale});
        QCOMPARE(history->currentItem()->data(workspace::Id).toString(), wanted.id);
        deliverPendingTimer(window, QStringLiteral("visibleChatReadTimer"));
        QVERIFY(std::none_of(backend->reads.cbegin(), backend->reads.cend(), [](const auto &read) {
            return read.scope == QStringLiteral("chat42") && read.visible.contains(QStringLiteral("42"));
        }));

        auto activateNotice = [&](int row) {
            sections->setCurrentRow(2);
            notices->setCurrentRow(row);
            notices->setFocus();
            QTest::keyClick(notices, Qt::Key_Return, Qt::ControlModifier);
        };
        auto *searchToggle = findAccessible<QPushButton>(&window, QStringLiteral("Найти в чате"));
        auto *searchField = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск по сообщениям"));
        QVERIFY(searchToggle && searchField);
        searchToggle->click();
        searchField->setText(QStringLiteral("старый запрос"));
        QTest::keyClick(searchField, Qt::Key_Return);
        Message latest;
        latest.id = QStringLiteral("99");
        latest.text = QStringLiteral("Последнее сообщение");
        backend->linkedMessages = {latest};
        activateNotice(4);
        QTRY_COMPARE(sections->currentRow(), 0);
        QTRY_COMPARE(history->count(), 1);
        QCOMPARE(history->item(0)->data(workspace::Id).toString(), latest.id);
        emit backend->messageContextLoaded(QStringLiteral("chat42"), wanted.id, {wanted});
        QCOMPARE(history->item(0)->data(workspace::Id).toString(), latest.id);

        activateNotice(1);
        auto *task = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(task);
        auto *copyLink = findAccessible<QPushButton>(task, QStringLiteral("Скопировать ссылку"));
        QVERIFY(copyLink && copyLink->isEnabled());
        task->close();
        activateNotice(2);
        QCOMPARE(backend->requestedPerson, QStringLiteral("19"));
        auto *person = window.findChild<QDialog *>(QStringLiteral("personProfileDialog"));
        QVERIFY(person);
        QVERIFY(!person->isModal());
        emit backend->personLoaded({QStringLiteral("18"), QStringLiteral("Чужой профиль"), {}, {}});
        auto *name = findAccessible<QLabel>(person, QStringLiteral("Имя сотрудника"));
        QVERIFY(name);
        QCOMPARE(name->text(), QStringLiteral("Загружаем профиль…"));
        emit backend->personLoaded({QStringLiteral("19"), QStringLiteral("Анна Лебедева"),
                                     QStringLiteral("Инженер"), {}});
        QCOMPARE(name->text(), QStringLiteral("Анна Лебедева"));
        person->close();
        activateNotice(3);
        QCOMPARE(sections->currentRow(), 1);
        auto *projects = findAccessible<QListWidget>(&window, QStringLiteral("Проекты"));
        QVERIFY(projects && projects->currentItem());
        QCOMPARE(projects->currentItem()->data(workspace::Id).toString(), QStringLiteral("101"));
    }
};

QTEST_MAIN(UiNotificationsTest)
#include "tst_ui_notifications.moc"
