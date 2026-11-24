#pragma once

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace bx::ui {

inline constexpr int TaskPickerDraft = Qt::UserRole + 150;

// The current value and the keyboard cursor are independent in picker lists.
class TaskPickerChoiceDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    static constexpr int RowHeight = 34;

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        return {160, RowHeight};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem row(option);
        initStyleOption(&row, index);
        const bool confirmed = index.data(Qt::CheckStateRole).toInt() == Qt::Checked;
        const bool draft = index.data(TaskPickerDraft).toBool();
        const bool cursor = row.state.testFlag(QStyle::State_Selected);
        const auto box = row.rect.adjusted(1, 1, -2, -1);
        painter->save();
        painter->setClipRect(row.rect);
        painter->setRenderHint(QPainter::Antialiasing);
        if (confirmed || draft || cursor) {
            painter->setPen(cursor ? QPen(QColor("#54877e")) : Qt::NoPen);
            painter->setBrush(confirmed ? QColor("#e1f0ec") : draft ? QColor("#e5eff4") : QColor("#f3f7f5"));
            painter->drawRoundedRect(box, 6, 6);
        }

        int textLeft = box.left() + 10;
        if (!row.icon.isNull()) {
            constexpr int iconSize = 22;
            row.icon.paint(painter, QRect(textLeft, box.center().y() - iconSize / 2, iconSize, iconSize));
            textLeft += iconSize + 8;
        }
        const int rightPadding = confirmed || draft ? 31 : 10;
        const QRect textRect(textLeft, box.top(), qMax(0, box.right() - rightPadding - textLeft), box.height());
        painter->setFont(row.font);
        painter->setPen(QColor("#182d3b"));
        painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                          row.fontMetrics.elidedText(row.text, Qt::ElideRight, textRect.width()));
        const QPointF mark(box.right() - 16, box.center().y());
        if (confirmed) {
            painter->setPen(QPen(QColor("#26685d"), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter->setBrush(Qt::NoBrush);
            QPainterPath check(mark + QPointF(-5, 0));
            check.lineTo(mark + QPointF(-1.5, 3.5));
            check.lineTo(mark + QPointF(5, -4));
            painter->drawPath(check);
        } else if (draft) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor("#28657b"));
            painter->drawEllipse(mark, 3.5, 3.5);
        }
        painter->restore();
    }
};

class TaskPickerValueLabel final : public QLabel {
public:
    explicit TaskPickerValueLabel(QWidget *parent) : QLabel(parent)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setTextFormat(Qt::PlainText);
    }

    void setFullText(const QString &text)
    {
        if (m_fullText == text)
            return;
        m_fullText = text;
        setAccessibleDescription(text);
        updateElision();
    }

    void setWrapLongText(bool wrap)
    {
        if (wordWrap() == wrap)
            return;
        setWordWrap(wrap);
        updateElision();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QLabel::resizeEvent(event);
        updateElision();
    }
    void changeEvent(QEvent *event) override
    {
        QLabel::changeEvent(event);
        if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
            updateElision();
    }

private:
    void updateElision()
    {
        auto line = m_fullText;
        line.replace(QLatin1Char('\n'), QLatin1Char(' '));
        line.replace(QLatin1Char('\r'), QLatin1Char(' '));
        const auto width = contentsRect().width();
        const auto visible = !wordWrap() && width > 0 ? fontMetrics().elidedText(line, Qt::ElideRight, width) : line;
        setText(visible);
        setToolTip(visible == line ? QString()
            : QStringLiteral("<qt>%1</qt>").arg(m_fullText.toHtmlEscaped()));
    }

    QString m_fullText;
};


class TaskPickerSummary final : public QWidget {
public:
    explicit TaskPickerSummary(QWidget *parent) : QWidget(parent)
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);
        auto *currentLine = addLine(layout, "taskPickerCurrent");
        addCaption(currentLine, QStringLiteral("Сейчас"));
        m_icon = new QLabel(currentLine->parentWidget());
        m_icon->setFixedSize(22, 22);
        currentLine->addWidget(m_icon);
        m_current = addValue(currentLine, QStringLiteral("Текущее значение"));
        auto *pendingLine = addLine(layout, "taskPickerPending");
        m_pendingCaption = addCaption(pendingLine, QStringLiteral("Будет"));
        m_pending = addValue(pendingLine, QStringLiteral("Новый выбор"));
        m_pending->parentWidget()->hide();
    }

    void setCurrent(const QString &text, const QIcon &icon = {})
    {
        m_current->setFullText(text.isEmpty() ? QStringLiteral("—") : text);
        m_icon->setVisible(!icon.isNull());
        m_icon->setPixmap(icon.isNull() ? QPixmap() : icon.pixmap(22, 22));
    }

    void setPending(const QString &text, const QString &caption = QStringLiteral("Будет"))
    {
        m_pendingCaption->setText(caption);
        m_pending->setFullText(text);
        m_pending->parentWidget()->setVisible(!text.isEmpty());
    }

private:
    QHBoxLayout *addLine(QVBoxLayout *layout, const char *name)
    {
        auto *line = new QWidget(this);
        line->setObjectName(QString::fromLatin1(name));
        auto *content = new QHBoxLayout(line);
        content->setContentsMargins(8, 5, 8, 5);
        content->setSpacing(7);
        layout->addWidget(line);
        return content;
    }

    static QLabel *addCaption(QHBoxLayout *line, const QString &caption)
    {
        auto *label = new QLabel(caption, line->parentWidget());
        label->setObjectName(QStringLiteral("taskPickerCaption"));
        line->addWidget(label);
        return label;
    }

    static TaskPickerValueLabel *addValue(QHBoxLayout *line, const QString &accessible)
    {
        auto *value = new TaskPickerValueLabel(line->parentWidget());
        value->setObjectName(QStringLiteral("taskPickerValue"));
        value->setAccessibleName(accessible);
        line->addWidget(value, 1);
        return value;
    }

    QLabel *m_icon = nullptr;
    QLabel *m_pendingCaption = nullptr;
    TaskPickerValueLabel *m_current = nullptr;
    TaskPickerValueLabel *m_pending = nullptr;
};

inline QIcon taskStageColorIcon(const QString &color)
{
    const QColor supplied(QLatin1Char('#') + color);
    QPixmap dot(22, 22);
    dot.fill(Qt::transparent);
    QPainter painter(&dot);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(supplied.isValid() ? supplied : QColor("#bccbc7"));
    painter.drawEllipse(QRect(5, 5, 12, 12));
    painter.end();
    return QIcon(dot);
}

} // namespace bx::ui
