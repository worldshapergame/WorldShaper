#include "core/log.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>

namespace ws {
namespace {

std::atomic<LogLevel> g_level{WS_DEBUG ? LogLevel::Debug : LogLevel::Info};
std::mutex g_mutex;

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

void log_write(LogLevel level, std::string_view category, std::string_view message) {
    std::FILE* out = (level >= LogLevel::Warn) ? stderr : stdout;
    std::lock_guard<std::mutex> lock(g_mutex);
    std::fprintf(out, "[%s] %-8.*s %.*s\n", level_name(level),
                 static_cast<int>(category.size()), category.data(),
                 static_cast<int>(message.size()), message.data());
    // Always flush. Log volume is low, and a crash that swallows the last twenty lines
    // costs far more time than the flush ever will.
    std::fflush(out);
}

}  // namespace ws
