#include "ui/chat_panel.h"
#include "ui/chat_widgets.h"
#include "ui/task_dialogs.h"
#include "ui/requisite_avatar.h"
#include "ui/reaction_catalog.h"
#include "ui/workspace_directory.h"
#include "ui/workspace_presentation.h"
#include "ui/workspace_message_previews.h"
#include "ui/workspace_message_viewers.h"
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTabBar>
#include <QTimer>
#include <QWindow>

namespace bx {
using workspace::label;
using ui::emptyState;
using ui::ListViewportAnchor;
using ui::listViewportAnchor;
using ui::restoreListViewport;
using ui::presentMessages;
using ui::showEmpty;

ChatPanel::ChatPanel(Backend *backend, bool readOnly, Workspace workspace, QWidget *parent)
    : QWidget(parent), m_backend(backend), m_readOnly(readOnly), m_workspace(std::move(workspace)),
      m_historySearch(backend, false),
      m_chatSendController(ui::ComposerSendController::Surface::Chat, backend,
          [this](const QString &, const QString &status, bool loading) {
              emit statusChanged(status, loading);
              updateComposers();
          })
{
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *splitter = new DirectorySplitter(QStringLiteral("ui/chatDirectoryWidth"), 272, 218, this);
    splitter->setObjectName(QStringLiteral("chatDirectorySplitter"));
    splitter->setAccessibleName(QStringLiteral("Ширина списка чатов"));
    root->addWidget(splitter);
    auto *side = directory(QStringLiteral("Чаты"), &m_chats, splitter);
    m_chats->setAccessibleName(QStringLiteral("Недавние чаты"));
    m_taskChatPressEnd = new QTimer(this);
    m_taskChatPressEnd->setSingleShot(true);
    connect(m_taskChatPressEnd, &QTimer::timeout, this, [this] {
        m_taskChatPressActive = false;
        if (m_active && m_chats->isVisible() && std::exchange(m_taskChatRefreshDeferred, false) && m_chatScope == 1
            && m_chatDirectories[1].query.isEmpty())
            showChatResults(m_chatDirectories[1].recent, true, true);
    });
    m_chats->viewport()->installEventFilter(this);
    connect(m_chats, &QListWidget::currentItemChanged, this, &ChatPanel::onChatSelected);
    const auto focusSelectedComposer = [this](const QListWidgetItem *item) {
        if (item && item->data(workspace::Id).toString() == m_currentChatId) {
            focusChatComposer();
            scheduleVisibleChatRead();
        }
    };
    connect(m_chats, &QListWidget::itemClicked, this, focusSelectedComposer);
    connect(m_chats, &QListWidget::itemActivated, this, focusSelectedComposer);
    auto *sideLayout = qobject_cast<QVBoxLayout *>(side->layout());
    sideLayout->setContentsMargins(8, 13, 8, 8);
    sideLayout->setSpacing(6);
    sideLayout->itemAt(0)->widget()->hide();
    m_chatTabs = new QTabBar(side);
    m_chatTabs->setObjectName(QStringLiteral("chatScopeTabs"));
    m_chatTabs->setAccessibleName(QStringLiteral("Список чатов"));
    m_chatTabs->addTab(QStringLiteral("Чаты"));
    m_chatTabs->addTab(QStringLiteral("Чаты задач"));
    m_chatTabs->setExpanding(true);
    m_chatTabs->setDrawBase(false);
    m_chatTabs->setUsesScrollButtons(false);
    m_chatTabs->setElideMode(Qt::ElideRight);
    m_chatTabs->ensurePolished();
    for (int scope = 0; scope < m_chatTabs->count(); ++scope) {
        auto *badge = new QLabel(m_chatTabs);
        badge->setObjectName(QStringLiteral("chatScopeUnreadBadge"));
        badge->setAlignment(Qt::AlignCenter);
        badge->setAttribute(Qt::WA_TransparentForMouseEvents);
        badge->ensurePolished();
        const auto metrics = badge->fontMetrics();
        // QTabBar positions the button with its current size when it is installed.
        badge->setFixedSize(qMax(20, metrics.horizontalAdvance(QStringLiteral("99+")) + 8), metrics.height() + 2);
        m_chatTabs->setTabButton(scope, QTabBar::RightSide, badge);
        badge->hide();
    }
    const auto margins = sideLayout->contentsMargins();
    side->setMinimumWidth(qMax(196, m_chatTabs->minimumSizeHint().width() + margins.left() + margins.right()));
    sideLayout->insertWidget(0, m_chatTabs);
    connect(m_chatTabs, &QTabBar::currentChanged, this, &ChatPanel::onChatScopeChanged);
    m_chats->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_chats, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &point) { showChatPinMenu(m_chats, point); });
    connect(m_chats->verticalScrollBar(), &QScrollBar::valueChanged, this, &ChatPanel::avatarsRequested);
    auto *chatSearchRow = new QHBoxLayout;
    chatSearchRow->setSpacing(2);
    m_unreadOnlyButton = new ui::GlyphButton(ui::Glyph::Unread, QStringLiteral("Только непрочитанные чаты"), side);
    m_unreadOnlyButton->setCheckable(true);
    m_unreadOnlyButton->setToolTip(QStringLiteral("Только непрочитанные чаты"));
    chatSearchRow->addWidget(m_unreadOnlyButton);
    m_chatSearch = new QLineEdit(side);
    ui::editor(m_chatSearch);
    m_chatSearch->setAccessibleName(QStringLiteral("Поиск чатов"));
    m_chatSearch->setPlaceholderText(QStringLiteral("Поиск чатов"));
    chatSearchRow->addWidget(m_chatSearch, 1);
    m_chatSearchClear = new ui::GlyphButton(ui::Glyph::Close, QStringLiteral("Очистить поиск чатов"), side);
    chatSearchRow->addWidget(m_chatSearchClear);
    m_createGroupChat = ui::button(QStringLiteral("+"), QStringLiteral("Создать групповой чат"), side);
    m_createGroupChat->setToolTip(QStringLiteral("Создать групповой чат"));
    if (m_readOnly || m_backend->isDemo())
        m_createGroupChat->hide();
    connect(m_createGroupChat, &QPushButton::clicked, this, &ChatPanel::promptGroupChat);
    chatSearchRow->addWidget(m_createGroupChat);
    sideLayout->insertLayout(1, chatSearchRow);
    m_chatSearchProgress = label({}, "quiet", side);
    m_chatSearchProgress->setAccessibleName(QStringLiteral("Состояние поиска чатов"));
    m_chatSearchProgress->hide();
    sideLayout->insertWidget(2, m_chatSearchProgress);
    connect(m_chatSearch, &QLineEdit::returnPressed, this, &ChatPanel::runSearchChats);
    connect(m_chatSearch, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (text.trimmed().isEmpty())
            clearChatSearch();
        else
            m_chatSearchTimer->start();
    });
    connect(m_chatSearchClear, &QPushButton::clicked, this, &ChatPanel::clearChatSearch);
    connect(m_unreadOnlyButton, &QPushButton::toggled, this, [this](bool checked) {
        if (m_unreadOnlyChats == checked)
            return;
        m_unreadOnlyChats = checked;
        showChatResults(m_chatSource, m_chatSourceSearching);
    });
    splitter->addWidget(side);

    auto *reading = new QWidget(splitter);
    reading->setMinimumWidth(325);
    reading->setObjectName(QStringLiteral("reading"));
    auto *layout = new QVBoxLayout(reading);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *top = new QWidget(reading);
    top->setObjectName(QStringLiteral("conversationHeader"));
    top->setMinimumHeight(55);
    auto *header = new QHBoxLayout(top);
    header->setContentsMargins(12, 6, 8, 6);
    header->setSpacing(5);
    m_chatAvatar = new ui::AvatarBadge(top);
    m_chatAvatar->hide();
    header->addWidget(m_chatAvatar);
    auto *chatHeading = new QWidget(top);
    auto *chatHeadingLayout = new QVBoxLayout(chatHeading);
    chatHeadingLayout->setContentsMargins(4, 0, 0, 0);
    chatHeadingLayout->setSpacing(0);
    m_chatTitle = label(QStringLiteral("Чаты"), "conversationTitle", chatHeading);
    m_chatTitle->setAccessibleName(QStringLiteral("Открытый чат"));
    m_chatTitle->setMinimumWidth(0);
    m_chatTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    chatHeadingLayout->addWidget(m_chatTitle);
    m_chatContactInfo = label({}, "quiet", chatHeading);
    m_chatContactInfo->setAccessibleName(QStringLiteral("Статус и должность контакта"));
    m_chatContactInfo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_chatContactInfo->hide();
    chatHeadingLayout->addWidget(m_chatContactInfo);
    header->addWidget(chatHeading, 1);
    m_openChatTask = ui::button(QStringLiteral("Открыть задачу"), QStringLiteral("Открыть карточку задачи"), top);
    connect(m_openChatTask, &QPushButton::clicked, this, [this] { emit taskRequested(m_openChatTaskId); });
    m_openChatTask->hide();
    header->addWidget(m_openChatTask);
    auto *assets = ui::button(QStringLiteral("Вложения"), QStringLiteral("Медиа, файлы и ссылки чата"), top);
    connect(assets, &QPushButton::clicked, this, [this] { showChatAssets(m_currentChatId, window()); });
    header->addWidget(assets);
    m_messageSearchToggle = new ui::GlyphButton(ui::Glyph::Search, QStringLiteral("Найти в чате"), top);
    m_messageSearchToggle->setCheckable(true);
    header->addWidget(m_messageSearchToggle);
    auto *refresh = new ui::GlyphButton(ui::Glyph::Refresh, QStringLiteral("Обновить чат (F5)"), top);
    connect(refresh, &QPushButton::clicked, this, &ChatPanel::refreshRequested);
    header->addWidget(refresh);
    layout->addWidget(top);
    m_chatPinned = new ui::PinnedMessageBar(reading);
    m_chatPinned->setOnOpen([this](const QString &id) { openChatMessageContext(m_currentChatId, id); });
    layout->addWidget(m_chatPinned);
    m_latestChat = new QPushButton(QStringLiteral("К последним сообщениям"), reading);
    m_latestChat->setAccessibleName(QStringLiteral("К последним сообщениям"));
    m_latestChat->hide();
    connect(m_latestChat, &QPushButton::clicked, this, [this] {
        resetMessageContext();
        m_backend->requestChatMessages(m_currentChatId);
    });
    layout->addWidget(m_latestChat, 0, Qt::AlignRight);

    auto *searchBar = new QWidget(reading);
    searchBar->setObjectName(QStringLiteral("conversationSearch"));
    auto *messageSearchRow = new QHBoxLayout(searchBar);
    messageSearchRow->setContentsMargins(12, 3, 8, 3);
    messageSearchRow->setSpacing(5);
    m_messageSearch = new QLineEdit(searchBar);
    ui::editor(m_messageSearch);
    m_messageSearch->setAccessibleName(QStringLiteral("Поиск по сообщениям"));
    m_messageSearch->setPlaceholderText(QStringLiteral("Поиск по истории чата"));
    messageSearchRow->addWidget(m_messageSearch, 1);
    m_messageSearchClear = new ui::GlyphButton(ui::Glyph::Close, QStringLiteral("Очистить поиск по сообщениям"), searchBar);
    messageSearchRow->addWidget(m_messageSearchClear);
    layout->addWidget(searchBar);
    searchBar->hide();
    connect(m_messageSearchToggle, &QPushButton::toggled, this, [this, searchBar](bool show) {
        searchBar->setVisible(show);
        if (show)
            m_messageSearch->setFocus(Qt::ShortcutFocusReason);
        else
            clearMessageSearch();
    });
    auto *find = new QAction(reading);
    find->setShortcut(QKeySequence::Find);
    find->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(find, &QAction::triggered, this, [this] {
        m_messageSearchToggle->setChecked(true);
        m_messageSearch->setFocus(Qt::ShortcutFocusReason);
    });
    reading->addAction(find);
    auto *closeSearch = new QAction(searchBar);
    closeSearch->setShortcut(QKeySequence(Qt::Key_Escape));
    closeSearch->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(closeSearch, &QAction::triggered, this, [this] {
        m_messageSearchToggle->setChecked(false);
        m_messageSearchToggle->setFocus(Qt::ShortcutFocusReason);
    });
    searchBar->addAction(closeSearch);
    connect(m_messageSearch, &QLineEdit::returnPressed, this, &ChatPanel::runMessageSearch);
    connect(m_messageSearch, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (text.trimmed().isEmpty())
            clearMessageSearch();
        else
            m_messageSearchTimer->start();
    });
    connect(m_messageSearchClear, &QPushButton::clicked, this, &ChatPanel::clearMessageSearch);

    auto *canvas = new ui::ConversationCanvas(reading);
    auto *timeline = new QVBoxLayout(canvas);
    timeline->setContentsMargins(12, 5, 12, 8);
    timeline->setSpacing(0);
    m_chatMessages = new ui::HistoryList(canvas);
    static_cast<ui::HistoryList *>(m_chatMessages)->setOnViewportChanged([this] { scheduleVisibleChatRead(); });
    m_chatMessages->setAccessibleName(QStringLiteral("Сообщения чата"));
    workspace::configureRows(m_chatMessages, workspace::Row::Message);
    m_workspace.bindHistory(m_chatMessages);
    static_cast<workspace::RowPainter *>(m_chatMessages->itemDelegate())->setMessageViewersHandler(
        [this](const QString &id, const QPoint &anchor) { openMessageViewers(id, anchor); });
    connect(m_chatMessages->verticalScrollBar(), &QScrollBar::valueChanged, this, &ChatPanel::avatarsRequested);
    auto *history = static_cast<ui::HistoryList *>(m_chatMessages);
    history->setOnBrowsingOlder([this](bool active) {
        m_latestChat->setVisible(active || !m_contextDialogId.isEmpty());
    });
    timeline->addWidget(history->olderButton(canvas), 0, Qt::AlignHCenter);
    timeline->addWidget(m_chatMessages, 1);
    history->setOnOlder([this](const QString &before) {
        if (m_active && !m_currentChatId.isEmpty() && !m_messageSearchActive)
            m_backend->requestOlderChatMessages(m_currentChatId, before, false);
    });
    m_chatEmpty = emptyState(QStringLiteral("Загружаем чаты…"), canvas);
    ui::setLoadingIndicator(m_chatEmpty, true);
    timeline->addWidget(m_chatEmpty, 1);
    m_chatMessages->hide();
    layout->addWidget(canvas, 1);

    m_chatComposer = new ui::ChatComposer(reading, QStringLiteral("Текст сообщения"),
        QStringLiteral("Эмодзи в чате"), QStringLiteral("Прикрепить файл в чат"), QStringLiteral("Отправить сообщение"));
    m_chatInput = m_chatComposer->input();
    m_chatComposer->setEditLatest([this, history] { return editLatestMessage(history, m_currentChatId); });
    m_chatComposer->setTypingActions([this] {
        return writable() && m_active && isActiveWindow() ? m_currentChatId : QString();
    }, [this](const QString &dialog) { m_backend->notifyChatTyping(dialog); });
    history->setComposer(m_chatInput);
    history->setReplyActions([this](const ui::ChatReply &reply) {
        m_chatComposer->setReply(reply);
        m_chatInput->setFocus(Qt::OtherFocusReason);
    }, [this] { return writable() && !m_currentChatId.isEmpty(); },
       [this](const QString &id) { openChatMessageContext(m_currentChatId, id); });
    history->setMessageMenu([this](QMenu &menu, const QPersistentModelIndex &index, const QString &href) {
        addMessageActions(menu, index, m_currentChatId, m_chatScope == 1, href);
    });
    history->setReactionActions([this](const QModelIndex &index, const QString &code, const QPoint &anchor) {
        actOnMessageReaction(index, m_currentChatId, code, anchor, window());
    }, [this] { return writable() && !m_backend->isDemo() && !m_currentChatId.isEmpty(); });
    m_chatEmoji = m_chatComposer->emoji();
    m_chatFile = m_chatComposer->file();
    m_chatSend = m_chatComposer->send();
    if (m_readOnly)
        m_chatComposer->setPlaceholderText(QStringLiteral("Режим чтения"));
    connect(m_chatFile, &QPushButton::clicked, this, [this] { m_workspace.pickFile(m_currentChatId, QStringLiteral("chat")); });
    connect(m_chatSend, &QPushButton::clicked, this, &ChatPanel::onSendChat);
    connect(m_chatInput, &QPlainTextEdit::textChanged, this, &ChatPanel::updateComposers);
    m_chatComposer->setEditStateChanged([this] { updateComposers(); });
    m_chatComposer->setImageDraftHandlers([this] { updateComposers(); },
                                          [this](const QString &error) { emit statusChanged(error); });
    m_chatComposer->setMentionSearchHandler([this](const QString &id, const QString &query) {
        if (!m_currentChatId.isEmpty())
            m_backend->searchPeople(id, query, m_currentChatId);
        else
            m_chatComposer->setMentionResults(id, {});
    });
    m_chatComposer->setMentionAvatars([this](const Person &person) {
        return ui::requisiteAvatarIcon([this](const QUrl &url) { return m_workspace.avatar(url); },
                                       person.name, person.avatarUrl, workspace::AvatarKind::Person);
    }, [this] { emit avatarsRequested(); });
    m_chatInput->installEventFilter(this);
    layout->addWidget(m_chatComposer);
    m_chatComposer->hide();
    splitter->addWidget(reading);
    splitter->setStretchFactor(1, 1);
    connect(splitter, &QSplitter::splitterMoved, this, &ChatPanel::avatarsRequested);
    connectBackend();
}

