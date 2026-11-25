#include "ui/workspace_style.h"
#include "ui/workspace_presentation.h"

#include <QDateTime>
#include <QFontDatabase>
#include <QLocale>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTest>

#include <iterator>

class MessageMarkupTest final : public QObject {
    Q_OBJECT

private slots:
    void decodesOfficialMarkup_data();
    void decodesOfficialMarkup();
    void rendersBbCodeQuotesInTaskContent();
    void presentsLeadingAttributedChatQuoteSeparately();
    void timestampMatchesLocalTime();
    void paintsSafeRichContent();
    void keepsEmojiClustersAndMarkup();
    void paintsEmojiInBodiesAndQuotes();
    void paintsAndHitsLargerReactionBadges();
    void paintsRecognizableAttachmentTypesInChatAndTaskRows();
    void renderedPhotoUsesOnlyItsThumbnailHeight();
    void opensLinkOnlyOnClick();
    void clicksRelativeTaskLink();
    void userMentionLinksOnlyValidProfile();
    void detectsLiteralUnicodeUrlsAndSkipsCode();
    void paintsCircularLinkIcon();
    void paintsTaskPreviewLabelsWithStrongerInk();
    void paintsTaskPreviewRowCues();
    void showsPointingCursorOnReadReceipt();
    void keepsBothPaddedLinkCardsClickable();
    void expandsLongMessagesAndTaskPreviewsOnWideChat();
    void omitsOnlyTaskPreviewsFromNotices();

signals:
    void webLinkOpened(const QUrl &url);
};

void MessageMarkupTest::decodesOfficialMarkup_data()
{
    QTest::addColumn<QString>("source");
    QTest::addColumn<QString>("display");
    const auto row = [](const char *name, const char *source, const char *display) {
        QTest::newRow(name) << QString::fromUtf8(source) << QString::fromUtf8(display);
    };
    row("project event", "[USER=1 REPLACE]Александр Константинов[/USER] создал(а) проект",
        "Александр Константинов создал(а) проект");
    row("nested styles", "[b]Bold [i]italic[/i] [u]underlined[/u][/b] [s]strike[/s] "
        "[size=24px]large[/size] [color=#123]color[/color][BR]next",
        "Bold italic underlined strike large color\nnext");
    row("mentions", "[USER=all][/USER] [CHAT=imol|7]Поддержка[/CHAT] "
        "[CONTEXT=chat7/4]Сообщение[/CONTEXT] [SOURCE=1]Источник[/SOURCE]",
        "Все участники Поддержка Сообщение Источник");
    row("links and actions", "[URL]https://example.test/[/URL] [URL=/tasks/]Задачи[/URL] "
        "[SEND]Отправить[/SEND] [PUT=введите][/PUT] [CALL=+1 23]Телефон[/CALL] [PCH=9]Звонок[/PCH]",
        "https://example.test/ Задачи Отправить введите Телефон Звонок");
    row("files", "[DISK=42] [ICON=https://example.test/favicon.png] "
        "[IMG size=medium]https://example.test/picture.png[/IMG] [ATTACH=8]", "Файл Иконка Изображение Вложение");
    row("image description", "[img alt=Blue%20sky size=small][url]https://example.test/p.png[/url][/img]",
        "Изображение · Blue sky");
    row("rating", "[LIKE] [DISLIKE] [RATING=4]", "👍 👎 Оценка 4 из 5");
    row("code", "[CODE]\n [LIST][*] test [B]bold[/B] [/LIST] [/CODE][ICODE]<b>[url]</b>[/ICODE]",
        " [LIST][*] test [B]bold[/B] [/LIST] \n<b>[url]</b>");
    row("list", "[LIST=1 start=5][*]Яблоки[LIST][*]Красные[/LIST][*]Груши[/LIST]",
        "5. Яблоки\n• Красные\n6. Груши");
    row("table", "[TABLE][TR][TH]A[/TH][TH]B[/TH][/TR][TR][TD][b]1[/b][/TD][TD]2[/TD][/TR][/TABLE]",
        "A\tB\n1\t2");
    row("heading and rule", "[H1]Тема[/H1][H2]Подтема[/H2][HR]Строка", "Тема\nПодтема\nСтрока");
    row("arrow quote", ">> Кому[BR]>> Ответ\nДалее", "Кому\nОтвет\nДалее");
    row("reply quote", "------------------------------------------------------\nИмя [11:12] #chat7/1\n"
        "[i]Цитата[/i]\n------------------------------------------------------\nОтвет", "Имя [11:12]\nЦитата\nОтвет");
    row("unknown literal", "[unknown][][][/unknown] [b]unfinished", "[unknown][][][/unknown] [b]unfinished");
    row("invalid values", "[COLOR=red]red[/COLOR] [RATING=9] [USER=hello]name[/USER]",
        "[COLOR=red]red[/COLOR] [RATING=9] [USER=hello]name[/USER]");
    row("action prefix", "/me [b]действие[/b]", "действие");
    row("loud prefix", "/loud сообщение", "сообщение");
}

void MessageMarkupTest::decodesOfficialMarkup()
{
    QFETCH(QString, source);
    QFETCH(QString, display);

    QCOMPARE(bx::workspace::markupPlainText(source), display);
}

void MessageMarkupTest::rendersBbCodeQuotesInTaskContent()
{
    using namespace Qt::StringLiterals;
    using namespace bx::workspace;
    QListWidget description;
    configureRows(&description, Row::TaskContent);
    const auto *item = addMarkupRow(&description, {},
        u"[QUOTE][USER=32]Сергей[/USER] написал:\n[B]Исходный текст[/B]"
         "[quote]Уточнение[/quote][/QUOTE]Источник"_s);
    QCOMPARE(item->text(), u"Сергей написал:\nИсходный текст\nУточнение\nИсточник"_s);
    const auto html = item->data(MarkupHtml).toString();
    QCOMPARE(html.count(u"<blockquote"), 2);
    QVERIFY(html.contains(u"href=\"/company/personal/user/32/\""));
    QVERIFY(html.contains(u"<b>Исходный текст</b>"));

    const auto malicious = parseMessageMarkup(u"[QUOTE]<img src=x>[URL=javascript:alert(1)]ссылка[/URL][/QUOTE]"_s);
    QVERIFY(malicious.html.contains(u"&lt;img"));
    QVERIFY(!malicious.html.contains(u"href="));
    const auto unbalanced = u"[QUOTE]Незакрытая цитата"_s;
    QCOMPARE(markupPlainText(unbalanced), unbalanced);
    const auto mixed = u"[QUOTE]Открытая цитата\n------------------------------------------------------\nТекст[/QUOTE]"_s;
    QVERIFY(!parseMessageMarkup(mixed).html.contains(u"<blockquote"));
}

