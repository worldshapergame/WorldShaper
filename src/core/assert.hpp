#pragma once
// Assertions and panics.
//
// documentation/02-architecture-overview.md, "Error philosophy":
//   invariant violations crash loudly in development and are logged + self-healed in
//   release. WS_ASSERT is the development-only form; WS_CHECK is always compiled in and
//   is used for anything that must never be allowed to continue silently.

#include <cstdlib>

#include "core/log.hpp"

namespace ws {

[[noreturn]] inline void panic(const char* file, int line, const char* expr,
                               std::string_view message) {
    log_write(LogLevel::Fatal, "assert",
              std::format("{}:{}  ({})  {}", file, line, expr, message));
    std::abort();
}

}  // namespace ws

#define WS_CHECK(expr, ...)                                                        \
    do {                                                                           \
        if (!(expr)) [[unlikely]] {                                                \
            ::ws::panic(__FILE__, __LINE__, #expr, std::format("" __VA_ARGS__));   \
        }                                                                          \
    } while (false)

#if WS_DEBUG
#define WS_ASSERT(expr, ...) WS_CHECK(expr, __VA_ARGS__)
#else
#define WS_ASSERT(expr, ...) ((void)0)
#endif

#define WS_UNREACHABLE() ::ws::panic(__FILE__, __LINE__, "unreachable", "")
