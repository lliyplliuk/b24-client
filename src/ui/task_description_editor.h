#pragma once

#include "ui/loading_indicator.h"
#include "ui/message_markup.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFontMetrics>
#include <QHash>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QTextBlock>
#include <QTextEdit>
#include <QTextList>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>

namespace bx::ui {
namespace task_description {

using namespace Qt::StringLiterals;

inline const QString ClipboardType = u"application/x-bitrix24-task-description"_s;
inline const QString TokenScheme = u"bx-task-description-token"_s;

class SafeDocument final : public QTextDocument {
public:
    using QTextDocument::QTextDocument;

protected:
    QVariant loadResource(int, const QUrl &) override { return {}; }
};

inline QString htmlText(QString text)
{
    return text.toHtmlEscaped().replace(u"\r\n"_s, u"\n"_s).replace(u'\r', u'\n')
        .replace(u'\n', u"<br>"_s);
}

inline bool isDiskId(const QString &id)
{
    static const QRegularExpression pattern(u"^n?[1-9][0-9]{0,18}$"_s, QRegularExpression::CaseInsensitiveOption);
    return pattern.match(id).hasMatch();
}

inline QString diskReference(const QString &id, const QSize &size)
{
    auto result = u"[disk file id=%1"_s.arg(id);
    if (size.isValid() && !size.isEmpty())
        result += u" width=%1 height=%2"_s.arg(size.width()).arg(size.height());
    return result + u']';
}

struct Mark {
    qsizetype start;
    qsizetype end;
    QString name;
    QString argument;
    bool close;
    qsizetype pair = -1;
};

inline QList<Mark> marks(const QString &source)
{
    static const QRegularExpression syntax(uR"(\[(/?)([a-z][a-z0-9]*|\*)([ =][^\[\]\r\n]{0,2048})?\])"_s,
        QRegularExpression::CaseInsensitiveOption);
    QList<Mark> result;
    QList<qsizetype> stack;
    auto matches = syntax.globalMatch(source);
    while (matches.hasNext() && result.size() < 16384) {
        const auto match = matches.next();
        Mark mark{match.capturedStart(), match.capturedEnd(), match.captured(2).toLower(),
            match.captured(3), !match.capturedView(1).isEmpty()};
        const auto &name = mark.name;
        const auto &arg = mark.argument;
        const bool simple = name == u"b" || name == u"i" || name == u"u" || name == u"s" || name == u"p"
            || name == u"code" || name == u"icode";
        const bool paired = (simple && arg.isEmpty()) || (name == u"url" && (arg.isEmpty() || arg.startsWith(u'=')))
            || name == u"list";
        const bool standalone = !mark.close && (((name == u"*" || name == u"br") && arg.isEmpty()) || name == u"disk");
        if ((!standalone && !paired) || (mark.close && !arg.isEmpty()))
            continue;
        const bool insideCode = !stack.isEmpty()
            && (result[stack.constLast()].name == u"code" || result[stack.constLast()].name == u"icode");
        if (insideCode && (!mark.close || result[stack.constLast()].name != name))
            continue;
        if (mark.close) {
            if (!stack.isEmpty() && result[stack.constLast()].name == name)
                result[stack.takeLast()].pair = result.size();
        } else if (paired) {
            stack.append(result.size());
        }
        result.append(std::move(mark));
    }
    return result;
}

class Decoder {
public:
    using TokenHtml = std::function<QString(const QString &raw, const QString &label)>;

    Decoder(const QString &source, TokenHtml renderToken)
        : m_source(source), m_marks(marks(source)), m_token(std::move(renderToken)) {}

    QString html() const
    {
        return u"<body style=\"white-space:pre-wrap;\">"_s + range(0, m_marks.size(), 0, m_source.size(), 0)
            + u"</body>"_s;
    }

private:
    const QString &m_source;
    const QList<Mark> m_marks;
    const TokenHtml m_token;