void MessageMarkupTest::presentsLeadingAttributedChatQuoteSeparately()
{
    using namespace Qt::StringLiterals;
    using namespace bx::workspace;
    const auto divider = u"------------------------------------------------------"_s;
    const auto quote = divider + u"[BR]FrozaSupport [сегодня, 13:54] #chat7/1[BR]"
        u"[B]Цитата[/B] [URL=https://quoted.example.test/]ссылка[/URL][BR]"_s + divider;
    const auto body = u"[B]Ответ[/B] [URL=/company/personal/user/7/tasks/task/view/42/]Задача[/URL]"_s;
    const auto source = quote + u"[BR]"_s + body;
    QListWidget history;
    configureRows(&history, Row::Message);
    QLabel status;
    bx::Message chat;
    chat.id = u"51"_s;
    chat.text = source;
    bx::ui::presentMessages(&history, &status, {chat});
    const auto *item = history.item(0);
    QCOMPARE(item->data(ReplyByline).toString(), u"FrozaSupport"_s);
    QCOMPARE(item->data(ReplyPreview).toString(), u"Цитата ссылка"_s);
    QVERIFY(item->data(ReplyTargetId).toString().isEmpty());
    QCOMPARE(item->data(Detail).toString(), u"Ответ Задача"_s);
    QCOMPARE(item->data(MessageLinks).toStringList(), QStringList{u"/company/personal/user/7/tasks/task/view/42/"_s});
    QCOMPARE(item->data(RawMessageBody).toString(), source);
    QVERIFY(item->text().contains(u"Цитата ссылка"));
    QVERIFY(markupPlainText(source).contains(u"FrozaSupport [сегодня, 13:54]\nЦитата ссылка"));

    chat.replyId = u"32"_s;
    chat.replyAuthor = u"Участник"_s;
    chat.replyText = u"Родная цитата"_s;
    bx::ui::presentMessages(&history, &status, {chat});
    item = history.item(0);
    QCOMPARE(item->data(ReplyTargetId).toString(), u"32"_s);
    QCOMPARE(item->data(ReplyPreview).toString(), u"Родная цитата"_s);
    QVERIFY(item->data(Detail).toString().contains(u"Цитата ссылка"));

    chat.replyAuthor = u"FrozaSupport"_s;
    chat.replyText = u"Цитата ссылка"_s;
    bx::ui::presentMessages(&history, &status, {chat});
    item = history.item(0);
    QCOMPARE(item->data(ReplyTargetId).toString(), u"32"_s);
    QCOMPARE(item->data(ReplyPreview).toString(), u"Цитата ссылка"_s);
    QCOMPARE(item->data(Detail).toString(), u"Ответ Задача"_s);

    chat.replyId.clear();
    chat.replyAuthor.clear();
    chat.replyText.clear();
    chat.text = QString(source).replace(u"[BR]"_s, u"\r\n"_s)
                               .replace(u"FrozaSupport"_s, u"[USER=7]FrozaSupport[/USER]"_s);
    bx::ui::presentMessages(&history, &status, {chat});
    QCOMPARE(history.item(0)->data(ReplyByline).toString(), u"FrozaSupport"_s);

    const auto invalid = QStringList{
        u"До цитаты[BR]"_s + source,
        quote.chopped(divider.size()) + u"[BR]"_s + body,
        divider + u"[BR]Автор [не дата][BR]Цитата[BR]"_s + divider + u"[BR]"_s + body,
        divider + u"[BR]Автор [13:54][BR][QUOTE]Вложенная цитата[/QUOTE][BR]"_s + divider + u"[BR]"_s + body,
        divider + u"[BR]Автор [13:54][BR]Текст "_s + divider + u"[BR]"_s + body,
        quote + u"Текст на той же строке"_s,
        quote,
        quote + u"[BR][BR] "_s,
        u"[CODE]"_s + source + u"[/CODE]"_s,
    };
    for (const auto &markup : invalid) {
        chat.text = markup;
        bx::ui::presentMessages(&history, &status, {chat});
        item = history.item(0);
        QVERIFY(item->data(ReplyByline).toString().isEmpty());
        QCOMPARE(item->data(Detail).toString(), markupPlainText(markup));
    }
}

void MessageMarkupTest::keepsBothPaddedLinkCardsClickable()
{
    using namespace Qt::StringLiterals;
    using namespace bx::workspace;
    QListWidget list;
    auto *delegate = configureRows(&list, Row::Message);
    QStringList opened;
    delegate->setMessageLinkHandler([&opened](const QString &link) { opened.append(link); });
    delegate->setLinkPreviewSource([](const QModelIndex &, const QString &) {
        return bx::MessageLinkPreview{u"Страница"_s, {u"Описание страницы"_s}};
    });
    const auto first = u"https://example.test/first"_s;
    const auto second = u"https://example.test/second"_s;
    auto *item = addMarkupRow(&list, u"Анна"_s, first + u'\n' + second, u"13:59"_s, u"101"_s);
    list.resize(540, 300);
    list.show();
    QCoreApplication::processEvents();
    const auto row = list.visualItemRect(item);
    const auto bubble = delegate->receiptArea(list.model()->index(0, 0), row);
    QCOMPARE(bubble.top() - row.top(), 3);
    QCOMPARE(row.bottom() - bubble.bottom(), 4);

    const auto image = list.viewport()->grab().toImage();
    const int x = bubble.right() - 20;
    const QColor cardFill("#f6faf8");
    QVector<QPair<int, int>> cardRows;
    for (int y = bubble.top(); y <= bubble.bottom(); ++y) {
        if (image.pixelColor(x, y) != cardFill)
            continue;
        if (cardRows.isEmpty() || cardRows.last().second + 1 != y)
            cardRows.append({y, y});
        else
            cardRows.last().second = y;
    }
    QCOMPARE(cardRows.size(), 2);
    QVERIFY(cardRows[1].first - cardRows[0].second >= 6);
    for (const auto &[top, bottom] : cardRows)
        QTest::mouseClick(list.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(x, (top + bottom) / 2));
    QCOMPARE(opened, QStringList({first, second}));
}

void MessageMarkupTest::timestampMatchesLocalTime()
{
    using namespace Qt::StringLiterals;
    const auto expected = QLocale().toString(QDateTime::fromSecsSinceEpoch(1645844720), u"HH:mm"_s);
    QCOMPARE(bx::workspace::markupPlainText(u"[TIMESTAMP=1645844720 FORMAT=SHORT_TIME_FORMAT]"_s), expected);
    const auto unknown = u"[TIMESTAMP=1645844720 FORMAT=UNKNOWN]"_s;
    QCOMPARE(bx::workspace::markupPlainText(unknown), unknown);
}

