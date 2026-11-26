#include "ui/main_window.h"
#include "ui/chat_panel.h"
#include "ui/workspace_message_previews.h"
#include "ui/projects_panel.h"
#include "ui/avatar_badge.h"
#include "ui/history_list.h"
#include "ui/workspace_row_style.h"

#include <QBuffer>
#include <QDialog>
#include <QImageReader>
#include <QTimer>

namespace bx {

void MainWindow::onAvatarLoaded(const QUrl &url, const QByteArray &bytes)
{
    if (!m_requestedAvatars.remove(url))
        return;
    auto *photo = new QPixmap;
    if (!bytes.isEmpty() && bytes.size() <= 512 * 1024) {
        QBuffer input;
        input.setData(bytes);
        input.open(QIODevice::ReadOnly);
        QImageReader reader(&input);
        const auto format = reader.format();
        const auto size = reader.size();
        if ((format == "png" || format == "jpeg" || format == "webp" || format == "gif") &&
            size.isValid() && !size.isEmpty() && size.width() <= 1200 && size.height() <= 1200) {
            const auto scaled = size.scaled(144, 144, Qt::KeepAspectRatio);
            reader.setScaledSize(QSize(qMax(1, scaled.width()), qMax(1, scaled.height())));
            const auto image = reader.read();
            if (!image.isNull() && image.width() <= 1200 && image.height() <= 1200) {
                const int side = qMin(image.width(), image.height());
                const QRect crop((image.width() - side) / 2, (image.height() - side) / 2, side, side);
                *photo = QPixmap::fromImage(image.copy(crop).scaled(72, 72, Qt::IgnoreAspectRatio,
                                                                    Qt::SmoothTransformation));
            }
        }
    }
    m_avatars.insert(url, photo);
    m_projectsPanel->refreshBoardAvatars();
    m_chatPanel->refreshMentionAvatars();
    m_tasks->refreshAvatars();
    updateAvatarRows();
    scheduleAvatars();
}

void MainWindow::scheduleAvatars()
{
    if (m_avatarUpdateQueued)
        return;
    m_avatarUpdateQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_avatarUpdateQueued = false;
        loadVisibleAvatars();
        m_messagePreviews->loadVisibleChatPreviews();
    });
}

void MainWindow::requestAvatar(const QUrl &url)
{
    if (m_requestedAvatars.size() >= 4 || !url.isValid() || url.scheme() != QStringLiteral("https") ||
        url.host().isEmpty() || !url.userInfo().isEmpty() || m_avatars.contains(url) || m_requestedAvatars.contains(url))
        return;
    m_requestedAvatars.insert(url);
    m_backend->requestAvatar(url);
}

void MainWindow::loadVisibleAvatars()
{
    const auto requestVisible = [this](QListWidget *list) {
        if (!list || !list->isVisible())
            return;
        const auto first = list->indexAt(QPoint(4, 3));
        for (int row = first.isValid() ? first.row() : 0; row < list->count(); ++row) {
            const auto *item = list->item(row);
            const auto rect = list->visualItemRect(item);
            if (rect.top() >= list->viewport()->height())
                break;
            if (rect.bottom() >= 0)
                requestAvatar(item->data(workspace::AvatarUrl).toUrl());
        }
    };
    m_tasks->requestVisibleAvatars();
    if (m_lastSection == 0) {
        m_chatPanel->requestVisibleAvatars([this](const QUrl &url) { requestAvatar(url); }, requestVisible);
    } else if (m_lastSection == 1) {
        m_projectsPanel->requestVisibleAvatars([this](const QUrl &url) { requestAvatar(url); }, requestVisible);
    }
    if (auto *history = m_tasks->history())
        requestVisible(history);
    if (m_personDialog)
        requestAvatar(m_personAvatarUrl);
}

void MainWindow::updateAvatarRows()
{
    const auto updateList = [this](QListWidget *list) {
        for (int i = 0; i < list->count(); ++i) {
            auto *item = list->item(i);
            const auto url = item->data(workspace::AvatarUrl).toUrl();
            if (!url.isEmpty()) {
                const auto *photo = m_avatars.object(url);
                item->setData(workspace::AvatarImage, photo ? QVariant::fromValue(*photo) : QVariant{});
            }
        }
        if (list == m_chatPanel->history() || list == m_tasks->history())
            list->viewport()->update();
    };
    m_chatPanel->updateAvatars(updateList);
    m_projectsPanel->updateAvatars(updateList);
    if (auto *history = m_tasks->history())
        updateList(history);
    if (m_personDialog) {
        if (const auto *photo = m_avatars.object(m_personAvatarUrl))
            m_personAvatar->setPhoto(*photo);
    }
    m_events->repaintRows();
}

} // namespace bx
