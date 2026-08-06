#include <doctest/doctest.h>

#include <vector>

#include "core/block_pool.hpp"

using namespace ws;

TEST_CASE("a full pool hands out a larger block rather than refusing") {
    // Segregated fit has one failure mode that matters here: the class you want is empty,
    // the pool is carved out, and there is plenty of free space in *other* classes. Refusing
    // then makes the caller evict and retry, and evicting something of the wrong size does
    // not help — so it evicts again, and again.
    BlockPool pool;
    pool.create(512, std::vector<u32>{64, 128, 256});

    // Carve the whole pool into 256-blocks, then free them all.
    const u32 a = pool.allocate(256);
    const u32 b = pool.allocate(256);
    REQUIRE(a != kNoOffset);
    REQUIRE(b != kNoOffset);
    REQUIRE(pool.allocate(64) == kNoOffset);   // nothing left to carve
    pool.release(a, 256);
    pool.release(b, 256);

    // Now ask for something small. The 64 class has never been used and the cursor is spent,
    // but there are two 256-blocks going begging.
    const u32 small = pool.allocate(64);
    CHECK(small != kNoOffset);
    CHECK(pool.stats().upgrades == 1);
    CHECK(pool.validate());
}

TEST_CASE("an upgraded block goes back to the class it came from") {
    // The subtle half. If a 256-block handed out for a 64-byte request were returned to the
    // 64 class's list, the next three allocations from that list would overlap it.
    BlockPool pool;
    pool.create(256, std::vector<u32>{64, 256});

    const u32 big = pool.allocate(256);
    REQUIRE(big != kNoOffset);
    pool.release(big, 256);

    const u32 upgraded = pool.allocate(64);
    REQUIRE(upgraded != kNoOffset);
    REQUIRE(pool.stats().upgrades == 1);
    pool.release(upgraded, 64);
    CHECK(pool.validate());

    // It has to be usable as a 256 again, which it would not be if it had been filed as 64.
    const u32 again = pool.allocate(256);
    CHECK(again == big);
    CHECK(pool.validate());
}

TEST_CASE("a genuinely full pool still says so") {
    BlockPool pool;
    pool.create(128, std::vector<u32>{64});
    CHECK(pool.allocate(64) != kNoOffset);
    CHECK(pool.allocate(64) != kNoOffset);
    CHECK(pool.allocate(64) == kNoOffset);   // no class is larger, nothing is free
    CHECK(pool.stats().upgrades == 0);
    CHECK(pool.validate());
}

TEST_CASE("the accounting survives a round of upgrades") {
    BlockPool pool;
    pool.create(1024, std::vector<u32>{64, 128, 256, 512});

    std::vector<std::pair<u32, u32>> live;
    for (u32 size : {512u, 256u, 128u, 64u}) {
        const u32 offset = pool.allocate(size);
        REQUIRE(offset != kNoOffset);
        live.emplace_back(offset, size);
    }
    REQUIRE(pool.validate());

    // Free the big ones, then ask for small ones that have to be upgraded into them.
    pool.release(live[0].first, live[0].second);
    pool.release(live[1].first, live[1].second);
    for (u32 i = 0; i < 2; ++i) {
        const u32 offset = pool.allocate(64);
        REQUIRE(offset != kNoOffset);
        live.emplace_back(offset, 64u);
        REQUIRE(pool.validate());
    }

    // Everything back, and the pool is whole again.
    for (usize i = 2; i < live.size(); ++i) pool.release(live[i].first, live[i].second);
    CHECK(pool.validate());
    CHECK(pool.stats().live_allocations == 0);
    CHECK(pool.stats().in_use == 0);
}
