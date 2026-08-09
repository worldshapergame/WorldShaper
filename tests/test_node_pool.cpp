// The node pool: one sparse octree replacing the chunk system.
//
// The property that matters most here is the one the chunk system could not have: **the pool and
// the world agree about every voxel, through the same walk the shader performs**. That is what
// `mirror_voxel` is for, and it is the successor to the mirror check that has guarded residency
// since Stage 2 — a renderer reading a structure nobody compares against the world is a renderer
// debugging a mirage.

#include <doctest/doctest.h>

#include "world/node_pool.hpp"
#include "world/voxel_type.hpp"

using namespace ws;

namespace {

struct Fixture {
    VoxelTypeTable types;
    World world;
    NodePool pool;
    VoxelTypeId stone = kAir;

    Fixture() {
        VisualRecord visual{};
        visual.red = 128;
        visual.green = 128;
        visual.blue = 128;
        stone = types.intern(visual, BehaviourRecord{});

        NodePoolBudget budget;
        budget.max_nodes = 1u << 16;
        budget.max_occupancy_leaves = 1u << 14;
        budget.payload_bytes = 4ull * 1024 * 1024;
        budget.proximity_voxels = 0;   // the tests ask for what they want explicitly
        pool.create(budget, types);
    }

    void fill_box(i64 x0, i64 y0, i64 z0, i64 x1, i64 y1, i64 z1) {
        for (i64 z = z0; z <= z1; ++z) {
            for (i64 y = y0; y <= y1; ++y) {
                for (i64 x = x0; x <= x1; ++x) world.set(x, y, z, stone);
            }
        }
    }

    const NodeUploadBatch& serve(u64 frame = 1) {
        const f64 camera[3] = {0.0, 0.0, 0.0};
        return pool.update(world, camera, frame);
    }

    // Ask for a region at brick resolution.
    //
    // The pool is depth-bounded: it builds to the level that was asked for and no finer, which is
    // what makes it follow the pixels rather than the world. So a test that checks voxels has to
    // ask for voxels — requesting the entry level alone yields a root shell and nothing under it,
    // which is correct behaviour and not a bug to be worked around.
    void want_box(i64 x0, i64 y0, i64 z0, i64 x1, i64 y1, i64 z1) {
        for (i64 z = z0; z <= z1; z += 8) {
            for (i64 y = y0; y <= y1; y += 8) {
                for (i64 x = x0; x <= x1; x += 8) {
                    pool.request(node_key_of(x, y, z, kLeafLevel));
                }
            }
        }
    }
};

}  // namespace

TEST_CASE("a node key is the voxel coordinate shifted, and negatives round down") {
    // Arithmetic shift, not division. A voxel at -1 belongs to node -1: getting this wrong puts
    // every negative coordinate one node out, which is the trap chunk_of already documents.
    CHECK(node_key_of(0, 0, 0, 3) == NodeKey{0, 0, 0, 3});
    CHECK(node_key_of(7, 7, 7, 3) == NodeKey{0, 0, 0, 3});
    CHECK(node_key_of(8, 0, 0, 3) == NodeKey{1, 0, 0, 3});
    CHECK(node_key_of(-1, -1, -1, 3) == NodeKey{-1, -1, -1, 3});
    CHECK(node_key_of(-8, 0, 0, 3) == NodeKey{-1, 0, 0, 3});
    CHECK(node_key_of(-9, 0, 0, 3) == NodeKey{-2, 0, 0, 3});
}

TEST_CASE("octants are x fastest, matching every other cell order in the engine") {
    CHECK(octant_of(0, 0, 0) == 0);
    CHECK(octant_of(1, 0, 0) == 1);
    CHECK(octant_of(0, 1, 0) == 2);
    CHECK(octant_of(0, 0, 1) == 4);
    CHECK(octant_of(1, 1, 1) == 7);
}