void MessageMarkupTest::userMentionLinksOnlyValidProfile()
{
    using namespace Qt::StringLiterals;
    using namespace bx::workspace;
    const auto valid = parseMessageMarkup(u"[USER=0019 REPLACE]Анна <b>[/USER]"_s);
    QCOMPARE(valid.plain, u"Анна <b>"_s);
    QVERIFY(valid.html.contains(u"href=\"/company/personal/user/19/\""));
    QVERIFY(valid.html.contains(u"&lt;b&gt;"));
    const auto invalid = parseMessageMarkup(u"[USER=all]Все[/USER] [USER=0]Ноль[/USER]"_s);
    QVERIFY(!invalid.html.contains(u"href="));
}

void MessageMarkupTest::detectsLiteralUnicodeUrlsAndSkipsCode()
{
    using namespace Qt::StringLiterals;
    using namespace bx::workspace;
    const auto link = u"https://example.test/ссылки/日本語"_s;
    const auto markup = parseMessageMarkup(u"Файл (%1). [CODE]https://example.test/code[/CODE]"_s.arg(link));
    QCOMPARE(markup.links, QStringList{QUrl(link).toString(QUrl::FullyEncoded)});
    QVERIFY(!markup.html.contains(u"href=\"https://example.test/code\""));
    QVERIFY(markup.plain.contains(link));
}

void MessageMarkupTest::paintsCircularLinkIcon()
{
    using namespace Qt::StringLiterals;
    using namespace bx::workspace;
    QListWidget list;
    list.resize(460, 240);
    auto *painter = configureRows(&list, Row::Message);
    painter->setLinkPreviewSource([](const QModelIndex &, const QString &) {
        return bx::MessageLinkPreview{u"example.test"_s, {u"/details"_s}};
    });
    auto *item = addMarkupRow(&list, u"Автор"_s, u"https://example.test/details"_s, {}, u"1"_s);
    list.show();
    QVERIFY(QTest::qWaitForWindowExposed(&list));
    const auto image = list.viewport()->grab().toImage();
    const auto row = list.visualItemRect(item).intersected(image.rect());
    const QColor iconBackground("#d9ebe5");
    int pixels = 0;
    for (int y = row.top(); y <= row.bottom(); ++y)
        for (int x = row.left(); x <= row.right(); ++x)
            pixels += image.pixelColor(x, y) == iconBackground ? 1 : 0;
    QVERIFY2(pixels > 120, "The preview should draw the circular link icon");
}

void MessageMarkupTest::paintsTaskPreviewLabelsWithStrongerInk()
{
    using namespace Qt::StringLiterals;
    using namespace bx::workspace;
    QListWidget list;
    const int requestedWidth = qEnvironmentVariableIntValue("B24_TASK_TYPOGRAPHY_WIDTH");
    list.resize(requestedWidth > 0 ? requestedWidth : 520, 360);
    auto *painter = configureRows(&list, Row::Message);
    painter->setLinkPreviewSource([](const QModelIndex &, const QString &) {
        return bx::MessageLinkPreview{u"Восстановить печать"_s,
            {u"Статус · Ждёт выполнения"_s, u"Постановщик · Мадина Дациева"_s, u"Исполнитель · Александр Пушкин"_s,
             u"Описание · Заменить принтер · восстановить печать"_s, u"Проект · IT sysadmin"_s},
             bx::MessageLinkPreviewKind::Task};
    });
    auto *item = addMarkupRow(&list, u"Автор"_s, u"[URL=/tasks/task/view/42/]Задача[/URL]"_s, {}, u"1"_s);
    list.show();
    QVERIFY(QTest::qWaitForWindowExposed(&list));
    const auto image = list.viewport()->grab().toImage();
    const auto row = list.visualItemRect(item).intersected(image.rect());
    const QColor cardColor("#f6faf8");
    int cardTop = -1;
    for (int y = row.top(); y <= row.bottom() && cardTop < 0; ++y)
        if (image.pixelColor(row.left() + 205, y) == cardColor)
            cardTop = y;
    QVERIFY(cardTop >= 0);
    int strongPixels = 0;
    for (int y = cardTop + 29; y < qMin(cardTop + 113, row.bottom()); ++y)
        for (int x = row.left() + 85; x < row.left() + 190; ++x)
            strongPixels += image.pixelColor(x, y) == Ink ? 1 : 0;
    QVERIFY(strongPixels > 20);
    const auto screenshot = qEnvironmentVariable("B24_TASK_TYPOGRAPHY_SCREENSHOT");
    if (!screenshot.isEmpty())
        QVERIFY(image.save(screenshot));
}

void MessageMarkupTest::paintsTaskPreviewRowCues()
{
    using namespace Qt::StringLiterals;
    using namespace bx::workspace;
    QListWidget list;
    const int requestedWidth = qEnvironmentVariableIntValue("B24_TASK_ROW_CUES_WIDTH");
    list.resize(requestedWidth > 0 ? requestedWidth : 520, 360);
    auto *painter = configureRows(&list, Row::Message);
    const QColor creatorTone("#bb5328");
    const QColor assigneeTone("#8249bd");
    const QColor stageTone("#3a8c68");
    const QColor projectTone("#e5a53d");
    QPixmap creatorPhoto(24, 24);
    creatorPhoto.fill(creatorTone);
    QPixmap assigneePhoto(24, 24);
    assigneePhoto.fill(assigneeTone);
    painter->setLinkAvatarSource([&](const QUrl &url) -> const QPixmap * {
        if (url.path() == u"/creator"_s)
            return &creatorPhoto;
        if (url.path() == u"/assignee"_s)
            return &assigneePhoto;
        return nullptr;
    });
    painter->setLinkPreviewSource([&](const QModelIndex &, const QString &) {
        bx::MessageLinkPreview preview{u"Восстановить печать"_s,
            {u"Статус · Ждёт выполнения"_s, u"Стадия · Проверка"_s,
             u"Постановщик · Мадина Дациева"_s, u"Исполнитель · Александр Пушкин"_s,
             u"Описание · Заменить принтер и вернуть пакетную печать"_s, u"Проект · IT sysadmin"_s},
             bx::MessageLinkPreviewKind::Task};
        bx::Project project{u"44"_s, u"IT sysadmin"_s, {}};
        project.avatarColor = projectTone.name();
        project.avatarType = u"folder"_s;
        preview.project = project;
        preview.creatorAvatarUrl = QUrl(u"https://example.test/creator"_s);
        preview.assigneeAvatarUrl = QUrl(u"https://example.test/assignee"_s);
        preview.stageColor = stageTone.name().mid(1);
        return preview;
    });
    auto *item = addMarkupRow(&list, u"Автор"_s, u"[URL=/tasks/task/view/42/]Задача[/URL]"_s, {}, u"1"_s);
    list.show();
    QVERIFY(QTest::qWaitForWindowExposed(&list));
    const auto image = list.viewport()->grab().toImage();
    const auto visible = list.visualItemRect(item).intersected(image.rect());
    const auto paintColors = [&](const QColor &color) {
        int pixels = 0;
        for (int y = visible.top(); y <= visible.bottom(); ++y)
            for (int x = visible.left() + 90; x <= visible.right(); ++x)
                pixels += image.pixelColor(x, y) == color ? 1 : 0;
        return pixels;
    };
    QVERIFY(paintColors(creatorTone) > 50);
    QVERIFY(paintColors(assigneeTone) > 50);
    QVERIFY(paintColors(stageTone) > 30);
    QVERIFY(paintColors(projectTone) > 20);
    const auto screenshot = qEnvironmentVariable("B24_TASK_ROW_CUES_SCREENSHOT");
    if (!screenshot.isEmpty())
        QVERIFY(image.save(screenshot));
}

