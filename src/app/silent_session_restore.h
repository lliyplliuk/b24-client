#pragma once

#include "signin/session_restore.h"

#include <QDialog>
#include <QEventLoop>

#include <functional>

namespace bx {

enum class RestoreFailureAction { OfferRetry, OpenBrowser, OfferBrowser };

inline RestoreFailureAction restoreFailureAction(signin::RestoreStatus status, bool openOnFallback)
{
    if (openOnFallback)
        return RestoreFailureAction::OpenBrowser;
    return status == signin::RestoreStatus::Unavailable
        ? RestoreFailureAction::OfferRetry : RestoreFailureAction::OfferBrowser;
}

// Keep the modal sign-in fallback and the saved-session wait in one path.
class SilentSessionRestore final {
public:
    void run(QDialog &dialog, const std::function<void()> &restore)
    {
        restore();
        if (!m_finished)
            m_wait.exec();
        if (m_finished && dialog.result() != QDialog::Accepted)
            dialog.exec();
    }

    void finish()
    {
        m_finished = true;
        m_wait.quit();
    }

private:
    QEventLoop m_wait;
    bool m_finished = false;
};

} // namespace bx
