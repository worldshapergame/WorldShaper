#include "core/log.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace ws {
namespace {

std::atomic<LogLevel> g_level{WS_DEBUG ? LogLevel::Debug : LogLevel::Info};
std::mutex g_mutex;
std::FILE* g_file = nullptr;

// The tail of the log, kept in plain storage so a crash handler can read it without taking
// a lock. Sized to hold the last few seconds of a busy frame loop, which in practice is
// where the explanation for a crash lives.
constexpr usize kRingLines = 512;
constexpr usize kRingLineLen = 192;
char g_ring[kRingLines][kRingLineLen];
std::atomic<u64> g_ring_written{0};

const char* level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "?????";
}

}  // namespace

void log_set_level(LogLevel level) { g_level.store(level, std::memory_order_relaxed); }

LogLevel log_level() { return g_level.load(std::memory_order_relaxed); }

bool log_open_file(const char* path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        std::fclose(g_file);
        g_file = nullptr;
    }
    g_file = std::fopen(path, "wb");
    return g_file != nullptr;
}

void log_close_file() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        std::fclose(g_file);
        g_file = nullptr;
    }
}

usize log_recent(char* buffer, usize capacity) {
    if (capacity == 0) return 0;
    const u64 written = g_ring_written.load(std::memory_order_acquire);
    const u64 count = written < kRingLines ? written : kRingLines;
    const u64 first = written - count;

    usize used = 0;
    for (u64 i = 0; i < count; ++i) {
        const char* line = g_ring[(first + i) % kRingLines];
        const usize len = ::strnlen(line, kRingLineLen);
        if (used + len + 2 > capacity) break;
        std::memcpy(buffer + used, line, len);
        used += len;
        buffer[used++] = '\n';
    }
    buffer[used] = '\0';
    return used;
}

void log_write(LogLevel level, std::string_view category, std::string_view message) {
    std::FILE* out = (level >= LogLevel::Warn) ? stderr : stdout;
    std::lock_guard<std::mutex> lock(g_mutex);

    // The ring copy is truncated to a fixed width; the console and the file get the whole
    // message, because a shader compile error is long and every line of it matters.
    const u64 slot = g_ring_written.load(std::memory_order_relaxed) % kRingLines;
    std::snprintf(g_ring[slot], kRingLineLen, "[%s] %-8.*s %.*s", level_name(level),
                  static_cast<int>(category.size()), category.data(),
                  static_cast<int>(message.size()), message.data());
    g_ring_written.fetch_add(1, std::memory_order_release);

    std::fprintf(out, "[%s] %-8.*s %.*s\n", level_name(level),
                 static_cast<int>(category.size()), category.data(),
                 static_cast<int>(message.size()), message.data());
    // Always flush. Log volume is low, and a crash that swallows the last twenty lines
    // costs far more time than the flush ever will.
    std::fflush(out);
    if (g_file) {
        std::fprintf(g_file, "[%s] %-8.*s %.*s\n", level_name(level),
                     static_cast<int>(category.size()), category.data(),
                     static_cast<int>(message.size()), message.data());
        std::fflush(g_file);
    }
}

}  // namespace ws
