#pragma once

#include <QDesktopServices>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
#include <QUrl>

namespace bx::ui {

inline void offerToOpenSavedFile(QWidget *parent, const QString &path)
{
    const QFileInfo file(path);
    if (!parent || !parent->isVisible() || parent->window()->isMinimized() ||
        !file.isAbsolute() || !file.isFile())
        return;

    auto *offer = new QMessageBox(parent);
    offer->setObjectName(QStringLiteral("savedFileOffer"));
    offer->setAttribute(Qt::WA_DeleteOnClose);
    offer->setWindowTitle(QStringLiteral("Файл сохранён"));
    offer->setIcon(QMessageBox::Information);
    offer->setTextFormat(Qt::PlainText);
    offer->setText(file.fileName());
    auto *open = offer->addButton(QStringLiteral("Открыть"), QMessageBox::AcceptRole);
    open->setAccessibleName(open->text());
    auto *close = offer->addButton(QStringLiteral("Закрыть"), QMessageBox::RejectRole);
    offer->setDefaultButton(close);
    offer->setEscapeButton(close);
    QObject::connect(open, &QPushButton::clicked, offer, [path] {
        if (QFileInfo(path).isFile())
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });
    offer->open();
}

} // namespace bx::ui
