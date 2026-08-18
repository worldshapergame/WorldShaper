// The field compiler, held to the one standard that matters: it answers the same distance.
//
// D204's rule is that two readers of one description are the failure mode, and
// `documentation/21-renderer-rewrite.md` names the worst version of it — "two renderers computing
// the same world". A pass that rewrites the expression a clip is made of is a second reader of
// exactly that kind, so most of this file is not about whether the rewrite fired. It is about
// whether the shape moved.
//
// Three arms, deliberately, because a single comparison cannot tell an arithmetic fault from a
// culling one:
//
//   the control    `compile_field` with every rewrite OFF. It walks the same code, places the same
//                  nodes and must come out the same size as what went in. Anything this arm gets
//                  wrong is a fault in the rebuild rather than in a rewrite, and it would otherwise
//                  hide inside the rewrites' own numbers.
//   the semantics  control against compiled, NEITHER with bounds built, so no union culls anything
//                  and what is compared is the arithmetic alone.
//   the shipped    the original field as `load_clip_script` hands it over — bounds built, culls
//                  live — against the compiled one with its own bounds built. This is the
//                  configuration the sampler actually runs, and it is the one whose worst deviation
//                  is quoted in metres against a 3.125 cm voxel.
//
// The estate is the scene, because it is the clip the game ships and the one every number in
// D681-D686 was taken on. A synthetic field would prove the rewrites can fire; only the estate can
// say whether they do.

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "forge/clip_script.hpp"
#include "forge/compile.hpp"
#include "forge/field.hpp"
#include "world/tags.hpp"
#include "world/voxel_type.hpp"

using namespace ws;
using namespace ws::forge;

namespace {

// One voxel at the resolution the estate is authored at: 32 to the metre.
constexpr f64 kVoxel = 1.0 / 32.0;

CompileOptions all_off() {
    CompileOptions o;
    o.fold_transforms = false;
    o.flatten_combines = false;
    o.rebalance_combines = false;
    o.fold_constants = false;
    o.remove_identities = false;
    o.drop_duplicates = false;
    o.share_identical = false;
    o.absorb_into_shared_leaves = false;
    return o;
}

// The transform fold, switched back on.
//
// It is OFF by default because it measured at 0.87x on the estate — see the table in
// `compile.hpp` and the one this file prints — but the rewrite itself is correct, and the cases
// below are what say so. A rewrite refused on cost still has to be a rewrite that works, or the
// flag is a trap for whoever turns it on.
CompileOptions with_fold() {
    CompileOptions o;
    o.fold_transforms = true;
    o.absorb_into_shared_leaves = true;
    return o;
}

// What two fields disagree by, over a set of points, and where.
struct Agreement {
    usize points = 0;
    usize near_surface = 0;      // |d| under a voxel in either field: where a disagreement moves matter
    usize sign_flips = 0;        // the only disagreement that can add or remove a voxel
    f64 worst = 0.0;             // largest |a - b| anywhere
    f64 worst_near_surface = 0.0;
    Vec3 worst_at{0, 0, 0};
};

void compare_at(const Field& fa, u32 ra, const Field& fb, u32 rb, Vec3 p, Agreement& out) {
    const f64 a = fa.eval(ra, p);
    const f64 b = fb.eval(rb, p);
    const f64 gap = std::abs(a - b);
    ++out.points;
    if (gap > out.worst) { out.worst = gap; out.worst_at = p; }
    if ((a < 0.0) != (b < 0.0)) ++out.sign_flips;
    if (std::abs(a) < kVoxel || std::abs(b) < kVoxel) {
        ++out.near_surface;
        if (gap > out.worst_near_surface) out.worst_near_surface = gap;
    }
}

// A grid over a box, offset off the lattice so that no point lands exactly on a plane the clip was
// drawn on. A building is axis-aligned and a grid over it is too, so an unshifted grid asks about
// the one set of points where every face, every join and every corner coincides — which is both the
// least representative sample and the one where a tie in a `min` can go either way for reasons that
// have nothing to do with the rewrite.
void grid(Vec3 low, Vec3 high, u32 steps, const Field& fa, u32 ra, const Field& fb, u32 rb,
          Agreement& out) {
    const Vec3 span = high - low;
    for (u32 i = 0; i < steps; ++i) {
        for (u32 j = 0; j < steps; ++j) {
            for (u32 k = 0; k < steps; ++k) {
                const f64 u = (static_cast<f64>(i) + 0.31731) / static_cast<f64>(steps);
                const f64 v = (static_cast<f64>(j) + 0.57113) / static_cast<f64>(steps);
                const f64 w = (static_cast<f64>(k) + 0.11939) / static_cast<f64>(steps);
                compare_at(fa, ra, fb, rb, Vec3{low.x + span.x * u, low.y + span.y * v,
                                                low.z + span.z * w}, out);
            }
        }
    }
}

std::string estate_path() { return std::string(WS_ASSET_SOURCE_DIR) + "/../clips/facility.clip"; }

struct Loaded {
    VoxelTypeTable types;
    TagRegistry tags;
    Script script;
    Loaded() { script = load_clip_script(estate_path(), types, tags); }
};

}  // namespace

