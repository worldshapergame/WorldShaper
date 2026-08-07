#pragma once
// Minimal levelled logging. Thread-safe, no allocation on the hot path.

#include <format>
#include <string_view>

#include "core/types.hpp"

namespace ws {

enum class LogLevel : u8 { Trace, Debug, Info, Warn, Error, Fatal };

// Set the minimum level that will be printed. Default: Info (Debug in debug builds).
void log_set_level(LogLevel level);
LogLevel log_level();

// Tee everything to a file as well as the console. A build started from Explorer has no
// console at all, so without this a crash leaves nothing behind to read. Opening twice
// closes the previous file. Returns false if the path could not be opened for writing.
bool log_open_file(const char* path);
void log_close_file();

// The last few hundred lines, oldest first, written into `buffer` as one newline-separated
// block and always null-terminated. Returns the number of bytes written, not counting the
// terminator.
//
// Deliberately lock-free: the caller is usually a crash handler, and the thread that just
// faulted may well be the one holding the log mutex. A torn line in a crash report is a far
// smaller problem than a deadlock in the code meant to explain the crash.
usize log_recent(char* buffer, usize capacity);

// Prefer the macros below; this is the sink they all funnel into.
void log_write(LogLevel level, std::string_view category, std::string_view message);

namespace detail {
template <typename... Args>
void log_fmt(LogLevel level, std::string_view category,
             std::format_string<Args...> fmt, Args&&... args) {
    if (level < log_level()) return;
    log_write(level, category, std::format(fmt, std::forward<Args>(args)...));
}
}  // namespace detail

#define WS_LOG_TRACE(cat, ...) ::ws::detail::log_fmt(::ws::LogLevel::Trace, cat, __VA_ARGS__)
#define WS_LOG_DEBUG(cat, ...) ::ws::detail::log_fmt(::ws::LogLevel::Debug, cat, __VA_ARGS__)
#define WS_LOG_INFO(cat, ...)  ::ws::detail::log_fmt(::ws::LogLevel::Info,  cat, __VA_ARGS__)
#define WS_LOG_WARN(cat, ...)  ::ws::detail::log_fmt(::ws::LogLevel::Warn,  cat, __VA_ARGS__)
#define WS_LOG_ERROR(cat, ...) ::ws::detail::log_fmt(::ws::LogLevel::Error, cat, __VA_ARGS__)
#define WS_LOG_FATAL(cat, ...) ::ws::detail::log_fmt(::ws::LogLevel::Fatal, cat, __VA_ARGS__)

}  // namespace ws
