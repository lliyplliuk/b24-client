#pragma once

#include "core/models.h"
#include "core/notification_targets.h"
#include "ui/workspace_style.h"

#include <QFileInfo>
#include <QHash>
#include <QPersistentModelIndex>
#include <QScrollBar>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <functional>

class QMenu;

namespace bx::ui {

class ChatComposer;
struct ChatDraft;
void switchDraft(ChatComposer *composer, QHash<QString, ChatDraft> &drafts, const QString &from, const QString &to);

QMenu *directoryPinMenu(QWidget *parent, bool pinned, std::function<bool()> canChange,
                        std::function<void()> togglePin);

struct ListViewportAnchor {
    QStringList successors;
    int offset = 0;
    int scroll = 0;
};
ListViewportAnchor listViewportAnchor(const QListWidget *list);
void restoreListViewport(QListWidget *list, const ListViewportAnchor &anchor);
QLabel *emptyState(const QString &text, QWidget *parent);

bool positiveTaskId(const QString &id);
QString taskLink(const QUrl &origin, const QString &projectId, const QString &taskId);
QPersistentModelIndex messageIndex(const QAbstractItemModel *model, const QString &id);
QStringList attachmentNames(const QStringList &files);
QString clockLabel(const QDateTime &time, bool withDate = false);
void showEmpty(QListWidget *list, QLabel *state, const QString &text, bool loading = false);

enum class MessageView { History, Search, Context };
QString messageDayLabel(QDate day);
void presentMessages(QListWidget *list, QLabel *state, const QVector<Message> &messages,
                     MessageView view = MessageView::History);
void setProjectRowUnreadCount(QListWidgetItem *item, qint64 unread, bool partial);
void setChatRowBadges(QListWidgetItem *item, const Chat &chat);
QListWidgetItem *addChatRow(QListWidget *list, const Chat &chat);

} // namespace bx::ui
