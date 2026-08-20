// `Field::eval_block` against `Field::eval`, bit for bit, for every op in the language.
//
// This file IS the promise `src/forge/field_block.cpp` makes. The block evaluator exists to walk one
// expression once for a whole block of points instead of once per point, and the only thing that
// makes that a change rather than a rewrite of the world is that it must produce the SAME f64 — not
// a close one. Every world in this repository is gated on a content hash and the sampler settles
// boxes on these numbers, so a last-bit difference is a different world.
//
// So the comparison here is `==` on the double, never `Approx`, and it is made:
//
//   * for every node of a field built to contain every `Op` there is, not only the roots — an
//     intermediate node is a root somebody else's expression will hand to `eval_block`;
//   * with the bounds built and with them NOT built, because the union's cull is only armed in the
//     first case and the two arms take different code;
//   * with a bounding hierarchy over the unions, because that arm cannot be batched at all and has
//     to fall through to `eval` correctly rather than quietly;
//   * over blocks a quarter of a metre across (which is what the sampler actually asks), over blocks
//     that straddle a surface, and over blocks scattered widely enough that different points in one
//     block take different turns through the same union;
//   * and finally on `clips/facility.clip`, which is the real thing: 3,000-odd nodes, seven
//     buildings, every weathering op in use.
//
// # The case that is easy to get wrong, written down
//
// The tempting shape for a block evaluator is to cull a union child once for the whole block, on the
// block's own bounds, reasoning that a child the running answer already beats was never going to win
// the minimum. **D644 measured that assumption and it is false**: `sd_ellipsoid`, `sd_cone`,
// `sd_prism` and `sd_platonic` are bounded approximations and can answer as little as 0.53 of the
// distance to their own box, so a child `eval` skipped may report LESS than the running answer, and a
// block evaluator that evaluates it anyway returns a different number. `a union of the four bounded
// primitives, spread out` below is the case that catches it, and it is the reason there is a case at
// all for shapes nobody would author.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "forge/clip_script.hpp"
#include "forge/field.hpp"
#include "world/tags.hpp"
#include "world/voxel_type.hpp"

using namespace ws;
using namespace ws::forge;

namespace {

// A deterministic spread of points, so a failure is reproducible and a passing run means the same
// thing tomorrow. No `rand()`: a test whose input changes between runs cannot be bisected.
struct Spray {
    u64 state = 0x243F6A8885A308D3ull;
    f64 next(f64 lo, f64 hi) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        const u64 bits = (state >> 11);
        const f64 unit = static_cast<f64>(bits) / static_cast<f64>(1ull << 53);
        return lo + (hi - lo) * unit;
    }
};

// The block the sampler actually asks: 8x8x8 points inside a box a quarter of a metre across, which
// is one node of the render tree.
std::vector<Vec3> node_block(Vec3 corner, u32 side = 8, f64 span = 0.25) {
    std::vector<Vec3> pts;
    pts.reserve(static_cast<usize>(side) * side * side);
    const f64 n = static_cast<f64>(side);
    for (u32 i = 0; i < side; ++i) {
        for (u32 j = 0; j < side; ++j) {
            for (u32 k = 0; k < side; ++k) {
                pts.push_back(Vec3{corner.x + span * (static_cast<f64>(i) + 0.5) / n,
                                   corner.y + span * (static_cast<f64>(j) + 0.5) / n,
                                   corner.z + span * (static_cast<f64>(k) + 0.5) / n});
            }
        }
    }
    return pts;
}

std::vector<Vec3> scattered(usize count, f64 reach) {
    Spray spray;
    std::vector<Vec3> pts;
    pts.reserve(count);
    for (usize i = 0; i < count; ++i) {
        pts.push_back(Vec3{spray.next(-reach, reach), spray.next(-reach, reach),
                           spray.next(-reach, reach)});
    }
    return pts;
}

