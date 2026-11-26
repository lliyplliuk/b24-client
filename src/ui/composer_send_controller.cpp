#include "ui/composer_send_controller.h"

#include "core/backend.h"
#include "ui/chat_widgets.h"

#include <utility>

namespace bx::ui {

ComposerSendController::ComposerSendController(Surface surface, Backend *backend, Status status)
    : m_surface(surface), m_backend(backend), m_status(std::move(status))
{
}

void ComposerSendController::reset()
{
    m_pendingId.clear();
    m_pendingDraft = {};
}

void ComposerSendController::send(ChatComposer &composer, QString id, bool writable, bool imagePending,
                                  const UploadImage &uploadImage)
{
    if (!writable || pending() || id.isEmpty() || imagePending)
        return;
    const auto text = composer.wireText().trimmed();
    if (text.size() > kMessageCharacters || (text.isEmpty() && !composer.hasDraftImage()))
        return;

    composer.cancelTyping();
    if (composer.hasDraftImage()) {
        uploadImage(text);
        return;
    }

    m_pendingId = id;
    m_pendingDraft = composer.draft();
    const auto replyId = m_pendingDraft.reply.id;
    m_status(id, QStringLiteral("Отправляем сообщение…"), true);
    if (replyId.isEmpty()) {
        if (isTask())
            m_backend->sendTaskMessage(id, text);
        else
            m_backend->sendChatMessage(id, text);
    } else if (isTask()) {
        m_backend->sendTaskReply(id, text, replyId);
    } else {
        m_backend->sendChatReply(id, text, replyId);
    }
}

void ComposerSendController::confirm(const QString &scope, ChatComposer *composer, const QString &currentId,
                                     QHash<QString, ChatDraft> &drafts)
{
    if (!pending() || scope != operationKey(isTask() ? OperationKind::Task : OperationKind::Chat, m_pendingId))
        return;
    const auto id = m_pendingId;
    clearSentDraft(composer, drafts, currentId, id, m_pendingDraft);
    reset();
    m_status(id, {}, false);
}

bool ComposerSendController::fail(const QString &id, const QString &reason, bool networkFailure)
{
    if (!pending() || id != m_pendingId)
        return false;
    reset();
    m_status(id, networkFailure
        ? QStringLiteral("Не удалось подтвердить отправку. Проверьте историю перед повторной отправкой")
        : reason, false);
    return true;
}

void clearSentDraft(ChatComposer *composer, QHash<QString, ChatDraft> &drafts,
                    const QString &current, const QString &pending, const ChatDraft &sent)
{
    auto sentWithoutImage = sent;
    sentWithoutImage.image.reset();
    if (composer && current == pending) {
        const auto currentDraft = composer->draft();
        if (currentDraft == sent || (sent.image && currentDraft == sentWithoutImage))
            composer->setDraft({});
        else if (sent.image && currentDraft.image == sent.image)
            composer->clearDraftImage();
    }
    const auto saved = drafts.find(pending);
    if (saved == drafts.end())
        return;
    if (*saved == sent || (sent.image && *saved == sentWithoutImage)) {
        drafts.erase(saved);
    } else if (sent.image && saved->image == sent.image) {
        saved->image.reset();
        if (saved->isEmpty())
            drafts.erase(saved);
    }
}

} // namespace bx::ui
