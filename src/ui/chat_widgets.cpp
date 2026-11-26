#include "ui/chat_widgets.h"

namespace bx::ui {

HistoryList::HistoryList(QWidget *parent) : QListWidget(parent)
{
    viewport()->setAutoFillBackground(false);
    m_stickyDay = new QLabel(viewport());
    m_stickyDay->setObjectName(QStringLiteral("chatStickyDay"));
    m_stickyDay->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_stickyDay->setAlignment(Qt::AlignCenter);
    m_stickyDay->setFont(workspace::rowFont(secondaryTextPixels, true));
    m_stickyDay->setStyleSheet(QStringLiteral(
        "QLabel#chatStickyDay { color: #486961; background: #dce9e2; border-radius: 9px; }"));
    m_stickyDay->hide();
    const auto historyChanged = [this] {
        m_wheelAnimation.stop();
        scheduleAlignment();
    };
    connect(model(), &QAbstractItemModel::rowsInserted, this, historyChanged);
    connect(model(), &QAbstractItemModel::rowsRemoved, this, historyChanged);
    connect(model(), &QAbstractItemModel::modelReset, this, historyChanged);
    auto *scroll = verticalScrollBar();
    m_wheelAnimation.setDuration(120);
    m_wheelAnimation.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_wheelAnimation, &QVariantAnimation::stateChanged, this, [this](QAbstractAnimation::State state) {
        if (state == QAbstractAnimation::Stopped)
            m_olderOnWheelEnd = false;
    });
    connect(&m_wheelAnimation, &QVariantAnimation::valueChanged, this, [this, scroll](const QVariant &value) {
        if (m_wheelAnimation.state() != QAbstractAnimation::Running)
            return;
        scroll->setValue(value.toInt());
        if (m_olderOnWheelEnd && scroll->value() == scroll->minimum()) {
            m_olderOnWheelEnd = false;
            if (!m_failedOlder)
                requestOlder();
        }
    });
    connect(scroll, &QScrollBar::rangeChanged, &m_wheelAnimation, &QVariantAnimation::stop);
    connect(scroll, &QScrollBar::sliderPressed, &m_wheelAnimation, &QVariantAnimation::stop);
    connect(scroll, &QScrollBar::valueChanged, this, [this](int value) {
        if (m_wheelAnimation.currentValue().toInt() != value)
            m_wheelAnimation.stop();
        clearMessageHover();
        updateStickyDay();
        if (m_onViewportChanged)
            m_onViewportChanged();
    });
    connect(scroll, &QScrollBar::actionTriggered, this, [this] {
        m_wheelAnimation.stop();
        resumeReadAfterJump();
    });
}

HistoryList::~HistoryList()
{
    // m_wheelAnimation dies before QWidget deletes children (model, scrollbar);
    // their signals must not reach lambdas that touch the animation.
    m_wheelAnimation.stop();
    if (model())
        QObject::disconnect(model(), nullptr, this, nullptr);
    QObject::disconnect(verticalScrollBar(), nullptr, this, nullptr);
    QObject::disconnect(verticalScrollBar(), nullptr, &m_wheelAnimation, nullptr);
}

void HistoryList::setOnViewportChanged(std::function<void()> handler)
{
    m_onViewportChanged = std::move(handler);
}

bool HistoryList::showingLiveHistory() const
{
    return m_liveReadHistory && !m_browsingOlder;
}

void HistoryList::setLiveReadHistory(bool live)
{
    m_liveReadHistory = live;
    if (!live) {
        m_wheelAnimation.stop();
        if (m_openMessageMenu)
            m_openMessageMenu->close();
        m_readPausedByJump = false;
        m_pendingOlder.clear();
        if (m_browsingOlder) {
            clearHistoryWindow();
            clear();
            setBrowsingOlder(false);
        }
    }
    updateOlderButton();
}

void HistoryList::setOnBrowsingOlder(std::function<void(bool)> handler)
{
    m_onBrowsingOlder = std::move(handler);
}

void HistoryList::returnToLatest()
{
    if (m_browsingOlder)
        setLiveReadHistory(false);
}

QWidget *HistoryList::olderButton(QWidget *parent)
{
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    m_olderProgress = new QLabel(row);
    m_olderProgress->setFixedWidth(19);
    layout->addWidget(m_olderProgress);
    m_olderButton = new QPushButton(QStringLiteral("Более ранние сообщения"), row);
    m_olderButton->setObjectName(QStringLiteral("olderChatMessages"));
    m_olderButton->setAutoDefault(false);
    connect(m_olderButton, &QPushButton::clicked, this, [this] { requestOlder(); });
    layout->addWidget(m_olderButton);
    updateOlderButton();
    return row;
}

void HistoryList::setOnOlder(std::function<void(const QString &)> handler)
{
    m_onOlder = std::move(handler);
}

void HistoryList::resetHistory(const QString &scope)
{
    if (m_openMessageMenu)
        m_openMessageMenu->close();
    if (auto *delegate = textDelegate())
        delegate->clearTextSelection();
    m_pickingMessageText = false;
    m_pressedTextLink.clear();
    m_historyScope = scope;
    m_removedHistoryIds.clear();
    m_removedHistoryThrough = 0;
    clearHistoryWindow();
    setLiveReadHistory(false);
}

