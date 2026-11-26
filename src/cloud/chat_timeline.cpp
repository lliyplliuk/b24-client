#include "chat_timeline.h"
#include "core/tunables.h"

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

} // namespace bx
