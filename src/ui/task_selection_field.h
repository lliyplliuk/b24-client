#pragma once

#include "ui/task_picker_choices.h"
#include <QFrame>
#include <QIcon>
#include <QLineEdit>
#include <QScrollArea>
#include <QPushButton>
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

        bool operator==(const Entry &other) const;
    };

    explicit TaskSelectionField(QLineEdit *input, QWidget *parent, bool wrap = false);
    void setSelection(const QVector<Entry> &entries);
    void refreshIcons();
    QList<QUrl> visibleAvatarUrls() const;
    void fitSelection();
    void setHeightBudget(int height);

    std::function<QIcon(const Entry &)> iconFor;
    std::function<void(const QString &)> onRemove;
    std::function<void()> onVisibleChanged;
    std::function<void()> onHeightChanged;

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Pill {
        QWidget *widget;
        QLabel *avatar;
        TaskPickerValueLabel *value;
    };
    bool updateIcons();
    int placeRows(int available);
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
