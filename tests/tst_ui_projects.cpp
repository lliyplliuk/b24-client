#include "pilot_fixture.h"

using namespace bx;
using namespace bx::test;

class UiProjectsTest : public PilotFixture {
    Q_OBJECT

private slots:
    void asynchronousAvatarRepliesPaintProjectLogosAndKanbanPeople()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdAvatarResponses = true;
        const auto asset = [](const QString &name) {
            return QUrl(QStringLiteral("https://example.bitrix24.ru/b123/resize_cache/") + name);
        };
        const auto picture = [](const QColor &color) {
            QImage image(48, 48, QImage::Format_RGB32);
            image.fill(color);
            QByteArray bytes;
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
            return bytes;
        };
        const auto logo = asset(QStringLiteral("folder.png"));
        const auto creator = asset(QStringLiteral("creator.png"));
        const auto assignee = asset(QStringLiteral("assignee.png"));
        const auto offscreen = asset(QStringLiteral("offscreen.png"));
        const QColor logoColor("#3e8268"), creatorColor("#ae4985"), assigneeColor("#416fa4");
        backend->avatarImages = {{logo, picture(logoColor)}, {creator, picture(creatorColor)},
                                 {assignee, picture(assigneeColor)}};
        backend->projects.first().avatarUrl = logo;
        backend->projects.first().avatarType = QStringLiteral("folder");
        for (int i = 0; i < 34; ++i) {
            Project project;
            project.id = QString::number(200 + i);
            project.name = QStringLiteral("Проект %1").arg(i + 1);
            project.avatarType = QStringLiteral("folder");
            if (i < 4) {
                project.avatarUrl = asset(QStringLiteral("project-%1.png").arg(i));
                backend->avatarImages.insert(project.avatarUrl, picture(logoColor));
            } else if (i == 33) {
                project.avatarUrl = offscreen;
            }
            backend->projects.append(project);
        }
        auto &stage = backend->stages.first();
        stage.tasks.first().creatorAvatarUrl = creator;
        stage.tasks.first().assigneeAvatarUrl = assignee;
        const auto stageName = stage.name;
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        findAccessible<QListWidget>(&window, QStringLiteral("Разделы"))->setCurrentRow(1);
        auto *projects = findAccessible<QListWidget>(&window, QStringLiteral("Проекты"));
        auto *cards = findAccessible<QListWidget>(&window, stageName);
        QVERIFY(projects && cards && cards->count() > 0);
        QTRY_COMPARE(backend->avatarRequests.size(), 4);
        QVERIFY(!backend->avatarRequests.contains(offscreen));
        QVERIFY(backend->avatarRequests.contains(logo));
        QVERIFY(!backend->avatarRequests.contains(creator));
        QVERIFY(!backend->avatarRequests.contains(assignee));
        for (const auto &url : backend->avatarRequests) {
            QVERIFY(backend->avatarImages.contains(url));
            emit backend->avatarLoaded(url, url == logo ? QByteArray() : backend->avatarImages.value(url));
        }
        QTRY_COMPARE(backend->avatarRequests.size(), 7);
        const auto finalBatch = backend->avatarRequests.mid(4);
        for (const auto &url : finalBatch) {
            QVERIFY(backend->avatarImages.contains(url));
            emit backend->avatarLoaded(url, url == creator ? QByteArray() : backend->avatarImages.value(url));
        }
        QCoreApplication::processEvents();
        QCOMPARE(backend->avatarRequests.count(logo), 1);
        QCOMPARE(backend->avatarRequests.count(creator), 1);
        QCOMPARE(backend->avatarRequests.count(assignee), 1);
        QVERIFY(!backend->avatarRequests.contains(offscreen));

        const auto containsPaint = [](QListWidget *list, const QColor &color) {
            const auto image = list->viewport()->grab().toImage();
            const auto pixels = list->visualItemRect(list->item(0)).intersected(image.rect());
            for (int y = pixels.top(); y <= pixels.bottom(); ++y)
                for (int x = pixels.left(); x <= pixels.right(); ++x)
                    if (image.pixelColor(x, y) == color)
                        return true;
            return false;
        };
        QVERIFY(containsPaint(cards, assigneeColor));
        QVERIFY(!containsPaint(projects, logoColor));
        QVERIFY(!containsPaint(cards, creatorColor));