    QString text(qsizetype start, qsizetype end) const { return m_source.mid(start, end - start); }

    QString range(qsizetype first, qsizetype last, qsizetype start, qsizetype end, int depth) const
    {
        if (depth >= 16)
            return htmlText(text(start, end));
        QString result;
        for (auto i = first; i < last; ++i) {
            const auto &mark = m_marks[i];
            result += htmlText(text(start, mark.start));
            start = mark.end;
            const auto raw = text(mark.start, mark.end);
            if (!mark.close && mark.name == u"disk") {
                result += disk(raw, mark.argument);
            } else if (!mark.close && mark.name == u"br") {
                result += u"<br>";
            } else if (!mark.close && mark.pair > i && mark.pair < last) {
                const auto inner = pair(i, depth + 1);
                if (inner.isNull()) {
                    result += htmlText(raw);
                    continue;
                }
                result += inner;
                i = mark.pair;
                start = m_marks[i].end;
            } else {
                result += htmlText(raw);
            }
        }
        return result + htmlText(text(start, end));
    }

    QString pair(qsizetype index, int depth) const
    {
        const auto &open = m_marks[index];
        const auto &close = m_marks[open.pair];
        if (open.name == u"code" || open.name == u"icode"
            || (open.name == u"list" && !open.argument.isEmpty() && open.argument != u"=1"))
            return htmlText(text(open.start, close.end));
        if (open.name == u"list")
            return list(index, depth);
        const auto body = range(index + 1, open.pair, open.end, close.start, depth);
        if (open.name == u"url") {
            const auto address = open.argument.isEmpty() ? text(open.end, close.start).trimmed() : open.argument.mid(1);
            const auto target = workspace::messageLinkTarget(address);
            if (target.isEmpty())
                return {};
            return u"<a href=\"%1\">%2</a>"_s.arg(target.toHtmlEscaped(), body.isEmpty() ? htmlText(address) : body);
        }
        return u"<"_s + open.name + u">"_s + body + u"</"_s + open.name + u">"_s;
    }

    QString list(qsizetype index, int depth) const
    {
        const auto &open = m_marks[index];
        QList<qsizetype> items;
        for (auto i = index + 1; i < open.pair; ++i) {
            if (m_marks[i].name == u"*")
                items.append(i);
            else if (m_marks[i].pair > i)
                i = m_marks[i].pair;
        }
        if (items.isEmpty())
            return {};
        const auto lead = text(open.end, m_marks[items.constFirst()].start);
        if (!lead.trimmed().isEmpty())
            return {};
        const auto tag = open.argument == u"=1" ? u"ol"_s : u"ul"_s;
        QString result = u"<"_s + tag + u">"_s;
        for (qsizetype i = 0; i < items.size(); ++i) {
            const auto itemIndex = items[i];
            const auto stop = i + 1 < items.size() ? items[i + 1] : open.pair;
            result += u"<li>"_s + range(itemIndex + 1, stop, m_marks[itemIndex].end, m_marks[stop].start, depth) + u"</li>"_s;
        }
        return result + u"</"_s + tag + u">"_s;
    }

    QString disk(const QString &raw, const QString &argument) const
    {
        static const QRegularExpression fileId(
            uR"(^\s+file\s+id=(n?[1-9][0-9]{0,18})(?:\s|$))"_s, QRegularExpression::CaseInsensitiveOption);
        const auto id = argument.startsWith(u'=') ? argument.mid(1) : fileId.match(argument).captured(1);
        return isDiskId(id) ? m_token(raw, QObject::tr("Файл %1").arg(id)) : htmlText(raw);
    }
};

inline QString literalText(QString text)
{
    return text.replace(QChar::LineSeparator, u'\n').replace(QChar::Nbsp, u' ');
}

class Input final : public QTextEdit {
public:
    struct Token {
        QString raw;
        QString label;
    };

