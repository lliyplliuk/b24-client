#pragma once

#include "core/notification_targets.h"
#include "core/tunables.h"
#include "core/worktime.h"
#include "ui/chat_reply.h"
#include "ui/glyphs.h"
#include "ui/message_menu_window.h"
#include "ui/people_suggestions.h"
#include "ui/workspace_style.h"
#include "ui/workspace_presentation.h"

#include <QBuffer>
#include <QButtonGroup>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFrame>
#include <QGridLayout>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QImageReader>
#include <QImageWriter>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QMenu>
#include <QMimeData>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextLayout>
#include <QTemporaryDir>
#include <QTimer>
#include <QScrollBar>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace bx::ui {

class GlyphButton final : public QPushButton {
public:
    GlyphButton(Glyph glyph, const QString &name, QWidget *parent, bool primary = false)
        : QPushButton(parent), m_glyph(glyph), m_primary(primary)
    {
        setAccessibleName(name);
        setToolTip(name);
        setAutoDefault(false);
        setFixedSize(primary ? 40 : 36, 36);
    }

    void setGlyph(Glyph glyph)
    {
        m_glyph = glyph;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool active = isEnabled() && (isChecked() || isDown() || underMouse());
        const auto surface = m_primary
            ? (isEnabled() ? (active ? QColor("#174658") : workspace::Accent) : QColor("#e6efeb"))
            : (active ? QColor("#e2efe9") : QColor(Qt::transparent));
        painter.setPen(hasFocus() ? QPen(workspace::Accent, 1.4) : Qt::NoPen);
        painter.setBrush(surface);
        painter.drawRoundedRect(rect().adjusted(2, 2, -2, -2), 8, 8);
        const auto color = isEnabled()
            ? (m_primary ? QColor(Qt::white) : workspace::Accent) : QColor("#687e84");
        paintGlyph(painter, m_glyph, QRectF((width() - 19) / 2.0, (height() - 19) / 2.0, 19, 19), color);
    }

private:
    Glyph m_glyph;
    bool m_primary;
};

constexpr int SectionUnreadCount = Qt::UserRole + 1;
constexpr int SectionWorkdayStatus = Qt::UserRole + 2;
constexpr int SectionWorkdayElapsed = Qt::UserRole + 3;

class SectionRailPainter final : public QStyledItemDelegate {
public:
    explicit SectionRailPainter(QObject *parent) : QStyledItemDelegate(parent) {}
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return {79, 65}; }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (index.row() < 0 || index.row() > 3)
            return;
        static constexpr Glyph glyphs[] = {Glyph::Chat, Glyph::Projects, Glyph::Notices, Glyph::Clock};
        auto caption = index.row() == 2 ? QStringLiteral("События") : index.data().toString();
        const auto row = option.rect.adjusted(2, 2, -2, -2);
        const bool active = option.state & QStyle::State_Selected;
        const bool focused = option.state & QStyle::State_HasFocus;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(focused ? QPen(QColor("#97cbbf"), 1.2) : Qt::NoPen);
        p->setBrush(active ? QColor("#2b5660") : (option.state & QStyle::State_MouseOver
            ? QColor("#244954") : QColor(Qt::transparent)));
        p->drawRoundedRect(row, 9, 9);
        const auto color = active ? QColor("#dff6ed") : QColor("#b1caCD");
        const auto elapsed = index.data(SectionWorkdayElapsed).toString();
        const bool running = WorkdayState(index.data(SectionWorkdayStatus).toInt()) == WorkdayState::Running;
        const auto clockColor = running ? QColor("#a6e3cb") : color;
        const int iconTop = row.top() + (elapsed.isEmpty() ? 7 : 3);
        paintGlyph(*p, glyphs[index.row()], QRectF(row.center().x() - 10, iconTop, 21, 21), clockColor);
        p->setPen(color);
        p->setFont(workspace::rowFont(bodyTextPixels, active));
        const int captionTop = elapsed.isEmpty() ? 33 : 25;
        p->drawText(row.adjusted(2, captionTop, -2, -4), Qt::AlignHCenter | Qt::AlignTop, caption);
        if (!elapsed.isEmpty()) {
            p->setPen(clockColor);
            p->setFont(workspace::rowFont(secondaryTextPixels, true));
            p->drawText(row.adjusted(2, 43, -2, -1), Qt::AlignHCenter | Qt::AlignTop, elapsed);
        }
        if (running) {
            p->setPen(Qt::NoPen);
            p->setBrush(clockColor);
            p->drawEllipse(QRect(row.center().x() + 10, iconTop + 1, 5, 5));
        }
        const auto count = index.data(SectionUnreadCount).toLongLong();
        if (count > 0) {
            const auto text = count > 99 ? QStringLiteral("99+") : QString::number(count);
            p->setFont(workspace::rowFont(secondaryTextPixels, true));
            const int width = qMax(19, p->fontMetrics().horizontalAdvance(text) + 6);
            const QRect badge(row.right() - width, row.top(), width, 19);
            p->setPen(Qt::NoPen);
            p->setBrush(QColor("#aa4c58"));
            p->drawRoundedRect(badge, 7, 7);
            p->setPen(Qt::white);
            p->drawText(badge, Qt::AlignCenter, text);
        }
        p->restore();
    }
};

class AvatarBadge final : public QWidget {
public:
    explicit AvatarBadge(QWidget *parent) : QWidget(parent) { setFixedSize(35, 35); }

    void setTitle(const QString &title, workspace::AvatarKind kind = workspace::AvatarKind::Person,
                  const QColor &projectColor = {}, const QString &projectVariant = {}, const QString &projectType = {})
    {
        m_title = title;
        m_kind = kind;
        m_projectColor = projectColor;
        m_projectVariant = projectVariant;
        m_projectType = projectType;
        m_photo = {};
        setVisible(!title.isEmpty());
        update();
    }
    void setPhoto(const QPixmap &photo)
    {
        m_photo = photo;
        update();
    }
    void setOnline(bool online)
    {
        m_online = online;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        workspace::paintAvatar(painter, rect().adjusted(0, 0, -1, -1), m_title, m_kind, m_photo,
                               m_projectColor, m_projectVariant, m_projectType);
        if (m_online && m_kind == workspace::AvatarKind::Person)
            workspace::paintOnlineDot(painter, rect().adjusted(0, 0, -1, -1));
    }

private:
    QString m_title;
    workspace::AvatarKind m_kind = workspace::AvatarKind::Person;
    QPixmap m_photo;
    QColor m_projectColor;
    QString m_projectVariant;
    QString m_projectType;
    bool m_online = false;
};

