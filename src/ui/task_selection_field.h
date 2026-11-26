#pragma once

#include "ui/task_picker_choices.h"
#include "ui/typography.h"

#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QUrl>

#include <functional>

namespace bx::ui {

class TaskSelectionField final : public QFrame {
public:
    struct Entry {
        QString id;
        QString name;
        QUrl avatar;
        bool pending = false;

        bool operator==(const Entry &other) const
        {
            return id == other.id && name == other.name && avatar == other.avatar && pending == other.pending;
        }
    };

    explicit TaskSelectionField(QLineEdit *input, QWidget *parent, bool wrap = false)
        : QFrame(parent), m_input(input), m_wrap(wrap), m_content(this)
    {
        setObjectName(QStringLiteral("taskSelectionField"));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMinimumWidth(inputWidth + 9);
        setFixedHeight(34);
        if (m_wrap) {
            m_scroll = new QScrollArea(this);
            m_scroll->setObjectName(QStringLiteral("taskInlineSelectionScroll"));
            m_scroll->setFrameShape(QFrame::NoFrame);
            m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            m_scroll->setFocusPolicy(Qt::NoFocus);
            m_content = new QWidget;
            m_content->setObjectName(QStringLiteral("taskInlineSelectionItems"));
            m_scroll->setWidget(m_content);
            connect(m_scroll->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
                if (onVisibleChanged)
                    onVisibleChanged();
            });
        }
        m_input->setParent(m_content);
        m_input->setObjectName(QStringLiteral("taskSelectionSearch"));
        m_input->setFrame(false);
        m_input->setMinimumWidth(inputWidth);
        m_input->installEventFilter(this);
        m_input->show();
    }

    void setSelection(const QVector<Entry> &entries)
    {
        if (entries == m_entries)
            return;
        for (const auto &pill : m_pills) {
            pill.widget->hide();
            pill.widget->deleteLater();
        }
        m_entries = entries;
        m_pills.clear();
        QPushButton *previousRemove = nullptr;
        for (const auto &entry : entries) {
            auto *pill = new QWidget(m_content);
            pill->setObjectName(QStringLiteral("taskInlineSelectionPill"));
            pill->setProperty("pending", entry.pending);
            auto *parts = new QHBoxLayout(pill);
            parts->setContentsMargins(5, 2, 6, 2);
            parts->setSpacing(5);
            auto *avatar = new QLabel(pill);
            avatar->setFixedSize(20, 20);
            parts->addWidget(avatar);

            auto *value = new TaskPickerValueLabel(pill);
            value->setObjectName(QStringLiteral("taskInlineSelectionValue"));
            value->setAccessibleName(entry.name);
            value->setFullText(entry.name);
            value->setAccessibleDescription(entry.pending ? QStringLiteral("Новый выбор") : QStringLiteral("Текущий выбор"));
            value->setToolTip(plainTooltip(entry.name));
            parts->addWidget(value, 1);
            if (entry.pending) {
                auto *mark = new QWidget(pill);
                mark->setObjectName(QStringLiteral("taskInlineDraftDot"));
                mark->setFixedSize(6, 6);
                parts->addWidget(mark);
            }
            if (onRemove) {
                auto *remove = new QPushButton(QStringLiteral("×"), pill);
                remove->setObjectName(QStringLiteral("taskInlineSelectionRemove"));
                remove->setFixedSize(20, 22);
                remove->setAccessibleName(QStringLiteral("Удалить наблюдателя %1").arg(entry.name));
                remove->setAutoDefault(false);
                remove->installEventFilter(this);
                connect(remove, &QPushButton::clicked, this, [this, id = entry.id] { onRemove(id); });
                parts->addWidget(remove);
                if (previousRemove)
                    QWidget::setTabOrder(previousRemove, remove);
                previousRemove = remove;
            }
            pill->show();
            m_pills.append({pill, avatar, value});
        }
        if (previousRemove)
            QWidget::setTabOrder(previousRemove, m_input);
        updateIcons();
        fitSelection();
    }

    void refreshIcons()
    {
        if (updateIcons())
            fitSelection();
    }

    QList<QUrl> visibleAvatarUrls() const
    {
        QList<QUrl> urls;
        for (int i = 0; i < m_entries.size(); ++i) {
            const auto *pill = m_pills.at(i).widget;
            const auto &url = m_entries.at(i).avatar;
            if (!url.isEmpty() && pill->isVisible() && !pill->visibleRegion().isEmpty())
                urls.append(url);
        }
        return urls;
    }

