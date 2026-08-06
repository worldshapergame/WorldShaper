#include <doctest/doctest.h>

#include <vector>

#include "core/hash.hpp"
#include "world/chunk.hpp"

using namespace ws;

TEST_CASE("an empty chunk holds one node and nothing else") {
    Chunk chunk;
    CHECK(chunk.empty());
    CHECK(chunk.brick_count() == 0);
    CHECK(chunk.node_count() == 1);
    CHECK(chunk.get(0, 0, 0) == kAir);
    CHECK(chunk.get(255, 255, 255) == kAir);
    CHECK(chunk.validate());
}

TEST_CASE("carving air where nothing exists allocates nothing") {
    // Digging through open sky must not materialise a brick per step, or flying around
    // with the carve tool held down would allocate the whole world.
    Chunk chunk;
    for (u32 i = 0; i < 256; ++i) CHECK_FALSE(chunk.set(i, 0, 0, kAir));
    CHECK(chunk.brick_count() == 0);
    CHECK(chunk.node_count() == 1);
    CHECK(chunk.validate());
}

TEST_CASE("one voxel allocates exactly one path and one brick") {
    Chunk chunk;
    CHECK(chunk.set(100, 40, 7, 5));
    CHECK(chunk.brick_count() == 1);
    CHECK(chunk.node_count() == kChunkDepth);   // root plus four interior levels
    CHECK(chunk.get(100, 40, 7) == 5);
    CHECK(chunk.get(100, 40, 8) == kAir);
    CHECK(chunk.validate());
}

TEST_CASE("voxels land in the right brick") {
    Chunk chunk;
    chunk.set(0, 0, 0, 1);
    chunk.set(8, 0, 0, 2);     // next brick along x
    chunk.set(0, 8, 0, 3);
    chunk.set(0, 0, 8, 4);

    CHECK(chunk.brick_count() == 4);
    REQUIRE(chunk.brick(0, 0, 0) != nullptr);
    REQUIRE(chunk.brick(1, 0, 0) != nullptr);
    CHECK(chunk.brick(0, 0, 0)->get(0, 0, 0) == 1);
    CHECK(chunk.brick(1, 0, 0)->get(0, 0, 0) == 2);
    CHECK(chunk.brick(0, 1, 0)->get(0, 0, 0) == 3);
    CHECK(chunk.brick(0, 0, 1)->get(0, 0, 0) == 4);
    CHECK(chunk.validate());
}

TEST_CASE("every voxel position is addressable and distinct") {
    Chunk chunk;
    // A diagonal touches every brick along the main axis without filling the chunk.
    for (u32 i = 0; i < 256; ++i) chunk.set(i, i, i, 1 + (i % 7));
    for (u32 i = 0; i < 256; ++i) REQUIRE(chunk.get(i, i, i) == 1 + (i % 7));
    CHECK(chunk.brick_count() == 32);
    CHECK(chunk.validate());
}

TEST_CASE("a full chunk allocates the whole tree and no more") {
    Chunk chunk;
    for (u32 z = 0; z < 256; z += 8) {
        for (u32 y = 0; y < 256; y += 8) {
            for (u32 x = 0; x < 256; x += 8) chunk.set(x, y, z, 3);
        }
    }
    CHECK(chunk.brick_count() == 32 * 32 * 32);
    // 1 + 8 + 64 + 512 + 4096 interior nodes.
    CHECK(chunk.node_count() == 4681);
    CHECK(chunk.validate());
}

TEST_CASE("compact frees bricks that became air, and the nodes above them") {
    Chunk chunk;
    for (u32 i = 0; i < 256; ++i) chunk.set(i, i, i, 9);
    REQUIRE(chunk.brick_count() == 32);
    const u32 nodes_before = chunk.node_count();

    for (u32 i = 0; i < 256; ++i) chunk.set(i, i, i, kAir);
    CHECK(chunk.brick_count() == 32);        // set() never frees, by design

    chunk.compact();
    CHECK(chunk.brick_count() == 0);
    CHECK(chunk.empty());
    CHECK(chunk.node_count() == 1);
    CHECK(chunk.node_count() < nodes_before);
    CHECK(chunk.validate());
}

