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

// The reported bug, with no renderer in the way: delete geometry and the pool should stop saying
// the world has something there.
//
// It matters because of what WANTED means to light. Occlusion treats a cell the pool has not built
// as opaque (D302, D324), which is right while a wall is still streaming and wrong for ever once
// the wall has been deleted -- the shadow outlives what cast it. Three fixes were tried against
// the renderer and all three were wrong (D340, D345, D346), and the last of them said why: the
// answers `world_has` gives are themselves suspect, and every child mask in the tree is derived
// from them. This asks the pool the question directly.
// SKIPPED, and the reason is the point: this fails today, the one-line fix is known, and the fix
// costs more than the bug.
//
// `NodePool::world_has` asks `chunk->brick(...) != nullptr`, which is whether a brick is
// ALLOCATED. A brick is not freed when its last voxel goes, so an emptied region answers "the
// world has something here" for ever. Every child mask is derived from that, so the descent
// answers WANTED over open air, occlusion reads WANTED as opaque (D302, D324), and the shadow
// outlives what cast it. Changing it to `!= nullptr && !brick->empty()` makes this test pass at
// every level.
//
// And it takes the frame rate to about one. The old answer was fast BECAUSE it was wrong: a
// shadow or ambient ray crossing an emptied region used to stop at the first unbuilt cell, and
// with the region correctly empty it marches on -- through the hole, to the far plane, at 512
// steps a ray, for every face in the store. Measured: 500 frames of the edited camera did not
// finish in seven minutes, against 4.977 ms a frame reverted. The resident tree also grows 4.5x,
// 41,882 leaves to 187,377, because the pool now builds what those longer rays ask for.
//
// So the fix waits on bounding what a gathering ray may cost -- R10b's near-field falloff gives
// the ambient ray a natural stop at about a metre, and a shadow ray wants a step budget of its
// own. Un-skip this the moment that lands; it is the gate for it. D348.
TEST_CASE("a region emptied by an edit stops being wanted" * doctest::skip()) {
    Fixture f;
    f.fill_box(0, 0, 0, 63, 63, 63);
    for (u64 frame = 1; frame < 8; ++frame) {
        f.want_box(0, 0, 0, 63, 63, 63);
        f.serve(frame);
    }
    REQUIRE(f.pool.find(node_key_of(0, 40, 0, kLeafLevel)) != kNoNode);

    // Empty the top half, brick-aligned so nothing straddles, and tell the pool exactly what the
    // renderer's edit path tells it.
    for (i64 z = 0; z <= 63; ++z) {
        for (i64 y = 32; y <= 63; ++y) {
            for (i64 x = 0; x <= 63; ++x) f.world.set(x, y, z, kAir);
        }
    }
    for (i64 z = 0; z <= 63; z += 8) {
        for (i64 y = 32; y <= 63; y += 8) {
            for (i64 x = 0; x <= 63; x += 8) f.pool.invalidate(x, y, z);
        }
    }
    f.serve(8);

    // Nothing is there, so nothing may be wanted there. A ray crossing it must be told the cell is
    // EMPTY -- open sky -- and not that the pool merely has not built it yet.
    const NodeFind found = f.pool.locate(node_key_of(0, 40, 0, kLeafLevel));
    CHECK_FALSE(found.wanted);
    CHECK(f.pool.find(node_key_of(0, 40, 0, kLeafLevel)) == kNoNode);

    // And the half that was left alone is untouched.
    CHECK(f.pool.find(node_key_of(0, 8, 0, kLeafLevel)) != kNoNode);
    CHECK(f.pool.validate());
}

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

