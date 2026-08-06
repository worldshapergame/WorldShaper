#include <doctest/doctest.h>

#include <vector>

#include "core/block_pool.hpp"
#include "core/hash.hpp"
#include "world/gpu_brick.hpp"
#include "world/op.hpp"
#include "world/residency.hpp"
#include "world/world.hpp"

using namespace ws;

namespace {
// The encoder needs a type table to look colours up in, for the filtered average a brick
// carries for use at reduced detail. These tests do not care what the colours are, so one
// shared table is enough.
VoxelTypeTable types;
}  // namespace

// ---------------------------------------------------------------------------------------
// Block pool
// ---------------------------------------------------------------------------------------

TEST_CASE("the block pool rounds to size classes and reports the waste") {
    BlockPool pool;
    pool.create(1 << 20);

    CHECK(pool.class_size_for(1) == 64);
    CHECK(pool.class_size_for(64) == 64);
    CHECK(pool.class_size_for(65) == 128);
    CHECK(pool.class_size_for(144) == 192);
    CHECK(pool.class_size_for(2048) == 2048);
    CHECK(pool.class_size_for(2049) == 0);   // beyond the largest class

    const u32 offset = pool.allocate(144);
    CHECK(offset != kNoOffset);
    CHECK(offset % kBlockGranularity == 0);
    const BlockPoolStats stats = pool.stats();
    CHECK(stats.in_use == 192);
    CHECK(stats.requested == 144);
    CHECK(stats.waste_fraction() == doctest::Approx(1.0 - 144.0 / 192.0));
    CHECK(pool.validate());
}

TEST_CASE("freed blocks are reused rather than growing the pool") {
    BlockPool pool;
    pool.create(4096);

    std::vector<u32> offsets;
    for (u32 i = 0; i < 8; ++i) offsets.push_back(pool.allocate(256));
    for (u32 offset : offsets) REQUIRE(offset != kNoOffset);
    CHECK(pool.stats().in_use == 8 * 256);

    for (u32 offset : offsets) pool.release(offset, 256);
    CHECK(pool.stats().in_use == 0);
    CHECK(pool.stats().free_listed == 8 * 256);
    CHECK(pool.validate());

    // The next eight come out of the free list, not fresh space.
    for (u32 i = 0; i < 8; ++i) REQUIRE(pool.allocate(256) != kNoOffset);
    CHECK(pool.stats().high_water == 8 * 256);
    CHECK(pool.validate());
}

TEST_CASE("a full pool reports failure instead of overrunning") {
    BlockPool pool;
    pool.create(512);
    CHECK(pool.allocate(256) != kNoOffset);
    CHECK(pool.allocate(256) != kNoOffset);
    CHECK(pool.allocate(64) == kNoOffset);
    CHECK(pool.stats().failures == 1);
    CHECK(pool.validate());
}

TEST_CASE("allocate and release churn never corrupts the pool") {
    BlockPool pool;
    pool.create(1 << 18);
    std::vector<std::pair<u32, u32>> live;

    for (u32 step = 0; step < 40000; ++step) {
        const u64 h = hash_cell(step, 0, 0, step, 4);
        const bool grow = live.empty() || (hash_range(h, 100) < 60);
        if (grow) {
            const u32 size = 64 + hash_range(hash_mix(h), 1900);
            const u32 offset = pool.allocate(size);
            if (offset != kNoOffset) live.emplace_back(offset, size);
        } else {
            const u32 index = hash_range(hash_mix(h + 1), static_cast<u32>(live.size()));
            pool.release(live[index].first, live[index].second);
            live[index] = live.back();
            live.pop_back();
        }
        if (step % 4000 == 0) REQUIRE(pool.validate());
    }

    REQUIRE(pool.validate());
    for (const auto& [offset, size] : live) pool.release(offset, size);
    CHECK(pool.stats().in_use == 0);
    CHECK(pool.validate());
}

// ---------------------------------------------------------------------------------------
// GPU brick encoding
// ---------------------------------------------------------------------------------------

