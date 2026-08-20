// R2b's second half: a node finer than the pixel is never STORED.
//
// The half that has been blocked since D259 on one sentence — "eviction can only drop what it can
// afford to rebuild" — with R12 named as what unblocks it. What is tested here is the way out that
// does not need the card: the pool never has to rebuild a node the MARCHER's own descent cannot
// target, because nothing will ever report it missing. `node_march` clamps its target to
// `max(floor(log2(t * pixel_angle)) + dither, kLeafLevel)` and the dither is never negative, so the
// finest level any ray at a distance can address is fixed and knowable from the CPU.
//
// Three things have to hold for that to be a policy rather than a hope, and each has a test:
//
//   the rule agrees with the LADDER's rule rather than being a second one (D674 is the evening this
//   project lost to a rule that was working, read through an instrument that could not tell
//   "correct" from "broken");
//
//   the node the marcher stops at keeps the colour it was folded from — which is the whole safety
//   argument, because a node whose subtree has been given up is the thing being DRAWN, and
//   `fold_children` re-folding it from eight empty slots paints it black;
//
//   the control arm holds the same node. A gate that cannot fail is not a gate (D621), so every
//   residency assertion here is made twice, once with the rule off, and the two must differ.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdio>

#include "world/node_pool.hpp"
#include "world/voxel_type.hpp"

using namespace ws;

namespace {

// The ladder's split loop, re-derived rather than restated.
//
// This is deliberately NOT `subpixel_finest_level`'s formula written a second way — that would be a
// tautology, and trap 26 is exactly a set of checks that all agree because they are all downstream
// of one reader. It is `Application::refine_candidate`'s test as it is written in main.cpp:
//
//     keen = across / max(away, 0.5);   split while keen > kRefineSplitAt
//
// with `across` the node's own size in metres. What comes back is the level the ladder settles a
// node at, from which its CELLS are `2^(level-3)` voxels across — a ladder node being eight cells a
// side, which is the eight folded into `kRefineSplitAt`.
u32 ladder_settled_level(f64 distance_metres) {
    u32 level = 20;
    while (level > 0) {
        const f64 across = static_cast<f64>(i64{1} << level) / 32.0;
        if (!(across / std::max(distance_metres, 0.5) > kLadderSplitAt)) break;
        --level;
    }
    return level;
}

struct Fixture {
    VoxelTypeTable types;
    World world;
    NodePool pool;
    VoxelTypeId stone = kAir;

    // A pixel angle far coarser than a real one, so the thresholds land a few thousand voxels from
    // the origin instead of a few hundred metres and the test world stays small. The rule is a
    // ratio, so nothing about its shape depends on the constant — and the shipping constant is
    // pinned separately, against the ladder, in "the rule is the ladder's rule".
    // `infinite` is R8e's arm, and it defaults to the shipped one so that every case written before
    // R8a keeps exactly the pool it had. R8e is meant to change what a DESCENT can target and
    // nothing about what the pool keeps, so a residency case that reads differently between the two
    // is the interesting failure rather than a case needing updating.
    explicit Fixture(bool rule_on, f64 pixel_angle = 0.25, bool infinite = false) {
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
        budget.subpixel_rule = rule_on;
        budget.pixel_angle = pixel_angle;
        budget.infinite_detail = infinite;
        pool.create(budget, types);
    }

    void fill_box(i64 x0, i64 y0, i64 z0, i64 x1, i64 y1, i64 z1) {
        for (i64 z = z0; z <= z1; ++z) {
            for (i64 y = y0; y <= y1; ++y) {
                for (i64 x = x0; x <= x1; ++x) world.set(x, y, z, stone);
            }
        }
    }

    const NodeUploadBatch& serve(u64 frame) {
        const f64 camera[3] = {0.0, 0.0, 0.0};
        return pool.update(world, camera, frame);
    }

    void want(i64 x, i64 y, i64 z) { pool.request(node_key_of(x, y, z, kLeafLevel)); }