void HistoryList::showContext(const QString &scope, QLabel *state, const QVector<Message> &messages)
{
    if (m_openMessageMenu)
        m_openMessageMenu->close();
    if (m_historyScope != scope)
        resetHistory(scope);
    clearHistoryWindow();
    m_historyState = state;
    for (const auto &message : messages)
        if (!historyRemoved(message.id.toLongLong()))
            m_history.append(message);
    m_liveReadHistory = false;
    setBrowsingOlder(true);
    renderHistory(false);
}

void HistoryList::showHistory(const QString &scope, QLabel *state, const QVector<Message> &latest)
{
    if (m_historyScope != scope)
        resetHistory(scope);
    if (m_browsingOlder)
        return;
    const bool preserve = m_liveReadHistory && !atLatest();
    m_historyState = state;
    const auto first = oldestId(latest).toLongLong();
    qint64 last = 0;
    for (const auto &entry : latest)
        last = qMax(last, entry.id.toLongLong());
    const bool advancing = last > m_latestLast;
    m_history.erase(std::remove_if(m_history.begin(), m_history.end(), [this, first, advancing](const Message &entry) {
        const auto id = entry.id.toLongLong();
        return first <= 0 || id >= first || id <= 0 || (!advancing && id >= m_latestFirst) || historyRemoved(id);
    }), m_history.end());
    for (const auto &entry : latest) {
        if (!historyRemoved(entry.id.toLongLong()))
            m_history.append(entry);
    }
    m_latestFirst = first;
    m_latestLast = last;
    if (m_history.size() > maxHistoryRows)
        m_history.remove(0, m_history.size() - maxHistoryRows);
    m_liveReadHistory = true;
    renderHistory(preserve);
}

bool HistoryList::addOlder(const QString &scope, const QString &before, const QVector<Message> &messages, bool hasMore)
{
    if (!pagingReady() || !m_historyState || scope != m_historyScope || before != m_pendingOlder)
        return false;
    m_pendingOlder.clear();
    m_failedOlder = false;
    QVector<Message> older;
    QSet<qint64> buffered;
    for (const auto &entry : m_history)
        buffered.insert(entry.id.toLongLong());
    const auto limit = before.toLongLong();
    const auto currentOldest = oldestId(m_history).toLongLong();
    qint64 scanned = 0;
    for (const auto &entry : messages) {
        const auto id = entry.id.toLongLong();
        if (id <= 0 || id >= limit)
            continue;
        scanned = scanned == 0 ? id : qMin(scanned, id);
        if ((currentOldest == 0 || id < currentOldest) && !buffered.contains(id) && !historyRemoved(id)) {
            buffered.insert(id);
            older.append(entry);
        }
    }
    m_scannedOlder = scanned;
    m_hasOlder = hasMore && scanned > m_removedHistoryThrough;
    if (!older.isEmpty()) {
        m_history = older + m_history;
        if (m_history.size() > maxHistoryRows) {
            m_history.resize(maxHistoryRows);
            m_liveReadHistory = false;
            setBrowsingOlder(true);
        }
        renderHistory(true);
    } else {
        updateOlderButton();
    }
    return true;
}

void HistoryList::removeHistory(const QStringList &ids, qint64 through)
{
    if (ids.isEmpty() && through == 0) {
        clearHistoryWindow();
        if (pagingReady() && m_historyState)
            renderHistory(false);
        return;
    }
    m_removedHistoryThrough = qMax(m_removedHistoryThrough, through);
    for (const auto &id : ids) {
        const auto numeric = id.toLongLong();
        if (numeric <= m_removedHistoryThrough)
            continue;
        if (!m_removedHistoryIds.contains(numeric) && m_removedHistoryIds.size() >= 2 * maxHistoryRows)
            m_removedHistoryIds.erase(m_removedHistoryIds.begin());
        m_removedHistoryIds.insert(numeric);
    }
    m_removedHistoryIds.removeIf([this](qint64 id) { return id <= m_removedHistoryThrough; });
    const auto before = m_history.size();
    m_history.erase(std::remove_if(m_history.begin(), m_history.end(), [this](const Message &message) {
        return historyRemoved(message.id.toLongLong());
    }), m_history.end());
    if (before != m_history.size() && pagingReady() && m_historyState)
        renderHistory(true);
    else
        updateOlderButton();
}

void HistoryList::olderFailed(const QString &scope, const QString &before, const QString &reason)
{
    if (scope != m_historyScope || before != m_pendingOlder)
        return;
    m_pendingOlder.clear();
    m_failedOlder = true;
    qWarning("Bitrix24 history: failed to load an earlier page");
    if (m_olderButton)
        m_olderButton->setToolTip(reason);
    updateOlderButton();
}