void MessageMarkupTest::showsPointingCursorOnReadReceipt()
{
    using namespace Qt::StringLiterals;
    using namespace bx::workspace;
    QListWidget list;
    list.resize(430, 220);
    auto *painter = configureRows(&list, Row::Message);
    QString clicked;
    painter->setMessageViewersHandler([&clicked](const QString &id, const QPoint &) { clicked = id; });
    auto *item = addMarkupRow(&list, u"Автор"_s, u"Отправленное сообщение"_s, u"12:00"_s, u"701"_s);
    item->setData(Outgoing, true);
    item->setData(MessageSeen, true);
    item->setData(ReceiptMessageId, u"701"_s);
    list.show();
    QVERIFY(QTest::qWaitForWindowExposed(&list));
    const auto bubble = painter->receiptArea(list.model()->index(0, 0), list.visualItemRect(item));
    const QPoint doubleCheck(bubble.right() - 14, bubble.bottom() - 9);
    QTest::mouseMove(list.viewport(), doubleCheck);
    QCOMPARE(list.viewport()->cursor().shape(), Qt::PointingHandCursor);
    QTest::mouseClick(list.viewport(), Qt::LeftButton, Qt::NoModifier, doubleCheck);
    QCOMPARE(clicked, u"701"_s);
    item->setData(MessageSeen, false);
    QTest::mouseMove(list.viewport(), doubleCheck - QPoint(1, 0));
    QCOMPARE(list.viewport()->cursor().shape(), Qt::ArrowCursor);
    clicked.clear();
    QTest::mouseClick(list.viewport(), Qt::LeftButton, Qt::NoModifier, doubleCheck);
    QVERIFY(clicked.isEmpty());
}

void MessageMarkupTest::expandsLongMessagesAndTaskPreviewsOnWideChat()
{
    using namespace Qt::StringLiterals;
    using namespace bx::workspace;
    QListWidget list;
    list.resize(1157, 630);
    auto *painter = configureRows(&list, Row::Message);
    auto *shortMessage = addMarkupRow(&list, {}, u"Да, проверю сегодня"_s, u"19:28"_s, u"1"_s);
    const auto text = u"По одному артикулу могут приходить детали разных поставщиков. "
                      "Эту поставку не могли выдать две недели, пока не сверили коробки на складе. "
                      "Проверим похожие случаи и уточним, на каком этапе в программе разошлись данные. "
                      "После проверки приложим накладную к задаче и обновим комментарий."_s;
    auto *forwarded = addMarkupRow(&list, {}, text, u"19:28"_s, u"2"_s);
    forwarded->setData(ForwardByline, u"Переслано · Сергей Киричук"_s);
    auto *quoted = addMarkupRow(&list, {}, u"Проверил"_s, u"19:28"_s, u"3"_s);
    quoted->setData(ReplyByline, u"Сергей Киричук"_s);
    quoted->setData(ReplyPreview, text);
    auto *attachment = addMarkupRow(&list, {}, {}, u"19:28"_s, u"4"_s);
    bx::MessageFile file;
    file.name = u"График отпусков.xlsx"_s;
    attachment->setData(MessageFiles, QVariant::fromValue(QVector<bx::MessageFile>{file}));
    const auto taskLink = u"/tasks/task/view/42/"_s;
    auto *task = addMarkupRow(&list, {}, u"[URL=/tasks/task/view/42/]Задача[/URL]"_s, {}, u"5"_s);
    auto *web = addMarkupRow(&list, {}, u"[URL=https://example.test/]Сайт[/URL]"_s, {}, u"6"_s);
    painter->setLinkPreviewSource([&](const QModelIndex &, const QString &link) {
        if (link == taskLink)
            return bx::MessageLinkPreview{u"Сверить данные по поступившим коробкам и восстановить документы"_s,
                {u"Статус · Ждёт выполнения"_s, u"Исполнитель · Александр Пушкин"_s,
                 u"Описание · Сверить коробки с накладной, уточнить расхождения и обновить комментарий задачи"_s,
                 u"Проект · Поступления"_s}, bx::MessageLinkPreviewKind::Task};
        return bx::MessageLinkPreview{u"Сайт"_s, {u"example.test"_s}, bx::MessageLinkPreviewKind::External};
    });
    QString clicked;
    painter->setMessageLinkHandler([&](const QString &link) { clicked = link; });
    list.show();
    QVERIFY(QTest::qWaitForWindowExposed(&list));
    const auto bubble = [&](QListWidgetItem *item) {
        return painter->receiptArea(list.indexFromItem(item), list.visualItemRect(item));
    };
    const int wideShort = bubble(shortMessage).width();
    const int wideForward = bubble(forwarded).width();
    const int wideForwardHeight = bubble(forwarded).height();
    const int wideFile = bubble(attachment).width();
    const int wideTask = bubble(task).width();
    const int wideWeb = bubble(web).width();
    QVERIFY(wideForward > 700 && wideForward <= 780);
    QVERIFY(bubble(quoted).width() > 570);
    QVERIFY(wideTask > 455 && wideTask < wideForward);
    QVERIFY(wideFile <= 344 && wideWeb <= 344);
    const auto taskCard = bubble(task);
    const QPoint linkTarget(taskCard.right() - 18, taskCard.bottom() - 20);
    QTest::mouseMove(list.viewport(), linkTarget);
    QCOMPARE(list.viewport()->cursor().shape(), Qt::PointingHandCursor);
    QTest::mouseClick(list.viewport(), Qt::LeftButton, Qt::NoModifier, linkTarget);
    QCOMPARE(clicked, taskLink);
    const auto screenshot = qEnvironmentVariable("B24_CHAT_WIDTH_SCREENSHOT");
    if (!screenshot.isEmpty())
        QVERIFY(list.grab().save(screenshot + u"-wide.png"_s));

    list.resize(400, 750);
    list.doItemsLayout();
    QTRY_VERIFY(bubble(forwarded).width() <= list.viewport()->width() * 80 / 100);
    QVERIFY(bubble(forwarded).height() > wideForwardHeight);
    QCOMPARE(bubble(shortMessage).width(), wideShort);
    QCOMPARE(bubble(attachment).width(), wideFile);
    QVERIFY(bubble(task).width() < wideTask && bubble(web).width() <= wideWeb);
    for (auto *item : {shortMessage, forwarded, quoted, attachment, task, web}) {
        list.scrollToItem(item, QAbstractItemView::EnsureVisible);
        QVERIFY(list.viewport()->rect().contains(bubble(item)));
    }
    if (!screenshot.isEmpty()) {
        list.scrollToTop();
        QVERIFY(list.grab().save(screenshot + u"-narrow.png"_s));
    }
}