    // The same, with a view that tells the pool how big a pixel actually is. This is the path
    // `main.cpp` now fills in, and it is the only way the rule can be right above 1280 lines.
    const NodeUploadBatch& serve_at(u64 frame, f32 pixel_angle) {
        const f64 camera[3] = {0.0, 0.0, 0.0};
        NodeView view;
        view.origin[0] = 0.0; view.origin[1] = 0.0; view.origin[2] = 0.0;
        view.forward[0] = 0.0f; view.forward[1] = 0.0f; view.forward[2] = 1.0f;
        view.right[0] = 1.0f;   view.right[1] = 0.0f;   view.right[2] = 0.0f;
        view.up[0] = 0.0f;      view.up[1] = 1.0f;      view.up[2] = 0.0f;
        view.tan_half_fov = 1.0f;
        view.aspect = 1.0f;
        view.pixel_angle = pixel_angle;
        view.valid = true;
        return pool.update(world, camera, frame, &view);
    }
};

// What one pixel subtends, from the same three numbers `main.cpp` now gives the pool and the same
// three `visibility.comp` gives the rays: `2 * lens.x / resolution.y * lens.z`.
//
// The lens is 90 degrees, so `tan(45)` is exactly 1 and the vertical half-angle drops out as a
// one. That is why the whole disagreement below is a statement about the number of LINES and
// about nothing else.
constexpr f64 pixel_angle_for(f64 lines, f64 detail_bias = 1.0) {
    return 2.0 * 1.0 / lines * detail_bias;
}

// The two sites every residency test uses: one brick the camera is on top of, and one far enough
// away that no ray can address a brick there at all.
constexpr i64 kNearAt = 32;     // 1 m from the camera
constexpr i64 kFarAt = 4096;    // 128 m at 32 voxels a metre

}  // namespace

TEST_CASE("the rule is the ladder's rule, not a second one") {
    // D674 cost an evening because a line could not tell a correct answer from a broken one, and the
    // arithmetic that would have settled it was four lines. This is those four lines, standing.
    //
    // The claim: at any distance, the finest node the marcher can address is exactly the size of the
    // finest CELL the ladder will ever sample there. Both sides are computed here — the pool's from
    // `subpixel_finest_level`, the ladder's by running its own split loop — and they must agree at
    // every distance, not merely at the three D674 names.
    for (i64 metres = 1; metres <= 4000; ++metres) {
        const f64 d = static_cast<f64>(metres);
        const f64 d_voxels = d * 32.0;

        const i32 ladder = static_cast<i32>(ladder_settled_level(d)) - 3;
        const i32 want = std::clamp(ladder, static_cast<i32>(kLeafLevel),
                                    static_cast<i32>(kMarcherMaxDetail));
        const u32 got = subpixel_finest_level(d_voxels, kSubPixelAngle);
        CHECK(got == static_cast<u32>(want));
    }

    // And the three sentences D674 actually wrote down, named rather than left implicit: metre 32
    // within 31.25 m, metre 16 to 62.5, metre 8 to 125. The level a settled node sits at and the
    // resolution it is sampled at are the same fact said two ways — `refine_resolution` is
    // `256 / 2^level`, so level 3 IS metre 32 — and getting the two an index apart is the mistake
    // this block exists to make loud. It caught itself being written that way.
    CHECK(ladder_settled_level(31.24) == 3);    // metre 32
    CHECK(ladder_settled_level(31.26) == 4);    // metre 16
    CHECK(ladder_settled_level(62.4) == 4);
    CHECK(ladder_settled_level(62.6) == 5);     // metre 8
    CHECK(ladder_settled_level(124.9) == 5);
    CHECK(ladder_settled_level(125.1) == 6);    // metre 4

    // The pool's own thresholds at the shipping constant, written out so a change to `kSubPixelAngle`
    // has to come here and say what it now means.
    CHECK(subpixel_finest_level(249.0 * 32.0, kSubPixelAngle) == 3);   // a leaf is still addressable
    CHECK(subpixel_finest_level(251.0 * 32.0, kSubPixelAngle) == 4);   // past here it is not
    CHECK(subpixel_finest_level(499.0 * 32.0, kSubPixelAngle) == 4);
    CHECK(subpixel_finest_level(501.0 * 32.0, kSubPixelAngle) == 5);

    // Nothing coarser than the marcher's own coarsest march level, ever. `node_march` clamps its
    // target to kNodeMaxDetail, so past about 1.6 km it stops asking coarser and goes on asking for
    // level 7 — and a rule that eroded past 7 would take the very node those rays land on.
    CHECK(subpixel_finest_level(100000.0 * 32.0, kSubPixelAngle) == kMarcherMaxDetail);

    // A camera that has not been set answers "keep everything" rather than "keep nothing". The
    // difference is an empty screen.
    CHECK(subpixel_finest_level(0.0, kSubPixelAngle) == kLeafLevel);
}

