#pragma once

#include <QString>

// Numeric parameters that are NOT fixed by the brief. They are collected here
// so that every one of them can be reviewed/approved in one place.
// Each value below is a *proposal* (see docs/verification.md, section
// "Limits to confirm"), never a value silently invented at a call site.
namespace bx::tunables {

// Maximum number of entities fetched per on-demand request (bounded data).
inline constexpr int kPageSizeChats = 50;
inline constexpr int kPageSizeMessages = 100;
inline constexpr int kRetainedChatMessages = 2 * kPageSizeMessages;
inline constexpr int kPageSizeProjects = 50;
inline constexpr int kPageSizeBoardTasks = 200;
inline constexpr int kPageSizeNotifications = 100;

// Bound automatic attachment media fetches and locally opened videos.
inline constexpr qint64 kChatVideoPreviewBytes = 16LL * 1024 * 1024;
inline constexpr qint64 kChatVideoPlaybackBytes = 64LL * 1024 * 1024;
inline constexpr qint64 kChatClipboardPngBytes = 16LL * 1024 * 1024;
inline constexpr qsizetype kChatImageCaptionCharacters = 20000;

// Push&Pull reconnect policy: attempt counter and delay growth.
inline constexpr int kReconnectMaxAttempts = 10;
inline constexpr int kReconnectBaseDelayMs = 1000;
inline constexpr int kReconnectMaxDelayMs = 30000;

} // namespace bx::tunables
