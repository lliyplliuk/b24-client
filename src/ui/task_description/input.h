#pragma once

#include <QTextEdit>
#include <QHash>
#include <QImage>

#include <functional>

namespace bx::ui::task_description {

class Input final : public QTextEdit {
public:
    struct Token {
        QString raw;
        QString label;
    };

    explicit Input(QWidget *parent);
    void load(const QString &description);
    QString bbCode() const;
    void attach(const QString &raw, const QString &name);
    void requestFiles(const QStringList &paths);
    std::function<void(const QStringList &)> filesPasted;
    std::function<void(const QImage &)> imagePasted;
    bool uploadEnabled = true;

protected:
    QMimeData *createMimeDataFromSelection() const override;
    bool canInsertFromMimeData(const QMimeData *mime) const override;
    void insertFromMimeData(const QMimeData *mime) override;

private:
    QList<Token> m_tokens;
    QHash<QString, QImage> m_chips;
    QTextCursor m_uploadCursor;

    void holdUploadCursor();
    QString decode(const QString &source);
    QString tokenHtml(const QString &raw, const QString &label);
    QImage tokenImage(const QString &label);
    QString chipText(const QTextCharFormat &format, bool plain) const;

    struct Style {
        QString open;
        QString close;
        bool operator==(const Style &other) const;
    };
    static QList<Style> styles(const QTextCharFormat &format);

    // Keep common tags open across consecutive QText fragments (for instance bold around an italic word).
    static void changeStyle(QString &output, QList<Style> &current, const QList<Style> &desired);
    QString encode(const QTextDocument &document, int start, int end, bool plain) const;
};

} // namespace bx::ui::task_description
