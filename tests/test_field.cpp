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

#include "forge/field.hpp"

using namespace ws;
using namespace ws::forge;

namespace {

constexpr f64 kLoose = 1e-6;

// A shape's distance at a point, for brevity in the checks below.
f64 at(const Field& f, u32 node, f64 x, f64 y, f64 z) {
    return f.eval(node, Vec3{x, y, z});
}

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
    // guessing at it, because a jump that is too long leaves holes nobody would notice.
    Field g;
    const u32 base = g.sphere({0, 0, 0}, 1.0);
    const u32 unbounded = g.cells(0.5, 1u);   // a distance, not a bounded pattern
    const u32 lumpy = g.displace(base, unbounded, 0.1);
    CHECK(g.skip_slack() > 1e20);
    CHECK(g.metric_slack(lumpy) > 1e20);
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
