#include <doctest/doctest.h>

#include <vector>

#include "world/op.hpp"
#include "world/thumb_cache.hpp"
#include "world/world.hpp"

using namespace ws;

namespace {

VoxelTypeTable types;

// A slab wide enough to need more thumbnails than a small budget can hold. `span` is in
// voxels, so 600 is a couple of chunks either way, not six hundred.
void build_slab(World& world, i64 span) {
    MatterLedger ledger;
    apply_op(world, Op::fill_box(0, 0, -span, -8, -span, span, 0, span, 1,
                                 MatterReason::Generation),
             ledger);
}

// A long thin strip along x. Reaches tens of chunks away for a couple of million voxels,
// where a slab that wide would be hundreds of millions and far too slow for a test.
void build_strip(World& world, i64 span) {
    MatterLedger ledger;
    apply_op(world, Op::fill_box(0, 0, -span, -8, -8, span, 0, 0, 1, MatterReason::Generation),
             ledger);
}

u32 pump(ThumbnailCache& cache, const World& world, const ChunkCoord& centre, u32 frames) {
    u32 built = 0;
    for (u32 i = 0; i < frames; ++i) {
        const ThumbnailBatch& batch = cache.update(world, centre, i + 1);
        built += batch.built;
        REQUIRE(cache.validate());
    }
    return built;
}

}  // namespace

TEST_CASE("thumbnails fill in around the camera and then stop") {
    World world;
    build_slab(world, 600);

    ThumbnailBudget budget;
    budget.max_thumbs = 4096;
    budget.radius_chunks = 8;
    ThumbnailCache cache;
    cache.create(budget, types);

    pump(cache, world, ChunkCoord{0, 0, 0}, 200);

    CHECK(cache.resident_count() > 0);
    // Nothing left wanting: it converged rather than rebuilding the same chunks forever.
    const ThumbnailBatch& settled = cache.update(world, ChunkCoord{0, 0, 0}, 999);
    CHECK(settled.built == 0);
    CHECK(settled.wanted == 0);
}

TEST_CASE("a thumbnail reads back through the wrapped grid the way the shader reads it") {
    World world;
    build_slab(world, 300);

    ThumbnailBudget budget;
    budget.max_thumbs = 4096;
    budget.radius_chunks = 4;
    ThumbnailCache cache;
    cache.create(budget, types);
    pump(cache, world, ChunkCoord{0, 0, 0}, 200);

    // The slab's top surface is at y = 0, so chunk (0,-1,0) holds matter in its top cells.
    REQUIRE(cache.resident(ChunkCoord{0, -1, 0}));
    u32 solid = 0;
    for (u32 z = 0; z < kThumbAxis; ++z) {
        for (u32 x = 0; x < kThumbAxis; ++x) {
            if (cache.sample(ChunkCoord{0, -1, 0}, x, kThumbAxis - 1, z) != 0) ++solid;
        }
    }
    CHECK(solid == kThumbAxis * kThumbAxis);

    // A chunk nobody asked about has nothing, and says so rather than reading someone
    // else's slot through the wrapped grid.
    CHECK(cache.sample(ChunkCoord{9999, 7777, -5555}, 0, 0, 0) == 0);
}

TEST_CASE("a full cache keeps the near chunks and drops the far ones") {
    // The policy that matters. With a budget too small for the radius, what must never
    // happen is the near chunks being evicted by ones behind them — that churns forever and
    // the world flickers at exactly the distance you are looking at.
    World world;
    build_strip(world, 12800);   // ±50 chunks along x, far more than 24 thumbnails

    ThumbnailBudget budget;
    budget.max_thumbs = 24;
    budget.radius_chunks = 40;
    budget.max_builds_per_frame = 8;
    ThumbnailCache cache;
    cache.create(budget, types);

    pump(cache, world, ChunkCoord{0, 0, 0}, 400);
    CHECK(cache.resident_count() == 24);

    // Whatever it settled on, it is the near ones — and it settled.
    const ThumbnailBatch& settled = cache.update(world, ChunkCoord{0, 0, 0}, 401);
    CHECK(settled.built == 0);
    CHECK(settled.evicted == 0);
    CHECK(cache.resident(ChunkCoord{0, -1, -1}));
    CHECK_FALSE(cache.resident(ChunkCoord{35, -1, -1}));   // exists, but far, so not held
    CHECK(cache.validate());
}

TEST_CASE("editing a chunk rebuilds its thumbnail, in place") {
    World world;
    build_slab(world, 200);

    ThumbnailBudget budget;
    budget.max_thumbs = 1024;
    budget.radius_chunks = 3;
    ThumbnailCache cache;
    cache.create(budget, types);
    pump(cache, world, ChunkCoord{0, 0, 0}, 100);

    const ChunkCoord target{0, -1, 0};
    REQUIRE(cache.resident(target));
    REQUIRE(cache.sample(target, 4, kThumbAxis - 1, 4) != 0);

    // Carve the whole top metre away.
    MatterLedger ledger;
    apply_op(world, Op::fill_box(1, 0, 0, -32, 0, 255, -1, 255, kAir, MatterReason::PlayerBreak),
             ledger);
    cache.invalidate(target);

    pump(cache, world, ChunkCoord{0, 0, 0}, 20);
    CHECK(cache.sample(target, 4, kThumbAxis - 1, 4) == 0);
    CHECK(cache.resident(target));   // rebuilt in place, never blinked out
    CHECK(cache.validate());
}

TEST_CASE("moving the camera brings the new side in") {
    World world;
    build_strip(world, 12800);   // ±50 chunks along x

    ThumbnailBudget budget;
    budget.max_thumbs = 512;
    budget.radius_chunks = 6;
    budget.max_builds_per_frame = 8;
    ThumbnailCache cache;
    cache.create(budget, types);

    pump(cache, world, ChunkCoord{0, 0, 0}, 200);
    REQUIRE(cache.resident(ChunkCoord{0, -1, -1}));
    CHECK_FALSE(cache.resident(ChunkCoord{40, -1, -1}));

    pump(cache, world, ChunkCoord{40, 0, 0}, 200);
    CHECK(cache.resident(ChunkCoord{40, -1, -1}));
    // What it left behind is kept, not dropped: the thumbnail is still correct and still
    // two kilobytes, so throwing it away would only mean rebuilding it if you turn round.
    // Distance decides who is evicted under pressure, not who is held.
    CHECK(cache.resident(ChunkCoord{0, -1, -1}));
    CHECK(cache.validate());
}
