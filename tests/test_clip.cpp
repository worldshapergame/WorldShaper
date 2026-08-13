#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "core/hash.hpp"
#include "game/clip.hpp"
#include "world/ledger.hpp"
#include "world/world.hpp"

using namespace ws;

namespace {

constexpr VoxelTypeId kRock = 3;
constexpr VoxelTypeId kWood = 4;
constexpr VoxelTypeId kGlass = 5;
constexpr f64 kPi = 3.14159265358979323846;
constexpr f64 kDegrees = kPi / 180.0;

World scattered_world(i64 x0, i64 y0, i64 z0, i64 x1, i64 y1, i64 z1) {
    World world;
    for (i64 z = z0; z <= z1; ++z) {
        for (i64 y = y0; y <= y1; ++y) {
            for (i64 x = x0; x <= x1; ++x) {
                const u64 h = hash_cell(x, y, z, 0, 0x5EED1234ull);
                const VoxelTypeId choice[4] = {kAir, kRock, kWood, kGlass};
                world.set(x, y, z, choice[h & 3]);
            }
        }
    }
    return world;
}

}  // namespace

TEST_CASE("capture_clip lifts exactly what is in the box") {
    World world = scattered_world(-9, -9, -9, 9, 9, 9);
    world.set(100, 0, 0, kRock);   // outside: must not appear

    const Clip clip = capture_clip(world, -5, -4, -3, 5, 4, 3);
    REQUIRE(clip.size[0] == 11);
    REQUIRE(clip.size[1] == 9);
    REQUIRE(clip.size[2] == 7);
    CHECK(clip.covered_count() == clip.cell_count());

    for (i32 z = 0; z < clip.size[2]; ++z) {
        for (i32 y = 0; y < clip.size[1]; ++y) {
            for (i32 x = 0; x < clip.size[0]; ++x) {
                REQUIRE(clip.at(x, y, z) == world.get(-5 + x, -4 + y, -3 + z));
            }
        }
    }
}

TEST_CASE("capture_clip reads air where the world has nothing") {
    World world;
    const Clip clip = capture_clip(world, 0, 0, 0, 7, 7, 7);
    CHECK(clip.solid_count() == 0);
    CHECK(clip.covered_count() == 512);
}

TEST_CASE("rotation never loses or invents a voxel") {
    // The whole reason rotation is three shears instead of a resample. A bijection cannot
    // change the count, so this is an exact equality at every angle, not a tolerance.
    World world = scattered_world(-12, -12, -12, 12, 12, 12);
    const Clip clip = capture_clip(world, -10, -8, -6, 10, 8, 6);
    const u64 solid = clip.solid_count();
    const u64 covered = clip.covered_count();
    REQUIRE(solid > 0);

    for (u32 axis = 0; axis < 3; ++axis) {
        for (i32 step = 1; step <= 48; ++step) {
            const Clip turned = rotate_clip(clip, axis, static_cast<f64>(step) * 7.5 * kDegrees);
            REQUIRE(turned.solid_count() == solid);
            REQUIRE(turned.covered_count() == covered);
        }
        for (i32 step = 1; step <= 32; ++step) {
            const Clip turned =
                rotate_clip(clip, axis, static_cast<f64>(step) * 11.25 * kDegrees);
            REQUIRE(turned.solid_count() == solid);
            REQUIRE(turned.covered_count() == covered);
        }
    }
}

TEST_CASE("four quarter turns are the identity") {
    World world = scattered_world(-6, -6, -6, 6, 6, 6);
    const Clip clip = capture_clip(world, -5, -4, -3, 5, 4, 3);

    for (u32 axis = 0; axis < 3; ++axis) {
        Clip turned = clip;
        for (i32 i = 0; i < 4; ++i) turned = rotate_clip(turned, axis, kPi * 0.5);
        CHECK(turned.size[0] == clip.size[0]);
        CHECK(turned.size[1] == clip.size[1]);
        CHECK(turned.size[2] == clip.size[2]);
        CHECK(turned.content_hash() == clip.content_hash());
    }
}