TEST_CASE("a brick encodes and decodes back to the same voxels") {
    Brick brick;
    for (u32 i = 0; i < 512; ++i) brick.set(i, (i % 6 == 0) ? kAir : (1 + (i % 9)));

    GpuBrickHeader header;
    std::vector<u8> payload;
    u64 occupancy[kBrickWords];
    encode_brick(brick, types, header, payload, occupancy);

    CHECK(header.index_bits == 4);   // ten distinct values including air
    CHECK(payload.size() == gpu_payload_size(brick));

    for (u32 i = 0; i < 512; ++i) {
        REQUIRE(decode_voxel(header, payload.data(), i) == brick.get(i));
    }
    CHECK(decode_brick_hash(header, payload.data()) == brick.content_hash());
}

TEST_CASE("a uniform brick needs no payload at all") {
    Brick brick(77);
    GpuBrickHeader header;
    std::vector<u8> payload;
    u64 occupancy[kBrickWords];
    encode_brick(brick, types, header, payload, occupancy);

    CHECK(header.uniform());
    CHECK(header.uniform_type == 77);
    CHECK(payload.empty());
    CHECK(gpu_payload_size(brick) == 0);
    CHECK(decode_voxel(header, nullptr, 300) == 77);
    for (u32 w = 0; w < kBrickWords; ++w) REQUIRE(occupancy[w] == ~0ull);
}

TEST_CASE("every index width round-trips") {
    struct Case { u32 distinct; u32 expect_bits; };
    const Case cases[] = {{1, 0}, {2, 1}, {4, 2}, {16, 4}, {200, 8}, {512, 32}};

    for (const Case& c : cases) {
        Brick brick;
        for (u32 i = 0; i < 512; ++i) brick.set(i, 1 + (i % c.distinct));
        // The GPU encoding mirrors the brick's form, and set() deliberately never shrinks
        // one. compact() is what gets a brick to its smallest form, and therefore to its
        // smallest GPU footprint — a brick filled with a single value is only *uniform*
        // once it has been compacted.
        brick.compact();

        GpuBrickHeader header;
        std::vector<u8> payload;
        u64 occupancy[kBrickWords];
        encode_brick(brick, types, header, payload, occupancy);

        REQUIRE(header.index_bits == c.expect_bits);
        for (u32 i = 0; i < 512; ++i) {
            REQUIRE(decode_voxel(header, payload.empty() ? nullptr : payload.data(), i) ==
                    brick.get(i));
        }
    }
}

TEST_CASE("the GPU encoding decodes correctly however the brick was built") {
    // The GPU path copies the brick's own packed layout rather than re-deriving a
    // canonical one, because re-deriving it measured 22 ms on a frame that streamed four
    // chunks. So two bricks with identical contents may produce different *bytes* — a
    // stale palette entry here, a different slot order there — and what must hold is that
    // they decode to the same voxels and the same occupancy.
    //
    // Byte-identity is still required of the save format, and is tested there.
    Brick direct(4);
    Brick carved;
    for (u32 i = 0; i < 512; ++i) carved.set(i, 1 + (i % 100));
    for (u32 i = 0; i < 512; ++i) carved.set(i, 4);

    GpuBrickHeader ha, hb;
    std::vector<u8> pa, pb;
    u64 oa[kBrickWords];
    u64 ob[kBrickWords];
    encode_brick(direct, types, ha, pa, oa);
    encode_brick(carved, types, hb, pb, ob);

    for (u32 i = 0; i < 512; ++i) {
        REQUIRE(decode_voxel(ha, pa.empty() ? nullptr : pa.data(), i) ==
                decode_voxel(hb, pb.empty() ? nullptr : pb.data(), i));
    }
    for (u32 w = 0; w < kBrickWords; ++w) REQUIRE(oa[w] == ob[w]);
    CHECK(decode_brick_hash(ha, pa.empty() ? nullptr : pa.data()) ==
          decode_brick_hash(hb, pb.empty() ? nullptr : pb.data()));

    // And the shortcut has to actually be worth something: the brick that was carved down
    // to one value still carries its old palette, so its payload is the larger of the two.
    CHECK(pa.size() <= pb.size());
}

