#include "ui/task_dialogs.h"

namespace bx::ui {

TaskCardDialog::TaskCardDialog(const Task &task, const QString &projectId, QWidget *parent, bool detailsLoaded)
    : QDialog(parent, Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
                      Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint),
      m_task(task), m_projectId(projectId)
{
    setProperty("workspaceDialog", true);
    setWindowTitle(QStringLiteral("Задача №%1").arg(task.id));
    setObjectName(QStringLiteral("taskCardDialog"));
    setAccessibleName(QStringLiteral("Карточка задачи"));
    // Keep modal input handling while letting the window manager maximize the native window.
    setWindowModality(Qt::WindowModal);
    resize(900, 630);
    setMinimumSize(660, 420);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 13, 16, 12);
    layout->setSpacing(9);
    auto *header = new QHBoxLayout;
    auto *number = workspace::label(QStringLiteral("№%1").arg(task.id), "quiet", this);
    number->setAccessibleName(QStringLiteral("Номер задачи"));
    header->addWidget(number);
    auto *copyNumber = new GlyphButton(Glyph::Copy, QStringLiteral("Скопировать номер"), this);
    copyNumber->setFixedSize(26, 26);
    connect(copyNumber, &QPushButton::clicked, this, [this] {
        copy(m_task.id, QStringLiteral("Номер скопирован"));
    });
    header->addWidget(copyNumber);
    m_copyLink = new GlyphButton(Glyph::Link, QStringLiteral("Скопировать ссылку"), this);
    m_copyLink->setFixedSize(26, 26);
    setLink({});
    connect(m_copyLink, &QPushButton::clicked, this, [this] {
        copy(m_link, QStringLiteral("Ссылка скопирована"));
    });
    header->addWidget(m_copyLink);
    m_copyFeedback = workspace::label({}, "taskCopyFeedback", this);
    m_copyFeedback->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    header->addWidget(m_copyFeedback, 1);
    m_copyFeedbackTimer.setSingleShot(true);
    m_copyFeedbackTimer.setInterval(1800);
    connect(&m_copyFeedbackTimer, &QTimer::timeout, m_copyFeedback, &QLabel::clear);
    m_maximize = button(QStringLiteral("Развернуть"), QStringLiteral("Развернуть карточку задачи"), this);
    connect(m_maximize, &QPushButton::clicked, this, [this] {
        if (isMaximized())
            showNormal();
        else
            showMaximized();
    });
    header->addWidget(m_maximize);
    layout->addLayout(header);
    m_taskTitle = workspace::label(QString(), "taskCardHeading", this);
    m_taskTitle->setAccessibleName(QStringLiteral("Название открытой задачи"));
    m_taskTitle->setWordWrap(true);
    layout->addWidget(m_taskTitle);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setObjectName(QStringLiteral("taskCardSplitter"));
    m_splitter->setAccessibleName(QStringLiteral("Ширина реквизитов задачи"));
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(5);
    m_splitter->setOpaqueResize(false);
    m_detailsScroll = new QScrollArea(m_splitter);
    m_detailsScroll->setObjectName(QStringLiteral("taskDetailsScroll"));
    m_detailsScroll->setAccessibleName(QStringLiteral("Реквизиты и файлы задачи"));
    m_detailsScroll->setWidgetResizable(true);
    m_detailsScroll->setFrameShape(QFrame::NoFrame);
    m_detailsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_detailsScroll->setMinimumWidth(280);
    auto *fields = new QWidget(m_detailsScroll);
    fields->setObjectName(QStringLiteral("taskDetailsPane"));
    m_detailsScroll->setWidget(fields);
    auto *form = new QVBoxLayout(fields);
    form->setContentsMargins(0, 0, 6, 0);
    form->setSpacing(9);
    auto section = [fields, form](const QString &title) {
        auto *container = new QWidget(fields);
        container->setObjectName(QStringLiteral("taskSection"));
        auto *content = new QVBoxLayout(container);
        content->setContentsMargins(10, 8, 10, 9);
        content->setSpacing(5);
        auto *heading = new QHBoxLayout;
        if (!title.isEmpty())
            heading->addWidget(workspace::label(title, "taskSectionHeading", container), 1);
        content->addLayout(heading);
        form->addWidget(container);
        return std::make_pair(content, heading);
    };
    auto [description, descriptionHeader] = section(QStringLiteral("Описание"));
    m_edit = button(QStringLiteral("Изменить"), QStringLiteral("Изменить задачу"), fields);
    connect(m_edit, &QPushButton::clicked, this, [this] { setEditingDescription(true); });
    descriptionHeader->addWidget(m_edit);
    m_descriptionRows = new TaskContentList(QStringLiteral("Описание и встроенные файлы"), fields);
    description->addWidget(m_descriptionRows);
    m_fields = new TaskFields(fields, false);
    description->addWidget(m_fields);
    m_editActions = new QWidget(fields);
    auto *editActions = new QHBoxLayout(m_editActions);
    editActions->setContentsMargins(0, 3, 0, 0);
    editActions->addStretch(1);
    m_cancelEdit = button(QStringLiteral("Отменить"), QStringLiteral("Отменить изменения задачи"), m_editActions);
    connect(m_cancelEdit, &QPushButton::clicked, this, [this] {
        m_fields->setTask(m_task, true);
        setEditingDescription(false);
        setStatus({});
    });
    editActions->addWidget(m_cancelEdit);
    m_save = button(QStringLiteral("Сохранить"), QStringLiteral("Сохранить задачу"), m_editActions);
    m_save->setObjectName(QStringLiteral("primary"));
    connect(m_save, &QPushButton::clicked, this, [this] {
        const auto error = m_fields->validation();
        const auto edit = m_fields->edit();
        const auto &rights = m_task.editRights;
        if (!error.isEmpty()) {
            setStatus(error);
        } else if ((edit.title && !rights.title) || (edit.description && !rights.description)
                   || (edit.highPriority && !rights.priority)) {
            setStatus(QStringLiteral("Портал больше не разрешает сохранить часть изменений"));
        } else if (m_onSave) {
            m_onSave(edit);
        }
    });
    editActions->addWidget(m_save);
    description->addWidget(m_editActions);
    m_fields->hide();
    m_editActions->hide();

    auto [attachments, attachmentsHeader] = section(QStringLiteral("Файлы"));
    m_attach = button(QStringLiteral("Добавить файл"), QStringLiteral("Прикрепить файл к задаче"), fields);
    connect(m_attach, &QPushButton::clicked, this, [this] {
        if (m_onFile)
            m_onFile(true);
    });
    attachmentsHeader->addWidget(m_attach);
    m_attachmentRows = new TaskContentList(QStringLiteral("Вложения задачи"), fields);
    attachments->addWidget(m_attachmentRows);