TEST_CASE("a quarter turn swaps the two axes it turns in") {
    World world;
    world.set(0, 0, 0, kRock);
    const Clip clip = capture_clip(world, 0, 0, 0, 3, 1, 0);   // 4 x 2 x 1
    REQUIRE(clip.size[0] == 4);
    REQUIRE(clip.size[1] == 2);

    // About z, the x and y extents trade places.
    const Clip turned = rotate_clip(clip, 2, kPi * 0.5);
    CHECK(turned.size[0] == 2);
    CHECK(turned.size[1] == 4);
    CHECK(turned.size[2] == 1);
}

TEST_CASE("rotating by zero changes nothing") {
    World world = scattered_world(0, 0, 0, 8, 8, 8);
    const Clip clip = capture_clip(world, 0, 0, 0, 8, 8, 8);
    for (u32 axis = 0; axis < 3; ++axis) {
        const Clip turned = rotate_clip(clip, axis, 0.0);
        CHECK(turned.content_hash() == clip.content_hash());
    }
}

TEST_CASE("a rotated clip marks the corners that were never part of it") {
    World world;
    for (i64 z = 0; z < 8; ++z) {
        for (i64 y = 0; y < 8; ++y) {
            for (i64 x = 0; x < 8; ++x) world.set(x, y, z, kRock);
        }
    }
    const Clip clip = capture_clip(world, 0, 0, 0, 7, 7, 7);
    const Clip turned = rotate_clip(clip, 2, 30.0 * kDegrees);

    // The array has grown to hold a tilted cube, so some cells are outside it. They are not
    // air — they are nothing, and a stamp must leave them alone.
    CHECK(turned.cell_count() > turned.covered_count());
    CHECK(turned.covered_count() == clip.covered_count());
}

TEST_CASE("mirroring twice is the identity") {
    World world = scattered_world(0, 0, 0, 10, 10, 10);
    const Clip clip = capture_clip(world, 0, 0, 0, 9, 8, 7);
    for (u32 axis = 0; axis < 3; ++axis) {
        CHECK(mirror_clip(mirror_clip(clip, axis), axis).content_hash() == clip.content_hash());
    }
}

TEST_CASE("stamping a clip reproduces it exactly") {
    World world = scattered_world(0, 0, 0, 20, 20, 20);
    const Clip clip = capture_clip(world, 2, 3, 4, 14, 12, 11);

    std::vector<Op> ops;
    clip_to_ops(clip, 200, 300, 400, PasteMode::Replace, 1, 1, ops);
    CHECK(!ops.empty());

    World target;
    MatterLedger ledger;
    apply_ops(target, ops, ledger);
    for (i32 z = 0; z < clip.size[2]; ++z) {
        for (i32 y = 0; y < clip.size[1]; ++y) {
            for (i32 x = 0; x < clip.size[0]; ++x) {
                REQUIRE(target.get(200 + x, 300 + y, 400 + z) == clip.at(x, y, z));
            }
        }
    }
}

