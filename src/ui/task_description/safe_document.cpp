#include "ui/task_description/safe_document.h"

#include <QVariant>

namespace bx::ui::task_description {

QVariant SafeDocument::loadResource(int, const QUrl &)
{
    return {};
}

} // namespace bx::ui::task_description
