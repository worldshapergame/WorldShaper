// The clip pipeline end to end: a file goes in, voxels and numbers come out.
//
// The tests that matter here are the ones about *arithmetic*, not about parsing. A parser that
// mangles a word produces an error message; a sampler that is a voxel out produces a room whose
// doorway is 1.97 m and whose far wall does not line up with the near one, and nothing complains.
// So most of what follows asserts sizes and volumes against numbers worked out by hand.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "forge/clip_script.hpp"
#include "forge/measure.hpp"
#include "game/clip.hpp"
#include "forge/sample.hpp"
#include "world/tags.hpp"
#include "world/voxel_type.hpp"

using namespace ws;
using namespace ws::forge;

namespace {

struct Built {
    Script script;
    SampleResult result;
    Measurement measurement;
};

Built build(const std::string& text) {
    Built out;
    VoxelTypeTable types;
    TagRegistry tags;
    out.script = parse_clip_script(text, types, tags);
    if (out.script.ok()) {
        out.result = sample(out.script.field, out.script.solid, out.script.paint,
                            out.script.settings, nullptr);
        out.measurement = measure(out.result.clip, out.script.settings.voxels_per_metre);
    }
    return out;
}

}  // namespace

TEST_CASE("a box of a known size samples to exactly the voxels it should") {
    // Two metres by one by three, at thirty-two voxels to the metre, is 64 x 32 x 96 and not one
    // voxel more. Off-by-one here compounds: every wall in a room is a box, and a room whose
    // walls are each a voxel too fat has a floor plan that does not close.
    const Built b = build(R"(
metre 32
bounds 0 0 0  2 1 3
material stone rgb=120,120,116
let block = box 0 0 0  2 1 3
paint stone
solid block
)");
    REQUIRE(b.script.errors.empty());
    REQUIRE(b.script.ok());
    CHECK(b.measurement.size[0] == 64);
    CHECK(b.measurement.size[1] == 32);
    CHECK(b.measurement.size[2] == 96);
    CHECK(b.measurement.solid == 64u * 32u * 96u);
    CHECK(b.measurement.cubic_metres() == doctest::Approx(6.0));
}

TEST_CASE("the sampled volume of a sphere is within a per cent of the formula") {
    // A sphere of radius one metre holds 4/3 pi cubic metres. A voxel sampler cannot be exact,
    // but it can be close, and being close is the check that the sampling grid is not biased —
    // a half-voxel offset shows up here as a systematic couple of per cent.
    const Built b = build(R"(
metre 32
bounds -1.2 -1.2 -1.2  1.2 1.2 1.2
material stone rgb=120,120,116
let ball = sphere 0 0 0 r=1
paint stone
solid ball
)");
    REQUIRE(b.script.ok());
    const f64 expected = 4.0 / 3.0 * 3.14159265358979 * 1.0;
    CHECK(b.measurement.cubic_metres() == doctest::Approx(expected).epsilon(0.01));
}

TEST_CASE("a hollow room has the wall thickness it was given") {
    // A four metre cube, shelled to a tenth of a metre. The wall should be 0.1 m thick, which at
    // thirty-two voxels to the metre is three or four voxels — measured by walking a line
    // through the middle and looking at the solid run at each end.
    // The bounds are the box, deliberately. Cut a wider volume and the line through the middle
    // starts in the open air outside the room, so "the gap" becomes the whole width of the
    // sample with the walls as interruptions — a true measurement of the wrong thing.
    const Built b = build(R"(
metre 32
bounds -2 -2 -2  2 2 2
material stone rgb=120,120,116
let walls = shell { box -2 -2 -2  2 2 2 } thickness=0.1
paint stone
solid walls
)");
    REQUIRE(b.script.ok());
    const Clip& clip = b.result.clip;
    // Straight through the middle along x: solid, air, solid.
    const i32 mid_y = clip.size[1] / 2;
    const i32 mid_z = clip.size[2] / 2;
    const Span gap = gap_along(clip, 0, mid_y, mid_z);
    CHECK(gap.any);
    CHECK(gap.contiguous);
    // The hollow is the inside of a four metre cube less two wall thicknesses.
    CHECK(b.measurement.metres(gap.length()) == doctest::Approx(3.8).epsilon(0.05));
}

TEST_CASE("a doorway is the width it was cut to") {
    // The measurement that catches the most common authoring mistake there is: an opening cut
    // with a box whose corners are written as sizes rather than positions.
    const Built b = build(R"(
metre 32
bounds -2 0 -0.4  2 2.6 0.4
material stone rgb=120,120,116
let wall = box -2 0 -0.2  2 2.6 0.2
let door = box -0.5 0 -0.3  0.5 2.1 0.3
let cut  = difference { wall door }
paint stone
solid cut
)");
    REQUIRE(b.script.ok());
    const Clip& clip = b.result.clip;
    // Across the doorway at knee height, the gap should be exactly one metre.
    const i32 knee = clip.size[1] / 5;
    const i32 mid_z = clip.size[2] / 2;
    const Span gap = gap_along(clip, 0, knee, mid_z);
    CHECK(gap.any);
    CHECK(gap.contiguous);
    CHECK(b.measurement.metres(gap.length()) == doctest::Approx(1.0).epsilon(0.04));

    // And its head height is 2.1 m: above that the wall closes again.
    const i32 centre_x = clip.size[0] / 2;
    const Span column = span_along(clip, 1, centre_x, mid_z);
    CHECK(column.any);
    CHECK(b.measurement.metres(column.first) == doctest::Approx(2.1).epsilon(0.03));
}

TEST_CASE("a symmetric clip is symmetric to the voxel") {
    // Mirror symmetry is the cheapest check there is for a whole class of mistakes: a shape
    // built from two halves that do not quite match, or a pattern whose phase is not centred.
    const Built b = build(R"(
metre 16
bounds -2 -2 -2  2 2 2
material stone rgb=120,120,116
let a = box -1.5 -1 -1  1.5 1 1
let b = cylinder 0 0 0 r=0.8 h=3 axis=y
let both = union { a b }
paint stone
solid both
)");
    REQUIRE(b.script.ok());
    CHECK(mirror_mismatch(b.result.clip, 0) == 0);
    CHECK(mirror_mismatch(b.result.clip, 1) == 0);
    CHECK(mirror_mismatch(b.result.clip, 2) == 0);
}

TEST_CASE("paint rules stack, and a later coat covers an earlier one") {
    const Built b = build(R"(
metre 16
bounds 0 0 0  2 2 2
material stone rgb=120,120,116
material moss  rgb=60,110,50
let block = box 0 0 0  2 2 2
let high  = axis of=y
paint stone
paint moss where=high above=1.0
solid block
)");
    REQUIRE(b.script.ok());
    REQUIRE(b.measurement.types.size() == 2);
    // Half the height is above one metre, so the two materials should be near enough equal.
    const u64 total = b.measurement.solid;
    for (const TypeShare& share : b.measurement.types) {
        CHECK(share.fraction == doctest::Approx(0.5).epsilon(0.02));
    }
    CHECK(total > 0);
}

