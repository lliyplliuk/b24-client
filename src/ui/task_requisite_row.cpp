#include "ui/task_requisite_row.h"

#include "ui/avatar_badge.h"
#include "ui/requisite_avatar.h"

#include <QHBoxLayout>
#include <QVBoxLayout>

namespace bx::ui {

bool TaskRequisiteRow::Entry::operator==(const Entry &other) const
{
    return name == other.name && url == other.url && projectColor == other.projectColor
        && projectVariant == other.projectVariant && projectType == other.projectType;
}

TaskRequisiteRow::TaskRequisiteRow(const QString &caption, QWidget *parent, std::optional<workspace::AvatarKind> kind,
                                   bool multiple)
    : QPushButton(parent), m_caption(caption), m_kind(kind), m_multiple(multiple)
{
    setObjectName(QStringLiteral("taskRequisiteRow"));
    setAccessibleName(caption);
    setAutoDefault(false);
    auto policy = QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    auto *line = new QHBoxLayout(this);
    line->setContentsMargins(8, 4, 8, 4);
    line->setSpacing(9);
    auto *label = workspace::label(caption, "fieldCaption", this);
    label->setFixedWidth(100);
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    line->addWidget(label, 0, Qt::AlignTop);
    auto *content = new QWidget(this);
    content->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_values = new QVBoxLayout(content);
    m_values->setContentsMargins(0, 0, 0, 0);
    m_values->setSpacing(4);
    line->addWidget(content, 1);
}

QSize TaskRequisiteRow::sizeHint() const
{
    return layout()->sizeHint();
}

QSize TaskRequisiteRow::minimumSizeHint() const
{
    return layout()->minimumSize();
}

int TaskRequisiteRow::heightForWidth(int width) const
{
    return layout()->totalHeightForWidth(width);
}

void TaskRequisiteRow::setEditable(bool editable)
{
    setEnabled(editable);
    setCursor(editable ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void TaskRequisiteRow::setEntries(QVector<Entry> entries)
{
    if (m_values->count() && m_entries == entries)
        return;
    m_entries = std::move(entries);
    while (auto *item = m_values->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_avatars.clear();
    auto display = m_entries;
    if (display.isEmpty())
        display.append({QStringLiteral("—"), {}});
    QStringList descriptions;
    for (const auto &entry : display) {
        const auto &name = entry.name;
        auto *line = new QWidget(m_values->parentWidget());
        auto *parts = new QHBoxLayout(line);
        parts->setContentsMargins(0, 0, 0, 0);
        parts->setSpacing(5);
        const auto color = QColor(entry.projectColor);
        const bool mark = m_kind != workspace::AvatarKind::Project
            || hasRequisiteProjectMark(entry.url, color, entry.projectVariant, entry.projectType);
        if (m_kind && !m_entries.isEmpty() && (mark || !entry.url.isEmpty())) {
            auto *avatar = new AvatarBadge(line);
            avatar->setObjectName(QStringLiteral("taskRequisiteAvatar"));
            avatar->setFixedSize(21, 21);
            avatar->setTitle(name, *m_kind, color, entry.projectVariant, entry.projectType);
            avatar->setVisible(mark);
            parts->addWidget(avatar, 0, Qt::AlignTop);
            m_avatars.append({avatar, entry.url, mark});
        }
        auto *value = workspace::label(name, "taskRequisiteValue", line);
        auto policy = value->sizePolicy();
        policy.setHorizontalPolicy(QSizePolicy::Ignored);
        value->setSizePolicy(policy);
        value->setAccessibleName(m_multiple ? name : m_caption);
        parts->addWidget(value, 1);
        m_values->addWidget(line);
        descriptions.append(name);
    }
    setAccessibleDescription(descriptions.join(QStringLiteral(", ")));
    updateGeometry();
}

QList<QUrl> TaskRequisiteRow::visibleAvatarUrls(const QWidget *viewport) const
{
    QList<QUrl> urls;
    for (const auto &[avatar, url, mark] : m_avatars) {
        const auto *target = mark ? avatar : avatar->parentWidget();
        if (!url.isEmpty() && target->isVisibleTo(viewport)
            && viewport->rect().intersects(QRect(target->mapTo(viewport, QPoint()), target->size())))
            urls.append(url);
    }
    return urls;
}

void TaskRequisiteRow::refreshAvatars(const AvatarSource &source)
{
    for (const auto &[avatar, url, mark] : m_avatars) {
        const auto *photo = source && !url.isEmpty() ? source(url) : nullptr;
        avatar->setPhoto(photo ? *photo : QPixmap());
        avatar->setVisible(mark || (photo && !photo->isNull()));
    }
}

} // namespace bx::ui