    void fitSelection()
    {
        const int available = qMax(1, width() - 9);
        int height = placeRows(available);
        if (m_scroll) {
            const int shownHeight = qMin(height, m_heightBudget);
            const bool overflow = height > shownHeight;
            const int contentWidth = available - (overflow ? m_scroll->verticalScrollBar()->sizeHint().width() : 0);
            if (overflow)
                height = placeRows(contentWidth);
            m_scroll->setVerticalScrollBarPolicy(overflow ? Qt::ScrollBarAlwaysOn : Qt::ScrollBarAlwaysOff);
            m_content->setFixedSize(contentWidth, height);
            m_scroll->setGeometry(4, 3, available, shownHeight);
            if (overflow && m_input->hasFocus())
                m_scroll->ensureWidgetVisible(m_input, 0, 2);
            height = shownHeight;
        }
        if (minimumHeight() != height + 6) {
            setFixedHeight(height + 6);
            if (onHeightChanged)
                onHeightChanged();
        }
        if (onVisibleChanged)
            onVisibleChanged();
    }

    void setHeightBudget(int height)
    {
        if (m_heightBudget == height)
            return;
        m_heightBudget = height;
        fitSelection();
    }

    std::function<QIcon(const Entry &)> iconFor;
    std::function<void(const QString &)> onRemove;
    std::function<void()> onVisibleChanged;
    std::function<void()> onHeightChanged;

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QFrame::resizeEvent(event);
        if (event->oldSize().width() != event->size().width())
            fitSelection();
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (m_scroll && event->type() == QEvent::FocusIn)
            m_scroll->ensureWidgetVisible(static_cast<QWidget *>(watched), 0, 2);
        return QFrame::eventFilter(watched, event);
    }

private:
    struct Pill {
        QWidget *widget;
        QLabel *avatar;
        TaskPickerValueLabel *value;
    };

    bool updateIcons()
    {
        bool visibilityChanged = false;
        for (int i = 0; i < m_entries.size(); ++i) {
            const auto icon = iconFor ? iconFor(m_entries.at(i)) : QIcon();
            auto *avatar = m_pills.at(i).avatar;
            if (avatar->isHidden() != icon.isNull()) {
                avatar->setVisible(!icon.isNull());
                visibilityChanged = true;
            }
            avatar->setPixmap(icon.pixmap(20, 20));
        }
        return visibilityChanged;
    }

    int placeRows(int available)
    {
        constexpr int gap = 4;
        constexpr int chipHeight = 28;
        const QPoint origin = m_wrap ? QPoint() : QPoint(4, 3);
        QVector<QPair<QWidget *, QSize>> row;
        int y = 0;
        int used = 0;
        int rowHeight = chipHeight;
        const auto finishRow = [&] {
            int x = 0;
            for (const auto &[widget, size] : row) {
                widget->setGeometry(QRect(origin + QPoint(x, y + (rowHeight - size.height()) / 2), size));
                x += size.width() + gap;
            }
            y += rowHeight + gap;
            row.clear();
            used = 0;
            rowHeight = chipHeight;
        };
        for (int i = 0; i < m_pills.size(); ++i) {
            const auto &pill = m_pills.at(i);
            const auto &entry = m_entries.at(i);
            pill.value->ensurePolished();
            const auto *parts = pill.widget->layout();
            const auto margins = parts->contentsMargins();
            int fixedWidth = margins.left() + margins.right();
            for (int j = 0; j < parts->count(); ++j) {
                const auto *part = parts->itemAt(j)->widget();
                if (part != pill.value && (part != pill.avatar || !part->isHidden()))
                    fixedWidth += qBound(part->minimumWidth(), part->sizeHint().width(), part->maximumWidth())
                        + parts->spacing();
            }
            const int desired = fixedWidth + pill.value->fontMetrics().horizontalAdvance(entry.name) + 3;
            const int limit = m_wrap ? available : qMax(1, qMin(285, available - inputWidth - gap));
            const int width = qMin(limit, desired);
            const bool wrapName = m_wrap && desired > available;
            pill.value->setWrapLongText(wrapName);
            const int height = wrapName ? qMax(chipHeight, pill.value->heightForWidth(qMax(1, width - fixedWidth))
                                                         + margins.top() + margins.bottom()) : chipHeight;
            if (used > 0 && used + width > available)
                finishRow();
            row.append({pill.widget, QSize(width, height)});
            used += width + gap;
            rowHeight = qMax(rowHeight, height);
        }
        if (used > 0 && available - used < inputWidth)
            finishRow();
        row.append({m_input, QSize(qMax(1, available - used), chipHeight)});
        finishRow();
        return y - gap;
    }

    static constexpr int inputWidth = 106;
    QLineEdit *m_input;
    const bool m_wrap;
    QWidget *m_content;
    QScrollArea *m_scroll = nullptr;
    int m_heightBudget = QWIDGETSIZE_MAX;
    QVector<Entry> m_entries;
    QVector<Pill> m_pills;
};

} // namespace bx::ui