TEST_CASE("paste modes touch only what they say they will") {
    World world;
    // A clip that is half matter, half air.
    for (i64 x = 0; x < 4; ++x) world.set(x, 0, 0, kRock);
    const Clip clip = capture_clip(world, 0, 0, 0, 7, 0, 0);   // 4 rock then 4 air
    REQUIRE(clip.solid_count() == 4);

    SUBCASE("replace clears where the clip is empty") {
        World target;
        MatterLedger ledger;
        for (i64 x = 0; x < 8; ++x) target.set(100 + x, 0, 0, kGlass);
        std::vector<Op> ops;
        clip_to_ops(clip, 100, 0, 0, PasteMode::Replace, 1, 1, ops);
        apply_ops(target, ops, ledger);
        for (i64 x = 0; x < 4; ++x) CHECK(target.get(100 + x, 0, 0) == kRock);
        for (i64 x = 4; x < 8; ++x) CHECK(target.get(100 + x, 0, 0) == kAir);
    }

    SUBCASE("matter only leaves the rest alone") {
        World target;
        MatterLedger ledger;
        for (i64 x = 0; x < 8; ++x) target.set(100 + x, 0, 0, kGlass);
        std::vector<Op> ops;
        clip_to_ops(clip, 100, 0, 0, PasteMode::SolidOnly, 1, 1, ops);
        apply_ops(target, ops, ledger);
        for (i64 x = 0; x < 4; ++x) CHECK(target.get(100 + x, 0, 0) == kRock);
        for (i64 x = 4; x < 8; ++x) CHECK(target.get(100 + x, 0, 0) == kGlass);
    }

    SUBCASE("into empty space disturbs nothing that exists") {
        World target;
        MatterLedger ledger;
        target.set(100, 0, 0, kGlass);   // occupied; the clip wants rock here
        target.set(102, 0, 0, kGlass);
        std::vector<Op> ops;
        clip_to_ops(clip, 100, 0, 0, PasteMode::IntoAir, 1, 1, ops);
        apply_ops(target, ops, ledger);
        CHECK(target.get(100, 0, 0) == kGlass);   // survived
        CHECK(target.get(101, 0, 0) == kRock);    // was empty, filled
        CHECK(target.get(102, 0, 0) == kGlass);   // survived
        CHECK(target.get(103, 0, 0) == kRock);
    }
}

// D620. A Replace paste ERASES where the clip is empty, and `paste_clip` writes whole bricks
// through `brick_for_write().assign()` — which the chunk never sees voxel by voxel, so nothing
// above it unlinks a brick the write emptied. Left allocated, that brick is `NodePool::world_has`
// telling the render tree the world holds matter here: the descent calls the cell occupied,
// `build_leaf` finds nothing to build and refuses, the marcher draws a filled cube it can never
// replace, occlusion treats it as opaque — and the chisel's own CPU raycast passes straight
// through, which is the sentence from the player that found this.
//
// The gate is `world_has`'s own question rather than the count, because the count is a symptom and
// the question is the fault. `any_matter_in` asks it by brick pointer for exactly this reason.
TEST_CASE("a replace paste that empties a brick leaves nothing claiming matter") {
    // One brick's worth of glass at the origin brick, and a clip of pure air over it.
    static constexpr i64 kBrickLow[3] = {0, 0, 0};
    static constexpr i64 kBrickHigh[3] = {7, 7, 7};
    World target;
    MatterLedger ledger;
    for (i64 z = 0; z < 8; ++z) {
        for (i64 y = 0; y < 8; ++y) {
            for (i64 x = 0; x < 8; ++x) target.set(x, y, z, kGlass);
        }
    }
    REQUIRE(any_matter_in(target, kBrickLow, kBrickHigh));
    REQUIRE(target.stats().empty_bricks == 0);

    World source;   // nothing in it at all
    const Clip air = capture_clip(source, 0, 0, 0, 7, 7, 7);
    REQUIRE(air.solid_count() == 0);

    SUBCASE("the brick goes with its last voxel") {
        const PasteStats stats =
            paste_clip(target, ledger, air, 0, 0, 0, PasteMode::Replace,
                       MatterReason::PlayerPlace, 1);
        CHECK(stats.bricks_emptied == 1);
        CHECK(target.get(3, 3, 3) == kAir);
        // The three that have to agree, and the third is the one that was wrong: the voxels are
        // gone, the count is nought, and the question every child mask in the render tree is
        // derived from now answers no.
        CHECK(target.stats().empty_bricks == 0);
        CHECK_FALSE(any_matter_in(target, kBrickLow, kBrickHigh));
        // The chunk had one brick and now has none, so it goes too -- `world_has` answers level 8
        // and above out of the set of chunks that exist, and an empty one claims eight metres.
        CHECK(target.stats().empty_chunks == 0);
        CHECK_FALSE(target.has_chunk(ChunkCoord{0, 0, 0}));
    }

    SUBCASE("the control arm is the fault, so the gate can fail") {
        const PasteStats stats =
            paste_clip(target, ledger, air, 0, 0, 0, PasteMode::Replace,
                       MatterReason::PlayerPlace, 1, nullptr, 0, 1, /*drop_empty=*/false);
        CHECK(stats.bricks_emptied == 1);
        CHECK(target.get(3, 3, 3) == kAir);
        CHECK(target.stats().empty_bricks == 1);
        // Every voxel is gone and the world still says there is matter here. That is the lump.
        CHECK(any_matter_in(target, kBrickLow, kBrickHigh));
    }

    SUBCASE("a brick the paste only partly clears stays, and stays honest") {
        World keep;
        keep.set(0, 0, 0, kRock);
        const Clip one = capture_clip(keep, 0, 0, 0, 7, 7, 7);
        const PasteStats stats =
            paste_clip(target, ledger, one, 0, 0, 0, PasteMode::Replace,
                       MatterReason::PlayerPlace, 1);
        CHECK(stats.bricks_emptied == 0);
        CHECK(target.get(0, 0, 0) == kRock);
        CHECK(target.get(3, 3, 3) == kAir);
        CHECK(target.stats().empty_bricks == 0);
        CHECK(any_matter_in(target, kBrickLow, kBrickHigh));
    }
}

