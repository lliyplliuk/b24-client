#pragma once

#include <QTextDocument>

namespace bx::ui::task_description {

class SafeDocument final : public QTextDocument {
public:
    using QTextDocument::QTextDocument;

protected:
    QVariant loadResource(int, const QUrl &) override;
};

} // namespace bx::ui::task_description
