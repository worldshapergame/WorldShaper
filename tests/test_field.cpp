// The field language every clip is made of.
//
// These are the tests that make the rest trustworthy. A clip is authored by writing an
// expression and looking at what comes out, and "looking at what comes out" only tells you
// something if the primitives underneath are right — a sphere that is quietly a tenth too small
// produces a room that is quietly wrong everywhere, and no screenshot shows it.
//
// So each shape is checked at the places its definition pins down: the centre, the surface, and
// a point outside at a known distance. A signed distance field says more than "inside or out",
// and the extra is exactly what displacement and rounding rely on, so it is worth asserting.

#include <doctest/doctest.h>

#include <cmath>
#include <string>

#include "forge/clip_script.hpp"
#include "forge/field.hpp"
#include "world/tags.hpp"
#include "world/voxel_type.hpp"

using namespace ws;
using namespace ws::forge;

namespace {

constexpr f64 kLoose = 1e-6;

// A shape's distance at a point, for brevity in the checks below.
f64 at(const Field& f, u32 node, f64 x, f64 y, f64 z) {
    return f.eval(node, Vec3{x, y, z});
}

// The mouldings are compositions of the operations above rather than nodes of their own, so they
// are checked here with the rest of the vocabulary — but they are written in the clip language,
// which means going through the parser to get at them.
struct Sections {
    VoxelTypeTable types;
    TagRegistry tags;
    Script script;

    explicit Sections(const std::string& body) {
        script = parse_clip_script("material stone rgb=1,2,3\n" + body + "\npaint stone\n", types,
                                   tags);
    }
    u32 shape(const char* name) const {
        u32 node = 0;
        REQUIRE(script.part(name, node));
        return node;
    }
    // Is the section solid at this point of its own plane? The run is z, so z of zero is the
    // middle of it.
    bool solid_at(const char* name, f64 across, f64 up) const {
        return script.field.eval(shape(name), Vec3{across, up, 0.0}) < 0.0;
    }
};

}  // namespace

TEST_CASE("a sphere measures its own radius") {
    Field f;
    const u32 s = f.sphere({0, 0, 0}, 2.0);
    CHECK(at(f, s, 0, 0, 0) == doctest::Approx(-2.0));   // the centre is two metres in
    CHECK(at(f, s, 2, 0, 0) == doctest::Approx(0.0));    // the surface is the surface
    CHECK(at(f, s, 5, 0, 0) == doctest::Approx(3.0));    // and outside is a real distance
}

TEST_CASE("a box is a distance, not merely a yes or no") {
    Field f;
    const u32 b = f.box({0, 0, 0}, {1, 1, 1});
    CHECK(at(f, b, 0, 0, 0) == doctest::Approx(-1.0));
    CHECK(at(f, b, 1, 0, 0) == doctest::Approx(0.0));
    CHECK(at(f, b, 3, 0, 0) == doctest::Approx(2.0));
    // Diagonally outside a corner the answer is the corner distance, which is what tells a
    // rounding operation how much to cut away.
    CHECK(at(f, b, 2, 2, 2) == doctest::Approx(std::sqrt(3.0)));
}

TEST_CASE("rounding a box pulls its surface in by the radius it adds") {
    // A rounded box the same size as a sharp one must not be bigger: the corner radius is taken
    // out of the half extent and given back as the round, so the overall size is what was asked
    // for. Getting this backwards makes every rounded thing in a clip slightly too large.
    Field f;
    const u32 sharp = f.box({0, 0, 0}, {1, 1, 1});
    const u32 round = f.box({0, 0, 0}, {1, 1, 1}, 0.25);
    CHECK(at(f, sharp, 1, 0, 0) == doctest::Approx(0.0));
    CHECK(at(f, round, 1, 0, 0) == doctest::Approx(0.0));   // face still where it was
    // The corner is inset, so a point on the old corner is now outside.
    CHECK(at(f, round, 1, 1, 1) > 0.0);
}

TEST_CASE("a cylinder is round across its axis and flat along it") {
    Field f;
    const u32 c = f.cylinder({0, 0, 0}, 1.0, 2.0, 1);   // upright
    CHECK(at(f, c, 0, 0, 0) == doctest::Approx(-1.0));
    CHECK(at(f, c, 1, 0, 0) == doctest::Approx(0.0));
    CHECK(at(f, c, 0, 2, 0) == doctest::Approx(0.0));
    CHECK(at(f, c, 0, 4, 0) == doctest::Approx(2.0));
    // Round across: the diagonal is the same distance as the axis, which a box would fail.
    CHECK(at(f, c, std::sqrt(0.5), 0, std::sqrt(0.5)) == doctest::Approx(0.0));
}

TEST_CASE("a torus has a hole in it") {
    Field f;
    const u32 t = f.torus({0, 0, 0}, 2.0, 0.5, 1);
    CHECK(at(f, t, 2, 0, 0) == doctest::Approx(-0.5));   // in the tube
    CHECK(at(f, t, 0, 0, 0) > 0.0);                      // the hole is not the shape
    CHECK(at(f, t, 2.5, 0, 0) == doctest::Approx(0.0));
}

TEST_CASE("a plane divides space") {
    Field f;
    const u32 p = f.plane({0, 1, 0}, 3.0);
    CHECK(at(f, p, 0, 0, 0) == doctest::Approx(-3.0));
    CHECK(at(f, p, 0, 3, 0) == doctest::Approx(0.0));
    CHECK(at(f, p, 0, 4, 0) == doctest::Approx(1.0));
    // A normal that is not a unit vector is normalised on the way in, so the result stays a
    // distance. Without that, every plane written casually would scale the field around it.
    const u32 q = f.plane({0, 5, 0}, 3.0);
    CHECK(at(f, q, 0, 4, 0) == doctest::Approx(1.0));
}

TEST_CASE("a prism has the number of sides it was asked for") {
    Field f;
    // A hexagon of circumradius one: its corners are one metre out, its faces cos(30) = 0.866.
    const u32 hex = f.prism({0, 0, 0}, 1.0, 1.0, 6, 1, 0.0);
    CHECK(at(f, hex, 0, 0, 0) == doctest::Approx(-std::cos(3.14159265358979 / 6.0)));
    // A corner sits at angle pi/6 between two face normals, at the full circumradius.
    const f64 corner_angle = 3.14159265358979 / 6.0;
    CHECK(at(f, hex, std::cos(corner_angle), 0, std::sin(corner_angle)) ==
          doctest::Approx(0.0).epsilon(0.02));
    // A face centre sits at the apothem.
    CHECK(at(f, hex, std::cos(3.14159265358979 / 6.0) * 0.0 + 0.866025, 0, 0) ==
          doctest::Approx(0.0).epsilon(0.02));
}