namespace {
Clip scaled(const Clip& clip, f64 uniform) {
    const f64 factor[3] = {uniform, uniform, uniform};
    return scale_clip(clip, factor);
}
}  // namespace

TEST_CASE("growing by a whole number replaces each voxel with a solid block of itself") {
    World world = scattered_world(0, 0, 0, 6, 6, 6);
    const Clip clip = capture_clip(world, 0, 0, 0, 5, 4, 3);

    for (i32 k = 2; k <= 4; ++k) {
        const Clip bigger = scaled(clip, static_cast<f64>(k));
        REQUIRE(bigger.size[0] == clip.size[0] * k);
        REQUIRE(bigger.size[1] == clip.size[1] * k);
        REQUIRE(bigger.size[2] == clip.size[2] * k);
        // Exactly k³ times the matter, and no holes: every cell of every block matches the
        // voxel it came from.
        CHECK(bigger.solid_count() == clip.solid_count() * static_cast<u64>(k) * k * k);
        for (i32 z = 0; z < bigger.size[2]; ++z) {
            for (i32 y = 0; y < bigger.size[1]; ++y) {
                for (i32 x = 0; x < bigger.size[0]; ++x) {
                    REQUIRE(bigger.at(x, y, z) == clip.at(x / k, y / k, z / k));
                    REQUIRE(bigger.covered(x, y, z) == clip.covered(x / k, y / k, z / k));
                }
            }
        }
    }
}

TEST_CASE("growing then shrinking by the same ratio is the identity") {
    // The round trip is exact, which is what says the two directions agree about where the
    // boundaries are.
    World world = scattered_world(0, 0, 0, 10, 10, 10);
    const Clip clip = capture_clip(world, 0, 0, 0, 7, 6, 5);
    for (i32 k = 2; k <= 4; ++k) {
        const Clip round_trip = scaled(scaled(clip, static_cast<f64>(k)), 1.0 / k);
        CHECK(round_trip.size[0] == clip.size[0]);
        CHECK(round_trip.size[1] == clip.size[1]);
        CHECK(round_trip.size[2] == clip.size[2]);
        CHECK(round_trip.content_hash() == clip.content_hash());
    }
}

