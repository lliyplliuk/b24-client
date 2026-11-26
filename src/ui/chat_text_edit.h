#pragma once

#include <QPlainTextEdit>
#include <functional>

namespace bx::ui {

bool validChatClipboardImageSize(const QSize &size);

class ChatTextEdit final : public QPlainTextEdit {
public:
    explicit ChatTextEdit(QWidget *parent);
    bool composing() const;
    std::function<void()> userEdited;
    std::function<bool()> mayPasteImage;
    std::function<void(const QImage &)> imagePasted;
    void setSingleLinePlaceholder(const QString &text);

protected:
    bool canInsertFromMimeData(const QMimeData *source) const override;
    void insertFromMimeData(const QMimeData *source) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;

private:
    bool acceptsImagePaste() const;

    static constexpr int singleLineHeight = 43;
    static constexpr int maximumVisibleLines = 6;
    void scheduleHeightUpdate();
    void updatePlaceholder();

    QString m_fullPlaceholder;
    bool m_preedit = false;
    bool m_heightUpdatePending = false;
};

} // namespace bx::ui
