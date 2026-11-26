#pragma once

#include <functional>

class QListWidget;
class QPixmap;
class QPoint;
class QString;
class QUrl;
class QWidget;

namespace bx {
class Backend;

namespace workspace {

struct ViewerAvatars {
    std::function<const QPixmap *(const QUrl &)> cached;
    std::function<void(const QUrl &)> request;
};

QWidget *openMessageViewers(Backend *backend, QListWidget *history, const QString &messageId,
                            const QPoint &anchor, const QString &scope, bool task,
                            std::function<bool()> sameHistory, ViewerAvatars avatars);

} // namespace workspace
} // namespace bx
