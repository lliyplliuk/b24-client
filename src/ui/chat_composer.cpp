#include "ui/chat_composer.h"

#include "core/notification_targets.h"
#include "core/tunables.h"
#include "ui/chat_text_edit.h"
#include "ui/emoji_panel.h"
#include "ui/glyph_button.h"
#include "ui/people_suggestions.h"
#include "ui/workspace_style.h"
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QImageWriter>
#include <QKeyEvent>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QVBoxLayout>
#include <utility>

namespace bx::ui {

ChatComposer::ChatComposer(QWidget *host, const QString &inputName, const QString &emojiName, const QString &fileName,
                           const QString &sendName)
    : QFrame(host)
{
    setObjectName(QStringLiteral("chatComposer"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(9, 6, 9, 6);
    outer->setSpacing(4);
    m_typing = workspace::label(QStringLiteral("Печатает…"), "quiet", this);
    m_typing->setTextFormat(Qt::PlainText);
    m_typing->setAccessibleName(QStringLiteral("Индикатор набора сообщения"));
    outer->addWidget(m_typing);
    m_typing->hide();
    m_typingExpiry.setSingleShot(true);
    connect(&m_typingExpiry, &QTimer::timeout, this, [this] { setTypingIndicator(m_typingUser, 0); });
    m_typingDelay.setSingleShot(true);
    m_typingDelay.setInterval(5000);
    connect(&m_typingDelay, &QTimer::timeout, this, [this] {
        if (m_typingScope && m_sendTyping && m_typingScope() == m_pendingTyping && !m_input->document()->isEmpty())
            m_sendTyping(m_pendingTyping);
    });
    m_quote = new QFrame(this);
    m_quote->setObjectName(QStringLiteral("composerReply"));
    m_quote->setStyleSheet(QStringLiteral("QFrame#composerReply {background:#e8f1ee;border-left:3px solid #54877a;border-radius:5px;}"));
    auto *quoteRow = new QHBoxLayout(m_quote);
    quoteRow->setContentsMargins(8, 2, 4, 2);
    m_quoteText = workspace::label({}, "quiet", m_quote);
    m_quoteText->setAccessibleName(QStringLiteral("Цитата в черновике"));
    m_quoteText->setWordWrap(false);
    m_quoteText->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    quoteRow->addWidget(m_quoteText, 1);
    auto *cancel = new GlyphButton(Glyph::Close, QStringLiteral("Отменить ответ"), m_quote);
    quoteRow->addWidget(cancel);
    connect(cancel, &QPushButton::clicked, this, [this] {
        if (!cancelMessageEdit())
            setReply({});
        m_input->setFocus(Qt::OtherFocusReason);
    });
    m_cancelQuote = cancel;
    outer->addWidget(m_quote);
    m_quote->hide();

    m_imageRow = new QFrame(this);
    m_imageRow->setObjectName(QStringLiteral("composerPastedImage"));
    m_imageRow->setStyleSheet(QStringLiteral("QFrame#composerPastedImage {background:#e8f1ee;border-radius:5px;}"));
    auto *imageRow = new QHBoxLayout(m_imageRow);
    imageRow->setContentsMargins(8, 4, 4, 4);
    m_imagePreview = new QLabel(m_imageRow);
    m_imagePreview->setAccessibleName(QStringLiteral("Изображение из буфера"));
    m_imagePreview->setAlignment(Qt::AlignCenter);
    m_imagePreview->setFixedSize(80, 52);
    imageRow->addWidget(m_imagePreview);
    auto *filename = workspace::label(pastedImageName(), "quiet", m_imageRow);
    filename->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    imageRow->addWidget(filename, 1);
    auto *removeImage = new GlyphButton(Glyph::Close, QStringLiteral("Удалить изображение из черновика"), m_imageRow);
    imageRow->addWidget(removeImage);
    connect(removeImage, &QPushButton::clicked, this, [this] {
        clearDraftImage();
        m_input->setFocus(Qt::OtherFocusReason);
    });
    outer->addWidget(m_imageRow);
    m_imageRow->hide();

    auto *layout = new QHBoxLayout;
    layout->setSpacing(4);
    outer->addLayout(layout);
    m_file = new GlyphButton(Glyph::Attach, fileName, this);
    layout->addWidget(m_file, 0, Qt::AlignBottom);
    m_input = new ChatTextEdit(this);
    m_input->mayPasteImage = [this] { return !editingMessage(); };
    m_input->imagePasted = [this](const QImage &image) { stageImage(image); };
    m_input->userEdited = [this] {
        if (editingMessage() || !m_typingScope || m_typingDelay.isActive())
            return;
        m_pendingTyping = m_typingScope();
        if (!m_pendingTyping.isEmpty())
            m_typingDelay.start();
    };
    m_input->setObjectName(QStringLiteral("chatEditor"));
    m_input->setAccessibleName(inputName);
    setPlaceholderText(QStringLiteral("Напишите сообщение"));
    m_input->setToolTip(QStringLiteral("Enter — отправить, Shift+Enter — новая строка"));
    m_input->setTabChangesFocus(true);
    m_input->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_input->setFrameShape(QFrame::NoFrame);
    auto palette = m_input->palette();
    palette.setColor(QPalette::PlaceholderText, workspace::Muted);
    m_input->setPalette(palette);
    layout->addWidget(m_input, 1);
    m_mentions = new PeopleSuggestions(m_input);
    m_mentions->setOnChosen([this](const Person &person) { insertMention(person); });
    m_mentionCheck.setSingleShot(true);
    connect(&m_mentionCheck, &QTimer::timeout, this, [this] {
        const auto found = currentCandidate();
        if (found == m_candidate)
            return;
        m_candidate = found;
        if (found.start < 0)
            m_mentions->dismiss();
        else
            m_mentions->search(found.query);
    });
    connect(m_input, &QPlainTextEdit::textChanged, this, [this] { scheduleMentionSearch(); });
    connect(m_input, &QPlainTextEdit::cursorPositionChanged, this, [this] { scheduleMentionSearch(); });
    m_emoji = new GlyphButton(Glyph::Emoji, emojiName, this);
    m_emoji->setCheckable(true);
    layout->addWidget(m_emoji, 0, Qt::AlignBottom);
    m_send = new GlyphButton(Glyph::Send, sendName, this, true);
    m_sendName = sendName;
    layout->addWidget(m_send, 0, Qt::AlignBottom);

    connect(m_emoji, &QPushButton::clicked, this, [this, host] {
        if (!m_panel)
            m_panel = new EmojiPanel(host, m_emoji, m_input);
        if (m_panel->isVisible())
            m_panel->dismiss(true);
        else
            m_panel->openPanel();
    });
}

ChatComposer::~ChatComposer()
{
    delete m_panel.data();
}

QPlainTextEdit *ChatComposer::input() const
{
    return m_input;
}

void ChatComposer::setPlaceholderText(const QString &text)
{
    m_input->setSingleLinePlaceholder(text);
}

QPushButton *ChatComposer::file() const
{
    return m_file;
}

QPushButton *ChatComposer::emoji() const
{
    return m_emoji;
}

QPushButton *ChatComposer::send() const
{
    return m_send;
}

const ChatReply &ChatComposer::reply() const
{
    return m_reply;
}

bool ChatComposer::hasDraftImage() const
{
    return bool(m_image);
}

void ChatComposer::clearDraftImage()
{
    setDraftImage({});
}

void ChatComposer::forgetDraftImages()
{
    m_preEditDraft.image.reset();
    clearDraftImage();
}

void ChatComposer::setImageDraftHandlers(std::function<void()> changed, std::function<void(const QString &)> error)
{
    m_onImageChanged = std::move(changed);
    m_onImageError = std::move(error);
}

bool ChatComposer::composing() const
{
    return m_input->composing();
}

bool ChatComposer::editingMessage() const
{
    return !m_editId.isEmpty();
}

bool ChatComposer::messageEditPending() const
{
    return m_editPending;
}

bool ChatComposer::matchesEdit(const QString &dialog, const QString &id) const
{
    return editingMessage() && m_editDialog == dialog && m_editId == id;
}

void ChatComposer::setEditStateChanged(std::function<void()> callback)
{
    m_onEditStateChanged = std::move(callback);
}

void ChatComposer::setEditLatest(std::function<bool()> callback)
{
    m_onEditLatest = std::move(callback);
}

void ChatComposer::beginMessageEdit(const QString &dialog, const QString &id, const QString &text,
                                    std::function<void(const QString &)> submit)
{
    if (editingMessage()) {
        m_input->setFocus(Qt::OtherFocusReason);
        return;
    }
    m_preEditDraft = draft();
    m_reply = {};
    m_preEditAnchor = m_input->textCursor().anchor();
    m_preEditPosition = m_input->textCursor().position();
    m_editDialog = dialog;
    m_editId = id;
    m_editOriginal = text;
    m_submitEdit = std::move(submit);
    m_send->setGlyph(Glyph::Check);
    m_send->setAccessibleName(QStringLiteral("Сохранить сообщение"));
    m_cancelQuote->setAccessibleName(QStringLiteral("Отменить редактирование сообщения"));
    m_cancelQuote->setToolTip(QStringLiteral("Отменить редактирование (Escape)"));
    m_input->setToolTip(QStringLiteral("Enter — сохранить, Shift+Enter — новая строка, Escape — отменить"));
    setDraft({text, {}, {}});
    m_input->moveCursor(QTextCursor::End);
    m_input->setFocus(Qt::OtherFocusReason);
    updateEditBanner();
}

bool ChatComposer::canSaveMessageEdit(const QString &text) const
{
    return !m_editPending && text != m_editOriginal && !text.trimmed().isEmpty() && text.size() <= kMessageCharacters;
}

bool ChatComposer::submitMessageEdit()
{
    if (!editingMessage())
        return false;
    const auto text = wireText();
    if (m_send->isEnabled() && canSaveMessageEdit(text)) {
        const auto submit = m_submitEdit;
        m_editPending = true;
        m_editSubmitted = text;
        updateEditBanner();
        submit(text);
    }
    return true;
}

bool ChatComposer::cancelMessageEdit()
{
    if (!editingMessage())
        return false;
    if (!m_editPending)
        finishMessageEdit();
    return true;
}

void ChatComposer::finishMessageEdit()
{
    if (!editingMessage())
        return;
    m_editId.clear();
    m_editDialog.clear();
    m_editOriginal.clear();
    m_editSubmitted.clear();
    m_editPending = false;
    m_submitEdit = {};
    m_send->setGlyph(Glyph::Send);
    m_send->setAccessibleName(m_sendName);
    m_cancelQuote->setAccessibleName(QStringLiteral("Отменить ответ"));
    m_cancelQuote->setToolTip(QStringLiteral("Отменить ответ"));
    m_input->setToolTip(QStringLiteral("Enter — отправить, Shift+Enter — новая строка"));
    const auto saved = std::exchange(m_preEditDraft, {});
    setDraft(saved);
    auto cursor = m_input->textCursor();
    const int end = m_input->document()->characterCount() - 1;
    cursor.setPosition(qBound(0, m_preEditAnchor, end));
    cursor.setPosition(qBound(0, m_preEditPosition, end), QTextCursor::KeepAnchor);
    m_input->setTextCursor(cursor);
    updateEditBanner();
}

void ChatComposer::acceptMessageEdit(const QString &text)
{
    if (m_editPending && m_editSubmitted == text)
        finishMessageEdit();
}

void ChatComposer::failMessageEdit()
{
    if (!m_editPending)
        return;
    m_editPending = false;
    m_editSubmitted.clear();
    updateEditBanner();
}

void ChatComposer::setTypingActions(std::function<QString()> scope, std::function<void(const QString &)> notify)
{
    m_typingScope = std::move(scope);
    m_sendTyping = std::move(notify);
}

void ChatComposer::cancelTyping()
{
    m_typingDelay.stop();
}

void ChatComposer::setTypingIndicator(const QString &user, int duration, const QString &name)
{
    if (duration <= 0 && user != m_typingUser)
        return;
    m_typingUser = duration > 0 ? user : QString();
    if (duration > 0) {
        m_typingExpiry.start(duration);
        setTypingName(user, name);
    } else {
        m_typingExpiry.stop();
    }
    m_typing->setVisible(duration > 0);
}

void ChatComposer::setTypingName(const QString &user, const QString &name)
{
    if (m_typingUser != user || !m_typingExpiry.isActive())
        return;
    m_typing->setText(name.isEmpty() ? QStringLiteral("Печатает…") : QStringLiteral("%1 печатает…").arg(name));
}

void ChatComposer::clearTypingIndicator()
{
    setTypingIndicator(m_typingUser, 0);
}

void ChatComposer::setReply(const ChatReply &reply)
{
    if (editingMessage())
        return;
    m_reply = reply;
    const auto author = reply.author.isEmpty() ? QStringLiteral("Ответ") : QStringLiteral("Ответ · %1").arg(reply.author);
    const auto summary = QStringLiteral("%1 — %2").arg(author, reply.preview);
    m_quoteText->setText(summary);
    setLoadingIndicator(m_quoteText, false);
    m_quoteText->setAccessibleName(QStringLiteral("Цитата в черновике"));
    m_quoteText->setToolTip(plainTooltip(summary));
    m_quote->setVisible(!reply.id.isEmpty());
}

void ChatComposer::setMentionSearchHandler(std::function<void(const QString &, const QString &)> handler)
{
    dismissMentionSearch();
    m_mentionSearchEnabled = bool(handler);
    m_mentions->setSearchHandler(std::move(handler));
}

void ChatComposer::setMentionResults(const QString &id, const QVector<Person> &people)
{
    m_mentions->setResults(id, people);
}

void ChatComposer::setMentionSearchError(const QString &id)
{
    m_mentions->showRequestError(id);
}

void ChatComposer::setMentionAvatars(std::function<QIcon(const Person &)> icon, std::function<void()> onVisible)
{
    m_mentions->setAvatarIcon(std::move(icon));
    m_mentions->onVisibleChanged = std::move(onVisible);
}

QList<QUrl> ChatComposer::visibleMentionAvatarUrls() const
{
    return m_mentions->visibleAvatarUrls();
}

void ChatComposer::refreshMentionAvatars()
{
    if (m_mentions->isVisible())
        m_mentions->refreshAvatars();
}

bool ChatComposer::handleMentionKey(QKeyEvent *event)
{
    return !m_input->composing() && m_mentions->handleKey(event);
}

bool ChatComposer::handleComposerKey(QKeyEvent *event)
{
    if (handleMentionKey(event))
        return true;
    if (event->key() == Qt::Key_Up && event->modifiers() == Qt::NoModifier
        && !composing() && !editingMessage() && !m_mentions->isVisible()
        && m_input->document()->isEmpty() && m_reply.id.isEmpty() && !hasDraftImage() && m_onEditLatest)
        return m_onEditLatest();
    return event->key() == Qt::Key_Escape && cancelMessageEdit();
}

void ChatComposer::dismissMentionSearch()
{
    m_mentionCheck.stop();
    m_mentions->dismiss();
    m_candidate = {};
}

ChatDraft ChatComposer::draft() const
{
    return {m_input->toPlainText(), m_reply, mentionSpans(), m_image};
}

QString ChatComposer::wireText() const
{
    auto text = m_input->toPlainText();
    const auto mentions = mentionSpans();
    for (auto i = mentions.crbegin(); i != mentions.crend(); ++i) {
        text.replace(i->start, i->name.size() + 1, QStringLiteral("[USER=%1]%2[/USER]").arg(i->id, i->name));
    }
    return text;
}

void ChatComposer::setDraft(const ChatDraft &draft)
{
    cancelTyping();
    setTypingIndicator(m_typingUser, 0);
    dismissMentionSearch();
    m_loadingDraft = true;
    m_input->setPlainText(draft.text);
    m_hasMentions = !draft.mentions.isEmpty();
    const bool undoEnabled = m_input->isUndoRedoEnabled();
    m_input->setUndoRedoEnabled(false);
    for (const auto &mention : draft.mentions) {
        if (mention.start < 0 || draft.text.mid(mention.start, mention.name.size() + 1) != QLatin1Char('@') + mention.name)
            continue;
        QTextCursor cursor(m_input->document());
        cursor.setPosition(mention.start);
        cursor.setPosition(mention.start + mention.name.size() + 1, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(mentionFormat(mention.id, mention.name));
    }
    m_input->setUndoRedoEnabled(undoEnabled);
    m_loadingDraft = false;
    setReply(draft.reply);
    setDraftImage(draft.image);
}

void ChatComposer::dismissEmoji()
{
    if (m_panel)
        m_panel->dismiss();
}

void ChatComposer::updateEditBanner()
{
    m_cancelQuote->setEnabled(!m_editPending);
    if (editingMessage()) {
        const auto title = m_editPending ? QStringLiteral("Сохраняем сообщение…")
                                         : QStringLiteral("Редактирование сообщения");
        m_quoteText->setText(title);
        setLoadingIndicator(m_quoteText, m_editPending);
        m_quoteText->setAccessibleName(title);
        m_quoteText->setToolTip(title);
        m_quote->show();
    }
    if (m_onEditStateChanged)
        m_onEditStateChanged();
}

QTextCharFormat ChatComposer::mentionFormat(const QString &id, const QString &name)
{
    QTextCharFormat format;
    format.setForeground(workspace::Accent);
    format.setFontWeight(QFont::DemiBold);
    format.setProperty(MentionId, id);
    format.setProperty(MentionName, name);
    return format;
}

QVector<ChatMention> ChatComposer::mentionSpans() const
{
    QVector<ChatMention> result;
    if (!m_hasMentions)
        return result;
    for (auto block = m_input->document()->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            const auto format = fragment.charFormat();
            const auto id = format.stringProperty(MentionId);
            const auto name = format.stringProperty(MentionName);
            if (!id.isEmpty() && !name.isEmpty() && fragment.text() == QLatin1Char('@') + name)
                result.append({fragment.position(), id, name});
        }
    }
    return result;
}

ChatComposer::Candidate ChatComposer::currentCandidate() const
{
    const auto cursor = m_input->textCursor();
    if (m_input->isReadOnly() || m_input->composing() || cursor.hasSelection() || !m_input->hasFocus())
        return {};
    const auto block = cursor.block();
    const int vicinity = qMax(block.position(), cursor.position() - 50);
    auto prefix = cursor;
    prefix.setPosition(vicinity, QTextCursor::KeepAnchor);
    const auto before = prefix.selectedText();
    const int marker = qMax(before.lastIndexOf(QLatin1Char('@')), before.lastIndexOf(QLatin1Char('+')));
    if (marker < 0 || (marker == 0 ? vicinity != block.position() : !before[marker - 1].isSpace()))
        return {};
    const auto query = before.mid(marker + 1);
    static const QRegularExpression name(QStringLiteral("^\\p{L}[\\p{L}\\p{M}\\p{N}_.’'-]*$"));
    if (!query.isEmpty() && !name.match(query).hasMatch())
        return {};
    const int start = vicinity + marker;
    QTextCursor trigger(m_input->document());
    trigger.setPosition(start + 1);
    if (trigger.charFormat().hasProperty(MentionId))
        return {};
    return {start, cursor.position(), query};
}

void ChatComposer::scheduleMentionSearch()
{
    if (m_mentionSearchEnabled && !m_loadingDraft && !m_mentionCheck.isActive())
        m_mentionCheck.start(0);
}

void ChatComposer::insertMention(const Person &person)
{
    const auto found = currentCandidate();
    const auto id = portalTaskIdentifier(person.id);
    if (found.start < 0 || !(found == m_candidate) || id.isEmpty())
        return;
    auto name = person.name.simplified().left(96);
    name.remove(QLatin1Char('[')).remove(QLatin1Char(']'));
    name = name.simplified();
    if (name.isEmpty())
        return;
    auto cursor = m_input->textCursor();
    cursor.setPosition(found.start);
    cursor.setPosition(found.end, QTextCursor::KeepAnchor);
    m_hasMentions = true;
    cursor.beginEditBlock();
    cursor.insertText(QLatin1Char('@') + name, mentionFormat(id, name));
    cursor.insertText(QStringLiteral(" "), QTextCharFormat{});
    cursor.endEditBlock();
    m_input->setTextCursor(cursor);
    m_input->setFocus(Qt::OtherFocusReason);
    dismissMentionSearch();
}

QString ChatComposer::pastedImageName()
{
    return QStringLiteral("Из буфера.png");
}

void ChatComposer::stageImage(const QImage &image)
{
    const auto failure = [this](const QString &message) {
        if (m_onImageError)
            m_onImageError(message);
    };
    if (image.isNull() || !validChatClipboardImageSize(image.size())) {
        failure(QStringLiteral("Не удалось вставить изображение. Размер или формат не поддерживается."));
        return;
    }
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, "png");
    writer.setCompression(60);
    const auto source = image.convertToFormat(image.hasAlphaChannel() ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    if (source.isNull() || !writer.write(source)) {
        failure(QStringLiteral("Не удалось подготовить изображение для отправки"));
        return;
    }
    if (png.size() > tunables::kChatClipboardPngBytes) {
        failure(QStringLiteral("Изображение после сжатия превышает 16 МиБ"));
        return;
    }
    auto directory = std::make_shared<QTemporaryDir>(QDir::temp().filePath(QStringLiteral("bitrix24-chat-XXXXXX")));
    const auto path = directory->filePath(pastedImageName());
    QFile output(path);
    if (!directory->isValid() || !output.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        || output.write(png) != png.size() || !output.flush()) {
        failure(QStringLiteral("Не удалось подготовить изображение для отправки"));
        return;
    }
    output.close();
    auto thumbnail = source.scaled(m_imagePreview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    thumbnail.setDevicePixelRatio(1);
    setDraftImage(std::make_shared<ChatDraftImage>(ChatDraftImage{std::move(directory), path, thumbnail}));
}

void ChatComposer::setDraftImage(std::shared_ptr<const ChatDraftImage> image)
{
    if (m_image == image)
        return;
    m_image = std::move(image);
    m_imagePreview->setPixmap(m_image ? QPixmap::fromImage(m_image->thumbnail) : QPixmap{});
    m_imageRow->setVisible(bool(m_image));
    if (m_onImageChanged)
        m_onImageChanged();
}

bool ChatComposer::Candidate::operator==(const Candidate &other) const
{
    return start == other.start && end == other.end && query == other.query;
}

} // namespace bx::ui