TEST_CASE("compact keeps the surviving voxels untouched") {
    Chunk chunk;
    chunk.set(10, 10, 10, 4);
    chunk.set(200, 200, 200, 5);
    chunk.set(100, 100, 100, 6);
    chunk.set(100, 100, 100, kAir);   // this brick becomes empty

    const u64 before = chunk.content_hash();
    chunk.compact();
    CHECK(chunk.content_hash() == before);
    CHECK(chunk.get(10, 10, 10) == 4);
    CHECK(chunk.get(200, 200, 200) == 5);
    CHECK(chunk.get(100, 100, 100) == kAir);
    CHECK(chunk.brick_count() == 2);
    CHECK(chunk.validate());
}

TEST_CASE("freed bricks and nodes are reused rather than leaked") {
    Chunk chunk;
    for (u32 round = 0; round < 8; ++round) {
        for (u32 i = 0; i < 256; ++i) chunk.set(i, i, i, 1);
        for (u32 i = 0; i < 256; ++i) chunk.set(i, i, i, kAir);
        chunk.compact();
        REQUIRE(chunk.validate());
    }
    // After eight fill/clear cycles the structure must be back to nothing, and the pools
    // must not have grown once per cycle.
    CHECK(chunk.empty());
    CHECK(chunk.node_count() == 1);
    CHECK(chunk.bytes() < 200 * 1024);
}

TEST_CASE("content hash ignores how the chunk was built") {
    Chunk direct;
    direct.set(5, 6, 7, 11);
    direct.set(200, 100, 50, 12);

    Chunk roundabout;
    roundabout.set(5, 6, 7, 99);
    roundabout.set(5, 6, 7, 11);
    roundabout.set(60, 60, 60, 42);
    roundabout.set(60, 60, 60, kAir);
    roundabout.set(200, 100, 50, 12);
    roundabout.compact();

    CHECK(direct.content_hash() == roundabout.content_hash());
}

TEST_CASE("different contents hash differently") {
    Chunk a;
    a.set(1, 2, 3, 7);
    Chunk b;
    b.set(1, 2, 3, 8);
    CHECK(a.content_hash() != b.content_hash());

    Chunk c;
    c.set(3, 2, 1, 7);   // same type, different place
    CHECK(a.content_hash() != c.content_hash());
}

TEST_CASE("a random edit storm never breaks an invariant") {
    Chunk chunk;
    std::vector<VoxelTypeId> shadow(256ull * 256 * 256, kAir);
    auto index_of = [](u32 x, u32 y, u32 z) { return (z * 256ull + y) * 256ull + x; };

    for (u32 step = 0; step < 30000; ++step) {
        const u64 h = hash_cell(step, 3, 5, step, 11);
        const u32 x = hash_range(h, 256);
        const u32 y = hash_range(hash_mix(h + 1), 256);
        const u32 z = hash_range(hash_mix(h + 2), 256);
        const u32 roll = hash_range(hash_mix(h + 3), 100);
        const VoxelTypeId value = (roll < 40) ? kAir : (1 + hash_range(hash_mix(h + 4), 20));

        const bool changed = chunk.set(x, y, z, value);
        REQUIRE(changed == (shadow[index_of(x, y, z)] != value));
        shadow[index_of(x, y, z)] = value;

        if (step % 5000 == 0) {
            REQUIRE(chunk.validate());
            chunk.compact();
            REQUIRE(chunk.validate());
        }
    }

    REQUIRE(chunk.validate());
    u64 solid = 0;
    for (u32 z = 0; z < 256; ++z) {
        for (u32 y = 0; y < 256; ++y) {
            for (u32 x = 0; x < 256; ++x) {
                REQUIRE(chunk.get(x, y, z) == shadow[index_of(x, y, z)]);
                if (shadow[index_of(x, y, z)] != kAir) ++solid;
            }
        }
    }
    CHECK(chunk.solid_voxels() == solid);
}
