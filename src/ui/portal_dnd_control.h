#pragma once

#include "ui/loading_indicator.h"

#include "core/backend.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>
#include <functional>

namespace bx::ui {

inline QString portalStatusLabel(const QString &status)
{
    if (status == QLatin1String("online"))
        return QStringLiteral("В сети");
    if (status == QLatin1String("away"))
        return QStringLiteral("Отошёл");
    if (status == QLatin1String("break"))
        return QStringLiteral("На перерыве");
    if (status == QLatin1String("dnd"))
        return QStringLiteral("Не беспокоить");
    return {};
}

inline void addPortalDndControl(QVBoxLayout *content, Backend *backend, std::function<bool()> writable)
{
    if (backend->isDemo())
        return;
    auto *panel = new QWidget(content->parentWidget());
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *current = new QLabel(QStringLiteral("Статус на портале загружается…"), panel);
    current->setWordWrap(true);
    layout->addWidget(current);
    auto *row = new QHBoxLayout;
    auto *choice = new QComboBox(panel);
    choice->setAccessibleName(QStringLiteral("Новый статус на портале"));
    row->addWidget(choice, 1);
    auto *save = new QPushButton(QStringLiteral("Установить"), panel);
    save->setAccessibleName(QStringLiteral("Установить статус на портале"));
    row->addWidget(save);
    layout->addLayout(row);
    auto *hint = new QLabel(panel);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    content->addWidget(panel);

    const auto enable = [panel, choice, save, writable] {
        const bool ready = writable() && !panel->property("pending").toBool()
            && !panel->property("status").toString().isEmpty();
        choice->setEnabled(ready);
        save->setEnabled(ready && !choice->currentData().toString().isEmpty());
    };
    QObject::connect(choice, &QComboBox::currentIndexChanged, panel, [enable] { enable(); });
    QObject::connect(backend, &Backend::connectionChanged, panel, [enable] { enable(); });
    QObject::connect(backend, &Backend::portalStatusLoaded, panel,
                     [panel, current, choice, enable](const QString &status) {
        setLoadingIndicator(current, false);
        const auto label = portalStatusLabel(status);
        if (label.isEmpty())
            return;
        current->setText(QStringLiteral("Статус на портале: %1").arg(label));
        if (panel->property("status").toString() != status) {
            panel->setProperty("status", status);
            choice->clear();
            choice->addItem(QStringLiteral("Выберите новый статус"));
            if (status == QLatin1String("dnd")) {
                for (const auto *key : {"online", "away", "break"})
                    choice->addItem(portalStatusLabel(QString::fromLatin1(key)), QString::fromLatin1(key));
            } else {
                const auto dnd = QStringLiteral("dnd");
                choice->addItem(portalStatusLabel(dnd), dnd);
            }
        }
        enable();
    });
    QObject::connect(save, &QPushButton::clicked, panel, [panel, choice, backend, hint, enable, writable] {
        const auto requested = choice->currentData().toString();
        if (!writable() || panel->property("pending").toBool() || requested.isEmpty())
            return;
        panel->setProperty("pending", true);
        hint->setText(QStringLiteral("Проверяем и меняем статус на портале…"));
        setLoadingIndicator(hint, true);
        enable();
        backend->updatePortalStatus(panel->property("status").toString(), requested);
    });
    QObject::connect(backend, &Backend::portalStatusUpdated, panel, [panel, hint, enable](const QString &) {
        panel->setProperty("pending", false);
        hint->setText(QStringLiteral("Портал подтвердил новый статус"));
        setLoadingIndicator(hint, false);
        enable();
    });
    QObject::connect(backend, &Backend::failed, panel, [panel, current, hint, enable](const QString &operation, const QString &reason) {
        if (operation != QLatin1String("portal-status") && operation != QLatin1String("portal-status-update"))
            return;
        panel->setProperty("pending", false);
        hint->setText(reason);
        setLoadingIndicator(current, false);
        setLoadingIndicator(hint, false);
        enable();
    });
    enable();
    setLoadingIndicator(current, true);
    backend->requestPortalStatus();
}

} // namespace bx::ui
