#include "ui/notifications_panel.h"

#include "ui/task_dialog_controls.h"
#include "ui/workspace_presentation.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWindow>
#include <utility>

namespace bx {
using ui::emptyState;
using workspace::label;

NotificationsPanel::NotificationsPanel(Backend *backend, bool readOnly, Workspace workspace, QWidget *parent)
    : QWidget(parent), m_backend(backend), m_readOnly(readOnly), m_workspace(std::move(workspace))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 19, 24, 15);
    layout->setSpacing(12);
    auto *header = new QHBoxLayout;
    header->addWidget(label(QStringLiteral("События"), "pageTitle", this), 1);
    m_noticeReadAll = ui::button(QStringLiteral("Отметить все прочитанными"),
                                  QStringLiteral("Прочитать все уведомления"), this);
    header->addWidget(m_noticeReadAll, 0, Qt::AlignTop);
    auto *refresh = new QPushButton(QStringLiteral("Обновить"), this);
    refresh->setToolTip(QStringLiteral("Обновить уведомления (F5)"));
    connect(refresh, &QPushButton::clicked, this, &NotificationsPanel::refreshRequested);
    header->addWidget(refresh, 0, Qt::AlignTop);
    auto *settings = ui::button(QStringLiteral("Настройки"), QStringLiteral("Настройки уведомлений портала"), this);
    settings->setVisible(!m_backend->isDemo());
    header->addWidget(settings, 0, Qt::AlignTop);
    connect(settings, &QPushButton::clicked, this, &NotificationsPanel::showSettings);
    layout->addLayout(header);
    auto *filters = new QHBoxLayout;
    m_noticeSearch = new QLineEdit(this);
    ui::editor(m_noticeSearch);
    m_noticeSearch->setAccessibleName(QStringLiteral("Поиск уведомлений"));
    m_noticeSearch->setPlaceholderText(QStringLiteral("Поиск уведомлений"));
    m_noticeSearch->setClearButtonEnabled(true);
    m_noticeSearch->setMaxLength(200);
    filters->addWidget(m_noticeSearch, 1);
    m_noticeUnreadOnly = new QCheckBox(QStringLiteral("Только непрочитанные"), this);
    m_noticeUnreadOnly->setAccessibleName(QStringLiteral("Только непрочитанные уведомления"));
    filters->addWidget(m_noticeUnreadOnly);
    m_noticeSort = new QComboBox(this);
    ui::editor(m_noticeSort);
    m_noticeSort->setAccessibleName(QStringLiteral("Порядок уведомлений"));
    m_noticeSort->addItems({QStringLiteral("Сначала новые"), QStringLiteral("Сначала старые")});
    filters->addWidget(m_noticeSort);
    layout->addLayout(filters);

    m_noticeRead = new QAction(QStringLiteral("Отметить прочитанным"), this);
    m_noticeRead->setObjectName(QStringLiteral("readNotice"));
    m_noticeDelete = new QAction(QStringLiteral("Удалить уведомление"), this);
    m_noticeDelete->setObjectName(QStringLiteral("deleteNotice"));
    m_noticeSearchTimer = new QTimer(this);
    m_noticeSearchTimer->setSingleShot(true);
    m_noticeSearchTimer->setInterval(280);
    connect(m_noticeSearchTimer, &QTimer::timeout, this, &NotificationsPanel::runNotificationSearch);
    connect(m_noticeSearch, &QLineEdit::textChanged, m_noticeSearchTimer, qOverload<>(&QTimer::start));
    connect(m_noticeSearch, &QLineEdit::returnPressed, this, &NotificationsPanel::runNotificationSearch);
    connect(m_noticeUnreadOnly, &QCheckBox::toggled, this, [this] {
        m_noticeExpandedId.clear();
        showNotifications(true, false);
    });
    connect(m_noticeSort, &QComboBox::currentIndexChanged, this, [this] { showNotifications(true, false); });

    m_notifications = new QListWidget(this);
    m_notifications->setAccessibleName(QStringLiteral("Уведомления"));
    m_noticeReadTimer = new QTimer(this);
    m_noticeReadTimer->setObjectName(QStringLiteral("noticeReadTimer"));
    m_noticeReadTimer->setSingleShot(true);
    m_noticeReadTimer->setInterval(160);
    connect(m_noticeReadTimer, &QTimer::timeout, this, &NotificationsPanel::readVisibleNotices);
    connect(m_notifications->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        emit previewsRequested();
        scheduleVisibleNoticeRead();
        scheduleOlderNotifications();
    });
    connect(m_notifications->verticalScrollBar(), &QScrollBar::rangeChanged,
            this, &NotificationsPanel::scheduleOlderNotifications);
    connect(m_notifications, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (item)
            m_noticeSelectedId = item->data(workspace::Id).toString();
        updateNotificationActions();
    });
    connectNoticeActions();
    auto *delegate = workspace::configureRows(m_notifications, workspace::Row::Notice);
    m_workspace.bindFiles(m_notifications);
    delegate->setNoticeHandlers([this](const QModelIndex &index, int target) {
        QTimer::singleShot(0, this, [this, index = QPersistentModelIndex(index), target] {
            if (index.isValid())
                openNotificationTarget(m_notifications->item(index.row()), target);
        });
    }, [this](const QModelIndex &index) {
        toggleNoticeDetails(m_notifications->item(index.row()));
    }, [this](const QModelIndex &index, const QPoint &position) {
        showNoticeMenu(m_notifications->item(index.row()), position);
    });
    const auto noticeShortcut = [this](const QList<QKeySequence> &keys) {
        auto *action = new QAction(m_notifications);
        action->setShortcuts(keys);
        action->setShortcutContext(Qt::WidgetShortcut);
        m_notifications->addAction(action);
        return action;
    };
    connect(noticeShortcut({QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter), QKeySequence(Qt::Key_Space)}),
            &QAction::triggered, this, [this] { toggleNoticeDetails(m_notifications->currentItem()); });
    connect(noticeShortcut({QKeySequence(Qt::CTRL | Qt::Key_Return), QKeySequence(Qt::CTRL | Qt::Key_Enter)}),
            &QAction::triggered, this, [this] {
        if (const auto *item = m_notifications->currentItem()) {
            const auto notice = item->data(workspace::NoticePresentation).value<workspace::NoticeRow>();
            openNotificationTarget(item, notice.primaryTarget);
        }
    });
    connect(noticeShortcut({QKeySequence(Qt::Key_Menu), QKeySequence(Qt::SHIFT | Qt::Key_F10)}),
            &QAction::triggered, this, [this] {
        auto *item = m_notifications->currentItem();
        if (item)
            showNoticeMenu(item, m_notifications->visualItemRect(item).topLeft());
    });
    m_notifications->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_notifications, &QListWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        showNoticeMenu(m_notifications->itemAt(position), position);
    });
    layout->addWidget(m_notifications, 1);
    m_notificationsEmpty = emptyState(QStringLiteral("Уведомлений пока нет"), this);
    m_notificationsEmpty->setAccessibleName(QStringLiteral("Состояние уведомлений"));
    layout->addWidget(m_notificationsEmpty, 1);
    m_notifications->hide();

    connect(qApp, &QGuiApplication::focusWindowChanged, this, [this] { scheduleVisibleNoticeRead(); });
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive)
            scheduleVisibleNoticeRead();
    });
    connectNotificationHistory();
}

void NotificationsPanel::setSectionActive(bool active)
{
    if (active)
        clearReadAndPageFailures();
    else if (m_active) {
        m_noticeReadTimer->stop();
        m_readNoticesAfterPending = false;
        m_noticeExpandedId.clear();
    }
    m_active = active;
}

} // namespace bx
