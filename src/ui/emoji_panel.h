#pragma once

#include <QFrame>

class QGridLayout;
class QPlainTextEdit;
class QPushButton;

namespace bx::ui {

class EmojiTile;

class EmojiPanel final : public QFrame {
public:
    EmojiPanel(QWidget *host, QPushButton *anchor, QPlainTextEdit *input);
    ~EmojiPanel() override;
    void openPanel();
    void dismiss(bool focus = false);

protected:
    bool eventFilter(QObject *target, QEvent *event) override;

private:
    struct Emoji { const char *glyph; const char *name; };
    void showCategory(int category);
    void reposition();

    QWidget *m_host;
    QPushButton *m_anchor;
    QPlainTextEdit *m_input;
    QGridLayout *m_grid = nullptr;
    EmojiTile *m_first = nullptr;
};

} // namespace bx::ui