void MessageMarkupTest::omitsOnlyTaskPreviewsFromNotices()
{
    using namespace Qt::StringLiterals;
    using namespace bx::workspace;
    const auto taskLink = u"/tasks/task/view/42/"_s;
    const auto webLink = u"https://example.test/"_s;
    const auto text = u"[URL=%1]Задача[/URL] и [URL=%2]сайт[/URL]"_s.arg(taskLink, webLink);
    QListWidget list;
    list.resize(600, 440);
    auto *painter = configureRows(&list, Row::Notice);
    const auto makeNotice = [&] {
        auto *item = addMarkupRow(&list, u"Уведомление"_s, text, {}, u"1"_s);
        item->setData(NoticeActions, QStringList{u"Открыть сообщение"_s, u"Задача #42"_s});
        return item;
    };
    auto *mixed = makeNotice();
    auto *externalOnly = makeNotice();
    painter->setLinkPreviewSource([&](const QModelIndex &index, const QString &link) {
        if (link != taskLink)
            return bx::MessageLinkPreview{u"Сайт"_s, {u"example.test"_s}, bx::MessageLinkPreviewKind::External};
        return index.row() == 0
            ? bx::MessageLinkPreview{u"Карточка задачи"_s, {u"Статус · Открыта"_s}, bx::MessageLinkPreviewKind::Task}
            : bx::MessageLinkPreview{};
    });
    QString openedLink;
    int openedAction = -1;
    painter->setMessageLinkHandler([&](const QString &link) { openedLink = link; });
    painter->setNoticeActionHandler([&](const QModelIndex &, int action) { openedAction = action; });
    list.show();
    QVERIFY(QTest::qWaitForWindowExposed(&list));
    const auto row = list.visualItemRect(mixed);
    QCOMPARE(row.height(), list.visualItemRect(externalOnly).height());
    const auto index = list.indexFromItem(mixed);
    const auto card = painter->receiptArea(index, row);
    const auto textLink = card.topLeft() + QPoint(28, 40);
    QCOMPARE(painter->anchorAt(index, row, textLink), taskLink);
    QTest::mouseClick(list.viewport(), Qt::LeftButton, Qt::NoModifier, textLink);
    QCOMPARE(openedLink, taskLink);

    QPoint externalPreview;
    for (int y = card.top(); y <= card.bottom(); ++y) {
        const QPoint point(card.center().x(), y);
        const auto link = painter->linkAt(index, row, point);
        QVERIFY(link != taskLink);
        if (link == webLink)
            externalPreview = point;
    }
    QVERIFY(!externalPreview.isNull());
    QTest::mouseClick(list.viewport(), Qt::LeftButton, Qt::NoModifier, externalPreview);
    QCOMPARE(openedLink, webLink);
    QTest::mouseClick(list.viewport(), Qt::LeftButton, Qt::NoModifier,
                      QPoint(card.left() + 40, card.bottom() - 10));
    QCOMPARE(openedAction, 0);
}

void MessageMarkupTest::paintsSafeRichContent()
{
    using namespace Qt::StringLiterals;
    using namespace bx::workspace;

    QListWidget list;
    list.resize(430, 350);
    configureRows(&list, Row::Message);
    auto *item = addMarkupRow(&list, u"Автор"_s,
        u"[H1]Заголовок[/H1][B]<script>[/B] [IMG]https://example.test/picture.png[/IMG] [URL=javascript:alert(1)]Click[/URL]"_s,
        u"12:03"_s, u"4"_s);
    const auto html = item->data(MarkupHtml).toString();
    QVERIFY(html.contains(u"<h1>Заголовок</h1>"));
    QVERIFY(html.contains(u"<b>&lt;script&gt;</b>"));
    QVERIFY(!html.contains(u"example.test"));
    QVERIFY(!html.contains(u"javascript:"));
    QVERIFY(!item->text().contains(u"[H1]"));
    QVERIFY(list.sizeHintForRow(0) > 55);
    list.show();
    QVERIFY(QTest::qWaitForWindowExposed(&list));
    QVERIFY(!list.grab().isNull());
}

void MessageMarkupTest::keepsEmojiClustersAndMarkup()
{
    using namespace Qt::StringLiterals;
    using namespace bx::workspace;
    const auto text = u"1. 🤝 👩🏽‍💻 🇷🇺 #️⃣ ⚙️ ☎︎ текст"_s;
    const auto ranges = messageEmojiFormats(text);
    QStringList graphemes;
    for (const auto &range : ranges)
        graphemes.append(text.mid(range.start, range.length));
    QCOMPARE(graphemes, QStringList({u"🤝"_s, u"👩🏽‍💻"_s, u"🇷🇺"_s, u"#️⃣"_s, u"⚙️"_s}));

    QTextDocument document;
    document.setHtml(u"<a href='/tasks/'>Ссылка 🤝</a>"_s);
    applyMessageEmojiFonts(document);
    QTextCursor cursor(&document);
    cursor.setPosition(2);
    const auto plain = cursor.charFormat();
    cursor.setPosition(document.characterCount() - 1);
    const auto emoji = cursor.charFormat();
    QCOMPARE(plain.anchorHref(), u"/tasks/"_s);
    QCOMPARE(emoji.anchorHref(), plain.anchorHref());
    QVERIFY(!plain.fontFamilies().toStringList().contains(u"Noto Color Emoji"_s));
    QCOMPARE(emoji.fontFamilies().toStringList(), emojiFontFamilies());
}

