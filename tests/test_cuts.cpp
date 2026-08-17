// Does this cut remove exactly what it should?
//
// Every other test in this directory asks whether a shape is the shape it says it is. This one
// asks the question that has actually gone wrong in this repository twice, and both times the
// only symptom was a picture somebody looked at weeks later:
//
//   D608  Every room's void was subtracted from *everything*, so it deleted the sconces, benches
//         and statues standing IN that room. `clips/facility.clip` around line 105 is the
//         best description of the failure there is: a room's void is the AIR OF A ROOM and a
//         fitting stands in a room's air by definition, while a door's void is an OPENING THROUGH
//         A WALL and nothing may stand in one. Two different kinds of thing, subtracted in one
//         line, and the result was eight sconces with no bowl and no flame, two missing statues,
//         and the whole indoor emissive load of the building gone. No error, no warning, no
//         failing test — a flat dark rectangle on a wall and the player saying "i cant tell what
//         they are".
//
//   The windows. `clips/facility/windows.clip` cuts its openings "1.80 m (4 M) deep from that
//         face, which is more than any sane outer wall, so a window goes through whatever
//         thickness walls.clip settles on". That is a deliberate over-reach with a written-down
//         assumption underneath it, and the assumption is about a file that is allowed to change.
//         The day something stands 1.80 m behind an outer wall, every window on that front bores
//         a hole through it, and again nothing says so.
//
// Neither of those is a bug in `Op::Difference`. `max(d, -e)` is right and has been right the
// whole time. They are bugs in WHAT WAS SUBTRACTED FROM WHAT, which is the only kind of cut bug
// there has ever been here, and the only way to catch one is to state what the cut is supposed to
// remove and check that it removed that and no more.
//
// # What these tests assert, and why it is three things and not one
//
// A total volume cannot tell over-cutting apart from under-cutting somewhere else: a cut that
// eats a bench and misses half a doorway can weigh exactly right. So every case here asserts
//
//   the census    every sample point in a lattice, against a statement of what the cut means
//                 written out in terms of its OPERANDS' own signs rather than in terms of the
//                 difference — which is the mathematical definition of the set operation, and so
//                 is independent of the code under test in the way that matters;
//   the volume    a count, so that a cut which is right everywhere it was looked at and absent
//                 from a region nobody wrote a predicate for still fails; and
//   the points    named coordinates with the geometry worked out by hand, so a failure says
//                 "the bore is solid at (0.45, 0, 0)" rather than "1,304 points disagree".
//
// And every failure message names WHICH cut and WHERE. A count on its own is not something
// anybody can act on.
//
// # Why over the field rather than over the sampler
//
// Because a cut is a property of the expression and not of the resolution. `test_sample.cpp`
// already holds the sampler to asking every voxel; putting these there as well would test the
// sampler twice and the subtraction once. Here there are no voxels, no jobs, no files: build the
// field, sample it on a lattice, assert.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "forge/field.hpp"

using namespace ws;
using namespace ws::forge;

namespace {

constexpr f64 kPi = 3.14159265358979323846;

// Negative is matter. Said once, so that a test reading `solid(...)` cannot quietly mean the
// opposite of the one beside it.
bool solid(const Field& f, u32 node, Vec3 p) { return f.eval(node, p) < 0.0; }

// Where a failure happened, in the units the clip was authored in. Formatted rather than streamed
// because `Vec3` has no operator<< and giving it one would be a change to `src/`.
std::string spot(Vec3 p) {
    char buffer[96];
    std::snprintf(buffer, sizeof buffer, "(%.3f, %.3f, %.3f)", p.x, p.y, p.z);
    return std::string(buffer);
}

// A lattice of sample points over a box, taken at the CENTRE of each cell.
//
// At the centre for the same reason `SampleSettings::sample_at_centre` defaults to true, and for
// one more that matters here: every surface in every case below sits at a whole number of steps
// from the lattice's own low corner, so a sample point is always half a step from the nearest
// boundary and NO assertion in this file turns on which way a tie broke. That is deliberate. A
// test whose answer depends on whether `-0.0` compares less than zero is a test that will one day
// fail on a different compiler for no reason anybody can find.
struct Lattice {
    Vec3 low{0, 0, 0};
    f64 step = 0.1;
    i32 count[3]{1, 1, 1};

    static Lattice over(Vec3 low, Vec3 high, f64 step) {
        Lattice g;
        g.low = low;
        g.step = step;
        g.count[0] = static_cast<i32>(std::llround((high.x - low.x) / step));
        g.count[1] = static_cast<i32>(std::llround((high.y - low.y) / step));
        g.count[2] = static_cast<i32>(std::llround((high.z - low.z) / step));
        return g;
    }