TEST_CASE("a rough surface has more area than a smooth one of the same volume") {
    // The measurement that notices texture, and the reason exposed faces are counted at all.
    //
    // Measured on a slab rather than a sphere, and that is not laziness. A voxelised sphere is
    // already all staircase — its exposed area is half again what the smooth maths says — and a
    // gentle displacement can just as easily cut across a step as add one, so the number moves
    // in either direction and says nothing. A flat face has no staircase to hide in, so every
    // face the roughness adds is a face that was not there before.
    const Built smooth = build(R"(
metre 32
bounds -1 0 -1  1 0.5 1
material stone rgb=120,120,116
let slab = box -1 0 -1  1 0.4 1
paint stone
solid slab
)");
    const Built rough = build(R"(
metre 32
bounds -1 0 -1  1 0.5 1
material stone rgb=120,120,116
let grain = fbm size=0.08 octaves=3
let slab  = displace { box -1 0 -1  1 0.4 1  grain } amount=0.05
paint stone
solid slab
)");
    REQUIRE(smooth.script.ok());
    REQUIRE(rough.script.ok());
    // Roughly the same amount of matter...
    CHECK(rough.measurement.cubic_metres() ==
          doctest::Approx(smooth.measurement.cubic_metres()).epsilon(0.2));
    // ...and appreciably more surface.
    CHECK(rough.measurement.square_metres() > smooth.measurement.square_metres() * 1.15);
}

TEST_CASE("a parameter changes the clip without the file being read again") {
    // The property the whole design turns on. Parse once, then move a number and re-sample.
    VoxelTypeTable types;
    TagRegistry tags;
    Script script = parse_clip_script(R"(
metre 16
bounds -2 -2 -2  2 2 2
param radius 1.0
material stone rgb=120,120,116
let ball = sphere 0 0 0 r=radius
paint stone
solid ball
)",
                                     types, tags);
    REQUIRE(script.ok());

    const SampleResult first = sample(script.field, script.solid, script.paint, script.settings);
    const Measurement small = measure(first.clip, script.settings.voxels_per_metre);

    // Note what a parameter does *not* do: `r=radius` reads the parameter's value at parse time,
    // so this is the honest state of things today — the dial exists and is named, and wiring it
    // through to the shape's argument is the next piece of work. What is asserted here is that
    // the slot is there and holds what the file said.
    CHECK(script.field.parameter_count() == 1);
    CHECK(script.field.get_parameter("radius", 0.0) == doctest::Approx(1.0));
    CHECK(script.field.set_parameter("radius", 1.5));
    CHECK(script.field.get_parameter("radius", 0.0) == doctest::Approx(1.5));
    CHECK(small.cubic_metres() > 0.0);
}

TEST_CASE("errors are collected with line numbers and parsing carries on") {
    VoxelTypeTable types;
    TagRegistry tags;
    const Script script = parse_clip_script(R"(
metre 32
bounds 0 0 0 1 1 1
let a = wobble 1 2 3
material stone rgb=1,2,3
let b = box 0 0 0 1 1 1
paint nosuchmaterial
solid b
)",
                                           types, tags);
    // Two complaints: the unknown shape and the unknown material. Both, not just the first.
    REQUIRE(script.errors.size() >= 2);
    CHECK(script.errors[0].line > 0);
    bool mentions_wobble = false;
    bool mentions_material = false;
    for (const ScriptError& e : script.errors) {
        if (e.message.find("wobble") != std::string::npos) mentions_wobble = true;
        if (e.message.find("nosuchmaterial") != std::string::npos) mentions_material = true;
    }
    CHECK(mentions_wobble);
    CHECK(mentions_material);
}

// The parser's whole contract is that it collects errors and carries on, and braces were where it
// could not: `block` -> `expression` -> `call` -> `block` recursed once per `{` with no bound, so a
// file whose braces have desynchronised is read through a stack as deep as the rest of the file is
// long. The clip viewer's baker died in exactly that cycle, and a parser fed files a player writes
// must not be able to end a process however wrong the file is.
//
// Sixty-four is the limit. Nothing anybody writes nests eight deep.
TEST_CASE("a file whose braces do not balance is an error rather than a crash") {
    // Nested, not repeated: `let a = union {` four thousand times is four thousand statements the
    // parser recovers from one at a time and never nests through, which is what the first version
    // of this test asserted against and is why it passed while proving nothing.
    std::string text = "metre 8\nbounds 0 0 0  1 1 1\nlet a = ";
    for (int i = 0; i < 4000; ++i) text += "union { ";
    text += "sphere 0 0 0 r=0.5\n";

    VoxelTypeTable types;
    TagRegistry tags;
    const Script script = parse_clip_script(text, types, tags);

    CHECK(!script.errors.empty());
    CHECK_FALSE(script.ok());
    bool mentions_braces = false;
    for (const ScriptError& e : script.errors) {
        if (e.message.find("unbalanced") != std::string::npos) mentions_braces = true;
    }
    CHECK(mentions_braces);
}

// AND AT EVERY DEPTH, because the fault under this was not the depth.
//
// The test above passed at 4000 while 90, 95, 96, 100, 104, 110 and 1000 segfaulted and 80, 120 and
// 128 did not. That is not a bound being exceeded, it is a heap read one past the end of the token
// array: `block()` checks `!done()`, calls `expression()`, a nested `block()` hits the depth limit
// and sets `at_ = tokens_.size()` to abandon the file, and `block()` then reads `peek().text` for
// its error message. Whether that crashes depends on what happens to sit after the vector, which is
// why the same input in a different process behaved differently and why D666 could only record what
// it saw. AddressSanitizer names it in one line.
//
// So this sweeps, and it is worth the second and a half it costs. A single depth proves nothing
// about a fault that is decided by the allocator.
TEST_CASE("unbalanced braces are an error at every depth, not only at convenient ones") {
    for (const int depth : {65, 66, 80, 90, 95, 96, 100, 104, 110, 120, 128, 200, 1000}) {
        std::string text = "metre 8\nbounds 0 0 0  1 1 1\nlet a = ";
        for (int i = 0; i < depth; ++i) text += "union { ";
        text += "sphere 0 0 0 r=0.5\n";

        VoxelTypeTable types;
        TagRegistry tags;
        const Script script = parse_clip_script(text, types, tags);

        INFO("depth ", depth);
        CHECK_FALSE(script.ok());
        CHECK(!script.errors.empty());
    }
}

// The same read, reached without any nesting at all: a file that simply stops in the middle of a
// block. `expression()` fails on a token that is not a shape, `block()` advances past it, and the
// loop asks for the next one -- which is not there.
TEST_CASE("a block that is never closed is an error rather than a read past the last token") {
    VoxelTypeTable types;
    TagRegistry tags;
    for (const char* tail : {"let a = union {",
                             "let a = union { sphere 0 0 0 r=0.5",
                             "let a = union { ?",
                             "let a = union { union { union {"}) {
        const Script script = parse_clip_script("metre 8\nbounds 0 0 0  1 1 1\n" + std::string(tail),
                                                types, tags);
        INFO("tail ", tail);
        CHECK_FALSE(script.ok());
    }
}

TEST_CASE("every cell of a clip with no region belongs to it, however empty") {
    // The mask says which cells are the clip's business, separately from which hold matter, and
    // with no region that is all of them. It matters for stamping: an empty cell *inside* the
    // clip clears what it lands on, where a cell outside is left alone — so a hole in the mask
    // is a clip that fails to clear, and nothing about the voxels shows it.
    //
    // It was holed. The sampler skips ahead through empty space using the distance, and the jump
    // skipped the marking along with the evaluation. A printed slice showed it immediately as
    // ragged tears through the air around the shape; no measurement of the matter could have.
    const Built b = build(R"(
metre 32
bounds -2 -2 -2  2 2 2
material stone rgb=120,120,116
let ball = sphere 0 0 0 r=0.4
paint stone
solid ball
)");
    REQUIRE(b.script.ok());
    const u64 cells = static_cast<u64>(b.result.clip.size[0]) *
                      static_cast<u64>(b.result.clip.size[1]) *
                      static_cast<u64>(b.result.clip.size[2]);
    CHECK(b.measurement.covered == cells);
    CHECK(b.measurement.solid > 0);
    CHECK(b.measurement.solid < cells);   // and it really is mostly air, so jumps really happened
}