void MessageMarkupTest::paintsEmojiInBodiesAndQuotes()
{
    using namespace Qt::StringLiterals;
    using namespace bx::workspace;
    const auto families = QFontDatabase::families();
    if (!families.contains(u"Noto Color Emoji"_s) || !families.contains(u"DejaVu Sans"_s))
        QSKIP("Noto Color Emoji and DejaVu Sans are required for the native color rendering check");
    const auto originalFont = QApplication::font();
    const auto restoreFont = qScopeGuard([originalFont] { QApplication::setFont(originalFont); });
    auto font = originalFont;
    font.setFamily(u"DejaVu Sans"_s);
    font.setStyleStrategy(QFont::NoFontMerging);
    QApplication::setFont(font);

    QListWidget list;
    list.resize(460, 340);
    configureRows(&list, Row::Message);
    auto *plain = addMarkupRow(&list, u"Автор"_s, u"Помогите 😀"_s, {}, u"1"_s);
    auto *rich = addMarkupRow(&list, u"Автор"_s, u"[B]Готово[/B] 🤝"_s, {}, u"2"_s);
    auto *reply = addMarkupRow(&list, u"Автор"_s, u"Спасибо"_s, {}, u"3"_s);
    reply->setData(ReplyByline, u"Получатель"_s);
    reply->setData(ReplyPreview, u"Помогите 🤝"_s);
    list.show();
    QVERIFY(QTest::qWaitForWindowExposed(&list));
    const auto image = list.viewport()->grab().toImage();
    for (auto *item : {plain, rich, reply}) {
        const auto rect = list.visualItemRect(item);
        const auto dpr = image.devicePixelRatio();
        const auto pixels = QRectF(rect.topLeft() * dpr, rect.size() * dpr).toAlignedRect().intersected(image.rect());
        int goldPixels = 0;
        for (int y = pixels.top(); y <= pixels.bottom(); ++y) {
            for (int x = pixels.left(); x <= pixels.right(); ++x) {
                const auto rgb = image.pixel(x, y);
                if (qRed(rgb) > 190 && qGreen(rgb) > 95 && qBlue(rgb) < 95)
                    ++goldPixels;
            }
        }
        QVERIFY2(goldPixels > 4, qPrintable(item->data(Id).toString()));
    }
}

void MessageMarkupTest::renderedPhotoUsesOnlyItsThumbnailHeight()
{
    using namespace Qt::StringLiterals;
    using namespace bx::workspace;
    QListWidget list;
    list.resize(560, 540);
    configureRows(&list, Row::Message);
    bx::MessageFile photo;
    photo.name = u"pictured.png"_s;
    photo.previewUrl = QUrl(u"https://example.test/preview.png"_s);
    photo.downloadUrl = QUrl(u"https://example.test/original.png"_s);
    photo.previewSize = QSize(240, 120);
    photo.isImage = true;
    bx::MessageFile file;
    file.name = u"release.zip"_s;
    file.downloadUrl = QUrl(u"https://example.test/archive.zip"_s);
    auto *main = addMarkupRow(&list, {}, {}, u"15:20"_s, u"1"_s);
    main->setData(MessageFiles, QVariant::fromValue(QVector<bx::MessageFile>{file}));
    auto *quoted = addMarkupRow(&list, {}, u"Ответ"_s, u"15:21"_s, u"2"_s);
    quoted->setData(ReplyByline, u"Ответ на сообщение"_s);
    quoted->setData(ReplyTargetId, u"1"_s);
    auto replyPhoto = photo;
    replyPhoto.name = u"reply-photo.png"_s;
    replyPhoto.downloadUrl = QUrl(u"https://example.test/reply-original.png"_s);
    auto *painter = static_cast<RowPainter *>(list.itemDelegate());
    QList<QUrl> menus, viewed, directDownloads;
    QStringList replyTargets;
    painter->setMessagePhotoMenuHandler([&menus](const QModelIndex &, const bx::MessageFile &file, const QRect &) {
        menus.append(file.downloadUrl);
    });
    painter->setMessagePhotoHandler([&viewed](const bx::MessageFile &file) { viewed.append(file.downloadUrl); });
    painter->setMessageDownloadHandler([&directDownloads](const bx::MessageFile &file) {
        directDownloads.append(file.downloadUrl);
    });
    painter->setReplyNavigation([&replyTargets](const QString &id) { replyTargets.append(id); });
    QPixmap pixmap;
    painter->setMessagePreviewSource([&](const QUrl &url) { return url == photo.previewUrl ? &pixmap : nullptr; });
    list.show();
    QVERIFY(QTest::qWaitForWindowExposed(&list));
    const auto savedHeight = [&](QListWidgetItem *item) {
        return painter->sizeHint({}, list.model()->index(list.row(item), 0)).height();
    };
    const auto mainWithoutPhoto = savedHeight(main);
    const auto quotedWithoutPhoto = savedHeight(quoted);
    main->setData(MessageFiles, QVariant::fromValue(QVector<bx::MessageFile>{photo, file}));
    quoted->setData(ReplyFiles, QVariant::fromValue(QVector<bx::MessageFile>{replyPhoto}));
    pixmap = QPixmap(photo.previewSize);
    pixmap.fill(QColor("#528eb1"));
    const auto index = list.model()->index(0, 0);
    const int mainWidth = painter->receiptArea(index, list.visualItemRect(main)).width() - 20;
    constexpr int thumbnailGap = 5;
    QCOMPARE(savedHeight(main) - mainWithoutPhoto,
             bx::ui::chatPreviewDisplaySize(photo.previewSize, mainWidth).height() + thumbnailGap);
    const auto compactQuoteHeight = savedHeight(quoted);
    QVERIFY(compactQuoteHeight > quotedWithoutPhoto);
    QVERIFY(compactQuoteHeight - quotedWithoutPhoto < bx::ui::chatPreviewDisplaySize(photo.previewSize, mainWidth).height());
    replyPhoto.previewSize = QSize(80, 200);
    quoted->setData(ReplyFiles, QVariant::fromValue(QVector<bx::MessageFile>{replyPhoto}));
    QCOMPARE(savedHeight(quoted), compactQuoteHeight);
    list.doItemsLayout();
    const auto image = list.viewport()->grab().toImage();
    const auto shot = qEnvironmentVariable("B24_PHOTO_ATTACHMENT_SCREENSHOT");
    if (!shot.isEmpty())
        QVERIFY(image.save(shot));
    const auto previewBounds = [&](const QListWidgetItem *item) {
        QRect painted;
        const auto row = list.visualItemRect(item).intersected(image.rect());
        for (int y = row.top(); y <= row.bottom(); ++y)
            for (int x = row.left(); x <= row.right(); ++x)
                if (image.pixelColor(x, y) == QColor("#528eb1"))
                    painted = painted.united(QRect(x, y, 1, 1));
        return painted;
    };
    const auto mainBounds = previewBounds(main);
    QVERIFY(mainBounds.width() >= 70 && mainBounds.height() >= 54);
    const auto menuPoint = mainBounds.topRight() + QPoint(-14, 16);
    QTest::mouseMove(list.viewport(), menuPoint);
    QCOMPARE(list.viewport()->cursor().shape(), Qt::PointingHandCursor);
    QTest::mouseClick(list.viewport(), Qt::LeftButton, {}, menuPoint);
    QTest::mouseClick(list.viewport(), Qt::LeftButton, {}, mainBounds.center());
    const auto compactBounds = previewBounds(quoted);
    QVERIFY(compactBounds.width() >= 32 && compactBounds.height() >= 32);
    QVERIFY(compactBounds.width() < mainBounds.width() && compactBounds.height() < mainBounds.height());
    QTest::mouseClick(list.viewport(), Qt::LeftButton, {}, compactBounds.center());
    QCOMPARE(replyTargets, QStringList{u"1"_s});
    QCOMPARE(menus, QList<QUrl>{photo.downloadUrl});
    QCOMPARE(viewed, menus);
    QVERIFY(directDownloads.isEmpty());
    const auto archiveCard = previewBounds(main).bottomLeft() + QPoint(16, 25);
    QTest::mouseClick(list.viewport(), Qt::LeftButton, {}, archiveCard);
    QCOMPARE(directDownloads, QList<QUrl>{file.downloadUrl});
}

