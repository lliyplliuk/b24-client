#pragma once

#include "core/backend.h"
#include "ui/public_link_preview.h"

#include <QCache>
#include <QPersistentModelIndex>
#include <QPixmap>
#include <QPointer>

class QListWidget;
class QMenu;
class QWidget;

namespace bx {

class NotificationsPanel;
namespace ui { class MessageFiles; class TaskCardDialog; }
namespace workspace { class RowPainter; }

class WorkspaceMessagePreviews final : public QObject {
public:
    using PublicLinkRequest = std::function<bool(const QUrl &, ui::PublicLinkPreviewFetcher::Completion)>;
    struct Workspace {
        std::function<QListWidget *()> chatMessages;
        std::function<QString()> chatId;
        std::function<QString()> chatTaskId;
        std::function<ui::TaskCardDialog *()> taskCard;
        std::function<NotificationsPanel *()> events;
        std::function<bool()> online;
        std::function<bool()> recovering;
        std::function<void(const QUrl &)> requestAvatar;
        std::function<const QPixmap *(const QUrl &)> avatar;
        std::function<void()> scheduleAvatars;
    };

    WorkspaceMessagePreviews(Backend *backend, ui::MessageFiles *files, PublicLinkRequest request,
                             Workspace workspace, QWidget *window);

    void connectBackend();
    void scheduleExternalLinkPreviews();
    void bindLinkCards(QListWidget *list, workspace::RowPainter *delegate);
    void refreshChatMediaRows();
    void loadVisibleChatPreviews();
    void addPublicLinkPreviewActions(QMenu &menu, const QPersistentModelIndex &message,
                                     const std::function<QPersistentModelIndex()> &resolve);
    void scheduleLinkPreviewLayout();
    void invalidateTaskLinkPreview(const QString &taskId);
    void resetLinkPreviews();

private:
    bool isCurrentTaskLink(const QListWidget *history, const QUrl &url) const;
    void requestExternalLinkPreviews(QListWidget *history, qint64 newest);
    bool needsPublicLinkPreview(QListWidget *history, const QString &id, const QUrl &url) const;
    void requestPublicLinkPreview(QListWidget *history, const QString &id, const QUrl &url);
    void loadMessageLinkPreview(const QString &href);
    void refreshLinkPreviewRows();

    Backend *m_backend;
    ui::MessageFiles *m_files;
    Workspace m_workspace;
    QWidget *m_window;
    QCache<QUrl, MessageLinkPreview> m_chatLinkPreviews{300};
    QCache<QPair<QString, QUrl>, ChatAsset> m_externalLinkPreviews{150};
    PublicLinkRequest m_publicLinkRequest;
    ui::PublicLinkPreviewFetcher *m_publicLinkFetcher = nullptr;
    QCache<QUrl, ui::PublicLinkMetadata> m_publicLinkPreviews{300};
    quint64 m_publicLinkAccountGeneration = 0;
    bool m_publicLinkPending = false;
    QPointer<QListWidget> m_externalLinkHistory;
    quint64 m_externalLinkGeneration = 0;
    qint64 m_latestExternalLinkMessage = 0;
    bool m_externalLinkPending = false;
    QHash<QUrl, quint64> m_pendingLinkPreviews;
    int m_linkPreviewRequests = 0;
    quint64 m_lastLinkPreviewRequest = 0;
    bool m_linkPreviewLayoutQueued = false;
    bool m_linkPreviewAccess = true;
};

} // namespace bx