class PinnedMessageBar final : public QWidget {
public:
    explicit PinnedMessageBar(QWidget *parent) : QWidget(parent)
    {
        setObjectName(QStringLiteral("pinnedMessageBar"));
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setStyleSheet(QStringLiteral(R"(
            QWidget#pinnedMessageBar QPushButton {
                text-align: left; padding: 5px 10px; margin-bottom: 2px;
                color: #215c70; background: #eaf3ef; border: 1px solid #becec7; border-radius: 4px;
            }
            QWidget#pinnedMessageBar QPushButton#choosePinnedMessage { text-align: center; padding: 5px 0; }
            QWidget#pinnedMessageBar QPushButton:hover { background: #dfede7; }
            QWidget#pinnedMessageBar QPushButton:focus { border-color: #215c70; }
        )"));
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(2);
        m_body = new QPushButton(this);
        m_body->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        m_body->setAccessibleName(QStringLiteral("Закреплённые сообщения"));
        m_body->setAutoDefault(false);
        m_body->setCursor(Qt::PointingHandCursor);
        layout->addWidget(m_body, 1);
        m_selector = new QPushButton(QStringLiteral("▾"), this);
        m_selector->setObjectName(QStringLiteral("choosePinnedMessage"));
        m_selector->setAccessibleName(QStringLiteral("Выбрать закреплённое сообщение"));
        m_selector->setToolTip(m_selector->accessibleName());
        m_selector->setAutoDefault(false);
        m_selector->setCursor(Qt::PointingHandCursor);
        m_selector->setFixedWidth(32);
        m_selector->hide();
        layout->addWidget(m_selector);
        connect(m_body, &QPushButton::clicked, this, [this] {
            if (m_onOpen && !m_messages.isEmpty())
                m_onOpen(QString(m_messages.front().id));
        });
        connect(m_selector, &QPushButton::clicked, this, [this] { choosePinned(); });
        hide();
    }

    void setMessages(const QVector<Message> &messages)
    {
        if (m_menu)
            m_menu->close();
        m_messages.clear();
        for (const auto &message : messages)
            if (!message.id.isEmpty())
                m_messages.append(message);
        m_selector->setVisible(m_messages.size() > 1);
        setVisible(!m_messages.isEmpty());
        refreshText();
    }
    void setOnOpen(std::function<void(const QString &)> handler) { m_onOpen = std::move(handler); }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        refreshText();
    }

private:
    static QString preview(const Message &message)
    {
        const auto text = workspace::markupPlainText(message.text).simplified();
        return text.isEmpty() ? QStringLiteral("Сообщение №%1").arg(message.id) : text.left(180);
    }
    void refreshText()
    {
        if (m_messages.isEmpty())
            return;
        const auto title = m_messages.size() == 1
            ? QStringLiteral("Закреплено · %1").arg(preview(m_messages.front()))
            : QStringLiteral("Закреплённые сообщения · %1 · %2").arg(m_messages.size()).arg(preview(m_messages.front()));
        const int reserve = m_selector->isHidden() ? 0 : m_selector->width() + 2;
        m_body->setText(m_body->fontMetrics().elidedText(title, Qt::ElideRight, qMax(1, width() - reserve - 26)));
        m_body->setToolTip(QStringLiteral("<qt>%1</qt>").arg(title.toHtmlEscaped()));
        m_body->setAccessibleDescription(preview(m_messages.front()));
    }
    void choosePinned()
    {
        if (!m_onOpen || m_messages.size() < 2 || m_menu)
            return;
        auto *menu = new QMenu(this);
        m_menu = menu;
        connect(menu, &QMenu::aboutToHide, this, [this, menu] {
            m_menu.clear();
            menu->deleteLater();
        });
        for (const auto &message : m_messages) {
            auto *action = menu->addAction(QStringLiteral("%1 · №%2").arg(preview(message), message.id));
            connect(action, &QAction::triggered, this, [this, id = message.id] {
                if (m_onOpen)
                    m_onOpen(id);
            });
        }
        menu->popup(m_selector->mapToGlobal(m_selector->rect().bottomLeft()));
    }

    QPushButton *m_body;
    QPushButton *m_selector;
    QVector<Message> m_messages;
    QPointer<QMenu> m_menu;
    std::function<void(const QString &)> m_onOpen;
};

class ConversationCanvas final : public QWidget {
public:
    explicit ConversationCanvas(QWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_OpaquePaintEvent);
        m_tile = QPixmap(32, 32);
        m_tile.fill(Qt::transparent);
        QPainter painter(&m_tile);
        painter.setPen(QColor("#d8e6de"));
        painter.drawPoint(4, 4);
        painter.drawPoint(20, 20);
        painter.setPen(QColor("#dbe8e0"));
        painter.drawLine(19, 20, 21, 20);
        painter.drawLine(20, 19, 20, 21);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor("#e5efe9"));
        painter.drawTiledPixmap(rect(), m_tile);
    }

private:
    QPixmap m_tile;
};

class HistoryList final : public QListWidget {
public:
    struct VisibleMessages {
        QStringList incoming;
        QStringList all;
    };

    explicit HistoryList(QWidget *parent) : QListWidget(parent)
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

    void setOnViewportChanged(std::function<void()> handler) { m_onViewportChanged = std::move(handler); }
    bool showingLiveHistory() const { return m_liveReadHistory && !m_browsingOlder; }
    void setLiveReadHistory(bool live)
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

    void setOnBrowsingOlder(std::function<void(bool)> handler) { m_onBrowsingOlder = std::move(handler); }
    void returnToLatest()
    {
        if (m_browsingOlder)
            setLiveReadHistory(false);
    }

    QWidget *olderButton(QWidget *parent)
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

    void setOnOlder(std::function<void(const QString &)> handler) { m_onOlder = std::move(handler); }

    void resetHistory(const QString &scope = {})
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

    void showContext(const QString &scope, QLabel *state, const QVector<Message> &messages)
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

    void showHistory(const QString &scope, QLabel *state, const QVector<Message> &latest)
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

    bool addOlder(const QString &scope, const QString &before, const QVector<Message> &messages, bool hasMore)
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

    void removeHistory(const QStringList &ids, qint64 through)
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

    void olderFailed(const QString &scope, const QString &before, const QString &reason)
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

    void confirmMessageReaction(const QString &id, const QString &code, bool present)
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

    void confirmMessageEdit(const QString &id, const QString &text)
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

    void confirmRead(const QStringList &ids, qint64 readThrough)
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

    void setReplyActions(std::function<void(const ChatReply &)> reply, std::function<bool()> allowed,
                         std::function<void(const QString &)> navigate)
    {
        m_onReply = std::move(reply);
        m_replyAllowed = std::move(allowed);
        m_onOriginal = std::move(navigate);
        static_cast<workspace::RowPainter *>(itemDelegate())->setReplyNavigation(m_onOriginal);
    }

    void setMessageMenu(std::function<void(QMenu &, const QPersistentModelIndex &, const QString &)> handler)
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