void MessageMarkupTest::paintsRecognizableAttachmentTypesInChatAndTaskRows()
{
    using namespace Qt::StringLiterals;
    using namespace bx::workspace;
    struct Sample { QString name; QColor ink; };
    const Sample samples[]{
        {u"Android.APK"_s, QColor("#345d31")}, {u"bundle.aab"_s, QColor("#345d31")},
        {u"release.zip"_s, QColor("#79531f")}, {u"backup.TAR.GZ"_s, QColor("#79531f")},
        {u"package.7z"_s, QColor("#79531f")}, {u"bundle.RAR"_s, QColor("#79531f")},
        {u"guide.pdf"_s, QColor("#8e3735")}, {u"report.docx"_s, QColor("#275479")},
        {u"totals.xlsx"_s, QColor("#28604a")}, {u"summary.pptx"_s, QColor("#755074")},
        {u"photo.webp"_s, QColor("#216171")}, {u"track.flac"_s, QColor("#5d4b87")},
        {u"recording.mp4"_s, QColor("#864a37")}, {u"main.cpp"_s, QColor("#395776")},
        {u"setup.exe"_s, QColor("#344e60")}, {u"unknown.custom"_s, QColor("#4c6463")},
    };
    constexpr int cellWidth = 330;
    constexpr int cellHeight = 96;
    QImage grid(cellWidth * 2, cellHeight * 8, QImage::Format_RGB32);
    grid.fill(QColor("#e8f3ee"));
    QPainter canvas(&grid);
    QVector<QByteArray> silhouettes;

    for (int number = 0; number < int(std::size(samples)); ++number) {
        const auto &sample = samples[number];
        QListWidget list;
        const auto style = number % 2 == 0 ? Row::Message : Row::TaskContent;
        configureRows(&list, style);
        auto *item = addMarkupRow(&list, {}, {}, style == Row::Message ? u"15:20"_s : QString(), u"1"_s);
        item->setData(Outgoing, style == Row::Message);
        bx::MessageFile file;
        file.name = sample.name;
        file.size = 251904;
        file.downloadUrl = QUrl(u"https://example.test/download"_s);
        item->setData(MessageFiles, QVariant::fromValue(QVector<bx::MessageFile>{file}));
        auto *painter = static_cast<RowPainter *>(list.itemDelegate());
        const auto index = list.model()->index(0, 0);
        const auto origin = QPoint(number % 2 * cellWidth, number / 2 * cellHeight);
        QStyleOptionViewItem option;
        option.initFrom(&list);
        option.rect = QRect(origin + QPoint(12, 0), QSize(cellWidth - 24, 0));
        option.rect.setHeight(painter->sizeHint(option, index).height());
        QVERIFY(QRect(origin, QSize(cellWidth, cellHeight)).contains(option.rect));
        painter->paint(&canvas, option, index);
        const auto bubble = painter->receiptArea(index, option.rect);
        const int iconLeft = bubble.left() + 16;
        QByteArray mask;
        int inkPixels = 0;
        for (int y = option.rect.top(); y <= option.rect.bottom(); ++y) {
            QByteArray scanline;
            for (int x = iconLeft; x < iconLeft + 34; ++x) {
                const auto color = grid.pixelColor(x, y);
                const bool ink = qAbs(color.red() - sample.ink.red()) + qAbs(color.green() - sample.ink.green())
                    + qAbs(color.blue() - sample.ink.blue()) < 20;
                scanline.append(ink ? '1' : '0');
                inkPixels += ink;
            }
            if (scanline.contains('1'))
                mask += scanline;
        }
        QVERIFY2(inkPixels >= 10, qPrintable(sample.name));
        silhouettes.append(mask);
    }
    canvas.end();
    QCOMPARE(silhouettes[0], silhouettes[1]);
    for (int archive = 3; archive <= 5; ++archive)
        QCOMPARE(silhouettes[2], silhouettes[archive]);
    QVERIFY(silhouettes[0] != silhouettes[2]);
    for (int number = 7; number < silhouettes.size(); ++number)
        QVERIFY2(silhouettes[number] != silhouettes[number - 1], qPrintable(samples[number].name));

    const auto screenshot = qEnvironmentVariable("B24_ATTACHMENT_TYPE_SCREENSHOT");
    if (!screenshot.isEmpty())
        QVERIFY(grid.save(screenshot));
}

void MessageMarkupTest::paintsAndHitsLargerReactionBadges()
{
    using namespace Qt::StringLiterals;
    using namespace bx::workspace;
    QListWidget list;
    list.resize(315, 290);
    configureRows(&list, Row::Message);
    auto *plain = addMarkupRow(&list, {}, u"Да"_s, u"11:18"_s, u"1"_s);
    auto *reacted = addMarkupRow(&list, {}, u"Да"_s, u"11:18"_s, u"2"_s);
    for (auto *item : {plain, reacted}) {
        item->setData(Outgoing, true);
        item->setData(ReceiptMessageId, item->data(Id));
    }
    reacted->setData(MessageReactionCounts, QVariantMap{{u"like"_s, 1024}});
    reacted->setData(OwnMessageReactions, QStringList{u"like"_s});
    auto *painter = static_cast<RowPainter *>(list.itemDelegate());
    QStringList actions;
    painter->setReactionActions([&actions](const QModelIndex &, const QString &code, const QPoint &) {
        actions.append(code);
    }, [] { return true; });

    list.show();
    QVERIFY(QTest::qWaitForWindowExposed(&list));
    const auto row = list.visualItemRect(reacted);
    const auto index = list.model()->index(list.row(reacted), 0);
    const auto bubble = painter->receiptArea(index, row);
    // The larger chip's top area remains clickable and the outgoing delivery stays separate.
    const auto badge = bubble.bottomLeft() + QPoint(21, -21);
    QTest::mouseMove(list.viewport(), badge);
    QCOMPARE(list.viewport()->cursor().shape(), Qt::PointingHandCursor);
    QStyleOptionViewItem option;
    option.rect = row;
    QHelpEvent tooltip(QEvent::ToolTip, badge, list.viewport()->mapToGlobal(badge));
    QVERIFY(painter->helpEvent(&tooltip, &list, option, index));
    QVERIFY(QToolTip::text().contains(u"Снять реакцию «Нравится»"_s));
    QToolTip::hideText();
    QTest::mouseClick(list.viewport(), Qt::LeftButton, Qt::NoModifier, badge);
    QCOMPARE(actions, QStringList{u"like"_s});
    QTest::mouseClick(list.viewport(), Qt::LeftButton, Qt::NoModifier, bubble.bottomRight() + QPoint(-15, -9));
    QCOMPARE(actions.size(), 1);
    QVERIFY(row.height() >= list.visualItemRect(plain).height() + 6);

    if (QFontDatabase::families().contains(u"Noto Color Emoji"_s)) {
        const auto image = list.viewport()->grab().toImage();
        const auto dpr = image.devicePixelRatio();
        const auto icon = QRectF(QPointF(bubble.left() + 10, badge.y() - 1) * dpr, QSizeF(24, 22) * dpr)
                              .toAlignedRect().intersected(image.rect());
        int coloredRows = 0;
        for (int y = icon.top(); y <= icon.bottom(); ++y) {
            for (int x = icon.left(); x <= icon.right(); ++x) {
                const auto rgb = image.pixel(x, y);
                if (qRed(rgb) > 190 && qGreen(rgb) > 95 && qBlue(rgb) < 95) {
                    ++coloredRows;
                    break;
                }
            }
        }
        QVERIFY(coloredRows >= qRound(11 * dpr));
    }
}