TEST_CASE("a triangle is a prism with three sides, not a special case") {
    Field f;
    const u32 tri = f.prism({0, 0, 0}, 1.0, 1.0, 3, 1, 0.0);
    CHECK(at(f, tri, 0, 0, 0) < 0.0);
    CHECK(at(f, tri, 2, 0, 0) > 0.0);
    // Its inradius is half its circumradius, which is what makes it a triangle rather than
    // some other three-sided thing.
    CHECK(at(f, tri, 0.5, 0, 0) == doctest::Approx(0.0).epsilon(0.02));
}

TEST_CASE("the platonic solids contain their centre and exclude their circumradius") {
    Field f;
    for (u32 which = 0; which < 5; ++which) {
        const u32 s = f.platonic({0, 0, 0}, 1.0, which);
        CHECK(at(f, s, 0, 0, 0) < 0.0);
        // Nothing reaches beyond the circumradius in any direction.
        CHECK(at(f, s, 1.5, 0, 0) > 0.0);
        CHECK(at(f, s, 0, 1.5, 0) > 0.0);
        CHECK(at(f, s, 0, 0, 1.5) > 0.0);
    }
}

TEST_CASE("a cube built as a platonic solid agrees with a box of the same size") {
    // The same solid two ways, which is the check that the circumradius convention is right:
    // a cube of circumradius sqrt(3) has half extent one.
    Field f;
    const u32 cube = f.platonic({0, 0, 0}, std::sqrt(3.0), 1);
    const u32 box = f.box({0, 0, 0}, {1, 1, 1});
    for (f64 x = -1.5; x <= 1.5; x += 0.25) {
        for (f64 y = -1.5; y <= 1.5; y += 0.25) {
            CHECK(at(f, cube, x, y, 0.3) == doctest::Approx(at(f, box, x, y, 0.3)).epsilon(0.01));
        }
    }
}

TEST_CASE("union takes whichever is nearer and difference carves") {
    Field f;
    const u32 a = f.sphere({-1, 0, 0}, 1.0);
    const u32 b = f.sphere({1, 0, 0}, 1.0);
    const u32 both = f.unite({a, b});
    CHECK(at(f, both, -1, 0, 0) == doctest::Approx(-1.0));
    CHECK(at(f, both, 1, 0, 0) == doctest::Approx(-1.0));

    const u32 cut = f.subtract({a, b});
    CHECK(at(f, cut, -1, 0, 0) == doctest::Approx(-1.0));   // still there
    CHECK(at(f, cut, 1, 0, 0) > 0.0);                       // and this part is gone

    const u32 common = f.intersect({a, b});
    CHECK(at(f, common, 0, 0, 0) == doctest::Approx(0.0));  // they meet exactly at the origin
}

TEST_CASE("more parts than a node holds still combine") {
    // A node carries four children; an author writing `union { a b c d e f }` should not have to
    // know that. Six spheres in a row, and every one of them has to be in the result.
    Field f;
    std::vector<u32> parts;
    for (i32 i = 0; i < 6; ++i) {
        parts.push_back(f.sphere({static_cast<f64>(i) * 3.0, 0, 0}, 1.0));
    }
    const u32 all = f.unite(parts);
    for (i32 i = 0; i < 6; ++i) {
        CHECK(at(f, all, static_cast<f64>(i) * 3.0, 0, 0) == doctest::Approx(-1.0));
    }
}

TEST_CASE("a smooth union has no crease where a hard one does") {
    Field f;
    const u32 a = f.sphere({-0.9, 0, 0}, 1.0);
    const u32 b = f.sphere({0.9, 0, 0}, 1.0);
    const u32 hard = f.unite({a, b});
    const u32 soft = f.smooth_unite({a, b}, 0.5);
    // In the valley between them the blended surface bulges out, so it is further inside.
    CHECK(at(f, soft, 0, 0, 0) < at(f, hard, 0, 0, 0));
}

TEST_CASE("a shell is hollow and has the thickness asked for") {
    Field f;
    const u32 solid = f.sphere({0, 0, 0}, 2.0);
    const u32 hollow = f.shell(solid, 0.1);
    CHECK(at(f, hollow, 0, 0, 0) > 0.0);              // the middle is air now
    CHECK(at(f, hollow, 2, 0, 0) == doctest::Approx(-0.1));
    CHECK(at(f, hollow, 1.95, 0, 0) < 0.0);           // inside the wall
    CHECK(at(f, hollow, 1.5, 0, 0) > 0.0);            // and out the other side of it
}

TEST_CASE("translate, mirror and rotate move the shape, not the point") {
    Field f;
    const u32 s = f.sphere({0, 0, 0}, 1.0);
    const u32 moved = f.translate(s, {5, 0, 0});
    CHECK(at(f, moved, 5, 0, 0) == doctest::Approx(-1.0));
    CHECK(at(f, moved, 0, 0, 0) == doctest::Approx(4.0));

    // A box off to one side, mirrored, appears on both.
    const u32 b = f.box({2, 0, 0}, {0.5, 0.5, 0.5});
    const u32 both = f.mirror(b, 0);
    CHECK(at(f, both, 2, 0, 0) == doctest::Approx(-0.5));
    CHECK(at(f, both, -2, 0, 0) == doctest::Approx(-0.5));

    // A quarter turn about y takes the x axis to *minus* z, which is what a right-handed turn
    // about an upward axis does. Worth pinning down: the sign of a rotation is the sort of thing
    // every author gets wrong once, and a clip that comes out mirrored is hard to spot.
    const u32 bar = f.box({3, 0, 0}, {1, 0.2, 0.2});
    const u32 turned = f.rotate(bar, {0, 0.25, 0});
    CHECK(at(f, turned, 0, 0, -3) < 0.0);
    CHECK(at(f, turned, 3, 0, 0) > 0.0);   // and it is no longer where it started
}

TEST_CASE("repeat tiles a shape and its limit stops it") {
    Field f;
    const u32 post = f.cylinder({0, 0, 0}, 0.2, 1.0, 1);
    const u32 row = f.repeat(post, {2, 0, 0}, {2, 0, 0});
    CHECK(at(f, row, 0, 0, 0) < 0.0);
    CHECK(at(f, row, 2, 0, 0) < 0.0);
    CHECK(at(f, row, 4, 0, 0) < 0.0);
    // Past the limit the tiling stops rather than going on for ever.
    CHECK(at(f, row, 6, 0, 0) > 0.0);
    CHECK(at(f, row, 8, 0, 0) > 0.0);
}

TEST_CASE("polar repeat puts copies round a circle") {
    Field f;
    const u32 spoke = f.box({1.5, 0, 0}, {1.0, 0.1, 0.1});
    const u32 wheel = f.polar_repeat(spoke, 4, 1);
    CHECK(at(f, wheel, 1.5, 0, 0) < 0.0);
    CHECK(at(f, wheel, 0, 0, 1.5) < 0.0);
    CHECK(at(f, wheel, -1.5, 0, 0) < 0.0);
    CHECK(at(f, wheel, 0, 0, -1.5) < 0.0);
}

