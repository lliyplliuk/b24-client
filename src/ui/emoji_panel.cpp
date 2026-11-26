#include "ui/emoji_panel.h"

#include "ui/emoji_tile.h"
#include "ui/glyph_button.h"
#include "ui/workspace_style.h"
#include <QButtonGroup>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QVBoxLayout>

namespace bx::ui {

EmojiPanel::EmojiPanel(QWidget *host, QPushButton *anchor, QPlainTextEdit *input)
    : QFrame(host), m_host(host), m_anchor(anchor), m_input(input)
{
    setObjectName(QStringLiteral("emojiPanel"));
    setAccessibleName(QStringLiteral("Выбор эмодзи"));
    setStyleSheet(QStringLiteral(R"(
            QFrame#emojiPanel { background: #fffefa; border: 1px solid #aac6bd; border-radius: 11px; }
            QFrame#emojiPanel QPushButton#emojiCategory {
                background: transparent; padding: 5px 9px; border: 1px solid transparent; font-size: 14px;
            }
            QFrame#emojiPanel QPushButton#emojiCategory:checked { background: #e2f0ea; color: #164c51; }
            QFrame#emojiPanel QPushButton#emojiCategory:focus { border-color: #215c70; }
        )"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 8, 10, 10);
    root->setSpacing(6);
    auto *header = new QHBoxLayout;
    auto *title = workspace::label(QStringLiteral("Эмодзи"), "sectionTitle", this);
    header->addWidget(title, 1);
    auto *close = new GlyphButton(Glyph::Close, QStringLiteral("Закрыть выбор эмодзи"), this);
    close->setFixedSize(30, 29);
    connect(close, &QPushButton::clicked, this, [this] { dismiss(true); });
    header->addWidget(close);
    root->addLayout(header);

    auto *navigation = new QHBoxLayout;
    navigation->setSpacing(3);
    auto *categories = new QButtonGroup(this);
    const QStringList labels = {QStringLiteral("Эмоции"), QStringLiteral("Жесты"), QStringLiteral("Разное")};
    for (int i = 0; i < labels.size(); ++i) {
        auto *category = new QPushButton(labels[i], this);
        category->setObjectName(QStringLiteral("emojiCategory"));
        category->setCheckable(true);
        category->setAutoDefault(false);
        category->setChecked(i == 0);
        categories->addButton(category, i);
        navigation->addWidget(category);
    }
    navigation->addStretch(1);
    root->addLayout(navigation);
    connect(categories, &QButtonGroup::idClicked, this, [this](int category) { showCategory(category); });
    m_grid = new QGridLayout;
    m_grid->setSpacing(3);
    root->addLayout(m_grid);
    showCategory(0);
    hide();
}

EmojiPanel::~EmojiPanel()
{
    if (qApp)
        qApp->removeEventFilter(this);
}

void EmojiPanel::openPanel()
{
    reposition();
    show();
    raise();
    m_anchor->setChecked(true);
    qApp->installEventFilter(this);
    if (m_first)
        m_first->setFocus(Qt::OtherFocusReason);
}

void EmojiPanel::dismiss(bool focus)
{
    if (isHidden())
        return;
    qApp->removeEventFilter(this);
    hide();
    m_anchor->setChecked(false);
    if (focus)
        m_input->setFocus(Qt::OtherFocusReason);
}

bool EmojiPanel::eventFilter(QObject *target, QEvent *event)
{
    if (target == m_host && event->type() == QEvent::Hide) {
        dismiss();
        return false;
    }
    if (!isVisible())
        return false;
    const auto *widget = qobject_cast<QWidget *>(target);
    if (event->type() == QEvent::MouseButtonPress) {
        if (widget && widget != this && !isAncestorOf(widget) &&
            widget != m_anchor && !m_anchor->isAncestorOf(widget))
            dismiss();
    } else if (event->type() == QEvent::KeyPress && widget && widget->window() == m_host->window()) {
        if (static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            dismiss(true);
            return true;
        }
    } else if (target == m_anchor && event->type() == QEvent::EnabledChange && !m_anchor->isEnabled()) {
        dismiss();
    } else if (target == m_host->window() && event->type() == QEvent::WindowDeactivate) {
        dismiss();
    } else if ((target == m_anchor || target == m_host) &&
               (event->type() == QEvent::Resize || event->type() == QEvent::Move)) {
        reposition();
    }
    return false;
}

void EmojiPanel::showCategory(int category)
{
    static constexpr Emoji contents[][12] = {
        {{"😀", "Улыбка"}, {"😄", "Радость"}, {"😂", "Смех"}, {"🙂", "Лёгкая улыбка"},
         {"😉", "Подмигивание"}, {"😍", "Влюблённость"}, {"😎", "В очках"}, {"🤔", "Размышление"},
         {"😮", "Удивление"}, {"😢", "Грусть"}, {"😭", "Плач"}, {"😡", "Злость"}},
        {{"👍", "Нравится"}, {"👎", "Не нравится"}, {"👏", "Аплодисменты"}, {"🙌", "Поднятые руки"},
         {"🤝", "Рукопожатие"}, {"🙏", "Спасибо"}, {"👋", "Приветствие"}, {"✌️", "Победа"},
         {"💪", "Сила"}, {"👌", "Хорошо"}, {"🤞", "Скрещённые пальцы"}, {"🫶", "Сердце руками"}},
        {{"❤️", "Сердце"}, {"🎉", "Праздник"}, {"🔥", "Огонь"}, {"✅", "Отметка"},
         {"❌", "Крестик"}, {"⚠️", "Внимание"}, {"💡", "Идея"}, {"📌", "Кнопка"},
         {"📅", "Календарь"}, {"🚀", "Ракета"}, {"☕", "Кофе"}, {"🍀", "Клевер"}},
    };
    while (auto *item = m_grid->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    int i = 0;
    for (const auto &emoji : contents[category]) {
        const auto glyph = QString::fromUtf8(emoji.glyph);
        auto *tile = new EmojiTile(glyph, QString::fromUtf8(emoji.name), this);
        if (i == 0)
            m_first = tile;
        m_grid->addWidget(tile, i / 6, i % 6);
        connect(tile, &QPushButton::clicked, this, [this, glyph] {
            if (!m_input->isReadOnly()) {
                m_input->insertPlainText(glyph);
                m_input->setFocus(Qt::OtherFocusReason);
            }
        });
        ++i;
    }
}

void EmojiPanel::reposition()
{
    const auto size = sizeHint();
    const auto point = m_anchor->mapTo(m_host, QPoint());
    const int x = qBound(4, point.x() + m_anchor->width() - size.width(),
                         qMax(4, m_host->width() - size.width() - 4));
    setGeometry(QRect(QPoint(x, qMax(4, point.y() - size.height() - 8)), size));
}

} // namespace bx::ui