void MessageMarkupTest::opensLinkOnlyOnClick()
{
    using namespace Qt::StringLiterals;
    using namespace bx::workspace;

    QListWidget list;
    list.resize(430, 350);
    configureRows(&list, Row::Message);
    const auto destination = u"https://example.test/task/42?a=1&b=2"_s;
    auto *item = addMarkupRow(&list, u"Автор"_s, u"[URL=%1]Задача[/URL]"_s.arg(destination), {}, u"4"_s);
    list.show();
    QVERIFY(QTest::qWaitForWindowExposed(&list));
    QSignalSpy received(this, &MessageMarkupTest::webLinkOpened);
    QDesktopServices::setUrlHandler(u"https"_s, this, "webLinkOpened");
    QVERIFY(!list.grab().isNull());
    const auto row = list.visualItemRect(item);
    QTest::mouseMove(list.viewport(), row.topLeft() + QPoint(56, 31));
    QCOMPARE(list.viewport()->cursor().shape(), Qt::PointingHandCursor);
    QTest::mouseMove(list.viewport(), row.topLeft() + QPoint(16, 14));
    QCOMPARE(list.viewport()->cursor().shape(), Qt::ArrowCursor);
    QTest::mouseClick(list.viewport(), Qt::LeftButton, Qt::NoModifier, row.topLeft() + QPoint(16, 14));
    const auto beforeLinkClick = received.size();
    QTest::mouseClick(list.viewport(), Qt::LeftButton, Qt::NoModifier, row.topLeft() + QPoint(56, 31));
    QDesktopServices::unsetUrlHandler(u"https"_s);
    QCOMPARE(beforeLinkClick, 0);
    QCOMPARE(received.size(), 1);
    QCOMPARE(received.takeFirst().constFirst().toUrl(), QUrl(destination));
}

void MessageMarkupTest::clicksRelativeTaskLink()
{
    using namespace Qt::StringLiterals;
    using namespace bx::workspace;

    QListWidget list;
    list.resize(430, 350);
    configureRows(&list, Row::Message);
    const auto href = u"/workgroups/group/7/tasks/task/view/501/"_s;
    auto *item = addMarkupRow(&list, u"Сергей Ульвич"_s,
        u"Сергей Ульвич создал [URL=%1]задачу[/URL] на основании сообщения"_s.arg(href),
        u"16:30"_s, u"4"_s);
    const auto html = item->data(MarkupHtml).toString();
    QVERIFY(html.contains(u"href=\"%1\""_s.arg(href)));

    list.show();
    QVERIFY(QTest::qWaitForWindowExposed(&list));
    auto *painter = static_cast<RowPainter *>(list.itemDelegate());
    QVERIFY(painter);
    QStringList clicked;
    painter->setMessageLinkHandler([&clicked](const QString &target) { clicked.append(target); });

    const auto index = list.model()->index(list.row(item), 0);
    const auto row = list.visualItemRect(item);
    QPoint target;
    for (int y = row.top(); y < row.bottom() && target.isNull(); ++y) {
        for (int x = row.left(); x < row.right(); ++x) {
            if (!painter->anchorAt(index, row, QPoint(x, y)).isEmpty()) {
                target = QPoint(x, y);
                break;
            }
        }
    }
    QVERIFY(!target.isNull());
    QTest::mouseClick(list.viewport(), Qt::LeftButton, Qt::NoModifier, target);
    QCOMPARE(clicked.size(), 1);
    QCOMPARE(clicked.constFirst(), href);

    const auto reference = bx::ui::taskReference(QUrl(u"https://example.bitrix24.ru%1"_s.arg(href)));
    QCOMPARE(reference.projectId, u"7"_s);
    QCOMPARE(reference.taskId, u"501"_s);
    const auto legacy = bx::ui::taskReference(QUrl(u"https://example.bitrix24.ru/tasks/?task_id=502"_s));
    QCOMPARE(legacy.taskId, u"502"_s);
    QVERIFY(legacy.projectId.isEmpty());
    for (const auto &path : {u"/tasks/task/view/501/"_s,
                            u"/company/personal/user/19/tasks/task/view/501/"_s,
                            u"/extranet/tasks/task/view/501/"_s,
                            u"/extranet/company/personal/user/19/tasks/task/view/501/"_s}) {
        const auto personal = bx::ui::taskReference(QUrl(u"https://example.bitrix24.ru%1"_s.arg(path)));
        QCOMPARE(personal.taskId, u"501"_s);
        QVERIFY(personal.projectId.isEmpty());
    }
    const auto extranet = bx::ui::taskReference(QUrl(u"https://example.bitrix24.ru/extranet%1"_s.arg(href)));
    QCOMPARE(extranet.taskId, u"501"_s);
    QCOMPARE(extranet.projectId, u"7"_s);
    QVERIFY(bx::ui::taskReference(QUrl(u"https://example.bitrix24.ru/x%1"_s.arg(href))).taskId.isEmpty());
    QVERIFY(bx::ui::taskReference(QUrl(u"https://example.bitrix24.ru/tasks-extra/?task_id=502"_s)).taskId.isEmpty());
    QVERIFY(bx::ui::taskReference(QUrl(u"https://example.bitrix24.ru/tasks/?task_id=0"_s)).taskId.isEmpty());
}

QTEST_MAIN(MessageMarkupTest)
#include "tst_message_markup.moc"