TEST_CASE("displacement moves a surface by the pattern times the amount") {
    Field f;
    const u32 wall = f.plane({0, 1, 0}, 0.0);
    const u32 wave = f.sine(0, 4.0, 0.0);          // one period every four metres along x
    const u32 rippled = f.displace(wall, wave, 0.5);
    // At a quarter period the sine is one, so the surface is pushed a full half metre.
    CHECK(at(f, rippled, 1.0, 0.0, 0.0) == doctest::Approx(0.5));
    // At three quarters it is minus one, so the other way.
    CHECK(at(f, rippled, 3.0, 0.0, 0.0) == doctest::Approx(-0.5));
    // And where the sine crosses zero the surface is where it always was.
    CHECK(at(f, rippled, 0.0, 0.0, 0.0) == doctest::Approx(0.0));
}

TEST_CASE("noise is deterministic, bounded, and actually varies") {
    Field f;
    const u32 n = f.noise(1.0, 1234u);
    const f64 first = at(f, n, 0.3, 0.7, 1.1);
    CHECK(at(f, n, 0.3, 0.7, 1.1) == doctest::Approx(first));   // same place, same answer

    f64 lo = 1e9, hi = -1e9;
    for (f64 x = 0; x < 8.0; x += 0.37) {
        const f64 v = at(f, n, x, x * 0.5, -x);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
        CHECK(v >= -1.000001);
        CHECK(v <= 1.000001);
    }
    CHECK(hi - lo > 0.2);   // a pattern that never moves is not a pattern
}

TEST_CASE("fbm with more octaves has finer detail than one with fewer") {
    Field f;
    const u32 smooth = f.fbm(2.0, 1u, 0.5, 2.0, 7u);
    const u32 detailed = f.fbm(2.0, 5u, 0.5, 2.0, 7u);
    // Measured as how much the value moves between nearby samples: more octaves, more movement.
    f64 rough_smooth = 0.0;
    f64 rough_detailed = 0.0;
    f64 previous_s = at(f, smooth, 0, 0, 0);
    f64 previous_d = at(f, detailed, 0, 0, 0);
    for (f64 x = 0.05; x < 4.0; x += 0.05) {
        const f64 s = at(f, smooth, x, 0, 0);
        const f64 d = at(f, detailed, x, 0, 0);
        rough_smooth += std::abs(s - previous_s);
        rough_detailed += std::abs(d - previous_d);
        previous_s = s;
        previous_d = d;
    }
    CHECK(rough_detailed > rough_smooth);
}

TEST_CASE("a checker alternates and stripes have the duty asked for") {
    Field f;
    const u32 c = f.checker({1, 1, 1});
    CHECK(at(f, c, 0.5, 0.5, 0.5) == doctest::Approx(-1.0));
    CHECK(at(f, c, 1.5, 0.5, 0.5) == doctest::Approx(1.0));
    CHECK(at(f, c, 1.5, 1.5, 0.5) == doctest::Approx(-1.0));

    const u32 s = f.stripes(0, 1.0, 0.25);
    CHECK(at(f, s, 0.1, 0, 0) == doctest::Approx(-1.0));   // inside the quarter
    CHECK(at(f, s, 0.5, 0, 0) == doctest::Approx(1.0));    // outside it
}

TEST_CASE("brick courses are offset from each other") {
    Field f;
    // Bricks a quarter metre long and an eighth high, with a thin joint, on a wall facing z.
    const u32 b = f.bricks({0.25, 0.125, 0}, 0.01, 2);
    // The joint at the end of a brick in one course should not line up with the course above.
    const f64 course0 = at(f, b, 0.25, 0.06, 0.0);   // a joint on the lower course
    const f64 course1 = at(f, b, 0.25, 0.19, 0.0);   // the same place one course up
    CHECK(course0 > 0.0);        // in the mortar
    CHECK(course1 < course0);    // and on the brick above it, because the bond is staggered
}

TEST_CASE("a parameter can be moved without rebuilding anything") {
    // The property live tweaking depends on: the graph is built once, and turning a dial writes
    // a slot. If this ever needs the field rebuilt, a clip cannot fluctuate while you watch it.
    Field f;
    const u32 r = f.parameter("radius", 1.0);
    // A sphere whose radius is a parameter is the distance from a point *minus* that parameter.
    // Arithmetic, not carving: `subtract` joins solids, and using it here would ask for the max
    // of two numbers rather than their difference.
    const u32 shape = f.add({f.radius({0, 0, 0}), f.negate(r)});
    const usize before = f.size();

    CHECK(f.eval(shape, {1.0, 0, 0}) == doctest::Approx(0.0));
    CHECK(f.set_parameter("radius", 2.0));
    CHECK(f.eval(shape, {2.0, 0, 0}) == doctest::Approx(0.0));
    CHECK(f.eval(shape, {1.0, 0, 0}) == doctest::Approx(-1.0));
    CHECK(f.size() == before);   // nothing was added to move it
}

TEST_CASE("the same parameter name asked for twice is the same slot") {
    Field f;
    const u32 a = f.parameter("height", 3.0);
    const u32 b = f.parameter("height", 99.0);   // the initial value of an existing name is kept
    CHECK(f.eval(a, {0, 0, 0}) == doctest::Approx(3.0));
    CHECK(f.eval(b, {0, 0, 0}) == doctest::Approx(3.0));
    CHECK(f.parameter_count() == 1);
    f.set_parameter("height", 5.0);
    CHECK(f.eval(a, {0, 0, 0}) == doctest::Approx(5.0));
    CHECK(f.eval(b, {0, 0, 0}) == doctest::Approx(5.0));
}

TEST_CASE("a normal points away from the surface") {
    Field f;
    const u32 s = f.sphere({0, 0, 0}, 1.0);
    const Vec3 n = f.normal_at(s, {1, 0, 0});
    CHECK(n.x == doctest::Approx(1.0).epsilon(0.01));
    CHECK(std::abs(n.y) < 0.01);
    CHECK(std::abs(n.z) < 0.01);

    const Vec3 up = f.normal_at(f.plane({0, 1, 0}, 0.0), {3, 0, -2});
    CHECK(up.y == doctest::Approx(1.0).epsilon(0.01));
}

TEST_CASE("stairs rise in steps rather than a ramp") {
    Field f;
    // A flight four metres long and two high, in steps of half a metre by a quarter.
    const u32 s = f.stairs({0, 0, 0}, {1, 1, 2}, 0.5, 0.25);
    // Low down at the bottom of the flight is solid; high up at the bottom is not.
    CHECK(at(f, s, 0, -0.9, -1.9) < 0.0);
    CHECK(at(f, s, 0, 0.9, -1.9) > 0.0);
    // And at the top of the flight the solid reaches higher than it does at the bottom.
    f64 top_at_start = -1.0;
    f64 top_at_end = -1.0;
    for (f64 y = -1.0; y <= 1.0; y += 0.01) {
        if (at(f, s, 0, y, -1.9) < 0.0) top_at_start = y;
        if (at(f, s, 0, y, 1.9) < 0.0) top_at_end = y;
    }
    CHECK(top_at_end > top_at_start);
}