    auto [details, detailsHeader] = section(QStringLiteral("Реквизиты"));
    auto [project, projectHeader] = section({});
    auto [observers, observersHeader] = section({});
    auto requisite = [](QVBoxLayout *group, const QString &caption,
                        std::optional<workspace::AvatarKind> kind = {}, bool multiple = false) {
        auto *row = new TaskRequisiteRow(caption, group->parentWidget(), kind, multiple);
        group->addWidget(row);
        return row;
    };
    using Field = TaskDetailEditor::Field;
    m_requisitePopup = new QFrame(this);
    m_requisitePopup->setObjectName(QStringLiteral("taskRequisitePopup"));
    m_requisitePopup->installEventFilter(this);
    m_requisitePopupLayout = new QVBoxLayout(m_requisitePopup);
    m_requisitePopupLayout->setContentsMargins(11, 8, 11, 8);
    m_requisitePopup->hide();
    auto *popupEscape = new QShortcut(QKeySequence(Qt::Key_Escape), m_requisitePopup);
    popupEscape->setContext(Qt::WidgetWithChildrenShortcut);
    connect(popupEscape, &QShortcut::activated, this, [this] {
        if (m_stageOpen && m_cancelStage->isEnabled() && !hasStageDraft())
            closeStageEditor();
        else if (!m_stageOpen && !m_detailEditor->changed())
            m_detailEditor->cancel();
    });
    connect(m_detailsScroll->verticalScrollBar(), &QScrollBar::valueChanged,
            this, [this] { positionRequisitePopup(); });
    m_detailEditor = new TaskDetailEditor(fields);
    m_detailEditor->onSave = [this](const TaskEdit &edit) {
        if (!m_onSave)
            return;
        auto request = edit;
        request.requireDelegate = m_delegatingFromFooter && edit.assigneeId.has_value();
        m_onSave(request);
    };
    m_detailEditor->onAvatarsVisible = [this] {
        if (m_onRequisiteAvatarChoicesChanged)
            m_onRequisiteAvatarChoicesChanged();
    };
    m_detailEditor->onPickerLayoutChanged = [this] {
        QTimer::singleShot(0, m_requisitePopup, [this] {
            m_requisitePopupLayout->invalidate();
            positionRequisitePopup();
        });
    };
    m_detailEditor->onModeChanged = [this] {
        updateButtons();
        if (!m_detailEditor->active()) {
            m_delegatingFromFooter = false;
            hideRequisitePopup();
            if (m_activeDetail && m_activeDetail->isEnabled())
                m_activeDetail->setFocus(Qt::OtherFocusReason);
        }
    };
    auto connectRequisite = [this](TaskRequisiteRow *row, QVBoxLayout *group, Field field) {
        m_requisiteButtons.append({row, field});
        connect(row, &QPushButton::clicked, this, [this, row, group, field] {
            if (m_stageOpen || !m_detailEditor->canStart(field))
                return;
            m_activeDetail = row;
            if (auto *owner = m_detailEditor->parentWidget(); owner && owner->layout())
                owner->layout()->removeWidget(m_detailEditor);
            const bool picker = field != Field::Deadline && field != Field::Status;
            if (picker)
                openRequisitePopup(row, m_detailEditor);
            else
                group->insertWidget(group->indexOf(row) + 1, m_detailEditor);
            m_detailEditor->open(field);
            if (picker) {
                positionRequisitePopup();
                m_detailEditor->focusEditor();
            } else {
                scrollToRequisiteEditor(m_detailEditor);
            }
        });
    };
    m_creator = requisite(details, QStringLiteral("Постановщик"), workspace::AvatarKind::Person);
    connectRequisite(m_creator, details, Field::Creator);
    m_assignee = requisite(details, QStringLiteral("Исполнитель"), workspace::AvatarKind::Person);
    connectRequisite(m_assignee, details, Field::Assignee);
    m_deadline = requisite(details, QStringLiteral("Крайний срок"));
    connectRequisite(m_deadline, details, Field::Deadline);
    auto *divider = new QWidget(details->parentWidget());
    divider->setObjectName(QStringLiteral("taskRequisiteDivider"));
    divider->setFixedHeight(1);
    details->addWidget(divider);
    m_taskStatus = requisite(details, QStringLiteral("Статус"));
    connectRequisite(m_taskStatus, details, Field::Status);

    auto *createdRow = new QHBoxLayout;
    createdRow->setContentsMargins(8, 4, 8, 4);
    auto *createdCaption = workspace::label(QStringLiteral("Дата создания"), "fieldCaption", fields);
    createdCaption->setFixedWidth(100);
    createdRow->addWidget(createdCaption, 0, Qt::AlignTop);
    m_createdAt = workspace::label({}, "taskRequisiteValue", fields);
    m_createdAt->setAccessibleName(QStringLiteral("Дата создания"));
    m_createdAt->setTextInteractionFlags(Qt::TextSelectableByMouse);
    createdRow->addWidget(m_createdAt, 1);
    details->addLayout(createdRow);

    m_project = requisite(project, QStringLiteral("Проект"), workspace::AvatarKind::Project);
    connectRequisite(m_project, project, Field::Project);
    m_existingStage = requisite(project, QStringLiteral("Этап"));
    m_existingStage->setProperty("stageField", true);
    m_stageEditor = new QWidget(m_requisitePopup);
    auto *stageLayout = new QVBoxLayout(m_stageEditor);
    stageLayout->setContentsMargins(8, 3, 8, 4);
    stageLayout->setSpacing(5);
    m_stageSummary = new TaskPickerSummary(m_stageEditor);
    stageLayout->addWidget(m_stageSummary);
    m_stages = new QListWidget(m_stageEditor);
    m_stages->setItemDelegate(new TaskPickerChoiceDelegate(m_stages));
    m_stages->setObjectName(QStringLiteral("taskRequisiteChoices"));
    m_stages->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_stages->setIconSize(QSize(18, 18));
    m_stages->setAccessibleName(QStringLiteral("Этап задачи"));
    stageLayout->addWidget(m_stages);
    auto *stageActions = new QHBoxLayout;
    stageActions->addStretch(1);
    m_cancelStage = button(QStringLiteral("Отмена"), QStringLiteral("Отменить изменение этапа"), m_stageEditor);
    connect(m_cancelStage, &QPushButton::clicked, this, [this] { closeStageEditor(); });
    stageActions->addWidget(m_cancelStage);
    m_move = button(QStringLiteral("Повторить"), QStringLiteral("Повторить перенос задачи"), m_stageEditor);
    m_move->setObjectName(QStringLiteral("primary"));
    connect(m_move, &QPushButton::clicked, this, &TaskCardDialog::submitStage);
    stageActions->addWidget(m_move);
    stageLayout->addLayout(stageActions);
    m_requisitePopupLayout->addWidget(m_stageEditor);
    connect(m_stages, &QListWidget::itemClicked, this, &TaskCardDialog::selectStage);
    connect(m_stages, &QListWidget::itemActivated, this, &TaskCardDialog::selectStage);
    connect(m_existingStage, &QPushButton::clicked, this, [this] {
        m_stageBaseline = m_task.stageId;
        m_stageChoice = m_stageBaseline;
        m_stageSubmitAttempted = false;
        m_stages->setCurrentItem(stageItem(m_stageBaseline));
        m_stageOpen = true;
        openRequisitePopup(m_existingStage, m_stageEditor);
        updateButtons();
        positionRequisitePopup();
        m_stages->setFocus(Qt::OtherFocusReason);
    });
    m_observers = requisite(observers, QStringLiteral("Наблюдатели"), workspace::AvatarKind::Person, true);
    connectRequisite(m_observers, observers, Field::Observers);

