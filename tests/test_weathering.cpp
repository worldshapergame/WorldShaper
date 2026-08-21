// Weathering, and the rule that no two voxels should be alike.
//
// Both are easy to implement in a way that looks plausible and is wrong. Weathering that is a
// texture pasted over a surface looks like weathering in a screenshot and stops looking like it
// the moment the shape changes; variation that is not deterministic gives a different clip every
// time it is built, which is unusable for anything shared. So these check the properties rather
// than the appearance: that the derived quantities really do follow the geometry, that the
// deformation really does cut matter away, and that building the same clip twice gives the same
// clip.

#include <doctest/doctest.h>

#include <set>
#include <string>

#include "forge/clip_script.hpp"
#include "forge/field.hpp"
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
    VariationReport variety;
};

Built build(const std::string& text) {
    Built out;
    VoxelTypeTable types;
    TagRegistry tags;
    out.script = parse_clip_script(text, types, tags);
    if (out.script.ok()) {
        out.result = sample(out.script.field, out.script.solid, out.script.paint,
                            out.script.settings, nullptr);
        out.variety = apply_variation(out.result.clip, types, out.script.field,
                                      out.script.variation, out.script.settings, out.result);
        out.measurement = measure(out.result.clip, out.script.settings.voxels_per_metre);
    }
    return out;
}

}  // namespace

TEST_CASE("curvature tells an outside edge from an inside corner") {
    // The quantity everything shape-aware rests on. Positive where a surface bulges towards the
    // outside — an arris, which wind and traffic wear away — and negative in a hollow, where
    // sand, soot and moss collect. A weathering that cannot tell those apart is a texture.
    Field f;
    const u32 block = f.box({0, 0, 0}, {1, 1, 1});
    const u32 curve = f.curvature(block, 0.08);

    // Just outside a corner of the box: convex.
    CHECK(f.eval(curve, {1.05, 1.05, 1.05}) > 0.0);
    // Just outside the middle of a face: flat, so near zero either way.
    CHECK(std::abs(f.eval(curve, {1.05, 0.0, 0.0})) < 0.5);

    // An inside corner, made by carving a smaller box out of a larger one.
    Field g;
    const u32 outer = g.box({0, 0, 0}, {2, 2, 2});
    const u32 inner = g.box({2, 2, 0}, {1.5, 1.5, 3});
    const u32 room = g.subtract({outer, inner});
    const u32 hollow = g.curvature(room, 0.08);
    CHECK(g.eval(hollow, {0.45, 0.45, 0}) < 0.0);
}

TEST_CASE("occlusion knows a cavity from the open air") {
    Field f;
    const u32 slab = f.box({0, -1, 0}, {4, 1, 4});
    const u32 cavity = f.occlusion(slab, 0.2);
    // Well above the slab: nothing around, so nothing occludes.
    CHECK(f.eval(cavity, {0, 1.0, 0}) == doctest::Approx(0.0));
    // Buried in it: everything around is solid.
    CHECK(f.eval(cavity, {0, -1.0, 0}) == doctest::Approx(1.0));
    // Sitting on the surface: about half.
    const f64 at_surface = f.eval(cavity, {0, 0.0, 0});
    CHECK(at_surface > 0.2);
    CHECK(at_surface < 0.8);
}

TEST_CASE("a crack is the seam between cells, and seams are thin") {
    // Cracks are not drawn as lines. They are where two nearest scattered points are equally
    // near, which is a network that branches and closes on itself the way real cracking does.
    Field f;
    const u32 seams = f.cell_edge(1.0, 7u);
    // The value is a distance, so it is never negative and is small only near a seam.
    u32 near_seam = 0;
    u32 samples = 0;
    for (f64 x = 0; x < 6.0; x += 0.05) {
        const f64 v = f.eval(seams, {x, 0.3, 0.7});
        CHECK(v >= 0.0);
        ++samples;
        if (v < 0.05) ++near_seam;
    }
    // Thin: a small fraction of the line is on a seam, which is what makes it read as a crack
    // rather than as marbling.
    CHECK(near_seam > 0);
    CHECK(near_seam * 4 < samples);
}

