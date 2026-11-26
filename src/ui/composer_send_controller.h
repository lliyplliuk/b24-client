#pragma once

#include "ui/chat_reply.h"

#include <QHash>
#include <functional>

namespace bx {
class Backend;

namespace ui {

class ChatComposer;

class ComposerSendController {
public:
    enum class Surface { Chat, Task };
    using Status = std::function<void(const QString &id, const QString &text, bool loading)>;
    using UploadImage = std::function<void(const QString &caption)>;

    ComposerSendController(Surface surface, Backend *backend, Status status);

    bool isTask() const { return m_surface == Surface::Task; }
    bool pending() const { return !m_pendingId.isEmpty(); }
    void reset();
    void send(ChatComposer &composer, QString id, bool writable, bool imagePending,
              const UploadImage &uploadImage);
    void confirm(const QString &scope, ChatComposer *composer, const QString &currentId,
                 QHash<QString, ChatDraft> &drafts);
    bool fail(const QString &id, const QString &reason, bool networkFailure);

private:
    Surface m_surface;
    Backend *m_backend;
    Status m_status;
    QString m_pendingId;
    ChatDraft m_pendingDraft;
};

void clearSentDraft(ChatComposer *composer, QHash<QString, ChatDraft> &drafts,
                    const QString &current, const QString &pending, const ChatDraft &sent);

} // namespace ui
} // namespace bx
