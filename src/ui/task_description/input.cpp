#include "ui/task_description/input.h"

#include "ui/task_description/safe_document.h"
#include "ui/task_description/bb_code.h"
#include "ui/task_description/decoder.h"
#include "ui/message_markup.h"

#include <QFontMetrics>
#include <QMimeData>
#include <QPainter>
#include <QTextBlock>
#include <QTextList>
#include <QUrl>

namespace bx::ui::task_description {

Input::Input(QWidget *parent)
    : QTextEdit(parent)
{
    setDocument(new SafeDocument(this));
    setAcceptRichText(true);
}

void Input::load(const QString &description)
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

QString Input::bbCode() const
{
    return encode(*document(), 0, document()->characterCount() - 1, false);
}

void Input::attach(const QString &raw, const QString &name)
{
    auto cursor = m_uploadCursor.isNull() ? textCursor() : m_uploadCursor;
    cursor.clearSelection();
    cursor.insertHtml(tokenHtml(raw, name));
    if (m_uploadCursor.isNull())
        setTextCursor(cursor);
    else
        m_uploadCursor = cursor;
}

void Input::requestFiles(const QStringList &paths)
{
    if (!uploadEnabled || !filesPasted || paths.isEmpty())
        return;
    holdUploadCursor();
    filesPasted(paths);
}

QMimeData *Input::createMimeDataFromSelection() const
{
    auto *result = new QMimeData;
    const auto cursor = textCursor();
    const auto source = encode(*document(), cursor.selectionStart(), cursor.selectionEnd(), false);
    result->setData(ClipboardType, source.toUtf8());
    result->setText(encode(*document(), cursor.selectionStart(), cursor.selectionEnd(), true));
    return result;
}

bool Input::canInsertFromMimeData(const QMimeData *mime) const
{
    return mime->hasFormat(ClipboardType) || mime->hasText() || mime->hasHtml()
        || (uploadEnabled && (mime->hasUrls() || mime->hasImage()));
}

void Input::insertFromMimeData(const QMimeData *mime)
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

void Input::holdUploadCursor()
{
    m_uploadCursor = textCursor();
    m_uploadCursor.clearSelection();
    m_uploadCursor.setKeepPositionOnInsert(true);
}

QString Input::decode(const QString &source)
{
    return Decoder(source, [this](const QString &raw, const QString &label) {
        return tokenHtml(raw, label);
    }).html();
}

QString Input::tokenHtml(const QString &raw, const QString &label)
{
    const auto url = QUrl(u"%1:/%2"_s.arg(TokenScheme).arg(m_tokens.size()));
    m_tokens.append({raw, label});
    const auto chip = tokenImage(label);
    document()->addResource(QTextDocument::ImageResource, url, chip);
    return u"<img src=\"%1\" width=\"%2\" height=\"%3\">"_s.arg(url.toString()).arg(chip.width()).arg(chip.height());
}

QImage Input::tokenImage(const QString &label)
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

QString Input::chipText(const QTextCharFormat &format, bool plain) const
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

bool Input::Style::operator==(const Style &other) const
{
    return open == other.open;
}

QList<Input::Style> Input::styles(const QTextCharFormat &format)
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

void Input::changeStyle(QString &output, QList<Style> &current, const QList<Style> &desired)
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

QString Input::encode(const QTextDocument &document, int start, int end, bool plain) const
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

} // namespace bx::ui::task_description