    explicit Input(QWidget *parent) : QTextEdit(parent)
    {
        setDocument(new SafeDocument(this));
        setAcceptRichText(true);
    }

    void load(const QString &description)
    {
        m_uploadCursor = {};
        auto *previous = document();
        auto *fresh = new SafeDocument(this);
        fresh->setDefaultFont(previous->defaultFont());
        setDocument(fresh);
        m_tokens.clear();
        m_chips.clear();
        fresh->setHtml(decode(description));
        fresh->setModified(false);
    }

    QString bbCode() const
    {
        return encode(*document(), 0, document()->characterCount() - 1, false);
    }

    void attach(const QString &raw, const QString &name)
    {
        auto cursor = m_uploadCursor.isNull() ? textCursor() : m_uploadCursor;
        cursor.clearSelection();
        cursor.insertHtml(tokenHtml(raw, name));
        if (m_uploadCursor.isNull())
            setTextCursor(cursor);
        else
            m_uploadCursor = cursor;
    }

    void requestFiles(const QStringList &paths)
    {
        if (!uploadEnabled || !filesPasted || paths.isEmpty())
            return;
        holdUploadCursor();
        filesPasted(paths);
    }

    std::function<void(const QStringList &)> filesPasted;
    std::function<void(const QImage &)> imagePasted;
    bool uploadEnabled = true;

protected:
    QMimeData *createMimeDataFromSelection() const override
    {
        auto *result = new QMimeData;
        const auto cursor = textCursor();
        const auto source = encode(*document(), cursor.selectionStart(), cursor.selectionEnd(), false);
        result->setData(ClipboardType, source.toUtf8());
        result->setText(encode(*document(), cursor.selectionStart(), cursor.selectionEnd(), true));
        return result;
    }

    bool canInsertFromMimeData(const QMimeData *mime) const override
    {
        return mime->hasFormat(ClipboardType) || mime->hasText() || mime->hasHtml()
            || (uploadEnabled && (mime->hasUrls() || mime->hasImage()));
    }

    void insertFromMimeData(const QMimeData *mime) override
    {
        if (isReadOnly())
            return;
        if (mime->hasFormat(ClipboardType)) {
            insertHtml(decode(QString::fromUtf8(mime->data(ClipboardType))));
            return;
        }
        if (mime->hasUrls()) {
            QStringList paths;
            for (const auto &url : mime->urls()) {
                if (url.isLocalFile())
                    paths.append(url.toLocalFile());
            }
            if (!paths.isEmpty()) {
                requestFiles(paths);
                return;
            }
        }
        if (mime->hasImage()) {
            if (uploadEnabled && imagePasted) {
                const auto image = qvariant_cast<QImage>(mime->imageData());
                if (!image.isNull()) {
                    holdUploadCursor();
                    imagePasted(image);
                }
            }
            return;
        }
        if (mime->hasHtml()) {
            SafeDocument clipboard;
            clipboard.setHtml(mime->html());
            // Only the formatting represented in Bitrix BBCode comes into the live document.
            insertHtml(decode(encode(clipboard, 0, clipboard.characterCount() - 1, false)));
        } else if (mime->hasText()) {
            insertPlainText(mime->text());
        }
    }

private:
    QList<Token> m_tokens;
    QHash<QString, QImage> m_chips;
    QTextCursor m_uploadCursor;

    void holdUploadCursor()
    {
        m_uploadCursor = textCursor();
        m_uploadCursor.clearSelection();
        m_uploadCursor.setKeepPositionOnInsert(true);
    }

    QString decode(const QString &source)
    {
        return Decoder(source, [this](const QString &raw, const QString &label) {
            return tokenHtml(raw, label);
        }).html();
    }

    QString tokenHtml(const QString &raw, const QString &label)
    {
        const auto url = QUrl(u"%1:/%2"_s.arg(TokenScheme).arg(m_tokens.size()));
        m_tokens.append({raw, label});
        const auto chip = tokenImage(label);
        document()->addResource(QTextDocument::ImageResource, url, chip);
        return u"<img src=\"%1\" width=\"%2\" height=\"%3\">"_s.arg(url.toString()).arg(chip.width()).arg(chip.height());
    }