// The comparison, and the whole point of the file. Returns the index of the first disagreement, or
// -1. Kept as a returned index rather than a CHECK inside the loop so that a failure names the point
// and the two numbers rather than printing five hundred passes.
isize first_disagreement(const Field& f, u32 node, const std::vector<Vec3>& pts,
                         std::vector<f64>& block) {
    block.assign(pts.size(), 0.0);
    f.eval_block(node, pts.data(), pts.size(), block.data());
    for (usize i = 0; i < pts.size(); ++i) {
        const f64 one = f.eval(node, pts[i]);
        // Bit for bit. `==` is deliberate; two NaNs would compare unequal and that is a failure
        // worth hearing about, because the field is not supposed to make any.
        if (!(block[i] == one)) return static_cast<isize>(i);
    }
    return -1;
}

void agrees(const Field& f, u32 node, const std::vector<Vec3>& pts, const char* what) {
    std::vector<f64> block;
    const isize bad = first_disagreement(f, node, pts, block);
    if (bad < 0) return;
    const usize i = static_cast<usize>(bad);
    std::string message = std::string(what) + ": node " + std::to_string(node) + " (" +
                          op_name(f.op_at(node)) + ") disagrees at point " + std::to_string(i) +
                          " (" + std::to_string(pts[i].x) + ", " + std::to_string(pts[i].y) + ", " +
                          std::to_string(pts[i].z) + "): block " + std::to_string(block[i]) +
                          " vs eval " + std::to_string(f.eval(node, pts[i]));
    FAIL_CHECK(message);
}

// Every node of the field, not only the ones a name was bound to.
void every_node_agrees(const Field& f, const std::vector<Vec3>& pts, const char* what) {
    std::vector<f64> block;
    for (u32 node = 0; node < static_cast<u32>(f.size()); ++node) {
        const isize bad = first_disagreement(f, node, pts, block);
        if (bad < 0) continue;
        const usize i = static_cast<usize>(bad);
        std::string message = std::string(what) + ": node " + std::to_string(node) + " (" +
                              op_name(f.op_at(node)) + ") disagrees at point " + std::to_string(i) +
                              ": block " + std::to_string(block[i]) + " vs eval " +
                              std::to_string(f.eval(node, pts[i]));
        FAIL_CHECK(message);
        return;   // one report, not four thousand
    }
}

