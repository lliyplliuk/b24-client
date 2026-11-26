#pragma once

#include <QWidget>
#include <QKeySequence>
#include <QTextFormat>
#include <QTextBlock>
#include <QImage>

#include <functional>

class QHBoxLayout;
class QLabel;
class QToolButton;

namespace bx::ui::task_description { class Input; }

namespace bx::ui {

class TaskDescriptionEditor final : public QWidget {
public:
    explicit TaskDescriptionEditor(QWidget *parent = nullptr);
    void setBbCode(const QString &description);
    void setCompactHeight();
    QString bbCode() const;
    void insertAttachment(const QString &diskId, const QString &name = {}, const QSize &imageSize = {});
    void setEditEnabled(bool enabled);
    void setUploadStatus(const QString &status, bool loading = false);
    void setUploadEnabled(bool enabled);
    void setAttachHandler(std::function<void(const QStringList &)> handler);
    void setImagePasteHandler(std::function<void(const QImage &)> handler);

private:
    task_description::Input *m_input = nullptr;
    QWidget *m_tools = nullptr;
    QHBoxLayout *m_toolRow = nullptr;
    QToolButton *m_bold = nullptr;
    QToolButton *m_italic = nullptr;
    QToolButton *m_underline = nullptr;
    QToolButton *m_strike = nullptr;
    QToolButton *m_bullets = nullptr;
    QToolButton *m_numbers = nullptr;
    QToolButton *m_attach = nullptr;
    QLabel *m_upload = nullptr;
    QString m_original;
    bool m_allowUpload = true;

    QToolButton *tool(const QString &title, const QString &accessible, bool checkable = false);
    void bindShortcut(QKeySequence::StandardKey key, QToolButton *button);
    void refreshUpload();
    void apply(const QTextCharFormat &format);
    void formatList(QTextListFormat::Style style, bool active);
    void insertLink();
};

} // namespace bx::ui