TEST_CASE("weathering by cracks takes matter away and weathering by growth adds it") {
    const Built plain = build(R"(
metre 16
bounds -1.6 0 -1.6  1.6 2.2 1.6
material stone rgb=150,148,142
let block = box -1.2 0 -1.2  1.2 1.8 1.2
paint stone
solid block
)");
    const Built cracked = build(R"(
metre 16
bounds -1.6 0 -1.6  1.6 2.2 1.6
material stone rgb=150,148,142
let block = box -1.2 0 -1.2  1.2 1.8 1.2
paint stone
solid block
weather cracks 0.8 scale=0.7 seed=3
)");
    REQUIRE(plain.script.ok());
    REQUIRE(cracked.script.ok());
    CHECK(cracked.measurement.solid < plain.measurement.solid);
    // And it is fissures rather than a shrink: far more surface for slightly less matter.
    CHECK(cracked.measurement.exposed_faces > plain.measurement.exposed_faces);
}

TEST_CASE("each weathering brings its own materials and puts them where the shape says") {
    const Built b = build(R"(
metre 16
bounds -1.6 0 -1.6  1.6 2.2 1.6
material stone rgb=150,148,142
let block = box -1.2 0 -1.2  1.2 1.8 1.2
paint stone
solid block
weather overgrown 0.7 scale=1.0 seed=5
)");
    REQUIRE(b.script.ok());
    // Moss and lichen were never declared in the file; the weathering brought them.
    bool has_moss = false;
    for (const std::string& name : b.script.material_names) {
        if (name == "moss") has_moss = true;
    }
    CHECK(has_moss);
    CHECK(b.measurement.types.size() >= 2);
}

// --- weathering that knows where it is allowed to work ----------------------------------------
//
// The debt this pays. Weathering used to act on the whole clip: one `weather desert` on a building
// standing in a garden bleached the grass as readily as the stone, because the coats it adds are
// keyed on a value rather than a place and they go on after everything the author painted. The
// building shipped with no weathering at all rather than with that — which left the one system
// written specifically for a stone building outdoors untested on the only stone building there is.

namespace {

// How many voxels of the clip are of a named material. Weathering brings its own materials, so
// counting them is how "did this coat land here and not there" is asked.
u64 count_of(const Built& b, const std::string& name) {
    VoxelTypeId wanted = 0;
    bool found = false;
    for (usize i = 0; i < b.script.material_names.size(); ++i) {
        if (b.script.material_names[i] == name) {
            wanted = static_cast<VoxelTypeId>(i);
            found = true;
        }
    }
    if (!found) return 0;
    u64 total = 0;
    for (VoxelTypeId v : b.result.clip.voxels) {
        if (v == wanted) ++total;
    }
    return total;
}

// Two blocks side by side of the same material, one named and one not. Whatever `weather` is
// asked to do, it has to happen to the left one and not the right one.
std::string two_blocks(const std::string& weathering) {
    return R"(
metre 16
bounds -2.2 0 -1.1   2.2 1.7 1.1
material stone rgb=150,148,142
let left  = box -2.0 0 -1.0  -0.1 1.5 1.0
let right = box  0.1 0 -1.0   2.0 1.5 1.0
let both  = union { left right }
paint stone
solid both
)" + weathering;
}

}  // namespace

