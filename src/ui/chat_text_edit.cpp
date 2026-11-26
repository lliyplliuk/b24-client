#include "ui/chat_text_edit.h"

#include "core/tunables.h"
#include <QBuffer>
#include <QImageReader>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QPixmap>
#include <QResizeEvent>
#include <QTextBlock>
#include <QTextLayout>
#include <QTimer>
#include <QtMath>

namespace bx::ui {

bool validChatClipboardImageSize(const QSize &size)
{
    constexpr qint64 maxPixels = 12 * 1024 * 1024;
    return !size.isEmpty() && size.width() <= 8192 && size.height() <= 8192
        && qint64(size.width()) * size.height() <= maxPixels;
}

ChatTextEdit::ChatTextEdit(QWidget *parent)
    : QPlainTextEdit(parent)
{
    setFixedHeight(singleLineHeight);
    connect(this, &QPlainTextEdit::textChanged, this, [this] { scheduleHeightUpdate(); });
}

bool ChatTextEdit::composing() const
{
    return m_preedit;
}

void ChatTextEdit::setSingleLinePlaceholder(const QString &text)
{
    m_fullPlaceholder = text;
    updatePlaceholder();
}

bool ChatTextEdit::canInsertFromMimeData(const QMimeData *source) const
{
    const bool image = source->hasImage() || source->hasFormat(QStringLiteral("image/png"));
    return (acceptsImagePaste() && image) || QPlainTextEdit::canInsertFromMimeData(source);
}

void ChatTextEdit::insertFromMimeData(const QMimeData *source)
{
    const bool rawPng = source->hasFormat(QStringLiteral("image/png"));
    if (acceptsImagePaste() && (rawPng || source->hasImage())) {
        QImage image;
        if (rawPng) {
            const auto bytes = source->data(QStringLiteral("image/png"));
            if (!bytes.isEmpty() && bytes.size() <= tunables::kChatClipboardPngBytes) {
                QBuffer buffer;
                buffer.setData(bytes);
                buffer.open(QIODevice::ReadOnly);
                QImageReader reader(&buffer, "png");
                if (validChatClipboardImageSize(reader.size()))
                    image = reader.read();
            }
        } else {
            const auto data = source->imageData();
            image = qvariant_cast<QImage>(data);
            if (image.isNull())
                image = qvariant_cast<QPixmap>(data).toImage();
        }
        if (!image.isNull() || !(source->hasText() || source->hasHtml())) {
            imagePasted(image);
            return;
        }
    }
    QPlainTextEdit::insertFromMimeData(source);
}

void ChatTextEdit::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    updatePlaceholder();
    if (event->size().width() != event->oldSize().width())
        scheduleHeightUpdate();
}

void ChatTextEdit::changeEvent(QEvent *event)
{
    QPlainTextEdit::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        updatePlaceholder();
        scheduleHeightUpdate();
    }
}

void ChatTextEdit::keyPressEvent(QKeyEvent *event)
{
    const auto revision = document()->revision();
    QPlainTextEdit::keyPressEvent(event);
    if (userEdited && !m_preedit && !document()->isEmpty() && document()->revision() != revision)
        userEdited();
}

void ChatTextEdit::inputMethodEvent(QInputMethodEvent *event)
{
    m_preedit = !event->preeditString().isEmpty();
    const auto revision = document()->revision();
    QPlainTextEdit::inputMethodEvent(event);
    if (userEdited && !event->commitString().isEmpty() && !document()->isEmpty()
        && document()->revision() != revision)
        userEdited();
}

bool ChatTextEdit::acceptsImagePaste() const
{
    return !isReadOnly() && imagePasted && mayPasteImage && mayPasteImage();
}

void ChatTextEdit::scheduleHeightUpdate()
{
    if (m_heightUpdatePending)
        return;
    m_heightUpdatePending = true;
    QTimer::singleShot(0, this, [this] {
        m_heightUpdatePending = false;
        int lines = 0;
        for (auto block = document()->begin(); block.isValid() && lines < maximumVisibleLines; block = block.next()) {
            blockBoundingRect(block);
            lines += qMax(1, block.layout()->lineCount());
        }
        const int target = singleLineHeight + (qBound(1, lines, maximumVisibleLines) - 1) * fontMetrics().lineSpacing();
        if (target != minimumHeight())
            setFixedHeight(target);
    });
}

void ChatTextEdit::updatePlaceholder()
{
    const int width = qMax(0, qFloor(viewport()->width() - 2 * document()->documentMargin()));
    setPlaceholderText(fontMetrics().elidedText(m_fullPlaceholder, Qt::ElideRight, width));
}

} // namespace bx::ui
