#include "chat_timeline.h"
#include "core/tunables.h"

#include <algorithm>

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

void ChatTimeline::forgetReplyPreview(const QString &dialog, const QString &message)
{
    replyPreviews.remove(replyPreviewKey(dialog, message));
}

bool ChatTimeline::hasTaskForDialog(const QString &dialog) const
{
    return std::find(taskDialogs.cbegin(), taskDialogs.cend(), dialog) != taskDialogs.cend();
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
