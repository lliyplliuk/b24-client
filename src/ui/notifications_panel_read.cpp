#include "ui/notifications_panel.h"

#include "core/notification_targets.h"
#include "ui/workspace_presentation.h"
#include <QApplication>
#include <utility>

namespace bx {
bool NotificationsPanel::canReadVisibleNotices() const
{
    return m_active && writable() && m_noticeAction.isEmpty() && m_workspace.sessionReady()
        && !m_noticeSearchPending && !m_noticeSearchTimer->isActive()
        && !window()->isMinimized() && QApplication::activeWindow() == window()
        && QGuiApplication::applicationState() == Qt::ApplicationActive
        && !QApplication::activeModalWidget() && !QApplication::activePopupWidget();
}

void NotificationsPanel::scheduleVisibleNoticeRead()
{
    if (!m_noticeReadTimer || !canReadVisibleNotices())
        return;
    if (!m_pendingNoticeRead.isEmpty())
        m_readNoticesAfterPending = true;
    else
        m_noticeReadTimer->start();
}

void NotificationsPanel::readVisibleNotices()
{
    if (!canReadVisibleNotices() || !m_pendingNoticeRead.isEmpty())
        return;
    bool readClientUpdate = false;
    for (const int row : visibleNoticeRows()) {
        const auto *item = m_notifications->item(row);
        if (!item->data(workspace::Count).toBool())
            continue;
        auto id = item->data(workspace::Id).toString();
        if (m_clientUpdates && id == m_clientUpdates->notice().id) {
            readClientUpdate = true;
            continue;
        }
        if (!isLocalNotificationId(id))
            id = portalTaskIdentifier(id);
        if (!id.isEmpty() && !m_failedNoticeRead.contains(id) && !m_pendingNoticeRead.contains(id))
            m_pendingNoticeRead.append(id);
    }
    if (readClientUpdate)
        m_clientUpdates->markRead();
    if (!m_pendingNoticeRead.isEmpty())
        m_backend->markNotificationsRead(QStringList(m_pendingNoticeRead));
}

void NotificationsPanel::finishVisibleNoticeRead(bool success)
{
    if (!success) {
        for (const auto &id : std::as_const(m_pendingNoticeRead))
            m_failedNoticeRead.insert(id);
    }
    m_pendingNoticeRead.clear();
    if (std::exchange(m_readNoticesAfterPending, false))
        scheduleVisibleNoticeRead();
}

} // namespace bx
