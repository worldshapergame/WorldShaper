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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "forge/clip_script.hpp"
#include "forge/compile.hpp"
#include "forge/field.hpp"
#include "forge/measure.hpp"
#include "forge/sample.hpp"
#include "game/clip.hpp"
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
    // Explicitly the UNCOMPILED clip, and the two lines are not decoration. The switch ships ON
    // since D695, so loading through the default would hand every case below a field that has
    // already been compiled and then compile it a second time — and the table this file prints
    // would silently be about what a SECOND pass is worth, which is nothing.
    Loaded() {
        compile_fields(false);
        script = load_clip_script(estate_path(), types, tags);
        compile_fields(true);
    }
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
    //
    // **This case used to SIGSEGV, and that is why the whole file was pulled out of `main`.** The
    // counters at the top of `compile_field` ran BEFORE the root was checked for range, and
    // `count_op` then indexed a one-entry `seen` array and a one-node field ninety-nine entries
    // past the end: garbage `children`, garbage indices pushed onto its stack, and a fault a few
    // frames later. doctest aborts the process on a crash, so the suite reported 493 SKIPPED and
    // one failure carrying ZERO failed assertions — a crashed case reads QUIETER in the summary
    // than one that merely fails.
    Field f;
    const u32 b = f.box({0, 0, 0}, {1, 1, 1});
    CompileReport rep;
    const Field out = compile_field(f, b + 99, &rep);
    CHECK_FALSE(rep.ok);
    CHECK(out.size() == f.size());

    // A refusal hands back something a caller can apply blindly: the roots it was given and an
    // identity remap. "I could not" must not need a special case at every call site to be safe.
    REQUIRE(rep.roots.size() == 1);
    CHECK(rep.roots[0] == b + 99);
    REQUIRE(rep.remap.size() == f.size());
    for (usize i = 0; i < f.size(); ++i) CHECK(rep.remap[i] == static_cast<u32>(i));

    // And the same refusal when the bad root is one of several, because half a remapped Script is
    // worse than none.
    CompileReport set;
    const Field kept = compile_field(f, std::vector<u32>{b, b + 99}, &set);
    CHECK_FALSE(set.ok);
    CHECK(kept.size() == f.size());
    REQUIRE(set.roots.size() == 2);
    CHECK(set.roots[0] == b);
}

TEST_CASE("a set of roots goes in and the same set comes out") {
    // The blocker this signature exists for. A `Script` names four kinds of node at once — the
    // solid, the sample region, and every paint rule's test and place — and a compilation renumbers
    // every one of them. Compiling from the solid alone leaves the other three aimed at whatever
    // now occupies their old index: a building painted from the wrong shapes, silently.
    Field f;
    const u32 a = f.box({0, 0, 0}, {1, 1, 1});
    const u32 b = f.sphere({2, 0, 0}, 1.0);
    const u32 c = f.cylinder({-2, 0, 0}, 0.5, 2.0, 1);
    const u32 solid = f.unite({a, b, c});
    const u32 region = f.box({0, 0, 0}, {8, 8, 8});
    const u32 rule = f.translate(b, {0, 3, 0});
    const std::vector<u32> roots{solid, region, rule, a};

    CompileReport rep;
    const Field out = compile_field(f, roots, &rep);
    REQUIRE(rep.ok);
    REQUIRE(rep.roots.size() == roots.size());
    CHECK(rep.root == rep.roots[0]);

    // Every root answers what it answered, and they are still four different questions.
    for (f64 x = -4.0; x < 5.0; x += 0.19) {
        for (f64 y = -1.0; y < 4.0; y += 0.37) {
            const Vec3 p{x, y, -0.11};
            for (usize r = 0; r < roots.size(); ++r) {
                CHECK(out.eval(rep.roots[r], p) ==
                      doctest::Approx(f.eval(roots[r], p)).epsilon(1e-12));
            }
        }
    }

    // One field, one copy of everything shared. `a` is both a root and a child of `solid`, and
    // `b` is a child of `solid` and the subject of `rule`; neither may be duplicated for either.
    CHECK(rep.nodes_after <= rep.nodes_before + f.parameter_count());

    // And what the caller's OTHER indices became, for the names a script keeps for its tools.
    REQUIRE(rep.remap.size() == f.size());
    CHECK(rep.remap[solid] == rep.roots[0]);
    CHECK(rep.remap[a] == rep.roots[3]);
    CHECK(rep.remap[b] != kUnmapped);
    for (f64 x = -4.0; x < 5.0; x += 0.29) {
        const Vec3 p{x, 0.2, 0.1};
        CHECK(out.eval(rep.remap[b], p) == doctest::Approx(f.eval(b, p)).epsilon(1e-12));
    }
}

