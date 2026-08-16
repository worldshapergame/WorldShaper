#include <doctest/doctest.h>

#include <vector>

#include "core/jobs.hpp"
#include "forge/measure.hpp"
#include "forge/world_stipple.hpp"
#include "game/clip.hpp"
#include "world/world.hpp"

using namespace ws;
using namespace ws::forge;

namespace {

// A slab of stone across several chunks with things scattered on top of it, which is the shape the
// world clean is asked about: a wide flat surface whose specks are almost all on one plane, cut by
// chunk boundaries the geometry knows nothing about.
//
// Five chunks across in x and four in z, three voxels deep, so the shipped walk sees twenty boxes
// and the shared one sees eighty. Small deliberately: what the tests below assert is exact
// agreement between two ways of counting, and exactness does not need size.
World slab_world(VoxelTypeId stone, VoxelTypeId gilt, VoxelTypeId moss) {
    World world;
    for (i64 z = -300; z <= 300; ++z) {
        for (i64 x = -300; x <= 600; ++x) {
            for (i64 y = 0; y < 3; ++y) world.set(x, y, z, stone);
        }
    }
    // Accidents: lone voxels of somebody else's material, deliberately placed so that some of them
    // sit exactly on a chunk face (x = -256, -1, 0, 255, 256) and some are nowhere near one.
    //
    // NO TWO OF THEM TOUCH, and that is a property the fixture has to have rather than a detail of
    // where they went. Three of these in a row is not three specks -- each has a neighbour of its
    // own material, so none of them is alone and the pass correctly leaves all three. The first
    // version of this world put 255, 256 and 257 side by side to straddle a chunk face and then
    // asserted ten repaints; six is the right answer to that world and the test found it.
    const i64 gilt_at[][2] = {{-256, 0},  {-1, 0},   {1, 0},     {17, 40}, {128, 96},
                              {255, 0},   {256, 60}, {257, 120}, {0, 200}, {400, -200}};
    for (const auto& at : gilt_at) world.set(at[0], 3, at[1], gilt);
    // And a dither: moss on every other voxel of the top face over one stretch, which is what the
    // verdict exists to protect and what must come out of both walks as the same population.
    for (i64 z = -100; z <= 100; z += 2) {
        for (i64 x = -100; x <= 100; x += 2) world.set(x, 3, z, moss);
    }
    return world;
}

}  // namespace

// The claim every other test here rests on, and it is D629's method under a different cut.
//
// The shipped pass captures a whole chunk with a one-voxel skirt; the shared walk cuts the chunk
// into slabs and captures each with its own. Both are only sound because of one property -- the
// INTERIORS tile the world exactly once, so every voxel is judged once by its real neighbours -- and
// if that property holds, the two counts are equal material for material. Not close. The verdict is
// a ratio with a floor on its numerator (D625), so a handful of voxels either way moves which
// materials are spared, and "nearly the same counts" is not an answer to anything.
TEST_CASE("one walk in slabs counts what the shipped walk counts, chunk for chunk") {
    const VoxelTypeId stone = 21;
    const VoxelTypeId gilt = 22;
    const VoxelTypeId moss = 23;
    const World world = slab_world(stone, gilt, moss);

    const StippleCounts shipped = stipple_counts_from_world_serial(world, nullptr);
    const WorldStippleScan walked = scan_world_specks(world, nullptr);
    REQUIRE(shipped.any());
    CHECK(walked.counts.by_type.size() == shipped.by_type.size());
    for (const auto& [type, want] : shipped.by_type) {
        const auto found = walked.counts.by_type.find(type);
        REQUIRE(found != walked.counts.by_type.end());
        CHECK(found->second.surface == want.surface);
        CHECK(found->second.specks == want.specks);
    }

    // And therefore the same verdict, which is the only part of the counts that reaches the world.
    const StippleVerdict from_shipped = stipple_verdict(shipped, 0.05);
    const StippleVerdict from_walk = stipple_verdict(walked.counts, 0.05);
    CHECK(from_shipped.allowed.size() == from_walk.allowed.size());
    for (const auto& [type, may] : from_shipped.allowed) {
        const auto found = from_walk.allowed.find(type);
        REQUIRE(found != from_walk.allowed.end());
        CHECK(found->second == may);
    }
    // The dither is spared and the accidents are not, or this world is not testing what it says.
    CHECK_FALSE(from_walk.allowed.at(moss));
    CHECK(from_walk.allowed.at(gilt));

    // Every speck the walk found is one the counts counted. The two come out of one pass, so this
    // is not a coincidence to be checked -- it is the thing that makes the pass a single walk, and
    // it fails loudly if the speck test and the counting test are ever allowed to drift apart.
    u64 counted = 0;
    for (const auto& [type, pair] : walked.counts.by_type) {
        (void)type;
        counted += pair.specks;
    }
    CHECK(walked.specks.size() == counted);
}

