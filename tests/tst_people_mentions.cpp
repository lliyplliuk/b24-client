#include "ui/chat_widgets.h"

#include <QApplication>
#include <QTest>

using namespace Qt::StringLiterals;

class PeopleMentionsTest final : public QObject {
    Q_OBJECT

private slots:
    void triggersAndDraft();
    void normalPlusAndEmail();
    void plainPlusAndInitialPeopleCanCoexist();
    void taskPeoplePickerSelectsAndRejectsStaleResult();
};

void PeopleMentionsTest::triggersAndDraft()
{
    QWidget window;
    bx::ui::ChatComposer chat(&window, u"Текст"_s, u"Эмодзи"_s, u"Файл"_s, u"Отправить"_s);
    bx::ui::ChatComposer task(&window, u"Текст задачи"_s, u"Эмодзи"_s, u"Файл"_s, u"Отправить"_s);
    window.resize(500, 400);
    chat.setGeometry(0, 0, 480, 100);
    task.setGeometry(0, 150, 480, 100);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const auto exercise = [&](bx::ui::ChatComposer &composer, char trigger) {
        QString request, query;
        composer.setMentionSearchHandler([&](const QString &id, const QString &text) {
            request = id;
            query = text;
        });
        composer.input()->setFocus();
        QTest::keyClicks(composer.input(), QString(QLatin1Char(trigger)) + u"An"_s);
        QTRY_COMPARE(query, u"An"_s);
        composer.setMentionResults(u"stale"_s, {{u"999"_s, u"wrong"_s}});
        composer.setMentionResults(request, {{u"38"_s, u"Anna [B]"_s}});
        QKeyEvent select(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QVERIFY(composer.handleMentionKey(&select));
        QCOMPARE(composer.wireText().trimmed(), u"[USER=38]Anna B[/USER]"_s);
        const auto draft = composer.draft();
        composer.setDraft({});
        QVERIFY(composer.wireText().isEmpty());
        composer.setDraft(draft);
        QCOMPARE(composer.wireText().trimmed(), u"[USER=38]Anna B[/USER]"_s);
        composer.setMentionSearchHandler({});
    };
    exercise(chat, '@');
    exercise(task, '+');
}

void PeopleMentionsTest::normalPlusAndEmail()
{
    QWidget window;
    bx::ui::ChatComposer composer(&window, u"Текст"_s, u"Эмодзи"_s, u"Файл"_s, u"Отправить"_s);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QApplication::setActiveWindow(&window);
    int count = 0;
    composer.setMentionSearchHandler([&](const QString &, const QString &) { ++count; });
    composer.input()->setFocus();
    QTRY_VERIFY(composer.input()->hasFocus());
    const auto text = u"foo@one.test and +123 plus 1+2"_s;
    QTest::keyClicks(composer.input(), text);
    QTest::qWait(260);
    QCOMPARE(count, 0);
    QCOMPARE(composer.wireText(), text);
}

void PeopleMentionsTest::plainPlusAndInitialPeopleCanCoexist()
{
    QWidget window;
    window.resize(500, 350);
    bx::ui::ChatComposer composer(&window, u"Текст"_s, u"Эмодзи"_s, u"Файл"_s, u"Отправить"_s);
    QString request;
    composer.setMentionSearchHandler([&](const QString &id, const QString &query) {
        QVERIFY(query.isEmpty());
        request = id;
    });
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QApplication::setActiveWindow(&window);
    composer.input()->setFocus();
    QTRY_VERIFY(composer.input()->hasFocus());
    QTest::keyClicks(composer.input(), u"+"_s);
    QTRY_VERIFY(!request.isEmpty());
    composer.setMentionResults(request, {{u"91"_s, u"Ирина"_s}});
    QKeyEvent select(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QVERIFY(!composer.handleMentionKey(&select));
    QCOMPARE(composer.wireText(), u"+"_s);
    QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QVERIFY(composer.handleMentionKey(&down));
    QVERIFY(composer.handleMentionKey(&select));
    QCOMPARE(composer.wireText().trimmed(), u"[USER=91]Ирина[/USER]"_s);
    auto cursor = composer.input()->textCursor();
    cursor.setPosition(2);
    cursor.deleteChar();
    QCOMPARE(composer.wireText().trimmed(), u"@Иина"_s);
}

void PeopleMentionsTest::taskPeoplePickerSelectsAndRejectsStaleResult()
{
    QWidget window;
    window.resize(500, 350);
    QLineEdit anchor(&window);
    anchor.setGeometry(10, 150, 300, 35);
    bx::ui::PeopleSuggestions picker(&anchor);
    picker.attachToInput(&anchor);
    QString request, chosen;
    picker.setSearchHandler([&](const QString &id, const QString &) { request = id; });
    picker.setOnChosen([&](const bx::Person &person) { chosen = person.id; });
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QApplication::setActiveWindow(&window);
    anchor.setFocus();
    QTRY_VERIFY(anchor.hasFocus());
    QTest::keyClicks(&anchor, u"Ir"_s);
    QTRY_VERIFY(!request.isEmpty());
    const QVector<bx::Person> results{{u"91"_s, u"Ирина"_s}};
    picker.setResults(request, results);
    auto *list = picker.findChild<QListWidget *>();
    QVERIFY(list && list->item(0));
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, list->visualItemRect(list->item(0)).center());
    QCOMPARE(chosen, u"91"_s);
    QVERIFY(!picker.isVisible());
    picker.setResults(request, results);
    QVERIFY(!picker.isVisible());
}

QTEST_MAIN(PeopleMentionsTest)
#include "tst_people_mentions.moc"
