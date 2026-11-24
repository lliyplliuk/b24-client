#pragma once

#include <QString>
#include <QVector>

namespace bx::ui {

struct ChatReply {
    QString id;
    QString author;
    QString preview;
};

struct ChatMention {
    int start;
    QString id;
    QString name;

    bool operator==(const ChatMention &other) const
    {
        return start == other.start && id == other.id && name == other.name;
    }
};

struct ChatDraft {
    QString text;
    ChatReply reply;
    QVector<ChatMention> mentions;

    bool isEmpty() const { return text.isEmpty() && reply.id.isEmpty(); }
    bool operator==(const ChatDraft &other) const
    {
        return text == other.text && reply.id == other.reply.id && mentions == other.mentions;
    }
};

} // namespace bx::ui
