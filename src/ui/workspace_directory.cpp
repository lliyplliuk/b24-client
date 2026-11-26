#include "ui/workspace_directory.h"
#include "ui/workspace_style.h"

#include <QCryptographicHash>
#include <QVBoxLayout>

namespace bx {

QSettings chatPreferences()
{
    return QSettings(QSettings::defaultFormat(), QSettings::UserScope, QStringLiteral("bitrix24-client"),
                     QStringLiteral("bitrix24-client"));
}

DirectorySplitter::DirectorySplitter(const QString &key, int regularWidth, int compactWidth, QWidget *parent)
    : QSplitter(Qt::Horizontal, parent), m_key(key), m_regularWidth(regularWidth),
      m_compactWidth(compactWidth)
{
    setHandleWidth(5);
    setChildrenCollapsible(false);
    setOpaqueResize(false);
    QSettings settings(QStringLiteral("bitrix24-client"), QStringLiteral("bitrix24-client"));
    m_preferredWidth = qMax(0, settings.value(m_key).toInt());
    connect(this, &QSplitter::splitterMoved, this, [this](int position, int) {
        m_preferredWidth = position - contentsRect().left();
        QSettings settings(QStringLiteral("bitrix24-client"), QStringLiteral("bitrix24-client"));
        settings.setValue(m_key, m_preferredWidth);
    });
}

void DirectorySplitter::resizeEvent(QResizeEvent *event)
{
    QSplitter::resizeEvent(event);
    restoreWidth();
}

void DirectorySplitter::showEvent(QShowEvent *event)
{
    QSplitter::showEvent(event);
    restoreWidth();
}

void DirectorySplitter::restoreWidth()
{
    if (!isVisible() || count() != 2)
        return;
    const int available = contentsRect().width() - handle(1)->width();
    const int minimum = widget(0)->minimumWidth();
    const int maximum = qMax(minimum, available - widget(1)->minimumWidth());
    const int preferred = m_preferredWidth > 0 ? m_preferredWidth
        : (window()->width() < 855 ? m_compactWidth : m_regularWidth);
    const int left = qBound(minimum, preferred, maximum);
    setSizes({left, qMax(0, available - left)});
}

QWidget *directory(const QString &title, QListWidget **list, QWidget *parent, QLabel **titleLabel)
{
    auto *side = new QWidget(parent);
    side->setObjectName(QStringLiteral("directory"));
    auto *layout = new QVBoxLayout(side);
    layout->setContentsMargins(10, 20, 9, 10);
    layout->setSpacing(10);
    auto *heading = workspace::label(title, "sectionTitle", side);
    if (titleLabel)
        *titleLabel = heading;
    heading->setContentsMargins(7, 0, 0, 0);
    layout->addWidget(heading);
    *list = new QListWidget(side);
    (*list)->setAccessibleName(title);
    workspace::configureRows(*list, workspace::Row::Directory);
    layout->addWidget(*list, 1);
    return side;
}

void selectAvailable(QListWidget *list, QListWidgetItem *selection)
{
    if (!selection && list->count() > 0 && !list->item(0)->data(workspace::Id).toString().isEmpty())
        selection = list->item(0);
    list->setCurrentItem(selection);
}

QString chatPreferenceKey(const QString &accountScope, QLatin1String preference)
{
    if (accountScope.isEmpty())
        return {};
    const auto key = QCryptographicHash::hash(accountScope.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QStringLiteral("ui/accounts/") + QString::fromLatin1(key) + QLatin1Char('/') + preference;
}

} // namespace bx