TEST_CASE("terrain-shaped bricks stay inside the GPU memory budget") {
    // 16 header + 64 occupancy + payload. documentation/09 budgets the brick pool against
    // roughly half a byte per voxel, so this is the number that has to hold.
    Brick brick;
    for (u32 i = 0; i < 512; ++i) {
        brick.set(i, 1 + hash_range(hash_cell(i, 0, 0, 0, 1), 3));
    }
    BlockPool pool;
    pool.create(1 << 16);
    const u32 payload = pool.class_size_for(gpu_payload_size(brick));
    const f64 per_voxel = static_cast<f64>(16 + 64 + payload) / 512.0;
    CHECK(per_voxel < 0.55);
}

// ---------------------------------------------------------------------------------------
// Residency
// ---------------------------------------------------------------------------------------

namespace {

void build_terrain(World& world, i64 span) {
    MatterLedger ledger;
    // A slab with some carving, so chunks contain a realistic mixture of full, partial and
    // empty bricks.
    apply_op(world, Op::fill_box(0, 0, -span, -8, -span, span, 8, span, 1,
                                 MatterReason::Generation),
             ledger);
    for (i64 i = 0; i < 40; ++i) {
        const u64 h = hash_cell(i, 0, 0, 0, 2);
        const i64 x = static_cast<i64>(hash_range(h, static_cast<u32>(span * 2))) - span;
        const i64 z = static_cast<i64>(hash_range(hash_mix(h), static_cast<u32>(span * 2))) - span;
        apply_op(world, Op::fill_box(1, 0, x, -8, z, x + 5, 8, z + 5, kAir,
                                     MatterReason::PlayerBreak),
                 ledger);
    }
}

}  // namespace

TEST_CASE("requesting a chunk uploads it, and the mirror matches the world exactly") {
    // The Stage 2 consistency check: what the GPU holds must be bit-identical to what the
    // CPU holds, or every later stage is debugging a mirage.
    World world;
    build_terrain(world, 300);

    ResidencyBudget budget;
    budget.payload_bytes = 64ull * 1024 * 1024;
    budget.max_bricks = 262144;
    ResidencyManager residency;
    residency.create(budget, types);

    // Streaming is budgeted per frame, so a large request set arrives over several
    // frames. Pump until nothing is deferred — that is exactly what the renderer does by
    // asking again every frame.
    u64 frame = 1;
    u64 total_added = 0;
    for (; frame <= 200; ++frame) {
        for (const ChunkCoord& coord : world.sorted_chunk_coords()) residency.request(coord);
        const UploadBatch& batch = residency.update(world, frame);
        total_added += batch.chunks_added;
        REQUIRE_FALSE(batch.out_of_memory);
        REQUIRE(residency.validate());
        if (batch.chunks_deferred == 0 && batch.chunks_added == 0) break;
    }

    CHECK(frame < 200);   // it converged rather than churning
    CHECK(total_added == world.chunk_count());
    CHECK(residency.resident_chunk_count() == world.chunk_count());

    for (const ChunkCoord& coord : world.sorted_chunk_coords()) {
        REQUIRE(residency.resident(coord));
        REQUIRE(residency.mirror_hash(coord) == world.chunk_hash(coord));
    }
}