// --------------------------------------------------------------------------------------
// The rewrites, one at a time, on fields small enough to reason about by hand.
// --------------------------------------------------------------------------------------

TEST_CASE("a translate over a box is a box somewhere else") {
    Field f;
    const u32 b = f.box({0, 0, 0}, {1, 1, 1});
    const u32 t = f.translate(b, {5, 0, 0});

    CompileReport rep;
    const Field c = compile_field(f, t, &rep, with_fold());

    REQUIRE(rep.ok);
    CHECK(rep.absorbed_by_centre == 1);
    CHECK(rep.nodes_before == 2);
    CHECK(rep.nodes_after == 1);          // the translate is gone, not moved
    CHECK(rep.depth_before == 2);
    CHECK(rep.depth_after == 1);
    CHECK(c.node(rep.root).op == forge::Op::Box);

    for (f64 x = -2.0; x < 9.0; x += 0.37) {
        CHECK(c.eval(rep.root, Vec3{x, 0.13, -0.21}) ==
              doctest::Approx(f.eval(t, Vec3{x, 0.13, -0.21})).epsilon(1e-12));
    }
}

TEST_CASE("a chain of translates is one vector") {
    Field f;
    const u32 s = f.sphere({1, 0, 0}, 0.5);
    const u32 a = f.translate(s, {1, 0, 0});
    const u32 b = f.translate(a, {0, 2, 0});
    const u32 c = f.translate(b, {0, 0, 3});

    CompileReport rep;
    const Field out = compile_field(f, c, &rep, with_fold());
    REQUIRE(rep.ok);
    CHECK(rep.chains_merged == 2);
    CHECK(rep.translates_before == 3);
    CHECK(rep.translates_after == 0);
    CHECK(rep.nodes_after == 1);
    CHECK(out.eval(rep.root, Vec3{2, 2, 3}) == doctest::Approx(f.eval(c, Vec3{2, 2, 3})));
}

TEST_CASE("a translate over a union reaches every leaf of it") {
    // The estate's own shape: one translate over an assembly. It must not survive as a node and it
    // must not be copied once per leaf either.
    Field f;
    const u32 a = f.box({0, 0, 0}, {1, 1, 1});
    const u32 b = f.sphere({3, 0, 0}, 1.0);
    const u32 c = f.cylinder({-3, 0, 0}, 0.5, 2.0, 1);
    const u32 u = f.unite({a, b, c});
    const u32 t = f.translate(u, {0, 10, 0});

    CompileReport rep;
    const Field out = compile_field(f, t, &rep, with_fold());
    REQUIRE(rep.ok);
    CHECK(rep.translates_after == 0);
    CHECK(rep.absorbed_by_centre == 3);
    CHECK(rep.nodes_after == 4);            // three primitives and the union: the translate is gone
    for (f64 y = 8.0; y < 13.0; y += 0.29) {
        CHECK(out.eval(rep.root, Vec3{0.4, y, 0.1}) ==
              doctest::Approx(f.eval(t, Vec3{0.4, y, 0.1})).epsilon(1e-12));
    }
}

