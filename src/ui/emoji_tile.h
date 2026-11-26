#pragma once

#include <QPushButton>

namespace bx::ui {

class EmojiTile final : public QPushButton {
public:
    EmojiTile(const QString &glyph, const QString &name, QWidget *parent);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QString m_glyph;
};

} // namespace bx::ui
