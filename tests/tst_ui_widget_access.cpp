#include "ui/chat_composer.h"
#include "ui/history_list.h"
#include "ui/task_card_dialog.h"
#include "ui/task_description_editor.h"

#include <QTest>

class UiWidgetAccessTest final : public QObject {
    Q_OBJECT

private slots:
    void taskCardExposesItsEditorsAndHistory()
    {
        bx::ui::TaskCardDialog card(bx::Task{}, {}, nullptr);
        QVERIFY(card.composer());
        QVERIFY(card.messageInput());
        QCOMPARE(card.messageInput(), card.composer()->input());
        QVERIFY(dynamic_cast<bx::ui::HistoryList *>(card.messageList()));
        QVERIFY(card.descriptionEditor());
    }
};

QTEST_MAIN(UiWidgetAccessTest)
#include "tst_ui_widget_access.moc"
