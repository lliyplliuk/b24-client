#include "ui/people_suggestions.h"

#include "ui/loading_indicator.h"
#include "ui/mention_suggestion_delegate.h"
#include "ui/task_picker_choice_delegate.h"
#include <QKeyEvent>
#include <QScrollBar>
#include <QUuid>
#include <QVBoxLayout>

namespace bx::ui {

PeopleSuggestions::PeopleSuggestions(QWidget *anchor, QWidget *embeddedParent)
    : QFrame(embeddedParent ? embeddedParent : anchor->window()),
      m_anchor(anchor), m_embedded(embeddedParent != nullptr)
{
    setObjectName(QStringLiteral("peopleSuggestions"));
    setStyleSheet(QStringLiteral(
        "#peopleSuggestions {background:#fffefa;border:1px solid #c7dcd4;border-radius:8px;}"
        "QListWidget {background:#fffefa;border:0;color:#182d3b;outline:none;}"
        "QListWidget::item {padding:6px 8px;} QListWidget::item:selected {background:#dcecf0;color:#182d3b;}"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(5, 5, 5, 5);
    m_list = new QListWidget(this);
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setAccessibleName(tr("Найденные сотрудники"));
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setTextElideMode(Qt::ElideRight);
    if (m_embedded) {
        m_list->setItemDelegate(new TaskPickerChoiceDelegate(m_list));
    } else {
        m_list->setItemDelegate(new MentionSuggestionDelegate(m_list));
        m_list->setMouseTracking(true);
    }
    layout->addWidget(m_list);
    connect(m_list->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (onVisibleChanged)
            onVisibleChanged();
    });
    m_state = new QLabel(this);
    m_state->setTextFormat(Qt::PlainText);
    m_state->setContentsMargins(8, 5, 8, 5);
    layout->addWidget(m_state);
    connect(m_list, &QListWidget::itemClicked, this, [this] { choose(); });
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        if (m_search && !m_request.isEmpty()) {
            const auto request = m_request;
            const auto query = m_query;
            m_search(request, query);
        }
    });
    anchor->installEventFilter(this);
    connect(anchor, &QObject::destroyed, this, &QObject::deleteLater);
    hide();
}

void PeopleSuggestions::setSearchHandler(std::function<void(const QString &, const QString &)> handler)
{
    resetInitialSuggestions();
    m_search = std::move(handler);
}

void PeopleSuggestions::setOnChosen(std::function<void(const Person &)> handler)
{
    m_chosen = std::move(handler);
}

void PeopleSuggestions::setIsChosen(std::function<bool(const Person &)> isChosen)
{
    m_isChosen = std::move(isChosen);
}

void PeopleSuggestions::setIsDraft(std::function<bool(const Person &)> isDraft)
{
    m_isDraft = std::move(isDraft);
}

void PeopleSuggestions::setAvatarIcon(std::function<QIcon(const Person &)> icon)
{
    m_icon = std::move(icon);
    m_list->setIconSize(QSize(22, 22));
    refreshAvatars();
}

void PeopleSuggestions::refreshAvatars()
{
    if (m_icon)
        for (int i = 0; i < m_people.size() && i < m_list->count(); ++i)
            m_list->item(i)->setIcon(m_icon(m_people[i]));
}

QList<QUrl> PeopleSuggestions::visibleAvatarUrls() const
{
    QList<QUrl> urls;
    if (!m_list->isVisible())
        return urls;
    const auto visible = m_embedded ? m_list->viewport()->rect() : m_list->viewport()->visibleRegion().boundingRect();
    for (int i = 0; i < m_people.size() && i < m_list->count(); ++i) {
        if (!m_people[i].avatarUrl.isEmpty()
            && visible.intersects(m_list->visualItemRect(m_list->item(i))))
            urls.append(m_people[i].avatarUrl);
    }
    return urls;
}

void PeopleSuggestions::attachToInput(QLineEdit *input)
{
    if (m_field)
        return;
    m_field = input;
    if (input != m_anchor)
        input->installEventFilter(this);
    connect(input, &QLineEdit::textEdited, this, [this](const QString &text) {
        if (text.trimmed().isEmpty() && !m_embedded)
            dismiss();
        else
            search(text);
    });
}

void PeopleSuggestions::search(const QString &query)
{
    const auto trimmed = query.trimmed();
    if (!m_search || !m_anchor || !m_anchor->isVisible() || (!m_request.isEmpty() && m_query == trimmed))
        return;
    m_timer.stop();
    m_query = trimmed;
    m_request = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (m_embedded && m_query.isEmpty() && m_initialPeople) {
        showPeople(*m_initialPeople);
        return;
    }
    m_people.clear();
    m_list->clear();
    const bool initial = m_embedded && m_query.isEmpty();
    showState(initial ? tr("Загружаем сотрудников…") : tr("Ищем сотрудников…"), true);
    m_timer.start(initial ? 0 : 180);
}

void PeopleSuggestions::setResults(const QString &requestId, const QVector<Person> &people)
{
    if (requestId.isEmpty() || requestId != m_request)
        return;
    showPeople(people);
    if (m_embedded && m_query.isEmpty())
        m_initialPeople = m_people;
}

bool PeopleSuggestions::showRequestError(const QString &requestId)
{
    if (requestId.isEmpty() || requestId != m_request)
        return false;
    m_request.clear();
    m_timer.stop();
    showState(m_query.isEmpty() ? tr("Не удалось загрузить список.\nВведите имя для поиска.")
                                : tr("Ошибка поиска.\nИзмените запрос."));
    return true;
}