TEST_CASE("remap and smoothstep put a pattern into the range a rule wants") {
    Field f;
    const u32 wave = f.sine(0, 4.0, 0.0);              // -1 to 1
    const u32 zero_one = f.remap(wave, -1.0, 1.0, 0.0, 1.0);
    CHECK(at(f, zero_one, 1.0, 0, 0) == doctest::Approx(1.0));
    CHECK(at(f, zero_one, 3.0, 0, 0) == doctest::Approx(0.0));
    CHECK(at(f, zero_one, 0.0, 0, 0) == doctest::Approx(0.5));

    const u32 hard = f.step(wave, 0.0);
    CHECK(at(f, hard, 1.0, 0, 0) == doctest::Approx(1.0));
    CHECK(at(f, hard, 3.0, 0, 0) == doctest::Approx(0.0));
}

TEST_CASE("bounding boxes speed a union up without changing a single answer") {
    // The optimisation that lets a union of thirty parts cost less than thirty evaluations, and
    // the check that it is exact rather than nearly exact.
    //
    // It was not, once. The box distance is zero for a point *inside* a box, and a shape a point
    // is inside reports a negative distance, so the first version skipped children that could
    // have been more negative still. Nothing appeared or vanished — the sign was always right —
    // but the magnitude moved, and the magnitude is what surface normals are made of, so a paint
    // rule that followed the normals put four hundred voxels of moss in the wrong place. Sampling
    // the same field twice and demanding the same number is the only way that shows up.
    Field f;
    std::vector<u32> parts;
    for (i32 i = 0; i < 12; ++i) {
        const f64 x = static_cast<f64>(i % 4) * 2.0;
        const f64 y = static_cast<f64>(i / 4) * 2.0;
        parts.push_back(f.sphere({x, y, 0}, 0.8));
        parts.push_back(f.box({x, y, 2.0}, {0.5, 0.5, 0.5}));
    }
    const u32 all = f.unite(parts);

    // Every answer, before and after the boxes exist.
    std::vector<f64> before;
    for (f64 z = -1.0; z <= 3.0; z += 0.31) {
        for (f64 y = -1.0; y <= 5.0; y += 0.29) {
            for (f64 x = -1.0; x <= 7.0; x += 0.27) {
                before.push_back(f.eval(all, {x, y, z}));
            }
        }
    }
    f.build_bounds();
    usize at = 0;
    for (f64 z = -1.0; z <= 3.0; z += 0.31) {
        for (f64 y = -1.0; y <= 5.0; y += 0.29) {
            for (f64 x = -1.0; x <= 7.0; x += 0.27) {
                CHECK(f.eval(all, {x, y, z}) == doctest::Approx(before[at++]));
            }
        }
    }
    CHECK(at == before.size());
}

TEST_CASE("the slack a displacement adds is what stops the sampler skipping too far") {
    Field f;
    const u32 wall = f.plane({0, 1, 0}, 0.0);
    const u32 wave = f.sine(0, 1.0, 0.0);
    CHECK(f.skip_slack() == doctest::Approx(0.0));   // nothing displaced yet

    // Twice the amplitude, not once. A reading may be `a` further out than the true surface, and
    // the point being asked about may be `a` further in, so a skip has to survive both.
    const u32 rippled = f.displace(wall, wave, 0.25);
    CHECK(f.skip_slack() == doctest::Approx(0.5));
    CHECK(f.metric_slack(rippled) == doctest::Approx(0.5));

    // Displacing by something whose range is not known turns skipping off entirely rather than
    // guessing at it, because a jump that is too long leaves holes nobody would notice. A raw
    // coordinate is the plainest example: it grows without limit, so no amount of allowance is
    // enough.
    Field g;
    const u32 base = g.sphere({0, 0, 0}, 1.0);
    const u32 unbounded = g.coordinate(1);
    const u32 lumpy = g.displace(base, unbounded, 0.1);
    CHECK(g.skip_slack() > 1e20);
    CHECK(g.metric_slack(lumpy) > 1e20);
}

TEST_CASE("a displacement is charged what its pattern can actually reach") {
    // The allowance used to be a list of the ops that happen to land in [-1, 1], which answered
    // "unknown" to every pattern built by arithmetic — and unknown means infinite slack, which
    // means no box in the clip can be settled and every voxel is asked through the whole
    // expression. `multiply { mask amount }` is the ordinary way to write "only here, and only
    // this much", and it was the shape of every deformation weathering produces.
    Field f;
    const u32 wall = f.plane({0, 1, 0}, 0.0);
    const u32 grain = f.fbm(0.3, 3, 0.5, 2.0, 5u);

    // A tenth of the noise, so a tenth of the allowance.
    const u32 gentle = f.multiply({grain, f.constant(0.1)});
    const u32 softened = f.displace(wall, gentle, 0.2);
    CHECK(f.metric_slack(softened) == doctest::Approx(0.04));   // 0.2 x 0.1, twice over

    // A mask that can only ever be between zero and one costs what it says.
    Field g;
    const u32 slab = g.plane({0, 1, 0}, 0.0);
    const u32 ball = g.sphere({0, 0, 0}, 1.0);
    const u32 mask = g.smoothstep(g.negate(ball), -0.05, 0.0);
    const u32 scoped = g.displace(slab, g.multiply({mask, g.constant(0.5)}), 0.08);
    CHECK(g.metric_slack(scoped) == doctest::Approx(0.08));     // 0.08 x 0.5, twice over

    // And the range really is a range, not a magnitude: a pattern that swings wider than one is
    // charged more, where the old rule quietly charged it the same and grew the bounding box by
    // too little.
    Field h;
    const u32 floor_ = h.plane({0, 1, 0}, 0.0);
    const u32 big = h.multiply({h.sine(0, 1.0, 0.0), h.constant(4.0)});
    CHECK(h.metric_slack(h.displace(floor_, big, 0.1)) == doctest::Approx(0.8));
}

