#include "cloud/cloud_backend.h"
#include "cloud/cloud_chat_metadata.h"
#include "cloud/cloud_rows.h"

#include "signin/session_capture.h"
#include "signin/session_restore.h"

#include <QCryptographicHash>
#include <QLoggingCategory>
#include <QJsonArray>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QUrlQuery>
#include <QUuid>
#include <algorithm>
#include <memory>
#include <utility>

namespace bx {
using namespace cloud_rows;

namespace {

Q_LOGGING_CATEGORY(sessionLog, "bitrix24.session")

const QString recentOp = operationKey(OperationKind::Chats);
const QString taskRecentOp = operationKey(OperationKind::TaskChats);
const QString pushOp = operationKey(OperationKind::Push);
const QString chatIdentityOp = operationKey(OperationKind::ChatHistoryIdentity);
const QString sessionRecoveryOp = operationKey(OperationKind::SessionRecovery);
const QString sessionKeepaliveOp = operationKey(OperationKind::SessionKeepalive);
const QString csrfRefreshOp = operationKey(OperationKind::CsrfRefresh);

} // namespace

CloudBackend::CloudBackend(Session session, bool allowLoopback, QObject *parent)
    : Backend(parent), m_session(std::move(session)), m_allowLoopback(allowLoopback),
      m_portalHttp(*this, m_http, this), m_projects(*this, m_portalHttp, this),
      m_tasks(*this, *this, *this, m_portalHttp, this),
      m_chats(*this, *this, *this, *this, *this, m_timeline, m_portalHttp, this), m_notices(*this, m_timeline, m_portalHttp, this),
      m_pull(*this, m_portalHttp, this)
{
    connect(&m_notices, &NotificationsService::notificationsLoaded, this, &Backend::notificationsLoaded);
    connect(&m_notices, &NotificationsService::notificationHistoryPageStateChanged, this, &Backend::notificationHistoryPageStateChanged);
    connect(&m_notices, &NotificationsService::notificationsFound, this, &Backend::notificationsFound);
    connect(&m_notices, &NotificationsService::notificationReadStateChanged, this, &Backend::notificationReadStateChanged);
    connect(&m_notices, &NotificationsService::notificationRemoved, this, &Backend::notificationRemoved);
    connect(&m_notices, &NotificationsService::notificationActionFinished, this, &Backend::notificationActionFinished);
    connect(&m_notices, &NotificationsService::notificationSettingsLoaded, this, &Backend::notificationSettingsLoaded);
    connect(&m_notices, &NotificationsService::notificationReceived, this, &CloudBackend::notificationReceived);
    connect(&m_notices, &NotificationsService::failed, this, &Backend::failed);
    connect(&m_chats, &ChatsService::popupSettingsChanged, &m_notices, &NotificationsService::handleSettingsPull);
    connect(&m_chats, &ChatsService::notificationStateEvent, &m_notices, &NotificationsService::handleNotificationStatePull);
    connect(&m_chats, &ChatsService::incomingNoticeEvent, &m_notices, &NotificationsService::handleIncomingNoticePull);
    connect(&m_chats, &ChatsService::chatMessagesSeen, this, &Backend::chatMessagesSeen);
    connect(&m_chats, &ChatsService::chatTypingChanged, this, &Backend::chatTypingChanged);
    connect(&m_chats, &ChatsService::chatActionSucceeded, this, &Backend::chatActionSucceeded);
    connect(&m_chats, &ChatsService::chatHistoryRemoved, this, &Backend::chatHistoryRemoved);
    connect(&m_chats, &ChatsService::chatMessagesLoaded, this, &Backend::chatMessagesLoaded);
    connect(&m_chats, &ChatsService::chatOwnReactionChanged, this, &Backend::chatOwnReactionChanged);
    connect(&m_chats, &ChatsService::chatPinChanged, this, &Backend::chatPinChanged);
    connect(&m_chats, &ChatsService::chatsFound, this, &Backend::chatsFound);
    connect(&m_chats, &ChatsService::failed, this, &Backend::failed);
    connect(&m_chats, &ChatsService::groupChatCreated, this, &Backend::groupChatCreated);
    connect(&m_chats, &ChatsService::messageEditAccepted, this, &Backend::messageEditAccepted);
    connect(&m_chats, &ChatsService::messageSent, this, &Backend::messageSent);
    connect(&m_chats, &ChatsService::messagesFound, this, &Backend::messagesFound);
    connect(&m_chats, &ChatsService::olderChatMessagesLoaded, this, &Backend::olderChatMessagesLoaded);
    connect(&m_chats, &ChatsService::pinnedMessagesLoaded, this, &Backend::pinnedMessagesLoaded);
    connect(&m_chats, &ChatsService::recentChatsLoaded, this, &Backend::recentChatsLoaded);
    connect(&m_chats, &ChatsService::taskChatsFound, this, &Backend::taskChatsFound);
    connect(&m_chats, &ChatsService::taskChatsLoaded, this, &Backend::taskChatsLoaded);
    connect(&m_chats, &ChatsService::taskMessagesLoaded, this, &Backend::taskMessagesLoaded);
    connect(&m_tasks, &TasksService::taskCreated, this, &Backend::taskCreated);
    connect(&m_tasks, &TasksService::subtaskCreated, this, &Backend::subtaskCreated);
    connect(&m_tasks, &TasksService::taskDeleted, this, &Backend::taskDeleted);
    connect(&m_tasks, &TasksService::taskUpdated, this, &Backend::taskUpdated);
    connect(&m_tasks, &TasksService::taskSummaryLoaded, this, &Backend::taskSummaryLoaded);
    connect(&m_tasks, &TasksService::taskLoaded, this, &Backend::taskLoaded);
    connect(&m_tasks, &TasksService::taskChecklistLoaded, this, &Backend::taskChecklistLoaded);
    connect(&m_tasks, &TasksService::taskChecklistChanged, this, &Backend::taskChecklistChanged);
    connect(&m_tasks, &TasksService::subtasksLoaded, this, &Backend::subtasksLoaded);
    connect(&m_tasks, &TasksService::taskDependenciesLoaded, this, &Backend::taskDependenciesLoaded);
    connect(&m_tasks, &TasksService::taskHistoryLoaded, this, &Backend::taskHistoryLoaded);
    connect(&m_tasks, &TasksService::taskLinkPreviewInvalidated, this, &Backend::taskLinkPreviewInvalidated);
    connect(&m_tasks, &TasksService::taskFileAttached, this, &Backend::taskFileAttached);
    connect(&m_tasks, &TasksService::fileUploaded, this, &Backend::fileUploaded);
    connect(&m_tasks, &TasksService::failed, this, &Backend::failed);
    connect(&m_projects, &ProjectsService::boardLoaded, this, &Backend::boardLoaded);
    connect(&m_projects, &ProjectsService::boardTasksPageLoaded, this, &Backend::boardTasksPageLoaded);
    connect(&m_projects, &ProjectsService::boardUpdated, this, &Backend::boardUpdated);
    connect(&m_projects, &ProjectsService::failed, this, &Backend::failed);
    connect(&m_projects, &ProjectsService::projectCreated, this, &Backend::projectCreated);
    connect(&m_projects, &ProjectsService::projectFilesLoaded, this, &Backend::projectFilesLoaded);
    connect(&m_projects, &ProjectsService::projectMembersLoaded, this, &Backend::projectMembersLoaded);
    connect(&m_projects, &ProjectsService::projectPinChanged, this, &Backend::projectPinChanged);
    connect(&m_projects, &ProjectsService::projectStagesLoaded, this, &Backend::projectStagesLoaded);
    connect(&m_projects, &ProjectsService::projectTaskChatBadgesChanged, this, &Backend::projectTaskChatBadgesChanged);
    connect(&m_projects, &ProjectsService::projectTaskChatsLoaded, this, &Backend::projectTaskChatsLoaded);
    connect(&m_projects, &ProjectsService::projectTasksFound, this, &Backend::projectTasksFound);
    connect(&m_projects, &ProjectsService::projectTimelineLoaded, this, &Backend::projectTimelineLoaded);
    connect(&m_projects, &ProjectsService::projectUnreadCountsChanged, this, &Backend::projectUnreadCountsChanged);
    connect(&m_projects, &ProjectsService::projectsLoaded, this, &Backend::projectsLoaded);
    connect(&m_projects, &ProjectsService::projectsMoreAvailable, this, &Backend::projectsMoreAvailable);
    connect(&m_projects, &ProjectsService::projectsScopeLoaded, this, &Backend::projectsScopeLoaded);

    m_http.setProxy(QNetworkProxy::NoProxy);
    m_refresh.setSingleShot(true);
    connect(&m_refresh, &QTimer::timeout, this, &CloudBackend::refreshDirtyViews);
    m_historyCatchup.setInterval(15 * 1000);
    m_historyCatchup.setTimerType(Qt::VeryCoarseTimer);
    m_historyCatchup.setObjectName(QStringLiteral("active-history-catchup"));
    m_historyCatchup.setParent(this);
    connect(&m_historyCatchup, &QTimer::timeout, this, [this] {
        m_timeline.invalidateOpenViews(m_activeSection == 0);
        scheduleRefresh();
    });
    m_sessionKeepalive.setInterval(15 * 60 * 1000);
    m_sessionKeepalive.setTimerType(Qt::VeryCoarseTimer);
    connect(&m_sessionKeepalive, &QTimer::timeout, this, [this] {
        ajax(QStringLiteral("im.v2.Recent.load"),
             {{QStringLiteral("filter[recentSection]"), QStringLiteral("default")},
              {QStringLiteral("limit"), QStringLiteral("1")}},
             sessionKeepaliveOp, [](const QJsonValue &) {});
    });
    connect(this, &Backend::failed, this, [this](const QString &operation, const QString &) {
        if (operation == chatIdentityOp)
            m_chats.resumeHistoriesWithIdentity();
        m_notices.handleOperationFailure(operation);
        m_tasks.handleOperationFailure(operation);
    });
    connect(&m_pull, &PullChannel::failed, this, &Backend::failed);
    connect(&m_pull, &PullChannel::offline, this, [this] { setOnline(false); });
    connect(&m_pull, &PullChannel::taskEvent, this, &CloudBackend::handleTaskPull);
    connect(&m_pull, &PullChannel::presenceEvent, this, &CloudBackend::handlePresencePull);
    connect(&m_pull, &PullChannel::imEvent, &m_chats, &ChatsService::handleImPull);
    connect(&m_pull, &PullChannel::connected, this, [this] {
        const bool refreshPopupSettings = m_resumed && m_notices.reconnectSettingsNeeded();
        if (m_resumed)
            m_timeline.resetPullMuteFlags();
        if (refreshPopupSettings)
            m_notices.resetReconnectSettings();
        setOnline(true);
        if (refreshPopupSettings)
            loadChatPopupSettings();
        if (m_resumed) {
            loadChatBadges();
            invalidateActiveView();
        }
        m_resumed = true;
    });
}

CloudBackend::~CloudBackend()
{
    stop();
}

QString CloudBackend::displayName() const
{
    return m_session.origin.host();
}

void CloudBackend::start()
{
    if (m_started)
        return;
    m_started = true;
    m_authExpired = false;
    m_resumed = false;
    m_pull.resetBackoff();
    if (!PortalHttp::sessionUsable(m_session, m_allowLoopback)) {
        emit failed(pushOp, QStringLiteral("Для подключения нужны HTTPS, Cookie и CSRF веб-сессии"));
        return;
    }
    m_initialPortalStatusPending = true;
    m_pull.connectPull();
    loadChatBadges();
    updateHistoryCatchup();
    m_sessionKeepalive.start();
}

void CloudBackend::stop()
{
    m_started = false;
    m_userId.clear();
    resetAccountScopedState();
    m_refresh.stop();
    m_sessionKeepalive.stop();
    m_pull.stop();
    setOnline(false);
}

void CloudBackend::resetAccountScopedState()
{
    ++m_accountScopeRevision;
    resetWorktime();
    const auto pending = m_portalHttp.pendingRequests();

    cancelSessionConfirmation();
    m_portalStatus.clear();
    m_initialPortalStatusPending = m_started;
    m_unboundPortalStatuses.clear();
    ++m_portalStatusRevision;
    m_lastPortalStatusPushTime = 0;

    m_tasks.reset();
    const bool projectCountsChanged = m_projects.reset();

    clearRasterAssets();
    m_chats.reset();
    m_chatAnchors.clear();
    m_chatAnchorCount = 0;
    m_pendingAnchorEvents.clear();
    m_chatContacts.clear();
    m_registeredMessageFileUrls.clear();
    m_messageViewerMembership.clear();
    m_messageViewerCounts.clear();

    m_pull.reset();
    // MainWindow reopens history only when a view requests it for the new account.
    m_timeline.resetAccount();
    m_portalHttp.resetHistoryRetries();
    updateHistoryCatchup();

    m_notices.resetSettings();
    resetChatCounters();
    if (projectCountsChanged)
        emit projectUnreadCountsChanged({}, false);
    publishUnreadChatCount();
    m_notificationsDirty = false;
    m_notices.reset();

    m_portalHttp.reset(pending);
}

void CloudBackend::setOnline(bool online)
{
    if (m_online == online)
        return;
    m_online = online;
    emit connectionChanged(online);
}

void CloudBackend::setForeground(bool foreground)
{
    if (m_foreground == foreground)
        return;
    const bool wasWatching = canAutoRefreshChats();
    m_foreground = foreground;
    if (foreground && m_activeSection == 1)
        m_projects.publishProjectUnreadCounts();
    if (foreground && m_started)
        invalidateActiveView();
    updateChatViewActivity(wasWatching);
}

void CloudBackend::setWorkspaceVisible(bool visible)
{
    if (m_workspaceVisible == visible)
        return;
    const bool wasWatching = canAutoRefreshChats();
    m_workspaceVisible = visible;
    updateChatViewActivity(wasWatching);
}

void CloudBackend::updateChatViewActivity(bool wasWatching)
{
    const bool watching = canAutoRefreshChats();
    if (wasWatching == watching)
        return;
    if (!watching) {
        m_refresh.stop();
        m_portalHttp.invalidateHistory(false);
        m_portalHttp.invalidateHistory(true);
    } else if (!m_foreground && m_started) {
        m_timeline.invalidateOpenViews(m_activeSection == 0);
        scheduleRefresh();
    }
    updateHistoryCatchup();
}

void CloudBackend::setActiveSection(int section)
{
    if (m_activeSection == section)
        return;
    m_activeSection = section;
    if (section != 0)
        m_portalHttp.invalidateHistory(false);
    if (section == 1)
        m_projects.refreshUnreadCounts();
    scheduleRefresh();
    updateHistoryCatchup();
}

void CloudBackend::clearOpenHistory(bool task)
{
    m_portalHttp.invalidateHistory(task);
    m_timeline.openHistory({}, task);
    m_notices.clearHistory(task);
    updateHistoryCatchup();
}

void CloudBackend::updateHistoryCatchup(bool restart)
{
    const bool watchingChat = m_activeSection == 0 && !m_timeline.activeDialog().isEmpty();
    // A task card can be open over any workspace section.
    const bool watchingTask = !m_timeline.activeTask().isEmpty();
    const bool shouldRun = m_started && canAutoRefreshChats() && !m_authExpired && (watchingChat || watchingTask);
    // With two histories open, traffic in one must not postpone the safety refresh for the other.
    const bool restartSingleHistory = restart && !(watchingChat && watchingTask);
    if (shouldRun && (restartSingleHistory || !m_historyCatchup.isActive()))
        m_historyCatchup.start();
    else if (!shouldRun)
        m_historyCatchup.stop();
}

void CloudBackend::applyRenewedSession(const Session &renewed)
{
    if (!renewed.isValid()) {
        rejectSession();
    } else if (renewed.cookie != m_session.cookie || renewed.csrf != m_session.csrf) {
        m_session = renewed;
        ++m_sessionRevision;
        emit sessionUpdated(m_session);
    }
}

void CloudBackend::rejectSession()
{
    if (m_authExpired)
        return;
    m_authExpired = true;
    m_portalHttp.invalidateHistory(false);
    m_portalHttp.invalidateHistory(true);
    m_messageViewerMembership.clear();
    m_messageViewerCounts.clear();
    ++m_sessionRevision;
    auto completion = std::exchange(m_sessionConfirmationCallback, {});
    cancelSessionConfirmation();
    m_pull.cancelRetry();
    m_refresh.stop();
    m_sessionKeepalive.stop();
    updateHistoryCatchup();
    m_pull.scheduleReconnect();
    qCWarning(sessionLog) << "Portal authorization expired; waiting for the user to renew it";
    emit messageLinkPreviewAccessChanged(false);
    emit sessionRejected();
    if (completion)
        completion(false);
}

void CloudBackend::cancelSessionConfirmation()
{
    m_sessionConfirmationCallback = {};
    if (auto *probe = std::exchange(m_sessionConfirmation, nullptr)) {
        probe->cancel();
        probe->deleteLater();
    }
}

void CloudBackend::confirmSession(quint64 revision, std::function<void(bool)> onConfirmed)
{
    if (!m_started || m_authExpired || revision != m_sessionRevision) {
        if (onConfirmed)
            onConfirmed(m_started && !m_authExpired);
        return;
    }
    if (m_sessionConfirmation && m_sessionConfirmationRevision == revision) {
        if (onConfirmed)
            m_sessionConfirmationCallback = std::move(onConfirmed);
        return;
    }
    auto previous = std::move(m_sessionConfirmationCallback);
    cancelSessionConfirmation();
    if (previous)
        previous(true);
    auto *probe = new signin::SessionRestore(this);
    m_sessionConfirmation = probe;
    m_sessionConfirmationRevision = revision;
    m_sessionConfirmationCallback = std::move(onConfirmed);
    connect(probe, &signin::SessionRestore::verified, this,
            [this, probe, revision](signin::RestoreStatus result, const Session &verified) {
        if (probe != m_sessionConfirmation)
            return;
        m_sessionConfirmation = nullptr;
        auto completion = std::exchange(m_sessionConfirmationCallback, {});
        probe->deleteLater();
        if (!m_started || m_authExpired)
            return;
        if (revision != m_sessionRevision) {
            if (completion)
                completion(true);
            return;
        }
        if (result == signin::RestoreStatus::Rejected)
            rejectSession();
        const bool accepted = result == signin::RestoreStatus::Authenticated
            && verified.origin == m_session.origin && PortalHttp::sessionUsable(verified, m_allowLoopback);
        if (accepted && (verified.csrf != m_session.csrf || verified.cookie != m_session.cookie)) {
            m_session = verified;
            ++m_sessionRevision;
            emit sessionUpdated(m_session);
            invalidateActiveView();
        }
        if (completion)
            completion(accepted);
    });
    probe->verify(m_session, m_allowLoopback);
}

bool CloudBackend::refreshCsrf(Session candidate, quint64 revision)
{
    if (m_portalHttp.contains(csrfRefreshOp))
        return true;
    auto verified = std::make_shared<Session>(std::move(candidate));
    m_portalHttp.webRequest(actionUrl(QStringLiteral("im.v2.Recent.load")),
               {{QStringLiteral("filter[recentSection]"), QStringLiteral("default")},
                {QStringLiteral("limit"), QStringLiteral("1")}},
               csrfRefreshOp, QStringLiteral("data"), [this, verified, revision](const QJsonValue &data) {
        if (revision != m_sessionRevision || m_authExpired)
            return;
        const auto response = data.toObject();
        const auto user = numericId(field(response, {"userId", "USER_ID"}));
        if ((!user.isEmpty() && !m_userId.isEmpty() && user != m_userId) || !verified->isValid()) {
            rejectSession();
            return;
        }
        if (!response.value(QStringLiteral("recentItems")).isArray())
            return;
        m_session = *verified;
        ++m_sessionRevision;
        qCInfo(sessionLog) << "Portal CSRF token renewed";
        emit sessionUpdated(m_session);
        invalidateActiveView();
    }, {}, verified);
    return m_portalHttp.contains(csrfRefreshOp);
}

void CloudBackend::invalidateActiveView()
{
    switch (m_activeSection) {
    case 1:
        m_projects.invalidateOpenBoard();
        break;
    case 2:
        m_notificationsDirty = true;
        break;
    }
    m_timeline.invalidateOpenViews(m_activeSection == 0);
    scheduleRefresh();
}

void CloudBackend::scheduleRefresh()
{
    if (!m_started || !canAutoRefreshChats() || m_authExpired || m_refresh.isActive())
        return;
    const bool sectionDirty = m_activeSection == 0 ? m_timeline.isRecentDirty() || m_timeline.isHistoryDirty(false)
        : m_foreground && (m_activeSection == 1 ? m_projects.boardDirty() : m_activeSection == 2 && m_notificationsDirty);
    if (sectionDirty || m_timeline.isHistoryDirty(true))
        m_refresh.start(250);
}

void CloudBackend::refreshDirtyViews()
{
    if (!m_started || !canAutoRefreshChats() || m_authExpired)
        return;
    const auto canRequest = [this](const QString &operation) {
        return !m_portalHttp.atCapacity() && !m_portalHttp.contains(operation);
    };
    if (m_activeSection == 0) {
        const bool taskChats = m_timeline.showingTaskChats();
        if (canRequest(taskChats ? taskRecentOp : recentOp) && m_timeline.takeRecentDirty())
            m_chats.loadRecentChats(taskChats);
        const auto &dialog = m_timeline.activeDialog();
        if (!dialog.isEmpty() && canRequest(operationKey(OperationKind::Chat, dialog)) && m_timeline.takeHistoryDirty(false))
            requestChatMessages(dialog);
    } else if (m_foreground && m_activeSection == 1) {
        m_projects.refreshDirtyBoard();
    } else if (m_foreground && m_activeSection == 2 && m_notificationsDirty && canRequest(m_notices.historyRequestKey())) {
        m_notificationsDirty = false;
        requestNotifications();
    }
    const auto &task = m_timeline.activeTask();
    if (!task.isEmpty() && canRequest(operationKey(OperationKind::TaskMessages, task)) && m_timeline.takeHistoryDirty(true))
        requestTaskMessages(task);
}

void CloudBackend::invalidateTaskChat(const QString &taskId)
{
    if (!taskId.isEmpty()) {
        if (taskId == m_timeline.activeTask())
            m_timeline.invalidateHistory(true);
        const auto &taskChats = m_timeline.recentChats(true);
        const auto listed = std::find_if(taskChats.cbegin(), taskChats.cend(),
            [&taskId](const Chat &chat) { return chat.taskId == taskId; });
        if (listed != taskChats.cend() && m_timeline.showingTaskChats())
            m_timeline.invalidateRecent();
        const auto &dialog = m_timeline.activeDialog();
        if (!dialog.isEmpty() && (m_timeline.taskDialog(taskId) == dialog
            || (listed != taskChats.cend() && listed->id == dialog)))
            m_timeline.invalidateHistory(false);
    }
    scheduleRefresh();
}

void CloudBackend::failShape(const QString &operation)
{
    emit failed(operation, QStringLiteral("Портал вернул неподдерживаемый формат данных"));
}

void CloudBackend::withCurrentUser(const QString &operation, std::function<void(qint64)> onUser)
{
    if (!numericId(m_userId).isEmpty()) {
        onUser(m_userId.toLongLong());
        return;
    }
    requestCurrentUser(operation, [this, onUser = std::move(onUser)](const QString &id) {
        if (!id.isEmpty())
            rememberCurrentUser(id);
        onUser(id.toLongLong());
    });
}

void CloudBackend::requestCurrentPerson(PersonHandler complete)
{
    const auto operation = operationKey(OperationKind::CurrentPerson, QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto account = m_accountScopeRevision;
    withCurrentUser(operation, [this, operation, account, complete = std::move(complete)](qint64 userId) mutable {
        if (account != m_accountScopeRevision || userId <= 0) {
            complete({});
            return;
        }
        loadPerson(QString::number(userId), operation, std::move(complete), true);
    });
}

void CloudBackend::requestCurrentUser(const QString &operation,
                                      std::function<void(const QString &)> onUser,
                                      const std::shared_ptr<Session> &candidate)
{
    const bool optional = defaultRequestPolicy(parseOperation(operation).kind) == RequestPolicy::Optional;
    const auto extension = QStringLiteral("socialnetwork.v2.current-user");
    auto url = actionUrl(QStringLiteral("main.bitrix.main.controller.loadext.resolveextensions"));
    QUrlQuery query(url);
    query.addQueryItem(QStringLiteral("e"), extension);
    url.setQuery(query);
    m_portalHttp.webRequest(url, {{QStringLiteral("extension[0]"), extension}, {QStringLiteral("coreLoaded"), QStringLiteral("Y")}},
               operation, QStringLiteral("data"), [this, extension, operation, optional, onUser = std::move(onUser)](QJsonValue data) {
        if (!data.isArray() && !optional)
            return failShape(operation);
        for (const auto &entry : data.toArray()) {
            const auto item = entry.toObject();
            if (item.value(QStringLiteral("extension")) != extension)
                continue;
            const auto settings = item.value(QStringLiteral("settings")).toObject();
            const auto id = numericId(field(settings, {"id"}));
            if (!id.isEmpty()) {
                onUser(id);
                return;
            }
            break;
        }
        if (optional)
            onUser({});
        else
            emit failed(operation, QStringLiteral("Портал не подтвердил текущего пользователя"));
    }, {}, candidate);
}

void CloudBackend::recoverSession(const Session &candidate, std::function<bool()> stillAllowed)
{
    if (!m_started || !m_authExpired || m_portalHttp.contains(sessionRecoveryOp))
        return;
    if (candidate.origin != m_session.origin || numericId(m_userId).isEmpty()) {
        emit failed(sessionRecoveryOp, QStringLiteral("Чтобы открыть другой портал или аккаунт, выйдите из текущего рабочего окна."));
        return;
    }
    const auto expectedUser = m_userId;
    auto verified = std::make_shared<Session>(candidate);
    requestCurrentUser(sessionRecoveryOp, [this, verified, expectedUser, stillAllowed = std::move(stillAllowed)](const QString &id) {
        if (stillAllowed && !stillAllowed())
            return;
        if (!PortalHttp::sessionUsable(*verified, m_allowLoopback)) {
            emit failed(sessionRecoveryOp, QStringLiteral("Портал отклонил новый вход. Обновите подключение ещё раз."));
            return;
        }
        if (id != expectedUser || m_userId != expectedUser) {
            emit failed(sessionRecoveryOp, QStringLiteral("Подключитесь под тем же аккаунтом, чтобы сохранить рабочее окно и черновики."));
            return;
        }
        m_portalHttp.abortAll();
        resetWorktime();
        m_session = *verified;
        ++m_sessionRevision;
        m_authExpired = false;
        m_pull.resetBackoff();
        m_sessionKeepalive.start();
        qCInfo(sessionLog) << "Portal authorization renewed";
        emit sessionUpdated(m_session);
        emit messageLinkPreviewAccessChanged(true);
        emit sessionRecovered();
        m_pull.connectPull();
        loadChatBadges();
        invalidateActiveView();
        updateHistoryCatchup();
    }, verified);
}

void CloudBackend::rememberCurrentUser(const QString &id)
{
    if (m_userId == id || numericId(id).isEmpty())
        return;
    const bool accountChanged = !m_userId.isEmpty();
    m_userId = id;
    if (accountChanged) {
        ++m_sessionRevision;
        resetAccountScopedState();
        m_pull.scheduleReconnect(true);
    }
    bindPortalStatusIdentity(accountChanged);
    emit accountScopeChanged();
    m_chats.resumeRecentWithIdentity();
}

bool CloudBackend::isCurrentHistory(bool task, const QString &key, const QString &dialog) const
{
    return canAutoRefreshChats() && !dialog.isEmpty()
        && (task ? m_timeline.activeTask() == key && m_timeline.taskDialog(key) == dialog
                 : m_activeSection == 0 && m_timeline.activeDialog() == dialog);
}

QString CloudBackend::settingsScope() const
{
    const auto identity = numericId(m_userId);
    if (identity.isEmpty())
        return {};
    const auto origin = m_session.origin.toEncoded(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment);
    const auto digest = QCryptographicHash::hash(origin + '\n' + identity.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QString::fromLatin1(digest);
}

void CloudBackend::rememberLoadedTask(const QString &taskId, const QJsonObject &task, quint64 bindingRevision)
{
    rememberTaskTitle(taskId, field(task, {"title", "TITLE"}));
    const auto chatId = explicitMessageChatId(task);
    const auto dialog = chatDialog(QString::number(chatId));
    if (!dialog.isEmpty()) {
        bool groupValid = false;
        const auto group = field(task, {"groupId", "GROUP_ID"}).toLongLong(&groupValid);
        if (groupValid && group >= 0 && bindingRevision == m_projects.unreadBindingRevision()) {
            m_projects.rememberProjectUnreadTask(chatId, taskId.toLongLong(), group);
            m_projects.publishProjectUnreadCounts();
        }
        m_timeline.rememberTaskDialog(taskId, dialog);
    } else {
        m_timeline.forgetTaskDialog(taskId);
    }
}

void CloudBackend::clearDeletedTaskHistory(const QString &taskId)
{
    if (m_timeline.activeTask() == taskId)
        clearOpenHistory(true);
    invalidateTaskChat(taskId);
}

void CloudBackend::forgetTaskChat(const QString &taskId)
{
    m_timeline.forgetTaskDialog(taskId);
    m_chats.forgetTaskTitle(taskId);
}

} // namespace bx