    QImage tokenImage(const QString &label)
    {
        const auto visualLabel = m_chips.size() >= 128 && !m_chips.contains(label) ? tr("Файл") : label;
        const auto existing = m_chips.constFind(visualLabel);
        if (existing != m_chips.cend())
            return existing.value();
        auto font = document()->defaultFont();
        font.setWeight(QFont::DemiBold);
        const QFontMetrics fontMetrics(font);
        const auto shortLabel = fontMetrics.elidedText(visualLabel, Qt::ElideRight, 250);
        const QSize size(fontMetrics.horizontalAdvance(shortLabel) + 16, fontMetrics.height() + 6);
        QImage chip(size, QImage::Format_ARGB32_Premultiplied);
        chip.fill(Qt::transparent);
        QPainter painter(&chip);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(u"#e1eff0"_s));
        painter.drawRoundedRect(chip.rect().adjusted(0, 0, -1, -1), 5, 5);
        painter.setPen(QColor(u"#215c70"_s));
        painter.setFont(font);
        painter.drawText(chip.rect(), Qt::AlignCenter, shortLabel);
        painter.end();
        m_chips.insert(visualLabel, chip);
        return chip;
    }

    QString chipText(const QTextCharFormat &format, bool plain) const
    {
        const QUrl url(format.toImageFormat().name());
        if (url.scheme() != TokenScheme)
            return {};
        bool ok = false;
        const auto index = url.path().mid(1).toLongLong(&ok);
        if (!ok || index < 0 || index >= m_tokens.size())
            return {};
        return plain ? m_tokens[index].label : m_tokens[index].raw;
    }

    struct Style {
        QString open;
        QString close;
        bool operator==(const Style &other) const { return open == other.open; }
    };

    static QList<Style> styles(const QTextCharFormat &format)
    {
        QList<Style> result;
        const auto link = format.isAnchor() ? workspace::messageLinkTarget(format.anchorHref()) : QString{};
        if (!link.isEmpty())
            result.append({u"[url=%1]"_s.arg(link), u"[/url]"_s});
        if (format.fontWeight() >= QFont::DemiBold)
            result.append({u"[b]"_s, u"[/b]"_s});
        if (format.fontItalic())
            result.append({u"[i]"_s, u"[/i]"_s});
        if (format.fontUnderline() && !format.isAnchor())
            result.append({u"[u]"_s, u"[/u]"_s});
        if (format.fontStrikeOut())
            result.append({u"[s]"_s, u"[/s]"_s});
        return result;
    }

    // Keep common tags open across consecutive QText fragments (for instance bold around an italic word).
    static void changeStyle(QString &output, QList<Style> &current, const QList<Style> &desired)
    {
        qsizetype same = 0;
        while (same < qMin(current.size(), desired.size()) && current[same] == desired[same])
            ++same;
        for (auto i = current.size(); i > same; --i)
            output += current[i - 1].close;
        for (auto i = same; i < desired.size(); ++i)
            output += desired[i].open;
        current = desired;
    }

