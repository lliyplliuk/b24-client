#include "ui/history_list.h"

#include "ui/glyph_button.h"
#include "ui/message_menu_window.h"
#include "ui/workspace_presentation.h"
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCursor>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QPlainTextEdit>
#include <QWheelEvent>

namespace bx::ui {

void HistoryList::keyboardSearch(const QString &)
{
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

} // namespace bx::ui