TEST_CASE("skipping empty space does not change a single voxel") {
    // The same clip sampled with the jump able to fire and with it disabled by an unbounded
    // displacement must agree exactly. This is the check that the jump length is a sound bound
    // rather than one that usually works.
    const Built plain = build(R"(
metre 32
bounds -2 -2 -2  2 2 2
material stone rgb=120,120,116
let ball = sphere 0 0 0 r=1.2
paint stone
solid ball
)");
    const Built no_jump = build(R"(
metre 32
bounds -2 -2 -2  2 2 2
material stone rgb=120,120,116
let far  = cells size=8
let ball = displace { sphere 0 0 0 r=1.2  far } amount=0
paint stone
solid ball
)");
    REQUIRE(plain.script.ok());
    REQUIRE(no_jump.script.ok());
    CHECK(plain.measurement.solid == no_jump.measurement.solid);
    CHECK(plain.measurement.exposed_faces == no_jump.measurement.exposed_faces);
}

TEST_CASE("a region narrows the clip to something other than a box") {
    // Without this every clip is rectangular, and a round tower stamped into a wall takes a
    // square bite out of it.
    const Built b = build(R"(
metre 16
bounds -2 -2 -2  2 2 2
material stone rgb=120,120,116
let block = box -2 -2 -2  2 2 2
let round = cylinder 0 0 0 r=1.5 h=4 axis=y
paint stone
solid block
region round
)");
    REQUIRE(b.script.ok());
    // The corners of the box are outside the clip, so they are neither solid nor covered.
    CHECK(b.result.clip.covered(0, 16, 0) == false);
    CHECK(b.result.clip.covered(b.result.clip.size[0] / 2, 16, b.result.clip.size[2] / 2) == true);
    CHECK(b.measurement.covered < static_cast<u64>(b.measurement.size[0]) *
                                      b.measurement.size[1] * b.measurement.size[2]);
}

TEST_CASE("stairs measured tread by tread rise by the amount asked for") {
    // The check that a staircase is walkable: every step the same, and the rise what was asked.
    const Built b = build(R"(
metre 32
bounds -0.6 0 -0.1  0.6 2.0 3.1
material stone rgb=120,120,116
let flight = stairs -0.6 0 0  0.6 1.8 3.0 run=0.30 rise=0.18
paint stone
solid flight
)");
    REQUIRE(b.script.ok());
    const Clip& clip = b.result.clip;
    const i32 mid_x = clip.size[0] / 2;
    // Walk along the flight and record where the top of the solid is at each of several points.
    std::vector<i32> tops;
    for (i32 z = 8; z < clip.size[2] - 8; z += 16) {
        i32 top = -1;
        for (i32 y = 0; y < clip.size[1]; ++y) {
            if (clip.at(mid_x, y, z) != kAir) top = y;
        }
        tops.push_back(top);
    }
    REQUIRE(tops.size() >= 3);
    // It goes up, and it never goes down.
    for (usize i = 1; i < tops.size(); ++i) CHECK(tops[i] >= tops[i - 1]);
    CHECK(tops.back() > tops.front());
}

// --- part of the way round ----------------------------------------------------------------
//
// `revolve`, `around` and `arc` all take `from` and `to` in turns. The failures these invite are
// quiet ones — a sweep a quarter turn out measures a plausible volume, and a range written across
// the seam at zero produces an empty shape if the wrap is dropped — so what is asserted here is
// WHICH SIDE the matter came out on, and that leaving the keys off changes nothing at all.

TEST_CASE("from and to left off leave a sweep exactly as it was") {
    // The control arm, in the language rather than in the field: the same three shapes written
    // with and without a whole-turn range have to sample to the same voxel count. Anything else
    // is every clip in the repository quietly moving.
    const Built plain = build(R"(
metre 16
bounds -3 -1 -3  3 1 3
material stone rgb=120,120,116
let ring  = revolve { sphere 2 0 0 r=0.4 } axis=y
let hoop  = torus 0 0 0 ring=1.0 tube=0.15 axis=y
let posts = around { cylinder 2.6 0 0 r=0.12 h=1.6 axis=y } count=6 axis=y
paint stone
solid union { ring hoop posts }
)");
    const Built ranged = build(R"(
metre 16
bounds -3 -1 -3  3 1 3
material stone rgb=120,120,116
let ring  = revolve { sphere 2 0 0 r=0.4 } axis=y from=0 to=1
let hoop  = arc 0 0 0 ring=1.0 tube=0.15 axis=y from=0.3 to=1.3
let posts = around { cylinder 2.6 0 0 r=0.12 h=1.6 axis=y } count=6 axis=y from=0 to=1
paint stone
solid union { ring hoop posts }
)");
    REQUIRE(plain.script.ok());
    REQUIRE(ranged.script.ok());
    CHECK(ranged.measurement.solid == plain.measurement.solid);
    CHECK(ranged.measurement.exposed_faces == plain.measurement.exposed_faces);
}

TEST_CASE("a half revolve is half the matter, on the side the turns say") {
    // A turn of nought is along the first cross-axis — x, for a sweep about y — and grows toward
    // the second, which is z. So `from=0 to=0.5` keeps the +z half and drops the -z one.
    const Built b = build(R"(
metre 16
bounds -3 -1 -3  3 1 3
material stone rgb=120,120,116
let apse = revolve { sphere 2 0 0 r=0.4 } axis=y from=0 to=0.5
paint stone
solid apse
)");
    REQUIRE(b.script.ok());
    const Clip& clip = b.result.clip;
    // Nothing at all on the -z side of the cut, and plenty on the +z side.
    usize plus_z = 0, minus_z = 0;
    for (i32 z = 0; z < clip.size[2]; ++z) {
        for (i32 y = 0; y < clip.size[1]; ++y) {
            for (i32 x = 0; x < clip.size[0]; ++x) {
                if (clip.at(x, y, z) == kAir) continue;
                if (z > clip.size[2] / 2) ++plus_z;
                else if (z < clip.size[2] / 2 - 1) ++minus_z;
            }
        }
    }
    CHECK(plus_z > 1000);
    CHECK(minus_z == 0);
    // A torus of ring 2 and tube 0.4 holds 2 pi^2 R r^2; half of it is that halved.
    const f64 whole = 2.0 * 3.14159265358979 * 3.14159265358979 * 2.0 * 0.4 * 0.4;
    CHECK(b.measurement.cubic_metres() == doctest::Approx(whole * 0.5).epsilon(0.03));
}

TEST_CASE("a range written across the seam at zero is not an empty shape") {
    // `from=0.75 to=0.25` runs 0.75 -> 0 -> 0.25, so the matter is centred on +x. Dropping the
    // wrap gives a span of minus a half and a clip with nothing in it, which is exactly the sort
    // of failure that reports success.
    const Built b = build(R"(
metre 16
bounds -3 -1 -3  3 1 3
material stone rgb=120,120,116
let niche = revolve { sphere 2 0 0 r=0.4 } axis=y from=0.75 to=0.25
paint stone
solid niche
)");
    REQUIRE(b.script.ok());
    const Clip& clip = b.result.clip;
    usize plus_x = 0, minus_x = 0;
    for (i32 z = 0; z < clip.size[2]; ++z) {
        for (i32 y = 0; y < clip.size[1]; ++y) {
            for (i32 x = 0; x < clip.size[0]; ++x) {
                if (clip.at(x, y, z) == kAir) continue;
                if (x > clip.size[0] / 2) ++plus_x;
                else if (x < clip.size[0] / 2 - 1) ++minus_x;
            }
        }
    }
    CHECK(plus_x > 1000);
    CHECK(minus_x == 0);
}