void HistoryList::confirmMessageReaction(const QString &id, const QString &code, bool present)
{
    for (auto &message : m_history) {
        if (message.id != id)
            continue;
        auto &entries = message.reactions;
        const auto found = std::find_if(entries.begin(), entries.end(),
                                        [&code](const MessageReaction &entry) { return entry.code == code; });
        if (found == entries.end()) {
            if (present)
                entries.append({code, 1, true});
        } else if (found->own != present) {
            found->own = present;
            if (present)
                ++found->count;
            else if (--found->count <= 0)
                entries.erase(found);
        }
        break;
    }
    for (int row = 0; row < count(); ++row) {
        auto *entry = item(row);
        if (entry->data(workspace::ReceiptMessageId).toString() != id)
            continue;
        auto own = entry->data(workspace::OwnMessageReactions).toStringList();
        if (own.contains(code) == present)
            break;
        auto counts = entry->data(workspace::MessageReactionCounts).toMap();
        const int total = counts.value(code).toInt() + (present ? 1 : -1);
        if (total > 0)
            counts.insert(code, total);
        else
            counts.remove(code);
        if (present)
            own.append(code);
        else
            own.removeAll(code);
        entry->setData(workspace::MessageReactionCounts, counts);
        entry->setData(workspace::OwnMessageReactions, own);
        refreshRowSizes();
        break;
    }
}

void HistoryList::confirmMessageEdit(const QString &id, const QString &text)
{
    for (auto &message : m_history) {
        if (message.id != id || message.text == text)
            continue;
        message.text = text;
        if (pagingReady() && m_historyState)
            renderHistory(true);
        break;
    }
}

void HistoryList::confirmRead(const QStringList &ids, qint64 readThrough)
{
    for (auto &message : m_history) {
        const auto id = message.id.toLongLong();
        if (ids.contains(message.id) || (id > 0 && id <= readThrough))
            message.unread = false;
    }
    if (!m_liveReadHistory || (ids.isEmpty() && readThrough <= 0))
        return;
    QListWidgetItem *previous = nullptr;
    QListWidgetItem *next = nullptr;
    for (int row = 0; row < count(); ++row) {
        auto *message = item(row);
        if (message->data(workspace::UnreadDivider).toBool())
            previous = message;
        if (!message->data(workspace::MessageUnread).toBool())
            continue;
        const auto id = message->data(workspace::ReceiptMessageId).toString();
        const auto number = id.toLongLong();
        if (!id.isEmpty() && (ids.contains(id) || (number > 0 && number <= readThrough)))
            message->setData(workspace::MessageUnread, false);
        else if (!next)
            next = message;
    }
    if (next == previous)
        return;
    const auto prefix = workspace::UnreadMessages + QStringLiteral(". ");
    if (previous) {
        previous->setData(workspace::UnreadDivider, false);
        const auto description = previous->data(Qt::AccessibleTextRole).toString();
        if (description.startsWith(prefix))
            previous->setData(Qt::AccessibleTextRole, description.mid(prefix.size()));
    }
    if (next) {
        next->setData(workspace::UnreadDivider, true);
        next->setData(Qt::AccessibleTextRole, prefix + next->data(Qt::AccessibleTextRole).toString());
    }
    refreshRowSizes();
}

void HistoryList::setReplyActions(std::function<void(const ChatReply &)> reply, std::function<bool()> allowed,
                                  std::function<void(const QString &)> navigate)
{
    m_onReply = std::move(reply);
    m_replyAllowed = std::move(allowed);
    m_onOriginal = std::move(navigate);
    static_cast<workspace::RowPainter *>(itemDelegate())->setReplyNavigation(m_onOriginal);
}

void HistoryList::setMessageMenu(std::function<void(QMenu &, const QPersistentModelIndex &, const QString &)> handler)
{
    m_messageMenu = std::move(handler);
    if (!m_messageMenu) {
        if (m_openMessageMenu)
            m_openMessageMenu->close();
        clearMessageHover();
        return;
    }
    if (m_messageActions)
        return;
    m_messageActions = createMessageHoverButton(Glyph::More, QStringLiteral("Действия сообщения"));
    viewport()->setMouseTracking(true);
    connect(m_messageActions, &QPushButton::clicked, this, [this] {
        const auto message = m_hoverMessage;
        const QRect anchor(m_messageActions->mapToGlobal(QPoint{}), m_messageActions->size());
        clearMessageHover();
        showMessageMenu(message, anchor);
    });
}

void HistoryList::setReactionActions(std::function<void(const QModelIndex &, const QString &, const QPoint &)> action,
                                     std::function<bool()> allowed)
{
    auto *delegate = static_cast<workspace::RowPainter *>(itemDelegate());
    delegate->setReactionActions(std::move(action), std::move(allowed));
    if (m_messageReaction)
        return;
    m_messageReaction = createMessageHoverButton(Glyph::Emoji, QStringLiteral("Добавить реакцию"));
    connect(m_messageReaction, &QPushButton::clicked, this, [this, delegate] {
        const auto message = m_hoverMessage;
        const auto anchor = m_messageReaction->mapToGlobal(m_messageReaction->rect().bottomLeft());
        clearMessageHover();
        delegate->openReactionPalette(message, anchor);
    });
}

bool HistoryList::focusOnMessage(const QString &id)
{
    for (int row = 0; row < count(); ++row) {
        auto *entry = item(row);
        if (entry->data(workspace::ReceiptMessageId).toString() != id)
            continue;
        m_readPausedByJump = m_liveReadHistory;
        setCurrentItem(entry);
        scrollToItem(entry, QAbstractItemView::PositionAtCenter);
        setFocus(Qt::OtherFocusReason);
        return true;
    }
    return false;
}

