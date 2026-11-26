#pragma once

#include "ui/task_content_list.h"
#include "ui/task_requisite_row.h"
#include "ui/workspace_presentation.h"
#include "ui/chat_widgets.h"
#include "ui/task_description_editor.h"
#include "ui/task_relations.h"
#include <QDialog>

class QSplitter;
class QScrollArea;

namespace bx::ui {

class TaskFields;
class TaskCardRequisites;
class TaskCardActions;

class TaskCardDialog final : public QDialog {
public:
    TaskCardDialog(const Task &task, const QString &projectId, QWidget *parent, bool detailsLoaded = true);
    void setLink(const QString &link);
    void setStatus(const QString &text, bool loading = false);
    void setTask(const Task &task, bool detailsLoaded = true);
    void setStages(const QVector<Stage> &stages);
    void setWriteState(bool writable, bool editing, bool moving, bool sending, bool uploading);
    QString taskId() const { return m_task.id; }
    QString projectId() const { return m_projectId; }
    QPlainTextEdit *messageInput() const { return m_input; }
    ChatComposer *composer() const { return m_composer; }
    QListWidget *messageList() const { return m_messages; }
    TaskContentList *descriptionList() const { return m_descriptionRows; }
    TaskContentList *attachmentList() const { return m_attachmentRows; }
    TaskDescriptionEditor *descriptionEditor() const;
    QScrollBar *detailsScrollBar() const;
    void setProjectCaption(const QString &caption);
    void acceptEdit();
    QString searchQuery() const;
    QString taskChatDialogId() const { return m_task.chatDialogId; }
    TaskRelationsPanel *relations() const { return m_relations; }
    void setPinnedMessages(const QVector<Message> &messages);
    void setOnPinned(std::function<void(const QString &)> handler);
    void setOnLatest(std::function<void()> handler);
    bool focusMessage(const QString &id);
    void setContextMode(bool active);
    void showMessages(const QVector<Message> &messages, MessageView view = MessageView::History);
    void setChatStatus(const QString &text, bool loading = false);
    void setOnCopy(std::function<void(const QString &)> handler);
    void setOnSave(std::function<void(const TaskEdit &)> handler);
    void setOnDelete(std::function<void()> handler);
    bool canDeleteTask() const;
    void cancelFooterDelegation();
    void setProjectChoices(const QVector<Project> &projects);
    void setCanDetachProject(bool allowed);
    void setMoreProjectsAvailable(bool available);
    void setRequisiteAvatars(TaskRequisiteRow::AvatarSource source);
    void setOnRequisiteAvatarChoicesChanged(std::function<void()> changed);
    QList<QUrl> visibleRequisiteAvatarUrls() const;
    void refreshRequisiteAvatars();
    void setOnMoreProjects(std::function<void()> handler);
    void setPeopleSearch(const std::function<void(const QString &, const QString &)> &handler);
    void setPeopleResults(const QString &id, const QVector<Person> &people);
    bool setPeopleSearchError(const QString &id);
    void setOnMove(std::function<void(const QString &)> handler);
    void setOnSend(std::function<void()> handler);
    void setOnSearch(std::function<void(const QString &)> handler);
    void setOnFile(std::function<void(bool)> handler);

    // Несохранённые изменения карточки: черновик сообщения, правки полей, идущая отправка или загрузка.
    bool hasUnsavedChanges() const;

    // Диалог подтверждения отбрасывания правок. По умолчанию — предупреждение с выбором.
    void setDiscardConfirmation(std::function<bool()> handler);

    // Спрашивает пользователя перед отбрасыванием правок. false — закрытие отменено.
    bool confirmDiscardIfNeeded(bool remember = true);

protected:
    void closeEvent(QCloseEvent *event) override;
    void reject() override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *target, QEvent *event) override;

private:
    bool confirmDiscardDialog();
    void send();
    void searchMessages();
    void copy(const QString &value, const QString &feedback);
    void restoreDetailsWidth();
    void setEditingDescription(bool enabled);
    friend class TaskCardRequisites;
    friend class TaskCardActions;
    static void addTaskFile(TaskContentList *list, const MessageFile &file, int serial);
    void showTaskContent(const Task &task, bool detailsLoaded);
    bool canUseTaskActions() const;
    void updateButtons();

    Task m_task;
    QString m_projectId;
    QString m_link;
    QLabel *m_taskTitle = nullptr;
    TaskCardRequisites *m_requisites = nullptr;
    QSplitter *m_splitter = nullptr;
    QScrollArea *m_detailsScroll = nullptr;
    TaskContentList *m_descriptionRows = nullptr;
    TaskContentList *m_attachmentRows = nullptr;
    TaskRelationsPanel *m_relations = nullptr;
    QWidget *m_editActions = nullptr;
    QPushButton *m_edit = nullptr;
    QPushButton *m_cancelEdit = nullptr;
    QPushButton *m_maximize = nullptr;
    int m_detailsWidth = 0;
    bool m_editMode = false;
    QLabel *m_empty = nullptr;
    QLabel *m_status = nullptr;
    TaskFields *m_fields = nullptr;
    QListWidget *m_messages = nullptr;
    QPlainTextEdit *m_input = nullptr;
    ChatComposer *m_composer = nullptr;
    QLineEdit *m_messageSearch = nullptr;
    QPushButton *m_searchToggle = nullptr;
    PinnedMessageBar *m_pinned = nullptr;
    QPushButton *m_contextBack = nullptr;
    bool m_contextMode = false;
    QPushButton *m_copyLink = nullptr;
    QLabel *m_copyFeedback = nullptr;
    QTimer m_copyFeedbackTimer;
    QPushButton *m_save = nullptr;
    QPushButton *m_attach = nullptr;
    QPushButton *m_file = nullptr;
    QPushButton *m_send = nullptr;
    QPushButton *m_emoji = nullptr;
    bool m_writable = false;
    bool m_editing = false;
    bool m_moving = false;
    bool m_sending = false;
    bool m_uploading = false;
    bool m_discardConfirmed = false;
    TaskCardActions *m_taskActions = nullptr;
    std::function<void(const QString &)> m_onCopy;
    std::function<void(const TaskEdit &)> m_onSave;
    std::function<void()> m_onDelete;
    std::function<void()> m_onSend;
    std::function<void(const QString &)> m_onSearch;
    std::function<void()> m_onLatest;
    std::function<void(bool)> m_onFile;
    std::function<bool()> m_confirmDiscard;
};

} // namespace bx::ui
