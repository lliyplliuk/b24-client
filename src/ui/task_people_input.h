#pragma once

#include "core/models.h"
#include "ui/requisite_avatar.h"

#include <QWidget>

class QLineEdit;

namespace bx::ui {
class TaskSelectionField;
class PeopleSuggestions;

class TaskPeopleInput final : public QWidget {
public:
    TaskPeopleInput(const QString &caption, bool multiple, QWidget *parent);
    void setPeople(const QVector<TaskPerson> &people);
    void setConfirmedPeople(const QVector<TaskPerson> &people, bool followPortal = false);
    const QVector<TaskPerson> &people() const;
    QLineEdit *input() const;
    void dismiss();
    void setSearchHandler(const std::function<void(const QString &, const QString &)> &handler);
    void setResults(const QString &id, const QVector<Person> &people);
    bool showRequestError(const QString &id);
    void showInitialSuggestions();
    void setAvatarSource(const RequisiteAvatarSource &source, std::function<void()> visibleChanged);
    QList<QUrl> visibleAvatarUrls() const;
    void refreshAvatars();
    void fitSuggestions();
    std::function<void()> onChange;
    std::function<void()> onLayoutChanged;
    std::function<void()> onExplicitSelection;

private:
    void fitSelectionToPopup();
    void notifyLayoutChanged();
    static bool containsPerson(const QVector<TaskPerson> &people, const QString &id);
    void renderPills();
    void renderSelection();

    const bool m_multiple;
    TaskSelectionField *m_selection = nullptr;
    QLineEdit *m_input = nullptr;
    PeopleSuggestions *m_suggestions = nullptr;
    QVector<TaskPerson> m_people;
    QVector<TaskPerson> m_confirmedPeople;
    RequisiteAvatarSource m_source;
};

} // namespace bx::ui
