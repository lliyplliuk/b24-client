#pragma once

#include "ui/workspace_style.h"

#include <QCryptographicHash>
#include <QSettings>
#include <QSplitter>
#include <QVBoxLayout>

namespace bx {

QSettings chatPreferences();

class DirectorySplitter final : public QSplitter {
public:
    DirectorySplitter(const QString &key, int regularWidth, int compactWidth, QWidget *parent);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void restoreWidth();

    const QString m_key;
    const int m_regularWidth;
    const int m_compactWidth;
    int m_preferredWidth = 0;
};

QWidget *directory(const QString &title, QListWidget **list, QWidget *parent, QLabel **titleLabel = nullptr);

void selectAvailable(QListWidget *list, QListWidgetItem *selection);

QString chatPreferenceKey(const QString &accountScope, QLatin1String preference);

} // namespace bx