TEST_CASE("an edited chunk is refreshed even when the frame it was edited on was full") {
    // The trap this guards is that streaming is *pull*-based. The renderer reports chunks it
    // wanted and could not find; a chunk that is resident but out of date is found, so it is
    // never reported. Nothing but the edit itself knows it went stale.
    //
    // So a one-frame request is not enough. An edit spanning many chunks cannot be served in
    // one frame's upload budget, and whatever is left over has no second chance — it draws
    // pre-edit contents until something unrelated evicts it. That is a hole or a ghost slab
    // sitting in mid-air, and it lasts until you happen to edit inside it again.
    World world;
    MatterLedger ledger;
    apply_op(world, Op::fill_box(0, 0, -400, 0, -400, 400, 0, 400, 1, MatterReason::Generation),
             ledger);
    const std::vector<ChunkCoord> coords = world.sorted_chunk_coords();
    REQUIRE(coords.size() > 8);

    ResidencyBudget budget;
    budget.payload_bytes = 64ull << 20;
    budget.max_bricks = 262144;
    ResidencyManager residency;
    residency.create(budget, types);

    for (u64 frame = 1; frame <= 400; ++frame) {
        for (const ChunkCoord& coord : coords) residency.request(coord);
        const UploadBatch& batch = residency.update(world, frame);
        if (batch.chunks_deferred == 0 && batch.chunks_added == 0) break;
    }
    REQUIRE(residency.resident_chunk_count() == coords.size());

    // Now change every one of them, in a single edit, and say so once — exactly as the
    // application does. Deliberately more chunks than one frame can upload.
    apply_op(world, Op::fill_box(1, 0, -400, 0, -400, 400, 0, 400, 2, MatterReason::PlayerPlace),
             ledger);
    for (const ChunkCoord& coord : coords) residency.invalidate(coord);

    // From here on nothing asks again. The renderer has no reason to: it can find all of
    // them. Only the carried-over invalidation gets them refreshed.
    u32 refreshed = 0;
    for (u64 frame = 401; frame <= 800; ++frame) {
        const UploadBatch& batch = residency.update(world, frame);
        refreshed += batch.chunks_refreshed;
        REQUIRE_FALSE(batch.out_of_memory);
        if (batch.chunks_deferred == 0 && batch.chunks_refreshed == 0) break;
    }

    CHECK(refreshed == coords.size());
    for (const ChunkCoord& coord : coords) {
        REQUIRE(residency.mirror_hash(coord) == world.chunk_hash(coord));
    }
}

TEST_CASE("invalidating a chunk that is not resident costs nothing") {
    // The set must not become a work queue for chunks nobody is looking at. Those are
    // fetched on demand, complete and correct, the moment a ray wants one — uploading them
    // here would spend the frame budget on the wrong chunks.
    World world;
    MatterLedger ledger;
    apply_op(world, Op::fill_box(0, 0, -300, 0, -300, 300, 0, 300, 1, MatterReason::Generation),
             ledger);

    ResidencyBudget budget;
    budget.payload_bytes = 16ull << 20;
    budget.max_bricks = 65536;
    ResidencyManager residency;
    residency.create(budget, types);

    for (const ChunkCoord& coord : world.sorted_chunk_coords()) residency.invalidate(coord);
    const UploadBatch& batch = residency.update(world, 1);
    CHECK(batch.chunks_added == 0);
    CHECK(batch.chunks_refreshed == 0);
    CHECK(batch.chunks_deferred == 0);
    CHECK(residency.validate());
}

TEST_CASE("individual voxels read back out of the mirror") {
    World world;
    world.set(5, 6, 7, 42);
    world.set(200, 100, 30, 43);

    ResidencyBudget budget;
    budget.payload_bytes = 1 << 20;
    budget.max_bricks = 4096;
    ResidencyManager residency;
    residency.create(budget, types);
    residency.request(ChunkCoord{0, 0, 0});
    residency.update(world, 1);

    CHECK(residency.mirror_voxel(ChunkCoord{0, 0, 0}, 5, 6, 7) == 42);
    CHECK(residency.mirror_voxel(ChunkCoord{0, 0, 0}, 200, 100, 30) == 43);
    CHECK(residency.mirror_voxel(ChunkCoord{0, 0, 0}, 5, 6, 8) == kAir);
    CHECK(residency.mirror_voxel(ChunkCoord{9, 9, 9}, 0, 0, 0) == kAir);
}