// The whole point of the change: it may be threaded, and threading it may not move a voxel.
//
// The pool is handed a range and hands pieces of it out on demand, so which worker takes which box
// is decided by timing and is different on every run. If anything in the accumulation were order
// dependent this is where it would show, and it would show as a flake rather than a failure -- so
// the check is over several runs and it is over the SPECK LIST as well as the totals, because the
// list is what the repaint is driven from and a sum can agree while an order does not.
TEST_CASE("the world walk gives the same answer threaded as it does on one thread") {
    const VoxelTypeId stone = 21;
    const VoxelTypeId gilt = 22;
    const VoxelTypeId moss = 23;
    const World world = slab_world(stone, gilt, moss);

    const WorldStippleScan alone = scan_world_specks(world, nullptr);
    JobSystem jobs(4);
    for (int round = 0; round < 4; ++round) {
        const WorldStippleScan threaded = scan_world_specks(world, &jobs);
        REQUIRE(threaded.counts.by_type.size() == alone.counts.by_type.size());
        for (const auto& [type, want] : alone.counts.by_type) {
            const auto found = threaded.counts.by_type.find(type);
            REQUIRE(found != threaded.counts.by_type.end());
            CHECK(found->second.surface == want.surface);
            CHECK(found->second.specks == want.specks);
        }
        REQUIRE(threaded.specks.size() == alone.specks.size());
        bool same = true;
        for (usize i = 0; i < alone.specks.size(); ++i) {
            same = same && threaded.specks[i].at[0] == alone.specks[i].at[0] &&
                   threaded.specks[i].at[1] == alone.specks[i].at[1] &&
                   threaded.specks[i].at[2] == alone.specks[i].at[2] &&
                   threaded.specks[i].was == alone.specks[i].was &&
                   threaded.specks[i].becomes == alone.specks[i].becomes;
        }
        CHECK(same);
    }
}

// And the world it leaves: the same voxels, the same hash, however the boxes were dealt out.
TEST_CASE("cleaning the world threaded leaves the world one thread leaves") {
    const VoxelTypeId stone = 21;
    const VoxelTypeId gilt = 22;
    const VoxelTypeId moss = 23;

    const auto cleaned_hash = [&](JobSystem* jobs, u64& repainted, u64& left) {
        World world = slab_world(stone, gilt, moss);
        const WorldStippleScan scan = scan_world_specks(world, jobs);
        const StippleVerdict verdict = stipple_verdict(scan.counts, 0.05);
        const WorldCleanReport out = repaint_world_specks(world, scan, verdict);
        repainted = out.repainted;
        left = out.left;
        return world.content_hash();
    };

    u64 repainted_alone = 0;
    u64 left_alone = 0;
    const u64 alone = cleaned_hash(nullptr, repainted_alone, left_alone);
    CHECK(repainted_alone == 10);   // the ten accidents, and nothing else
    CHECK(left_alone > 0);          // the dither, judged and let alone

    JobSystem jobs(4);
    for (int round = 0; round < 4; ++round) {
        u64 repainted = 0;
        u64 left = 0;
        CHECK(cleaned_hash(&jobs, repainted, left) == alone);
        CHECK(repainted == repainted_alone);
        CHECK(left == left_alone);
    }

    // A paint pass, so the matter cannot move. If this ever fails the pass has become something
    // else and every volume, component and walkability figure taken through it is wrong.
    World before = slab_world(stone, gilt, moss);
    const u64 solid_before = before.stats().solid_voxels;
    const WorldStippleScan scan = scan_world_specks(before, &jobs);
    repaint_world_specks(before, scan, stipple_verdict(scan.counts, 0.05));
    CHECK(before.stats().solid_voxels == solid_before);

    // And a fixed point: nothing is left for a second pass to find.
    const WorldStippleScan again = scan_world_specks(before, &jobs);
    const WorldCleanReport twice =
        repaint_world_specks(before, again, stipple_verdict(again.counts, 0.05));
    CHECK(twice.repainted == 0);
}