TEST_CASE("an expression that is not a distance says nothing about its neighbourhood") {
    Field f;

    // Shapes, and everything built out of them by combining, moving or offsetting, are distances
    // in metres: a reading at one point bounds the reading at every point near it, which is what
    // lets a whole block of voxels be settled from its centre.
    const u32 a = f.sphere({0, 0, 0}, 1.0);
    const u32 b = f.box({2, 0, 0}, {0.5, 0.5, 0.5}, 0.0);
    CHECK(f.metric_slack(a) == doctest::Approx(0.0));
    CHECK(f.metric_slack(f.unite({a, b})) == doctest::Approx(0.0));
    CHECK(f.metric_slack(f.subtract({a, b})) == doctest::Approx(0.0));
    CHECK(f.metric_slack(f.translate(a, {1, 2, 3})) == doctest::Approx(0.0));
    CHECK(f.metric_slack(f.round_off(a, 0.1)) == doctest::Approx(0.0));

    // A coordinate moves exactly one metre per metre, so `below=0` on one is a half space and is
    // decidable for a block just as a shape is.
    CHECK(f.metric_slack(f.coordinate(1)) == doctest::Approx(0.0));

    // Patterns are not. A noise value at the centre of a block says nothing whatever about the
    // value a voxel away, so a rule keyed on one has to be asked per voxel — which is exactly
    // what makes it more expensive, and exactly why the sampler needs to know the difference.
    CHECK(f.metric_slack(f.fbm(0.5, 4, 0.5, 2.0, 1u)) > 1e20);
    CHECK(f.metric_slack(f.noise(0.5, 1u)) > 1e20);
    CHECK(f.metric_slack(f.cells(0.5, 1u)) > 1e20);

    // Twisting a shape stretches space, so distances stop being distances.
    CHECK(f.metric_slack(f.twist(a, 0.5, 1)) > 1e20);
}

TEST_CASE("scale keeps the field usable as a distance") {
    // Scaling by two must not make every distance twice what it is, or a march through the
    // result steps straight past the surface. Under-stating is safe; over-stating is not.
    Field f;
    const u32 s = f.sphere({0, 0, 0}, 1.0);
    const u32 wide = f.scale(s, {2, 1, 1});
    CHECK(at(f, wide, 2, 0, 0) == doctest::Approx(0.0).epsilon(kLoose));
    CHECK(at(f, wide, 0, 1, 0) == doctest::Approx(0.0).epsilon(kLoose));
    CHECK(at(f, wide, 0, 0, 0) < 0.0);
    // Nowhere does it claim more distance than there is.
    for (f64 x = 0; x < 5.0; x += 0.1) {
        const f64 d = at(f, wide, x, 0, 0);
        CHECK(d <= std::abs(x - 2.0) + kLoose);
    }
}

// --- revolving -------------------------------------------------------------------------------
//
// The operation the classical orders are made of, so these are checked against shapes whose exact
// distance is already known from another direction: revolving a disc about an axis through its
// centre is a sphere, revolving it about an axis beside it is a torus. If the two disagree by
// anything at all the sweep is not the sweep it claims to be.

TEST_CASE("revolving a circle about its own centre is a sphere, exactly") {
    Field f;
    const u32 disc = f.sphere({0, 0, 0}, 1.5);
    const u32 swept = f.revolve(disc, {0, 0, 0}, 1);
    const u32 ball = f.sphere({0, 0, 0}, 1.5);
    for (f64 z = -3.0; z <= 3.0; z += 0.37) {
        for (f64 y = -3.0; y <= 3.0; y += 0.41) {
            for (f64 x = -3.0; x <= 3.0; x += 0.43) {
                CHECK(at(f, swept, x, y, z) == doctest::Approx(at(f, ball, x, y, z)));
            }
        }
    }
}

TEST_CASE("revolving a circle beside the axis is a torus, exactly") {
    Field f;
    const u32 section = f.sphere({2.0, 0, 0}, 0.5);
    const u32 swept = f.revolve(section, {0, 0, 0}, 1);
    const u32 ring = f.torus({0, 0, 0}, 2.0, 0.5, 1);
    for (f64 z = -3.0; z <= 3.0; z += 0.37) {
        for (f64 y = -1.5; y <= 1.5; y += 0.29) {
            for (f64 x = -3.0; x <= 3.0; x += 0.43) {
                CHECK(at(f, swept, x, y, z) == doctest::Approx(at(f, ring, x, y, z)));
            }
        }
    }
}

TEST_CASE("a revolved profile is placed where its axis is put, and stays a distance") {
    Field f;
    // A section from 0.30 to 0.45 out and 0 to 0.20 up: a plain fillet, revolved into a ring.
    const u32 section = f.box({0.375, 0.10, 0}, {0.075, 0.10, 1.0}, 0.0);
    const u32 ring = f.revolve(section, {5.0, 1.8, -2.0}, 1);

    // On the axis, inside the height of the band, is the hole in the middle.
    CHECK(at(f, ring, 5.0, 1.9, -2.0) == doctest::Approx(0.30));
    // Out at the middle of the band, all the way round.
    CHECK(at(f, ring, 5.375, 1.9, -2.0) == doctest::Approx(-0.075));
    CHECK(at(f, ring, 5.0, 1.9, -2.0 + 0.375) == doctest::Approx(-0.075));
    CHECK(at(f, ring, 5.0 - 0.375, 1.9, -2.0) == doctest::Approx(-0.075));
    // Above the band: the distance up to it.
    CHECK(at(f, ring, 5.375, 2.3, -2.0) == doctest::Approx(0.30));

    // It says as much about its neighbourhood as the profile does, which is what lets a whole
    // block of voxels inside a column base be settled from one reading.
    CHECK(f.metric_slack(ring) == doctest::Approx(0.0));

    f.build_bounds();
    const Field::Aabb box = f.bounds_of(ring);
    CHECK(!box.infinite());
    CHECK(box.low.x == doctest::Approx(5.0 - 0.45));
    CHECK(box.high.z == doctest::Approx(-2.0 + 0.45));
    CHECK(box.low.y == doctest::Approx(1.8));
    CHECK(box.high.y == doctest::Approx(2.0));
}

// --- the volute ------------------------------------------------------------------------------

TEST_CASE("a spiral is a real distance to the tube it sweeps") {
    Field f;
    const u32 scroll = f.spiral({0, 0, 0}, 1.0, 0.6, 0.1, 2.0, 2);   // in the x-y plane

    // It starts where it was told to, at radius one along +x.
    CHECK(at(f, scroll, 1.0, 0, 0) == doctest::Approx(-0.1).epsilon(0.02));
    // Half a turn in, the radius has fallen by the square root of the per-turn ratio.
    const f64 half = std::sqrt(0.6);
    CHECK(at(f, scroll, -half, 0, 0) == doctest::Approx(-0.1).epsilon(0.05));
    // A whole turn in, it is six tenths of where it began — and the point on the way out at
    // radius one is on the tube again, because the curve passed through there too.
    CHECK(at(f, scroll, 0.6, 0, 0) == doctest::Approx(-0.1).epsilon(0.05));

    // Off the plane of the curve by more than the tube: outside, by the amount expected.
    CHECK(at(f, scroll, 1.0, 0, 0.4) == doctest::Approx(0.3).epsilon(0.02));
    // Well outside the whole thing.
    CHECK(at(f, scroll, 4.0, 0, 0) == doctest::Approx(2.9).epsilon(0.02));

    // Never over-stated anywhere: a march that believed a distance longer than the truth would
    // step through the scroll and delete it.
    CHECK(f.metric_slack(scroll) == doctest::Approx(0.0));
    for (f64 y = -1.6; y <= 1.6; y += 0.11) {
        for (f64 x = -1.6; x <= 1.6; x += 0.13) {
            const f64 d = at(f, scroll, x, y, 0.0);
            const f64 moved = at(f, scroll, x + 0.05, y, 0.0);
            CHECK(std::abs(moved - d) <= 0.05 + kLoose);   // one-Lipschitz, as a distance must be
        }
    }

    f.build_bounds();
    const Field::Aabb box = f.bounds_of(scroll);
    CHECK(!box.infinite());
    CHECK(box.high.x == doctest::Approx(1.1));
    CHECK(box.high.z == doctest::Approx(0.1));
}