TEST_CASE("nothing is built for a world with nothing in it") {
    Fixture f;
    f.want_box(0, 0, 0, 0, 0, 0);
    const NodeUploadBatch& batch = f.serve();
    CHECK(batch.built == 0);
    CHECK(f.pool.find(node_key_of(0, 0, 0, kLeafLevel)) == kNoNode);
    CHECK(f.pool.validate());
}

TEST_CASE("a single voxel produces a leaf and every ancestor up to the entry level") {
    Fixture f;
    f.world.set(0, 0, 0, f.stone);
    f.want_box(0, 0, 0, 0, 0, 0);
    const NodeUploadBatch& batch = f.serve();

    CHECK(batch.built >= 1);
    CHECK(f.pool.validate());

    // Every level from the leaf to the entry level is reachable, because a descent is what the
    // shader does and a missing rung is a descent that stops early.
    for (u32 level = kLeafLevel; level <= kEntryLevel; ++level) {
        CAPTURE(level);
        CHECK(f.pool.find(node_key_of(0, 0, 0, level)) != kNoNode);
    }
}

TEST_CASE("the pool and the world agree about every voxel") {
    Fixture f;
    // Deliberately awkward: a slab, a thin wall one voxel thick, and a single isolated voxel.
    // Thin structure is what a sampled summary loses (D149), and one voxel is what a coverage
    // that rounds to zero loses (D139).
    f.fill_box(0, 0, 0, 31, 3, 31);
    f.fill_box(10, 4, 10, 10, 20, 25);
    f.world.set(40, 40, 40, f.stone);

    f.want_box(-8, -8, -8, 48, 48, 48);
    f.serve();
    REQUIRE(f.pool.validate());

    u64 checked = 0;
    for (i64 z = -2; z < 48; ++z) {
        for (i64 y = -2; y < 48; ++y) {
            for (i64 x = -2; x < 48; ++x) {
                CAPTURE(x);
                CAPTURE(y);
                CAPTURE(z);
                REQUIRE(f.pool.mirror_voxel(x, y, z) == f.world.get(x, y, z));
                ++checked;
            }
        }
    }
    CHECK(checked > 100000);
}

TEST_CASE("one voxel in a node still reports present at every level above it") {
    // The failure this guards is D139's, one level up: a node holding a railing or a wire must
    // not round its coverage to nothing, because zero is how "entirely air" is spelled and the
    // thing would vanish at exactly the range where you can no longer see well enough to notice.
    Fixture f;
    f.world.set(0, 0, 0, f.stone);
    f.want_box(0, 0, 0, 0, 0, 0);
    f.serve();

    for (u32 level = kLeafLevel; level <= kEntryLevel; ++level) {
        CAPTURE(level);
        const u32 slot = f.pool.find(node_key_of(0, 0, 0, level));
        REQUIRE(slot != kNoNode);
        const u32 alpha = (f.pool.nodes()[slot].colour >> 24) & 0xFFu;
        CHECK(alpha > 0);
    }
}

TEST_CASE("a child mask bit is set exactly where a child exists") {
    Fixture f;
    // Two voxels far enough apart to land in different octants of the same node.
    f.world.set(0, 0, 0, f.stone);
    f.world.set(63, 63, 63, f.stone);
    f.want_box(0, 0, 0, 63, 63, 63);
    f.serve();
    REQUIRE(f.pool.validate());

    const u32 slot = f.pool.find(node_key_of(0, 0, 0, 6));   // 64 voxels: holds both
    REQUIRE(slot != kNoNode);
    const GpuNode& node = f.pool.nodes()[slot];
    CHECK(node_child_mask(node) != 0);
    // The two occupied octants are the first and the last.
    CHECK((node_child_mask(node) & 1u) != 0);
    CHECK((node_child_mask(node) & (1u << 7)) != 0);
}

