#include "ui/chat_panel.h"
#include "ui/task_dialog_controls.h"
#include "ui/chat_widgets.h"
#include "ui/workspace_presentation.h"
#include "ui/workspace_directory.h"
#include <QInputDialog>
#include <QRegularExpression>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QTabBar>
#include <QTimer>
#include <QSignalBlocker>
#include <algorithm>
#include <utility>

namespace bx {

using workspace::label;
using ui::ListViewportAnchor;
using ui::listViewportAnchor;
using ui::restoreListViewport;

QWidget *ChatPanel::createDirectory(QWidget *splitter)
{
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
    return side;
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

void ChatPanel::promptGroupChat()
{
    if (!writable())
        return;
    bool accepted = false;
    const auto name = QInputDialog::getText(window(), QStringLiteral("Новый групповой чат"),
                                           QStringLiteral("Название"), QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted || name.isEmpty())
        return;
    const auto members = QInputDialog::getText(window(), QStringLiteral("Участники нового чата"),
        QStringLiteral("ID сотрудников через запятую"), QLineEdit::Normal, {}, &accepted);
    if (accepted && writable()) {
        static const QRegularExpression separators(QStringLiteral("[,\\s]+"));
        m_backend->createGroupChat(name, members.split(separators, Qt::SkipEmptyParts));
    }
}

void ChatPanel::promptChatMember(const QString &dialog, bool add)
{
    if (!writable())
        return;
    bool accepted = false;
    const auto title = add ? QStringLiteral("Добавить участника") : QStringLiteral("Удалить участника");
    const auto id = QInputDialog::getText(window(), title, QStringLiteral("ID сотрудника"),
                                        QLineEdit::Normal, {}, &accepted).trimmed();
    if (accepted && !id.isEmpty() && writable())
        m_backend->changeChatMember(dialog, id, add);
}

} // namespace bx