// --- the zoo ---------------------------------------------------------------------------------
//
// One field containing at least one instance of every `Op` in `field.hpp`, plus the compositions
// that have their own code path: a union whose children are the four primitives that under-state,
// a difference with a cutter far away (so the cull fires), a multiply whose first factor is nought
// (so the short circuit fires), repeats on one, two and three axes, and both arms of every op that
// has a partial sweep.
//
// Written as one field rather than one per case because `every_node_agrees` then covers the
// intermediate nodes too, and because a shared sub-expression handed to two parents is exactly the
// DAG shape the real clips have.
u32 build_zoo(Field& f) {
    std::vector<u32> all;

    // --- constants and coordinates ---
    all.push_back(f.constant(1.5));
    all.push_back(f.parameter("dial", 0.375));
    all.push_back(f.coordinate(0));
    all.push_back(f.coordinate(1));
    all.push_back(f.coordinate(2));
    all.push_back(f.radius(Vec3{0.1, -0.2, 0.3}));

    // --- solids ---
    const u32 sphere = f.sphere(Vec3{0.0, 0.0, 0.0}, 0.4);
    const u32 boxy = f.box(Vec3{0.1, 0.0, 0.0}, Vec3{0.3, 0.2, 0.25}, 0.05);
    const u32 plain_box = f.box(Vec3{0.0, 0.0, 0.0}, Vec3{0.5, 0.5, 0.5}, 0.0);
    const u32 cyl = f.cylinder(Vec3{0.0, 0.1, 0.0}, 0.3, 0.4, 1);
    const u32 cap = f.capsule(Vec3{-0.3, 0.0, 0.0}, Vec3{0.3, 0.2, 0.1}, 0.12);
    const u32 tor = f.torus(Vec3{0.0, 0.0, 0.0}, 0.35, 0.09, 1);
    const u32 arc_whole = f.arc(Vec3{0.0, 0.0, 0.0}, 0.35, 0.09, 1, 0.0, 1.0);
    const u32 arc_part = f.arc(Vec3{0.0, 0.0, 0.0}, 0.35, 0.09, 1, 0.1, 0.6);
    const u32 cone = f.cone(Vec3{0.0, -0.4, 0.0}, 0.3, 0.6, 1);
    const u32 half = f.plane(Vec3{0.0, 1.0, 0.0}, 0.2);
    const u32 ellip = f.ellipsoid(Vec3{0.0, 0.0, 0.0}, Vec3{0.4, 0.2, 0.3});
    const u32 prism = f.prism(Vec3{0.0, 0.0, 0.0}, 0.3, 0.25, 6, 1, 0.05);
    all.insert(all.end(), {sphere, boxy, plain_box, cyl, cap, tor, arc_whole, arc_part, cone, half,
                           ellip, prism});
    for (u32 which = 0; which < 5; ++which) {
        all.push_back(f.platonic(Vec3{0.0, 0.0, 0.0}, 0.3, which));
    }
    all.push_back(f.wedge(Vec3{0.0, 0.0, 0.0}, Vec3{0.3, 0.2, 0.25}, 1, 0));
    all.push_back(f.stairs(Vec3{0.0, 0.0, 0.0}, Vec3{0.4, 0.3, 0.2}, 0.1, 0.08));

    // --- swept, both arms of the partial sweep ---
    const u32 profile = f.box(Vec3{0.3, 0.0, 0.0}, Vec3{0.08, 0.2, 0.5}, 0.0);
    all.push_back(f.revolve(profile, Vec3{0.0, 0.0, 0.0}, 1, 0.0, 1.0));
    all.push_back(f.revolve(profile, Vec3{0.0, 0.0, 0.0}, 1, 0.1, 0.55));
    all.push_back(f.revolve(profile, Vec3{0.05, 0.0, -0.05}, 0, 0.0, 1.0));
    all.push_back(f.spiral(Vec3{0.0, 0.0, 0.0}, 0.3, 0.7, 0.05, 2.0, 2));

    // --- combining ---
    all.push_back(f.unite({sphere, boxy}));
    all.push_back(f.unite({sphere, boxy, cyl}));
    all.push_back(f.unite({sphere, boxy, cyl, cap}));
    all.push_back(f.intersect({sphere, plain_box}));
    all.push_back(f.subtract({plain_box, sphere}));
    all.push_back(f.smooth_unite({sphere, boxy}, 0.08));
    all.push_back(f.smooth_subtract({plain_box, sphere}, 0.08));
    all.push_back(f.smooth_intersect({sphere, plain_box}, 0.08));
    all.push_back(f.chamfer_unite({sphere, boxy}, 0.05));
    all.push_back(f.chamfer_subtract({plain_box, sphere}, 0.05));
    all.push_back(f.chamfer_intersect({sphere, plain_box}, 0.05));
    // A blend of nought and one of a negative width, because both are early-outs in the helper.
    all.push_back(f.smooth_unite({sphere, cyl}, 0.0));
    all.push_back(f.chamfer_unite({sphere, cyl}, 0.0));

    // THE CASE D644 IS ABOUT: four bounded primitives, spread far enough apart that a point near one
    // of them culls the other three on their boxes. If the block evaluator ever culls differently
    // from `eval` — more OR less — this is where it shows.
    const u32 far_ellip = f.translate(f.ellipsoid(Vec3{0, 0, 0}, Vec3{0.5, 0.2, 0.3}),
                                      Vec3{-2.0, 0.0, 0.0});
    const u32 far_cone = f.translate(f.cone(Vec3{0, 0, 0}, 0.4, 0.8, 1), Vec3{2.0, 0.0, 0.0});
    const u32 far_prism = f.translate(f.prism(Vec3{0, 0, 0}, 0.4, 0.3, 5, 1, 0.0),
                                      Vec3{0.0, 0.0, 2.0});
    const u32 far_plato = f.translate(f.platonic(Vec3{0, 0, 0}, 0.4, 3), Vec3{0.0, 0.0, -2.0});
    const u32 bounded_four = f.unite({far_ellip, far_cone, far_prism, far_plato});
    all.push_back(bounded_four);
    // And a difference whose cutter is far away, which is the other cull.
    all.push_back(f.subtract({bounded_four, f.translate(sphere, Vec3{6.0, 0.0, 0.0})}));

    // --- moving the point ---
    all.push_back(f.translate(boxy, Vec3{0.2, -0.1, 0.05}));
    all.push_back(f.rotate(boxy, Vec3{0.05, 0.125, -0.3}));
    all.push_back(f.scale(boxy, Vec3{1.5, 1.5, 1.5}));
    all.push_back(f.scale(boxy, Vec3{2.0, 0.5, 1.25}));
    all.push_back(f.scale(boxy, Vec3{0.0, 0.0, 0.0}));   // the "a zero means one" arm
    all.push_back(f.mirror(boxy, 0));
    all.push_back(f.mirror(f.translate(boxy, Vec3{0.4, 0, 0}), 2));
    all.push_back(f.repeat(sphere, Vec3{0.5, 0.0, 0.0}, Vec3{0, 0, 0}));
    all.push_back(f.repeat(sphere, Vec3{0.5, 0.0, 0.5}, Vec3{0, 0, 0}));
    all.push_back(f.repeat(sphere, Vec3{0.5, 0.4, 0.5}, Vec3{0, 0, 0}));
    all.push_back(f.repeat(sphere, Vec3{0.5, 0.4, 0.5}, Vec3{2, 1, 2}));
    // Off-centre in its cell, which is the case the leaning-neighbour walk exists for.
    all.push_back(f.repeat(f.translate(sphere, Vec3{0.18, 0.0, 0.0}), Vec3{0.5, 0.0, 0.0},
                           Vec3{3, 0, 0}));
    all.push_back(f.scatter(f.ellipsoid(Vec3{0, 0, 0}, Vec3{0.06, 0.04, 0.05}),
                            Vec3{0.15, 0.0, 0.15}, Vec3{0, 0, 0}, 0.45, 0.5));
    all.push_back(f.scatter(f.sphere(Vec3{0, 0, 0}, 0.05), Vec3{0.15, 0.0, 0.15}, Vec3{4, 0, 4},
                            0.0, 0.0));
    all.push_back(f.polar_repeat(f.translate(cyl, Vec3{0.6, 0.0, 0.0}), 8, 1, 0.0, 1.0));
    all.push_back(f.polar_repeat(f.translate(cyl, Vec3{0.6, 0.0, 0.0}), 7, 1, 0.1, 0.4));
    all.push_back(f.polar_repeat(f.translate(cyl, Vec3{0.6, 0.0, 0.0}), 1, 2, 0.25, 0.5));

    // --- changing the answer ---
    all.push_back(f.shell(boxy, 0.03));
    all.push_back(f.round_off(boxy, 0.04));
    all.push_back(f.offset(boxy, -0.02));
    all.push_back(f.twist(boxy, 0.35, 1));
    all.push_back(f.bend(boxy, 0.2, 0));

    // --- patterns ---
    const u32 grain = f.fbm(0.3, 4, 0.5, 2.0, 7, Vec3{1.0, 6.0, 1.0});
    const u32 wave = f.sine(1, 0.25, 0.125);
    all.push_back(wave);
    all.push_back(f.sine(0, 0.0, 0.0));   // the "period nought means one" arm
    all.push_back(f.waves(2, 0.3, 0.2, 0.1));
    all.push_back(f.noise(0.25, 3, Field::kNoStretch));
    all.push_back(f.noise(0.25, 3, Vec3{2.0, 0.0, 0.5}));
    all.push_back(grain);
    all.push_back(f.ridged(0.3, 3, 0.5, 2.1, 11, Vec3{1.0, 1.0, 4.0}));
    all.push_back(f.rasp(0.12, 0.4, 5, Vec3{3.0, 1.0, 1.0}));
    all.push_back(f.cells(0.2, 9, Field::kNoStretch));
    all.push_back(f.cell_edge(0.2, 9, Vec3{1.0, 2.0, 1.0}));
    all.push_back(f.checker(Vec3{0.2, 0.2, 0.2}));
    all.push_back(f.checker(Vec3{0.2, 0.0, 0.0}));
    all.push_back(f.stripes(0, 0.3, 0.4));
    all.push_back(f.bricks(Vec3{0.24, 0.08, 0.0}, 0.012, 2));
    all.push_back(f.displace(boxy, grain, 0.05));
    all.push_back(f.displace(f.unite({sphere, boxy}), wave, 0.03));

    // --- what the shape is doing here ---
    all.push_back(f.curvature(boxy, 0.05));
    all.push_back(f.curvature(f.unite({sphere, boxy}), 0.0));   // the default-radius arm
    all.push_back(f.occlusion(boxy, 0.15));
    all.push_back(f.occlusion(f.unite({sphere, boxy}), 0.0));
    all.push_back(f.facing(boxy, 1));
    all.push_back(f.facing(f.unite({sphere, cyl}), 0));

    // --- arithmetic ---
    all.push_back(f.add({wave, grain}));
    all.push_back(f.add({wave, grain, f.constant(0.25)}));
    all.push_back(f.multiply({wave, grain}));
    // The short circuit: a first factor that really is nought over most of the block.
    const u32 gate = f.step(f.coordinate(0), 0.0);
    all.push_back(f.multiply({gate, f.occlusion(boxy, 0.15)}));
    all.push_back(f.multiply({gate, f.occlusion(boxy, 0.15), f.curvature(boxy, 0.05)}));
    all.push_back(f.minimum({wave, grain}));
    all.push_back(f.maximum({wave, grain}));
    all.push_back(f.blend(wave, grain, 0.35));
    all.push_back(f.blend(wave, grain, 2.0));   // clamped
    all.push_back(f.remap(grain, -1.0, 1.0, 0.0, 1.0));
    all.push_back(f.remap(grain, 0.5, 0.5, 0.0, 1.0));   // the zero-span arm
    all.push_back(f.absolute(grain));
    all.push_back(f.negate(grain));
    all.push_back(f.step(grain, 0.0));
    all.push_back(f.smoothstep(grain, -0.2, 0.2));
    all.push_back(f.smoothstep(grain, 0.1, 0.1));   // the zero-span arm
    all.push_back(f.clamp_to(grain, -0.5, 0.5));
    all.push_back(f.power(grain, 1.5));
    all.push_back(f.power(grain, 1.0));

    // One root over the lot, so there is a single deep expression as well as a hundred shallow ones.
    // Built in fours because that is what a union node holds.
    std::vector<u32> level = all;
    while (level.size() > 1) {
        std::vector<u32> next;
        for (usize i = 0; i < level.size(); i += 4) {
            std::vector<u32> group;
            for (usize j = i; j < level.size() && j < i + 4; ++j) group.push_back(level[j]);
            next.push_back(group.size() == 1 ? group[0] : f.unite(group));
        }
        level.swap(next);
    }
    return level[0];
}