TEST_CASE("a name is carried without becoming a reference") {
    // The whole design problem D690 left behind, on a field small enough to count by hand.
    //
    // `wing` is a union inside a union with ONE parent, so `gather` flattens it away and it has no
    // node of its own in the output. It is also a name the author bound and `--part wing` has to
    // keep working. Those two are in direct conflict the moment a name becomes a root: `refs[wing]`
    // goes to two, the flatten is refused, and the rewrite this pass exists for stops firing.
    Field f;
    const u32 a = f.sphere({0, 0, 0}, 1.0);
    const u32 b = f.sphere({2, 0, 0}, 1.0);
    const u32 wing = f.unite({a, b});
    const u32 c = f.sphere({4, 0, 0}, 1.0);
    const u32 whole = f.unite({wing, c});

    // What the building comes to with nobody asking about `wing` at all.
    CompileReport bare;
    const Field plain = compile_field(f, whole, &bare);
    REQUIRE(bare.ok);
    CHECK(bare.combines_flattened == 1);
    CHECK(bare.nodes_for_roots == 4);          // three spheres and one union
    CHECK(bare.remap[wing] == kUnmapped);      // it dissolved: `remap` alone cannot answer

    // As a NAME. The building is node-for-node what it was, and `wing` still answers.
    CompileReport named;
    const Field kept = compile_field(f, std::vector<u32>{whole}, std::vector<u32>{wing}, &named);
    REQUIRE(named.ok);
    CHECK(named.combines_flattened == bare.combines_flattened);
    CHECK(named.nodes_for_roots == bare.nodes_for_roots);
    CHECK(named.depth_after == bare.depth_after);
    CHECK(named.roots[0] == bare.roots[0]);
    REQUIRE(named.names.size() == 1);
    CHECK(named.names[0] != kUnmapped);
    CHECK(named.names_rebuilt == 1);           // it had dissolved, so it was built out of its parts
    CHECK(named.name_nodes_added == 1);        // and cost exactly the one union it is

    // As a ROOT, which is the move that would have worked and is the move that ruins it.
    CompileReport as_root;
    compile_field(f, std::vector<u32>{whole, wing}, &as_root);
    REQUIRE(as_root.ok);
    CHECK(as_root.combines_flattened == 0);    // the flatten is refused: `wing` now has two parents
    CHECK(as_root.nodes_for_roots > bare.nodes_for_roots);

    // And the answer is the shape the name meant, not something near it.
    for (f64 x = -3.0; x < 7.0; x += 0.11) {
        const Vec3 p{x, 0.13, -0.07};
        CHECK(kept.eval(named.names[0], p) == doctest::Approx(f.eval(wing, p)).epsilon(1e-12));
        CHECK(kept.eval(named.roots[0], p) == doctest::Approx(f.eval(whole, p)).epsilon(1e-12));
    }
}

