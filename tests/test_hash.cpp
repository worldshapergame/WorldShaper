#include <doctest/doctest.h>

#include <array>
#include <vector>

#include "core/fixed.hpp"
#include "core/hash.hpp"

using namespace ws;

TEST_CASE("hash is stable across runs") {
    // These values are frozen on purpose. If a change to hash_mix makes this test fail,
    // that change rewrites the outcome of every simulation that ever ran — version the
    // hash instead of editing these numbers.
    CHECK(hash_mix(0) == 0xE220A8397B1DCDAFull);
    CHECK(hash_mix(1) == 0x910A2DEC89025CC1ull);
    CHECK(hash_cell(0, 0, 0, 0, 0) == 0x78AE5A9A6B5FD45Eull);
}

TEST_CASE("neighbouring cells produce uncorrelated values") {
    // A cellular automaton asking its own coordinate must not get a value that marches in
    // lockstep with its neighbour, or sand falls in visible diagonal stripes.
    u64 previous = hash_cell(0, 0, 0, 0, 0);
    i32 low_bit_agreements = 0;
    for (i64 x = 1; x < 512; ++x) {
        const u64 h = hash_cell(x, 0, 0, 0, 0);
        if ((h & 1) == (previous & 1)) ++low_bit_agreements;
        previous = h;
    }
    // Adjacent agreement should be near 50%; anything outside 40–60% signals structure.
    CHECK(low_bit_agreements > 204);
    CHECK(low_bit_agreements < 307);
}

TEST_CASE("changing any single input changes the output") {
    const u64 base = hash_cell(10, 20, 30, 40, 50);
    CHECK(hash_cell(11, 20, 30, 40, 50) != base);
    CHECK(hash_cell(10, 21, 30, 40, 50) != base);
    CHECK(hash_cell(10, 20, 31, 40, 50) != base);
    CHECK(hash_cell(10, 20, 30, 41, 50) != base);
    CHECK(hash_cell(10, 20, 30, 40, 51) != base);
}

TEST_CASE("salts give independent streams at the same place and tick") {
    // Two simulation systems querying the same cell on the same tick must not agree.
    i32 matches = 0;
    for (i64 x = 0; x < 1000; ++x) {
        if (hash_range(hash_cell(x, 0, 0, 7, 1), 4) == hash_range(hash_cell(x, 0, 0, 7, 2), 4)) {
            ++matches;
        }
    }
    // Expected ~250 by chance for a 1-in-4 draw.
    CHECK(matches > 180);
    CHECK(matches < 320);
}

TEST_CASE("hash_range stays inside its bound and is roughly uniform") {
    std::array<i32, 6> buckets{};
    for (i64 i = 0; i < 60000; ++i) {
        const u32 v = hash_range(hash_cell(i, i * 3, i * 7, 0, 0), 6);
        REQUIRE(v < 6);
        ++buckets[v];
    }
    for (i32 count : buckets) {
        CHECK(count > 9000);   // perfectly uniform would be 10000
        CHECK(count < 11000);
    }
}

TEST_CASE("hash_unit_fx stays inside [0,1)") {
    for (i64 i = 0; i < 10000; ++i) {
        const i32 v = hash_unit_fx(hash_cell(i, 0, 0, 0, 0));
        CHECK(v >= 0);
        CHECK(v < kFxOne);
    }
}

TEST_CASE("byte hashing is order sensitive and stable") {
    const std::vector<u8> a{1, 2, 3, 4};
    const std::vector<u8> b{4, 3, 2, 1};
    CHECK(hash_bytes(a.data(), a.size()) != hash_bytes(b.data(), b.size()));
    CHECK(hash_bytes(a.data(), a.size()) == hash_bytes(a.data(), a.size()));
    CHECK(hash_bytes(nullptr, 0) == 0xCBF29CE484222325ull);
}

TEST_CASE("negative coordinates hash as well as positive ones") {
    CHECK(hash_cell(-1, -1, -1, 0, 0) != hash_cell(1, 1, 1, 0, 0));
    CHECK(hash_cell(-1, 0, 0, 0, 0) != hash_cell(0, -1, 0, 0, 0));
}
