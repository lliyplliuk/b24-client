#pragma once

#include "core/models.h"
#include "ui/workspace_style.h"

#include <QDialog>
#include <QHash>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

#include <functional>

namespace bx::ui {

class ProjectToolsDialog final : public QDialog {
public:
    ProjectToolsDialog(QString projectId, const QString &title, QWidget *parent)
        : QDialog(parent), m_projectId(std::move(projectId))
    {
        setProperty("workspaceDialog", true);
        setWindowTitle(QStringLiteral("Проект · %1").arg(title));
        setAccessibleName(QStringLiteral("Инструменты проекта"));
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowModality(Qt::WindowModal);
        resize(570, 500);
        auto *layout = new QVBoxLayout(this);
        layout->addWidget(workspace::label(title, "pageTitle", this));
        m_tabs = new QTabWidget(this);
        layout->addWidget(m_tabs, 1);

        auto *members = new QWidget(m_tabs);
        auto *membersLayout = new QVBoxLayout(members);
        m_membersList = new QListWidget(members);
        m_membersList->setAccessibleName(QStringLiteral("Список участников проекта"));
        workspace::configureRows(m_membersList, workspace::Row::Directory);
        membersLayout->addWidget(m_membersList, 1);
        m_tabs->addTab(members, QStringLiteral("Участники"));

        auto *files = new QWidget(m_tabs);
        auto *filesLayout = new QVBoxLayout(files);
        auto *folder = new QHBoxLayout;
        m_back = new QPushButton(QStringLiteral("Назад"), files);
        folder->addWidget(m_back);
        m_path = workspace::label(QStringLiteral("Диск проекта"), "quiet", files);
        folder->addWidget(m_path, 1);
        filesLayout->addLayout(folder);
        m_filesList = new QListWidget(files);
        m_filesList->setAccessibleName(QStringLiteral("Список файлов проекта"));
        workspace::configureRows(m_filesList, workspace::Row::Directory);
        filesLayout->addWidget(m_filesList, 1);
        auto *actions = new QHBoxLayout;
        m_more = new QPushButton(QStringLiteral("Ещё файлы"), files);
        m_more->hide();
        actions->addWidget(m_more);
        actions->addStretch(1);
        m_save = new QPushButton(QStringLiteral("Скачать"), files);
        actions->addWidget(m_save);
        filesLayout->addLayout(actions);
        m_tabs->addTab(files, QStringLiteral("Файлы"));

        m_status = workspace::label(QString(), "quiet", this);
        m_status->setAccessibleName(QStringLiteral("Состояние инструментов проекта"));
        layout->addWidget(m_status);
        auto *footer = new QHBoxLayout;
        auto *refresh = new QPushButton(QStringLiteral("Обновить"), this);
        auto *close = new QPushButton(QStringLiteral("Закрыть"), this);
        footer->addWidget(refresh);
        footer->addStretch(1);
        footer->addWidget(close);
        layout->addLayout(footer);

        connect(m_tabs, &QTabWidget::currentChanged, this, [this] {
            m_status->clear();
            updateControls();
            if ((m_tabs->currentIndex() == 0 && !m_membersRequested)
                || (m_tabs->currentIndex() == 1 && !m_filesRequested))
                reload();
        });
        connect(refresh, &QPushButton::clicked, this, &ProjectToolsDialog::reload);
        connect(close, &QPushButton::clicked, this, &QDialog::reject);
        connect(m_back, &QPushButton::clicked, this, [this] {
            if (m_folders.isEmpty() || m_filesLoading)
                return;
            m_folders.removeLast();
            requestFiles(0);
        });
        connect(m_more, &QPushButton::clicked, this, [this] {
            if (!m_filesLoading && m_nextOffset >= 0)
                requestFiles(m_nextOffset);
        });
        connect(m_filesList, &QListWidget::currentItemChanged, this, [this] { updateControls(); });
        connect(m_filesList, &QListWidget::itemActivated, this, [this] {
            if (m_filesLoading)
                return;
            const auto selected = selectedFile();
            if (selected.folder) {
                m_folders.append({selected.file.id, selected.file.name});
                requestFiles(0);
            } else {
                saveSelected();
            }
        });
        connect(m_save, &QPushButton::clicked, this, &ProjectToolsDialog::saveSelected);
        updateControls();
    }

    const QString &projectId() const { return m_projectId; }
    QString folderId() const { return m_folders.isEmpty() ? QString() : m_folders.last().first; }
    std::function<void()> onMembers;
    std::function<void(const QString &, int)> onFiles;
    std::function<void(const MessageFile &)> onDownload;

    void reload()
    {
        if (m_tabs->currentIndex() == 1) {
            requestFiles(0);
        } else if (!m_membersLoading && onMembers) {
            m_membersRequested = m_membersLoading = true;
            m_membersList->clear();
            workspace::loadingRow(m_membersList, QStringLiteral("Загружаем участников…"));
            m_status->clear();
            onMembers();
        }
    }