    Vec3 at(i32 x, i32 y, i32 z) const {
        return {low.x + (static_cast<f64>(x) + 0.5) * step,
                low.y + (static_cast<f64>(y) + 0.5) * step,
                low.z + (static_cast<f64>(z) + 0.5) * step};
    }
    usize points() const {
        return static_cast<usize>(count[0]) * static_cast<usize>(count[1]) *
               static_cast<usize>(count[2]);
    }
    f64 cell() const { return step * step * step; }
};

using Where = std::function<bool(Vec3)>;

// Walk every point of the lattice. Split out so that the counting helpers below cannot walk it
// three different ways and disagree about which points there are.
template <typename Visit>
void walk(const Lattice& g, Visit visit) {
    for (i32 z = 0; z < g.count[2]; ++z) {
        for (i32 y = 0; y < g.count[1]; ++y) {
            for (i32 x = 0; x < g.count[0]; ++x) visit(g.at(x, y, z));
        }
    }
}

// How many sample points hold matter — the volume, in points rather than in cubic metres, because
// a count is exact and a volume is a count multiplied by something.
usize matter_points(const Field& f, u32 node, const Lattice& g) {
    usize n = 0;
    walk(g, [&](Vec3 p) { if (solid(f, node, p)) ++n; });
    return n;
}

usize points_where(const Lattice& g, const Where& where) {
    usize n = 0;
    walk(g, [&](Vec3 p) { if (where(p)) ++n; });
    return n;
}

f64 volume(const Field& f, u32 node, const Lattice& g) {
    return static_cast<f64>(matter_points(f, node, g)) * g.cell();
}

struct Tally {
    usize judged = 0;   // points the predicate named at all
    usize wrong = 0;    // of those, how many the field disagreed with
    Vec3 first{};       // and the first one, so the message can point at it
};

Tally judge(const Field& f, u32 node, const Lattice& g, const Where& where, bool expect_matter) {
    Tally t;
    walk(g, [&](Vec3 p) {
        if (!where(p)) return;
        ++t.judged;
        if (solid(f, node, p) == expect_matter) return;
        if (t.wrong == 0) t.first = p;
        ++t.wrong;
    });
    return t;
}

// Every point the predicate names must be matter (or air), and the message says how many were not,
// out of how many were looked at, and where the first one is.
//
// The `judged > 0` line is trap 10 written into the instrument rather than left to each caller: a
// census that looked at nothing passes, and a difference that deleted the entire world agrees
// perfectly with a predicate that happened to name no points at all. Every helper here has to be
// able to say it ran.
void must_be(const Field& f, u32 node, const Lattice& g, const Where& where, bool expect_matter,
             const char* what) {
    const Tally t = judge(f, node, g, where, expect_matter);
    // Composed into ONE std::string rather than streamed into INFO a piece at a time, and that is
    // not a matter of taste. This repository's doctest is built without
    // DOCTEST_CONFIG_TREAT_CHAR_STAR_AS_STRING, so `toString(const char*)` is never declared and a
    // bare string literal handed to INFO falls through to the generic pointer stringifier: the
    // first draft of this file printed
    //
    //     logged: 1: 1288 of 3600 sample points that should be 1, the first at (-0.350, ...)
    //
    // where the two `1`s are the name of the cut and the direction it failed in. The counts and
    // the coordinate survived because numbers and `std::string` have their own overloads. A
    // message that cannot say which cut is a message nobody can act on, which is the entire point
    // of this file, so the whole line is built as a string first.
    const std::string message =
        std::string(what) + ": " +
        (expect_matter ? "THE CUT REACHED TOO FAR -- " : "THE CUT DID NOT REACH -- ") +
        std::to_string(t.wrong) + " of " + std::to_string(t.judged) +
        " sample points that should be " +
        (expect_matter ? "matter are air" : "air hold matter") + ", the first at " +
        spot(t.first);
    INFO(message);
    REQUIRE(t.judged > 0);
    REQUIRE(t.wrong == 0);
}

// The whole lattice against a statement of what the shape means: matter exactly where `want` says
// so, air exactly everywhere else. Both halves, because either on its own passes for a cut that
// removed nothing or for one that removed everything.
void must_cut_exactly(const Field& f, u32 node, const Lattice& g, const Where& want,
                      const char* what) {
    must_be(f, node, g, want, true, what);
    must_be(f, node, g, [&want](Vec3 p) { return !want(p); }, false, what);
}

// The shapes that recur, so that a case can say what it is about instead of re-deriving a block
// with a hole in it. `block` is two metres on a side about the origin; `bore` is a half-metre
// sphere at its centre, so the carved shape's own distance in the bore is a number that can be
// written down: half a metre less however far out you have moved.
struct Carved {
    Field f;
    u32 block = 0;
    u32 bore = 0;
    u32 shape = 0;

    Carved() {
        block = f.box({0, 0, 0}, {1, 1, 1});
        bore = f.sphere({0, 0, 0}, 0.5);
        shape = f.subtract({block, bore});
    }
    // The definition of the difference, in terms of the two operands' own signs.
    Where want() const {
        return [this](Vec3 p) { return solid(f, block, p) && !solid(f, bore, p); };
    }
};

f64 sphere_volume(f64 r) { return 4.0 / 3.0 * kPi * r * r * r; }

}  // namespace

// ================================================================================================
// THE PLAIN CASE, WHICH EVERYTHING ELSE IS A COMPLICATION OF
// ================================================================================================

TEST_CASE("a difference removes what is inside the cutter and leaves what is outside it") {
    // The whole claim in one field: matter inside the subtracted shape is gone, matter outside it
    // is untouched, and the boundary between the two is where the cutter's own surface is and not
    // a sample point either side of it.
    Carved c;
    const Lattice g = Lattice::over({-1.5, -1.5, -1.5}, {1.5, 1.5, 1.5}, 0.05);

    must_cut_exactly(c.f, c.shape, g, c.want(), "a sphere bored out of a block");

    // The volume, twice. First exactly, against the two operands counted on the same lattice —
    // which is an integer identity and cannot drift with the step size — and then against the
    // arithmetic, so the lattice itself is anchored to something real.
    const usize whole = matter_points(c.f, c.block, g);
    const usize removed = points_where(g, [&](Vec3 p) {
        return solid(c.f, c.block, p) && solid(c.f, c.bore, p);
    });
    CHECK(matter_points(c.f, c.shape, g) == whole - removed);
    CHECK(removed > 0);
    CHECK(volume(c.f, c.shape, g) ==
          doctest::Approx(8.0 - sphere_volume(0.5)).epsilon(0.01));

    // Named points, worked out by hand, so that a failure says where rather than how many.
    CHECK(c.f.eval(c.shape, {0.00, 0, 0}) > 0.0);    // the middle of the bore is air
    CHECK(c.f.eval(c.shape, {0.45, 0, 0}) > 0.0);    // still air five centimetres from its wall
    CHECK(c.f.eval(c.shape, {0.55, 0, 0}) < 0.0);    // and matter five centimetres the other side
    CHECK(c.f.eval(c.shape, {0.99, 0, 0}) < 0.0);    // the block's own face has not moved
    CHECK(c.f.eval(c.shape, {1.01, 0, 0}) > 0.0);

    // And the cut's surface is a real distance, not merely a sign. Everything in the last section
    // of this file — shell, round, offset, displace — moves a surface by a number of metres, and
    // that only means anything if the difference reports metres in the first place.
    CHECK(c.f.eval(c.shape, {0, 0, 0}) == doctest::Approx(0.5));
    CHECK(c.f.eval(c.shape, {0.2, 0, 0}) == doctest::Approx(0.3));
}

