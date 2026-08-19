// The node pool: one sparse octree replacing the chunk system.
//
// The property that matters most here is the one the chunk system could not have: **the pool and
// the world agree about every voxel, through the same walk the shader performs**. That is what
// `mirror_voxel` is for, and it is the successor to the mirror check that has guarded residency
// since Stage 2 — a renderer reading a structure nobody compares against the world is a renderer
// debugging a mirage.

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "core/jobs.hpp"
#include "core/time.hpp"
#include "forge/clip_script.hpp"
#include "forge/sample.hpp"
#include "game/clip.hpp"
#include "world/ledger.hpp"
#include "world/node_pool.hpp"
#include "world/tags.hpp"
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

    // The same, with a camera at the origin looking down +z through a 90 degree field.
    const NodeUploadBatch& serve_looking(u64 frame) {
        const f64 camera[3] = {0.0, 0.0, 0.0};
        NodeView view;
        view.forward[0] = 0.0f; view.forward[1] = 0.0f; view.forward[2] = 1.0f;
        view.right[0] = 1.0f;   view.right[1] = 0.0f;   view.right[2] = 0.0f;
        view.up[0] = 0.0f;      view.up[1] = 1.0f;      view.up[2] = 0.0f;
        view.tan_half_fov = 1.0f;
        view.aspect = 1.0f;
        view.valid = true;
        return pool.update(world, camera, frame, &view);
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
// `NodePool::world_has` asks `chunk->brick(...) != nullptr`, which is whether a brick is
// ALLOCATED. A brick used not to be freed when its last voxel went, so an emptied region answered
// "the world has something here" for ever. Every child mask is derived from that, so the descent
// answered WANTED over open air, occlusion reads WANTED as opaque (D302, D324), and the shadow
// outlived what cast it.
//
// The reader is not what changed. Making `world_has` test emptiness is correct and costs a scan
// past every emptied brick -- 726 ms of CPU a frame after a large deletion, and the test was left
// skipped for it (D348, D349). The brick is now freed inside `Chunk::set`, at the moment its last
// voxel is cleared, so `!= nullptr` is both correct and the O(1) answer it always was. This test
// does not care which of the two it is: it asks the pool the question the renderer asks.
TEST_CASE("a region emptied by an edit stops being wanted") {
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

TEST_CASE("a node's coverage is its PROJECTION along each direction, and the fold is a maximum") {
    // R5d blends a pixel by this byte, so what it means has to be pinned rather than believed.
    // The renderer has two coverage-like numbers on a node and using the wrong one is a mistake this
    // project has already made twice from opposite sides (trap 6): the alpha of the filtered colour
    // is a VOLUMETRIC fill fraction that halves at every level, and these six bytes are a PROJECTED
    // one, per direction, which is the fraction of a pixel that is matter.
    Fixture f;
    // A post two voxels square running the full height of one brick, which is the shape the whole
    // stage exists for -- a railing at a distance.
    f.fill_box(0, 0, 0, 1, 7, 1);
    f.want_box(0, 0, 0, 0, 0, 0);
    f.serve();

    auto faces_at = [&f](i64 x, i64 y, i64 z, u32 level, u32 out[6]) {
        const u32 slot = f.pool.find(node_key_of(x, y, z, level));
        REQUIRE(slot != kNoNode);
        const u32 xy = f.pool.nodes()[slot].coverage_xy;
        const u32 zz = f.pool.nodes()[slot].coverage_z;
        out[0] = xy & 0xFFu;
        out[1] = (xy >> 8) & 0xFFu;
        out[2] = (xy >> 16) & 0xFFu;
        out[3] = (xy >> 24) & 0xFFu;
        out[4] = zz & 0xFFu;
        out[5] = (zz >> 8) & 0xFFu;
    };

    u32 leaf[6];
    faces_at(0, 0, 0, kLeafLevel, leaf);
    // Looked at from the side, the post covers two of the eight columns across and all eight down:
    // 16 of 64, which the byte carries as 16 * 255 / 64 = 63. From above it is two by two, 4 of 64,
    // which is 15. Exact both times, and the same figure both ways along an axis because a
    // projection has no sense to it.
    CHECK(leaf[0] == 63);   // +x
    CHECK(leaf[1] == 63);   // -x
    CHECK(leaf[2] == 15);   // +y
    CHECK(leaf[3] == 15);   // -y
    CHECK(leaf[4] == 63);   // +z
    CHECK(leaf[5] == 63);   // -z

    // ...and the FOLD is a maximum over the children, not a projection of its own.
    //
    // That is a deliberate choice made where `fold_children` makes it -- it errs towards *present*,
    // on the same argument as the floor of one that stops a single voxel rounding away -- and the
    // consequence belongs here rather than only in a comment, because it is what limits R5d. The
    // level above this holds eight of these bricks and only one of them has the post in it, so the
    // TRUE projection there is a quarter of what the brick reports; the max hands back the brick's
    // figure unchanged. A coarse node therefore reads as more solid than it is, and the edge
    // anti-aliasing it gets is conservative rather than excessive -- which is the safe direction,
    // and is why a silhouette across a node that is half solid wall gets no blending at all.
    u32 above[6];
    faces_at(0, 0, 0, kLeafLevel + 1, above);
    for (u32 face = 0; face < 6; ++face) {
        CAPTURE(face);
        CHECK(above[face] == leaf[face]);
    }
}

TEST_CASE("a solid node covers all of itself, in all six directions") {
    // The other end of the same rule, and the one R5d's threshold turns on: a face-on projection of
    // anything solid is full, so a flat wall reports 255 and never pays for a second march. If this
    // ever stopped being true the stage would cost a march on every coarse pixel in the frame.
    Fixture f;
    f.fill_box(0, 0, 0, 7, 7, 7);
    f.want_box(0, 0, 0, 0, 0, 0);
    f.serve();

    const u32 slot = f.pool.find(node_key_of(0, 0, 0, kLeafLevel));
    REQUIRE(slot != kNoNode);
    CHECK(f.pool.nodes()[slot].coverage_xy == 0xFFFFFFFFu);
    CHECK((f.pool.nodes()[slot].coverage_z & 0xFFFFu) == 0xFFFFu);
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

TEST_CASE("an edit refreshes the brick it touched, and not the half kilometre around it") {
    Fixture f;
    f.fill_box(0, 0, 0, 7, 7, 7);
    f.want_box(0, 0, 0, 7, 7, 7);
    f.serve(1);
    REQUIRE(f.pool.find(node_key_of(0, 0, 0, kLeafLevel)) != kNoNode);

    f.world.set(0, 0, 0, kAir);
    f.pool.invalidate(0, 0, 0);
    // Deliberately NOT wanted again: no request is made on this frame, so nothing but the edit
    // path itself can put the brick back.
    f.serve(2);

    // Present, and holding what the world holds.
    //
    // This asserted `== kNoNode` and passed for a reason the comment beside it named honestly: the
    // property under test is that the STALE copy is gone -- a resident-but-wrong node is the one
    // thing feedback can never discover, because a ray that finds it does not report it (D131) --
    // and absence was standing in for that. Absence is a weaker claim than correctness AND it is a
    // visible fault: while a brick is missing the marcher paints an ancestor's folded colour over
    // the whole cell (R2d), so an edit flashed a flat slab of the wrong colour for the three frames
    // the feedback round trip takes to ask for it back. The edit path knows the brick and the world
    // holds the answer, so it re-derives it in place and the stand-in never runs.
    //
    // Checked through `mirror_voxel`, which reads the way the shader reads -- entry hash, descent,
    // child mask, leaf payload -- so this is the stale-copy guarantee itself rather than a proxy
    // for it.
    REQUIRE(f.pool.find(node_key_of(0, 0, 0, kLeafLevel)) != kNoNode);
    CHECK(f.pool.mirror_voxel(0, 0, 0) == kAir);
    CHECK(f.pool.mirror_voxel(1, 0, 0) == f.stone);

    // And that the root above it survived, which is the other half and used not to be true. This
    // asserted the ROOT was gone, which passed for the wrong reason: an edit dropped the entry
    // node and every ancestor, so one voxel cost everything within 512 m.
    CHECK(f.pool.find(node_key_of(0, 0, 0, kEntryLevel)) != kNoNode);
    CHECK(f.pool.validate());
}

// The same thing when the edit empties the brick outright, which is the case the refresh must NOT
// answer by keeping a node: a brick with nothing in it is not a node, it is the absence of one, and
// the parent's child mask is where that is said. Getting this wrong is D133 -- a bit left set over
// nothing is a ray reporting a node the world does not have, every frame, for ever.
TEST_CASE("an edit that empties a brick leaves no node behind") {
    Fixture f;
    f.fill_box(0, 0, 0, 7, 7, 7);
    f.want_box(0, 0, 0, 7, 7, 7);
    f.serve(1);
    REQUIRE(f.pool.find(node_key_of(0, 0, 0, kLeafLevel)) != kNoNode);

    for (i64 z = 0; z < 8; ++z) {
        for (i64 y = 0; y < 8; ++y) {
            for (i64 x = 0; x < 8; ++x) f.world.set(x, y, z, kAir);
        }
    }
    f.pool.invalidate(0, 0, 0);
    f.serve(2);

    CHECK(f.pool.find(node_key_of(0, 0, 0, kLeafLevel)) == kNoNode);
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

// The instrument D425 asked for, which is what found D427: a count of evictions that took a node
// the camera was looking at.
//
// It has to be an independent witness or it is worth nothing. `node_last_read_` decides what is
// cold, it is stamped from feedback, and feedback was the thing under suspicion -- a count taken
// from that signal would have agreed with the policy however wrong the policy was. So the test is
// that the frustum, and only the frustum, decides which side of the count an eviction lands on.
TEST_CASE("the eviction instrument counts what the camera could see, and nothing else") {
    Fixture f;
    // Two boxes, one in front of a camera looking down +z and one directly behind it. Both are
    // asked for, both go cold, and both are evicted -- the policy does not know the difference and
    // must not start to.
    f.fill_box(64, -32, 256, 127, 31, 319);      // ahead
    f.fill_box(64, -32, -320, 127, 31, -257);    // behind
    NodePoolBudget budget;
    budget.max_nodes = 1u << 16;
    budget.max_occupancy_leaves = 1u << 14;
    budget.payload_bytes = 4ull * 1024 * 1024;
    budget.proximity_voxels = 0;
    budget.cold_frames = 4;
    f.pool.create(budget, f.types);

    for (u64 frame = 1; frame < 20; ++frame) {
        f.want_box(64, -32, 256, 127, 31, 319);
        f.want_box(64, -32, -320, 127, 31, -257);
        f.serve_looking(frame);
    }
    REQUIRE(f.pool.find(node_key_of(64, 0, 256, kLeafLevel)) != kNoNode);
    REQUIRE(f.pool.find(node_key_of(64, 0, -320, kLeafLevel)) != kNoNode);

    u32 evicted = 0;
    u32 on_screen = 0;
    for (u64 frame = 20; frame < 200; ++frame) {
        const NodeUploadBatch& batch = f.serve_looking(frame);
        evicted += batch.evicted_nodes;
        on_screen += batch.evicted_on_screen;
    }
    // Both halves went, and only one of them was ever on screen. Half the box behind the camera
    // shares no plane with the one in front, so the split is not a near miss.
    CHECK(evicted > 0);
    CHECK(on_screen > 0);
    CHECK(on_screen < evicted);
    CHECK(f.pool.validate());
}

// The other half of the instrument, and the one that measures the harm rather than a proxy for it:
// a node the pool evicted and then had to build again straight away.
//
// D427's whole finding is this number being large on a camera that is not moving -- 37,606 leaves
// at 1280x800 over one settled run, none of which any ray had ever reported reading.
TEST_CASE("a node evicted and immediately wanted again is counted as churn") {
    Fixture f;
    f.fill_box(0, 0, 0, 63, 63, 63);
    NodePoolBudget budget;
    budget.max_nodes = 1u << 16;
    budget.max_occupancy_leaves = 1u << 14;
    budget.payload_bytes = 4ull * 1024 * 1024;
    budget.proximity_voxels = 0;
    budget.cold_frames = 4;
    f.pool.create(budget, f.types);

    for (u64 frame = 1; frame < 20; ++frame) {
        f.want_box(0, 0, 0, 63, 63, 63);
        f.serve(frame);
    }
    REQUIRE(f.pool.stats().churn == 0);   // nothing has been evicted yet, so nothing can come back

    // Silence long enough to go cold, which evicts it...
    for (u64 frame = 20; frame < 60; ++frame) f.serve(frame);
    REQUIRE(f.pool.stats().evictions > 0);

    // ...and then ask for exactly what was thrown away. That is the loop, and this is the number
    // that names it: a rebuild is invisible in `built`, which cannot tell a node arriving for the
    // first time from one arriving for the fifth.
    f.want_box(0, 0, 0, 63, 63, 63);
    const NodeUploadBatch& batch = f.serve(61);
    CHECK(batch.churned > 0);
    CHECK(f.pool.stats().churn == batch.churned);

    // And a node that comes back long after it went is not churn: the cold window is meant to be
    // long enough that turning round and back costs nothing, so a return outside kChurnWindow is
    // the policy working rather than failing.
    for (u64 frame = 62; frame < 100; ++frame) f.serve(frame);
    const u64 settled = f.pool.stats().churn;
    for (u64 frame = 100; frame < 100 + kChurnWindow + 40; ++frame) f.serve(frame);
    f.want_box(0, 0, 0, 63, 63, 63);
    f.serve(100 + kChurnWindow + 40);
    CHECK(f.pool.stats().churn == settled);
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

// A writer that changes the world and does not say so, which is what the clip ladder was.
//
// The pool's own machinery cannot notice this and is not supposed to: feedback reports what a ray
// could not FIND, and a brick that is resident but out of date is found every time. So the fault
// is silent by construction -- the GPU mirror matches, the node count is healthy, and the tree the
// renderer walks holds a shape the world gave up seconds ago. `stale_leaves` is the instrument
// that says so, and this is the case it was written against. D397.
TEST_CASE("a leaf the world has rewritten behind the pool's back is reported, and invalidate fixes it") {
    Fixture f;
    f.fill_box(0, 0, 0, 15, 15, 15);
    f.want_box(0, 0, 0, 15, 15, 15);
    f.serve(1);
    REQUIRE(f.pool.validate());
    REQUIRE(f.pool.stale_leaves(f.world) == 0);

    // The world changes and nobody tells the pool -- a paste, not an edit.
    f.world.set(4, 4, 4, kAir);
    f.serve(2);

    NodeKey first{};
    CHECK(f.pool.stale_leaves(f.world, &first) == 1);
    CHECK(first.level == kLeafLevel);
    CHECK(first.x == 0);
    // ...and the pool is still drawing the voxel the world gave up, which is the fault the count
    // is standing in for.
    CHECK(f.pool.mirror_voxel(4, 4, 4) == f.stone);

    // Told, it agrees again.
    f.pool.invalidate(4, 4, 4);
    f.want_box(0, 0, 0, 15, 15, 15);
    f.serve(3);
    CHECK(f.pool.stale_leaves(f.world) == 0);
    CHECK(f.pool.mirror_voxel(4, 4, 4) == kAir);
    CHECK(f.pool.validate());
}

// The other half of it: a brick the world has given up entirely, which is what a paste that
// empties a region leaves behind. Occupancy cannot be compared against a brick that is not there,
// so this is the branch that has to answer "gone" rather than "different".
TEST_CASE("a leaf whose brick the world has dropped is reported as stale") {
    Fixture f;
    f.fill_box(0, 0, 0, 7, 7, 7);
    f.want_box(0, 0, 0, 7, 7, 7);
    f.serve(1);
    REQUIRE(f.pool.stale_leaves(f.world) == 0);

    for (i64 z = 0; z < 8; ++z) {
        for (i64 y = 0; y < 8; ++y) {
            for (i64 x = 0; x < 8; ++x) f.world.set(x, y, z, kAir);
        }
    }
    f.serve(2);
    CHECK(f.pool.stale_leaves(f.world) == 1);
}

// D515. The edit refresh descends the pool's own tree and prunes to the edited box, instead of
// being handed every brick in that box and walking up from each one. Same answer, and the cost is
// the number of built nodes an edit reaches rather than the volume it covers -- 718 ms to 7 on a
// 36-million-voxel delete, which is the whole reason for the change.
//
// What it risks is the field nothing was comparing. A child mask decides where a ray is ALLOWED to
// look, so refreshing the wrong set of nodes leaves either a bit set over nothing -- a phantom
// request every frame for ever, D133 -- or a bit clear over something, which is geometry no
// feedback will ever ask for because feedback only reports what a ray could not find. Both are
// silent: the GPU mirror agrees, `stale_leaves` agrees, and the count of nodes looks healthy.
//
// So the gate is `stale_masks`, and it is checked against an arm that must NOT be clean, because a
// detector that never fires and a pool that is never wrong look identical from the clean side
// (trap 15).
TEST_CASE("an edited box refreshes every mask under it, and an unannounced edit does not") {
    Fixture f;
    // Two bricks apart in every axis, so the edit lands well inside a built subtree and there are
    // ancestors above it that have to be re-derived rather than merely a leaf to drop.
    f.fill_box(0, 0, 0, 63, 63, 63);
    f.want_box(0, 0, 0, 63, 63, 63);
    f.serve(1);
    REQUIRE(f.pool.validate());
    REQUIRE(f.pool.stale_leaves(f.world) == 0);
    REQUIRE(f.pool.stale_masks(f.world) == 0);

    // Empty one whole brick, so the world genuinely loses a child and the masks above it change
    // rather than only the leaf's contents.
    for (i64 z = 8; z < 16; ++z) {
        for (i64 y = 8; y < 16; ++y) {
            for (i64 x = 8; x < 16; ++x) f.world.set(x, y, z, kAir);
        }
    }

    SUBCASE("nobody tells the pool, so it is wrong and says so") {
        f.serve(2);
        NodeKey first{};
        CHECK(f.pool.stale_masks(f.world, &first) > 0);
        CHECK(first.level > kLeafLevel);
    }

    SUBCASE("the box is announced, so every mask under it comes right") {
        const i64 lo[3] = {8, 8, 8};
        const i64 hi[3] = {15, 15, 15};
        f.pool.invalidate_box(lo, hi);
        f.serve(2);
        CHECK(f.pool.stale_masks(f.world) == 0);
        CHECK(f.pool.stale_leaves(f.world) == 0);
        CHECK(f.pool.validate());
    }

    SUBCASE("one brick announced as a box is the same as announcing it as a brick") {
        // `invalidate` is now `invalidate_box` over one brick, so the narrow path a single-voxel
        // chisel takes has to land on the identical answer.
        f.pool.invalidate(8, 8, 8);
        f.serve(2);
        CHECK(f.pool.stale_masks(f.world) == 0);
        CHECK(f.pool.stale_leaves(f.world) == 0);
    }
}

// The cost claim itself, headless: announcing a box must not depend on how many bricks it spans.
//
// Measured as WORK rather than as time, because a wall-clock assertion in a test suite is a
// flake waiting for a busy machine. The old shape visited one node per brick in the box; this one
// visits only nodes that exist, so a box covering thousands of empty bricks costs what the built
// tree under it costs and nothing more.
TEST_CASE("announcing a large empty box costs what the pool holds, not what the box spans") {
    Fixture f;
    f.fill_box(0, 0, 0, 15, 15, 15);
    f.want_box(0, 0, 0, 15, 15, 15);
    f.serve(1);
    REQUIRE(f.pool.stale_masks(f.world) == 0);

    const u32 built = f.pool.node_watermark();

    // A box a thousand bricks a side, almost entirely over empty world. Per brick this is a billion
    // announcements; from the tree it is the handful of nodes that are actually there.
    const i64 lo[3] = {-4096, -4096, -4096};
    const i64 hi[3] = {4095, 4095, 4095};
    f.pool.invalidate_box(lo, hi);
    f.serve(2);

    // Nothing was invented and nothing was lost: the pool still holds what it held, and it still
    // agrees with the world on both fields.
    CHECK(f.pool.node_watermark() == built);
    CHECK(f.pool.stale_masks(f.world) == 0);
    CHECK(f.pool.stale_leaves(f.world) == 0);
    CHECK(f.pool.validate());
}

// ==================================================================================================
// R8b -- hashed variation: where a node's children come from when nothing else can answer
// ==================================================================================================
//
// The child source for a world with no field behind it. `21-renderer-rewrite.md` section 7 lists
// three, in the order they are tried -- the material's own field, then this, then what the player
// carved -- and this is the middle one and the only one that is ALWAYS available. A hand-carved
// world has no field by construction; nor has a world loaded from a file whose clip is gone.
//
// Everything below is headless and there is a reason it is headless FIRST. Residency was built this
// way in Stage 2, two of R1a's four bugs were caught by a mirror walker and by nothing else, and
// the sentence that explains both is that a structure the renderer walks and nobody compares
// against the world is a renderer debugging a mirage. So `mirror_variation` exists, walks the tree
// exactly as `shaders/variation.glsl` walks it, and is asserted against the world before the
// renderer is allowed anywhere near it.

namespace {

// The pool, with the source switched to whichever arm the case wants. One binary, one flag (D407).
struct VariationFixture {
    VoxelTypeTable types;
    World world;
    NodePool pool;
    VoxelTypeId stone = kAir;
    VoxelTypeId brass = kAir;

    explicit VariationFixture(bool source_on, u32 seed = kVariationSeed) {
        VisualRecord grey{};
        grey.red = 128; grey.green = 130; grey.blue = 126;
        stone = types.intern(grey, BehaviourRecord{});
        VisualRecord yellow{};
        yellow.red = 200; yellow.green = 170; yellow.blue = 40;
        brass = types.intern(yellow, BehaviourRecord{});

        NodePoolBudget budget;
        budget.max_nodes = 1u << 16;
        budget.max_occupancy_leaves = 1u << 14;
        budget.payload_bytes = 4ull * 1024 * 1024;
        budget.proximity_voxels = 0;
        budget.hashed_variation = source_on;
        budget.variation_seed = seed;
        pool.create(budget, types);
    }

    void fill_box(i64 x0, i64 y0, i64 z0, i64 x1, i64 y1, i64 z1, VoxelTypeId type) {
        for (i64 z = z0; z <= z1; ++z) {
            for (i64 y = y0; y <= y1; ++y) {
                for (i64 x = x0; x <= x1; ++x) world.set(x, y, z, type);
            }
        }
    }

    void want_box(i64 x0, i64 y0, i64 z0, i64 x1, i64 y1, i64 z1) {
        for (i64 z = z0; z <= z1; z += 8) {
            for (i64 y = y0; y <= y1; y += 8) {
                for (i64 x = x0; x <= x1; x += 8) pool.request(node_key_of(x, y, z, kLeafLevel));
            }
        }
    }

    void serve(u64 frame) {
        const f64 camera[3] = {0.0, 0.0, 0.0};
        pool.update(world, camera, frame);
    }
};

}  // namespace

TEST_CASE("the variation chain is integer from end to end, and its constants are pinned") {
    // Not a tautology, and it is worth saying why. `shaders/variation.glsl` is a second
    // implementation of every function in this block, and the only thing that keeps two
    // implementations in step is a set of values written down where a change to either has to come
    // and edit them. If one of these moves, the shader moves with it.
    CHECK(node_hash_mix(0u) == 0u);
    CHECK(variation_root_hash(0, 0, 0, 0u) ==
          node_hash_mix(node_hash_mix(node_hash_mix(node_hash_mix(0x9E3779B9u)))));

    // The quantiser, which is the one floating-point step in the whole source and happens once.
    CHECK(variation_amount_q(0.0f) == 0u);
    CHECK(variation_amount_q(-1.0f) == 0u);
    CHECK(variation_amount_q(0.05f) == 13u);     // section 7's own worked example: colour=0.05
    CHECK(variation_amount_q(1.0f) == 256u);
    CHECK(variation_amount_q(4.0f) == 256u);     // clamped, never wrapped

    // With no amount at all, a child is its parent exactly. That is the control arm's arithmetic
    // and it has to be an identity rather than "very nearly".
    for (u32 channel = 0; channel < 256; ++channel) {
        CHECK(variation_nudge(channel, 0u, 0u) == channel);
        CHECK(variation_nudge(channel, 255u, 0u) == channel);
    }

    // Full scale saturates and never wraps. A wrapped byte is a black speck in a white wall.
    CHECK(variation_nudge(0u, 0u, 256u) == 0u);
    CHECK(variation_nudge(255u, 255u, 256u) == 255u);
    CHECK(variation_nudge(128u, 0u, 256u) == 0u);
    CHECK(variation_nudge(128u, 255u, 256u) == 255u);

    // And at the shipping amount it is a nudge rather than a repaint: 0.05 of full scale is about
    // thirteen of 255, either side, and the two extremes of the byte are what those look like.
    CHECK(variation_nudge(128u, 255u, variation_amount_q(0.05f)) == 140u);
    CHECK(variation_nudge(128u, 0u, variation_amount_q(0.05f)) == 116u);
    CHECK(variation_nudge(128u, 128u, variation_amount_q(0.05f)) == 128u);   // the middle moves not

    // Alpha is coverage and is carried through untouched, at every amount.
    for (u32 amount = 0; amount <= 256; amount += 32) {
        CHECK((variation_child_colour(0xC0806040u, 0x12345678u, amount) & 0xFF000000u) ==
              0xC0000000u);
    }
}

TEST_CASE("same key, same children -- twice, on two pools that never met") {
    // The promise the source makes, and the only way to check it is to make it twice from nothing.
    VariationFixture a(true);
    VariationFixture b(true);
    a.fill_box(0, 0, 0, 15, 15, 15, a.stone);
    b.fill_box(0, 0, 0, 15, 15, 15, b.stone);
    a.want_box(0, 0, 0, 15, 15, 15);
    b.want_box(0, 0, 0, 15, 15, 15);
    a.serve(1);
    b.serve(1);

    const i64 lo[3] = {0, 0, 0};
    const i64 hi[3] = {15, 15, 15};
    u64 cells_a = 0, cells_b = 0;
    const u64 hash_a = a.pool.variation_fingerprint(a.world, lo, hi, 1, 4, &cells_a);
    const u64 hash_b = b.pool.variation_fingerprint(b.world, lo, hi, 1, 4, &cells_b);
    CHECK(cells_a == 4096);
    CHECK(cells_a == cells_b);
    CHECK(hash_a == hash_b);

    // ...and the same walk, cell for cell, two levels down, over the whole box. A fingerprint that
    // agreed while a cell did not would be a fingerprint reading past what it is supposed to cover.
    for (i64 z = 0; z < 16 * 4; ++z) {
        for (i64 y = 0; y < 16 * 4; ++y) {
            for (i64 x = 0; x < 16 * 4; ++x) {
                const VariationSample sa = a.pool.mirror_variation(x, y, z, 2);
                const VariationSample sb = b.pool.mirror_variation(x, y, z, 2);
                REQUIRE(sa.hash == sb.hash);
                REQUIRE(sa.colour == sb.colour);
                REQUIRE(sa.matter == sb.matter);
            }
        }
    }

    // A different seed is a different world's grain. Without this the case above would pass on a
    // source that ignored its seed entirely.
    VariationFixture other(true, kVariationSeed ^ 0xABCDu);
    other.fill_box(0, 0, 0, 15, 15, 15, other.stone);
    other.want_box(0, 0, 0, 15, 15, 15);
    other.serve(1);
    CHECK(other.pool.variation_fingerprint(other.world, lo, hi, 1, 4, nullptr) != hash_a);
}

TEST_CASE("the variation mirror agrees with the shader's descent, voxel for voxel") {
    // The standard every reader in this file is held to. At depth 0 the source must answer exactly
    // what `mirror_voxel` answers -- the walk the shader performs -- and at every depth below it
    // the MATTER must still be that answer, because this source varies the material and never the
    // shape.
    VariationFixture f(true);
    f.fill_box(0, 0, 0, 23, 23, 23, f.stone);
    f.fill_box(8, 8, 8, 15, 15, 15, f.brass);      // a different material inside it
    f.fill_box(10, 10, 10, 13, 13, 13, kAir);      // and a void inside that
    f.want_box(-8, -8, -8, 31, 31, 31);
    f.serve(1);
    REQUIRE(f.pool.stale_masks(f.world) == 0);
    REQUIRE(f.pool.stale_leaves(f.world) == 0);

    u64 solid = 0;
    for (i64 z = -8; z < 32; ++z) {
        for (i64 y = -8; y < 32; ++y) {
            for (i64 x = -8; x < 32; ++x) {
                const VoxelTypeId walked = f.pool.mirror_voxel(x, y, z);
                REQUIRE(walked == f.world.get(x, y, z));

                const VariationSample at = f.pool.mirror_variation(x, y, z, 0);
                REQUIRE(at.type == walked);
                REQUIRE(at.matter == (walked != kAir));
                if (walked != kAir) ++solid;

                // Three levels down, in the near cell and the far cell of the same voxel. Matter is
                // the voxel's; the chain seed is not.
                for (u32 depth = 1; depth <= 3; ++depth) {
                    const i64 base = static_cast<i64>(1) << depth;
                    const VariationSample near_cell =
                        f.pool.mirror_variation(x * base, y * base, z * base, depth);
                    const VariationSample far_cell = f.pool.mirror_variation(
                        x * base + base - 1, y * base + base - 1, z * base + base - 1, depth);
                    REQUIRE(near_cell.matter == (walked != kAir));
                    REQUIRE(far_cell.matter == (walked != kAir));
                    REQUIRE(near_cell.type == walked);
                    REQUIRE(far_cell.type == walked);
                    if (walked != kAir) REQUIRE(near_cell.hash != far_cell.hash);
                }
            }
        }
    }
    CHECK(solid > 0);
}

TEST_CASE("a negative coordinate is one voxel out or it is not, and the mirror says which") {
    // `node_key_of`'s trap, one level further down. A cell at -1 belongs to voxel -1, not to voxel
    // 0, and an implementation that divides instead of shifting puts every negative coordinate one
    // voxel out -- silently, and only over the half of the world nobody's test world covers.
    VariationFixture f(true);
    f.fill_box(-16, -16, -16, -1, -1, -1, f.stone);
    f.want_box(-16, -16, -16, -1, -1, -1);
    f.serve(1);

    for (u32 depth = 1; depth <= 3; ++depth) {
        const i64 span = static_cast<i64>(1) << depth;
        CHECK(f.pool.mirror_variation(-1, -1, -1, depth).matter);
        CHECK(f.pool.mirror_variation(-span, -span, -span, depth).matter);
        CHECK_FALSE(f.pool.mirror_variation(0, 0, 0, depth).matter);
        CHECK(f.pool.mirror_variation(-1, -1, -1, depth).type == f.stone);
        CHECK(f.pool.mirror_variation(-span * 16, 0, 0, depth).type == kAir);
    }
}

TEST_CASE("the grain compounds down the chain, and the control arm has none of it") {
    VariationFixture on(true);
    VariationFixture off(false);
    on.fill_box(0, 0, 0, 7, 7, 7, on.stone);
    off.fill_box(0, 0, 0, 7, 7, 7, off.stone);
    on.want_box(0, 0, 0, 7, 7, 7);
    off.want_box(0, 0, 0, 7, 7, 7);
    on.serve(1);
    off.serve(1);

    const VariationSample voxel = on.pool.mirror_variation(3, 3, 3, 0);
    REQUIRE(voxel.matter);

    // Off: a sub-voxel cell is its voxel, exactly. That is what a build with no child source draws
    // -- the parent stands in -- so the control arm is a real arm and not a disabled feature.
    CHECK_FALSE(off.pool.hashed_variation());
    const u32 plain = off.pool.mirror_variation(3, 3, 3, 0).colour;
    for (u32 depth = 1; depth <= 6; ++depth) {
        const i64 base = static_cast<i64>(1) << depth;
        CHECK(off.pool.mirror_variation(3 * base + 1, 3 * base + 2, 3 * base + 3, depth).colour ==
              plain);
    }

    // On: it moves, and it never leaves the byte or touches the coverage.
    CHECK(on.pool.hashed_variation());
    u32 moved = 0;
    for (u32 depth = 1; depth <= 6; ++depth) {
        const i64 base = static_cast<i64>(1) << depth;
        const VariationSample cell =
            on.pool.mirror_variation(3 * base + 1, 3 * base + 2, 3 * base + 3, depth);
        REQUIRE(cell.matter);
        CHECK((cell.colour >> 24) == (voxel.colour >> 24));   // coverage untouched
        if (cell.colour != voxel.colour) ++moved;
    }
    CHECK(moved >= 5);

    // The eight children of one parent are eight different chains. A source whose siblings shared
    // a seed would draw eight identical cells and read as no variation at all.
    const VariationChildren kids = on.pool.variation_children_of(3, 3, 3, 0);
    CHECK(kids.mask == 0xFFu);
    u32 distinct = 0;
    for (u32 a = 0; a < 8; ++a) {
        bool unique = true;
        for (u32 b = 0; b < a; ++b) unique = unique && kids.hash[a] != kids.hash[b];
        distinct += unique ? 1u : 0u;
    }
    CHECK(distinct == 8);

    // Air has no children, on either arm. A source that gave air eight colours would be a source
    // with an opinion about shape.
    const VariationChildren nothing = on.pool.variation_children_of(64, 64, 64, 0);
    CHECK(nothing.mask == 0u);
    for (u32 octant = 0; octant < 8; ++octant) CHECK(nothing.colour[octant] == 0u);
}

TEST_CASE("only the low bits of a sub-voxel coordinate are read") {
    // The claim `shaders/variation.glsl` rests on, and the reason the marcher can call it at all.
    //
    // A ray inside a brick holds the cell's LOCAL offset within its voxel, not an absolute
    // sub-voxel coordinate — and an absolute one would not fit in the `int` the shader works in
    // past about depth 7 at world scale. `variation_octant` reads bit `step` of each axis and
    // `step` never reaches `depth`, so the two are the same walk. If that ever stopped being true
    // the shader would drift from the CPU silently, on the deep end only, where nobody looks.
    VariationFixture f(true);
    f.fill_box(0, 0, 0, 7, 7, 7, f.stone);
    f.want_box(0, 0, 0, 7, 7, 7);
    f.serve(1);

    constexpr u32 kDepth = 5;
    constexpr i64 kSpan = static_cast<i64>(1) << kDepth;
    const i64 vx = 5, vy = 2, vz = 6;
    const VariationSample voxel = f.pool.mirror_variation(vx, vy, vz, 0);
    REQUIRE(voxel.matter);

    for (i64 ox = 0; ox < kSpan; ox += 7) {
        for (i64 oy = 0; oy < kSpan; oy += 5) {
            for (i64 oz = 0; oz < kSpan; oz += 3) {
                // What the pool answers, from the absolute coordinate.
                const VariationSample absolute = f.pool.mirror_variation(
                    vx * kSpan + ox, vy * kSpan + oy, vz * kSpan + oz, kDepth);

                // ...and the same walk driven by the LOCAL offset alone, which is the form the
                // shader's `variation_descend` takes.
                u32 hash = variation_root_hash(vx, vy, vz, f.pool.variation_seed());
                u32 colour = voxel.colour;
                for (u32 step = kDepth; step > 0; --step) {
                    const u32 octant = octant_of(ox >> (step - 1), oy >> (step - 1), oz >> (step - 1));
                    hash = variation_child_hash(hash, octant);
                    colour = variation_child_colour(colour, hash,
                                                    variation_amount_q(kVariationColour));
                }
                REQUIRE(absolute.hash == hash);
                REQUIRE(absolute.colour == colour);
            }
        }
    }
}

TEST_CASE("a hand-carved world with no clip behind it subdivides, and keeps subdividing") {
    // The case the source exists for: nobody sampled this, no field can be asked about it, and it
    // still has to have children at whatever depth somebody walks to.
    VariationFixture f(true);
    for (i64 i = 0; i < 64; ++i) f.world.set(i, i / 2, (i * 3) % 17, f.stone);   // a carved scrawl
    f.want_box(-8, -8, -8, 71, 39, 23);
    f.serve(1);

    for (i64 i = 0; i < 64; ++i) {
        const i64 x = i, y = i / 2, z = (i * 3) % 17;
        REQUIRE(f.pool.mirror_variation(x, y, z, 0).matter);
        // Twenty levels below a voxel is 3.125 cm / 2^20 -- thirty nanometres, past anything a
        // 32-bit ray can tell apart (D156), and it still answers. Depth is unbounded because the
        // tree is the coordinate: there is no sub-voxel coordinate anywhere to overflow.
        for (u32 depth : {1u, 4u, 12u, 20u}) {
            const i64 base = static_cast<i64>(1) << depth;
            const VariationSample deep =
                f.pool.mirror_variation(x * base, y * base, z * base, depth);
            REQUIRE(deep.matter);
            REQUIRE(deep.type == f.stone);
            REQUIRE(deep.depth == depth);
        }
    }

    // Walking towards it and away again is the same grain. Asked twice, six hundred frames apart,
    // because the fault this rules out is a source that remembers anything at all.
    const i64 lo[3] = {0, 0, 0};
    const i64 hi[3] = {63, 31, 16};
    const u64 first = f.pool.variation_fingerprint(f.world, lo, hi, 1, 3, nullptr);
    f.serve(2);
    f.serve(600);
    CHECK(f.pool.variation_fingerprint(f.world, lo, hi, 1, 3, nullptr) == first);
}

// ==================================================================================================
// The facility, voxel for voxel. Skipped by default -- it samples a real clip.
//
//   build\bin\ws_tests.exe --no-skip --test-case="R8b over the whole facility"
//
// This is R8b's gate and it is deliberately not a synthetic box. The synthetic cases above prove
// the arithmetic; this one proves that the walk lands on the same voxels the shader's descent lands
// on over a real building, with real materials, real openings and real coordinates -- which is the
// thing a hand-written test world cannot be trusted to cover, because it was written by whoever
// also wrote the walk.
// ==================================================================================================

TEST_CASE("R8b over the whole facility" * doctest::skip()) {
    const char* candidates[] = {"clips/facility.clip", "../clips/facility.clip",
                                "../../clips/facility.clip", "../../../clips/facility.clip"};
    std::string clip_path;
    for (const char* candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            clip_path = candidate;
            break;
        }
    }
    REQUIRE_MESSAGE(!clip_path.empty(), "clips/facility.clip is not beside the test binary");

    // Four voxels a metre, and `WS_FACILITY_PER_METRE` to raise it. The walk below is every voxel
    // of the building four times over, so the resolution sets the runtime cubed: metre 4 is a
    // couple of million voxels and a minute, metre 8 is twenty million and long enough that nobody
    // runs it twice. The question this asks -- does the walk land on the same voxels the shader's
    // descent lands on, over a real building with real materials and real openings -- is the same
    // question at either.
    i32 per_metre = 4;
    if (const char* asked = std::getenv("WS_FACILITY_PER_METRE")) per_metre = std::atoi(asked);

    JobSystem jobs;
    TagRegistry tags;
    VoxelTypeTable types;
    forge::Script script = forge::load_clip_script(clip_path, types, tags);
    script.settings.voxels_per_metre = per_metre;

    const u64 sample_began = now_ns();
    const forge::SampleResult built =
        forge::sample(script.field, script.solid, script.paint, script.settings, &jobs);
    const f64 sample_ms = ns_to_ms(now_ns() - sample_began);
    REQUIRE_FALSE(built.clip.empty());

    World world;
    MatterLedger ledger;
    paste_clip(world, ledger, built.clip, built.origin_voxel[0], built.origin_voxel[1],
               built.origin_voxel[2], PasteMode::Replace, MatterReason::Generation, 1, &jobs,
               types.type_count());

    NodePoolBudget budget;
    budget.proximity_voxels = 0;
    budget.hashed_variation = true;
    NodePool pool;
    pool.create(budget, types);

    // Ask for the whole thing at brick resolution, and keep serving until it stops building. The
    // pool is depth-bounded, so a test that checks voxels has to ask for voxels.
    const i64 lo[3] = {built.origin_voxel[0], built.origin_voxel[1], built.origin_voxel[2]};
    const i64 hi[3] = {lo[0] + static_cast<i64>(built.clip.size[0]) - 1,
                       lo[1] + static_cast<i64>(built.clip.size[1]) - 1,
                       lo[2] + static_cast<i64>(built.clip.size[2]) - 1};
    for (i64 z = lo[2]; z <= hi[2]; z += 8) {
        for (i64 y = lo[1]; y <= hi[1]; y += 8) {
            for (i64 x = lo[0]; x <= hi[0]; x += 8) {
                pool.request(node_key_of(x, y, z, kLeafLevel));
            }
        }
    }
    const f64 camera[3] = {0.0, 0.0, 0.0};
    for (u64 frame = 1; frame <= 64; ++frame) pool.update(world, camera, frame);

    REQUIRE(pool.stale_masks(world) == 0);
    REQUIRE(pool.stale_leaves(world) == 0);

    // Every voxel of the building, through the same walk the shader performs, at the voxel and at
    // three levels below it. `mirror_voxel` against the world is the standard; `mirror_variation`
    // against `mirror_voxel` is the claim that the child source did not move anything.
    u64 voxels = 0, solid = 0, disagreed = 0, sub_disagreed = 0;
    for (i64 z = lo[2]; z <= hi[2]; ++z) {
        for (i64 y = lo[1]; y <= hi[1]; ++y) {
            for (i64 x = lo[0]; x <= hi[0]; ++x) {
                ++voxels;
                const VoxelTypeId truth = world.get(x, y, z);
                const VoxelTypeId walked = pool.mirror_voxel(x, y, z);
                if (walked != truth) ++disagreed;
                if (truth != kAir) ++solid;

                const VariationSample at = pool.mirror_variation(x, y, z, 0);
                if (at.type != walked || at.matter != (walked != kAir)) ++sub_disagreed;
                for (u32 depth = 1; depth <= 3; ++depth) {
                    const i64 base = static_cast<i64>(1) << depth;
                    const VariationSample cell = pool.mirror_variation(
                        x * base + base - 1, y * base + base - 1, z * base + base - 1, depth);
                    if (cell.matter != (walked != kAir) || cell.type != walked) ++sub_disagreed;
                }
            }
        }
    }

    u64 cells = 0;
    const u64 print = pool.variation_fingerprint(world, lo, hi, 4, 3, &cells);
    std::printf(
        "\nR8b facility  %s at %d voxels a metre, sampled in %.0f ms\n"
        "world         %llu voxels in the clip's box, %llu solid, %llu nodes built\n"
        "mirror        %llu voxels disagree with the world, %llu sub-voxel cells disagree with the "
        "mirror\n"
        "fingerprint   %016llx over %llu solid voxels, stride 4, three levels down\n",
        clip_path.c_str(), per_metre, sample_ms, static_cast<unsigned long long>(voxels),
        static_cast<unsigned long long>(solid),
        static_cast<unsigned long long>(pool.node_watermark()),
        static_cast<unsigned long long>(disagreed),
        static_cast<unsigned long long>(sub_disagreed),
        static_cast<unsigned long long>(print), static_cast<unsigned long long>(cells));

    CHECK(disagreed == 0);
    CHECK(sub_disagreed == 0);
    CHECK(solid > 0);

    // Two pools, one world: the same fingerprint. The two-RUN half of this gate is the exe's own
    // `R8b fingerprint` line under `--hashed-variation --settle`; this is the half a suite can
    // assert on every build.
    NodePool second;
    second.create(budget, types);
    CHECK(second.variation_fingerprint(world, lo, hi, 4, 3, nullptr) == print);
}

// ---- R12c's other half: the clip's extent, before anything has been sampled --------------------
//
// The stage's own report (D699) says exactly what was missing and why it is here rather than in the
// marcher: at frame 1 the world holds 0 chunks, so `index_world` seeds no roots, so the SHADER's
// `node_locate` finds nothing in the entry table and answers kFoundEmpty -- and a descent that
// answers empty has nothing to derive. `seed_from_clip` puts the clip's own answer in the tree as
// addressing so the same descent answers WANTED.
//
// # The discriminator these tests use, and why it is not `NodeFind::wanted`
//
// `NodePool::locate` answers `wanted = true` when there is no root at the entry level, and the
// shader answers kFoundEmpty for the same tree. That is not a disagreement to fix here -- it is
// deliberate on both sides and both comments say so -- but it does mean `wanted` alone cannot tell
// the seeded arm from the control arm. What CAN is `slot`: a descent that found a root has one, a
// descent that found no root has kNoNode. So every case below asserts on the pair.

namespace {

// A stand-in for `forge::box_may_hold_matter`: conservative in the one direction that matters, and
// with no field behind it, so the tests pin the pool's own arithmetic rather than the sampler's.
// True for any box that meets the chosen solid, plus a voxel of slack in every direction so that
// "may" really is may.
struct SeedOracle {
    i64 lo[3]{0, 0, 0};
    i64 hi[3]{0, 0, 0};
    mutable u64 asked = 0;

    bool operator()(const i64 box_lo[3], const i64 box_hi[3]) const {
        ++asked;
        for (u32 axis = 0; axis < 3; ++axis) {
            if (box_lo[axis] > hi[axis] + 1) return false;
            if (box_hi[axis] < lo[axis] - 1) return false;
        }
        return true;
    }
};

// The extent a clip would hand over: 2,048 voxels a side from the origin, which is 64 m and sits
// inside one entry-level block, so the whole seed is one root and the arithmetic is checkable by
// hand.
constexpr i64 kSeedLo[3] = {0, 0, 0};
constexpr i64 kSeedHi[3] = {2047, 2047, 2047};

}  // namespace

TEST_CASE("the clip's extent gives a cold pool the roots the first frame needs") {
    Fixture fixture;
    SeedOracle oracle;
    // The "building": one 512-voxel cell in the middle of the extent.
    oracle.lo[0] = oracle.lo[1] = oracle.lo[2] = 1024;
    oracle.hi[0] = oracle.hi[1] = oracle.hi[2] = 1535;

    // The control arm first, and it is the whole of D699's missing half in two lines: an empty
    // world has no root, so the descent the shader performs finds nothing at the entry level.
    const NodeFind cold = fixture.pool.locate(node_key_of(1200, 1200, 1200, kLeafLevel));
    CHECK(cold.slot == kNoNode);

    const NodeSeedReport seeded =
        fixture.pool.seed_from_clip(fixture.world, kSeedLo, kSeedHi, std::cref(oracle));
    CHECK(seeded.live);
    CHECK(seeded.roots == 1);
    CHECK(seeded.shells > 1);
    CHECK(seeded.cells > 0);
    // More questions than the seed itself counted, because `seed_from_clip` runs `mirror_seed` at
    // the end and the mirror asks the field AGAIN about every bit it finds. That is the point of it
    // -- an audit that reuses the seed's own answers is trap 26 -- so this is `>=` rather than `==`.
    CHECK(oracle.asked > seeded.asked);
    CHECK(fixture.pool.clip_seed_live());

    // Inside the clip's matter: a root, and WANTED. This is the pair the marcher reads as
    // "evaluate the field here" rather than "fly through".
    const NodeFind inside = fixture.pool.locate(node_key_of(1200, 1200, 1200, kLeafLevel));
    CHECK(inside.slot != kNoNode);
    CHECK(inside.wanted);
    CHECK(!inside.empty_below);

    // Inside the extent and away from the matter: a root, and EMPTY at a known size, which is what
    // lets a ray jump the cell instead of paying a field evaluation for it. This is the whole
    // reason the seed goes below the root at all.
    const NodeFind air = fixture.pool.locate(node_key_of(64, 64, 64, kLeafLevel));
    CHECK(air.slot != kNoNode);
    CHECK(!air.wanted);
    CHECK(air.empty_below);

    // Outside the extent altogether: no root, exactly as before the seed.
    const NodeFind elsewhere = fixture.pool.locate(node_key_of(1 << 20, 0, 0, kLeafLevel));
    CHECK(elsewhere.slot == kNoNode);

    CHECK(fixture.pool.validate());
}

TEST_CASE("what the clip seeds is addressing and never storage") {
    Fixture fixture;
    SeedOracle oracle;
    oracle.lo[0] = oracle.lo[1] = oracle.lo[2] = 0;
    oracle.hi[0] = oracle.hi[1] = oracle.hi[2] = 2047;   // the whole extent may hold matter

    const NodeSeedReport seeded =
        fixture.pool.seed_from_clip(fixture.world, kSeedLo, kSeedHi, std::cref(oracle));
    REQUIRE(seeded.live);

    // R2's residency argument is that what the pool holds follows the pixels. A seed that allocated
    // a leaf, an occupancy word or a payload byte would have undone it before a frame was drawn.
    CHECK(fixture.pool.leaf_watermark() == 0);
    CHECK(fixture.pool.payload_watermark() == 0);
    const NodePoolStats stats = fixture.pool.stats();
    CHECK(stats.leaves == 0);
    CHECK(stats.payload_in_use == 0);
    CHECK(stats.builds == 0);   // `builds_` counts what the WORLD built; the clip built nothing
    CHECK(stats.evictions == 0);

    // ...and the addressing itself is small. 2,048 voxels a side at an 8 m floor is 512 cells, so
    // the shells above them are a few hundred records of thirty-two bytes.
    CHECK(seeded.cells == 512);
    CHECK(fixture.pool.node_watermark() < 1024);

    // A frame served against the still-empty world builds nothing and evicts nothing: the seed is
    // not work the pool now has to do.
    const NodeUploadBatch& batch = fixture.serve(1);
    CHECK(batch.built == 0);
    CHECK(batch.evicted == 0);
    CHECK(batch.evicted_nodes == 0);
    CHECK(batch.no_room == 0);
    CHECK(!batch.out_of_memory);
    CHECK(fixture.pool.clip_seed_live());
}

TEST_CASE("the seed's mirror says what it agrees with, and the three answers are kept apart") {
    Fixture fixture;
    SeedOracle oracle;
    oracle.lo[0] = oracle.lo[1] = oracle.lo[2] = 1024;
    oracle.hi[0] = oracle.hi[1] = oracle.hi[2] = 1535;

    const NodeSeedReport seeded =
        fixture.pool.seed_from_clip(fixture.world, kSeedLo, kSeedHi, std::cref(oracle));
    REQUIRE(seeded.live);

    const NodeSeedMirror mirror = fixture.pool.mirror_seed(fixture.world, std::cref(oracle));
    CHECK(mirror.shells == seeded.shells);
    CHECK(mirror.bits > 0);

    // The two that must be nought. `differs_field` nought is "the seed never invented a bit the
    // field does not allow"; `misses_world` nought is "the seed never lost one the world holds".
    // D621 is what the second one looks like when nobody measures it.
    CHECK(mirror.differs_field == 0);
    CHECK(mirror.misses_world == 0);
    // ...and the two that say the seed did what it says on the tin.
    CHECK(mirror.leaves == 0);
    CHECK(mirror.children == 0);
    CHECK(mirror.agrees_field == mirror.bits);
    // Every bit exceeds the world, because the world is empty. That is the disagreement this whole
    // change is, stated by an instrument rather than left to be discovered from a screenshot.
    CHECK(mirror.exceeds_world == mirror.bits);

    // And the world-facing audit is not confused by it: a seeded mask is not a stale mask, because
    // it never claimed to be the world's.
    CHECK(fixture.pool.stale_masks(fixture.world) == 0);
    CHECK(fixture.pool.stale_leaves(fixture.world) == 0);
}

TEST_CASE("the clip's seed retires the moment the world holds anything") {
    Fixture fixture;
    SeedOracle oracle;
    oracle.lo[0] = oracle.lo[1] = oracle.lo[2] = 1024;
    oracle.hi[0] = oracle.hi[1] = oracle.hi[2] = 1535;

    REQUIRE(fixture.pool.seed_from_clip(fixture.world, kSeedLo, kSeedHi, std::cref(oracle)).live);
    const u32 seeded_nodes = fixture.pool.node_watermark();

    // The ladder pastes. Only a corner of what the clip claimed, which is the case that matters:
    // the seed must not survive it anywhere, because a mask nothing can build under is a phantom
    // request every frame for ever.
    fixture.fill_box(1024, 1024, 1024, 1039, 1039, 1039);
    fixture.serve(2);

    CHECK(!fixture.pool.clip_seed_live());
    const NodeSeedMirror after = fixture.pool.mirror_seed(fixture.world, std::cref(oracle));
    CHECK(after.shells == 0);   // nothing carries the flag any more
    // What is left is the pool the control arm would have had: one root, its mask the world's own.
    CHECK(fixture.pool.stale_masks(fixture.world) == 0);
    CHECK(fixture.pool.validate());
    CHECK(fixture.pool.node_watermark() <= seeded_nodes);

    // The world's own geometry still streams through the root the seed left behind.
    //
    // The descent is asked AFTER this rather than before it, and the difference is worth stating:
    // straight after retirement the root is a bare shell, so every point under a set mask bit
    // answers WANTED at the root's own level -- which is the ordinary state of an unbuilt tree and
    // is what the control arm answers too. What retiring changes is what happens once the chain
    // exists, and that is the line below.
    fixture.want_box(1024, 1024, 1024, 1039, 1039, 1039);
    fixture.serve(3);
    CHECK(fixture.pool.mirror_voxel(1030, 1030, 1030) == fixture.stone);
    CHECK(fixture.pool.mirror_voxel(1500, 1500, 1500) == kAir);
    CHECK(fixture.pool.stale_masks(fixture.world) == 0);
    CHECK(fixture.pool.stale_leaves(fixture.world) == 0);

    // A cell the clip claimed and the world does not have reads as EMPTY rather than WANTED, which
    // is the whole of what retiring is for: with the seed still standing this cell would be one the
    // marcher derives from the field every frame, for ever, over a world that will never hold it.
    const NodeFind gone = fixture.pool.locate(node_key_of(1500, 1500, 1500, kLeafLevel));
    CHECK(gone.slot != kNoNode);
    CHECK(!gone.wanted);
    CHECK(gone.empty_below);
}

TEST_CASE("a world that already has chunks is not seeded from its clip") {
    Fixture fixture;
    SeedOracle oracle;
    oracle.lo[0] = oracle.lo[1] = oracle.lo[2] = 1024;
    oracle.hi[0] = oracle.hi[1] = oracle.hi[2] = 1535;

    // A warm load: the world came off disk before the pool was made. It has real roots one
    // `index_world` away, and a clip mask over them would be the one disagreement `mirror_seed`
    // forbids -- so this is refused rather than merged.
    fixture.fill_box(1024, 1024, 1024, 1039, 1039, 1039);
    const NodeSeedReport refused =
        fixture.pool.seed_from_clip(fixture.world, kSeedLo, kSeedHi, std::cref(oracle));
    CHECK(!refused.live);
    CHECK(refused.roots == 0);
    CHECK(refused.shells == 0);
    CHECK(oracle.asked == 0);
    CHECK(!fixture.pool.clip_seed_live());
    CHECK(fixture.pool.node_watermark() == 0);
}

TEST_CASE("a pool that was seeded settles on the same tree as one that never was") {
    // The gate that matters more than the first frame: a first frame that changes the world it
    // settles to is a different world, not a faster one. Two pools, one world, the same requests.
    SeedOracle oracle;
    oracle.lo[0] = oracle.lo[1] = oracle.lo[2] = 1024;
    oracle.hi[0] = oracle.hi[1] = oracle.hi[2] = 1535;

    Fixture seeded;
    REQUIRE(seeded.pool.seed_from_clip(seeded.world, kSeedLo, kSeedHi, std::cref(oracle)).live);
    seeded.serve(1);   // a frame with the seed standing and the world still empty

    Fixture control;

    for (Fixture* arm : {&seeded, &control}) {
        arm->fill_box(1024, 1024, 1024, 1055, 1055, 1055);
        arm->serve(2);
        arm->want_box(1024, 1024, 1024, 1055, 1055, 1055);
        arm->serve(3);
    }

    u64 differ = 0;
    u64 solid = 0;
    for (i64 z = 1020; z < 1060; ++z) {
        for (i64 y = 1020; y < 1060; ++y) {
            for (i64 x = 1020; x < 1060; ++x) {
                const VoxelTypeId a = seeded.pool.mirror_voxel(x, y, z);
                const VoxelTypeId b = control.pool.mirror_voxel(x, y, z);
                if (a != b) ++differ;
                if (a != kAir) ++solid;
            }
        }
    }
    CHECK(differ == 0);
    CHECK(solid == 32 * 32 * 32);
    CHECK(seeded.pool.stale_masks(seeded.world) == 0);
    CHECK(seeded.pool.stale_leaves(seeded.world) == 0);
    CHECK(seeded.pool.validate());
}