    QString encode(const QTextDocument &document, int start, int end, bool plain) const
    {
        const bool own = &document == this->document();
        QString result;
        QList<const QTextList *> activeLists;
        bool first = true;
        for (auto block = document.findBlock(start); block.isValid() && block.position() <= end; block = block.next()) {
            const auto *list = plain ? nullptr : block.textList();
            if (list) {
                const int indent = qMax(1, list->format().indent());
                while (!activeLists.isEmpty() && activeLists.constLast() != list
                    && activeLists.constLast()->format().indent() >= indent) {
                    result += u"[/list]";
                    activeLists.removeLast();
                }
                if (activeLists.isEmpty() || activeLists.constLast() != list) {
                    if (activeLists.isEmpty() && !first)
                        result += u'\n';
                    const auto type = list->format().style();
                    result += type == QTextListFormat::ListDecimal ? u"[list=1]"_s : u"[list]"_s;
                    activeLists.append(list);
                }
                result += u"[*]";
            } else {
                while (!activeLists.isEmpty()) {
                    result += u"[/list]";
                    activeLists.removeLast();
                }
                if (!first)
                    result += u'\n';
            }
            first = false;
            QList<Style> current;
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const auto fragment = it.fragment();
                const auto from = qMax(start, fragment.position());
                const auto to = qMin(end, fragment.position() + fragment.length());
                if (!fragment.isValid() || to <= from)
                    continue;
                const auto format = fragment.charFormat();
                if (format.isImageFormat()) {
                    changeStyle(result, current, {});
                    if (own)
                        result += chipText(format, plain);
                } else {
                    if (!plain)
                        changeStyle(result, current, styles(format));
                    result += literalText(fragment.text().mid(from - fragment.position(), to - from));
                }
            }
            changeStyle(result, current, {});
        }
        for (qsizetype i = 0; i < activeLists.size(); ++i)
            result += u"[/list]";
        return result;
    }
};

} // namespace task_description

class TaskDescriptionEditor final : public QWidget {
public:
    explicit TaskDescriptionEditor(QWidget *parent = nullptr) : QWidget(parent)
    {
        using namespace Qt::StringLiterals;
        setObjectName(u"taskDescriptionEditor"_s);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(5);
        m_tools = new QWidget(this);
        m_tools->setObjectName(u"taskDescriptionTools"_s);
        m_tools->setStyleSheet(u"#taskDescriptionTools {background:#edf3f2;border:1px solid #dce4e4;border-radius:8px;}"
            "QToolButton {padding:4px 8px;color:#215c70;background:transparent;border-radius:5px;border:1px solid transparent;}"
            "QToolButton:hover {background:#e1eff0;} QToolButton:checked {background:#dcecf0;border-color:#aec9ce;}"
            "QToolButton:disabled {color:#81929b;}"_s);
        m_toolRow = new QHBoxLayout(m_tools);
        m_toolRow->setContentsMargins(5, 4, 5, 4);
        m_toolRow->setSpacing(3);
        layout->addWidget(m_tools);
        m_input = new task_description::Input(this);
        m_input->setObjectName(u"taskDescriptionInput"_s);
        m_input->setAccessibleName(tr("Описание задачи"));
        m_input->setPlaceholderText(tr("Добавьте описание задачи"));
        m_input->setMinimumHeight(140);
        layout->addWidget(m_input, 1);
        m_upload = new QLabel(this);
        m_upload->setTextFormat(Qt::PlainText);
        m_upload->setWordWrap(true);
        m_upload->setObjectName(u"taskDescriptionUploadStatus"_s);
        m_upload->hide();
        layout->addWidget(m_upload);

        m_bold = tool(u"B"_s, tr("Полужирный"), true);
        m_italic = tool(u"I"_s, tr("Курсив"), true);
        m_underline = tool(u"U"_s, tr("Подчёркнутый"), true);
        m_strike = tool(u"S"_s, tr("Зачёркнутый"), true);
        auto font = m_bold->font();
        font.setBold(true);
        m_bold->setFont(font);
        font = m_italic->font();
        font.setItalic(true);
        m_italic->setFont(font);
        font = m_underline->font();
        font.setUnderline(true);
        m_underline->setFont(font);
        font = m_strike->font();
        font.setStrikeOut(true);
        m_strike->setFont(font);

        connect(m_bold, &QToolButton::clicked, this, [this](bool active) {
            QTextCharFormat format;
            format.setFontWeight(active ? QFont::Bold : QFont::Normal);
            apply(format);
        });
        connect(m_italic, &QToolButton::clicked, this, [this](bool active) {
            QTextCharFormat format;
            format.setFontItalic(active);
            apply(format);
        });
        connect(m_underline, &QToolButton::clicked, this, [this](bool active) {
            QTextCharFormat format;
            format.setFontUnderline(active);
            apply(format);
        });
        connect(m_strike, &QToolButton::clicked, this, [this](bool active) {
            QTextCharFormat format;
            format.setFontStrikeOut(active);
            apply(format);
        });
        bindShortcut(QKeySequence::Bold, m_bold);
        bindShortcut(QKeySequence::Italic, m_italic);
        bindShortcut(QKeySequence::Underline, m_underline);
        m_bullets = tool(u"•"_s, tr("Маркированный список"), true);
        m_numbers = tool(u"1."_s, tr("Нумерованный список"), true);
        connect(m_bullets, &QToolButton::clicked, this, [this](bool active) { formatList(QTextListFormat::ListDisc, active); });
        connect(m_numbers, &QToolButton::clicked, this, [this](bool active) { formatList(QTextListFormat::ListDecimal, active); });
        m_toolRow->addStretch();
        auto *link = tool(tr("Ссылка"), tr("Вставить ссылку"));
        connect(link, &QToolButton::clicked, this, [this] { insertLink(); });
        m_attach = tool(tr("Файл"), tr("Прикрепить файл"));
        connect(m_attach, &QToolButton::clicked, this, [this] {
            auto *dialog = new QFileDialog(this, tr("Выберите файлы"));
            dialog->setFileMode(QFileDialog::ExistingFiles);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            connect(dialog, &QFileDialog::filesSelected, m_input, &task_description::Input::requestFiles);
            dialog->open();
        });

        connect(m_input, &QTextEdit::currentCharFormatChanged, this, [this](const QTextCharFormat &format) {
            m_bold->setChecked(format.fontWeight() >= QFont::DemiBold);
            m_italic->setChecked(format.fontItalic());
            m_underline->setChecked(format.fontUnderline() && !format.isAnchor());
            m_strike->setChecked(format.fontStrikeOut());
        });
        connect(m_input, &QTextEdit::cursorPositionChanged, this, [this] {
            const auto *list = m_input->textCursor().currentList();
            const auto style = list ? list->format().style() : QTextListFormat::ListStyleUndefined;
            m_bullets->setChecked(style == QTextListFormat::ListDisc);
            m_numbers->setChecked(style == QTextListFormat::ListDecimal);
        });
        refreshUpload();
    }

