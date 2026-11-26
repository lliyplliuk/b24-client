#include "ui/row_painter.h"
#include "ui/row_paint_primitives.h"
#include "ui/message_markup.h"
#include "ui/reaction_catalog.h"

#include <QAbstractTextDocumentLayout>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QHelpEvent>
#include <QListWidget>
#include <QMouseEvent>
#include <QToolTip>

namespace bx::workspace {

bool RowPainter::showPhotoMenuAt(const QModelIndex &index, const QPoint &point) const
{
    if (!m_messagePhotoMenu || !index.isValid())
        return false;
    const auto target = fileAt(index, m_view->visualRect(index), point);
    if (!target || !target->geometry.thumbnail.contains(point)
        || photoMenuRect(target->geometry.thumbnail, target->file).isEmpty())
        return false;
    const auto global = m_view->viewport()->mapToGlobal(point);
    m_messagePhotoMenu(index, target->file, QRect(global, QSize(1, 1)));
    return true;
}

QString RowPainter::anchorAt(const QModelIndex &index, const QRect &row, const QPoint &viewportPos) const
{
    if (m_style == Row::Notice && !index.data(NoticePresentation).value<NoticeRow>().expanded)
        return {};
    const auto rect = m_style == Row::Notice ? noticeBodyRect(row, index) : messageContentRect(row, index);
    if (!rect.contains(viewportPos))
        return {};
    const auto *document = bodyDocument(index, rect.width());
    if (!document)
        return {};
    return document->documentLayout()->anchorAt(viewportPos - rect.topLeft());
}

QString RowPainter::linkAt(const QModelIndex &index, const QRect &row, const QPoint &point) const
{
    if (const auto href = anchorAt(index, row, point); !href.isEmpty())
        return href;
    auto card = firstLinkCard(row, index);
    for (const auto &[href, preview] : linkPreviews(index)) {
        card.setHeight(linkCardHeight(preview));
        if (card.contains(point))
            return href;
        card.translate(0, card.height() + linkCardGap);
    }
    return {};
}

QRect RowPainter::receiptArea(const QModelIndex &index, const QRect &row) const
{
    return m_style == Row::Notice ? noticeRect(row, index) : messageBubble(row, index);
}

bool RowPainter::activateTextLink(const QString &href, const QPoint &point) const
{
    if (href.isEmpty() || !m_textIndex.isValid())
        return false;
    const auto rect = m_view->visualRect(m_textIndex);
    if (anchorAt(m_textIndex, rect, point) == href) {
        if (auto action = linkActionAt(m_textIndex, rect, point)) {
            action();
            return true;
        }
    }
    return false;
}

bool RowPainter::editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
                             const QModelIndex &index)
{
    if ((m_style == Row::Message || m_style == Row::TaskContent || m_style == Row::Notice) &&
        event->type() == QEvent::MouseButtonRelease) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton) {
            if (auto action = rowActionAt(index, option.rect, mouse->position().toPoint())) {
                action();
                return true;
            }
        }
    }
    return QStyledItemDelegate::editorEvent(event, model, option, index);
}

bool RowPainter::helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
                           const QModelIndex &index)
{
    if (event && event->type() == QEvent::ToolTip) {
        if (const auto target = fileAt(index, option.rect, event->pos())) {
            const auto &geometry = target->geometry;
            auto bounds = geometry.thumbnail.isEmpty() ? geometry.control : geometry.thumbnail;
            auto hint = QStringLiteral("<span style=\"white-space: pre-wrap\">%1</span>")
                            .arg(target->file.name.toHtmlEscaped());
            const auto menu = photoMenuRect(geometry.thumbnail, target->file);
            if (menu.contains(event->pos())) {
                bounds = menu;
                hint.prepend(target->file.isVideo ? QStringLiteral("Сохранить видео<br>")
                                                  : QStringLiteral("Действия с изображением<br>"));
            }
            QToolTip::showText(event->globalPos(), QStringLiteral("<qt>%1</qt>").arg(hint), view->viewport(), bounds);
            return true;
        }
        const auto quote = replyHeaderRect(option.rect, index);
        if (!index.data(ReplyByline).toString().isEmpty() && quote.contains(event->pos())) {
            const auto attachments = replyAttachments(index);
            if (!attachments.representative && attachments.documents == 1) {
                QToolTip::showText(event->globalPos(), QStringLiteral("<qt><span style=\"white-space: pre-wrap\">%1</span></qt>")
                    .arg(attachments.firstDocument.toHtmlEscaped()), view->viewport(), quote);
                return true;
            }
        }
        const auto receipt = viewersReceipt(index, option.rect);
        if (receipt.contains(event->pos())) {
            QToolTip::showText(event->globalPos(), QStringLiteral("Кто прочитал сообщение"),
                               view->viewport(), receipt);
            return true;
        }
    }
    if (event && event->type() == QEvent::ToolTip && canReact(index)) {
        for (const auto &target : reactionTargets(index, messageBubble(option.rect, index))) {
            if (!target.rect.contains(event->pos()))
                continue;
            const auto *reaction = ui::findReaction(target.code);
            const auto title = reaction ? QString::fromUtf8(reaction->caption) : target.code;
            const auto hint = (target.own ? QStringLiteral("Снять реакцию «%1»")
                                           : QStringLiteral("Поставить реакцию «%1»")).arg(title);
            QToolTip::showText(event->globalPos(), ui::plainTooltip(hint),
                               view->viewport(), target.rect);
            return true;
        }
    }
    return QStyledItemDelegate::helpEvent(event, view, option, index);
}

