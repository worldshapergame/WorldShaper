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

// --- part of the way round --------------------------------------------------------------------
//
// An apse, a niche head, a half dome, an arch ring and a curved colonnade are all a sweep through
// LESS than a whole turn, and the way they go wrong is quiet: a sweep that is a quarter turn out
// measures a perfectly plausible volume, and a distance that is right in SIGN and wrong in
// MAGNITUDE outside the cut leaves the shape looking correct in every slice while the surface
// normals near the cut — and therefore the paint that follows them — are wrong. So both are
// asserted here, and the full turn is asserted to be untouched to the last bit.

TEST_CASE("a sweep over a whole turn is the same node it always was, to the last bit") {
    // Not `Approx`. The whole point of storing a full turn as a width of exactly one is that the
    // existing fast path is taken and the arithmetic is character for character what it was, so
    // every clip in the repository measures identically. A near-miss here is a content hash that
    // moves under a building nobody changed.
    Field f;
    const u32 section = f.sphere({2.0, 0, 0}, 0.5);
    const u32 plain = f.revolve(section, {0, 0, 0}, 1);
    const u32 whole = f.revolve(section, {0, 0, 0}, 1, 0.0, 1.0);
    const u32 offset_whole = f.revolve(section, {0, 0, 0}, 1, 0.25, 1.25);
    const u32 ends_meet = f.revolve(section, {0, 0, 0}, 1, 0.4, 0.4);

    const u32 ring = f.torus({0.5, 1, -2}, 2.0, 0.4, 1);
    const u32 hoop = f.arc({0.5, 1, -2}, 2.0, 0.4, 1);
    const u32 hoop_wrapped = f.arc({0.5, 1, -2}, 2.0, 0.4, 1, 0.7, 1.7);

    const u32 post = f.cylinder({3.0, 0, 0}, 0.2, 1.0, 1);
    const u32 eight = f.polar_repeat(post, 8, 1);
    const u32 eight_full = f.polar_repeat(post, 8, 1, 0.0, 1.0);

    for (f64 z = -3.0; z <= 3.0; z += 0.31) {
        for (f64 y = -1.5; y <= 1.5; y += 0.27) {
            for (f64 x = -3.0; x <= 3.0; x += 0.29) {
                CHECK(at(f, whole, x, y, z) == at(f, plain, x, y, z));
                CHECK(at(f, offset_whole, x, y, z) == at(f, plain, x, y, z));
                CHECK(at(f, ends_meet, x, y, z) == at(f, plain, x, y, z));
                CHECK(at(f, hoop, x, y, z) == at(f, ring, x, y, z));
                CHECK(at(f, hoop_wrapped, x, y, z) == at(f, ring, x, y, z));
                CHECK(at(f, eight_full, x, y, z) == at(f, eight, x, y, z));
            }
        }
    }
}

TEST_CASE("a half revolve is half the shape, and the half it is told to be") {
    Field f;
    // A disc of radius a half, two metres out, turned about y. Whole it is a torus; from 0 to 0.5
    // it is the half of that torus on the +z side, because a turn of nought is along the first
    // cross-axis (x for a y sweep) and grows toward the second (z).
    const u32 section = f.sphere({2.0, 0, 0}, 0.5);
    const u32 whole = f.revolve(section, {0, 0, 0}, 1);
    const u32 half = f.revolve(section, {0, 0, 0}, 1, 0.0, 0.5);

    CHECK(at(f, half, 0, 0, 2.0) == doctest::Approx(-0.5));    // a quarter turn in: solid
    CHECK(at(f, half, 0, 0, -2.0) == doctest::Approx(2.5));    // three quarters round: air
    // Exactly on a cut plane is the surface, so the distance is nought and not a negative number.
    CHECK(at(f, half, 2.0, 0, 0) == doctest::Approx(0.0));
    CHECK(at(f, half, -2.0, 0, 0) == doctest::Approx(0.0));

    // Half the volume, counted rather than argued.
    usize inside_whole = 0, inside_half = 0;
    for (f64 z = -2.6; z <= 2.6; z += 0.05) {
        for (f64 y = -0.6; y <= 0.6; y += 0.05) {
            for (f64 x = -2.6; x <= 2.6; x += 0.05) {
                if (at(f, whole, x, y, z) < 0.0) ++inside_whole;
                if (at(f, half, x, y, z) < 0.0) ++inside_half;
            }
        }
    }
    REQUIRE(inside_whole > 1000);
    CHECK(static_cast<f64>(inside_half) / static_cast<f64>(inside_whole) ==
          doctest::Approx(0.5).epsilon(0.01));
}

TEST_CASE("outside the cut the distance is to the CUT, not to the full revolution") {
    // The trap this feature is most likely to fall into, and the one that does not show in a
    // slice: outside the angular wedge the nearest matter is on an END CAP. Return the full
    // revolution's distance there and every voxel keeps its sign, so nothing appears or vanishes
    // — but the magnitude is wrong, and magnitude is what surface normals are made of. That is
    // the same fault, in a new place, as the union box test that put four hundred voxels of moss
    // where they did not belong.
    Field f;
    const u32 half = f.revolve(f.sphere({2.0, 0, 0}, 0.5), {0, 0, 0}, 1, 0.0, 0.5);

    // Out at the middle of the section, just past the cut plane at z = 0: the whole revolution
    // would say -0.5 because the point is inside the torus. The truth is the distance to the cap,
    // which is exactly how far past the plane the point stands.
    for (const f64 e : {0.01, 0.05, 0.1, 0.2}) {
        CHECK(at(f, half, 2.0, 0, -e) == doctest::Approx(e));
        CHECK(at(f, half, -2.0, 0, -e) == doctest::Approx(e));
    }
    // And inside the solid, near a cap, the depth is to the CAP and not to the swept face: the
    // caps are surface too.
    CHECK(at(f, half, 2.0, 0, 0.02) == doctest::Approx(-0.02));
    CHECK(at(f, half, -2.0, 0, 0.02) == doctest::Approx(-0.02));

    // Which is another way of saying the field stays one-Lipschitz through the cut — a march that
    // believed a longer distance than the truth would step straight through the cap.
    for (f64 z = -3.0; z <= 3.0; z += 0.07) {
        for (f64 x = -3.0; x <= 3.0; x += 0.07) {
            const f64 d = at(f, half, x, 0.0, z);
            CHECK(std::abs(at(f, half, x + 0.01, 0.0, z) - d) <= 0.01 + kLoose);
            CHECK(std::abs(at(f, half, x, 0.0, z + 0.01) - d) <= 0.01 + kLoose);
        }
    }
    CHECK(f.metric_slack(half) == doctest::Approx(0.0));
}