// The blocks every zoo arm is checked over. Chosen to include the sampler's own shape (a quarter of
// a metre), blocks that straddle a surface, and one spread wide enough that points in the same block
// disagree about which union child is nearest — which is what makes the per-point cull replay a
// thing the test can see.
std::vector<std::vector<Vec3>> zoo_blocks() {
    std::vector<std::vector<Vec3>> blocks;
    blocks.push_back(node_block(Vec3{0.0, 0.0, 0.0}, 4));         // over the origin
    blocks.push_back(node_block(Vec3{0.30, -0.12, 0.05}, 4));     // straddling several surfaces
    blocks.push_back(node_block(Vec3{-2.1, -0.1, 0.0}, 4));       // on the far ellipsoid
    blocks.push_back(node_block(Vec3{1.9, -0.3, -0.1}, 4));       // on the far cone
    blocks.push_back(node_block(Vec3{0.0, 0.0, 1.85}, 4));        // on the far prism
    blocks.push_back(node_block(Vec3{7.0, 7.0, 7.0}, 4));         // outside everything
    blocks.push_back(scattered(128, 3.0));                        // points that disagree
    blocks.push_back(scattered(97, 0.6));                         // an odd count
    blocks.push_back(std::vector<Vec3>{Vec3{0.123, -0.456, 0.789}});   // one point
    return blocks;
}