TEST_CASE("weathering scoped to a shape leaves everything else alone") {
    const Built plain = build(two_blocks(""));
    const Built whole = build(two_blocks("weather overgrown 0.8 scale=0.6 seed=5\n"));
    const Built scoped = build(two_blocks("weather overgrown 0.8 scale=0.6 seed=5 on=left\n"));
    REQUIRE(plain.script.ok());
    REQUIRE(whole.script.ok());
    REQUIRE(scoped.script.ok());

    // Unscoped it covers both blocks; scoped it covers rather less, because half the clip is now
    // out of bounds for it.
    const u64 all_moss = count_of(whole, "moss") + count_of(whole, "lichen");
    const u64 some_moss = count_of(scoped, "moss") + count_of(scoped, "lichen");
    CHECK(all_moss > 0);
    CHECK(some_moss > 0);
    CHECK(some_moss < all_moss);

    // And the half it was not asked about is untouched: exactly the stone it started as, voxel
    // for voxel. Counted on the right-hand block alone, which is the whole point of the feature.
    const Clip& before = plain.result.clip;
    const Clip& after = scoped.result.clip;
    REQUIRE(before.size[0] == after.size[0]);
    const i32 middle = before.size[0] / 2;
    u64 differing = 0;
    for (i32 z = 0; z < before.size[2]; ++z) {
        for (i32 y = 0; y < before.size[1]; ++y) {
            for (i32 x = middle + 2; x < before.size[0]; ++x) {
                const usize i = before.index(x, y, z);
                if (before.voxels[i] != after.voxels[i]) ++differing;
            }
        }
    }
    CHECK(differing == 0);
}

TEST_CASE("a scoped deformation cuts only inside its own shape") {
    // The other half of scoping, and the half that cannot be seen in a material count: cracks take
    // matter away, and a crack scoped to one block must not open in the block beside it.
    const Built plain = build(two_blocks(""));
    const Built scoped = build(two_blocks("weather cracks 0.9 scale=0.5 seed=11 on=left\n"));
    REQUIRE(scoped.script.ok());

    CHECK(scoped.measurement.solid < plain.measurement.solid);

    const Clip& before = plain.result.clip;
    const Clip& after = scoped.result.clip;
    const i32 middle = before.size[0] / 2;
    u64 lost_left = 0;
    u64 lost_right = 0;
    for (i32 z = 0; z < before.size[2]; ++z) {
        for (i32 y = 0; y < before.size[1]; ++y) {
            for (i32 x = 0; x < before.size[0]; ++x) {
                const usize i = before.index(x, y, z);
                if (before.voxels[i] == kAir || after.voxels[i] != kAir) continue;
                if (x < middle) ++lost_left;
                else ++lost_right;
            }
        }
    }
    CHECK(lost_left > 0);
    CHECK(lost_right == 0);
}

TEST_CASE("weathering on a name nothing was bound to is an error, not a silent whole-clip coat") {
    VoxelTypeTable types;
    TagRegistry tags;
    const Script script = parse_clip_script(R"(
metre 8
bounds 0 0 0 1 1 1
material stone rgb=1,2,3
let b = box 0 0 0 1 1 1
paint stone
solid b
weather desert 0.5 on=podium
)",
                                           types, tags);
    REQUIRE(!script.errors.empty());
    bool named = false;
    for (const ScriptError& e : script.errors) {
        if (e.message.find("podium") != std::string::npos) named = true;
    }
    CHECK(named);
}

TEST_CASE("weathering no longer costs the sampler every skip it had") {
    // Every deformation weathering builds is a pattern multiplied by an amount, and the old rule
    // for "how far can this move the surface" only knew about bare noises. So a weathered clip
    // reported an unbounded allowance, no box in it could ever be settled, and every voxel of the
    // bounding volume was asked through the whole expression. That is why it was too slow to ship,
    // and it was arithmetic rather than weathering that made it so.
    const Built weathered = build(two_blocks("weather desert 0.5 scale=0.8 seed=3 on=left\n"));
    REQUIRE(weathered.script.ok());
    CHECK(weathered.result.slack < 1.0);
    CHECK(weathered.result.voxels_settled > weathered.result.voxels_asked);
}

TEST_CASE("an unknown weathering is an error rather than a silent nothing") {
    VoxelTypeTable types;
    TagRegistry tags;
    const Script script = parse_clip_script(R"(
metre 8
bounds 0 0 0 1 1 1
material stone rgb=1,2,3
let b = box 0 0 0 1 1 1
paint stone
solid b
weather rusted 0.5
)",
                                           types, tags);
    REQUIRE(!script.errors.empty());
    bool named = false;
    for (const ScriptError& e : script.errors) {
        if (e.message.find("rusted") != std::string::npos) named = true;
    }
    CHECK(named);
}

