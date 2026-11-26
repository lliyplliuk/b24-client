#include "ui/workspace_theme.h"

namespace bx::workspace {

QString styleSheet()
{
    return QStringLiteral(R"(
        QMainWindow, QWidget#workspace { background: #f4f4ef; color: #182d3b; }
        QWidget#rail { background: #193742; }
        QLabel#brand { color: #e8f6f0; background: #31525e; border-radius: 9px;
                       font-size: 14px; font-weight: 700; }
        QLabel#mode { color: #b2c8ce; font-size: 14px; }
        QLabel#mode[online="true"] { color: #b8ded2; }
        QListWidget, QPlainTextEdit { background: transparent; color: #182d3b; border: 0; }
        QListWidget#sections { color: #dce8ec; outline: 0; }
        QWidget#directory { background: #f6f7f1; border-right: 1px solid #dbe6e0; }
        QTabBar#chatScopeTabs::tab, QTabBar#projectScopeTabs::tab {
            color: #42606a; background: transparent; border: 1px solid transparent;
            min-width: 38px; padding: 6px 5px; margin: 0 2px 2px 0; border-radius: 6px;
            font-weight: 600;
        }
        QTabBar#chatScopeTabs::tab:hover, QTabBar#projectScopeTabs::tab:hover { background: #ebf1ee; }
        QTabBar#chatScopeTabs::tab:selected, QTabBar#projectScopeTabs::tab:selected {
            color: #215c70; background: #e1efec; border-color: #d3e4df;
        }
        QTabBar#chatScopeTabs::tab:focus, QTabBar#projectScopeTabs::tab:focus { border-color: #215c70; }
        QTabBar#chatScopeTabs QLabel#chatScopeUnreadBadge {
            color: white; background: #aa4c58; border-radius: 8px; font-size: 14px; font-weight: 700;
        }
        QSplitter#chatDirectorySplitter::handle:hover { background: #83aba5; }
        QWidget#conversationHeader { background: #fffefa; border-bottom: 1px solid #d9e5df; }
        QLabel#conversationTitle { color: #182d3b; font-size: 18px; font-weight: 650; }
        QWidget#conversationSearch { background: #f6f9f4; border-bottom: 1px solid #d9e5df; }
        QFrame#chatComposer { background: #fffefa; border-top: 1px solid #d2e2da; }
        QPlainTextEdit#chatEditor { background: #fffefa; padding: 4px 5px;
                                   border: 1px solid transparent; border-radius: 7px;
                                   selection-background-color: #215c70;
                                   selection-color: #fffefa; }
        QPlainTextEdit#chatEditor:focus { border-color: #8fb6aa; }
        QWidget#reading, QWidget#taskPane, QWidget#taskDetail { background: #fffefa; }
        QWidget#taskPane { border: 1px solid #dce4e4; border-radius: 10px; }
        QWidget#column { background: #e8edeb; border-radius: 10px; }
        QLabel { color: #182d3b; }
        QLabel#pageTitle { font-size: 22px; font-weight: 650; }
        QLabel#sectionTitle { font-size: 18px; font-weight: 650; }
        QLabel#columnTitle { font-weight: 650; }
        QLabel#quiet, QLabel#status { color: #516674; font-size: 14px; }
        QLabel#empty { color: #516674; }
        QLabel#stageCount { color: #355568; background: #dce5e4; border-radius: 8px; font-size: 14px; }
        QPushButton { color: #294b59; background: #f5f8f5; border: 1px solid #c8d5d4; border-radius: 7px; padding: 6px 12px; font-weight: 600; }
        QPushButton:hover { background: #e7f0ef; }
        QPushButton:focus { border-color: #215c70; }
        QPushButton#primary { color: white; background: #215c70; border-color: #215c70; }
        QPushButton#primary:hover { background: #194d5f; }
        QPushButton#primary:focus { border-color: #a8d9e5; }
        QPushButton:disabled, QPushButton#primary:disabled { color: #50626a; background: #e6edec; border-color: #d3dedc; }
        QScrollArea { background: transparent; border: 0; }
        QSplitter::handle { background: #dce4e4; }
        QScrollBar:vertical { width: 9px; background: transparent; margin: 2px; }
        QScrollBar:horizontal { height: 9px; background: transparent; margin: 2px; }
        QScrollBar::handle { background: #9fb3b8; border-radius: 3px; min-height: 24px; min-width: 24px; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
        QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
    )");
}

} // namespace bx::workspace
