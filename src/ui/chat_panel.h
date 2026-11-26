#pragma once

#include "ui/workspace_message_search.h"
#include "core/operations.h"
#include "ui/composer_send_controller.h"

#include <QCache>
#include <QPointer>
#include <QWidget>

class QAbstractItemModel;
class QDialog;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QPersistentModelIndex;
class QPlainTextEdit;
class QPushButton;
class QTabBar;
class QTimer;
class QLabel;

namespace bx {
class WorkspaceMessagePreviews;
namespace ui { class MessageFiles; class TaskCardDialog; class AvatarBadge; class GlyphButton; class HistoryList; class PinnedMessageBar; }

class ChatPanel final : public QWidget {
    Q_OBJECT
public:
    struct Workspace {
        std::function<bool()> writable;
        std::function<bool()> uploadPending;
        std::function<bool()> taskSendPending;
        std::function<bool()> sessionReady;
        std::function<ui::TaskCardDialog *()> card;
        std::function<bool(const QWidget *)> readActiveTask;
        std::function<const QPixmap *(const QUrl &)> avatar;
        std::function<void(const QUrl &)> requestAvatar;
        std::function<void(QListWidget *)> bindHistory;
        std::function<void(const QString &, const QString &)> pickFile;
        std::function<bool(const QString &, bool)> pastedImagePending;
        std::function<void(ui::ComposerSendController &, ui::ChatComposer *, const QString &)> sendFromComposer;
        std::function<QUrl(const QString &)> messageLinkDestination;
        WorkspaceMessagePreviews *previews;
        ui::MessageFiles *files;
    };
    ChatPanel(Backend *backend, bool readOnly, Workspace workspace, QWidget *parent);
    void setSectionActive(bool active);
    void refresh(bool reloadHistory);
    void windowDeactivated();
    void fileUploaded(const QString &scope, const QString &status, const ui::ChatDraft &draft);
    QListWidget *history() const { return m_chatMessages; }
    ui::ChatComposer *composer() const { return m_chatComposer; }
    const QString &currentDialog() const { return m_currentChatId; }
    const QString &linkedTaskId() const { return m_openChatTaskId; }
    void requestVisibleAvatars(const std::function<void(const QUrl &)> &request,
                               const std::function<void(QListWidget *)> &requestVisible) const;
    void updateAvatars(const std::function<void(QListWidget *)> &updateList);
    void scheduleVisibleChatRead();
    void updateComposers();
    void updateChatUnreadBadges();
    void openLinkedChat(const NotificationTarget &target);
    void resetMessageContext();
    void setShowDismissedChats(bool show);
    bool editLatestMessage(const ui::HistoryList *history, const QString &dialog);
    void actOnMessageReaction(const QPersistentModelIndex &message, const QString &dialog, const QString &code,
                              const QPoint &anchor, QWidget *parent);
    void addMessageActions(QMenu &menu, const QPersistentModelIndex &message, const QString &dialog, bool taskChat,
                           const QString &href);
    void resetAccount();
    void showMessageContext(const QString &dialogId, const QString &messageId, const QVector<Message> &messages);
    bool handleFailure(const QString &operation, OperationKind kind, const QStringList &ids,
                              const QString &reason, bool networkFailure, QString &footer, bool &statusPresented);
    bool failMessageContext(const QStringList &ids, const QString &reason);
    void refreshMentionAvatars();
    void clearPendingReactions();

signals:
    void statusChanged(const QString &text, bool loading = false);
    void avatarsRequested();
    void avatarRowsChanged();
    void historyClosed(bool task);
    void taskRequested(const QString &id);
    void sectionRequested();
    void refreshRequested();
    void linkRequested(const QString &href);
    void unreadCountChanged(qint64 count);
    void dialogChanged();
    void sectionActiveChanged();
    void directoryEmpty();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void openMessageViewers(const QString &messageId, const QPoint &anchor);
    void onChatSelected();
    void focusChatComposer();
    void reportVisibleChatRead();
    void updateChatContactPresentation();
    void openChatRow(const QListWidgetItem &item);
    void onSendChat();
    void onChatListLoaded(int scope, const QVector<Chat> &chats);
    void handleChatTyping(const QString &dialog, const QString &user, int duration);
    void rememberTypingName(const QString &user, const QString &name);
    void setChatTabUnreadCount(int scope, qint64 count);
    void onChatsFound(int scope, const QString &query, const QVector<Chat> &chats);
    void requestCurrentChatList();
    void closeCurrentChat(bool scopeChanged = false);
    void onChatScopeChanged(int scope);
    void runSearchChats();
    void clearChatSearch();
    void runMessageSearch();
    void clearMessageSearch();
    void showChatResults(const QVector<Chat> &chats, bool useSearchMode = true, bool backgroundRefresh = false);
    void showChatPinMenu(QListWidget *list, const QPoint &point);
    void openChatMessageContext(const QString &dialogId, const QString &messageId);
    bool focusChatMessage(const QString &id);
    void showMessageResults(const QVector<Message> &messages);
    QVariantMap reactionFrequency() const;
    void incrementReactionFrequency(const QString &code);
    QWidget *messageActionOwner(const QAbstractItemModel *model, const QString &dialog);
    bool beginMessageEdit(const QPersistentModelIndex &message, const QString &dialog);
    std::function<QPersistentModelIndex()> messageActionTarget(const QPersistentModelIndex &message,
                                                              const QString &dialog);
    void recordConfirmedReaction(const QString &dialog, const QString &id, const QString &code);
    void showChatAssets(const QString &dialog, QWidget *parent);
    void promptGroupChat();
    void promptChatMember(const QString &dialog, bool add);
    void showSearchResponse(const QString &scopeId, const QString &query, const QVector<Message> &messages);
    void chatSearchFailed(const QString &scope, const QString &query, const QString &reason);
    void connectBackend();
    bool writable() const { return m_workspace.writable(); }
    Backend *m_backend;
    bool m_readOnly;
    Workspace m_workspace;
    WorkspaceMessageSearch m_historySearch;
    bool m_active = true;
    QString m_reactionAccountScope;
    bool m_showDismissedChats = false;

