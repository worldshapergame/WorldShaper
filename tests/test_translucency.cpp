// What R4e claims about stone, in numbers, held against the clip that was written to test it.
//
// # Why these numbers are worth a test at all
//
// `clips/translucency_test.clip` states the design in its own header: *"what makes marble marble is
// that the thin parts glow and the thick parts do not, and no clip says which is which — the
// renderer has to find it out by counting the voxels a scattered ray crosses before it is
// extinguished."* So the whole feature rests on one mapping — a byte to a depth — and on the
// extinction over the distance a ray actually measured.
//
// That mapping is a decision, and a decision with a number in it is exactly the thing that rots
// silently: someone changes `kTranslucentDepthVoxels` from four to eight, every panel in that clip
// still glows, and nothing says the two ends of the row stopped being different. What follows is
// the row itself, panel by panel, at the thicknesses the clip actually builds.
//
// As with `test_refraction.cpp`, this pins the ARITHMETIC and not the picture: the shader is what
// measures the crossing, and whether it measures it correctly needs a graphics card and an eye.

#include <doctest/doctest.h>

#include <cmath>
#include <initializer_list>

#include "core/types.hpp"

using namespace ws;

namespace {

// shaders/node.glsl, `face_medium_depth`. Four voxels at the top of the byte's range.
constexpr f64 kTranslucentDepthVoxels = 4.0;
f64 depth_voxels(int translucency) {
    return static_cast<f64>(translucency) / 255.0 * kTranslucentDepthVoxels;
}

// What survives a crossing of `voxels` voxels of it. shaders/shade_faces.comp casts a coin against
// this and the counter converges to it; here it is the value the coin is weighted by.
f64 survives(int translucency, f64 voxels) {
    const f64 depth = depth_voxels(translucency);
    if (depth <= 0.0) return 0.0;
    return std::exp(-voxels / depth);
}

// The clip is authored in metres at 32 voxels to the metre.
f64 voxels_of(f64 metres) { return metres * 32.0; }

}   // namespace

TEST_CASE("a material that never wrote the byte scatters nothing, at any thickness") {
    // The property that makes this safe to apply to every face in the building: the branch cannot
    // be entered by stone, metal or glass, because `translucent=` defaults to nought and nought is
    // not a small amount of glow, it is none.
    CHECK(survives(0, 0.1) == doctest::Approx(0.0));
    CHECK(survives(0, 1.0) == doctest::Approx(0.0));
    CHECK(depth_voxels(0) == doctest::Approx(0.0));
}

TEST_CASE("marble stops light in about a voxel, which is what the clip says it does") {
    // `clips/translucency_test.clip`: "Marble stops light in about a voxel, so a wall of it is stone
    // and only the lip of a moulding is not." The facility's marble is translucent=110.
    const int marble = 110;
    CHECK(depth_voxels(marble) == doctest::Approx(1.725).epsilon(0.01));

    // One voxel through: still clearly lit. Four voxels -- the lip of the moulding the clip builds
    // at 0.07 m -- about a tenth, which reads as a glowing edge against a dark body.
    CHECK(survives(marble, 1.0) > 0.5);
    CHECK(survives(marble, 4.0) == doctest::Approx(0.099).epsilon(0.05));

    // And the body behind it -- the clip's `tt_body`, 0.35 m of solid stone -- is stone. It is not
    // exactly nothing and the honest number is worth writing down rather than rounding away: 0.15%
    // of the sun gets through eleven voxels of marble, and the composite then takes the material's
    // own 43% share of that, which is 0.0007. A frame has 255 levels in it; this is a fifth of one.
    CHECK(survives(marble, voxels_of(0.35)) == doctest::Approx(0.0015).epsilon(0.05));
    CHECK(survives(marble, voxels_of(0.35)) * (110.0 / 255.0) < 1.0 / 255.0);
}

TEST_CASE("the five panels of the test clip come out as five different things") {
    // The row the clip builds, in the order it builds them. p0 is the control -- the same stone
    // with the byte left at zero -- and the four after it are alabaster at 220, from a thirteenth
    // of a metre down to a hand's breadth.
    const int alabaster = 220;
    const f64 p0 = voxels_of(0.13);   // marble, translucency 0: the picture the renderer used to draw
    const f64 p1 = voxels_of(0.04);
    const f64 p2 = voxels_of(0.07);
    const f64 p3 = voxels_of(0.16);
    const f64 p4 = voxels_of(0.40);

    CHECK(survives(0, p0) == doctest::Approx(0.0));

    // Thicker is darker, strictly, which is the whole of what the row is for.
    CHECK(survives(alabaster, p1) > survives(alabaster, p2));
    CHECK(survives(alabaster, p2) > survives(alabaster, p3));
    CHECK(survives(alabaster, p3) > survives(alabaster, p4));

    // And the four are far enough apart to be four things rather than one thing with noise on it.
    // A tenth of a stop between neighbours would be a row nobody could read.
    CHECK(survives(alabaster, p1) == doctest::Approx(0.692).epsilon(0.02));
    CHECK(survives(alabaster, p2) == doctest::Approx(0.531).epsilon(0.02));
    CHECK(survives(alabaster, p3) == doctest::Approx(0.231).epsilon(0.02));
    CHECK(survives(alabaster, p4) == doctest::Approx(0.025).epsilon(0.05));

    // The thickest panel is not black either, and that matters: a hand's breadth of alabaster with
    // the sun behind it is dim, not dead. What kills it entirely is the metre-of-matter budget the
    // marcher gives up at, which no panel in this clip reaches.
    CHECK(survives(alabaster, p4) > 0.0);
}

TEST_CASE("the marcher's budget is far past anything the byte can describe") {
    // `kCrossedMax` is 32 voxels, and it is a bound on the WALK rather than a physical constant.
    // The check is that it never becomes the thing that decides a picture: the most translucent
    // material describable has to be extinguished long before a ray gives up, or the budget would
    // be a hidden second material property.
    const f64 crossed_max = 32.0;
    CHECK(survives(255, crossed_max) < 1e-3);
    // ...with three voxels of margin at the very top of the range, so the two never meet.
    CHECK(survives(255, crossed_max - 3.0) < 1e-2);
}

TEST_CASE("the coin is an unbiased estimator of the fraction it stands in for") {
    // `face_accumulate` counts whole samples, so a term that is 43% transmitted arrives as 43% of
    // samples reaching the sun. The estimator is a coin weighted by `survives`, and what makes that
    // legitimate is that its mean IS the fraction -- so the face converges to the same number a
    // fractional counter would have held, using counters that already exist.
    //
    // Checked over a deterministic sweep rather than with a generator: a coin against p over
    // uniformly spaced draws in [0,1) lands heads exactly floor(p*n) times, so the estimate is
    // within one sample of p for every p. That is the property; the shader's hash supplies the
    // uniformity and is its own business.
    for (const int translucency : {40, 110, 220, 255}) {
        for (const f64 thickness : {0.5, 1.0, 2.0, 4.0, 8.0}) {
            const f64 p = survives(translucency, thickness);
            const int n = 512;
            int heads = 0;
            for (int i = 0; i < n; ++i) {
                if ((static_cast<f64>(i) + 0.5) / static_cast<f64>(n) < p) ++heads;
            }
            const f64 estimate = static_cast<f64>(heads) / static_cast<f64>(n);
            CHECK(estimate == doctest::Approx(p).epsilon(0.01).scale(1.0));
        }
    }
}
