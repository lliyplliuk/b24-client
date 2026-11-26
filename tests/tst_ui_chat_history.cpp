#include "ui/history_list.h"
#include "pilot_fixture.h"

using namespace bx;
using namespace bx::test;

class UiChatHistoryTest : public PilotFixture {
    Q_OBJECT

private slots:
    void quickReactionsAndFullPaletteWorkInBothHistories()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        backend->chats.first().id = QStringLiteral("chat91");
        auto &message = backend->messages.first();
        message.id = QStringLiteral("701");
        message.text = QStringLiteral("Готово завтра\nнаписать отчёт");
        message.outgoing = true;
        message.reactions = {{QStringLiteral("fire"), 3, true}};
        auto taskMessage = message;
        taskMessage.id = QStringLiteral("801");
        taskMessage.text = QStringLiteral("[B]Готово[/B]\nОк");
        backend->taskMessages = {taskMessage};
        backend->stages.first().tasks.first().chatDialogId = QStringLiteral("chat67");
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        emit backend->connectionChanged(true);
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        QTRY_COMPARE(history->count(), 1);

        const auto bubble = [](QListWidget *rows, int row = 0) {
            const auto *delegate = static_cast<const workspace::RowPainter *>(rows->itemDelegate());
            return delegate->receiptArea(rows->model()->index(row, 0), rows->visualItemRect(rows->item(row)));
        };
        const auto bodyPoint = [&bubble](QListWidget *rows) {
            const auto rect = bubble(rows);
            return QPoint(rect.left() + 12, rect.top() + 6
                + (rows->item(0)->data(workspace::Title).toString().isEmpty() ? 0 : 20)
                + QFontMetrics(workspace::rowFont(ui::bodyTextPixels)).height() / 2);
        };
        const auto chipPoint = [&bubble](QListWidget *rows) {
            const auto rect = bubble(rows);
            return QPoint(rect.left() + 15, rect.bottom() - 10);
        };
        auto *clipboard = QGuiApplication::clipboard();
        const auto copySelection = [clipboard](QListWidget *rows, const QString &expected) {
            clipboard->clear();
            QTest::keyClick(rows, Qt::Key_C, Qt::ControlModifier);
            QCOMPARE(clipboard->text(), expected);
        };
        const auto selectWord = [&bodyPoint](QListWidget *rows) {
            const auto point = bodyPoint(rows) + QPoint(14, 0);
            QTest::mouseDClick(rows->viewport(), Qt::LeftButton, Qt::NoModifier, point);
            QTest::mouseRelease(rows->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        };
        selectWord(history);
        copySelection(history, QStringLiteral("Готово"));
        QVERIFY(backend->messageActionId.isEmpty());
        QTest::keyClick(history, Qt::Key_Right, Qt::ShiftModifier);
        copySelection(history, QStringLiteral("Готово "));

        const auto metrics = QFontMetrics(workspace::rowFont(ui::bodyTextPixels));
        const auto start = bodyPoint(history);
        const auto end = start + QPoint(metrics.horizontalAdvance(QStringLiteral("написать")), metrics.lineSpacing());
        auto *viewport = history->viewport();
        QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, start);
        QMouseEvent drag(QEvent::MouseMove, end, viewport->mapToGlobal(end),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(viewport, &drag);
        QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, end);
        copySelection(history, QStringLiteral("Готово завтра\nнаписать"));
        QTest::keyClick(history, Qt::Key_Escape);
        clipboard->clear();
        QTest::keyClick(history, Qt::Key_C, Qt::ControlModifier);
        QVERIFY(clipboard->text() != QStringLiteral("Готово завтра\nнаписать"));

        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, chipPoint(history));
        QCOMPARE(backend->messageActionDialog, QStringLiteral("chat91"));
        QCOMPARE(backend->messageActionId, message.id);
        QCOMPARE(backend->messageActionValue, QStringLiteral("fire"));
        QCOMPARE(backend->messageAction, MessageAction::RemoveReaction);

        emit backend->chatOwnReactionChanged(QStringLiteral("chat92"), message.id, QStringLiteral("fire"), false);
        QCOMPARE(history->item(0)->data(workspace::MessageReactionCounts).toMap().value(QStringLiteral("fire")).toInt(), 3);
        emit backend->chatOwnReactionChanged(QStringLiteral("chat91"), message.id, QStringLiteral("fire"), false);
        emit backend->chatOwnReactionChanged(QStringLiteral("chat91"), message.id, QStringLiteral("fire"), false);
        QCOMPARE(history->item(0)->data(workspace::MessageReactionCounts).toMap().value(QStringLiteral("fire")).toInt(), 2);
        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, chipPoint(history));
        QCOMPARE(backend->messageAction, MessageAction::React);

        auto *more = findAccessible<QPushButton>(history, QStringLiteral("Действия сообщения"));
        QVERIFY(more && more->isHidden());
        QCoreApplication::processEvents();
        const bool offscreen = QGuiApplication::platformName() == QLatin1String("offscreen");
        const auto hover = [offscreen](QListWidget *rows, const QPoint &point) {
            auto *viewport = rows->viewport();
            if (offscreen) {
                QMouseEvent move(QEvent::MouseMove, point, viewport->mapToGlobal(point),
                                 Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                QCoreApplication::sendEvent(viewport, &move);
            } else {
                QTest::mouseMove(viewport, point);
            }
        };
        const auto verifyRightSide = [&bubble, &hover](QListWidget *rows, QPushButton *button) {
            const auto rect = bubble(rows);
            const int edge = rows->viewport()->rect().right();
            QVERIFY(edge - rect.right() > 50);
            for (const int y : {rect.top(), rect.center().y(), rect.bottom()}) {
                hover(rows, QPoint(edge, rect.top() - 1));
                QTRY_VERIFY(button->isHidden());
                hover(rows, QPoint(edge, y));
                QTRY_VERIFY(button->isVisible());
            }
            hover(rows, QPoint(edge, rect.bottom() + 1));
            QTRY_VERIFY(button->isHidden());
        };
        verifyRightSide(history, more);
        hover(history, QPoint(bubble(history).right() + 2, bubble(history).center().y()));
        QTRY_VERIFY(more->isVisible());
        QVERIFY(more->geometry().left() > bubble(history).right());
        const auto between = QPoint(bubble(history).right() + 1, more->geometry().center().y());
        QTest::mouseMove(history->viewport(), between);
        QVERIFY(more->isVisible());
        QTest::mouseMove(more, more->rect().center());
        QVERIFY(more->isVisible());
        QTest::mouseClick(more, Qt::LeftButton);
        auto *actions = visibleMessageMenu();
        QVERIFY(actions && actions->objectName() == QStringLiteral("messageContextMenu"));
        QAction *reactionAction = nullptr;
        for (auto *action : actions->actions())
            if (action->text() == QStringLiteral("Реакция"))
                reactionAction = action;
        QVERIFY(reactionAction && reactionAction->menu());
        auto *reactions = reactionAction->menu();
        const QPointer<QMenu> originalMenu(actions);
        QAction *allReactions = nullptr;
        for (auto *action : reactions->actions())
            if (action->text() == QStringLiteral("Все реакции…"))
                allReactions = action;
        QVERIFY(allReactions);
        if (offscreen) {
            allReactions->trigger();
        } else {
            actions->setActiveAction(reactionAction);
            QTest::keyClick(actions, Qt::Key_Right);
            QTRY_VERIFY_WITH_TIMEOUT(reactions->isVisible(), 1800);
            reactions->setActiveAction(allReactions);
            QTest::keyClick(reactions, Qt::Key_Return);
        }
        QTRY_VERIFY_WITH_TIMEOUT(visibleMessageMenu(QStringLiteral("reactionPalette")), 1800);
        auto *popup = visibleMessageMenu(QStringLiteral("reactionPalette"));
        QVERIFY(popup && popup->objectName() == QStringLiteral("reactionPalette"));
        auto *choices = findAccessible<QListWidget>(popup, QStringLiteral("Выберите реакцию"));
        QVERIFY(choices);
        QCOMPARE(choices->count(), 58);
        QSet<QString> keys;
        for (int row = 0; row < choices->count(); ++row) {
            const auto *item = choices->item(row);
            keys.insert(item->data(Qt::UserRole).toString());
            QVERIFY(!item->text().isEmpty() && !item->data(Qt::AccessibleTextRole).toString().isEmpty());
        }
        QCOMPARE(keys.size(), 58);
        QVERIFY(keys.contains(QStringLiteral("saluteFace")) && keys.contains(QStringLiteral("smilingFaceWithHalo")));
        choices->setCurrentRow(choices->count() - 1);
        QTest::keyClick(choices, Qt::Key_Return);
        QCOMPARE(backend->messageActionValue, QStringLiteral("smilingFaceWithHalo"));
        QCOMPARE(backend->messageAction, MessageAction::React);
        if (originalMenu)
            originalMenu->hide();
        QCoreApplication::processEvents();

        emit backend->connectionChanged(false);
        backend->messageActionId.clear();
        selectWord(history);
        copySelection(history, QStringLiteral("Готово"));
        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, chipPoint(history));
        QVERIFY(backend->messageActionId.isEmpty());
        emit backend->connectionChanged(true);

        auto nextMessage = message;
        nextMessage.id = QStringLiteral("702");
        selectWord(history);
        copySelection(history, QStringLiteral("Готово"));
        emit backend->chatMessagesLoaded(QStringLiteral("chat91"), {message, nextMessage});
        QTRY_COMPARE(history->count(), 2);
        copySelection(history, QStringLiteral("Готово"));

        auto revisedMessage = message;
        revisedMessage.text = QStringLiteral("Исправлено завтра\nнаписать отчёт");
        emit backend->chatMessagesLoaded(QStringLiteral("chat91"), {revisedMessage, nextMessage});
        QTRY_COMPARE(history->item(0)->data(workspace::Detail).toString(), revisedMessage.text);
        clipboard->clear();
        QTest::keyClick(history, Qt::Key_C, Qt::ControlModifier);
        QVERIFY(clipboard->text() != QStringLiteral("Готово"));

        QCoreApplication::processEvents();
        const auto firstBubble = bubble(history);
        const auto nextBubble = bubble(history, 1);
        const int edge = history->viewport()->rect().right();
        hover(history, QPoint(edge, firstBubble.center().y()));
        QTRY_VERIFY(more->isVisible());
        QTRY_VERIFY(firstBubble.contains(QPoint(firstBubble.center().x(), more->geometry().center().y())));
        hover(history, QPoint(edge, nextBubble.center().y()));
        QTRY_VERIFY(more->isVisible());
        QTRY_VERIFY(nextBubble.contains(QPoint(nextBubble.center().x(), more->geometry().center().y())));

        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        QApplication::setActiveWindow(card);
        auto *taskHistory = findAccessible<QListWidget>(card, QStringLiteral("Чат карточки"));
        QVERIFY(taskHistory);
        QTRY_COMPARE(taskHistory->count(), 1);
        selectWord(taskHistory);
        copySelection(taskHistory, QStringLiteral("Готово"));
        QVERIFY(backend->messageActionId.isEmpty());
        QTest::keyClick(taskHistory, Qt::Key_A, Qt::ControlModifier);
        copySelection(taskHistory, QStringLiteral("Готово\nОк"));
        QTest::mouseClick(taskHistory->viewport(), Qt::LeftButton, Qt::NoModifier, chipPoint(taskHistory));
        QCOMPARE(backend->messageActionDialog, QStringLiteral("chat67"));
        QCOMPARE(backend->messageActionId, taskMessage.id);
        QCOMPARE(backend->messageActionValue, QStringLiteral("fire"));
        QCOMPARE(backend->messageAction, MessageAction::RemoveReaction);

        auto *taskMore = findAccessible<QPushButton>(taskHistory, QStringLiteral("Действия сообщения"));
        QVERIFY(taskMore && taskMore->isHidden());
        card->resize(660, 500);
        QCoreApplication::processEvents();
        QVERIFY(taskHistory->viewport()->rect().contains(bodyPoint(taskHistory)));
        verifyRightSide(taskHistory, taskMore);
        hover(taskHistory, QPoint(taskHistory->viewport()->rect().right(), bubble(taskHistory).center().y()));
        QTRY_VERIFY_WITH_TIMEOUT(taskMore->isVisible(), 1800);
        QVERIFY(taskHistory->viewport()->rect().contains(taskMore->geometry()));
        if (!offscreen) {
            QTest::mouseMove(taskMore, taskMore->rect().center());
            QTRY_VERIFY(taskMore->isVisible());
        }
        QTest::mouseClick(taskMore, Qt::LeftButton);
        auto *taskMenu = visibleMessageMenu();
        QVERIFY(taskMenu && taskMenu->objectName() == QStringLiteral("messageContextMenu"));
        const auto taskActions = taskMenu->actions();
        QVERIFY(std::any_of(taskActions.cbegin(), taskActions.cend(), [](const QAction *action) {
            return action->text() == QStringLiteral("Реакция");
        }));
        taskMenu->hide();
        QCoreApplication::processEvents();
        hover(taskHistory, bodyPoint(taskHistory) + QPoint(1, 0));
        QTRY_VERIFY_WITH_TIMEOUT(taskMore->isVisible(), 1800);
        const auto taskPoint = bodyPoint(taskHistory);
        QWheelEvent wheel(taskPoint, taskHistory->viewport()->mapToGlobal(taskPoint), {}, QPoint(0, -120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(taskHistory->viewport(), &wheel);
        QVERIFY(taskMore->isHidden());
        hover(taskHistory, taskPoint + QPoint(2, 0));
        QTRY_VERIFY_WITH_TIMEOUT(taskMore->isVisible(), 1800);
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(taskHistory, &leave);
        QVERIFY(taskMore->isHidden());

        auto *entry = taskHistory->item(0);
        entry->setData(workspace::Outgoing, false);
        entry->setData(workspace::Meta, QString());
        entry->setData(workspace::MessageReactionCounts, QVariantMap());
        const auto modelIndex = taskHistory->model()->index(0, 0);
        const auto *delegate = taskHistory->itemDelegate();
        const QStyleOptionViewItem option;
        const int height = delegate->sizeHint(option, modelIndex).height();
        entry->setData(workspace::ReceiptMessageId, QString());
        QCOMPARE(delegate->sizeHint(option, modelIndex).height(), height);
    }

    void ranksConfirmedReactionAdditionsPerAccount()
    {
        const auto dialog = QStringLiteral("chat91");
        const auto id = QStringLiteral("701");
        const auto neutral = QStringLiteral("neutralFace");
        const auto heart = QStringLiteral("redHeart");
        const auto like = QStringLiteral("like");
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        backend->accountScope = QStringLiteral("reaction-frequency-portal-account-one");
        backend->chats.first().id = dialog;
        backend->messages.first().id = id;

        const auto getQuickMenu = [](MainWindow &window) -> QMenu * {
            auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
            if (!history || !history->count())
                return nullptr;
            history->setCurrentRow(0);
            const auto point = history->visualItemRect(history->item(0)).center();
            QContextMenuEvent event(QContextMenuEvent::Keyboard, point, history->viewport()->mapToGlobal(point));
            QApplication::sendEvent(history->viewport(), &event);
            const auto *menu = visibleMessageMenu();
            if (menu) {
                for (auto *action : menu->actions()) {
                    if (action->text() == QStringLiteral("Реакция"))
                        return action->menu();
                }
            }
            return nullptr;
        };
        const auto checkFirst = [&](MainWindow &window, const QString &code) {
            auto *quick = getQuickMenu(window);
            QVERIFY(quick);
            const auto label = quick->actions().first()->text();
            qobject_cast<QWidget *>(quick->parent())->hide();
            QVERIFY2(label.startsWith(workspace::reactionGlyph(code)), qPrintable(label));
        };
        const auto triggerQuick = [&](MainWindow &window, const QString &code) {
            auto *quick = getQuickMenu(window);
            QVERIFY(quick);
            QAction *choice = nullptr;
            for (auto *action : quick->actions()) {
                if (action->text().startsWith(workspace::reactionGlyph(code))) {
                    choice = action;
                    break;
                }
            }
            QVERIFY(choice);
            choice->trigger();
            qobject_cast<QWidget *>(quick->parent())->hide();
            QCOMPARE(backend->messageActionValue, code);
        };
        {
            MainWindow window(backend.get());
            window.show();
            QApplication::setActiveWindow(&window);
            emit backend->connectionChanged(true);
            QTRY_VERIFY(findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"))->count() > 0);
            checkFirst(window, like);
            triggerQuick(window, heart);
            emit backend->chatOwnReactionChanged(QStringLiteral("chat92"), id, heart, true);
            checkFirst(window, like);
            emit backend->failed(Backend::reactionOperationKey(dialog, id, heart), QStringLiteral("Отклонено"));
            emit backend->chatOwnReactionChanged(dialog, id, heart, true);
            checkFirst(window, like);
            emit backend->chatOwnReactionChanged(dialog, id, heart, false);

            triggerQuick(window, neutral);
            emit backend->chatOwnReactionChanged(dialog, id, neutral, true);
            emit backend->chatOwnReactionChanged(dialog, id, neutral, true);
            checkFirst(window, neutral);
            triggerQuick(window, neutral);
            QCOMPARE(backend->messageAction, MessageAction::RemoveReaction);
            emit backend->chatOwnReactionChanged(dialog, id, neutral, false);
            checkFirst(window, neutral);

            triggerQuick(window, heart);
            triggerQuick(window, neutral);
            emit backend->chatOwnReactionChanged(dialog, id, neutral, true);
            emit backend->chatOwnReactionChanged(dialog, id, heart, true);
            checkFirst(window, neutral);

            triggerQuick(window, heart);
            emit backend->chatOwnReactionChanged(dialog, id, heart, false);
            triggerQuick(window, heart);
            emit backend->chatOwnReactionChanged(dialog, id, heart, true);
            checkFirst(window, heart);
        }
        {
            MainWindow window(backend.get());
            window.show();
            QApplication::setActiveWindow(&window);
            emit backend->connectionChanged(true);
            QTRY_VERIFY(findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"))->count() > 0);
            checkFirst(window, heart);
            backend->accountScope = QStringLiteral("reaction-frequency-portal-account-two");
            emit backend->accountScopeChanged();
            emit backend->recentChatsLoaded(backend->chats);
            checkFirst(window, like);
        }

        const auto halo = QStringLiteral("smilingFaceWithHalo");
        ui::ReactionPalette palette({}, ui::rankedReactionChoices({{heart, 2}, {halo, 3}}), nullptr);
        const auto *choices = findAccessible<QListWidget>(&palette, QStringLiteral("Выберите реакцию"));
        QVERIFY(choices);
        QCOMPARE(choices->count(), 58);
        QCOMPARE(choices->item(0)->data(Qt::UserRole).toString(), halo);
        QCOMPARE(choices->item(1)->data(Qt::UserRole).toString(), heart);
    }

    void exposesReactionAndMessagePermalinkFromHistoryMenu()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        backend->chats.first().id = QStringLiteral("chat91");
        backend->messages.first().id = QStringLiteral("701");
        backend->messages.first().outgoing = true;
        backend->messages.first().reactions = {{QStringLiteral("like"), 2, true}};
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        emit backend->connectionChanged(true);
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        QTRY_COMPARE(history->count(), 1);
        const auto getMenu = [history] {
            history->setCurrentRow(0);
            const auto point = history->visualItemRect(history->item(0)).center();
            QContextMenuEvent event(QContextMenuEvent::Keyboard, point, history->viewport()->mapToGlobal(point));
            QApplication::sendEvent(history->viewport(), &event);
            return visibleMessageMenu();
        };
        auto *menu = getMenu();
        QVERIFY(menu);
        const auto findAction = [](QMenu *menu, const QString &label) {
            for (auto *action : menu->actions()) {
                if (action->text() == label)
                    return action;
            }
            return static_cast<QAction *>(nullptr);
        };
        auto *reaction = findAction(menu, QStringLiteral("Реакция"));
        QVERIFY(reaction && reaction->menu());
        auto *own = reaction->menu()->actions().first();
        QVERIFY(own->isChecked());
        own->trigger();
        QCOMPARE(backend->messageActionDialog, QStringLiteral("chat91"));
        QCOMPARE(backend->messageActionId, QStringLiteral("701"));
        QCOMPARE(backend->messageActionValue, QStringLiteral("like"));
        QCOMPARE(backend->messageAction, MessageAction::RemoveReaction);
        auto *copy = findAction(menu, QStringLiteral("Скопировать ссылку на сообщение"));
        QVERIFY(copy);
        copy->trigger();
        QCOMPARE(QGuiApplication::clipboard()->text(),
                 QStringLiteral("https://example.bitrix24.ru/online/?IM_DIALOG=chat91&IM_MESSAGE=701"));
        menu->hide();
        emit backend->chatMessagesSeen(QStringLiteral("chat92"), {QStringLiteral("701")});
        QVERIFY(!history->item(0)->data(workspace::MessageSeen).toBool());
        emit backend->chatMessagesSeen(QStringLiteral("chat91"), {QStringLiteral("701")});
        QVERIFY(history->item(0)->data(workspace::MessageSeen).toBool());
    }

    void confirmsOnlyTheOriginalMessageWithoutClosingWorkspace_data()
    {
        QTest::addColumn<bool>("taskChat");
        QTest::newRow("ordinary-chat") << false;
        QTest::newRow("task-card-chat") << true;
    }

    void confirmsOnlyTheOriginalMessageWithoutClosingWorkspace()
    {
        QFETCH(bool, taskChat);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        backend->accountScope = QStringLiteral("fixture/delete-message");
        const auto dialog = taskChat ? QStringLiteral("chat67") : QStringLiteral("chat91");
        const auto messageId = QStringLiteral("701");
        backend->chats.first().id = QStringLiteral("chat91");
        Message message;
        message.id = messageId;
        message.outgoing = true;
        message.text = QStringLiteral("Сообщение для удаления");
        Message neighbour = message;
        neighbour.id = QStringLiteral("702");
        neighbour.text = QStringLiteral("Оставить это сообщение");
        backend->messages = {message, neighbour};
        backend->taskMessages = backend->messages;
        backend->stages.first().tasks.first().chatDialogId = dialog;
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        emit backend->connectionChanged(true);
        QWidget *owner = &window;
        if (taskChat) {
            window.openTaskCard(QStringLiteral("501"));
            owner = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
            QVERIFY(owner && owner->isVisible());
        }
        auto *history = findAccessible<QListWidget>(owner, taskChat ? QStringLiteral("Чат карточки")
                                                                   : QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        QTRY_COMPARE(history->count(), 2);
        const auto selectDelete = [&](bool confirm, bool replace) {
            history->setCurrentRow(0);
            const auto point = history->visualItemRect(history->item(0)).center();
            QContextMenuEvent event(QContextMenuEvent::Keyboard, point, history->viewport()->mapToGlobal(point));
            QApplication::sendEvent(history->viewport(), &event);
            auto *menu = visibleMessageMenu();
            QVERIFY(menu);
            QAction *remove = nullptr;
            for (auto *entry : menu->actions())
                if (entry->text() == QStringLiteral("Удалить…"))
                    remove = entry;
            QVERIFY(remove);
            menu->setActiveAction(remove);
            bool answered = false;
            bool correctParent = false;
            bool defaultCancel = false;
            bool idleBeforeAnswer = false;
            QTimer probe;
            connect(&probe, &QTimer::timeout, &window, [&] {
                auto *question = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                if (!question || question->windowTitle() != QStringLiteral("Удалить сообщение"))
                    return;
                probe.stop();
                answered = true;
                correctParent = question->parentWidget() == owner;
                idleBeforeAnswer = backend->messageActionId.isEmpty();
                auto *cancel = question->button(QMessageBox::Cancel);
                if (!cancel)
                    cancel = question->button(QMessageBox::No);
                defaultCancel = question->defaultButton() == cancel;
                if (replace)
                    history->item(0)->setData(workspace::ReceiptMessageId, QStringLiteral("999"));
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                if (auto *button = confirm ? question->button(QMessageBox::Yes) : cancel)
                    button->click();
            });
            probe.start(1);
            QTest::keyClick(menu, Qt::Key_Return);
            QTRY_VERIFY(answered);
            QVERIFY(correctParent);
            QVERIFY(defaultCancel);
            QVERIFY(idleBeforeAnswer);
            QVERIFY(window.isVisible() && owner->isVisible());
        };
        selectDelete(false, false);
        QVERIFY(backend->messageActionId.isEmpty());
        selectDelete(true, true);
        QVERIFY(backend->messageActionId.isEmpty());
        history->item(0)->setData(workspace::ReceiptMessageId, messageId);
        selectDelete(true, false);
        QCOMPARE(backend->messageActionDialog, dialog);
        QCOMPARE(backend->messageActionId, messageId);
        QCOMPARE(backend->messageAction, MessageAction::Delete);
        emit backend->chatHistoryRemoved(dialog, {messageId});
        QTRY_COMPARE(history->count(), 1);
        QCOMPARE(history->item(0)->data(workspace::ReceiptMessageId).toString(), neighbour.id);
        QVERIFY(window.isVisible() && owner->isVisible());
    }

    void inlineMessageEditingRestoresDraftAndKeepsTheTaskCardOpen()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        backend->chats.first().id = QStringLiteral("chat91");
        auto &message = backend->messages.first();
        message.id = QStringLiteral("701");
        message.text = QStringLiteral("Первая строка\nВторая строка");
        message.outgoing = true;
        auto taskMessage = message;
        taskMessage.id = QStringLiteral("801");
        backend->taskMessages = {taskMessage};
        backend->stages.first().tasks.first().chatDialogId = QStringLiteral("chat67");
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        emit backend->connectionChanged(true);

        const auto edit = [](QListWidget *history) {
            history->setCurrentRow(0);
            const auto point = history->visualItemRect(history->item(0)).center();
            QContextMenuEvent event(QContextMenuEvent::Keyboard, point, history->viewport()->mapToGlobal(point));
            QApplication::sendEvent(history->viewport(), &event);
            auto *menu = visibleMessageMenu();
            QVERIFY(menu);
            auto *action = [&]() -> QAction * {
                for (auto *candidate : menu->actions())
                    if (candidate->text() == QStringLiteral("Редактировать…"))
                        return candidate;
                return nullptr;
            }();
            QVERIFY(action);
            QTimer::singleShot(0, [] {
                auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (modal && modal->inherits("QInputDialog"))
                    modal->reject();
            });
            menu->hide();
            action->trigger();
        };
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *composer = dynamic_cast<ui::ChatComposer *>(
            window.findChild<QFrame *>(QStringLiteral("chatComposer")));
        QVERIFY(history && composer);
        QTRY_COMPARE(history->count(), 1);
        ui::ChatReply draftReply;
        draftReply.id = QStringLiteral("700");
        draftReply.author = QStringLiteral("Автор");
        draftReply.preview = QStringLiteral("Исходное сообщение");
        composer->setDraft({QStringLiteral("Мой черновик"), draftReply, {}});
        edit(history);
        QCOMPARE(composer->input()->toPlainText(), message.text);
        QVERIFY(composer->reply().id.isEmpty());
        QTest::keyClick(composer->input(), Qt::Key_Escape);
        QCOMPARE(composer->input()->toPlainText(), QStringLiteral("Мой черновик"));
        QCOMPARE(composer->reply().id, draftReply.id);
        QVERIFY(backend->messageActionId.isEmpty());
        edit(history);
        composer->input()->moveCursor(QTextCursor::End);
        QTest::keyClick(composer->input(), Qt::Key_Return, Qt::ShiftModifier);
        QTest::keyClicks(composer->input(), QStringLiteral("line 3"));
        QTest::keyClick(composer->input(), Qt::Key_Return);
        QCOMPARE(backend->messageAction, MessageAction::Edit);
        QCOMPARE(backend->messageActionId, QStringLiteral("701"));
        const auto text = message.text + QStringLiteral("\nline 3");
        QCOMPARE(backend->messageActionValue, text);
        emit backend->failed(QStringLiteral("message-edit:chat91:701"), QStringLiteral("Не удалось сохранить"));
        QCOMPARE(composer->input()->toPlainText(), text);
        QTest::keyClick(composer->input(), Qt::Key_Return);
        emit backend->messageEditAccepted(QStringLiteral("chat91"), QStringLiteral("701"), text);
        QCOMPARE(composer->input()->toPlainText(), QStringLiteral("Мой черновик"));
        QCOMPARE(composer->reply().id, draftReply.id);
        QCOMPARE(history->item(0)->data(workspace::RawMessageBody).toString(), text);

        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        QApplication::setActiveWindow(card);
        auto *taskHistory = findAccessible<QListWidget>(card, QStringLiteral("Чат карточки"));
        auto *taskComposer = dynamic_cast<ui::ChatComposer *>(
            card->findChild<QFrame *>(QStringLiteral("chatComposer")));
        QVERIFY(taskHistory && taskComposer);
        QTRY_COMPARE(taskHistory->count(), 1);
        taskComposer->input()->setPlainText(QStringLiteral("Черновик задачи"));
        edit(taskHistory);
        QCOMPARE(taskComposer->input()->toPlainText(), taskMessage.text);
        QTest::keyClick(taskComposer->input(), Qt::Key_Escape);
        QVERIFY(card->isVisible());
        QCOMPARE(taskComposer->input()->toPlainText(), QStringLiteral("Черновик задачи"));
    }

    void chatWheelUsesCompactNotchesAndKeepsNativePreciseScrolling()
    {
        ui::HistoryList history(nullptr);
        QListWidget native;
        QLabel state;
        QLabel nativeState;
        const auto scope = QStringLiteral("chat51");
        QVector<Message> messages;
        for (int index = 101; index <= 140; ++index) {
            Message message;
            message.id = QString::number(index);
            message.text = QStringLiteral("Сообщение %1").arg(index);
            messages.append(message);
        }
        for (auto *list : {static_cast<QListWidget *>(&history), &native}) {
            workspace::configureRows(list, workspace::Row::Message);
            list->resize(480, 260);
            list->show();
        }
        history.showHistory(scope, &state, messages);
        ui::presentMessages(&native, &nativeState, messages);
        QTRY_VERIFY(history.verticalScrollBar()->maximum() > 500);
        QTRY_COMPARE(native.verticalScrollBar()->maximum(), history.verticalScrollBar()->maximum());
        const auto send = [](QListWidget &list, QPoint angle, QPoint pixels = {},
                             Qt::ScrollPhase phase = Qt::NoScrollPhase,
                             Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
            auto *view = list.viewport();
            const auto pos = view->rect().center();
            QWheelEvent wheel(pos, view->mapToGlobal(pos), pixels, angle, Qt::NoButton, modifiers, phase, false);
            QApplication::sendEvent(view, &wheel);
        };
        const auto movement = [&send](QListWidget &list, QPoint angle, QPoint pixels = {},
                                     Qt::ScrollPhase phase = Qt::NoScrollPhase,
                                     Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
            auto *scroll = list.verticalScrollBar();
            const int start = scroll->maximum() / 2;
            scroll->setValue(start);
            send(list, angle, pixels, phase, modifiers);
            return scroll->value() - start;
        };
        const int nativeNotch = movement(native, {0, -120});
        QVERIFY(nativeNotch > 3);
        const int chatNotch = qRound(nativeNotch / 8.0);
        auto *scroll = history.verticalScrollBar();
        auto &wheel = testClock(history);
        const auto finishNotch = [&wheel] {
            QCOMPARE(wheel.state(), QAbstractAnimation::Running);
            wheel.setCurrentTime(wheel.duration());
        };
        const int middle = scroll->maximum() / 2;
        scroll->setValue(middle);
        send(history, {0, -120});
        QCOMPARE(scroll->value(), middle);
        finishNotch();
        QCOMPARE(scroll->value(), middle + chatNotch);
        send(history, {0, 120});
        QCOMPARE(scroll->value(), middle + chatNotch);
        finishNotch();
        QCOMPARE(scroll->value(), middle);

        send(history, {0, -120});
        send(history, {0, -120});
        QCOMPARE(scroll->value(), middle);
        finishNotch();
        QCOMPARE(scroll->value(), middle + 2 * chatNotch);

        const int beforeChange = scroll->value();
        send(history, {0, -120});
        send(history, {0, 120});
        finishNotch();
        QCOMPARE(scroll->value(), beforeChange - chatNotch);

        const auto precisePixels = QPoint(0, -17);
        const int preciseMovement = movement(native, {0, -120}, precisePixels, Qt::ScrollUpdate);
        const int beforePrecise = scroll->value();
        send(history, {0, -120});
        send(history, {0, -120}, precisePixels, Qt::ScrollUpdate);
        QCOMPARE(wheel.state(), QAbstractAnimation::Stopped);
        QCOMPARE(scroll->value(), beforePrecise + preciseMovement);

        send(history, {0, -120});
        scroll->setValue(middle);
        QCOMPARE(wheel.state(), QAbstractAnimation::Stopped);
        QCOMPARE(scroll->value(), middle);
        QVERIFY(qAbs(movement(history, {0, -30}) - movement(native, {0, -30})) <= 1);
        QCOMPARE(movement(history, {0, -120}, {}, Qt::NoScrollPhase, Qt::ControlModifier),
                 movement(native, {0, -120}, {}, Qt::NoScrollPhase, Qt::ControlModifier));

        QStringList older;
        history.setOnOlder([&older](const QString &before) { older.append(before); });
        const int nearTop = scroll->minimum() + qMax(1, chatNotch / 2);
        scroll->setValue(nearTop);
        send(history, {0, 120});
        send(history, {0, 120});
        send(history, {0, -120});
        finishNotch();
        QCOMPARE(scroll->value(), nearTop + chatNotch);
        QVERIFY(older.isEmpty());

        scroll->setValue(nearTop);
        send(history, {0, 120});
        send(history, {0, 120});
        finishNotch();
        QCOMPARE(scroll->value(), scroll->minimum());
        QTRY_COMPARE_WITH_TIMEOUT(older, QStringList{QStringLiteral("101")}, 500);
        send(history, {0, 120});
        QCOMPARE(older, QStringList{QStringLiteral("101")});
    }

    void olderPagesContinuePastCacheLimitAndFromMessageContext()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        const auto dialog = backend->chats.first().id;
        const auto taskDialog = QStringLiteral("chat51");
        backend->stages.first().tasks.first().chatDialogId = taskDialog;
        const auto message = [](int id) {
            Message value;
            value.id = QString::number(id);
            value.text = QStringLiteral("Сообщение %1").arg(id);
            return value;
        };
        backend->messages.clear();
        for (int id = 101; id <= 300; ++id)
            backend->messages.append(message(id));
        backend->taskMessages = {message(400)};
        backend->pinnedMessages.insert(dialog, {message(90)});
        backend->pinnedMessages.insert(taskDialog, {message(95)});
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        const auto rowId = [](QListWidget *history, int row) {
            return history->item(row)->data(workspace::Id).toString();
        };
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *pager = window.findChild<QPushButton *>(QStringLiteral("olderChatMessages"));
        auto *latest = findAccessible<QPushButton>(&window, QStringLiteral("К последним сообщениям"));
        QVERIFY(history && pager && latest);
        QTRY_COMPARE(history->count(), 200);
        history->scrollToTop();
        QVERIFY(pager->isVisible());
        pager->click();
        QCOMPARE(backend->olderBefore, QStringLiteral("101"));
        emit backend->olderChatMessagesLoaded(dialog, QStringLiteral("101"), false, {message(100)}, true);
        QCOMPARE(history->count(), 200);
        QCOMPARE(rowId(history, 0), QStringLiteral("100"));
        QCOMPARE(rowId(history, 199), QStringLiteral("299"));
        QVERIFY(pager->isVisible() && latest->isVisible());
        emit backend->chatMessagesLoaded(dialog, backend->messages);
        QCOMPARE(rowId(history, 0), QStringLiteral("100"));
        latest->click();
        QTRY_COMPARE(history->count(), 200);
        QCOMPARE(rowId(history, 0), QStringLiteral("101"));
        QVERIFY(!latest->isVisible());

        auto *pin = findAccessible<QPushButton>(&window, QStringLiteral("Закреплённые сообщения"));
        QVERIFY(pin && pin->isVisible());
        pin->click();
        QCOMPARE(backend->contextMessage, QStringLiteral("90"));
        emit backend->messageContextLoaded(dialog, QStringLiteral("90"), {message(89), message(90), message(91)});
        QVERIFY(pager->isVisible());
        pager->click();
        QCOMPARE(backend->olderBefore, QStringLiteral("89"));
        emit backend->olderChatMessagesLoaded(dialog, QStringLiteral("89"), false, {message(88)}, false);
        QCOMPARE(rowId(history, 0), QStringLiteral("88"));
        QVERIFY(!pager->isVisible() && latest->isVisible());

        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        auto *taskPin = findAccessible<QPushButton>(card, QStringLiteral("Закреплённые сообщения"));
        auto *taskPager = card->findChild<QPushButton *>(QStringLiteral("olderChatMessages"));
        auto *taskHistory = findAccessible<QListWidget>(card, QStringLiteral("Чат карточки"));
        QVERIFY(taskPin && taskPin->isVisible() && taskPager && taskHistory);
        taskPin->click();
        QCOMPARE(backend->contextMessage, QStringLiteral("95"));
        emit backend->messageContextLoaded(taskDialog, QStringLiteral("95"), {message(94), message(95)});
        QVERIFY(taskPager->isVisible());
        taskPager->click();
        QCOMPARE(backend->olderScope, QStringLiteral("501"));
        QCOMPARE(backend->olderBefore, QStringLiteral("94"));
        QVERIFY(backend->olderTask);
        emit backend->olderChatMessagesLoaded(QStringLiteral("501"), QStringLiteral("94"), true, {message(93)}, false);
        QCOMPARE(rowId(taskHistory, 0), QStringLiteral("93"));
        emit backend->pinnedMessagesLoaded(taskDialog, {message(92)});
        taskPin->click();
        QCOMPARE(backend->contextMessage, QStringLiteral("92"));
        emit backend->chatHistoryRemoved(taskDialog, {});
        emit backend->messageContextLoaded(taskDialog, QStringLiteral("92"), {message(92)});
        QCOMPARE(taskHistory->count(), 0);

        emit backend->pinnedMessagesLoaded(dialog, {message(87)});
        pin->click();
        QCOMPARE(backend->contextMessage, QStringLiteral("87"));
        emit backend->chatHistoryRemoved(dialog, {});
        emit backend->messageContextLoaded(dialog, QStringLiteral("87"), {message(87)});
        QCOMPARE(history->count(), 0);
    }

    void closingTaskCardAndSwitchingChatsReportTheirOwnHistory()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        MainWindow window(backend.get());
        window.show();
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("chatScopeTabs"));
        QVERIFY(history && tabs);
        QTRY_COMPARE(history->count(), 1);
        const auto messageId = history->item(0)->data(workspace::ReceiptMessageId);

        QSignalSpy closed(&window, &MainWindow::historyClosed);
        const auto taskId = backend->stages.first().tasks.first().id;
        window.openTaskCard(taskId);
        QPointer<QDialog> card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card && card->isVisible());
        const auto draft = QStringLiteral("Черновик комментария");
        auto *input = findAccessible<QPlainTextEdit>(card, QStringLiteral("Текст в чате карточки"));
        QVERIFY(input);
        input->setPlainText(draft);
        const auto reads = backend->taskReadRequests.count(taskId);
        window.openTaskCard(taskId);
        QCOMPARE(backend->taskReadRequests.count(taskId), reads);
        QCOMPARE(window.findChildren<QDialog *>(QStringLiteral("taskCardDialog")).size(), 1);
        QCOMPARE(input->toPlainText(), draft);
        QVERIFY(closed.isEmpty());
        static_cast<ui::TaskCardDialog *>(card.data())->setDiscardConfirmation([] { return true; });
        QVERIFY(card->close());
        QCOMPARE(closed.size(), 1);
        QCOMPARE(closed.takeFirst().first().toBool(), true);
        window.openTaskCard(taskId);
        QTRY_VERIFY(card.isNull());
        auto *reopened = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(reopened && reopened->isVisible());
        input = findAccessible<QPlainTextEdit>(reopened, QStringLiteral("Текст в чате карточки"));
        QVERIFY(input);
        QCOMPARE(input->toPlainText(), draft);
        input->clear();
        QVERIFY(reopened->close());
        QCOMPARE(closed.size(), 1);
        QCOMPARE(closed.takeFirst().first().toBool(), true);
        QCOMPARE(history->count(), 1);
        QCOMPARE(history->item(0)->data(workspace::ReceiptMessageId), messageId);

        tabs->setCurrentIndex(1);
        QCOMPARE(closed.size(), 1);
        QCOMPARE(closed.takeFirst().first().toBool(), false);
    }

    void earlierHistoryKeepsViewportAndChatScope()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto other = backend->chats.first();
        other.id = QStringLiteral("chat42");
        other.title = QStringLiteral("Другая переписка");
        backend->chats.append(other);
        const auto message = [](int id) {
            Message entry;
            entry.id = QString::number(id);
            entry.text = QStringLiteral("Сообщение №%1").arg(id);
            return entry;
        };
        backend->messages.clear();
        for (int id = 101; id <= 125; ++id)
            backend->messages.append(message(id));
        backend->linkedMessages = {message(501)};
        backend->taskMessages = {message(601)};
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *editor = findAccessible<QPlainTextEdit>(&window, QStringLiteral("Текст сообщения"));
        auto *pager = window.findChild<QPushButton *>(QStringLiteral("olderChatMessages"));
        QVERIFY(chats && history && editor && pager);
        QTRY_COMPARE(history->count(), 25);
        history->scrollToTop();
        editor->setPlainText(QStringLiteral("Сохранённый черновик"));
        QTRY_VERIFY(pager->isVisible());
        pager->click();
        QCOMPARE(backend->olderBefore, QStringLiteral("101"));
        QVERIFY(!backend->olderTask);
        QVERIFY(!pager->isEnabled());
        emit backend->olderChatMessagesLoaded(QStringLiteral("c-1"), QStringLiteral("999"), false,
                                              {message(31)}, true);
        QCOMPARE(history->count(), 25);
        emit backend->olderChatMessagesLoaded(QStringLiteral("c-1"), QStringLiteral("101"), false,
                                              {message(99), message(100), message(101)}, true);
        QTRY_COMPARE(history->count(), 27);
        QCOMPARE(history->item(0)->data(workspace::Id).toString(), QStringLiteral("99"));
        QTRY_VERIFY(history->itemAt(QPoint(4, 1)));
        QCOMPARE(history->itemAt(QPoint(4, 1))->data(workspace::Id).toString(), QStringLiteral("101"));
        QCOMPARE(editor->toPlainText(), QStringLiteral("Сохранённый черновик"));

        auto latest = backend->messages;
        latest.removeFirst();
        latest.append(message(126));
        emit backend->chatMessagesLoaded(QStringLiteral("c-1"), latest);
        QCOMPARE(history->count(), 28);
        QCOMPARE(history->item(27)->data(workspace::Id).toString(), QStringLiteral("126"));
        latest.removeFirst();
        emit backend->chatMessagesLoaded(QStringLiteral("c-1"), latest);
        QCOMPARE(history->count(), 27);
        QCOMPARE(history->item(3)->data(workspace::Id).toString(), QStringLiteral("103"));
        pager->click();
        QCOMPARE(backend->olderBefore, QStringLiteral("99"));
        emit backend->chatHistoryRemoved(QStringLiteral("c-1"), {QStringLiteral("97"), QStringLiteral("98")});
        emit backend->olderChatMessagesLoaded(QStringLiteral("c-1"), QStringLiteral("99"), false,
                                              {message(97), message(98)}, true);
        QCOMPARE(history->item(0)->data(workspace::Id).toString(), QStringLiteral("99"));
        pager->click();
        QCOMPARE(backend->olderBefore, QStringLiteral("97"));
        emit backend->olderChatMessagesLoaded(QStringLiteral("c-1"), QStringLiteral("97"), false,
                                              {message(96)}, true);
        QCOMPARE(history->item(0)->data(workspace::Id).toString(), QStringLiteral("96"));
        emit backend->chatHistoryRemoved(QStringLiteral("c-1"), {}, 100);
        QCOMPARE(history->item(0)->data(workspace::Id).toString(), QStringLiteral("101"));
        QVERIFY(!pager->isVisible());
        emit backend->chatHistoryRemoved(QStringLiteral("c-1"), {});
        QVERIFY(history->count() == 0);
        chats->setCurrentRow(1);
        QTRY_COMPARE(history->count(), 1);
        emit backend->olderChatMessagesLoaded(QStringLiteral("c-1"), QStringLiteral("101"), false,
                                              {message(98)}, false);
        QCOMPARE(history->item(0)->data(workspace::Id).toString(), QStringLiteral("501"));

        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        auto *taskHistory = findAccessible<QListWidget>(card, QStringLiteral("Чат карточки"));
        auto *taskPager = card->findChild<QPushButton *>(QStringLiteral("olderChatMessages"));
        QVERIFY(taskHistory && taskPager);
        QTRY_VERIFY(taskPager->isVisible());
        taskPager->click();
        QCOMPARE(backend->olderScope, QStringLiteral("501"));
        QCOMPARE(backend->olderBefore, QStringLiteral("601"));
        QVERIFY(backend->olderTask);
        emit backend->olderChatMessagesLoaded(QStringLiteral("501"), QStringLiteral("601"), true,
                                              {message(600)}, false);
        QTRY_COMPARE(taskHistory->count(), 2);
        QCOMPARE(taskHistory->item(0)->data(workspace::Id).toString(), QStringLiteral("600"));
        QVERIFY(!taskPager->isVisible());
    }

    void onlyVisibleForegroundHistoryMarksActualMessageIds()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->messages.clear();
        for (int index = 0; index < 32; ++index) {
            Message message;
            message.id = QString::number(1000 + index);
            message.text = QStringLiteral("Сообщение %1").arg(index);
            backend->messages.append(message);
        }
        backend->messages.last().outgoing = true;
        Message withoutId;
        withoutId.text = QStringLiteral("Нет идентификатора портала");
        backend->messages.append(withoutId);
        for (const auto &id : {QStringLiteral("2001"), QStringLiteral("2002")}) {
            Message taskMessage;
            taskMessage.id = id;
            taskMessage.text = QStringLiteral("Сообщение в задаче");
            backend->taskMessages.append(taskMessage);
        }
        MainWindow window(backend.get());
        window.resize(930, 555);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *search = findAccessible<QLineEdit>(&window, QStringLiteral("Поиск по сообщениям"));
        auto *searchToggle = findAccessible<QPushButton>(&window, QStringLiteral("Найти в чате"));
        QVERIFY(history && search && searchToggle);
        deliverPendingTimer(window, QStringLiteral("visibleChatReadTimer"));
        QVERIFY(!backend->reads.isEmpty());
        const auto last = backend->reads.constLast();
        QCOMPARE(last.scope, QStringLiteral("c-1"));
        QVERIFY(!last.task && last.visible.contains(QStringLiteral("1031")));
        QVERIFY(!last.incoming.contains(QStringLiteral("1031")));
        QVERIFY(!last.visible.contains(QStringLiteral("1000")));
        QVERIFY(!last.visible.contains(QStringLiteral("33")));

        QWidget other;
        other.show();
        QApplication::setActiveWindow(&other);
        const auto beforeBackground = backend->reads.size();
        history->scrollToTop();
        deliverPendingTimer(window, QStringLiteral("visibleChatReadTimer"));
        QCOMPARE(backend->reads.size(), beforeBackground);
        other.close();
        QApplication::setActiveWindow(&window);
        history->setFocus(Qt::OtherFocusReason);
        deliverPendingTimer(window, QStringLiteral("visibleChatReadTimer"));
        QVERIFY(backend->reads.size() > beforeBackground);
        QVERIFY(backend->reads.constLast().incoming.contains(QStringLiteral("1000")));
        QVERIFY(!backend->reads.constLast().visible.contains(QStringLiteral("1031")));

        const auto beforeSearch = backend->reads.size();
        searchToggle->click();
        const auto query = QStringLiteral("Проверка");
        search->setText(query);
        QTest::keyClick(search, Qt::Key_Return);
        Message found;
        found.id = QStringLiteral("9001");
        found.text = query;
        emit backend->messagesFound(QStringLiteral("c-1"), query, false, {found});
        QTRY_COMPARE(history->count(), 1);
        deliverPendingTimer(window, QStringLiteral("visibleChatReadTimer"));
        QCOMPARE(backend->reads.size(), beforeSearch);

        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        sections->setCurrentRow(1);
        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        QApplication::setActiveWindow(card);
        deliverPendingTimer(window, QStringLiteral("visibleChatReadTimer"));
        QVERIFY(backend->reads.size() > beforeSearch);
        const auto task = backend->reads.constLast();
        QVERIFY(task.task);
        QCOMPARE(task.scope, QStringLiteral("501"));
        QCOMPARE(task.incoming, QStringList({QStringLiteral("2001"), QStringLiteral("2002")}));
    }

    void visibleHistoryRetriesReadOnReconnectAndReselect()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        Message message;
        message.id = QStringLiteral("701");
        message.text = QStringLiteral("Новое сообщение");
        message.unread = true;
        backend->messages = {message};
        MainWindow window(backend.get());
        window.resize(930, 555);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
        QVERIFY(history && chats);
        QTRY_COMPARE(history->count(), 1);
        deliverPendingTimer(window, QStringLiteral("visibleChatReadTimer"));
        QVERIFY(backend->reads.isEmpty());

        emit backend->connectionChanged(true);
        deliverPendingTimer(window, QStringLiteral("visibleChatReadTimer"));
        QVERIFY(!backend->reads.isEmpty());
        QCOMPARE(backend->reads.last().scope, QStringLiteral("c-1"));
        QCOMPARE(backend->reads.last().incoming, QStringList{message.id});
        const auto before = backend->reads.size();
        QVERIFY(chats->currentItem());
        QTest::mouseClick(chats->viewport(), Qt::LeftButton, Qt::NoModifier,
                          chats->visualItemRect(chats->currentItem()).center());
        deliverPendingTimer(window, QStringLiteral("visibleChatReadTimer"));
        QVERIFY(backend->reads.size() > before);
        QCOMPARE(backend->reads.last().incoming, QStringList{message.id});
    }

    void liveUnreadDividerAdvancesOnlyOnMatchingConfirmedRead()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        for (int id = 701; id <= 719; ++id) {
            Message incoming;
            incoming.id = QString::number(id);
            incoming.text = QStringLiteral("Непрочитанное");
            incoming.unread = true;
            backend->messages.append(incoming);
        }
        backend->stages.first().tasks.first().chatDialogId = QStringLiteral("chat67");
        backend->taskMessages = {backend->messages.at(1), backend->messages.at(2)};
        MainWindow window(backend.get());
        window.resize(930, 555);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        const auto divider = [](QListWidget *list) {
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->data(workspace::UnreadDivider).toBool())
                    return list->item(i)->data(workspace::ReceiptMessageId).toString();
            return QString();
        };
        QTRY_COMPARE(divider(history), QStringLiteral("701"));
        emit backend->chatReadConfirmed(QStringLiteral("chat999"), {QStringLiteral("701")}, 0);
        QCOMPARE(divider(history), QStringLiteral("701"));
        emit backend->chatReadConfirmed(QStringLiteral("c-1"), {QStringLiteral("701"), QStringLiteral("703")}, 0);
        QCOMPARE(divider(history), QStringLiteral("702"));
        QVERIFY(!history->item(1)->data(Qt::AccessibleTextRole).toString().startsWith(workspace::UnreadMessages));
        QVERIFY(history->item(2)->data(Qt::AccessibleTextRole).toString().startsWith(workspace::UnreadMessages));
        emit backend->chatReadConfirmed(QStringLiteral("c-1"), {}, 717);
        QCOMPARE(divider(history), QStringLiteral("718"));
        QVERIFY(history->item(history->count() - 1)->data(workspace::MessageUnread).toBool());
        emit backend->chatReadConfirmed(QStringLiteral("c-1"), {}, 719);
        QVERIFY(divider(history).isEmpty());

        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        auto *taskHistory = findAccessible<QListWidget>(card, QStringLiteral("Чат карточки"));
        QVERIFY(taskHistory);
        QCOMPARE(divider(taskHistory), QStringLiteral("701"));
        emit backend->chatReadConfirmed(QStringLiteral("c-1"), {}, 702);
        QCOMPARE(divider(taskHistory), QStringLiteral("701"));
        emit backend->chatReadConfirmed(QStringLiteral("chat67"), {QStringLiteral("701")}, 0);
        QCOMPARE(divider(taskHistory), QStringLiteral("702"));
    }

    void lateReactionAnchorReadsOnlyItsVisibleActiveHistory()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->messages.clear();
        for (int id = 701; id <= 728; ++id) {
            Message sent;
            sent.id = QString::number(id);
            sent.text = QStringLiteral("Исходящее сообщение");
            sent.outgoing = true;
            backend->messages.append(sent);
        }
        backend->stages.first().tasks.first().chatDialogId = QStringLiteral("chat67");
        Message taskMessage = backend->messages.last();
        taskMessage.id = QStringLiteral("801");
        backend->taskMessages = {taskMessage};
        MainWindow window(backend.get());
        window.resize(930, 555);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *readTimer = window.findChild<QTimer *>(QStringLiteral("visibleChatReadTimer"));
        QVERIFY(readTimer);
        deliverPendingTimer(window, QStringLiteral("visibleChatReadTimer"));
        QVERIFY(!backend->reads.isEmpty());
        QVERIFY(backend->reads.last().visible.contains(QStringLiteral("728")));
        QVERIFY(backend->reads.last().incoming.isEmpty());

        const auto regularBefore = backend->reads.size();
        backend->chats.first().hasReaction = true;
        emit backend->recentChatsLoaded(backend->chats);
        emit backend->chatBadgeAnchorAvailable(QStringLiteral("chat999"));
        QVERIFY(!readTimer->isActive());
        QCOMPARE(backend->reads.size(), regularBefore);

        emit backend->chatBadgeAnchorAvailable(QStringLiteral("c-1"));
        QCOMPARE(backend->reads.size(), regularBefore);
        fireTimer(*readTimer);
        QVERIFY(backend->reads.size() > regularBefore);
        QCOMPARE(backend->reads.last().scope, QStringLiteral("c-1"));
        QVERIFY(backend->reads.last().visible.contains(QStringLiteral("728")));
        QVERIFY(!backend->reads.last().visible.contains(QStringLiteral("701")));
        QVERIFY(backend->reads.last().incoming.isEmpty());

        const auto afterRegular = backend->reads.size();
        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        QApplication::setActiveWindow(card);
        deliverPendingTimer(window, QStringLiteral("visibleChatReadTimer"));
        QVERIFY(backend->reads.size() > afterRegular);
        QVERIFY(backend->reads.last().task);
        const auto taskBefore = backend->reads.size();
        emit backend->chatBadgeAnchorAvailable(QStringLiteral("c-1"));
        QVERIFY(!readTimer->isActive());
        QCOMPARE(backend->reads.size(), taskBefore);
        emit backend->chatBadgeAnchorAvailable(QStringLiteral("chat67"));
        QCOMPARE(backend->reads.size(), taskBefore);
        fireTimer(*readTimer);
        QVERIFY(backend->reads.size() > taskBefore);
        QVERIFY(backend->reads.last().task);
        QCOMPARE(backend->reads.last().scope, QStringLiteral("501"));
        QCOMPARE(backend->reads.last().visible, QStringList{taskMessage.id});
    }

    void contactPresenceAndPinnedHeaderFollowOpenedDialogs()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto &contact = backend->chats.first();
        contact.kind = QStringLiteral("im");
        contact.position = QStringLiteral("Инженер");
        contact.onlineUntil = QDateTime::currentDateTimeUtc().addSecs(30);
        Chat taskChat;
        taskChat.id = QStringLiteral("task-dialog");
        taskChat.title = QStringLiteral("Чат задачи");
        taskChat.taskChat = true;
        taskChat.taskId = QStringLiteral("501");
        backend->taskChats = {taskChat};
        backend->stages.first().tasks.first().chatDialogId = taskChat.id;
        const auto message = [](QString id, QString text) {
            Message result;
            result.id = id;
            result.text = text;
            return result;
        };
        const auto first = message(QStringLiteral("41"), QStringLiteral("Первое закрепление"));
        const auto second = message(QStringLiteral("42"), QStringLiteral("Второе закрепление"));
        const auto taskPin = message(QStringLiteral("52"), QStringLiteral("Закрепление задачи"));
        backend->pinnedMessages.insert(contact.id, {first, second});
        backend->pinnedMessages.insert(taskChat.id, {taskPin});
        MainWindow window(backend.get());
        window.resize(940, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *header = findAccessible<QLabel>(&window, QStringLiteral("Статус и должность контакта"));
        auto *bar = findAccessible<QPushButton>(&window, QStringLiteral("Закреплённые сообщения"));
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
        QVERIFY(header && bar && chats);
        QTRY_VERIFY(header->isVisible() && bar->isVisible());
        QCOMPARE(header->text(), QStringLiteral("В сети · Инженер"));
        QCOMPARE(backend->pinnedRequests, QStringList{contact.id});
        const auto row = chats->visualItemRect(chats->item(0));
        const auto hasOnlineDot = [chats, row] {
            const auto shot = chats->viewport()->grab().toImage();
            const auto area = row.intersected(shot.rect());
            for (int y = area.top(); y <= area.bottom(); ++y)
                for (int x = area.left(); x <= area.left() + 54 && x <= area.right(); ++x)
                    if (shot.pixelColor(x, y) == QColor("#24875b"))
                        return true;
            return false;
        };
        QVERIFY(hasOnlineDot());
        const auto capture = qEnvironmentVariable("B24_CHAT_HEADER_SCREENSHOT");
        if (!capture.isEmpty())
            QVERIFY(window.grab().save(capture));

        emit backend->chatContactChanged(contact.id, QDateTime::currentDateTimeUtc().addMSecs(180), contact.position);
        QTRY_COMPARE(header->text(), QStringLiteral("Не в сети · Инженер"));
        QVERIFY(!hasOnlineDot());
        emit backend->chatContactChanged(contact.id, {}, contact.position);
        QCOMPARE(header->text(), contact.position);
        emit backend->pinnedMessagesLoaded(QStringLiteral("other"), {});
        QVERIFY(bar->isVisible());

        bar->setFocus();
        QTest::keyClick(bar, Qt::Key_Space);
        QCOMPARE(backend->contextDialog, contact.id);
        QCOMPARE(backend->contextMessage, first.id);
        QVERIFY(!QApplication::activePopupWidget());
        auto *choosePin = findAccessible<QPushButton>(&window, QStringLiteral("Выбрать закреплённое сообщение"));
        QVERIFY(choosePin && choosePin->isVisible());
        choosePin->setFocus();
        QTest::keyClick(choosePin, Qt::Key_Space);
        QTRY_VERIFY(QApplication::activePopupWidget());
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        QVERIFY(menu && menu->actions().size() == 2);
        menu->actions().at(1)->trigger();
        menu->close();
        QCOMPARE(backend->contextDialog, contact.id);
        QCOMPARE(backend->contextMessage, second.id);

        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("chatScopeTabs"));
        QVERIFY(tabs);
        tabs->setCurrentIndex(1);
        QVERIFY(!header->isVisible());
        QVERIFY(bar->isVisible());
        bar->click();
        QCOMPARE(backend->contextDialog, taskChat.id);
        QCOMPARE(backend->contextMessage, taskPin.id);

        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        sections->setCurrentRow(1);
        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        auto *taskBar = findAccessible<QPushButton>(card, QStringLiteral("Закреплённые сообщения"));
        QVERIFY(taskBar && taskBar->isVisible());
        taskBar->click();
        QCOMPARE(backend->contextDialog, taskChat.id);
        QCOMPARE(backend->contextMessage, taskPin.id);
        emit backend->messageContextLoaded(taskChat.id, taskPin.id, {taskPin});
        auto *messages = findAccessible<QListWidget>(card, QStringLiteral("Чат карточки"));
        QVERIFY(messages && messages->currentItem());
        QCOMPARE(messages->currentItem()->data(workspace::Id).toString(), taskPin.id);
        auto *latest = findAccessible<QPushButton>(card, QStringLiteral("К последним сообщениям задачи"));
        QVERIFY(latest && latest->isVisible());
        latest->click();
        QVERIFY(!latest->isVisible());
    }

    void pinnedJumpsKeepLoadedHistoryAndShowStickyDates()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        QVector<Message> history;
        const QDate earlier(2026, 2, 10);
        const QDate later(2026, 2, 11);
        for (int row = 0; row < 38; ++row) {
            Message message;
            message.id = QString::number(701 + row);
            message.text = QStringLiteral("Сообщение %1").arg(row);
            message.createdAt = QDateTime(row < 20 ? earlier : later, QTime(12, row));
            history.append(message);
        }
        const auto localPin = history.at(6);
        backend->messages = history;
        backend->taskMessages = history;
        backend->stages.first().tasks.first().chatDialogId = QStringLiteral("chat67");
        backend->pinnedMessages.insert(QStringLiteral("c-1"), {localPin, history.first()});
        backend->pinnedMessages.insert(QStringLiteral("chat67"), {localPin});
        MainWindow window(backend.get());
        window.resize(940, 570);
        window.show();
        QApplication::setActiveWindow(&window);

        const auto verify = [&](QWidget *surface, const QString &historyName) {
            auto *messages = findAccessible<QListWidget>(surface, historyName);
            auto *bar = findAccessible<QPushButton>(surface, QStringLiteral("Закреплённые сообщения"));
            QVERIFY(messages && bar && bar->isVisible());
            deliverPendingTimer(window, QStringLiteral("visibleChatReadTimer"));
            QVERIFY(!backend->reads.isEmpty());
            const auto reads = backend->reads.size();
            bar->click();
            QVERIFY(!QApplication::activePopupWidget());
            QVERIFY(backend->contextMessage.isEmpty());
            QTRY_VERIFY(messages->currentItem());
            QCOMPARE(messages->currentItem()->data(workspace::Id).toString(), localPin.id);
            QCOMPARE(messages->count(), history.size());
            const auto focused = messages->visualItemRect(messages->currentItem());
            QVERIFY(qAbs(focused.center().y() - messages->viewport()->rect().center().y()) <= 40);
            deliverPendingTimer(window, QStringLiteral("visibleChatReadTimer"));
            QCOMPARE(backend->reads.size(), reads);

            auto *day = messages->findChild<QLabel *>(QStringLiteral("chatStickyDay"));
            QVERIFY(day);
            messages->scrollToItem(messages->item(7), QAbstractItemView::PositionAtTop);
            QTRY_VERIFY(day->isVisible());
            QCOMPARE(day->text(), QStringLiteral("10.02.2026"));
            messages->setCurrentRow(messages->count() - 1);
            QTest::keyClick(messages, Qt::Key_End);
            QTRY_COMPARE(day->text(), QStringLiteral("11.02.2026"));
            messages->verticalScrollBar()->setValue(messages->verticalScrollBar()->minimum());
            QTRY_VERIFY(!day->isVisible());
        };
        verify(&window, QStringLiteral("Сообщения чата"));

        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        QApplication::setActiveWindow(card);
        verify(card, QStringLiteral("Чат карточки"));
    }

    void destroyingPopulatedHistoryCancelsPendingAlignment()
    {
        auto history = std::make_unique<ui::HistoryList>(nullptr);
        int viewportChanges = 0;
        history->setOnViewportChanged([&] { ++viewportChanges; });
        history->addItem(QStringLiteral("message"));
        history.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCOMPARE(viewportChanges, 0);
    }

    void createsTaskFromConfirmedMessageInBothHistories()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        backend->accountScope = QStringLiteral("fixture/account");
        backend->chats.first().id = QStringLiteral("chat91");
        backend->messages.first().id = QStringLiteral("701");
        backend->messages.first().text = QStringLiteral("Исходное сообщение");
        backend->taskMessages = backend->messages;
        backend->taskMessages.first().id = QStringLiteral("801");
        backend->stages.first().tasks.first().chatDialogId = QStringLiteral("chat51");
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        emit backend->connectionChanged(true);
        auto *mainHistory = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(mainHistory);
        QTRY_COMPARE(mainHistory->count(), 1);

        const auto offerAction = [](QListWidget *history) -> QAction * {
            history->setCurrentRow(0);
            const auto point = history->visualItemRect(history->item(0)).center();
            QContextMenuEvent event(QContextMenuEvent::Keyboard, point, history->viewport()->mapToGlobal(point));
            QApplication::sendEvent(history->viewport(), &event);
            auto *menu = visibleMessageMenu();
            if (!menu)
                return nullptr;
            for (auto *action : menu->actions())
                if (action->text() == QStringLiteral("Создать задачу из сообщения…"))
                    return action;
            menu->close();
            return nullptr;
        };
        const auto confirm = [&](QListWidget *history, QWidget *parent, bool accept, bool replaceSource = false) {
            auto *action = offerAction(history);
            QVERIFY(action);
            auto *menu = visibleMessageMenu();
            menu->setActiveAction(action);
            QTest::keyClick(menu, Qt::Key_Return);
            auto *box = parent->findChild<QMessageBox *>(QStringLiteral("createTaskFromMessageConfirm"));
            QVERIFY(box && box->isVisible());
            QCOMPARE(box->parentWidget(), parent);
            QVERIFY(box->text().contains(QStringLiteral("Исходное сообщение")));
            if (accept) {
                auto *create = findAccessible<QPushButton>(box, QStringLiteral("Создать задачу"));
                QVERIFY(create);
                if (replaceSource)
                    history->item(0)->setData(workspace::ReceiptMessageId, QStringLiteral("799"));
                create->click();
            } else {
                QTest::keyClick(box, Qt::Key_Escape);
            }
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        };
        confirm(mainHistory, &window, false);
        QVERIFY(backend->messageActionId.isEmpty());
        confirm(mainHistory, &window, true, true);
        QVERIFY(backend->messageActionId.isEmpty());
        mainHistory->item(0)->setData(workspace::ReceiptMessageId, QStringLiteral("701"));
        confirm(mainHistory, &window, true);
        QCOMPARE(backend->messageActionDialog, QStringLiteral("chat91"));
        QCOMPARE(backend->messageActionId, QStringLiteral("701"));
        QCOMPARE(backend->messageAction, MessageAction::CreateTask);
        QCOMPARE(mainHistory->count(), 1); // The portal, not the client, inserts the new record.

        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        auto *taskHistory = findAccessible<QListWidget>(card, QStringLiteral("Чат карточки"));
        QVERIFY(taskHistory);
        QTRY_COMPARE(taskHistory->count(), 1);
        confirm(taskHistory, card, true);
        QCOMPARE(backend->messageActionDialog, QStringLiteral("chat51"));
        QCOMPARE(backend->messageActionId, QStringLiteral("801"));
        QCOMPARE(backend->messageAction, MessageAction::CreateTask);
        taskHistory->item(0)->setData(workspace::SystemMessage, true);
        QVERIFY(!offerAction(taskHistory));
        taskHistory->item(0)->setData(workspace::SystemMessage, false);
        emit backend->connectionChanged(false);
        QVERIFY(!offerAction(taskHistory));
    }

    void keepsMessageViewersRecoverableAndOnScreen_data()
    {
        QTest::addColumn<bool>("recover");
        QTest::newRow("retry-and-scope") << true;
        QTest::newRow("asynchronous-growth-at-edge") << false;
    }

    void keepsMessageViewersRecoverableAndOnScreen()
    {
        QFETCH(bool, recover);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdViewerResponses = true;
        backend->accountScope = QStringLiteral("fixture/viewer-first");
        Message message;
        message.id = QStringLiteral("701");
        message.author = QStringLiteral("Я");
        message.text = QStringLiteral("Отправленное сообщение");
        message.outgoing = true;
        message.seenByOthers = true;
        backend->messages = {message};
        Chat other;
        other.id = QStringLiteral("chat42");
        other.title = QStringLiteral("Другой чат");
        backend->chats.append(other);
        MainWindow window(backend.get());
        window.resize(880, 600);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *chats = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
        auto *footer = findAccessible<QLabel>(&window, QStringLiteral("Состояние"));
        QVERIFY(history && chats && footer && window.screen());
        QTRY_COMPARE(history->count(), 1);
        QVERIFY(chats->count() >= 2);
        const auto available = window.screen()->availableGeometry();
        auto *delegate = static_cast<const workspace::RowPainter *>(history->itemDelegate());
        const auto receipt = [&] {
            return delegate->receiptArea(history->model()->index(0, 0), history->visualItemRect(history->item(0)));
        };
        QVERIFY(!receipt().isEmpty());
        if (!recover) {
            const auto target = available.bottomRight() - QPoint(30, 30);
            QTRY_VERIFY(history->viewport()->rect().contains(receipt().center()));
            for (int pass = 0; pass < 2; ++pass) {
                QCoreApplication::processEvents();
                window.move(window.pos() + target - history->viewport()->mapToGlobal(receipt().bottomLeft()));
            }
            QCoreApplication::processEvents();
            const auto nearEdge = history->viewport()->mapToGlobal(receipt().bottomLeft());
            QVERIFY2(available.contains(nearEdge),
                     qPrintable(QStringLiteral("available %1x%2, actual %3,%4").arg(available.width())
                       .arg(available.height()).arg(nearEdge.x()).arg(nearEdge.y())));
            QVERIFY(nearEdge.y() > available.bottom() - 50);
            QVERIFY(nearEdge.x() > available.right() - 50);
        }
        const auto clickReceipt = [&] {
            const auto box = receipt();
            QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier,
                              QPoint(box.right() - 14, box.bottom() - 9));
        };
        clickReceipt();
        QCOMPARE(backend->viewerReplies.size(), 1);
        QPointer<QFrame> popup = history->findChild<QFrame *>(QStringLiteral("messageViewersMenu"));
        QVERIFY(popup && popup->isVisible());
        const auto takeReply = [&] { return std::exchange(backend->viewerReplies.last(), {}); };
        const auto findRetry = [&] {
            return findAccessible<QPushButton>(popup, QStringLiteral("Повторить загрузку списка прочитавших"));
        };
        auto *rows = popup->findChild<QListWidget *>(QStringLiteral("messageViewersList"));
        QPushButton *close = nullptr;
        for (auto *button : popup->findChildren<QPushButton *>())
            if (button->text() == QStringLiteral("Закрыть"))
                close = button;
        QVERIFY(rows && close);
        MessageViewers result;
        result.count = 33;
        for (int i = 0; i < result.count; ++i)
            result.people.append({QString::number(100 + i), QStringLiteral("Сотрудник %1").arg(i), {}, {}});
        if (recover) {
            const auto initial = QStringLiteral("Текущий статус");
            emit backend->failed(QStringLiteral("fixture"), initial);
            takeReply()(std::nullopt);
            QCOMPARE(footer->text(), initial);
            auto *retry = findRetry();
            QVERIFY(retry && retry->isVisible() && retry->isEnabled());
            retry->click();
            QCOMPARE(backend->viewerReplies.size(), 2);
            QVERIFY(!retry->isVisible() || !retry->isEnabled());
            retry->click();
            QCOMPARE(backend->viewerReplies.size(), 2);
            takeReply()(result);
            QCOMPARE(rows->count(), 33);
            QCOMPARE(rows->item(0)->text(), QStringLiteral("Сотрудник 0"));
            QVERIFY(!retry->isVisible());
            popup->close();
            QTRY_VERIFY(popup.isNull());

            clickReceipt();
            QCOMPARE(backend->viewerReplies.size(), 3);
            popup = history->findChild<QFrame *>(QStringLiteral("messageViewersMenu"));
            QVERIFY(popup && popup->isVisible());
            auto late = takeReply();
            chats->setCurrentRow(1);
            QTRY_VERIFY(popup.isNull());
            late(result);

            chats->setCurrentRow(0);
            QTRY_COMPARE(history->count(), 1);
            clickReceipt();
            QCOMPARE(backend->viewerReplies.size(), 4);
            popup = history->findChild<QFrame *>(QStringLiteral("messageViewersMenu"));
            QVERIFY(popup && popup->isVisible());
            late = takeReply();
            backend->accountScope = QStringLiteral("fixture/viewer-second");
            emit backend->accountScopeChanged();
            QTRY_VERIFY(popup.isNull());
            late(result);
            return;
        }

        const auto anchor = history->viewport()->mapToGlobal(receipt().bottomLeft());
        takeReply()(result);
        QTRY_COMPARE(rows->count(), 33);
        QVERIFY(rows->isVisible() && close->isVisible());
        QTRY_VERIFY(rows->verticalScrollBar()->maximum() > 0);
        const auto inside = [popup](const QWidget *widget) {
            return popup->rect().contains(QRect(widget->mapTo(popup, QPoint()), widget->size()));
        };
        QTRY_VERIFY(inside(rows) && inside(close));
        QTRY_VERIFY(available.contains(popup->frameGeometry()));
        QVERIFY(popup->frameGeometry().bottom() < anchor.y());
        if (const auto path = qEnvironmentVariable("B24_VIEWERS_EDGE_SCREENSHOT"); !path.isEmpty())
            QVERIFY(popup->grab().save(path));
    }

    void opensViewersFromReadReceiptsInBothHistories()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        Message message;
        message.id = QStringLiteral("701");
        message.author = QStringLiteral("Я");
        message.text = QStringLiteral("Отправленное сообщение");
        message.outgoing = true;
        message.seenByOthers = true;
        backend->messages = {message};
        backend->taskMessages = backend->messages;
        MainWindow window(backend.get());
        window.resize(960, 790);
        window.show();
        QVERIFY(backend->viewedReceipts.isEmpty());
        const auto check = [&backend](QListWidget *history, bool task, const QString &scope) {
            QVERIFY(history);
            QTRY_COMPARE(history->count(), 1);
            const auto *delegate = static_cast<const workspace::RowPainter *>(history->itemDelegate());
            const auto box = delegate->receiptArea(history->model()->index(0, 0), history->visualItemRect(history->item(0)));
            QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier,
                              QPoint(box.right() - 14, box.bottom() - 9));
            QTRY_VERIFY(history->findChild<QFrame *>(QStringLiteral("messageViewersMenu")));
            auto *menu = history->findChild<QFrame *>(QStringLiteral("messageViewersMenu"));
            QVERIFY(menu->isVisible());
            const auto *rows = menu->findChild<QListWidget *>(QStringLiteral("messageViewersList"));
            QVERIFY(rows);
            QCOMPARE(rows->count(), 1);
            QCOMPARE(rows->item(0)->text(), QStringLiteral("Анна Волкова"));
            QVERIFY(rows->isVisible());
            QVERIFY(menu->rect().contains(QRect(rows->mapTo(menu, QPoint{}), rows->size())));
            QCOMPARE(backend->viewedReceipts.last().scope, scope);
            QCOMPARE(backend->viewedReceipts.last().id, QStringLiteral("701"));
            QCOMPARE(backend->viewedReceipts.last().task, task);
            QTRY_VERIFY(backend->avatarRequests.contains(backend->viewerAvatar));
            const auto capture = qEnvironmentVariable("B24_VIEWERS_SCREENSHOT");
            if (!capture.isEmpty() && !task)
                QVERIFY(menu->grab().save(capture));
            menu->close();
        };
        check(findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата")), false, QStringLiteral("c-1"));
        window.openTaskCard(QStringLiteral("501"));
        auto *task = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(task);
        check(findAccessible<QListWidget>(task, QStringLiteral("Чат карточки")), true, QStringLiteral("501"));
        QCOMPARE(backend->viewedReceipts.size(), 2);
    }

    void ownAndIncomingMessagesShareLeftAlignmentAndKeepDifferentColors()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto own = backend->messages.front();
        own.id = QStringLiteral("2");
        own.outgoing = true;
        backend->messages.append(own);
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        QTRY_COMPARE(history->count(), 2);
        QCoreApplication::processEvents();
        const auto screenshot = history->viewport()->grab().toImage();
        const auto firstBubblePixel = [&screenshot](const QRect &row, const QColor &color) {
            const auto area = row.intersected(screenshot.rect());
            for (int x = area.left(); x <= area.right(); ++x)
                for (int y = area.top(); y <= area.bottom(); ++y)
                    if (screenshot.pixelColor(x, y) == color)
                        return x;
            return -1;
        };
        const int incoming = firstBubblePixel(history->visualItemRect(history->item(0)), workspace::Surface);
        const int outgoing = firstBubblePixel(history->visualItemRect(history->item(1)), QColor("#c8e7d8"));
        QVERIFY(incoming >= 0 && outgoing >= 0);
        QVERIFY(qAbs(incoming - outgoing) <= 1);
        QVERIFY(outgoing < history->viewport()->width() / 2);
    }

    void unreadBoundaryAnchorsHistoryAndStaysOutOfSearch_data()
    {
        QTest::addColumn<bool>("task");
        QTest::newRow("chat") << false;
        QTest::newRow("task-chat") << true;
    }

    void messageActionsUseTheAdjacentGutter_data()
    {
        QTest::addColumn<bool>("task");
        QTest::newRow("chat") << false;
        QTest::newRow("task-chat") << true;
    }

    void messageActionsUseTheAdjacentGutter()
    {
        QFETCH(bool, task);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        Message message{QStringLiteral("701"), QStringLiteral("Анна"), QStringLiteral("Короткое сообщение"),
                        QDateTime::currentDateTime()};
        backend->messages = backend->taskMessages = {message};
        backend->stages.first().tasks.first().chatDialogId = QStringLiteral("chat73");
        MainWindow window(backend.get());
        window.resize(810, 620);
        window.show();
        emit backend->connectionChanged(true);
        const auto dialog = backend->chats.first().id;
        const auto feedback = QStringLiteral("Сообщение переслано");
        emit backend->chatActionSucceeded(dialog, feedback);
        emit backend->chatActionSucceeded(dialog, {});
        const auto *status = findAccessible<QLabel>(&window, QStringLiteral("Состояние"));
        QVERIFY(status && status->isVisible());
        QCOMPARE(status->text(), feedback);
        QWidget *owner = &window;
        if (task) {
            window.openTaskCard(QStringLiteral("501"));
            owner = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
            QVERIFY(owner);
            owner->resize(680, 500);
        }
        auto *history = findAccessible<QListWidget>(owner, task ? QStringLiteral("Чат карточки")
                                                               : QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        auto *more = findAccessible<QPushButton>(history, QStringLiteral("Действия сообщения"));
        auto *react = findAccessible<QPushButton>(history, QStringLiteral("Добавить реакцию"));
        QVERIFY(more && react);
        QTRY_COMPARE(history->count(), 1);
        QCoreApplication::processEvents();
        const auto index = history->model()->index(0, 0);
        const auto *painter = static_cast<const workspace::RowPainter *>(history->itemDelegate());
        const auto bubble = painter->receiptArea(index, history->visualRect(index));
        const auto gutter = QPoint(bubble.right() + 43, bubble.top() + 10);
        QVERIFY(history->viewport()->rect().contains(gutter));
        const auto move = [history](const QPoint &point) {
            auto *viewport = history->viewport();
            QMouseEvent event(QEvent::MouseMove, point, viewport->mapToGlobal(point),
                              Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(viewport, &event);
        };
        move(gutter);
        QTRY_VERIFY(more->isVisible());
        QVERIFY(history->viewport()->rect().contains(more->geometry()));
        QVERIFY(!bubble.intersects(more->geometry()));
        QVERIFY(react->isVisible());
        QVERIFY(history->viewport()->rect().contains(react->geometry()));
        QVERIFY(!bubble.intersects(react->geometry()) && !more->geometry().intersects(react->geometry()));
#ifdef Q_OS_LINUX
        const auto offscreen = QGuiApplication::platformName() == QStringLiteral("offscreen");
#else
        constexpr bool offscreen = false;
#endif
        if (!offscreen)
            QTest::mouseMove(react, react->rect().center());
        QTest::mouseClick(react, Qt::LeftButton);
        auto *palette = history->findChild<QMenu *>(QStringLiteral("reactionPalette"));
        QTRY_VERIFY(palette && palette->isVisible());
        auto *choices = findAccessible<QListWidget>(palette, QStringLiteral("Выберите реакцию"));
        QVERIFY(choices);
        QCOMPARE(choices->count(), 58);
        QTRY_COMPARE(choices->verticalScrollBar()->maximum(), 0);
        QVERIFY(choices->viewport()->rect().contains(choices->visualItemRect(choices->item(57))));
        choices->setCurrentRow(0);
        const auto code = choices->currentItem()->data(Qt::UserRole).toString();
        QTest::keyClick(choices, Qt::Key_Return);
        QCOMPARE(backend->messageActionId, message.id);
        QCOMPARE(backend->messageActionDialog, task ? QStringLiteral("chat73") : backend->chats.first().id);
        QCOMPARE(backend->messageActionValue, code);
        QCOMPARE(backend->messageAction, MessageAction::React);
        move(QPoint(gutter.x(), bubble.top() - 1));
        QVERIFY(more->isHidden());
        QVERIFY(react->isHidden());
    }

    void messageMenuKeepsTheTargetThroughRefresh()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        const auto dialog = backend->chats.first().id;
        const Message target{QStringLiteral("701"), QStringLiteral("Анна"), QStringLiteral("Первая строка"),
                             QDateTime::currentDateTime()};
        const Message next{QStringLiteral("702"), QStringLiteral("Михаил"), QStringLiteral("Новое сообщение"),
                           QDateTime::currentDateTime().addSecs(1)};
        backend->messages = {target};
        MainWindow window(backend.get());
        window.resize(800, 620);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *quote = findAccessible<QLabel>(&window, QStringLiteral("Цитата в черновике"));
        QVERIFY(history && quote);
        QTRY_COMPARE(history->count(), 1);
        const auto open = [history] {
            history->setCurrentRow(0);
            const auto point = history->visualItemRect(history->item(0)).center();
            QContextMenuEvent event(QContextMenuEvent::Keyboard, point, history->viewport()->mapToGlobal(point));
            QApplication::sendEvent(history->viewport(), &event);
            return QPointer<QMenu>(visibleMessageMenu());
        };
        auto menu = open();
        QVERIFY(menu && menu->isVisible());
        emit backend->chatMessagesLoaded(dialog, QVector<Message>{target, next});
        QTRY_COMPARE(history->count(), 2);
        QVERIFY(menu && menu->isVisible());
        QCOMPARE(menu->actions().first()->text(), QStringLiteral("Ответить"));
        menu->setActiveAction(menu->actions().first());
        QTest::keyClick(menu, Qt::Key_Return);
        QTRY_VERIFY(quote->isVisible());
        QVERIFY(quote->text().contains(target.text));
        QTRY_VERIFY(!menu || !menu->isVisible());

        menu = open();
        QVERIFY(menu && menu->isVisible());
        emit backend->chatMessagesLoaded(dialog, QVector<Message>{next});
        QTRY_VERIFY(!menu || !menu->isVisible());
    }

    void messageMenuOpensAboveOutsideThePointerAndReleasesAppSwitch()
    {
        QWidget window;
        auto *layout = new QVBoxLayout(&window);
        auto *history = new ui::HistoryList(&window);
        workspace::configureRows(history, workspace::Row::Message);
        layout->addWidget(history);
        QLabel state(&window);
        int selected = 0;
        history->setMessageMenu([&selected](QMenu &menu, const QPersistentModelIndex &, const QString &) {
            menu.addAction(QStringLiteral("Действие"), [&selected] { ++selected; });
            auto *submenu = menu.addMenu(QStringLiteral("Ещё"));
            submenu->addAction(QStringLiteral("Выбор"), [&selected] { ++selected; });
        });
        const auto screen = QGuiApplication::primaryScreen()->availableGeometry();
        window.resize(350, 220);
        window.move(screen.left() + 20, screen.bottom() - window.height() - 3);
        window.show();
        QApplication::setActiveWindow(&window);
        history->showHistory(QStringLiteral("chat73"), &state, {{QStringLiteral("701"), QStringLiteral("Анна"),
                             QStringLiteral("Сообщение"), QDateTime::currentDateTime()}});
        QCoreApplication::processEvents();
        const auto point = history->visualItemRect(history->item(0)).bottomLeft() + QPoint(40, -2);
        const auto global = history->viewport()->mapToGlobal(point);
        QContextMenuEvent event(QContextMenuEvent::Mouse, point, global);
        QApplication::sendEvent(history->viewport(), &event);
        const auto menu = QPointer<QMenu>(history->findChild<QMenu *>(QStringLiteral("messageContextMenu")));
        QVERIFY(menu && menu->isVisible());
        QVERIFY(menu->geometry().bottom() < global.y());
        QVERIFY(screen.contains(menu->geometry()));
        QVERIFY(menu->windowType() != Qt::Popup);
        QVERIFY(menu->actions().last()->menu()->windowType() != Qt::Popup);
        QVERIFY(!QApplication::activePopupWidget());
        const auto local = menu->mapFromGlobal(global);
        for (const auto kind : {QEvent::MouseMove, QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            const auto button = kind == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
            QMouseEvent mouse(kind, local, global, button, button, Qt::NoModifier);
            QApplication::sendEvent(menu, &mouse);
        }
        QVERIFY(!menu->activeAction());
        QCOMPARE(selected, 0);
        QTest::keyClick(menu, Qt::Key_Escape);
        QTRY_VERIFY(!menu || !menu->isVisible());
    }

    void copiesTheExactClickedMessageLink_data()
    {
        QTest::addColumn<bool>("task");
        QTest::newRow("ordinary-chat") << false;
        QTest::newRow("task-chat") << true;
    }

    void copiesTheExactClickedMessageLink()
    {
        QFETCH(bool, task);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        const auto relative = QStringLiteral("/tasks/task/view/41/?probe=x%2Fy#fragment");
        const auto local = QStringLiteral("https://example.bitrix24.ru") + relative;
        const auto publicUrl = QStringLiteral("https://public.example.com/path?token=first%2Fkeep&display=full#target");
        Message message{QStringLiteral("701"), QStringLiteral("Анна"),
                        QStringLiteral("[URL=%1]Задача[/URL] и [URL=%2]Внешняя ссылка[/URL]").arg(relative, publicUrl),
                        QDateTime::currentDateTime()};
        backend->messages = backend->taskMessages = {message};
        const auto taskDialog = QStringLiteral("chat73");
        backend->stages.first().tasks.first().chatDialogId = taskDialog;
        MainWindow window(backend.get(), true);
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QWidget *pane = &window;
        if (task) {
            window.openTaskCard(QStringLiteral("501"));
            pane = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
            QVERIFY(pane);
        }
        auto *list = findAccessible<QListWidget>(pane, task ? QStringLiteral("Чат карточки")
                                                           : QStringLiteral("Сообщения чата"));
        QVERIFY(list);
        QTRY_COMPARE(list->count(), 1);
        const auto index = list->model()->index(0, 0);
        const auto *painter = static_cast<const workspace::RowPainter *>(list->itemDelegate());
        const auto findPoint = [list, painter, index](const QString &href, bool card) {
            const auto row = list->visualRect(index);
            const auto visible = row.intersected(list->viewport()->rect());
            for (int y = visible.top(); y <= visible.bottom(); y += 2)
                for (int x = visible.left(); x <= visible.right(); x += 2) {
                    const QPoint point(x, y);
                    const auto anchor = painter->anchorAt(index, row, point);
                    if (card ? anchor.isEmpty() && painter->linkAt(index, row, point) == href : anchor == href)
                        return point;
                }
            return QPoint();
        };
        const auto findAction = [](QMenu *menu, const QString &name) -> QAction * {
            for (auto *action : menu->actions())
                if (action->text() == name)
                    return action;
            return nullptr;
        };
        const auto menuName = QStringLiteral("messageContextMenu");
        const auto copyName = QStringLiteral("Скопировать ссылку");
        const auto openMenu = [list](const QPoint &point, QContextMenuEvent::Reason reason) {
            QContextMenuEvent event(reason, point, list->viewport()->mapToGlobal(point));
            QCoreApplication::sendEvent(list->viewport(), &event);
        };
        for (const auto &[href, expected, card] : {std::tuple{relative, local, false}, std::tuple{publicUrl, publicUrl, true}}) {
            const auto point = findPoint(href, card);
            QVERIFY(!point.isNull());
            openMenu(point, QContextMenuEvent::Mouse);
            auto *menu = list->findChild<QMenu *>(menuName);
            QVERIFY(menu);
            auto *copy = findAction(menu, copyName);
            QVERIFY(copy && !copy->menu() && copy->isEnabled());
            copy->trigger();
            QCOMPARE(QGuiApplication::clipboard()->text(), expected);
            menu->close();
            QTRY_VERIFY(!list->findChild<QMenu *>(menuName));
        }

        list->setCurrentRow(0);
        openMenu(list->visualRect(index).center(), QContextMenuEvent::Keyboard);
        auto *menu = list->findChild<QMenu *>(menuName);
        QVERIFY(menu);
        auto *links = findAction(menu, copyName);
        QVERIFY(links && links->menu());
        QCOMPARE(links->menu()->actions().size(), 2);
        links->menu()->actions().at(0)->trigger();
        QCOMPARE(QGuiApplication::clipboard()->text(), local);
        links->menu()->actions().at(1)->trigger();
        QCOMPARE(QGuiApplication::clipboard()->text(), publicUrl);
        auto *permalink = findAction(menu, QStringLiteral("Скопировать ссылку на сообщение"));
        QVERIFY(permalink);
        permalink->trigger();
        const QUrl reference(QGuiApplication::clipboard()->text());
        QCOMPARE(reference.path(), QStringLiteral("/online/"));
        QCOMPARE(QUrlQuery(reference).queryItemValue(QStringLiteral("IM_MESSAGE")), message.id);
        QCOMPARE(QUrlQuery(reference).queryItemValue(task ? QStringLiteral("IM_TASK") : QStringLiteral("IM_DIALOG")),
                 task ? taskDialog : backend->chats.first().id);
        menu->close();
    }

    void historyKeepsTheViewportWhenLinkPreviewsGrow_data()
    {
        QTest::addColumn<bool>("task");
        QTest::newRow("ordinary-chat") << false;
        QTest::newRow("task-chat") << true;
    }

    void historyKeepsTheViewportWhenLinkPreviewsGrow()
    {
        QFETCH(bool, task);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdLinkPreviews = true;
        auto &messages = task ? backend->taskMessages : backend->messages;
        messages.clear();
        const auto now = QDateTime::currentDateTime();
        for (int i = 0; i < 20; ++i) {
            Message message{QString::number(701 + i), QStringLiteral("Сергей"),
                            QStringLiteral("Сообщение %1").arg(i), now.addSecs((i - 20) * 360)};
            if (i == 3 || i == 16)
                message.text = QStringLiteral("https://example.bitrix24.ru/tasks/task/view/%1/").arg(i == 3 ? 52 : 53);
            messages.append(message);
        }
        MainWindow window(backend.get(), true);
        window.resize(960, 600);
        window.show();
        QApplication::setActiveWindow(&window);
        QWidget *pane = &window;
        if (task) {
            window.openTaskCard(QStringLiteral("501"));
            pane = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
            QVERIFY(pane);
        }
        auto *list = findAccessible<QListWidget>(pane, task ? QStringLiteral("Чат карточки")
                                                           : QStringLiteral("Сообщения чата"));
        QVERIFY(list);
        QTRY_COMPARE(list->count(), 20);
        const auto take = [&backend](const QString &path) {
            for (auto &request : backend->linkPreviewRequests)
                if (request.first.path() == path && request.second)
                    return std::exchange(request.second, {});
            return Backend::LinkPreviewHandler{};
        };
        Backend::LinkPreviewHandler latestPreview;
        QTRY_VERIFY(latestPreview || (latestPreview = take(QStringLiteral("/tasks/task/view/53/"))));
        auto *scroll = list->verticalScrollBar();
        QVERIFY(scroll->maximum() > 0);
        QCOMPARE(scroll->value(), scroll->maximum());
        const auto preview = MessageLinkPreview{QStringLiteral("Карточка задачи"),
            {QStringLiteral("Статус"), QStringLiteral("Постановщик"), QStringLiteral("Исполнитель"),
             QStringLiteral("Описание")}, MessageLinkPreviewKind::Task};
        const int before = scroll->maximum();
        latestPreview(preview);
        QTRY_VERIFY(scroll->maximum() > before);
        QCOMPARE(scroll->value(), scroll->maximum());
        QVERIFY(list->viewport()->rect().contains(list->visualItemRect(list->item(list->count() - 1)).bottomLeft()));

        const auto *olderRow = list->item(5);
        list->scrollToItem(olderRow, QAbstractItemView::PositionAtTop);
        Backend::LinkPreviewHandler olderPreview;
        QTRY_VERIFY(olderPreview || (olderPreview = take(QStringLiteral("/tasks/task/view/52/"))));
        const int olderOffset = list->visualItemRect(olderRow).top();
        const int olderMaximum = scroll->maximum();
        olderPreview(preview);
        QTRY_VERIFY(scroll->maximum() > olderMaximum);
        QCOMPARE(list->visualItemRect(olderRow).top(), olderOffset);
        QVERIFY(scroll->value() < scroll->maximum());
    }

    void unreadBoundaryAnchorsHistoryAndStaysOutOfSearch()
    {
        QFETCH(bool, task);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        const auto now = QDateTime::currentDateTime();
        auto &history = task ? backend->taskMessages : backend->messages;
        constexpr int firstUnread = 8;
        constexpr int total = 32;
        history.clear();
        for (int i = 0; i < total; ++i) {
            auto message = Message{QString::number(i + 1), QStringLiteral("Анна"),
                QStringLiteral("Сообщение %1").arg(i + 1), now.addSecs((i - total) * 30)};
            message.unread = i >= firstUnread || i == 6;
            message.outgoing = i == 6;
            history.append(message);
        }

        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QWidget *pane = &window;
        if (task) {
            QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
            window.openTaskCard(QStringLiteral("501"));
            pane = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
            QVERIFY(pane);
        }

        auto *messages = findAccessible<QListWidget>(pane, task ? QStringLiteral("Чат карточки")
                                                               : QStringLiteral("Сообщения чата"));
        QVERIFY(messages);
        auto *scroll = messages->verticalScrollBar();
        const auto anchored = [messages, scroll, firstUnread] {
            return messages->visualItemRect(messages->item(firstUnread)).top() >= 0 &&
                   messages->visualItemRect(messages->item(firstUnread)).top() < 10 &&
                   scroll->value() < scroll->maximum();
        };
        QTRY_COMPARE(messages->count(), total);
        QTRY_VERIFY2(anchored(), qPrintable(QStringLiteral("boundary top: %1; scroll: %2/%3")
            .arg(messages->visualItemRect(messages->item(firstUnread)).top())
            .arg(scroll->value()).arg(scroll->maximum())));
        for (int i = 0; i < total; ++i)
            QCOMPARE(messages->item(i)->data(workspace::UnreadDivider).toBool(), i == firstUnread);
        QVERIFY(messages->item(firstUnread)->data(Qt::AccessibleTextRole).toString()
            .startsWith(workspace::UnreadMessages));
        QCOMPARE(messages->item(firstUnread)->data(workspace::Title).toString(), QStringLiteral("Анна"));

        const auto scope = task ? QStringLiteral("501") : QStringLiteral("c-1");
        const auto updateHistory = [&] {
            if (task)
                emit backend->taskMessagesLoaded(scope, history);
            else
                emit backend->chatMessagesLoaded(scope, history);
        };
        const int previous = scroll->value();
        auto newest = history.back();
        newest.id = QStringLiteral("33");
        history.append(newest);
        updateHistory();
        QTRY_COMPARE(messages->count(), total + 1);
        QCOMPARE(scroll->value(), previous);
        messages->scrollToBottom();
        newest.id = QStringLiteral("34");
        history.append(newest);
        updateHistory();
        QTRY_COMPARE(messages->count(), total + 2);
        QTRY_COMPARE(scroll->value(), scroll->maximum());
        QVERIFY(messages->item(firstUnread)->data(workspace::UnreadDivider).toBool());
        QVERIFY(!messages->item(total + 1)->data(workspace::UnreadDivider).toBool());

        auto *showSearch = findAccessible<QPushButton>(pane, task ? QStringLiteral("Найти в чате задачи")
                                                                   : QStringLiteral("Найти в чате"));
        auto *search = findAccessible<QLineEdit>(pane, task ? QStringLiteral("Поиск по чату карточки")
                                                           : QStringLiteral("Поиск по сообщениям"));
        QVERIFY(showSearch);
        QVERIFY(search);
        QTest::mouseClick(showSearch, Qt::LeftButton);
        QTRY_VERIFY(search->isVisible() && search->hasFocus());
        const auto query = QStringLiteral("Сообщение 9");
        search->setText(query);
        QTest::keyClick(search, Qt::Key_Return);
        emit backend->messagesFound(scope, query, task, {history[firstUnread]});
        updateHistory();
        QTRY_COMPARE(messages->count(), 1);
        QVERIFY(!messages->item(0)->data(workspace::UnreadDivider).toBool());
        QTest::mouseClick(showSearch, Qt::LeftButton);
        QTRY_COMPARE(messages->count(), total + 2);
        QTRY_VERIFY(anchored());
    }
};

QTEST_MAIN(UiChatHistoryTest)
#include "tst_ui_chat_history.moc"