TEST_CASE("seven columns from here round to there stand on both ends of the arc") {
    // The spacing an author means: over an arc the first copy is ON `from` and the last is ON
    // `to`, n copies and n-1 gaps. Asserted by counting the separate pieces — seven columns that
    // do not touch are seven components.
    const Built b = build(R"(
metre 16
bounds -3 -1 -3  3 1 3
material stone rgb=120,120,116
let colonnade = around { cylinder 2.4 0 0 r=0.14 h=1.6 axis=y } count=7 axis=y from=-0.1944 to=0.1944
paint stone
solid colonnade
)");
    REQUIRE(b.script.ok());
    const Connectivity joined = connectivity(b.result.clip);
    CHECK(joined.components == 7);
    // 140 degrees centred on +x, so the whole colonnade sits on the +x side.
    const Clip& clip = b.result.clip;
    usize minus_x = 0;
    for (i32 z = 0; z < clip.size[2]; ++z) {
        for (i32 y = 0; y < clip.size[1]; ++y) {
            for (i32 x = 0; x < clip.size[0] / 2; ++x) {
                if (clip.at(x, y, z) != kAir) ++minus_x;
            }
        }
    }
    CHECK(minus_x == 0);
}

TEST_CASE("an arch ring is a half torus that stands up") {
    // `arc` about z runs from +x round through +y to -x, so `from=0 to=0.5` is an arch and not a
    // bowl. One component, because the two ends of one arc are joined by the arc.
    const Built b = build(R"(
metre 16
bounds -2 -0.4 -0.5  2 2 0.5
material stone rgb=120,120,116
let ring = arc 0 0 0 ring=1.4 tube=0.16 axis=z from=0 to=0.5
paint stone
solid ring
)");
    REQUIRE(b.script.ok());
    const Connectivity joined = connectivity(b.result.clip);
    CHECK(joined.components == 1);
    const Clip& clip = b.result.clip;
    usize below = 0;
    for (i32 z = 0; z < clip.size[2]; ++z) {
        for (i32 x = 0; x < clip.size[0]; ++x) {
            for (i32 y = 0; y < clip.size[1]; ++y) {
                // The springing is at y = 0, which is 0.4 m up from the bottom of the bounds.
                if (clip.at(x, y, z) != kAir && y < static_cast<i32>(0.4 * 16.0) - 4) ++below;
            }
        }
    }
    CHECK(below == 0);
    // Half a torus of ring 1.4 and tube 0.16, plus the two round caps, which together make one
    // more sphere of the tube's radius.
    const f64 pi = 3.14159265358979;
    const f64 expected = 0.5 * (2.0 * pi * pi * 1.4 * 0.16 * 0.16) +
                         4.0 / 3.0 * pi * 0.16 * 0.16 * 0.16;
    CHECK(b.measurement.cubic_metres() == doctest::Approx(expected).epsilon(0.05));
}

TEST_CASE("a slice reads as a picture of the shape") {
    const Built b = build(R"(
metre 8
bounds -1 -1 -1  1 1 1
material stone rgb=120,120,116
let ball = sphere 0 0 0 r=0.9
paint stone
solid ball
)");
    REQUIRE(b.script.ok());
    const std::string text = slice_text(b.result.clip, 2, b.result.clip.size[2] / 2, 1);
    CHECK(text.find('#') != std::string::npos);
    // A circle: the middle row is wider than the top one.
    std::vector<usize> widths;
    usize count = 0;
    for (char c : text) {
        if (c == '\n') {
            widths.push_back(count);
            count = 0;
        } else if (c == '#') {
            ++count;
        }
    }
    REQUIRE(widths.size() > 4);
    CHECK(widths[widths.size() / 2] > widths.front());
}

TEST_CASE("a material declared twice is one entry in the tool's palette, not two") {
    // The palette is what Q and E step through in game, and it came out **550 long for the 25
    // materials the facility declares**: `_contract.clip` holds them all and twenty-two fragments
    // include it, so every one of those includes pushed the whole list again.
    //
    // It cannot be de-duplicated by type id, which is why it was not. `behaviour.material` is the
    // count of names seen so far, so re-declaring `granite` interns a record that differs in that
    // one field and mints a NEW id for a material that is identical in every way a player can see.
    // The name is the identity; the id is not.
    //
    // Reported as "changing material with q and e no longer works", which is what a palette
    // twenty-two times too long feels like from the other side of the screen.
    const Built b = build(R"(
metre 32
bounds 0 0 0  1 1 1
material stone rgb=120,120,116
material moss  rgb=60,110,50
material stone rgb=120,120,116
material moss  rgb=60,110,50
let block = box 0 0 0  1 1 1
paint stone
solid block
)");
    REQUIRE(b.script.errors.empty());
    REQUIRE(b.script.ok());
    CHECK(b.script.material_types.size() == 2);
    // ...and in the order the clip declares them, because that is the order a player steps
    // through. A replacement that appended would put the second `stone` after `moss`.
    CHECK(b.script.material_types[0] != b.script.material_types[1]);
}

TEST_CASE("re-declaring a material replaces what the palette holds for that name") {
    // A fragment is allowed to override a material the contract declared, and when it does the
    // palette has to hold the fragment's version rather than the contract's -- so this is a
    // replacement and not a skip. Painting with the name and stepping to it with E must reach the
    // same voxel type, or the tool hands you a material the world was not built with.
    const Built b = build(R"(
metre 32
bounds 0 0 0  1 1 1
material stone rgb=120,120,116
material stone rgb=200,40,40
let block = box 0 0 0  1 1 1
paint stone
solid block
)");
    REQUIRE(b.script.errors.empty());
    REQUIRE(b.script.ok());
    REQUIRE(b.script.material_types.size() == 1);
    // The palette's entry is the LAST declaration, which is the one `paint stone` used.
    REQUIRE(b.script.material_types[0] < b.script.material_names.size());
    CHECK(b.script.material_names[b.script.material_types[0]] == "stone");
}

