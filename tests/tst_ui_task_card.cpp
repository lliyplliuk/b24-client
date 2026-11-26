#include "pilot_fixture.h"

using namespace bx;
using namespace bx::test;

class UiTaskCardTest : public PilotFixture {
    Q_OBJECT

private slots:
    void taskCardShowsNumberAndCopiesNumberAndLink()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);

        window.openTaskCard(QStringLiteral("501"));
        auto *card = dynamic_cast<ui::TaskCardDialog *>(
            window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        QVERIFY(card);
        QApplication::setActiveWindow(card);
        auto *number = findAccessible<QLabel>(card, QStringLiteral("Номер задачи"));
        QVERIFY(number);
        QCOMPARE(number->text(), QStringLiteral("№501"));

        auto *copyNumber = findAccessible<QPushButton>(card, QStringLiteral("Скопировать номер"));
        auto *copyLink = findAccessible<QPushButton>(card, QStringLiteral("Скопировать ссылку"));
        QVERIFY(copyNumber && copyLink);
        QVERIFY(copyNumber->text().isEmpty() && copyLink->text().isEmpty());
        QCOMPARE(copyNumber->toolTip(), copyNumber->accessibleName());
        QCOMPARE(copyLink->toolTip(), copyLink->accessibleName());
        QVERIFY(number->geometry().right() < copyNumber->geometry().left());
        QVERIFY(copyNumber->geometry().right() < copyLink->geometry().left());
        QVERIFY(copyLink->geometry().left() - number->geometry().right() < 48);
        auto *feedback = card->findChild<QLabel *>(QStringLiteral("taskCopyFeedback"));
        QVERIFY(feedback && feedback->text().isEmpty());
        const auto linkGeometry = copyLink->geometry();

        copyNumber->setFocus();
        QVERIFY(copyNumber->hasFocus());
        QTest::keyClick(copyNumber, Qt::Key_Space);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("501"));
        QCOMPARE(feedback->text(), QStringLiteral("Номер скопирован"));
        QVERIFY(copyNumber->hasFocus());
        QCoreApplication::sendPostedEvents(card, QEvent::LayoutRequest);
        QCOMPARE(copyLink->geometry(), linkGeometry);

        QTest::mouseClick(copyLink, Qt::LeftButton);
        QCOMPARE(QGuiApplication::clipboard()->text(),
                 QStringLiteral("https://example.bitrix24.ru/workgroups/group/101/tasks/task/view/501/"));
        QCOMPARE(feedback->text(), QStringLiteral("Ссылка скопирована"));
        QCoreApplication::sendPostedEvents(card, QEvent::LayoutRequest);
        QCOMPARE(copyLink->geometry(), linkGeometry);
        auto &feedbackTimer = testClock(*card);
        QVERIFY(feedbackTimer.isActive());
        QCOMPARE(feedbackTimer.interval(), 1800);
        fireTimer(feedbackTimer);
        QVERIFY(feedback->text().isEmpty());

        const auto clipboard = QGuiApplication::clipboard()->text();
        card->setLink(QString());
        QVERIFY(!copyLink->isEnabled());
        QTest::mouseClick(copyLink, Qt::LeftButton);
        QCOMPARE(QGuiApplication::clipboard()->text(), clipboard);
        QVERIFY(feedback->text().isEmpty());

        QTest::keyClick(card, Qt::Key_Escape);
        QVERIFY(!card->isVisible());
    }

    void taskCardActionsRequireRightsAndWaitForConfirmation()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdTaskUpdate = true;
        auto &task = backend->stages.first().tasks.first();
        task.projectId = QStringLiteral("101");
        task.statusActions = {TaskStatusAction::Start, TaskStatusAction::Complete, TaskStatusAction::Defer};
        task.editRights.assignee = true;
        task.editRights.remove = true;
        task.editRights.rate = true;
        task.rating = TaskRating::Positive;
        task.assigneeId = QStringLiteral("11");
        task.assignee = QStringLiteral("Анна");
        backend->people = {{QStringLiteral("14"), QStringLiteral("Вера"), {}, {}}};
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        auto *projects = findAccessible<QListWidget>(&window, QStringLiteral("Проекты"));
        QVERIFY(sections && projects);
        sections->setCurrentRow(1);
        QTRY_COMPARE(projects->count(), 1);
        projects->setCurrentRow(0);
        QTRY_VERIFY(backend->boardRequests > 0);
        window.openTaskCard(QStringLiteral("501"));
        auto *card = static_cast<ui::TaskCardDialog *>(window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        QVERIFY(card && card->isVisible());
        auto *primary = card->findChild<QPushButton *>(QStringLiteral("taskPrimaryAction"));
        auto *complete = findAccessible<QPushButton>(card, QStringLiteral("Завершить задачу"));
        auto *more = card->findChild<QMenu *>(QStringLiteral("taskOverflowMenu"));
        auto *rating = card->findChild<QMenu *>(QStringLiteral("taskRatingMenu"));
        QVERIFY(primary && complete && more && rating);
        const auto action = [](QMenu *menu, const QString &label) {
            for (auto *entry : menu->actions())
                if (entry->text() == label)
                    return entry;
            return static_cast<QAction *>(nullptr);
        };
        auto *delegate = action(more, QStringLiteral("Делегировать"));
        auto *defer = action(more, QStringLiteral("Отложить"));
        auto *remove = action(more, QStringLiteral("Удалить задачу"));
        auto *positive = action(rating, QStringLiteral("Положительная"));
        auto *negative = action(rating, QStringLiteral("Отрицательная"));
        QVERIFY(delegate && defer && remove && positive && negative);
        QTRY_VERIFY(primary->isVisible() && primary->isEnabled());
        QVERIFY(complete->isVisible());
        QVERIFY(!delegate->isVisible());
        QVERIFY(defer->isVisible() && remove->isVisible());
        QVERIFY(positive->isChecked() && !negative->isChecked());

        primary->click();
        QVERIFY(backend->lastUpdate.statusAction == TaskStatusAction::Start);
        QVERIFY(!primary->isEnabled() && !complete->isEnabled());
        task.statusActions = {TaskStatusAction::Pause, TaskStatusAction::Complete, TaskStatusAction::Defer};
        emit backend->taskUpdated(task.id);
        QCOMPARE(primary->text(), QStringLiteral("Приостановить"));
        defer->trigger();
        QVERIFY(backend->lastUpdate.statusAction == TaskStatusAction::Defer);
        emit backend->taskUpdated(task.id);
        negative->trigger();
        QVERIFY(backend->lastUpdate.rating == TaskRating::Negative);
        QVERIFY(positive->isChecked() && !negative->isChecked());
        task.rating = TaskRating::Negative;
        task.editRights.delegate = true;
        emit backend->taskUpdated(task.id);
        QVERIFY(negative->isChecked() && !positive->isChecked());
        QVERIFY(delegate->isVisible());
        delegate->trigger();
        auto *picker = card->findChild<QWidget *>(QStringLiteral("taskRequisitePopup"));
        QVERIFY(picker && picker->isVisible());
        QVERIFY(!complete->isEnabled() && !remove->isEnabled());
        auto *cancel = findAccessible<QPushButton>(picker, QStringLiteral("Отменить изменение реквизита"));
        QVERIFY(cancel);
        cancel->click();
        QTRY_VERIFY(!picker->isVisible());
        const auto selectAssignee = [picker] {
            auto *search = findAccessible<QLineEdit>(picker, QStringLiteral("Поиск сотрудника · Исполнитель"));
            QVERIFY(search && search->isVisible());
            const auto shownSuggestions = [picker]() -> QListWidget * {
                for (auto *list : picker->findChildren<QListWidget *>())
                    if (list->isVisible() && list->accessibleName() == QStringLiteral("Найденные сотрудники"))
                        return list;
                return nullptr;
            };
            QTRY_VERIFY(shownSuggestions());
            auto *suggestions = shownSuggestions();
            QTRY_VERIFY(!suggestions->findItems(QStringLiteral("Вера"), Qt::MatchExactly).isEmpty());
            suggestions->setCurrentItem(suggestions->findItems(QStringLiteral("Вера"), Qt::MatchExactly).first());
            QTest::keyClick(search, Qt::Key_Return);
        };
        auto *assignee = findAccessible<QPushButton>(card, QStringLiteral("Исполнитель"));
        QVERIFY(assignee && assignee->isEnabled());
        assignee->click();
        selectAssignee();
        QCOMPARE(backend->lastUpdate.assigneeId.value_or(QString()), QStringLiteral("14"));
        QVERIFY(!backend->lastUpdate.requireDelegate);
        emit backend->taskUpdated(task.id);
        delegate->trigger();
        selectAssignee();
        QCOMPARE(backend->lastUpdate.assigneeId.value_or(QString()), QStringLiteral("14"));
        QVERIFY(backend->lastUpdate.requireDelegate);
        emit backend->taskUpdated(task.id);

        card->messageInput()->setPlainText(QStringLiteral("Черновик"));
        QVERIFY(!remove->isEnabled());
        card->messageInput()->clear();
        QVERIFY(remove->isEnabled());
        remove->trigger();
        auto *confirm = card->findChild<QMessageBox *>(QStringLiteral("taskDeleteConfirmation"));
        QVERIFY(confirm && confirm->isVisible());
        QVERIFY(confirm->defaultButton() == confirm->button(QMessageBox::Cancel));
        confirm->button(QMessageBox::Cancel)->click();
        QVERIFY(backend->deletedTasks.isEmpty());
        QTRY_VERIFY(!card->findChild<QMessageBox *>(QStringLiteral("taskDeleteConfirmation")));
        remove->trigger();
        confirm = card->findChild<QMessageBox *>(QStringLiteral("taskDeleteConfirmation"));
        QVERIFY(confirm);
        confirm->button(QMessageBox::Yes)->click();
        QCOMPARE(backend->deletedTasks, QStringList{task.id});
        QVERIFY(card->isVisible() && !remove->isEnabled());
        emit backend->taskDeleted(QStringLiteral("502"));
        QVERIFY(card->isVisible());
        emit backend->failed(QStringLiteral("delete-task:") + task.id, QStringLiteral("Портал отклонил удаление"));
        QVERIFY(card->isVisible() && remove->isEnabled());
        QTRY_VERIFY(!card->findChild<QMessageBox *>(QStringLiteral("taskDeleteConfirmation")));
        remove->trigger();
        confirm = card->findChild<QMessageBox *>(QStringLiteral("taskDeleteConfirmation"));
        QVERIFY(confirm);
        confirm->button(QMessageBox::Yes)->click();
        QCOMPARE(backend->deletedTasks, (QStringList{task.id, task.id}));
        const auto boards = backend->boardRequests;
        emit backend->taskDeleted(task.id);
        QTRY_VERIFY(!window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        QCOMPARE(backend->boardRequests, boards + 1);
    }

    void taskCardSavesEditAndSendsMessage()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        backend->holdTaskUpdate = true;
        backend->stages.first().tasks.first().priority = QStringLiteral("2");
        window.openTaskCard(QStringLiteral("501"));

        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        auto *edit = findAccessible<QPushButton>(card, QStringLiteral("Изменить задачу"));
        QVERIFY(edit && edit->isEnabled());
        edit->click();
        auto *title = findAccessible<QLineEdit>(card, QStringLiteral("Заголовок задачи"));
        QVERIFY(title);
        auto *priority = findAccessible<QCheckBox>(card, QStringLiteral("Высокий приоритет"));
        QVERIFY(priority);
        QVERIFY(priority->isChecked());
        priority->setChecked(false);
        title->setText(QStringLiteral("Починить кран и трубу"));
        auto *save = findAccessible<QPushButton>(card, QStringLiteral("Сохранить задачу"));
        QVERIFY(save);
        auto late = backend->stages.first().tasks.first();
        late.attachments = {QStringLiteral("название.txt")};
        emit backend->taskLoaded(late);
        QCOMPARE(title->text(), QStringLiteral("Починить кран и трубу"));
        save->click();
        QVERIFY(!save->isEnabled());
        QCOMPARE(backend->updatedTask, QStringLiteral("501"));
        QVERIFY(backend->lastUpdate.title.has_value());
        QCOMPARE(*backend->lastUpdate.title, QStringLiteral("Починить кран и трубу"));
        QCOMPARE(backend->lastUpdate.highPriority, std::optional<bool>(false));

        emit backend->failed(QStringLiteral("update-task:501"), QStringLiteral("Изменение отклонено"));
        QVERIFY(save->isEnabled());
        QCOMPARE(title->text(), QStringLiteral("Починить кран и трубу"));
        auto *input = findAccessible<QPlainTextEdit>(card, QStringLiteral("Текст в чате карточки"));
        QVERIFY(input);
        input->setPlainText(QStringLiteral("Сообщение в карточке"));
        auto *send = findAccessible<QPushButton>(card, QStringLiteral("Отправить в карточке"));
        QVERIFY(send);
        send->click();
        QCOMPARE(backend->sentTask, QStringLiteral("Сообщение в карточке"));
        input->setPlainText(QStringLiteral("Незаконченный ответ"));
        static_cast<ui::TaskCardDialog *>(card)->setDiscardConfirmation([] { return true; });
        QPointer<QDialog> beforeClose(card);
        QTest::keyClick(card, Qt::Key_Escape);
        QTRY_VERIFY(beforeClose.isNull());
        window.openTaskCard(QStringLiteral("501"));
        card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        input = findAccessible<QPlainTextEdit>(card, QStringLiteral("Текст в чате карточки"));
        QCOMPARE(input->toPlainText(), QStringLiteral("Незаконченный ответ"));
    }

    void taskProjectPickerUsesProjectBrandingForTheRowChoiceAndPill()
    {
        Task task;
        task.id = QStringLiteral("501");
        task.name = QStringLiteral("Монтаж");
        task.projectId = QStringLiteral("101");
        task.project = QStringLiteral("WEB разработка");
        task.editRights.project = true;

        QWidget parent;
        ui::TaskCardDialog card(task, task.projectId, &parent);
        card.setCanDetachProject(true);
        card.setWriteState(true, false, false, false, false);
        QPixmap photo(32, 32);
        const QColor logoColor(QStringLiteral("#e13b52"));
        photo.fill(logoColor);
        bool logoLoaded = false;
        Project project{task.projectId, task.project, {}};
        project.avatarType = QStringLiteral("folder");
        project.avatarColor = QStringLiteral("#2059ad");
        project.avatarUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/upload/web-project.png"));
        card.setRequisiteAvatars([&](const QUrl &url) -> const QPixmap * {
            return logoLoaded && url == project.avatarUrl ? &photo : nullptr;
        });
        card.show();
        QApplication::setActiveWindow(&card);

        auto *projectRow = findAccessible<QPushButton>(&card, QStringLiteral("Проект"));
        QVERIFY(projectRow && projectRow->isEnabled());
        projectRow->click();
        auto *choices = findAccessible<QListWidget>(&card, QStringLiteral("Проект задачи"));
        auto *search = findAccessible<QLineEdit>(&card, QStringLiteral("Поиск проекта"));
        QVERIFY(choices && choices->isVisible() && search && search->isVisible());
        const auto chipIcon = [search]() {
            for (const auto *pill : search->parentWidget()->findChildren<QWidget *>(QStringLiteral("taskInlineSelectionPill"))) {
                if (!pill->isVisible())
                    continue;
                for (const auto *label : pill->findChildren<QLabel *>()) {
                    if (!label->pixmap().isNull())
                        return label->pixmap().toImage();
                }
            }
            return QImage{};
        };
        const auto rowIcon = [projectRow]() {
            auto *avatar = projectRow->findChild<QWidget *>(QStringLiteral("taskRequisiteAvatar"));
            return avatar && avatar->isVisible() ? avatar->grab().toImage() : QImage{};
        };
        const auto suggestionIcon = [choices] {
            return choices->currentItem() ? choices->currentItem()->icon().pixmap(22, 22).toImage() : QImage{};
        };
        const auto fallback = suggestionIcon();
        const auto fallbackChip = chipIcon();
        const auto fallbackRow = rowIcon();
        card.setProjectChoices({project});
        QCOMPARE(choices->currentItem()->data(Qt::UserRole).toString(), project.id);
        QPixmap expected(22, 22);
        expected.fill(Qt::transparent);
        {
            QPainter painter(&expected);
            painter.setRenderHint(QPainter::Antialiasing);
            workspace::paintAvatar(painter, QRect(0, 0, 21, 21), project.name, workspace::AvatarKind::Project,
                                   {}, QColor(project.avatarColor), project.avatarVariant, project.avatarType);
        }
        QCOMPARE(suggestionIcon(), expected.toImage());
        QVERIFY(chipIcon() != fallbackChip);
        QTRY_VERIFY(rowIcon() != fallbackRow);
        QVERIFY(card.visibleRequisiteAvatarUrls().contains(project.avatarUrl));
        QVERIFY(fallback.isNull() && fallbackChip.isNull() && fallbackRow.isNull());

        project.avatarType.clear();
        project.avatarVariant = QStringLiteral("guest");
        card.setProjectChoices({project});
        QVERIFY(suggestionIcon().isNull() && chipIcon().isNull() && rowIcon().isNull());
        QVERIFY(card.visibleRequisiteAvatarUrls().contains(project.avatarUrl));
        const int searchOffsetWithoutLogo = search->x();
        logoLoaded = true;
        card.refreshRequisiteAvatars();
        QCOMPARE(suggestionIcon().pixelColor(10, 10), logoColor);
        QCOMPARE(chipIcon().pixelColor(10, 10), logoColor);
        QTRY_VERIFY(!rowIcon().isNull());
        QCOMPARE(rowIcon().pixelColor(10, 10), logoColor);
        QVERIFY(search->x() > searchOffsetWithoutLogo);
    }

    void taskCardSavesAnExplicitPickerChoiceAndCanRetry_data()
    {
        QTest::addColumn<QString>("field");
        QTest::newRow("project") << QStringLiteral("Проект");
        QTest::newRow("stage") << QStringLiteral("Этап");
        QTest::newRow("assignee") << QStringLiteral("Исполнитель");
    }

    void taskCardSavesAnExplicitPickerChoiceAndCanRetry()
    {
        QFETCH(QString, field);
        const bool project = field == QStringLiteral("Проект");
        const bool stageField = field == QStringLiteral("Этап");
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto &task = backend->stages.first().tasks.first();
        task.projectId = QStringLiteral("101");
        task.project = backend->projects.first().name;
        task.editRights.project = true;
        task.editRights.stage = true;
        task.editRights.assignee = true;
        task.assigneeId = QStringLiteral("12");
        task.assignee = QStringLiteral("Анна");
        backend->projects.append({QStringLiteral("102"), QStringLiteral("Склад"), {}});
        backend->people = {{QStringLiteral("12"), QStringLiteral("Анна"), {}, {}},
                           {QStringLiteral("14"), QStringLiteral("Вера"), {}, {}}};
        backend->holdTaskUpdate = true;
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        QApplication::setActiveWindow(card);
        auto *button = findAccessible<QPushButton>(card, field);
        QVERIFY(button && button->isEnabled());
        button->click();

        auto *popup = card->findChild<QFrame *>(QStringLiteral("taskRequisitePopup"));
        QVERIFY(popup && popup->isVisible());
        const auto listName = project ? QStringLiteral("Проект задачи")
            : stageField ? QStringLiteral("Этап задачи") : QStringLiteral("Найденные сотрудники");
        QListWidget *list = nullptr;
        QTRY_VERIFY([&] {
            for (auto *candidate : popup->findChildren<QListWidget *>()) {
                if (candidate->accessibleName() == listName && candidate->isVisible() && candidate->count() >= 2) {
                    list = candidate;
                    return true;
                }
            }
            return false;
        }());
        auto *activation = project || stageField ? static_cast<QWidget *>(list)
            : findAccessible<QLineEdit>(popup, QStringLiteral("Поиск сотрудника · Исполнитель"));
        QVERIFY(activation);
        const auto retryName = stageField ? QStringLiteral("Повторить перенос задачи")
            : QStringLiteral("Повторить изменение реквизита");
        auto *retry = findAccessible<QPushButton>(popup, retryName);
        if (retry)
            QVERIFY(!retry->isVisible());
        QListWidgetItem *confirmed = nullptr;
        for (int row = 0; row < list->count(); ++row)
            if (list->item(row)->checkState() == Qt::Checked)
                confirmed = list->item(row);
        QVERIFY(confirmed);
        list->setCurrentItem(confirmed);
        QTest::keyClick(activation, Qt::Key_Return);
        QCOMPARE(backend->taskUpdateRequests, 0);
        QVERIFY(popup->isVisible());
        QTRY_VERIFY(list->isVisible());
        QTest::keyClick(activation, Qt::Key_Down);
        QCOMPARE(backend->taskUpdateRequests, 0);
        const auto destination = project ? QStringLiteral("Склад")
            : stageField ? backend->stages[1].name : QStringLiteral("Вера");
        const auto candidates = list->findItems(destination, Qt::MatchExactly);
        QCOMPARE(candidates.size(), 1);
        list->setCurrentItem(candidates.first());
        QCOMPARE(backend->taskUpdateRequests, 0);
        if (project) {
            const auto target = list->visualItemRect(candidates.first()).center();
            QVERIFY(list->viewport()->rect().contains(target));
            QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, target);
        } else {
            QTest::keyClick(activation, Qt::Key_Return);
        }
        QCOMPARE(backend->taskUpdateRequests, 1);
        const auto verifyPatch = [&] {
            QCOMPARE(backend->updatedTask, QStringLiteral("501"));
            QCOMPARE(backend->lastUpdate.projectId, project ? std::optional<QString>(QStringLiteral("102")) : std::nullopt);
            QCOMPARE(backend->lastUpdate.stageId, stageField ? std::optional<QString>(QStringLiteral("602")) : std::nullopt);
            QCOMPARE(backend->lastUpdate.assigneeId, !project && !stageField
                         ? std::optional<QString>(QStringLiteral("14")) : std::nullopt);
        };
        verifyPatch();
        QTest::keyClick(activation, Qt::Key_Return);
        QCOMPARE(backend->taskUpdateRequests, 1);

        emit backend->failed(QStringLiteral("update-task:501"), QStringLiteral("Портал отклонил изменение"));
        retry = findAccessible<QPushButton>(popup, retryName);
        QTRY_VERIFY(retry && retry->isVisible() && retry->isEnabled());
        if (project || stageField)
            list->setCurrentItem(confirmed);
        retry->click();
        QCOMPARE(backend->taskUpdateRequests, 2);
        verifyPatch();
        if (project) {
            task.projectId = QStringLiteral("102");
            task.project = destination;
        } else if (stageField) {
            task.stageId = QStringLiteral("602");
            task.stageName = destination;
        } else {
            task.assigneeId = QStringLiteral("14");
            task.assignee = destination;
        }
        emit backend->taskUpdated(task.id);
        QTRY_VERIFY(!popup->isVisible());
    }

    void taskCardAppliesOnePermittedRequisiteAndKeepsDraftOnConflict()
    {
        Task task;
        task.id = QStringLiteral("501");
        task.name = QStringLiteral("Монтаж");
        task.projectId = QStringLiteral("101");
        task.project = QStringLiteral("Стройка");
        task.stageId = QStringLiteral("601");
        task.stageName = QStringLiteral("Работа");
        task.createdBy = QStringLiteral("Постановщик");
        task.creatorId = QStringLiteral("11");
        task.assignee = QStringLiteral("Исполнитель");
        task.assigneeId = QStringLiteral("12");
        task.observers = {QStringLiteral("Анна")};
        task.observerPeople = {{QStringLiteral("13"), QStringLiteral("Анна")}};
        task.due = QStringLiteral("2026-11-23T09:00:00+03:00");
        task.createdAt = QDateTime(QDate(2026, 11, 15), QTime(12, 40));
        task.statusCode = QStringLiteral("3");
        task.status = QStringLiteral("Выполняется");
        task.editRights.title = true;
        task.editRights.assignee = true;
        task.editRights.observers = true;
        task.editRights.deadline = true;
        task.editRights.project = true;
        task.editRights.stage = true;
        task.statusActions = {TaskStatusAction::Pause, TaskStatusAction::Complete};
        task.deadlineReasonRequired = true;

        QWidget parent;
        bx::ui::TaskCardDialog card(task, task.projectId, &parent);
        card.setProjectChoices({{QStringLiteral("102"), QStringLiteral("Склад"), {}}});
        card.setWriteState(true, false, false, false, false);
        QString request;
        QString query;
        card.setPeopleSearch([&](const QString &id, const QString &term) {
            request = id;
            query = term;
        });
        QVector<TaskEdit> edits;
        card.setOnSave([&edits](const TaskEdit &edit) { edits.append(edit); });
        card.show();
        QApplication::setActiveWindow(&card);
        const auto *deadline = findAccessible<QLabel>(&card, QStringLiteral("Крайний срок"));
        const auto *created = findAccessible<QLabel>(&card, QStringLiteral("Дата создания"));
        QVERIFY(deadline && created);
        QCOMPARE(deadline->text(), QDateTime::fromString(task.due, Qt::ISODate)
                                   .toLocalTime().toString(QStringLiteral("dd.MM.yyyy  HH:mm")));
        QCOMPARE(created->text(), QStringLiteral("15.11.2026  12:40"));
        auto *apply = findAccessible<QPushButton>(&card, QStringLiteral("Применить изменение реквизита"));
        auto *cancel = findAccessible<QPushButton>(&card, QStringLiteral("Отменить изменение реквизита"));
        QVERIFY(apply && cancel);
        QVERIFY(!findAccessible<QPushButton>(&card, QStringLiteral("Изменить реквизиты задачи")));
        const auto row = [&card](const QString &label) {
            return findAccessible<QPushButton>(&card, label);
        };
        QVERIFY(row(QStringLiteral("Постановщик")) && !row(QStringLiteral("Постановщик"))->isEnabled());
        QVERIFY(!row(QStringLiteral("Дата создания")));
        auto *observers = row(QStringLiteral("Наблюдатели"));
        QVERIFY(observers && observers->isEnabled());
        QVERIFY(findAccessible<QLabel>(observers, QStringLiteral("Анна")));
        observers->setFocus();
        QTest::keyClick(observers, Qt::Key_Space);
        auto *search = findAccessible<QLineEdit>(&card, QStringLiteral("Поиск сотрудника · Наблюдатели"));
        QVERIFY(search && search->isVisible());
        auto *popup = card.findChild<QFrame *>(QStringLiteral("taskRequisitePopup"));
        QVERIFY(popup && popup->isVisible() && popup->isAncestorOf(search));
        QVERIFY(card.rect().contains(popup->geometry()));
        QTest::keyClicks(search, "bor");
        QTRY_COMPARE(query, QStringLiteral("bor"));
        card.setPeopleResults(QStringLiteral("stale"), {{QStringLiteral("99"), QStringLiteral("Чужой ответ"), {}, {}}});
        QVERIFY(!apply->isEnabled());
        card.setPeopleResults(request, {{QStringLiteral("14"), QStringLiteral("Борис"), {}, {}}});
        if (const auto path = qEnvironmentVariable("B24_REQUISITE_POPUP_DIR"); !path.isEmpty()) {
            QCoreApplication::processEvents();
            QVERIFY(card.grab().save(path + QStringLiteral("/people-popup.png")));
        }
        QTest::keyClick(search, Qt::Key_Return);
        QVERIFY(card.hasUnsavedChanges() && apply->isEnabled());
        auto *added = findAccessible<QPushButton>(&card, QStringLiteral("Удалить наблюдателя Борис"));
        QVERIFY(added);
        QTRY_VERIFY(added->isVisible());
        apply->click();
        QCOMPARE(edits.size(), 1);
        const QStringList oldObservers{QStringLiteral("13")};
        const QStringList newObservers{QStringLiteral("13"), QStringLiteral("14")};
        QCOMPARE(edits.last().originalObserverIds, std::optional<QStringList>(oldObservers));
        QCOMPARE(edits.last().observerIds, std::optional<QStringList>(newObservers));
        QVERIFY(!edits.last().creatorId && !edits.last().deadline && !edits.last().statusAction);
        auto *removeOriginal = findAccessible<QPushButton>(&card, QStringLiteral("Удалить наблюдателя Анна"));
        QVERIFY(removeOriginal);
        removeOriginal->click();
        QVERIFY(card.hasUnsavedChanges() && !apply->isEnabled());

        task.observers.append(QStringLiteral("Вера"));
        task.observerPeople.append({QStringLiteral("15"), QStringLiteral("Вера")});
        card.setTask(task);
        QVERIFY(findAccessible<QLabel>(observers, QStringLiteral("Вера")));
        QVERIFY(card.hasUnsavedChanges() && !apply->isEnabled());
        cancel->click();
        QVERIFY(!card.hasUnsavedChanges());
        QVERIFY(row(QStringLiteral("Крайний срок")));
        row(QStringLiteral("Крайний срок"))->click();
        auto *withoutDeadline = findAccessible<QCheckBox>(&card, QStringLiteral("Без срока"));
        auto *reason = findAccessible<QLineEdit>(&card, QStringLiteral("Причина изменения срока"));
        QVERIFY(withoutDeadline && reason && reason->isVisible());
        withoutDeadline->setChecked(true);
        QVERIFY(!apply->isEnabled());
        reason->setText(QStringLiteral("Новая оценка"));
        QVERIFY(apply->isEnabled());
        apply->click();
        QCOMPARE(edits.size(), 2);
        QCOMPARE(edits.last().deadline, std::optional<QString>(QString()));
        QCOMPARE(edits.last().deadlineReason, std::optional<QString>(QStringLiteral("Новая оценка")));
        card.acceptEdit();

        QVERIFY(row(QStringLiteral("Статус")));
        row(QStringLiteral("Статус"))->click();
        auto *status = findAccessible<QComboBox>(&card, QStringLiteral("Статус задачи"));
        QVERIFY(status && status->isVisible());
        QCOMPARE(status->count(), 3);
        QCOMPARE(status->itemText(1), QStringLiteral("Приостановить"));
        status->setCurrentIndex(2);
        apply->click();
        QCOMPARE(edits.last().statusAction, std::optional<TaskStatusAction>(TaskStatusAction::Complete));
        card.acceptEdit();

        card.setStages({{QStringLiteral("601"), QStringLiteral("Работа"), {}, 0}});
        QVERIFY(row(QStringLiteral("Проект")));
        row(QStringLiteral("Проект"))->click();
        auto *project = findAccessible<QListWidget>(&card, QStringLiteral("Проект задачи"));
        QVERIFY(project && project->isVisible());
        auto *projectSearch = findAccessible<QLineEdit>(&card, QStringLiteral("Поиск проекта"));
        QVERIFY(projectSearch && projectSearch->isVisible() && popup->isAncestorOf(projectSearch));
        auto *projectField = projectSearch->parentWidget();
        QCOMPARE(projectField->objectName(), QStringLiteral("taskSelectionField"));
        auto *currentProjectPill = projectField->findChild<QLabel *>(QStringLiteral("taskInlineSelectionValue"));
        QVERIFY(currentProjectPill);
        QCOMPARE(currentProjectPill->text(), QStringLiteral("Стройка"));
        if (const auto path = qEnvironmentVariable("B24_REQUISITE_POPUP_DIR"); !path.isEmpty()) {
            QCoreApplication::processEvents();
            QVERIFY(card.grab().save(path + QStringLiteral("/project-popup.png")));
        }
        QTest::keyClick(projectSearch, Qt::Key_Escape);
        QTRY_VERIFY(!popup->isVisible());
        QVERIFY(row(QStringLiteral("Проект"))->isEnabled());
        row(QStringLiteral("Проект"))->click();
        QVERIFY(projectSearch->isVisible());
        projectSearch->setText(QStringLiteral("Склад"));
        QCOMPARE(project->count(), 1);
        const auto previousEdits = edits.size();
        project->setCurrentRow(0);
        QCOMPARE(edits.size(), previousEdits);
        QTest::keyClick(projectSearch, Qt::Key_Return);
        QCOMPARE(edits.size(), previousEdits + 1);
        QCOMPARE(edits.last().projectId, std::optional<QString>(QStringLiteral("102")));
        QCOMPARE(apply->accessibleName(), QStringLiteral("Повторить изменение реквизита"));
        QVERIFY(apply->isEnabled());
        auto portalProject = task;
        portalProject.projectId = QStringLiteral("105");
        portalProject.project = QStringLiteral("Архив");
        card.setTask(portalProject);
        QVERIFY(!apply->isEnabled());
        QCOMPARE(projectSearch->text(), QStringLiteral("Склад"));
        bool foundProjectConflict = false;
        for (const auto *label : projectField->parentWidget()->parentWidget()->parentWidget()->findChildren<QLabel *>())
            foundProjectConflict |= label->isVisible() && label->text().contains(QStringLiteral("Сейчас на портале: Архив"));
        QVERIFY(foundProjectConflict);
        card.setTask(task);
        QVERIFY(apply->isEnabled());
        projectSearch->setText(QStringLiteral("Стройка"));
        QVERIFY(!apply->isEnabled());
        apply->click();
        QCOMPARE(edits.size(), previousEdits + 1);
        projectSearch->clear();
        QVERIFY(project->currentItem());
        QCOMPARE(project->currentItem()->text(), QStringLiteral("Склад"));
        QVERIFY(apply->isEnabled());
        apply->click();
        QCOMPARE(edits.size(), previousEdits + 2);
        QCOMPARE(edits.last().projectId, std::optional<QString>(QStringLiteral("102")));
        card.acceptEdit();
        QCOMPARE(card.projectId(), QStringLiteral("102"));
        auto *stage = findAccessible<QListWidget>(&card, QStringLiteral("Этап задачи"));
        QVERIFY(stage);
        QCOMPARE(stage->count(), 0);

        card.setCanDetachProject(true);
        row(QStringLiteral("Проект"))->click();
        const auto noProject = project->findItems(QStringLiteral("Без проекта"), Qt::MatchExactly);
        QCOMPARE(noProject.size(), 1);
        project->setCurrentItem(noProject.first());
        QTest::keyClick(project, Qt::Key_Return);
        QCOMPARE(edits.last().projectId, std::optional<QString>(QStringLiteral("0")));
        card.acceptEdit();
        QVERIFY(card.projectId().isEmpty());
    }

    void taskPeoplePickerLoadsInitialSuggestionsAndRestoresAfterTyping()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto &task = backend->stages.first().tasks.first();
        task.assigneeId = QStringLiteral("12");
        task.assignee = QStringLiteral("Анна");
        task.editRights.assignee = true;
        backend->holdTaskUpdate = true;
        backend->people = {{QStringLiteral("12"), QStringLiteral("Анна"), {}, {}},
                           {QStringLiteral("13"), QStringLiteral("Вера"), {}, {}},
                           {QStringLiteral("21"), QStringLiteral("Алексей"), {}, {}},
                           {QStringLiteral("22"), QStringLiteral("Галина"), {}, {}},
                           {QStringLiteral("23"), QStringLiteral("Ирина"), {}, {}},
                           {QStringLiteral("24"), QStringLiteral("Сергей"), {}, {}},
                           {QStringLiteral("25"), QStringLiteral("Владислав"), {}, {}},
                           {QStringLiteral("26"), QStringLiteral("Татьяна"), {}, {}}};
        const auto initialCount = backend->people.size();
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        window.openTaskCard(task.id);
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        QCOMPARE(backend->peopleRequests.size(), 0);
        auto *assignee = findAccessible<QPushButton>(card, QStringLiteral("Исполнитель"));
        QVERIFY(assignee && assignee->isEnabled());
        assignee->click();
        auto *search = findAccessible<QLineEdit>(card, QStringLiteral("Поиск сотрудника · Исполнитель"));
        QVERIFY(search && search->isVisible());
        QTRY_COMPARE_WITH_TIMEOUT(backend->peopleRequests.size(), 1, 1000);
        const QPair<QString, QString> initialRequest;
        QCOMPARE(backend->peopleRequests.first(), initialRequest);
        const auto initialId = backend->lastPeopleRequestId;
        QListWidget *suggestions = nullptr;
        for (auto *list : card->findChildren<QListWidget *>()) {
            if (list->isVisible() && list->accessibleName() == QStringLiteral("Найденные сотрудники"))
                suggestions = list;
        }
        QVERIFY(suggestions);
        QCOMPARE(suggestions->count(), initialCount);
        QTRY_VERIFY(suggestions->viewport()->visibleRegion().boundingRect().height()
                    >= 6 * suggestions->sizeHintForRow(0));
        QCOMPARE(suggestions->item(0)->text(), QStringLiteral("Анна"));
        QCOMPARE(suggestions->item(0)->checkState(), Qt::Checked);
        QVERIFY(!suggestions->item(0)->icon().isNull());
        auto *popup = card->findChild<QFrame *>(QStringLiteral("taskRequisitePopup"));
        QVERIFY(popup);
        auto *field = search->parentWidget();
        QCOMPARE(field->objectName(), QStringLiteral("taskSelectionField"));
        const auto pillValues = [](const QWidget *inputField) {
            QList<QLabel *> labels;
            for (auto *label : inputField->findChildren<QLabel *>(QStringLiteral("taskInlineSelectionValue")))
                if (label->isVisible())
                    labels.append(label);
            return labels;
        };
        QTRY_COMPARE(pillValues(field).size(), 1);
        QCOMPARE(pillValues(field).front()->text(), QStringLiteral("Анна"));
        QVERIFY(!pillValues(field).front()->parentWidget()->property("pending").toBool());
        QVERIFY(search->width() >= 100);
        for (const auto *summary : popup->findChildren<QLabel *>()) {
            if (summary->accessibleName() == QStringLiteral("Текущее значение"))
                QVERIFY(!summary->isVisible());
        }
        QTest::keyClick(search, Qt::Key_Down);
        QTest::keyClick(search, Qt::Key_Down);
        QCOMPARE(suggestions->currentItem()->text(), QStringLiteral("Вера"));
        QCOMPARE(suggestions->item(0)->checkState(), Qt::Checked);
        QCOMPARE(pillValues(field).front()->text(), QStringLiteral("Анна"));
        QVERIFY(!pillValues(field).front()->parentWidget()->property("pending").toBool());

        if (const auto dir = qEnvironmentVariable("B24_PICKER_DESIGN_DIR"); !dir.isEmpty()) {
            QCoreApplication::processEvents();
            QVERIFY(card->grab().save(dir + QStringLiteral("/people-wide.png")));
        }

        backend->people = {{QStringLiteral("14"), QStringLiteral("Борис"), {}, {}}};
        QTest::keyClicks(search, "bor");
        QTRY_COMPARE(backend->peopleRequests.size(), 2);
        QCOMPARE(backend->peopleRequests.last().first, QStringLiteral("bor"));
        QCOMPARE(suggestions->count(), 1);
        QCOMPARE(suggestions->item(0)->text(), QStringLiteral("Борис"));
        const auto *status = findAccessible<QLabel>(&window, QStringLiteral("Состояние"));
        QVERIFY(status);
        const auto previousStatus = status->text();
        emit backend->failed(QStringLiteral("search-people:") + initialId, QStringLiteral("Просроченный поиск"));
        QCOMPARE(status->text(), previousStatus);
        QCOMPARE(suggestions->item(0)->text(), QStringLiteral("Борис"));
        auto portalTask = task;
        portalTask.assignee = QStringLiteral("Галина");
        portalTask.assigneeId = QStringLiteral("15");
        emit backend->taskLoaded(portalTask);
        QCOMPARE(search->text(), QStringLiteral("bor"));
        QTRY_COMPARE(pillValues(field).size(), 1);
        QCOMPARE(pillValues(field).front()->text(), QStringLiteral("Галина"));
        QVERIFY(!pillValues(field).front()->parentWidget()->property("pending").toBool());
        portalTask.assignee = QStringLiteral("Галина Петрова");
        emit backend->taskLoaded(portalTask);
        QTRY_COMPARE(pillValues(field).size(), 1);
        QCOMPARE(pillValues(field).front()->text(), QStringLiteral("Галина Петрова"));
        emit backend->taskLoaded(task);
        QTest::keyClick(search, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(search, Qt::Key_Backspace);
        QTRY_COMPARE(suggestions->count(), initialCount);
        QCOMPARE(suggestions->item(0)->checkState(), Qt::Checked);
        QCOMPARE(suggestions->item(1)->text(), QStringLiteral("Вера"));
        QCOMPARE(backend->peopleRequests.size(), 2);
        const int wideListHeight = suggestions->height();
        card->resize(660, 420);
        QTRY_VERIFY(suggestions->height() < wideListHeight);
        QVERIFY(card->rect().contains(popup->geometry()));
        if (const auto dir = qEnvironmentVariable("B24_PICKER_DESIGN_DIR"); !dir.isEmpty()) {
            QCoreApplication::processEvents();
            QVERIFY(card->grab().save(dir + QStringLiteral("/people-compact.png")));
        }

        for (int i = 0; i < 2; ++i)
            QTest::keyClick(search, Qt::Key_Down);
        QTest::keyClick(search, Qt::Key_Return);
        QCOMPARE(backend->lastUpdate.assigneeId, std::optional<QString>(QStringLiteral("13")));
        emit backend->failed(QStringLiteral("update-task:501"), QStringLiteral("Портал отклонил изменение"));
        QTRY_COMPARE(pillValues(field).size(), 1);
        QCOMPARE(pillValues(field).front()->text(), QStringLiteral("Вера"));
        QVERIFY(pillValues(field).front()->parentWidget()->property("pending").toBool());
        QCOMPARE(suggestions->item(0)->checkState(), Qt::Checked);
        QVERIFY(suggestions->item(1)->data(bx::ui::TaskPickerDraft).toBool());
        auto *cancel = findAccessible<QPushButton>(popup, QStringLiteral("Отменить изменение реквизита"));
        QVERIFY(cancel && cancel->isVisible());
        auto *applyPerson = findAccessible<QPushButton>(popup, QStringLiteral("Повторить изменение реквизита"));
        QVERIFY(applyPerson && applyPerson->isEnabled());
        emit backend->taskLoaded(portalTask);
        QVERIFY(!applyPerson->isEnabled());
        QTRY_COMPARE(pillValues(field).size(), 1);
        QCOMPARE(pillValues(field).front()->text(), QStringLiteral("Вера"));
        QVERIFY(pillValues(field).front()->parentWidget()->property("pending").toBool());
        const auto labels = popup->findChildren<QLabel *>();
        QVERIFY(std::any_of(labels.cbegin(), labels.cend(), [](const QLabel *label) {
            return label->isVisible() && label->text().contains(QStringLiteral("Сейчас на портале: Галина Петрова"));
        }));
        cancel->click();
        auto updated = task;
        updated.editRights.observers = true;
        updated.observers = {
            QStringLiteral("Анастасия Александровна Константинопольская"),
            QStringLiteral("Екатерина Дмитриевна Великогородская"),
            QStringLiteral("Валентина Вячеславовна Новосибирская")};
        for (int i = 0; i < updated.observers.size(); ++i)
            updated.observerPeople.append({QString::number(100 + i), updated.observers.at(i), {}});
        emit backend->taskLoaded(updated);
        backend->people = {{QStringLiteral("200"), QStringLiteral("Новый наблюдатель"), {}, {}}};
        auto *observers = findAccessible<QPushButton>(card, QStringLiteral("Наблюдатели"));
        QVERIFY(observers && observers->isEnabled());
        observers->click();
        auto *observerSearch = findAccessible<QLineEdit>(popup, QStringLiteral("Поиск сотрудника · Наблюдатели"));
        QVERIFY(observerSearch && observerSearch->isVisible());
        QTRY_COMPARE(backend->peopleRequests.size(), 3);
        const auto *observerField = observerSearch->parentWidget();
        while (observerField && observerField->objectName() != QStringLiteral("taskSelectionField"))
            observerField = observerField->parentWidget();
        QVERIFY(observerField);
        QTRY_COMPARE(pillValues(observerField).size(), 3);
        const auto *firstValue = pillValues(observerField).front();
        QCOMPARE(firstValue->accessibleName(), updated.observers.first());
        QTRY_COMPARE(firstValue->text(), updated.observers.first());
        const auto fieldRect = [observerField](const QWidget *child) {
            return QRect(child->mapTo(observerField, QPoint()), child->size());
        };
        const auto lastChip = [&] { return fieldRect(pillValues(observerField).last()->parentWidget()); };
        QTRY_VERIFY(lastChip().top() > fieldRect(firstValue->parentWidget()).top());
        const auto wrapsInOrder = [&] {
            for (const auto *value : pillValues(observerField)) {
                const auto *chip = value->parentWidget();
                if (!observerField->rect().contains(fieldRect(chip))
                    || !chip->visibleRegion().boundingRect().contains(chip->rect()))
                    return false;
            }
            const auto input = fieldRect(observerSearch);
            return input.top() > lastChip().bottom() || input.left() > lastChip().right();
        };
        QVERIFY(wrapsInOrder());
        QTest::keyClick(observerSearch, Qt::Key_Down);
        QTest::keyClick(observerSearch, Qt::Key_Return);
        QTRY_COMPARE(pillValues(observerField).size(), 4);
        QCOMPARE(pillValues(observerField).last()->accessibleName(), QStringLiteral("Новый наблюдатель"));
        QVERIFY(observerSearch->width() >= 100);
        QTRY_VERIFY(wrapsInOrder());
        QVERIFY(pillValues(observerField).last()->parentWidget()->property("pending").toBool());
        auto *apply = findAccessible<QPushButton>(popup, QStringLiteral("Применить изменение реквизита"));
        QVERIFY(apply && apply->isEnabled());
        const auto withinPopup = [popup](const QWidget *control) {
            return popup->rect().contains(QRect(control->mapTo(popup, QPoint()), control->size()));
        };
        QTRY_VERIFY(withinPopup(apply) && withinPopup(cancel));
        if (const auto dir = qEnvironmentVariable("B24_OBSERVER_WRAP_DIR"); !dir.isEmpty()) {
            QVERIFY(popup->grab().save(dir + QStringLiteral("/observers-narrow.png")));
            card->resize(1070, 650);
            QTRY_VERIFY(popup->width() >= 390);
            QTRY_VERIFY(wrapsInOrder());
            QCoreApplication::processEvents();
            QVERIFY(popup->grab().save(dir + QStringLiteral("/observers-wide.png")));
        }
        auto *remove = findAccessible<QPushButton>(popup, QStringLiteral("Удалить наблюдателя Новый наблюдатель"));
        QVERIFY(remove && remove->isVisible());
        remove->click();
        QTRY_COMPARE(pillValues(observerField).size(), 3);
        QVERIFY(!apply->isEnabled());
    }

    void taskRequisitesLoadAndPaintScopedPortraits()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        const QUrl creator(QStringLiteral("https://example.bitrix24.ru/upload/card-creator.png"));
        const QUrl assignee(QStringLiteral("https://example.bitrix24.ru/upload/card-assignee.png"));
        const QUrl observer(QStringLiteral("https://example.bitrix24.ru/upload/card-observer.png"));
        const QUrl project(QStringLiteral("https://example.bitrix24.ru/upload/card-project.png"));
        const QColor color("#bf327c");
        QImage image(32, 32, QImage::Format_RGB32);
        image.fill(color);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(image.save(&buffer, "PNG"));
        for (const auto &url : {creator, assignee, observer, project})
            backend->avatarImages.insert(url, bytes);
        auto &task = backend->stages.first().tasks.first();
        task.projectId = backend->projects.first().id;
        task.status = QStringLiteral("Выполняется");
        task.statusCode = QStringLiteral("3");
        task.statusActions = {TaskStatusAction::Complete};
        task.createdAt = QDateTime(QDate(2026, 1, 1), QTime(9, 15));
        task.creatorId = QStringLiteral("72");
        task.createdBy = QStringLiteral("Алексей Смирнов");
        task.creatorAvatarUrl = creator;
        task.assigneeId = QStringLiteral("73");
        task.assignee = QStringLiteral("Ирина Волкова");
        task.assigneeAvatarUrl = assignee;
        task.observerPeople = {{QStringLiteral("74"), QStringLiteral("Анна Петрова"), observer},
                               {QStringLiteral("75"), QStringLiteral("Иван Соколов"), {}}};
        task.observers = {QStringLiteral("Анна Петрова"), QStringLiteral("Иван Соколов")};
        backend->projects.first().avatarUrl = project;
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        window.openTaskCard(task.id);
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        card->resize(660, 420);
        auto *details = card->findChild<QScrollArea *>(QStringLiteral("taskDetailsScroll"));
        QVERIFY(details);
        const auto checkPhoto = [&](const QString &caption, const QUrl &url) {
            auto *row = findAccessible<QPushButton>(card, caption);
            QVERIFY(row);
            auto *badge = row->findChild<QWidget *>(QStringLiteral("taskRequisiteAvatar"));
            QVERIFY2(badge, qPrintable(caption));
            details->ensureWidgetVisible(badge, 4, 4);
            QTRY_VERIFY(backend->avatarRequests.contains(url));
            const auto centerColor = [badge] {
                const auto pixels = badge->grab().toImage();
                return pixels.pixelColor(pixels.rect().center());
            };
            QTRY_COMPARE(centerColor(), color);
            QCOMPARE(backend->avatarRequests.count(url), 1);
        };
        checkPhoto(QStringLiteral("Постановщик"), creator);
        checkPhoto(QStringLiteral("Исполнитель"), assignee);
        checkPhoto(QStringLiteral("Проект"), project);
        checkPhoto(QStringLiteral("Наблюдатели"), observer);
        const auto *observers = findAccessible<QPushButton>(card, QStringLiteral("Наблюдатели"));
        QVERIFY(observers);
        QCOMPARE(observers->findChildren<QWidget *>(QStringLiteral("taskRequisiteAvatar")).size(), 2);
        if (const auto output = qEnvironmentVariable("B24_REQUISITE_SCREENSHOT"); !output.isEmpty()) {
            card->resize(900, 650);
            details->ensureWidgetVisible(findAccessible<QPushButton>(card, QStringLiteral("Наблюдатели")), 4, 4);
            QCoreApplication::processEvents();
            QVERIFY(card->grab().save(output));
        }
    }

    void taskFilesPaintSmallJpegOriginalWhenVisible()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto &task = backend->stages.first().tasks.first();
        task.description = QStringLiteral("Описание\n").repeated(30);
        MessageFile photo;
        photo.id = QStringLiteral("90");
        photo.name = QStringLiteral("Упаковка.jpg");
        photo.size = 173000;
        photo.isImage = true;
        photo.downloadUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/bitrix/tools/disk/uf.php?action=download&attachedId=90"));
        photo.previewUrl = photo.downloadUrl;
        task.files = {photo};
        task.attachments = {photo.name};
        const QColor fill("#a46640");
        QByteArray bytes;
        QBuffer buffer(&bytes);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QImage jpeg(240, 140, QImage::Format_RGB32);
        jpeg.fill(fill);
        QVERIFY(jpeg.save(&buffer, "JPEG", 92));
        backend->messageImages.insert(photo.previewUrl, bytes);

        MainWindow window(backend.get());
        window.resize(960, 690);
        window.show();
        window.openTaskCard(task.id);
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        card->resize(760, 520);
        auto *details = card->findChild<QScrollArea *>(QStringLiteral("taskDetailsScroll"));
        auto *files = findAccessible<QListWidget>(card, QStringLiteral("Вложения задачи"));
        QVERIFY(details && files);
        details->setFixedHeight(110);
        QTRY_COMPARE(files->count(), 1);
        QTRY_VERIFY(files->height() > 0);
        QTRY_VERIFY(details->verticalScrollBar()->maximum() > 0);
        details->verticalScrollBar()->setValue(details->verticalScrollBar()->minimum());
        QTRY_VERIFY(!files->viewport()->visibleRegion().intersects(files->visualItemRect(files->item(0))));
        QVERIFY(!backend->imageRequests.contains(photo.previewUrl));

        details->ensureWidgetVisible(files, 0, 6);
        QTRY_VERIFY(files->viewport()->visibleRegion().intersects(files->visualItemRect(files->item(0))));
        QTRY_COMPARE(backend->imageRequests.count(photo.previewUrl), 1);
        QTRY_VERIFY(files->visualItemRect(files->item(0)).height() > 110);
        QTRY_VERIFY(files->visualItemRect(files->item(0)).bottom() < files->viewport()->height());
        details->setFixedHeight(300);
        details->ensureWidgetVisible(files);
        const auto row = files->visualItemRect(files->item(0));
        const auto sample = files->viewport()->grab().toImage().pixelColor(row.left() + 65, row.top() + 45);
        QVERIFY(qAbs(sample.red() - fill.red()) < 5 && qAbs(sample.green() - fill.green()) < 5
                && qAbs(sample.blue() - fill.blue()) < 5);
        if (const auto capture = qEnvironmentVariable("B24_TASK_FILES_SCREENSHOT"); !capture.isEmpty())
            QVERIFY(files->viewport()->grab().save(capture));
        QTest::mouseClick(files->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(row.left() + 65, row.top() + 45));
        QTRY_VERIFY(card->findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));
        QCOMPARE(backend->photoUrl, photo.downloadUrl);
    }

    void taskCardShowsAuthorizedSummaryWhileFinalDetailsArePending()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdTaskDetails = true;
        backend->accountScope = QStringLiteral("fixture/task-summary-first");
        MainWindow window(backend.get());
        window.resize(960, 690);
        window.show();
        window.onConnectionChanged(true);
        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        auto *title = findAccessible<QLabel>(card, QStringLiteral("Название открытой задачи"));
        auto *status = findAccessible<QLabel>(card, QStringLiteral("Состояние карточки"));
        auto *files = findAccessible<QListWidget>(card, QStringLiteral("Вложения задачи"));
        auto *edit = findAccessible<QPushButton>(card, QStringLiteral("Изменить задачу"));
        auto *attach = findAccessible<QPushButton>(card, QStringLiteral("Прикрепить файл к задаче"));
        auto *input = findAccessible<QPlainTextEdit>(card, QStringLiteral("Текст в чате карточки"));
        auto *history = static_cast<ui::TaskCardDialog *>(card)->messageList();
        QVERIFY(title && status && files && edit && attach && input);
        QVERIFY(status->isVisible());
        QVERIFY(backend->taskChatReadRequests.isEmpty());
        QCOMPARE(files->item(0)->data(workspace::Detail).toString(), QStringLiteral("Файлы ещё не загружены"));
        const auto pinsBeforeSummary = backend->pinnedRequests;

        Task summary = backend->stages.first().tasks.first();
        summary.id = QStringLiteral("502");
        summary.name = QStringLiteral("Чужая задача");
        summary.chatDialogId = QStringLiteral("chat42");
        emit backend->taskSummaryLoaded(summary);
        QVERIFY(title->text() != summary.name);
        QVERIFY(backend->taskChatReadRequests.isEmpty());
        QCOMPARE(backend->pinnedRequests, pinsBeforeSummary);
        summary.id = QStringLiteral("501");
        summary.name = QStringLiteral("Краткие данные задачи");
        emit backend->taskSummaryLoaded(summary);
        QCOMPARE(title->text(), summary.name);
        QVERIFY(status->isHidden());
        QVERIFY(!edit->isEnabled() && !attach->isEnabled() && input->isReadOnly());
        QCOMPARE(files->item(0)->data(workspace::Detail).toString(), QStringLiteral("Файлы ещё не загружены"));
        QCOMPARE(backend->taskChatReadRequests, QStringList{summary.id});
        const auto expectedPins = pinsBeforeSummary + QStringList{summary.chatDialogId};
        QCOMPARE(backend->pinnedRequests, expectedPins);
        QTRY_COMPARE(history->count(), backend->taskMessages.size());

        auto full = summary;
        full.name = QStringLiteral("Полная карточка");
        emit backend->taskLoaded(full);
        QCOMPARE(title->text(), full.name);
        QVERIFY(edit->isEnabled() && attach->isEnabled() && !input->isReadOnly());
        QCOMPARE(files->item(0)->data(workspace::Detail).toString(), QStringLiteral("Файлов пока нет"));
        emit backend->taskSummaryLoaded(summary);
        QCOMPARE(title->text(), full.name);
        QCOMPARE(backend->taskChatReadRequests, QStringList{summary.id});
        QCOMPARE(backend->pinnedRequests, expectedPins);

        card->close();
        QTRY_VERIFY(!window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        window.openTaskCard(summary.id);
        card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        const QPointer<QDialog> previousCard(card);
        backend->accountScope = QStringLiteral("fixture/task-summary-second");
        emit backend->accountScopeChanged();
        emit backend->taskSummaryLoaded(summary);
        QTRY_VERIFY(previousCard.isNull());
        QVERIFY(!window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        QCOMPARE(backend->taskChatReadRequests, QStringList{summary.id});
    }

    void taskCardShowsRichDetailsAndEmbedsPasteOnConfirmedUpload()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto &task = backend->stages.first().tasks.first();
        task.description = QStringLiteral("[B]Проверить крепёж[/B]\n[custom]Служебный текст[/custom]\n"
                                          "[disk file id=n12345 width=240 height=150]\n[disk file id=n99999]");
        task.descriptionChecksum = QStringLiteral("test-checksum");
        task.createdBy = QStringLiteral("Алексей Смирнов");
        task.assignee = QStringLiteral("Ирина Волкова");
        task.project = QStringLiteral("Рабочий проект");
        task.status = QStringLiteral("В работе");
        task.createdAt = QDateTime(QDate(2026, 4, 12), QTime(11, 0));
        task.observers = {QStringLiteral("Ольга Иванова")};
        MessageFile photo;
        photo.id = QStringLiteral("5643");
        photo.diskObjectId = QStringLiteral("n12345");
        photo.name = QStringLiteral("Крепёж.png");
        photo.isImage = true;
        photo.previewSize = QSize(240, 150);
        photo.previewUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/preview/task-463"));
        photo.downloadUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/download/task-463"));
        task.files = {photo};
        task.attachments = {photo.name, QStringLiteral("Счёт.pdf")};
        const QColor fill("#9d694c");
        {
            QByteArray bytes;
            QBuffer buffer(&bytes);
            QVERIFY(buffer.open(QIODevice::WriteOnly));
            QImage image(240, 150, QImage::Format_RGB32);
            image.fill(fill);
            QVERIFY(image.save(&buffer, "PNG"));
            backend->messageImages.insert(photo.previewUrl, bytes);
        }
        MainWindow window(backend.get());
        window.resize(960, 690);
        window.show();
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        sections->setCurrentRow(1);
        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        auto *description = findAccessible<QListWidget>(card, QStringLiteral("Описание и встроенные файлы"));
        auto *attachments = findAccessible<QListWidget>(card, QStringLiteral("Вложения задачи"));
        auto *creator = findAccessible<QLabel>(card, QStringLiteral("Постановщик"));
        auto *editor = findAccessible<QTextEdit>(card, QStringLiteral("Описание задачи"));
        QVERIFY(description && attachments && creator && editor);
        auto *detailsPane = card->findChild<QWidget *>(QStringLiteral("taskDetailsPane"));
        QVERIFY(detailsPane && detailsPane->layout());
        auto *detailsLayout = detailsPane->layout();
        const int descriptionSection = detailsLayout->indexOf(description->parentWidget());
        QVERIFY(descriptionSection >= 0);
        QCOMPARE(detailsLayout->indexOf(attachments->parentWidget()), descriptionSection + 1);
        QVERIFY(detailsLayout->itemAt(descriptionSection + 2)->widget()->isAncestorOf(creator));
        QVERIFY(!editor->isVisible());
        QCOMPARE(creator->text(), task.createdBy);
        QCOMPARE(description->count(), 2);
        QCOMPARE(attachments->count(), 2);
        const auto rendered = description->item(0)->data(workspace::MarkupHtml).toString();
        QVERIFY(rendered.contains(QStringLiteral("<b>Проверить крепёж</b>")));
        QVERIFY(!rendered.contains(QStringLiteral("[disk file")));
        QVERIFY(rendered.contains(QStringLiteral("Вложение недоступно")));
        QTRY_COMPARE(backend->imageRequests.count(photo.previewUrl), 1);
        QTRY_VERIFY(description->visualItemRect(description->item(1)).height() > 110);
        QTRY_VERIFY(description->visualItemRect(description->item(1)).bottom() < description->viewport()->height());
        const auto capture = qEnvironmentVariable("B24_TASK_RICH_SCREENSHOT");
        if (!capture.isEmpty())
            QVERIFY(card->grab().save(capture));
        const auto rect = description->visualItemRect(description->item(1));
        const QPoint photoPoint(rect.left() + 32, rect.top() + 30);
        QContextMenuEvent photoMenuEvent(QContextMenuEvent::Mouse, photoPoint,
                                         description->viewport()->mapToGlobal(photoPoint));
        QCoreApplication::sendEvent(description->viewport(), &photoMenuEvent);
        QTRY_VERIFY(visibleMessageMenu(QStringLiteral("imageContextMenu")));
        visibleMessageMenu(QStringLiteral("imageContextMenu"))->actions().last()->trigger();
        QTRY_VERIFY(!visibleMessageMenu(QStringLiteral("imageContextMenu")));
        QCOMPARE(QGuiApplication::clipboard()->image().pixelColor(3, 3), fill);
        QTest::mouseClick(description->viewport(), Qt::LeftButton, Qt::NoModifier,
                          photoPoint);
        QTRY_VERIFY(card->findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));
        QCOMPARE(backend->photoUrl, photo.downloadUrl);
        card->findChild<QDialog *>(QStringLiteral("chatPhotoViewer"))->reject();
        QTRY_VERIFY(!card->findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));

        auto *edit = findAccessible<QPushButton>(card, QStringLiteral("Изменить задачу"));
        auto *save = findAccessible<QPushButton>(card, QStringLiteral("Сохранить задачу"));
        QVERIFY(edit && save);
        edit->click();
        QTRY_VERIFY(editor->isVisible() && !editor->isReadOnly());
        auto cursor = editor->textCursor();
        cursor.movePosition(QTextCursor::End);
        editor->setTextCursor(cursor);
        backend->attachmentEmbedId = QStringLiteral("n77777");
        const QImage pasted(20, 12, QImage::Format_RGB32);
        QGuiApplication::clipboard()->setImage(pasted);
        editor->paste();
        QCOMPARE(backend->attachedTask, task.id);
        QVERIFY(!QFileInfo::exists(backend->attachedPath));
        save->click();
        QVERIFY(backend->lastUpdate.description);
        QCOMPARE(backend->lastUpdate.originalDescription, std::optional<QString>(task.description));
        QCOMPARE(backend->lastUpdate.descriptionChecksum, std::optional<QString>(task.descriptionChecksum));
        QVERIFY(backend->lastUpdate.description->contains(QStringLiteral("[custom]Служебный текст[/custom]")));
        QVERIFY(backend->lastUpdate.description->contains(QStringLiteral("[disk file id=n12345")));
        QVERIFY(backend->lastUpdate.description->contains(QStringLiteral("[disk file id=n77777 width=20 height=12]")));
        QTRY_VERIFY(!editor->isVisible());
    }

    void taskDescriptionAttachmentQueueRefreshesAtEnd_data()
    {
        QTest::addColumn<bool>("failSecond");
        QTest::newRow("both confirmed") << false;
        QTest::newRow("partial success") << true;
    }

    void taskDescriptionAttachmentQueueRefreshesAtEnd()
    {
        QFETCH(bool, failSecond);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdTaskAttachment = true;
        backend->holdTaskUpdate = true;
        const auto taskId = QStringLiteral("501");
        auto &task = backend->stages.first().tasks.first();
        MainWindow window(backend.get());
        window.show();
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        sections->setCurrentRow(1);
        window.openTaskCard(taskId);
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        auto *edit = findAccessible<QPushButton>(card, QStringLiteral("Изменить задачу"));
        auto *save = findAccessible<QPushButton>(card, QStringLiteral("Сохранить задачу"));
        auto *editor = findAccessible<QTextEdit>(card, QStringLiteral("Описание задачи"));
        auto *files = findAccessible<QListWidget>(card, QStringLiteral("Вложения задачи"));
        QVERIFY(edit && save && editor && files);
        edit->click();
        editor->moveCursor(QTextCursor::End);
        const auto draft = QStringLiteral("Мой несохранённый черновик");
        editor->insertPlainText(draft);
        QTemporaryDir input;
        QVERIFY(input.isValid());
        const auto first = input.filePath(QStringLiteral("первый.txt"));
        const auto second = input.filePath(QStringLiteral("второй.txt"));
        for (const auto &path : {first, second}) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("content"), 7);
        }
        auto *clipboard = new QMimeData;
        clipboard->setUrls({QUrl::fromLocalFile(first), QUrl::fromLocalFile(second)});
        QGuiApplication::clipboard()->setMimeData(clipboard);
        const int previousReads = backend->taskReadRequests.count(taskId);
        editor->paste();
        QCOMPARE(backend->attachedPath, first);
        const auto acknowledge = [&](const QString &path, const QString &diskId) {
            MessageFile file;
            file.name = QFileInfo(path).fileName();
            file.diskObjectId = diskId;
            task.files.append(file);
            emit backend->taskFileAttached(taskId, diskId);
            emit backend->fileUploaded(taskId, QStringLiteral("task"));
        };
        acknowledge(first, QStringLiteral("n81"));
        QCOMPARE(backend->attachedPath, second);
        QCOMPARE(backend->taskReadRequests.count(taskId), previousReads);
        if (failSecond)
            emit backend->failed(QStringLiteral("attach-task:501"), QStringLiteral("Второй файл отклонён"));
        else
            acknowledge(second, QStringLiteral("n82"));
        QCOMPARE(backend->taskReadRequests.count(taskId), previousReads + 1);
        QCOMPARE(files->count(), failSecond ? 1 : 2);
        QCOMPARE(files->item(0)->data(workspace::MessageFiles).value<QVector<MessageFile>>().front().name,
                 QFileInfo(first).fileName());
        QVERIFY(editor->toPlainText().contains(draft));
        QVERIFY(save->isEnabled());
        save->click();
        QVERIFY(backend->lastUpdate.description);
        QVERIFY(backend->lastUpdate.description->contains(QStringLiteral("id=n81")));
        QCOMPARE(backend->lastUpdate.description->contains(QStringLiteral("id=n82")), !failSecond);
    }

    void resetsExistingTaskAttachmentOnAccountChange_data()
    {
        QTest::addColumn<bool>("description");
        QTest::newRow("task attachments") << false;
        QTest::newRow("description batch") << true;
    }

    void resetsExistingTaskAttachmentOnAccountChange()
    {
        QFETCH(bool, description);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->accountScope = QStringLiteral("fixture/existing-attachment-first");
        backend->holdTaskAttachment = true;
        const auto task = backend->stages.first().tasks.first();
        MainWindow window(backend.get());
        window.show();
        window.openTaskCard(task.id);
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        auto *attach = findAccessible<QPushButton>(card, QStringLiteral("Прикрепить файл к задаче"));
        auto *edit = findAccessible<QPushButton>(card, QStringLiteral("Изменить задачу"));
        auto *editor = findAccessible<QTextEdit>(card, QStringLiteral("Описание задачи"));
        auto *descriptionStatus = card->findChild<QLabel *>(QStringLiteral("taskDescriptionUploadStatus"));
        QVERIFY(attach && edit && editor && descriptionStatus);
        QTemporaryDir input;
        QVERIFY(input.isValid());
        const auto first = input.filePath(QStringLiteral("target.txt"));
        const auto second = input.filePath(QStringLiteral("queued.txt"));
        for (const auto &path : {first, second}) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("content"), 7);
        }
        const auto choose = [&window](QPushButton *button, const QString &path) {
            QTimer::singleShot(0, &window, [path] {
                if (auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                    dialog->selectFile(path);
                    QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
                }
            });
            button->click();
        };
        QTRY_VERIFY(attach->isEnabled());
        if (description) {
            edit->click();
            auto *clipboard = new QMimeData;
            clipboard->setUrls({QUrl::fromLocalFile(first), QUrl::fromLocalFile(second)});
            QGuiApplication::clipboard()->setMimeData(clipboard);
            editor->paste();
            QVERIFY(descriptionStatus->isVisible());
        } else {
            choose(attach, first);
        }
        QCOMPARE(backend->attachedPath, first);
        QVERIFY(!attach->isEnabled());
        const QPointer<QDialog> previousCard(card);
        backend->accountScope = QStringLiteral("fixture/existing-attachment-second");
        emit backend->accountScopeChanged();
        QTRY_VERIFY(previousCard.isNull());
        window.openTaskCard(task.id);
        card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        attach = findAccessible<QPushButton>(card, QStringLiteral("Прикрепить файл к задаче"));
        editor = findAccessible<QTextEdit>(card, QStringLiteral("Описание задачи"));
        descriptionStatus = card->findChild<QLabel *>(QStringLiteral("taskDescriptionUploadStatus"));
        QVERIFY(attach && editor && descriptionStatus);
        QVERIFY(attach->isEnabled());
        QVERIFY(descriptionStatus->isHidden());
        emit backend->taskFileAttached(task.id, QStringLiteral("n123456"));
        emit backend->fileUploaded(task.id, QStringLiteral("task"));
        QCOMPARE(backend->attachmentRequests.size(), 1);
        QVERIFY(!editor->toPlainText().contains(QStringLiteral("target.txt")));
        choose(attach, second);
        QCOMPARE(backend->attachmentRequests.size(), 2);
        QCOMPARE(backend->attachedTask, task.id);
        QCOMPARE(backend->attachedPath, second);
    }

    void taskCardMaximizeChangesNativeFrameGeometry()
    {
        const auto platform = QApplication::platformName();
        if (platform != QLatin1String("xcb") && !platform.startsWith(QLatin1String("wayland")))
            QSKIP("Requires a native window manager with enough room to maximize");
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        MainWindow window(backend.get());
        window.resize(970, 650);
        window.show();
        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        QVERIFY(QTest::qWaitForWindowExposed(card));
        const auto available = card->screen()->availableGeometry().size();
        if (available.width() < 1100 || available.height() < 770)
            QSKIP("Native test screen is too small to distinguish the restored frame");
        auto *maximize = findAccessible<QPushButton>(card, QStringLiteral("Развернуть карточку задачи"));
        QVERIFY(maximize);
        QCOMPARE(card->windowModality(), Qt::WindowModal);
        const auto restoredSize = card->frameGeometry().size();
        maximize->click();
        QTRY_VERIFY_WITH_TIMEOUT(card->frameGeometry().width() >= available.width() - 48
                                 && card->frameGeometry().height() >= available.height() - 72, 3000);
        QVERIFY(card->frameGeometry().width() > restoredSize.width()
                && card->frameGeometry().height() > restoredSize.height());
        maximize->click();
        QTRY_COMPARE(card->frameGeometry().size(), restoredSize);
        QCOMPARE(card->windowModality(), Qt::WindowModal);
    }

    void taskCardSplitterPersistsAfterMaximizeAndReopen()
    {
        QSettings settings(QStringLiteral("bitrix24-client"), QStringLiteral("bitrix24-client"));
        const auto key = QStringLiteral("ui/taskCardDetailsWidth");
        settings.remove(key);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        MainWindow window(backend.get());
        window.resize(960, 690);
        window.show();
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        sections->setCurrentRow(1);
        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        card->resize(900, 540);
        auto *splitter = card->findChild<QSplitter *>(QStringLiteral("taskCardSplitter"));
        auto *maximize = findAccessible<QPushButton>(card, QStringLiteral("Развернуть карточку задачи"));
        QVERIFY(splitter && maximize);
        const int old = splitter->sizes().first();
        auto *handle = splitter->handle(1);
        const auto start = handle->rect().center();
        QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(handle, start + QPoint(48, 0));
        QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier, start + QPoint(48, 0));
        QTRY_VERIFY(splitter->sizes().first() > old + 25);
        const int saved = splitter->sizes().first();
        QCOMPARE(settings.value(key).toInt(), saved);
        maximize->click();
        QTRY_VERIFY(card->isMaximized());
        QVERIFY(splitter->sizes().at(1) >= 285);
        maximize->click();
        QTRY_VERIFY(!card->isMaximized());
        QTRY_COMPARE(splitter->sizes().first(), saved);
        card->resize(660, 420);
        QTRY_VERIFY(splitter->sizes().first() >= 280 && splitter->sizes().at(1) >= 285);
        card->close();
        QTRY_VERIFY(!window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        window.openTaskCard(QStringLiteral("501"));
        card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        card->resize(900, 540);
        splitter = card->findChild<QSplitter *>(QStringLiteral("taskCardSplitter"));
        QVERIFY(splitter);
        QTRY_COMPARE(splitter->sizes().first(), saved);
        card->resize(660, 420);
        auto *description = findAccessible<QListWidget>(card, QStringLiteral("Описание и встроенные файлы"));
        QVERIFY(description);
        QTRY_VERIFY(description->viewport()->visibleRegion().boundingRect().height() >=
                    description->visualItemRect(description->item(0)).height());
        const auto compact = qEnvironmentVariable("B24_TASK_CARD_COMPACT_SCREENSHOT");
        if (!compact.isEmpty())
            QVERIFY(card->grab().save(compact));
        settings.remove(key);
    }

    void queuesNewTaskFilesUntilTheMatchingCreationAcknowledgement_data()
    {
        QTest::addColumn<bool>("subtask");
        QTest::addColumn<bool>("cancel");
        QTest::addColumn<bool>("changeAccount");
        QTest::newRow("task") << false << false << false;
        QTest::newRow("subtask") << true << false << false;
        QTest::newRow("cancel before acknowledgement") << false << true << false;
        QTest::newRow("account changes during upload") << false << false << true;
    }

    void queuesNewTaskFilesUntilTheMatchingCreationAcknowledgement()
    {
        QFETCH(bool, subtask);
        QFETCH(bool, cancel);
        QFETCH(bool, changeAccount);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->accountScope = QStringLiteral("fixture/task-attachment-first");
        backend->holdTaskCreation = true;
        backend->holdTaskAttachment = true;
        MainWindow window(backend.get());
        window.resize(960, 740);
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        if (subtask) {
            window.openTaskCard(QStringLiteral("501"));
            auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
            QVERIFY(card);
            QPushButton *create = nullptr;
            for (auto *button : card->findChildren<QPushButton *>()) {
                if (button->text() == QStringLiteral("Новая подзадача"))
                    create = button;
            }
            QVERIFY(create);
            QTRY_VERIFY(create->isEnabled());
            create->click();
        } else {
            auto *create = findAccessible<QPushButton>(&window, QStringLiteral("Создать задачу"));
            QVERIFY(create);
            create->click();
        }
        auto *form = findAccessible<QDialog>(&window, subtask ? QStringLiteral("Форма новой подзадачи")
                                                            : QStringLiteral("Форма новой задачи"));
        QVERIFY(form);
        auto *title = findAccessible<QLineEdit>(form, QStringLiteral("Заголовок задачи"));
        auto *editor = findAccessible<QTextEdit>(form, QStringLiteral("Описание задачи"));
        auto *attach = findAccessible<QToolButton>(form, QStringLiteral("Прикрепить файл"));
        auto *queue = findAccessible<QListWidget>(form, QStringLiteral("Файлы к новой задаче"));
        auto *remove = findAccessible<QPushButton>(form, QStringLiteral("Убрать выбранный файл"));
        auto *save = findAccessible<QPushButton>(form, QStringLiteral("Сохранить задачу"));
        QVERIFY(title && editor && attach && queue && remove && save);
        QVERIFY(attach->isEnabled());
        title->setText(QStringLiteral("Новая задача с файлами"));
        const auto description = QStringLiteral("Описание сохраняется без подстановок");
        editor->insertPlainText(description);
        QTemporaryDir files;
        QVERIFY(files.isValid());
        const auto first = files.filePath(QStringLiteral("первый.txt"));
        const auto second = files.filePath(QStringLiteral("второй.txt"));
        const auto unused = files.filePath(QStringLiteral("лишний.txt"));
        for (const auto &path : {first, second, unused}) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("file"), 4);
        }
        attach->click();
        auto *chooser = form->findChild<QFileDialog *>();
        QVERIFY(chooser);
        chooser->selectFile(first);
        QVERIFY(QMetaObject::invokeMethod(chooser, "accept"));
        QTRY_COMPARE(queue->count(), 1);
        auto *clipboard = new QMimeData;
        clipboard->setUrls({QUrl::fromLocalFile(first), QUrl::fromLocalFile(unused), QUrl::fromLocalFile(second),
                            QUrl::fromLocalFile(first)});
        QGuiApplication::clipboard()->setMimeData(clipboard);
        editor->paste();
        QCOMPARE(queue->count(), 3);
        QCOMPARE(queue->item(0)->text(), QFileInfo(first).fileName());
        queue->setCurrentRow(1);
        remove->click();
        QCOMPARE(queue->count(), 2);
        QCOMPARE(queue->item(1)->text(), QFileInfo(second).fileName());
        QVERIFY(backend->attachmentRequests.isEmpty());
        save->click();
        QCOMPARE(backend->createTaskCalls, 1);
        QCOMPARE(backend->lastCreate.description, std::optional<QString>(description));
        QVERIFY(backend->attachmentRequests.isEmpty());
        emit backend->taskCreated(QStringLiteral("999"), QStringLiteral("503"));
        QVERIFY(backend->attachmentRequests.isEmpty());

        if (cancel)
            form->reject();
        if (subtask)
            emit backend->subtaskCreated(backend->createdParent, backend->createdProject, QStringLiteral("502"));
        else
            emit backend->taskCreated(backend->createdProject, QStringLiteral("502"));
        if (cancel) {
            QVERIFY(backend->attachmentRequests.isEmpty());
            return;
        }
        QCOMPARE(backend->attachmentRequests.size(), 1);
        QCOMPARE(backend->attachmentRequests.last(), qMakePair(QStringLiteral("502"), first));
        if (changeAccount) {
            backend->accountScope = QStringLiteral("fixture/task-attachment-second");
            emit backend->accountScopeChanged();
            QVERIFY(!form->isVisible());
            emit backend->fileUploaded(QStringLiteral("502"), QStringLiteral("task"));
            QCOMPARE(backend->attachmentRequests.size(), 1);
            return;
        }
        emit backend->fileUploaded(QStringLiteral("999"), QStringLiteral("task"));
        QCOMPARE(backend->attachmentRequests.size(), 1);
        emit backend->fileUploaded(QStringLiteral("502"), QStringLiteral("task"));
        QCOMPARE(backend->attachmentRequests.size(), 2);
        QCOMPARE(backend->attachmentRequests.last(), qMakePair(QStringLiteral("502"), second));
        emit backend->failed(QStringLiteral("attach-task:502"), QStringLiteral("Не удалось загрузить файл"));
        QCOMPARE(queue->count(), 1);
        QVERIFY(form->isVisible());
        QCOMPARE(save->text(), QStringLiteral("Повторить загрузку"));
        QVERIFY(save->isEnabled());
        save->click();
        QCOMPARE(backend->createTaskCalls, 1);
        QCOMPARE(backend->attachmentRequests.size(), 3);
        QCOMPARE(backend->attachmentRequests.last(), qMakePair(QStringLiteral("502"), second));
        emit backend->fileUploaded(QStringLiteral("502"), QStringLiteral("task"));
        QVERIFY(!form->isVisible());
    }

    void createsTaskWithNamedDefaults_data()
    {
        QTest::addColumn<QString>("profileState");
        QTest::addColumn<bool>("subtask");
        QTest::newRow("confirmed task") << QStringLiteral("confirmed") << false;
        QTest::newRow("confirmed subtask") << QStringLiteral("confirmed") << true;
        QTest::newRow("profile unavailable") << QStringLiteral("unavailable") << false;
        QTest::newRow("profile still loading") << QStringLiteral("loading") << false;
    }

    void createsTaskWithNamedDefaults()
    {
        QFETCH(QString, profileState);
        QFETCH(bool, subtask);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdCurrentPerson = true;
        backend->holdTaskCreation = true;
        backend->accountScope = QStringLiteral("fixture/task-defaults-first");
        backend->stages.first().tasks.first().projectId = QStringLiteral("101");
        backend->stages.prepend({QStringLiteral("0"), QStringLiteral("Без стадии"), {}});
        MainWindow window(backend.get());
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        if (subtask) {
            window.openTaskCard(QStringLiteral("501"));
            auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
            QVERIFY(card);
            QPushButton *create = nullptr;
            for (auto *button : card->findChildren<QPushButton *>()) {
                if (button->text() == QStringLiteral("Новая подзадача"))
                    create = button;
            }
            QVERIFY(create);
            QTRY_VERIFY(create->isEnabled());
            create->click();
        } else {
            auto *create = findAccessible<QPushButton>(&window, QStringLiteral("Создать задачу"));
            QVERIFY(create && create->isEnabled());
            create->click();
        }
        auto *form = findAccessible<QDialog>(&window, subtask ? QStringLiteral("Форма новой подзадачи")
                                                            : QStringLiteral("Форма новой задачи"));
        QVERIFY(form);
        auto *stage = findAccessible<QComboBox>(form, QStringLiteral("Стадия"));
        auto *creator = findAccessible<QPushButton>(form, QStringLiteral("Постановщик"));
        auto *assignee = findAccessible<QPushButton>(form, QStringLiteral("Ответственный"));
        auto *title = findAccessible<QLineEdit>(form, QStringLiteral("Заголовок задачи"));
        auto *save = findAccessible<QPushButton>(form, QStringLiteral("Сохранить задачу"));
        QVERIFY(stage && creator && assignee && title && save);
        QCOMPARE(stage->currentText(), QStringLiteral("В работе"));
        QCOMPARE(stage->currentData().toString(), QStringLiteral("601"));
        QCOMPARE(stage->findData(QStringLiteral("0")), -1);
        QCOMPARE(assignee->text(), QStringLiteral("Загружаем профиль…"));
        QCOMPARE(creator->text(), assignee->text());
        QCOMPARE(backend->currentPersonRequests.size(), 1);
        const bool confirmed = profileState == QLatin1String("confirmed");
        if (profileState != QLatin1String("loading")) {
            backend->currentPersonRequests.takeFirst()(confirmed ? backend->currentPerson : Person());
            const auto expectedName = confirmed ? backend->currentPerson.name : QStringLiteral("Профиль недоступен · Выбрать");
            QCOMPARE(assignee->text(), expectedName);
            QCOMPARE(creator->text(), expectedName);
        }
        QVERIFY(assignee->isEnabled() && creator->isEnabled());
        title->setText(QStringLiteral("Задача со стартовыми реквизитами"));
        QVERIFY(save->isEnabled());
        save->click();
        QCOMPARE(backend->createTaskCalls, 1);
        QCOMPARE(backend->createdParent, subtask ? QStringLiteral("501") : QString());
        QCOMPARE(backend->lastCreate.stageId, std::optional<QString>(QStringLiteral("601")));
        const auto expectedUser = confirmed ? std::optional<QString>(backend->currentPerson.id) : std::nullopt;
        QCOMPARE(backend->lastCreate.creatorId, expectedUser);
        QCOMPARE(backend->lastCreate.assigneeId, expectedUser);

        if (profileState == QLatin1String("loading")) {
            auto staleResponse = backend->currentPersonRequests.takeFirst();
            const QPointer<QDialog> oldForm(form);
            backend->accountScope = QStringLiteral("fixture/task-defaults-second");
            emit backend->accountScopeChanged();
            QTRY_VERIFY(oldForm.isNull());
            auto *create = findAccessible<QPushButton>(&window, QStringLiteral("Создать задачу"));
            QVERIFY(create && create->isEnabled());
            create->click();
            auto *next = findAccessible<QDialog>(&window, QStringLiteral("Форма новой задачи"));
            QVERIFY(next);
            auto *nextCreator = findAccessible<QPushButton>(next, QStringLiteral("Постановщик"));
            QVERIFY(nextCreator);
            staleResponse(backend->currentPerson);
            QCOMPARE(nextCreator->text(), QStringLiteral("Загружаем профиль…"));
            QCOMPARE(backend->currentPersonRequests.size(), 1);
            backend->currentPersonRequests.takeFirst()({QStringLiteral("20"), QStringLiteral("Илья Власов"), {}, {}});
            QCOMPARE(nextCreator->text(), QStringLiteral("Илья Власов"));
        }
    }

    void createsTaskWithSelectedRequisites()
    {
        const auto applicationFont = QApplication::font();
        const auto restoreFont = qScopeGuard([applicationFont] { QApplication::setFont(applicationFont); });
        ui::installApplicationTypography(*qApp);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdProjectStages = true;
        backend->holdTaskCreation = true;
        backend->holdCurrentPerson = true;
        backend->projects.append({QStringLiteral("102"), QStringLiteral("Проект два"), {}});
        backend->stages.append({QStringLiteral("0"), QStringLiteral("Без стадии"), {}});
        backend->people = {{QStringLiteral("12"), QStringLiteral("Анна"), {}, {}},
                           {QStringLiteral("13"), QStringLiteral("Вера"), {}, {}},
                           {QStringLiteral("14"), QStringLiteral("Борис"), {}, {}},
                           {QStringLiteral("15"), QStringLiteral("Ольга"), {}, {}}};
        MainWindow window(backend.get());
        window.resize(860, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        auto *create = findAccessible<QPushButton>(&window, QStringLiteral("Создать задачу"));
        QVERIFY(create && create->isEnabled());
        create->click();
        auto *form = findAccessible<QDialog>(&window, QStringLiteral("Форма новой задачи"));
        QVERIFY(form);
        auto *project = findAccessible<QPushButton>(form, QStringLiteral("Проект"));
        auto *stage = findAccessible<QComboBox>(form, QStringLiteral("Стадия"));
        auto *creator = findAccessible<QPushButton>(form, QStringLiteral("Постановщик"));
        auto *assignee = findAccessible<QPushButton>(form, QStringLiteral("Ответственный"));
        auto *observers = findAccessible<QPushButton>(form, QStringLiteral("Наблюдатели"));
        QVERIFY(project && stage && creator && assignee && observers);
        QVERIFY(!observers->visibleRegion().isEmpty());
        QVERIFY(project->text().contains(QStringLiteral("Проект один")));
        QTRY_COMPARE(stage->currentData().toString(), QStringLiteral("601"));
        QCOMPARE(stage->findData(QStringLiteral("0")), -1);
        stage->setCurrentIndex(stage->findData(QStringLiteral("602")));
        emit backend->projectStagesLoaded(QStringLiteral("101"), backend->stages);
        QCOMPARE(stage->currentData().toString(), QStringLiteral("602"));
        project->setFocus();
        QTest::keyClick(project, Qt::Key_Space);
        auto *projects = findAccessible<QListWidget>(form, QStringLiteral("Проект задачи"));
        QVERIFY(projects && projects->isVisible());
        const auto destination = projects->findItems(QStringLiteral("Проект два"), Qt::MatchExactly);
        QCOMPARE(destination.size(), 1);
        projects->setCurrentItem(destination.first());
        QTest::keyClick(projects, Qt::Key_Return);
        QCOMPARE(backend->projectStageRequests.last(), QStringLiteral("102"));
        QVERIFY(project->text().contains(QStringLiteral("Проект два")));
        QVERIFY(!stage->isEnabled());
        QCOMPARE(stage->findData(QStringLiteral("601")), -1);
        emit backend->projectStagesLoaded(QStringLiteral("101"), backend->stages);
        QVERIFY(!stage->isEnabled());
        emit backend->failed(QStringLiteral("project-stages:102"), QStringLiteral("Временно недоступно"));
        auto *retryStage = findAccessible<QPushButton>(form, QStringLiteral("Повторить загрузку стадий"));
        QVERIFY(retryStage && retryStage->isVisible());
        retryStage->click();
        QCOMPARE(backend->projectStageRequests.count(QStringLiteral("102")), 2);
        emit backend->projectStagesLoaded(QStringLiteral("102"), {});
        QCOMPARE(stage->currentText(), QStringLiteral("Нет стадий"));
        QVERIFY(!stage->isEnabled() && !stage->currentData().isValid());
        emit backend->projectStagesLoaded(QStringLiteral("102"), {{QStringLiteral("611"), QStringLiteral("Проверка"), {}}});
        QVERIFY(stage->isEnabled());
        QCOMPARE(stage->currentText(), QStringLiteral("Проверка"));
        QCOMPARE(stage->currentData().toString(), QStringLiteral("611"));

        const auto pickPeople = [&](QPushButton *button, const QString &inputName, const QStringList &names) {
            button->click();
            auto *input = findAccessible<QLineEdit>(form, QStringLiteral("Поиск сотрудника · ") + inputName);
            QTRY_VERIFY(input && input->isVisible());
            for (const auto &name : names) {
                QListWidget *results = nullptr;
                QTRY_VERIFY([&] {
                    for (auto *list : form->findChildren<QListWidget *>()) {
                        if (list->accessibleName() == QStringLiteral("Найденные сотрудники")
                            && list->isVisible() && !list->findItems(name, Qt::MatchExactly).isEmpty()) {
                            results = list;
                            return true;
                        }
                    }
                    return false;
                }());
                auto *item = results->findItems(name, Qt::MatchExactly).first();
                results->scrollToItem(item);
                QTest::mouseClick(results->viewport(), Qt::LeftButton, Qt::NoModifier, results->visualItemRect(item).center());
            }
            if (button == creator) {
                QCOMPARE(backend->currentPersonRequests.size(), 1);
                backend->currentPersonRequests.takeFirst()(backend->currentPerson);
            }
            auto *apply = findAccessible<QPushButton>(form, QStringLiteral("Применить изменение реквизита"));
            if (apply && apply->isVisible())
                apply->click();
        };
        pickPeople(assignee, QStringLiteral("Ответственный"), {QStringLiteral("Анна")});
        pickPeople(creator, QStringLiteral("Постановщик"), {QStringLiteral("Вера")});
        pickPeople(observers, QStringLiteral("Наблюдатели"), {QStringLiteral("Борис"), QStringLiteral("Ольга")});
        auto *title = findAccessible<QLineEdit>(form, QStringLiteral("Заголовок задачи"));
        QVERIFY(title);
        title->setText(QStringLiteral("Проверить реквизиты"));
        auto *save = findAccessible<QPushButton>(form, QStringLiteral("Сохранить задачу"));
        QVERIFY(save && save->isEnabled());
        save->click();
        QCOMPARE(backend->createdProject, QStringLiteral("102"));
        QCOMPARE(backend->lastCreate.stageId, std::optional<QString>(QStringLiteral("611")));
        QCOMPARE(backend->lastCreate.assigneeId, std::optional<QString>(QStringLiteral("12")));
        QCOMPARE(backend->lastCreate.creatorId, std::optional<QString>(QStringLiteral("13")));
        QCOMPARE(backend->lastCreate.observerIds, std::optional<QStringList>({QStringLiteral("14"), QStringLiteral("15")}));
        QCOMPARE(backend->createTaskCalls, 1);
        emit backend->taskCreated(QStringLiteral("101"), QStringLiteral("999"));
        QVERIFY(form->isVisible());
        emit backend->taskCreated(QStringLiteral("102"), QStringLiteral("502"));
        QVERIFY(!form->isVisible());
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card && card->isVisible());
        QCOMPARE(static_cast<ui::TaskCardDialog *>(card)->taskId(), QStringLiteral("502"));
        QCOMPARE(static_cast<ui::TaskCardDialog *>(card)->projectId(), QStringLiteral("102"));
    }

    void markupAndSearchInReadOnlyTaskCard()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->chats.first().lastMessage = QStringLiteral("[B]Предпросмотр[/B]");
        auto &message = backend->messages.first();
        message.text = QStringLiteral("[USER=1 REPLACE]Анна[/USER] [B]проверила[/B]");
        message.forwarded = true;
        message.forwardedAuthor = QStringLiteral("[B]Иван[/B]");
        message.replyId = QStringLiteral("m-source");
        message.replyAuthor = QStringLiteral("Алексей");
        message.replyText = QStringLiteral("[B]Проверить кран[/B]");
        message.attachments = {QStringLiteral("C:\\temp\\<file>.txt")};
        backend->taskMessages = {message};
        backend->stages.first().tasks.first().description = QStringLiteral("[B]Описание рабочей задачи[/B]");
        backend->notifications = {{QStringLiteral("n-1"), {}, QStringLiteral("[B]Новая задача[/B]"),
            QStringLiteral("[I]Описание[/I]"), {}, false}};
        MainWindow window(backend.get(), true);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *chatList = findAccessible<QListWidget>(&window, QStringLiteral("Недавние чаты"));
        auto *messages = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(chatList);
        QVERIFY(messages);
        QTRY_COMPARE(chatList->item(0)->data(workspace::Detail).toString(), QStringLiteral("Предпросмотр"));
        QCOMPARE(messages->count(), 1);
        QCOMPARE(messages->item(0)->data(workspace::Detail).toString(),
                 QStringLiteral("Анна проверила\nВложения: <file>.txt"));
        QVERIFY(messages->item(0)->data(workspace::MarkupHtml).toString().contains(QStringLiteral("&lt;file&gt;")));
        QCOMPARE(messages->item(0)->data(workspace::ForwardByline).toString(), QStringLiteral("Переслано · Иван"));
        QCOMPARE(messages->item(0)->data(workspace::ReplyPreview).toString(), QStringLiteral("Проверить кран"));
        QVERIFY(messages->item(0)->data(Qt::AccessibleTextRole).toString().contains(QStringLiteral("Ответ · Алексей")));
        QVERIFY(messages->item(0)->toolTip().isEmpty());
        QTest::keyClick(&window, Qt::Key_3, Qt::AltModifier);
        auto *notices = findAccessible<QListWidget>(&window, QStringLiteral("Уведомления"));
        QVERIFY(notices);
        QCOMPARE(notices->item(0)->data(workspace::Title).toString(), QStringLiteral("Новая задача"));
        QVERIFY(!notices->item(0)->data(workspace::MarkupHtml).toString().isEmpty());
        QVERIFY(notices->item(0)->toolTip().isEmpty());

        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        window.openTaskCard(QStringLiteral("501"));
        auto *card = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(card);
        auto *description = findAccessible<QListWidget>(card, QStringLiteral("Описание и встроенные файлы"));
        QVERIFY(description && description->count() == 1);
        QCOMPARE(description->item(0)->text(), QStringLiteral("Описание рабочей задачи"));
        QVERIFY(description->item(0)->toolTip().isEmpty());
        QVERIFY(!findAccessible<QPushButton>(card, QStringLiteral("Сохранить задачу"))->isEnabled());
        QVERIFY(!findAccessible<QPushButton>(card, QStringLiteral("Повторить перенос задачи"))->isEnabled());
        QVERIFY(!findAccessible<QPushButton>(card, QStringLiteral("Отправить файл в чат задачи"))->isEnabled());
        auto *search = findAccessible<QLineEdit>(card, QStringLiteral("Поиск по чату карточки"));
        auto *taskMessages = findAccessible<QListWidget>(card, QStringLiteral("Чат карточки"));
        auto *showSearch = findAccessible<QPushButton>(card, QStringLiteral("Найти в чате задачи"));
        QVERIFY(search);
        QVERIFY(taskMessages);
        QVERIFY(showSearch);
        QVERIFY(!search->isVisible());
        QTest::mouseClick(showSearch, Qt::LeftButton);
        QTRY_VERIFY(search->isVisible() && search->hasFocus());
        QCOMPARE(taskMessages->item(0)->data(workspace::Detail).toString(), messages->item(0)->data(workspace::Detail).toString());
        QVERIFY(taskMessages->item(0)->toolTip().isEmpty());
        QCOMPARE(taskMessages->item(0)->data(workspace::ReplyPreview).toString(), QStringLiteral("Проверить кран"));
        search->setText(QStringLiteral("проверила"));
        QTest::keyClick(search, Qt::Key_Return);
        QVERIFY(backend->searchedTask);
        QCOMPARE(backend->searchedScope, QStringLiteral("501"));
        emit backend->messagesFound(QStringLiteral("501"), QStringLiteral("проверила"), true, backend->taskMessages);
        emit backend->taskMessagesLoaded(QStringLiteral("501"), {});
        QCOMPARE(taskMessages->count(), 1);
        QVERIFY(!taskMessages->item(0)->data(workspace::MarkupHtml).toString().isEmpty());
        QTest::mouseClick(showSearch, Qt::LeftButton);
        QVERIFY(!search->isVisible());
        QCOMPARE(taskMessages->item(0)->data(workspace::Detail).toString(), messages->item(0)->data(workspace::Detail).toString());
    }

    void taskCardCanCloseImmediatelyAfterSendAcknowledgement()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdMessageSends = true;
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        const auto taskId = QStringLiteral("501");
        window.openTaskCard(taskId);
        auto *dialog = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        const QPointer card(dynamic_cast<ui::TaskCardDialog *>(dialog));
        QVERIFY(card);
        QTRY_VERIFY(card->isVisible());
        QVERIFY(!card->hasUnsavedChanges());

        int confirmations = 0;
        card->setDiscardConfirmation([&confirmations] { ++confirmations; return false; });
        auto *input = card->messageInput();
        const auto sent = QStringLiteral("Ответ по задаче");
        input->setPlainText(sent);
        QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(backend->taskSendRequests, 1);
        QCOMPARE(backend->sentTask, sent);
        QVERIFY(card->hasUnsavedChanges());
        QVERIFY(!card->close());
        QCOMPARE(confirmations, 1);

        const auto historyRequests = backend->taskChatReadRequests.size();
        emit backend->messageSent(QStringLiteral("task:") + taskId, QStringLiteral("900"));
        QCOMPARE(backend->taskChatReadRequests.size(), historyRequests);
        QVERIFY(input->toPlainText().isEmpty());
        QVERIFY(!card->hasUnsavedChanges());

        input->setPlainText(QStringLiteral("Новый черновик"));
        QVERIFY(card->hasUnsavedChanges());
        QVERIFY(!card->close());
        QCOMPARE(confirmations, 2);

        input->clear();
        QVERIFY(card->close());
        QCOMPARE(confirmations, 2);
        QVERIFY(!card || !card->isVisible());
    }

    void taskCardWarnsBeforeClosingWithUnsavedChanges()
    {
        DemoBackend demo;
        demo.start();
        MainWindow window(&demo);
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        QTRY_COMPARE(sections->currentRow(), 1);
        auto *board = window.findChild<BoardWidget *>();
        QVERIFY(board);
        QTRY_VERIFY(board->findChildren<QListWidget *>().size() > 1);
        QListWidget *work = nullptr;
        QTRY_VERIFY((work = findAccessible<QListWidget>(board, QStringLiteral("В работе"))));
        QVERIFY(work->count() > 0);
        work->setFocus();
        QTest::keyClick(work, Qt::Key_Down);
        QTest::keyClick(work, Qt::Key_Return);
        auto *card = static_cast<ui::TaskCardDialog *>(window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        QVERIFY(card);
        QTRY_VERIFY(card->isVisible());
        auto *input = card->messageInput();
        QVERIFY(input);
        input->setPlainText(QStringLiteral("черновик сообщения"));
        QVERIFY(card->hasUnsavedChanges());

        int asked = 0;
        card->setDiscardConfirmation([&asked] { ++asked; return false; });
        QTest::keyClick(card, Qt::Key_Escape);
        QCOMPARE(asked, 1);
        QVERIFY(card->isVisible());
        card->close();
        QCOMPARE(asked, 2);
        QVERIFY(card->isVisible());
        QVERIFY(!window.confirmWorkspaceClose());
        QVERIFY(!window.close());
        QCOMPARE(asked, 4);
        QVERIFY(window.isVisible());
        QCOMPARE(input->toPlainText(), QStringLiteral("черновик сообщения"));

        bool nestedCloseRejected = false;
        card->setDiscardConfirmation([&] {
            ++asked;
            nestedCloseRejected = !window.closeWorkspace();
            return true;
        });
        QVERIFY(window.confirmWorkspaceClose());
        QVERIFY(card->isVisible());
        QVERIFY(window.closeWorkspace());
        QVERIFY(nestedCloseRejected);
        QCOMPARE(asked, 6);
        QVERIFY(!card->isVisible());
        demo.stop();
    }

    void taskCardReplacementKeepsCardWhenDiscardIsDeclined()
    {
        DemoBackend demo;
        demo.start();
        MainWindow window(&demo);
        window.resize(960, 650);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        QTRY_COMPARE(sections->currentRow(), 1);
        auto *board = window.findChild<BoardWidget *>();
        QVERIFY(board);
        QTRY_VERIFY(board->findChildren<QListWidget *>().size() > 1);
        QListWidget *work = nullptr;
        QTRY_VERIFY((work = findAccessible<QListWidget>(board, QStringLiteral("В работе"))));
        QVERIFY(work->count() > 0);
        work->setFocus();
        QTest::keyClick(work, Qt::Key_Down);
        QTest::keyClick(work, Qt::Key_Return);
        auto *card = static_cast<ui::TaskCardDialog *>(window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        QVERIFY(card);
        QTRY_VERIFY(card->isVisible());
        const auto taskId = card->taskId();
        card->messageInput()->setPlainText(QStringLiteral("черновик"));
        int asked = 0;
        card->setDiscardConfirmation([&asked] { ++asked; return false; });

        window.openTaskCard(taskId == QStringLiteral("t-1") ? QStringLiteral("t-2") : QStringLiteral("t-1"));
        QCOMPARE(asked, 1);
        auto *current = static_cast<ui::TaskCardDialog *>(window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        QVERIFY(current);
        QCOMPARE(current->taskId(), taskId);
        QVERIFY(current->isVisible());
        QCOMPARE(current->messageInput()->toPlainText(), QStringLiteral("черновик"));
        demo.stop();
    }
};

QTEST_MAIN(UiTaskCardTest)
#include "tst_ui_task_card.moc"
