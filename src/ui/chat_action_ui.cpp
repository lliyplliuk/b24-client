#include "ui/main_window.h"
#include "ui/task_dialogs.h"
#include "ui/reaction_palette.h"
#include "ui/message_menu_window.h"

#include "ui/workspace_presentation.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>
#include <array>
#include <memory>
#include <utility>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPersistentModelIndex>
#include <QRegularExpression>
#include <QSet>
#include <QUrlQuery>

namespace bx {

QWidget *MainWindow::messageActionOwner(const QAbstractItemModel *model, const QString &dialog)
{
    if (!model || dialog.isEmpty())
        return nullptr;
    if (m_card && m_card->isVisible() && m_card->taskChatDialogId() == dialog && m_card->messageList()->model() == model)
        return m_card;
    if ((!m_card || !m_card->isVisible()) && m_lastSection == 0 && m_currentChatId == dialog
        && m_chatMessages->isVisible() && m_chatMessages->model() == model)
        return this;
    return nullptr;
}

std::function<QPersistentModelIndex()> MainWindow::messageActionTarget(const QPersistentModelIndex &message,
                                                                      const QString &dialog)
{
    const QPointer<QAbstractItemModel> model(const_cast<QAbstractItemModel *>(message.model()));
    const auto id = message.data(workspace::ReceiptMessageId).toString();
    return [this, model, dialog, message, id, account = m_backend->settingsScope()] {
        if (account != m_backend->settingsScope() || !messageActionOwner(model, dialog))
            return QPersistentModelIndex{};
        return message.isValid() && message.data(workspace::ReceiptMessageId).toString() == id
            ? message : ui::messageIndex(model, id);
    };
}

void MainWindow::recordConfirmedReaction(const QString &dialog, const QString &id, const QString &code)
{
    if (m_reactionAccountScope == m_backend->settingsScope()
        && m_pendingReactionAdds.remove(Backend::reactionOperationKey(dialog, id, code)))
        incrementReactionFrequency(code);
}

void MainWindow::actOnMessageReaction(const QPersistentModelIndex &message, const QString &dialog, const QString &code,
                                      const QPoint &anchor, QWidget *parent)
{
    const auto id = message.data(workspace::ReceiptMessageId).toString();
    if (!writable() || m_backend->isDemo() || !message.isValid() || id.isEmpty() || dialog.isEmpty()
        || message.data(workspace::SystemMessage).toBool())
        return;
    const auto own = message.data(workspace::OwnMessageReactions).toStringList();
    if (!code.isEmpty()) {
        const bool remove = own.contains(code);
        const auto account = m_backend->settingsScope();
        if (account != m_reactionAccountScope) {
            m_pendingReactionAdds.clear();
            m_reactionAccountScope = account;
        }
        if (!remove && !account.isEmpty() && ui::findReaction(code)) {
            const auto key = Backend::reactionOperationKey(dialog, id, code);
            constexpr qsizetype maxPendingReactions = 32;
            if (!m_pendingReactionAdds.contains(key) && m_pendingReactionAdds.size() >= maxPendingReactions)
                m_pendingReactionAdds.erase(m_pendingReactionAdds.begin());
            m_pendingReactionAdds.insert(key);
        }
        m_backend->changeMessage(dialog, id, remove ? MessageAction::RemoveReaction : MessageAction::React, code);
        return;
    }
    auto *owner = messageActionOwner(message.model(), dialog);
    if (!owner || (parent && owner != parent))
        return;
    auto *history = owner == m_card ? m_card->messageList() : m_chatMessages;
    auto *palette = new ui::ReactionPalette(own, ui::rankedReactionChoices(reactionFrequency()), history);
    connect(palette, &QMenu::aboutToHide, palette, &QObject::deleteLater);
    connect(m_backend, &Backend::accountScopeChanged, palette, &QMenu::close);
    const auto resolve = messageActionTarget(message, dialog);
    connect(message.model(), &QAbstractItemModel::modelReset, palette, [palette, resolve] {
        QTimer::singleShot(0, palette, [palette, resolve] { if (!resolve().isValid()) palette->close(); });
    });
    palette->setOnChosen([this, resolve, dialog](const QString &selected) {
        actOnMessageReaction(resolve(), dialog, selected, {}, nullptr);
    });
    ui::showMessageMenuWindow(*palette, QRect(anchor, QSize(1, 1)));
}

void MainWindow::addMessageActions(QMenu &menu, const QPersistentModelIndex &message,
                                   const QString &dialog, bool taskChat, const QString &href)
{
    const auto resolve = messageActionTarget(message, dialog);
    connect(m_backend, &Backend::accountScopeChanged, &menu, &QMenu::close);
    QVector<QUrl> links;
    const auto candidates = href.isEmpty() ? message.data(workspace::MessageLinks).toStringList() : QStringList{href};
    for (const auto &candidate : candidates) {
        const auto url = messageLinkDestination(candidate);
        if (!url.isEmpty() && !links.contains(url))
            links.append(url);
    }
    const auto copyLink = [this, resolve](QMenu *parent, const QString &label, const QUrl &url) {
        connect(parent->addAction(label), &QAction::triggered, this,
                [this, resolve, url] {
            const auto links = resolve().data(workspace::MessageLinks).toStringList();
            if (std::any_of(links.cbegin(), links.cend(), [this, &url](const QString &href) {
                return messageLinkDestination(href) == url;
            }))
                QGuiApplication::clipboard()->setText(url.toString(QUrl::FullyEncoded));
        });
    };
    if (links.size() == 1) {
        copyLink(&menu, QStringLiteral("Скопировать ссылку"), links.first());
    } else if (links.size() > 1) {
        auto *choices = menu.addMenu(QStringLiteral("Скопировать ссылку"));
        for (const auto &url : std::as_const(links)) {
            const auto label = url.toDisplayString().left(160).replace(QLatin1Char('&'), QStringLiteral("&&"));
            copyLink(choices, label, url);
        }
    }
    const auto id = message.data(workspace::ReceiptMessageId).toString();
    if (auto *owner = messageActionOwner(message.model(), dialog)) {
        auto *list = owner == m_card ? m_card->messageList() : m_chatMessages;
        const auto *painter = static_cast<const workspace::RowPainter *>(list->itemDelegate());
        const auto photos = painter->photoOriginals(message);
        if (!photos.isEmpty()) {
            const auto label = QStringLiteral("Скачать оригинал");
            auto *choices = photos.size() > 1 ? menu.addMenu(label) : &menu;
            for (const auto &file : photos) {
                auto *download = choices->addAction(photos.size() == 1 ? label
                    : QString(file.name).replace(QLatin1Char('&'), QStringLiteral("&&")));
                connect(download, &QAction::triggered, this, [this, resolve, dialog, file] {
                    const auto current = resolve();
                    if (auto *parent = messageActionOwner(current.model(), dialog);
                        parent && workspace::RowPainter::hasOriginal(current, file))
                        saveMessageFile(file, parent);
                });
            }
        }
    }
    const auto portal = m_backend->portalOrigin();
    if (id.isEmpty() || dialog.isEmpty() || portal.host().isEmpty() || message.data(workspace::SystemMessage).toBool())
        return;

    connect(menu.addAction(QStringLiteral("Скопировать ссылку на сообщение")), &QAction::triggered, this,
            [portal, dialog, id, taskChat, resolve] {
        if (!resolve().isValid())
            return;
        auto link = portal;
        link.setPath(QStringLiteral("/online/"));
        QUrlQuery query;
        query.addQueryItem(taskChat ? QStringLiteral("IM_TASK") : QStringLiteral("IM_DIALOG"), dialog);
        query.addQueryItem(QStringLiteral("IM_MESSAGE"), id);
        link.setQuery(query);
        QGuiApplication::clipboard()->setText(link.toString(QUrl::FullyEncoded));
    });
    if (!m_backend->isDemo()) {
        connect(menu.addAction(QStringLiteral("Медиа, файлы и ссылки чата")), &QAction::triggered, this,
                [this, dialog, resolve] {
            if (auto *parent = messageActionOwner(resolve().model(), dialog))
                showChatAssets(dialog, parent);
        });
    }
    addPublicLinkPreviewActions(menu, message, resolve);
    if (!writable() || m_backend->isDemo())
        return;

    const auto invoke = [this, dialog, id, resolve](MessageAction action, const QString &value = {}) {
        const auto current = resolve();
        if (writable() && current.isValid() && !current.data(workspace::SystemMessage).toBool())
            m_backend->changeMessage(dialog, id, action, value);
    };
    auto *reactions = menu.addMenu(QStringLiteral("Реакция"));
    const auto own = message.data(workspace::OwnMessageReactions).toStringList();
    const auto counts = message.data(workspace::MessageReactionCounts).toMap();
    const auto choices = ui::rankedReactionChoices(reactionFrequency());
    constexpr qsizetype quickReactionCount = 4;
    for (qsizetype i = 0; i < qMin(quickReactionCount, choices.size()); ++i) {
        const auto *reaction = choices.at(i);
        const auto code = QString::fromLatin1(reaction->code);
        const auto count = counts.value(code).toInt();
        const auto label = QString::fromUtf8(reaction->emoji) + QLatin1Char(' ') + QString::fromUtf8(reaction->caption);
        auto *action = reactions->addAction(count ? QStringLiteral("%1 (%2)").arg(label).arg(count) : label);
        action->setCheckable(true);
        action->setChecked(own.contains(code));
        connect(action, &QAction::triggered, this, [this, resolve, dialog, code] {
            actOnMessageReaction(resolve(), dialog, code, {}, nullptr);
        });
    }
    connect(reactions->addAction(QStringLiteral("Все реакции…")), &QAction::triggered, this,
            [this, resolve, dialog, reactions] {
        const auto anchor = reactions->pos();
        QTimer::singleShot(0, this, [this, resolve, dialog, anchor] {
            const auto message = resolve();
            if (auto *parent = messageActionOwner(message.model(), dialog))
                actOnMessageReaction(message, dialog, {}, anchor, parent);
        });
    });
    for (const auto &code : own) {
        if (!ui::findReaction(code)) {
            connect(reactions->addAction(QStringLiteral("Снять реакцию %1").arg(code)), &QAction::triggered,
                    this, [invoke, code] { invoke(MessageAction::RemoveReaction, code); });
        }
    }

    connect(menu.addAction(QStringLiteral("Переслать…")), &QAction::triggered, this, [this, resolve, dialog, id] {
        if (!writable() || !resolve().isValid())
            return;
        QStringList recipients;
        QHash<QString, QString> recipientIds;
        QSet<QString> seen;
        for (const auto &directory : m_chatDirectories) {
            for (const auto &chat : directory.recent + directory.found) {
                if (chat.id.isEmpty() || seen.contains(chat.id) || (chat.dismissed && !m_showDismissedChats))
                    continue;
                seen.insert(chat.id);
                const auto name = QStringLiteral("%1 · %2").arg(chat.title, chat.id);
                recipients.append(name);
                recipientIds.insert(name, chat.id);
            }
        }
        bool accepted = false;
        const auto choice = QInputDialog::getItem(this, QStringLiteral("Переслать сообщение"),
            QStringLiteral("Получатель или ID диалога"), recipients, 0, true, &accepted).trimmed();
        if (accepted && !choice.isEmpty() && writable() && resolve().isValid())
            m_backend->forwardMessage(dialog, id, recipientIds.value(choice, choice));
    });

    if (!message.data(workspace::SystemMessage).toBool()) {
        connect(menu.addAction(QStringLiteral("Создать задачу из сообщения…")), &QAction::triggered, this,
                [this, resolve, dialog, id] {
            const auto message = resolve();
            if (!writable() || !m_sessionRecovery->isHidden() || !message.isValid())
                return;
            auto *parent = messageActionOwner(message.model(), dialog);
            const auto name = QStringLiteral("createTaskFromMessageConfirm");
            if (!parent || parent->findChild<QMessageBox *>(name))
                return;
            auto preview = message.data(workspace::Detail).toString().simplified();
            if (preview.size() > 500)
                preview = preview.left(500) + QChar(0x2026);
            if (preview.isEmpty()) {
                const auto files = message.data(workspace::MessageFiles).value<QVector<MessageFile>>();
                preview = files.isEmpty() ? QStringLiteral("Сообщение №%1").arg(id)
                                          : files.first().name.simplified().left(500);
            }
            const auto author = message.data(workspace::MessageAuthor).toString().simplified().left(120);
            auto *confirm = new QMessageBox(parent);
            confirm->setObjectName(name);
            confirm->setAttribute(Qt::WA_DeleteOnClose);
            confirm->setWindowTitle(QStringLiteral("Задача из сообщения"));
            confirm->setTextFormat(Qt::PlainText);
            confirm->setText(author.isEmpty() ? preview : author + QLatin1Char('\n') + preview);
            confirm->setInformativeText(QStringLiteral("В этом чате появится запись со ссылкой на задачу."));
            auto *create = confirm->addButton(QStringLiteral("Создать задачу"), QMessageBox::AcceptRole);
            create->setAccessibleName(create->text());
            auto *cancel = confirm->addButton(QStringLiteral("Отмена"), QMessageBox::RejectRole);
            confirm->setDefaultButton(cancel);
            confirm->setEscapeButton(cancel);
            connect(m_backend, &Backend::accountScopeChanged, confirm, &QDialog::reject);
            connect(create, &QPushButton::clicked, confirm,
                    [this, parent, resolve, dialog, id] {
                const auto message = resolve();
                if (writable() && m_sessionRecovery->isHidden() && messageActionOwner(message.model(), dialog) == parent &&
                    message.isValid() &&
                    message.data(workspace::ReceiptMessageId).toString() == id &&
                    !message.data(workspace::SystemMessage).toBool())
                    m_backend->changeMessage(dialog, id, MessageAction::CreateTask);
            });
            confirm->open();
        });
    }

    auto *pins = menu.addMenu(QStringLiteral("Закрепление"));
    connect(pins->addAction(QStringLiteral("Закрепить")), &QAction::triggered,
            this, [invoke] { invoke(MessageAction::Pin); });
    connect(pins->addAction(QStringLiteral("Открепить")), &QAction::triggered,
            this, [invoke] { invoke(MessageAction::Unpin); });
    auto *favorites = menu.addMenu(QStringLiteral("Избранное"));
    connect(favorites->addAction(QStringLiteral("Добавить в избранное")), &QAction::triggered,
            this, [invoke] { invoke(MessageAction::Favorite); });
    connect(favorites->addAction(QStringLiteral("Убрать из избранного")), &QAction::triggered,
            this, [invoke] { invoke(MessageAction::Unfavorite); });

    if (!message.data(workspace::Outgoing).toBool() || message.data(workspace::SystemMessage).toBool())
        return;
    connect(menu.addAction(QStringLiteral("Редактировать…")), &QAction::triggered, this, [this, resolve, dialog, id] {
        const auto message = resolve();
        if (!writable() || !message.isValid() || !message.data(workspace::Outgoing).toBool()
            || message.data(workspace::SystemMessage).toBool())
            return;
        const bool cardHistory = messageActionOwner(message.model(), dialog) == m_card;
        auto *composer = cardHistory ? m_card->composer() : m_chatComposer;
        if (!composer || composer->input()->isReadOnly() || !m_pendingUpload.isEmpty()
            || (cardHistory ? !m_pendingTaskId.isEmpty() : !m_pendingChatId.isEmpty()))
            return;
        composer->beginMessageEdit(dialog, id, message.data(workspace::RawMessageBody).toString(),
                                  [this, resolve, dialog, id](const QString &text) {
            const auto message = resolve();
            if (writable() && message.isValid() && message.data(workspace::Outgoing).toBool()
                && !message.data(workspace::SystemMessage).toBool())
                m_backend->changeMessage(dialog, id, MessageAction::Edit, text);
        });
    });
    connect(menu.addAction(QStringLiteral("Удалить…")), &QAction::triggered, this, [this, resolve, dialog, id] {
        const auto currentOwner = [this, resolve, dialog, id]() -> QWidget * {
            const auto message = resolve();
            if (!writable() || !m_sessionRecovery->isHidden()
                || !message.isValid() || message.data(workspace::ReceiptMessageId).toString() != id
                || !message.data(workspace::Outgoing).toBool() || message.data(workspace::SystemMessage).toBool())
                return nullptr;
            return messageActionOwner(message.model(), dialog);
        };
        QTimer::singleShot(0, this, [this, currentOwner, dialog, id] {
            auto *owner = currentOwner();
            const auto name = QStringLiteral("messageDeleteConfirmation");
            if (!owner || owner->findChild<QMessageBox *>(name))
                return;
            auto *confirm = new QMessageBox(QMessageBox::Warning, QStringLiteral("Удалить сообщение"),
                QStringLiteral("Удалить это сообщение?"), QMessageBox::Yes | QMessageBox::Cancel, owner);
            confirm->setObjectName(name);
            confirm->setAttribute(Qt::WA_DeleteOnClose);
            confirm->setDefaultButton(QMessageBox::Cancel);
            confirm->setEscapeButton(QMessageBox::Cancel);
            confirm->button(QMessageBox::Yes)->setText(QStringLiteral("Удалить"));
            connect(m_backend, &Backend::accountScopeChanged, confirm, &QDialog::reject);
            connect(confirm, &QMessageBox::finished, this,
                    [this, owner = QPointer<QWidget>(owner), currentOwner, dialog, id](int result) {
                if (result == QMessageBox::Yes && owner && currentOwner() == owner)
                    m_backend->changeMessage(dialog, id, MessageAction::Delete);
            });
            confirm->open();
        });
    });
}

void MainWindow::showChatAssets(const QString &scope, QWidget *parent)
{
    if (scope.isEmpty() || m_backend->isDemo())
        return;
    if (m_chatAssetsDialog) {
        if (m_chatAssetsDialog->property("dialog").toString() == scope) {
            m_chatAssetsDialog->raise();
            m_chatAssetsDialog->activateWindow();
            return;
        }
        m_chatAssetsDialog->close();
    }

    auto *dialog = new QDialog(parent);
    m_chatAssetsDialog = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(m_backend, &Backend::accountScopeChanged, dialog, &QDialog::reject);
    dialog->setProperty("dialog", scope);
    dialog->setWindowTitle(QStringLiteral("Вложения чата"));
    dialog->resize(620, 440);
    auto *layout = new QVBoxLayout(dialog);
    auto *tabs = new QTabWidget(dialog);
    tabs->setAccessibleName(QStringLiteral("Вложения чата"));
    layout->addWidget(tabs);

    struct Page {
        QListWidget *list = nullptr;
        QLabel *status = nullptr;
        QPushButton *more = nullptr;
        QVector<ChatAsset> rows;
        QSet<QString> seen;
        QString cursor;
        bool loaded = false;
        bool pending = false;
    };
    const auto pages = std::make_shared<std::array<Page, 3>>();
    constexpr qsizetype maxRows = 200;
    const auto labels = std::array{QStringLiteral("Медиа"), QStringLiteral("Файлы"), QStringLiteral("Ссылки")};
    const auto fetch = [this, pages, scope](int index) {
        auto &page = pages->at(size_t(index));
        if (page.pending || (page.loaded && page.cursor.isEmpty()))
            return;
        page.pending = true;
        page.more->setEnabled(false);
        page.status->setText(QStringLiteral("Загрузка…"));
        ui::setLoadingIndicator(page.status, true);
        m_backend->requestChatAssets(scope, ChatAssetKind(index), page.cursor);
    };
    for (int i = 0; i < int(labels.size()); ++i) {
        auto &page = pages->at(size_t(i));
        auto *tab = new QWidget(tabs);
        auto *content = new QVBoxLayout(tab);
        page.list = new QListWidget(tab);
        page.list->setAccessibleName(labels[size_t(i)]);
        content->addWidget(page.list, 1);
        page.status = new QLabel(tab);
        content->addWidget(page.status);
        page.more = new QPushButton(QStringLiteral("Загрузить ещё"), tab);
        content->addWidget(page.more, 0, Qt::AlignRight);
        page.more->hide();
        connect(page.more, &QPushButton::clicked, dialog, [fetch, i] { fetch(i); });
        connect(page.list, &QListWidget::itemActivated, dialog, [this, pages, dialog, i](const QListWidgetItem *item) {
            const auto &page = pages->at(size_t(i));
            const auto row = page.list->row(item);
            if (row < 0 || row >= page.rows.size())
                return;
            const auto &asset = page.rows.at(row);
            if (!asset.link.isEmpty())
                openMessageLink(asset.link.toString(QUrl::FullyEncoded));
            else if (asset.file.isImage && (!asset.file.previewUrl.isEmpty() || !asset.file.downloadUrl.isEmpty())) {
                QVector<MessageFile> loaded;
                if (i == int(ChatAssetKind::Media)) {
                    for (const auto &candidate : page.rows)
                        if (candidate.link.isEmpty())
                            loaded.append(candidate.file);
                }
                openMessagePhoto(asset.file, dialog, loaded);
            } else
                saveMessageFile(asset.file, dialog);
        });
        tabs->addTab(tab, labels[size_t(i)]);
    }

    connect(m_backend, &Backend::chatAssetsLoaded, dialog,
            [pages, scope, maxRows](const QString &received, ChatAssetKind kind, const QString &cursor,
                                  const QVector<ChatAsset> &assets, const QString &next) {
        const auto index = int(kind);
        if (received != scope || index < 0 || index >= int(pages->size()))
            return;
        auto &page = pages->at(size_t(index));
        if (!page.pending || cursor != page.cursor)
            return;
        page.pending = false;
        page.loaded = true;
        for (const auto &asset : assets) {
            if (page.rows.size() >= maxRows)
                break;
            if (page.seen.contains(asset.id))
                continue;
            page.seen.insert(asset.id);
            page.rows.append(asset);
            auto *item = new QListWidgetItem(workspace::markupPlainText(asset.name).left(250), page.list);
            if (!asset.link.isEmpty())
                item->setToolTip(asset.link.toString(QUrl::FullyEncoded));
        }
        page.cursor = page.rows.size() < maxRows ? next : QString();
        page.status->setText(page.rows.isEmpty() && page.cursor.isEmpty() ? QStringLiteral("Список пуст") : QString());
        ui::setLoadingIndicator(page.status, false);
        page.more->setText(QStringLiteral("Загрузить ещё"));
        page.more->setVisible(!page.cursor.isEmpty());
        page.more->setEnabled(true);
    });
    connect(m_backend, &Backend::failed, dialog, [pages, scope](const QString &op, const QString &reason) {
        for (int i = 0; i < int(pages->size()); ++i) {
            auto &page = pages->at(size_t(i));
            if (!page.pending || op != QStringLiteral("chat-assets:%1:%2:%3").arg(scope).arg(i).arg(page.cursor))
                continue;
            page.pending = false;
            page.status->setText(reason);
            ui::setLoadingIndicator(page.status, false);
            page.more->setText(QStringLiteral("Повторить"));
            page.more->setEnabled(true);
            page.more->show();
        }
    });
    connect(tabs, &QTabWidget::currentChanged, dialog, [pages, fetch](int index) {
        if (index >= 0 && !pages->at(size_t(index)).loaded)
            fetch(index);
    });
    dialog->show();
    fetch(0);
}

void MainWindow::promptGroupChat()
{
    if (!writable())
        return;
    bool accepted = false;
    const auto name = QInputDialog::getText(this, QStringLiteral("Новый групповой чат"),
                                           QStringLiteral("Название"), QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted || name.isEmpty())
        return;
    const auto members = QInputDialog::getText(this, QStringLiteral("Участники нового чата"),
        QStringLiteral("ID сотрудников через запятую"), QLineEdit::Normal, {}, &accepted);
    if (accepted && writable()) {
        static const QRegularExpression separators(QStringLiteral("[,\\s]+"));
        m_backend->createGroupChat(name, members.split(separators, Qt::SkipEmptyParts));
    }
}

void MainWindow::promptChatMember(const QString &dialog, bool add)
{
    if (!writable())
        return;
    bool accepted = false;
    const auto title = add ? QStringLiteral("Добавить участника") : QStringLiteral("Удалить участника");
    const auto id = QInputDialog::getText(this, title, QStringLiteral("ID сотрудника"),
                                        QLineEdit::Normal, {}, &accepted).trimmed();
    if (accepted && !id.isEmpty() && writable())
        m_backend->changeChatMember(dialog, id, add);
}

} // namespace bx