TEST_CASE("a name over a translate nothing else references still answers") {
    // The estate's own shape, and the exact reason 0 of 5,091 survived. `apply_origin` moves the
    // clip by wrapping the solid, every paint rule AND every named part each in its own fresh
    // translate — and the parts' translates are reachable from nothing at all.
    Field f;
    const u32 shaft = f.cylinder({0, 0, 0}, 0.2, 2.0, 1);
    const u32 cap = f.box({0, 2.0, 0}, {0.3, 0.1, 0.3});
    const u32 column = f.unite({shaft, cap});
    const u32 whole = f.unite({column, f.sphere({3, 0, 0}, 1.0)});
    const Vec3 by{0, -3.5, 0};
    const u32 moved = f.translate(whole, by);
    const u32 named = f.translate(column, by);   // what `apply_origin` binds the name to

    CompileReport rep;
    const Field out =
        compile_field(f, std::vector<u32>{moved}, std::vector<u32>{named}, &rep);
    REQUIRE(rep.ok);
    REQUIRE(rep.names.size() == 1);
    CHECK(rep.names[0] != kUnmapped);
    for (f64 y = -6.0; y < 1.0; y += 0.07) {
        const Vec3 p{0.05, y, 0.03};
        CHECK(out.eval(rep.names[0], p) == doctest::Approx(f.eval(named, p)).epsilon(1e-12));
    }
}

TEST_CASE("a name with no answer is dropped and not guessed") {
    // The property D690 kept on purpose and this change does not soften: a name that cannot be
    // answered comes back as `kUnmapped`, never as node 0 — which is a perfectly good node and
    // would hand `--part` the first primitive of the clip instead of an error.
    Field f;
    const u32 b = f.box({0, 0, 0}, {1, 1, 1});
    const u32 s = f.sphere({4, 0, 0}, 1.0);

    CompileReport rep;
    const Field out = compile_field(f, std::vector<u32>{b}, std::vector<u32>{s, b + 99, b}, &rep);
    REQUIRE(rep.ok);
    REQUIRE(rep.names.size() == 3);
    CHECK(rep.names[0] != kUnmapped);              // reachable from nothing, and still carried
    CHECK(rep.names[1] == kUnmapped);              // out of range
    CHECK(rep.names[2] == rep.roots[0]);           // a name that is also a root is that root
    CHECK(rep.names_dropped_out_of_range == 1);
    CHECK(rep.names_kept == 2);
    for (f64 x = 0.0; x < 6.0; x += 0.13) {
        const Vec3 p{x, 0.2, 0.1};
        CHECK(out.eval(rep.names[0], p) == doctest::Approx(f.eval(s, p)).epsilon(1e-12));
    }

    // And a refusal hands the names back as themselves, like the roots and the remap, so "I could
    // not" needs no special case at the call site to be safe.
    CompileReport bad;
    compile_field(f, std::vector<u32>{b + 99}, std::vector<u32>{s}, &bad);
    CHECK_FALSE(bad.ok);
    REQUIRE(bad.names.size() == 1);
    CHECK(bad.names[0] == s);
}