TEST_CASE("a shape used twice under two placements is not copied") {
    // The refusal that keeps this pass honest. `column` has two parents, so specialising it for
    // either would duplicate the whole subtree — and a compiler that doubles a clip is the one
    // failure nobody sees in a screenshot.
    Field f;
    const u32 shaft = f.cylinder({0, 0, 0}, 0.2, 2.0, 1);
    const u32 cap = f.box({0, 2.0, 0}, {0.3, 0.1, 0.3});
    const u32 column = f.unite({shaft, cap});
    const u32 left = f.translate(column, {-2, 0, 0});
    const u32 right = f.translate(column, {2, 0, 0});
    const u32 pair = f.unite({left, right});

    CompileReport rep;
    const Field out = compile_field(f, pair, &rep, with_fold());
    REQUIRE(rep.ok);
    CHECK(rep.refused_shared == 2);
    CHECK(rep.translates_after == 2);       // both had to stay
    CHECK(rep.nodes_after <= rep.nodes_before);
    for (f64 x = -3.0; x < 3.0; x += 0.17) {
        CHECK(out.eval(rep.root, Vec3{x, 1.0, 0.05}) ==
              doctest::Approx(f.eval(pair, Vec3{x, 1.0, 0.05})).epsilon(1e-12));
    }
}

TEST_CASE("a translate carries through a rotate and a scale") {
    Field f;
    const u32 b = f.box({0, 0, 0}, {1, 0.5, 0.25});
    const u32 r = f.rotate(b, {0.05, 0.125, 0.3});
    const u32 s = f.scale(r, {2.0, 1.0, 0.5});
    const u32 t = f.translate(s, {3, -1, 2});

    CompileReport rep;
    const Field out = compile_field(f, t, &rep, with_fold());
    REQUIRE(rep.ok);
    CHECK(rep.translates_after == 0);
    CHECK(rep.pushed_through_transform == 2);
    for (f64 x = -1.0; x < 7.0; x += 0.23) {
        for (f64 y = -3.0; y < 2.0; y += 0.41) {
            const Vec3 p{x, y, 1.7};
            CHECK(out.eval(rep.root, p) == doctest::Approx(f.eval(t, p)).epsilon(1e-11));
        }
    }
}

TEST_CASE("a translate cannot cross a mirror it is not parallel to") {
    Field f;
    const u32 b = f.box({2, 0, 0}, {0.5, 0.5, 0.5});
    const u32 m = f.mirror(b, 0);
    const u32 across = f.translate(m, {1, 0, 0});    // on the fold axis: must stay
    const u32 along = f.translate(m, {0, 1, 0});     // in the fold plane: may cross

    CompileReport blocked;
    const Field kept = compile_field(f, across, &blocked, with_fold());
    CHECK(blocked.translates_after == 1);
    CompileReport crossed;
    const Field gone = compile_field(f, along, &crossed, with_fold());
    CHECK(crossed.translates_after == 0);
    CHECK(crossed.pushed_through_transform == 1);

    for (f64 x = -4.0; x < 4.0; x += 0.13) {
        const Vec3 p{x, 0.7, 0.1};
        CHECK(kept.eval(blocked.root, p) == doctest::Approx(f.eval(across, p)).epsilon(1e-12));
        CHECK(gone.eval(crossed.root, p) == doctest::Approx(f.eval(along, p)).epsilon(1e-12));
    }
}