TEST_CASE("the boxes round a revolve, a spiral and a scaled shape cull nothing they should keep") {
    // The same demand as for every other box: a bound that is wrong by a little produces a clip
    // with pieces missing, and the pieces are missing quietly. So every answer is taken before the
    // boxes exist and demanded again after.
    Field f;
    const u32 section = f.box({0.6, 0.2, 0}, {0.2, 0.2, 2.0}, 0.0);
    const u32 ring = f.revolve(section, {1.0, 0, 1.0}, 1);
    const u32 scroll = f.spiral({-1.0, 0.5, 0}, 0.6, 0.6, 0.07, 2.0, 2);
    const u32 squashed = f.scale(f.cylinder({0.0, 0.0, 0.0}, 0.3, 0.5, 2), {2.0, 1.0, 1.0});
    const u32 all = f.unite({ring, scroll, squashed});

    std::vector<f64> before;
    for (f64 z = -1.5; z <= 2.5; z += 0.23) {
        for (f64 y = -1.0; y <= 1.5; y += 0.19) {
            for (f64 x = -2.5; x <= 2.5; x += 0.21) {
                before.push_back(f.eval(all, {x, y, z}));
            }
        }
    }
    f.build_bounds();
    usize seen = 0;
    for (f64 z = -1.5; z <= 2.5; z += 0.23) {
        for (f64 y = -1.0; y <= 1.5; y += 0.19) {
            for (f64 x = -2.5; x <= 2.5; x += 0.21) {
                CHECK(f.eval(all, {x, y, z}) == doctest::Approx(before[seen++]));
            }
        }
    }
    CHECK(seen == before.size());
}

// --- the mouldings ---------------------------------------------------------------------------
//
// Each one is checked at the places its own definition pins down: which corner is stone, which is
// air, and where the curve crosses. A moulding that is merely the right size and the wrong curve
// is the difference between a building and a drawing of one, and no measurement of a clip catches
// it — only asking the section itself does.

TEST_CASE("a fillet is the whole rectangle and nothing outside it") {
    Sections s("let band = fillet 0.30 0  0.45 0.10");
    CHECK(s.solid_at("band", 0.32, 0.02));
    CHECK(s.solid_at("band", 0.44, 0.09));
    CHECK(!s.solid_at("band", 0.46, 0.05));
    CHECK(!s.solid_at("band", 0.38, 0.12));
}

TEST_CASE("an ovolo swells and a cavetto hollows, between the same two corners") {
    // The two quarter rounds. Both run from the front of one end to the back of the other; the
    // ovolo keeps the stone inside the arc and the cavetto outside it, so between the same corners
    // the ovolo is the fatter of the two everywhere off the diagonal.
    Sections s(
        "let out = ovolo   0 0  0.2 0.2\n"
        "let in_ = cavetto 0 0  0.2 0.2\n");

    // The corner the numbers start at is stone in both; the far corner is air in both.
    CHECK(s.solid_at("out", 0.01, 0.01));
    CHECK(s.solid_at("in_", 0.01, 0.01));
    CHECK(!s.solid_at("out", 0.19, 0.19));
    CHECK(!s.solid_at("in_", 0.19, 0.19));

    // Halfway along the diagonal is the point that tells them apart: inside the ovolo's arc,
    // outside the cavetto's.
    CHECK(s.solid_at("out", 0.10, 0.10));
    CHECK(!s.solid_at("in_", 0.10, 0.10));

    // The ovolo is tangent to the ends: full projection along the bottom, none at the top.
    CHECK(s.solid_at("out", 0.19, 0.01));
    CHECK(!s.solid_at("out", 0.19, 0.15));
}

TEST_CASE("swapping the corners turns a moulding over") {
    // The whole of the interface. An ovolo that swells toward the top and one that swells toward
    // the bottom are the same four numbers in a different order, which is why there is no flip.
    Sections s(
        "let up   = ovolo 0 0    0.2 0.2\n"
        "let down = ovolo 0 0.2  0.2 0\n");
    CHECK(s.solid_at("up", 0.19, 0.01));
    CHECK(!s.solid_at("up", 0.19, 0.19));
    CHECK(s.solid_at("down", 0.19, 0.19));
    CHECK(!s.solid_at("down", 0.19, 0.01));
}

TEST_CASE("a bead is a half round standing off a flat back") {
    Sections s("let torus_ = bead 0 0  0.10 0.20");
    // The flat back, at mid height, and the crown of the round.
    CHECK(s.solid_at("torus_", 0.005, 0.10));
    CHECK(s.solid_at("torus_", 0.095, 0.10));
    // The corners of the rectangle are cut away by the round — that is what makes it a bead and
    // not a fillet.
    CHECK(!s.solid_at("torus_", 0.09, 0.19));
    CHECK(!s.solid_at("torus_", 0.09, 0.01));
    // And it really is round: at a quarter of the way up, the face has come in.
    CHECK(!s.solid_at("torus_", 0.099, 0.02));
}

TEST_CASE("a scotia is a deep hollow, and its deepest point is above the middle") {
    // The asymmetry is the whole character of the moulding: the lower sweep is the longer one,
    // which is why a scotia reads as a scotia and not as a groove.
    Sections s("let hollow = scotia 0 0  0.20 0.40");

    // Full at both ends, cut right back to the first corner's face where it is deepest.
    CHECK(s.solid_at("hollow", 0.10, 0.01));
    CHECK(s.solid_at("hollow", 0.10, 0.39));
    CHECK(!s.solid_at("hollow", 0.15, 0.24));
    CHECK(!s.solid_at("hollow", 0.02, 0.24));

    // Equally far above and below the deepest point, the stone left behind is thicker above.
    CHECK(!s.solid_at("hollow", 0.05, 0.12));
    CHECK(s.solid_at("hollow", 0.05, 0.36));
}