    void setBbCode(const QString &description)
    {
        m_original = description;
        m_input->load(description);
    }

    void setCompactHeight()
    {
        m_input->setMinimumHeight(64);
        m_input->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    }

    QString bbCode() const
    {
        return m_input->document()->isModified() ? m_input->bbCode() : m_original;
    }

    void insertAttachment(const QString &diskId, const QString &name = {}, const QSize &imageSize = {})
    {
        if (task_description::isDiskId(diskId))
            m_input->attach(task_description::diskReference(diskId, imageSize),
                            name.isEmpty() ? tr("Файл %1").arg(diskId) : name.simplified());
    }

    void setEditEnabled(bool enabled)
    {
        m_input->setReadOnly(!enabled);
        m_tools->setEnabled(enabled);
        refreshUpload();
    }

    void setUploadStatus(const QString &status, bool loading = false)
    {
        m_upload->setText(status);
        setLoadingIndicator(m_upload, loading);
        m_upload->setVisible(!status.isEmpty());
    }

    void setUploadEnabled(bool enabled)
    {
        m_allowUpload = enabled;
        refreshUpload();
    }

    void setAttachHandler(std::function<void(const QStringList &)> handler)
    {
        m_input->filesPasted = std::move(handler);
        refreshUpload();
    }

    void setImagePasteHandler(std::function<void(const QImage &)> handler)
    {
        m_input->imagePasted = std::move(handler);
        refreshUpload();
    }

private:
    task_description::Input *m_input = nullptr;
    QWidget *m_tools = nullptr;
    QHBoxLayout *m_toolRow = nullptr;
    QToolButton *m_bold = nullptr;
    QToolButton *m_italic = nullptr;
    QToolButton *m_underline = nullptr;
    QToolButton *m_strike = nullptr;
    QToolButton *m_bullets = nullptr;
    QToolButton *m_numbers = nullptr;
    QToolButton *m_attach = nullptr;
    QLabel *m_upload = nullptr;
    QString m_original;
    bool m_allowUpload = true;

