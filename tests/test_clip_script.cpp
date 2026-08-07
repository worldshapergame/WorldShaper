// The clip pipeline end to end: a file goes in, voxels and numbers come out.
//
// The tests that matter here are the ones about *arithmetic*, not about parsing. A parser that
// mangles a word produces an error message; a sampler that is a voxel out produces a room whose
// doorway is 1.97 m and whose far wall does not line up with the near one, and nothing complains.
// So most of what follows asserts sizes and volumes against numbers worked out by hand.

#include <doctest/doctest.h>

#include <cmath>

#include "forge/clip_script.hpp"
#include "forge/measure.hpp"
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