std::string facility_path() {
    return std::string(WS_ASSET_SOURCE_DIR) + "/../clips/facility.clip";
}

}  // namespace

TEST_CASE("eval_block matches eval on every op, with the bounds built") {
    Field f;
    const u32 root = build_zoo(f);
    f.build_bounds();
    CHECK(root < f.size());

    for (const std::vector<Vec3>& block : zoo_blocks()) {
        every_node_agrees(f, block, "bounds built");
    }
}

TEST_CASE("eval_block matches eval on every op with no bounds at all") {
    // `bounds_` empty is a different arm of the union and of the difference: no sort, no cull, and
    // the children come in declaration order. A field is in this state until `build_bounds()` is
    // called, and plenty of callers never call it.
    Field f;
    const u32 root = build_zoo(f);
    CHECK(root < f.size());
    for (const std::vector<Vec3>& block : zoo_blocks()) {
        every_node_agrees(f, block, "no bounds");
    }
}

TEST_CASE("eval_block matches eval with a bounding hierarchy over the unions") {
    // The accelerated union is the one arm `eval_block` cannot batch, so what is being checked here
    // is that it falls through to `eval` rather than quietly answering something else.
    Field f;
    const u32 root = build_zoo(f);
    f.accelerate_from(2);
    f.build_bounds();
    CHECK(root < f.size());
    CHECK(f.accelerator_count() > 0);
    for (const std::vector<Vec3>& block : zoo_blocks()) {
        every_node_agrees(f, block, "accelerated");
    }
}

