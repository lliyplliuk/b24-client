#pragma once

#include "core/models.h"
#include <QFrame>
#include <QIcon>
#include <QPointer>
#include <QLineEdit>
#include <QListWidget>
#include <QLabel>
#include <QTimer>
#include <functional>
#include <optional>
#include "ui/mention_suggestion_delegate.h"

namespace bx::ui {

class PeopleSuggestions final : public QFrame {
public:
    explicit PeopleSuggestions(QWidget *anchor, QWidget *embeddedParent = nullptr);
    void setSearchHandler(std::function<void(const QString &, const QString &)> handler);
    void setOnChosen(std::function<void(const Person &)> handler);
    void setIsChosen(std::function<bool(const Person &)> isChosen);
    void setIsDraft(std::function<bool(const Person &)> isDraft);
    void setAvatarIcon(std::function<QIcon(const Person &)> icon);
    void refreshAvatars();
    QList<QUrl> visibleAvatarUrls() const;
    std::function<void()> onVisibleChanged;
    std::function<void()> onEmbeddedLayoutChanged;
    void attachToInput(QLineEdit *input);
    void search(const QString &query);
    void setResults(const QString &requestId, const QVector<Person> &people);
    bool showRequestError(const QString &requestId);
    void resetInitialSuggestions();
    void fitEmbeddedResults();
    void refreshChecks();

private:
    void showPeople(const QVector<Person> &people);

public:
    bool handleKey(QKeyEvent *event);
    void dismiss();

protected:
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    void choose();
    void showState(const QString &text, bool loading = false);
    int embeddedResultHeight() const;
    void open(int height);
    QPointer<QWidget> m_anchor;
    QPointer<QLineEdit> m_field;
    QListWidget *m_list = nullptr;
    QLabel *m_state = nullptr;
    QTimer m_timer;
    QString m_request;
    QString m_query;
    QVector<Person> m_people;
    std::optional<QVector<Person>> m_initialPeople;
    bool m_embedded = false;
    std::function<QIcon(const Person &)> m_icon;
    std::function<bool(const Person &)> m_isChosen;
    std::function<bool(const Person &)> m_isDraft;
    std::function<void(const QString &, const QString &)> m_search;
    std::function<void(const Person &)> m_chosen;
};

} // namespace bx::ui
