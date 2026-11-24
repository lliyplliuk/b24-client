#pragma once

#include <QDialog>
#include <QUrl>

class QLabel;
class QLineEdit;
class QPushButton;
class QResizeEvent;

namespace bx {

class ConnectionDialog final : public QDialog {
    Q_OBJECT

public:
    explicit ConnectionDialog(const QUrl &prefill = {}, QWidget *parent = nullptr);

    QUrl portal() const;
    static QUrl normalizePortal(const QString &address);
    void setCheckingSavedSession();
    void setWaiting(const QString &message);
    void setError(const QString &message);
    void resetFeedback();

signals:
    void connectRequested(const QUrl &portal);
    void addressEdited();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void showFeedback(const QString &message, bool error, bool loading = false);
    void submit();

    QWidget *m_illustration = nullptr;
    QLineEdit *m_address = nullptr;
    QLabel *m_feedback = nullptr;
    QPushButton *m_continue = nullptr;
};

} // namespace bx
