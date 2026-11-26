#include "ui/workspace_message_viewers.h"

#include "core/backend.h"
#include "core/notification_targets.h"
#include "ui/workspace_style.h"

#include <QGuiApplication>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QVBoxLayout>

namespace bx::workspace {

QWidget *openMessageViewers(Backend *backend, QListWidget *history, const QString &messageId,
                            const QPoint &anchor, const QString &scope, bool task,
                            std::function<bool()> sameHistory, ViewerAvatars avatars)
{
    if (scope.isEmpty() || portalTaskIdentifier(messageId).isEmpty())
        return nullptr;

    const auto *screen = QGuiApplication::screenAt(anchor);
    if (!screen)
        screen = history->screen();
    if (!screen || screen->availableGeometry().isEmpty())
        return nullptr;
    const auto available = screen->availableGeometry();

    auto *menu = new QFrame(history, Qt::Popup);
    menu->setObjectName(QStringLiteral("messageViewersMenu"));
    menu->setAccessibleName(QStringLiteral("Кто прочитал сообщение"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setFrameShape(QFrame::StyledPanel);
    menu->setAutoFillBackground(true);
    menu->setMinimumWidth(qMin(260, available.width()));
    menu->setMaximumSize(available.size());
    auto *layout = new QVBoxLayout(menu);
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    layout->setContentsMargins(10, 8, 10, 8);
    auto *heading = new QLabel(QStringLiteral("Просмотрели сообщение"), menu);
    heading->setFont(workspace::rowFont(ui::bodyTextPixels, true));
    heading->setWordWrap(true);
    layout->addWidget(heading);
    auto *status = new QLabel(QStringLiteral("Загружаем список…"), menu);
    status->setObjectName(QStringLiteral("messageViewersStatus"));
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    layout->addWidget(status);
    auto *rows = new QListWidget(menu);
    rows->setAccessibleName(QStringLiteral("Список прочитавших"));
    rows->setObjectName(QStringLiteral("messageViewersList"));
    rows->setIconSize(QSize(22, 22));
    rows->setSelectionMode(QAbstractItemView::NoSelection);
    rows->setFrameShape(QFrame::NoFrame);
    rows->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    const int rowWidth = qBound(1, available.width() - 22, 230);
    rows->setMinimumWidth(rowWidth);
    rows->hide();
    layout->addWidget(rows);
    auto *actions = new QHBoxLayout;
    auto *retry = new QPushButton(QStringLiteral("Повторить"), menu);
    retry->setAccessibleName(QStringLiteral("Повторить загрузку списка прочитавших"));
    retry->setAutoDefault(false);
    retry->hide();
    actions->addWidget(retry);
    actions->addStretch();
    auto *close = new QPushButton(QStringLiteral("Закрыть"), menu);
    close->setAutoDefault(false);
    actions->addWidget(close);
    layout->addLayout(actions);
    QObject::connect(close, &QPushButton::clicked, menu, &QWidget::close);
    QObject::connect(backend, &Backend::accountScopeChanged, menu, &QWidget::close);
    QObject::connect(backend, &Backend::messageLinkPreviewAccessChanged, menu, [menu](bool available) {
        if (!available)
            menu->close();
    });

    const auto refreshAvatars = [avatars, rows] {
        if (!rows->isVisible())
            return;
        for (int row = 0; row < rows->count(); ++row) {
            auto *item = rows->item(row);
            if (!rows->viewport()->rect().intersects(rows->visualItemRect(item)))
                continue;
            const auto url = item->data(workspace::AvatarUrl).toUrl();
            const auto *photo = avatars.cached(url);
            QPixmap pixmap(22, 22);
            pixmap.fill(Qt::transparent);
            {
                QPainter painter(&pixmap);
                painter.setRenderHint(QPainter::Antialiasing);
                workspace::paintAvatar(painter, pixmap.rect(), item->text(), workspace::AvatarKind::Person,
                                       photo ? *photo : QPixmap{});
            }
            item->setIcon(QIcon(pixmap));
            avatars.request(url);
        }
    };
    QObject::connect(rows->verticalScrollBar(), &QScrollBar::valueChanged, menu, refreshAvatars);
    QObject::connect(backend, &Backend::avatarLoaded, menu, refreshAvatars, Qt::QueuedConnection);
    constexpr int rowHeight = 30;
    const auto positionPopup = [anchor, available, menu, layout, rows] {
        layout->invalidate();
        const int width = qBound(menu->minimumWidth(), layout->sizeHint().width(), available.width());
        const auto heightForWidth = [layout, width] {
            const int wrapped = layout->totalHeightForWidth(width);
            return wrapped >= 0 ? wrapped : layout->sizeHint().height();
        };
        if (!rows->isHidden()) {
            const int wanted = qMin(rows->count(), 6) * rowHeight + 2;
            rows->setFixedHeight(0);
            layout->invalidate();
            rows->setFixedHeight(qBound(0, available.height() - heightForWidth(), wanted));
            layout->invalidate();
        }
        const int height = qMin(available.height(), heightForWidth());
        menu->resize(width, height);
        layout->activate();
        const int maxX = available.right() - width + 1;
        const int maxY = available.bottom() - height + 1;
        const int y = anchor.y() > maxY ? anchor.y() - height : anchor.y();
        menu->move(qBound(available.left(), anchor.x(), maxX), qBound(available.top(), y, maxY));
    };

    const auto request = [backend, scope, messageId, task, sameHistory, popup = QPointer<QFrame>(menu),
                          status, heading, rows, retry, rowWidth, refreshAvatars, positionPopup] {
        if (!popup || !popup->isVisible())
            return;
        if (!sameHistory()) {
            popup->close();
            return;
        }
        status->setText(QStringLiteral("Загружаем список…"));
        status->show();
        rows->hide();
        retry->setEnabled(false);
        retry->hide();
        positionPopup();
        backend->requestMessageViewers(scope, messageId, task,
            [popup, sameHistory, status, heading, rows, retry, rowWidth, refreshAvatars, positionPopup]
            (std::optional<MessageViewers> result) {
                if (!popup || !popup->isVisible())
                    return;
                if (!sameHistory()) {
                    popup->close();
                    return;
                }
                if (!result) {
                    status->setText(QStringLiteral("Не удалось получить список"));
                    retry->setEnabled(true);
                    retry->show();
                } else if (result->people.isEmpty()) {
                    status->setText(QStringLiteral("Портал ещё не подтвердил просмотры"));
                } else {
                    if (result->count >= 0)
                        heading->setText(QStringLiteral("Просмотрели сообщение · %1").arg(result->count));
                    rows->clear();
                    for (const auto &person : result->people) {
                        auto *row = new QListWidgetItem(person.name, rows);
                        row->setData(workspace::AvatarUrl, person.avatarUrl);
                        row->setSizeHint(QSize(rowWidth, rowHeight));
                    }
                    rows->show();
                    if (result->hasMore)
                        status->setText(QStringLiteral("Показаны первые %1").arg(rows->count()));
                    else
                        status->hide();
                }
                positionPopup();
                refreshAvatars();
            });
    };
    QObject::connect(retry, &QPushButton::clicked, menu, request);
    positionPopup();
    menu->show();
    request();
    return menu;
}

} // namespace bx::workspace
