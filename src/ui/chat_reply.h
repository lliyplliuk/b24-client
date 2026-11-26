#pragma once

#include <QImage>
#include <QString>
#include <QVector>

#include <memory>

class QTemporaryDir;

namespace bx::ui {

inline constexpr qsizetype kMessageCharacters = 20000;

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

struct ChatDraftImage {
    std::shared_ptr<QTemporaryDir> directory;
    QString path;
    QImage thumbnail;
};

struct ChatDraft {
    QString text;
    ChatReply reply;
    QVector<ChatMention> mentions;
    std::shared_ptr<const ChatDraftImage> image;

    bool isEmpty() const { return text.isEmpty() && reply.id.isEmpty() && !image; }
    bool operator==(const ChatDraft &other) const
    {
        return text == other.text && reply.id == other.reply.id && mentions == other.mentions && image == other.image;
    }
};

} // namespace bx::ui