    auto relations = section(QStringLiteral("Чек-листы"));
    m_relations = new TaskRelationsPanel(fields);
    relations.first->addWidget(m_relations);
    form->addStretch(1);
    m_splitter->addWidget(m_detailsScroll);

    auto *chat = new QWidget(m_splitter);
    chat->setMinimumWidth(285);
    chat->setObjectName(QStringLiteral("reading"));
    auto *thread = new QVBoxLayout(chat);
    thread->setContentsMargins(0, 0, 0, 0);
    thread->setSpacing(0);
    auto *top = new QWidget(chat);
    top->setObjectName(QStringLiteral("conversationHeader"));
    auto *head = new QHBoxLayout(top);
    head->setContentsMargins(9, 1, 5, 1);
    head->addWidget(workspace::label(QStringLiteral("Чат задачи"), "conversationTitle", top), 1);
    auto *searchToggle = new GlyphButton(Glyph::Search, QStringLiteral("Найти в чате задачи"), top);
    m_searchToggle = searchToggle;
    searchToggle->setCheckable(true);
    head->addWidget(searchToggle);
    thread->addWidget(top);
    m_pinned = new PinnedMessageBar(chat);
    thread->addWidget(m_pinned);
    m_contextBack = button(QStringLiteral("К последним сообщениям"), QStringLiteral("К последним сообщениям задачи"), chat);
    m_contextBack->hide();
    connect(m_contextBack, &QPushButton::clicked, this, [this] {
        if (m_onLatest)
            m_onLatest();
    });
    thread->addWidget(m_contextBack, 0, Qt::AlignRight);

    auto *searchBar = new QWidget(chat);
    searchBar->setObjectName(QStringLiteral("conversationSearch"));
    auto *search = new QHBoxLayout(searchBar);
    search->setContentsMargins(7, 2, 4, 2);
    search->setSpacing(4);
    m_messageSearch = new QLineEdit(searchBar);
    editor(m_messageSearch);
    m_messageSearch->setAccessibleName(QStringLiteral("Поиск по чату карточки"));
    m_messageSearch->setPlaceholderText(QStringLiteral("Поиск по истории"));
    search->addWidget(m_messageSearch, 1);
    auto *clear = new GlyphButton(Glyph::Close, QStringLiteral("Очистить поиск в карточке"), searchBar);
    connect(clear, &QPushButton::clicked, m_messageSearch, &QLineEdit::clear);
    connect(m_messageSearch, &QLineEdit::returnPressed, this, [this] { searchMessages(); });
    connect(m_messageSearch, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (text.trimmed().isEmpty())
            searchMessages();
    });
    search->addWidget(clear);
    thread->addWidget(searchBar);
    searchBar->hide();
    connect(searchToggle, &QPushButton::toggled, this, [this, searchBar](bool show) {
        searchBar->setVisible(show);
        if (show)
            m_messageSearch->setFocus(Qt::ShortcutFocusReason);
        else
            m_messageSearch->clear();
    });
    auto *find = new QAction(chat);
    find->setShortcut(QKeySequence::Find);
    find->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(find, &QAction::triggered, this, [this, searchToggle] {
        searchToggle->setChecked(true);
        m_messageSearch->setFocus(Qt::ShortcutFocusReason);
    });
    chat->addAction(find);

    auto *canvas = new ConversationCanvas(chat);
    auto *timeline = new QVBoxLayout(canvas);
    timeline->setContentsMargins(6, 4, 6, 5);
    timeline->setSpacing(0);
    m_messages = new HistoryList(canvas);
    m_messages->setAccessibleName(QStringLiteral("Чат карточки"));
    workspace::configureRows(m_messages, workspace::Row::Message);
    static_cast<HistoryList *>(m_messages)->setOnBrowsingOlder([this](bool active) {
        m_contextBack->setVisible(active || m_contextMode);
    });
    timeline->addWidget(static_cast<HistoryList *>(m_messages)->olderButton(canvas), 0, Qt::AlignHCenter);
    timeline->addWidget(m_messages, 1);
    m_empty = workspace::label(QStringLiteral("Загружаем чат…"), "empty", canvas);
    setLoadingIndicator(m_empty, true);
    m_empty->setAlignment(Qt::AlignCenter);
    timeline->addWidget(m_empty, 1);
    m_messages->hide();
    thread->addWidget(canvas, 1);

    m_composer = new ChatComposer(chat, QStringLiteral("Текст в чате карточки"),
        QStringLiteral("Эмодзи в карточке"), QStringLiteral("Отправить файл в чат задачи"),
        QStringLiteral("Отправить в карточке"));
    m_input = m_composer->input();
    static_cast<HistoryList *>(m_messages)->setComposer(m_input);
    m_input->installEventFilter(this);
    connect(m_input, &QPlainTextEdit::textChanged, this, [this] { updateButtons(); });
    m_emoji = m_composer->emoji();
    m_file = m_composer->file();
    connect(m_file, &QPushButton::clicked, this, [this] {
        if (m_onFile)
            m_onFile(false);
    });
    m_send = m_composer->send();
    connect(m_send, &QPushButton::clicked, this, [this] { send(); });
    m_composer->setEditStateChanged([this] { updateButtons(); });
    m_composer->setImageDraftHandlers([this] { updateButtons(); },
                                      [this](const QString &error) { setStatus(error); });
    thread->addWidget(m_composer);
    m_splitter->addWidget(chat);
    m_splitter->setStretchFactor(1, 1);
    QSettings settings(QStringLiteral("bitrix24-client"), QStringLiteral("bitrix24-client"));
    m_detailsWidth = qBound(0, settings.value(QStringLiteral("ui/taskCardDetailsWidth")).toInt(), 2000);
    restoreDetailsWidth();
    connect(m_splitter, &QSplitter::splitterMoved, this, [this] {
        m_detailsWidth = m_splitter->sizes().first();
        QSettings settings(QStringLiteral("bitrix24-client"), QStringLiteral("bitrix24-client"));
        settings.setValue(QStringLiteral("ui/taskCardDetailsWidth"), m_detailsWidth);
    });
    layout->addWidget(m_splitter, 1);
    addTaskActions(layout);
    m_status = workspace::label(QString(), "quiet", this);
    m_status->setAccessibleName(QStringLiteral("Состояние карточки"));
    layout->addWidget(m_status);
    m_status->hide();
    setTask(task, detailsLoaded);
    updateButtons();
}

