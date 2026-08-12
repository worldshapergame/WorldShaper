#include <doctest/doctest.h>

#include <vector>

#include "core/dirty_set.hpp"

using namespace ws;

// The dirty set had no tests at all, which is how `clear_range` -- the primitive the partial upload
// of D544 turns on -- would have gone in unchecked. A wrong `marked_` here does not throw: it makes
// `empty()` lie, and an upload that believes the set is empty leaves a stale record on the card,
// which shows up as a wrong picture a thousand frames later rather than as a failure.

TEST_CASE("marking counts each element once and coalesces into runs") {
    DirtySet set;
    set.create(1000);
    CHECK(set.empty());
    CHECK(set.marked() == 0);

    set.mark(10);
    set.mark(10);  // twice is once
    set.mark(11);
    CHECK(set.marked() == 2);
    CHECK_FALSE(set.empty());

    set.mark(2000);  // out of range is ignored, not undefined
    CHECK(set.marked() == 2);

    const auto runs = set.runs(0);
    REQUIRE(runs.size() == 1);
    CHECK(runs[0].first == 10);
    CHECK(runs[0].second == 2);
}

TEST_CASE("a gap joins two runs that are near each other") {
    DirtySet set;
    set.create(1000);
    set.mark(10);
    set.mark(14);

    CHECK(set.runs(0).size() == 2);   // four clean elements between them
    CHECK(set.runs(3).size() == 2);   // still too far: the gap is elements BETWEEN
    const auto joined = set.runs(4);
    REQUIRE(joined.size() == 1);
    CHECK(joined[0].first == 10);
    CHECK(joined[0].second == 5);
}

TEST_CASE("clear_range clears exactly the span and keeps the count honest") {
    DirtySet set;
    set.create(1000);
    set.mark_range(100, 200);
    REQUIRE(set.marked() == 200);

    set.clear_range(100, 50);
    CHECK(set.marked() == 150);
    auto runs = set.runs(0);
    REQUIRE(runs.size() == 1);
    CHECK(runs[0].first == 150);
    CHECK(runs[0].second == 150);

    // Clearing a span that is already clean changes nothing -- the count must not go negative,
    // and it is unsigned, so a wrong decrement here would read as four billion marked.
    set.clear_range(0, 100);
    CHECK(set.marked() == 150);

    set.clear_range(150, 150);
    CHECK(set.empty());
    CHECK(set.runs(0).empty());
}

TEST_CASE("clear_range respects word boundaries and the end of the set") {
    DirtySet set;
    set.create(300);
    set.mark_range(0, 300);
    REQUIRE(set.marked() == 300);

    // Inside one word.
    set.clear_range(1, 3);
    CHECK(set.marked() == 297);
    // Head, whole words, and tail in one call.
    set.clear_range(60, 80);
    CHECK(set.marked() == 217);
    CHECK_FALSE(set.runs(0).empty());
    // Past the end is clamped rather than reaching into the spare bits of the last word.
    set.clear_range(290, 1000);
    CHECK(set.marked() == 207);
    // Wholly past the end, and a zero count, are both no-ops.
    set.clear_range(400, 10);
    set.clear_range(0, 0);
    CHECK(set.marked() == 207);

    // What is left must match a bit-by-bit reckoning, since the count is what `empty()` trusts.
    u64 counted = 0;
    for (const auto& run : set.runs(0)) counted += run.second;
    CHECK(counted == set.marked());
}

TEST_CASE("clearing every staged run drains a set the same as clear()") {
    // The shape D544 relies on: runs() hands back a snapshot, so marking each run clean while
    // walking that snapshot is safe and ends with nothing marked.
    DirtySet set;
    set.create(4096);
    for (u64 i = 0; i < 4096; i += 7) set.mark(i);
    const u64 before = set.marked();
    REQUIRE(before > 0);

    u64 cleared = 0;
    for (const auto& run : set.runs(64)) {
        set.clear_range(run.first, run.second);
        ++cleared;
    }
    CHECK(cleared > 0);
    CHECK(set.empty());
    CHECK(set.marked() == 0);

    // And a partial walk leaves exactly the rest marked, which is the whole point of it.
    for (u64 i = 0; i < 4096; i += 7) set.mark(i);
    const auto runs = set.runs(0);
    REQUIRE(runs.size() > 2);
    set.clear_range(runs[0].first, runs[0].second);
    set.clear_range(runs[1].first, runs[1].second);
    CHECK(set.marked() == before - runs[0].second - runs[1].second);
}

TEST_CASE("create wipes what was there before") {
    DirtySet set;
    set.create(100);
    set.mark_range(0, 100);
    REQUIRE(set.marked() == 100);
    set.create(64);
    CHECK(set.empty());
    CHECK(set.runs(0).empty());
}
