#pragma once

#include "core/models.h"
#include <QDialog>
#include <functional>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace bx::ui {

class StageManagerDialog final : public QDialog {
public:
    explicit StageManagerDialog(const QString &projectId, QWidget *parent);

    QString projectId() const { return m_projectId; }

    void setStages(const QVector<Stage> &stages);
    void setStatus(const QString &text, bool loading = false);
    void setWritable(bool writable);
    void setOnSave(std::function<void(const QString &, const QString &, const QString &, const QString &)> handler);
    void setOnDelete(std::function<void(const QString &)> handler);

private:
    bool validRow(int index) const;

    QString currentStage() const;

    void chooseColor();

    void move(int offset);

    void updateColorSwatch();

    void updateButtons();

    QString m_projectId;
    QVector<Stage> m_data;
    QListWidget *m_stages = nullptr;
    QLineEdit *m_title = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_add = nullptr;
    QPushButton *m_color = nullptr;
    QPushButton *m_rename = nullptr;
    QPushButton *m_up = nullptr;
    QPushButton *m_down = nullptr;
    QPushButton *m_remove = nullptr;
    bool m_writable = true;
    std::function<void(const QString &, const QString &, const QString &, const QString &)> m_onSave;
    std::function<void(const QString &)> m_onDelete;
};

} // namespace bx::ui