TEST_CASE("eval_block matches eval across a surface, voxel by voxel") {
    // A block that straddles a surface is the one the sampler cares about most: it is where the
    // sign changes, so a last-bit difference is a voxel that is there in one arm and not the other.
    Field f;
    const u32 shell = f.shell(f.round_off(f.subtract({f.box(Vec3{0, 0, 0}, Vec3{0.5, 0.5, 0.5}, 0.02),
                                                      f.sphere(Vec3{0.3, 0.3, 0.0}, 0.35)}),
                                          0.01),
                             0.04);
    const u32 pattern = f.fbm(0.2, 4, 0.5, 2.0, 3, Field::kNoStretch);
    const u32 root = f.displace(shell, pattern, 0.02);
    f.build_bounds();

    // A voxel is 1/32 m. Walk a 32-voxel line straight through the wall, one block a step.
    std::vector<Vec3> line;
    for (u32 i = 0; i < 512; ++i) {
        const f64 t = -0.8 + 1.6 * (static_cast<f64>(i) + 0.5) / 512.0;
        line.push_back(Vec3{t, 0.02, 0.011});
    }
    agrees(f, root, line, "straddling line");
    agrees(f, shell, line, "straddling line, shell");

    // And a solid 0.25 m block sitting on the face.
    agrees(f, root, node_block(Vec3{0.42, -0.1, -0.1}), "block on the face");
}

TEST_CASE("eval_block answers a block of one, and of none") {
    Field f;
    const u32 root = build_zoo(f);
    f.build_bounds();

    f64 one = 12345.0;
    const Vec3 p{0.21, -0.07, 0.13};
    f.eval_block(root, &p, 1, &one);
    CHECK(one == f.eval(root, p));

    // Nothing asked, nothing written, nothing read.
    f.eval_block(root, nullptr, 0, nullptr);
}

