#include "ui/row_painter.h"
#include "ui/row_paint_primitives.h"
#include "ui/message_emoji.h"

#include <QAbstractTextDocumentLayout>
#include <QListWidget>
#include <QtMath>

namespace bx::workspace {

std::optional<RowPainter::TextSelection> RowPainter::snapshotTextSelection() const
{
    const auto id = m_textIndex.data(ReceiptMessageId).toString();
    if (id.isEmpty())
        return std::nullopt;
    return TextSelection{id, m_textIndex.data(Detail).toString(), m_textIndex.data(MarkupHtml).toString(),
                         m_textAnchor, m_textPosition, m_textVerticalX};
}

void RowPainter::restoreTextSelection(const QModelIndex &index, const TextSelection &selection)
{
    if (index.data(Detail).toString() != selection.detail || index.data(MarkupHtml).toString() != selection.markup)
        return;
    m_textIndex = index;
    m_textAnchor = selection.anchor;
    m_textPosition = selection.position;
    m_textVerticalX = selection.verticalX;
    m_view->viewport()->update(m_view->visualRect(index));
}

bool RowPainter::pickMessageText(const QModelIndex &index, const QPoint &point, TextPick pick)
{
    const auto rect = selectableTextRect(index, m_view->visualRect(index));
    if (!rect.contains(point))
        return false;
    auto *document = bodyDocument(index, rect.width());
    if (!document)
        return false;
    QTextCursor cursor(document);
    if (pick == TextPick::Extend && m_textIndex == index) {
        cursor.setPosition(m_textAnchor);
        cursor.setPosition(textPosition(*document, rect, point), QTextCursor::KeepAnchor);
    } else {
        cursor.setPosition(textPosition(*document, rect, point));
        if (pick == TextPick::Word)
            cursor.select(QTextCursor::WordUnderCursor);
    }
    saveTextSelection(index, cursor);
    return true;
}

void RowPainter::extendTextSelection(const QPoint &point)
{
    auto cursor = selectionCursor();
    if (cursor.isNull())
        return;
    const auto rect = selectableTextRect(m_textIndex, m_view->visualRect(m_textIndex));
    cursor.setPosition(textPosition(*cursor.document(), rect, point), QTextCursor::KeepAnchor);
    saveTextSelection(m_textIndex, cursor);
}

bool RowPainter::hasTextCursor() const
{
    return m_textIndex.isValid();
}

void RowPainter::clearTextSelection()
{
    const auto old = m_textIndex;
    m_textIndex = QPersistentModelIndex{};
    if (old.isValid())
        m_view->viewport()->update(m_view->visualRect(old));
}

QString RowPainter::selectedText(const QModelIndex &index) const
{
    if (index.isValid() && m_textIndex != index)
        return {};
    auto text = selectionCursor().selectedText();
    return text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'))
               .replace(QChar::LineSeparator, QLatin1Char('\n'));
}

void RowPainter::selectMessageText()
{
    auto cursor = selectionCursor();
    if (!cursor.isNull()) {
        cursor.select(QTextCursor::Document);
        saveTextSelection(m_textIndex, cursor);
    }
}

void RowPainter::moveTextSelection(QTextCursor::MoveOperation operation)
{
    auto cursor = selectionCursor();
    if (!cursor.isNull()) {
        cursor.setVerticalMovementX(m_textVerticalX);
        cursor.movePosition(operation, QTextCursor::KeepAnchor);
        saveTextSelection(m_textIndex, cursor);
    }
}

QTextDocument *RowPainter::bodyDocument(const QModelIndex &index, int width) const
{
    const auto html = index.data(MarkupHtml).toString();
    const auto plain = html.isEmpty() ? index.data(Detail).toString() : QString();
    if (html.isEmpty() && (plain.isEmpty() || m_style == Row::Notice))
        return nullptr;
    const auto key = QString::number(width) + (html.isEmpty() ? QStringLiteral(":plain:") + plain
                                                              : QStringLiteral(":html:") + html);
    if (auto *existing = m_documents.object(key))
        return existing;
    auto *document = new QTextDocument;
    document->setUndoRedoEnabled(false);
    document->setDocumentMargin(0);
    document->setDefaultFont(rowFont(ui::bodyTextPixels));
    QTextOption options;
    options.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    document->setDefaultTextOption(options);
    if (html.isEmpty())
        document->setPlainText(plain);
    else
        document->setHtml(html);
    applyMessageEmojiFonts(*document);
    document->setTextWidth(qMax(1, width));
    m_documents.insert(key, document);
    return document;
}

QRect RowPainter::selectableTextRect(const QModelIndex &index, const QRect &row) const
{
    if ((m_style != Row::Message && m_style != Row::TaskContent) || !index.isValid() ||
        index.data(Id).toString().isEmpty() || index.data(Detail).toString().isEmpty())
        return {};
    return messageContentRect(row, index);
}

int RowPainter::textPosition(const QTextDocument &document, const QRect &rect, const QPoint &point)
{
    const auto offset = point - rect.topLeft();
    const int end = document.characterCount() - 1;
    if (offset.y() < 0)
        return 0;
    if (offset.y() >= document.size().height())
        return end;
    return qBound(0, document.documentLayout()->hitTest(offset, Qt::FuzzyHit), end);
}

QTextCursor RowPainter::selectionCursor() const
{
    if (!m_textIndex.isValid())
        return {};
    const auto rect = selectableTextRect(m_textIndex, m_view->visualRect(m_textIndex));
    auto *document = bodyDocument(m_textIndex, rect.width());
    if (!document)
        return {};
    QTextCursor cursor(document);
    const int end = document->characterCount() - 1;
    cursor.setPosition(qBound(0, m_textAnchor, end));
    cursor.setPosition(qBound(0, m_textPosition, end), QTextCursor::KeepAnchor);
    return cursor;
}

void RowPainter::saveTextSelection(const QModelIndex &index, const QTextCursor &cursor)
{
    const auto old = m_textIndex;
    m_textIndex = index;
    m_textAnchor = cursor.anchor();
    m_textPosition = cursor.position();
    m_textVerticalX = cursor.verticalMovementX();
    if (old.isValid() && old != index)
        m_view->viewport()->update(m_view->visualRect(old));
    m_view->viewport()->update(m_view->visualRect(index));
}

int RowPainter::bodyHeight(const QModelIndex &index, const QString &text, int width) const
{
    if (const auto *document = bodyDocument(index, width))
        return qMax(QFontMetrics(rowFont(ui::bodyTextPixels)).height(), qCeil(document->size().height()));
    return wrappedText(text, width, rowFont(ui::bodyTextPixels));
}

void RowPainter::body(QPainter &p, const QRect &rect, const QModelIndex &index, const QString &text,
                      const QColor &color) const
{
    auto *document = bodyDocument(index, rect.width());
    if (!document) {
        paragraph(p, rect, text, ui::bodyTextPixels, color);
        return;
    }
    p.save();
    p.setClipRect(rect, Qt::IntersectClip);
    p.translate(rect.topLeft());
    QAbstractTextDocumentLayout::PaintContext context;
    context.palette.setColor(QPalette::Text, color);
    if (m_textIndex == index && m_textAnchor != m_textPosition) {
        QTextCursor cursor(document);
        const int end = document->characterCount() - 1;
        cursor.setPosition(qBound(0, m_textAnchor, end));
        cursor.setPosition(qBound(0, m_textPosition, end), QTextCursor::KeepAnchor);
        QTextCharFormat format;
        format.setBackground(m_view->palette().brush(QPalette::Highlight));
        format.setForeground(m_view->palette().brush(QPalette::HighlightedText));
        context.selections.append({cursor, format});
    }
    document->documentLayout()->draw(&p, context);
    p.restore();
}

} // namespace bx::workspace