TEST_CASE("a compiled script builds the same building") {
    // The wiring, end to end and both arms of one build. Four paint rules keyed on four different
    // shapes, one of them confined by `on=`, and a `region` that is not the solid — which is the
    // whole set of indices a compilation can scramble. The clip is sampled twice and the two
    // answers are compared by CONTENT HASH, because a rule pointing at the wrong shape after a
    // renumber produces a building of exactly the right size wearing the wrong materials.
    const char* text = R"(
metre 16
bounds -3 0 -3  3 4 3
material stone rgb=120,120,116
material moss  rgb=60,120,60
material lead  rgb=90,90,100
material lamp  rgb=255,240,200 emit=200
let base   = box 0 0.5 0  5 1 5
let post   = cylinder 0 2 0 r=0.4 h=2
let ball   = sphere 0 3.2 0 r=0.7
let all    = union { base post ball }
let damp   = sphere 0 0.6 0 r=1.6
let cap    = box 0 3.2 0  1.6 1.6 1.6
let hollow = box 0 4 0  8 3 8
paint stone
paint moss where=damp below=0
paint lead where=cap below=0 on=ball
paint lamp where=ball below=-0.2
region all
solid all
)";

    // One materials table for both arms, so a material carries the same id in each and the two
    // hashes are comparable at all.
    VoxelTypeTable types;
    TagRegistry tags;

    compile_fields(false);
    const Script plain = parse_clip_script(text, types, tags);
    compile_fields(true);
    const Script built = parse_clip_script(text, types, tags);
    compile_fields(true);    // BACK TO THE DEFAULT, which is on since D695. Left off, every
                             // clip every other case in the suite parses would quietly be the
                             // arm the game does not ship, in whatever order doctest runs them.

    REQUIRE(plain.errors.empty());
    REQUIRE(built.errors.empty());

    // The compilation has to have DONE something, or this case proves only that the switch is
    // wired to nothing. D682's lesson: a rewrite that fires nought times must say so in its own
    // line, or the next session measures the whole pass and concludes the idea is worthless.
    CHECK(built.field.size() < plain.field.size());

    const SampleResult a = sample(plain.field, plain.solid, plain.paint, plain.settings, nullptr);
    const SampleResult b = sample(built.field, built.solid, built.paint, built.settings, nullptr);
    CHECK(b.clip.content_hash() == a.clip.content_hash());

    // --- and `--part`, which is the whole reason this switch was off -------------------------
    //
    // Every name the file bound, sampled on its own on both arms and compared by CONTENT HASH.
    // `hollow` is in there deliberately: nothing builds with it, so it is a name over a subtree
    // the roots never reach, and it is the case that has to be REBUILT rather than looked up.
    REQUIRE(plain.parts.size() == 7);
    CHECK(built.parts.size() == plain.parts.size());
    for (const auto& entry : plain.parts) {
        u32 piece = 0;
        REQUIRE_MESSAGE(built.part(entry.first, piece), entry.first.c_str());
        const SampleResult was =
            sample(plain.field, entry.second, plain.paint, plain.settings, nullptr);
        const SampleResult now = sample(built.field, piece, built.paint, built.settings, nullptr);
        CHECK_MESSAGE(now.clip.content_hash() == was.clip.content_hash(), entry.first.c_str());
        CHECK(measure(now.clip).solid == measure(was.clip).solid);
    }
}

TEST_CASE("a compiled script keeps the shape the variation is scaled by") {
    // The FIFTH index a `Script` holds that BUILDS, and it was not in the root set until now.
    // `variation by=<shape>` asks a field how far inside that shape a voxel is and scales the
    // colour stray by it, so a renumber leaves it aimed at whatever occupies its old index — the
    // right building in the right materials with the grain coming from somewhere else. Nothing
    // caught it because `clips/facility.clip` has no `by=`.
    const char* text = R"(
metre 16
bounds -3 0 -3  3 4 3
material stone rgb=120,120,116
material moss  rgb=60,120,60
let damp  = sphere 0 0.6 0 r=1.6
let base  = box 0 0.5 0  5 1 5
let post  = cylinder 0 2 0 r=0.4 h=2
let all   = union { base post }
paint stone
paint moss where=damp below=0
variation colour=0.25 rough=0.25 seed=9 by=damp
solid all
)";
    // `damp` is declared FIRST and used LAST on purpose. The compilation emits what the solid needs
    // before it reaches `by`, so the sphere moves from index 0 to index 3 — without that the two
    // indices coincide, the case passes whether or not `variation.by` is re-pointed at all, and it
    // proves nothing. That was the first version of this case, and it passed with the bug in.
    VoxelTypeTable types;
    TagRegistry tags;

    compile_fields(false);
    const Script plain = parse_clip_script(text, types, tags);
    compile_fields(true);
    const Script built = parse_clip_script(text, types, tags);
    compile_fields(true);    // back to the default

    REQUIRE(plain.errors.empty());
    REQUIRE(built.errors.empty());
    REQUIRE(plain.variation.has_by);
    REQUIRE(built.variation.has_by);

    // The renumber has to have MOVED it, or this case proves nothing about the re-pointing.
    CHECK(built.variation.by != plain.variation.by);
    for (f64 x = -3.0; x < 3.0; x += 0.13) {
        const Vec3 p{x, 0.7, 0.2};
        CHECK(built.field.eval(built.variation.by, p) ==
              doctest::Approx(plain.field.eval(plain.variation.by, p)).epsilon(1e-12));
    }

    // And end to end: the variation is what mints the records, so a `by` pointing at the wrong
    // shape comes out as a different number of distinct types over the same voxels.
    SampleResult was = sample(plain.field, plain.solid, plain.paint, plain.settings, nullptr);
    SampleResult now = sample(built.field, built.solid, built.paint, built.settings, nullptr);
    REQUIRE(now.clip.content_hash() == was.clip.content_hash());
    VoxelTypeTable a_types = types, b_types = types;
    const VariationReport a =
        apply_variation(was.clip, a_types, plain.field, plain.variation, plain.settings, was,
                        nullptr);
    const VariationReport b =
        apply_variation(now.clip, b_types, built.field, built.variation, built.settings, now,
                        nullptr);
    CHECK(b.voxels == a.voxels);
    CHECK(b.distinct_types == a.distinct_types);
    CHECK(b.largest_group == a.largest_group);
    CHECK(now.clip.content_hash() == was.clip.content_hash());
}