TEST_CASE("a part beside the world wins over the one the game ships, and is the only thing that "
          "can freeze a world") {
    // D494's resolution order, pinned, because it is what a world is assembled out of and because
    // getting it wrong is invisible from the game.
    //
    // Beside-wins is deliberate: it is what lets a player copy the facility's parts next to their
    // own world, edit a wall, and get their wall. Nothing here argues with that. What this test is
    // for is the OTHER half of it, which had no test and cost five days.
    //
    // Before D494 the game copied those parts into the player's worlds folder itself. The copying
    // stopped; the copies stayed. So an upgraded shelf holds a folder of fragments frozen at
    // whatever date it was made, beside-wins makes that folder the building, and every fix to the
    // shipped clip afterwards goes into the game and never into the world. Reported as a doorway
    // still barred after the bars were removed, and reasonably blamed on a cache -- a frozen world
    // and a stale cache are the same picture from the player's chair.
    //
    // Three cases, because the bug lives in the difference between them: beside wins when it is
    // there, shipped is used when it is not, and a name that is in neither place is an error that
    // says so rather than a world that silently loses a wall.
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "ws_test_include_order";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root / "mine" / "parts", ignored);
    std::filesystem::create_directories(root / "shipped" / "parts", ignored);

    const auto put = [](const std::filesystem::path& at, const std::string& text) {
        std::ofstream out(at, std::ios::binary);
        out << text;
    };
    put(root / "mine" / "world.clip", "include \"parts/wall.clip\"\n");
    put(root / "mine" / "parts" / "wall.clip", "let wall = box 0 0 0  1 1 1\n");
    put(root / "shipped" / "parts" / "wall.clip", "let wall = box 0 0 0  9 9 9\n");

    const std::string shipped = (root / "shipped").string();
    const std::string manifest = (root / "mine" / "world.clip").string();

    {
        std::vector<SourceLine> origin;
        std::vector<ScriptError> errors;
        const std::string text = expand_includes(manifest, origin, errors, shipped);
        CHECK(errors.empty());
        // The player's copy, not the game's. The 9s are the shipped file and must not appear.
        CHECK(text.find("1 1 1") != std::string::npos);
        CHECK(text.find("9 9 9") == std::string::npos);
    }

    // Take the copy away and the same manifest builds from the game's own parts -- which is what
    // makes deleting it the cure for a frozen world rather than a way to empty one. Before D494
    // this left a world that opened as an empty sky, three times.
    std::filesystem::remove(root / "mine" / "parts" / "wall.clip", ignored);
    {
        std::vector<SourceLine> origin;
        std::vector<ScriptError> errors;
        const std::string text = expand_includes(manifest, origin, errors, shipped);
        CHECK(errors.empty());
        CHECK(text.find("9 9 9") != std::string::npos);
    }

    // And a piece that is in neither place is one error naming the piece, not a quiet hole in the
    // building.
    put(root / "mine" / "world.clip", "include \"parts/roof.clip\"\n");
    {
        std::vector<SourceLine> origin;
        std::vector<ScriptError> errors;
        expand_includes(manifest, origin, errors, shipped);
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].message.find("parts/roof.clip") != std::string::npos);
    }

    std::filesystem::remove_all(root, ignored);
}

TEST_CASE("paint specks: a voxel alone in its material is found, a thin line is not") {
    // The audit behind D609. A speck is a solid voxel touching air whose six face neighbours are
    // none of them its own material -- which is what a paint rule bleeding two centimetres onto
    // its neighbour leaves behind, and what nothing else here can see: the volume is right, the
    // components are right because a speck is welded to what it sits on, and two voxels out of
    // forty thousand do not move a histogram.
    //
    // Three cases, because the value of the number is entirely in what it does NOT flag.
    Clip clip;
    clip.size[0] = clip.size[1] = clip.size[2] = 7;
    clip.voxels.assign(7 * 7 * 7, kAir);
    clip.inside.assign(7 * 7 * 7, 1);
    const VoxelTypeId stone = 11;
    const VoxelTypeId gilt = 12;

    // A solid slab of one material, THREE voxels deep so there is a genuinely buried layer.
    // Two was not enough and the test caught it: the underside of a two-deep slab is a face
    // touching air, so every voxel in it is on the surface and nothing is buried at all.
    for (i32 z = 1; z <= 5; ++z) {
        for (i32 x = 1; x <= 5; ++x) {
            for (i32 y = 1; y <= 3; ++y) clip.voxels[clip.index(x, y, z)] = stone;
        }
    }
    clip.build_coarse();
    CHECK(paint_specks(clip).specks == 0);

    // One voxel of another material dropped into the top face. Alone on all six sides, touching
    // air above: exactly the report.
    clip.voxels[clip.index(3, 3, 3)] = gilt;
    clip.build_coarse();
    {
        const SpeckReport dots = paint_specks(clip);
        REQUIRE(dots.specks == 1);
        REQUIRE(dots.by_type.size() == 1);
        CHECK(dots.by_type[0].type == gilt);
        REQUIRE(dots.examples.size() == 1);
        CHECK(dots.examples[0].at[0] == 3);
        CHECK(dots.examples[0].at[1] == 3);
        CHECK(dots.examples[0].at[2] == 3);
    }

    // A ONE-VOXEL-WIDE INLAY LINE of the same material is not a speck and must never be reported
    // as one: consecutive voxels along it are face neighbours of each other. This is the case
    // that decides whether the number is usable at all -- the building is full of lines a voxel
    // and a half across, and an audit that cried about every one of them would be turned off.
    for (i32 x = 1; x <= 5; ++x) clip.voxels[clip.index(x, 3, 3)] = gilt;
    clip.build_coarse();
    {
        const SpeckReport dots = paint_specks(clip);
        CHECK(dots.specks == 0);
    }

    // And a buried voxel of the wrong material is not reported, because nobody can see it. The
    // fraction printed beside the count would otherwise sit on a denominator no one can look at.
    for (i32 x = 1; x <= 5; ++x) clip.voxels[clip.index(x, 3, 3)] = stone;
    clip.voxels[clip.index(3, 2, 3)] = gilt;   // the middle layer, stone on all six faces
    clip.build_coarse();
    {
        const SpeckReport dots = paint_specks(clip);
        CHECK(dots.specks == 0);
    }
}

TEST_CASE("despeckle repaints a lone voxel and leaves a deliberate stipple alone") {
    // D610. The pass has to do two opposite things and the whole of its value is in the second.
    Clip clip;
    clip.size[0] = clip.size[1] = clip.size[2] = 11;
    clip.voxels.assign(11 * 11 * 11, kAir);
    clip.inside.assign(11 * 11 * 11, 1);
    const VoxelTypeId stone = 21;
    const VoxelTypeId gilt = 22;
    const VoxelTypeId moss = 23;

    for (i32 z = 1; z <= 9; ++z) {
        for (i32 x = 1; x <= 9; ++x) {
            for (i32 y = 1; y <= 3; ++y) clip.voxels[clip.index(x, y, z)] = stone;
        }
    }
    // One accident: a single gilt voxel in the top face, alone on all six sides.
    clip.voxels[clip.index(5, 3, 5)] = gilt;
    // And a stipple: moss scattered across the same face so that most moss voxels are isolated.
    // This is what a weathering coat looks like, and it must survive the pass untouched.
    // Twenty-four of them, because a stipple is a large SHARE and a large NUMBER: see the floor
    // in despeckle(). A dither of four voxels is not a dither.
    for (i32 z = 1; z <= 9; z += 2) {
        for (i32 x = 1; x <= 9; x += 2) {
            if (x == 5 && z == 5) continue;
            clip.voxels[clip.index(x, 3, z)] = moss;
        }
    }
    clip.build_coarse();

    const usize moss_before = static_cast<usize>(
        std::count(clip.voxels.begin(), clip.voxels.end(), moss));
    REQUIRE(moss_before > 0);

    const DespeckleReport out = despeckle(clip);

    // The accident is gone, repainted as the stone it was sitting on.
    CHECK(clip.at(5, 3, 5) == stone);
    CHECK(out.repainted == 1);
    REQUIRE(out.by_type.size() == 1);
    CHECK(out.by_type[0].type == gilt);

    // Every moss voxel is exactly where it was: a dither smoothed away is a coat that no longer
    // exists, and this is the assertion that stops a later "improvement" from eating one.
    CHECK(static_cast<usize>(std::count(clip.voxels.begin(), clip.voxels.end(), moss)) ==
          moss_before);
    CHECK(out.left > 0);

    // And no matter moved. Despeckling is a paint pass; if it ever changes the solid count it has
    // become something else and every measurement taken through it is wrong.
    CHECK(std::count(clip.voxels.begin(), clip.voxels.end(), kAir) ==
          11 * 11 * 11 - static_cast<i64>(9 * 9 * 3));

    // Idempotent: running it twice must change nothing the second time.
    const DespeckleReport again = despeckle(clip);
    CHECK(again.repainted == 0);
}