TEST_CASE("no two voxels alike, and the same clip twice is the same clip") {
    const std::string text = R"(
metre 16
bounds -1.1 0 -1.1  1.1 1.6 1.1
material stone rgb=150,148,142 rough=200
variation colour=0.05 rough=0.10 seed=9
let block = box -1 0 -1  1 1.4 1
paint stone
solid block
)";
    const Built a = build(text);
    const Built b = build(text);
    REQUIRE(a.script.ok());

    // The rule: a surface with no two square centimetres the same. Not literally achievable at
    // every scale, so what is asserted is that it is close and that the report says how close.
    CHECK(a.variety.voxels > 1000);
    CHECK(a.variety.distinct_types > a.variety.voxels / 4);
    CHECK(a.variety.largest_group < a.variety.voxels / 100);

    // Deterministic: hashed from where a voxel is, not from when it was built. A clip that comes
    // out differently each time cannot be shared, cached, or compared against itself.
    CHECK(a.variety.distinct_types == b.variety.distinct_types);
    CHECK(a.variety.largest_group == b.variety.largest_group);
    REQUIRE(a.result.clip.cell_count() == b.result.clip.cell_count());
    CHECK(a.result.clip.content_hash() == b.result.clip.content_hash());
}

TEST_CASE("variation stays inside its budget so the renderer's table cannot overflow") {
    // The budget is a safety rail rather than a knob. A clip that asks for more records than the
    // GPU's type table holds used to take the renderer down with an assertion, which is a poor
    // way to learn that a surface was too varied.
    const Built b = build(R"(
metre 16
bounds -1.1 0 -1.1  1.1 1.6 1.1
material stone rgb=150,148,142 rough=200
variation colour=0.08 rough=0.15 seed=3 budget=64
let block = box -1 0 -1  1 1.4 1
paint stone
solid block
)");
    REQUIRE(b.script.ok());
    CHECK(b.variety.distinct_types <= 64);
    CHECK(b.variety.reused > 0);   // it really did run out, so the rail was really tested
    CHECK(b.measurement.solid == b.variety.voxels);
}

TEST_CASE("variation leaves a sealed box sealed and empty space empty") {
    // Variation rewrites types, and a pass that rewrites types is a pass that can invent them.
    // Bounds are the box, so the only air on a line through the middle is the room itself —
    // sample wider and the "gap" becomes the outdoors with the walls as interruptions.
    const Built b = build(R"(
metre 16
bounds -1.2 0 -1.2  1.2 1.6 1.2
material stone rgb=150,148,142
variation colour=0.06 rough=0.1 seed=2
let walls = shell { box -1.2 0 -1.2  1.2 1.6 1.2 } thickness=0.12
paint stone
solid walls
)");
    REQUIRE(b.script.ok());
    // The hollow is still hollow: a line through the middle finds air.
    const Clip& clip = b.result.clip;
    const Span gap = gap_along(clip, 0, clip.size[1] / 2, clip.size[2] / 2);
    CHECK(gap.any);
    CHECK(gap.contiguous);
    CHECK(b.variety.voxels == b.measurement.solid);
}

// --- a property read from a field -----------------------------------------------------------------
//
// *I cant seem to connect something like a pattern of noise to the rgb red of a voxel type even
// though its the same blue type of wire.* It was the same kind of wire and the language had nowhere
// to put it: a voxel type's properties were numbers and only numbers, so every voxel of a type was
// the same colour before variation perturbed it and the same colour after.
//
// Checked as a PROPERTY rather than as a picture: the driven channel takes many values across the
// block and the two beside it take exactly one, which is the whole claim and cannot be true by
// accident.

namespace {

struct Painted {
    VoxelTypeTable types;
    Script script;
    SampleResult result;
    VariationReport variety;
    std::set<VoxelTypeId> before;   // the ids the paint put down, before any record was minted

    u8 one_of(u8 VisualRecord::*channel) {
        for (VoxelTypeId id : result.clip.voxels) {
            if (id != kAir) return types.visual_of(id).*channel;
        }
        return 0;
    }