TEST_CASE("eval_block counts the same field visits per point that eval does") {
    // The counter is what D722's 632 visits an evaluation is a count of, and a block evaluator that
    // counted differently would make every figure taken with it incomparable with every figure taken
    // before it. So: one visit per node per point, either way.
    //
    // Not asserted equal, because the two are allowed to differ where the block skips work `eval`
    // would have done — a multiply whose factors are all nought for the whole block, say. Asserted
    // NEAR, which is what makes the number still mean something.
    Field f;
    const u32 root = build_zoo(f);
    f.build_bounds();
    const std::vector<Vec3> block = node_block(Vec3{0.0, 0.0, 0.0});

    reset_field_visits();
    for (const Vec3& p : block) (void)f.eval(root, p);
    const u64 by_point = field_visits();

    std::vector<f64> out(block.size());
    reset_field_visits();
    f.eval_block(root, block.data(), block.size(), out.data());
    const u64 by_block = field_visits();

    std::printf("\n--- field visits, one block of %zu on the zoo -----------------------------\n"
                "  eval, one point at a time   %llu\n"
                "  eval_block                  %llu   (%.3fx)\n",
                block.size(), static_cast<unsigned long long>(by_point),
                static_cast<unsigned long long>(by_block),
                static_cast<f64>(by_block) / static_cast<f64>(by_point));

    CHECK(by_block > 0);
    // EQUAL, not merely close: the block evaluator walks a child over exactly the sub-block of
    // points  would have walked it for, so the two counts are the same number. If they ever
    // are not, the cull replay has drifted and the content hash is next.
    CHECK(by_block == by_point);
}

TEST_CASE("eval_block matches eval on clips/facility.clip") {
    // The real thing. Three thousand nodes, seven buildings, every weathering op in use, and the
    // clip every performance figure in this repository is taken against.
    VoxelTypeTable types;
    TagRegistry tags;
    const Script script = load_clip_script(facility_path(), types, tags);
    REQUIRE_MESSAGE(script.errors.empty(), "clips/facility.clip did not parse");
    REQUIRE(script.has_solid);
    const Field& f = script.field;

    // The solid, over blocks spread through the estate: some in matter, some in air, some on a face.
    const Vec3 corners[] = {
        {0.0, 0.0, 0.0},     {2.0, 0.5, 3.0},     {-6.0, 1.0, -4.0},   {12.0, 4.0, 8.0},
        {-14.0, 0.0, 10.0},  {6.0, 8.0, -6.0},    {0.0, 12.0, 0.0},    {20.0, 2.0, 20.0},
        {-2.5, 3.25, 1.75},  {9.0, 0.125, -11.0},
    };
    for (const Vec3& corner : corners) {
        agrees(f, script.solid, node_block(corner), "facility solid");
    }
    agrees(f, script.solid, scattered(512, 25.0), "facility solid, scattered");

    // And every named part, which is where the patterns and the weathering live.
    const std::vector<Vec3> block = node_block(Vec3{1.0, 1.0, 1.0}, 4);
    const std::vector<Vec3> wide = scattered(64, 20.0);
    for (const std::pair<std::string, u32>& part : script.parts) {
        if (part.second >= f.size()) continue;
        agrees(f, part.second, block, part.first.c_str());
        agrees(f, part.second, wide, part.first.c_str());
    }

    // And every paint rule's expression, which is what `metric_slack` and the block sampler will be
    // asking about next.
    for (const PaintRule& rule : script.paint) {
        if (rule.test < f.size()) agrees(f, rule.test, block, "paint rule test");
        if (rule.has_place && rule.place < f.size()) agrees(f, rule.place, block, "paint rule place");
    }
}

