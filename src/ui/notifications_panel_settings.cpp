#include "ui/notifications_panel.h"

#include "ui/portal_dnd_control.h"
#include "ui/workspace_presentation.h"
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace bx {
void NotificationsPanel::showSettings()
{
    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("notificationSettingsDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::finished, this, [this] {
        QTimer::singleShot(0, this, &NotificationsPanel::scheduleVisibleNoticeRead);
    });
    dialog->setWindowTitle(QStringLiteral("Уведомления портала"));
    dialog->setMinimumWidth(390);
    auto *content = new QVBoxLayout(dialog);
    auto *status = workspace::label(QStringLiteral("Загружаем настройки…"), "minorLabel", dialog);
    status->setWordWrap(true);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    auto *close = buttons->button(QDialogButtonBox::Close);
    close->setText(QStringLiteral("Закрыть"));
    close->setIcon(QIcon());
    auto *retry = buttons->addButton(QStringLiteral("Повторить загрузку"), QDialogButtonBox::ActionRole);
    retry->setAccessibleName(QStringLiteral("Повторить загрузку настроек"));
    retry->hide();
    const auto loadSettings = [this, retry, status] {
        retry->hide();
        status->setText(QStringLiteral("Загружаем настройки…"));
        ui::setLoadingIndicator(status, true);
        m_backend->requestNotificationSettings();
    };
    connect(retry, &QPushButton::clicked, dialog, loadSettings);
    auto addSetting = [this, dialog, content, status](const QString &title, const QString &name, bool volume) {
        auto *container = new QWidget(dialog);
        auto *row = new QHBoxLayout(container);
        row->setContentsMargins(0, 0, 0, 0);
        std::function<int()> value;
        std::function<void(int)> setValue;
        if (volume) {
            auto *input = new QSpinBox(container);
            input->setRange(0, 100);
            input->setSuffix(QStringLiteral(" %"));
            input->setAccessibleName(title);
            auto *caption = new QLabel(title, container);
            caption->setBuddy(input);
            row->addWidget(caption, 1);
            row->addWidget(input);
            value = [input] { return input->value(); };
            setValue = [input](int number) { input->setValue(number); };
        } else {
            auto *input = new QCheckBox(title, container);
            row->addWidget(input, 1);
            value = [input] { return int(input->isChecked()); };
            setValue = [input](int number) { input->setChecked(number != 0); };
        }
        auto *save = new QPushButton(QStringLiteral("Сохранить"), container);
        save->setAccessibleName(QStringLiteral("Сохранить настройку «%1»").arg(title));
        row->addWidget(save);
        content->addWidget(container);
        container->hide();
        connect(save, &QPushButton::clicked, dialog, [this, dialog, status, name, value] {
            if (!writable() || !dialog->property("pendingSetting").toString().isEmpty())
                return;
            dialog->setProperty("pendingSetting", name);
            status->setText(QStringLiteral("Сохраняем…"));
            ui::setLoadingIndicator(status, true);
            m_backend->updateNotificationSetting(name, value());
        });
        connect(m_backend, &Backend::notificationSettingsLoaded, container,
                [this, dialog, container, name, value, setValue](const QVariantMap &confirmed, const QString &updated) {
            const auto current = confirmed.value(name);
            container->setVisible(current.isValid());
            container->setEnabled(writable());
            const auto previous = container->property("confirmed");
            const bool awaiting = dialog->property("pendingSetting").toString() == name;
            if (current.isValid() && (updated == name || (!awaiting && (!previous.isValid() || previous.toInt() == value()))))
                setValue(current.toInt());
            container->setProperty("confirmed", current);
        });
    };
    addSetting(QStringLiteral("Без звука"), QStringLiteral("soundMuted"), false);
    addSetting(QStringLiteral("Общая громкость"), QStringLiteral("soundVolumeGeneral"), true);
    addSetting(QStringLiteral("Громкость сообщений"), QStringLiteral("soundVolumeMessages"), true);
    addSetting(QStringLiteral("На компьютере"), QStringLiteral("notifySchemeSendSite"), false);
    addSetting(QStringLiteral("По почте"), QStringLiteral("notifySchemeSendEmail"), false);
    addSetting(QStringLiteral("В мобильном приложении"), QStringLiteral("notifySchemeSendPush"), false);
    connect(m_backend, &Backend::notificationSettingsLoaded, dialog,
            [this, dialog, status, retry](const QVariantMap &confirmed, const QString &updated) {
        const auto pending = dialog->property("pendingSetting").toString();
        if (!pending.isEmpty() && pending != updated)
            return;
        dialog->setProperty("pendingSetting", QString());
        retry->setVisible(confirmed.isEmpty());
        status->setText(confirmed.isEmpty() ? QStringLiteral("Портал не вернул настройки уведомлений")
            : writable() ? QString() : QStringLiteral("Настройки открыты для чтения"));
        ui::setLoadingIndicator(status, false);
    });
    connect(m_backend, &Backend::failed, dialog, [dialog, status, retry](const QString &action, const QString &reason) {
        const auto kind = parseOperation(action).kind;
        if (kind == OperationKind::NotificationSettingsUpdate ||
            (kind == OperationKind::NotificationSettings && dialog->property("pendingSetting").toString().isEmpty())) {
            dialog->setProperty("pendingSetting", QString());
            status->setText(reason);
            ui::setLoadingIndicator(status, false);
            if (kind == OperationKind::NotificationSettings)
                retry->show();
        }
    });
    ui::addPortalDndControl(content, m_backend, [this] { return writable(); });
    content->addWidget(status);
    content->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->open();
    loadSettings();
}

} // namespace bx