    QToolButton *tool(const QString &title, const QString &accessible, bool checkable = false)
    {
        auto *button = new QToolButton(m_tools);
        button->setText(title);
        button->setAccessibleName(accessible);
        button->setToolTip(accessible);
        button->setCheckable(checkable);
        m_toolRow->addWidget(button);
        return button;
    }

    void bindShortcut(QKeySequence::StandardKey key, QToolButton *button)
    {
        auto *shortcut = new QShortcut(QKeySequence(key), m_input);
        shortcut->setContext(Qt::WidgetShortcut);
        connect(shortcut, &QShortcut::activated, button, &QToolButton::click);
    }

    void refreshUpload()
    {
        const bool allowed = m_allowUpload && !m_input->isReadOnly();
        m_input->uploadEnabled = allowed && (m_input->filesPasted || m_input->imagePasted);
        m_attach->setEnabled(allowed && bool(m_input->filesPasted));
    }

    void apply(const QTextCharFormat &format)
    {
        m_input->mergeCurrentCharFormat(format);
        m_input->setFocus();
    }

    void formatList(QTextListFormat::Style style, bool active)
    {
        auto cursor = m_input->textCursor();
        if (active) {
            QTextListFormat format;
            format.setStyle(style);
            format.setIndent(1);
            cursor.createList(format);
        } else {
            const int end = cursor.selectionEnd();
            const bool hasSelection = cursor.hasSelection();
            for (auto block = m_input->document()->findBlock(cursor.selectionStart());
                 block.isValid() && (hasSelection ? block.position() < end : block.position() <= end); block = block.next()) {
                if (auto *list = block.textList())
                    list->remove(block);
            }
        }
        m_input->setFocus();
    }

    void insertLink()
    {
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Вставить ссылку"));
        auto *form = new QVBoxLayout(&dialog);
        auto *label = new QLabel(tr("Адрес ссылки"), &dialog);
        auto *address = new QLineEdit(&dialog);
        address->setPlaceholderText(QStringLiteral("https://"));
        label->setBuddy(address);
        form->addWidget(label);
        form->addWidget(address);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        const auto current = m_input->textCursor().charFormat().anchorHref();
        address->setText(current);
        auto *accept = buttons->button(QDialogButtonBox::Ok);
        accept->setEnabled(!workspace::messageLinkTarget(current).isEmpty());
        connect(address, &QLineEdit::textChanged, accept, [accept](const QString &value) {
            accept->setEnabled(!workspace::messageLinkTarget(value).isEmpty());
        });
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        form->addWidget(buttons);
        dialog.setMinimumWidth(360);
        if (dialog.exec() != QDialog::Accepted)
            return;
        const auto target = workspace::messageLinkTarget(address->text());
        QTextCharFormat format;
        format.setAnchor(true);
        format.setAnchorHref(target);
        format.setFontUnderline(true);
        format.setForeground(QColor(QStringLiteral("#215c70")));
        auto cursor = m_input->textCursor();
        if (cursor.hasSelection()) {
            cursor.mergeCharFormat(format);
        } else {
            cursor.insertText(target, format);
            m_input->setTextCursor(cursor);
            m_input->setCurrentCharFormat(QTextCharFormat{});
        }
        m_input->setFocus();
    }
};

} // namespace bx::ui