// D649, and it is the arithmetic half of R11d's blocker: can a verdict taken box by box be the
// verdict taken over the whole thing?
//
// The ladder samples one node at a time and the stipple verdict is a ratio per material over the
// WHOLE building, so R11d's question is whether the per-box counts sum to the whole-box counts.
// D628 measured that they do not: `paint_specks` reads outside the box it is given as air, so every
// voxel on a box's own face is counted as surface and as a speck, and 296 of a node's 512 cells are
// its own face. `stipple_counts(clip, margin)` is the repair -- count only the interior and let the
// neighbours come out of a shell -- and this is the claim it has to hold up:
//
//   the interiors tile the region exactly once, so the sum over them is the same population the
//   whole region would count, voxel for voxel.
//
// Asserted here with no sampler in the way, because the claim is about counting and not about
// fields; `--stipple-tiled` is the same question asked of the facility, where the boxes come from
// the sampler and the answer cost 2.02x to obtain (D649).
TEST_CASE("counts summed over boxes with a one-voxel skirt are the whole region's counts") {
    constexpr i32 kEdge = 32;
    constexpr i32 kTile = 8;
    const VoxelTypeId stone = 21;
    const VoxelTypeId moss = 23;
    const VoxelTypeId bronze = 24;

    Clip whole;
    whole.size[0] = whole.size[1] = whole.size[2] = kEdge;
    whole.voxels.assign(static_cast<usize>(kEdge) * kEdge * kEdge, kAir);
    whole.inside.assign(whole.voxels.size(), 1);

    // A slab with a top face, a dither over that face, and a one-voxel rib standing on it. The rib
    // is the case that matters: a material whose every voxel is on some box's face is the one a
    // naive sum invents specks for and a shrunken population loses altogether.
    for (i32 z = 0; z < kEdge; ++z) {
        for (i32 x = 0; x < kEdge; ++x) {
            for (i32 y = 8; y < 13; ++y) whole.voxels[whole.index(x, y, z)] = stone;
        }
    }
    for (i32 z = 1; z < kEdge; z += 2) {
        for (i32 x = 1; x < kEdge; x += 2) whole.voxels[whole.index(x, 12, z)] = moss;
    }
    for (i32 z = 0; z < kEdge; ++z) whole.voxels[whole.index(16, 13, z)] = bronze;
    whole.build_coarse();

    // The reference: the same cells the interior boxes hold, counted in one pass. A margin of one
    // tile is exactly "everything but the outer shell of boxes".
    const StippleCounts reference = stipple_counts(whole, kTile);
    REQUIRE(reference.any());

    // Every interior box, copied out with a one-voxel skirt, counted one voxel in.
    const auto box_at = [&](i32 tx, i32 ty, i32 tz, i32 skirt) {
        const i32 edge = kTile + 2 * skirt;
        Clip out;
        out.size[0] = out.size[1] = out.size[2] = edge;
        out.voxels.assign(static_cast<usize>(edge) * edge * edge, kAir);
        out.inside.assign(out.voxels.size(), 1);
        for (i32 z = 0; z < edge; ++z) {
            for (i32 y = 0; y < edge; ++y) {
                for (i32 x = 0; x < edge; ++x) {
                    out.voxels[out.index(x, y, z)] = whole.at(tx * kTile - skirt + x,
                                                              ty * kTile - skirt + y,
                                                              tz * kTile - skirt + z);
                }
            }
        }
        out.build_coarse();
        return out;
    };

    StippleCounts skirted;
    StippleCounts bare;
    for (i32 tz = 1; tz + 1 < kEdge / kTile; ++tz) {
        for (i32 ty = 1; ty + 1 < kEdge / kTile; ++ty) {
            for (i32 tx = 1; tx + 1 < kEdge / kTile; ++tx) {
                skirted.add(stipple_counts(box_at(tx, ty, tz, 1), 1));
                bare.add(stipple_counts(box_at(tx, ty, tz, 0), 0));
            }
        }
    }

    // Exact, material for material, both counts. "Close" is not an answer to whether a sum is a
    // sum: the verdict is a ratio with a floor on the numerator, so a few voxels either way can
    // move which materials are spared.
    CHECK(skirted.by_type.size() == reference.by_type.size());
    for (const auto& [type, want] : reference.by_type) {
        const auto found = skirted.by_type.find(type);
        REQUIRE(found != skirted.by_type.end());
        CHECK(found->second.surface == want.surface);
        CHECK(found->second.specks == want.specks);
    }

    // And the same verdict, which is the thing that reaches the world.
    const StippleVerdict from_whole = stipple_verdict(reference, 0.05);
    const StippleVerdict from_boxes = stipple_verdict(skirted, 0.05);
    CHECK(from_whole.allowed.size() == from_boxes.allowed.size());
    for (const auto& [type, may] : from_whole.allowed) {
        const auto found = from_boxes.allowed.find(type);
        REQUIRE(found != from_boxes.allowed.end());
        CHECK(found->second == may);
    }
    // The dither survives both readings, which is what the whole pass exists for.
    CHECK_FALSE(from_whole.allowed.at(moss));
    CHECK_FALSE(from_boxes.allowed.at(moss));

    // The control, and it must stay broken. Summing the boxes with no skirt is what the ladder
    // does today, and D628 refuted it: the invented air on every box face turns interior voxels
    // into surface and lone voxels into specks. If this ever starts agreeing, the margin path has
    // stopped doing anything and the test above has stopped testing it.
    bool bare_agrees = bare.by_type.size() == reference.by_type.size();
    for (const auto& [type, want] : reference.by_type) {
        const auto found = bare.by_type.find(type);
        if (found == bare.by_type.end() || found->second.surface != want.surface ||
            found->second.specks != want.specks) {
            bare_agrees = false;
        }
    }
    CHECK_FALSE(bare_agrees);
}

// `origin` moves the names a file bound, and not only its solid.
//
// `apply_origin` translates the solid, every paint rule and the bounds, and its comment says the
// whole point is that nothing is left behind. `script.parts` was left behind for the life of the
// facility, and because it does not fail, it answers: asking for one part sampled an unmoved shape
// inside a box that had moved. The facility shifts 3.50 m, so `--part part_dome` reported an
// 11.75 x 1.00 x 11.75 m saucer wearing one material instead of a 4 m dome wearing six -- the
// slice of it that happened to still fall inside the dropped box, painted by whatever rule was
// 3.50 m lower. `--part part_pilasters` reported eleven materials on a part that paints two.
//
// The shipped building was never wrong: it is built from `solid`, which moved correctly. Only the
// measurements were, which is worse in its way -- two separate agents concluded their own fragment
// was broken before either suspected the instrument.
TEST_CASE("origin moves a bound part, not only the solid") {
    VoxelTypeTable types;
    TagRegistry tags;
    const Script script = parse_clip_script(
        "metre 8\n"
        "bounds -4 -4 -4   4 4 4\n"
        "let part_ball = sphere 0 1 0 r=0.5\n"
        "solid part_ball\n"
        "origin 0 -3 0\n",
        types, tags);
    REQUIRE(script.ok());

    u32 part = 0;
    REQUIRE(script.part("part_ball", part));

    // The sphere was written centred on y = 1 and the origin drops everything by 3, so the part a
    // tool asks for by name must now be centred on y = -2 exactly as the solid is. Sampling the
    // field at both centres is the whole assertion: the old one must be air, the new one matter.
    const Field& f = script.field;
    CHECK(f.eval(part, Vec3{0.0, -2.0, 0.0}) < 0.0);
    CHECK(f.eval(part, Vec3{0.0, 1.0, 0.0}) > 0.0);

    // And the part still agrees with the solid it was unioned into, which is the property that
    // actually matters: a tool that asks for one part must see the same matter the building has.
    CHECK(f.eval(script.solid, Vec3{0.0, -2.0, 0.0}) < 0.0);
    CHECK(f.eval(part, Vec3{0.0, -2.0, 0.0})
          == doctest::Approx(f.eval(script.solid, Vec3{0.0, -2.0, 0.0})));
}