TEST_CASE("how much of one evaluation the block evaluator saves") {
    // THE number this whole file exists for: 512 points inside a box a quarter of a metre across,
    // which is one node of the render tree, asked one at a time against asked all at once.
    VoxelTypeTable types;
    TagRegistry tags;
    const Script script = load_clip_script(facility_path(), types, tags);
    REQUIRE_MESSAGE(script.errors.empty(), "clips/facility.clip did not parse");
    const Field& f = script.field;

    struct Where {
        const char* name;
        Vec3 corner;
    };
    const Where places[] = {
        {"near the origin", {0.0, 0.0, 0.0}},
        {"in the wall", {2.0, 1.0, 0.0}},
        {"out in the air", {0.0, 30.0, 0.0}},
        {"just off the site", {40.0, 0.0, 40.0}},
        {"on the ground", {4.0, 0.0, -4.0}},
        {"up in the roof", {-6.0, 9.0, 2.0}},
    };

    std::printf("\n--- one 0.25 m block of 512 points, clips/facility.clip ---------------------\n"
                "  %-18s %12s %12s %8s %10s\n", "where", "one at a time", "as a block", "ratio",
                "visits");
    for (const Where& place : places) {
        const std::vector<Vec3> block = node_block(place.corner);
        std::vector<f64> out(block.size(), 0.0);
        std::vector<f64> one(block.size(), 0.0);

        // Warm both arms before either is timed, so neither pays for the other's cache misses.
        for (usize i = 0; i < block.size(); ++i) one[i] = f.eval(script.solid, block[i]);
        f.eval_block(script.solid, block.data(), block.size(), out.data());

        // THE BEST OF NINE INTERLEAVED ROUNDS, and the shape of that is not fussiness.
        //
        // One block of five hundred points is milliseconds of work, which is long enough to measure
        // and long enough to be interrupted. A single reading is therefore an upper bound polluted
        // by whatever else the machine felt like doing — this file's own first readings swung by
        // eight times on unchanged code with a stray `WorldShaper.exe` at 448 CPU-seconds beside
        // them, which is D722's trap exactly, and it does not look like noise, it looks like a
        // result. The minimum of several rounds is the one statistic contention can only push the
        // wrong way, so it is the one taken; the arms are interleaved so neither can own a quiet
        // stretch the other did not get.
        f64 by_point = 1e30;
        f64 by_block = 1e30;
        for (u32 round = 0; round < 9; ++round) {
            const auto t0 = std::chrono::steady_clock::now();
            for (usize i = 0; i < block.size(); ++i) one[i] = f.eval(script.solid, block[i]);
            const auto t1 = std::chrono::steady_clock::now();
            f.eval_block(script.solid, block.data(), block.size(), out.data());
            const auto t2 = std::chrono::steady_clock::now();
            by_point = std::min(by_point, std::chrono::duration<f64>(t1 - t0).count());
            by_block = std::min(by_block, std::chrono::duration<f64>(t2 - t1).count());
        }

        const f64 per_point_us = by_point * 1e6;
        const f64 per_block_us = by_block * 1e6;

        // The visit counts, taken outside the timing so the counter is not what is being measured.
        reset_field_visits();
        for (usize i = 0; i < block.size(); ++i) one[i] = f.eval(script.solid, block[i]);
        const u64 visits_point = field_visits();
        reset_field_visits();
        f.eval_block(script.solid, block.data(), block.size(), out.data());
        const u64 visits_block = field_visits();

        std::printf("  %-18s %9.1f us %9.1f us %7.2fx %10llu%s\n", place.name, per_point_us,
                    per_block_us, (per_block_us > 0.0) ? per_point_us / per_block_us : 0.0,
                    static_cast<unsigned long long>(visits_block),
                    (visits_block == visits_point) ? "" : "  MISMATCH");
        CHECK(visits_block == visits_point);

        // The promise, on the same numbers the timing was taken over.
        for (usize i = 0; i < block.size(); ++i) CHECK(out[i] == one[i]);
    }
    std::printf("\n");
}