TEST_CASE("shrinking keeps a one-voxel-thick slope instead of eating it") {
    // The case a plain majority vote gets wrong: every block a thin diagonal passes through
    // is mostly air, so the majority is air and the slope disappears. Air does not vote.
    World world;
    for (i64 i = 0; i < 64; ++i) world.set(i, i, 0, kRock);   // a 45-degree line, one thick
    const Clip clip = capture_clip(world, 0, 0, 0, 63, 63, 0);
    REQUIRE(clip.solid_count() == 64);

    for (f64 ratio : {0.5, 0.25, 0.4, 0.13}) {
        const Clip smaller = scaled(clip, ratio);
        // Still a line from one corner to the other, still connected, still there.
        CHECK(smaller.solid_count() > 0);
        CHECK(smaller.at(0, 0, 0) != kAir);
        CHECK(smaller.at(smaller.size[0] - 1, smaller.size[1] - 1, 0) != kAir);
        // Every row and column it spans has something in it — no gaps punched through.
        for (i32 x = 0; x < smaller.size[0]; ++x) {
            bool any = false;
            for (i32 y = 0; y < smaller.size[1] && !any; ++y) any = smaller.at(x, y, 0) != kAir;
            REQUIRE(any);
        }
    }
}

TEST_CASE("shrinking keeps a right-angled corner square") {
    // A corner voxel is one of eight in a halving, so a majority vote rounds it off.
    World world;
    for (i64 y = 0; y < 32; ++y) world.set(0, y, 0, kRock);   // the upright
    for (i64 x = 0; x < 32; ++x) world.set(x, 0, 0, kRock);   // the foot
    const Clip clip = capture_clip(world, 0, 0, 0, 31, 31, 0);

    const Clip smaller = scaled(clip, 0.5);
    CHECK(smaller.at(0, 0, 0) != kAir);                       // the corner itself
    CHECK(smaller.at(smaller.size[0] - 1, 0, 0) != kAir);     // the far end of the foot
    CHECK(smaller.at(0, smaller.size[1] - 1, 0) != kAir);     // the top of the upright
}

TEST_CASE("shrinking a wall one voxel thick does not thin it away") {
    World world;
    for (i64 z = 0; z < 40; ++z) {
        for (i64 y = 0; y < 40; ++y) world.set(0, y, z, kRock);
    }
    const Clip clip = capture_clip(world, 0, 0, 0, 0, 39, 39);
    const Clip smaller = scaled(clip, 0.3);
    CHECK(smaller.size[0] == 1);
    CHECK(smaller.solid_count() == smaller.cell_count());   // still a solid wall
}

TEST_CASE("resizing at an awkward ratio leaves no holes") {
    // The property that matters at fractional ratios: a solid block stays solid. A forward
    // resample scatters source voxels into the destination and misses, leaving stripes.
    World world;
    for (i64 z = 0; z < 13; ++z) {
        for (i64 y = 0; y < 13; ++y) {
            for (i64 x = 0; x < 13; ++x) world.set(x, y, z, kRock);
        }
    }
    const Clip clip = capture_clip(world, 0, 0, 0, 12, 12, 12);

    const f64 ratios[] = {0.37, 0.5, 0.81, 1.0, 1.5, 2.25, 4.5, 9.843};
    for (f64 ratio : ratios) {
        const Clip resized = scaled(clip, ratio);
        REQUIRE(resized.cell_count() > 0);
        // Every cell is inside and solid: no gaps anywhere, at any ratio.
        CHECK(resized.covered_count() == resized.cell_count());
        CHECK(resized.solid_count() == resized.cell_count());
    }
}

TEST_CASE("resizing can go per axis and never below one voxel") {
    World world = scattered_world(0, 0, 0, 20, 20, 20);
    const Clip clip = capture_clip(world, 0, 0, 0, 15, 11, 7);

    const f64 stretched[3] = {3.0, 1.0, 0.5};
    const Clip out = scale_clip(clip, stretched);
    CHECK(out.size[0] == 48);
    CHECK(out.size[1] == 12);
    CHECK(out.size[2] == 4);

    // Flattened as far as it will go, an axis stops at a single voxel rather than vanishing.
    const f64 flat[3] = {0.0001, 0.0001, 0.0001};
    const Clip thin = scale_clip(clip, flat);
    CHECK(thin.size[0] >= 1);
    CHECK(thin.size[1] >= 1);
    CHECK(thin.size[2] >= 1);
}