        QTest::keyClick(&window, Qt::Key_F5);
        QTRY_COMPARE_WITH_TIMEOUT(backend->avatarRequests.size(), 9, 1200);
        QCOMPARE(backend->avatarRequests.count(logo), 2);
        QCOMPARE(backend->avatarRequests.count(creator), 2);
        QCOMPARE(backend->avatarRequests.count(assignee), 1);
        QVERIFY(!backend->avatarRequests.contains(offscreen));
        emit backend->avatarLoaded(logo, backend->avatarImages.value(logo));
        emit backend->avatarLoaded(creator, backend->avatarImages.value(creator));
        cards = findAccessible<QListWidget>(&window, stageName);
        QVERIFY(cards);
        QVERIFY(containsPaint(projects, logoColor));
        QVERIFY(containsPaint(cards, creatorColor));
        QVERIFY(containsPaint(cards, assigneeColor));
    }

    void kanbanColumnEdgesResizeIndependentlyAndPersist()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        const auto account = QStringLiteral("fixture/kanban-widths");
        backend->accountScope = account;
        auto first = backend->stages.first();
        first.name = QStringLiteral("Первая колонка");
        auto task = first.tasks.first();
        first.tasks.clear();
        for (int i = 0; i < 20; ++i) {
            task.id = QString::number(650 + i);
            first.tasks.append(task);
        }
        backend->stages = {first};
        for (int i = 1; i <= 5; ++i) {
            Stage stage;
            stage.id = QString::number(50 + i);
            stage.name = QStringLiteral("Этап %1").arg(i);
            stage.tasks = {task};
            backend->stages.append(stage);
        }
        Project another;
        another.id = QStringLiteral("102");
        another.name = QStringLiteral("Другой проект");
        backend->projects.append(another);

        const auto moveGrip = [](QWidget *grip, const QPoint &global, int delta, Qt::MouseButtons buttons) {
            const auto point = global + QPoint(delta, 0);
            QMouseEvent move(QEvent::MouseMove, grip->mapFromGlobal(point), point,
                             Qt::NoButton, buttons, Qt::NoModifier);
            QApplication::sendEvent(grip, &move);
        };
        const auto drag = [&](QWidget *grip, int delta) {
            const auto local = grip->rect().center();
            const auto global = grip->mapToGlobal(local);
            QTest::mousePress(grip, Qt::LeftButton, Qt::NoModifier, local);
            moveGrip(grip, global, delta, Qt::LeftButton);
            QTest::mouseRelease(grip, Qt::LeftButton, Qt::NoModifier, local);
        };

        {
            MainWindow window(backend.get());
            window.resize(960, 650);
            window.show();
            QApplication::setActiveWindow(&window);
            auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
            QVERIFY(sections);
            sections->setCurrentRow(1);
            auto *board = window.findChild<BoardWidget *>();
            QVERIFY(board);
            auto *scroll = board->findChild<QScrollArea *>();
            auto *firstList = findAccessible<QListWidget>(board, first.name);
            auto *nextList = findAccessible<QListWidget>(board, backend->stages.at(1).name);
            QVERIFY(scroll && firstList && nextList);
            firstList->setCurrentRow(3);
            const auto *selected = firstList->currentItem();
            const auto *firstColumn = firstList->parentWidget();
            const auto *nextColumn = nextList->parentWidget();
            QTRY_COMPARE(firstColumn->width(), 232);
            QCOMPARE(nextColumn->width(), 232);
            QTRY_COMPARE(nextColumn->x() - firstColumn->x(), 244);
            auto *grip = firstColumn->findChild<QWidget *>(QStringLiteral("kanbanColumnGrip"));
            QVERIFY(grip && grip->isVisible());
            QVERIFY(grip->geometry().left() > firstList->geometry().right());
            QVERIFY(!findAccessible<QComboBox>(&window, QStringLiteral("Количество видимых колонок канбана")));
            const int loaded = backend->boardRequests;
            drag(grip, 90);
            QTRY_COMPARE(firstColumn->width(), 322);
            QCOMPARE(nextColumn->width(), 232);
            QCOMPARE(firstList->currentItem(), selected);
            QCOMPARE(backend->boardRequests, loaded);

            const auto local = grip->rect().center();
            const auto global = grip->mapToGlobal(local);
            QTest::mousePress(grip, Qt::LeftButton, Qt::NoModifier, local);
            moveGrip(grip, global, -60, Qt::LeftButton);
            QCOMPARE(firstColumn->width(), 262);
            moveGrip(grip, global, -80, Qt::NoButton);
            QCOMPARE(firstColumn->width(), 322);
            QTest::mouseRelease(grip, Qt::LeftButton, Qt::NoModifier, local);
            QTest::keyClick(grip, Qt::Key_Right);
            QCOMPARE(firstColumn->width(), 338);
            QTest::keyClick(grip, Qt::Key_Left);
            QCOMPARE(firstColumn->width(), 322);

            window.resize(720, 500);
            QTRY_COMPARE(window.width(), 720);
            QCOMPARE(firstColumn->width(), 322);
            QCOMPARE(nextColumn->width(), 232);
            scroll->horizontalScrollBar()->setValue(scroll->horizontalScrollBar()->maximum());
            auto *last = findAccessible<QListWidget>(board, backend->stages.last().name);
            QVERIFY(last);
            auto *lastGrip = last->parentWidget()->findChild<QWidget *>(QStringLiteral("kanbanColumnGrip"));
            QVERIFY(lastGrip);
            QTRY_VERIFY(lastGrip->mapTo(scroll->viewport(), QPoint()).x() + lastGrip->width() <= scroll->viewport()->width());
            drag(lastGrip, -40);
            QTRY_COMPARE(last->parentWidget()->width(), 192);
            drag(lastGrip, 72);
            QTRY_COMPARE(last->parentWidget()->width(), 264);
            QCOMPARE(firstColumn->width(), 322);
            const auto widthOfFirst = [&] {
                auto *list = findAccessible<QListWidget>(board, first.name);
                return list ? list->parentWidget()->width() : -1;
            };

            QTest::keyClick(&window, Qt::Key_F5);
            QTRY_VERIFY(backend->boardRequests > loaded);
            QTRY_COMPARE(widthOfFirst(), 322);
            auto *filter = findAccessible<QLineEdit>(&window, QStringLiteral("Фильтр задач"));
            QVERIFY(filter);
            filter->setText(QStringLiteral("нет совпадений"));
            QTRY_COMPARE(widthOfFirst(), 322);
            filter->clear();
            auto *projects = findAccessible<QListWidget>(&window, QStringLiteral("Проекты"));
            QVERIFY(projects && projects->count() == 2);
            projects->setCurrentRow(1);
            emit backend->boardLoaded(another.id, backend->stages);
            QTRY_COMPARE(widthOfFirst(), 232);
            projects->setCurrentRow(0);
            QTRY_COMPARE(widthOfFirst(), 322);

            backend->accountScope = QStringLiteral("fixture/another-user");
            emit backend->accountScopeChanged();
            QTRY_COMPARE(widthOfFirst(), 232);
        }

        backend->accountScope = account;
        MainWindow reopened(backend.get());
        reopened.resize(960, 650);
        reopened.show();
        auto *sections = findAccessible<QListWidget>(&reopened, QStringLiteral("Разделы"));
        QVERIFY(sections);
        sections->setCurrentRow(1);
        auto *firstList = findAccessible<QListWidget>(&reopened, first.name);
        QVERIFY(firstList);
        QTRY_COMPARE(firstList->parentWidget()->width(), 322);
    }

    void kanbanCardsWrapAndLoadOnlyVisiblePeople()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        const QUrl creator(QStringLiteral("https://example.bitrix24.ru/upload/creator.png"));
        const QUrl assignee(QStringLiteral("https://example.bitrix24.ru/upload/assignee.png"));
        const QUrl lower(QStringLiteral("https://example.bitrix24.ru/upload/lower.png"));
        const QUrl later(QStringLiteral("https://example.bitrix24.ru/upload/later.png"));
        const QColor creatorColor("#bd4380");
        const QColor assigneeColor("#336cab");
        auto image = [](const QColor &color) {
            QImage pixels(32, 32, QImage::Format_RGB32);
            pixels.fill(color);
            QByteArray bytes;
            QBuffer output(&bytes);
            output.open(QIODevice::WriteOnly);
            pixels.save(&output, "PNG");
            return bytes;
        };
        backend->avatarImages = {{creator, image(creatorColor)}, {assignee, image(assigneeColor)}};
        auto stage = backend->stages.first();
        stage.name = QStringLiteral("В работе");
        auto card = stage.tasks.first();
        card.name = QStringLiteral("Сверить статусы возвратов СверхдлинныйИдентификаторНакладной");
        card.createdBy = QStringLiteral("Мария Котова");
        card.assignee = QStringLiteral("Игорь Смирнов");
        card.tags = {QStringLiteral("Техподдержка"), QStringLiteral("Логистика"), QStringLiteral("Возвраты")};
        card.creatorAvatarUrl = creator;
        card.assigneeAvatarUrl = assignee;
        card.due = QStringLiteral("23 ноября, 09:00");
        stage.tasks = {card};
        Task shortCard = card;
        shortCard.name = QStringLiteral("Сверить статусы");
        shortCard.tags.clear();
        shortCard.due.clear();
        for (int i = 0; i < 14; ++i) {
            shortCard.id = QString::number(720 + i);
            stage.tasks.append(shortCard);
        }
        stage.tasks.last().creatorAvatarUrl = lower;
        stage.taskCount = stage.tasks.size();
        backend->stages = {stage};
        for (int i = 1; i <= 6; ++i) {
            auto next = stage;
            next.id = QString::number(40 + i);
            next.name = QStringLiteral("Этап %1").arg(i);
            next.tasks = {shortCard};
            next.taskCount = next.tasks.size();
            if (i == 6)
                next.tasks.first().creatorAvatarUrl = later;
            backend->stages.append(next);
        }

        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        findAccessible<QListWidget>(&window, QStringLiteral("Разделы"))->setCurrentRow(1);
        auto *board = window.findChild<BoardWidget *>();
        QVERIFY(board);
        auto *column = findAccessible<QListWidget>(board, stage.name);
        QVERIFY(column && column->count() >= 2);
        const auto *first = column->item(0);
        QTRY_VERIFY_WITH_TIMEOUT(column->visualItemRect(first).height() > 120, 1200);
        QCOMPARE(first->data(workspace::Title).toString(), card.name);
        QVERIFY(first->toolTip().contains(QStringLiteral("Постановщик: Мария Котова")));
        QVERIFY(first->toolTip().contains(QStringLiteral("Исполнитель: Игорь Смирнов")));
        QVERIFY(first->data(Qt::AccessibleDescriptionRole).toString().contains(QStringLiteral("Логистика")));
        QTRY_COMPARE(backend->avatarRequests.count(creator), 1);
        QTRY_COMPARE(backend->avatarRequests.count(assignee), 1);
        QVERIFY(!backend->avatarRequests.contains(lower));
        QVERIFY(!backend->avatarRequests.contains(later));
        const auto wide = qEnvironmentVariable("B24_KANBAN_WIDE_SCREENSHOT");
        if (!wide.isEmpty())
            QVERIFY(window.grab().save(wide));

        window.resize(720, 500);
        QCoreApplication::processEvents();
        QTRY_COMPARE_WITH_TIMEOUT(column->visualItemRect(first).height(), column->sizeHintForRow(0), 1000);
        const auto rect = column->visualItemRect(first);
        QVERIFY(rect.height() > column->visualItemRect(column->item(1)).height());
        QStyleOptionViewItem option;
        option.initFrom(column->viewport());
        option.rect = QRect(QPoint(), rect.size());
        QImage snapshot(option.rect.size(), QImage::Format_ARGB32_Premultiplied);
        snapshot.fill(Qt::transparent);
        {
            QPainter painter(&snapshot);
            column->itemDelegate()->paint(&painter, option, column->model()->index(0, 0));
        }
        const auto boundsOf = [&snapshot](const QColor &color) {
            QRect bounds;
            for (int y = 0; y < snapshot.height(); ++y)
                for (int x = 0; x < snapshot.width(); ++x)
                    if (snapshot.pixelColor(x, y) == color)
                        bounds = bounds.united(QRect(x, y, 1, 1));
            return bounds;
        };
        const auto creatorBounds = boundsOf(creatorColor);
        const auto assigneeBounds = boundsOf(assigneeColor);
        QVERIFY(creatorBounds.height() >= 18);
        QVERIFY(assigneeBounds.height() >= 18);
        QVERIFY(creatorBounds.right() < assigneeBounds.left());
        const auto compact = qEnvironmentVariable("B24_KANBAN_COMPACT_SCREENSHOT");
        if (!compact.isEmpty())
            QVERIFY(window.grab().save(compact));

        column->scrollToBottom();
        QTRY_COMPARE(backend->avatarRequests.count(lower), 1);
        auto *scroll = board->findChild<QScrollArea *>();
        QVERIFY(scroll && scroll->horizontalScrollBar()->maximum() > 0);
        scroll->horizontalScrollBar()->setValue(scroll->horizontalScrollBar()->maximum());
        QTRY_COMPARE(backend->avatarRequests.count(later), 1);

        board->setView(BoardWidget::View::List);
        auto *list = findAccessible<QListWidget>(board, QStringLiteral("Список задач"));
        QVERIFY(list && list->isVisible());
        QCOMPARE(list->item(0)->text().section(QLatin1Char('\n'), 0, 0), card.name);
    }

    void boardOpensTaskAndTaskChat()
    {
        DemoBackend demo;
        demo.start();
        QPointer<MainWindow> window = new MainWindow(&demo);
        window->setAttribute(Qt::WA_DeleteOnClose);
        window->resize(960, 650);
        window->show();
        QApplication::setActiveWindow(window);

        auto *sections = findAccessible<QListWidget>(window, QStringLiteral("Разделы"));
        auto *projects = findAccessible<QListWidget>(window, QStringLiteral("Проекты"));
        QVERIFY(sections);
        QVERIFY(projects);
        QTest::keyClick(window, Qt::Key_2, Qt::AltModifier);
        QTRY_COMPARE(sections->currentRow(), 1);
        QTRY_VERIFY(projects->currentItem());

        auto *board = window->findChild<BoardWidget *>();
        QVERIFY(board);
        // Доска приходит асинхронно: ждём колонки этапов (в списке задач есть всегда).
        QTRY_VERIFY(board->findChildren<QListWidget *>().size() > 1);
        QListWidget *work = nullptr;
        QTRY_VERIFY((work = findAccessible<QListWidget>(board, QStringLiteral("В работе"))));
        QVERIFY(work->count() > 0);
        work->setFocus();
        QTest::keyClick(work, Qt::Key_Down);
        QTest::keyClick(work, Qt::Key_Return);

        const auto checkTask = [&window](const QString &name) {
            auto *card = window->findChild<QDialog *>(QStringLiteral("taskCardDialog"));
            QVERIFY(card);
            QVERIFY(card->isVisible());
            QCOMPARE(card->windowModality(), Qt::WindowModal);
            auto *title = findAccessible<QLineEdit>(card, QStringLiteral("Заголовок задачи"));
            auto *messages = findAccessible<QListWidget>(card, QStringLiteral("Чат карточки"));
            QVERIFY(title);
            QVERIFY(messages);
            QTRY_COMPARE(title->text(), name);
            QTRY_COMPARE(messages->count(), 2);
        };
        checkTask(QStringLiteral("Собрать тестовый стенд"));
        QPointer<QDialog> card = window->findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        const auto modalPath = qEnvironmentVariable("B24_TASK_SCREENSHOT");
        if (!modalPath.isEmpty())
            QVERIFY(card->grab().save(modalPath));
        QTest::keyClick(card, Qt::Key_Escape);
        QTRY_VERIFY(card.isNull());

        const auto screenshotPath = qEnvironmentVariable("B24_SMOKE_SCREENSHOT");
        if (!screenshotPath.isEmpty())
            QVERIFY(window->grab().save(screenshotPath));
        const auto compactPath = qEnvironmentVariable("B24_COMPACT_SCREENSHOT");
        if (!compactPath.isEmpty()) {
            window->resize(720, 500);
            QCoreApplication::processEvents();
            QVERIFY(window->grab().save(compactPath));
        }
        for (const auto &stage : {QStringLiteral("К выполнению"), QStringLiteral("Готово")}) {
            auto *column = findAccessible<QListWidget>(board, stage);
            QVERIFY(column);
            const auto expected = column->item(0)->text().section(QLatin1Char('\n'), 0, 0);
            column->setCurrentRow(0);
            QTest::keyClick(column, Qt::Key_Return);
            checkTask(expected);
            card = window->findChild<QDialog *>(QStringLiteral("taskCardDialog"));
            QTest::keyClick(card, Qt::Key_Escape);
            QTRY_VERIFY(card.isNull());
        }

        bool ok = false;
        const int sampleMs = qEnvironmentVariableIntValue("B24_SMOKE_SAMPLE_MS", &ok);
        const bool sample = ok && sampleMs > 0 && sampleMs <= 10000;
        if (sample) {
            qInfo("B24_UI_OPEN");
            // Keep the requested sampling window open for the external smoke profiler.
            QTest::qWait(sampleMs);
        }
        window->close();
        QTRY_VERIFY(window.isNull());
        if (sample) {
            qInfo("B24_UI_CLOSED");
            // Keep the requested sampling window open for the external smoke profiler.
            QTest::qWait(sampleMs);
        }
        demo.stop();
    }

    void showsProjectToolsAndDistinguishesFolderDenial()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->projectToolsFixture = true;
        MainWindow window(backend.get());
        window.resize(1020, 690);
        window.show();
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        sections->setCurrentRow(1);
        auto *open = findAccessible<QPushButton>(&window, QStringLiteral("Участники и файлы проекта"));
        QVERIFY(open && open->isEnabled());
        open->click();
        auto *tools = findAccessible<QDialog>(&window, QStringLiteral("Инструменты проекта"));
        QVERIFY(tools);
        auto *members = findAccessible<QListWidget>(tools, QStringLiteral("Список участников проекта"));
        QVERIFY(members);
        QCOMPARE(members->count(), 1);
        QCOMPARE(members->item(0)->data(workspace::Detail).toString(), QStringLiteral("Владелец"));
        emit backend->projectMembersLoaded(QStringLiteral("202"), {});
        QCOMPARE(members->item(0)->data(workspace::Title).toString(), QStringLiteral("Анна"));

        auto *tabs = tools->findChild<QTabWidget *>();
        QVERIFY(tabs);
        tabs->setCurrentIndex(1);
        auto *files = findAccessible<QListWidget>(tools, QStringLiteral("Список файлов проекта"));
        QVERIFY(files);
        QCOMPARE(files->item(0)->data(workspace::Title).toString(), QStringLiteral("Общие материалы"));
        files->setCurrentRow(0);
        emit files->itemActivated(files->item(0));
        QCOMPARE(files->item(0)->data(workspace::Detail).toString(), QStringLiteral("Файлы недоступны"));
        auto *status = findAccessible<QLabel>(tools, QStringLiteral("Состояние инструментов проекта"));
        QVERIFY(status);
        QCOMPARE(status->text(), QStringLiteral("Доступ запрещён"));
    }

    void movesTaskToAnotherColumn()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->stages[1].color = QStringLiteral("22cc99");
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        backend->holdTaskUpdate = true;
        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        auto *stageRequisite = findAccessible<QPushButton>(card, QStringLiteral("Этап"));
        QVERIFY(stageRequisite && stageRequisite->isEnabled());
        stageRequisite->click();
        auto *stage = findAccessible<QListWidget>(card, QStringLiteral("Этап задачи"));
        QVERIFY(stage && stage->isVisible());
        QCOMPARE(stage->count(), 2);
        auto *popup = card->findChild<QFrame *>(QStringLiteral("taskRequisitePopup"));
        QVERIFY(popup && popup->isVisible() && popup->isAncestorOf(stage));
        QCOMPARE(stage->item(0)->checkState(), Qt::Checked);
        QVERIFY(!stage->item(1)->icon().isNull());
        if (const auto path = qEnvironmentVariable("B24_REQUISITE_POPUP_DIR"); !path.isEmpty()) {
            QCoreApplication::processEvents();
            QVERIFY(card->grab().save(path + QStringLiteral("/stage-popup.png")));
        }
        stage->setCurrentRow(1);
        auto *move = findAccessible<QPushButton>(card, QStringLiteral("Повторить перенос задачи"));
        QVERIFY(move && !move->isVisible());
        QVERIFY(backend->updatedTask.isEmpty());
        QTest::keyClick(stage, Qt::Key_Return);
        QCOMPARE(backend->updatedTask, QStringLiteral("501"));
        QCOMPARE(backend->lastUpdate.stageId, std::optional<QString>(QStringLiteral("602")));
        QVERIFY(backend->movedTask.isEmpty());
        QVERIFY(!move->isEnabled());
        emit backend->failed(QStringLiteral("update-task:501"), QStringLiteral("Портал отклонил перенос"));
        QVERIFY(move->isEnabled());
    }

    void kanbanMoveWaitsForServerAndRejectsUnavailableStages()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->stages.append({QStringLiteral("search-only"), QStringLiteral("Без этапа"), {}});
        backend->stages.append({QStringLiteral("0"), QStringLiteral("Этап не назначен"), {}});
        MainWindow window(backend.get());
        window.resize(1060, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        auto *board = window.findChild<BoardWidget *>();
        auto *status = window.findChild<QLabel *>(QStringLiteral("status"));
        QVERIFY(board && status);
        const auto request = [board](const QString &from, const QString &to) {
            return QMetaObject::invokeMethod(board, "taskMoveRequested", Qt::DirectConnection,
                Q_ARG(QString, QStringLiteral("501")), Q_ARG(QString, from), Q_ARG(QString, to));
        };
        const auto hasTask = [board](const QString &stage) {
            const auto *list = findAccessible<QListWidget>(board, stage);
            return list && list->count() && list->item(0)->data(workspace::Id).toString() == QStringLiteral("501");
        };
        QVERIFY(hasTask(QStringLiteral("В работе")));
        QVERIFY(request(QStringLiteral("601"), QStringLiteral("601")));
        QVERIFY(request(QStringLiteral("601"), QStringLiteral("search-only")));
        QVERIFY(request(QStringLiteral("601"), QStringLiteral("0")));
        QVERIFY(request(QStringLiteral("602"), QStringLiteral("601")));
        QVERIFY(backend->movedTask.isEmpty());
        QVERIFY(request(QStringLiteral("601"), QStringLiteral("602")));
        QCOMPARE(backend->movedProject, QStringLiteral("101"));
        QCOMPARE(backend->movedTask, QStringLiteral("501"));
        QCOMPARE(backend->movedStage, QStringLiteral("602"));
        QCOMPARE(status->text(), QStringLiteral("Переносим задачу…"));
        QVERIFY(hasTask(QStringLiteral("В работе")));
        QVERIFY(!hasTask(QStringLiteral("Готово")));
        backend->movedTask.clear();
        QVERIFY(request(QStringLiteral("601"), QStringLiteral("602")));
        QVERIFY(backend->movedTask.isEmpty());
        emit backend->failed(QStringLiteral("board-edit:101"), QStringLiteral("Портал отклонил перенос"));
        QCOMPARE(status->text(), QStringLiteral("Портал отклонил перенос"));
        QVERIFY(hasTask(QStringLiteral("В работе")));
        QVERIFY(request(QStringLiteral("601"), QStringLiteral("602")));
        QCOMPARE(backend->movedTask, QStringLiteral("501"));
        backend->stages[1].tasks.append(backend->stages[0].tasks.takeFirst());
        backend->stages[1].tasks.first().stageId = QStringLiteral("602");
        backend->holdBoardRead = true;
        const auto reloads = backend->boardRequests;
        emit backend->boardUpdated(QStringLiteral("101"));
        QCOMPARE(backend->boardRequests, reloads + 1);
        QCOMPARE(status->text(), QStringLiteral("Задача перенесена"));
        backend->movedTask.clear();
        QVERIFY(request(QStringLiteral("601"), QStringLiteral("602")));
        QVERIFY(backend->movedTask.isEmpty());
        emit backend->failed(QStringLiteral("board:101"), QStringLiteral("Временный сбой"));
        QCOMPARE(status->text(), QStringLiteral("Не удалось обновить доску после переноса. Нажмите F5"));
        QVERIFY(hasTask(QStringLiteral("В работе")));
        backend->holdBoardRead = false;
        QTest::keyClick(&window, Qt::Key_F5);
        QCOMPARE(backend->boardRequests, reloads + 2);
        QCOMPARE(status->text(), QStringLiteral("Задача перенесена"));
        QVERIFY(hasTask(QStringLiteral("Готово")));
        QVERIFY(!hasTask(QStringLiteral("В работе")));
        backend->movedTask.clear();
        emit backend->connectionChanged(false);
        QVERIFY(request(QStringLiteral("602"), QStringLiteral("601")));
        QVERIFY(backend->movedTask.isEmpty());
    }

    void kanbanNativeDragMovesOnlyBetweenRealColumns()
    {
        const auto pointerTool = QStandardPaths::findExecutable(QStringLiteral("xdotool"));
        if (QGuiApplication::platformName() != QLatin1String("xcb") || pointerTool.isEmpty())
            QSKIP("Native pointer test requires X11 and xdotool");

        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->stages.append({QStringLiteral("synthetic"), QStringLiteral("Без этапа"), {}});
        MainWindow window(backend.get());
        window.resize(1080, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        auto *board = window.findChild<BoardWidget *>();
        auto *source = findAccessible<QListWidget>(board, QStringLiteral("В работе"));
        auto *target = findAccessible<QListWidget>(board, QStringLiteral("Готово"));
        auto *synthetic = findAccessible<QListWidget>(board, QStringLiteral("Без этапа"));
        QVERIFY(source && target && synthetic);
        QVERIFY(target->isVisible());
        const auto from = source->viewport()->mapToGlobal(source->visualItemRect(source->item(0)).center());
        const auto targetArea = target->viewport()->rect().intersected(target->viewport()->visibleRegion().boundingRect());
        QVERIFY(!targetArea.isEmpty());
        const auto drag = [&](const QPoint &to, const QString &button = QStringLiteral("1")) {
            QProcess pointer;
            pointer.start(pointerTool, {QStringLiteral("mousemove"), QString::number(from.x()), QString::number(from.y()),
                QStringLiteral("mousedown"), button,
                QStringLiteral("sleep"), QStringLiteral("0.13"),
                QStringLiteral("mousemove"), QString::number(from.x() + 26), QString::number(from.y()),
                QStringLiteral("sleep"), QStringLiteral("0.13"),
                QStringLiteral("mousemove"), QString::number(to.x()), QString::number(to.y()),
                QStringLiteral("sleep"), QStringLiteral("0.13"),
                QStringLiteral("mouseup"), button});
            QVERIFY(pointer.waitForStarted());
            QTRY_COMPARE_WITH_TIMEOUT(pointer.state(), QProcess::NotRunning, 3000);
            QCOMPARE(pointer.exitCode(), 0);
        };
        const auto to = target->viewport()->mapToGlobal(targetArea.center());
        drag(to, QStringLiteral("2"));
        QVERIFY2(backend->movedTask.isEmpty(), "middle-button gesture started a drag");
        drag(from + QPoint(20, 25));
        QVERIFY2(backend->movedTask.isEmpty(), "same-column drop requested a move");
        const auto invalidArea = synthetic->viewport()->visibleRegion().boundingRect();
        QVERIFY(!invalidArea.isEmpty());
        drag(synthetic->viewport()->mapToGlobal(invalidArea.center()));
        QVERIFY2(backend->movedTask.isEmpty(), "synthetic stage accepted a drop");
        QVERIFY(!window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        drag(to);
        QTRY_COMPARE(backend->movedTask, QStringLiteral("501"));
        QCOMPARE(backend->movedStage, QStringLiteral("602"));
        QVERIFY(!window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
    }

    void listViewShowsFilteredTasks()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);

        auto *listView = findAccessible<QPushButton>(&window, QStringLiteral("Вид Список"));
        QVERIFY(listView);
        listView->click();
        auto *list = findAccessible<QListWidget>(&window, QStringLiteral("Список задач"));
        QVERIFY(list);
        QTRY_COMPARE(list->count(), 1);
        QVERIFY(list->item(0)->text().contains(QStringLiteral("Починить кран")));

        auto *filter = findAccessible<QLineEdit>(&window, QStringLiteral("Фильтр задач"));
        QVERIFY(filter);
        filter->setText(QStringLiteral("№501"));
        QTRY_COMPARE(list->count(), 1);
        filter->setText(QStringLiteral("несуществующая"));
        QTRY_COMPARE(list->count(), 1);
        auto *searchStatus = findAccessible<QLabel>(&window, QStringLiteral("Состояние поиска задач"));
        QVERIFY(searchStatus && searchStatus->isVisible());
        QCOMPARE(searchStatus->text(), QStringLiteral("Ищем задачи в проекте…"));
        QVERIFY(!list->item(0)->text().contains(QStringLiteral("Ничего не найдено")));

        filter->setText(QStringLiteral("Архив"));
        QTest::keyClick(filter, Qt::Key_Return);
        QCOMPARE(backend->searchedProject, QStringLiteral("101"));
        QCOMPARE(backend->searchedTaskQuery, QStringLiteral("Архив"));
        auto remote = backend->stages.first().tasks.first();
        remote.id = QStringLiteral("799");
        remote.name = QStringLiteral("Проверить архивную задачу");
        emit backend->projectTasksFound(QStringLiteral("101"), QStringLiteral("Архив"), QVector<Task>{remote});
        QVERIFY(searchStatus->isHidden());
        QCOMPARE(list->item(0)->data(workspace::Id).toString(), remote.id);
        emit backend->boardLoaded(QStringLiteral("101"), backend->stages);
        QCOMPARE(list->item(0)->data(workspace::Id).toString(), remote.id);
        filter->clear();
        QCOMPARE(list->item(0)->data(workspace::Id).toString(), QStringLiteral("501"));
    }

    void projectSearchResumesAfterLeavingTheSection()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        auto *sections = window.findChild<QListWidget *>(QStringLiteral("sections"));
        auto *filter = findAccessible<QLineEdit>(&window, QStringLiteral("Фильтр задач"));
        auto *searchStatus = findAccessible<QLabel>(&window, QStringLiteral("Состояние поиска задач"));
        QVERIFY(sections && filter && searchStatus);
        sections->setCurrentRow(1);
        filter->setText(QStringLiteral("Архив"));
        QVERIFY(searchStatus->isVisible());

        sections->setCurrentRow(0);
        QVERIFY(searchStatus->isHidden());
        QVERIFY(backend->searchedTaskQuery.isEmpty());
        sections->setCurrentRow(1);
        QTRY_COMPARE(backend->searchedTaskQuery, QStringLiteral("Архив"));
        QVERIFY(searchStatus->isVisible());

        sections->setCurrentRow(0);
        QVERIFY(searchStatus->isHidden());
        emit backend->projectTasksFound(QStringLiteral("101"), QStringLiteral("Архив"), {});
        backend->searchedTaskQuery.clear();
        sections->setCurrentRow(1);
        QTRY_COMPARE(backend->searchedTaskQuery, QStringLiteral("Архив"));
        emit backend->projectTasksFound(QStringLiteral("101"), QStringLiteral("Архив"), {});
        QVERIFY(searchStatus->isHidden());
    }

    void ganttFiltersWithoutServerSearchAndRefreshesAfterTaskCreation()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        Task first = backend->stages.first().tasks.first();
        first.planStart = QDateTime(QDate(2026, 11, 21), QTime(9, 0));
        first.planEnd = first.planStart.addDays(2);
        Task second = first;
        second.id = QStringLiteral("502");
        second.name = QStringLiteral("Проверить склад");
        backend->timelineTasks = {first, second};
        backend->timelineLimited = true;
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        auto *gantt = findAccessible<QPushButton>(&window, QStringLiteral("Вид Гант"));
        auto *filter = findAccessible<QLineEdit>(&window, QStringLiteral("Фильтр задач"));
        auto *timeline = findAccessible<QListWidget>(&window, QStringLiteral("Диаграмма Ганта проекта"));
        auto *range = findAccessible<QLabel>(&window, QStringLiteral("Период плановых дат"));
        QVERIFY(gantt && filter && timeline && range);
        gantt->click();
        QVERIFY(filter->isVisible());
        QTRY_COMPARE(timeline->count(), 2);
        filter->setText(QStringLiteral("№502"));
        QTest::keyClick(filter, Qt::Key_Return);
        QCOMPARE(timeline->count(), 1);
        QCOMPARE(timeline->item(0)->data(workspace::Id).toString(), second.id);
        QVERIFY(backend->searchedTaskQuery.isEmpty());
        filter->setText(QStringLiteral("архив"));
        QCOMPARE(timeline->count(), 0);
        QVERIFY(range->text().contains(QStringLiteral("В загруженных задачах")));

        filter->clear();
        const auto before = backend->timelineRequests.size();
        auto third = second;
        third.name = QStringLiteral("Уточнить сроки");
        third.id = QStringLiteral("503");
        backend->timelineTasks.append(third);
        emit backend->taskCreated(QStringLiteral("101"), third.id);
        QCOMPARE(backend->timelineRequests.size(), before + 1);
        QCOMPARE(timeline->count(), 3);
    }

    void boardLoadsNextPageOnlyOnUserScrollAtTwoThirds()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        const auto sample = backend->stages.first().tasks.first();
        const auto firstStage = backend->stages.first().id;
        const auto project = QStringLiteral("101");
        const auto tasks = [&sample](int first, int count) {
            QVector<Task> result;
            for (int i = 0; i < count; ++i) {
                auto task = sample;
                task.id = QString::number(first + i);
                result.append(task);
            }
            return result;
        };
        backend->stages.first().tasks = tasks(500, 24);
        backend->stages.first().taskCount = 79;
        backend->stages.first().hasMoreTasks = true;
        backend->stages[1].hasMoreTasks = true;
        for (int i = 3; i <= 7; ++i)
            backend->stages.append({QString::number(600 + i), QStringLiteral("Этап %1").arg(i), {}});
        backend->stages.last().hasMoreTasks = true;

        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        const auto scrollDown = [](QListWidget *list, QPoint angle = QPoint(0, -120), QPoint pixels = {},
                                   Qt::ScrollPhase phase = Qt::NoScrollPhase) {
            auto *viewport = list->viewport();
            const auto point = viewport->rect().center();
            QWheelEvent wheel(point, viewport->mapToGlobal(point), pixels, angle,
                              Qt::NoButton, Qt::NoModifier, phase, false);
            QApplication::sendEvent(viewport, &wheel);
        };
        const auto loadingVisible = [](const QLabel *label) {
            const auto *indicator = label->findChild<QWidget *>(QStringLiteral("sharedLoadingIndicator"));
            return indicator && indicator->isVisible();
        };
        auto *board = window.findChild<BoardWidget *>();
        QVERIFY(board);
        QPointer<QListWidget> column = findAccessible<QListWidget>(board, QStringLiteral("В работе"));
        QTRY_VERIFY(column && column->verticalScrollBar()->maximum() > 100);
        QVERIFY(!board->findChild<QPushButton *>(QStringLiteral("moreStageTasks")));
        QCOMPARE(column->count(), 24);
        QCoreApplication::sendPostedEvents(board, QEvent::MetaCall);
        QVERIFY(backend->boardPageRequests.isEmpty());

        auto *scrollbar = column->verticalScrollBar();
        scrollDown(column);
        QCoreApplication::sendPostedEvents(board, QEvent::MetaCall);
        QVERIFY(backend->boardPageRequests.isEmpty());
        scrollbar->setValue(scrollbar->maximum());
        QCoreApplication::sendPostedEvents(board, QEvent::MetaCall);
        QVERIFY(backend->boardPageRequests.isEmpty());
        scrollbar->setValue(0);
        scrollbar->setSliderPosition(scrollbar->maximum());
        QCoreApplication::sendPostedEvents(board, QEvent::MetaCall);
        QVERIFY(backend->boardPageRequests.isEmpty());
        scrollDown(column, QPoint(120, -1));
        scrollDown(column, QPoint(120, -1), QPoint(120, -1));
        QCoreApplication::sendPostedEvents(board, QEvent::MetaCall);
        QVERIFY(backend->boardPageRequests.isEmpty());
        const int threshold = (2 * scrollbar->maximum() + 2) / 3;
        scrollbar->setValue(threshold - 1);
        scrollDown(column, QPoint(0, -120), {}, Qt::ScrollMomentum);
        QTRY_COMPARE(backend->boardPageRequests.size(), 1);
        QCOMPARE(backend->boardPageRequests.last().second, firstStage);
        auto *status = board->findChild<QLabel *>(QStringLiteral("quiet"), Qt::FindDirectChildrenOnly);
        QVERIFY(status && status->isVisible() && loadingVisible(status));
        const auto second = tasks(524, 25);
        emit backend->boardTasksPageLoaded(QStringLiteral("999"), firstStage, second, false);
        QCOMPARE(column->count(), 24);
        scrollDown(column);
        QCoreApplication::sendPostedEvents(board, QEvent::MetaCall);
        QCOMPARE(backend->boardPageRequests.size(), 1);

        column->setCurrentRow(5);
        const auto *selected = column->currentItem();
        const auto previousPosition = scrollbar->value();
        const int firstPageMaximum = scrollbar->maximum();
        emit backend->boardTasksPageLoaded(project, firstStage, second, true);
        QVERIFY(column);
        QCOMPARE(column->count(), 49);
        QCOMPARE(column->currentItem(), selected);
        QCOMPARE(scrollbar->value(), previousPosition);
        QVERIFY(!loadingVisible(status));
        QVERIFY(!status->isVisible());
        QVERIFY(findAccessible<QLabel>(board, QStringLiteral("Задач в колонке: 79")));
        // Appending rows schedules a list layout independently of the board's queued page check.
        column->doItemsLayout();
        QVERIFY(scrollbar->maximum() > firstPageMaximum);
        QCoreApplication::sendPostedEvents(board, QEvent::MetaCall);
        QCOMPARE(backend->boardPageRequests.size(), 1);

        scrollbar->setValue(0);
        QStyleOptionSlider options;
        options.initFrom(scrollbar);
        options.orientation = Qt::Vertical;
        options.minimum = scrollbar->minimum();
        options.maximum = scrollbar->maximum();
        options.pageStep = scrollbar->pageStep();
        options.singleStep = scrollbar->singleStep();
        options.sliderPosition = scrollbar->sliderPosition();
        options.sliderValue = scrollbar->value();
        const auto handle = scrollbar->style()->subControlRect(QStyle::CC_ScrollBar, &options,
                                                               QStyle::SC_ScrollBarSlider, scrollbar);
        QVERIFY(!handle.isEmpty());
        QTest::mousePress(scrollbar, Qt::LeftButton, Qt::NoModifier, handle.center());
        QVERIFY(scrollbar->isSliderDown());
        const QPoint bottom(handle.center().x(), scrollbar->height() - 3);
        QMouseEvent drag(QEvent::MouseMove, bottom, scrollbar->mapToGlobal(bottom),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(scrollbar, &drag);
        QVERIFY(3 * qint64(scrollbar->value()) >= 2 * qint64(scrollbar->maximum()));
        QTest::mouseRelease(scrollbar, Qt::LeftButton, Qt::NoModifier, bottom);
        QTRY_COMPARE(backend->boardPageRequests.size(), 2);
        QCOMPARE(backend->boardPageRequests.last().second, firstStage);
        emit backend->boardTasksPageLoaded(project, firstStage, tasks(549, 15), true);
        QCOMPARE(column->count(), 64);

        auto *listButton = findAccessible<QPushButton>(&window, QStringLiteral("Вид Список"));
        QVERIFY(listButton);
        listButton->click();
        auto *list = findAccessible<QListWidget>(board, QStringLiteral("Список задач"));
        QVERIFY(list && list->count() == 64);
        list->setFocus();
        QTest::keyClick(list, Qt::Key_End);
        QTRY_COMPARE(backend->boardPageRequests.size(), 3);
        QCOMPARE(backend->boardPageRequests.last().second, firstStage);
        auto third = tasks(564, 15);
        third.prepend(second.first());
        emit backend->boardTasksPageLoaded(project, firstStage, third, false);
        QCOMPARE(list->count(), 79);
        QCOMPARE(list->item(78)->data(workspace::Id).toString(), QStringLiteral("578"));
        QCoreApplication::sendPostedEvents(board, QEvent::MetaCall);
        QCOMPARE(backend->boardPageRequests.size(), 3);

        auto *kanbanButton = findAccessible<QPushButton>(&window, QStringLiteral("Вид Канбан"));
        QVERIFY(kanbanButton);
        kanbanButton->click();
        auto *shortColumn = findAccessible<QListWidget>(board, QStringLiteral("Готово"));
        QVERIFY(shortColumn);
        QCOMPARE(shortColumn->verticalScrollBar()->maximum(), 0);
        scrollDown(shortColumn);
        QTRY_COMPARE(backend->boardPageRequests.size(), 4);
        QCOMPARE(backend->boardPageRequests.last().second, backend->stages.at(1).id);
        emit backend->failed(QStringLiteral("board-column:101:602"), QStringLiteral("Нет связи"));
        QVERIFY(!loadingVisible(status));
        scrollDown(shortColumn);
        QCoreApplication::sendPostedEvents(board, QEvent::MetaCall);
        QCOMPARE(backend->boardPageRequests.size(), 4);
    }

    void projectDirectoryWidthPersistsIndependentlyFromChat()
    {
        QSettings settings(QStringLiteral("bitrix24-client"), QStringLiteral("bitrix24-client"));
        const auto projectKey = QStringLiteral("ui/projectDirectoryWidth");
        const auto chatKey = QStringLiteral("ui/chatDirectoryWidth");
        settings.remove(projectKey);
        const auto clearWidths = qScopeGuard([&] {
            settings.remove(projectKey);
            settings.remove(chatKey);
        });
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        int chatWidth = 0;
        int savedWidth = 0;
        {
            MainWindow window(backend.get());
            window.resize(960, 650);
            window.show();
            auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
            auto *chats = window.findChild<QSplitter *>(QStringLiteral("chatDirectorySplitter"));
            QVERIFY(sections && chats);
            chatWidth = qMax(320, chats->widget(0)->minimumWidth() + 32);
            settings.setValue(chatKey, chatWidth);
            sections->setCurrentRow(1);
            auto *splitter = window.findChild<QSplitter *>(QStringLiteral("projectDirectorySplitter"));
            QVERIFY(splitter);
            QTRY_VERIFY(splitter->isVisible());
            const int old = splitter->sizes().first();
            QVERIFY(old >= 132);
            auto *handle = splitter->handle(1);
            const auto start = handle->rect().center();
            QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, start);
            QTest::mouseMove(handle, start + QPoint(63, 0));
            QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier, start + QPoint(63, 0));
            QTRY_VERIFY(splitter->sizes().first() > old + 30);
            savedWidth = splitter->sizes().first();
            QCOMPARE(settings.value(projectKey).toInt(), savedWidth);
            QCOMPARE(settings.value(chatKey).toInt(), chatWidth);
        }
        MainWindow restored(backend.get());
        const auto wideWindow = qMax(960, chatWidth + 550);
        restored.resize(wideWindow, 650);
        restored.show();
        auto *sections = findAccessible<QListWidget>(&restored, QStringLiteral("Разделы"));
        auto *splitter = restored.findChild<QSplitter *>(QStringLiteral("projectDirectorySplitter"));
        auto *chats = restored.findChild<QSplitter *>(QStringLiteral("chatDirectorySplitter"));
        QVERIFY(sections && splitter && chats);
        QTRY_COMPARE(chats->sizes().first(), chatWidth);
        sections->setCurrentRow(1);
        QTRY_COMPARE(splitter->sizes().first(), savedWidth);
        restored.resize(720, 500);
        QTRY_VERIFY(splitter->sizes().at(1) >= 430);
        restored.resize(wideWindow, 650);
        QTRY_COMPARE(splitter->sizes().first(), savedWidth);
    }

    void kanbanStageColorsPaintAndSaveWithoutRenaming()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto &stage = backend->stages.first();
        stage.color = QStringLiteral("11c4e9");
        backend->stages.last().color = QStringLiteral("254763");
        const QString stageId = stage.id;
        const QString name = stage.name;
        const QString darkName = backend->stages.last().name;
        backend->stages.append({QStringLiteral("603"), QStringLiteral("Без цвета"), {}});
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);

        const auto colorIn = [](const QListWidget *list) {
            auto *header = list ? list->parentWidget()->findChild<QWidget *>(QStringLiteral("kanbanStageHeader")) : nullptr;
            if (!header || !header->isVisible())
                return QColor();
            const auto image = header->grab().toImage();
            return image.pixelColor(qRound(12 * image.devicePixelRatio()), qRound(4 * image.devicePixelRatio()));
        };
        QTRY_COMPARE(colorIn(findAccessible<QListWidget>(&window, name)), QColor("#11c4e9"));
        QTRY_COMPARE(colorIn(findAccessible<QListWidget>(&window, darkName)), QColor("#254763"));
        QTRY_COMPARE(colorIn(findAccessible<QListWidget>(&window, QStringLiteral("Без цвета"))), QColor("#dce5e4"));
        const auto screenshot = qEnvironmentVariable("B24_STAGE_COLORS_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(window.grab().save(screenshot + QStringLiteral("-wide.png")));

        auto *stageTools = findAccessible<QPushButton>(&window, QStringLiteral("Управление этапами"));
        QVERIFY(stageTools && stageTools->isEnabled());
        stageTools->click();
        auto *manager = window.findChild<QDialog *>(QStringLiteral("stageManagerDialog"));
        QVERIFY(manager);
        auto *stages = findAccessible<QListWidget>(manager, QStringLiteral("Список этапов"));
        auto *title = findAccessible<QLineEdit>(manager, QStringLiteral("Название этапа"));
        auto *color = findAccessible<QPushButton>(manager, QStringLiteral("Изменить цвет этапа"));
        QVERIFY(stages && title && color);
        stages->setCurrentRow(0);
        title->setText(QStringLiteral("Несохранённое название"));
        color->setFocus();
        QTest::keyClick(color, Qt::Key_Space);
        auto *picker = manager->findChild<QColorDialog *>();
        QVERIFY(picker && picker->isVisible());
        QCOMPARE(picker->currentColor(), QColor("#11c4e9"));
        picker->setCurrentColor(QColor("#7b47d1"));
        picker->accept();
        QCOMPARE(backend->savedStage, stageId);
        QVERIFY(backend->savedStageTitle.isEmpty());
        QCOMPARE(backend->savedAfterStage, QString());
        QCOMPARE(backend->savedStageColor, QStringLiteral("7b47d1"));
        QTRY_COMPARE(colorIn(findAccessible<QListWidget>(&window, name)), QColor("#7b47d1"));
        QCOMPARE(title->text(), QStringLiteral("Несохранённое название"));
        manager->reject();

        MainWindow readOnly(backend.get(), true);
        readOnly.resize(720, 500);
        readOnly.show();
        QApplication::setActiveWindow(&readOnly);
        QTest::keyClick(&readOnly, Qt::Key_2, Qt::AltModifier);
        const auto *readOnlyStages = findAccessible<QPushButton>(&readOnly, QStringLiteral("Управление этапами"));
        QVERIFY(readOnlyStages && !readOnlyStages->isEnabled());
        QTRY_COMPARE(colorIn(findAccessible<QListWidget>(&readOnly, name)), QColor("#7b47d1"));
        if (!screenshot.isEmpty())
            QVERIFY(readOnly.grab().save(screenshot + QStringLiteral("-compact.png")));
    }

    void createsTaskProjectAndManagesStages()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);

        auto *newTask = findAccessible<QPushButton>(&window, QStringLiteral("Создать задачу"));
        QVERIFY(newTask);
        newTask->click();
        auto *form = findAccessible<QDialog>(&window, QStringLiteral("Форма новой задачи"));
        QVERIFY(form);
        auto *title = findAccessible<QLineEdit>(form, QStringLiteral("Заголовок задачи"));
        QVERIFY(title);
        title->setText(QStringLiteral("Новая задача"));
        auto *save = findAccessible<QPushButton>(form, QStringLiteral("Сохранить задачу"));
        QVERIFY(save);
        save->click();
        QCOMPARE(backend->createdProject, QStringLiteral("101"));
        QVERIFY(backend->lastCreate.title.has_value());
        QCOMPARE(*backend->lastCreate.title, QStringLiteral("Новая задача"));
        if (auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog")))
            card->reject();

        auto *newProject = findAccessible<QPushButton>(&window, QStringLiteral("Создать проект"));
        QVERIFY(newProject);
        newProject->click();
        auto *nameDialog = window.findChild<QDialog *>(QStringLiteral("nameDialog"));
        QVERIFY(nameDialog);
        auto *name = findAccessible<QLineEdit>(nameDialog, QStringLiteral("Название"));
        QVERIFY(name);
        name->setText(QStringLiteral("Новый проект"));
        auto *confirm = findAccessible<QPushButton>(nameDialog, QStringLiteral("Подтвердить название"));
        QVERIFY(confirm);
        confirm->click();
        QCOMPARE(backend->createdProjectName, QStringLiteral("Новый проект"));
        auto *projects = findAccessible<QListWidget>(&window, QStringLiteral("Проекты"));
        QVERIFY(projects);
        QCOMPARE(projects->currentItem()->data(workspace::Id).toString(), QStringLiteral("102"));
        projects->setCurrentRow(0);

        auto *stagesButton = findAccessible<QPushButton>(&window, QStringLiteral("Управление этапами"));
        QVERIFY(stagesButton);
        stagesButton->click();
        auto *manager = window.findChild<QDialog *>(QStringLiteral("stageManagerDialog"));
        QVERIFY(manager);
        auto *stageList = findAccessible<QListWidget>(manager, QStringLiteral("Список этапов"));
        QVERIFY(stageList);
        QTRY_COMPARE(stageList->count(), 2);
        stageList->setCurrentRow(0);
        auto *stageTitle = findAccessible<QLineEdit>(manager, QStringLiteral("Название этапа"));
        QVERIFY(stageTitle);
        stageTitle->setText(QStringLiteral("В работе 2"));
        auto *rename = findAccessible<QPushButton>(manager, QStringLiteral("Переименовать этап"));
        QVERIFY(rename);
        rename->click();
        QCOMPARE(backend->savedStage, QStringLiteral("601"));
        QCOMPARE(backend->savedStageTitle, QStringLiteral("В работе 2"));

        auto *remove = findAccessible<QPushButton>(manager, QStringLiteral("Удалить этап"));
        QVERIFY(remove);
        QVERIFY(!remove->isEnabled());
        remove->click();
        QVERIFY(backend->deletedStage.isEmpty());

        stageList->setCurrentRow(1);
        auto *moveUp = findAccessible<QPushButton>(manager, QStringLiteral("Переместить этап выше"));
        QVERIFY(moveUp);
        moveUp->click();
        QCOMPARE(backend->savedStage, QStringLiteral("602"));
        QCOMPARE(backend->savedAfterStage, QStringLiteral("0"));

        backend->stages[1].taskCount.reset();
        emit backend->boardLoaded(QStringLiteral("101"), backend->stages);
        QVERIFY(!remove->isEnabled());
        backend->stages[1].taskCount = 0;
        emit backend->boardLoaded(QStringLiteral("101"), backend->stages);
        QVERIFY(remove->isEnabled());
        QTimer::singleShot(0, [] {
            auto *confirm = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (confirm)
                confirm->button(QMessageBox::Yes)->click();
        });
        remove->click();
        QCOMPARE(backend->deletedStage, QStringLiteral("602"));
    }

    void projectSearchFiltersLoadedCatalogAndKeepsTaskFilter()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        Project second;
        second.id = QStringLiteral("102");
        second.name = QStringLiteral("Второй проект");
        backend->projects = {backend->projects.first(), second};
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        auto *projects = findAccessible<QListWidget>(&window, QStringLiteral("Проекты"));
        QVERIFY(projects);
        QTRY_COMPARE(projects->count(), 2);

        auto *search = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск по названию проектов"));
        QVERIFY(search);
        auto *taskFilter = findAccessible<QLineEdit>(&window, QStringLiteral("Фильтр задач"));
        QVERIFY(taskFilter);
        QVERIFY(taskFilter->text().isEmpty());
        search->setText(QStringLiteral("Второй"));
        QTRY_COMPARE(projects->count(), 1);
        QCOMPARE(projects->item(0)->data(workspace::Id).toString(), QStringLiteral("102"));
        QVERIFY(taskFilter->text().isEmpty());
        search->clear();
        QTRY_COMPARE(projects->count(), 2);
    }

    void projectAvatarUsesSystemIconUnlessCustomPhotoIsAvailable()
    {
        const auto render = [](const QString &type, const QPixmap &photo = {}) {
            QImage image(48, 48, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing);
            workspace::paintAvatar(painter, image.rect(), QStringLiteral("Проект"),
                                   workspace::AvatarKind::Project, photo, {}, {}, type);
            return image;
        };
        const auto initials = render(QString());
        QCOMPARE(render(QStringLiteral("unknown")), initials);
        const QStringList types{QStringLiteral("folder"), QStringLiteral("checks"), QStringLiteral("pie"),
                                QStringLiteral("bag"), QStringLiteral("members")};
        const QStringList aliases{QStringLiteral("folder"), QStringLiteral("tasks"), QStringLiteral("chart"),
                                  QStringLiteral("briefcase"), QStringLiteral("group")};
        QVector<QImage> icons;
        for (qsizetype i = 0; i < types.size(); ++i) {
            const auto system = render(types.at(i));
            QVERIFY(system != initials);
            QVERIFY(!icons.contains(system));
            QCOMPARE(render(aliases.at(i)), system);
            icons.append(system);
        }
        QPixmap custom(32, 32);
        custom.fill(QColor("#429360"));
        QCOMPARE(render(QStringLiteral("folder"), custom), render(QString(), custom));
        QCOMPARE(render(QStringLiteral("folder"), custom).pixelColor(24, 24), QColor("#429360"));
    }

    void displaysProjectUnreadCountsBeforeOpeningChats()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->accountScope = QStringLiteral("fixture/project-unread-first");
        backend->holdProjectTaskChats = true;
        backend->projects.first().pinned = true;
        backend->projects.append({QStringLiteral("102"), QStringLiteral("Склад"), {}});
        backend->projects[1].pinned = true;
        backend->projects.append({QStringLiteral("103"), QStringLiteral("Проверка"), {}});
        MainWindow window(backend.get());
        window.resize(850, 630);
        window.show();
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        auto *projects = findAccessible<QListWidget>(&window, QStringLiteral("Проекты"));
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("projectScopeTabs"));
        QVERIFY(sections && projects && tabs);
        sections->setCurrentRow(1);
        QTRY_COMPARE(projects->count(), 3);
        QCOMPARE(tabs->currentIndex(), 0);
        QVERIFY(backend->projectTaskChatRequests.isEmpty());

        auto *first = projects->item(0);
        auto *second = projects->item(1);
        const auto directoryIndex = projects->currentRow();
        emit backend->projectUnreadCountsChanged({
            {QStringLiteral("101"), 310}, {QStringLiteral("102"), 1021}, {QStringLiteral("104"), 6}}, true);
        QCOMPARE(projects->item(0), first);
        QCOMPARE(projects->item(1), second);
        QCOMPARE(projects->currentRow(), directoryIndex);
        QCOMPARE(first->data(workspace::Count).toLongLong(), 310);
        QCOMPARE(second->data(workspace::Count).toLongLong(), 1021);
        QCOMPARE(projects->item(2)->data(workspace::Count).toLongLong(), 0);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты (310)"));
        QVERIFY(first->data(Qt::AccessibleDescriptionRole).toString().contains(QStringLiteral("Закреплён")));
        QVERIFY(first->data(Qt::AccessibleDescriptionRole).toString().contains(QStringLiteral("310")));
        QVERIFY(backend->projectTaskChatRequests.isEmpty());
        if (const auto capture = qEnvironmentVariable("B24_PROJECT_UNREAD_SCREENSHOT"); !capture.isEmpty())
            QVERIFY(window.grab().save(capture));

        projects->setCurrentRow(1);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты (1021)"));
        QVERIFY(backend->projectTaskChatRequests.isEmpty());

        backend->projects[1].name = QStringLiteral("Склад материалов");
        backend->projects.append({QStringLiteral("104"), QStringLiteral("Следующая страница"), {}});
        emit backend->projectsLoaded(backend->projects);
        QCOMPARE(projects->currentItem()->data(workspace::Title).toString(), backend->projects[1].name);
        QCOMPARE(projects->currentItem()->data(workspace::Count).toLongLong(), 1021);
        QCOMPARE(projects->item(3)->data(workspace::Count).toLongLong(), 6);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты (1021)"));

        emit backend->projectPinChanged(QStringLiteral("102"), false);
        QCOMPARE(projects->currentItem()->data(workspace::Count).toLongLong(), 1021);
        QVERIFY(!projects->currentItem()->data(Qt::AccessibleDescriptionRole).toString().contains(QStringLiteral("Закреплён")));
        emit backend->projectPinChanged(QStringLiteral("102"), true);
        QVERIFY(projects->currentItem()->data(Qt::AccessibleDescriptionRole).toString().contains(QStringLiteral("Закреплён")));
        emit backend->projectUnreadCountsChanged({}, true);
        QCOMPARE(projects->item(0)->data(workspace::Count).toLongLong(), 0);
        QCOMPARE(projects->item(1)->data(workspace::Count).toLongLong(), 0);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты"));
        QVERIFY(backend->projectTaskChatRequests.isEmpty());

        emit backend->projectUnreadCountsChanged({{QStringLiteral("102"), 2}}, false);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты (2+)"));
        QVERIFY(projects->currentItem()->data(workspace::CountPartial).toBool());
        QVERIFY(projects->currentItem()->data(Qt::AccessibleDescriptionRole).toString().contains(QStringLiteral("не менее 2")));
        tabs->setCurrentIndex(1);
        QCOMPARE(backend->projectTaskChatRequests.last(), qMakePair(QStringLiteral("102"), 0));
        Chat chat;
        chat.id = QStringLiteral("chat42");
        chat.taskId = QStringLiteral("501");
        chat.title = QStringLiteral("Задача");
        chat.taskChat = true;
        chat.unread = 5;
        emit backend->projectTaskChatsLoaded(QStringLiteral("102"), 0, {chat}, 50);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты (5+)"));
        emit backend->projectUnreadCountsChanged({{QStringLiteral("102"), 9}}, false);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты (9+)"));
        emit backend->projectUnreadCountsChanged({}, true);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты"));
        QCOMPARE(projects->currentItem()->data(workspace::Count).toLongLong(), 0);
        emit backend->projectUnreadCountsChanged({{QStringLiteral("102"), 4}}, true);
        tabs->setCurrentIndex(0);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты (4)"));

        backend->accountScope = QStringLiteral("fixture/project-unread-second");
        emit backend->accountScopeChanged();
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты"));
        QCOMPARE(projects->currentItem()->data(workspace::Count).toLongLong(), 0);
    }

    void projectTaskChatPaneStartsOnTheTaskView()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        sections->setCurrentRow(1);
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("projectScopeTabs"));
        QVERIFY(tabs);
        QCOMPARE(tabs->tabText(0), QStringLiteral("Задачи"));
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты"));
        QCOMPARE(tabs->currentIndex(), 0);
        auto *tasks = findAccessible<QLineEdit>(&window, QStringLiteral("Фильтр задач"));
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Чаты задач проекта"));
        QVERIFY(tasks && chats);
        QVERIFY(tasks->isVisible());
        QVERIFY(!chats->isVisible());
        tabs->setCurrentIndex(1);
        QVERIFY(!tasks->isVisible());
        tabs->setCurrentIndex(0);
        QVERIFY(tasks->isVisible());
    }

    void projectTaskChatsPageAndUpdateOnlyTheOpenProject()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->accountScope = QStringLiteral("fixture/project-chat-first");
        backend->holdProjectTaskChats = true;
        backend->projects.first().pinned = true;
        backend->projects.append({QStringLiteral("102"), QStringLiteral("Склад"), {}});
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        auto *projects = findAccessible<QListWidget>(&window, QStringLiteral("Проекты"));
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("projectScopeTabs"));
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Чаты задач проекта"));
        auto *more = window.findChild<QPushButton *>(QStringLiteral("moreProjectTaskChats"));
        auto *state = findAccessible<QLabel>(&window, QStringLiteral("Состояние чатов проекта"));
        QVERIFY(sections && projects && tabs && chats && more && state);
        sections->setCurrentRow(1);
        QTRY_COMPARE(projects->currentItem()->data(workspace::Id).toString(), QStringLiteral("101"));
        QVERIFY(backend->projectTaskChatRequests.isEmpty());
        tabs->setCurrentIndex(1);
        QCOMPARE(backend->projectTaskChatRequests.size(), 1);
        QCOMPARE(backend->projectTaskChatRequests.last(), qMakePair(QStringLiteral("101"), 0));
        QVERIFY(state->isVisible());
        QVERIFY(!more->isVisible());
        QVERIFY(!chats->isVisible());

        Chat first;
        first.id = QStringLiteral("chat42");
        first.title = QStringLiteral("Монтаж");
        first.taskId = QStringLiteral("501");
        first.taskChat = true;
        first.unread = 3;
        Chat second = first;
        second.id = QStringLiteral("chat43");
        second.taskId = QStringLiteral("502");
        second.title = QStringLiteral("Доставка");
        second.unread = 2;
        emit backend->projectTaskChatsLoaded(QStringLiteral("102"), 0, {second}, -1);
        emit backend->projectTaskChatsLoaded(QStringLiteral("101"), 50, {second}, -1);
        QCOMPARE(chats->count(), 0);
        emit backend->projectTaskChatsLoaded(QStringLiteral("101"), 0, {first}, 50);
        QTRY_VERIFY(chats->isVisible() && more->isVisible());
        QCOMPARE(chats->item(0)->data(workspace::Count).toInt(), 3);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты (3+)"));
        QVERIFY(tabs->tabToolTip(1).contains(QStringLiteral("в загруженных чатах")));
        if (const auto capture = qEnvironmentVariable("B24_PROJECT_CHATS_SCREENSHOT"); !capture.isEmpty())
            QVERIFY(window.grab().save(capture));

        first.unread = 0;
        Chat badge;
        badge.id = first.id;
        badge.serverChatId = 42;
        emit backend->projectTaskChatBadgesChanged(QStringLiteral("102"), {badge});
        QCOMPARE(chats->item(0)->data(workspace::Count).toInt(), 3);
        emit backend->projectTaskChatBadgesChanged(QStringLiteral("101"), {badge, second});
        QCOMPARE(chats->count(), 1);
        QCOMPARE(chats->item(0)->data(workspace::Count).toInt(), 0);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты"));
        more->click();
        more->click();
        QCOMPARE(backend->projectTaskChatRequests.size(), 2);
        QCOMPARE(backend->projectTaskChatRequests.last(), qMakePair(QStringLiteral("101"), 50));
        QVERIFY(!more->isEnabled());
        emit backend->failed(QStringLiteral("project-task-chats:102"), QStringLiteral("Чужая ошибка"));
        QVERIFY(!more->isEnabled());
        emit backend->failed(QStringLiteral("project-task-chats:101"), QStringLiteral("Не удалось загрузить"));
        QTRY_VERIFY(more->isEnabled());
        QCOMPARE(more->text(), QStringLiteral("Повторить"));
        QCOMPARE(chats->count(), 1);
        more->click();
        QCOMPARE(backend->projectTaskChatRequests.last(), qMakePair(QStringLiteral("101"), 50));
        emit backend->projectTaskChatsLoaded(QStringLiteral("101"), 50, {first, second}, -1);
        QCOMPARE(chats->count(), 2);
        QVERIFY(!more->isVisible());
        QCOMPARE(chats->item(1)->data(workspace::Count).toInt(), 2);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Чаты (2)"));

        emit chats->itemActivated(chats->item(0));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        QCOMPARE(backend->taskReadRequests.last(), first.taskId);
        card->close();
        QTRY_VERIFY(!window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        projects->setCurrentRow(1);
        QCOMPARE(projects->currentItem()->data(workspace::Id).toString(), QStringLiteral("102"));
        QCOMPARE(backend->projectTaskChatRequests[backend->projectTaskChatRequests.size() - 2], qMakePair(QString(), 0));
        QCOMPARE(backend->projectTaskChatRequests.last(), qMakePair(QStringLiteral("102"), 0));
        emit backend->projectTaskChatsLoaded(QStringLiteral("101"), 0, {first}, -1);
        QCOMPARE(chats->count(), 0);
        emit backend->projectTaskChatsLoaded(QStringLiteral("102"), 0, {second}, -1);
        QCOMPARE(chats->item(0)->data(workspace::TaskId).toString(), second.taskId);

        const auto requests = backend->projectTaskChatRequests.size();
        emit backend->taskCreated(QStringLiteral("101"), QStringLiteral("503"));
        QCOMPARE(backend->projectTaskChatRequests.size(), requests);
        emit backend->taskCreated(QStringLiteral("102"), QStringLiteral("504"));
        QCOMPARE(backend->projectTaskChatRequests.size(), requests + 2);
        QCOMPARE(backend->projectTaskChatRequests.last(), qMakePair(QStringLiteral("102"), 0));
        emit backend->projectTaskChatsLoaded(QStringLiteral("102"), 0, {second}, -1);
        QCOMPARE(chats->count(), 1);

        backend->accountScope = QStringLiteral("fixture/project-chat-second");
        emit backend->accountScopeChanged();
        QCOMPARE(backend->projectTaskChatRequests.last(), qMakePair(QString(), 0));
        emit backend->projectTaskChatsLoaded(QStringLiteral("102"), 0, {second}, -1);
        QCOMPARE(chats->count(), 0);
    }

    void projectTaskChatsRefreshAfterMovingATask_data()
    {
        QTest::addColumn<bool>("showSource");
        QTest::newRow("moving-out") << true;
        QTest::newRow("moving-in") << false;
    }

    void projectTaskChatsRefreshAfterMovingATask()
    {
        QFETCH(bool, showSource);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        const auto source = QStringLiteral("101");
        const auto target = QStringLiteral("102");
        const auto selected = showSource ? source : target;
        auto &task = backend->stages.first().tasks.first();
        task.projectId = source;
        task.editRights.project = true;
        backend->projects.append({target, QStringLiteral("Склад"), {}});
        backend->holdTaskUpdate = true;
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        window.onConnectionChanged(true);
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        auto *projects = findAccessible<QListWidget>(&window, QStringLiteral("Проекты"));
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("projectScopeTabs"));
        QVERIFY(sections && projects && tabs);
        sections->setCurrentRow(1);
        QTRY_COMPARE(projects->count(), 2);
        projects->setCurrentRow(showSource ? 0 : 1);
        tabs->setCurrentIndex(1);
        QCOMPARE(backend->projectTaskChatRequests.last(), qMakePair(selected, 0));
        window.openMessageLink(ui::taskLink(backend->portalOrigin(), source, task.id));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        auto *project = findAccessible<QPushButton>(card, QStringLiteral("Проект"));
        QVERIFY(project && project->isEnabled());
        project->click();
        auto *choices = findAccessible<QListWidget>(card, QStringLiteral("Проект задачи"));
        QVERIFY(choices);
        const auto targetRows = choices->findItems(QStringLiteral("Склад"), Qt::MatchExactly);
        QCOMPARE(targetRows.size(), 1);
        choices->setCurrentItem(targetRows.first());
        QVERIFY(backend->updatedTask.isEmpty());
        const auto boardRequests = backend->boardRequests;
        const auto requests = backend->projectTaskChatRequests.size();
        QTest::keyClick(choices, Qt::Key_Return);
        QCOMPARE(backend->lastUpdate.projectId, std::optional<QString>(target));
        QCOMPARE(backend->projectTaskChatRequests.size(), requests);
        task.projectId = target;
        emit backend->taskUpdated(task.id);
        QCOMPARE(backend->projectTaskChatRequests.size(), requests + 2);
        QCOMPARE(backend->projectTaskChatRequests[requests], qMakePair(QString(), 0));
        QCOMPARE(backend->projectTaskChatRequests.last(), qMakePair(selected, 0));
        QCOMPARE(backend->boardRequests, boardRequests);
    }

    void projectDirectoryDrawsThePinBesideTheTitle()
    {
        QListWidget list;
        auto *painter = workspace::configureRows(&list, workspace::Row::Directory);
        auto *row = workspace::addRow(&list, QStringLiteral("Проект"), {}, {}, QStringLiteral("101"));
        row->setData(workspace::AvatarType, int(workspace::AvatarKind::Project));
        const auto paintRow = [&] {
            QImage image(QSize(190, 52), QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter output(&image);
            QStyleOptionViewItem option;
            option.rect = image.rect();
            painter->paint(&output, option, list.indexFromItem(row));
            return image.copy(QRect(158, 7, 22, 18));
        };
        const auto withoutPin = paintRow();
        row->setData(workspace::Pinned, true);
        QVERIFY(paintRow() != withoutPin);
    }

    void projectRailKeepsPagesAndShowsIncompleteSearch()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        Project second;
        second.id = QStringLiteral("102");
        second.name = QStringLiteral("Проект со второй страницы");
        backend->moreProjects = {second};
        Notification notice;
        notice.id = QStringLiteral("71");
        notice.title = QStringLiteral("Проект");
        notice.targets = {{NotificationTargetKind::Project, second.id, {}, {}, QStringLiteral("Открыть проект")}};
        backend->notifications = {notice};

        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        auto *projects = findAccessible<QListWidget>(&window, QStringLiteral("Проекты"));
        auto *search = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск по названию проектов"));
        auto *more = window.findChild<QPushButton *>(QStringLiteral("moreProjects"));
        QVERIFY(sections && projects && search && more);
        sections->setCurrentRow(1);
        QCOMPARE(backend->projectRequests, 1);
        QVERIFY(more->isVisible() && more->isEnabled());
        search->setText(QStringLiteral("второй"));
        QCOMPARE(projects->item(0)->data(workspace::Detail).toString(),
                 QStringLiteral("В загруженных проектах совпадений нет"));
        QCOMPARE(backend->projectMoreRequests, 0);
        QTest::keyClick(more, Qt::Key_Space);
        QCOMPARE(backend->projectMoreRequests, 1);
        QCOMPARE(projects->item(0)->data(workspace::Id).toString(), second.id);
        QVERIFY(more->isHidden());

        sections->setCurrentRow(0);
        sections->setCurrentRow(1);
        QCOMPARE(backend->projectRequests, 1);
        QCOMPARE(projects->count(), 1);
        QCOMPARE(projects->item(0)->data(workspace::Id).toString(), second.id);
        search->setText(QStringLiteral("Нет совпадений"));
        QCOMPARE(projects->item(0)->data(workspace::Detail).toString(), QStringLiteral("Проектов по запросу нет"));
        sections->setCurrentRow(2);
        auto *notices = findAccessible<QListWidget>(&window, QStringLiteral("Уведомления"));
        QVERIFY(notices && notices->count() == 1);
        notices->setCurrentRow(0);
        QApplication::setActiveWindow(&window);
        notices->setFocus();
        QTest::keyClick(notices, Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(sections->currentRow(), 1);
        QCOMPARE(backend->projectRequests, 1);
        QVERIFY(search->text().isEmpty());
        QVERIFY(projects->currentItem());
        QCOMPARE(projects->currentItem()->data(workspace::Id).toString(), second.id);

        QTest::keyClick(&window, Qt::Key_F5);
        QCOMPARE(backend->projectRequests, 2);
        emit backend->projectCreated(QStringLiteral("103"));
        QCOMPARE(backend->projectRequests, 3);
    }

    void projectRailLoadsOnForwardScrollAndRetriesExactlyOnce()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->projects.clear();
        for (int i = 101; i < 128; ++i)
            backend->projects.append({QString::number(i), QStringLiteral("Проект %1").arg(i), {}});
        backend->moreProjects = {{QStringLiteral("128"), QStringLiteral("Следующая страница"), {}}};
        backend->holdProjectMore = true;
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        auto *projects = findAccessible<QListWidget>(&window, QStringLiteral("Проекты"));
        auto *more = window.findChild<QPushButton *>(QStringLiteral("moreProjects"));
        QVERIFY(sections && projects && more);
        sections->setCurrentRow(1);
        auto *scroll = projects->verticalScrollBar();
        QTRY_VERIFY(scroll->maximum() > 10);
        QTRY_VERIFY(more->isHidden());
        QCOMPARE(backend->projectMoreRequests, 0);
        projects->setCurrentRow(4);
        const auto selection = projects->currentItem()->data(workspace::Id).toString();
        const auto threshold = [&] { return scroll->minimum() + ((scroll->maximum() - scroll->minimum()) * 2 + 2) / 3; };
        scroll->setValue(threshold() / 2);
        scroll->triggerAction(QAbstractSlider::SliderSingleStepAdd);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QVERIFY(scroll->value() < threshold());
        QCOMPARE(backend->projectMoreRequests, 0);

        // Leaving the board before the queued scroll check runs must cancel this load.
        scroll->triggerAction(QAbstractSlider::SliderToMaximum);
        sections->setCurrentRow(0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCOMPARE(backend->projectMoreRequests, 0);
        sections->setCurrentRow(1);
        scroll->setValue(scroll->minimum());
        const auto boundary = threshold();
        scroll->setTracking(false);
        scroll->setSliderDown(true);
        scroll->setSliderPosition(boundary);
        scroll->triggerAction(QAbstractSlider::SliderMove);
        scroll->setSliderDown(false);
        scroll->setTracking(true);
        QCOMPARE(scroll->value(), boundary);
        QTRY_COMPARE(backend->projectMoreRequests, 1);
        emit backend->projectsMoreAvailable(true); // This signal precedes the fetched page.
        scroll->triggerAction(QAbstractSlider::SliderToMaximum);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCOMPARE(backend->projectMoreRequests, 1);

        emit backend->failed(QStringLiteral("projects"), QStringLiteral("Страница недоступна"));
        QTRY_VERIFY(more->isVisible() && more->isEnabled());
        QVERIFY(more->text().contains(QStringLiteral("Повторить")));
        scroll->setValue(threshold() - 1);
        scroll->triggerAction(QAbstractSlider::SliderToMaximum);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCOMPARE(backend->projectMoreRequests, 1);
        more->click();
        QCOMPARE(backend->projectMoreRequests, 2);
        QVERIFY(!more->isEnabled());

        const auto position = scroll->value();
        auto extended = backend->projects + backend->moreProjects;
        for (int i = 129; i < 140; ++i)
            extended.append({QString::number(i), QStringLiteral("Проект %1").arg(i), {}});
        emit backend->projectsMoreAvailable(true);
        emit backend->projectsLoaded(extended);
        QTRY_COMPARE(projects->count(), extended.size());
        QTRY_COMPARE(scroll->value(), position);
        QCOMPARE(projects->currentItem()->data(workspace::Id).toString(), selection);
        QTRY_VERIFY(more->isHidden());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCOMPARE(backend->projectMoreRequests, 2);
        scroll->triggerAction(QAbstractSlider::SliderToMaximum);
        QTRY_COMPARE(backend->projectMoreRequests, 3);
        emit backend->projectsMoreAvailable(false);
        emit backend->projectsLoaded(extended);
        scroll->setValue(scroll->minimum());
        scroll->triggerAction(QAbstractSlider::SliderToMaximum);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCOMPARE(backend->projectMoreRequests, 3);
        QVERIFY(more->isHidden());
    }

    void narrowPanelShowsFullNameTooltips()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        Project second;
        second.id = QStringLiteral("102");
        second.name = QStringLiteral("Очень длинное название второго проекта для узкой панели");
        backend->projects = {backend->projects.first(), second};
        MainWindow window(backend.get());
        window.resize(720, 500);
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        auto *projects = findAccessible<QListWidget>(&window, QStringLiteral("Проекты"));
        QVERIFY(projects);
        QTRY_COMPARE(projects->count(), 2);
        bool found = false;
        for (int row = 0; row < projects->count(); ++row) {
            const auto tooltip = projects->item(row)->toolTip();
            if (tooltip.contains(QStringLiteral("Очень длинное название второго проекта для узкой панели")))
                found = true;
        }
        QVERIFY(found);
        for (int i = 0; i < projects->count(); ++i)
            if (projects->item(i)->data(workspace::Id).toString() == second.id)
                projects->setCurrentRow(i);
        auto *title = findAccessible<QLabel>(&window, QStringLiteral("Открытый проект"));
        QVERIFY(title);
        QVERIFY(title->toolTip().contains(QStringLiteral("Очень длинное название второго проекта для узкой панели")));
        QVERIFY(title->width() >= title->fontMetrics().horizontalAdvance(QStringLiteral("название")));
    }
};

QTEST_MAIN(UiProjectsTest)
#include "tst_ui_projects.moc"