// --- the words added for detail: branch, scatter, chamfer, the readers, the stretch -----------
//
// These are checked through the parser rather than against `Field` directly, because what is being
// added is VOCABULARY: the arithmetic is tested in test_field.cpp, and what can go wrong here is a
// key read under the wrong name, a default that is not the one documented, or a word that quietly
// shadows one an existing clip already uses.

TEST_CASE("branch grows a trunk that forks, and the same seed grows the same tree twice") {
    // The generator exists because a tree is currently written a capsule at a time — four citrus in
    // clips/facility/terrace.clip are one straight capsule and a lumpy ball each. So the two things
    // that have to be true are that it makes MORE than one limb, and that it makes the SAME limbs
    // every time, on every machine: a clip whose tree differs between two players is not a clip.
    VoxelTypeTable types;
    TagRegistry tags;
    const std::string body =
        "material bark rgb=90,70,50\n"
        "let tree = branch 0 0 0 h=1.2 r=0.05 levels=4 count=3 spread=0.11 seed=7\n"
        "paint bark\nsolid tree\n";
    const Script one = parse_clip_script(body, types, tags);
    REQUIRE(one.errors.empty());
    u32 tree = 0;
    REQUIRE(one.part("tree", tree));

    // 2 segments x (1 + 3 + 9 + 27) limbs of capsule, plus the unions over them.
    CHECK(one.field.size() > 80);

    // The base of the trunk is inside the wood, and a metre out to the side is not.
    CHECK(one.field.eval(tree, Vec3{0, 0.05, 0}) < 0.0);
    CHECK(one.field.eval(tree, Vec3{1.5, 0.6, 0}) > 0.0);

    // It reaches UP: the trunk plus four levels of shrinking limbs stands taller than the trunk.
    bool above_the_trunk = false;
    for (int i = -20; i <= 20 && !above_the_trunk; ++i) {
        for (int k = -20; k <= 20; ++k) {
            if (one.field.eval(tree, Vec3{i * 0.05, 1.45, k * 0.05}) < 0.0) {
                above_the_trunk = true;
                break;
            }
        }
    }
    CHECK(above_the_trunk);

    VoxelTypeTable types_again;
    TagRegistry tags_again;
    const Script twice = parse_clip_script(body, types_again, tags_again);
    REQUIRE(twice.errors.empty());
    u32 same = 0;
    REQUIRE(twice.part("tree", same));
    for (int i = -8; i <= 8; ++i) {
        for (int j = 0; j <= 12; ++j) {
            const Vec3 p{i * 0.13, j * 0.13, 0.07};
            CHECK(one.field.eval(tree, p) == twice.field.eval(same, p));
        }
    }
}

TEST_CASE("a different seed grows a different tree, and a bigger plan is refused with its sums") {
    VoxelTypeTable types;
    TagRegistry tags;
    const Script script = parse_clip_script(
        "material bark rgb=90,70,50\n"
        "let a = branch 0 0 0 h=1.0 r=0.04 levels=4 count=3 seed=1\n"
        "let b = branch 0 0 0 h=1.0 r=0.04 levels=4 count=3 seed=2\n"
        "paint bark\nsolid a\n",
        types, tags);
    REQUIRE(script.errors.empty());
    u32 a = 0, b = 0;
    REQUIRE(script.part("a", a));
    REQUIRE(script.part("b", b));
    usize differing = 0;
    for (int i = -8; i <= 8; ++i) {
        for (int j = 0; j <= 10; ++j) {
            const Vec3 p{i * 0.11, j * 0.14, 0.05};
            if (script.field.eval(a, p) != script.field.eval(b, p)) ++differing;
        }
    }
    CHECK(differing > 40);

    // A plan that would cost thousands of nodes is refused BEFORE any of it is pushed, and the
    // message carries the arithmetic — "too complex" tells an author nothing they can act on.
    VoxelTypeTable big_types;
    TagRegistry big_tags;
    const Script too_big = parse_clip_script(
        "material bark rgb=90,70,50\n"
        "let huge = branch 0 0 0 levels=9 count=6 segments=8\n"
        "paint bark\nsolid huge\n",
        big_types, big_tags);
    REQUIRE(!too_big.errors.empty());
    CHECK(too_big.errors.front().message.find("capsules") != std::string::npos);
}

TEST_CASE("scatter reads repeat's keys and adds the two that break the lattice") {
    VoxelTypeTable types;
    TagRegistry tags;
    const Script script = parse_clip_script(
        "material gravel rgb=140,134,120\n"
        "let pebble = sphere 0 0 0 r=0.02\n"
        "let grid = repeat { pebble } x=0.1 z=0.1 nx=6 nz=6\n"
        "let bed  = scatter { pebble } x=0.1 z=0.1 nx=6 nz=6 jitter=0.45 turn=0.5\n"
        "let flat = scatter { pebble } x=0.1 z=0.1 nx=6 nz=6 jitter=0 turn=0\n"
        "paint gravel\nsolid bed\n",
        types, tags);
    REQUIRE(script.errors.empty());
    u32 grid = 0, bed = 0, flat = 0;
    REQUIRE(script.part("grid", grid));
    REQUIRE(script.part("bed", bed));
    REQUIRE(script.part("flat", flat));

    usize differing = 0;
    for (int i = -14; i <= 14; ++i) {
        for (int k = -14; k <= 14; ++k) {
            const Vec3 p{i * 0.033, 0.0, k * 0.041};
            if (script.field.eval(bed, p) != script.field.eval(grid, p)) ++differing;
            // ...and with both dials at nought it is the grid, exactly.
            CHECK(script.field.eval(flat, p) == script.field.eval(grid, p));
        }
    }
    CHECK(differing > 200);
}

TEST_CASE("chamfer= cuts a seam flat and changes nothing about the shapes it joins") {
    VoxelTypeTable types;
    TagRegistry tags;
    const Script script = parse_clip_script(
        "material stone rgb=120,120,116\n"
        "let plinth = box -1 0 -1  1 0.4 1\n"
        "let die    = box -0.8 0.4 -0.8  0.8 1.2 0.8\n"
        "let plain  = union { plinth die }\n"
        "let cut    = union { plinth die } chamfer=0.06\n"
        "paint stone\nsolid cut\n",
        types, tags);
    REQUIRE(script.errors.empty());
    u32 plain = 0, cut = 0;
    REQUIRE(script.part("plain", plain));
    REQUIRE(script.part("cut", cut));

    // In the re-entrant angle where the die stands on the plinth the chamfer has added stone.
    CHECK(script.field.eval(plain, Vec3{0.83, 0.43, 0.0}) > 0.0);
    CHECK(script.field.eval(cut, Vec3{0.83, 0.43, 0.0}) < 0.0);
    // Half a metre from the seam nothing has moved.
    CHECK(script.field.eval(cut, Vec3{0.0, 0.9, 0.0}) ==
          doctest::Approx(script.field.eval(plain, Vec3{0.0, 0.9, 0.0})));
    CHECK(script.field.eval(cut, Vec3{0.0, 0.1, 0.0}) ==
          doctest::Approx(script.field.eval(plain, Vec3{0.0, 0.1, 0.0})));
}