TEST_CASE("a cut whose operand is nowhere near removes nothing, and a cut that lands is seen") {
    // Half of this test is worthless on its own and that is the point.
    //
    // "Subtracting something the shape does not touch changes nothing" is satisfied perfectly by a
    // difference that never removes anything at all — by a `subtract` that returned its first
    // operand, by a cull that skipped every cutter, by an author who wrote the cut and never
    // wired it in. So the same assertion is made twice over: once with a cutter that misses, where
    // the count must not move by a single point, and once with a cutter that lands, where it must
    // move by exactly the overlap. The second is the arm that catches a cut that silently missed.
    Field f;
    const u32 block = f.box({0, 0, 0}, {1, 1, 1});
    const u32 elsewhere = f.sphere({5, 0, 0}, 0.5);
    const u32 landing = f.sphere({0.9, 0, 0}, 0.5);

    const u32 missed = f.subtract({block, elsewhere});
    const u32 hit = f.subtract({block, landing});

    const Lattice g = Lattice::over({-1.5, -1.5, -1.5}, {1.5, 1.5, 1.5}, 0.05);
    const usize whole = matter_points(f, block, g);
    REQUIRE(whole > 0);

    // The miss: not "about the same", the same.
    must_cut_exactly(f, missed, g, [&](Vec3 p) { return solid(f, block, p); },
                     "a cutter five metres away");
    CHECK(matter_points(f, missed, g) == whole);

    // The landing: exactly the overlap and nothing else.
    const usize overlap = points_where(g, [&](Vec3 p) {
        return solid(f, block, p) && solid(f, landing, p);
    });
    CHECK(overlap > 0);
    CHECK(matter_points(f, hit, g) == whole - overlap);
    must_cut_exactly(f, hit, g,
                     [&](Vec3 p) { return solid(f, block, p) && !solid(f, landing, p); },
                     "a cutter biting the block's face");

    // And the two are not the same field, which is what says the count above was capable of moving.
    CHECK(matter_points(f, hit, g) < matter_points(f, missed, g));
}

TEST_CASE("a difference is not a union: a cutter that swallows its target leaves nothing") {
    // Three degenerate cuts that a subtraction with its operands the wrong way round, or with a
    // `min` where the `max` should be, gets wrong in a way no ordinary case notices.
    Field f;
    const u32 small = f.sphere({0, 0, 0}, 0.4);
    const u32 large = f.sphere({0, 0, 0}, 1.0);
    const Lattice g = Lattice::over({-1.5, -1.5, -1.5}, {1.5, 1.5, 1.5}, 0.05);

    // Subtracted from itself: nothing survives anywhere.
    CHECK(matter_points(f, f.subtract({small, small}), g) == 0);
    // Swallowed: nothing survives either.
    CHECK(matter_points(f, f.subtract({small, large}), g) == 0);
    // The other way round is a hollow shell of matter, and specifically is NOT empty — otherwise
    // the two checks above pass against a difference that always returns nothing.
    const usize hollow = matter_points(f, f.subtract({large, small}), g);
    CHECK(hollow > 0);
    CHECK(hollow == matter_points(f, large, g) - matter_points(f, small, g));

    // One operand is the operand. `Field::combine` folds a list into a chain of four-child nodes
    // and a list of one has no chain to fold; a difference that invented an empty second operand
    // here would subtract a constant from everything.
    CHECK(f.subtract({large}) == large);
}

// ================================================================================================
// THE WINDOW BUG, IN MINIATURE
// ================================================================================================

TEST_CASE("a cut sized to one wall does not reach the wall standing behind it") {
    // `clips/facility/windows.clip`: "the openings are cut 1.80 m (4 M) deep from that face, which
    // is more than any sane outer wall, so a window goes through whatever thickness walls.clip
    // settles on. If that wall ends up thicker than 1.80 the openings stop short and this file has
    // to change: it is the single assumption it makes about anybody else."
    //
    // That is one half of the risk written down. The other half is not: a cut that reaches 1.80 m
    // to be sure of getting through 0.30 m of wall also reaches 1.50 m past it, into whatever is
    // there. Here that is an inner partition a metre behind the front, and the two arms below are
    // the same building with the same window cut to two different depths.
    Field f;
    const u32 front = f.box({0, 0, 0.15}, {2.0, 1.5, 0.15});    // z 0.00 .. 0.30, the outer wall
    const u32 behind = f.box({0, 0, 1.15}, {2.0, 1.5, 0.15});   // z 1.00 .. 1.30, a partition
    const u32 built = f.unite({front, behind});

    // Through the wall and 0.10 clear on each side: the depth the wall actually is.
    const u32 sized = f.box({0, 0, 0.15}, {0.4, 0.6, 0.25});    // z -0.10 .. 0.40
    // And the facility's own number, from a face at z = 0: 1.80 m deep.
    const u32 reaching = f.box({0, 0, 0.85}, {0.4, 0.6, 0.95}); // z -0.10 .. 1.80

    const u32 right = f.subtract({built, sized});
    const u32 wrong = f.subtract({built, reaching});

    const Lattice g = Lattice::over({-2.5, -2.0, -0.5}, {2.5, 2.0, 2.0}, 0.1);

    // The whole shape, against what the sized cut means.
    must_cut_exactly(f, right, g,
                     [&](Vec3 p) {
                         return (solid(f, front, p) || solid(f, behind, p)) && !solid(f, sized, p);
                     },
                     "a window cut to the thickness of its own wall");

    // The assertion this test exists for, said on its own so that its failure message names the
    // wall behind rather than a count over the whole building.
    must_be(f, right, g, [&](Vec3 p) { return solid(f, behind, p); }, true,
            "the partition standing a metre behind the window");
    CHECK(matter_points(f, right, g) ==
          matter_points(f, built, g) -
              points_where(g, [&](Vec3 p) { return solid(f, front, p) && solid(f, sized, p); }));

    // The opening really is an opening: a window that removes nothing passes every check above.
    must_be(f, right, g, [&](Vec3 p) { return solid(f, sized, p); }, false,
            "the window opening itself");

    // The other arm, which is the bug: the over-deep cut bores through the partition too, and it
    // takes exactly the points where the two overlap. Asserting the damage rather than merely its
    // absence is what says the check above can see anything at all.
    const usize bored = points_where(g, [&](Vec3 p) {
        return solid(f, behind, p) && solid(f, reaching, p);
    });
    CHECK(bored > 0);
    CHECK(matter_points(f, wrong, g) == matter_points(f, right, g) - bored);
    const Tally hole = judge(f, wrong, g, [&](Vec3 p) { return solid(f, behind, p); }, true);
    CHECK(hole.wrong == bored);
}

