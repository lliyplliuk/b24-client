#include "ui/chat_panel.h"
#include "ui/task_dialog_controls.h"
#include "ui/chat_widgets.h"
#include "ui/workspace_presentation.h"
#include "ui/workspace_directory.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPushButton>
#include <QTimer>
#include <QGuiApplication>

namespace bx {

using workspace::label;
using ui::emptyState;

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
    splitter->addWidget(createDirectory(splitter));

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

    layout->addWidget(createMessageSearch(reading));

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

    createComposer(reading, history);
    layout->addWidget(m_chatComposer);
    m_chatComposer->hide();
    splitter->addWidget(reading);
    splitter->setStretchFactor(1, 1);
    connect(splitter, &QSplitter::splitterMoved, this, &ChatPanel::avatarsRequested);
    connectBackend();
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