    void setReactionActions(std::function<void(const QModelIndex &, const QString &, const QPoint &)> action,
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

    bool focusOnMessage(const QString &id)
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

    VisibleMessages visibleMessageIds() const
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

    void setComposer(QPlainTextEdit *composer)
    {
        m_composer = composer;
        setAttribute(Qt::WA_InputMethodEnabled, composer != nullptr);
    }

    void refreshRowSizes()
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

protected:
    void updateGeometries() override
    {
        const bool follow = m_liveReadHistory && !m_browsingOlder && atLatest();
        QListWidget::updateGeometries();
        // Parent layout can resize the viewport after the first scrollToBottom().
        if (follow)
            verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    }

    void mouseMoveEvent(QMouseEvent *event) override
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

    void mousePressEvent(QMouseEvent *event) override
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

    void mouseReleaseEvent(QMouseEvent *event) override
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

    void leaveEvent(QEvent *event) override
    {
        clearMessageHover();
        QListWidget::leaveEvent(event);
    }

    bool viewportEvent(QEvent *event) override
    {
        if (event->type() == QEvent::Leave && m_messageActions && m_messageActions->isVisible() &&
            !m_hoverActionsBounds.contains(viewport()->mapFromGlobal(QCursor::pos())))
            clearMessageHover();
        return QListWidget::viewportEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
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

    void contextMenuEvent(QContextMenuEvent *event) override
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

    bool showMessageMenu(const QPersistentModelIndex &index, const QRect &anchor, const QString &href = {})
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

    void showEvent(QShowEvent *event) override
    {
        QListWidget::showEvent(event);
        scheduleAlignment();
    }

    void hideEvent(QHideEvent *event) override
    {
        m_wheelAnimation.stop();
        if (m_openMessageMenu)
            m_openMessageMenu->close();
        clearMessageHover();
        QListWidget::hideEvent(event);
    }

    void focusOutEvent(QFocusEvent *event) override
    {
        if (event->reason() == Qt::MouseFocusReason && (!m_openMessageMenu || !m_openMessageMenu->isVisible())) {
            if (auto *delegate = textDelegate())
                delegate->clearTextSelection();
        }
        QListWidget::focusOutEvent(event);
    }

    void keyboardSearch(const QString &) override {}

    void keyPressEvent(QKeyEvent *event) override
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

    void inputMethodEvent(QInputMethodEvent *event) override
    {
        sendToComposer(event);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QListWidget::resizeEvent(event);
        scheduleAlignment();
    }

    void wheelEvent(QWheelEvent *event) override
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

private:
    workspace::RowPainter *textDelegate() const
    {
        return dynamic_cast<workspace::RowPainter *>(itemDelegate());
    }

    bool pickMessageText(const QModelIndex &index, const QPoint &point, workspace::RowPainter::TextPick pick)
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

    void copyMessageText(const QModelIndex &index = {}) const
    {
        if (const auto *delegate = textDelegate()) {
            const auto selected = delegate->selectedText(index);
            if (!selected.isEmpty())
                QApplication::clipboard()->setText(selected);
        }
    }

    bool handleTextSelectionKey(const QKeyEvent *event)
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

    void resumeReadAfterJump()
    {
        if (!m_readPausedByJump)
            return;
        m_readPausedByJump = false;
        if (m_onViewportChanged)
            m_onViewportChanged();
    }

    void updateStickyDay()
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

    static constexpr qsizetype maxHistoryRows = tunables::kRetainedChatMessages;

    bool atLatest() const { return verticalScrollBar()->value() == verticalScrollBar()->maximum(); }

    bool pagingReady() const { return m_liveReadHistory || m_browsingOlder; }

    void clearHistoryWindow()
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

    void setBrowsingOlder(bool active)
    {
        if (m_browsingOlder == active)
            return;
        m_browsingOlder = active;
        if (m_onBrowsingOlder)
            m_onBrowsingOlder(active);
    }

    GlyphButton *createMessageHoverButton(Glyph glyph, const QString &name)
    {
        auto *button = new GlyphButton(glyph, name, viewport());
        constexpr int size = workspace::RowPainter::messageActionSize;
        button->setFixedSize(size, size);
        button->setFocusPolicy(Qt::NoFocus);
        button->setCursor(Qt::PointingHandCursor);
        button->hide();
        return button;
    }

    void hoverMessage(const QPoint &point)
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

    void clearMessageHover()
    {
        m_hoverMessage = QPersistentModelIndex();
        if (m_messageActions)
            m_messageActions->hide();
        if (m_messageReaction)
            m_messageReaction->hide();
    }

    bool historyRemoved(qint64 id) const
    {
        return id > 0 && (id <= m_removedHistoryThrough || m_removedHistoryIds.contains(id));
    }

    static QString oldestId(const QVector<Message> &messages)
    {
        qint64 oldest = 0;
        for (const auto &message : messages) {
            const auto id = message.id.toLongLong();
            if (id > 0 && (oldest == 0 || id < oldest))
                oldest = id;
        }
        return oldest > 0 ? QString::number(oldest) : QString{};
    }

    qint64 olderCursor() const
    {
        auto cursor = oldestId(m_history).toLongLong();
        if (m_scannedOlder > 0)
            cursor = cursor > 0 ? qMin(cursor, m_scannedOlder) : m_scannedOlder;
        return cursor;
    }

    void requestOlder()
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

    void updateOlderButton()
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

    void renderHistory(bool preserve)
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

    bool openFocusedMedia()
    {
        auto *entry = currentItem();
        if (!entry)
            return false;
        auto *delegate = static_cast<workspace::RowPainter *>(itemDelegate());
        if (!delegate)
            return false;
        return delegate->activateMedia(model()->index(row(entry), 0));
    }

    void sendToComposer(QEvent *event)
    {
        if (m_composer && m_composer->isVisible() && m_composer->isEnabled() && !m_composer->isReadOnly()) {
            m_composer->setFocus(Qt::OtherFocusReason);
            QCoreApplication::sendEvent(m_composer, event);
        }
        event->accept();
    }

    void scheduleAlignment()
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

    QVariantAnimation m_wheelAnimation;
    bool m_olderOnWheelEnd = false;
    bool m_queued = false;
    bool m_liveReadHistory = false;
    bool m_readPausedByJump = false;
    QLabel *m_stickyDay = nullptr;
    bool m_browsingOlder = false;
    bool m_hasOlder = true;
    bool m_failedOlder = false;
    QString m_historyScope;
    QString m_pendingOlder;
    QVector<Message> m_history;
    QSet<qint64> m_removedHistoryIds;
    qint64 m_removedHistoryThrough = 0;
    qint64 m_scannedOlder = 0;
    qint64 m_latestFirst = 0;
    qint64 m_latestLast = 0;
    QPointer<QLabel> m_historyState;
    QPushButton *m_olderButton = nullptr;
    QLabel *m_olderProgress = nullptr;
    std::function<void(const QString &)> m_onOlder;
    std::function<void(bool)> m_onBrowsingOlder;
    QPointer<QPlainTextEdit> m_composer;
    std::function<void()> m_onViewportChanged;
    std::function<void(const ChatReply &)> m_onReply;
    std::function<bool()> m_replyAllowed;
    std::function<void(const QString &)> m_onOriginal;
    std::function<void(QMenu &, const QPersistentModelIndex &, const QString &)> m_messageMenu;
    QPointer<QMenu> m_openMessageMenu;
    bool m_pickingMessageText = false;
    QPoint m_textPressPoint;
    QString m_pressedTextLink;
    QPersistentModelIndex m_hoverMessage;
    QRect m_hoverBounds;
    GlyphButton *m_messageActions = nullptr;
    GlyphButton *m_messageReaction = nullptr;
    QRect m_hoverActionsBounds;
};

class EmojiTile final : public QPushButton {
public:
    EmojiTile(const QString &glyph, const QString &name, QWidget *parent)
        : QPushButton(parent), m_glyph(glyph)
    {
        setAccessibleName(name);
        setToolTip(name);
        setAutoDefault(false);
        setFixedSize(43, 42);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (underMouse() || hasFocus() || isDown()) {
            painter.setPen(hasFocus() ? QPen(workspace::Accent, 1.1) : Qt::NoPen);
            painter.setBrush(QColor("#e8f2ed"));
            painter.drawRoundedRect(rect().adjusted(2, 2, -2, -2), 7, 7);
        }
        auto font = workspace::rowFont(28);
        font.setFamilies({QStringLiteral("Noto Color Emoji"), QStringLiteral("Segoe UI Emoji"),
                          QStringLiteral("Apple Color Emoji"), font.family()});
        painter.setFont(font);
        painter.setPen(workspace::Ink);
        painter.drawText(rect(), Qt::AlignCenter, m_glyph);
    }

private:
    QString m_glyph;
};

class EmojiPanel final : public QFrame {
public:
    EmojiPanel(QWidget *host, QPushButton *anchor, QPlainTextEdit *input)
        : QFrame(host), m_host(host), m_anchor(anchor), m_input(input)
    {
        setObjectName(QStringLiteral("emojiPanel"));
        setAccessibleName(QStringLiteral("Выбор эмодзи"));
        setStyleSheet(QStringLiteral(R"(
            QFrame#emojiPanel { background: #fffefa; border: 1px solid #aac6bd; border-radius: 11px; }
            QFrame#emojiPanel QPushButton#emojiCategory {
                background: transparent; padding: 5px 9px; border: 1px solid transparent; font-size: 14px;
            }
            QFrame#emojiPanel QPushButton#emojiCategory:checked { background: #e2f0ea; color: #164c51; }
            QFrame#emojiPanel QPushButton#emojiCategory:focus { border-color: #215c70; }
        )"));
        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(10, 8, 10, 10);
        root->setSpacing(6);
        auto *header = new QHBoxLayout;
        auto *title = workspace::label(QStringLiteral("Эмодзи"), "sectionTitle", this);
        header->addWidget(title, 1);
        auto *close = new GlyphButton(Glyph::Close, QStringLiteral("Закрыть выбор эмодзи"), this);
        close->setFixedSize(30, 29);
        connect(close, &QPushButton::clicked, this, [this] { dismiss(true); });
        header->addWidget(close);
        root->addLayout(header);

        auto *navigation = new QHBoxLayout;
        navigation->setSpacing(3);
        auto *categories = new QButtonGroup(this);
        const QStringList labels = {QStringLiteral("Эмоции"), QStringLiteral("Жесты"), QStringLiteral("Разное")};
        for (int i = 0; i < labels.size(); ++i) {
            auto *category = new QPushButton(labels[i], this);
            category->setObjectName(QStringLiteral("emojiCategory"));
            category->setCheckable(true);
            category->setAutoDefault(false);
            category->setChecked(i == 0);
            categories->addButton(category, i);
            navigation->addWidget(category);
        }
        navigation->addStretch(1);
        root->addLayout(navigation);
        connect(categories, &QButtonGroup::idClicked, this, [this](int category) { showCategory(category); });
        m_grid = new QGridLayout;
        m_grid->setSpacing(3);
        root->addLayout(m_grid);
        showCategory(0);
        hide();
    }

    ~EmojiPanel() override
    {
        if (qApp)
            qApp->removeEventFilter(this);
    }

    void openPanel()
    {
        reposition();
        show();
        raise();
        m_anchor->setChecked(true);
        qApp->installEventFilter(this);
        if (m_first)
            m_first->setFocus(Qt::OtherFocusReason);
    }

    void dismiss(bool focus = false)
    {
        if (isHidden())
            return;
        qApp->removeEventFilter(this);
        hide();
        m_anchor->setChecked(false);
        if (focus)
            m_input->setFocus(Qt::OtherFocusReason);
    }

protected:
    bool eventFilter(QObject *target, QEvent *event) override
    {
        if (target == m_host && event->type() == QEvent::Hide) {
            dismiss();
            return false;
        }
        if (!isVisible())
            return false;
        const auto *widget = qobject_cast<QWidget *>(target);
        if (event->type() == QEvent::MouseButtonPress) {
            if (widget && widget != this && !isAncestorOf(widget) &&
                widget != m_anchor && !m_anchor->isAncestorOf(widget))
                dismiss();
        } else if (event->type() == QEvent::KeyPress && widget && widget->window() == m_host->window()) {
            if (static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
                dismiss(true);
                return true;
            }
        } else if (target == m_anchor && event->type() == QEvent::EnabledChange && !m_anchor->isEnabled()) {
            dismiss();
        } else if (target == m_host->window() && event->type() == QEvent::WindowDeactivate) {
            dismiss();
        } else if ((target == m_anchor || target == m_host) &&
                   (event->type() == QEvent::Resize || event->type() == QEvent::Move)) {
            reposition();
        }
        return false;
    }

private:
    struct Emoji { const char *glyph; const char *name; };

    void showCategory(int category)
    {
        static constexpr Emoji contents[][12] = {
            {{"😀", "Улыбка"}, {"😄", "Радость"}, {"😂", "Смех"}, {"🙂", "Лёгкая улыбка"},
             {"😉", "Подмигивание"}, {"😍", "Влюблённость"}, {"😎", "В очках"}, {"🤔", "Размышление"},
             {"😮", "Удивление"}, {"😢", "Грусть"}, {"😭", "Плач"}, {"😡", "Злость"}},
            {{"👍", "Нравится"}, {"👎", "Не нравится"}, {"👏", "Аплодисменты"}, {"🙌", "Поднятые руки"},
             {"🤝", "Рукопожатие"}, {"🙏", "Спасибо"}, {"👋", "Приветствие"}, {"✌️", "Победа"},
             {"💪", "Сила"}, {"👌", "Хорошо"}, {"🤞", "Скрещённые пальцы"}, {"🫶", "Сердце руками"}},
            {{"❤️", "Сердце"}, {"🎉", "Праздник"}, {"🔥", "Огонь"}, {"✅", "Отметка"},
             {"❌", "Крестик"}, {"⚠️", "Внимание"}, {"💡", "Идея"}, {"📌", "Кнопка"},
             {"📅", "Календарь"}, {"🚀", "Ракета"}, {"☕", "Кофе"}, {"🍀", "Клевер"}},
        };
        while (auto *item = m_grid->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        int i = 0;
        for (const auto &emoji : contents[category]) {
            const auto glyph = QString::fromUtf8(emoji.glyph);
            auto *tile = new EmojiTile(glyph, QString::fromUtf8(emoji.name), this);
            if (i == 0)
                m_first = tile;
            m_grid->addWidget(tile, i / 6, i % 6);
            connect(tile, &QPushButton::clicked, this, [this, glyph] {
                if (!m_input->isReadOnly()) {
                    m_input->insertPlainText(glyph);
                    m_input->setFocus(Qt::OtherFocusReason);
                }
            });
            ++i;
        }
    }

    void reposition()
    {
        const auto size = sizeHint();
        const auto point = m_anchor->mapTo(m_host, QPoint());
        const int x = qBound(4, point.x() + m_anchor->width() - size.width(),
                             qMax(4, m_host->width() - size.width() - 4));
        setGeometry(QRect(QPoint(x, qMax(4, point.y() - size.height() - 8)), size));
    }

    QWidget *m_host;
    QPushButton *m_anchor;
    QPlainTextEdit *m_input;
    QGridLayout *m_grid = nullptr;
    EmojiTile *m_first = nullptr;
};

inline bool validChatClipboardImageSize(const QSize &size)
{
    constexpr qint64 maxPixels = 12 * 1024 * 1024;
    return !size.isEmpty() && size.width() <= 8192 && size.height() <= 8192
        && qint64(size.width()) * size.height() <= maxPixels;
}

class ChatTextEdit final : public QPlainTextEdit {
public:
    explicit ChatTextEdit(QWidget *parent) : QPlainTextEdit(parent)
    {
        setFixedHeight(singleLineHeight);
        connect(this, &QPlainTextEdit::textChanged, this, [this] { scheduleHeightUpdate(); });
    }
    bool composing() const { return m_preedit; }
    std::function<void()> userEdited;
    std::function<bool()> mayPasteImage;
    std::function<void(const QImage &)> imagePasted;

    void setSingleLinePlaceholder(const QString &text)
    {
        m_fullPlaceholder = text;
        updatePlaceholder();
    }

protected:
    bool canInsertFromMimeData(const QMimeData *source) const override
    {
        const bool image = source->hasImage() || source->hasFormat(QStringLiteral("image/png"));
        return (acceptsImagePaste() && image) || QPlainTextEdit::canInsertFromMimeData(source);
    }

    void insertFromMimeData(const QMimeData *source) override
    {
        const bool rawPng = source->hasFormat(QStringLiteral("image/png"));
        if (acceptsImagePaste() && (rawPng || source->hasImage())) {
            QImage image;
            if (rawPng) {
                const auto bytes = source->data(QStringLiteral("image/png"));
                if (!bytes.isEmpty() && bytes.size() <= tunables::kChatClipboardPngBytes) {
                    QBuffer buffer;
                    buffer.setData(bytes);
                    buffer.open(QIODevice::ReadOnly);
                    QImageReader reader(&buffer, "png");
                    if (validChatClipboardImageSize(reader.size()))
                        image = reader.read();
                }
            } else {
                const auto data = source->imageData();
                image = qvariant_cast<QImage>(data);
                if (image.isNull())
                    image = qvariant_cast<QPixmap>(data).toImage();
            }
            if (!image.isNull() || !(source->hasText() || source->hasHtml())) {
                imagePasted(image);
                return;
            }
        }
        QPlainTextEdit::insertFromMimeData(source);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QPlainTextEdit::resizeEvent(event);
        updatePlaceholder();
        if (event->size().width() != event->oldSize().width())
            scheduleHeightUpdate();
    }

    void changeEvent(QEvent *event) override
    {
        QPlainTextEdit::changeEvent(event);
        if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
            updatePlaceholder();
            scheduleHeightUpdate();
        }
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        const auto revision = document()->revision();
        QPlainTextEdit::keyPressEvent(event);
        if (userEdited && !m_preedit && !document()->isEmpty() && document()->revision() != revision)
            userEdited();
    }

    void inputMethodEvent(QInputMethodEvent *event) override
    {
        m_preedit = !event->preeditString().isEmpty();
        const auto revision = document()->revision();
        QPlainTextEdit::inputMethodEvent(event);
        if (userEdited && !event->commitString().isEmpty() && !document()->isEmpty()
            && document()->revision() != revision)
            userEdited();
    }

private:
    bool acceptsImagePaste() const
    {
        return !isReadOnly() && imagePasted && mayPasteImage && mayPasteImage();
    }

    static constexpr int singleLineHeight = 43;
    static constexpr int maximumVisibleLines = 6;

    void scheduleHeightUpdate()
    {
        if (m_heightUpdatePending)
            return;
        m_heightUpdatePending = true;
        QTimer::singleShot(0, this, [this] {
            m_heightUpdatePending = false;
            int lines = 0;
            for (auto block = document()->begin(); block.isValid() && lines < maximumVisibleLines; block = block.next()) {
                blockBoundingRect(block);
                lines += qMax(1, block.layout()->lineCount());
            }
            const int target = singleLineHeight + (qBound(1, lines, maximumVisibleLines) - 1) * fontMetrics().lineSpacing();
            if (target != minimumHeight())
                setFixedHeight(target);
        });
    }

    void updatePlaceholder()
    {
        const int width = qMax(0, qFloor(viewport()->width() - 2 * document()->documentMargin()));
        setPlaceholderText(fontMetrics().elidedText(m_fullPlaceholder, Qt::ElideRight, width));
    }

    QString m_fullPlaceholder;
    bool m_preedit = false;
    bool m_heightUpdatePending = false;
};

class ChatComposer final : public QFrame {
public:
    ChatComposer(QWidget *host, const QString &inputName, const QString &emojiName, const QString &fileName,
                 const QString &sendName)
        : QFrame(host)
    {
        setObjectName(QStringLiteral("chatComposer"));
        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(9, 6, 9, 6);
        outer->setSpacing(4);
        m_typing = workspace::label(QStringLiteral("Печатает…"), "quiet", this);
        m_typing->setTextFormat(Qt::PlainText);
        m_typing->setAccessibleName(QStringLiteral("Индикатор набора сообщения"));
        outer->addWidget(m_typing);
        m_typing->hide();
        m_typingExpiry.setSingleShot(true);
        connect(&m_typingExpiry, &QTimer::timeout, this, [this] { setTypingIndicator(m_typingUser, 0); });
        m_typingDelay.setSingleShot(true);
        m_typingDelay.setInterval(5000);
        connect(&m_typingDelay, &QTimer::timeout, this, [this] {
            if (m_typingScope && m_sendTyping && m_typingScope() == m_pendingTyping && !m_input->document()->isEmpty())
                m_sendTyping(m_pendingTyping);
        });
        m_quote = new QFrame(this);
        m_quote->setObjectName(QStringLiteral("composerReply"));
        m_quote->setStyleSheet(QStringLiteral("QFrame#composerReply {background:#e8f1ee;border-left:3px solid #54877a;border-radius:5px;}"));
        auto *quoteRow = new QHBoxLayout(m_quote);
        quoteRow->setContentsMargins(8, 2, 4, 2);
        m_quoteText = workspace::label({}, "quiet", m_quote);
        m_quoteText->setAccessibleName(QStringLiteral("Цитата в черновике"));
        m_quoteText->setWordWrap(false);
        m_quoteText->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        quoteRow->addWidget(m_quoteText, 1);
        auto *cancel = new GlyphButton(Glyph::Close, QStringLiteral("Отменить ответ"), m_quote);
        quoteRow->addWidget(cancel);
        connect(cancel, &QPushButton::clicked, this, [this] {
            if (!cancelMessageEdit())
                setReply({});
            m_input->setFocus(Qt::OtherFocusReason);
        });
        m_cancelQuote = cancel;
        outer->addWidget(m_quote);
        m_quote->hide();

        m_imageRow = new QFrame(this);
        m_imageRow->setObjectName(QStringLiteral("composerPastedImage"));
        m_imageRow->setStyleSheet(QStringLiteral("QFrame#composerPastedImage {background:#e8f1ee;border-radius:5px;}"));
        auto *imageRow = new QHBoxLayout(m_imageRow);
        imageRow->setContentsMargins(8, 4, 4, 4);
        m_imagePreview = new QLabel(m_imageRow);
        m_imagePreview->setAccessibleName(QStringLiteral("Изображение из буфера"));
        m_imagePreview->setAlignment(Qt::AlignCenter);
        m_imagePreview->setFixedSize(80, 52);
        imageRow->addWidget(m_imagePreview);
        auto *filename = workspace::label(pastedImageName(), "quiet", m_imageRow);
        filename->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        imageRow->addWidget(filename, 1);
        auto *removeImage = new GlyphButton(Glyph::Close, QStringLiteral("Удалить изображение из черновика"), m_imageRow);
        imageRow->addWidget(removeImage);
        connect(removeImage, &QPushButton::clicked, this, [this] {
            clearDraftImage();
            m_input->setFocus(Qt::OtherFocusReason);
        });
        outer->addWidget(m_imageRow);
        m_imageRow->hide();

        auto *layout = new QHBoxLayout;
        layout->setSpacing(4);
        outer->addLayout(layout);
        m_file = new GlyphButton(Glyph::Attach, fileName, this);
        layout->addWidget(m_file, 0, Qt::AlignBottom);
        m_input = new ChatTextEdit(this);
        m_input->mayPasteImage = [this] { return !editingMessage(); };
        m_input->imagePasted = [this](const QImage &image) { stageImage(image); };
        m_input->userEdited = [this] {
            if (editingMessage() || !m_typingScope || m_typingDelay.isActive())
                return;
            m_pendingTyping = m_typingScope();
            if (!m_pendingTyping.isEmpty())
                m_typingDelay.start();
        };
        m_input->setObjectName(QStringLiteral("chatEditor"));
        m_input->setAccessibleName(inputName);
        setPlaceholderText(QStringLiteral("Напишите сообщение"));
        m_input->setToolTip(QStringLiteral("Enter — отправить, Shift+Enter — новая строка"));
        m_input->setTabChangesFocus(true);
        m_input->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_input->setFrameShape(QFrame::NoFrame);
        auto palette = m_input->palette();
        palette.setColor(QPalette::PlaceholderText, workspace::Muted);
        m_input->setPalette(palette);
        layout->addWidget(m_input, 1);
        m_mentions = new PeopleSuggestions(m_input);
        m_mentions->setOnChosen([this](const Person &person) { insertMention(person); });
        m_mentionCheck.setSingleShot(true);
        connect(&m_mentionCheck, &QTimer::timeout, this, [this] {
            const auto found = currentCandidate();
            if (found == m_candidate)
                return;
            m_candidate = found;
            if (found.start < 0)
                m_mentions->dismiss();
            else
                m_mentions->search(found.query);
        });
        connect(m_input, &QPlainTextEdit::textChanged, this, [this] { scheduleMentionSearch(); });
        connect(m_input, &QPlainTextEdit::cursorPositionChanged, this, [this] { scheduleMentionSearch(); });
        m_emoji = new GlyphButton(Glyph::Emoji, emojiName, this);
        m_emoji->setCheckable(true);
        layout->addWidget(m_emoji, 0, Qt::AlignBottom);
        m_send = new GlyphButton(Glyph::Send, sendName, this, true);
        m_sendName = sendName;
        layout->addWidget(m_send, 0, Qt::AlignBottom);

        connect(m_emoji, &QPushButton::clicked, this, [this, host] {
            if (!m_panel)
                m_panel = new EmojiPanel(host, m_emoji, m_input);
            if (m_panel->isVisible())
                m_panel->dismiss(true);
            else
                m_panel->openPanel();
        });
    }

    ~ChatComposer() override { delete m_panel.data(); }

    QPlainTextEdit *input() const { return m_input; }
    void setPlaceholderText(const QString &text) { m_input->setSingleLinePlaceholder(text); }
    QPushButton *file() const { return m_file; }
    QPushButton *emoji() const { return m_emoji; }
    QPushButton *send() const { return m_send; }
    const ChatReply &reply() const { return m_reply; }
    bool hasDraftImage() const { return bool(m_image); }
    void clearDraftImage() { setDraftImage({}); }
    void forgetDraftImages()
    {
        m_preEditDraft.image.reset();
        clearDraftImage();
    }
    void setImageDraftHandlers(std::function<void()> changed, std::function<void(const QString &)> error)
    {
        m_onImageChanged = std::move(changed);
        m_onImageError = std::move(error);
    }
    bool composing() const { return m_input->composing(); }
    bool editingMessage() const { return !m_editId.isEmpty(); }
    bool messageEditPending() const { return m_editPending; }
    bool matchesEdit(const QString &dialog, const QString &id) const
    {
        return editingMessage() && m_editDialog == dialog && m_editId == id;
    }
    void setEditStateChanged(std::function<void()> callback) { m_onEditStateChanged = std::move(callback); }
    void setEditLatest(std::function<bool()> callback) { m_onEditLatest = std::move(callback); }
    void beginMessageEdit(const QString &dialog, const QString &id, const QString &text,
                          std::function<void(const QString &)> submit)
    {
        if (editingMessage()) {
            m_input->setFocus(Qt::OtherFocusReason);
            return;
        }
        m_preEditDraft = draft();
        m_reply = {};
        m_preEditAnchor = m_input->textCursor().anchor();
        m_preEditPosition = m_input->textCursor().position();
        m_editDialog = dialog;
        m_editId = id;
        m_editOriginal = text;
        m_submitEdit = std::move(submit);
        m_send->setGlyph(Glyph::Check);
        m_send->setAccessibleName(QStringLiteral("Сохранить сообщение"));
        m_cancelQuote->setAccessibleName(QStringLiteral("Отменить редактирование сообщения"));
        m_cancelQuote->setToolTip(QStringLiteral("Отменить редактирование (Escape)"));
        m_input->setToolTip(QStringLiteral("Enter — сохранить, Shift+Enter — новая строка, Escape — отменить"));
        setDraft({text, {}, {}});
        m_input->moveCursor(QTextCursor::End);
        m_input->setFocus(Qt::OtherFocusReason);
        updateEditBanner();
    }
    bool canSaveMessageEdit(const QString &text) const
    {
        return !m_editPending && text != m_editOriginal && !text.trimmed().isEmpty() && text.size() <= 20000;
    }
    bool submitMessageEdit()
    {
        if (!editingMessage())
            return false;
        const auto text = wireText();
        if (m_send->isEnabled() && canSaveMessageEdit(text)) {
            const auto submit = m_submitEdit;
            m_editPending = true;
            m_editSubmitted = text;
            updateEditBanner();
            submit(text);
        }
        return true;
    }
    bool cancelMessageEdit()
    {
        if (!editingMessage())
            return false;
        if (!m_editPending)
            finishMessageEdit();
        return true;
    }
    void finishMessageEdit()
    {
        if (!editingMessage())
            return;
        m_editId.clear();
        m_editDialog.clear();
        m_editOriginal.clear();
        m_editSubmitted.clear();
        m_editPending = false;
        m_submitEdit = {};
        m_send->setGlyph(Glyph::Send);
        m_send->setAccessibleName(m_sendName);
        m_cancelQuote->setAccessibleName(QStringLiteral("Отменить ответ"));
        m_cancelQuote->setToolTip(QStringLiteral("Отменить ответ"));
        m_input->setToolTip(QStringLiteral("Enter — отправить, Shift+Enter — новая строка"));
        const auto saved = std::exchange(m_preEditDraft, {});
        setDraft(saved);
        auto cursor = m_input->textCursor();
        const int end = m_input->document()->characterCount() - 1;
        cursor.setPosition(qBound(0, m_preEditAnchor, end));
        cursor.setPosition(qBound(0, m_preEditPosition, end), QTextCursor::KeepAnchor);
        m_input->setTextCursor(cursor);
        updateEditBanner();
    }
    void acceptMessageEdit(const QString &text)
    {
        if (m_editPending && m_editSubmitted == text)
            finishMessageEdit();
    }
    void failMessageEdit()
    {
        if (!m_editPending)
            return;
        m_editPending = false;
        m_editSubmitted.clear();
        updateEditBanner();
    }
    void setTypingActions(std::function<QString()> scope, std::function<void(const QString &)> notify)
    {
        m_typingScope = std::move(scope);
        m_sendTyping = std::move(notify);
    }
    void cancelTyping() { m_typingDelay.stop(); }
    void setTypingIndicator(const QString &user, int duration, const QString &name = {})
    {
        if (duration <= 0 && user != m_typingUser)
            return;
        m_typingUser = duration > 0 ? user : QString();
        if (duration > 0) {
            m_typingExpiry.start(duration);
            setTypingName(user, name);
        } else {
            m_typingExpiry.stop();
        }
        m_typing->setVisible(duration > 0);
    }
    void setTypingName(const QString &user, const QString &name)
    {
        if (m_typingUser != user || !m_typingExpiry.isActive())
            return;
        m_typing->setText(name.isEmpty() ? QStringLiteral("Печатает…") : QStringLiteral("%1 печатает…").arg(name));
    }
    void clearTypingIndicator() { setTypingIndicator(m_typingUser, 0); }
    void setReply(const ChatReply &reply)
    {
        if (editingMessage())
            return;
        m_reply = reply;
        const auto author = reply.author.isEmpty() ? QStringLiteral("Ответ") : QStringLiteral("Ответ · %1").arg(reply.author);
        const auto summary = QStringLiteral("%1 — %2").arg(author, reply.preview);
        m_quoteText->setText(summary);
        setLoadingIndicator(m_quoteText, false);
        m_quoteText->setAccessibleName(QStringLiteral("Цитата в черновике"));
        m_quoteText->setToolTip(QStringLiteral("<qt>%1</qt>").arg(summary.toHtmlEscaped()));
        m_quote->setVisible(!reply.id.isEmpty());
    }
    void setMentionSearchHandler(std::function<void(const QString &, const QString &)> handler)
    {
        dismissMentionSearch();
        m_mentionSearchEnabled = bool(handler);
        m_mentions->setSearchHandler(std::move(handler));
    }
    void setMentionResults(const QString &id, const QVector<Person> &people) { m_mentions->setResults(id, people); }
    void setMentionSearchError(const QString &id) { m_mentions->showRequestError(id); }
    void setMentionAvatars(std::function<QIcon(const Person &)> icon, std::function<void()> onVisible)
    {
        m_mentions->setAvatarIcon(std::move(icon));
        m_mentions->onVisibleChanged = std::move(onVisible);
    }
    QList<QUrl> visibleMentionAvatarUrls() const { return m_mentions->visibleAvatarUrls(); }
    void refreshMentionAvatars()
    {
        if (m_mentions->isVisible())
            m_mentions->refreshAvatars();
    }
    bool handleMentionKey(QKeyEvent *event) { return !m_input->composing() && m_mentions->handleKey(event); }
    bool handleComposerKey(QKeyEvent *event)
    {
        if (handleMentionKey(event))
            return true;
        if (event->key() == Qt::Key_Up && event->modifiers() == Qt::NoModifier
            && !composing() && !editingMessage() && !m_mentions->isVisible()
            && m_input->document()->isEmpty() && m_reply.id.isEmpty() && !hasDraftImage() && m_onEditLatest)
            return m_onEditLatest();
        return event->key() == Qt::Key_Escape && cancelMessageEdit();
    }
    void dismissMentionSearch()
    {
        m_mentionCheck.stop();
        m_mentions->dismiss();
        m_candidate = {};
    }
    ChatDraft draft() const { return {m_input->toPlainText(), m_reply, mentionSpans(), m_image}; }
    QString wireText() const
    {
        auto text = m_input->toPlainText();
        const auto mentions = mentionSpans();
        for (auto i = mentions.crbegin(); i != mentions.crend(); ++i) {
            text.replace(i->start, i->name.size() + 1, QStringLiteral("[USER=%1]%2[/USER]").arg(i->id, i->name));
        }
        return text;
    }
    void setDraft(const ChatDraft &draft)
    {
        cancelTyping();
        setTypingIndicator(m_typingUser, 0);
        dismissMentionSearch();
        m_loadingDraft = true;
        m_input->setPlainText(draft.text);
        m_hasMentions = !draft.mentions.isEmpty();
        const bool undoEnabled = m_input->isUndoRedoEnabled();
        m_input->setUndoRedoEnabled(false);
        for (const auto &mention : draft.mentions) {
            if (mention.start < 0 || draft.text.mid(mention.start, mention.name.size() + 1) != QLatin1Char('@') + mention.name)
                continue;
            QTextCursor cursor(m_input->document());
            cursor.setPosition(mention.start);
            cursor.setPosition(mention.start + mention.name.size() + 1, QTextCursor::KeepAnchor);
            cursor.mergeCharFormat(mentionFormat(mention.id, mention.name));
        }
        m_input->setUndoRedoEnabled(undoEnabled);
        m_loadingDraft = false;
        setReply(draft.reply);
        setDraftImage(draft.image);
    }
    void dismissEmoji()
    {
        if (m_panel)
            m_panel->dismiss();
    }

private:
    void updateEditBanner()
    {
        m_cancelQuote->setEnabled(!m_editPending);
        if (editingMessage()) {
            const auto title = m_editPending ? QStringLiteral("Сохраняем сообщение…")
                                             : QStringLiteral("Редактирование сообщения");
            m_quoteText->setText(title);
            setLoadingIndicator(m_quoteText, m_editPending);
            m_quoteText->setAccessibleName(title);
            m_quoteText->setToolTip(title);
            m_quote->show();
        }
        if (m_onEditStateChanged)
            m_onEditStateChanged();
    }

    struct Candidate {
        int start = -1;
        int end = -1;
        QString query;

        bool operator==(const Candidate &other) const
        {
            return start == other.start && end == other.end && query == other.query;
        }
    };
    static constexpr int MentionId = QTextFormat::UserProperty + 41;
    static constexpr int MentionName = QTextFormat::UserProperty + 42;

    PeopleSuggestions *m_mentions = nullptr;
    Candidate m_candidate;
    QTimer m_mentionCheck;
    bool m_mentionSearchEnabled = false;
    bool m_loadingDraft = false;
    bool m_hasMentions = false;

    static QTextCharFormat mentionFormat(const QString &id, const QString &name)
    {
        QTextCharFormat format;
        format.setForeground(workspace::Accent);
        format.setFontWeight(QFont::DemiBold);
        format.setProperty(MentionId, id);
        format.setProperty(MentionName, name);
        return format;
    }

    QVector<ChatMention> mentionSpans() const
    {
        QVector<ChatMention> result;
        if (!m_hasMentions)
            return result;
        for (auto block = m_input->document()->begin(); block.isValid(); block = block.next()) {
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const auto fragment = it.fragment();
                const auto format = fragment.charFormat();
                const auto id = format.stringProperty(MentionId);
                const auto name = format.stringProperty(MentionName);
                if (!id.isEmpty() && !name.isEmpty() && fragment.text() == QLatin1Char('@') + name)
                    result.append({fragment.position(), id, name});
            }
        }
        return result;
    }

    Candidate currentCandidate() const
    {
        const auto cursor = m_input->textCursor();
        if (m_input->isReadOnly() || m_input->composing() || cursor.hasSelection() || !m_input->hasFocus())
            return {};
        const auto block = cursor.block();
        const int vicinity = qMax(block.position(), cursor.position() - 50);
        auto prefix = cursor;
        prefix.setPosition(vicinity, QTextCursor::KeepAnchor);
        const auto before = prefix.selectedText();
        const int marker = qMax(before.lastIndexOf(QLatin1Char('@')), before.lastIndexOf(QLatin1Char('+')));
        if (marker < 0 || (marker == 0 ? vicinity != block.position() : !before[marker - 1].isSpace()))
            return {};
        const auto query = before.mid(marker + 1);
        static const QRegularExpression name(QStringLiteral("^\\p{L}[\\p{L}\\p{M}\\p{N}_.’'-]*$"));
        if (!query.isEmpty() && !name.match(query).hasMatch())
            return {};
        const int start = vicinity + marker;
        QTextCursor trigger(m_input->document());
        trigger.setPosition(start + 1);
        if (trigger.charFormat().hasProperty(MentionId))
            return {};
        return {start, cursor.position(), query};
    }

    void scheduleMentionSearch()
    {
        if (m_mentionSearchEnabled && !m_loadingDraft && !m_mentionCheck.isActive())
            m_mentionCheck.start(0);
    }

    void insertMention(const Person &person)
    {
        const auto found = currentCandidate();
        const auto id = portalTaskIdentifier(person.id);
        if (found.start < 0 || !(found == m_candidate) || id.isEmpty())
            return;
        auto name = person.name.simplified().left(96);
        name.remove(QLatin1Char('[')).remove(QLatin1Char(']'));
        name = name.simplified();
        if (name.isEmpty())
            return;
        auto cursor = m_input->textCursor();
        cursor.setPosition(found.start);
        cursor.setPosition(found.end, QTextCursor::KeepAnchor);
        m_hasMentions = true;
        cursor.beginEditBlock();
        cursor.insertText(QLatin1Char('@') + name, mentionFormat(id, name));
        cursor.insertText(QStringLiteral(" "), QTextCharFormat{});
        cursor.endEditBlock();
        m_input->setTextCursor(cursor);
        m_input->setFocus(Qt::OtherFocusReason);
        dismissMentionSearch();
    }

    static QString pastedImageName() { return QStringLiteral("Из буфера.png"); }

    void stageImage(const QImage &image)
    {
        const auto failure = [this](const QString &message) {
            if (m_onImageError)
                m_onImageError(message);
        };
        if (image.isNull() || !validChatClipboardImageSize(image.size())) {
            failure(QStringLiteral("Не удалось вставить изображение. Размер или формат не поддерживается."));
            return;
        }
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        QImageWriter writer(&buffer, "png");
        writer.setCompression(60);
        const auto source = image.convertToFormat(image.hasAlphaChannel() ? QImage::Format_ARGB32 : QImage::Format_RGB32);
        if (source.isNull() || !writer.write(source)) {
            failure(QStringLiteral("Не удалось подготовить изображение для отправки"));
            return;
        }
        if (png.size() > tunables::kChatClipboardPngBytes) {
            failure(QStringLiteral("Изображение после сжатия превышает 16 МиБ"));
            return;
        }
        auto directory = std::make_shared<QTemporaryDir>(QDir::temp().filePath(QStringLiteral("bitrix24-chat-XXXXXX")));
        const auto path = directory->filePath(pastedImageName());
        QFile output(path);
        if (!directory->isValid() || !output.open(QIODevice::WriteOnly | QIODevice::NewOnly)
            || output.write(png) != png.size() || !output.flush()) {
            failure(QStringLiteral("Не удалось подготовить изображение для отправки"));
            return;
        }
        output.close();
        auto thumbnail = source.scaled(m_imagePreview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        thumbnail.setDevicePixelRatio(1);
        setDraftImage(std::make_shared<ChatDraftImage>(ChatDraftImage{std::move(directory), path, thumbnail}));
    }

    void setDraftImage(std::shared_ptr<const ChatDraftImage> image)
    {
        if (m_image == image)
            return;
        m_image = std::move(image);
        m_imagePreview->setPixmap(m_image ? QPixmap::fromImage(m_image->thumbnail) : QPixmap{});
        m_imageRow->setVisible(bool(m_image));
        if (m_onImageChanged)
            m_onImageChanged();
    }

    ChatTextEdit *m_input = nullptr;
    QFrame *m_imageRow = nullptr;
    QLabel *m_imagePreview = nullptr;
    std::shared_ptr<const ChatDraftImage> m_image;
    std::function<void()> m_onImageChanged;
    std::function<void(const QString &)> m_onImageError;
    QLabel *m_typing = nullptr;
    QTimer m_typingExpiry;
    QTimer m_typingDelay;
    QString m_typingUser;
    QString m_pendingTyping;
    std::function<QString()> m_typingScope;
    std::function<void(const QString &)> m_sendTyping;
    QPushButton *m_file = nullptr;
    QPushButton *m_emoji = nullptr;
    GlyphButton *m_send = nullptr;
    QPointer<EmojiPanel> m_panel;
    QFrame *m_quote = nullptr;
    QLabel *m_quoteText = nullptr;
    QPushButton *m_cancelQuote = nullptr;
    ChatReply m_reply;
    ChatDraft m_preEditDraft;
    int m_preEditAnchor = 0;
    int m_preEditPosition = 0;
    QString m_sendName;
    QString m_editId;
    QString m_editDialog;
    QString m_editOriginal;
    QString m_editSubmitted;
    bool m_editPending = false;
    std::function<void()> m_onEditStateChanged;
    std::function<bool()> m_onEditLatest;
    std::function<void(const QString &)> m_submitEdit;
};

} // namespace bx::ui