void ChatPanel::onChatSelected()
{
    const auto *item = m_chats->currentItem();
    if (!item)
        return;
    const auto id = item->data(workspace::Id).toString();
    const bool leftRetainedTask = !m_retainedTaskChat.id.isEmpty() && id != m_retainedTaskChat.id;
    openChatRow(*item);
    if (leftRetainedTask) {
        QTimer::singleShot(0, this, [this, id] {
            if (m_active && m_chatScope == 1 && m_currentChatId == id
                && m_chatDirectories[1].query.isEmpty()
                && !m_retainedTaskChat.id.isEmpty() && id != m_retainedTaskChat.id)
                showChatResults(m_chatDirectories[1].recent, true, true);
        });
    }
}

void ChatPanel::focusChatComposer()
{
    if (!m_currentChatId.isEmpty() && m_chatInput->isVisible() && !m_chatInput->isReadOnly())
        m_chatInput->setFocus(Qt::OtherFocusReason);
}

void ChatPanel::scheduleVisibleChatRead()
{
    if (m_visibleChatReadTimer)
        m_visibleChatReadTimer->start();
}

void ChatPanel::reportVisibleChatRead()
{
    if (!writable())
        return;
    const auto *active = QApplication::activeWindow();
    const auto *modal = QApplication::activeModalWidget();
    if (!active || active->isMinimized() || QApplication::activePopupWidget() || (modal && modal != active))
        return;
    if (m_workspace.readActiveTask(active))
        return;
    if (active != window() || !m_active || m_currentChatId.isEmpty() ||
        m_messageSearchActive || !m_contextDialogId.isEmpty())
        return;
    const auto seen = static_cast<ui::HistoryList *>(m_chatMessages)->visibleMessageIds();
    if (!seen.all.isEmpty())
        m_backend->readChatMessages(m_currentChatId, seen.incoming, seen.all);
}