HistoryList::VisibleMessages HistoryList::visibleMessageIds() const
{
    VisibleMessages result;
    if (!m_liveReadHistory || m_readPausedByJump || !isVisible() || count() == 0)
        return result;
    const auto visible = viewport()->visibleRegion().boundingRect();
    if (visible.isEmpty())
        return result;
    const auto *delegate = static_cast<const workspace::RowPainter *>(itemDelegate());
    const auto first = indexAt(QPoint(visible.left() + 2, visible.top()));
    for (int row = first.isValid() ? first.row() : 0; row < count(); ++row) {
        const auto *entry = item(row);
        const auto rect = visualItemRect(entry);
        if (rect.top() > visible.bottom())
            break;
        const auto id = entry->data(workspace::ReceiptMessageId).toString();
        if (id.isEmpty())
            continue;
        const auto body = delegate->receiptArea(model()->index(row, 0), rect);
        if (body.isEmpty() || body.intersected(visible).height() < qMin(24, body.height()))
            continue;
        result.all.append(id);
        if (!entry->data(workspace::Outgoing).toBool())
            result.incoming.append(id);
    }
    return result;
}

void HistoryList::setComposer(QPlainTextEdit *composer)
{
    m_composer = composer;
    setAttribute(Qt::WA_InputMethodEnabled, composer != nullptr);
}

void HistoryList::refreshRowSizes()
{
    auto *scroll = verticalScrollBar();
    const bool follow = atLatest();
    const auto *first = itemAt(QPoint(4, 2));
    const int before = first ? visualItemRect(first).top() : 0;
    doItemsLayout();
    if (follow)
        scrollToBottom();
    else if (first)
        scroll->setValue(scroll->value() + visualItemRect(first).top() - before);
    scheduleAlignment();
}

void HistoryList::updateGeometries()
{
    const bool follow = m_liveReadHistory && !m_browsingOlder && atLatest();
    QListWidget::updateGeometries();
    // Parent layout can resize the viewport after the first scrollToBottom().
    if (follow)
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

void HistoryList::mouseMoveEvent(QMouseEvent *event)
{
    const auto point = event->position().toPoint();
    if (m_pickingMessageText && event->buttons().testFlag(Qt::LeftButton)) {
        if ((point - m_textPressPoint).manhattanLength() >= QApplication::startDragDistance())
            m_pressedTextLink.clear();
        if (m_pressedTextLink.isEmpty()) {
            textDelegate()->extendTextSelection(point);
            viewport()->setCursor(Qt::IBeamCursor);
        }
        event->accept();
        return;
    }
    hoverMessage(point);
    QListWidget::mouseMoveEvent(event);
}

void HistoryList::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        const auto point = event->position().toPoint();
        const auto index = indexAt(point);
        const bool extend = event->modifiers().testFlag(Qt::ShiftModifier);
        const auto pick = extend ? workspace::RowPainter::TextPick::Extend : workspace::RowPainter::TextPick::Position;
        m_pressedTextLink.clear();
        if (pickMessageText(index, point, pick)) {
            if (!extend)
                m_pressedTextLink = textDelegate()->anchorAt(index, visualRect(index), point);
            event->accept();
            return;
        }
        m_pickingMessageText = false;
        if (auto *delegate = textDelegate())
            delegate->clearTextSelection();
    }
    QListWidget::mousePressEvent(event);
}

void HistoryList::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_pickingMessageText) {
        const auto point = event->position().toPoint();
        m_pickingMessageText = false;
        const bool linkClick = !m_pressedTextLink.isEmpty() &&
            (point - m_textPressPoint).manhattanLength() < QApplication::startDragDistance() &&
            textDelegate()->activateTextLink(m_pressedTextLink, point);
        if (!linkClick && point != m_textPressPoint)
            textDelegate()->extendTextSelection(point);
        m_pressedTextLink.clear();
        event->accept();
        return;
    }
    QListWidget::mouseReleaseEvent(event);
}

void HistoryList::leaveEvent(QEvent *event)
{
    clearMessageHover();
    QListWidget::leaveEvent(event);
}

bool HistoryList::viewportEvent(QEvent *event)
{
    if (event->type() == QEvent::Leave && m_messageActions && m_messageActions->isVisible() &&
        !m_hoverActionsBounds.contains(viewport()->mapFromGlobal(QCursor::pos())))
        clearMessageHover();
    return QListWidget::viewportEvent(event);
}

void HistoryList::mouseDoubleClickEvent(QMouseEvent *event)
{
    const auto point = event->position().toPoint();
    const auto index = indexAt(point);
    if (event->button() == Qt::LeftButton &&
        pickMessageText(index, point, workspace::RowPainter::TextPick::Word)) {
        m_pressedTextLink.clear();
        event->accept();
        return;
    }
    QListWidget::mouseDoubleClickEvent(event);
}

