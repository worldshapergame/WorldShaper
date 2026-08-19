// R5c's and R5d's second halves, in numbers, and the three things about them that can rot quietly.
//
// Both changes live almost entirely in `shaders/resolve.comp`, which no test can run: there is no
// card in this suite and there is no software path for a compute shader. So this pins what CAN be
// held from here, and each of the three is something that has already gone wrong in this repository
// in some other file.
//
//   - **The wire.** R5d's third layer is nine bits appended to a word two shaders share, one of
//     which writes it and the other reads it. D133 is the fault where a writer and a reader
//     disagree about which bit means what and the picture merely looks a little wrong; D703's own
//     table names it as trap 7 -- "a writer and reader that disagree give a mirrored reflection
//     that looks very nearly right". The bit numbers are read out of both files and held against
//     each other and against the eighteen bits that were already spoken for.
//
//   - **The arithmetic.** R5c's whole claim is that both arms of a pair of levels aim at the SAME
//     number. That is an identity, it is four lines, and it is exactly the sort of thing that is
//     true when it is written and false after somebody changes one side of it. The identity is
//     checked, and so is the one inexactness in it -- the composite recomputes the blend weight from
//     a `t` that is up to a brick's diagonal further along the ray than the one the marcher chose
//     the level at -- against the bound that makes it harmless.
//
//   - **The determinism.** §1's standing clause is *no per-pixel random numbers* and the handover
//     has named `hash_u32` in the composite as its last holdout since R3d. Reading the file settles
//     what that function actually was, and the check is that no call to it survives -- because a
//     clause nobody can test is a clause that comes back.
//
// As with test_sun_confidence.cpp, test_refraction.cpp and test_translucency.cpp, this pins the
// ARITHMETIC and not the picture. Whether the composite draws the right three layers over the right
// pixels needs a graphics card and an eye.

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>

#include "core/types.hpp"
#include "gpu/render_params.hpp"

using namespace ws;

