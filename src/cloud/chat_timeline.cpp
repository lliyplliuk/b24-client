#include "chat_timeline.h"
#include "core/tunables.h"

#include <algorithm>
#include <utility>

namespace bx {

ChatTimeline::RecentOverlay &ChatTimeline::recentOverlay(const QString &dialog)
{
    if (!recentOverlays.contains(dialog) && recentOverlays.size() >= 3 * tunables::kPageSizeChats)
        recentOverlays.erase(recentOverlays.begin());
    return recentOverlays[dialog];
}

ChatTimeline::DialogMessageState &ChatTimeline::dialogMessageState(const QString &dialog)
{
    if (!dialogMessageStates.contains(dialog) && dialogMessageStates.size() >= tunables::kPageSizeChats) {
        const auto taskDialog = taskDialogs.value(openTask);
        auto candidate = dialogMessageStates.begin();
        while (candidate != dialogMessageStates.end()
               && (candidate.key() == openDialog || candidate.key() == taskDialog))
            ++candidate;
        if (candidate != dialogMessageStates.end())
            dialogMessageStates.erase(candidate);
    }
    return dialogMessageStates[dialog];
}

QString ChatTimeline::replyPreviewKey(const QString &dialog, const QString &message)
{
    return dialog + QLatin1Char('\n') + message;
}

void ChatTimeline::rememberSentTemplate(const QString &id)
{
    sentTemplates.enqueue(id);
    if (sentTemplates.size() > 64)
        sentTemplates.dequeue();
}

const ChatTimeline::DialogMessageState *ChatTimeline::findMessageState(const QString &dialog) const
{
    const auto it = dialogMessageStates.constFind(dialog);
    return it == dialogMessageStates.cend() ? nullptr : &it.value();
}

quint64 ChatTimeline::historyGeneration(const QString &dialog) const
{
    const auto *state = findMessageState(dialog);
    return state ? state->historyGeneration : 0;
}

qint64 ChatTimeline::lastReadId(const QString &dialog) const
{
    const auto *state = findMessageState(dialog);
    return state ? state->lastReadId : -1;
}

void ChatTimeline::openHistory(const QString &scope, bool task)
{
    (task ? openTask : openDialog) = scope;
    (task ? taskMessagesDirty : chatDirty) = false;
}

void ChatTimeline::invalidateHistory(bool task)
{
    (task ? taskMessagesDirty : chatDirty) = true;
}

void ChatTimeline::invalidateRecent()
{
    recentDirty = true;
}

void ChatTimeline::invalidateOpenViews(bool chatSection)
{
    if (chatSection) {
        invalidateRecent();
        chatDirty = !openDialog.isEmpty();
    }
    taskMessagesDirty = !openTask.isEmpty();
}

bool ChatTimeline::takeRecentDirty()
{
    return std::exchange(recentDirty, false);
}

bool ChatTimeline::takeHistoryDirty(bool task)
{
    return std::exchange(task ? taskMessagesDirty : chatDirty, false);
}

void ChatTimeline::resetPullMuteFlags()
{
    for (auto &state : dialogMessageStates)
        state.muteFromPull = false;
}

void ChatTimeline::rememberTaskDialog(const QString &task, const QString &dialog)
{
    if (taskDialogs.size() >= tunables::kPageSizeChats)
        taskDialogs.erase(taskDialogs.begin());
    taskDialogs.insert(task, dialog);
}

void ChatTimeline::forgetTaskDialog(const QString &task)
{
    taskDialogs.remove(task);
}

void ChatTimeline::rememberChatId(const QString &dialog, qint64 id)
{
    if (!dialogChatIds.contains(dialog) && dialogChatIds.size() >= tunables::kPageSizeChats)
        dialogChatIds.erase(dialogChatIds.begin());
    dialogChatIds.insert(dialog, id);
}

void ChatTimeline::updateRecentChat(const QString &dialog, const std::function<void(Chat &)> &update)
{
    for (auto &chat : recentChatItems) {
        if (chat.id == dialog)
            update(chat);
    }
}

void ChatTimeline::replaceRecentChats(QVector<Chat> chats, bool task)
{
    (task ? recentTaskChatItems : recentChatItems) = std::move(chats);
}

void ChatTimeline::replaceLegacyTasks(QVector<Chat> chats)
{
    legacyTaskChats = std::move(chats);
}

void ChatTimeline::selectRecentSection(bool task)
{
    showTaskChats = task;
}

quint64 ChatTimeline::nextBadgeRevision()
{
    return ++chatBadgeRevision;
}

bool ChatTimeline::hasReplyPreview(const QString &dialog, const QString &message) const
{
    return replyPreviews.contains(replyPreviewKey(dialog, message));
}

void ChatTimeline::forgetReplyPreview(const QString &dialog, const QString &message) const
{
    replyPreviews.remove(replyPreviewKey(dialog, message));
}

bool ChatTimeline::hasTaskForDialog(const QString &dialog) const
{
    return std::find(taskDialogs.cbegin(), taskDialogs.cend(), dialog) != taskDialogs.cend();
}

const ChatTimeline::ReplyPreview *ChatTimeline::replyPreview(const QString &dialog, const QString &message) const
{
    return replyPreviews.object(replyPreviewKey(dialog, message));
}

void ChatTimeline::rememberReplyPreview(const QString &dialog, const QString &message, ReplyPreview preview) const
{
    const auto cost = 1 + preview.files.size();
    replyPreviews.insert(replyPreviewKey(dialog, message), new ReplyPreview(std::move(preview)), cost);
}

void ChatTimeline::applyCounterSnapshotRevision(quint64 revision)
{
    chatCounterSnapshotRevision = revision;
    for (auto &overlay : recentOverlays) {
        if (overlay.counterRevision <= revision) {
            overlay.counter.reset();
            overlay.markedUnread.reset();
        }
    }
}

void ChatTimeline::resetAccount()
{
    replyPreviews.clear();
    dialogMessageStates.clear();
    taskDialogs.clear();
    dialogChatIds.clear();
    sentTemplates.clear();
    openHistory({}, false);
    openHistory({}, true);
    recentDirty = false;
    chatBadgeRevision = 0;
    chatCounterSnapshotRevision = 0;
    recentOverlays.clear();
    recentChatItems.clear();
    recentTaskChatItems.clear();
    legacyTaskChats.clear();
    // The selected recent section is a view preference shared across account switches.
}

} // namespace bx
