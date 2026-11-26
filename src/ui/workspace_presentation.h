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

namespace bx::ui {

inline bool positiveTaskId(const QString &id)
{
    if (id.isEmpty() || id.front() == QLatin1Char('0'))
        return false;
    for (auto character : id) {
        if (character < QLatin1Char('0') || character > QLatin1Char('9'))
            return false;
    }
    return true;
}

inline QString taskLink(const QUrl &origin, const QString &projectId, const QString &taskId)
{
    if (!origin.isValid() || origin.scheme() != QStringLiteral("https") || origin.host().isEmpty() ||
        !origin.userInfo().isEmpty() || origin.hasQuery() || origin.hasFragment() ||
        (!origin.path().isEmpty() && origin.path() != QStringLiteral("/")) ||
        !positiveTaskId(projectId) || !positiveTaskId(taskId))
        return {};
    auto url = origin;
    url.setPath(QStringLiteral("/workgroups/group/%1/tasks/task/view/%2/").arg(projectId, taskId));
    return url.toString(QUrl::FullyEncoded);
}

inline QPersistentModelIndex messageIndex(const QAbstractItemModel *model, const QString &id)
{
    if (!model || id.isEmpty())
        return {};
    for (int row = 0; row < model->rowCount(); ++row) {
        const auto index = model->index(row, 0);
        if (index.data(workspace::ReceiptMessageId).toString() == id)
            return index;
    }
    return {};
}

inline QStringList attachmentNames(const QStringList &files)
{
    QStringList names;
    for (auto path : files) {
        path.replace(QLatin1Char('\\'), QLatin1Char('/'));
        const auto name = QFileInfo(path).fileName();
        if (!name.isEmpty() && !names.contains(name))
            names.append(name);
    }
    return names;
}

inline QString clockLabel(const QDateTime &time, bool withDate = false)
{
    if (!time.isValid())
        return {};
    const auto local = time.toLocalTime();
    return withDate && local.date() != QDate::currentDate()
        ? local.toString(QStringLiteral("dd.MM  HH:mm")) : local.toString(QStringLiteral("HH:mm"));
}

inline void showEmpty(QListWidget *list, QLabel *state, const QString &text, bool loading = false)
{
    list->clear();
    list->hide();
    state->setText(text);
    setLoadingIndicator(state, loading);
    state->show();
}

enum class MessageView { History, Search, Context };

inline QString messageDayLabel(QDate day)
{
    return day == QDate::currentDate() ? QStringLiteral("Сегодня") : day.toString(QStringLiteral("dd.MM.yyyy"));
}

inline void presentMessages(QListWidget *list, QLabel *state, const QVector<Message> &messages,
                            MessageView view = MessageView::History)
{
    if (messages.isEmpty()) {
        showEmpty(list, state, view == MessageView::Search
            ? QStringLiteral("Ничего не найдено") : QStringLiteral("Сообщений пока нет"));
        return;
    }

    const auto *scroll = list->verticalScrollBar();
    const int previous = scroll->value();
    const bool openingHistory = view == MessageView::History && list->count() == 0;
    const bool follow = list->count() == 0 || scroll->maximum() - previous < 24;
    QHash<QString, const Message *> replySources;
    if (std::any_of(messages.cbegin(), messages.cend(), [](const Message &message) {
            return !message.replyId.isEmpty();
        })) {
        for (const auto &message : messages)
            if (!message.id.isEmpty())
                replySources.insert(message.id, &message);
    }
    list->clear();
    QListWidgetItem *firstUnread = nullptr;
    for (int i = 0; i < messages.size(); ++i) {
        const auto &message = messages[i];
        const bool unreadBoundary = view == MessageView::History && !firstUnread &&
                                    message.unread && !message.outgoing;
        const bool grouped = !unreadBoundary && i > 0 && messages[i - 1].author == message.author &&
                             messages[i - 1].authorAvatarUrl == message.authorAvatarUrl &&
                             messages[i - 1].system == message.system &&
                             messages[i - 1].outgoing == message.outgoing &&
                             messages[i - 1].createdAt.isValid() && message.createdAt.isValid() &&
                             messages[i - 1].createdAt.secsTo(message.createdAt) >= 0 &&
                             messages[i - 1].createdAt.secsTo(message.createdAt) <= 300;
        const auto author = message.system ? QStringLiteral("Системное сообщение") : message.outgoing ? QStringLiteral("Вы")
            : (message.author.isEmpty() ? QStringLiteral("Участник") : message.author);
        const auto meta = clockLabel(message.createdAt);
        const auto title = grouped || message.outgoing || message.system ? QString() : author;
        const bool nativeReply = !message.replyId.isEmpty() || !message.replyAuthor.isEmpty() ||
                                 !message.replyText.isEmpty() || !message.replyFiles.isEmpty();
        const auto legacyQuote = workspace::leadingAttributedMessageQuote(message.text);
        const auto *replySource = nativeReply ? replySources.value(message.replyId) : nullptr;
        const auto &replyFiles = message.replyFiles.isEmpty() && replySource ? replySource->files : message.replyFiles;
        QString replyAuthor;
        QString replyExcerpt;
        if (nativeReply) {
            replyAuthor = workspace::markupPlainText(message.replyAuthor.left(512)).simplified();
            if (replyAuthor.isEmpty() && replySource)
                replyAuthor = replySource->outgoing ? QStringLiteral("Вы") : replySource->author.simplified();
            const auto &rawQuote = message.replyText.isEmpty() && replySource ? replySource->text : message.replyText;
            replyExcerpt = workspace::markupPlainText(rawQuote.left(1024)).simplified().left(240);
        }
        const bool splitQuote = legacyQuote && (!nativeReply
            || (legacyQuote->author == replyAuthor && legacyQuote->excerpt == replyExcerpt));
        auto *item = workspace::addMarkupRow(list, title, splitQuote ? legacyQuote->body : message.text, meta,
                                              message.id.isEmpty() ? QString::number(i + 1) : message.id);
        if (!title.isEmpty())
            item->setData(workspace::AvatarUrl, message.authorAvatarUrl);
        QStringList context;
        item->setData(workspace::MessageFiles, QVariant::fromValue(message.files));
        if (message.forwarded) {
            const auto origin = workspace::markupPlainText(message.forwardedAuthor.left(512)).simplified();
            const auto byline = origin.isEmpty() ? QStringLiteral("Пересланное сообщение")
                                                  : QStringLiteral("Переслано · %1").arg(origin);
            item->setData(workspace::ForwardByline, byline);
            context.append(byline);
        }
        if (nativeReply) {
            item->setData(workspace::ReplyFiles, QVariant::fromValue(replyFiles));
            const auto byline = replyAuthor.isEmpty() ? QStringLiteral("Ответ на сообщение")
                                                     : QStringLiteral("Ответ · %1").arg(replyAuthor);
            if (replyExcerpt.isEmpty() && (!replyFiles.isEmpty() || (replySource && !replySource->attachments.isEmpty())))
                replyExcerpt = QStringLiteral("Вложение");
            if (replyExcerpt.isEmpty())
                replyExcerpt = message.replyId.isEmpty() ? QStringLiteral("Исходное сообщение недоступно")
                    : QStringLiteral("Сообщение №%1").arg(message.replyId);
            item->setData(workspace::ReplyTargetId, portalTaskIdentifier(message.replyId));
            item->setData(workspace::ReplyByline, byline);
            item->setData(workspace::ReplyPreview, replyExcerpt);
            context.append(byline + QStringLiteral(". ") + replyExcerpt);
            for (const auto &file : replyFiles)
                context.append(file.name);
        } else if (splitQuote) {
            item->setData(workspace::ReplyByline, legacyQuote->author);
            item->setData(workspace::ReplyPreview, legacyQuote->excerpt);
            context.append(legacyQuote->author + QStringLiteral(". ") + legacyQuote->excerpt);
        }
        if (unreadBoundary) {
            firstUnread = item;
            item->setData(workspace::UnreadDivider, true);
        }
        if (message.createdAt.isValid()) {
            const auto day = message.createdAt.toLocalTime().date();
            item->setData(workspace::MessageCalendarDay, day);
            if (i == 0 || !messages[i - 1].createdAt.isValid() ||
                day != messages[i - 1].createdAt.toLocalTime().date()) {
                item->setData(workspace::DayDivider, messageDayLabel(day));
            }
        }
        auto plain = item->data(workspace::Detail).toString();
        auto files = attachmentNames(message.attachments);
        for (const auto &file : message.files) {
            files.removeAll(attachmentNames({file.name}).value(0));
            context.append(file.name);
        }
        const auto fileText = files.isEmpty() ? QString() : QStringLiteral("Вложения: %1").arg(files.join(QStringLiteral(", ")));
        if (!fileText.isEmpty()) {
            if (!plain.isEmpty())
                plain += QLatin1Char('\n');
            plain += fileText;
            const auto html = item->data(workspace::MarkupHtml).toString();
            if (!html.isEmpty())
                item->setData(workspace::MarkupHtml, html + QStringLiteral("<br>") + fileText.toHtmlEscaped());
            item->setData(workspace::Detail, plain);
        }
        if (splitQuote || !fileText.isEmpty()) {
            // The delegate paints the split body, while the item retains the full text for whole-row consumers.
            auto listText = splitQuote ? workspace::markupPlainText(message.text) : plain;
            if (splitQuote && !fileText.isEmpty())
                listText += QLatin1Char('\n') + fileText;
            auto visible = QStringList{title, listText, meta};
            visible.removeAll(QString());
            item->setText(visible.join(QLatin1Char('\n')));
        }
        item->setData(workspace::Outgoing, message.outgoing);
        item->setData(workspace::MessageSeen, message.seenByOthers);
        item->setData(workspace::MessageUnread, view == MessageView::History && message.unread && !message.outgoing);
        item->setData(workspace::MessageAuthor, author);
        item->setData(workspace::ReceiptMessageId, portalTaskIdentifier(message.id));
        item->setData(workspace::SystemMessage, message.system);
        item->setData(workspace::RawMessageBody, message.text);
        QVariantMap reactionCounts;
        QStringList ownReactions;
        for (const auto &reaction : message.reactions) {
            reactionCounts.insert(reaction.code, reaction.count);
            if (reaction.own)
                ownReactions.append(reaction.code);
        }
        item->setData(workspace::MessageReactionCounts, reactionCounts);
        item->setData(workspace::OwnMessageReactions, ownReactions);
        context.prepend(author);
        if (unreadBoundary)
            context.prepend(workspace::UnreadMessages);
        context.append(clockLabel(message.createdAt, true));
        if (message.outgoing && message.seenByOthers)
            context.append(QStringLiteral("Просмотрено"));
        context.append(plain);
        context.removeAll(QString());
        item->setData(Qt::AccessibleTextRole, context.join(QStringLiteral(". ")));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    }
    state->hide();
    list->show();
    if (openingHistory && firstUnread) {
        list->scrollToItem(firstUnread, QAbstractItemView::PositionAtTop);
        if (!list->isVisible()) {
            const QPersistentModelIndex boundary(list->model()->index(list->row(firstUnread), 0));
            QTimer::singleShot(0, list, [list, boundary] {
                if (boundary.isValid())
                    list->scrollToItem(list->item(boundary.row()), QAbstractItemView::PositionAtTop);
            });
        }
    } else if (follow) {
        list->scrollToBottom();
    } else {
        list->verticalScrollBar()->setValue(previous);
    }
}

inline void setProjectRowUnreadCount(QListWidgetItem *item, qint64 unread, bool partial)
{
    item->setData(workspace::Count, unread);
    item->setData(workspace::CountPartial, unread > 0 && partial);
    QStringList status;
    if (item->data(workspace::Pinned).toBool())
        status.append(QStringLiteral("Закреплён"));
    if (unread > 0)
        status.append((partial ? QStringLiteral("Непрочитанных сообщений: не менее %1")
                               : QStringLiteral("Непрочитанных сообщений: %1")).arg(unread));
    item->setData(Qt::AccessibleDescriptionRole, status.join(QStringLiteral(". ")));
}

inline void setChatRowBadges(QListWidgetItem *item, const Chat &chat)
{
    item->setData(workspace::Count, chat.unread);
    item->setData(workspace::Pinned, chat.pinned);
    item->setData(workspace::MarkedUnread, chat.markedUnread);
    item->setData(workspace::HasMention, chat.hasMention);
    item->setData(workspace::HasReaction, chat.hasReaction);
    QStringList status;
    if (chat.pinned)
        status.append(QStringLiteral("Закреплён"));
    if (chat.unread > 0)
        status.append(QStringLiteral("Непрочитанных: %1").arg(chat.unread));
    else if (chat.markedUnread)
        status.append(QStringLiteral("Отмечен непрочитанным"));
    if (chat.hasMention)
        status.append(QStringLiteral("Непрочитанное упоминание"));
    if (chat.hasReaction)
        status.append(QStringLiteral("Новая реакция на ваше сообщение"));
    item->setData(Qt::AccessibleDescriptionRole, status.join(QStringLiteral(". ")));
}

inline QListWidgetItem *addChatRow(QListWidget *list, const Chat &chat)
{
    auto *item = workspace::addRow(list, chat.title, workspace::markupPlainText(chat.lastMessage),
                                   clockLabel(chat.updated), chat.id);
    item->setData(workspace::ChatMessagePreview, true);
    item->setData(workspace::AvatarUrl, chat.avatarUrl);
    item->setData(workspace::AvatarType, int(!chat.taskChat && chat.kind == QStringLiteral("im")
        ? workspace::AvatarKind::Person : workspace::AvatarKind::Chat));
    if (!chat.taskChat && chat.kind == QStringLiteral("im")) {
        item->setData(workspace::ContactOnlineUntil, chat.onlineUntil);
        item->setData(workspace::ContactPosition, chat.position);
    }
    item->setData(workspace::TaskId, chat.taskId);
    setChatRowBadges(item, chat);
    return item;
}

} // namespace bx::ui
