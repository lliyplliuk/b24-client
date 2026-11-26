#include "ui/notifications_panel.h"

#include "ui/workspace_presentation.h"
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QSignalBlocker>
#include <algorithm>
#include <iterator>
#include <utility>

namespace bx {
using ui::ListViewportAnchor;
using ui::clockLabel;
using ui::listViewportAnchor;
using ui::restoreListViewport;
using ui::showEmpty;

namespace {
void presentNoticeRow(QListWidgetItem *item, const workspace::NoticeRow &notice)
{
    item->setData(workspace::NoticePresentation, QVariant::fromValue(notice));
    QStringList text{notice.day, item->data(workspace::Title).toString(), item->data(workspace::Meta).toString(),
        notice.expanded ? item->data(workspace::Detail).toString() : notice.excerpt};
    text.removeAll(QString());
    item->setData(Qt::AccessibleTextRole, text.join(QStringLiteral(". ")));
    QStringList details;
    if (item->data(workspace::Count).toBool())
        details.append(QStringLiteral("Непрочитанное"));
    details.append(notice.expanded ? QStringLiteral("Подробности раскрыты") : QStringLiteral("Enter — подробности"));
    if (notice.primaryTarget >= 0)
        details.append(QStringLiteral("Ctrl+Enter — %1").arg(notice.primaryLabel));
    details.append(QStringLiteral("Shift+F10 — действия"));
    item->setData(Qt::AccessibleDescriptionRole, details.join(QStringLiteral(". ")));
}
} // namespace

void NotificationsPanel::showNotifications(bool markVisible, bool preserveScroll)
{
    if (!m_active)
        return;
    const bool remoteSearch = m_noticeQuery.size() >= 3;
    auto notices = remoteSearch ? m_noticeSearchResults : m_noticeHistory;
    if (m_clientUpdates) {
        const auto &local = m_clientUpdates->notice();
        if (!local.id.isEmpty() && (local.title.contains(m_noticeQuery, Qt::CaseInsensitive)
                                    || local.text.contains(m_noticeQuery, Qt::CaseInsensitive)))
            notices.append(local);
    }
    notices.erase(std::remove_if(notices.begin(), notices.end(), [this, remoteSearch](const Notification &notice) {
        return (m_noticeUnreadOnly->isChecked() && notice.read && notice.id != m_noticeExpandedId) ||
            (!remoteSearch && !m_noticeQuery.isEmpty() &&
             !notice.title.contains(m_noticeQuery, Qt::CaseInsensitive) &&
             !notice.text.contains(m_noticeQuery, Qt::CaseInsensitive));
    }), notices.end());
    const bool ascending = m_noticeSort->currentIndex() == 1;
    std::stable_sort(notices.begin(), notices.end(), [ascending](const Notification &a, const Notification &b) {
        if (a.createdAt != b.createdAt)
            return ascending ? a.createdAt < b.createdAt : a.createdAt > b.createdAt;
        return ascending ? a.id.toLongLong() < b.id.toLongLong() : a.id.toLongLong() > b.id.toLongLong();
    });
    const auto anchor = preserveScroll ? listViewportAnchor(m_notifications) : ListViewportAnchor{};
    const QSignalBlocker blocker(m_notifications);
    const QSignalBlocker scrollBlocker(m_notifications->verticalScrollBar());
    if (notices.isEmpty()) {
        const bool loading = remoteSearch ? m_noticeSearchPending : m_noticeHistoryState == NoticeHistoryState::Loading;
        auto message = QStringLiteral("Уведомлений пока нет");
        if (loading)
            message = remoteSearch ? QStringLiteral("Ищем уведомления…") : QStringLiteral("Загружаем уведомления…");
        else if (!remoteSearch && m_noticeHistory.isEmpty() && m_noticeHistoryState == NoticeHistoryState::Failed)
            message = QStringLiteral("Не удалось загрузить уведомления");
        else if (!m_noticeQuery.isEmpty())
            message = QStringLiteral("Уведомлений по запросу нет");
        else if (m_noticeUnreadOnly->isChecked())
            message = QStringLiteral("Непрочитанных уведомлений нет");
        showEmpty(m_notifications, m_notificationsEmpty, message, loading);
        updateNotificationActions();
        return;
    }
    m_notifications->clear();
    QListWidgetItem *selected = nullptr;
    QDate previousDay;
    const auto yesterday = QDate::currentDate().addDays(-1);
    for (const auto &notice : notices) {
        const auto &content = notice.markup.isEmpty() ? notice.text : notice.markup;
        auto *item = workspace::addMarkupRow(m_notifications, notice.displayTitle, content,
                                             clockLabel(notice.createdAt), notice.id);
        item->setData(workspace::Count, !notice.read);
        item->setData(workspace::NoticeTargets, QVariant::fromValue(notice.targets));
        workspace::NoticeRow row;
        const auto day = notice.createdAt.toLocalTime().date();
        if (day.isValid() && day != previousDay)
            row.day = day == yesterday ? QStringLiteral("Вчера") : ui::messageDayLabel(day);
        previousDay = day;
        row.excerpt = notice.excerpt;
        const auto message = std::find_if(notice.targets.cbegin(), notice.targets.cend(), [](const auto &target) {
            return target.kind == NotificationTargetKind::Chat && !target.messageId.isEmpty();
        });
        const auto task = std::find_if(notice.targets.cbegin(), notice.targets.cend(), [](const auto &target) {
            return target.kind == NotificationTargetKind::Task;
        });
        if (!notice.targets.isEmpty()) {
            const auto primary = message != notice.targets.cend() ? message
                : task != notice.targets.cend() ? task : notice.targets.cbegin();
            row.primaryLabel = actionLabel(*primary, false);
            row.primaryTarget = int(std::distance(notice.targets.cbegin(), primary));
        }
        row.expanded = !m_noticeExpandedId.isEmpty() && notice.id == m_noticeExpandedId;
        presentNoticeRow(item, row);
        if (notice.id == m_noticeSelectedId)
            selected = item;
    }
    if (selected)
        m_notifications->setCurrentItem(selected);
    m_notificationsEmpty->hide();
    m_notifications->show();
    restoreListViewport(m_notifications, anchor);
    updateNotificationActions();
    emit previewsRequested();
    if (markVisible)
        scheduleVisibleNoticeRead();
}

QVector<int> NotificationsPanel::visibleNoticeRows() const
{
    QVector<int> rows;
    if (!m_active || !m_notifications->isVisible())
        return rows;
    const auto visible = m_notifications->viewport()->visibleRegion().boundingRect();
    if (visible.isEmpty())
        return rows;
    const auto *delegate = static_cast<const workspace::RowPainter *>(m_notifications->itemDelegate());
    const auto first = m_notifications->indexAt(QPoint(visible.left() + 2, visible.top()));
    for (int row = first.isValid() ? first.row() : 0; row < m_notifications->count(); ++row) {
        const auto item = m_notifications->model()->index(row, 0);
        const auto bounds = m_notifications->visualRect(item);
        if (bounds.top() > visible.bottom())
            break;
        const auto card = delegate->receiptArea(item, bounds);
        if (!card.isEmpty() && card.intersected(visible).height() >= qMin(24, card.height()))
            rows.append(row);
    }
    return rows;
}

void NotificationsPanel::toggleNoticeDetails(QListWidgetItem *item)
{
    if (!item)
        return;
    const auto id = item->data(workspace::Id).toString();
    if (id.isEmpty())
        return;
    const auto anchor = listViewportAnchor(m_notifications);
    const auto previous = std::exchange(m_noticeExpandedId, {});
    auto row = item->data(workspace::NoticePresentation).value<workspace::NoticeRow>();
    row.expanded = !row.expanded;
    if (row.expanded)
        m_noticeExpandedId = id;
    if (auto *other = noticeItem(previous)) {
        if (m_noticeUnreadOnly->isChecked() && !other->data(workspace::Count).toBool()) {
            showNotifications();
            return;
        }
        if (other != item) {
            auto previousRow = other->data(workspace::NoticePresentation).value<workspace::NoticeRow>();
            previousRow.expanded = false;
            presentNoticeRow(other, previousRow);
        }
    }
    presentNoticeRow(item, row);
    restoreListViewport(m_notifications, anchor);
    emit previewsRequested();
}

QListWidgetItem *NotificationsPanel::noticeItem(const QString &id) const
{
    if (!id.isEmpty()) {
        for (int row = 0; row < m_notifications->count(); ++row) {
            auto *item = m_notifications->item(row);
            if (item->data(workspace::Id).toString() == id)
                return item;
        }
    }
    return nullptr;
}

void NotificationsPanel::scrollToTop()
{
    m_notifications->scrollToTop();
}

void NotificationsPanel::repaintRows()
{
    if (m_notifications->isVisible())
        m_notifications->viewport()->update();
}

void NotificationsPanel::refreshPreviewRows()
{
    if (m_notifications->isVisible()) {
        const auto anchor = listViewportAnchor(m_notifications);
        const QSignalBlocker blocker(m_notifications->verticalScrollBar());
        restoreListViewport(m_notifications, anchor);
        m_notifications->viewport()->update();
    }
}

QStringList NotificationsPanel::visiblePreviewLinks() const
{
    QStringList result;
    for (const int row : visibleNoticeRows()) {
        const auto *item = m_notifications->item(row);
        if (!item->data(workspace::NoticePresentation).value<workspace::NoticeRow>().expanded)
            continue;
        const auto links = item->data(workspace::MessageLinks).toStringList();
        result.append(links);
    }
    return result;
}

} // namespace bx