TEST_CASE("occlusion, curvature, facing and cell_edge are askable from a clip at last") {
    // `Field` has answered all four since the weathering was written and the language could not
    // ask any of them, so `weather sea 0.5` could put salt in a hollow and an author could not put
    // moss in one. What is checked is the SIGN of each, at a place whose answer is not in doubt.
    VoxelTypeTable types;
    TagRegistry tags;
    const Script script = parse_clip_script(
        "material stone rgb=120,120,116\n"
        "material moss  rgb=60,90,50\n"
        "let block  = box -1 0 -1  1 1 1\n"
        "let notch  = box  0.4 0.4 -2   2 2 2\n"
        "let carved = difference { block notch }\n"
        "let cavity = occlusion { carved } r=0.25\n"
        "let arris  = curvature { carved } r=0.10\n"
        "let up     = facing { carved } axis=y\n"
        "let craze  = cell_edge size=0.2 seed=3\n"
        "paint stone\n"
        "paint moss where=cavity above=0.55\n"
        "solid carved\n",
        types, tags);
    REQUIRE(script.errors.empty());
    u32 cavity = 0, arris = 0, up = 0, craze = 0;
    REQUIRE(script.part("cavity", cavity));
    REQUIRE(script.part("arris", arris));
    REQUIRE(script.part("up", up));
    REQUIRE(script.part("craze", craze));

    // Buried in the middle of the block, everything around is stone.
    CHECK(script.field.eval(cavity, Vec3{-0.5, 0.5, 0.0}) == doctest::Approx(1.0));
    // Out in the open beside it, nothing is.
    CHECK(script.field.eval(cavity, Vec3{-2.0, 0.5, 0.0}) == doctest::Approx(0.0));
    // The re-entrant corner the notch cut is concave, and the block's own top arris is convex.
    CHECK(script.field.eval(arris, Vec3{0.4, 0.4, 0.0}) < 0.0);
    CHECK(script.field.eval(arris, Vec3{-1.0, 1.0, 0.0}) > 0.0);
    // The top of the block faces up and the underside faces down.
    CHECK(script.field.eval(up, Vec3{-0.5, 1.0, 0.0}) > 0.5);
    CHECK(script.field.eval(up, Vec3{-0.5, 0.0, 0.0}) < -0.5);
    // A seam field is nought on a seam and positive away from one, never negative.
    CHECK(script.field.eval(craze, Vec3{0.13, 0.07, 0.21}) >= 0.0);

    // And the rule keyed on the cavity actually fires, which is the failure that looks like
    // success — a paint rule that never fires paints nothing and reports nothing.
    CHECK(script.paint.size() == 2);
}

TEST_CASE("stretch= runs a grain one way, and leaving it off is the grain that was there") {
    VoxelTypeTable types;
    TagRegistry tags;
    const Script script = parse_clip_script(
        "material bark rgb=90,70,50\n"
        "let plain   = fbm size=0.1 octaves=4 seed=11\n"
        "let ones    = fbm size=0.1 octaves=4 seed=11 stretch=1,1,1\n"
        "let running = fbm size=0.1 octaves=4 seed=11 stretch=1,8,1\n"
        "let evenly  = fbm size=0.1 octaves=4 seed=11 stretch=3\n"
        "let coarse  = fbm size=0.3 octaves=4 seed=11\n"
        "let trunk   = cylinder 0 0 0 r=0.2 h=2 axis=y\n"
        "paint bark\nsolid trunk\n",
        types, tags);
    REQUIRE(script.errors.empty());
    u32 plain = 0, ones = 0, running = 0, evenly = 0, coarse = 0;
    REQUIRE(script.part("plain", plain));
    REQUIRE(script.part("ones", ones));
    REQUIRE(script.part("running", running));
    REQUIRE(script.part("evenly", evenly));
    REQUIRE(script.part("coarse", coarse));

    usize moved = 0;
    for (int i = -10; i <= 10; ++i) {
        for (int j = -10; j <= 10; ++j) {
            const Vec3 p{i * 0.037, j * 0.041, 0.019};
            CHECK(script.field.eval(ones, p) == script.field.eval(plain, p));
            // One number is the same stretch on every axis, which is the same grain three times
            // coarser — the shape `size=` already has. Approximately and not bit for bit: one
            // divides the point by three and then by 0.1, the other divides it by 0.3, and those
            // are the same number by every measure except the last bit of the mantissa.
            CHECK(script.field.eval(evenly, p) ==
                  doctest::Approx(script.field.eval(coarse, p)).epsilon(1e-9));
            if (script.field.eval(running, p) != script.field.eval(plain, p)) ++moved;
        }
    }
    CHECK(moved > 300);
}

TEST_CASE("fewer levels is the same tree with its outer limbs left off") {
    // This is what makes the bark recipe work, so it is asserted rather than assumed.
    //
    // `clips/_trees.clip` found the trap the hard way: a displacement applies to everything under
    // it, and 0.032 m of bark grain into a twig of radius 0.019 ERASES THE TWIG wherever the noise
    // is negative -- silently, dropping lengths of tree and leaving whatever they carried floating.
    // The cure is to displace the thick wood only, and the cheapest way to say that here is to
    // grow the same tree twice at two depths and displace the shallow one.
    //
    // That only works if `levels` truncates rather than reshapes: the limbs a shallow tree grows
    // must be exactly the limbs the deep one grows, in the same places, so the two union without a
    // seam. They are -- a limb's direction, length and radius come from its own identity and the
    // seed, and nothing in the walk consults how much deeper it is going to go.
    VoxelTypeTable types;
    TagRegistry tags;
    const Script script = parse_clip_script(
        "material bark rgb=90,70,50\n"
        "let bole = branch 0 0 0 h=1.2 r=0.05 levels=2 count=3 spread=0.11 seed=5\n"
        "let full = branch 0 0 0 h=1.2 r=0.05 levels=5 count=3 spread=0.11 seed=5\n"
        "paint bark\nsolid full\n",
        types, tags);
    REQUIRE(script.errors.empty());
    u32 bole = 0, full = 0;
    REQUIRE(script.part("bole", bole));
    REQUIRE(script.part("full", full));

    // Everywhere the shallow tree is matter, the deep one is matter too -- and nowhere is the
    // shallow one nearer, because the deep one is the same wood plus more of it.
    const Field& f = script.field;
    usize inside = 0;
    for (int i = -14; i <= 14; ++i) {
        for (int j = 0; j <= 20; ++j) {
            for (int k = -14; k <= 14; ++k) {
                const Vec3 p{i * 0.06, j * 0.08, k * 0.06};
                const f64 shallow = f.eval(bole, p);
                REQUIRE(f.eval(full, p) <= shallow + 1e-9);
                if (shallow < 0.0) ++inside;
            }
        }
    }
    CHECK(inside > 0);   // a tree that is nowhere would satisfy the above for nothing
}
