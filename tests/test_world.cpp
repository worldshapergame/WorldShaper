#include <doctest/doctest.h>

#include <vector>

#include "core/hash.hpp"
#include "world/world.hpp"

using namespace ws;

TEST_CASE("chunk coordinates floor toward negative infinity") {
    // Division would truncate toward zero and put every negative voxel one chunk out.
    // This is the same class of bug as fixed-point floor_int, and it is worth its own test
    // because it would only show up as a seam at the origin.
    CHECK(chunk_of(0) == 0);
    CHECK(chunk_of(255) == 0);
    CHECK(chunk_of(256) == 1);
    CHECK(chunk_of(-1) == -1);
    CHECK(chunk_of(-256) == -1);
    CHECK(chunk_of(-257) == -2);

    CHECK(local_of(0) == 0);
    CHECK(local_of(255) == 255);
    CHECK(local_of(256) == 0);
    CHECK(local_of(-1) == 255);
    CHECK(local_of(-256) == 0);
}

TEST_CASE("an empty world costs nothing") {
    World world;
    CHECK(world.chunk_count() == 0);
    CHECK(world.get(0, 0, 0) == kAir);
    CHECK(world.get(-1'000'000, 5, 900'000) == kAir);
    CHECK(world.stats().bytes == 0);
    CHECK(world.validate());
}

TEST_CASE("writing air into empty space allocates nothing") {
    World world;
    CHECK_FALSE(world.set(10, 20, 30, kAir));
    CHECK(world.chunk_count() == 0);
}

TEST_CASE("voxels read back at any coordinate, positive or negative") {
    World world;
    const i64 places[][3] = {
        {0, 0, 0},        {255, 255, 255},  {256, 0, 0},      {-1, -1, -1},
        {-256, -256, -256}, {-257, 1, -3},  {1'000'000, -2'000'000, 500'000},
    };
    for (u32 i = 0; i < 7; ++i) {
        REQUIRE(world.set(places[i][0], places[i][1], places[i][2], 1 + i));
    }
    for (u32 i = 0; i < 7; ++i) {
        REQUIRE(world.get(places[i][0], places[i][1], places[i][2]) == 1 + i);
    }
    CHECK(world.validate());
}

TEST_CASE("neighbouring voxels across a chunk boundary stay neighbours") {
    World world;
    world.set(255, 0, 0, 1);
    world.set(256, 0, 0, 2);
    world.set(-1, 0, 0, 3);
    CHECK(world.get(255, 0, 0) == 1);
    CHECK(world.get(256, 0, 0) == 2);
    CHECK(world.get(-1, 0, 0) == 3);
    CHECK(world.chunk_count() == 3);
}

TEST_CASE("a far-flung world stays sparse") {
    World world;
    for (i64 i = 0; i < 64; ++i) {
        world.set(i * 100'000, 0, 0, 7);
    }
    CHECK(world.chunk_count() == 64);
    const WorldStats stats = world.stats();
    CHECK(stats.bricks == 64);
    CHECK(stats.solid_voxels == 64);
    CHECK(world.validate());
}

TEST_CASE("compact removes chunks that became empty") {
    World world;
    world.set(0, 0, 0, 5);
    world.set(10'000, 0, 0, 5);
    CHECK(world.chunk_count() == 2);

    world.set(10'000, 0, 0, kAir);
    CHECK(world.chunk_count() == 2);   // still there until compacted

    world.compact();
    CHECK(world.chunk_count() == 1);
    CHECK(world.get(0, 0, 0) == 5);
    CHECK(world.validate());
}

TEST_CASE("content hash does not depend on iteration order or build order") {
    World a;
    a.set(0, 0, 0, 1);
    a.set(5'000, 300, -700, 2);
    a.set(-9'000, 12, 4, 3);

    World b;
    b.set(-9'000, 12, 4, 3);
    b.set(0, 0, 0, 99);
    b.set(0, 0, 0, 1);
    b.set(5'000, 300, -700, 2);
    b.set(1'234, 5, 6, 8);
    b.set(1'234, 5, 6, kAir);
    b.compact();

    CHECK(a.content_hash() == b.content_hash());

    b.set(1, 1, 1, 4);
    CHECK(a.content_hash() != b.content_hash());
}

TEST_CASE("per-chunk hashes are what reconciliation compares") {
    World a;
    World b;
    for (i64 i = 0; i < 40; ++i) {
        a.set(i, i, i, 1 + static_cast<u32>(i % 4));
        b.set(i, i, i, 1 + static_cast<u32>(i % 4));
    }
    const ChunkCoord origin{0, 0, 0};
    CHECK(a.chunk_hash(origin) == b.chunk_hash(origin));
    CHECK(a.chunk_hash(ChunkCoord{99, 99, 99}) == 0);   // a chunk that does not exist

    b.set(3, 3, 3, 42);   // one peer diverges in one chunk
    CHECK(a.chunk_hash(origin) != b.chunk_hash(origin));
}

TEST_CASE("sorted chunk order is stable, which is what saving needs") {
    World world;
    world.set(5'000, 0, 0, 1);
    world.set(0, 0, 0, 1);
    world.set(-5'000, 0, 0, 1);
    world.set(0, 5'000, 0, 1);

    const std::vector<ChunkCoord> first = world.sorted_chunk_coords();
    const std::vector<ChunkCoord> second = world.sorted_chunk_coords();
    CHECK(first == second);
    REQUIRE(first.size() == 4);
    for (usize i = 1; i < first.size(); ++i) {
        const ChunkCoord& a = first[i - 1];
        const ChunkCoord& b = first[i];
        REQUIRE((a.z < b.z || (a.z == b.z && (a.y < b.y || (a.y == b.y && a.x < b.x)))));
    }
}

TEST_CASE("a random edit storm across many chunks stays consistent") {
    World world;
    std::unordered_map<u64, VoxelTypeId> shadow;
    auto key_of = [](i64 x, i64 y, i64 z) {
        return hash_cell(x, y, z, 0, 0xABCDEFull);
    };

    for (u32 step = 0; step < 40000; ++step) {
        const u64 h = hash_cell(step, 7, 13, step, 3);
        const i64 x = static_cast<i64>(hash_range(h, 2000)) - 1000;
        const i64 y = static_cast<i64>(hash_range(hash_mix(h + 1), 2000)) - 1000;
        const i64 z = static_cast<i64>(hash_range(hash_mix(h + 2), 2000)) - 1000;
        const u32 roll = hash_range(hash_mix(h + 3), 100);
        const VoxelTypeId value = (roll < 35) ? kAir : (1 + hash_range(hash_mix(h + 4), 12));

        world.set(x, y, z, value);
        if (value == kAir) {
            shadow.erase(key_of(x, y, z));
        } else {
            shadow[key_of(x, y, z)] = value;
        }

        if (step % 10000 == 0) REQUIRE(world.validate());
    }

    REQUIRE(world.validate());
    const u64 hash_before = world.content_hash();
    world.compact();
    CHECK(world.content_hash() == hash_before);
    CHECK(world.stats().solid_voxels == shadow.size());
}