TEST_CASE("resizing by one changes nothing") {
    World world = scattered_world(0, 0, 0, 5, 5, 5);
    const Clip clip = capture_clip(world, 0, 0, 0, 4, 4, 4);
    CHECK(scaled(clip, 1.0).content_hash() == clip.content_hash());
}

TEST_CASE("the occupancy mask marks exactly the blocks that hold matter") {
    World world = scattered_world(0, 0, 0, 30, 30, 30);
    const Clip clip = capture_clip(world, 0, 0, 0, 20, 17, 12);
    REQUIRE(clip.coarse_size[0] == 3);   // 21 voxels is three 8-blocks
    REQUIRE(clip.coarse_size[1] == 3);
    REQUIRE(clip.coarse_size[2] == 2);
    REQUIRE(clip.coarse.size() == clip.coarse_count());

    for (i32 bz = 0; bz < clip.coarse_size[2]; ++bz) {
        for (i32 by = 0; by < clip.coarse_size[1]; ++by) {
            for (i32 bx = 0; bx < clip.coarse_size[0]; ++bx) {
                bool any = false;
                for (i32 z = bz * 8; z < std::min((bz + 1) * 8, clip.size[2]) && !any; ++z) {
                    for (i32 y = by * 8; y < std::min((by + 1) * 8, clip.size[1]) && !any; ++y) {
                        for (i32 x = bx * 8; x < std::min((bx + 1) * 8, clip.size[0]) && !any;
                             ++x) {
                            any = clip.covered(x, y, z) && clip.at(x, y, z) != kAir;
                        }
                    }
                }
                REQUIRE((clip.coarse[clip.coarse_index(bx, by, bz)] != 0) == any);
            }
        }
    }
}

TEST_CASE("every transform leaves the occupancy mask correct") {
    // The mask is a second copy of the truth, so anything that produces a clip has to
    // rebuild it. Forgetting one shows up as a ghost with holes in it.
    World world = scattered_world(0, 0, 0, 20, 20, 20);
    const Clip clip = capture_clip(world, 0, 0, 0, 13, 11, 9);

    Clip cases[] = {clip, rotate_clip(clip, 1, 0.4), rotate_clip(clip, 2, kPi * 0.5),
                    mirror_clip(clip, 0), scaled(clip, 2.0), scaled(clip, 0.4)};
    for (const Clip& c : cases) {
        REQUIRE(c.coarse.size() == c.coarse_count());
        for (i32 z = 0; z < c.size[2]; ++z) {
            for (i32 y = 0; y < c.size[1]; ++y) {
                for (i32 x = 0; x < c.size[0]; ++x) {
                    if (!c.covered(x, y, z) || c.at(x, y, z) == kAir) continue;
                    REQUIRE(c.coarse[c.coarse_index(x >> 3, y >> 3, z >> 3)] != 0);
                }
            }
        }
    }
}

TEST_CASE("a rotated clip stamps the same number of voxels it started with") {
    // Conservation end to end: capture, turn, stamp, count. This is the property the whole
    // shear decomposition exists to guarantee, checked through the op path rather than on
    // the clip in isolation.
    World world = scattered_world(0, 0, 0, 16, 16, 16);
    const Clip clip2 = capture_clip(world, 1, 1, 1, 13, 11, 9);
    const Clip& clip = clip2;
    const Clip turned = rotate_clip(clip, 1, 22.5 * kDegrees);
    REQUIRE(turned.solid_count() == clip.solid_count());

    std::vector<Op> ops;
    clip_to_ops(turned, 500, 500, 500, PasteMode::SolidOnly, 1, 1, ops);

    World target;
    MatterLedger ledger;
    apply_ops(target, ops, ledger);
    CHECK(target.stats().solid_voxels == clip.solid_count());
}