// ================================================================================================
// D608: THE TWO KINDS OF VOID
// ================================================================================================

TEST_CASE("a room's void spares the furniture standing in it, and a doorway's void does not") {
    // The shape of D608, built small enough to assert every point of.
    //
    // `clips/facility.clip`: "A room's void is correctly written as *the air of that room less that
    // room's own stone*, which is what stops another fragment leaving a wall in somebody's room;
    // but a fitting is BY DEFINITION a thing standing in somebody's air, and it went with it."
    // And then the correction to the correction, reported from playing within a day: "`union {
    // hollowed part_fittings }` put the furniture back after EVERY void, so a sconce standing in a
    // doorway was no longer cut by the doorway ... a bronze bracket with its bowl alight, hanging
    // in mid-air in front of a closed door leaf."
    //
    // So there are two failures here and they pull in opposite directions, which is exactly why
    // one of them was introduced by the fix for the other. Both are asserted.
    Field f;

    // The room: a block with its air taken out, and one pilaster of the room's own stone standing
    // proud of the wall into that air.
    const u32 block = f.box({0, 0, 0}, {2.4, 1.9, 2.4});
    const u32 air = f.box({0, 0, 0}, {2.0, 1.5, 2.0});
    const u32 pilaster = f.box({1.9, 0, 0}, {0.2, 1.5, 0.4});
    const u32 part_room = f.unite({block, pilaster});
    // The air of the room, LESS the room's own stone. This is the construction the facility uses
    // and the reason it uses it: without the subtraction, the room's void would eat the room's own
    // pilaster the moment any other fragment's void overlapped it.
    const u32 void_room = f.subtract({air, pilaster});

    // A doorway through the minus-z wall, running 0.20 out past the wall and 0.20 into the room.
    const u32 void_door = f.box({0, -0.7, -2.2}, {0.5, 0.8, 0.4});

    // The furniture. A bench on the floor against the far wall, biting 0.10 into it — the
    // "0.09 m of backplate" that was the only thing left of the sconces — and a lamp standing in
    // the doorway, which is the fitting that must still be cut.
    const u32 bench = f.box({1.75, -1.2, 0.9}, {0.35, 0.3, 0.3});
    const u32 lamp = f.box({0, -0.3, -2.0}, {0.2, 0.2, 0.3});
    const u32 fittings = f.unite({bench, lamp});

    // What the facility does now: the rooms are carved out of the building, the openings are cut
    // through both the building and the furniture, and the furniture meets the building afterwards.
    const u32 right = f.unite({f.subtract({part_room, void_room, void_door}),
                               f.subtract({fittings, void_door})});
    // What it did before D608: every void subtracted from everything, furniture included.
    const u32 wrong = f.subtract({f.unite({part_room, fittings}), void_room, void_door});

    const Lattice g = Lattice::over({-3.0, -2.5, -3.0}, {3.0, 2.5, 3.0}, 0.1);

    must_cut_exactly(f, right, g,
                     [&](Vec3 p) {
                         const bool built = solid(f, part_room, p) && !solid(f, void_room, p) &&
                                            !solid(f, void_door, p);
                         const bool stood = solid(f, fittings, p) && !solid(f, void_door, p);
                         return built || stood;
                     },
                     "a room carved, a doorway cut, and the furniture put in afterwards");

    SUBCASE("the bench standing in the room's air survives the room being hollowed") {
        // D608 itself. Every point of the bench is matter.
        must_be(f, right, g, [&](Vec3 p) { return solid(f, bench, p); }, true,
                "the bench standing in the room (D608)");

        // And the failure it replaced, so that this test can tell the fix from the bug rather than
        // merely from a passing run: under the old assembly the bench keeps only the 0.10 buried in
        // the wall, and loses everything that was standing in the air.
        const usize buried = points_where(g, [&](Vec3 p) {
            return solid(f, bench, p) && !solid(f, air, p);
        });
        const usize standing = points_where(g, [&](Vec3 p) {
            return solid(f, bench, p) && solid(f, air, p);
        });
        CHECK(buried > 0);
        CHECK(standing > 0);
        const Tally lost = judge(f, wrong, g, [&](Vec3 p) { return solid(f, bench, p); }, true);
        CHECK(lost.wrong == standing);
        CHECK(lost.judged - lost.wrong == buried);
    }

    SUBCASE("the room really is hollow, which is what the void was subtracted for") {
        // The opposite failure, and the one a careless fix for D608 produces: stop subtracting the
        // room's void and the bench survives perfectly inside a solid block of stone.
        must_be(f, right, g,
                [&](Vec3 p) {
                    return solid(f, air, p) && !solid(f, pilaster, p) && !solid(f, bench, p) &&
                           !solid(f, lamp, p);
                },
                false, "the air of the room");
        // And the room's own stone is still standing in it. This is the clause the facility's
        // `difference { room_air room_stone }` exists for.
        must_be(f, right, g, [&](Vec3 p) { return solid(f, pilaster, p); }, true,
                "the room's own pilaster, which its void must not eat");
    }

    SUBCASE("a doorway is still an opening, and a lamp standing in one is still cut") {
        // The regression that followed the D608 fix by a day: `union { hollowed part_fittings }`
        // put the furniture back after every void, so the doorway stopped cutting the lamp.
        must_be(f, right, g, [&](Vec3 p) { return solid(f, void_door, p); }, false,
                "the doorway, which nothing may stand in");
        // Not vacuous: the lamp really does reach into the doorway, and the part of it that does
        // not is still there.
        CHECK(points_where(g, [&](Vec3 p) { return solid(f, lamp, p) && solid(f, void_door, p); }) >
              0);
        must_be(f, right, g,
                [&](Vec3 p) { return solid(f, lamp, p) && !solid(f, void_door, p); }, true,
                "the part of the lamp that is not in the doorway");
    }
}