TEST_CASE("a range written backwards over the seam is the shape on the correct side") {
    Field f;
    // `from=0.75 to=0.25` is the half turn that runs 0.75 -> 0 -> 0.25, so it is centred on the
    // first cross-axis. An author will write this, and the failure it invites is an empty shape.
    const u32 seam = f.revolve(f.sphere({2.0, 0, 0}, 0.5), {0, 0, 0}, 1, 0.75, 0.25);
    CHECK(at(f, seam, 2.0, 0, 0) == doctest::Approx(-0.5));    // the middle of the range
    CHECK(at(f, seam, -2.0, 0, 0) == doctest::Approx(2.5));    // half a turn away: air
    CHECK(at(f, seam, 0, 0, 2.0) == doctest::Approx(0.0));     // the ends, exactly on the caps
    CHECK(at(f, seam, 0, 0, -2.0) == doctest::Approx(0.0));

    // And a range with `to` below `from` sweeps the LONG way round, because the sweep always runs
    // the way `around` goes. 0.5 to 0.25 is three quarters of a turn, not one quarter.
    const u32 longway = f.revolve(f.sphere({2.0, 0, 0}, 0.5), {0, 0, 0}, 1, 0.5, 0.25);
    CHECK(at(f, longway, -2.0, 0, 0) == doctest::Approx(0.0));    // 0.5 turns is its `from` cap
    CHECK(at(f, longway, 2.0, 0, 0) == doctest::Approx(-0.5));    // through the seam: solid
    CHECK(at(f, longway, 0, 0, -2.0) == doctest::Approx(-0.5));   // and at 0.75 turns: solid
    const f64 mid = 0.375 * 6.283185307179586;                    // the quarter it must leave out
    CHECK(at(f, longway, 2.0 * std::cos(mid), 0, 2.0 * std::sin(mid)) > 0.0);
}

TEST_CASE("an arc is a torus that stops, with a round cap where it stops") {
    Field f;
    const u32 hoop = f.arc({0, 0, 0}, 2.0, 0.25, 1, 0.0, 0.5);
    // On the centre-line anywhere in the arc, the answer is minus the tube.
    CHECK(at(f, hoop, 2.0, 0, 0) == doctest::Approx(-0.25));
    CHECK(at(f, hoop, 0, 0, 2.0) == doctest::Approx(-0.25));
    CHECK(at(f, hoop, -2.0, 0, 0) == doctest::Approx(-0.25));
    // Round caps: past an end, the shape is a sphere about that end, so the distance is the
    // distance to the end point less the tube. A flat cap would answer differently by 0.03 here.
    CHECK(at(f, hoop, 2.0, 0, -0.75) == doctest::Approx(0.5));
    CHECK(at(f, hoop, 2.0, 0.6, -0.8) == doctest::Approx(1.0 - 0.25));
    // Nothing on the -z side beyond the caps.
    CHECK(at(f, hoop, 0, 0, -2.0) == doctest::Approx(std::sqrt(8.0) - 0.25));

    // The exact distance to the centre-line, everywhere, which is what makes this cheap: a real
    // torus segment has no closed form and this does.
    // The exact distance to the centre-line, everywhere: not the case analysis the node uses but
    // a plain minimisation over the arc, so a wrong end or a wrong wrap shows as metres. The
    // tolerance is the minimisation's own — it samples the arc, so it can only over-state, by at
    // most the spacing between samples.
    for (f64 z = -3.2; z <= 3.2; z += 0.31) {
        for (f64 y = -1.0; y <= 1.0; y += 0.29) {
            for (f64 x = -3.2; x <= 3.2; x += 0.31) {
                f64 nearest = 1e30;
                for (int i = 0; i <= 8000; ++i) {
                    const f64 turn = 0.5 * static_cast<f64>(i) / 8000.0 * 6.283185307179586;
                    const f64 cx = 2.0 * std::cos(turn), cz = 2.0 * std::sin(turn);
                    nearest = std::min(nearest, std::sqrt((x - cx) * (x - cx) + y * y +
                                                          (z - cz) * (z - cz)));
                }
                CHECK(at(f, hoop, x, y, z) == doctest::Approx(nearest - 0.25).epsilon(1e-3));
            }
        }
    }
    CHECK(f.metric_slack(hoop) == doctest::Approx(0.0));
}

TEST_CASE("seven columns from here round to there, with one on each end") {
    // The spacing decision, asserted: over an ARC there are n copies and n-1 gaps, first on
    // `from` and last on `to`. Over a whole turn there are n copies and n gaps, unchanged,
    // because a copy on each end would put two in one place.
    Field f;
    const u32 shaft = f.cylinder({3.0, 0, 0}, 0.2, 1.0, 1);
    const u32 fan = f.polar_repeat(shaft, 7, 1, 0.0, 0.25);
    const f64 tau = 6.283185307179586;
    for (int k = 0; k < 7; ++k) {
        const f64 turn = 0.25 * static_cast<f64>(k) / 6.0 * tau;
        CHECK(at(f, fan, 3.0 * std::cos(turn), 0, 3.0 * std::sin(turn)) ==
              doctest::Approx(-0.2));
    }
    // Between two of them, air.
    const f64 between = (0.25 / 12.0) * tau;
    CHECK(at(f, fan, 3.0 * std::cos(between), 0, 3.0 * std::sin(between)) > 0.0);
    // And nothing at all round the back of the circle.
    for (f64 turn = 0.35; turn < 0.95; turn += 0.05) {
        CHECK(at(f, fan, 3.0 * std::cos(turn * tau), 0, 3.0 * std::sin(turn * tau)) > 0.0);
    }
}

