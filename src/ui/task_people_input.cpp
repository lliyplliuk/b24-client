#include "ui/task_people_input.h"

#include "ui/people_suggestions.h"
#include "ui/task_selection_field.h"
#include "ui/workspace_style.h"

#include <QLineEdit>
#include <QVBoxLayout>

#include <algorithm>

namespace bx::ui {

TaskPeopleInput::TaskPeopleInput(const QString &caption, bool multiple, QWidget *parent)
    : QWidget(parent), m_multiple(multiple)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    m_input = new QLineEdit(this);
    m_input->setAccessibleName(QStringLiteral("Поиск сотрудника · %1").arg(caption));
    m_input->setPlaceholderText(QStringLiteral("Имя сотрудника"));
    m_input->setClearButtonEnabled(true);
    m_selection = new TaskSelectionField(m_input, this, m_multiple);
    layout->addWidget(m_selection);
    if (m_multiple) {
        m_selection->onRemove = [this](const QString &id) {
            m_people.erase(std::remove_if(m_people.begin(), m_people.end(),
                [&id](const TaskPerson &person) { return person.id == id; }), m_people.end());
            renderSelection();
            m_input->setFocus();
        };
    }

    m_suggestions = new PeopleSuggestions(m_input, this);
    layout->addWidget(m_suggestions);
    m_suggestions->attachToInput(m_input);
    m_selection->onHeightChanged = [this] { notifyLayoutChanged(); };
    m_suggestions->onEmbeddedLayoutChanged = [this] {
        fitSelectionToPopup();
        notifyLayoutChanged();
    };
    fitSelectionToPopup();
    m_suggestions->setIsChosen([this](const Person &person) {
        return containsPerson(m_confirmedPeople, person.id);
    });
    m_suggestions->setIsDraft([this](const Person &person) {
        return containsPerson(m_people, person.id) && !containsPerson(m_confirmedPeople, person.id);
    });
    setAvatarSource({}, {});
    m_suggestions->setOnChosen([this](const Person &person) {
        if (!isEnabled() || person.id.isEmpty())
            return;
        if (onExplicitSelection && containsPerson(m_people, person.id)) {
            m_input->clear();
            m_suggestions->search({});
            return;
        }
        if (m_multiple) {
            if (std::none_of(m_people.cbegin(), m_people.cend(), [&person](const TaskPerson &value) {
                return value.id == person.id;
            }))
                m_people.append({person.id, person.name, person.avatarUrl});
        } else {
            m_people = {{person.id, person.name, person.avatarUrl}};
        }
        m_input->clear();
        renderSelection();
        if (onExplicitSelection)
            onExplicitSelection();
        else
            m_suggestions->search({});
    });
}

void TaskPeopleInput::setPeople(const QVector<TaskPerson> &people)
{
    m_people = people;
    setConfirmedPeople(people);
    m_input->clear();
    m_suggestions->resetInitialSuggestions();
    renderSelection();
}

void TaskPeopleInput::setConfirmedPeople(const QVector<TaskPerson> &people, bool followPortal)
{
    m_confirmedPeople = people;
    if (followPortal) {
        m_people = people;
    } else {
        for (auto &selection : m_people) {
            const auto current = std::find_if(people.cbegin(), people.cend(), [&selection](const TaskPerson &person) {
                return selection.id == person.id;
            });
            if (current != people.cend())
                selection = *current;
        }
    }
    m_suggestions->refreshChecks();
    renderPills();
}

const QVector<TaskPerson> &TaskPeopleInput::people() const
{
    return m_people;
}

QLineEdit *TaskPeopleInput::input() const
{
    return m_input;
}

void TaskPeopleInput::dismiss()
{
    m_suggestions->dismiss();
}

void TaskPeopleInput::setSearchHandler(const std::function<void(const QString &, const QString &)> &handler)
{
    m_suggestions->setSearchHandler(handler);
}

void TaskPeopleInput::setResults(const QString &id, const QVector<Person> &people)
{
    m_suggestions->setResults(id, people);
}

bool TaskPeopleInput::showRequestError(const QString &id)
{
    return m_suggestions->showRequestError(id);
}

void TaskPeopleInput::showInitialSuggestions()
{
    m_suggestions->search({});
}

void TaskPeopleInput::setAvatarSource(const RequisiteAvatarSource &source, std::function<void()> visibleChanged)
{
    m_source = source;
    m_suggestions->setAvatarIcon([this](const Person &person) {
        return requisiteAvatarIcon(m_source, person.name, person.avatarUrl, workspace::AvatarKind::Person);
    });
    m_selection->iconFor = [this](const TaskSelectionField::Entry &entry) {
        return requisiteAvatarIcon(m_source, entry.name, entry.avatar, workspace::AvatarKind::Person);
    };
    m_suggestions->onVisibleChanged = visibleChanged;
    m_selection->onVisibleChanged = std::move(visibleChanged);
    m_selection->refreshIcons();
}

QList<QUrl> TaskPeopleInput::visibleAvatarUrls() const
{
    auto urls = m_selection->visibleAvatarUrls();
    urls.append(m_suggestions->visibleAvatarUrls());
    return urls;
}

void TaskPeopleInput::refreshAvatars()
{
    m_selection->refreshIcons();
    m_suggestions->refreshAvatars();
}

void TaskPeopleInput::fitSuggestions()
{
    m_suggestions->fitEmbeddedResults();
    fitSelectionToPopup();
}

void TaskPeopleInput::fitSelectionToPopup()
{
    if (m_multiple) {
        constexpr int popupChrome = 100;
        const int results = m_suggestions->isHidden() ? 0 : m_suggestions->height() + layout()->spacing();
        m_selection->setHeightBudget(qMax(112, window()->height() - results - popupChrome));
    }
}

void TaskPeopleInput::notifyLayoutChanged()
{
    layout()->invalidate();
    updateGeometry();
    if (onLayoutChanged)
        onLayoutChanged();
}

bool TaskPeopleInput::containsPerson(const QVector<TaskPerson> &people, const QString &id)
{
    return std::any_of(people.cbegin(), people.cend(), [&id](const TaskPerson &person) { return person.id == id; });
}

void TaskPeopleInput::renderPills()
{
    QVector<TaskSelectionField::Entry> entries;
    for (const auto &person : m_people) {
        if (person.id.isEmpty() && person.name.isEmpty())
            continue;
        const auto name = person.name.isEmpty() ? QStringLiteral("Сотрудник №%1").arg(person.id) : person.name;
        entries.append({person.id, name, person.avatarUrl, !containsPerson(m_confirmedPeople, person.id)});
    }
    m_selection->setSelection(entries);
}

void TaskPeopleInput::renderSelection()
{
    renderPills();
    m_suggestions->refreshChecks();
    if (onChange)
        onChange();
}

} // namespace bx::ui