    // How many different values one channel of the record takes, over every voxel that is not air.
    usize spread(u8 VisualRecord::*channel) {
        std::set<u8> seen;
        for (VoxelTypeId id : result.clip.voxels) {
            if (id == kAir) continue;
            seen.insert(types.visual_of(id).*channel);
        }
        return seen.size();
    }
};

void paint_it(Painted& out, const std::string& text) {
    TagRegistry tags;
    out.script = parse_clip_script(text, out.types, tags);
    REQUIRE(out.script.ok());
    out.result = sample(out.script.field, out.script.solid, out.script.paint,
                        out.script.settings, nullptr);
    for (VoxelTypeId id : out.result.clip.voxels) {
        if (id != kAir) out.before.insert(id);
    }
    out.variety = apply_variation(out.result.clip, out.types, out.script.field,
                                  out.script.variation, out.script.settings, out.result);
}

}  // namespace

TEST_CASE("a voxel type's colour can be read from a field") {
    const std::string driven =
        "metre 32\n"
        "let grain = fbm size=0.35 octaves=4 seed=3\n"
        "material speckle rgb=grain,90,70 rough=200\n"
        "let wall = box -1 0 -1  1 1 1\n"
        "solid wall\n"
        "paint speckle\n";

    Painted made;
    paint_it(made, driven);

    // The language read it: one property of one type, pointing at the field `grain` names.
    REQUIRE(made.script.variation.driven.size() == 1);
    CHECK(made.script.variation.driven[0].which == DrivenChannel::Which::Red);
    // The field it points at really is a pattern and not a constant.
    CHECK(made.script.field.eval(made.script.variation.driven[0].field, Vec3{0.1, 0.2, 0.3}) !=
          made.script.field.eval(made.script.variation.driven[0].field, Vec3{3.1, 1.2, 2.3}));
    // And it is filed against the type the paint actually put down.
    REQUIRE(made.before.size() == 1);
    CHECK(made.script.variation.driven[0].type == *made.before.begin());

    // And the pass applied it: the red takes many values over the block, the green and the blue
    // take exactly the one the document wrote.
    CHECK(made.variety.voxels > 1000);
    // **A pattern runs from -1 to 1 and not from 0 to 1**, which is the whole of why the first
    // version of this drew a flat black wall. Probed rather than assumed.
    const u32 f = made.script.variation.driven[0].field;
    CHECK(made.script.field.eval(f, Vec3{0.0, 0.5, 0.0}) < 0.0);
    CHECK(made.script.field.eval(f, Vec3{0.0, 0.5, 0.0}) > -1.0);
    CHECK(made.spread(&VisualRecord::red) > 8);
    CHECK(made.spread(&VisualRecord::green) == 1);
    CHECK(made.spread(&VisualRecord::blue) == 1);

    // The control arm: the same document with a NUMBER in that slot. Same shape, same paint, same
    // everything -- and one red.
    const std::string flat =
        "metre 32\n"
        "let grain = fbm size=0.35 octaves=4 seed=3\n"
        "material speckle rgb=128,90,70 rough=200\n"
        "let wall = box -1 0 -1  1 1 1\n"
        "solid wall\n"
        "paint speckle\n";

    Painted plain;
    paint_it(plain, flat);
    CHECK(plain.script.variation.driven.empty());
    CHECK(plain.spread(&VisualRecord::red) == 1);
    CHECK(plain.spread(&VisualRecord::green) == 1);

    // Nothing was minted at all in the control, because nothing asked for any: a clip with no
    // driven property and no variation pays for neither.
    CHECK(plain.variety.voxels == 0);
}

TEST_CASE("a property driven by something that names nothing is refused with a line") {
    VoxelTypeTable types;
    TagRegistry tags;
    const Script script = parse_clip_script(
        "metre 32\nmaterial speckle rgb=nosuchthing,90,70\nlet wall = box 0 0 0 1 1 1\n"
        "solid wall\n",
        types, tags);
    CHECK_FALSE(script.ok());
}