TEST_CASE("a partial sweep keeps its box, and the box is the whole turn's on purpose") {
    // Conservative, and said out loud: the segment's true extent is tighter and a box tighter than
    // the truth is a piece of the clip quietly missing. What has to hold is that the node still
    // HAS a box — an unbounded node is one no cull can skip — and that the box contains the shape.
    Field f;
    const u32 seg = f.revolve(f.sphere({2.0, 0, 0}, 0.5), {1, 2, 3}, 1, 0.1, 0.4);
    const u32 hoop = f.arc({-1, 0, 2}, 1.5, 0.3, 0, 0.2, 0.9);
    const u32 both = f.unite({seg, hoop});

    std::vector<f64> before;
    for (f64 z = -3.0; z <= 5.0; z += 0.23) {
        for (f64 y = -1.0; y <= 4.0; y += 0.19) {
            for (f64 x = -3.0; x <= 4.0; x += 0.21) before.push_back(f.eval(both, {x, y, z}));
        }
    }

    f.build_bounds();
    CHECK_FALSE(f.bounds_of(seg).infinite());
    CHECK_FALSE(f.bounds_of(hoop).infinite());
    CHECK(f.unbounded_nodes() == 0);
    // The whole revolution's box: as far out as the profile reaches, all the way round.
    CHECK(f.bounds_of(seg).low.x == doctest::Approx(1.0 - 2.5));
    CHECK(f.bounds_of(seg).high.z == doctest::Approx(3.0 + 2.5));
    CHECK(f.bounds_of(hoop).high.x == doctest::Approx(-1.0 + 0.3));

    // Everything solid is inside its own box, and the boxes changed no answer.
    usize seen = 0;
    for (f64 z = -3.0; z <= 5.0; z += 0.23) {
        for (f64 y = -1.0; y <= 4.0; y += 0.19) {
            for (f64 x = -3.0; x <= 4.0; x += 0.21) {
                CHECK(f.eval(both, {x, y, z}) == doctest::Approx(before[seen++]));
                for (const u32 node : {seg, hoop}) {
                    if (f.eval(node, {x, y, z}) >= 0.0) continue;
                    const Field::Aabb box = f.bounds_of(node);
                    CHECK(x >= box.low.x - kLoose);
                    CHECK(x <= box.high.x + kLoose);
                    CHECK(y >= box.low.y - kLoose);
                    CHECK(y <= box.high.y + kLoose);
                    CHECK(z >= box.low.z - kLoose);
                    CHECK(z <= box.high.z + kLoose);
                }
            }
        }
    }
    CHECK(seen == before.size());
}

