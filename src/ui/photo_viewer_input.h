#pragma once

#include <QObject>
#include <QPointer>
#include <QDialog>

namespace bx::ui {

class PhotoViewerInput final : public QObject {
public:
    explicit PhotoViewerInput(QDialog &viewer);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void retireIfIdle();
    static bool belongsTo(const QObject *target, const QWidget &owner);
    QPointer<QDialog> m_viewer;
    QPointer<QWidget> m_owner;
    Qt::MouseButton m_dismissedButton = Qt::NoButton;
    bool m_dismissedContextPending = false;
};

} // namespace bx::ui