TEST_CASE("the shader's walk finds the same voxels the world holds") {
    // mirror_voxel_world performs exactly the lookup the ray marcher will: wrapped grid
    // cell, chunk record, coordinate check, mask bit, rank via prefix and popcount,
    // occupancy, payload. If this disagrees with the world, the picture is wrong, and it
    // is far cheaper to find that out here than by looking at a broken image.
    World world;
    build_terrain(world, 300);

    ResidencyBudget budget;
    budget.payload_bytes = 64ull << 20;
    budget.max_bricks = 262144;
    budget.max_chunks = 1024;
    budget.max_chunk_uploads_per_frame = 64;
    budget.max_bricks_per_frame = 1 << 20;
    ResidencyManager residency;
    residency.create(budget, types);

    for (const ChunkCoord& coord : world.sorted_chunk_coords()) residency.request(coord);
    residency.update(world, 1);
    REQUIRE(residency.validate());

    u32 checked = 0;
    for (i64 z = -300; z <= 300; z += 7) {
        for (i64 y = -12; y <= 12; y += 3) {
            for (i64 x = -300; x <= 300; x += 7) {
                REQUIRE(residency.mirror_voxel_world(x, y, z) == world.get(x, y, z));
                ++checked;
            }
        }
    }
    CHECK(checked > 50000);

    // Somewhere with no chunk at all reads as air rather than as garbage.
    CHECK(residency.mirror_voxel_world(9'000'000, 0, 0) == kAir);
}

TEST_CASE("negative coordinates walk correctly through the wrapped grid") {
    // The grid index is a floor-modulo, not a remainder. With a remainder, every chunk at
    // a negative coordinate lands in the wrong cell and half the world is invisible.
    World world;
    for (i64 i = 1; i <= 40; ++i) {
        world.set(-i * 37, -i, -i * 11, 1 + static_cast<u32>(i % 5));
    }

    ResidencyBudget budget;
    budget.payload_bytes = 4 << 20;
    budget.max_bricks = 8192;
    budget.max_chunk_uploads_per_frame = 64;
    ResidencyManager residency;
    residency.create(budget, types);

    for (const ChunkCoord& coord : world.sorted_chunk_coords()) residency.request(coord);
    residency.update(world, 1);
    REQUIRE(residency.validate());

    for (i64 i = 1; i <= 40; ++i) {
        REQUIRE(residency.mirror_voxel_world(-i * 37, -i, -i * 11) ==
                world.get(-i * 37, -i, -i * 11));
    }
}

TEST_CASE("a chunk already resident and unchanged is a hit, not a re-upload") {
    World world;
    build_terrain(world, 100);
    const ChunkCoord coord = world.sorted_chunk_coords().front();

    ResidencyManager residency;
    residency.create(ResidencyBudget{16 << 20, 32768}, types);

    residency.request(coord);
    residency.update(world, 1);
    const u64 uploads_after_first = residency.stats().uploads;

    for (u64 frame = 2; frame < 10; ++frame) {
        residency.request(coord);
        const UploadBatch& batch = residency.update(world, frame);
        REQUIRE(batch.chunks_added == 0);
        REQUIRE(batch.chunks_refreshed == 0);
        REQUIRE(batch.payload_bytes == 0);
    }
    CHECK(residency.stats().uploads == uploads_after_first);
    CHECK(residency.stats().hit_rate() > 0.8);
}

TEST_CASE("editing a resident chunk re-uploads it") {
    World world;
    world.set(1, 1, 1, 5);
    const ChunkCoord coord{0, 0, 0};

    ResidencyManager residency;
    residency.create(ResidencyBudget{1 << 20, 4096}, types);
    residency.request(coord);
    residency.update(world, 1);
    CHECK(residency.mirror_hash(coord) == world.chunk_hash(coord));

    world.set(2, 2, 2, 6);
    residency.request(coord);
    const UploadBatch& batch = residency.update(world, 2);
    CHECK(batch.chunks_refreshed == 1);
    CHECK(residency.mirror_hash(coord) == world.chunk_hash(coord));
    CHECK(residency.validate());
}