TEST_CASE("a pattern under a translate keeps its translate rather than losing its grain") {
    // The one place a push must refuse on cost: a noise has no centre to absorb anything, so the
    // shift has to stay as a node. What must NOT happen is the shift being dropped.
    Field f;
    const u32 n = f.noise(0.5, 7);
    const u32 t = f.translate(n, {1.25, 0, 0});

    CompileReport rep;
    const Field out = compile_field(f, t, &rep, with_fold());
    REQUIRE(rep.ok);
    CHECK(rep.translates_after == 1);
    CHECK(rep.materialised == 1);
    for (f64 x = -2.0; x < 2.0; x += 0.07) {
        CHECK(out.eval(rep.root, Vec3{x, 0.3, 0.2}) ==
              doctest::Approx(f.eval(t, Vec3{x, 0.3, 0.2})).epsilon(1e-12));
    }
}

TEST_CASE("a nested union is one union") {
    Field f;
    const u32 a = f.sphere({0, 0, 0}, 1.0);
    const u32 b = f.sphere({2, 0, 0}, 1.0);
    const u32 inner = f.unite({a, b});
    const u32 c = f.sphere({4, 0, 0}, 1.0);
    const u32 outer = f.unite({inner, c});

    CompileReport rep;
    const Field out = compile_field(f, outer, &rep);
    REQUIRE(rep.ok);
    CHECK(rep.combines_flattened == 1);
    CHECK(rep.nodes_after == 4);           // three spheres and one union
    CHECK(rep.depth_after == 2);
    CHECK(out.node(rep.root).children == 3);
}

TEST_CASE("a wide union is a tree and not a list") {
    // `Field::combine` chains anything over four parts left-deep, and child 0 of a chain link is
    // the whole rest of the chain — a box no point is ever outside, so the cull can never reject
    // it. Twenty parts is seven frames that are always descended.
    Field f;
    std::vector<u32> parts;
    for (u32 i = 0; i < 20; ++i) parts.push_back(f.sphere({static_cast<f64>(i) * 3.0, 0, 0}, 1.0));
    const u32 u = f.unite(parts);

    const usize chain_depth = expression_depth(f, u);
    CHECK(chain_depth == 8);               // seven links plus the leaf

    CompileReport rep;
    const Field out = compile_field(f, u, &rep);
    REQUIRE(rep.ok);
    CHECK(rep.combines_rebalanced == 1);
    CHECK(rep.depth_after < chain_depth);

    for (f64 x = -2.0; x < 60.0; x += 0.31) {
        // Bit-exact, and not merely close: min is exactly associative and commutative.
        CHECK(out.eval(rep.root, Vec3{x, 0.2, 0.1}) == f.eval(u, Vec3{x, 0.2, 0.1}));
    }
}

TEST_CASE("an expression with no point in it is a number") {
    Field f;
    const u32 lump = f.add({f.constant(0.02), f.negate(f.constant(0.005))});
    const u32 b = f.box({0, 0, 0}, {1, 1, 1});
    const u32 shape = f.offset(b, 0.0);          // an identity, and it must go
    const u32 mixed = f.minimum({shape, lump});

    CompileReport rep;
    const Field out = compile_field(f, mixed, &rep);
    REQUIRE(rep.ok);
    CHECK(rep.constant_subtrees_folded == 1);
    CHECK(rep.identities_removed >= 1);
    CHECK(out.eval(rep.root, Vec3{0.5, 0, 0}) == doctest::Approx(f.eval(mixed, Vec3{0.5, 0, 0})));
    CHECK(out.eval(rep.root, Vec3{9, 9, 9}) == doctest::Approx(f.eval(mixed, Vec3{9, 9, 9})));
}

TEST_CASE("a dial still moves the shape it moved before") {
    Field f;
    const u32 r = f.parameter("radius", 1.5);
    // A parameter cannot be folded to a number, however constant it looks standing still.
    const u32 shape = f.offset(f.sphere({0, 0, 0}, 1.0), 0.0);
    const u32 both = f.minimum({shape, r});

    CompileReport rep;
    Field out = compile_field(f, both, &rep);
    REQUIRE(rep.ok);
    REQUIRE(out.parameter_count() == 1);
    CHECK(std::string(out.parameter_name(0)) == "radius");
    CHECK(out.set_parameter("radius", -4.0));
    CHECK(out.eval(rep.root, Vec3{10, 10, 10}) == doctest::Approx(-4.0));
}

