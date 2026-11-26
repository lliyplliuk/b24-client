#include "ui/chat_panel.h"
#include "ui/chat_widgets.h"
#include "ui/history_surface.h"
#include <QPushButton>

namespace bx {

void ChatPanel::createComposer(QWidget *reading, ui::HistoryList *history)
{
    m_chatComposer = new ui::ChatComposer(reading, QStringLiteral("Текст сообщения"),
        QStringLiteral("Эмодзи в чате"), QStringLiteral("Прикрепить файл в чат"), QStringLiteral("Отправить сообщение"));
    m_chatInput = m_chatComposer->input();
    m_chatComposer->setEditLatest([this, history] { return editLatestMessage(history, m_currentChatId); });
    m_chatComposer->setTypingActions([this] {
        return writable() && m_active && isActiveWindow() ? m_currentChatId : QString();
    }, [this](const QString &dialog) { m_backend->notifyChatTyping(dialog); });
    history->setComposer(m_chatInput);
    history->setReplyActions([this](const ui::ChatReply &reply) {
        m_chatComposer->setReply(reply);
        m_chatInput->setFocus(Qt::OtherFocusReason);
    }, [this] { return writable() && !m_currentChatId.isEmpty(); },
       [this](const QString &id) { openChatMessageContext(m_currentChatId, id); });
    history->setMessageMenu([this](QMenu &menu, const QPersistentModelIndex &index, const QString &href) {
        addMessageActions(menu, index, m_currentChatId, m_chatScope == 1, href);
    });
    history->setReactionActions([this](const QModelIndex &index, const QString &code, const QPoint &anchor) {
        actOnMessageReaction(index, m_currentChatId, code, anchor, window());
    }, [this] { return writable() && !m_backend->isDemo() && !m_currentChatId.isEmpty(); });
    m_chatEmoji = m_chatComposer->emoji();
    m_chatFile = m_chatComposer->file();
    m_chatSend = m_chatComposer->send();
    if (m_readOnly)
        m_chatComposer->setPlaceholderText(QStringLiteral("Режим чтения"));
    connect(m_chatFile, &QPushButton::clicked, this, [this] { m_workspace.pickFile(m_currentChatId, QStringLiteral("chat")); });
    connect(m_chatSend, &QPushButton::clicked, this, &ChatPanel::onSendChat);
    connect(m_chatInput, &QPlainTextEdit::textChanged, this, &ChatPanel::updateComposers);
    m_chatComposer->setEditStateChanged([this] { updateComposers(); });
    m_chatComposer->setImageDraftHandlers([this] { updateComposers(); },
                                          [this](const QString &error) { emit statusChanged(error); });
    ui::HistorySurface::bindMentions(m_chatComposer, m_backend, [this] { return m_currentChatId; },
                                     m_workspace.avatar, [this] { emit avatarsRequested(); });
    m_chatInput->installEventFilter(this);
}

void ChatPanel::focusChatComposer()
{
    if (!m_currentChatId.isEmpty() && m_chatInput->isVisible() && !m_chatInput->isReadOnly())
        m_chatInput->setFocus(Qt::OtherFocusReason);
}

void ChatPanel::onSendChat()
{
    if (m_chatComposer->submitMessageEdit())
        return;
    m_workspace.sendFromComposer(m_chatSendController, m_chatComposer, m_currentChatId);
}

void ChatPanel::updateComposers()
{
    const auto wire = m_chatComposer->wireText();
    const auto text = wire.trimmed();
    const bool enabled = writable() && !m_currentChatId.isEmpty();
    const bool editing = m_chatComposer->editingMessage();
    const bool saving = m_chatComposer->messageEditPending();
    const bool hasImage = m_chatComposer->hasDraftImage();
    const bool sending = m_chatSendController.pending() || m_workspace.pastedImagePending(m_currentChatId, false);
    m_chatInput->setReadOnly(m_readOnly || m_currentChatId.isEmpty() || saving);
    const bool canSubmit = editing ? m_chatComposer->canSaveMessageEdit(wire) : !sending
        && text.size() <= ui::kMessageCharacters && (hasImage ? !m_workspace.uploadPending() : !text.isEmpty());
    m_chatSend->setEnabled(enabled && canSubmit);
    m_chatSend->setToolTip(editing ? (saving ? QStringLiteral("Сохраняем сообщение…") : QStringLiteral("Сохранить сообщение"))
        : text.size() > ui::kMessageCharacters ? QStringLiteral("Максимум 20 000 символов")
        : sending || (hasImage && m_workspace.uploadPending()) ? QStringLiteral("Отправка…")
        : QStringLiteral("Отправить сообщение"));
    m_chatEmoji->setEnabled(enabled && !saving);
    m_chatFile->setEnabled(enabled && !editing && !m_workspace.uploadPending());
}

void ChatPanel::fileUploaded(const QString &scope, const QString &status, const ui::ChatDraft &draft)
{
    if (draft.image)
        clearSentDraft(m_chatComposer, m_chatDrafts, m_currentChatId, scope, draft);
    emit statusChanged(status);
    if (m_currentChatId == scope && !m_messageSearchActive)
        m_backend->requestChatMessages(scope);
}

} // namespace bx