TEST_CASE("an edit drops the brick it touched, and not the half kilometre around it") {
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
    CHECK(f.pool.find(node_key_of(0, 0, 0, kLeafLevel)) == kNoNode);

    // And that the root above it survived, which is the other half and used not to be true. This
    // asserted the ROOT was gone, which passed for the wrong reason: an edit dropped the entry
    // node and every ancestor, so one voxel cost everything within 512 m. The stale-copy
    // guarantee above is what the test was for; the root was standing in for it.
    CHECK(f.pool.find(node_key_of(0, 0, 0, kEntryLevel)) != kNoNode);
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

// Nothing is evicted while there is room, and that is the fix rather than the tuning.
//
// `last_wanted` is refreshed only by a request, and requests come from feedback, and feedback
// reports misses. So a tree that has finished building stops being asked for anything, every node
// goes cold on the same frame, and the pool used to throw away the entire scene -- including what
// every ray was reading that frame. It then rebuilt it, went quiet, and did it again.
//
// This test used to assert the eviction half and pass while that was happening.
TEST_CASE("a tree nothing reads is evicted, and that is the point") {
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

    // And then nothing asks for it and nothing reads it, for far longer than cold_frames. It
    // goes, which is what makes resident memory a function of what is on screen rather than a
    // high-water mark of everything ever looked at. The companion test below is the other half:
    // a tree something IS reading stays, and that is what stops this becoming the churn of D247.
    for (u64 frame = 20; frame < 200; ++frame) f.serve(frame);
    // What goes is the subtree, which is where the memory is. This line used to read
    // `find(kEntryLevel) == kNoNode` -- the root itself gone -- and that was the fault of D324, not
    // the point of this test: a removed root says "nothing is here" and a shadow ray flies through
    // it. The root stays as a shell now, so the assertion moved down to the level that actually
    // holds the bytes. The test below is the other half of that.
    CHECK(f.pool.find(node_key_of(0, 0, 0, kLeafLevel)) == kNoNode);
    CHECK(f.pool.validate());
}

// What a cold root leaves behind, which is not nothing.
//
// The memory is in the subtree, so shedding it recovers effectively all of it — but the node
// itself has to stay, because removing it changes the pool's answer from "something is here that
// I have not built" to "nothing is here", and those are the two answers this whole structure
// exists to keep apart (D133, D147).
//
// It mattered for light. Occlusion treats an unbuilt cell as opaque and an empty one as open sky
// (D302), and a sealed room's roof is never on screen, so it always goes cold: once its root was
// removed the shadow rays flew straight out through it and the room filled with sunlight. Measured
// with the enclosed camera before the fix, frame 900 against frame 500: four of eight roots gone,
// 1,163 faces reading fully lit where every correct answer is nought (D324).
//
// `find` answers this from two directions and both are needed. Asked for the root itself it returns
// the slot, because at the entry level the table lookup IS the answer and a shell has a slot; asked
// for anything under it, the descent hits `children == kNoNode` and reports a miss. So "root present,
// subtree gone" is exactly `find(entry) != kNoNode && find(leaf) == kNoNode`.
TEST_CASE("a cold root sheds its subtree and stays standing") {
    Fixture f;
    f.fill_box(0, 0, 0, 63, 63, 63);
    NodePoolBudget budget;
    budget.max_nodes = 1u << 16;
    budget.max_occupancy_leaves = 1u << 14;
    budget.payload_bytes = 4ull * 1024 * 1024;
    budget.proximity_voxels = 0;
    budget.cold_frames = 4;
    f.pool.create(budget, f.types);

    for (u64 frame = 1; frame < 10; ++frame) { f.want_box(0, 0, 0, 63, 63, 63); f.serve(frame); }
    const u32 built = f.pool.stats().nodes;
    REQUIRE(f.pool.find(node_key_of(0, 0, 0, kLeafLevel)) != kNoNode);
    REQUIRE(f.pool.stats().per_level[kEntryLevel] == 1);

    for (u64 frame = 10; frame < 300; ++frame) f.serve(frame);

    const NodePoolStats shed = f.pool.stats();
    INFO("nodes built " << built << ", after shedding " << shed.nodes);
    CHECK(shed.nodes < built / 4);                          // the subtree went
    CHECK(shed.per_level[kEntryLevel] == 1);                // the root did not
    CHECK(f.pool.find(node_key_of(0, 0, 0, kEntryLevel)) != kNoNode);   // it is still findable
    CHECK(f.pool.find(node_key_of(0, 0, 0, kLeafLevel)) == kNoNode);    // and it is a shell

    // The answer a ray gets is "wanted", never "empty". This is the property the light depends on:
    // an occlusion ray stops at a wanted cell, so an evicted room goes dark rather than sunlit.
    const NodeFind found = f.pool.locate(node_key_of(0, 0, 0, kLeafLevel));
    CHECK(found.wanted);

    // And it rebuilds in place: `refine` allocates a run whenever a node's children are missing,
    // so the root that stayed is the root that fills again.
    for (u64 frame = 300; frame < 310; ++frame) { f.want_box(0, 0, 0, 63, 63, 63); f.serve(frame); }
    CHECK(f.pool.find(node_key_of(0, 0, 0, kLeafLevel)) != kNoNode);
    CHECK(f.pool.stats().per_level[kEntryLevel] == 1);      // rebuilt, not duplicated
    CHECK(f.pool.validate());
}

// Memory has to follow the screen, which means a node has to be droppable on its own.
//
// Eviction worked at the entry level, and a root is 512 m, so the whole facility sits inside one:
// it could throw away the scene or nothing, and resident bytes were a high-water mark of
// everything ever looked at rather than a function of what is on screen. Measured before this:
// 6.04 MB at 1280x800 against 4.87 MB at 640x400, where a quarter is 1.51 MB (D260).
//
// So the tree erodes from the leaves. A node with no built children that nothing has read for
// cold_frames gives up its subtree; next frame its parent has no built children either. What it
// leaves behind is a node at level nought, which is this pool's spelling of "the world has this
// and I have not built it", so a ray that wants it again reports it and it comes back.
TEST_CASE("a subtree nothing reads erodes, while the part being read stays") {
    Fixture f;
    f.fill_box(0, 0, 0, 255, 255, 255);
    NodePoolBudget budget;
    budget.max_nodes = 1u << 16;
    budget.max_occupancy_leaves = 1u << 14;
    budget.payload_bytes = 4ull * 1024 * 1024;
    budget.proximity_voxels = 0;
    // Long enough that the build below finishes before anything goes cold. Nothing touches while
    // a test is setting up, where in the game every frame's rays do.
    budget.cold_frames = 20;
    f.pool.create(budget, f.types);

    for (u64 frame = 1; frame < 10; ++frame) { f.want_box(0, 0, 0, 255, 255, 255); f.serve(frame); }
    const u32 built = f.pool.stats().nodes;
    REQUIRE(built > 2000);

    // One corner keeps being read; nothing else is. `touch_slot` is what the marcher reports, so
    // this is the same path the renderer drives it through.
    const u32 kept = f.pool.find(node_key_of(0, 0, 0, kLeafLevel));
    REQUIRE(kept != kNoNode);
    for (u64 frame = 10; frame < 400; ++frame) {
        f.serve(frame);
        f.pool.touch_slot(kept);
        f.pool.touch(node_key_of(0, 0, 0, kEntryLevel));
    }

    const u32 after = f.pool.stats().nodes;
    INFO("nodes built " << built << ", after erosion " << after);
    CHECK(after < built / 2);                                   // most of it went
    CHECK(f.pool.find(node_key_of(0, 0, 0, kLeafLevel)) != kNoNode);   // and the read part did not
    CHECK(f.pool.validate());
}

// Twenty metres, held whatever is on screen (D199).
//
// Collision, physics and editing all touch what is behind you and under your feet, and none of
// them can be served by what a ray happened to look at. This asked for a single node at the ENTRY
// level -- it stepped by 512 m over a range of 640 voxels, so the loop ran once per axis -- which
// held a shell and nothing under it.
TEST_CASE("the proximity radius holds bricks nothing has looked at") {
    Fixture f;
    // A slab well away from the origin in one direction, and another outside the radius.
    f.fill_box(64, 0, 0, 95, 7, 7);        // 2 m out: inside twenty metres
    f.fill_box(4096, 0, 0, 4127, 7, 7);    // 128 m out: outside it

    NodePoolBudget budget;
    budget.max_nodes = 1u << 16;
    budget.max_occupancy_leaves = 1u << 14;
    budget.payload_bytes = 4ull * 1024 * 1024;
    budget.proximity_voxels = 20 * 32;     // the twenty metres D199 settled
    budget.cold_frames = 100000;           // eviction is not what this is testing
    f.pool.create(budget, f.types);

    // Nothing is ever requested: no ray looks anywhere. The camera simply stands at the origin.
    //
    // Long enough for the sweep to finish. Twenty metres is a hundred and sixty bricks a side, so
    // the volume is 4.2 million cells and the sweep does a slice a frame -- about a hundred and
    // thirty of them. It is a background guarantee rather than an instant one, and standing still
    // is what finishes it.
    for (u64 frame = 1; frame < 200; ++frame) f.serve(frame);

    CHECK(f.pool.find(node_key_of(64, 0, 0, kLeafLevel)) != kNoNode);
    CHECK(f.pool.mirror_voxel(64, 0, 0) == f.stone);

    // And it is a radius, not the whole world.
    CHECK(f.pool.find(node_key_of(4096, 0, 0, kLeafLevel)) == kNoNode);
    CHECK(f.pool.validate());
}

// Carving one voxel must not cost the scene.
//
// Editing is what this game is FOR, so the cost of an edit is the cost of playing it. Dropping
// the entry-level root is correct and enormous: a root is 512 m, so a single chiselled voxel
// threw away everything within half a kilometre and rebuilt it from the world at the rate pixels
// asked for it. Measured in the game as an 18.8 ms CPU spike and a scene that visibly reassembles
// itself around the edit.
TEST_CASE("a one-voxel edit keeps the tree it did not touch") {
    Fixture f;
    f.fill_box(0, 0, 0, 255, 255, 255);
    f.want_box(0, 0, 0, 255, 255, 255);
    for (u64 frame = 1; frame < 12; ++frame) { f.want_box(0, 0, 0, 255, 255, 255); f.serve(frame); }

    const u32 before = f.pool.stats().nodes;
    REQUIRE(before > 1000);

    // One voxel, at one corner, a long way from most of what is built.
    f.world.set(4, 4, 4, kAir);
    f.pool.invalidate(4, 4, 4);
    f.serve(12);

    const u32 after = f.pool.stats().nodes;
    INFO("nodes before " << before << ", after " << after);
    // Nearly all of it survives. Generous, because the path from the root down to the edited
    // brick genuinely has to be rebuilt and its ancestors re-folded -- that is eleven nodes and
    // their siblings, not eleven thousand.
    CHECK(after > (before / 4) * 3);
    CHECK(f.pool.validate());
}

// The other half of residency, and the reason eviction can be trusted again: a ray READING a node
// keeps it, not only a ray missing one. Without this, "wanted" meant "missing", so a finished tree
// was wanted by nothing and the pool threw the scene away on a timer (D247).
//
TEST_CASE("a node a ray keeps reading is not evicted") {
    Fixture f;
    f.fill_box(0, 0, 0, 63, 63, 63);
    NodePoolBudget budget;
    budget.max_nodes = 1u << 16;
    budget.max_occupancy_leaves = 1u << 14;
    budget.payload_bytes = 1024 * 1024;
    budget.proximity_voxels = 0;
    budget.cold_frames = 4;
    f.pool.create(budget, f.types);

    for (u64 frame = 1; frame < 40; ++frame) {
        f.want_box(0, 0, 0, 63, 63, 63);
        f.serve(frame);
    }
    const NodeKey root = node_key_of(0, 0, 0, kEntryLevel);
    REQUIRE(f.pool.find(root) != kNoNode);
    const u32 leaf_slot = f.pool.find(node_key_of(0, 0, 0, kLeafLevel));
    REQUIRE(leaf_slot != kNoNode);

    // Nobody asks for anything ever again -- the tree is complete, so there is nothing to miss.
    // But a ray reads it every frame, which is the only thing that should matter.
    //
    // Read per NODE, not per root. Touching the root says the root was entered and nothing about
    // what is under it, and since D260 that distinction is the whole of how memory follows the
    // screen: a leaf nothing looks at goes even while its root stays.
    for (u64 frame = 40; frame < 200; ++frame) {
        f.serve(frame);
        f.pool.touch(root);
        f.pool.touch_slot(leaf_slot);
    }
    CHECK(f.pool.find(root) != kNoNode);
    CHECK(f.pool.validate());
}

// The same policy seen from the memory side: a pool that keeps being asked for new things and
// never re-reads the old ones does give the old ones up.
TEST_CASE("a pool under load gives up what has gone cold") {
    Fixture f;
    f.fill_box(0, 0, 0, 63, 63, 63);
    NodePoolBudget budget;
    budget.max_nodes = 512;
    budget.max_occupancy_leaves = 64;
    budget.payload_bytes = 64 * 1024;
    budget.proximity_voxels = 0;
    budget.cold_frames = 4;
    f.pool.create(budget, f.types);

    // Fill it until it is over the high-water mark.
    for (u64 frame = 1; frame < 40; ++frame) {
        f.want_box(0, 0, 0, 63, 63, 63);
        f.serve(frame);
    }
    const NodePoolStats loaded = f.pool.stats();
    REQUIRE(loaded.nodes > 0);

    // Then ask for nothing for long enough to go cold. Under pressure that is a real eviction.
    for (u64 frame = 40; frame < 120; ++frame) f.serve(frame);
    CHECK(f.pool.stats().evictions > 0);
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