TEST_CASE("the compiler refuses rather than guessing") {
    // Nothing in the enum is unhandled today, so this asserts the shape of the refusal rather than
    // producing one: a field it cannot see the root of comes back as itself, not as nought.
    Field f;
    const u32 b = f.box({0, 0, 0}, {1, 1, 1});
    CompileReport rep;
    const Field out = compile_field(f, b + 99, &rep);
    CHECK_FALSE(rep.ok);
    CHECK(out.size() == f.size());
}

// --------------------------------------------------------------------------------------
// The estate. The numbers, and the gate.
// --------------------------------------------------------------------------------------

TEST_CASE("the estate compiles to the same shape") {
    Loaded loaded;
    REQUIRE_MESSAGE(loaded.script.errors.empty(), "clips/facility.clip did not parse");
    REQUIRE(loaded.script.has_solid);

    const Field& original = loaded.script.field;
    const u32 root = loaded.script.solid;

    // The control arm first, and it is a real arm: the same walk with every rewrite switched off.
    // If this one is not a faithful copy then none of the numbers below mean anything.
    CompileReport off;
    Field plain = compile_field(original, root, &off, all_off());
    REQUIRE(off.ok);
    CHECK(off.nodes_after <= off.nodes_before + original.parameter_count());
    CHECK(off.depth_after == off.depth_before);

    CompileReport on;
    Field built = compile_field(original, root, &on);
    REQUIRE(on.ok);

    // The fold's own numbers, from the arm that is off by default. Printed because "0 absorbed"
    // beside a switch nobody can see reads as a rewrite that does not work, and this one works
    // and is refused on cost — which is a different fact and the more useful one. D682's lesson,
    // exactly: `accelerator_count()` read nought on every clip ever built and nothing said whether
    // that was a decision or a result.
    CompileReport folded;
    compile_field(original, root, &folded, with_fold());

    std::printf(
        "\n--- compile_field on clips/facility.clip -------------------------------------\n"
        "  nodes            %zu reachable of %zu  ->  %zu   (%.1f%% of the walk removed)\n"
        "  deepest path     %zu  ->  %zu\n"
        "  translates       %zu  ->  %zu\n"
        "  control arm      %zu nodes, depth %zu  (every rewrite off)\n"
        "\n  the transform fold, OFF by default -- it measures 0.87x. Its arm's counters:\n"
        "  absorbed into a centre        %zu\n"
        "  absorbed into a plane         %zu\n"
        "  absorbed into a capsule       %zu\n"
        "  translate chains merged       %zu\n"
        "  pushed through a transform    %zu\n"
        "  distributed over an op        %zu\n"
        "  materialised (had to stay)    %zu\n"
        "  refused: shared subtree       %zu\n"
        "  refused: would have cost more %zu\n"
        "  its nodes / depth             %zu / %zu\n"
        "\n  combines flattened            %zu\n"
        "  combines re-balanced          %zu\n"
        "  duplicate children dropped    %zu\n"
        "  constant subtrees folded      %zu  (%zu nodes)\n"
        "  identities removed            %zu\n"
        "  shared by CSE                 %zu\n"
        "  parameter slots kept          %zu\n",
        on.nodes_before, on.nodes_in_input, on.nodes_after,
        100.0 * (1.0 - static_cast<f64>(on.nodes_after) / static_cast<f64>(on.nodes_before)),
        on.depth_before, on.depth_after, on.translates_before, on.translates_after,
        off.nodes_after, off.depth_after,
        folded.absorbed_by_centre, folded.absorbed_by_plane, folded.absorbed_by_capsule,
        folded.chains_merged, folded.pushed_through_transform, folded.pushed_through_pointwise,
        folded.materialised, folded.refused_shared, folded.refused_costly,
        folded.nodes_after, folded.depth_after,
        on.combines_flattened, on.combines_rebalanced, on.duplicate_children_dropped,
        on.constant_subtrees_folded, on.constant_nodes_removed, on.identities_removed,
        on.shared_by_cse, on.parameter_nodes_kept);

    // It must never grow. Every rewrite is proved non-increasing before it fires and this is the
    // assertion that says so on a real clip rather than in a comment.
    CHECK(on.nodes_after <= on.nodes_before + original.parameter_count());
    CHECK(on.depth_after <= on.depth_before);

    const Vec3 low = loaded.script.settings.low;
    const Vec3 high = loaded.script.settings.high;
    const Vec3 mid{(low.x + high.x) * 0.5, (low.y + high.y) * 0.5, (low.z + high.z) * 0.5};

    // Four arms, because ONE comparison cannot tell an arithmetic fault from a culling one, and
    // this pass changes both. `plain` and `built` have had no bounds built, so they cull nothing
    // and answer the field's plain arithmetic; `plain_cull` and `built_cull` are the same two
    // expressions with `build_bounds` called, which is the configuration the sampler runs.
    Field plain_cull = compile_field(original, root, nullptr, all_off());
    Field built_cull = compile_field(original, root, nullptr);
    plain_cull.build_bounds();
    built_cull.build_bounds();

    const auto sweep = [&](const Field& fa, u32 ra, const Field& fb, u32 rb) {
        Agreement out;
        grid(low, high, 26, fa, ra, fb, rb, out);
        grid({mid.x - 12, low.y, mid.z - 12}, {mid.x + 12, low.y + 24, mid.z + 12}, 22,
             fa, ra, fb, rb, out);
        return out;
    };
    const auto say = [&](const char* what, const Agreement& g) {
        std::printf("    %-44s worst %.3e m (%.2e voxel), near a surface %.3e m, %zu flips\n",
                    what, g.worst, g.worst / kVoxel, g.worst_near_surface, g.sign_flips);
    };

    const Agreement pure = sweep(plain, off.root, built, on.root);
    const Agreement orig_cull = sweep(plain, off.root, original, root);
    const Agreement new_cull = sweep(built, on.root, built_cull, on.root);
    const Agreement shipped = sweep(original, root, built_cull, on.root);

    std::printf("\n  agreement over %zu points, %zu of them within a voxel of a surface\n",
                pure.points, pure.near_surface);
    say("the rewrite, arithmetic only, no culling", pure);
    say("the ORIGINAL field's cull, against no cull", orig_cull);
    say("the COMPILED field's cull, against no cull", new_cull);
    say("shipped against shipped, both culling", shipped);
    std::printf("    worst shipped deviation at   %.4f %.4f %.4f\n"
                "  unbounded nodes  %zu of %zu  ->  %zu of %zu\n"
                "---------------------------------------------------------------------------\n\n",
                shipped.worst_at.x, shipped.worst_at.y, shipped.worst_at.z,
                original.unbounded_nodes(), original.size(),
                built_cull.unbounded_nodes(), built_cull.size());

    // THE GATE. A nanometre, and the arithmetic arm is the one held to it, because it is the only
    // arm that is this pass's own doing. The only rewrite that is not bit-exact is the
    // re-association of a subtraction — `p - (c + t)` where the original wrote `(p - t) - c` — plus
    // the round trip through the four builders that normalise a turn or a unit normal. Both are
    // ulps: at 100 m from the origin an ulp of a double is 1.4e-14 m, which is 4.5e-13 of a
    // 3.125 cm voxel and eight orders under the 0.47 microns of f32 error D676 already accepted
    // for the whole field.
    CHECK(pure.worst < 1e-9);
    CHECK(pure.sign_flips == 0);

    // And the two culling arms are the control that says whose the rest is. The cull in
    // `Field::eval` skips a child whose BOX is further from the point than the running answer,
    // which is only sound when a child answers at least the distance to its own box — and D644
    // measured four primitives that do not (ellipsoid 0.5877, cone 0.5300, prism 0.8660, platonic
    // 0.5774), with its own note that "the cull still reads these boxes". So the ORIGINAL field
    // already disagrees with its own arithmetic, and by how much is what `orig_cull` measures.
    // The compiled field disagrees with its own for the same reason and a little more often,
    // because flattening and re-balancing make a union's internal boxes tighter, which culls more.
    //
    // Both are far from any surface, and `worst_near_surface` is the number that decides whether a
    // voxel moves.
    CHECK(new_cull.worst <= orig_cull.worst * 4.0 + 1e-9);
    CHECK(shipped.worst_near_surface < 0.1 * kVoxel);
    CHECK(shipped.sign_flips == 0);

    // And `build_bounds` still works on the output, which is not free: a compiled node with no box
    // can never be culled, and 15% of the estate is already boxless.
    CHECK(built_cull.bounds_of(on.root).infinite() == original.bounds_of(root).infinite());
    const f64 was = static_cast<f64>(original.unbounded_nodes()) /
                    static_cast<f64>(original.size());
    const f64 now = static_cast<f64>(built_cull.unbounded_nodes()) /
                    static_cast<f64>(built_cull.size());
    CHECK(now <= was + 0.02);

    // --- what each rewrite is worth on its own ------------------------------------------
    //
    // Leave-one-out, because a pass measured whole cannot say which of seven ideas paid for
    // itself. D683's ledger is seven levers measured one at a time and it is the most useful
    // thing in that entry; this is the same discipline applied inside one change, so a rewrite
    // that is worth nothing here can be dropped by whoever reads this rather than carried.
    std::printf("  what each rewrite is worth, by taking it away\n"
                "    %-34s %6s %6s\n", "everything except...", "nodes", "depth");
    struct Arm { const char* name; bool CompileOptions::*flag; };
    const Arm arms[] = {
        {"folding transforms",        &CompileOptions::fold_transforms},
        {"absorbing into shared leaves", &CompileOptions::absorb_into_shared_leaves},
        {"flattening combines",       &CompileOptions::flatten_combines},
        {"re-balancing combines",     &CompileOptions::rebalance_combines},
        {"folding constants",         &CompileOptions::fold_constants},
        {"removing identities",       &CompileOptions::remove_identities},
        {"dropping duplicates",       &CompileOptions::drop_duplicates},
        {"sharing identical nodes",   &CompileOptions::share_identical},
    };
    for (const Arm& arm : arms) {
        CompileOptions without;
        without.*(arm.flag) = false;
        CompileReport r;
        const Field f = compile_field(original, root, &r, without);
        std::printf("    %-34s %6zu %6zu\n", arm.name, r.nodes_after, r.depth_after);
    }
    std::printf("    %-34s %6zu %6zu\n", "(everything)", on.nodes_after, on.depth_after);
    std::printf("    %-34s %6zu %6zu\n\n", "(nothing)", off.nodes_after, off.depth_after);

    // --- and what it costs to ASK, which is the thing node count is only a proxy for --------
    //
    // D682: filling one cell of the estate walks ~8,231 nodes and that walk is 76% of what loading
    // a world costs. A node count is not that walk — a node removed from a subtree the cull never
    // enters is worth nothing — so the two fields are timed over the same points, both culling,
    // three rounds interleaved so that neither arm gets the cold cache or the hot clock.
    //
    // Six to a side and no more, and that is not laziness. A point INSIDE the block is the
    // expensive one — D681 raised the turn cap to 4,194,304 because `occlusion` evaluates its
    // child fourteen times, `repeat` up to eight, and nested they multiply — so a few hundred
    // interior points is already seconds of walking, and a sixteen-cubed grid ran for ten minutes
    // without finishing. The agreement sweeps above are the dense measurement; this one is timing
    // and only needs to be repeatable.
    std::vector<Vec3> probes;
    for (u32 i = 0; i < 6; ++i) {
        for (u32 j = 0; j < 6; ++j) {
            for (u32 k = 0; k < 6; ++k) {
                probes.push_back(Vec3{-9.0 + 3.1531 * i, -2.0 + 2.9319 * j, -9.0 + 3.0937 * k});
            }
        }
    }
    CompileOptions no_balance;
    no_balance.rebalance_combines = false;
    CompileOptions folds_only = all_off();
    folds_only.fold_transforms = true;
    folds_only.absorb_into_shared_leaves = true;
    CompileOptions fold_no_copy = all_off();
    fold_no_copy.fold_transforms = true;
    CompileOptions shape_only = all_off();
    shape_only.flatten_combines = true;
    shape_only.rebalance_combines = true;
    CompileOptions no_fold;
    no_fold.fold_transforms = false;
    no_fold.absorb_into_shared_leaves = false;
    CompileReport rb, fo, ct, fn, sh, nf;
    Field no_fold_cull = compile_field(original, root, &nf, no_fold);
    no_fold_cull.build_bounds();
    Field fold_plain_cull = compile_field(original, root, &fn, fold_no_copy);
    Field shape_cull = compile_field(original, root, &sh, shape_only);
    fold_plain_cull.build_bounds();
    shape_cull.build_bounds();
    Field flat_cull = compile_field(original, root, &rb, no_balance);
    Field fold_cull = compile_field(original, root, &fo, folds_only);
    // THE CONTROL ARM, and without it none of the rows below mean anything. A compiled field is
    // not only a different expression, it is a different ARRAY: only the reachable nodes, in a
    // different order, in freshly allocated memory. That alone moves a walk that is 8,231 node
    // visits of pointer chasing. This arm is the same rebuild with every rewrite off, so the gap
    // between it and the original is layout, and the gap between it and the others is the work.
    Field control_cull = compile_field(original, root, &ct, all_off());
    flat_cull.build_bounds();
    fold_cull.build_bounds();
    control_cull.build_bounds();

    struct Timed { const char* name; const Field* f; u32 root; f64 best; usize nodes; };
    Timed timed[] = {
        {"original, as the clip parses",  &original,   root,     1e30, original.size()},
        {"rebuilt, every rewrite OFF",    &control_cull, ct.root, 1e30, control_cull.size()},
        {"compiled, everything",          &built_cull, on.root,  1e30, built_cull.size()},
        {"compiled, no re-balancing",     &flat_cull,  rb.root,  1e30, flat_cull.size()},
        {"compiled, the transform fold only", &fold_cull, fo.root, 1e30, fold_cull.size()},
        {"compiled, the fold, no leaf copies", &fold_plain_cull, fn.root, 1e30,
         fold_plain_cull.size()},
        {"compiled, flatten and re-balance only", &shape_cull, sh.root, 1e30, shape_cull.size()},
        {"compiled, everything but the fold", &no_fold_cull, nf.root, 1e30, no_fold_cull.size()},
    };
    f64 sink = 0.0;
    // Interleaved, all arms inside one loop: D407's rule, so no arm gets the cold cache or the
    // clock at a different temperature from another.
    for (u32 round = 0; round < 7; ++round) {
        for (Timed& t : timed) {
            const auto a = std::chrono::steady_clock::now();
            for (const Vec3& p : probes) sink += t.f->eval(t.root, p);
            const f64 ms = std::chrono::duration<f64, std::milli>(
                               std::chrono::steady_clock::now() - a).count();
            if (ms < t.best) t.best = ms;
        }
    }
    CHECK(sink != 12345.0);   // so nothing above is optimised away
    std::printf("  the cost of asking, %zu points inside the block, best of seven interleaved\n",
                probes.size());
    for (const Timed& t : timed) {
        std::printf("    %-36s %7.2f ms  (%6.2f us a point)  %.3fx   %zu nodes\n", t.name, t.best,
                    t.best * 1000.0 / static_cast<f64>(probes.size()), timed[0].best / t.best,
                    t.nodes);
    }
    std::printf("---------------------------------------------------------------------------\n\n");
}