void TaskCardDialog::setLink(const QString &link)
{
    m_link = link;
    m_copyLink->setEnabled(!link.isEmpty());
    m_copyLink->setToolTip(link.isEmpty() ? QStringLiteral("Ссылка на задачу недоступна")
                                         : m_copyLink->accessibleName());
}

void TaskCardDialog::setStatus(const QString &text, bool loading)
{
    setStatusLabel(m_status, text, loading);
}

void TaskCardDialog::setTask(const Task &task, bool detailsLoaded)
{
    const auto previousStage = m_task.stageId;
    const bool changedProject = task.projectId != m_projectId
        && (!task.projectId.isEmpty() || !m_task.projectId.isEmpty());
    if (changedProject) {
        m_projectId = task.projectId;
        m_projectCaption.clear();
        setLink({});
    }
    m_task = task;
    m_taskTitle->setText(task.name.isEmpty() ? QStringLiteral("Задача") : task.name);
    m_fields->setTask(task);
    m_detailEditor->setTask(task);
    setRequisite(m_creator, personName(task.createdBy, task.creatorId), task.creatorAvatarUrl);
    setRequisite(m_assignee, personName(task.assignee, task.assigneeId), task.assigneeAvatarUrl);
    setRequisite(m_deadline, task.due.isEmpty()
        ? QStringLiteral("Без срока") : TaskDetailEditor::formattedDate(task.due));
    const auto deadline = QDateTime::fromString(task.due, Qt::ISODate);
    static const QStringList openStatuses{QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"),
        QStringLiteral("6"), QStringLiteral("pending"), QStringLiteral("in_progress"), QStringLiteral("deferred")};
    const bool overdue = openStatuses.contains(task.statusCode) && deadline.isValid()
        && (task.due.size() == 10 ? deadline.date() < QDate::currentDate()
                                 : deadline < QDateTime::currentDateTime());
    const auto deadlineStyle = overdue
        ? QStringLiteral("QLabel#taskRequisiteValue { color: #a3372d; font-weight: 600; }") : QString();
    if (m_deadline->styleSheet() != deadlineStyle)
        m_deadline->setStyleSheet(deadlineStyle);
    setRequisite(m_taskStatus, task.status);
    m_createdAt->setText(task.createdAt.isValid()
        ? task.createdAt.toLocalTime().toString(QStringLiteral("dd.MM.yyyy  HH:mm")) : QStringLiteral("—"));
    showRequisiteProject();
    setObservers(task);
    const auto *selected = changedProject ? nullptr : stageItem(task.stageId);
    setRequisite(m_existingStage, selected ? selected->text() : task.stageName);
    refreshRequisiteAvatars();
    showTaskContent(task, detailsLoaded);
    if (changedProject) {
        closeStageEditor();
        setStages({});
    } else if (previousStage != task.stageId) {
        const QSignalBlocker blocker(m_stages);
        if (auto *previous = stageItem(previousStage)) {
            previous->setData(Qt::CheckStateRole, {});
            previous->setData(Qt::AccessibleDescriptionRole, {});
        }
        if (auto *current = stageItem(task.stageId)) {
            current->setCheckState(Qt::Checked);
            current->setData(Qt::AccessibleDescriptionRole, QStringLiteral("Текущий этап"));
        }
        if (!m_stageOpen)
            m_stages->setCurrentItem(stageItem(task.stageId));
    }
    updateButtons();
}

void TaskCardDialog::setStages(const QVector<Stage> &stages)
{
    const QSignalBlocker blocker(m_stages);
    m_stages->clear();
    for (const auto &stage : stages) {
        auto *option = new QListWidgetItem(stage.name, m_stages);
        option->setData(Qt::UserRole, stage.id);
        option->setFlags(option->flags() & ~Qt::ItemIsUserCheckable);
        option->setIcon(taskStageColorIcon(stage.color));
        if (stage.id == m_task.stageId) {
            option->setCheckState(Qt::Checked);
            option->setData(Qt::AccessibleDescriptionRole, QStringLiteral("Текущий этап"));
        }
    }
    fitStageList();
    auto *current = stageItem(m_task.stageId);
    auto *draft = stageItem(m_stageChoice);
    m_stages->setCurrentItem(m_stageOpen && draft ? draft : current);
    setRequisite(m_existingStage, current ? current->text() : m_task.stageName);
    updateButtons();
}

void TaskCardDialog::setWriteState(bool writable, bool editing, bool moving, bool sending, bool uploading)
{
    m_writable = writable;
    m_editing = editing;
    m_moving = moving;
    m_sending = sending;
    m_uploading = uploading;
    updateButtons();
}

TaskDescriptionEditor *TaskCardDialog::descriptionEditor() const
{
    return m_fields->descriptionEditor();
}

QScrollBar *TaskCardDialog::detailsScrollBar() const
{
    return m_detailsScroll->verticalScrollBar();
}

void TaskCardDialog::setProjectCaption(const QString &caption)
{
    m_projectCaption = caption;
    showRequisiteProject();
    refreshRequisiteAvatars();
}

void TaskCardDialog::acceptEdit()
{
    Task updated = m_task;
    if (m_stageOpen) {
        closeStageEditor();
        return;
    }
    if (m_detailEditor->active()) {
        m_detailEditor->accept(updated);
        setTask(updated);
        return;
    }
    if (!m_editMode)
        return;
    const auto pending = m_fields->edit();
    if (pending.title)
        updated.name = *pending.title;
    if (pending.description) {
        updated.description = *pending.description;
        updated.descriptionChecksum.clear();
    }
    if (pending.deadline)
        updated.due = *pending.deadline;
    if (pending.highPriority)
        updated.priority = *pending.highPriority ? QStringLiteral("2") : QStringLiteral("1");
    m_fields->setTask(updated, true);
    setTask(updated);
    setEditingDescription(false);
}

QString TaskCardDialog::searchQuery() const
{
    return m_messageSearch->text().trimmed();
}

void TaskCardDialog::setPinnedMessages(const QVector<Message> &messages)
{
    m_pinned->setMessages(messages);
}

void TaskCardDialog::setOnPinned(std::function<void(const QString &)> handler)
{
    m_pinned->setOnOpen(std::move(handler));
}

void TaskCardDialog::setOnLatest(std::function<void()> handler)
{
    m_onLatest = std::move(handler);
}

bool TaskCardDialog::focusMessage(const QString &id)
{
    return static_cast<HistoryList *>(m_messages)->focusOnMessage(id);
}

void TaskCardDialog::setContextMode(bool active)
{
    m_contextMode = active;
    m_contextBack->setVisible(active);
    if (active) {
        const QSignalBlocker search(m_messageSearch);
        const QSignalBlocker toggle(m_searchToggle);
        m_messageSearch->clear();
        m_searchToggle->setChecked(false);
        m_messageSearch->parentWidget()->hide();
        static_cast<HistoryList *>(m_messages)->setLiveReadHistory(false);
    } else {
        static_cast<HistoryList *>(m_messages)->returnToLatest();
    }
}