TEST_CASE("a far subtree is given up, and the node the marcher stops at keeps its colour") {
    Fixture f(true);
    REQUIRE(f.pool.subpixel_rule());   // WS_SUBPIXEL must not be set while the tests run

    f.fill_box(kNearAt, 0, 0, kNearAt + 7, 7, 7);
    f.fill_box(kFarAt, 0, 0, kFarAt + 7, 7, 7);

    // The FIRST build is allowed all the way down, and that is not a leak in the rule — it is what
    // makes the rule safe. A node stands in for its subtree by drawing the colour it was folded
    // from, and a node that has never had a built child has no colour. Build once, fold, then never
    // again.
    for (u64 frame = 1; frame <= 4; ++frame) {
        f.want(kNearAt, 0, 0);
        f.want(kFarAt, 0, 0);
        f.serve(frame);
    }
    const NodeKey far_leaf = node_key_of(kFarAt, 0, 0, kLeafLevel);
    const NodeKey near_leaf = node_key_of(kNearAt, 0, 0, kLeafLevel);
    REQUIRE(f.pool.find(far_leaf) != kNoNode);
    REQUIRE(f.pool.find(near_leaf) != kNoNode);

    // The level the marcher actually stops at out there, and its colour BEFORE anything is given up.
    const NodeKey stop_at = node_key_of(kFarAt, 0, 0, kMarcherMaxDetail);
    const u32 stop_slot = f.pool.find(stop_at);
    REQUIRE(stop_slot != kNoNode);
    const u32 colour_before = f.pool.nodes()[stop_slot].colour;
    REQUIRE((colour_before >> 24) != 0);   // it was folded from something

    // Now let the sweep run, asking for nothing. Every frame here is far inside `cold_frames`, so
    // age cannot be what takes anything: whatever goes, goes because nothing can address it.
    u32 given_up = 0;
    for (u64 frame = 5; frame <= 48; ++frame) given_up += f.serve(frame).evicted_subpixel;

    CHECK(given_up > 0);
    CHECK(f.pool.find(far_leaf) == kNoNode);      // the brick nobody can resolve is gone
    CHECK(f.pool.find(near_leaf) != kNoNode);     // the brick under the camera is not

    // The safety argument, asserted rather than reasoned about. `node_descend` stops here and
    // returns kFoundHere, and `node_march` draws this node's folded colour as an ordinary coarse
    // hit — but only if the run is still allocated and the colour survived.
    const u32 stop_after = f.pool.find(stop_at);
    REQUIRE(stop_after != kNoNode);
    CHECK(f.pool.nodes()[stop_after].children != kNoNode);
    CHECK(f.pool.nodes()[stop_after].colour == colour_before);

    // And the independent witness: of everything the pool is still holding, nothing is finer than
    // the rule allows. This walks every slot rather than the erosion slice, so it is a different
    // reader over a different set from the counter above.
    CHECK(f.pool.stats().subpixel_resident == 0);
    CHECK(f.pool.stats().subpixel_evicted > 0);

    CHECK(f.pool.validate());
    CHECK(f.pool.stale_masks(f.world) == 0);
    CHECK(f.pool.stale_leaves(f.world) == 0);
}

TEST_CASE("the control arm holds every one of them") {
    // The same sequence with the rule off. If this passed as well, the test above would be
    // measuring nothing — which is the fault D621 named when it insisted `--no-paste-drop` exist.
    Fixture f(false);
    REQUIRE(!f.pool.subpixel_rule());

    f.fill_box(kNearAt, 0, 0, kNearAt + 7, 7, 7);
    f.fill_box(kFarAt, 0, 0, kFarAt + 7, 7, 7);

    for (u64 frame = 1; frame <= 4; ++frame) {
        f.want(kNearAt, 0, 0);
        f.want(kFarAt, 0, 0);
        f.serve(frame);
    }
    for (u64 frame = 5; frame <= 48; ++frame) CHECK(f.serve(frame).evicted_subpixel == 0);

    CHECK(f.pool.find(node_key_of(kFarAt, 0, 0, kLeafLevel)) != kNoNode);
    CHECK(f.pool.find(node_key_of(kNearAt, 0, 0, kLeafLevel)) != kNoNode);

    // ...and this is the size of the prize, measured on the arm that is not taking it. The witness
    // is not gated on the rule precisely so that the control can report it.
    const NodePoolStats s = f.pool.stats();
    CHECK(s.subpixel_resident > 0);
    CHECK(s.subpixel_bytes > 0);
    CHECK(s.subpixel_evicted == 0);
    CHECK(s.subpixel_refused == 0);
}