void ChatPanel::updateChatContactPresentation()
{
    const auto now = QDateTime::currentDateTimeUtc();
    const bool known = m_chatContactIsPerson && m_chatContactUntil.isValid();
    const bool online = known && m_chatContactUntil > now;
    QStringList details;
    if (known)
        details.append(online ? QStringLiteral("В сети") : QStringLiteral("Не в сети"));
    if (m_chatContactIsPerson && !m_chatContactPosition.trimmed().isEmpty())
        details.append(m_chatContactPosition.simplified());
    const auto text = details.join(QStringLiteral(" · "));
    ui::setStatusLabel(m_chatContactInfo, text);
    m_chatContactInfo->setToolTip(ui::plainTooltip(text));
    m_chatAvatar->setOnline(online);
    m_chats->viewport()->update();
    m_chatContactTimer->stop();
    QDateTime next = online ? m_chatContactUntil : QDateTime();
    for (int i = 0; i < m_chats->count(); ++i) {
        const auto expiry = m_chats->item(i)->data(workspace::ContactOnlineUntil).toDateTime();
        if (expiry.isValid() && expiry > now && (!next.isValid() || expiry < next))
            next = expiry;
    }
    if (next.isValid())
        m_chatContactTimer->start(int(qBound(qint64(1), now.msecsTo(next) + 10, qint64(900000))));
}

