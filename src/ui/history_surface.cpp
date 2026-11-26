#include "ui/history_surface.h"

#include "core/backend.h"
#include "ui/chat_composer.h"
#include "ui/history_list.h"

#include <utility>

namespace bx::ui {

HistorySurface::HistorySurface(Backend *backend, bool task, std::function<View()> current, QObject *parent)
    : QObject(parent), m_current(std::move(current))
{
    connect(backend, &Backend::pinnedMessagesLoaded, this,
            [this](const QString &dialog, const QVector<Message> &messages) {
        if (!dialog.isEmpty() && m_current().dialogId == dialog)
            emit pinsChanged(messages);
    });
    connect(backend, &Backend::olderChatMessagesLoaded, this,
            [this, task](const QString &scope, const QString &before, bool taskPage,
                         const QVector<Message> &messages, bool more) {
        const auto view = m_current();
        if (taskPage == task && view.history && view.active && !view.searching && scope == view.pageScope
            && view.history->addOlder(scope, before, messages, more))
            emit olderPageAdded();
    });
    connect(backend, &Backend::chatReadConfirmed, this,
            [this](const QString &dialog, const QStringList &ids, qint64 through) {
        const auto view = m_current();
        if (view.history && !dialog.isEmpty() && dialog == view.dialogId && !view.searching && !view.inContext)
            view.history->confirmRead(ids, through);
    });
    connect(backend, &Backend::chatHistoryRemoved, this,
            [this](const QString &dialog, const QStringList &ids, qint64 through) {
        const auto view = m_current();
        if (!view.history || dialog.isEmpty() || dialog != view.dialogId)
            return;
        view.history->removeHistory(ids, through);
        if (ids.isEmpty() && through == 0 && !m_current().searching)
            emit historyCleared();
    });
}

void HistorySurface::bindMentions(ChatComposer *composer, Backend *backend, std::function<QString()> dialogId,
                                  RequisiteAvatarSource avatar, std::function<void()> requestAvatars)
{
    composer->setMentionSearchHandler([composer, backend, dialogId = std::move(dialogId)](const QString &id, const QString &query) {
        const auto scope = dialogId();
        if (scope.isEmpty())
            composer->setMentionResults(id, {});
        else
            backend->searchPeople(id, query, scope);
    });
    composer->setMentionAvatars([avatar = std::move(avatar)](const Person &person) {
        return requisiteAvatarIcon(avatar, person.name, person.avatarUrl, workspace::AvatarKind::Person);
    }, std::move(requestAvatars));
}

} // namespace bx::ui