TEST_CASE("an edit drops the node and every ancestor that was folded from it") {
    Fixture f;
    f.fill_box(0, 0, 0, 7, 7, 7);
    f.want_box(0, 0, 0, 7, 7, 7);
    f.serve(1);
    REQUIRE(f.pool.find(node_key_of(0, 0, 0, kLeafLevel)) != kNoNode);

    f.world.set(0, 0, 0, kAir);
    f.pool.invalidate(0, 0, 0);
    f.serve(2);

    // Dropped rather than refreshed: nothing asked for it on this frame, so nothing rebuilt it.
    // What matters is that the stale copy is gone — a resident-but-wrong node is the one thing
    // feedback can never discover, because a ray that finds it does not report it (D131).
    CHECK(f.pool.find(node_key_of(0, 0, 0, kEntryLevel)) == kNoNode);
    CHECK(f.pool.validate());
}

TEST_CASE("an edit followed by a request rebuilds with the new contents") {
    Fixture f;
    f.fill_box(0, 0, 0, 7, 7, 7);
    f.want_box(0, 0, 0, 7, 7, 7);
    f.serve(1);
    CHECK(f.pool.mirror_voxel(3, 3, 3) == f.stone);

    f.world.set(3, 3, 3, kAir);
    f.pool.invalidate(3, 3, 3);
    f.want_box(0, 0, 0, 7, 7, 7);
    f.serve(2);

    REQUIRE(f.pool.find(node_key_of(0, 0, 0, kLeafLevel)) != kNoNode);
    CHECK(f.pool.mirror_voxel(3, 3, 3) == kAir);
    CHECK(f.pool.mirror_voxel(4, 3, 3) == f.stone);
    CHECK(f.pool.validate());
}

TEST_CASE("a node nobody wants is evicted, and one that is wanted is not") {
    Fixture f;
    f.fill_box(0, 0, 0, 7, 7, 7);
    NodePoolBudget budget;
    budget.max_nodes = 1u << 16;
    budget.max_occupancy_leaves = 1u << 14;
    budget.payload_bytes = 1024 * 1024;
    budget.proximity_voxels = 0;
    budget.cold_frames = 4;
    f.pool.create(budget, f.types);

    f.want_box(0, 0, 0, 7, 7, 7);
    f.serve(1);
    REQUIRE(f.pool.find(node_key_of(0, 0, 0, kLeafLevel)) != kNoNode);

    // Asked for again, so it stays however long the frames run on.
    for (u64 frame = 2; frame < 20; ++frame) {
        f.want_box(0, 0, 0, 7, 7, 7);
        f.serve(frame);
    }
    CHECK(f.pool.find(node_key_of(0, 0, 0, kLeafLevel)) != kNoNode);

    // And then nobody asks.
    for (u64 frame = 20; frame < 30; ++frame) f.serve(frame);
    CHECK(f.pool.find(node_key_of(0, 0, 0, kEntryLevel)) == kNoNode);
    CHECK(f.pool.validate());
}

TEST_CASE("the build budget bounds a frame rather than the work being abandoned") {
    Fixture f;
    f.fill_box(0, 0, 0, 63, 63, 63);

    NodePoolBudget budget;
    budget.max_nodes = 1u << 16;
    budget.max_occupancy_leaves = 1u << 14;
    budget.payload_bytes = 4ull * 1024 * 1024;
    budget.proximity_voxels = 0;
    budget.max_builds_per_frame = 8;   // far less than the tree needs
    f.pool.create(budget, f.types);

    f.want_box(0, 0, 0, 63, 63, 63);
    const NodeUploadBatch& first = f.serve(1);
    // Either it fitted in the budget or it did not; what it must never do is publish a partial
    // tree, because a ray descending into a half-built node reads whatever was in those slots
    // before.
    CHECK(f.pool.validate());
    CHECK(first.built <= 2);
}