void HistoryList::contextMenuEvent(QContextMenuEvent *event)
{
    const bool keyboard = event->reason() == QContextMenuEvent::Keyboard;
    const auto viewportPoint = viewport()->mapFromGlobal(event->globalPos());
    auto *entry = keyboard ? currentItem() : itemAt(viewportPoint);
    if (!entry)
        return;
    const auto index = model()->index(row(entry), 0);
    const auto *painter = static_cast<workspace::RowPainter *>(itemDelegate());
    clearMessageHover();
    if (!keyboard && painter->showPhotoMenuAt(index, viewportPoint)) {
        event->accept();
        return;
    }
    const auto href = keyboard ? QString() : painter->linkAt(index, visualRect(index), viewportPoint);
    const auto point = keyboard ? viewport()->mapToGlobal(visualItemRect(entry).center()) : event->globalPos();
    if (showMessageMenu(index, QRect(point, QSize(1, 1)), href))
        event->accept();
}

bool HistoryList::showMessageMenu(const QPersistentModelIndex &index, const QRect &anchor, const QString &href)
{
    if (!index.isValid())
        return false;
    const auto id = index.data(workspace::ReceiptMessageId).toString();
    const auto origin = index.data(workspace::ReplyTargetId).toString();
    const bool canCopy = textDelegate() && !textDelegate()->selectedText(index).isEmpty();
    const bool reply = m_onReply && m_replyAllowed && m_replyAllowed() && !id.isEmpty()
        && !index.data(workspace::SystemMessage).toBool();
    if (!m_messageMenu && !canCopy && !reply && (origin.isEmpty() || !m_onOriginal))
        return false;
    if (m_openMessageMenu)
        m_openMessageMenu->close();
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("messageContextMenu"));
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    const auto currentMessage = [this, index, id, scope = m_historyScope] {
        if (scope != m_historyScope)
            return QPersistentModelIndex{};
        return index.isValid() ? index : messageIndex(model(), id);
    };
    const auto checkMessage = [menu, currentMessage] {
        QTimer::singleShot(0, menu, [menu, currentMessage] {
            if (!currentMessage().isValid())
                menu->close();
        });
    };
    connect(model(), &QAbstractItemModel::modelReset, menu, checkMessage);
    connect(model(), &QAbstractItemModel::rowsRemoved, menu, checkMessage);
    if (canCopy) {
        connect(menu->addAction(QStringLiteral("Копировать")), &QAction::triggered, this, [this, currentMessage] {
            const auto index = currentMessage();
            if (index.isValid())
                copyMessageText(index);
        });
    }
    if (reply) {
        connect(menu->addAction(QStringLiteral("Ответить")), &QAction::triggered, this, [this, currentMessage, id] {
            const auto index = currentMessage();
            if (!index.isValid() || !m_replyAllowed() || index.data(workspace::SystemMessage).toBool())
                return;
            auto reference = index.data(workspace::Detail).toString().simplified().left(240);
            const auto files = index.data(workspace::MessageFiles).value<QVector<MessageFile>>();
            if (reference.isEmpty() && !files.isEmpty())
                reference = files.first().name.simplified().left(240);
            if (reference.isEmpty())
                reference = QStringLiteral("Сообщение №%1").arg(id);
            m_onReply({id, index.data(workspace::MessageAuthor).toString(), reference});
        });
    }
    if (!origin.isEmpty() && m_onOriginal) {
        connect(menu->addAction(QStringLiteral("Перейти к исходному сообщению")), &QAction::triggered, this,
                [this, currentMessage, origin] {
            if (currentMessage().data(workspace::ReplyTargetId).toString() == origin)
                m_onOriginal(origin);
        });
    }
    if (m_messageMenu)
        m_messageMenu(*menu, index, href);
    if (menu->isEmpty()) {
        menu->deleteLater();
        return false;
    }
    m_openMessageMenu = menu;
    showMessageMenuWindow(*menu, anchor);
    return true;
}

void HistoryList::showEvent(QShowEvent *event)
{
    QListWidget::showEvent(event);
    scheduleAlignment();
}

void HistoryList::hideEvent(QHideEvent *event)
{
    m_wheelAnimation.stop();
    if (m_openMessageMenu)
        m_openMessageMenu->close();
    clearMessageHover();
    QListWidget::hideEvent(event);
}

void HistoryList::focusOutEvent(QFocusEvent *event)
{
    if (event->reason() == Qt::MouseFocusReason && (!m_openMessageMenu || !m_openMessageMenu->isVisible())) {
        if (auto *delegate = textDelegate())
            delegate->clearTextSelection();
    }
    QListWidget::focusOutEvent(event);
}