TEST_CASE("a request for what nobody can see is refused, and refusing it does not blacken anything") {
    Fixture f(true);
    f.fill_box(kFarAt, 0, 0, kFarAt + 7, 7, 7);

    for (u64 frame = 1; frame <= 4; ++frame) {
        f.want(kFarAt, 0, 0);
        f.serve(frame);
    }
    const NodeKey stop_at = node_key_of(kFarAt, 0, 0, kMarcherMaxDetail);
    const u32 stop_slot = f.pool.find(stop_at);
    REQUIRE(stop_slot != kNoNode);
    const u32 colour_before = f.pool.nodes()[stop_slot].colour;
    REQUIRE((colour_before >> 24) != 0);

    for (u64 frame = 5; frame <= 48; ++frame) f.serve(frame);
    REQUIRE(f.pool.find(node_key_of(kFarAt, 0, 0, kLeafLevel)) == kNoNode);

    // Ask again, the way a ray that had somehow reported it would. The pool must decline, and — the
    // part that is easy to get wrong and impossible to see — the fold on the way back up must not
    // wipe the colour of the node it stopped at. `fold_children` sets a colour to nought when none
    // of a node's children are built, which is right for a node that never had any and wrong for one
    // whose children have been given up. Without the skip this assertion reads 0 and the building
    // draws black.
    u32 refused = 0;
    for (u64 frame = 49; frame <= 56; ++frame) {
        f.want(kFarAt, 0, 0);
        refused += f.serve(frame).refused_subpixel;
    }
    CHECK(refused > 0);
    CHECK(f.pool.find(node_key_of(kFarAt, 0, 0, kLeafLevel)) == kNoNode);

    const u32 stop_after = f.pool.find(stop_at);
    REQUIRE(stop_after != kNoNode);
    CHECK(f.pool.nodes()[stop_after].colour == colour_before);
    CHECK(f.pool.nodes()[stop_after].children != kNoNode);
    CHECK(f.pool.stats().subpixel_refused > 0);
}

TEST_CASE("what the camera is standing in is untouched, and what is given up is stated") {
    Fixture f(true);
    f.fill_box(kNearAt, 0, 0, kNearAt + 7, 7, 7);
    f.fill_box(kFarAt, 0, 0, kFarAt + 7, 7, 7);

    for (u64 frame = 1; frame <= 4; ++frame) {
        f.want(kNearAt, 0, 0);
        f.want(kFarAt, 0, 0);
        f.serve(frame);
    }
    for (u64 frame = 5; frame <= 48; ++frame) f.serve(frame);

    // The near box, voxel for voxel, through the same walk the shader performs. This is the standard
    // the whole file is held to and the rule does not get an exemption from it.
    for (i64 z = 0; z < 8; ++z) {
        for (i64 y = 0; y < 8; ++y) {
            for (i64 x = kNearAt; x < kNearAt + 8; ++x) {
                CHECK(f.pool.mirror_voxel(x, y, z) == f.world.get(x, y, z));
            }
        }
    }

    // And the far box, said plainly rather than left to be discovered. The pool no longer answers a
    // per-voxel question about geometry 128 m away, because no ray asks one: `node_march` stops at
    // the level above and reads the folded colour. Everything that DOES need voxels behind you and
    // under your feet — collision, physics, the chisel — reads `World`, and the twenty-metre
    // proximity radius (D199) is what holds the pool's own copy of that at full detail.
    CHECK(f.pool.mirror_voxel(kFarAt, 0, 0) == kAir);
    CHECK(f.world.get(kFarAt, 0, 0) == f.stone);
}

