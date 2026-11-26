#include "ui/history_list.h"

#include "ui/glyph_button.h"
#include "ui/workspace_presentation.h"
#include <QHBoxLayout>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>

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