// ================================================================================================
// NESTING AND TRANSFORMS: THE CUT HAS TO ARRIVE IN EVERY COPY
// ================================================================================================

TEST_CASE("a cut inside a translate moves with the shape it was written against") {
    // The failure this stands guard over is a transform applied to the target and not to the
    // cutter — the hole stays at the origin while the block walks away, which on a building reads
    // as one bay with two windows and another with none.
    Carved c;
    const Vec3 by{1.3, -0.7, 0.4};
    const u32 moved = c.f.translate(c.shape, by);
    const Lattice g = Lattice::over({-1.0, -2.5, -1.5}, {3.5, 1.0, 2.0}, 0.05);

    must_cut_exactly(c.f, moved, g,
                     [&](Vec3 p) {
                         const Vec3 q = p - by;
                         return solid(c.f, c.block, q) && !solid(c.f, c.bore, q);
                     },
                     "a bore inside a translate");

    // The bore is where the block went and nowhere else.
    CHECK(c.f.eval(moved, by) > 0.0);                 // air at the moved centre
    CHECK(c.f.eval(moved, {0, 0, 0}) > 0.0);          // and no ghost hole left at the origin
    CHECK(c.f.eval(moved, by + Vec3{0.7, 0, 0}) < 0.0);
    CHECK(matter_points(c.f, moved, g) == matter_points(c.f, c.shape, g));
}

TEST_CASE("a cut inside a rotate turns with the shape, and turns the way the shape does") {
    // A quarter turn about y sends +x to -z: `Field::eval` turns the POINT by the negated angle,
    // so the SHAPE turns by the positive one, and `build_bounds` says the same thing in its own
    // comment. A rotation applied on the way out instead of on the way in, or applied to the
    // target and not to the cut, lands the hole on the wrong face — and a wrong face is something
    // nobody notices on a shape with four of them.
    //
    // The reference is written out as the geometry the turn is supposed to produce, in absolute
    // coordinates, rather than by turning the point back again — which would only assert that the
    // rotation agrees with itself.
    Field f;
    const u32 bar = f.box({0, 0, 0}, {1.0, 0.4, 0.4});
    const u32 hole = f.sphere({0.6, 0, 0}, 0.25);
    const u32 carved = f.subtract({bar, hole});
    const u32 turned = f.rotate(carved, {0, 0.25, 0});

    // After the turn: the bar lies along z, and the hole that was at x = +0.6 is at z = -0.6.
    const u32 turned_bar = f.box({0, 0, 0}, {0.4, 0.4, 1.0});
    const u32 turned_hole = f.sphere({0, 0, -0.6}, 0.25);

    const Lattice g = Lattice::over({-1.5, -1.5, -1.5}, {1.5, 1.5, 1.5}, 0.05);
    must_cut_exactly(f, turned, g,
                     [&](Vec3 p) { return solid(f, turned_bar, p) && !solid(f, turned_hole, p); },
                     "a bore inside a quarter turn about y");

    CHECK(f.eval(turned, {0, 0, -0.6}) > 0.0);   // the hole arrived here
    CHECK(f.eval(turned, {0.6, 0, 0}) > 0.0);    // and is no longer here -- outside the turned bar
    CHECK(f.eval(turned, {0, 0, 0.6}) < 0.0);    // solid on the opposite end, so it turned one way
    CHECK(matter_points(f, turned, g) == matter_points(f, carved, g));
}

TEST_CASE("a cut inside a mirror appears in both halves, not only the one it was drawn in") {
    // The most direct form of "not just the first instance". A fold is one node and it produces
    // two copies of everything below it; a cut that survives the fold in one copy and not the
    // other is the classic symmetrical building with a window on one side of the door.
    Field f;
    const u32 pier = f.box({1.2, 0, 0}, {0.6, 0.4, 0.4});
    const u32 window = f.sphere({1.5, 0, 0}, 0.2);
    const u32 unit = f.subtract({pier, window});
    const u32 both = f.mirror(unit, 0);

    // Written out as two piers with two windows, in absolute coordinates.
    const u32 left_pier = f.box({-1.2, 0, 0}, {0.6, 0.4, 0.4});
    const u32 left_window = f.sphere({-1.5, 0, 0}, 0.2);

    const Lattice g = Lattice::over({-2.5, -1.0, -1.0}, {2.5, 1.0, 1.0}, 0.05);
    must_cut_exactly(f, both, g,
                     [&](Vec3 p) {
                         return (solid(f, pier, p) && !solid(f, window, p)) ||
                                (solid(f, left_pier, p) && !solid(f, left_window, p));
                     },
                     "a window inside a mirror");

    CHECK(f.eval(both, {1.5, 0, 0}) > 0.0);    // the window in the half it was drawn in
    CHECK(f.eval(both, {-1.5, 0, 0}) > 0.0);   // and in the half the fold made
    CHECK(f.eval(both, {1.0, 0, 0}) < 0.0);
    CHECK(f.eval(both, {-1.0, 0, 0}) < 0.0);
    CHECK(matter_points(f, both, g) == 2 * matter_points(f, unit, g));
}

