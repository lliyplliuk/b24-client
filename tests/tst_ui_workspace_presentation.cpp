#include "ui/workspace_directory.h"
#include "ui/workspace_presentation.h"

#include <QImage>
#include <QPainter>
#include <QTest>

class WorkspacePresentationTest final : public QObject {
    Q_OBJECT

private slots:
    void paintsRowWithinBounds_data()
    {
        using bx::workspace::Row;
        QTest::addColumn<Row>("style");
        QTest::newRow("directory") << Row::Directory;
        QTest::newRow("message") << Row::Message;
        QTest::newRow("task-content") << Row::TaskContent;
        QTest::newRow("notice") << Row::Notice;
        QTest::newRow("card") << Row::Card;
    }

    void paintsRowWithinBounds()
    {
        using namespace bx::workspace;
        QFETCH(Row, style);
        QListWidget list;
        auto *delegate = configureRows(&list, style);
        auto *item = addRow(&list, QStringLiteral("1111"), QStringLiteral("Текст строки"),
                            QStringLiteral("12:34"), QStringLiteral("1"));
        const auto index = list.indexFromItem(item);
        QStyleOptionViewItem option;
        option.initFrom(&list);
        const auto size = delegate->sizeHint(option, index);
        QVERIFY(size.height() > 0);
        QCOMPARE(size.width(), 0);
        const int margin = 8;
        option.rect = QRect(margin, margin, list.viewport()->width(), size.height());
        QImage background(option.rect.width() + 2 * margin, size.height() + 2 * margin,
                          QImage::Format_ARGB32_Premultiplied);
        background.fill(Qt::transparent);
        const auto render = [&] {
            auto image = background;
            QPainter painter(&image);
            delegate->paint(&painter, option, index);
            painter.end();
            return image;
        };

        auto image = render();
        QVERIFY(image != background);
        item->setData(Title, QStringLiteral("8888"));
        QVERIFY(image.copy(option.rect) != render().copy(option.rect));
        QPainter eraseRow(&image);
        eraseRow.setCompositionMode(QPainter::CompositionMode_Clear);
        eraseRow.fillRect(option.rect, Qt::transparent);
        eraseRow.end();
        QCOMPARE(image, background);
    }

    void selectsDirectoryEntriesAndSkipsPlaceholder()
    {
        QListWidget list;
        bx::workspace::emptyRow(&list, QStringLiteral("Нет чатов"));
        bx::selectAvailable(&list, nullptr);
        QVERIFY(!list.currentItem());

        list.clear();
        bx::Chat chat;
        chat.id = QStringLiteral("chat1");
        chat.title = QStringLiteral("Команда");
        auto *first = bx::ui::addChatRow(&list, chat);
        chat.id = QStringLiteral("chat2");
        auto *second = bx::ui::addChatRow(&list, chat);

        bx::selectAvailable(&list, nullptr);
        QCOMPARE(list.currentItem(), first);
        bx::selectAvailable(&list, second);
        QCOMPARE(list.currentItem(), second);
    }

    void clearsChatBadgesWhenUnreadStatusChanges()
    {
        QListWidget list;
        bx::Chat chat;
        chat.unread = 2;
        chat.pinned = true;
        chat.hasMention = true;
        auto *item = bx::ui::addChatRow(&list, chat);
        QCOMPARE(item->data(Qt::AccessibleDescriptionRole).toString(),
                 QStringLiteral("Закреплён. Непрочитанных: 2. Непрочитанное упоминание"));

        bx::ui::setChatRowBadges(item, bx::Chat{});
        QCOMPARE(item->data(bx::workspace::Count).toInt(), 0);
        QVERIFY(!item->data(bx::workspace::Pinned).toBool());
        QVERIFY(!item->data(bx::workspace::HasMention).toBool());
        QVERIFY(item->data(Qt::AccessibleDescriptionRole).toString().isEmpty());
    }
};

QTEST_MAIN(WorkspacePresentationTest)
#include "tst_ui_workspace_presentation.moc"