void TaskCardDialog::showMessages(const QVector<Message> &messages, MessageView view)
{
    auto *history = static_cast<HistoryList *>(m_messages);
    if (view == MessageView::History) {
        history->showHistory(taskId(), m_empty, messages);
    } else if (view == MessageView::Context) {
        history->showContext(taskId(), m_empty, messages);
    } else {
        history->setLiveReadHistory(false);
        presentMessages(m_messages, m_empty, messages, view);
    }
}

void TaskCardDialog::setChatStatus(const QString &text, bool loading)
{
    static_cast<HistoryList *>(m_messages)->setLiveReadHistory(false);
    showEmpty(m_messages, m_empty, text, loading);
}

void TaskCardDialog::setOnCopy(std::function<void(const QString &)> handler)
{
    m_onCopy = std::move(handler);
}

void TaskCardDialog::setOnSave(std::function<void(const TaskEdit &)> handler)
{
    m_onSave = std::move(handler);
}

void TaskCardDialog::setOnDelete(std::function<void()> handler)
{
    m_onDelete = std::move(handler);
}

bool TaskCardDialog::canDeleteTask() const
{
    return m_task.editRights.remove && canUseTaskActions() && !hasUnsavedChanges();
}

void TaskCardDialog::cancelFooterDelegation()
{
    if (m_delegatingFromFooter)
        m_detailEditor->cancel();
}

void TaskCardDialog::setProjectChoices(const QVector<Project> &projects)
{
    m_requisiteProjects = projects;
    m_detailEditor->setProjectChoices(projects);
    showRequisiteProject();
    refreshRequisiteAvatars();
    updateButtons();
}

void TaskCardDialog::setCanDetachProject(bool allowed)
{
    m_detailEditor->setCanDetachProject(allowed);
    updateButtons();
}

void TaskCardDialog::setMoreProjectsAvailable(bool available)
{
    m_detailEditor->setMoreProjectsAvailable(available);
    updateButtons();
}

void TaskCardDialog::setRequisiteAvatars(TaskRequisiteRow::AvatarSource source)
{
    m_requisiteAvatars = std::move(source);
    m_detailEditor->setAvatarSource(m_requisiteAvatars);
    refreshRequisiteAvatars();
}

void TaskCardDialog::setOnRequisiteAvatarChoicesChanged(std::function<void()> changed)
{
    m_onRequisiteAvatarChoicesChanged = std::move(changed);
}

QList<QUrl> TaskCardDialog::visibleRequisiteAvatarUrls() const
{
    QList<QUrl> urls;
    urls.append(m_detailEditor->visibleAvatarUrls());
    for (const auto *row : {m_creator, m_assignee, m_project, m_observers})
        urls.append(row->visibleAvatarUrls(m_detailsScroll->viewport()));
    return urls;
}

void TaskCardDialog::refreshRequisiteAvatars()
{
    for (auto *row : {m_creator, m_assignee, m_project, m_observers})
        row->refreshAvatars(m_requisiteAvatars);
    m_detailEditor->refreshAvatars();
}

void TaskCardDialog::setOnMoreProjects(std::function<void()> handler)
{
    m_detailEditor->onMoreProjects = std::move(handler);
}

void TaskCardDialog::setPeopleSearch(const std::function<void(const QString &, const QString &)> &handler)
{
    m_detailEditor->setPeopleSearch(handler);
    updateButtons();
}

void TaskCardDialog::setPeopleResults(const QString &id, const QVector<Person> &people)
{
    m_detailEditor->setPeopleResults(id, people);
}

bool TaskCardDialog::setPeopleSearchError(const QString &id)
{
    return m_detailEditor->setPeopleSearchError(id);
}

void TaskCardDialog::setOnMove(std::function<void(const QString &)> handler)
{
    m_onMove = std::move(handler);
}

void TaskCardDialog::setOnSend(std::function<void()> handler)
{
    m_onSend = std::move(handler);
}

void TaskCardDialog::setOnSearch(std::function<void(const QString &)> handler)
{
    m_onSearch = std::move(handler);
}

void TaskCardDialog::setOnFile(std::function<void(bool)> handler)
{
    m_onFile = std::move(handler);
}

bool TaskCardDialog::hasUnsavedChanges() const
{
    if (m_sending || m_uploading || m_editing)
        return true;
    if (m_composer && !m_composer->draft().isEmpty())
        return true;
    return (m_fields && m_editMode && m_fields->edit().hasChanges())
        || (m_detailEditor && m_detailEditor->changed()) || hasStageDraft();
}

void TaskCardDialog::setDiscardConfirmation(std::function<bool()> handler)
{
    m_confirmDiscard = std::move(handler);
}

bool TaskCardDialog::confirmDiscardIfNeeded(bool remember)
{
    if (m_discardConfirmed || !hasUnsavedChanges())
        return true;
    const bool confirmed = m_confirmDiscard ? m_confirmDiscard() : confirmDiscardDialog();
    if (confirmed && remember)
        m_discardConfirmed = true;
    return confirmed;
}

void TaskCardDialog::closeEvent(QCloseEvent *event)
{
    if (!confirmDiscardIfNeeded()) {
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void TaskCardDialog::reject()
{
    if (m_composer->cancelMessageEdit())
        return;
    if (!confirmDiscardIfNeeded())
        return;
    QDialog::reject();
}

void TaskCardDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    if (m_splitter)
        restoreDetailsWidth();
    if (m_popupAnchor) {
        m_detailEditor->fitChoiceLists();
        fitStageList();
    }
    positionRequisitePopup();
}

void TaskCardDialog::changeEvent(QEvent *event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange && m_maximize) {
        m_maximize->setText(isMaximized() ? QStringLiteral("Восстановить") : QStringLiteral("Развернуть"));
        m_maximize->setAccessibleName(isMaximized() ? QStringLiteral("Восстановить размер карточки задачи")
                                                  : QStringLiteral("Развернуть карточку задачи"));
        QTimer::singleShot(0, this, [this] { restoreDetailsWidth(); });
    }
}

bool TaskCardDialog::eventFilter(QObject *target, QEvent *event)
{
    if (target == m_requisitePopup && event->type() == QEvent::LayoutRequest)
        positionRequisitePopup();
    if (target == m_input && event->type() == QEvent::KeyPress && !m_composer->composing()) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (m_composer->handleComposerKey(key))
            return true;
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
            !(key->modifiers() & Qt::ShiftModifier)) {
            send();
            return true;
        }
    }
    return QDialog::eventFilter(target, event);
}

bool TaskCardDialog::confirmDiscardDialog()
{
    auto *box = new QMessageBox(QMessageBox::Warning, QStringLiteral("Несохранённые изменения"),
        QStringLiteral("В карточке задачи есть несохранённые изменения. Закрыть карточку без сохранения?"),
        QMessageBox::NoButton, this);
    box->setAccessibleName(QStringLiteral("Несохранённые изменения в карточке"));
    auto *cancel = box->addButton(QStringLiteral("Отмена"), QMessageBox::RejectRole);
    auto *discard = box->addButton(QStringLiteral("Закрыть без сохранения"), QMessageBox::DestructiveRole);
    box->setDefaultButton(cancel);
    box->exec();
    box->deleteLater();
    return box->clickedButton() == discard;
}

