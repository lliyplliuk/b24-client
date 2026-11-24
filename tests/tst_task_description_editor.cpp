#include "ui/task_description_editor.h"

#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QTest>
#include <QTextEdit>
#include <QToolButton>

class TaskDescriptionEditorTest final : public QObject {
    Q_OBJECT

private slots:
    void retainsExistingDescriptionAndFiles();
    void nativeFormattingAndPaste();
    void clipboardUploadsAtInsertionPoint();
};

void TaskDescriptionEditorTest::retainsExistingDescriptionAndFiles()
{
    using namespace Qt::StringLiterals;
    bx::ui::TaskDescriptionEditor editor;
    auto *input = editor.findChild<QTextEdit *>(u"taskDescriptionInput"_s);
    QVERIFY(input);
    const auto raw = u"[P][B]Главное[/B]  [unknown=7]слово[/unknown]\n[CODE][B]literal[/B][/CODE]"_s
        + u"[disk file id=n1236765 width=600 height=434][/P]"_s;
    editor.setBbCode(raw);
    QCOMPARE(editor.bbCode(), raw);
    QVERIFY(!input->toPlainText().contains(u"disk file"));
    input->moveCursor(QTextCursor::End);
    input->insertPlainText(u" хвост"_s);
    const auto changed = editor.bbCode();
    QVERIFY(changed.contains(u"[b]Главное[/b]"));
    QVERIFY(changed.contains(u"[unknown=7]слово[/unknown]"));
    QVERIFY(changed.contains(u"[CODE][B]literal[/B][/CODE]"));
    QVERIFY(changed.contains(u"[disk file id=n1236765 width=600 height=434]"));
    QVERIFY(changed.endsWith(u" хвост"));
    editor.insertAttachment(u"n246"_s, u"photo.png"_s, QSize(640, 480));
    QVERIFY(editor.bbCode().contains(u"[disk file id=n246 width=640 height=480]"));
    editor.insertAttachment(u"n7]attack"_s, u"bad"_s);
    QVERIFY(!editor.bbCode().contains(u"attack"));
}

void TaskDescriptionEditorTest::nativeFormattingAndPaste()
{
    using namespace Qt::StringLiterals;
    bx::ui::TaskDescriptionEditor editor;
    auto *input = editor.findChild<QTextEdit *>(u"taskDescriptionInput"_s);
    QVERIFY(input);
    editor.setBbCode(u"[list=1][*][i]Один[/i][*][url=https://example.test/]Два[/url][/list]"_s);
    QVERIFY(input->document()->begin().textList());
    input->moveCursor(QTextCursor::End);
    input->insertPlainText(u" и три"_s);
    QVERIFY(editor.bbCode().contains(u"[list=1][*][i]Один[/i][*]"));
    QVERIFY(editor.bbCode().contains(u"[url=https://example.test/]"));
    auto *mime = new QMimeData;
    mime->setHtml(u"<b>bold</b><img src='https://example.test/p.png'>"_s);
    QApplication::clipboard()->setMimeData(mime);
    input->paste();
    QVERIFY(editor.bbCode().contains(u"[b]bold[/b]"));
    QVERIFY(!editor.bbCode().contains(u"p.png"));
}

void TaskDescriptionEditorTest::clipboardUploadsAtInsertionPoint()
{
    using namespace Qt::StringLiterals;
    bx::ui::TaskDescriptionEditor editor;
    auto *input = editor.findChild<QTextEdit *>(u"taskDescriptionInput"_s);
    QVERIFY(input);
    QStringList pending;
    editor.setAttachHandler([&](const QStringList &paths) { pending = paths; });
    QImage pasted;
    editor.setImagePasteHandler([&](const QImage &image) { pasted = image; });
    editor.setBbCode(u"До После"_s);
    auto cursor = input->textCursor();
    cursor.setPosition(3);
    input->setTextCursor(cursor);
    auto *files = new QMimeData;
    files->setUrls({QUrl::fromLocalFile(u"/local/synthetic.pdf"_s)});
    QApplication::clipboard()->setMimeData(files);
    input->paste();
    QCOMPARE(pending, QStringList({u"/local/synthetic.pdf"_s}));
    input->moveCursor(QTextCursor::End);
    input->insertPlainText(u" дополнено"_s);
    editor.insertAttachment(u"n1234"_s, u"synthetic.pdf"_s);
    QCOMPARE(editor.bbCode(), u"До [disk file id=n1234]После дополнено"_s);

    auto *image = new QMimeData;
    QImage expected(2, 3, QImage::Format_RGB32);
    expected.fill(Qt::red);
    image->setImageData(expected);
    QApplication::clipboard()->setMimeData(image);
    input->paste();
    QCOMPARE(pasted, expected);
    editor.insertAttachment(u"n1235"_s, u"снимок.png"_s, expected.size());
    QVERIFY(editor.bbCode().contains(u"[disk file id=n1235 width=2 height=3]"));
}

QTEST_MAIN(TaskDescriptionEditorTest)
#include "tst_task_description_editor.moc"