    QListWidget *m_chats = nullptr;
    QTabBar *m_chatTabs = nullptr;
    QListWidget *m_chatMessages = nullptr;
    QLabel *m_chatTitle = nullptr;
    QLabel *m_chatContactInfo = nullptr;
    QDateTime m_chatContactUntil;
    QString m_chatContactPosition;
    bool m_chatContactIsPerson = false;
    QCache<QString, QString> m_typingNames{64};
    QSet<QString> m_typingNamesPending;
    QTimer *m_chatContactTimer = nullptr;
    ui::PinnedMessageBar *m_chatPinned = nullptr;
    QLabel *m_chatEmpty = nullptr;
    ui::AvatarBadge *m_chatAvatar = nullptr;
    QPushButton *m_openChatTask = nullptr;
    QPushButton *m_latestChat = nullptr;
    QString m_openChatTaskId;
    QString m_contextDialogId;
    QString m_contextMessageId;
    QUrl m_chatAvatarUrl;
    ui::ChatComposer *m_chatComposer = nullptr;
    QPlainTextEdit *m_chatInput = nullptr;
    QPushButton *m_chatSend = nullptr;
    QLineEdit *m_chatSearch = nullptr;
    QLabel *m_chatSearchProgress = nullptr;
    QPushButton *m_chatSearchClear = nullptr;
    QPushButton *m_createGroupChat = nullptr;
    ui::GlyphButton *m_unreadOnlyButton = nullptr;
    bool m_unreadOnlyChats = false;
    QVector<Chat> m_chatSource;
    bool m_chatSourceSearching = false;
    Chat m_retainedTaskChat;
    int m_retainedTaskChatRow = -1;
    bool m_taskChatPressActive = false;
    bool m_taskChatRefreshDeferred = false;
    QTimer *m_taskChatPressEnd = nullptr;
    QLineEdit *m_messageSearch = nullptr;
    QPushButton *m_messageSearchClear = nullptr;
    QPushButton *m_messageSearchToggle = nullptr;
    QPushButton *m_chatEmoji = nullptr;
    QPushButton *m_chatFile = nullptr;
    struct ChatDirectory {
        QVector<Chat> recent;
        QVector<Chat> found;
        Chat linked;
        QString selected;
        QString query;
        QString input;
        bool loaded = false;
    };
    ChatDirectory m_chatDirectories[2];
    bool m_taskChatBadgesRequested = false;
    int m_chatScope = 0;
    bool m_messageSearchActive = false;
    QString m_messageSearchQuery;
    QString m_messageSearchScope;
    QPointer<QDialog> m_chatAssetsDialog;
    QTimer *m_chatSearchTimer = nullptr;
    QTimer *m_messageSearchTimer = nullptr;
    QTimer *m_visibleChatReadTimer = nullptr;
    QSet<QString> m_pendingPins;
    QHash<QString, ui::ChatDraft> m_chatDrafts;
    QSet<QString> m_pendingReactionAdds;
    QString m_currentChatId;
    ui::ComposerSendController m_chatSendController;
};

} // namespace bx
