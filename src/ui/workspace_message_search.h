#pragma once

#include "core/backend.h"

namespace bx {

// Each history surface keeps one outstanding search per dialog or task.
class WorkspaceMessageSearch {
public:
    WorkspaceMessageSearch(Backend *backend, bool task) : m_backend(backend), m_task(task) {}

    void request(const QString &scope, const QString &query)
    {
        if (m_pending.contains(scope))
            return;
        m_pending.insert(scope, query);
        m_backend->searchMessages(scope, query, m_task);
    }

    bool accept(const QString &scope, const QString &query)
    {
        if (m_pending.value(scope) != query)
            return false;
        m_pending.remove(scope);
        return true;
    }

    QString takeFailed(const QString &scope) { return m_pending.take(scope); }
    void reset() { m_pending.clear(); }

private:
    Backend *m_backend;
    bool m_task;
    QHash<QString, QString> m_pending;
};

} // namespace bx