// WHY THE ONE WALK DOES NOT REPRODUCE THE SHIPPED CLEAN VOXEL FOR VOXEL, and it is not the chunk
// seam it looks like.
//
// `forge::despeckle` says in its own comment that it reads from a copy and writes to the clip "so
// that a repainted voxel cannot become one of the neighbours the next voxel is judged against...
// It has to be one simultaneous step or it is not a rule." Only HALF of that is what the code does:
// `mine` is read out of the copy and the six neighbours are read out of `clip`, which is the array
// being written. So a repaint made at (x) IS one of the neighbours (x+1) is judged against, and the
// result depends on the scan order exactly as the comment says it must not.
//
// This test asserts the behaviour as it stands rather than as the comment describes it, because the
// per-node despeckle on the ladder's default path is built on it and changing it would move the
// world every load produces. What it is here for is that the next person to read that comment finds
// out in one run which of the two the code does -- and that anybody who fixes it is told, by a
// failing test, exactly what they have changed.
//
// The case is the comment's own: a lone voxel repainted into the material of its neighbour, whose
// neighbour is then no longer alone.
TEST_CASE("despeckle's neighbours are read live, not from the copy its comment describes") {
    const VoxelTypeId stone = 21;
    const VoxelTypeId first = 22;
    const VoxelTypeId second = 23;

    Clip clip;
    clip.size[0] = clip.size[1] = clip.size[2] = 11;
    clip.voxels.assign(11 * 11 * 11, kAir);
    clip.inside.assign(clip.voxels.size(), 1);
    for (i32 z = 1; z <= 9; ++z) {
        for (i32 x = 1; x <= 9; ++x) {
            for (i32 y = 1; y <= 3; ++y) clip.voxels[clip.index(x, y, z)] = stone;
        }
    }
    // Two lone voxels side by side on the top face, of two different materials. Neither has a
    // neighbour of its own material, so both are specks; the scan reaches `first` before `second`.
    clip.voxels[clip.index(5, 4, 5)] = first;
    clip.voxels[clip.index(6, 4, 5)] = second;
    clip.build_coarse();

    // `first` sees one `second` and one stone below it. The tie goes to whichever comes first in
    // the fixed neighbour order x-, x+, y-, y+, z-, z+, which is the `second` on its +x side -- so
    // it is repainted INTO the material of the voxel that has not been judged yet.
    const DespeckleReport out = despeckle(clip);
    CHECK(clip.at(5, 4, 5) == second);

    // And now the live read decides the rest. Reading neighbours from the copy, `second` would
    // still be alone and would be repainted in the same step; reading them from the clip, its
    // former neighbour is already wearing its material, so it has kin and is left where it is.
    CHECK(clip.at(6, 4, 5) == second);
    CHECK(out.repainted == 1);

    // The consequence, stated as a number so it cannot be argued with: run the pass again and it
    // finds the pair it has just created and leaves it alone for ever. One repaint, not two.
    const DespeckleReport again = despeckle(clip);
    CHECK(again.repainted == 0);
}