void TaskCardDialog::send()
{
    if (m_composer->submitMessageEdit())
        return;
    if (m_onSend && m_send->isEnabled())
        m_onSend();
}

void TaskCardDialog::searchMessages()
{
    if (m_onSearch)
        m_onSearch(searchQuery());
}

void TaskCardDialog::copy(const QString &value, const QString &feedback)
{
    if (!m_onCopy || value.isEmpty())
        return;
    m_onCopy(value);
    m_copyFeedback->setText(feedback);
    m_copyFeedbackTimer.start();
}

void TaskCardDialog::restoreDetailsWidth()
{
    const int available = width() - 37;
    const int preferred = m_detailsWidth > 0 ? m_detailsWidth : qMin(440, available * 53 / 100);
    const int left = qBound(280, preferred, qMax(280, available - 285));
    m_splitter->setSizes({left, qMax(285, available - left)});
}

void TaskCardDialog::setEditingDescription(bool enabled)
{
    m_editMode = enabled;
    m_fields->setVisible(enabled);
    m_editActions->setVisible(enabled);
    m_descriptionRows->setVisible(!enabled);
    m_edit->setVisible(!enabled);
    updateButtons();
}

QString TaskCardDialog::personName(const QString &name, const QString &id)
{
    return name.isEmpty() && !id.isEmpty() ? QStringLiteral("Сотрудник №%1").arg(id) : name;
}

void TaskCardDialog::setRequisite(TaskRequisiteRow *row, const QString &text, const QUrl &url)
{
    row->setEntries(text.isEmpty() ? QVector<TaskRequisiteRow::Entry>() : QVector<TaskRequisiteRow::Entry>{{text, url}});
}

void TaskCardDialog::setObservers(const Task &task)
{
    QVector<TaskRequisiteRow::Entry> observers;
    QStringList remaining = task.observers;
    for (const auto &person : task.observerPeople) {
        const auto name = personName(person.name, person.id);
        if (!name.isEmpty())
            observers.append({name, person.avatarUrl});
        remaining.removeOne(person.name);
    }
    for (const auto &name : std::as_const(remaining)) {
        if (!name.isEmpty())
            observers.append({name, {}});
    }
    m_observers->setEntries(std::move(observers));
}

void TaskCardDialog::showRequisiteProject()
{
    const auto item = std::find_if(m_requisiteProjects.cbegin(), m_requisiteProjects.cend(), [this](const Project &project) {
        return !m_projectId.isEmpty() && project.id == m_projectId;
    });
    auto name = m_task.project.isEmpty() ? m_projectCaption : m_task.project;
    if (name.isEmpty() && item != m_requisiteProjects.cend())
        name = item->name;
    if (name.isEmpty() || item == m_requisiteProjects.cend()) {
        setRequisite(m_project, name);
        return;
    }
    m_project->setEntries({{name, item->avatarUrl, item->avatarColor, item->avatarVariant, item->avatarType}});
}

QListWidgetItem *TaskCardDialog::stageItem(const QString &id) const
{
    if (!id.isEmpty()) {
        for (int i = 0; i < m_stages->count(); ++i)
            if (m_stages->item(i)->data(Qt::UserRole).toString() == id)
                return m_stages->item(i);
    }
    return nullptr;
}

void TaskCardDialog::fitStageList()
{
    const int visibleRows = height() >= 570 ? 7 : 4;
    m_stages->setFixedHeight(qMax(40, qMin(visibleRows, m_stages->count()) * TaskPickerChoiceDelegate::RowHeight + 5));
}

void TaskCardDialog::updateStageSummary()
{
    const auto *confirmed = stageItem(m_task.stageId);
    const auto *candidate = stageItem(m_stageChoice);
    m_stageSummary->setCurrent(confirmed ? confirmed->text() : m_task.stageName,
                               confirmed ? confirmed->icon() : QIcon());
    const bool draft = candidate && candidate != confirmed;
    m_stageSummary->setPending(draft ? candidate->text() : QString());
    for (int i = 0; i < m_stages->count(); ++i) {
        auto *row = m_stages->item(i);
        row->setData(TaskPickerDraft, draft && row == candidate);
        row->setData(Qt::AccessibleDescriptionRole, row == confirmed ? QStringLiteral("Текущий этап")
                     : draft && row == candidate ? QStringLiteral("Новый этап") : QString());
    }
}

void TaskCardDialog::selectStage(QListWidgetItem *item)
{
    if (!m_stageOpen || !m_stages->isEnabled() || !item)
        return;
    const auto choice = item->data(Qt::UserRole).toString();
    if (choice.isEmpty() || choice == m_stageChoice)
        return;
    m_stageChoice = choice;
    updateButtons();
    submitStage();
}

void TaskCardDialog::submitStage()
{
    if (!m_onMove || !m_move->isEnabled())
        return;
    m_stageSubmitAttempted = true;
    updateButtons();
    m_onMove(m_stageChoice);
}

bool TaskCardDialog::hasStageDraft() const
{
    return m_stageOpen && !m_stageChoice.isEmpty() && m_stageChoice != m_stageBaseline;
}

void TaskCardDialog::openRequisitePopup(TaskRequisiteRow *row, QWidget *editor)
{
    m_popupAnchor = row;
    m_detailsScroll->ensureWidgetVisible(row, 4, 4);
    if (m_requisitePopupLayout->indexOf(editor) < 0)
        m_requisitePopupLayout->addWidget(editor);
    m_requisitePopup->setAccessibleName(row->accessibleName());
    m_requisitePopup->show();
    m_requisitePopup->raise();
}

void TaskCardDialog::positionRequisitePopup()
{
    if (!m_popupAnchor || !m_requisitePopup->isVisible())
        return;
    const QRect bounds = rect().adjusted(9, 9, -9, -9);
    const QRect row(m_popupAnchor->mapTo(this, QPoint()), m_popupAnchor->size());
    const int width = qMin(bounds.width(), qBound(280, row.width() + 35, 475));
    const int height = qMin(bounds.height(), m_requisitePopup->sizeHint().height());
    const int below = row.bottom() + 5;
    const int y = below + height <= bounds.bottom() ? below : row.top() - height - 5;
    m_requisitePopup->setGeometry(qBound(bounds.left(), row.left(), bounds.right() - width + 1),
                                 qBound(bounds.top(), y, bounds.bottom() - height + 1), width, height);
}

void TaskCardDialog::hideRequisitePopup()
{
    m_popupAnchor = nullptr;
    m_requisitePopup->hide();
}

void TaskCardDialog::scrollToRequisiteEditor(QWidget *editor)
{
    QTimer::singleShot(0, editor, [this, editor] {
        if (!editor->isVisible())
            return;
        auto *focus = QApplication::focusWidget();
        m_detailsScroll->ensureWidgetVisible(focus && editor->isAncestorOf(focus) ? focus : editor, 8, 8);
    });
}