void ChatPanel::openChatRow(const QListWidgetItem &item)
{
    const auto chatId = item.data(workspace::Id).toString();
    if (chatId.isEmpty())
        return;
    const auto name = item.data(workspace::Title).toString();
    m_chatTitle->setText(name);
    m_chatTitle->setToolTip(ui::plainTooltip(name));
    const auto avatar = workspace::AvatarKind(item.data(workspace::AvatarType).toInt());
    m_chatAvatar->setTitle(name, avatar);
    m_chatContactIsPerson = m_chatScope == 0 && avatar == workspace::AvatarKind::Person;
    m_chatContactUntil = item.data(workspace::ContactOnlineUntil).toDateTime();
    m_chatContactPosition = item.data(workspace::ContactPosition).toString();
    updateChatContactPresentation();
    m_chatAvatarUrl = item.data(workspace::AvatarUrl).toUrl();
    if (const auto *photo = m_workspace.avatar(m_chatAvatarUrl))
        m_chatAvatar->setPhoto(*photo);
    const auto taskId = item.data(workspace::TaskId).toString();
    const bool taskChanged = m_openChatTaskId != taskId;
    m_openChatTaskId = taskId;
    m_openChatTask->setVisible(!m_openChatTaskId.isEmpty());
    m_chatDirectories[m_chatScope].selected = chatId;
    emit avatarsRequested();
    if (chatId == m_currentChatId) {
        if (taskChanged)
            m_workspace.previews->scheduleLinkPreviewLayout();
        return;
    }
    emit dialogChanged();
    m_workspace.previews->resetLinkPreviews();
    m_chatComposer->dismissEmoji();
    switchDraft(m_chatComposer, m_chatDrafts, m_currentChatId, chatId);
    m_messageSearchTimer->stop();
    {
        const QSignalBlocker blocker(m_messageSearch);
        m_messageSearch->clear();
    }
    m_messageSearchActive = false;
    m_messageSearchQuery.clear();
    m_messageSearchScope.clear();
    m_currentChatId = chatId;
    static_cast<ui::HistoryList *>(m_chatMessages)->resetHistory(chatId);
    m_chatPinned->setMessages({});
    if (m_contextDialogId != chatId)
        resetMessageContext();
    m_chatComposer->show();
    showEmpty(m_chatMessages, m_chatEmpty, QStringLiteral("Загружаем сообщения…"), true);
    emit statusChanged(QString());
    updateComposers();
    if (m_contextDialogId != chatId)
        m_backend->requestChatMessages(chatId);
    m_backend->requestPinnedMessages(chatId);
}