// ==================================================================================================
// The one decision R2b was owed: `NodeView::pixel_angle`, and where the fallback constant is wrong
// ==================================================================================================
//
// The rule needs to know how big a pixel is. `NodeView::pixel_angle` is the field that says so, and
// until now nothing filled it in — so the pool fell back to `kSubPixelAngle`, which is `main.cpp`'s
// ladder constant with the ladder's eight cells taken back out, 0.002 radians.
//
// That constant is a resolution, whether or not anybody meant it as one. `pixel_angle` is
// `2 * tan(fov/2) / lines`, the lens is 90 degrees so the tangent is exactly one, and 0.002 is
// therefore **one thousand lines**. Below a thousand it is conservative — it thinks pixels are
// smaller than they are, so it believes rays address finer nodes than they do, and it keeps more
// than it needs. Above a thousand the error runs the other way and it is not conservative at all:
// **the pool would give up nodes that rays are still reading.**
//
// This is the case that names the size of that, at 3840x2160.

TEST_CASE("at 4K the fallback constant is wrong, and it is wrong in the unsafe direction") {
    const f64 constant = kSubPixelAngle;
    const f64 at_4k = pixel_angle_for(2160.0);
    const f64 at_800 = pixel_angle_for(800.0);

    // What the constant IS, said as a resolution rather than as a number nobody can place.
    CHECK(constant == doctest::Approx(pixel_angle_for(1000.0)).epsilon(1e-9));
    CHECK(at_800 > constant);      // 800 lines: pixels bigger than the constant thinks. Conservative.
    CHECK(at_4k < constant);       // 2160 lines: pixels smaller. NOT conservative.
    CHECK(constant / at_4k == doctest::Approx(2.16).epsilon(0.01));

    // Walk every metre from 1 to 5000 and record where the two answers part company. `finest` is
    // the finest level a ray at that distance can address, so a HIGHER number from the constant
    // means the constant believes the world may be coarser there than it may actually be — which
    // is the direction that evicts something a ray is reading.
    i64 first_disagreement = 0;
    i64 worst_metre = 0;
    i32 worst_gap = 0;
    i64 disagreeing_metres = 0;
    i64 unsafe_metres = 0;
    for (i64 metres = 1; metres <= 5000; ++metres) {
        const f64 voxels = static_cast<f64>(metres) * 32.0;
        const i32 by_constant = static_cast<i32>(subpixel_finest_level(voxels, constant));
        const i32 by_truth = static_cast<i32>(subpixel_finest_level(voxels, at_4k));
        const i32 gap = by_constant - by_truth;
        // The constant is never FINER than the truth at 4K, at any distance. If it ever were, this
        // would be a two-sided error and the fix would not be a single line.
        REQUIRE(gap >= 0);
        if (gap != 0) {
            ++disagreeing_metres;
            if (first_disagreement == 0) first_disagreement = metres;
            if (gap > worst_gap) { worst_gap = gap; worst_metre = metres; }
        }
        // The unsafe set stated as its own question rather than as the same increment twice: at how
        // many distances does a 4K ray reach a level the constant has already written off?
        if (by_truth < by_constant) ++unsafe_metres;
    }

    // 8,000 voxels is 250 m: the constant's own leaf horizon, and the sentence the handover already
    // carries — "at 1280x800 the marcher addresses a leaf out to 250 m". At 4K it addresses one out
    // to 540, so the first metre at which the constant would take a leaf a ray is reading is 250.
    CHECK(first_disagreement == 250);
    CHECK(worst_gap == 2);
    CHECK(disagreeing_metres == unsafe_metres);   // one-sided, always in the unsafe direction
    CHECK(disagreeing_metres == 4070);            // 250 m to 4319 m; past 4320 both clamp to 7

    // The whole table, printed rather than summarised, because "they differ" is not a finding and
    // "they differ by two levels between 500 and 540 metres" is.
    std::printf(
        "\nR2b pixel angle   constant %.6f rad (= %.0f lines)   4K %.6f rad (2160 lines)   "
        "ratio %.3f\n",
        constant, 2.0 / constant, at_4k, constant / at_4k);
    std::printf("first metre they differ at %lld; worst gap %d levels at %lld m; %lld of 5000 "
                "metres disagree, every one of them with the constant too COARSE\n",
                static_cast<long long>(first_disagreement), worst_gap,
                static_cast<long long>(worst_metre),
                static_cast<long long>(disagreeing_metres));
    std::printf("  metres     finest by the constant    finest at 4K\n");
    for (i64 metres : {100, 249, 251, 400, 501, 539, 541, 900, 1001, 1079, 1081, 1999, 2001, 2159,
                       2161, 4319, 4321}) {
        const f64 voxels = static_cast<f64>(metres) * 32.0;
        std::printf("  %6lld            %u                      %u\n",
                    static_cast<long long>(metres),
                    subpixel_finest_level(voxels, constant),
                    subpixel_finest_level(voxels, at_4k));
    }

    // And the bands, named. Each is where one side has crossed a power of two and the other has not.
    CHECK(subpixel_finest_level(249.0 * 32.0, constant) == 3);
    CHECK(subpixel_finest_level(251.0 * 32.0, constant) == 4);
    CHECK(subpixel_finest_level(251.0 * 32.0, at_4k) == 3);      // a leaf, and the constant said no
    CHECK(subpixel_finest_level(501.0 * 32.0, constant) == 5);
    CHECK(subpixel_finest_level(501.0 * 32.0, at_4k) == 3);      // two levels apart
    CHECK(subpixel_finest_level(541.0 * 32.0, at_4k) == 4);
    CHECK(subpixel_finest_level(2001.0 * 32.0, constant) == kMarcherMaxDetail);
    CHECK(subpixel_finest_level(2001.0 * 32.0, at_4k) == 5);
}

