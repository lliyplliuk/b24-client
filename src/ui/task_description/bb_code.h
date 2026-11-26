#pragma once

#include <QString>
#include <QSize>

namespace bx::ui::task_description {

using namespace Qt::StringLiterals;

inline const QString ClipboardType = u"application/x-bitrix24-task-description"_s;
inline const QString TokenScheme = u"bx-task-description-token"_s;

QString htmlText(QString text);
bool isDiskId(const QString &id);
QString diskReference(const QString &id, const QSize &size);
QString literalText(QString text);

} // namespace bx::ui::task_description
