#pragma once

#include "core/models.h"
#include "ui/chat_reply.h"
#include <QFrame>
#include <QPointer>
#include <QTextCharFormat>
#include <QTimer>
#include <functional>

class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace bx::ui {

class ChatTextEdit;
class EmojiPanel;
class GlyphButton;
class PeopleSuggestions;

class ChatComposer final : public QFrame {
public:
    ChatComposer(QWidget *host, const QString &inputName, const QString &emojiName, const QString &fileName,
                 const QString &sendName);
    ~ChatComposer() override;

    QPlainTextEdit *input() const;
    void setPlaceholderText(const QString &text);
    QPushButton *file() const;
    QPushButton *emoji() const;
    QPushButton *send() const;
    const ChatReply &reply() const;
    bool hasDraftImage() const;
    void clearDraftImage();
    void forgetDraftImages();
    void setImageDraftHandlers(std::function<void()> changed, std::function<void(const QString &)> error);
    bool composing() const;
    bool editingMessage() const;
    bool messageEditPending() const;
    bool matchesEdit(const QString &dialog, const QString &id) const;
    void setEditStateChanged(std::function<void()> callback);
    void setEditLatest(std::function<bool()> callback);
    void beginMessageEdit(const QString &dialog, const QString &id, const QString &text,
                          std::function<void(const QString &)> submit);
    bool canSaveMessageEdit(const QString &text) const;
    bool submitMessageEdit();
    bool cancelMessageEdit();
    void finishMessageEdit();
    void acceptMessageEdit(const QString &text);
    void failMessageEdit();
    void setTypingActions(std::function<QString()> scope, std::function<void(const QString &)> notify);
    void cancelTyping();
    void setTypingIndicator(const QString &user, int duration, const QString &name = {});
    void setTypingName(const QString &user, const QString &name);
    void clearTypingIndicator();
    void setReply(const ChatReply &reply);
    void setMentionSearchHandler(std::function<void(const QString &, const QString &)> handler);
    void setMentionResults(const QString &id, const QVector<Person> &people);
    void setMentionSearchError(const QString &id);
    void setMentionAvatars(std::function<QIcon(const Person &)> icon, std::function<void()> onVisible);
    QList<QUrl> visibleMentionAvatarUrls() const;
    void refreshMentionAvatars();
    bool handleMentionKey(QKeyEvent *event);
    bool handleComposerKey(QKeyEvent *event);
    void dismissMentionSearch();
    ChatDraft draft() const;
    QString wireText() const;
    void setDraft(const ChatDraft &draft);
    void dismissEmoji();

private:
    void updateEditBanner();

    struct Candidate {
        int start = -1;
        int end = -1;
        QString query;

        bool operator==(const Candidate &other) const;
    };
    static constexpr int MentionId = QTextFormat::UserProperty + 41;
    static constexpr int MentionName = QTextFormat::UserProperty + 42;

    PeopleSuggestions *m_mentions = nullptr;
    Candidate m_candidate;
    QTimer m_mentionCheck;
    bool m_mentionSearchEnabled = false;
    bool m_loadingDraft = false;
    bool m_hasMentions = false;

    static QTextCharFormat mentionFormat(const QString &id, const QString &name);

    QVector<ChatMention> mentionSpans() const;

    Candidate currentCandidate() const;
    void scheduleMentionSearch();
    void insertMention(const Person &person);

    static QString pastedImageName();
    void stageImage(const QImage &image);
    void setDraftImage(std::shared_ptr<const ChatDraftImage> image);

    ChatTextEdit *m_input = nullptr;
    QFrame *m_imageRow = nullptr;
    QLabel *m_imagePreview = nullptr;
    std::shared_ptr<const ChatDraftImage> m_image;
    std::function<void()> m_onImageChanged;
    std::function<void(const QString &)> m_onImageError;
    QLabel *m_typing = nullptr;
    QTimer m_typingExpiry;
    QTimer m_typingDelay;
    QString m_typingUser;
    QString m_pendingTyping;
    std::function<QString()> m_typingScope;
    std::function<void(const QString &)> m_sendTyping;
    QPushButton *m_file = nullptr;
    QPushButton *m_emoji = nullptr;
    GlyphButton *m_send = nullptr;
    QPointer<EmojiPanel> m_panel;
    QFrame *m_quote = nullptr;
    QLabel *m_quoteText = nullptr;
    QPushButton *m_cancelQuote = nullptr;
    ChatReply m_reply;
    ChatDraft m_preEditDraft;
    int m_preEditAnchor = 0;
    int m_preEditPosition = 0;
    QString m_sendName;
    QString m_editId;
    QString m_editDialog;
    QString m_editOriginal;
    QString m_editSubmitted;
    bool m_editPending = false;
    std::function<void()> m_onEditStateChanged;
    std::function<bool()> m_onEditLatest;
    std::function<void(const QString &)> m_submitEdit;
};

} // namespace bx::ui
