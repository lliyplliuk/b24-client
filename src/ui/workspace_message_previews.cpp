#include "ui/workspace_message_previews.h"

#include "core/notification_targets.h"
#include "core/origin_guard.h"
#include "ui/chat_widgets.h"
#include "ui/message_files.h"
#include "ui/notifications_panel.h"
#include "ui/task_dialogs.h"
#include "ui/workspace_style.h"

#include <QApplication>
#include <QMenu>
#include <QWindow>

#include <memory>
#include <utility>

namespace bx {

namespace {

QUrl resolvedChatLink(const QUrl &portal, const QString &href)
{
    const QUrl url(href, QUrl::StrictMode);
    if (url.isRelative() && portal.isValid())
        return workspace::messageWebLink(portal.resolved(url).toString(QUrl::FullyEncoded));
    return workspace::messageWebLink(href);
}

QUrl publicLinkTarget(QUrl url)
{
    url.setFragment({});
    if (url.port() == 443)
        url.setPort(-1);
    return url;
}

bool allowsAutomaticPublicPreview(const QModelIndex &row)
{
    return row.data(workspace::Outgoing).toBool() && !row.data(workspace::SystemMessage).toBool()
        && row.data(workspace::ForwardByline).toString().isEmpty() && !row.data(workspace::MarkupBlock).toBool();
}

} // namespace

WorkspaceMessagePreviews::WorkspaceMessagePreviews(Backend *backend, ui::MessageFiles *files,
                                                  PublicLinkRequest request, Workspace workspace, QWidget *window)
    : QObject(window), m_backend(backend), m_files(files), m_workspace(std::move(workspace)), m_window(window),
      m_publicLinkRequest(std::move(request))
{
}

void WorkspaceMessagePreviews::connectBackend()
{
    connect(qApp, &QGuiApplication::focusWindowChanged, this, [this] {
        if (m_workspace.online())
            scheduleExternalLinkPreviews();
    });
    const auto resetLinkPreviewAccess = [this](bool available) {
        if (m_linkPreviewAccess != available && !m_publicLinkPreviews.isEmpty())
            scheduleLinkPreviewLayout();
        m_linkPreviewAccess = available;
        resetLinkPreviews();
    };
    connect(m_backend, &Backend::messageLinkPreviewAccessChanged, this, resetLinkPreviewAccess);
    connect(m_backend, &Backend::accountScopeChanged, this, [this, resetLinkPreviewAccess] {
        ++m_publicLinkAccountGeneration;
        if (!m_publicLinkPreviews.isEmpty()) {
            m_publicLinkPreviews.clear();
            scheduleLinkPreviewLayout();
        }
        resetLinkPreviewAccess(m_linkPreviewAccess);
    });
}

void WorkspaceMessagePreviews::scheduleExternalLinkPreviews()
{
    if (m_externalLinkHistory)
        m_workspace.scheduleAvatars();
}

void WorkspaceMessagePreviews::bindLinkCards(QListWidget *list, workspace::RowPainter *delegate)
{
    delegate->setLinkAvatarSource([this](const QUrl &url) { return m_workspace.avatar(url); });
    delegate->setLinkThumbnailSource([this](const QUrl &url) -> const QImage * {
        const auto *preview = m_publicLinkPreviews.object(url);
        return preview && !preview->thumbnail.isNull() ? &preview->thumbnail : nullptr;
    });
    delegate->setLinkPreviewSource([this, list](const QModelIndex &index, const QString &href) -> MessageLinkPreview {
        const auto portal = m_backend->portalOrigin();
        const auto url = resolvedChatLink(portal, href);
        if (url.isEmpty())
            return {};
        if (OriginGuard(portal).isSameOrigin(url)) {
            if (isCurrentTaskLink(list, url))
                return {};
            const auto *preview = m_chatLinkPreviews.object(url);
            return preview ? *preview : MessageLinkPreview{};
        }
        if (m_externalLinkHistory == list) {
            const auto id = index.data(workspace::ReceiptMessageId).toString();
            if (const auto *asset = m_externalLinkPreviews.object({id, url})) {
                MessageLinkPreview preview{asset->name.isEmpty() ? url.host() : asset->name, {url.host()}};
                if (!asset->description.isEmpty())
                    preview.details.prepend(asset->description);
                preview.thumbnailUrl = asset->thumbnailUrl;
                return preview;
            }
        }
        const auto *taskCard = m_workspace.taskCard();
        if (m_linkPreviewAccess && (list == m_workspace.chatMessages() || (taskCard && list == taskCard->messageList()))) {
            const auto target = publicLinkTarget(url);
            const auto *preview = m_publicLinkPreviews.object(target);
            if (preview && (!preview->title.isEmpty() || !preview->description.isEmpty() || !preview->thumbnail.isNull())) {
                QStringList details{url.host()};
                if (!preview->description.isEmpty())
                    details.prepend(preview->description);
                MessageLinkPreview card{preview->title.isEmpty() ? url.host() : preview->title, details};
                if (!preview->thumbnail.isNull())
                    card.thumbnailUrl = target;
                return card;
            }
        }
        const auto path = url.path(QUrl::FullyDecoded);
        return {url.host(), path.isEmpty() || path == QStringLiteral("/") ? QStringList{} : QStringList{path.left(180)}};
    });
}

bool WorkspaceMessagePreviews::isCurrentTaskLink(const QListWidget *history, const QUrl &url) const
{
    const auto *card = m_workspace.taskCard();
    QString taskId;
    if (card && history == card->messageList())
        taskId = card->taskId();
    else if (history == m_workspace.chatMessages())
        taskId = m_workspace.chatTaskId();
    const auto currentTaskId = portalTaskIdentifier(taskId);
    return !currentTaskId.isEmpty() && portalTaskUrl(url).taskId == currentTaskId;
}

void WorkspaceMessagePreviews::refreshChatMediaRows()
{
    const auto *card = m_workspace.taskCard();
    for (auto *list : {m_workspace.chatMessages(), card ? card->messageList() : nullptr}) {
        if (list && list->isVisible())
            static_cast<ui::HistoryList *>(list)->refreshRowSizes();
    }
    if (card) {
        card->descriptionList()->requestHeight();
        card->attachmentList()->requestHeight();
    }
}

void WorkspaceMessagePreviews::loadVisibleChatPreviews()
{
    const auto *card = m_workspace.taskCard();
    const auto load = [this, card](QListWidget *list) {
        auto *delegate = static_cast<workspace::RowPainter *>(list->itemDelegate());
        const auto visible = list->viewport()->visibleRegion().boundingRect();
        if (!list->isVisible() || list->count() == 0 || visible.isEmpty()) {
            delegate->setPreviewAnimation(false);
            return;
        }
        constexpr int nearby = 125;
        const auto first = list->indexAt(QPoint(4, qMax(0, visible.top() - nearby)));
        const int start = first.isValid() ? qMax(0, first.row() - 3) : 0;
        const bool chatHistory = list == m_workspace.chatMessages() || (card && list == card->messageList());
        const bool taskContent = card && (list == card->descriptionList() || list == card->attachmentList());
        const auto portal = m_backend->portalOrigin();
        const OriginGuard guard(portal);
        qint64 newestExternalLink = 0;
        QPair<QString, QUrl> publicCandidate;
        bool animatePreviews = false;
        const bool foreground = chatHistory && !m_publicLinkPending && !m_window->isMinimized()
            && QApplication::activeWindow() == list->window()
            && QGuiApplication::applicationState() == Qt::ApplicationActive;
        for (int row = start; row < list->count(); ++row) {
            const auto *item = list->item(row);
            const auto bounds = list->visualItemRect(item);
            if (bounds.top() > visible.bottom() + nearby)
                break;
            if (bounds.bottom() < visible.top() - nearby)
                continue;
            if (chatHistory) {
                const auto id = item->data(workspace::ReceiptMessageId).toString();
                const auto links = item->data(workspace::MessageLinks).toStringList();
                for (const auto &href : links) {
                    const auto url = resolvedChatLink(portal, href);
                    if (guard.isSameOrigin(url)) {
                        if (!isCurrentTaskLink(list, url))
                            loadMessageLinkPreview(href);
                    } else if (!url.isEmpty() && m_linkPreviewAccess && !id.isEmpty()) {
                        newestExternalLink = qMax(newestExternalLink, id.toLongLong());
                        const auto *preview = m_externalLinkHistory == list
                            ? m_externalLinkPreviews.object({id, url}) : nullptr;
                        if (preview && m_files->imagePreviews().begin(preview->thumbnailUrl))
                            m_backend->requestMessageImage(preview->thumbnailUrl);
                        if (foreground && publicCandidate.second.isEmpty() && !preview
                            && allowsAutomaticPublicPreview(list->model()->index(row, 0))
                            && bounds.intersected(visible).height() >= qMin(24, bounds.height())
                            && needsPublicLinkPreview(list, id, url))
                            publicCandidate = {id, url};
                    }
                }
            }
            const auto loadImage = [this, taskContent, &bounds, &visible](const MessageFile &file) {
                if (file.isVideo)
                    return;
                const auto url = m_files->imagePreviews().requestUrl(file.previewUrl, file.originalPreviewUrl);
                if (taskContent && url == file.downloadUrl && !bounds.intersects(visible))
                    return;
                if (m_files->imagePreviews().begin(url))
                    m_backend->requestMessageImage(url);
            };
            const auto files = item->data(workspace::MessageFiles).value<QVector<MessageFile>>();
            for (const auto &file : files)
                loadImage(file);
            if (bounds.intersects(visible)) {
                const auto index = list->model()->index(row, 0);
                if (chatHistory && !m_workspace.recovering()) {
                    if (const auto representative = delegate->visibleQuoteMedia(index, bounds, visible))
                        loadImage(*representative);
                    const auto videos = delegate->visibleVideos(index, bounds, visible);
                    for (const auto &file : videos)
                        m_files->requestVideo(file);
                }
                animatePreviews = animatePreviews || delegate->hasVisibleLoadingPreview(index, bounds, visible);
            }
        }
        delegate->setPreviewAnimation(animatePreviews);
        if (chatHistory) {
            requestExternalLinkPreviews(list, newestExternalLink);
            if (!publicCandidate.second.isEmpty())
                requestPublicLinkPreview(list, publicCandidate.first, publicCandidate.second);
        }
    };
    if (card) {
        static_cast<workspace::RowPainter *>(m_workspace.chatMessages()->itemDelegate())->setPreviewAnimation(false);
        load(card->messageList());
        load(card->descriptionList());
        load(card->attachmentList());
    } else {
        load(m_workspace.chatMessages());
    }
    for (const auto &href : m_workspace.events()->visiblePreviewLinks())
        loadMessageLinkPreview(href);
}

void WorkspaceMessagePreviews::requestExternalLinkPreviews(QListWidget *history, qint64 newest)
{
    const auto *card = m_workspace.taskCard();
    if (!m_linkPreviewAccess || m_externalLinkPending || newest <= m_latestExternalLinkMessage)
        return;
    const bool task = card && history == card->messageList();
    const auto scope = task ? card->taskId() : m_workspace.chatId();
    if (scope.isEmpty())
        return;
    m_latestExternalLinkMessage = newest;
    for (int row = 0; row < history->count(); ++row)
        m_latestExternalLinkMessage = qMax(m_latestExternalLinkMessage,
                                           history->item(row)->data(workspace::ReceiptMessageId).toLongLong());
    m_externalLinkPending = true;
    m_externalLinkHistory = history;
    m_backend->requestChatLinkPreviews(scope, task,
        [self = QPointer<WorkspaceMessagePreviews>(this), generation = m_externalLinkGeneration](QVector<ChatAsset> assets) {
            if (!self)
                return;
            self->m_externalLinkPending = false;
            if (generation == self->m_externalLinkGeneration) {
                const OriginGuard guard(self->m_backend->portalOrigin());
                bool added = false;
                for (auto &asset : assets) {
                    self->m_latestExternalLinkMessage = qMax(self->m_latestExternalLinkMessage,
                                                             asset.messageId.toLongLong());
                    if (asset.messageId.isEmpty() || asset.link.isEmpty() || guard.isSameOrigin(asset.link)
                        || (asset.description.isEmpty() && asset.thumbnailUrl.isEmpty()
                            && (asset.name.isEmpty() || asset.name == asset.link.toDisplayString())))
                        continue;
                    const auto key = qMakePair(asset.messageId, asset.link);
                    self->m_externalLinkPreviews.insert(key, new ChatAsset(std::move(asset)));
                    added = true;
                }
                if (added)
                    self->scheduleLinkPreviewLayout();
            }
            self->m_workspace.scheduleAvatars();
        });
}

bool WorkspaceMessagePreviews::needsPublicLinkPreview(QListWidget *history, const QString &id, const QUrl &url) const
{
    if (!history || !m_workspace.online() || !m_linkPreviewAccess || m_backend->isDemo() || m_workspace.recovering()
        || history != m_externalLinkHistory || !history->isVisible() || m_externalLinkPending
        || id.toLongLong() > m_latestExternalLinkMessage || OriginGuard(m_backend->portalOrigin()).isSameOrigin(url)
        || !ui::PublicLinkPreviewFetcher::accepts(url, !m_publicLinkRequest)
        || m_externalLinkPreviews.contains({id, url}))
        return false;
    return !id.isEmpty() && !m_publicLinkPreviews.contains(publicLinkTarget(url));
}

void WorkspaceMessagePreviews::requestPublicLinkPreview(QListWidget *history, const QString &id, const QUrl &url)
{
    if (m_publicLinkPending || !needsPublicLinkPreview(history, id, url))
        return;
    m_publicLinkPending = true;
    const auto target = publicLinkTarget(url);
    const auto finish = [self = QPointer<WorkspaceMessagePreviews>(this), target,
                         account = m_publicLinkAccountGeneration](std::optional<ui::PublicLinkMetadata> result) {
        if (!self)
            return;
        self->m_publicLinkPending = false;
        if (self->m_publicLinkAccountGeneration == account) {
            auto metadata = result.value_or(ui::PublicLinkMetadata{});
            const qsizetype imageCost = (metadata.thumbnail.sizeInBytes() + 8191) / 8192;
            const bool hasContent = !metadata.title.isEmpty() || !metadata.description.isEmpty() || imageCost > 0;
            self->m_publicLinkPreviews.insert(target, new ui::PublicLinkMetadata(std::move(metadata)), 1 + imageCost);
            if (hasContent)
                self->scheduleLinkPreviewLayout();
        }
        self->m_workspace.scheduleAvatars();
    };
    if (!m_publicLinkRequest && !m_publicLinkFetcher)
        m_publicLinkFetcher = new ui::PublicLinkPreviewFetcher(this);
    const bool started = m_publicLinkRequest ? m_publicLinkRequest(target, finish) : m_publicLinkFetcher->request(target, finish);
    if (!started)
        finish(std::nullopt);
}

void WorkspaceMessagePreviews::addPublicLinkPreviewActions(QMenu &menu, const QPersistentModelIndex &message,
                                             const std::function<QPersistentModelIndex()> &resolve)
{
    auto *history = m_externalLinkHistory.data();
    if (!history || history->model() != message.model() || message.data(workspace::SystemMessage).toBool()
        || allowsAutomaticPublicPreview(message))
        return;
    const auto id = message.data(workspace::ReceiptMessageId).toString();
    const auto links = message.data(workspace::MessageLinks).toStringList();
    QVector<QPair<QString, QUrl>> available;
    for (const auto &href : links) {
        const auto url = resolvedChatLink(m_backend->portalOrigin(), href);
        if (needsPublicLinkPreview(history, id, url))
            available.append({href, url});
    }
    for (const auto &[href, url] : available) {
        auto caption = QStringLiteral("Загрузить предпросмотр ссылки");
        if (available.size() > 1)
            caption += QStringLiteral(" · %1").arg(url.toDisplayString(QUrl::RemoveQuery | QUrl::RemoveFragment).left(96));
        auto *action = menu.addAction(caption);
        action->setEnabled(!m_publicLinkPending);
        connect(action, &QAction::triggered, this, [this, history = QPointer<QListWidget>(history), resolve, id, href, url] {
            const auto message = resolve();
            if (message.isValid() && message.data(workspace::ReceiptMessageId).toString() == id
                && !allowsAutomaticPublicPreview(message) && !message.data(workspace::SystemMessage).toBool()
                && message.data(workspace::MessageLinks).toStringList().contains(href))
                requestPublicLinkPreview(history, id, url);
        });
    }
}

void WorkspaceMessagePreviews::loadMessageLinkPreview(const QString &href)
{
    constexpr qsizetype concurrentLinkPreviews = 3;
    if (!m_linkPreviewAccess)
        return;
    const auto portal = m_backend->portalOrigin();
    const auto url = resolvedChatLink(portal, href);
    if (url.isEmpty() || !OriginGuard(portal).isSameOrigin(url))
        return;
    if (const auto *preview = m_chatLinkPreviews.object(url)) {
        if (!preview->title.isEmpty()) {
            m_workspace.requestAvatar(preview->imageUrl());
            m_workspace.requestAvatar(preview->creatorAvatarUrl);
            m_workspace.requestAvatar(preview->assigneeAvatarUrl);
        }
        return;
    }
    if (m_linkPreviewRequests >= concurrentLinkPreviews || m_pendingLinkPreviews.contains(url)
        || !portalLinkPreviewTarget(portal, url))
        return;
    const auto request = ++m_lastLinkPreviewRequest;
    m_pendingLinkPreviews.insert(url, request);
    ++m_linkPreviewRequests;
    m_backend->requestMessageLinkPreview(url,
        [self = QPointer<WorkspaceMessagePreviews>(this), url, request](MessageLinkPreview preview) {
            if (!self)
                return;
            --self->m_linkPreviewRequests;
            auto &pending = self->m_pendingLinkPreviews;
            const auto current = pending.find(url);
            if (current != pending.end() && current.value() == request) {
                pending.erase(current);
                const bool available = !preview.title.isEmpty();
                self->m_chatLinkPreviews.insert(url, new MessageLinkPreview(std::move(preview)));
                if (available)
                    self->scheduleLinkPreviewLayout();
            }
            self->m_workspace.scheduleAvatars();
        });
}

void WorkspaceMessagePreviews::invalidateTaskLinkPreview(const QString &taskId)
{
    if (taskId.isEmpty())
        return;
    const auto refersToTask = [&taskId](const QUrl &url) { return portalTaskUrl(url).taskId == taskId; };
    const auto removedPending = m_pendingLinkPreviews.removeIf(
        [&refersToTask](auto entry) { return refersToTask(entry.key()); });
    bool removedCached = false;
    bool removedCard = false;
    for (const auto &url : m_chatLinkPreviews.keys()) {
        if (!refersToTask(url))
            continue;
        const std::unique_ptr<MessageLinkPreview> preview(m_chatLinkPreviews.take(url));
        removedCached = true;
        removedCard = removedCard || (preview && !preview->title.isEmpty());
    }
    if (removedCard)
        scheduleLinkPreviewLayout();
    if (m_linkPreviewAccess && (removedCached || removedPending))
        m_workspace.scheduleAvatars();
}

void WorkspaceMessagePreviews::resetLinkPreviews()
{
    ++m_externalLinkGeneration;
    m_latestExternalLinkMessage = 0;
    m_externalLinkHistory.clear();
    m_pendingLinkPreviews.clear();
    if (!m_chatLinkPreviews.isEmpty() || !m_externalLinkPreviews.isEmpty()) {
        m_chatLinkPreviews.clear();
        m_externalLinkPreviews.clear();
        refreshLinkPreviewRows();
    }
    if (m_linkPreviewAccess)
        m_workspace.scheduleAvatars();
}

void WorkspaceMessagePreviews::scheduleLinkPreviewLayout()
{
    if (m_linkPreviewLayoutQueued)
        return;
    m_linkPreviewLayoutQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_linkPreviewLayoutQueued = false;
        refreshLinkPreviewRows();
    });
}

void WorkspaceMessagePreviews::refreshLinkPreviewRows()
{
    const auto *card = m_workspace.taskCard();
    for (auto *list : {m_workspace.chatMessages(), card ? card->messageList() : nullptr}) {
        if (list)
            static_cast<ui::HistoryList *>(list)->refreshRowSizes();
    }
    m_workspace.events()->refreshPreviewRows();
}

} // namespace bx