TEST_CASE("a cut inside a repeat is bored through every copy and the count is exact") {
    // A colonnade, a balustrade, a row of slats: the shape a repeat is written for, and the shape
    // where "the first one is right" is the easiest wrong answer to ship. `Op::Repeat` folds the
    // point into its nearest cell and then takes the minimum against the leaning neighbour, so a
    // carved unit is unioned with its own copies — and a cut that is lost in the fold, or a
    // neighbour check that consults an uncarved copy, invents matter in exactly one place per bay.
    Field f;
    const u32 unit = f.box({0, 0, 0}, {0.4, 0.4, 0.4});
    const u32 flue = f.cylinder({0, 0, 0}, 0.15, 1.0, 1);   // straight through, top to bottom
    const u32 carved = f.subtract({unit, flue});
    const u32 row = f.repeat(carved, {1.0, 0, 0}, {2, 0, 0});   // cells -2 .. 2, so five copies

    // The lattice covers each copy with the same relative sample points -- the period is a whole
    // number of steps and the low corner is on the grid -- so the counts below are exact integer
    // identities rather than approximations that happen to be close.
    const Lattice g = Lattice::over({-3.0, -0.6, -0.6}, {3.0, 0.6, 0.6}, 0.05);
    const Lattice one = Lattice::over({-0.5, -0.6, -0.6}, {0.5, 0.6, 0.6}, 0.05);

    must_cut_exactly(f, row, g,
                     [&](Vec3 p) {
                         const f64 cell = std::round(p.x);
                         if (std::abs(cell) > 2.0) return false;
                         const Vec3 q{p.x - cell, p.y, p.z};
                         return solid(f, carved, q);
                     },
                     "a flue inside a repeat");

    CHECK(matter_points(f, row, g) == 5 * matter_points(f, carved, one));
    CHECK(matter_points(f, carved, one) > 0);

    // Every copy, named, so a failure says which one. This is the assertion the title of the file
    // is about: the fifth bay is as much a bay as the first.
    for (i32 k = -2; k <= 2; ++k) {
        const f64 x = static_cast<f64>(k);
        CAPTURE(k);
        CHECK(f.eval(row, {x, 0, 0}) > 0.0);          // the flue is bored here
        CHECK(f.eval(row, {x + 0.3, 0, 0}) < 0.0);    // and there is stone beside it
        CHECK(f.eval(row, {x, 0.6, 0}) > 0.0);        // above the unit is air
    }
    // The limit is honoured: there is no sixth copy for the cut to be missing from.
    CHECK(f.eval(row, {3.0, 0, 0}) > 0.0);
    CHECK(f.eval(row, {3.3, 0, 0}) > 0.0);
}

TEST_CASE("a cut inside an around is bored through every copy of the ring") {
    // The same question asked of `PolarRepeat`, because a colonnade round a drum is how the
    // facility actually spaces its columns and the fold is angular rather than linear. Four copies
    // over a whole turn sit on +x, +z, -x and -z; each must carry its own bore.
    Field f;
    const u32 pier = f.box({1.2, 0, 0}, {0.4, 0.5, 0.3});
    const u32 bore = f.cylinder({1.2, 0, 0}, 0.15, 1.0, 1);
    const u32 unit = f.subtract({pier, bore});
    const u32 ring = f.polar_repeat(unit, 4, 1);

    const Lattice g = Lattice::over({-2.0, -1.0, -2.0}, {2.0, 1.0, 2.0}, 0.05);
    const Lattice one = Lattice::over({0.0, -1.0, -1.0}, {2.0, 1.0, 1.0}, 0.05);

    // Four copies of one unit, to within what a lattice can say about a shape that has been turned
    // relative to it. Exactness is asserted point by point below instead.
    CHECK(volume(f, ring, g) == doctest::Approx(4.0 * volume(f, unit, one)).epsilon(0.02));

    const Vec3 arms[4] = {{1.2, 0, 0}, {0, 0, 1.2}, {-1.2, 0, 0}, {0, 0, -1.2}};
    for (u32 i = 0; i < 4; ++i) {
        CAPTURE(i);
        const Vec3 c = arms[i];
        CHECK(f.eval(ring, c) > 0.0);                                    // the bore is here
        CHECK(f.eval(ring, {c.x * 1.2, 0, c.z * 1.2}) < 0.0);            // stone outside it
        CHECK(f.eval(ring, {c.x * 0.8, 0, c.z * 0.8}) < 0.0);            // stone inside it
        CHECK(f.eval(ring, {c.x, 0.9, c.z}) > 0.0);                      // air above the pier
    }
    // Between the arms there is nothing at all, which is what says the four checks above are about
    // four separate copies rather than one shape smeared round the axis.
    CHECK(f.eval(ring, {0.85, 0, 0.85}) > 0.0);
}

TEST_CASE("a cut survives four transforms stacked on top of each other") {
    // Every case above puts one transform over one cut. A clip does not: `clips/facility` reaches
    // forty nodes deep and the deepest evaluation goes thirty-six, so a window is inside a
    // translate inside a repeat inside a mirror inside a rotate before anybody sees it. The
    // failure mode is a cut that is right at one level and lost at the next, and it looks like a
    // building where the near elevation is correct and the far one is blank.
    Field f;
    const u32 bay = f.box({0, 0, 0}, {0.4, 0.5, 0.4});
    const u32 window = f.box({0, 0.1, 0}, {0.15, 0.2, 0.6});   // straight through in z
    const u32 carved = f.subtract({bay, window});

    const u32 lifted = f.translate(carved, {0, 0.6, 1.0});
    const u32 row = f.repeat(lifted, {1.0, 0, 0}, {1, 0, 0});   // three, at x = -1, 0, 1
    const u32 fronts = f.mirror(row, 2);                        // and the same at z = -1
    const u32 turned = f.rotate(fronts, {0, 0.25, 0});          // +x to -z

    // Six instances. The quarter turn takes (x, y, z) to (z, y, -x), so the centres are:
    const Vec3 centres[6] = {{1.0, 0.6, 1.0},  {1.0, 0.6, 0.0},  {1.0, 0.6, -1.0},
                             {-1.0, 0.6, 1.0}, {-1.0, 0.6, 0.0}, {-1.0, 0.6, -1.0}};

    const Lattice g = Lattice::over({-2.0, 0.0, -2.0}, {2.0, 1.2, 2.0}, 0.05);
    const Lattice one = Lattice::over({-0.6, -0.7, -0.7}, {0.6, 0.7, 0.7}, 0.05);
    CHECK(matter_points(f, turned, g) == 6 * matter_points(f, carved, one));

    for (u32 i = 0; i < 6; ++i) {
        CAPTURE(i);
        const Vec3 c = centres[i];
        // The window is at the unit's local y = +0.1, so a tenth of a metre above each centre.
        CHECK(f.eval(turned, {c.x, c.y + 0.1, c.z}) > 0.0);
        // And below it there is wall, which is what says the whole bay was not simply lost.
        CHECK(f.eval(turned, {c.x, c.y - 0.3, c.z}) < 0.0);
    }
    // Nothing between the two fronts.
    CHECK(f.eval(turned, {0, 0.7, 0}) > 0.0);
    CHECK(f.eval(turned, {0, 0.3, 0}) > 0.0);
}

