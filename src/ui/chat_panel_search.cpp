#include "ui/chat_panel.h"
#include "ui/task_dialog_controls.h"
#include "ui/chat_widgets.h"
#include "ui/workspace_presentation.h"
#include <QLineEdit>
#include <QTimer>
#include <QSignalBlocker>
#include <QHBoxLayout>

namespace bx {

using ui::presentMessages;
using ui::showEmpty;

QWidget *ChatPanel::createMessageSearch(QWidget *reading)
{
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
    return searchBar;
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

void ChatPanel::showMessageResults(const QVector<Message> &messages)
{
    static_cast<ui::HistoryList *>(m_chatMessages)->setLiveReadHistory(false);
    presentMessages(m_chatMessages, m_chatEmpty, messages, ui::MessageView::Search);
    emit avatarRowsChanged();
    emit avatarsRequested();
}

void ChatPanel::showSearchResponse(const QString &scopeId, const QString &query, const QVector<Message> &messages)
{
    if (!m_active || scopeId != m_messageSearchScope || !m_messageSearchActive)
        return;
    if (query != m_messageSearchQuery)
        m_historySearch.request(scopeId, m_messageSearchQuery);
    else if (query == m_messageSearch->text().trimmed())
        showMessageResults(messages);
}

void ChatPanel::chatSearchFailed(const QString &scope, const QString &query, const QString &reason)
{
    if (!m_active || !m_messageSearchActive || scope != m_messageSearchScope)
        return;
    if (query != m_messageSearchQuery)
        m_historySearch.request(m_messageSearchScope, m_messageSearchQuery);
    else if (query == m_messageSearch->text().trimmed())
        showEmpty(m_chatMessages, m_chatEmpty, reason);
}

} // namespace bx