void TaskCardDialog::closeStageEditor()
{
    if (!m_stageOpen)
        return;
    m_stageOpen = false;
    m_stageBaseline.clear();
    m_stageChoice.clear();
    m_stageSubmitAttempted = false;
    hideRequisitePopup();
    updateButtons();
    if (m_existingStage->isEnabled())
        m_existingStage->setFocus(Qt::OtherFocusReason);
}

void TaskCardDialog::addTaskFile(TaskContentList *list, const MessageFile &file, int serial)
{
    auto *item = workspace::addRow(list, {}, {}, {}, QString::number(serial));
    item->setData(workspace::MessageFiles, QVariant::fromValue(QVector<MessageFile>{file}));
    item->setData(Qt::AccessibleTextRole, file.name);
}

void TaskCardDialog::showTaskContent(const Task &task, bool detailsLoaded)
{
    static const QRegularExpression embedded(QStringLiteral("\\[disk\\s+file\\b([^\\]\\r\\n]*)\\]"),
                                              QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression id(QStringLiteral("(?:^|\\s)id\\s*=\\s*[\"']?(n?[0-9]+)(?=[\"'\\s]|$)"),
                                       QRegularExpression::CaseInsensitiveOption);
    QString description;
    QVector<MessageFile> inlineFiles;
    QSet<QString> seen;
    qsizetype start = 0;
    auto matches = embedded.globalMatch(task.description);
    while (matches.hasNext()) {
        const auto match = matches.next();
        description += task.description.sliced(start, match.capturedStart() - start);
        const auto fileId = id.match(match.captured(1)).captured(1);
        auto file = std::find_if(task.files.cbegin(), task.files.cend(), [&fileId](const MessageFile &entry) {
            return !fileId.isEmpty() && (entry.id == fileId || entry.diskObjectId == fileId);
        });
        if (file == task.files.cend()) {
            description += QStringLiteral("Вложение недоступно");
        } else {
            description += QStringLiteral("Файл в описании");
            const auto key = file->id.isEmpty() ? file->diskObjectId : file->id;
            if (!seen.contains(key)) {
                inlineFiles.append(*file);
                seen.insert(key);
            }
        }
        start = match.capturedEnd();
    }
    description += task.description.sliced(start);
    m_descriptionRows->clear();
    auto *body = workspace::addMarkupRow(m_descriptionRows, {},
        description.trimmed().isEmpty() ? QStringLiteral("Описание пока не добавлено") : description,
        {}, QStringLiteral("task-description"));
    body->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    for (int i = 0; i < inlineFiles.size(); ++i)
        addTaskFile(m_descriptionRows, inlineFiles[i], i + 1);
    m_attachmentRows->clear();
    auto unavailable = attachmentNames(task.attachments);
    for (const auto &file : task.files) {
        unavailable.removeAll(attachmentNames({file.name}).value(0));
        addTaskFile(m_attachmentRows, file, m_attachmentRows->count() + 1);
    }
    for (const auto &name : unavailable) {
        MessageFile file;
        file.name = name;
        addTaskFile(m_attachmentRows, file, m_attachmentRows->count() + 1);
    }
    if (!m_attachmentRows->count())
        workspace::emptyRow(m_attachmentRows, detailsLoaded ? QStringLiteral("Файлов пока нет")
                                                            : QStringLiteral("Файлы ещё не загружены"));
    m_descriptionRows->requestHeight();
    m_attachmentRows->requestHeight();
}

void TaskCardDialog::addTaskActions(QVBoxLayout *layout)
{
    m_taskActions = new QWidget(this);
    auto *row = new QHBoxLayout(m_taskActions);
    row->setContentsMargins(0, 0, 0, 0);
    m_primaryTaskActionButton = button({}, {}, m_taskActions);
    m_primaryTaskActionButton->setObjectName(QStringLiteral("taskPrimaryAction"));
    connect(m_primaryTaskActionButton, &QPushButton::clicked, this, [this] {
        if (m_primaryTaskAction)
            submitTaskStatus(*m_primaryTaskAction);
    });
    row->addWidget(m_primaryTaskActionButton);
    m_completeTaskAction = button(TaskDetailEditor::actionLabel(TaskStatusAction::Complete),
                                  QStringLiteral("Завершить задачу"), m_taskActions);
    m_completeTaskAction->setObjectName(QStringLiteral("primary"));
    connect(m_completeTaskAction, &QPushButton::clicked, this, [this] { submitTaskStatus(TaskStatusAction::Complete); });
    row->addWidget(m_completeTaskAction);

    m_taskOverflowButton = button(QStringLiteral("Ещё"), QStringLiteral("Другие действия с задачей"), m_taskActions);
    auto *more = new QMenu(m_taskOverflowButton);
    more->setObjectName(QStringLiteral("taskOverflowMenu"));
    m_taskOverflowButton->setMenu(more);
    for (const auto action : {TaskStatusAction::Start, TaskStatusAction::Pause, TaskStatusAction::Renew,
                              TaskStatusAction::Approve, TaskStatusAction::Disapprove, TaskStatusAction::Take,
                              TaskStatusAction::Defer}) {
        auto *item = more->addAction(TaskDetailEditor::actionLabel(action));
        connect(item, &QAction::triggered, this, [this, action] { submitTaskStatus(action); });
        m_taskStatusMenuActions.append({action, item});
    }
    m_taskDelegateAction = more->addAction(QStringLiteral("Делегировать"));
    connect(m_taskDelegateAction, &QAction::triggered, this, [this] {
        if (!canUseTaskActions() || !m_task.editRights.delegate || !m_task.editRights.assignee)
            return;
        m_delegatingFromFooter = true;
        m_assignee->click();
        if (!m_detailEditor->active())
            m_delegatingFromFooter = false;
    });
    m_taskDeleteSeparator = more->addSeparator();
    m_taskDeleteAction = more->addAction(QStringLiteral("Удалить задачу"));
    connect(m_taskDeleteAction, &QAction::triggered, this, [this] {
        if (canDeleteTask() && m_onDelete)
            m_onDelete();
    });
    connect(more, &QMenu::aboutToShow, this, [this] {
        m_taskDeleteAction->setEnabled(canDeleteTask());
    });
    row->addWidget(m_taskOverflowButton);
    row->addStretch(1);

    m_taskRatingButton = button(QStringLiteral("Оценка"), QStringLiteral("Оценка задачи"), m_taskActions);
    auto *rating = new QMenu(m_taskRatingButton);
    rating->setObjectName(QStringLiteral("taskRatingMenu"));
    m_taskRatingButton->setMenu(rating);
    for (const auto value : {TaskRating::Unrated, TaskRating::Positive, TaskRating::Negative}) {
        auto *item = rating->addAction(taskRatingLabel(value));
        item->setCheckable(true);
        connect(item, &QAction::triggered, this, [this, value] {
            if (canUseTaskActions() && m_task.editRights.rate && m_task.rating != value && m_onSave) {
                TaskEdit edit;
                edit.rating = value;
                m_onSave(edit);
            }
            updateTaskActions();
        });
        m_taskRatingMenuActions.append({value, item});
    }
    row->addWidget(m_taskRatingButton);
    layout->addWidget(m_taskActions);
}

QString TaskCardDialog::taskRatingLabel(TaskRating value)
{
    switch (value) {
    case TaskRating::Unrated: return QStringLiteral("Без оценки");
    case TaskRating::Positive: return QStringLiteral("Положительная");
    case TaskRating::Negative: return QStringLiteral("Отрицательная");
    }
    return {};
}

bool TaskCardDialog::canUseTaskActions() const
{
    return m_writable && !m_editing && !m_moving && !m_sending && !m_uploading && !m_editMode
        && !m_stageOpen && !m_detailEditor->active() && !m_composer->editingMessage();
}

void TaskCardDialog::submitTaskStatus(TaskStatusAction action)
{
    if (!canUseTaskActions() || !m_task.statusActions.contains(action) || !m_onSave)
        return;
    TaskEdit edit;
    edit.statusAction = action;
    m_onSave(edit);
}

void TaskCardDialog::updateTaskActions()
{
    if (!m_taskActions)
        return;
    const bool enabled = canUseTaskActions();
    const auto &allowed = m_task.statusActions;
    m_primaryTaskAction.reset();
    for (const auto action : {TaskStatusAction::Start, TaskStatusAction::Pause, TaskStatusAction::Renew,
                              TaskStatusAction::Approve, TaskStatusAction::Take}) {
        if (allowed.contains(action)) {
            m_primaryTaskAction = action;
            break;
        }
    }
    m_primaryTaskActionButton->setVisible(m_primaryTaskAction.has_value());
    m_primaryTaskActionButton->setEnabled(enabled);
    if (m_primaryTaskAction) {
        const auto text = TaskDetailEditor::actionLabel(*m_primaryTaskAction);
        m_primaryTaskActionButton->setText(text);
        m_primaryTaskActionButton->setAccessibleName(text);
    }
    const bool complete = allowed.contains(TaskStatusAction::Complete);
    m_completeTaskAction->setVisible(complete);
    m_completeTaskAction->setEnabled(enabled);

    const auto &rights = m_task.editRights;
    const bool delegate = rights.delegate && rights.assignee;
    bool overflow = delegate;
    for (const auto &[action, item] : std::as_const(m_taskStatusMenuActions)) {
        const bool visible = allowed.contains(action) && m_primaryTaskAction != action;
        item->setVisible(visible);
        item->setEnabled(enabled && visible);
        overflow |= visible;
    }
    m_taskDelegateAction->setVisible(delegate);
    m_taskDelegateAction->setEnabled(enabled && delegate);
    m_taskDeleteSeparator->setVisible(overflow && rights.remove);
    m_taskDeleteAction->setVisible(rights.remove);
    m_taskDeleteAction->setEnabled(enabled && rights.remove && m_input->document()->isEmpty()
                                   && m_composer->reply().id.isEmpty());
    overflow |= rights.remove;
    m_taskOverflowButton->setVisible(overflow);
    m_taskOverflowButton->setEnabled(enabled);

    const bool rated = m_task.rating != TaskRating::Unrated;
    m_taskRatingButton->setVisible(rights.rate || rated);
    m_taskRatingButton->setEnabled(enabled && rights.rate);
    m_taskRatingButton->setText(rated ? QStringLiteral("Оценка · %1").arg(taskRatingLabel(m_task.rating).toLower())
                                     : QStringLiteral("Оценка"));
    for (const auto &[value, item] : std::as_const(m_taskRatingMenuActions)) {
        item->setChecked(m_task.rating == value);
        item->setEnabled(enabled && rights.rate);
    }
    m_taskActions->setVisible(m_primaryTaskAction.has_value() || complete || overflow || rights.rate || rated);
}

void TaskCardDialog::updateButtons()
{
    m_relations->setWritable(m_writable);
    if (!m_send)
        return;
    const auto &rights = m_task.editRights;
    const bool allowsDescriptionEdits = rights.title || rights.description || rights.priority;
    m_detailEditor->setWriteState(m_writable && !m_editMode && !m_stageOpen, m_editing || m_moving);
    for (const auto &[button, field] : m_requisiteButtons)
        button->setEditable(m_detailEditor->canStart(field));
    m_fields->setWritable(m_writable && m_editMode && !m_editing, &rights);
    m_fields->descriptionEditor()->setUploadEnabled(m_writable && m_editMode && !m_editing && !m_uploading
                                                     && rights.description);
    m_edit->setEnabled(m_writable && !m_editing && !m_moving && !m_detailEditor->active()
                       && !m_stageOpen && allowsDescriptionEdits);
    m_cancelEdit->setEnabled(!m_editing && !m_uploading);
    m_save->setEnabled(m_writable && m_editMode && !m_editing && !m_uploading && allowsDescriptionEdits);
    const bool mayMove = m_writable && rights.stage && !m_moving && !m_editing && !m_editMode
        && !m_detailEditor->active() && m_stages->count() > 0;
    const bool stageCurrent = m_task.stageId == m_stageBaseline;
    m_existingStage->setEditable(mayMove && !m_stageOpen);
    m_stageEditor->setVisible(m_stageOpen);
    if (m_stageOpen)
        updateStageSummary();
    m_stages->setEnabled(m_stageOpen && mayMove);
    m_cancelStage->setEnabled(m_stageOpen && !m_moving && !m_editing);
    m_move->setVisible(hasStageDraft() && m_stageSubmitAttempted && !m_moving && !m_editing);
    m_move->setEnabled(m_stageOpen && mayMove && stageCurrent && hasStageDraft() && stageItem(m_stageChoice));
    m_move->setToolTip(stageCurrent ? QString() : QStringLiteral("Этап изменился на портале. Откройте поле заново."));
    const bool editingMessage = m_composer->editingMessage();
    const bool savingMessage = m_composer->messageEditPending();
    m_input->setReadOnly(!m_writable || savingMessage);
    m_emoji->setEnabled(m_writable && !savingMessage);
    m_attach->setEnabled(m_writable && !m_uploading);
    m_file->setEnabled(m_writable && !m_uploading && !editingMessage);
    const auto wire = m_composer->wireText();
    const auto text = wire.trimmed();
    const bool canSubmit = editingMessage ? m_composer->canSaveMessageEdit(wire) : !m_sending
        && text.size() <= kMessageCharacters && (m_composer->hasDraftImage() ? !m_uploading : !text.isEmpty());
    m_send->setEnabled(m_writable && canSubmit);
    m_send->setToolTip(editingMessage ? (savingMessage ? QStringLiteral("Сохраняем сообщение…") : QStringLiteral("Сохранить сообщение"))
                                       : m_send->accessibleName());
    updateTaskActions();
}

} // namespace bx::ui
