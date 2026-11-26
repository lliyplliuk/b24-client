#pragma once

#include "core/tunables.h"
#include "ui/chat_reply.h"
#include "ui/glyphs.h"
#include "ui/workspace_style.h"
#include <QPointer>
#include <QVariantAnimation>

class QMenu;
class QPlainTextEdit;

class QPushButton;

namespace bx::ui {

class GlyphButton;

class HistoryList final : public QListWidget {
public:
    struct VisibleMessages {
        QStringList incoming;
        QStringList all;
    };
    explicit HistoryList(QWidget *parent);
    ~HistoryList() override;
    void setOnViewportChanged(std::function<void()> handler);
    bool showingLiveHistory() const;
    void setLiveReadHistory(bool live);
    void setOnBrowsingOlder(std::function<void(bool)> handler);
    void returnToLatest();
    QWidget *olderButton(QWidget *parent);
    void setOnOlder(std::function<void(const QString &)> handler);
    void resetHistory(const QString &scope = {});
    void showContext(const QString &scope, QLabel *state, const QVector<Message> &messages);
    void showHistory(const QString &scope, QLabel *state, const QVector<Message> &latest);
    bool addOlder(const QString &scope, const QString &before, const QVector<Message> &messages, bool hasMore);
    void removeHistory(const QStringList &ids, qint64 through);
    void olderFailed(const QString &scope, const QString &before, const QString &reason);
    void confirmMessageReaction(const QString &id, const QString &code, bool present);
    void confirmMessageEdit(const QString &id, const QString &text);
    void confirmRead(const QStringList &ids, qint64 readThrough);
    void setReplyActions(std::function<void(const ChatReply &)> reply, std::function<bool()> allowed,
                         std::function<void(const QString &)> navigate);
    void setMessageMenu(std::function<void(QMenu &, const QPersistentModelIndex &, const QString &)> handler);
    void setReactionActions(std::function<void(const QModelIndex &, const QString &, const QPoint &)> action,
                            std::function<bool()> allowed);
    bool focusOnMessage(const QString &id);
    VisibleMessages visibleMessageIds() const;
    void setComposer(QPlainTextEdit *composer);
    void refreshRowSizes();

protected:
    void updateGeometries() override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    bool viewportEvent(QEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    bool showMessageMenu(const QPersistentModelIndex &index, const QRect &anchor, const QString &href = {});
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void keyboardSearch(const QString &) override;

    void keyPressEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    workspace::RowPainter *textDelegate() const;
    bool pickMessageText(const QModelIndex &index, const QPoint &point, workspace::RowPainter::TextPick pick);
    void copyMessageText(const QModelIndex &index = {}) const;
    bool handleTextSelectionKey(const QKeyEvent *event);
    void resumeReadAfterJump();
    void updateStickyDay();

    static constexpr qsizetype maxHistoryRows = tunables::kRetainedChatMessages;
    bool atLatest() const;
    bool pagingReady() const;
    void clearHistoryWindow();
    void setBrowsingOlder(bool active);
    GlyphButton *createMessageHoverButton(Glyph glyph, const QString &name);
    void hoverMessage(const QPoint &point);
    void clearMessageHover();
    bool historyRemoved(qint64 id) const;
    static QString oldestId(const QVector<Message> &messages);
    qint64 olderCursor() const;
    void requestOlder();
    void updateOlderButton();
    void renderHistory(bool preserve);
    bool openFocusedMedia();
    void sendToComposer(QEvent *event);
    void scheduleAlignment();

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

} // namespace bx::ui