    void setMembers(const QVector<ProjectMember> &members)
    {
        m_membersLoading = false;
        m_membersList->clear();
        for (const auto &member : members) {
            static const QString roles[] = {QStringLiteral("Владелец"), QStringLiteral("Модератор"),
                QStringLiteral("Участник"), QStringLiteral("Приглашён"), QStringLiteral("Ожидает вступления")};
            const auto detail = roles[static_cast<int>(member.role)];
            workspace::addRow(m_membersList, member.name, detail, member.position, member.id);
        }
        if (members.isEmpty())
            workspace::emptyRow(m_membersList, QStringLiteral("Участников нет"));
    }

    void setFiles(const QString &folder, const QVector<ProjectDiskItem> &items, int next)
    {
        if (folder != folderId())
            return;
        m_filesLoading = false;
        if (m_requestedOffset == 0)
            m_filesList->clear();
        int inserted = 0;
        for (const auto &item : items) {
            if (m_items.contains(item.file.id))
                continue;
            m_items.insert(item.file.id, item);
            ++inserted;
            const auto detail = item.folder ? QStringLiteral("Папка")
                : item.file.downloadUrl.isEmpty() ? QStringLiteral("Скачивание недоступно") : QStringLiteral("Файл");
            workspace::addRow(m_filesList, item.file.name, detail, {}, item.file.id);
        }
        if (m_items.isEmpty())
            workspace::emptyRow(m_filesList, QStringLiteral("В этой папке нет доступных файлов"));
        m_nextOffset = inserted == 0 && m_requestedOffset > 0 ? -1 : next;
        updateControls();
    }

    void membersFailed(const QString &reason)
    {
        m_membersLoading = false;
        m_membersList->clear();
        workspace::emptyRow(m_membersList, QStringLiteral("Участники недоступны"));
        if (m_tabs->currentIndex() == 0)
            m_status->setText(reason);
    }

    void filesFailed(const QString &folder, const QString &reason)
    {
        if (folder != folderId())
            return;
        m_filesLoading = false;
        if (m_requestedOffset == 0) {
            m_filesList->clear();
            workspace::emptyRow(m_filesList, QStringLiteral("Файлы недоступны"));
        }
        if (m_tabs->currentIndex() == 1)
            m_status->setText(reason);
        updateControls();
    }

private:
    void requestFiles(int offset)
    {
        if (m_filesLoading || !onFiles)
            return;
        m_filesRequested = m_filesLoading = true;
        m_requestedOffset = offset;
        m_status->clear();
        if (offset == 0) {
            m_nextOffset = -1;
            m_items.clear();
            m_filesList->clear();
            workspace::loadingRow(m_filesList, QStringLiteral("Загружаем файлы…"));
        }
        m_path->setText(m_folders.isEmpty() ? QStringLiteral("Диск проекта") : m_folders.last().second);
        updateControls();
        onFiles(folderId(), offset);
    }

    ProjectDiskItem selectedFile() const
    {
        const auto *item = m_filesList->currentItem();
        return item ? m_items.value(item->data(workspace::Id).toString()) : ProjectDiskItem{};
    }

    void saveSelected()
    {
        const auto item = selectedFile();
        if (!m_filesLoading && !item.folder && !item.file.downloadUrl.isEmpty() && onDownload)
            onDownload(item.file);
    }

    void updateControls()
    {
        const bool loadingMore = m_tabs->currentIndex() == 1 && m_filesLoading && m_requestedOffset > 0;
        if (loadingMore)
            m_status->setText(QStringLiteral("Загружаем ещё файлы…"));
        else if (!m_filesLoading && m_status->text() == QStringLiteral("Загружаем ещё файлы…"))
            m_status->clear();
        setLoadingIndicator(m_status, loadingMore);
        const auto item = selectedFile();
        m_back->setEnabled(!m_filesLoading && !m_folders.isEmpty());
        m_more->setVisible(m_nextOffset >= 0);
        m_more->setEnabled(!m_filesLoading);
        m_save->setEnabled(!m_filesLoading && !item.folder && !item.file.downloadUrl.isEmpty());
    }

    const QString m_projectId;
    QTabWidget *m_tabs = nullptr;
    QListWidget *m_membersList = nullptr;
    QListWidget *m_filesList = nullptr;
    QLabel *m_path = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_back = nullptr;
    QPushButton *m_more = nullptr;
    QPushButton *m_save = nullptr;
    QVector<QPair<QString, QString>> m_folders;
    QHash<QString, ProjectDiskItem> m_items;
    int m_nextOffset = -1;
    int m_requestedOffset = 0;
    bool m_membersRequested = false;
    bool m_membersLoading = false;
    bool m_filesRequested = false;
    bool m_filesLoading = false;
};

} // namespace bx::ui
