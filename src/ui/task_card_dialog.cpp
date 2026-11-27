#include "ui/task_card_dialog.h"

#include "ui/chat_reply.h"
#include "ui/task_card_actions.h"
#include "ui/task_card_requisites.h"
#include "ui/task_dialog_controls.h"
#include "ui/task_fields.h"

#include <QAction>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QVBoxLayout>

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
    m_requisites = new TaskCardRequisites(*this, fields, details, project, observers);

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
    m_taskActions = new TaskCardActions(*this);
    layout->addWidget(m_taskActions);
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
        setLink({});
    }
    m_task = task;
    m_taskTitle->setText(task.name.isEmpty() ? QStringLiteral("Задача") : task.name);
    m_fields->setTask(task);
    m_requisites->showTask(task, changedProject);
    showTaskContent(task, detailsLoaded);
    m_requisites->syncStage(task, changedProject, previousStage);
    updateButtons();
}

void TaskCardDialog::setStages(const QVector<Stage> &stages)
{
    m_requisites->setStages(stages);
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
    m_requisites->setProjectCaption(caption);
}

void TaskCardDialog::acceptEdit()
{
    Task updated = m_task;
    if (m_requisites->stageOpen()) {
        m_requisites->closeStageEditor();
        return;
    }
    if (m_requisites->editor()->active()) {
        m_requisites->editor()->accept(updated);
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
    m_requisites->cancelFooterDelegation();
}

void TaskCardDialog::setProjectChoices(const QVector<Project> &projects)
{
    m_requisites->setProjectChoices(projects);
}

void TaskCardDialog::setCanDetachProject(bool allowed)
{
    m_requisites->setCanDetachProject(allowed);
}

void TaskCardDialog::setMoreProjectsAvailable(bool available)
{
    m_requisites->setMoreProjectsAvailable(available);
}

void TaskCardDialog::setRequisiteAvatars(TaskRequisiteRow::AvatarSource source)
{
    m_requisites->setRequisiteAvatars(std::move(source));
}

void TaskCardDialog::setOnRequisiteAvatarChoicesChanged(std::function<void()> changed)
{
    m_requisites->setOnRequisiteAvatarChoicesChanged(std::move(changed));
}

QList<QUrl> TaskCardDialog::visibleRequisiteAvatarUrls() const
{
    return m_requisites->visibleRequisiteAvatarUrls();
}

void TaskCardDialog::refreshRequisiteAvatars()
{
    m_requisites->refreshRequisiteAvatars();
}

void TaskCardDialog::setOnMoreProjects(std::function<void()> handler)
{
    m_requisites->setOnMoreProjects(std::move(handler));
}

void TaskCardDialog::setPeopleSearch(const std::function<void(const QString &, const QString &)> &handler)
{
    m_requisites->setPeopleSearch(handler);
}

void TaskCardDialog::setPeopleResults(const QString &id, const QVector<Person> &people)
{
    m_requisites->setPeopleResults(id, people);
}

bool TaskCardDialog::setPeopleSearchError(const QString &id)
{
    return m_requisites->setPeopleSearchError(id);
}

void TaskCardDialog::setOnMove(std::function<void(const QString &)> handler)
{
    m_requisites->setOnMove(std::move(handler));
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
        || (m_requisites && m_requisites->editor()->changed()) || (m_requisites && m_requisites->hasStageDraft());
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
    if (m_requisites)
        m_requisites->resizePopup();
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

bool TaskCardDialog::canUseTaskActions() const
{
    return m_writable && !m_editing && !m_moving && !m_sending && !m_uploading && !m_editMode
        && m_requisites && !m_requisites->stageOpen() && !m_requisites->editor()->active()
        && !m_composer->editingMessage();
}

void TaskCardDialog::updateButtons()
{
    if (!m_requisites)
        return;
    m_relations->setWritable(m_writable);
    if (!m_send)
        return;
    const auto &rights = m_task.editRights;
    const bool allowsDescriptionEdits = rights.title || rights.description || rights.priority;
    m_requisites->updateWriteState();
    m_fields->setWritable(m_writable && m_editMode && !m_editing, &rights);
    m_fields->descriptionEditor()->setUploadEnabled(m_writable && m_editMode && !m_editing && !m_uploading
                                                     && rights.description);
    m_edit->setEnabled(m_writable && !m_editing && !m_moving && !m_requisites->editor()->active()
                       && !m_requisites->stageOpen() && allowsDescriptionEdits);
    m_cancelEdit->setEnabled(!m_editing && !m_uploading);
    m_save->setEnabled(m_writable && m_editMode && !m_editing && !m_uploading && allowsDescriptionEdits);
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
    if (m_taskActions)
        m_taskActions->refresh();
}

} // namespace bx::ui