bool ChatPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (m_chats && watched == m_chats->viewport()) {
        const auto type = event->type();
        if (m_active && m_chatScope == 1 && m_chatDirectories[1].query.isEmpty()
            && (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick)) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            const auto *row = m_chats->itemAt(mouse->position().toPoint());
            if (mouse->button() == Qt::LeftButton && row && !row->data(workspace::Id).toString().isEmpty()) {
                m_taskChatPressActive = true;
                m_taskChatPressEnd->stop();
            }
        } else if (m_taskChatPressActive && (type == QEvent::UngrabMouse || type == QEvent::Hide
                   || (type == QEvent::Leave && !QGuiApplication::mouseButtons().testFlag(Qt::LeftButton))
                   || (type == QEvent::MouseButtonRelease && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton))) {
            // Let QListWidget process the release and emit itemClicked before rebuilding its model.
            m_taskChatPressEnd->start(0);
        } else if (type == QEvent::Show && m_taskChatRefreshDeferred) {
            m_taskChatPressEnd->start(0);
        }
    }
    if (event->type() == QEvent::KeyPress && watched == m_chatInput && !m_chatComposer->composing()) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (m_chatComposer->handleComposerKey(key))
            return true;
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
            !(key->modifiers() & Qt::ShiftModifier)) {
            onSendChat();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ChatPanel::onSendChat()
{
    if (m_chatComposer->submitMessageEdit())
        return;
    m_workspace.sendFromComposer(m_chatSendController, m_chatComposer, m_currentChatId);
}

void ChatPanel::updateComposers()
{
    const auto wire = m_chatComposer->wireText();
    const auto text = wire.trimmed();
    const bool enabled = writable() && !m_currentChatId.isEmpty();
    const bool editing = m_chatComposer->editingMessage();
    const bool saving = m_chatComposer->messageEditPending();
    const bool hasImage = m_chatComposer->hasDraftImage();
    const bool sending = m_chatSendController.pending() || m_workspace.pastedImagePending(m_currentChatId, false);
    m_chatInput->setReadOnly(m_readOnly || m_currentChatId.isEmpty() || saving);
    const bool canSubmit = editing ? m_chatComposer->canSaveMessageEdit(wire) : !sending
        && text.size() <= ui::kMessageCharacters && (hasImage ? !m_workspace.uploadPending() : !text.isEmpty());
    m_chatSend->setEnabled(enabled && canSubmit);
    m_chatSend->setToolTip(editing ? (saving ? QStringLiteral("Сохраняем сообщение…") : QStringLiteral("Сохранить сообщение"))
        : text.size() > ui::kMessageCharacters ? QStringLiteral("Максимум 20 000 символов")
        : sending || (hasImage && m_workspace.uploadPending()) ? QStringLiteral("Отправка…")
        : QStringLiteral("Отправить сообщение"));
    m_chatEmoji->setEnabled(enabled && !saving);
    m_chatFile->setEnabled(enabled && !editing && !m_workspace.uploadPending());
}

void ChatPanel::onChatListLoaded(int scope, const QVector<Chat> &chats)
{
    auto &directory = m_chatDirectories[scope];
    directory.recent = chats;
    directory.loaded = true;
    updateChatUnreadBadges();
    if (m_active && scope == m_chatScope && directory.query.isEmpty())
        showChatResults(chats, true, true);
    if (scope == 0 && m_active && m_chatScope == 0 && !m_taskChatBadgesRequested
        && !m_chatDirectories[1].loaded && m_workspace.sessionReady()) {
        m_taskChatBadgesRequested = true;
        m_backend->requestTaskChatBadges();
    }
}

void ChatPanel::updateChatUnreadBadges()
{
    QHash<QString, qint64> serverIds;
    for (const auto &directory : m_chatDirectories) {
        for (const auto &chat : directory.recent) {
            if (!chat.id.isEmpty() && chat.serverChatId > 0)
                serverIds.insert(chat.id, chat.serverChatId);
        }
    }
    struct Counter {
        int unread = 0;
        int source = -1;
    };
    QHash<QString, Counter> counters;
    for (int scope = 0; scope < 2; ++scope) {
        for (const auto &chat : m_chatDirectories[scope].recent) {
            if (chat.id.isEmpty())
                continue;
            auto id = serverIds.value(chat.id);
            if (id <= 0 && chat.id.startsWith(QLatin1String("chat")))
                id = portalTaskIdentifier(chat.id.mid(4)).toLongLong();
            const auto key = id > 0 ? QStringLiteral("server:%1").arg(id) : QStringLiteral("dialog:") + chat.id;
            // A row from the dedicated task list supersedes older copies in the ordinary list.
            const int source = scope == 1 ? 2 : int(chat.taskChat || !chat.taskId.isEmpty());
            const auto unread = qMax(chat.unread, int(chat.markedUnread));
            auto &counter = counters[key];
            if (source > counter.source)
                counter = {unread, source};
            else if (source == counter.source)
                counter.unread = qMax(counter.unread, unread);
        }
    }
    qint64 counts[2]{};
    for (const auto &counter : counters)
        counts[counter.source > 0 ? 1 : 0] += counter.unread;
    for (int scope = 0; scope < 2; ++scope)
        setChatTabUnreadCount(scope, counts[scope]);
    emit unreadCountChanged(counts[0] + counts[1]);
}

void ChatPanel::handleChatTyping(const QString &dialog, const QString &user, int duration)
{
    auto *card = m_workspace.card();
    auto *task = card && card->isVisible() && card->taskChatDialogId() == dialog ? card->composer() : nullptr;
    auto *chat = m_active && m_currentChatId == dialog && m_chatComposer->isVisible()
        ? m_chatComposer : nullptr;
    if (!task && !chat)
        return;

    const auto id = portalTaskIdentifier(user);
    const auto &writer = id.isEmpty() ? user : id;
    QString name;
    const QString *cached = nullptr;
    if (duration > 0 && !id.isEmpty()) {
        const auto &recent = m_chatDirectories[0].recent;
        const auto contact = std::find_if(recent.cbegin(), recent.cend(), [&id](const Chat &entry) {
            return entry.id == id && !entry.taskChat && !entry.title.trimmed().isEmpty();
        });
        if (contact != recent.cend())
            name = contact->title.simplified().left(96);
        if (name.isEmpty()) {
            cached = m_typingNames.object(id);
            if (cached)
                name = *cached;
        }
    }
    for (auto *composer : {task, chat})
        if (composer)
            composer->setTypingIndicator(writer, duration, name);

    if (duration > 0 && !id.isEmpty() && name.isEmpty() && !cached && !window()->isMinimized()
        && m_typingNamesPending.size() < 8 && !m_typingNamesPending.contains(id)) {
        m_typingNamesPending.insert(id);
        m_backend->requestPerson(id);
    }
}

void ChatPanel::rememberTypingName(const QString &user, const QString &name)
{
    if (!m_typingNamesPending.remove(user))
        return;
    const auto display = name.simplified().left(96);
    m_typingNames.insert(user, new QString(display));
    if (auto *card = m_workspace.card(); card && card->isVisible())
        card->composer()->setTypingName(user, display);
    if (m_active && m_chatComposer->isVisible())
        m_chatComposer->setTypingName(user, display);
}

void ChatPanel::setChatTabUnreadCount(int scope, qint64 count)
{
    if (m_chatTabs->tabData(scope).toLongLong() == count)
        return;
    m_chatTabs->setTabData(scope, count);
    auto *badge = static_cast<QLabel *>(m_chatTabs->tabButton(scope, QTabBar::RightSide));
    if (count == 0) {
        badge->hide();
        badge->clear();
        badge->setAccessibleName({});
        m_chatTabs->setTabToolTip(scope, {});
        return;
    }
    const auto detail = QStringLiteral("Непрочитанных сообщений · %1").arg(count);
    badge->setText(count > 99 ? QStringLiteral("99+") : QString::number(count));
    badge->setAccessibleName(detail);
    badge->show();
    m_chatTabs->setTabToolTip(scope, m_chatTabs->tabText(scope) + QStringLiteral(" · ") + detail);
}

void ChatPanel::onChatsFound(int scope, const QString &query, const QVector<Chat> &chats)
{
    auto &directory = m_chatDirectories[scope];
    if (query.isEmpty() || query != directory.query)
        return;
    directory.found = chats;
    if (m_active && scope == m_chatScope && query == m_chatSearch->text().trimmed()) {
        ui::setStatusLabel(m_chatSearchProgress, {});
        showChatResults(chats);
    }
}

void ChatPanel::requestCurrentChatList()
{
    if (!m_chatDirectories[m_chatScope].loaded) {
        m_chats->clear();
        workspace::loadingRow(m_chats, QStringLiteral("Загружаем чаты…"));
    }
    if (m_chatScope == 0)
        m_backend->requestRecentChats();
    else
        m_backend->requestTaskChats();
}

void ChatPanel::closeCurrentChat(bool scopeChanged)
{
    m_chatComposer->dismissEmoji();
    switchDraft(m_chatComposer, m_chatDrafts, m_currentChatId, {});
    if (scopeChanged || !m_currentChatId.isEmpty())
        emit historyClosed(false);
    m_currentChatId.clear();
    static_cast<ui::HistoryList *>(m_chatMessages)->resetHistory();
    resetMessageContext();
    clearMessageSearch();
    m_chatTitle->setText(m_chatTabs->tabText(m_chatScope));
    m_chatTitle->setToolTip({});
    m_chatAvatar->setTitle({});
    m_chatContactIsPerson = false;
    updateChatContactPresentation();
    m_chatPinned->setMessages({});
    m_chatAvatarUrl.clear();
    m_openChatTaskId.clear();
    m_openChatTask->hide();
    m_chatComposer->hide();
    showEmpty(m_chatMessages, m_chatEmpty, QStringLiteral("Выберите чат"));
    updateComposers();
}

void ChatPanel::onChatScopeChanged(int scope)
{
    if (scope < 0 || scope > 1 || scope == m_chatScope)
        return;
    m_chatSearchTimer->stop();
    ui::setStatusLabel(m_chatSearchProgress, {});
    m_chatDirectories[m_chatScope].input = m_chatSearch->text();
    m_chatScope = scope;
    closeCurrentChat(true);
    auto &directory = m_chatDirectories[scope];
    {
        const QSignalBlocker blocker(m_chatSearch);
        m_chatSearch->setText(directory.input);
    }
    const auto searchLabel = scope == 0 ? QStringLiteral("Поиск чатов") : QStringLiteral("Поиск чатов задач");
    m_chatSearch->setAccessibleName(searchLabel);
    m_chatSearch->setPlaceholderText(scope == 0 ? searchLabel : QStringLiteral("Название или номер задачи"));
    m_createGroupChat->setVisible(scope == 0 && !m_readOnly && !m_backend->isDemo());
    m_chats->setAccessibleName(scope == 0 ? QStringLiteral("Недавние чаты") : QStringLiteral("Чаты задач"));
    emit statusChanged({});
    if (!directory.query.isEmpty()) {
        const auto &initial = std::any_of(directory.found.cbegin(), directory.found.cend(), [&directory](const Chat &chat) {
            return chat.id == directory.selected;
        }) ? directory.found : directory.recent;
        showChatResults(initial, false);
    }
    showChatResults(directory.query.isEmpty() ? directory.recent : directory.found);
    if (!m_chatSearch->text().trimmed().isEmpty())
        runSearchChats();
    else
        requestCurrentChatList();
}

void ChatPanel::runSearchChats()
{
    m_chatSearchTimer->stop();
    if (!m_active)
        return;
    const auto query = m_chatSearch->text().trimmed();
    if (query.isEmpty()) {
        clearChatSearch();
        return;
    }
    auto &directory = m_chatDirectories[m_chatScope];
    if (directory.query != query)
        directory.found.clear();
    directory.query = query;
    ui::setStatusLabel(m_chatSearchProgress, m_chatScope == 0 ? QStringLiteral("Ищем чаты…") : QStringLiteral("Ищем задачи…"), true);
    if (m_chatScope == 0)
        m_backend->searchChats(query);
    else
        m_backend->searchTaskChats(query);
}

void ChatPanel::clearChatSearch()
{
    m_chatSearchTimer->stop();
    ui::setStatusLabel(m_chatSearchProgress, {});
    auto &directory = m_chatDirectories[m_chatScope];
    const bool hadSearch = !directory.query.isEmpty() || !m_chatSearch->text().isEmpty();
    {
        const QSignalBlocker blocker(m_chatSearch);
        m_chatSearch->clear();
    }
    directory.query.clear();
    directory.found.clear();
    if (hadSearch) {
        showChatResults(directory.recent);
        requestCurrentChatList();
    }
}

void ChatPanel::runMessageSearch()
{
    m_messageSearchTimer->stop();
    if (!m_active)
        return;
    const auto query = m_messageSearch->text().trimmed();
    if (query.isEmpty()) {
        clearMessageSearch();
        return;
    }
    if (m_currentChatId.isEmpty()) {
        emit statusChanged(QStringLiteral("Откройте чат для поиска по истории"));
        return;
    }
    resetMessageContext();
    m_messageSearchQuery = query;
    m_messageSearchScope = m_currentChatId;
    m_messageSearchActive = true;
    static_cast<ui::HistoryList *>(m_chatMessages)->setLiveReadHistory(false);
    showEmpty(m_chatMessages, m_chatEmpty, QStringLiteral("Ищем сообщения…"), true);
    m_historySearch.request(m_currentChatId, query);
}

void ChatPanel::clearMessageSearch()
{
    m_messageSearchTimer->stop();
    const bool hadSearch = m_messageSearchActive || !m_messageSearch->text().isEmpty();
    {
        const QSignalBlocker blocker(m_messageSearch);
        m_messageSearch->clear();
    }
    m_messageSearchQuery.clear();
    m_messageSearchScope.clear();
    m_messageSearchActive = false;
    if (hadSearch && !m_currentChatId.isEmpty()) {
        showEmpty(m_chatMessages, m_chatEmpty, QStringLiteral("Загружаем сообщения…"), true);
        m_backend->requestChatMessages(m_currentChatId);
    }
}

void ChatPanel::showChatResults(const QVector<Chat> &chats, bool useSearchMode, bool backgroundRefresh)
{
    const auto &directory = m_chatDirectories[m_chatScope];
    const bool searching = useSearchMode && !directory.query.isEmpty();
    const bool taskRecent = m_chatScope == 1 && !searching;
    if (m_taskChatPressActive && taskRecent && backgroundRefresh) {
        m_taskChatRefreshDeferred = true;
        return;
    }
    m_taskChatPressActive = false;
    m_taskChatRefreshDeferred = false;
    const bool preserveViewport = taskRecent && backgroundRefresh
        && (!m_chatSource.isEmpty() || !m_retainedTaskChat.id.isEmpty());
    const auto anchor = preserveViewport ? listViewportAnchor(m_chats) : ListViewportAnchor{};
    const auto initialId = m_currentChatId.isEmpty() ? directory.selected : m_currentChatId;
    const auto *current = m_chats->currentItem();
    const int previousRow = preserveViewport && current && !m_currentChatId.isEmpty()
        && current->data(workspace::Id).toString() == m_currentChatId ? m_chats->row(current) : -1;
    const bool wasRetained = previousRow >= 0 && m_retainedTaskChat.id == initialId;
    const int retainedRow = wasRetained ? m_retainedTaskChatRow : previousRow;
    const auto selectedRect = previousRow >= 0 ? m_chats->visualItemRect(current) : QRect{};
    Chat previousTask;
    if (previousRow >= 0) {
        const auto previous = std::find_if(m_chatSource.cbegin(), m_chatSource.cend(), [&initialId](const Chat &chat) {
            return chat.taskChat && chat.id == initialId;
        });
        if (previous != m_chatSource.cend())
            previousTask = *previous;
        else if (wasRetained)
            previousTask = m_retainedTaskChat;
    }
    m_retainedTaskChat = {};
    m_retainedTaskChatRow = -1;
    auto ordered = chats;
    m_chatSource = chats;
    m_chatSourceSearching = useSearchMode;
    if (!searching && !directory.linked.id.isEmpty() &&
        std::none_of(ordered.cbegin(), ordered.cend(), [&directory](const Chat &chat) {
            return chat.id == directory.linked.id;
        })) {
        ordered.prepend(directory.linked);
    }
    const auto hidden = [this](const Chat &chat) {
        return !m_showDismissedChats && chat.dismissed && !chat.taskChat;
    };
    Chat hiddenLinked;
    if (!searching && !initialId.isEmpty() && initialId == directory.linked.id) {
        const auto match = std::find_if(ordered.cbegin(), ordered.cend(), [&initialId, &hidden](const Chat &chat) {
            return chat.id == initialId && hidden(chat);
        });
        if (match != ordered.cend())
            hiddenLinked = *match;
    }
    ordered.erase(std::remove_if(ordered.begin(), ordered.end(), hidden), ordered.end());
    if (m_unreadOnlyChats) {
        ordered.erase(std::remove_if(ordered.begin(), ordered.end(), [&previousTask](const Chat &chat) {
            return chat.unread <= 0 && !chat.markedUnread && !(chat.taskChat && chat.id == previousTask.id);
        }), ordered.end());
    }
    const auto rank = [](const Chat &chat) {
        if (chat.pinned)
            return 0;
        return chat.unread > 0 || chat.markedUnread ? 1 : 2;
    };
    std::stable_sort(ordered.begin(), ordered.end(), [&rank](const Chat &a, const Chat &b) {
        return rank(a) < rank(b);
    });
    if (!previousTask.id.isEmpty()) {
        const auto active = std::find_if(ordered.begin(), ordered.end(), [&initialId](const Chat &chat) {
            return chat.id == initialId;
        });
        if (active == ordered.end() || active->taskChat) {
            // Hold the row for the lifetime of the selection, independently of its changing badges.
            m_retainedTaskChat = active == ordered.end() ? previousTask : *active;
            m_retainedTaskChatRow = retainedRow;
            if (active != ordered.end())
                ordered.erase(active);
            const qsizetype pinnedCount = std::partition_point(ordered.cbegin(), ordered.cend(),
                [](const Chat &entry) { return entry.pinned; }) - ordered.cbegin();
            const auto first = m_retainedTaskChat.pinned ? qsizetype(0) : pinnedCount;
            const auto last = m_retainedTaskChat.pinned ? pinnedCount : ordered.size();
            ordered.insert(qBound(first, qsizetype(retainedRow), last), m_retainedTaskChat);
        }
    }
    {
        const QSignalBlocker blocker(m_chats);
        const QSignalBlocker scrollBlocker(preserveViewport ? m_chats->verticalScrollBar() : nullptr);
        m_chats->clear();
        QListWidgetItem *selected = nullptr;
        for (const auto &chat : ordered) {
            auto *item = ui::addChatRow(m_chats, chat);
            if (chat.id == initialId)
                selected = item;
        }
        if (searching || (m_unreadOnlyChats && !m_currentChatId.isEmpty()) || !hiddenLinked.id.isEmpty()) {
            if (selected)
                m_chats->setCurrentItem(selected);
        } else {
            selectAvailable(m_chats, selected);
        }
        if (ordered.isEmpty()) {
            emit directoryEmpty();
            auto text = m_chatScope == 0 ? QStringLiteral("Чатов пока нет") : QStringLiteral("Чатов задач пока нет");
            if (searching)
                text = QStringLiteral("Ничего не найдено");
            else if (m_unreadOnlyChats)
                text = QStringLiteral("Непрочитанных чатов пока нет");
            else if (!directory.loaded)
                text = QStringLiteral("Загружаем чаты…");
            if (!searching && !m_unreadOnlyChats && !directory.loaded)
                workspace::loadingRow(m_chats, text);
            else
                workspace::emptyRow(m_chats, text);
        }
        if (preserveViewport) {
            if (selected && !m_retainedTaskChat.id.isEmpty() && m_chats->viewport()->rect().intersects(selectedRect))
                restoreListViewport(m_chats, {{initialId}, selectedRect.top(), anchor.scroll});
            else
                restoreListViewport(m_chats, anchor);
        }
    }
    if (m_chats->currentItem() && !ordered.isEmpty())
        onChatSelected();
    else if (!hiddenLinked.id.isEmpty()) {
        // Explicit notification links can open a hidden chat without listing it.
        const std::unique_ptr<QListWidgetItem> item(ui::addChatRow(nullptr, hiddenLinked));
        openChatRow(*item);
    } else if (!searching && !(m_unreadOnlyChats && !m_currentChatId.isEmpty())) {
        closeCurrentChat();
    }
    emit avatarRowsChanged();
    emit avatarsRequested();
}

void ChatPanel::showChatPinMenu(QListWidget *list, const QPoint &point)
{
    const auto *item = list->itemAt(point);
    if (!item)
        return;
    const auto id = item->data(workspace::Id).toString();
    if (id.isEmpty())
        return;
    const auto op = operationKey(OperationKind::ChatPin, id);
    const bool pinned = item->data(workspace::Pinned).toBool();
    auto *menu = ui::directoryPinMenu(window(), pinned, [this, op] {
        return writable() && !m_pendingPins.contains(op);
    }, [this, op, id, pinned] {
        emit statusChanged(QString());
        m_pendingPins.insert(op);
        m_backend->setChatPinned(id, !pinned);
    });
    if (!m_backend->isDemo()) {
        connect(menu->addAction(QStringLiteral("Медиа, файлы и ссылки")), &QAction::triggered,
                this, [this, id] { showChatAssets(id, window()); });
    }
    if (m_chatScope == 0 && id.startsWith(QLatin1String("chat")) && !m_backend->isDemo()) {
        menu->addSeparator();
        connect(menu->addAction(QStringLiteral("Добавить участника")), &QAction::triggered,
                this, [this, id] { promptChatMember(id, true); });
        connect(menu->addAction(QStringLiteral("Удалить участника")), &QAction::triggered,
                this, [this, id] { promptChatMember(id, false); });
    }
    menu->popup(list->viewport()->mapToGlobal(point));
}

void ChatPanel::openLinkedChat(const NotificationTarget &target)
{
    int scope = target.taskChat ? 1 : 0;
    Chat linkedChat;
    for (int candidate = 0; candidate < 2; ++candidate) {
        const auto &directory = m_chatDirectories[candidate];
        for (const auto *list : {&directory.recent, &directory.found}) {
            const auto match = std::find_if(list->cbegin(), list->cend(), [&target](const Chat &chat) {
                return chat.id == target.id;
            });
            if (match != list->cend()) {
                scope = candidate;
                linkedChat = *match;
            }
        }
    }
    const bool sectionWillReloadChat = !m_active && m_chatScope == scope &&
        m_currentChatId == target.id && !m_messageSearchActive && m_contextDialogId.isEmpty();
    emit sectionRequested();
    m_chatTabs->setCurrentIndex(scope);
    auto &directory = m_chatDirectories[scope];
    m_chatSearchTimer->stop();
    m_messageSearchTimer->stop();
    if (m_messageSearchActive)
        showEmpty(m_chatMessages, m_chatEmpty, QStringLiteral("Загружаем сообщение…"), true);
    {
        const QSignalBlocker blocker(m_messageSearch);
        m_messageSearch->clear();
    }
    m_messageSearchActive = false;
    m_messageSearchQuery.clear();
    m_messageSearchScope.clear();
    {
        const QSignalBlocker blocker(m_chatSearch);
        m_chatSearch->clear();
    }
    directory.input.clear();
    directory.query.clear();
    directory.found.clear();
    ui::setStatusLabel(m_chatSearchProgress, {});
    if (linkedChat.id.isEmpty()) {
        linkedChat.id = target.id;
        linkedChat.title = QStringLiteral("Диалог %1").arg(target.id);
        linkedChat.taskChat = scope == 1;
    }
    directory.linked = linkedChat;
    if (target.messageId.isEmpty()) {
        resetMessageContext();
    } else if (m_currentChatId != target.id) {
        m_contextDialogId = target.id;
        m_contextMessageId = target.messageId;
    }
    const auto oldChat = m_currentChatId;
    if (oldChat != target.id) {
        switchDraft(m_chatComposer, m_chatDrafts, oldChat, {});
        m_currentChatId.clear();
        static_cast<ui::HistoryList *>(m_chatMessages)->resetHistory();
    }
    directory.selected = target.id;
    showChatResults(directory.recent);
    if (target.messageId.isEmpty()) {
        if (oldChat == target.id && !sectionWillReloadChat)
            m_backend->requestChatMessages(target.id);
        focusChatComposer();
        return;
    }
    openChatMessageContext(target.id, target.messageId);
}

void ChatPanel::openChatMessageContext(const QString &dialogId, const QString &messageId)
{
    const auto target = portalTaskIdentifier(messageId);
    if (!m_active || dialogId.isEmpty() || dialogId != m_currentChatId || target.isEmpty())
        return;
    const bool searching = m_messageSearchActive;
    m_messageSearchTimer->stop();
    {
        const QSignalBlocker search(m_messageSearch);
        const QSignalBlocker toggle(m_messageSearchToggle);
        m_messageSearch->clear();
        m_messageSearchToggle->setChecked(false);
        m_messageSearch->parentWidget()->hide();
    }
    m_messageSearchActive = false;
    m_messageSearchQuery.clear();
    m_messageSearchScope.clear();
    if (!searching && focusChatMessage(target))
        return;
    m_contextDialogId = dialogId;
    m_contextMessageId = target;
    m_latestChat->show();
    static_cast<ui::HistoryList *>(m_chatMessages)->setLiveReadHistory(false);
    showEmpty(m_chatMessages, m_chatEmpty, QStringLiteral("Загружаем сообщение…"), true);
    m_backend->requestMessageContext(dialogId, target);
}

void ChatPanel::resetMessageContext()
{
    m_contextDialogId.clear();
    m_contextMessageId.clear();
    if (m_chatMessages)
        static_cast<ui::HistoryList *>(m_chatMessages)->returnToLatest();
    if (m_latestChat)
        m_latestChat->hide();
}

bool ChatPanel::focusChatMessage(const QString &id)
{
    return static_cast<ui::HistoryList *>(m_chatMessages)->focusOnMessage(id);
}

void ChatPanel::showMessageResults(const QVector<Message> &messages)
{
    static_cast<ui::HistoryList *>(m_chatMessages)->setLiveReadHistory(false);
    presentMessages(m_chatMessages, m_chatEmpty, messages, ui::MessageView::Search);
    emit avatarRowsChanged();
    emit avatarsRequested();
}

void ChatPanel::setShowDismissedChats(bool show)
{
    if (m_showDismissedChats == show)
        return;
    m_showDismissedChats = show;
    if (m_active) {
        const auto &directory = m_chatDirectories[m_chatScope];
        showChatResults(directory.query.isEmpty() ? directory.recent : directory.found);
    }
}

QVariantMap ChatPanel::reactionFrequency() const
{
    const auto key = chatPreferenceKey(m_backend->settingsScope(), QLatin1String("reactionFrequency"));
    if (key.isEmpty())
        return {};
    auto settings = chatPreferences();
    settings.beginGroup(key);
    QVariantMap frequency;
    for (const auto &code : settings.childKeys()) {
        if (ui::findReaction(code))
            frequency.insert(code, settings.value(code));
    }
    return frequency;
}

void ChatPanel::incrementReactionFrequency(const QString &code)
{
    const auto key = chatPreferenceKey(m_backend->settingsScope(), QLatin1String("reactionFrequency"));
    if (key.isEmpty())
        return;
    auto settings = chatPreferences();
    settings.beginGroup(key);
    const auto count = settings.value(code).toULongLong();
    if (count < std::numeric_limits<qulonglong>::max())
        settings.setValue(code, count + 1);
}

void ChatPanel::refresh(bool reloadHistory)
{
    const auto selectedChat = m_currentChatId;
    const auto refreshHistory = [this, &selectedChat] {
        if (!selectedChat.isEmpty() && selectedChat == m_currentChatId)
            m_backend->requestChatMessages(selectedChat);
    };
    if (reloadHistory && !m_messageSearchActive)
        refreshHistory();
    if (!m_chatDirectories[m_chatScope].query.isEmpty())
        runSearchChats();
    else
        requestCurrentChatList();
    if (m_messageSearchActive)
        runMessageSearch();
    else if (!reloadHistory)
        refreshHistory();
}

void ChatPanel::openMessageViewers(const QString &messageId, const QPoint &anchor)
{
    const auto scope = m_currentChatId;
    const auto sameHistory = [panel = QPointer<ChatPanel>(this), directory = m_chatScope, scope] {
        return panel && panel->m_active && panel->m_chatMessages->isVisible()
            && panel->m_chatScope == directory && panel->m_currentChatId == scope;
    };
    auto *menu = workspace::openMessageViewers(m_backend, m_chatMessages, messageId, anchor, scope, false,
                                               sameHistory, {m_workspace.avatar, m_workspace.requestAvatar});
    if (!menu)
        return;
    const auto closeIfScopeChanged = [menu, sameHistory] {
        if (!sameHistory())
            menu->close();
    };
    connect(m_chats, &QListWidget::currentItemChanged, menu, closeIfScopeChanged, Qt::QueuedConnection);
    connect(m_chatTabs, &QTabBar::currentChanged, menu, closeIfScopeChanged, Qt::QueuedConnection);
    connect(this, &ChatPanel::sectionActiveChanged, menu, closeIfScopeChanged, Qt::QueuedConnection);
}

void ChatPanel::setSectionActive(bool active)
{
    if (m_active != active) {
        m_active = active;
        emit sectionActiveChanged();
    }
    if (!active) {
        static_cast<ui::HistoryList *>(m_chatMessages)->setLiveReadHistory(false);
        resetMessageContext();
    }
}

void ChatPanel::windowDeactivated()
{
    if (m_taskChatPressActive)
        m_taskChatPressEnd->start(0);
}

void ChatPanel::fileUploaded(const QString &scope, const QString &status, const ui::ChatDraft &draft)
{
    if (draft.image)
        clearSentDraft(m_chatComposer, m_chatDrafts, m_currentChatId, scope, draft);
    emit statusChanged(status);
    if (m_currentChatId == scope && !m_messageSearchActive)
        m_backend->requestChatMessages(scope);
}

void ChatPanel::requestVisibleAvatars(const std::function<void(const QUrl &)> &request,
                                    const std::function<void(QListWidget *)> &requestVisible) const
{
    request(m_chatAvatarUrl);
    requestVisible(m_chats);
    requestVisible(m_chatMessages);
}

void ChatPanel::updateAvatars(const std::function<void(QListWidget *)> &updateList)
{
    updateList(m_chats);
    updateList(m_chatMessages);
    if (const auto *photo = m_workspace.avatar(m_chatAvatarUrl))
        m_chatAvatar->setPhoto(*photo);
}

} // namespace bx
