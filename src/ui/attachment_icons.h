#pragma once

#include "ui/glyphs.h"

#include <QStringView>
#include <initializer_list>

namespace bx::ui {

struct AttachmentIcon {
    Glyph glyph;
    QColor background;
    QColor ink;
};

inline const AttachmentIcon &attachmentIcon(QStringView name)
{
    static const AttachmentIcon generic{Glyph::File, QColor("#e5eeeb"), QColor("#4c6463")};
    const auto dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0 || dot == name.size() - 1)
        return generic;
    const auto extension = name.mid(dot + 1);
    const auto is = [extension](std::initializer_list<const char *> candidates) {
        for (const auto *candidate : candidates) {
            if (extension.compare(QLatin1String(candidate), Qt::CaseInsensitive) == 0)
                return true;
        }
        return false;
    };
    static const AttachmentIcon archive{Glyph::Archive, QColor("#f5ead9"), QColor("#79531f")};
    static const AttachmentIcon android{Glyph::Android, QColor("#e3efdc"), QColor("#345d31")};
    static const AttachmentIcon pdf{Glyph::Pdf, QColor("#f5e3e2"), QColor("#8e3735")};
    static const AttachmentIcon document{Glyph::Document, QColor("#e4edf5"), QColor("#275479")};
    static const AttachmentIcon sheet{Glyph::Spreadsheet, QColor("#e4efe7"), QColor("#28604a")};
    static const AttachmentIcon slides{Glyph::Presentation, QColor("#f4e8f1"), QColor("#755074")};
    static const AttachmentIcon image{Glyph::Image, QColor("#e0eef0"), QColor("#216171")};
    static const AttachmentIcon audio{Glyph::Audio, QColor("#ede9f5"), QColor("#5d4b87")};
    static const AttachmentIcon video{Glyph::Video, QColor("#f5e9e1"), QColor("#864a37")};
    static const AttachmentIcon code{Glyph::Code, QColor("#e8eef3"), QColor("#395776")};
    static const AttachmentIcon executable{Glyph::Executable, QColor("#e9edf0"), QColor("#344e60")};

    if (is({"zip", "7z", "rar", "tar", "gz", "bz2", "xz", "zst", "tgz", "tbz2", "txz", "cab"}))
        return archive;
    if (is({"apk", "aab"}))
        return android;
    if (is({"pdf"}))
        return pdf;
    if (is({"doc", "docx", "odt", "rtf", "txt", "md", "log"}))
        return document;
    if (is({"xls", "xlsx", "xlsm", "ods", "csv", "tsv"}))
        return sheet;
    if (is({"ppt", "pptx", "odp", "key"}))
        return slides;
    if (is({"png", "jpg", "jpeg", "gif", "webp", "svg", "bmp", "tif", "tiff", "heic", "heif", "avif"}))
        return image;
    if (is({"mp3", "wav", "flac", "aac", "m4a", "ogg", "opus"}))
        return audio;
    if (is({"mp4", "mov", "avi", "mkv", "webm", "mpeg", "mpg", "m4v"}))
        return video;
    if (is({"c", "h", "cc", "cpp", "hpp", "js", "ts", "py", "go", "rs", "java", "kt", "php", "html", "css",
            "json", "xml", "yaml", "yml", "sql"}))
        return code;
    if (is({"exe", "msi", "appimage", "deb", "rpm", "sh", "bat", "cmd", "ps1"}))
        return executable;
    return generic;
}

} // namespace bx::ui
