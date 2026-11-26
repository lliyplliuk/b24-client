#pragma once

#include "ui/glyphs.h"
#include <QPushButton>

namespace bx::ui {

class GlyphButton final : public QPushButton {
public:
    GlyphButton(Glyph glyph, const QString &name, QWidget *parent, bool primary = false);
    void setGlyph(Glyph glyph);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    Glyph m_glyph;
    bool m_primary;
};

} // namespace bx::ui