// ================================================================================================
// THE BOX CULL, WHICH IS THE ONE THING THAT CAN MAKE A CUT SILENTLY MISS
// ================================================================================================

TEST_CASE("a wall of windows is cut the same with the difference's box cull on and off") {
    // `Op::Difference` in `Field::eval` skips a cutter whose bounding box is further away than the
    // running answer, and that skip is the only mechanism in the engine that can make a cut
    // present in the graph fail to happen. It is also load-bearing: an elevation is one wall and
    // twenty openings, and without it every point of that wall asks all twenty.
    //
    // `build_bounds()` is what turns it on, so the same field built twice — once with boxes, once
    // without — has to agree point for point. Every operand here is a box or a cylinder, whose
    // answers are true distances (`op_reports_true_distance`); D644 measured four primitives that
    // are NOT, found the cull can skip a cutter behind one of them, measured the fix at 6.8x the
    // sampling cost for a byte-identical building, and reverted it. So this is the guard over the
    // exact cases, and D644 is the written-down hole in the rest.
    const auto build = [](Field& f, std::vector<u32>& windows) {
        const u32 wall = f.box({0, 0, 0}, {4.0, 1.5, 0.2});
        for (i32 k = -3; k <= 3; ++k) {
            windows.push_back(
                f.box({static_cast<f64>(k) * 1.0, 0.2, 0}, {0.3, 0.4, 0.4}));
        }
        std::vector<u32> parts;
        parts.push_back(wall);
        for (u32 w : windows) parts.push_back(w);
        return std::pair<u32, u32>{wall, f.subtract(parts)};
    };

    Field bare;
    std::vector<u32> bare_windows;
    const auto bare_built = build(bare, bare_windows);

    Field boxed;
    std::vector<u32> boxed_windows;
    const auto boxed_built = build(boxed, boxed_windows);
    boxed.build_bounds();
    REQUIRE(boxed.unbounded_nodes() == 0);

    const Lattice g = Lattice::over({-5.0, -2.0, -1.0}, {5.0, 2.0, 1.0}, 0.1);

    const Where want = [&](Vec3 p) {
        if (!solid(bare, bare_built.first, p)) return false;
        for (u32 w : bare_windows) {
            if (solid(bare, w, p)) return false;
        }
        return true;
    };
    must_cut_exactly(bare, bare_built.second, g, want, "seven windows with no boxes to cull on");
    must_cut_exactly(boxed, boxed_built.second, g, want, "the same seven with the cull engaged");

    // Every window took its own bite and they are all the same size, which is the statement a
    // single total cannot make: six right and one missing weighs the same as seven slightly small.
    const usize wall_alone = matter_points(bare, bare_built.first, g);
    usize bites = 0;
    for (u32 w : bare_windows) {
        const usize bite = points_where(g, [&](Vec3 p) {
            return solid(bare, bare_built.first, p) && solid(bare, w, p);
        });
        CHECK(bite > 0);
        if (bites == 0) bites = bite;
        CHECK(bite == bites);
    }
    CHECK(matter_points(boxed, boxed_built.second, g) == wall_alone - 7 * bites);
}

// ================================================================================================
// THE FOUR OPERATIONS THAT MOVE A SURFACE, WRAPPED ROUND A CUT
// ================================================================================================
//
// A difference gives an exact boundary. `shell`, `round`, `offset` and `displace` each move every
// surface of whatever is under them — and the surface of a cut IS a surface of the shape, which is
// the part an author does not expect. Softening the arrises of a carved block makes its holes
// smaller. Shelling it puts matter back inside them. These are the cases where "exactly what it
// should" quietly stops being true, so what each one does to a cut is pinned rather than left to
// be discovered on a building.

TEST_CASE("rounding a carved shape fills its holes in by the radius it adds") {
    // `Op::Round` is `d - r`: everything grows by r, which is what rounds a convex arris. Inside a
    // bore that same r is taken OFF the hole, so a block rounded by five centimetres has a bore
    // five centimetres narrower than the one that was drawn. Nothing warns about it.
    Carved c;
    const f64 r = 0.1;
    const u32 rounded = c.f.round_off(c.shape, r);

    // Every sample point in this lattice is inside the block by more than the round, and closer to
    // the bore than 0.9 m, so the carved shape's own distance there is exactly 0.5 - |p| and the
    // arithmetic below can be written down rather than measured.
    const Lattice g = Lattice::over({-0.5, -0.5, -0.5}, {0.5, 0.5, 0.5}, 0.02);
    must_cut_exactly(c.f, rounded, g,
                     [&](Vec3 p) { return length(p) > 0.5 - r; },
                     "a bore inside a round");

    CHECK(c.f.eval(rounded, {0.45, 0, 0}) < 0.0);   // was air before the round, is matter now
    CHECK(c.f.eval(rounded, {0.35, 0, 0}) > 0.0);   // and the hole that is left is 0.4 across
    CHECK(c.f.eval(rounded, {1.05, 0, 0}) < 0.0);   // the outside grew by the same amount

    // `offset` is the same operation said the other way — the header says so, and a sign slip
    // between the two would make one of them a no-op that nobody would look for.
    const u32 by_offset = c.f.offset(c.shape, -r);
    const Lattice wide = Lattice::over({-1.5, -1.5, -1.5}, {1.5, 1.5, 1.5}, 0.05);
    must_cut_exactly(c.f, by_offset, wide,
                     [&](Vec3 p) { return solid(c.f, rounded, p); },
                     "offset by minus what round added");
}