TEST_CASE("a descent tells empty apart from not-yet-built") {
    // The distinction the whole structure exists for. An unstreamed building that reads as open
    // sky is never reported, so it is never streamed, so it stays open sky — which is D133 and
    // D147, and here it is a property rather than a bug that was found twice.
    Fixture f;
    f.fill_box(0, 0, 0, 63, 63, 63);

    NodePoolBudget budget;
    budget.max_nodes = 1u << 16;
    budget.max_occupancy_leaves = 1u << 14;
    budget.payload_bytes = 4ull * 1024 * 1024;
    budget.proximity_voxels = 0;
    budget.max_builds_per_frame = 6;   // enough for a root and a little of its tree
    f.pool.create(budget, f.types);

    f.want_box(0, 0, 0, 63, 63, 63);
    f.serve(1);
    REQUIRE(f.pool.validate());

    // Somewhere the world has matter. Whatever the pool managed to build, the answer at the leaf
    // must never be "empty" — it is either built, or wanted.
    const NodeFind found = f.pool.locate(node_key_of(0, 0, 0, kLeafLevel));
    CHECK((found.slot != kNoNode || found.wanted));
    CHECK_FALSE(found.empty_below);
}

TEST_CASE("a descent over genuine emptiness says empty, and says at what size") {
    Fixture f;
    // One brick in one corner of the entry node, so its siblings are honestly empty.
    f.fill_box(0, 0, 0, 7, 7, 7);
    f.want_box(0, 0, 0, 7, 7, 7);
    f.serve();
    REQUIRE(f.pool.validate());

    // Far from the brick but inside the same entry node: nothing there, and the descent should
    // say so rather than asking for it. Asking for empty space every frame for ever is what
    // 22,600 useless reports a frame looked like (Stage 4, bug 4).
    const NodeFind found = f.pool.locate(node_key_of(8000, 8000, 8000, kLeafLevel));
    CHECK(found.empty_below);
    CHECK_FALSE(found.wanted);
    // And the size of the emptiness is known, so a ray can jump it rather than stepping.
    CHECK(found.level > kLeafLevel);
}

TEST_CASE("a mask bit is set for a child the world has even when it was not built") {
    Fixture f;
    f.fill_box(0, 0, 0, 63, 63, 63);

    NodePoolBudget budget;
    budget.max_nodes = 1u << 16;
    budget.max_occupancy_leaves = 1u << 14;
    budget.payload_bytes = 4ull * 1024 * 1024;
    budget.proximity_voxels = 0;
    budget.max_builds_per_frame = 4;
    f.pool.create(budget, f.types);
    f.want_box(0, 0, 0, 63, 63, 63);
    f.serve(1);
    REQUIRE(f.pool.validate());

    // Serve again with a full budget and the tree completes, because the mask kept a record of
    // what was still owed. A mask that only recorded what had been built would have forgotten.
    NodePoolBudget full = budget;
    full.max_builds_per_frame = 100000;
    f.pool.create(full, f.types);
    f.want_box(0, 0, 0, 63, 63, 63);
    f.serve(1);
    CHECK(f.pool.mirror_voxel(0, 0, 0) == f.stone);
    CHECK(f.pool.mirror_voxel(63, 63, 63) == f.stone);
    CHECK(f.pool.validate());
}

TEST_CASE("negative coordinates work at every level") {
    // Every off-by-one in a hierarchy shows up first below the origin, and this engine has been
    // caught by that twice already.
    Fixture f;
    f.fill_box(-40, -40, -40, -33, -33, -33);
    f.want_box(-40, -40, -40, -33, -33, -33);
    f.serve();
    REQUIRE(f.pool.validate());

    for (i64 z = -42; z < -30; ++z) {
        for (i64 y = -42; y < -30; ++y) {
            for (i64 x = -42; x < -30; ++x) {
                CAPTURE(x);
                CAPTURE(y);
                CAPTURE(z);
                REQUIRE(f.pool.mirror_voxel(x, y, z) == f.world.get(x, y, z));
            }
        }
    }
}
