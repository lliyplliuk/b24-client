#pragma once

#include "ui/workspace_style.h"
#include <QPushButton>
#include <functional>

class QVBoxLayout;

namespace bx::ui {

class AvatarBadge;

class TaskRequisiteRow final : public QPushButton {
public:
    struct Entry {
        QString name;
        QUrl url;
        QString projectColor{};
        QString projectVariant{};
        QString projectType{};

        bool operator==(const Entry &other) const;
    };
    using AvatarSource = std::function<const QPixmap *(const QUrl &)>;

    TaskRequisiteRow(const QString &caption, QWidget *parent, std::optional<workspace::AvatarKind> kind = {},
                     bool multiple = false);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    int heightForWidth(int width) const override;

    void setEditable(bool editable);

    void setEntries(QVector<Entry> entries);

    QList<QUrl> visibleAvatarUrls(const QWidget *viewport) const;

    void refreshAvatars(const AvatarSource &source);

private:
    QString m_caption;
    std::optional<workspace::AvatarKind> m_kind;
    bool m_multiple = false;
    QVBoxLayout *m_values = nullptr;
    QVector<Entry> m_entries;
    struct Avatar {
        AvatarBadge *widget;
        QUrl url;
        bool showWithoutPhoto;
    };
    QVector<Avatar> m_avatars;
};

} // namespace bx::ui