TEST_CASE("a small budget evicts the least recently used chunk, never the wanted one") {
    World world;
    build_terrain(world, 600);
    const std::vector<ChunkCoord> coords = world.sorted_chunk_coords();
    REQUIRE(coords.size() > 4);

    // Deliberately far too small to hold everything.
    ResidencyManager residency;
    residency.create(ResidencyBudget{512 * 1024, 2048}, types);

    u64 frame = 1;
    for (const ChunkCoord& coord : coords) {
        residency.request(coord);
        residency.update(world, frame++);
        REQUIRE(residency.validate());
    }

    CHECK(residency.resident_chunk_count() < coords.size());
    CHECK(residency.stats().evictions > 0);
    // Whatever survived must still be correct — eviction must never corrupt a neighbour.
    for (const ChunkCoord& coord : coords) {
        if (residency.resident(coord)) {
            REQUIRE(residency.mirror_hash(coord) == world.chunk_hash(coord));
        }
    }
    // The most recent request is still there.
    CHECK(residency.resident(coords.back()));
}

TEST_CASE("thrashing a tight budget stays consistent and never leaks") {
    World world;
    build_terrain(world, 500);
    const std::vector<ChunkCoord> coords = world.sorted_chunk_coords();

    ResidencyManager residency;
    residency.create(ResidencyBudget{1 << 20, 8192}, types);

    for (u64 frame = 1; frame <= 200; ++frame) {
        const u64 h = hash_cell(static_cast<i64>(frame), 0, 0, frame, 6);
        const u32 count = 1 + hash_range(h, 3);
        for (u32 i = 0; i < count; ++i) {
            residency.request(coords[hash_range(hash_mix(h + i), static_cast<u32>(coords.size()))]);
        }
        residency.update(world, frame);
        REQUIRE(residency.validate());
    }

    const ResidencyStats stats = residency.stats();
    CHECK(stats.evictions > 0);
    CHECK(stats.payload_in_use <= stats.payload_capacity);

    // Everything still resident agrees with the world.
    for (const ChunkCoord& coord : coords) {
        if (residency.resident(coord)) {
            REQUIRE(residency.mirror_hash(coord) == world.chunk_hash(coord));
        }
    }

    // And after evicting the lot, the pool is empty rather than fragmented into oblivion.
    for (u64 frame = 201; frame <= 220; ++frame) residency.update(world, frame);
    residency.request(coords.front());
    residency.update(world, 999);
    CHECK(residency.validate());
}

TEST_CASE("duplicate requests in one frame cost one upload") {
    // The feedback buffer reports per pixel, so the same chunk arrives thousands of times.
    World world;
    world.set(0, 0, 0, 1);
    ResidencyManager residency;
    residency.create(ResidencyBudget{1 << 20, 4096}, types);

    for (u32 i = 0; i < 5000; ++i) residency.request(ChunkCoord{0, 0, 0});
    const UploadBatch& batch = residency.update(world, 1);
    CHECK(batch.chunks_added == 1);
    CHECK(residency.stats().requests == 1);
}

TEST_CASE("a chunk that no longer exists is dropped rather than kept stale") {
    World world;
    world.set(0, 0, 0, 1);
    const ChunkCoord coord{0, 0, 0};

    ResidencyManager residency;
    residency.create(ResidencyBudget{1 << 20, 4096}, types);
    residency.request(coord);
    residency.update(world, 1);
    REQUIRE(residency.resident(coord));

    world.set(0, 0, 0, kAir);
    world.compact();
    residency.request(coord);
    residency.update(world, 2);
    CHECK_FALSE(residency.resident(coord));
    CHECK(residency.validate());
}

TEST_CASE("out of memory is reported, not crashed through") {
    World world;
    build_terrain(world, 400);

    ResidencyManager residency;
    residency.create(ResidencyBudget{4096, 8}, types);   // absurdly small on purpose

    for (const ChunkCoord& coord : world.sorted_chunk_coords()) residency.request(coord);
    const UploadBatch& batch = residency.update(world, 1);
    CHECK(batch.out_of_memory);
    CHECK(residency.validate());
}
