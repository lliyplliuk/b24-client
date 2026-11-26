#pragma once

#include "ui/task_card_controller.h"
#include "ui/notifications_panel.h"
#include "ui/public_link_preview.h"

#include <QCache>
#include <QMainWindow>
#include <QPixmap>
#include <QPointer>
#include <QSet>

#include <memory>

class QLabel;
class QCheckBox;
class QDialog;
class QListWidget;
class QPushButton;
class QResizeEvent;
class QStackedWidget;
class QTimer;

namespace bx {

class ChatPanel;
class ProjectsPanel;
class WorkspaceMessagePreviews;
namespace ui { class WorktimePanel; class MessageFiles; }
namespace ui { class AvatarBadge; class ChatComposer; }

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    using PublicLinkRequest = std::function<bool(const QUrl &, ui::PublicLinkPreviewFetcher::Completion)>;
    explicit MainWindow(Backend *backend, bool readOnly = false, QWidget *parent = nullptr,
                        PublicLinkRequest publicLinkRequest = {});
    ~MainWindow() override;

    // Открытие карточки задачи (окно-модальный диалог).
    void openTaskCard(const QString &taskId);
    QString taskLinkFor(const QString &taskId) const;
    // Opens supported portal links in the workspace and other web links in the browser.
    void openMessageLink(const QString &href, QWidget *source = nullptr);
    void setClientUpdateFeed(ClientUpdateFeed *feed);
    void setLogoutAvailable(bool available);
    void setSessionRecoveryAvailable(bool available);
    bool confirmWorkspaceClose();
    bool closeWorkspace();

signals:
    void activeSectionChanged(int section);
    void logoutRequested();
    void sessionRecoveryRequested();
    void historyClosed(bool task);

public slots:
    void setStatus(const QString &text, bool loading = false);
    void onConnectionChanged(bool online);

protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onSectionChanged(int index);
    void onRefresh();
    void onFailed(const QString &operation, const QString &reason);

private:
    void buildUi();
    void onAvatarLoaded(const QUrl &url, const QByteArray &bytes);
    void refreshCurrentSection(bool reloadProjectDirectory = true);
    void showProfileSettings();
    void reloadChatVisibilityPreference();
    QUrl messageLinkDestination(const QString &href) const;
    void setRailUnreadCount(int row, qint64 count);
    void setRailStatus(int row, const QString &detail);
    void requestRailWorkday();
    void updateRailWorkday();
    void scheduleAvatars();
    void loadVisibleAvatars();
    void requestAvatar(const QUrl &url);
    void updateAvatarRows();
    void openPortalTarget(NotificationTarget target, QWidget *source = nullptr, const QUrl &messageUrl = {});
    void openPerson(const QString &personId, QWidget *source = nullptr);
    void bindMessageFiles(QListWidget *list, bool linkCards = false);
    void updateWriteAvailability();
    void pickFile(const QString &scope, const QString &kind);
    void sendFromComposer(ui::ComposerSendController &controller, ui::ChatComposer *composer, const QString &id);
    void uploadPastedChatImage(ui::ChatComposer *composer, const QString &scope, bool task, const QString &caption);
    bool writable() const { return !m_readOnly && m_online; }
    ChatPanel *m_chatPanel = nullptr;
    Backend *m_backend;
    bool m_readOnly;
    bool m_online = false;
    bool m_closingWorkspace = false;

    QListWidget *m_sections = nullptr;
    QTimer *m_railWorkdayTimer = nullptr;
    bool m_railWorkdayCurrent = false;
    QStackedWidget *m_stack = nullptr;
    ui::WorktimePanel *m_worktime = nullptr;
    QLabel *m_mode = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_logout = nullptr;
    QWidget *m_sessionRecovery = nullptr;
    QDialog *m_profileSettings = nullptr;
    QCheckBox *m_dismissedChatsCheck = nullptr;
    QLabel *m_chatSettingsAccountHint = nullptr;
    WorkspaceUpload m_upload;

    QCache<QUrl, QPixmap> m_avatars{128};
    std::unique_ptr<ui::MessageFiles> m_messageFiles;
    std::unique_ptr<WorkspaceMessagePreviews> m_messagePreviews;
    QSet<QUrl> m_requestedAvatars;
    bool m_avatarUpdateQueued = false;
    std::unique_ptr<TaskCardController> m_tasks;
    ProjectsPanel *m_projectsPanel = nullptr;
    NotificationsPanel *m_events = nullptr;
    QPointer<QDialog> m_personDialog;
    QLabel *m_personName = nullptr;
    QLabel *m_personPosition = nullptr;
    ui::AvatarBadge *m_personAvatar = nullptr;
    QString m_personId;
    QUrl m_personAvatarUrl;

    QString m_accountScope;
    int m_lastSection = 0;
};

} // namespace bx