TEST_CASE("the view's pixel angle reaches the rule, and the constant no longer decides") {
    // The line in `main.cpp` is one line and it is worth exactly nothing unless it arrives. So:
    // one pool, one world, two views, and a node whose fate differs between them.
    //
    // 300 m out. At 4K a ray there addresses a LEAF; on the fallback constant it does not, so the
    // constant's arm gives the brick up and the 4K arm keeps it. That is the whole of the unsafety,
    // reduced to one brick and two numbers.
    constexpr i64 kThreeHundred = 300 * 32;

    Fixture truth(true, kSubPixelAngle);   // the budget's fallback; the view will override it
    truth.fill_box(kThreeHundred, 0, 0, kThreeHundred + 7, 7, 7);
    for (u64 frame = 1; frame <= 4; ++frame) {
        truth.want(kThreeHundred, 0, 0);
        truth.serve_at(frame, static_cast<f32>(pixel_angle_for(2160.0)));
    }
    for (u64 frame = 5; frame <= 64; ++frame) {
        truth.serve_at(frame, static_cast<f32>(pixel_angle_for(2160.0)));
    }
    CHECK(truth.pool.find(node_key_of(kThreeHundred, 0, 0, kLeafLevel)) != kNoNode);
    CHECK(truth.pool.subpixel_finest_for(node_key_of(kThreeHundred, 0, 0, kLeafLevel)) == kLeafLevel);

    // The same pool, the same world, the same rule — told nothing, so it uses the constant.
    Fixture fallback(true, kSubPixelAngle);
    fallback.fill_box(kThreeHundred, 0, 0, kThreeHundred + 7, 7, 7);
    for (u64 frame = 1; frame <= 4; ++frame) {
        fallback.want(kThreeHundred, 0, 0);
        fallback.serve(frame);
    }
    for (u64 frame = 5; frame <= 64; ++frame) fallback.serve(frame);
    CHECK(fallback.pool.find(node_key_of(kThreeHundred, 0, 0, kLeafLevel)) == kNoNode);
    CHECK(fallback.pool.subpixel_finest_for(node_key_of(kThreeHundred, 0, 0, kLeafLevel)) == 4);

    // ...and at 800 lines the constant is conservative rather than wrong: it keeps what the rays
    // there address and a little more. Same brick, same distance, no eviction on either.
    Fixture eight_hundred(true, kSubPixelAngle);
    eight_hundred.fill_box(kThreeHundred, 0, 0, kThreeHundred + 7, 7, 7);
    for (u64 frame = 1; frame <= 4; ++frame) {
        eight_hundred.want(kThreeHundred, 0, 0);
        eight_hundred.serve_at(frame, static_cast<f32>(pixel_angle_for(800.0)));
    }
    for (u64 frame = 5; frame <= 64; ++frame) {
        eight_hundred.serve_at(frame, static_cast<f32>(pixel_angle_for(800.0)));
    }
    // 800 lines addresses a leaf out to 200 m only, so at 300 m the brick goes — and it goes on the
    // arm the constant was chosen to be safe for, which is the point: at or below a thousand lines
    // the constant keeps MORE than the rays need, never less.
    CHECK(eight_hundred.pool.subpixel_finest_for(node_key_of(kThreeHundred, 0, 0, kLeafLevel)) == 4);
}