// --------------------------------------------------------------------------------------
// The estate. The numbers, and the gate.
// --------------------------------------------------------------------------------------

TEST_CASE("the estate's names survive the compilation, and the building does not move") {
    // The one thing that kept `--compile-field` OFF. D690: `script.parts` survived only where a
    // name's node came through as itself, and on the estate that was **0 of 5,091**, because
    // `apply_origin` wraps every named part in its own fresh translate that nothing else
    // references. `--part` then answered "does not name anything" for every piece of the clip.
    //
    // Two halves, and the second is the one that makes the first worth having:
    //
    //   * every name answers the shape it named, over a dense grid in that name's own box;
    //   * the BUILDING is node-for-node, counter-for-counter what it is with no names in the call
    //     at all. That is the property adding the names as roots would have destroyed, and it is
    //     asserted here rather than argued: one flatten refused is one node of the walk kept.
    Loaded loaded;
    REQUIRE_MESSAGE(loaded.script.errors.empty(), "clips/facility.clip did not parse");
    REQUIRE(loaded.script.has_solid);
    const Field& original = loaded.script.field;

    // The root set `compile_whole_script` hands over: everything that BUILDS.
    std::vector<u32> roots;
    roots.push_back(loaded.script.solid);
    if (loaded.script.settings.has_bounds) roots.push_back(loaded.script.settings.bounds);
    for (const PaintRule& rule : loaded.script.paint) {
        roots.push_back(rule.test);
        if (rule.has_place) roots.push_back(rule.place);
    }
    // ...and the names, which are the other list and deliberately not roots.
    std::vector<u32> names;
    names.reserve(loaded.script.parts.size());
    for (const auto& entry : loaded.script.parts) names.push_back(entry.second);
    REQUIRE(names.size() > 1000);

    CompileReport bare;
    Field plain = compile_field(original, roots, &bare);
    REQUIRE(bare.ok);

    CompileReport with;
    Field built = compile_field(original, roots, names, &with);
    REQUIRE(with.ok);

    // THE PROPERTY. Not "about the same" — the same. The names took no part in `refs`, so every
    // decision the roots took was taken against the same counts, in the same order, at the same
    // node indices.
    REQUIRE(with.roots.size() == bare.roots.size());
    for (usize i = 0; i < with.roots.size(); ++i) CHECK(with.roots[i] == bare.roots[i]);
    CHECK(with.nodes_for_roots == bare.nodes_after);
    CHECK(with.depth_after == bare.depth_after);
    CHECK(with.translates_after == bare.translates_after);
    CHECK(with.combines_flattened == bare.combines_flattened);
    CHECK(with.combines_rebalanced == bare.combines_rebalanced);
    CHECK(with.constant_subtrees_folded == bare.constant_subtrees_folded);
    CHECK(with.identities_removed == bare.identities_removed);
    CHECK(with.duplicate_children_dropped == bare.duplicate_children_dropped);

    // And the promise the whole file is built on, with the names in: never bigger than the input.
    CHECK(with.nodes_after <= with.nodes_in_input);

    std::printf(
        "\n--- the names on clips/facility.clip ------------------------------------------\n"
        "  roots handed over            %zu\n"
        "  names handed over            %zu\n"
        "  names kept                   %zu   (%zu as themselves, %zu rebuilt)\n"
        "  names dropped                %zu   (%zu unhandled, %zu over budget, %zu out of range)\n"
        "  nodes: roots alone           %zu   (with no names in the call: %zu)\n"
        "  nodes: the whole field       %zu   of an input of %zu\n"
        "  the names cost               %zu nodes, and nought on the walk\n"
        "  deepest path                 %zu  ->  %zu\n"
        "-------------------------------------------------------------------------------\n",
        roots.size(), names.size(), with.names_kept, with.names_answered_as_themselves,
        with.names_rebuilt,
        names.size() - with.names_kept, with.names_dropped_unhandled,
        with.names_dropped_over_budget, with.names_dropped_out_of_range,
        with.nodes_for_roots, bare.nodes_after, with.nodes_after, with.nodes_in_input,
        with.name_nodes_added, with.depth_before, with.depth_after);

    // D690's own number was 0. Anything short of all of them is a finding and has to read as one.
    CHECK(with.names_kept == names.size());

    // --- and every name answers the shape it named ------------------------------------------
    //
    // Over the name's OWN box rather than the clip's, because a part is a few metres of a
    // 125 x 37 x 110 m site and a grid over the site would miss most of them entirely — which is
    // how a comparison comes back "identical" while saying nothing.
    Field built_cull = compile_field(original, roots, names, nullptr);
    built_cull.build_bounds();

    Agreement all;
    usize checked = 0, boxless = 0;
    for (usize i = 0; i < names.size(); ++i) {
        // Every 37th, which is a stride coprime with nothing in particular and walks the whole
        // list — the parts are declared building by building, so a prefix would be one building.
        if (i % 37 != 0) continue;
        const u32 a = names[i];
        const u32 b = with.names[i];
        REQUIRE(b != kUnmapped);
        const Field::Aabb box = original.bounds_of(a);
        if (box.infinite()) { ++boxless; continue; }
        const Vec3 pad{0.25, 0.25, 0.25};
        grid(box.low - pad, box.high + pad, 7, original, a, built_cull, b, all);
        ++checked;
    }
    std::printf("  %zu names sampled over their own boxes (%zu boxless, skipped), %zu points\n"
                "    worst %.3e m (%.2e voxel), near a surface %.3e m, %zu sign flips\n"
                "-------------------------------------------------------------------------------"
                "\n\n",
                checked, boxless, all.points, all.worst, all.worst / kVoxel,
                all.worst_near_surface, all.sign_flips);
    CHECK(checked > 50);
    CHECK(all.sign_flips == 0);
    CHECK(all.worst_near_surface < 0.1 * kVoxel);
}

