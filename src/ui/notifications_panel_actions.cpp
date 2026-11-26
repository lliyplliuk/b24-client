#include "ui/notifications_panel.h"

#include "core/notification_targets.h"
#include "ui/workspace_presentation.h"
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <algorithm>

namespace bx {
QString NotificationsPanel::actionLabel(const NotificationTarget &target, bool preferTargetLabel)
{
    if (preferTargetLabel) {
        const auto label = workspace::markupPlainText(target.label.left(256)).simplified();
        if (!label.isEmpty())
            return label;
    }
    switch (target.kind) {
    case NotificationTargetKind::Chat:
        if (target.taskChat && !target.messageId.isEmpty())
            return QStringLiteral("Открыть в чате задачи");
        return target.messageId.isEmpty() ? QStringLiteral("Открыть чат") : QStringLiteral("Открыть сообщение");
    case NotificationTargetKind::Task:
        return QStringLiteral("Открыть задачу");
    case NotificationTargetKind::Person:
        return QStringLiteral("Открыть профиль");
    case NotificationTargetKind::Project:
        return QStringLiteral("Открыть проект");
    }
    return {};
}

void NotificationsPanel::connectNoticeActions()
{
    const auto beginAction = [this](const QString &action) {
        m_noticeAction = action;
        emit statusChanged(QStringLiteral("Обновляем уведомления…"), true);
        updateNotificationActions();
    };
    connect(m_noticeRead, &QAction::triggered, this, [this, beginAction] {
        const auto *row = m_notifications->currentItem();
        if (!row || !m_noticeRead->isEnabled())
            return;
        const auto id = row->data(workspace::Id).toString();
        if (m_clientUpdates && id == m_clientUpdates->notice().id) {
            m_clientUpdates->markRead();
            return;
        }
        beginAction(operationKey(OperationKind::ReadNotification));
        m_backend->markNotificationRead(id);
    });
    connect(m_noticeReadAll, &QPushButton::clicked, this, [this, beginAction] {
        if (!m_noticeReadAll->isEnabled())
            return;
        if (m_clientUpdates)
            m_clientUpdates->markRead();
        if (!writable() || (m_noticeHistory.isEmpty() && m_noticeSearchResults.isEmpty()))
            return;
        beginAction(operationKey(OperationKind::ReadAllNotifications));
        m_backend->markAllNotificationsRead();
    });
    connect(m_noticeDelete, &QAction::triggered, this, [this, beginAction] {
        const auto *row = m_notifications->currentItem();
        if (!row || !m_noticeDelete->isEnabled())
            return;
        const auto id = row->data(workspace::Id).toString();
        if (QMessageBox::question(this, QStringLiteral("Удалить уведомление"),
                QStringLiteral("Удалить выбранное уведомление?"), QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel) != QMessageBox::Yes || !writable() || !m_noticeAction.isEmpty())
            return;
        beginAction(operationKey(OperationKind::DeleteNotification));
        m_backend->deleteNotification(id);
    });
}

void NotificationsPanel::updateNotificationActions()
{
    scheduleOlderNotifications();
    const bool canWrite = writable() && m_noticeAction.isEmpty();
    const auto *row = m_notifications->currentItem();
    const bool validRow = row && !portalTaskIdentifier(row->data(workspace::Id).toString()).isEmpty();
    const bool localUnread = m_clientUpdates && !m_clientUpdates->notice().id.isEmpty() && !m_clientUpdates->notice().read;
    const bool localSelected = localUnread && row && row->data(workspace::Id).toString() == m_clientUpdates->notice().id;
    m_noticeRead->setEnabled(localSelected || (canWrite && validRow && row->data(workspace::Count).toBool()));
    m_noticeDelete->setEnabled(canWrite && validRow);
    const bool portalRows = !m_noticeHistory.isEmpty() || !m_noticeSearchResults.isEmpty();
    m_noticeReadAll->setEnabled(m_noticeAction.isEmpty() && (localUnread || (canWrite && portalRows)));
    const auto tip = m_readOnly ? QStringLiteral("Недоступно в режиме чтения") : QString();
    m_noticeRead->setToolTip(tip);
    m_noticeDelete->setToolTip(tip);
    m_noticeReadAll->setToolTip(tip);
}

void NotificationsPanel::showNoticeMenu(QListWidgetItem *item, const QPoint &position)
{
    if (!item)
        return;
    m_notifications->setCurrentItem(item);
    const auto id = item->data(workspace::Id).toString();
    auto *menu = new QMenu(m_notifications);
    menu->setObjectName(QStringLiteral("noticeMenu"));
    connect(m_backend, &Backend::accountScopeChanged, menu, [this, menu, scope = m_backend->settingsScope()] {
        if (m_backend->settingsScope() != scope)
            menu->close();
    });
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    connect(menu, &QMenu::aboutToHide, this, [this] {
        QTimer::singleShot(0, this, &NotificationsPanel::scheduleVisibleNoticeRead);
    });
    const auto targets = item->data(workspace::NoticeTargets).value<QVector<NotificationTarget>>();
    const auto notice = item->data(workspace::NoticePresentation).value<workspace::NoticeRow>();
    const auto addTarget = [this, menu, id](const QString &label, const NotificationTarget &target) {
        connect(menu->addAction(label), &QAction::triggered, menu, [this, id, target] {
            if (m_active && noticeItem(id))
                emit targetRequested(target);
        });
    };
    if (notice.primaryTarget >= 0 && notice.primaryTarget < targets.size())
        addTarget(notice.primaryLabel, targets[notice.primaryTarget]);
    for (int n = 0; n < targets.size(); ++n)
        if (n != notice.primaryTarget)
            addTarget(actionLabel(targets[n]), targets[n]);
    for (const auto &href : item->data(workspace::MessageLinks).toStringList()) {
        const auto url = m_workspace.linkDestination(href);
        if (url.isEmpty())
            continue;
        const auto linked = portalLinkTarget(m_backend->portalOrigin(), url);
        if (linked && std::any_of(targets.cbegin(), targets.cend(), [&linked](const auto &target) {
            return target.kind == linked->kind && target.id == linked->id && target.messageId == linked->messageId;
        }))
            continue;
        auto *action = menu->addAction(QStringLiteral("Открыть ссылку · %1").arg(url.toDisplayString().left(100)));
        connect(action, &QAction::triggered, menu, [this, href, id] {
            if (m_active && noticeItem(id))
                emit linkRequested(href, m_notifications);
        });
    }
    if (!menu->isEmpty())
        menu->addSeparator();
    menu->addAction(m_noticeRead);
    menu->addAction(m_noticeDelete);
    menu->popup(m_notifications->viewport()->mapToGlobal(position));
}

void NotificationsPanel::openNotificationTarget(const QListWidgetItem *item, int action)
{
    if (!m_active || !item)
        return;
    const auto targets = item->data(workspace::NoticeTargets).value<QVector<NotificationTarget>>();
    if (action >= 0 && action < targets.size())
        emit targetRequested(targets[action]);
}

} // namespace bx