namespace {

std::string shader_text(const char* name) {
    const std::string path = std::string(WS_SHADER_SOURCE_DIR) + "/" + name;
    std::ifstream file(path, std::ios::binary);
    REQUIRE_MESSAGE(file.good(), "could not open " << path);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// ---- the wire, mirrored ------------------------------------------------------------------------
//
// `visibility_pack_behind` in shaders/visibility.comp, word z, as R5d's second half leaves it. The
// low nineteen bits are R4d's and D663's and are not this change's to move.
constexpr u32 kBehindFarCoverageShift = 19;
constexpr u32 kBehindSkyBeyondBit = 1u << 27;
constexpr u32 kBehindSpokenFor = 0x0007FFFFu;   // bits 0..18: g, b, far_hit, present, edge

u32 pack_behind_z(u32 green, u32 blue, bool far_hit, bool edge, u32 far_coverage, bool sky_beyond) {
    return green | (blue << 8u) | (far_hit ? (1u << 16u) : 0u) | (1u << 17u) |
           (edge ? (1u << 18u) : 0u) | ((far_coverage < 255u ? far_coverage : 255u) << 19u) |
           (sky_beyond ? kBehindSkyBeyondBit : 0u);
}

// ...and the two fields shaders/resolve.comp reads back out of it.
u32 unpack_far_coverage(u32 z) { return (z >> kBehindFarCoverageShift) & 0xFFu; }
bool unpack_sky_beyond(u32 z) { return (z & kBehindSkyBeyondBit) != 0; }

// ---- the composite's three layers, mirrored -----------------------------------------------------
//
// `c1` is how much of the pixel the NEAR node covers and `c2` how much of what is left the FAR node
// covers. The third layer is the sky and takes the remainder.
struct Layers {
    f64 near_share;
    f64 far_share;
    f64 sky_share;
};

Layers shares(f64 c1, f64 c2) {
    const f64 through = 1.0 - c1;
    return Layers{c1, through * c2, through * (1.0 - c2)};
}

// ---- R5c's blend weight, mirrored ---------------------------------------------------------------
//
// `voxel_blend_weight` in shaders/resolve.comp, and `node_march`'s own `detail` in
// shaders/node.glsl, which are the same two lines against the same numbers.
f64 detail_of(f64 t, f64 pixel_angle, f64 detail_bias) {
    const f64 footprint = t * pixel_angle * detail_bias;
    return std::log2(footprint < 1.0 ? 1.0 : footprint);
}

f64 blend_weight(f64 t, f64 pixel_angle, f64 detail_bias) {
    const f64 d = detail_of(t, pixel_angle, detail_bias);
    return d < 0.0 ? 0.0 : (d > 1.0 ? 1.0 : d);
}

// What a level-0 pixel of the pair draws: its own voxel colour, towards the brick average.
f64 level_zero_arm(f64 voxel, f64 brick, f64 w) { return voxel * (1.0 - w) + brick * w; }

// ...and what the level-1 pixel of the same pair draws, which is the marcher's half (D664): the
// brick average, towards the voxel the ray entered, by one minus the same fraction.
f64 level_one_arm(f64 voxel, f64 brick, f64 w) { return brick * w + voxel * (1.0 - w); }

// shaders/node.glsl, `kNodeBlendDeadband`.
constexpr f64 kNodeBlendDeadband = 1.0 / 16.0;

}   // namespace

TEST_CASE("R5d's third layer takes bits nothing else in the word was using") {
    // Nineteen bits were already spoken for and this change may not have moved one of them.
    const u32 was = pack_behind_z(0x7Fu, 0x3Cu, true, true, 255u, false);
    const u32 now = pack_behind_z(0x7Fu, 0x3Cu, true, true, 128u, true);
    CHECK((was & kBehindSpokenFor) == (now & kBehindSpokenFor));

    // ...and the two new fields survive the round trip.
    CHECK(unpack_far_coverage(now) == 128u);
    CHECK(unpack_sky_beyond(now));
    CHECK(unpack_far_coverage(was) == 255u);
    CHECK_FALSE(unpack_sky_beyond(was));

    // Nine bits, and the word still has four to spare above them. A field that ran into bit 32
    // would wrap into nothing and read as zero -- "solid" -- which is the failure that looks like
    // no change at all.
    CHECK((kBehindFarCoverageShift + 8u) == 27u);
    CHECK(kBehindSkyBeyondBit == (1u << 27));
    CHECK((now >> 28u) == 0u);
}

TEST_CASE("the writer and the reader agree about where those two fields are") {
    // Both halves of the wire, read out of the two files that carry them. D133's fault is a writer
    // and a reader that disagree by one shift, and the symptom is a picture that looks very nearly
    // right -- so the shifts are held against each other here rather than by eye.
    const std::string writer = shader_text("visibility.comp");
    const std::string reader = shader_text("resolve.comp");

    CHECK(writer.find("(min(far_coverage, 255u) << 19u)") != std::string::npos);
    CHECK(writer.find("(sky_beyond ? (1u << 27u) : 0u)") != std::string::npos);
    CHECK(reader.find("(behind.z >> 19u) & 0xFFu") != std::string::npos);
    CHECK(reader.find("(behind.z >> 27u) & 1u") != std::string::npos);

    // The eighteen bits D663 and R4d put there first are still read where they were written.
    CHECK(writer.find("(1u << 17u)") != std::string::npos);
    CHECK(reader.find("(behind.z >> 17u) & 1u") != std::string::npos);
    CHECK(reader.find("(behind.z >> 18u) & 1u") != std::string::npos);
}

TEST_CASE("three layers make one pixel, and a solid far layer is the two-layer answer exactly") {
    const Layers l = shares(0.25, 0.5);
    CHECK(l.near_share + l.far_share + l.sky_share == doctest::Approx(1.0));
    CHECK(l.near_share == doctest::Approx(0.25));
    CHECK(l.far_share == doctest::Approx(0.375));
    CHECK(l.sky_share == doctest::Approx(0.375));

    // The claim the control arm rests on: with the far node solid -- which is what 255 unpacks to,
    // what R4d's glass packs, and what `--no-edge-layers` and `--no-edge-aa` leave in the word --
    // the sky share is nought and the near and far shares are D663's two, to the bit.
    for (f64 c1 = 0.0; c1 <= 1.0; c1 += 0.125) {
        const Layers solid = shares(c1, 1.0);
        CHECK(solid.sky_share == 0.0);
        CHECK(solid.near_share == c1);
        CHECK(solid.far_share == 1.0 - c1);
    }

    // A far layer that covers nothing hands its whole share on rather than losing it, which is the
    // arithmetic that stops an open lattice at forty metres darkening the sky behind it.
    const Layers open = shares(0.5, 0.0);
    CHECK(open.far_share == 0.0);
    CHECK(open.sky_share == doctest::Approx(0.5));
}

TEST_CASE("R5c: both arms of the (0, 1) pair aim at the same number") {
    // The whole of R5c in one identity. Two neighbouring pixels of the 4x4 tile, one of which the
    // dither sent to level 0 and the other to level 1, sitting at the same fraction between the two
    // levels: what they draw is now the same number, where before this the level-0 arm drew the
    // voxel outright and was a whole step away.
    const f64 voxel = 0.20;   // a dark voxel...
    const f64 brick = 0.80;   // ...in a pale brick, which is the worst pair for this
    for (f64 w = 0.0; w <= 1.0; w += 0.0625) {
        CHECK(level_zero_arm(voxel, brick, w) == doctest::Approx(level_one_arm(voxel, brick, w)));
    }

    // The two ends are the two pictures at two scales, untouched.
    CHECK(level_zero_arm(voxel, brick, 0.0) == doctest::Approx(voxel));
    CHECK(level_zero_arm(voxel, brick, 1.0) == doctest::Approx(brick));

    // And what it removes: without the blend the level-0 arm is the voxel at every weight, so the
    // pair is the whole step apart wherever the tile straddles the boundary.
    CHECK(std::fabs(voxel - level_one_arm(voxel, brick, 0.5)) == doctest::Approx(0.3));
}

TEST_CASE("the weight the composite recomputes is the weight the marcher used, within a brick") {
    // The one honest inexactness. The marcher chooses the level at the `t` where the ray enters a
    // BRICK; the inner walk then steps single voxels and the visibility buffer records the `t` it
    // stopped at, up to a brick's diagonal further on. The composite has only the second number.
    const f64 pixel_angle = 2.0 / 800.0;   // 1280x800, a 90 degree lens
    const f64 bias = 1.0;                  // quality pinned, `--no-auto-quality --quality 7`

    // Where a level-0 pixel can be at all: the footprint runs from one voxel to two, which at these
    // numbers is 400 to 800 voxels, twelve and a half metres to twenty-five.
    const f64 near_t = 1.0 / (pixel_angle * bias);
    const f64 far_t = 2.0 / (pixel_angle * bias);
    CHECK(blend_weight(near_t, pixel_angle, bias) == doctest::Approx(0.0));
    CHECK(blend_weight(far_t, pixel_angle, bias) == doctest::Approx(1.0));

    // A brick is eight voxels and its diagonal is 13.86. Worst case the marcher measured the level
    // that much nearer than the buffer says, and the whole of the disagreement is at the near end of
    // the band where a proportional error in `t` is largest.
    const f64 brick_diagonal = 8.0 * 1.7320508075688772;
    f64 worst = 0.0;
    for (f64 t = near_t; t <= far_t; t += 1.0) {
        const f64 here = blend_weight(t, pixel_angle, bias);
        const f64 marcher = blend_weight(t - brick_diagonal, pixel_angle, bias);
        const f64 gap = std::fabs(here - marcher);
        if (gap > worst) worst = gap;
    }

    // Under the marcher's own deadband, which is what makes it not worth carrying the number across
    // the wire -- and forty times smaller than the whole step it removes.
    CHECK(worst < kNodeBlendDeadband);
    CHECK(worst < 0.06);
    CHECK(1.0 / worst > 16.0);

    // Beyond the band the clamp holds, rather than a `fract` wrapping a weight that has crept over
    // one back to nought and putting a seam across the middle of the blend.
    CHECK(blend_weight(far_t * 1.5, pixel_angle, bias) == doctest::Approx(1.0));
    CHECK(blend_weight(near_t * 0.5, pixel_angle, bias) == doctest::Approx(0.0));
}

TEST_CASE("the composite has no random number in it, and never had the dither it was accused of") {
    const std::string reader = shader_text("resolve.comp");

    // No CALL and no DEFINITION. The name survives on exactly one line, a `#define` that maps it to
    // `sky_cell_mix` for pt_sky.glsl's benefit -- that file is shared with clouds.comp, which keeps
    // its own copy of the mix, so the alias is here rather than a rename there. A call site would
    // have a bracket after it and this is what says there is none.
    CHECK(reader.find("hash_u32(") == std::string::npos);
    CHECK(reader.find("#define hash_u32 sky_cell_mix") != std::string::npos);

    // And the ordered dither the handover placed in this file is not in it. It is `node_bayer` in
    // shaders/node.glsl -- a fixed sixteen-entry table, not a hash -- and D664 keeps it on purpose,
    // because it decides which cell a ray stops on and therefore the coverage byte, the face key
    // and the depth. What R5c took off it was the colour. A CALL is what is looked for, here and
    // above, because the name has to be sayable in a comment for any of this to be explicable.
    CHECK(reader.find("node_bayer(") == std::string::npos);
    CHECK(shader_text("node.glsl").find("float node_bayer(ivec2 pixel)") != std::string::npos);

    // The two callers that made the composite look as though it had one. Both key on a WORLD CELL
    // and not on a pixel, so two frames from one camera get identical answers out of them, which is
    // what D194's gate measures.
    const std::string sky = shader_text("pt_sky.glsl");
    CHECK(sky.find("float sky_hash(vec2 cell)") != std::string::npos);
    CHECK(sky.find("vec3 star_field(vec3 dir)") != std::string::npos);
}

TEST_CASE("the parameter block's tail is in one order in both files, and no name is used twice") {
    // The assert in the header counts and does not compare, and FIVE separate worktrees appended a
    // vector to this block in one afternoon without any of them knowing about the others. So the
    // ORDER is checked here, and since one of those collisions was two agents choosing the same
    // NAME, so is that.
    //
    // That collision is why this case is worth more than it looks. Two worktrees both added a
    // `vec4 r5`; merged, git saw two identical `f32 r5[4];` lines and kept one, the `static_assert`
    // was satisfied because the SIZE was right, and `main.cpp` then filled one field twice with two
    // different sets of dials. The second write won and the composite read the sun's settings as its
    // own. **A duplicate name is the one failure the size assert cannot see**, and nothing but a
    // check like this one catches it.
    CHECK(sizeof(RenderParams) == 93u * 16u);
    CHECK(offsetof(RenderParams, r4) == 90u * 16u);
    CHECK(offsetof(RenderParams, r5) == 91u * 16u);
    CHECK(offsetof(RenderParams, r5b) == 92u * 16u);
    CHECK(offsetof(RenderParams, r5) - offsetof(RenderParams, r4) == 16u);
    CHECK(offsetof(RenderParams, r5b) - offsetof(RenderParams, r5) == 16u);

    // ...and shaders/params.glsl declares them in that same order, which is the half the assert
    // cannot see at all.
    const std::string params = shader_text("params.glsl");
    const usize at_r4 = params.find("vec4 r4;");
    const usize at_r5 = params.find("vec4 r5;");
    const usize at_r5b = params.find("vec4 r5b;");
    REQUIRE(at_r4 != std::string::npos);
    REQUIRE(at_r5 != std::string::npos);
    REQUIRE(at_r5b != std::string::npos);
    CHECK(at_r4 < at_r5);
    CHECK(at_r5 < at_r5b);

    // Every vector in the block is declared exactly once. `find` on the next occurrence rather than
    // a count, so the failure names which one repeated.
    for (const char* field : {"vec4 r4;", "vec4 r5;", "vec4 r5b;", "vec4 beam;", "uvec4 derive;"}) {
        const usize first = params.find(field);
        REQUIRE(first != std::string::npos);
        INFO("declared twice in params.glsl: " << field);
        CHECK(params.find(field, first + 1) == std::string::npos);
    }

    // Nothing declared after the last one, or the host would be writing one structure while every
    // shader reads another at every offset past the gap.
    const usize after = at_r5b + 9;
    CHECK(params.find("vec4 ", after) == std::string::npos);
    CHECK(params.find("uvec4 ", after) == std::string::npos);
    CHECK(params.find("ivec4 ", after) == std::string::npos);
}