TEST_CASE("--part answers the same voxels on both arms of the estate") {
    // The gate, in the terms the tool is used in: not "the field agrees" but "the same name cuts
    // the same voxels wearing the same materials". Content hash and voxel count, name by name,
    // across the whole parts list — which is the estate building by building, because the parts are
    // declared in include order.
    //
    // `run_clip_tool` cannot take this comparison itself: it samples the clip's own box, and since
    // D672 that box is 4016 x 1200 x 3536 = 17.0 billion cells, which is D693's silent exit. So each
    // name is sampled over ITS OWN box here, which is what somebody looking at a part wants anyway
    // and what `--clip-part` on the estate cannot currently give them.
    //
    // ONE materials table for both arms. A material interned in a different order carries a
    // different id, and two hashes over two id spaces are not a comparison at all — D692's own
    // fault, from the other end.
    VoxelTypeTable types;
    TagRegistry tags;

    compile_fields(false);
    const Script plain = load_clip_script(estate_path(), types, tags);
    compile_fields(true);
    const Script built = load_clip_script(estate_path(), types, tags);
    compile_fields(true);    // BACK TO THE DEFAULT, which is on since D695. Left off, every
                             // clip every other case in the suite parses would quietly be the
                             // arm the game does not ship, in whatever order doctest runs them.

    REQUIRE(plain.errors.empty());
    REQUIRE(built.errors.empty());
    REQUIRE(plain.parts.size() > 1000);
    CHECK(built.parts.size() == plain.parts.size());

    // Small enough to sample fifty times, fine enough that a part is more than a handful of cells.
    // Eight to the metre is 12.5 cm, and the box is capped about the part's own centre so that
    // `all` — which is the whole estate — costs what a doorway costs.
    //
    // The cap is what makes this affordable at all, and the reason is the paint: the estate carries
    // 685 rules and every solid voxel is offered every one of them, so the bill is voxels times
    // rules and not voxels. Three metres a side at 12.5 cm is 13,824 cells a name.
    const i32 metre = 8;
    const f64 kSpan = 1.5;   // ...a metre and a half either way of the centre, so three metres a side

    usize compared = 0, with_matter = 0, agreed = 0, skipped = 0;
    u64 total_voxels = 0;
    std::printf("\n--- --part on clips/facility.clip, both arms ----------------------------------"
                "\n    %-34s %10s  %s\n", "name", "voxels", "content hash");
    for (usize i = 0; i < plain.parts.size(); ++i) {
        // Every 149th, which walks the whole list and lands in a different building each time.
        if (i % 149 != 0) continue;
        const std::string& name = plain.parts[i].first;
        u32 piece = 0;
        REQUIRE_MESSAGE(built.part(name, piece), name.c_str());

        const Field::Aabb box = plain.field.bounds_of(plain.parts[i].second);
        if (box.infinite()) { ++skipped; continue; }
        const Vec3 mid{(box.low.x + box.high.x) * 0.5, (box.low.y + box.high.y) * 0.5,
                       (box.low.z + box.high.z) * 0.5};
        SampleSettings where = plain.settings;
        where.voxels_per_metre = metre;
        where.has_bounds = false;   // the part's own box IS the region here
        where.low = {std::max(box.low.x, mid.x - kSpan), std::max(box.low.y, mid.y - kSpan),
                     std::max(box.low.z, mid.z - kSpan)};
        where.high = {std::min(box.high.x, mid.x + kSpan), std::min(box.high.y, mid.y + kSpan),
                      std::min(box.high.z, mid.z + kSpan)};
        if (where.high.x <= where.low.x || where.high.y <= where.low.y ||
            where.high.z <= where.low.z) { ++skipped; continue; }
        SampleSettings same = where;

        const SampleResult was =
            sample(plain.field, plain.parts[i].second, plain.paint, where, nullptr);
        const SampleResult now = sample(built.field, piece, built.paint, same, nullptr);
        const u64 a = measure(was.clip, metre).solid;
        const u64 b = measure(now.clip, metre).solid;
        const bool same_hash = now.clip.content_hash() == was.clip.content_hash();
        std::printf("    %-34s %10llu  %016llx%s\n", name.c_str(),
                    static_cast<unsigned long long>(a),
                    static_cast<unsigned long long>(was.clip.content_hash()),
                    (same_hash && a == b) ? "" : "   <-- DIFFERS");
        std::fflush(stdout);   // so a slow run says which name it is on rather than nothing
        ++compared;
        if (a > 0) { ++with_matter; total_voxels += a; }
        if (same_hash && a == b) ++agreed;
        CHECK_MESSAGE(same_hash, name.c_str());
        CHECK_MESSAGE(a == b, name.c_str());
    }
    std::printf("    %zu names compared, %zu agreed, %zu of them with matter in "
                "(%llu voxels), %zu skipped for want of a box\n"
                "-------------------------------------------------------------------------------"
                "\n\n",
                compared, agreed, with_matter,
                static_cast<unsigned long long>(total_voxels), skipped);
    // Twenty names is the gate, and a name that comes back EMPTY on both arms agrees about
    // nothing. Both are asserted, so a comparison cannot pass by measuring air.
    CHECK(compared >= 20);
    CHECK(with_matter >= 20);
    CHECK(agreed == compared);
}

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
