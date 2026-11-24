#pragma once

#include <QRegularExpression>
#include <QStringList>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>

#include <algorithm>

namespace bx::workspace {

inline const QStringList &emojiFontFamilies()
{
    static const QStringList families{QStringLiteral("Noto Color Emoji"), QStringLiteral("Segoe UI Emoji"),
                                      QStringLiteral("Apple Color Emoji")};
    return families;
}

inline QList<QTextLayout::FormatRange> messageEmojiFormats(QStringView text)
{
    if (std::none_of(text.cbegin(), text.cend(), [](QChar unit) { return unit.unicode() >= 0x2000; }))
        return {};
    static const QRegularExpression emoji(QString::fromLatin1(
        R"((?=\p{Emoji_Presentation}(?!\x{FE0E})|\p{Emoji}\x{FE0F}|[#*0-9]\x{FE0F}?\x{20E3})\X)"));
    static const auto format = [] {
        QTextCharFormat result;
        result.setFontFamilies(emojiFontFamilies());
        return result;
    }();

    QList<QTextLayout::FormatRange> formats;
    for (auto matches = emoji.globalMatch(text); matches.hasNext();) {
        const auto match = matches.next();
        formats.append({int(match.capturedStart()), int(match.capturedLength()), format});
    }
    return formats;
}

inline void applyMessageEmojiFonts(QTextDocument &document)
{
    QTextCursor cursor(&document);
    for (auto block = document.begin(); block.isValid(); block = block.next()) {
        for (const auto &range : messageEmojiFormats(block.text())) {
            const int position = block.position() + range.start;
            cursor.setPosition(position);
            cursor.setPosition(position + range.length, QTextCursor::KeepAnchor);
            cursor.mergeCharFormat(range.format);
        }
    }
}

} // namespace bx::workspace