TEST_CASE("the mirror evaluator walks a partial sweep to the same number") {
    // A partial revolve asks its profile once, twice or three times depending on where the point
    // stands, so the non-recursive twin needs a step counter over SAMPLE POINTS rather than over
    // children — the same mechanism curvature and occlusion use. A second evaluator that disagrees
    // with the first by one voxel is the worst kind of fault this repository has had.
    Field f;
    const u32 seg = f.revolve(f.box({2.0, 0.3, 0}, {0.4, 0.3, 1.0}, 0.0), {0, 0, 0}, 1, 0.1, 0.6);
    const u32 hoop = f.arc({0, 0, 0}, 1.5, 0.3, 1, 0.8, 0.2);
    const u32 fan = f.polar_repeat(f.sphere({2.5, 0, 0}, 0.3), 4, 1, 0.05, 0.4);
    const u32 all = f.unite({seg, hoop, fan});

    forge::Op missing = forge::Op::Constant;
    REQUIRE(f.mirror_covers(all, &missing));
    for (f64 z = -3.0; z <= 3.0; z += 0.29) {
        for (f64 y = -1.5; y <= 1.5; y += 0.23) {
            for (f64 x = -3.0; x <= 3.0; x += 0.31) {
                f64 walked = 0.0;
                REQUIRE(f.mirror_eval(all, {x, y, z}, walked));
                CHECK(walked == f.eval(all, {x, y, z}));
            }
        }
    }
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

TEST_CASE("the boxless nodes are reported by op, and split from the ones standing over them") {
    // The instrument D636 is about. A raw count of boxless nodes cannot be acted on, because most
    // of it is ancestors of the one node that refused a box — and there is nothing to do to an
    // ancestor except bound what is under it.
    Field f;
    const u32 lump = f.box({0, 0, 0}, {1.0, 1.0, 1.0});
    const u32 stretched = f.scale(lump, {2.0, 1.0, 1.0});   // refuses a box of its own
    const u32 moved = f.translate(stretched, {4.0, 0, 0});  // boxless only because of the scale
    const u32 both = f.unite({moved, f.sphere({8, 0, 0}, 1.0)});
    f.build_bounds();

    REQUIRE(f.bounds_of(stretched).infinite());
    REQUIRE(f.bounds_of(moved).infinite());
    REQUIRE(f.bounds_of(both).infinite());

    usize made_here = 0;
    usize standing_over = 0;
    for (const Field::UnboundedCause& cause : f.unbounded_by_op()) {
        made_here += cause.source;
        standing_over += cause.downstream;
        // The scale is the source and nothing else is; the translate and the union are over it.
        if (cause.op == forge::Op::Scale) {
            CHECK(cause.source == 1);
            CHECK(cause.downstream == 0);
        }
        if (cause.op == forge::Op::Translate || cause.op == forge::Op::Union) {
            CHECK(cause.source == 0);
            CHECK(cause.downstream == 1);
        }
    }
    CHECK(made_here == 1);
    CHECK(standing_over == 2);

    // Sorted by what can be acted on, so reading the top of the list is reading the work.
    const std::vector<Field::UnboundedCause> causes = f.unbounded_by_op();
    REQUIRE(!causes.empty());
    CHECK(causes.front().op == forge::Op::Scale);

    // And a field where everything bounds says so by being empty rather than by a zero row.
    Field clean;
    clean.unite({clean.sphere({0, 0, 0}, 1.0), clean.box({3, 0, 0}, {1.0, 1.0, 1.0})});
    clean.build_bounds();
    CHECK(clean.unbounded_by_op().empty());
    CHECK(clean.unbounded_nodes() == 0);
}

TEST_CASE("every op says what it is called") {
    // The names are what makes the histogram above readable, and a missing one is a silent "?"
    // in the middle of a report rather than a compile error.
    for (usize i = 0; i <= static_cast<usize>(forge::Op::Power); ++i) {
        const char* name = op_name(static_cast<forge::Op>(i));
        CHECK(name != nullptr);
        CHECK(std::string(name) != "?");
    }
}

TEST_CASE("a hierarchy over a wide union changes no answer, and is off unless asked for") {
    // D637: the accelerator is off by default because it costs a fifth of the facility's sample
    // and rejects nothing. What must stay true is that turning it back on is free of consequence
    // to the ANSWERS — a hierarchy that changed one would be geometry quietly missing, which is
    // D613's class and the reason boxes are checked this way rather than by inspection.
    const auto colonnade = [](Field& f) {
        std::vector<u32> parts;
        for (int i = 0; i < 24; ++i) {
            const f64 x = static_cast<f64>(i) * 1.5;
            parts.push_back(f.cylinder({x, 0, 0}, 0.4, 2.0, 1));
        }
        return f.unite(parts);
    };

    Field plain;
    const u32 plain_root = colonnade(plain);
    plain.build_bounds();
    CHECK(plain.accelerator_count() == 0);   // the default builds none at all

    Field fast;
    const u32 fast_root = colonnade(fast);
    fast.accelerate_from(12);
    fast.build_bounds();
    CHECK(fast.accelerate_from() == 12);
    REQUIRE(fast.accelerator_count() == 1);

    // Every answer, at points inside a column, in the gaps, and well outside the whole run.
    for (int i = -4; i < 44; ++i) {
        for (int j = -2; j < 3; ++j) {
            const Vec3 p{static_cast<f64>(i) * 0.8, static_cast<f64>(j) * 1.1, 0.3};
            CHECK(fast.eval(fast_root, p) == doctest::Approx(plain.eval(plain_root, p)));
        }
    }
}

TEST_CASE("culling a wide union changes no answer, whichever way it is culled") {
    // D638 made the union's rejection stop at the first child it can reject, on the grounds that
    // the children are in ascending box distance and the running answer only shrinks — so one
    // rejection proves the rest. That is an argument, and an argument about a cull is the kind of
    // thing that is wrong quietly: the sign never changes, the magnitude does, and what moves is
    // a surface normal and the paint rule that follows it (the four hundred voxels of moss).
    //
    // So it is checked the only way that settles it: against the same field with no boxes at all,
    // where nothing is culled and nothing can be skipped wrongly.
    const auto street = [](Field& f) {
        std::vector<u32> parts;
        for (int i = 0; i < 30; ++i) {
            const f64 x = static_cast<f64>(i) * 2.0;
            // Alternating shapes so the parts are not interchangeable, and overlapping in pairs
            // so some boxes really do contain the same points.
            parts.push_back((i % 2 == 0) ? f.sphere({x, 0, 0}, 1.2)
                                         : f.box({x, 0.5, 0}, {0.7, 1.4, 0.7}));
        }
        return f.unite(parts);
    };

    Field bare;                      // no build_bounds: bounds_ is empty and nothing is culled
    const u32 bare_root = street(bare);

    Field culled;
    const u32 culled_root = street(culled);
    culled.build_bounds();

    Field hierarchy;                 // ...and the BVH path, which is a third way to the answer
    const u32 hierarchy_root = street(hierarchy);
    hierarchy.accelerate_from(12);
    hierarchy.build_bounds();
    REQUIRE(hierarchy.accelerator_count() >= 1);

    for (int i = -6; i < 130; ++i) {
        for (int j = -3; j < 4; ++j) {
            for (int k = -2; k < 3; ++k) {
                const Vec3 p{static_cast<f64>(i) * 0.47, static_cast<f64>(j) * 0.63,
                             static_cast<f64>(k) * 0.71};
                const f64 truth = bare.eval(bare_root, p);
                CHECK(culled.eval(culled_root, p) == doctest::Approx(truth));
                CHECK(hierarchy.eval(hierarchy_root, p) == doctest::Approx(truth));
            }
        }
    }
}

TEST_CASE("the mirror evaluator reaches the same answer as the recursive one") {
    // R12b: a compute shader cannot recurse, so `mirror_eval` walks the same nodes with an
    // explicit stack. It exists to be COMPARED — two evaluators of one world that disagree is
    // D204's fault in its worst form — and it is written on the CPU first because every mistake
    // this shape can make is one a test can catch here and nothing can catch in a shader.
    Field f;
    const u32 wall = f.box({0, 1.0, 0}, {3.0, 1.0, 0.4});
    const u32 hole = f.sphere({0.5, 1.2, 0}, 0.7);
    const u32 carved = f.subtract({wall, hole});
    const u32 post = f.cylinder({-2.0, 0.8, 0}, 0.25, 0.8, 1);
    const u32 run = f.repeat(post, {1.3, 0, 0}, {3, 0, 0});
    const u32 turned = f.rotate(f.unite({carved, run}), {0.0, 0.125, 0.0});
    const u32 moved = f.translate(turned, {0.4, 0, 0.2});
    const u32 root = f.unite({moved, f.torus({0, 2.4, 0}, 0.9, 0.2, 1)});
    f.build_bounds();

    REQUIRE(f.mirror_covers(root));
    u32 deepest = 0;
    for (int i = -8; i <= 8; ++i) {
        for (int j = -5; j <= 9; ++j) {
            for (int k = -6; k <= 6; ++k) {
                const Vec3 p{i * 0.55, j * 0.43, k * 0.61};
                f64 mine = 0.0;
                u32 used = 0;
                REQUIRE(f.mirror_eval(root, p, mine, &used));
                deepest = std::max(deepest, used);
                CHECK(mine == f.eval(root, p));
            }
        }
    }
    // ...and it stayed inside the stack a shader would have (D643: the facility's deepest is 41).
    CHECK(deepest < Field::kMirrorStack);
}

TEST_CASE("the single-precision mirror is the same walk, narrowed, and stays inside a micron") {
    // R12b's last unknown is whether `f32` is enough for the field, and `mirror_eval_single` is
    // the instrument that answers it: the same stack walk with every point and every answer
    // narrowed to `float` as it crosses a node boundary. `--field-single` runs it over a whole
    // building; this runs it over a small one, so that a regression in the narrowing has something
    // to fail against that does not need the estate and two minutes.
    //
    // Three things are asserted and they are different claims. It is the SAME WALK -- so a
    // structure the double arm can reach, the single arm can reach too. It is NARROWED -- so it
    // must not come back bit-identical to the double arm everywhere, or the `narrow` has stopped
    // happening and the mode would report a clean run for the wrong reason (trap 15: a clean
    // measurement and a measurement that never ran look identical). And it must LEAVE THE FLAG
    // DOWN -- the narrowing rides on a thread-local, so an arm run after a single-precision one
    // has to be double again.
    Field f;
    const u32 wall = f.box({0, 1.0, 0}, {3.0, 1.0, 0.4});
    const u32 hole = f.sphere({0.5, 1.2, 0}, 0.7);
    const u32 carved = f.subtract({wall, hole});
    const u32 post = f.cylinder({-2.0, 0.8, 0}, 0.25, 0.8, 1);
    const u32 run = f.repeat(post, {1.3, 0, 0}, {3, 0, 0});
    const u32 turned = f.rotate(f.unite({carved, run}), {0.0, 0.125, 0.0});
    const u32 moved = f.translate(turned, {0.4, 0, 0.2});
    const u32 root = f.unite({moved, f.torus({0, 2.4, 0}, 0.9, 0.2, 1)});
    f.build_bounds();
    REQUIRE(f.mirror_covers(root));

    u64 points = 0;
    u64 differed = 0;
    u64 changed_side = 0;
    f64 worst = 0.0;
    for (int i = -8; i <= 8; ++i) {
        for (int j = -5; j <= 9; ++j) {
            for (int k = -6; k <= 6; ++k) {
                const Vec3 p{i * 0.55, j * 0.43, k * 0.61};
                f64 walked = 0.0;
                f64 narrowed = 0.0;
                REQUIRE(f.mirror_eval(root, p, walked));
                REQUIRE(f.mirror_eval_single(root, p, narrowed));
                ++points;
                const f64 d = std::abs(walked - narrowed);
                worst = std::max(worst, d);
                if (d != 0.0) ++differed;
                if ((walked < 0.0) != (narrowed < 0.0)) ++changed_side;
            }
        }
    }
    REQUIRE(points > 1000);

    // Narrowing to `float` and back has a floor of about 6e-8 relative, and this shape is a few
    // metres across, so a micron is the order to expect. A metre would be a broken walk and a
    // bit-identical answer at every point would be a narrowing that is not happening.
    CHECK(worst < 1e-5);
    CHECK(differed > 0);
    // Ties are the only thing single precision can move on a field this size, and this lattice
    // deliberately misses every surface, so nothing here should change side at all.
    CHECK(changed_side == 0);

    // The flag is a thread-local and a walk that left it up would silently narrow every later
    // evaluation on this thread -- including the reference arm of the mode that uses it.
    for (int i = -8; i <= 8; ++i) {
        const Vec3 p{i * 0.55, 0.43, 0.61};
        f64 walked = 0.0;
        REQUIRE(f.mirror_eval(root, p, walked));
        CHECK(walked == f.eval(root, p));
    }
}

TEST_CASE("the mirror says which op it does not know, rather than answering anyway") {
    // Trap 7 in the second evaluator: "I could not" and "the answer is nought" must never be the
    // same reply. Every op the facility's solid reaches is mirrored, and one that is not says so
    // by name, once, instead of failing a voxel at a time.
    Field f;
    const u32 shape = f.unite({f.sphere({0, 0, 0}, 1.0), f.box({2, 0, 0}, {0.5, 0.5, 0.5})});
    f.build_bounds();
    forge::Op missing = forge::Op::Constant;
    CHECK(f.mirror_covers(shape, &missing));

    // A value node the mirror has no case for is refused, and named.
    Field odd;
    const u32 pattern = odd.cells(0.5, 7);
    const u32 with_cells = odd.displace(odd.sphere({0, 0, 0}, 1.0), pattern, 0.1);
    odd.build_bounds();
    f64 out = 0.0;
    if (!odd.mirror_covers(with_cells, &missing)) {
        CHECK(std::string(op_name(missing)) != "?");
        CHECK(!odd.mirror_eval(with_cells, {0.3, 0.2, 0.1}, out));
    }
}

TEST_CASE("four primitives answer less than the distance to their own box") {
    // D644, and it is pinned here because it is invisible from every direction except this one.
    // The box cull in `eval` assumes a node outside its box answers at least the distance to it.
    // These four are bounded approximations and do not, so a union can skip the nearest thing
    // there is. Refusing them the cull is sound and measured 12.3 s against 83.7 s on the
    // facility, so the cull still reads their boxes and this test records why that is a choice.
    CHECK(op_reports_true_distance(forge::Op::Sphere));
    CHECK(op_reports_true_distance(forge::Op::Box));
    CHECK(op_reports_true_distance(forge::Op::Cylinder));
    CHECK(op_reports_true_distance(forge::Op::Torus));
    CHECK(op_reports_true_distance(forge::Op::Capsule));
    CHECK_FALSE(op_reports_true_distance(forge::Op::Ellipsoid));
    CHECK_FALSE(op_reports_true_distance(forge::Op::Cone));
    CHECK_FALSE(op_reports_true_distance(forge::Op::Prism));
    CHECK_FALSE(op_reports_true_distance(forge::Op::Platonic));

    // And the fault itself, so that fixing it changes this test rather than nothing.
    Field f;
    const u32 egg = f.ellipsoid({0, 0, 0}, {2.0, 0.6, 1.1});
    f.build_bounds();
    const Field::Aabb box = f.bounds_of(egg);
    REQUIRE(!box.infinite());
    // Swept rather than pointed at: the under-statement is worst away from the major axis, and a
    // single hand-picked point is how this would quietly stop testing anything.
    bool found_under_report = false;
    f64 worst_ratio = 1.0;
    for (int i = -10; i <= 10 && !found_under_report; ++i) {
        for (int j = -10; j <= 10; ++j) {
            for (int k = -10; k <= 10; ++k) {
                const Vec3 p{i * 0.9, j * 0.9, k * 0.9};
                const f64 dx = std::max(std::max(box.low.x - p.x, p.x - box.high.x), 0.0);
                const f64 dy = std::max(std::max(box.low.y - p.y, p.y - box.high.y), 0.0);
                const f64 dz = std::max(std::max(box.low.z - p.z, p.z - box.high.z), 0.0);
                const f64 away = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (away <= 0.01) continue;
                worst_ratio = std::min(worst_ratio, f.eval(egg, p) / away);
            }
        }
    }
    CHECK(worst_ratio < 0.99);   // under-reports: the cull's assumption, broken
}

// --- the chamfer: a flat cut on one seam ------------------------------------------------------
//
// `round=` rounds a whole shape and `smooth=` blends a whole seam. Neither is what a mason cuts:
// a chamfer is FLAT, it is at forty-five degrees, and it applies to the one edge where two faces
// meet. So what these check is the flatness — the surface is a plane and the distance to it is a
// real distance — because a chamfer that is secretly a small round is indistinguishable in a
// screenshot and different in every raking light.

TEST_CASE("a chamfered union fills the valley with a plane at forty-five degrees") {
    Field f;
    const f64 k = 0.20;
    // Two half spaces meeting at the origin: solid where x <= 0, and solid where y <= 0.
    const u32 west = f.plane({1, 0, 0}, 0.0);
    const u32 south = f.plane({0, 1, 0}, 0.0);
    const u32 cut = f.chamfer_unite({west, south}, k);

    // The chamfer face is the plane x + y = k. Its middle is on the surface...
    CHECK(at(f, cut, k * 0.5, k * 0.5, 0.0) == doctest::Approx(0.0).epsilon(1e-9));
    // ...and stepping away from it along its own normal is a true distance, which is what says the
    // face is a plane rather than a curve wearing one's name.
    for (f64 t = 0.01; t < 0.06; t += 0.01) {
        const f64 off = t * 0.7071067811865476;
        CHECK(at(f, cut, k * 0.5 + off, k * 0.5 + off, 0.0) == doctest::Approx(t));
        CHECK(at(f, cut, k * 0.5 - off, k * 0.5 - off, 0.0) == doctest::Approx(-t));
    }
    // Far along either face the chamfer has done nothing at all: a seam treatment that quietly
    // moved the faces would be a rounding by another name.
    CHECK(at(f, cut, 0.0, 3.0, 0.0) == doctest::Approx(0.0));
    CHECK(at(f, cut, 3.0, 0.0, 0.0) == doctest::Approx(0.0));
    CHECK(at(f, cut, -0.4, 3.0, 0.0) == doctest::Approx(-0.4));
    CHECK(at(f, cut, 3.0, -0.4, 0.0) == doctest::Approx(-0.4));
}

TEST_CASE("a chamfered intersection takes the arris off and leaves both faces standing") {
    Field f;
    const f64 k = 0.30;
    const u32 quadrant = f.chamfer_intersect({f.plane({1, 0, 0}, 0.0), f.plane({0, 1, 0}, 0.0)}, k);

    // The arris that was at the origin is now the plane x + y = -k.
    CHECK(at(f, quadrant, -k * 0.5, -k * 0.5, 0.0) == doctest::Approx(0.0).epsilon(1e-9));
    // Just inside the old corner is air, and it was stone before the chamfer.
    CHECK(at(f, quadrant, -0.01, -0.01, 0.0) > 0.0);
    Field plain;
    CHECK(plain.eval(plain.intersect({plain.plane({1, 0, 0}, 0.0), plain.plane({0, 1, 0}, 0.0)}),
                     {-0.01, -0.01, 0.0}) < 0.0);
    // A metre back along either face nothing has moved.
    CHECK(at(f, quadrant, -1.0, -0.5, 0.0) == doctest::Approx(-0.5));
    CHECK(at(f, quadrant, -0.5, -1.0, 0.0) == doctest::Approx(-0.5));
}

TEST_CASE("a chamfer of nought is the plain join, to the last bit") {
    // The dial has to reach zero and land exactly on the operation it is a variation of, or every
    // author sweeping it discovers a step at the end of the sweep.
    Field f;
    const u32 a = f.sphere({-0.4, 0, 0}, 0.8);
    const u32 b = f.box({0.5, 0, 0}, {0.5, 0.5, 0.5});
    const u32 plain = f.unite({a, b});
    const u32 zero = f.chamfer_unite({a, b}, 0.0);
    const u32 cut_plain = f.subtract({b, a});
    const u32 cut_zero = f.chamfer_subtract({b, a}, 0.0);
    for (int i = -6; i <= 6; ++i) {
        for (int j = -4; j <= 4; ++j) {
            const Vec3 p{i * 0.27, j * 0.31, 0.13};
            CHECK(f.eval(zero, p) == f.eval(plain, p));
            CHECK(f.eval(cut_zero, p) == f.eval(cut_plain, p));
        }
    }
}

TEST_CASE("a chamfer never leaves the plain join by more than the slack it declares") {
    // The whole reason `chamfer_min` carries a clamp, and the exact property `metric_slack` sells.
    //
    // A chamfer is `min(a, b)` — which moves at most a metre per metre — plus a correction. Left
    // unclamped that correction is unbounded INSIDE two overlapping shapes: at the origin below,
    // both spheres answer -0.7, and the raw forty-five degree term is -1.166 against a plain join
    // of -0.700. Settling reads `metric_slack` and believes it, so an unbounded correction would
    // let a block be called solid on a reading that does not bound what is in it — matter silently
    // missing, which is D613's class.
    //
    // Clamped, the deviation is exactly one chamfer's half-diagonal and never more, and the slack
    // is twice that: once because the reading may be that far out, once because the point asked
    // about may be that far in.
    Field f;
    const f64 k = 0.25;
    const f64 furthest = k * 0.7071067811865476;
    const u32 one = f.sphere({-0.3, 0, 0}, 1.0);
    const u32 two = f.sphere({0.3, 0, 0}, 1.0);
    const u32 plain = f.unite({one, two});
    const u32 joined = f.chamfer_unite({one, two}, k);

    const f64 slack = f.metric_slack(joined);
    CHECK(slack < Field::kInfiniteSlack);
    CHECK(slack == doctest::Approx(2.0 * furthest));

    // The interior point the clamp was written for, pinned to the number: -0.700 - 0.177, and not
    // the -1.166 the unclamped term would have given.
    CHECK(f.eval(plain, {0, 0, 0}) == doctest::Approx(-0.7));
    CHECK(f.eval(joined, {0, 0, 0}) == doctest::Approx(-0.7 - furthest));

    f64 worst = 0.0;
    for (int i = -80; i <= 80; ++i) {
        for (int j = -40; j <= 40; ++j) {
            for (int m = -3; m <= 3; ++m) {
                const Vec3 p{i * 0.02, j * 0.02, m * 0.09};
                worst = std::max(worst, std::abs(f.eval(joined, p) - f.eval(plain, p)));
            }
        }
    }
    CHECK(worst <= furthest + 1e-9);
    CHECK(worst == doctest::Approx(furthest));   // and it does reach it, so the bound is tight
}

TEST_CASE("the box round a chamfered union holds the matter the chamfer added") {
    // A chamfer puts stone where neither shape was, so a box built from the two shapes alone would
    // cut the new fillet off — and cutting a piece off a clip is the failure that says nothing.
    Field f;
    const f64 k = 0.3;
    const u32 joined = f.chamfer_unite(
        {f.box({-0.6, 0, 0}, {0.5, 0.5, 0.5}), f.box({0, -0.6, 0}, {0.5, 0.5, 0.5})}, k);
    f.build_bounds();
    const Field::Aabb box = f.bounds_of(joined);
    REQUIRE(!box.infinite());
    for (int i = -30; i <= 30; ++i) {
        for (int j = -30; j <= 30; ++j) {
            const Vec3 p{i * 0.05, j * 0.05, 0.0};
            if (f.eval(joined, p) > 0.0) continue;
            CHECK(p.x >= box.low.x - 1e-9);
            CHECK(p.x <= box.high.x + 1e-9);
            CHECK(p.y >= box.low.y - 1e-9);
            CHECK(p.y <= box.high.y + 1e-9);
        }
    }
}

// --- scatter: the thing that stops a surface reading as a lattice ------------------------------

TEST_CASE("a scatter that draws nothing IS a repeat, and is stored as one") {
    // The dial reaching zero has to land on the operation it varies, and here it has to land on it
    // in NODES too: a scatter of no jitter that cost a hash per axis per sample would be a tax on
    // every author who set the dial to nought and left it there.
    Field f;
    const u32 pebble = f.sphere({0, 0, 0}, 0.02);
    const u32 grid = f.repeat(pebble, {0.1, 0, 0.1}, {5, 0, 5});
    const u32 same = f.scatter(pebble, {0.1, 0, 0.1}, {5, 0, 5}, 0.0, 0.0);
    CHECK(f.node(same).op == forge::Op::Repeat);
    for (int i = -20; i <= 20; ++i) {
        for (int k = -20; k <= 20; ++k) {
            const Vec3 p{i * 0.031, 0.0, k * 0.027};
            CHECK(f.eval(same, p) == f.eval(grid, p));
        }
    }
}

TEST_CASE("a scatter moves, turns and resizes its copies, and a repeat does neither") {
    Field f;
    const u32 chip = f.box({0, 0, 0}, {0.03, 0.01, 0.02});
    const u32 grid = f.repeat(chip, {0.12, 0, 0.12}, {6, 0, 6});
    const u32 bed = f.scatter(chip, {0.12, 0, 0.12}, {6, 0, 6}, 0.4, 0.5);
    usize differing = 0;
    usize looked = 0;
    for (int i = -18; i <= 18; ++i) {
        for (int k = -18; k <= 18; ++k) {
            const Vec3 p{i * 0.037, 0.0, k * 0.041};
            ++looked;
            if (std::abs(f.eval(bed, p) - f.eval(grid, p)) > 1e-9) ++differing;
        }
    }
    // Not "some differ" but "most differ": a scatter whose draw was constant, or keyed on
    // something that does not change from cell to cell, would still move a handful of points.
    CHECK(differing * 2 > looked);
}

TEST_CASE("a scatter is the true distance to the nearest copy, not to the copy in this cell") {
    // The leaning-neighbour walk, and the reason `repeat` has one. Folding blindly reports the
    // distance back to the copy in this cell, which OVER-states — the dangerous direction, because
    // a sampler that believes it skips over the thing that is there. With a jitter the copy sits
    // off-centre in its cell by construction, so this is the case that fold was written for.
    //
    // Checked as a Lipschitz property rather than against a second copy of the placement
    // arithmetic, which would only prove the two agree with each other.
    Field f;
    const u32 pebble = f.sphere({0, 0, 0}, 0.03);
    const u32 bed = f.scatter(pebble, {0.1, 0, 0.1}, {8, 0, 8}, 0.5, 0.5);
    const f64 step = 0.004;
    f64 worst = 0.0;
    for (int i = -120; i <= 120; ++i) {
        for (int k = -30; k <= 30; ++k) {
            const Vec3 p{i * step, 0.0, k * step * 3.0};
            const Vec3 q{p.x + step, p.y, p.z};
            worst = std::max(worst, std::abs(f.eval(bed, q) - f.eval(bed, p)));
        }
    }
    CHECK(worst <= step + 1e-9);
}

TEST_CASE("the box round a scatter holds every copy, shrunk and spun and moved") {
    // Three things can walk a copy out of a box built round the child as authored, and the one
    // that looks harmless is the shrinking: a shape modelled away from its own origin moves TOWARD
    // the origin as it shrinks, so a box round the full-sized shape does not contain the small
    // ones. The child here is deliberately off-origin for that reason.
    Field f;
    const u32 chip = f.sphere({0.06, 0, 0}, 0.02);
    const u32 bed = f.scatter(chip, {0.25, 0, 0.25}, {3, 0, 3}, 0.5, 0.5);
    f.build_bounds();
    const Field::Aabb box = f.bounds_of(bed);
    REQUIRE(!box.infinite());
    usize solid = 0;
    for (int i = -100; i <= 100; ++i) {
        for (int j = -6; j <= 6; ++j) {
            for (int k = -100; k <= 100; ++k) {
                const Vec3 p{i * 0.011, j * 0.011, k * 0.011};
                if (f.eval(bed, p) > 0.0) continue;
                ++solid;
                REQUIRE(p.x >= box.low.x - 1e-9);
                REQUIRE(p.x <= box.high.x + 1e-9);
                REQUIRE(p.y >= box.low.y - 1e-9);
                REQUIRE(p.y <= box.high.y + 1e-9);
                REQUIRE(p.z >= box.low.z - 1e-9);
                REQUIRE(p.z <= box.high.z + 1e-9);
            }
        }
    }
    CHECK(solid > 0);   // a box that holds nothing holds everything
}

TEST_CASE("a scatter says whether its copies fit, and refuses to promise when they do not") {
    // The settling test, which is most of what the op is worth: a gravel bed that can settle a box
    // samples in a second and one that cannot is asked per voxel.
    Field f;
    const u32 pebble = f.sphere({0, 0, 0}, 0.02);
    const u32 roomy = f.scatter(pebble, {0.2, 0, 0.2}, {4, 0, 4}, 0.3, 0.5);
    f.build_bounds();
    CHECK(f.metric_slack(roomy) == doctest::Approx(0.0));

    // The same pebble in a cell it cannot fit in, jitter and all: no bounded number of neighbours
    // is enough, and there is nothing honest to say.
    Field tight;
    const u32 big = tight.sphere({0, 0, 0}, 0.05);
    const u32 crowded = tight.scatter(big, {0.11, 0, 0.11}, {4, 0, 4}, 0.5, 0.0);
    tight.build_bounds();
    CHECK(tight.metric_slack(crowded) >= Field::kInfiniteSlack);

    // And the pair the cost measurement in field.hpp was taken on, so the number quoted there
    // stays attached to the thing that produced it. The same 0.030 x 0.016 x 0.024 stone in a
    // 0.09 m cell and in a 0.15 m one: spun, it is 0.077 m across and travels up to 0.041 m, so
    // the small cell cannot hold it and the bed stops settling. That is the 1.4x-against-2.6x in
    // the header, and it is one number in the clip that moves it.
    Field narrow;
    const u32 stone = narrow.ellipsoid({0, 0, 0}, {0.030, 0.016, 0.024});
    const u32 packed = narrow.scatter(stone, {0.09, 0, 0.09}, {12, 0, 12}, 0.45, 0.5);
    narrow.build_bounds();
    CHECK(narrow.metric_slack(packed) >= Field::kInfiniteSlack);

    Field spaced;
    const u32 same_stone = spaced.ellipsoid({0, 0, 0}, {0.030, 0.016, 0.024});
    const u32 spread = spaced.scatter(same_stone, {0.15, 0, 0.15}, {12, 0, 12}, 0.45, 0.5);
    spaced.build_bounds();
    CHECK(spaced.metric_slack(spread) == doctest::Approx(0.0));
}

TEST_CASE("the mirror evaluator walks a scatter and a chamfer to the same number") {
    // R12b's demand, applied to the two new ops: the shader-shaped walk reaches the recursive
    // evaluator's answer or says it cannot. A scatter is the harder of the two, because it is the
    // only op that changes the point AND the answer by the same drawn number.
    Field f;
    const u32 pebble = f.sphere({0.01, 0, 0}, 0.03);
    const u32 bed = f.scatter(pebble, {0.15, 0, 0.15}, {3, 0, 3}, 0.4, 0.5);
    const u32 arris = f.chamfer_intersect(
        {f.box({0, 0.4, 0}, {0.3, 0.3, 0.3}), f.sphere({0, 0.4, 0}, 0.42)}, 0.05);
    const u32 root = f.chamfer_unite({bed, arris}, 0.02);
    f.build_bounds();

    forge::Op missing = forge::Op::Constant;
    REQUIRE(f.mirror_covers(root, &missing));
    for (int i = -12; i <= 12; ++i) {
        for (int j = -6; j <= 10; ++j) {
            for (int k = -12; k <= 12; ++k) {
                const Vec3 p{i * 0.048, j * 0.057, k * 0.051};
                f64 mine = 0.0;
                REQUIRE(f.mirror_eval(root, p, mine));
                CHECK(mine == f.eval(root, p));
            }
        }
    }
}

// --- the stretch: a grain that runs one way ----------------------------------------------------

TEST_CASE("a stretched grain is the same grain, and a stretch of one changes nothing") {
    // Bit-identical, not approximately: every node in every clip in the repository was built
    // without a stretch, and a grain that moved by a rounding would move every displaced surface
    // in the building.
    Field f;
    const u32 plain = f.fbm(0.3, 4, 0.5, 2.0, 3);
    const u32 ones = f.fbm(0.3, 4, 0.5, 2.0, 3, {1.0, 1.0, 1.0});
    const u32 grains = f.cells(0.4, 2);
    const u32 grains_one = f.cells(0.4, 2, {1.0, 1.0, 1.0});
    for (int i = -9; i <= 9; ++i) {
        for (int j = -9; j <= 9; ++j) {
            const Vec3 p{i * 0.13, j * 0.17, 0.07};
            CHECK(f.eval(ones, p) == f.eval(plain, p));
            CHECK(f.eval(grains_one, p) == f.eval(grains, p));
        }
    }
}

TEST_CASE("a grain stretched along one axis varies less along it") {
    // The whole point: bark runs up a trunk, rain runs down a wall, a saw runs across a stone.
    // Measured as total variation along each axis rather than by eye, because "it looks streaky"
    // is not a thing a test can hold.
    Field f;
    const u32 bark = f.fbm(0.1, 4, 0.5, 2.0, 11, {1.0, 8.0, 1.0});
    const auto walk = [&](Vec3 from, Vec3 along) {
        f64 total = 0.0;
        f64 last = f.eval(bark, from);
        for (int i = 1; i <= 400; ++i) {
            const Vec3 p = from + along * (i * 0.005);
            const f64 now = f.eval(bark, p);
            total += std::abs(now - last);
            last = now;
        }
        return total;
    };
    const f64 across = walk({0.03, 0.11, 0.07}, {1, 0, 0});
    const f64 up = walk({0.03, 0.11, 0.07}, {0, 1, 0});
    CHECK(up * 3.0 < across);
}

TEST_CASE("a stretched grain still says how far it can swing, so a displacement can be bounded") {
    // `value_range` is what turns a displacement into a box. The stretch changes where the grain
    // is read and not what it can reach, so the range must be exactly what it was — a stretch that
    // quietly returned "unknown" would give every clip using one an infinite skip slack.
    Field f;
    const u32 bark = f.fbm(0.1, 4, 0.5, 2.0, 11, {1.0, 8.0, 1.0});
    f64 low = 0.0, high = 0.0;
    REQUIRE(f.value_range(bark, low, high));
    CHECK(low == doctest::Approx(-1.0));
    CHECK(high == doctest::Approx(1.0));

    const u32 trunk = f.displace(f.cylinder({0, 0, 0}, 0.2, 1.0, 1), bark, 0.02);
    f.build_bounds();
    const Field::Aabb box = f.bounds_of(trunk);
    REQUIRE(!box.infinite());
    CHECK(box.high.x == doctest::Approx(0.22));
}

TEST_CASE("a copy that hangs out of its own cell is refused a promise, however wide the cell") {
    // Two holes, both found by demanding that a field with a slack of nought really is a distance.
    //
    // The first was in `metric_slack` and not in the box, which is why it would have been
    // invisible: the bed looked right and sampled right, and the sampler was being told it could
    // settle a whole block from one reading when it could not. A copy is scaled about its OWN
    // origin, so a child modelled away from that origin walks toward it as it shrinks. The pebble
    // below is a 0.04 m ball centred on x = 0.06, so its box spans [0.04, 0.08]; at half size it
    // spans [0.02, 0.04], and the two together span [0.02, 0.08] — 0.06 m of room for a 0.04 m
    // stone. That is `scatter_footprint`, written once and read by the box and by this.
    //
    // The second was the test itself. "Narrower than a cell" is what `repeat` asks and it is not
    // enough here: `repeat`'s copies all sit the same way in their cells, a scatter's are placed
    // independently, and an independently placed copy that hangs over its own cell edge can be
    // beaten by one two cells away that jittered toward the point. Widening the cell does not fix
    // it, which is the surprising part and the reason this test widens the cell and still expects
    // a refusal: 0.06 m of stone with 0.075 m of travel reaches 0.1175 m out of a cell that only
    // owns 0.075 m of it. Measured before the fix, that bed moved 0.0098 m over a step of 0.003.
    Field crowded;
    const u32 offset_pebble = crowded.sphere({0.06, 0, 0}, 0.02);
    const u32 tight = crowded.scatter(offset_pebble, {0.09, 0, 0.09}, {4, 0, 4}, 0.5, 0.0);
    crowded.build_bounds();
    CHECK(crowded.metric_slack(tight) >= Field::kInfiniteSlack);

    Field wider;
    const u32 same_pebble = wider.sphere({0.06, 0, 0}, 0.02);
    const u32 loose = wider.scatter(same_pebble, {0.15, 0, 0.15}, {4, 0, 4}, 0.5, 0.0);
    wider.build_bounds();
    CHECK(wider.metric_slack(loose) >= Field::kInfiniteSlack);

    // The same stone modelled where it belongs — on its own origin — fits, and the field there
    // really is a distance, which is what the slack of nought promises.
    Field centred;
    const u32 on_origin = centred.sphere({0, 0, 0}, 0.02);
    const u32 bed = centred.scatter(on_origin, {0.15, 0, 0.15}, {4, 0, 4}, 0.5, 0.5);
    centred.build_bounds();
    CHECK(centred.metric_slack(bed) == doctest::Approx(0.0));

    const f64 step = 0.003;
    f64 worst = 0.0;
    for (int i = -160; i <= 160; ++i) {
        for (int k = -20; k <= 20; ++k) {
            const Vec3 p{i * step, 0.0, k * step * 4.0};
            const Vec3 q{p.x + step, p.y, p.z};
            worst = std::max(worst, std::abs(centred.eval(bed, q) - centred.eval(bed, p)));
        }
    }
    CHECK(worst <= step + 1e-9);
}