TEST_CASE("a cyma is an S, and the reverse one is the same S turned over") {
    Sections s(
        "let recta   = cyma         0 0  0.2 0.4\n"
        "let reversa = cyma_reversa 0 0  0.2 0.4\n");

    // Nearly the full projection at the end it swells at, nothing like it at the end it dies at.
    CHECK(s.solid_at("recta", 0.15, 0.02));
    CHECK(!s.solid_at("recta", 0.15, 0.38));
    CHECK(!s.solid_at("reversa", 0.15, 0.02));
    CHECK(s.solid_at("reversa", 0.15, 0.38));

    // Both cross the middle of the section at half its width, from either side of it, which is
    // where the two arcs meet and the curvature turns over.
    CHECK(s.solid_at("recta", 0.09, 0.19));
    CHECK(!s.solid_at("recta", 0.11, 0.19));
    CHECK(s.solid_at("recta", 0.09, 0.21));
    CHECK(!s.solid_at("recta", 0.11, 0.21));
    CHECK(s.solid_at("reversa", 0.09, 0.19));
    CHECK(!s.solid_at("reversa", 0.11, 0.21));

    // The convex half bulges past the halfway line and the hollow half falls short of it, which
    // is what makes it an S rather than a bevel.
    CHECK(s.solid_at("recta", 0.11, 0.10));      // convex, below
    CHECK(!s.solid_at("recta", 0.09, 0.30));     // hollow, above
    CHECK(s.solid_at("reversa", 0.11, 0.30));
    CHECK(!s.solid_at("reversa", 0.09, 0.10));
}

TEST_CASE("a moulding runs the way it is told to, and revolves into a ring") {
    // Both of the two things a section is ever for: run straight along a cornice, or turned about
    // an axis into a base. The same four numbers do each.
    Sections s(
        "let along_z = ovolo 0.30 1.00 -4.00   0.45 1.15 4.00\n"
        "let along_x = ovolo -4.00 1.00 0.30   4.00 1.15 0.45 run=x\n"
        "let ring    = revolve { ovolo 0.30 1.00  0.45 1.15 } axis=y\n");

    const Field& f = s.script.field;
    // The z-running one is stone from end to end along z and stops at its own faces.
    CHECK(f.eval(s.shape("along_z"), Vec3{0.31, 1.01, 3.9}) < 0.0);
    CHECK(f.eval(s.shape("along_z"), Vec3{0.31, 1.01, 4.5}) > 0.0);
    // The x-running one is the same section, read across z instead.
    CHECK(f.eval(s.shape("along_x"), Vec3{3.9, 1.01, 0.31}) < 0.0);
    CHECK(f.eval(s.shape("along_x"), Vec3{4.5, 1.01, 0.31}) > 0.0);

    // The revolved one is the same section all the way round and hollow up the middle.
    CHECK(f.eval(s.shape("ring"), Vec3{0.31, 1.01, 0.0}) < 0.0);
    CHECK(f.eval(s.shape("ring"), Vec3{0.0, 1.01, -0.31}) < 0.0);
    CHECK(f.eval(s.shape("ring"), Vec3{0.0, 1.05, 0.0}) > 0.0);
}

TEST_CASE("a spiral that opens outward is bounded by where it ends, not where it starts") {
    Field f;
    const u32 out = f.spiral({0, 0, 0}, 0.5, 2.0, 0.05, 2.0, 1);   // doubles every turn
    f.build_bounds();
    const Field::Aabb box = f.bounds_of(out);
    CHECK(box.high.x == doctest::Approx(0.5 * 4.0 + 0.05));
    // And the far end really is out there.
    CHECK(at(f, out, 2.0, 0, 0) == doctest::Approx(-0.05).epsilon(0.05));
}

// --- turning and stretching a shape ------------------------------------------------------------
//
// A box round a rotated shape is easy to write backwards: the evaluation turns the POINT by the
// negated angle, so a bound derived by copying that code lands the box on the mirror image of
// where the shape actually is. Nothing catches that except looking, because the clip still builds
// — the box simply excludes the shape, the sampler culls it everywhere, and the piece vanishes.
//
// So this does not check the box against arithmetic. It walks the shape, finds where it really is,
// and insists the box contains that.

namespace {

// Every point of `shape` that is inside it must be inside `box`. Walked coarsely over a region
// large enough to contain the shape wherever it ended up.
void bounds_really_contain(const Field& f, u32 shape, const Field::Aabb& box, f64 reach,
                           f64 step) {
    u32 found = 0;
    for (f64 x = -reach; x <= reach; x += step) {
        for (f64 y = -reach; y <= reach; y += step) {
            for (f64 z = -reach; z <= reach; z += step) {
                if (f.eval(shape, Vec3{x, y, z}) >= 0.0) continue;
                ++found;
                INFO("solid at " << x << "," << y << "," << z << " but the box is "
                                 << box.low.x << "," << box.low.y << "," << box.low.z << " to "
                                 << box.high.x << "," << box.high.y << "," << box.high.z);
                REQUIRE(x >= box.low.x - 1e-9);
                REQUIRE(x <= box.high.x + 1e-9);
                REQUIRE(y >= box.low.y - 1e-9);
                REQUIRE(y <= box.high.y + 1e-9);
                REQUIRE(z >= box.low.z - 1e-9);
                REQUIRE(z <= box.high.z + 1e-9);
            }
        }
    }
    // A test that found nothing solid would pass for the wrong reason.
    CHECK(found > 0);
}

}  // namespace

TEST_CASE("the bounds contain a rotated shape, whichever way it was turned") {
    // Long in x and thin in y and z, so turning it is unmistakable: a box that came out of the
    // wrong rotation is long along the wrong axis and the walk finds solid outside it.
    for (const Vec3 turns : {Vec3{0, 0, 0.125}, Vec3{0, 0.25, 0}, Vec3{0.1, 0.2, 0.3},
                             Vec3{0, 0, -0.125}}) {
        Field f;
        const u32 bar = f.box({0.4, 0, 0}, {0.9, 0.1, 0.1});
        const u32 turned = f.rotate(bar, turns);
        f.build_bounds();

        const Field::Aabb box = f.bounds_of(turned);
        REQUIRE(!box.infinite());
        bounds_really_contain(f, turned, box, 2.2, 0.05);
    }
}

TEST_CASE("a quarter turn about z puts the box where the shape went") {
    // The same claim stated in numbers, so a failure says which way it went wrong rather than
    // only that it did. A bar along +x, turned a quarter turn, lies along one of the y axes.
    Field f;
    const u32 bar = f.box({1.0, 0, 0}, {0.5, 0.1, 0.1});   // x from 0.5 to 1.5
    const u32 turned = f.rotate(bar, {0, 0, 0.25});
    f.build_bounds();

    const Field::Aabb box = f.bounds_of(turned);
    REQUIRE(!box.infinite());
    // Long in y now, and thin in x, whichever sign the turn came out as.
    CHECK(box.high.y - box.low.y == doctest::Approx(1.0));
    CHECK(box.high.x - box.low.x == doctest::Approx(0.2));
    bounds_really_contain(f, turned, box, 2.0, 0.05);
}