void PeopleSuggestions::resetInitialSuggestions()
{
    m_initialPeople.reset();
    dismiss();
}

void PeopleSuggestions::fitEmbeddedResults()
{
    if (m_embedded && m_list->isVisible() && !m_people.isEmpty())
        setFixedHeight(embeddedResultHeight());
}

void PeopleSuggestions::refreshChecks()
{
    if (!m_isChosen)
        return;
    for (int i = 0; i < m_people.size() && i < m_list->count(); ++i) {
        auto *item = m_list->item(i);
        const bool confirmed = m_isChosen(m_people[i]);
        const bool draft = m_isDraft && m_isDraft(m_people[i]);
        item->setData(Qt::CheckStateRole, confirmed ? QVariant(Qt::Checked) : QVariant());
        item->setData(TaskPickerDraft, draft);
        item->setData(Qt::AccessibleDescriptionRole, confirmed ? tr("Текущее значение")
            : draft ? tr("Новый выбор") : QString());
        item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
    }
}

void PeopleSuggestions::showPeople(const QVector<Person> &people)
{
    m_people.clear();
    m_list->clear();
    for (const auto &person : people) {
        if (person.id.isEmpty() || person.name.isEmpty())
            continue;
        if (m_people.size() == 20)
            break;
        m_people.append(person);
        const auto caption = person.position.isEmpty() ? person.name
            : tr("%1 · %2").arg(person.name, person.position);
        auto *item = new QListWidgetItem(m_embedded ? caption : person.name, m_list);
        item->setData(Qt::AccessibleTextRole, caption);
        if (!m_embedded) {
            auto nameFont = m_list->font();
            nameFont.setWeight(QFont::DemiBold);
            item->setFont(nameFont);
            item->setData(MentionSuggestionDelegate::PositionRole, person.position);
        }
    }
    if (m_people.isEmpty()) {
        showState(m_embedded && m_query.isEmpty()
            ? tr("Нет предложений.\nВведите имя для поиска.") : tr("Сотрудники не найдены"));
        return;
    }
    setLoadingIndicator(m_state, false);
    m_state->hide();
    m_list->show();
    m_list->setCurrentRow(m_query.isEmpty() ? -1 : 0);
    refreshChecks();
    refreshAvatars();
    const int height = m_embedded ? embeddedResultHeight()
        : qMin<int>(m_people.size(), 6) * m_list->sizeHintForRow(0) + 12;
    open(height);
    if (onVisibleChanged)
        onVisibleChanged();
}

bool PeopleSuggestions::handleKey(QKeyEvent *event)
{
    if (!isVisible() || (event->modifiers() & (Qt::ShiftModifier | Qt::AltModifier | Qt::ControlModifier)))
        return false;
    if (event->key() == Qt::Key_Escape) {
        dismiss();
        return true;
    }
    if (!m_people.isEmpty() && (event->key() == Qt::Key_Down || event->key() == Qt::Key_Up)) {
        const int current = m_list->currentRow();
        const int direction = event->key() == Qt::Key_Down ? 1 : -1;
        m_list->setCurrentRow(current == -1 ? (direction == 1 ? 0 : m_people.size() - 1)
            : qBound(0, current + direction, int(m_people.size()) - 1));
        return true;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && m_list->currentRow() >= 0) {
        choose();
        return true;
    }
    return false;
}

void PeopleSuggestions::dismiss()
{
    m_request.clear();
    m_timer.stop();
    hide();
}

bool PeopleSuggestions::eventFilter(QObject *object, QEvent *event)
{
    if (event->type() == QEvent::Hide || (!m_embedded && event->type() == QEvent::FocusOut))
        dismiss();
    if (object == m_field && event->type() == QEvent::KeyPress && handleKey(static_cast<QKeyEvent *>(event)))
        return true;
    return QFrame::eventFilter(object, event);
}

void PeopleSuggestions::choose()
{
    const auto row = m_list->currentRow();
    if (row < 0 || row >= m_people.size())
        return;
    const auto person = m_people[row];
    dismiss();
    if (m_chosen)
        m_chosen(person);
}

void PeopleSuggestions::showState(const QString &text, bool loading)
{
    m_list->hide();
    m_state->setText(text);
    setLoadingIndicator(m_state, loading);
    m_state->show();
    open(m_state->sizeHint().height() + 10);
}

int PeopleSuggestions::embeddedResultHeight() const
{
    const int limit = m_anchor && m_anchor->window()->height() >= 570 ? 7 : 4;
    return qMin<int>(m_people.size(), limit) * m_list->sizeHintForRow(0) + 12;
}

void PeopleSuggestions::open(int height)
{
    if (m_embedded) {
        setFixedHeight(height);
        show();
        if (onEmbeddedLayoutChanged)
            onEmbeddedLayoutChanged();
        return;
    }
    if (!m_anchor)
        return;
    const auto top = m_anchor->mapTo(parentWidget(), QPoint());
    const auto area = parentWidget()->rect();
    const int width = qMin(area.width(), qBound(230, m_anchor->width(), 380));
    const bool below = top.y() - height < area.top();
    const int y = below ? top.y() + m_anchor->height() + 3 : top.y() - height - 3;
    setGeometry(qBound(area.left(), top.x(), qMax(area.left(), area.right() - width + 1)),
                qBound(area.top(), y, qMax(area.top(), area.bottom() - height + 1)), width, height);
    show();
    raise();
}

} // namespace bx::ui
