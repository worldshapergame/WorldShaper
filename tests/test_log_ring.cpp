// The tail of the log that a crash report carries. If this loses lines, or hands back
// something unterminated, then every crash report is worth less exactly when it matters.

#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "core/log.hpp"
#include "doctest/doctest.h"

using namespace ws;

namespace {

// Big enough for anything these tests write, so a short read means a real fault rather than
// the buffer running out.
constexpr usize kScratch = 256 * 1024;

std::string recent() {
    std::vector<char> buffer(kScratch);
    const usize used = log_recent(buffer.data(), buffer.size());
    CHECK(buffer[used] == '\0');
    return std::string(buffer.data(), used);
}

int count_lines(const std::string& text, const std::string& needle) {
    int found = 0;
    usize at = 0;
    while ((at = text.find(needle, at)) != std::string::npos) {
        ++found;
        at += needle.size();
    }
    return found;
}

}  // namespace

TEST_CASE("log_recent returns the lines just written") {
    log_set_level(LogLevel::Trace);
    WS_LOG_INFO("ringtest", "alpha marker");
    WS_LOG_WARN("ringtest", "beta marker");
    const std::string text = recent();
    CHECK(text.find("alpha marker") != std::string::npos);
    CHECK(text.find("beta marker") != std::string::npos);
    // Order matters: a report reads bottom-up, and the last line before a crash is the one
    // that explains it.
    CHECK(text.find("alpha marker") < text.find("beta marker"));
}

TEST_CASE("log_recent keeps the newest lines when the ring wraps") {
    log_set_level(LogLevel::Trace);
    for (int i = 0; i < 2000; ++i) WS_LOG_INFO("ringtest", "wrap {}", i);
    const std::string text = recent();
    CHECK(text.find("wrap 1999") != std::string::npos);
    // The oldest of two thousand cannot have survived a ring that holds hundreds.
    CHECK(text.find("wrap 0\n") == std::string::npos);
}

TEST_CASE("log_recent respects a small buffer and still terminates") {
    log_set_level(LogLevel::Trace);
    for (int i = 0; i < 50; ++i) WS_LOG_INFO("ringtest", "small {}", i);

    char tiny[64];
    std::memset(tiny, 'x', sizeof(tiny));
    const usize used = log_recent(tiny, sizeof(tiny));
    CHECK(used < sizeof(tiny));
    CHECK(tiny[used] == '\0');
    CHECK(std::strlen(tiny) == used);

    // A capacity of one has room for the terminator and nothing else.
    char single[1] = {'x'};
    CHECK(log_recent(single, sizeof(single)) == 0);
    CHECK(single[0] == '\0');
    // Zero capacity must not be written to at all.
    char none[1] = {'x'};
    CHECK(log_recent(none, 0) == 0);
    CHECK(none[0] == 'x');
}

TEST_CASE("log_recent survives lines longer than the ring's width") {
    log_set_level(LogLevel::Trace);
    // Comfortably past the ring's line width, without filling the console with it.
    const std::string huge(400, 'L');
    WS_LOG_ERROR("ringtest", "{}", huge);
    const std::string text = recent();
    // Truncated, but present and still one well-formed line rather than a run-on.
    CHECK(text.find("LLLLLLLLLL") != std::string::npos);
    CHECK(text.back() == '\n');
}

TEST_CASE("concurrent writers lose no lines and produce no run-ons") {
    log_set_level(LogLevel::Trace);
    // Fewer lines in total than the ring holds, so every one must come back.
    constexpr int kThreads = 4;
    constexpr int kEach = 50;
    for (int i = 0; i < 600; ++i) WS_LOG_INFO("ringtest", "flush {}", i);

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < kEach; ++i) WS_LOG_INFO("racetest", "t{} n{}", t, i);
        });
    }
    for (std::thread& thread : threads) thread.join();

    const std::string text = recent();
    CHECK(count_lines(text, "racetest") == kThreads * kEach);
}

TEST_CASE("a file sink receives what the console does") {
    log_set_level(LogLevel::Trace);
    const std::string path = "ws_test_log_sink.txt";
    REQUIRE(log_open_file(path.c_str()));
    WS_LOG_INFO("ringtest", "written to the file");
    log_close_file();

    std::FILE* file = std::fopen(path.c_str(), "rb");
    REQUIRE(file != nullptr);
    std::string contents;
    char chunk[512];
    while (const usize read = std::fread(chunk, 1, sizeof(chunk), file)) {
        contents.append(chunk, read);
    }
    std::fclose(file);
    std::remove(path.c_str());
    CHECK(contents.find("written to the file") != std::string::npos);
}