TEST_CASE("a uniform scale is bounded and a stretched one is not") {
    // Uniform: the node reports the true distance, so a cull may read the box.
    for (const Vec3 by : {Vec3{2.0, 2.0, 2.0}, Vec3{0.5, 0.5, 0.5}, Vec3{-2.0, -2.0, -2.0}}) {
        Field f;
        const u32 lump = f.box({0.6, 0, 0}, {0.3, 0.2, 0.2});
        const u32 sized = f.scale(lump, by);
        f.build_bounds();

        const Field::Aabb box = f.bounds_of(sized);
        REQUIRE(!box.infinite());
        bounds_really_contain(f, sized, box, 2.5, 0.05);
    }

    // Stretched: the shape really is inside the scaled box, but the node under-reports how far
    // away it is, and a cull that believed the box would drop it while it was still the nearest
    // thing. So it gets no box, on purpose.
    Field f;
    const u32 lump = f.box({0.6, 0, 0}, {0.3, 0.2, 0.2});
    const u32 stretched = f.scale(lump, {2.0, 1.0, 1.0});
    f.build_bounds();
    CHECK(f.bounds_of(stretched).infinite());
}

TEST_CASE("min and max are union and intersection, and are bounded like them") {
    // How the facility cuts its rustication joints: a band of masonry intersected with an
    // arithmetic term that has no extent of its own. The band bounds the result; without this the
    // whole thing was unbounded and every solid voxel in the building was asked about it.
    Field f;
    const u32 band = f.box({0, 3.0, 0}, {5.0, 0.2, 5.0});
    const u32 open = f.add({f.constant(0.02), f.negate(f.sphere({0, 3.0, 0}, 0.4))});
    const u32 joint = f.maximum({band, open});
    f.build_bounds();

    const Field::Aabb box = f.bounds_of(joint);
    REQUIRE(!box.infinite());
    // The band's box, not the sphere's and not everywhere.
    CHECK(box.high.y - box.low.y == doctest::Approx(0.4));
    CHECK(box.high.x == doctest::Approx(5.0));

    // A union of the two is unbounded, because the arithmetic half is.
    const u32 either = f.minimum({band, open});
    f.build_bounds();
    CHECK(f.bounds_of(either).infinite());
}

TEST_CASE("a sum is settleable only when at most one of its terms moves") {
    Field f;
    const u32 ball = f.sphere({0, 0, 0}, 1.0);
    const u32 other = f.sphere({3, 0, 0}, 1.0);

    // A shape shifted by a constant is the same shape read at a different level, so it settles.
    CHECK(f.metric_slack(f.add({f.constant(0.02), ball})) == doctest::Approx(0.0));
    // Negating it changes nothing about how fast it moves.
    CHECK(f.metric_slack(f.negate(ball)) == doctest::Approx(0.0));
    CHECK(f.metric_slack(f.add({f.constant(0.02), f.negate(ball)})) == doctest::Approx(0.0));

    // Two moving terms can, added, vary twice as fast as either — so the sampler must not be
    // told it may decide a block from one reading at the centre.
    CHECK(f.metric_slack(f.add({ball, other})) >= Field::kInfiniteSlack);

    // min and max keep whichever term varies most, and both terms here are exact distances.
    CHECK(f.metric_slack(f.maximum({ball, other})) == doctest::Approx(0.0));
    CHECK(f.metric_slack(f.minimum({ball, other})) == doctest::Approx(0.0));

    // A pattern has no distance in it at all, and nothing built on one may claim otherwise.
    const u32 grain = f.fbm(0.1, 3, 0.5, 2.0, 1);
    CHECK(f.metric_slack(f.add({f.constant(0.02), grain})) >= Field::kInfiniteSlack);
    CHECK(f.metric_slack(f.maximum({ball, grain})) >= Field::kInfiniteSlack);
}

// The instrument R11's second step starts from, and it is written the way trap 15 says: the arm
// that must report something has to report it, or the clean arm proves nothing.
//
// "A quarter of the field carries no box" is not a work list. Two nodes with no box are opposite
// jobs — one whose own case in `build_bounds` gave up is a bound waiting to be written, and one
// that is unbounded only because a CHILD is will bound itself the moment the child does — and a
// pattern read by a paint rule is neither, because no shape evaluation ever walks it.
TEST_CASE("an unbounded node is counted against its op, and against whoever caused it") {
    Field f;
    const u32 wall = f.box({0, 2.0, 0}, {4.0, 2.0, 0.2});
    // Unbounded by its own case: a stretched scale is refused on purpose (see the test above),
    // and a half space really is infinite.
    const u32 stretched = f.scale(f.sphere({0, 0, 0}, 1.0), {2.0, 1.0, 1.0});
    const u32 ground = f.plane({0, 1, 0}, 0.0);
    // Unbounded only by inheritance: a translate of a bounded child would have a box.
    const u32 moved = f.translate(stretched, {1.0, 0, 0});
    const u32 shape = f.minimum({wall, moved, ground});
    // A pattern under nothing but a paint rule, which the shape cannot reach.
    const u32 grain = f.fbm(0.1, 3, 0.5, 2.0, 1);
    f.build_bounds();

    REQUIRE(f.bounds_of(shape).infinite());

    usize scale_own = 0, translate_inherited = 0, translate_own = 0, fbm_own = 0;
    for (const Field::Unbounded& row : f.unbounded_by_op()) {
        if (row.op == forge::Op::Scale) scale_own = row.own;
        if (row.op == forge::Op::Translate) { translate_own = row.own; translate_inherited = row.inherited; }
        if (row.op == forge::Op::Fbm) fbm_own = row.own;
    }
    CHECK(scale_own == 1);
    CHECK(fbm_own == 1);
    // The translate is a casualty, not a cause. Getting this backwards would send somebody to
    // write a bound for an op that already has a correct one.
    CHECK(translate_own == 0);
    CHECK(translate_inherited == 1);

    // And the filter that decides whether any of it is worth doing: the pattern is not under the
    // solid, so it is not a cull that failed. Nothing reaches it from `shape`.
    for (const Field::Unbounded& row : f.unbounded_by_op(shape)) {
        CHECK(row.op != forge::Op::Fbm);
    }
    CHECK(f.nodes_under(grain) == 1);
    CHECK(f.nodes_under(shape) < f.size());
    CHECK(f.nodes_under(Field::kEveryNode) == f.size());

    // A bounded field says so rather than saying nothing: no rows at all.
    Field plain;
    plain.box({0, 0, 0}, {1.0, 1.0, 1.0});
    plain.build_bounds();
    CHECK(plain.unbounded_by_op().empty());
}