bool RowPainter::activateMedia(const QModelIndex &index)
{
    if (!m_view)
        return false;
    if (m_style != Row::Message && m_style != Row::TaskContent)
        return false;
    for (const auto &file : files(index, MessageFiles)) {
        if (file.isVideo && m_messageVideo && downloadable(file)) {
            m_messageVideo(index, file);
            return true;
        }
        if (!file.isVideo && m_messagePhoto && (filePreview(file) || (file.isImage && downloadable(file)))) {
            m_messagePhoto(file);
            return true;
        }
    }
    return false;
}

bool RowPainter::eventFilter(QObject *object, QEvent *event)
{
    if (object == m_view->viewport()) {
        if (event->type() == QEvent::ContextMenu && m_style == Row::TaskContent) {
            auto *context = static_cast<QContextMenuEvent *>(event);
            if (context->reason() == QContextMenuEvent::Mouse
                && showPhotoMenuAt(m_view->indexAt(context->pos()), context->pos())) {
                context->accept();
                return true;
            }
        } else if (event->type() == QEvent::MouseMove) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            const auto point = mouse->position().toPoint();
            const auto index = m_view->indexAt(point);
            if (mouse->buttons() == Qt::NoButton) {
                const auto row = m_view->visualRect(index);
                if (index.isValid() && rowActionAt(index, row, point))
                    m_view->viewport()->setCursor(Qt::PointingHandCursor);
                else if (selectableTextRect(index, row).contains(point))
                    m_view->viewport()->setCursor(Qt::IBeamCursor);
                else
                    m_view->viewport()->unsetCursor();
            }
        } else if (event->type() == QEvent::MouseButtonDblClick) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            const auto point = mouse->position().toPoint();
            const auto index = m_view->indexAt(point);
            if (mouse->button() == Qt::LeftButton && index.isValid()) {
                const auto target = fileAt(index, m_view->visualRect(index), point);
                if (target && target->geometry.thumbnail.contains(point))
                    return true;
            }
        } else if (event->type() == QEvent::Leave) {
            m_view->viewport()->unsetCursor();
        }
    }
    return QStyledItemDelegate::eventFilter(object, event);
}

std::function<void()> RowPainter::rowActionAt(const QModelIndex &index, const QRect &row, const QPoint &point) const
{
    if (m_style == Row::Notice) {
        const auto notice = index.data(NoticePresentation).value<NoticeRow>();
        const auto menu = noticeMenuRect(row, index);
        if (m_noticeMenu && menu.contains(point))
            return [this, index, menu] { m_noticeMenu(index, menu.bottomLeft()); };
        if (m_noticeToggle && noticeHeaderRect(row, index, notice).contains(point))
            return [this, index] { m_noticeToggle(index); };
        if (m_noticeAction && notice.expanded && noticePrimaryRect(row, index, notice).contains(point))
            return [this, index, target = notice.primaryTarget] { m_noticeAction(index, target); };
        return linkActionAt(index, row, point);
    }
    const auto receipt = viewersReceipt(index, row);
    if (receipt.contains(point))
        return [this, id = index.data(ReceiptMessageId).toString(), receipt] {
            m_messageViewers(id, m_view->viewport()->mapToGlobal(receipt.bottomLeft()));
        };
    if (canReact(index)) {
        for (const auto &target : reactionTargets(index, messageBubble(row, index))) {
            if (target.rect.contains(point))
                return [this, index, target] {
                    m_reaction(index, target.code, m_view->viewport()->mapToGlobal(target.rect.bottomLeft()));
                };
        }
    }
    if (m_messageDownload || m_messagePhoto || m_messagePhotoMenu || m_messageVideo) {
        if (const auto target = fileAt(index, row, point)) {
            const auto &[file, geometry] = *target;
            const auto menu = photoMenuRect(geometry.thumbnail, file);
            if (menu.contains(point)) {
                const QRect anchor(m_view->viewport()->mapToGlobal(menu.topLeft()), menu.size());
                return [this, index, file = file, anchor] { m_messagePhotoMenu(index, file, anchor); };
            }
            const bool titleControl = geometry.thumbnail.isEmpty()
                && point.y() < geometry.control.top() + fileTitleTop + fileTitleHeight;
            if (file.isVideo && m_messageVideo && downloadable(file)
                && (geometry.thumbnail.contains(point) || titleControl)) {
                return [this, index, file = file] { m_messageVideo(index, file); };
            }
            const bool originalControl = file.isImage && titleControl && downloadable(file);
            const bool opensThumbnail = geometry.thumbnail.contains(point) && (filePreview(file) || downloadable(file));
            if (!file.isVideo && m_messagePhoto && (opensThumbnail || originalControl))
                return [this, file = file] { m_messagePhoto(file); };
            if (m_messageDownload && geometry.control.contains(point) && downloadable(file))
                return [this, file = file] { m_messageDownload(file); };
        }
    }
    const auto id = index.data(ReplyTargetId).toString();
    if (m_replyNavigation && !id.isEmpty() && replyHeaderRect(row, index).contains(point))
        return [this, id] { m_replyNavigation(id); };
    return linkActionAt(index, row, point);
}

std::function<void()> RowPainter::linkActionAt(const QModelIndex &index, const QRect &row, const QPoint &point) const
{
    const auto target = linkAt(index, row, point);
    if (!target.isEmpty()) {
        if (m_messageLink)
            return [this, target] { m_messageLink(target); };
        const auto url = messageWebLink(target);
        if (!url.isEmpty())
            return [url] { QDesktopServices::openUrl(url); };
    }
    return {};
}

} // namespace bx::workspace