// ==================================================================================================
// R8e against R2b: the mode changes what a descent can TARGET and nothing about what the pool keeps
// ==================================================================================================
//
// The two rules are about different things and it would be very easy for them to stop being. R2b
// evicts a node no ray can address; R8e says a ray near a wall can address something much finer than
// a brick. Read carelessly, the second says the first should stop firing near the camera — and it
// does not, because R2b's floor is the finest thing the pool STORES and that is a brick in both
// arms. `marcher_finest_for` answers below it, `subpixel_finest_for` never does, and this is what
// stands between them.
//
// It is also the shape trap 20 warns about: a mode that quietly made the erosion sweep keep more
// would look like R8e costing nothing and would be R8e being paid for out of R2b's measured table.

TEST_CASE("R8e does not move what R2b evicts") {
    // The same world, the same requests, the same frames, on both arms of the mode -- with R2b's own
    // rule ON, so there is something for the mode to have disturbed.
    Fixture shipped(true, 0.25, false);
    Fixture infinite(true, 0.25, true);
    REQUIRE(!shipped.pool.infinite_detail());
    REQUIRE(infinite.pool.infinite_detail());

    for (Fixture* f : {&shipped, &infinite}) {
        f->fill_box(kNearAt, 0, 0, kNearAt + 7, 7, 7);
        f->fill_box(kFarAt, 0, 0, kFarAt + 7, 7, 7);
        for (u64 frame = 1; frame <= 4; ++frame) {
            f->want(kNearAt, 0, 0);
            f->want(kFarAt, 0, 0);
            f->serve(frame);
        }
        for (u64 frame = 5; frame <= 64; ++frame) f->serve(frame);
    }

    // The near brick is kept and the far one is given up, on both arms and for the same reason.
    CHECK(shipped.pool.find(node_key_of(kNearAt, 0, 0, kLeafLevel)) != kNoNode);
    CHECK(infinite.pool.find(node_key_of(kNearAt, 0, 0, kLeafLevel)) != kNoNode);
    CHECK(shipped.pool.find(node_key_of(kFarAt, 0, 0, kLeafLevel)) == kNoNode);
    CHECK(infinite.pool.find(node_key_of(kFarAt, 0, 0, kLeafLevel)) == kNoNode);

    const NodePoolStats a = shipped.pool.stats();
    const NodePoolStats b = infinite.pool.stats();
    CHECK(a.nodes == b.nodes);
    CHECK(a.leaves == b.leaves);
    CHECK(a.total_bytes == b.total_bytes);
    CHECK(a.subpixel_refused == b.subpixel_refused);
    CHECK(a.subpixel_evicted == b.subpixel_evicted);
    CHECK(a.subpixel_resident == b.subpixel_resident);
    for (u32 level = 0; level < 32; ++level) REQUIRE(a.per_level[level] == b.per_level[level]);

    // ...and nothing below a brick is held on either, which is R8e's second gate asked of the
    // arm that is actually running the mode.
    CHECK(shipped.pool.sub_voxel_bytes() == 0);
    CHECK(infinite.pool.sub_voxel_bytes() == 0);

    // The one thing that DOES differ, so this pair is a gate rather than a pair of identical runs.
    //
    // This fixture's pixel angle is a deliberately coarse 0.25 rad, so a brick eight voxels out has
    // a footprint of two voxels: `floor(log2)` is 1, the shipped arm clamps that to the leaf, and
    // the mode does not. That is the clamp coming off, measured at a distance rather than at the
    // degenerate nought.
    const NodeKey eight_out = node_key_of(8, 0, 0, kLeafLevel);
    CHECK(shipped.pool.marcher_finest_for(eight_out) == static_cast<i32>(kLeafLevel));
    CHECK(infinite.pool.marcher_finest_for(eight_out) == 1);

    // ...and R2b's own reading of the same node is the same on both arms, which is the sentence this
    // whole case exists to hold: the mode moves the descent's target and not the residency floor.
    const NodeKey near_key = node_key_of(kNearAt, 0, 0, kLeafLevel);
    CHECK(shipped.pool.subpixel_finest_for(near_key) ==
          infinite.pool.subpixel_finest_for(near_key));
    CHECK(shipped.pool.subpixel_finest_for(eight_out) ==
          infinite.pool.subpixel_finest_for(eight_out));
}