void HistoryList::keyPressEvent(QKeyEvent *event)
{
    if (handleTextSelectionKey(event)) {
        event->accept();
        return;
    }
    switch (event->key()) {
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
    case Qt::Key_Home:
    case Qt::Key_End:
        m_wheelAnimation.stop();
        if (auto *delegate = textDelegate())
            delegate->clearTextSelection();
        resumeReadAfterJump();
        break;
    default:
        break;
    }
    const auto disallowed = Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier;
    const auto text = event->text();
    if ((event->modifiers() & disallowed) || text.isEmpty() ||
        (!text.front().isPrint() && !text.front().isHighSurrogate())) {
        if (!(event->modifiers() & (disallowed | Qt::ShiftModifier)) &&
            (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            openFocusedMedia()) {
            event->accept();
            return;
        }
        QListWidget::keyPressEvent(event);
        return;
    }
    sendToComposer(event);
}

void HistoryList::inputMethodEvent(QInputMethodEvent *event)
{
    sendToComposer(event);
}

void HistoryList::resizeEvent(QResizeEvent *event)
{
    QListWidget::resizeEvent(event);
    scheduleAlignment();
}

void HistoryList::wheelEvent(QWheelEvent *event)
{
    clearMessageHover();
    resumeReadAfterJump();
    auto *scroll = verticalScrollBar();
    const auto angle = event->angleDelta();
    const bool towardStart = angle.y() != 0 && (angle.y() > 0) == scroll->invertedControls();
    if (towardStart && scroll->value() == scroll->minimum() && !m_failedOlder)
        requestOlder();
    const bool detent = event->pixelDelta().isNull() && angle.x() == 0 && angle.y() != 0
        && angle.y() % QWheelEvent::DefaultDeltasPerStep == 0 && event->modifiers() == Qt::NoModifier
        && event->phase() == Qt::NoScrollPhase && event->deviceType() != QInputDevice::DeviceType::TouchPad;
    if (!detent) {
        m_wheelAnimation.stop();
        QListWidget::wheelEvent(event);
        return;
    }

    const int page = scroll->pageStep();
    const auto nativeNotch = qMin(qreal(page), qreal(scroll->singleStep()) * QApplication::wheelScrollLines());
    if (nativeNotch <= 0) {
        event->ignore();
        return;
    }
    const auto notches = qAbs(angle.y()) / QWheelEvent::DefaultDeltasPerStep;
    const int pixels = qMax(1, qRound(qMin(qreal(page), nativeNotch * notches / 8.0)));
    const int step = towardStart ? -pixels : pixels;
    const int current = scroll->value();
    const bool running = m_wheelAnimation.state() == QAbstractAnimation::Running;
    const int previousTarget = running ? m_wheelAnimation.endValue().toInt() : current;
    // Keep pending distance for consecutive notches in one direction, and reverse from the visible position.
    const bool sameDirection = step < 0 ? previousTarget < current : previousTarget > current;
    const int base = sameDirection ? previousTarget : current;
    const int target = int(qBound(qint64(scroll->minimum()), qint64(base) + step, qint64(scroll->maximum())));
    if (target == current) {
        m_wheelAnimation.stop();
        event->ignore();
        return;
    }
    if (!running || target != previousTarget) {
        m_wheelAnimation.stop();
        m_wheelAnimation.setStartValue(current);
        m_wheelAnimation.setEndValue(target);
        m_wheelAnimation.start();
    } else if (towardStart && target == scroll->minimum()) {
        // A further notch at the queued boundary asks for older history once we arrive.
        m_olderOnWheelEnd = true;
    }
    event->accept();
}

workspace::RowPainter *HistoryList::textDelegate() const
{
    return dynamic_cast<workspace::RowPainter *>(itemDelegate());
}

bool HistoryList::pickMessageText(const QModelIndex &index, const QPoint &point, workspace::RowPainter::TextPick pick)
{
    auto *delegate = textDelegate();
    if (!delegate || !delegate->pickMessageText(index, point, pick))
        return false;
    setFocus(Qt::MouseFocusReason);
    setCurrentIndex(index);
    m_textPressPoint = point;
    m_pickingMessageText = true;
    clearMessageHover();
    return true;
}

void HistoryList::copyMessageText(const QModelIndex &index) const
{
    if (const auto *delegate = textDelegate()) {
        const auto selected = delegate->selectedText(index);
        if (!selected.isEmpty())
            QApplication::clipboard()->setText(selected);
    }
}

bool HistoryList::handleTextSelectionKey(const QKeyEvent *event)
{
    auto *delegate = textDelegate();
    if (!delegate || !delegate->hasTextCursor())
        return false;
    if (event->matches(QKeySequence::Copy)) {
        copyMessageText();
        return true;
    }
    if (event->matches(QKeySequence::SelectAll)) {
        delegate->selectMessageText();
        return true;
    }
    if (event->key() == Qt::Key_Escape) {
        const bool selected = !delegate->selectedText().isEmpty();
        delegate->clearTextSelection();
        return selected;
    }
    using Binding = std::pair<QKeySequence::StandardKey, QTextCursor::MoveOperation>;
    static constexpr Binding bindings[]{
        {QKeySequence::SelectPreviousChar, QTextCursor::Left},
        {QKeySequence::SelectNextChar, QTextCursor::Right},
        {QKeySequence::SelectPreviousWord, QTextCursor::WordLeft},
        {QKeySequence::SelectNextWord, QTextCursor::WordRight},
        {QKeySequence::SelectPreviousLine, QTextCursor::Up},
        {QKeySequence::SelectNextLine, QTextCursor::Down},
        {QKeySequence::SelectStartOfLine, QTextCursor::StartOfLine},
        {QKeySequence::SelectEndOfLine, QTextCursor::EndOfLine},
        {QKeySequence::SelectStartOfDocument, QTextCursor::Start},
        {QKeySequence::SelectEndOfDocument, QTextCursor::End},
    };
    for (const auto &[key, operation] : bindings) {
        if (event->matches(key)) {
            delegate->moveTextSelection(operation);
            return true;
        }
    }
    return false;
}

void HistoryList::resumeReadAfterJump()
{
    if (!m_readPausedByJump)
        return;
    m_readPausedByJump = false;
    if (m_onViewportChanged)
        m_onViewportChanged();
}

void HistoryList::updateStickyDay()
{
    const auto visible = viewport()->visibleRegion().boundingRect();
    if (!isVisible() || visible.isEmpty() || verticalScrollBar()->minimum() == verticalScrollBar()->maximum()) {
        m_stickyDay->hide();
        return;
    }
    const auto first = indexAt(QPoint(visible.center().x(), visible.top()));
    int row = first.isValid() ? first.row() : 0;
    if (!first.isValid()) {
        int end = count();
        while (row < end) {
            const int middle = row + (end - row) / 2;
            if (visualItemRect(item(middle)).bottom() < visible.top())
                row = middle + 1;
            else
                end = middle;
        }
    }
    for (; row < count(); ++row) {
        const auto *entry = item(row);
        const auto bounds = visualItemRect(entry);
        if (bounds.top() > visible.bottom())
            break;
        const auto day = entry->data(workspace::MessageCalendarDay).toDate();
        if (!day.isValid())
            continue;
        const int stampTop = bounds.top() + 3;
        if (!entry->data(workspace::DayDivider).toString().isEmpty()
            && stampTop >= visible.top() && stampTop + 18 <= visible.bottom())
            break;
        const auto label = messageDayLabel(day);
        const int width = qMin(visible.width(), m_stickyDay->fontMetrics().horizontalAdvance(label) + 20);
        m_stickyDay->setText(label);
        m_stickyDay->setGeometry(visible.center().x() - width / 2, visible.top() + 3, width, 19);
        m_stickyDay->show();
        m_stickyDay->raise();
        return;
    }
    m_stickyDay->hide();
}

bool HistoryList::atLatest() const
{
    return verticalScrollBar()->value() == verticalScrollBar()->maximum();
}

bool HistoryList::pagingReady() const
{
    return m_liveReadHistory || m_browsingOlder;
}

void HistoryList::clearHistoryWindow()
{
    m_wheelAnimation.stop();
    m_history.clear();
    m_pendingOlder.clear();
    m_scannedOlder = 0;
    m_latestFirst = 0;
    m_latestLast = 0;
    m_hasOlder = true;
    m_failedOlder = false;
}

void HistoryList::setBrowsingOlder(bool active)
{
    if (m_browsingOlder == active)
        return;
    m_browsingOlder = active;
    if (m_onBrowsingOlder)
        m_onBrowsingOlder(active);
}

GlyphButton *HistoryList::createMessageHoverButton(Glyph glyph, const QString &name)
{
    auto *button = new GlyphButton(glyph, name, viewport());
    constexpr int size = workspace::RowPainter::messageActionSize;
    button->setFixedSize(size, size);
    button->setFocusPolicy(Qt::NoFocus);
    button->setCursor(Qt::PointingHandCursor);
    button->hide();
    return button;
}

void HistoryList::hoverMessage(const QPoint &point)
{
    if (!m_messageActions || !m_messageMenu)
        return;
    const auto *delegate = static_cast<const workspace::RowPainter *>(itemDelegate());
    if (m_hoverMessage.isValid() && m_messageActions->isVisible()
        && (m_hoverBounds.contains(point) || m_hoverActionsBounds.contains(point))
        && (!m_messageReaction || m_messageReaction->isVisible() == delegate->canReact(m_hoverMessage)))
        return;
    auto index = indexAt(point);
    if (!index.isValid() && m_hoverMessage.isValid())
        index = m_hoverMessage;
    if (!index.isValid() || index.data(workspace::ReceiptMessageId).toString().isEmpty()
        || index.data(workspace::SystemMessage).toBool()) {
        clearMessageHover();
        return;
    }
    const auto bubble = delegate->receiptArea(index, visualRect(index));
    const auto size = m_messageActions->size();
    const bool showReaction = m_messageReaction && delegate->canReact(index);
    const int width = showReaction ? workspace::RowPainter::messageActionGroupWidth : size.width();
    const auto height = viewport()->height() - size.height();
    if (bubble.isEmpty() || height < 0) {
        clearMessageHover();
        return;
    }
    constexpr int gap = workspace::RowPainter::messageActionGap;
    const int left = bubble.left() - gap - width;
    const int right = bubble.right() + 1 + gap;
    const bool rightFits = right + width <= viewport()->width();
    const bool useLeft = left >= 0 && (point.x() < bubble.left() || !rightFits);
    const QPoint position(useLeft ? left : right, qBound(0, bubble.top() + 1, height));
    constexpr int leftTolerance = 50;
    const auto reachable = bubble.adjusted(-leftTolerance, 0, viewport()->rect().right() - bubble.right(), 0);
    if ((!useLeft && !rightFits) || !reachable.contains(point)) {
        clearMessageHover();
        return;
    }
    m_hoverMessage = index;
    m_hoverBounds = reachable;
    m_hoverActionsBounds = QRect(position, QSize(width, size.height()));
    m_messageActions->setGeometry(QRect(position + QPoint(width - size.width(), 0), size));
    m_messageActions->show();
    m_messageActions->raise();
    if (m_messageReaction) {
        m_messageReaction->move(position);
        m_messageReaction->setVisible(showReaction);
        if (showReaction)
            m_messageReaction->raise();
    }
}

void HistoryList::clearMessageHover()
{
    m_hoverMessage = QPersistentModelIndex();
    if (m_messageActions)
        m_messageActions->hide();
    if (m_messageReaction)
        m_messageReaction->hide();
}

bool HistoryList::historyRemoved(qint64 id) const
{
    return id > 0 && (id <= m_removedHistoryThrough || m_removedHistoryIds.contains(id));
}

QString HistoryList::oldestId(const QVector<Message> &messages)
{
    qint64 oldest = 0;
    for (const auto &message : messages) {
        const auto id = message.id.toLongLong();
        if (id > 0 && (oldest == 0 || id < oldest))
            oldest = id;
    }
    return oldest > 0 ? QString::number(oldest) : QString{};
}

qint64 HistoryList::olderCursor() const
{
    auto cursor = oldestId(m_history).toLongLong();
    if (m_scannedOlder > 0)
        cursor = cursor > 0 ? qMin(cursor, m_scannedOlder) : m_scannedOlder;
    return cursor;
}

void HistoryList::requestOlder()
{
    const auto cursor = olderCursor();
    if (!pagingReady() || !m_onOlder || !m_hasOlder || !m_pendingOlder.isEmpty()
        || cursor <= m_removedHistoryThrough)
        return;
    m_pendingOlder = QString::number(cursor);
    m_failedOlder = false;
    if (m_olderButton)
        m_olderButton->setToolTip({});
    updateOlderButton();
    m_onOlder(m_pendingOlder);
}

void HistoryList::updateOlderButton()
{
    if (!m_olderButton)
        return;
    const bool available = pagingReady() && m_hasOlder && olderCursor() > m_removedHistoryThrough;
    const bool loading = available && !m_pendingOlder.isEmpty();
    m_olderButton->parentWidget()->setVisible(available);
    m_olderProgress->setVisible(loading);
    setLoadingIndicator(m_olderProgress, loading);
    m_olderButton->setEnabled(m_pendingOlder.isEmpty());
    m_olderButton->setText(!m_pendingOlder.isEmpty() ? QStringLiteral("Загружаем историю…")
        : m_failedOlder ? QStringLiteral("Повторить загрузку истории") : QStringLiteral("Более ранние сообщения"));
}

void HistoryList::renderHistory(bool preserve)
{
    const auto *top = preserve ? itemAt(QPoint(4, 1)) : nullptr;
    const auto anchor = top ? top->data(workspace::Id).toString() : QString{};
    const int offset = top ? visualItemRect(top).top() : 0;
    const auto selected = currentItem() ? currentItem()->data(workspace::Id).toString() : QString{};
    auto *delegate = textDelegate();
    const auto textSelection = delegate ? delegate->snapshotTextSelection() : std::nullopt;
    presentMessages(this, m_historyState, m_history,
                    m_browsingOlder ? MessageView::Context : MessageView::History);
    if (!anchor.isEmpty())
        doItemsLayout();
    for (int i = 0; i < count(); ++i) {
        auto *entry = item(i);
        const auto id = entry->data(workspace::Id).toString();
        if (id == selected)
            setCurrentItem(entry, QItemSelectionModel::NoUpdate);
        if (textSelection && entry->data(workspace::ReceiptMessageId).toString() == textSelection->messageId)
            delegate->restoreTextSelection(model()->index(i, 0), *textSelection);
        if (id != anchor)
            continue;
        scrollToItem(entry, QAbstractItemView::PositionAtTop);
        verticalScrollBar()->setValue(verticalScrollBar()->value() + visualItemRect(entry).top() - offset);
    }
    updateOlderButton();
}

bool HistoryList::openFocusedMedia()
{
    auto *entry = currentItem();
    if (!entry)
        return false;
    auto *delegate = static_cast<workspace::RowPainter *>(itemDelegate());
    if (!delegate)
        return false;
    return delegate->activateMedia(model()->index(row(entry), 0));
}

void HistoryList::sendToComposer(QEvent *event)
{
    if (m_composer && m_composer->isVisible() && m_composer->isEnabled() && !m_composer->isReadOnly()) {
        m_composer->setFocus(Qt::OtherFocusReason);
        QCoreApplication::sendEvent(m_composer, event);
    }
    event->accept();
}

void HistoryList::scheduleAlignment()
{
    if (m_queued)
        return;
    m_queued = true;
    QTimer::singleShot(0, this, [this] {
        m_queued = false;
        clearMessageHover();
        const int available = height() - 2 * frameWidth();
        int used = 0;
        for (int row = 0; row < count() && used <= available; ++row)
            used += qMax(0, sizeHintForRow(row));
        const int top = count() > 0 ? qMax(0, available - used) : 0;
        if (viewportMargins().top() != top)
            setViewportMargins(0, top, 0, 0);
        updateStickyDay();
        if (m_onViewportChanged)
            m_onViewportChanged();
    });
}

} // namespace bx::ui
