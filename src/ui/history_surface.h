#pragma once

#include "core/models.h"
#include "ui/requisite_avatar.h"

#include <QObject>
#include <functional>

namespace bx {
class Backend;

namespace ui {
class ChatComposer;
class HistoryList;

// Resolve the current widgets and identifiers at delivery time. A task card
// can close or be replaced while a page of messages is in flight.
class HistorySurface final : public QObject {
    Q_OBJECT
public:
    struct View {
        HistoryList *history = nullptr;
        QString dialogId;
        QString pageScope;
        bool active = false;
        bool searching = false;
        bool inContext = false;
    };

    HistorySurface(Backend *backend, bool task, std::function<View()> current, QObject *parent);

    static void bindMentions(ChatComposer *composer, Backend *backend, std::function<QString()> dialogId,
                             RequisiteAvatarSource avatar, std::function<void()> requestAvatars);

signals:
    void pinsChanged(const QVector<Message> &messages);
    void olderPageAdded();
    void historyCleared();

private:
    std::function<View()> m_current;
};
} // namespace ui
} // namespace bx