TEST_CASE("offsetting a carved shape opens its holes by exactly what it takes off the outside") {
    // The other direction, and the one an author reaches for to leave a tolerance: `d + v` shrinks
    // the solid by v, so the bore GROWS by v. A window opening offset outward to clear its dressing
    // is also a window opening that has moved v further into whatever is behind the wall.
    Carved c;
    const f64 v = 0.1;
    const u32 shrunk = c.f.offset(c.shape, v);

    const Lattice g = Lattice::over({-0.5, -0.5, -0.5}, {0.5, 0.5, 0.5}, 0.02);
    must_cut_exactly(c.f, shrunk, g, [&](Vec3 p) { return length(p) > 0.5 + v; },
                     "a bore inside an offset");

    CHECK(c.f.eval(shrunk, {0.55, 0, 0}) > 0.0);   // was matter, the offset opened it up
    CHECK(c.f.eval(shrunk, {0.65, 0, 0}) < 0.0);
    CHECK(c.f.eval(shrunk, {0.95, 0, 0}) > 0.0);   // and the outside came in by the same amount

    // The volume the offset removed, from both surfaces at once. A test that watched only the
    // outside would report this shape as simply smaller.
    const Lattice wide = Lattice::over({-1.5, -1.5, -1.5}, {1.5, 1.5, 1.5}, 0.05);
    CHECK(volume(c.f, shrunk, wide) ==
          doctest::Approx(1.8 * 1.8 * 1.8 - sphere_volume(0.5 + v)).epsilon(0.01));
}

TEST_CASE("shelling a carved shape lines the inside of the hole it was told to cut") {
    // The one that surprises people, and it follows straight from `|d| - t`: the shell straddles
    // every surface by t on each side, and the wall of a bore is a surface. So `shell` puts a
    // t-thick coat of matter back INSIDE the hole the difference removed. It is not a bug, and a
    // clip that hollows a carved block and expects its bores to stay clear has one.
    Carved c;
    const f64 t = 0.1;
    const u32 hollow = c.f.shell(c.shape, t);

    // Same lattice as the round test and for the same reason: here the carved distance is exactly
    // 0.5 - |p|, so the shell is exactly the band 0.4 < |p| < 0.6.
    const Lattice g = Lattice::over({-0.5, -0.5, -0.5}, {0.5, 0.5, 0.5}, 0.02);
    must_cut_exactly(c.f, hollow, g,
                     [&](Vec3 p) {
                         const f64 d = length(p);
                         return d > 0.5 - t && d < 0.5 + t;
                     },
                     "the lining a shell leaves inside a bore");

    CHECK(c.f.eval(hollow, {0, 0, 0}) > 0.0);      // the middle of the bore is still air
    CHECK(c.f.eval(hollow, {0.45, 0, 0}) < 0.0);   // but matter has come back inside its wall
    CHECK(c.f.eval(hollow, {0.55, 0, 0}) < 0.0);
    CHECK(c.f.eval(hollow, {0.65, 0, 0}) > 0.0);   // and the block behind it is hollow too
    CHECK(c.f.eval(hollow, {0.95, 0, 0}) < 0.0);   // except for its own skin

    // A shell is the set between two offsets, exactly: |d| < t is d < t and not d < -t. Asserting
    // it as an identity over the whole box ties the three operations together, so a change to any
    // one of them has to change a test rather than nothing.
    const Lattice wide = Lattice::over({-1.5, -1.5, -1.5}, {1.5, 1.5, 1.5}, 0.05);
    const u32 grown = c.f.offset(c.shape, -t);
    const u32 shrunk = c.f.offset(c.shape, t);
    must_cut_exactly(c.f, hollow, wide,
                     [&](Vec3 p) { return solid(c.f, grown, p) && !solid(c.f, shrunk, p); },
                     "a shell against the two offsets it lies between");
}

TEST_CASE("displacing a carved shape moves its cut by the displacement and no further") {
    // Displacement is the one operation that makes an exact boundary inexact on purpose, and the
    // whole sampler is built on knowing by HOW MUCH: `Field::skip_slack` charges the amplitude
    // against every skip, and an allowance that is too small is a hole in a wall that nobody can
    // find. The same number bounds what a displacement can do to a cut, so it is asserted here in
    // the form the sampler relies on.
    //
    // The pattern is a sine, whose range is exactly [-1, 1], so the bound is exactly the amount.
    Carved c;
    const f64 amount = 0.06;
    const u32 wave = c.f.sine(1, 0.4);   // along y, one period every 0.4 m
    const u32 rough = c.f.displace(c.shape, wave, amount);

    // Inside this lattice the carved distance is 0.5 - |p|, so "further in than the amount" and
    // "further out than the amount" are statements about a radius.
    const Lattice g = Lattice::over({-0.5, -0.5, -0.5}, {0.5, 0.5, 0.5}, 0.02);
    must_be(c.f, rough, g, [&](Vec3 p) { return length(p) > 0.5 + amount; }, true,
            "matter more than the displacement outside the bore");
    must_be(c.f, rough, g, [&](Vec3 p) { return length(p) < 0.5 - amount; }, false,
            "air more than the displacement inside the bore");

    // And it really did move, both ways, or the two bounds above are satisfied by a displacement
    // that did nothing at all. The sine is +1 at y = 0.1 and -1 at y = 0.3, so the bore is widened
    // by the full amount at the one height and narrowed by it at the other.
    CHECK(c.f.eval(c.shape, {0.55, 0.1, 0}) < 0.0);    // matter before the displacement
    CHECK(c.f.eval(rough, {0.55, 0.1, 0}) > 0.0);      // air after it: the bore opened by 0.06

    const Vec3 tighter{0.3618, 0.3, 0};                // 0.47 m from the centre
    CHECK(c.f.eval(c.shape, tighter) > 0.0);           // air before
    CHECK(c.f.eval(rough, tighter) < 0.0);             // matter after: the bore closed by 0.06

    // And the same bound as the sampler states it, so the two cannot drift apart. The surface
    // moved by `amount`, which is what `undisplaced` reports; the allowance a SKIP has to make is
    // twice that, because a reading taken at one point may be `amount` too far out while the point
    // it is vouching for is `amount` too far in. Getting that factor of two wrong cost 265 voxels
    // out of fifty-nine million the first time, which is nothing to look at and a hole in a wall
    // to stand in.
    f64 amplitude = 0.0;
    CHECK(c.f.undisplaced(rough, amplitude) == c.shape);
    CHECK(amplitude == doctest::Approx(amount));
    CHECK(c.f.skip_slack() == doctest::Approx(2.0 * amount));
}
