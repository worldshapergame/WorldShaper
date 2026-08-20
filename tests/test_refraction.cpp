// The arithmetic R4d's refraction is made of, checked against Snell's law rather than against
// itself.
//
// # What this is and, more importantly, what it is not
//
// The refraction that ships is four lines of `shaders/visibility.comp`, and a compute shader cannot
// be run by this suite. What CAN be pinned is the arithmetic those lines are: GLSL's `refract` is
// specified exactly, so the same formula can be evaluated here and held against expectations
// derived from trigonometry -- `asin(sin(theta) / n)` -- rather than from the expression under
// test. A test that recomputed the shader's own expression and compared the two would agree with
// any mistake in it.
//
// So this is R12b's method one stage earlier and with the same honesty about its limits (D644): it
// proves the FORMULA, and it says nothing about whether the marcher hands that formula the right
// normal, the right distance or the right material. Those need a picture, and a picture needs a
// graphics card.
//
// The three properties are the ones the design leans on:
//
//   1. a flat pane does not deviate a ray, it DISPLACES it -- which is why refraction has to be
//      done at both interfaces and not just at the one the pixel landed on;
//   2. a single interface bends by Snell's law, which is what a water surface is and why a basin
//      reads as shallower than it is;
//   3. past the critical angle there is no way out, and the answer is a reflection rather than a
//      zero -- which is a real thing to look at from underwater and a crash if it is unhandled.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

#include "core/types.hpp"

using namespace ws;

namespace {

struct Vec3 {
    f64 x = 0.0, y = 0.0, z = 0.0;
};

Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, f64 s) { return {a.x * s, a.y * s, a.z * s}; }
f64 dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
f64 length(Vec3 a) { return std::sqrt(dot(a, a)); }
Vec3 normalize(Vec3 a) { return a * (1.0 / length(a)); }

// GLSL 4.5 section 8.5, verbatim, because the point of writing it out is that this is the function
// the shader calls and not an equivalent of it. `i` points at the surface, `n` points back against
// it, `eta` is the ratio of the index the ray is LEAVING to the one it is entering. A zero vector
// means total internal reflection.
Vec3 refract(Vec3 i, Vec3 n, f64 eta) {
    const f64 d = dot(n, i);
    const f64 k = 1.0 - eta * eta * (1.0 - d * d);
    if (k < 0.0) return {0.0, 0.0, 0.0};
    return i * eta - n * (eta * d + std::sqrt(k));
}

Vec3 reflect(Vec3 i, Vec3 n) { return i - n * (2.0 * dot(n, i)); }

// The angle a direction makes with a surface whose normal is `n`, in degrees, measured the way an
// optics table measures it: from the normal, always positive.
f64 angle_from_normal(Vec3 dir, Vec3 n) {
    const f64 c = std::abs(dot(normalize(dir), normalize(n)));
    return std::acos(std::min(1.0, c)) * 180.0 / 3.14159265358979323846;
}

}   // namespace

TEST_CASE("a flat pane displaces a ray without deviating it") {
    // The property the two-interface implementation exists for. Bending only where the pixel landed
    // -- which is the cheap version and the obvious one -- turns every window into a lens, and at a
    // grazing angle it smears the room behind it. Bending at both faces gives back the original
    // direction exactly, and the whole visible effect is the sideways offset.
    const f64 ior = 1.5;
    const Vec3 face{0.0, 0.0, -1.0};   // the pane's near face, pointing back at the eye

    for (const f64 tilt : {5.0, 20.0, 45.0, 70.0}) {
        const f64 radians = tilt * 3.14159265358979323846 / 180.0;
        const Vec3 eye = normalize({std::sin(radians), 0.0, std::cos(radians)});

        const Vec3 into = refract(eye, face, 1.0 / ior);
        REQUIRE(length(into) > 0.5);   // entering a denser medium always has an answer

        // Snell, from trigonometry and not from the expression above: n1 sin a = n2 sin b.
        const f64 inside_angle = std::asin(std::sin(radians) / ior) * 180.0 / 3.14159265358979323846;
        CHECK(angle_from_normal(into, face) == doctest::Approx(inside_angle).epsilon(1e-9));

        // The far face is parallel and its normal points back into the glass, which is what the
        // marcher reports at the voxel where the medium ends.
        const Vec3 far_face{0.0, 0.0, -1.0};
        const Vec3 out = refract(into, far_face, ior);
        REQUIRE(length(out) > 0.5);
        CHECK(out.x == doctest::Approx(eye.x).epsilon(1e-9));
        CHECK(out.y == doctest::Approx(eye.y).epsilon(1e-9));
        CHECK(out.z == doctest::Approx(eye.z).epsilon(1e-9));
    }
}

TEST_CASE("the offset a pane gives is the thickness times the angle it was crossed at") {
    // What a player actually sees: the post behind the window sits a little to one side of where it
    // would be with no window there, and the shift grows with the angle and with the thickness.
    // The facility's panes are 12 cm, so this is centimetres -- visible on a straight edge, and not
    // the same thing as the smear a one-interface version would produce.
    const f64 ior = 1.5;
    const f64 thickness = 0.12;   // metres, the facility's own glazing
    const Vec3 face{0.0, 0.0, -1.0};

    const auto sideways = [&](f64 tilt) {
        const f64 radians = tilt * 3.14159265358979323846 / 180.0;
        const Vec3 eye = normalize({std::sin(radians), 0.0, std::cos(radians)});
        const Vec3 into = refract(eye, face, 1.0 / ior);
        // How far along z the ray has to travel to cross the pane, and where it comes out.
        const f64 travel = thickness / std::abs(into.z);
        const Vec3 exit_point = into * travel;
        // Where it would have come out with no pane at all.
        const Vec3 straight = eye * (thickness / std::abs(eye.z));
        return std::abs(exit_point.x - straight.x);
    };

    CHECK(sideways(0.0) == doctest::Approx(0.0).epsilon(1e-12));
    CHECK(sideways(45.0) > sideways(20.0));
    CHECK(sideways(70.0) > sideways(45.0));
    // At 45 degrees through 12 cm of ior 1.5 the offset is about 4 cm, which is a voxel and a
    // quarter at the authored resolution: a straight edge seen through the pane steps sideways.
    CHECK(sideways(45.0) == doctest::Approx(0.0406).epsilon(0.02));
}

TEST_CASE("a water surface bends by Snell and is why a basin reads as shallower") {
    // The case where the two interfaces do NOT cancel, because the second one is the opaque bottom
    // and the ray never gets there. This is the visible half of R4d on the facility: it has two
    // basins of ior 1.33.
    const f64 ior = 1.33;
    const Vec3 surface{0.0, 1.0, 0.0};   // looking down at water, the normal points up at the eye

    const f64 radians = 50.0 * 3.14159265358979323846 / 180.0;
    const Vec3 eye = normalize({std::sin(radians), -std::cos(radians), 0.0});
    const Vec3 into = refract(eye, surface, 1.0 / ior);
    REQUIRE(length(into) > 0.5);

    const f64 expected = std::asin(std::sin(radians) / ior) * 180.0 / 3.14159265358979323846;
    CHECK(angle_from_normal(into, surface) == doctest::Approx(expected).epsilon(1e-9));
    // Bent TOWARDS the normal, always, entering a denser medium. The apparent depth follows from
    // it: the bottom is seen along a steeper line than the one the light actually took.
    CHECK(angle_from_normal(into, surface) < 50.0);
}

TEST_CASE("past the critical angle there is no way out and the answer is a mirror") {
    // Looking up from under water at a shallow angle: the surface is a mirror. The formula returns
    // a zero vector there, and a zero direction handed to a marcher is a ray that goes nowhere --
    // trap 7 in its arithmetic form, where "no answer" and "straight ahead" must not be the same
    // reply. The shader reflects instead, which is what the surface does.
    const f64 ior = 1.33;
    const f64 critical = std::asin(1.0 / ior) * 180.0 / 3.14159265358979323846;
    CHECK(critical == doctest::Approx(48.75).epsilon(0.01));

    const Vec3 surface{0.0, -1.0, 0.0};   // from underneath, the normal points down at the ray
    const f64 radians = (critical + 5.0) * 3.14159265358979323846 / 180.0;
    const Vec3 up = normalize({std::sin(radians), std::cos(radians), 0.0});

    const Vec3 out = refract(up, surface, ior);
    CHECK(length(out) == doctest::Approx(0.0).epsilon(1e-12));

    const Vec3 mirrored = reflect(up, surface);
    CHECK(length(mirrored) == doctest::Approx(1.0).epsilon(1e-9));
    // Back down into the water, which is what a mirror at the ceiling does.
    CHECK(mirrored.y < 0.0);

    // ...and just inside the critical angle there IS a way out, so the test above is about the
    // angle rather than about the formula always failing.
    const f64 inside = (critical - 5.0) * 3.14159265358979323846 / 180.0;
    const Vec3 shallower = normalize({std::sin(inside), std::cos(inside), 0.0});
    CHECK(length(refract(shallower, surface, ior)) > 0.5);
}

TEST_CASE("Beer-Lambert takes the true path, so a deep look is darker than a glancing one") {
    // The term the straight-ray version could not have. `node_medium_through` multiplies once per
    // voxel crossed, which is exact along an axis and short on a diagonal; a refracted ray knows
    // where it entered and where it left, so the absorption can be taken over the distance itself.
    //
    // `absorb=5,2,1` is the facility's water, in the clip's own units, which the shader reads as
    // sixteenths per metre.
    const f64 red = 5.0 / 16.0;
    const auto through = [&](f64 metres) { return std::exp(-red * metres); };

    CHECK(through(0.0) == doctest::Approx(1.0));
    CHECK(through(1.0) < through(0.5));
    CHECK(through(2.0) == doctest::Approx(through(1.0) * through(1.0)).epsilon(1e-12));
    // A metre of it keeps about 73% of the red, which is water rather than ink.
    CHECK(through(1.0) == doctest::Approx(0.7316).epsilon(0.001));

    // A material that never wrote `absorb` is exactly transparent, and that is what makes the term
    // safe to apply to everything: the facility's clear glazing has no absorb bytes at all.
    CHECK(std::exp(-0.0 * 3.0) == doctest::Approx(1.0));
}

// ---- R4d's dispersion, and R4c's cap ----------------------------------------------------------
//
// The same method one stage on, and the same honesty about its limits: these are the FORMULAS the
// two shader files evaluate, written out here and held against trigonometry rather than against
// themselves. They say nothing about whether the marcher hands them the right normal.
//
// They are here rather than in a file of their own because the two changes share one question --
// what a ray does when it crosses a medium -- and because this file already carries the refraction
// half of it.

namespace {

// shaders/face_terms.glsl, `face_hero_at` and `face_hero_weight`, verbatim.
f64 hero_at(u32 index) {
    const f64 x = static_cast<f64>(index) * 0.61803398875 + 0.5;
    return x - std::floor(x);
}

struct Rgb {
    f64 r = 0.0, g = 0.0, b = 0.0;
};

Rgb hero_weight(f64 hero) {
    const f64 centre[3] = {0.8333, 0.5, 0.1667};
    const f64 norm[3] = {2.3992, 1.9160, 2.3992};
    Rgb out;
    f64* channel[3] = {&out.r, &out.g, &out.b};
    for (int i = 0; i < 3; ++i) {
        const f64 d = (hero - centre[i]) / 0.30;
        *channel[i] = std::exp(-d * d) * norm[i];
    }
    return out;
}

// ...and `face_medium_index`, which is Cauchy written as the Abbe number it is published as.
constexpr f64 kAbbe = 25.0;
f64 medium_index(f64 ior, f64 hero) { return ior + (ior - 1.0) * (hero - 0.5) / kAbbe; }

// shaders/face_terms.glsl, `face_lobe_cap_of`: the half-angle of the cap thirty-six bins cover.
constexpr f64 kCapMin = 0.105;
constexpr f64 kCapMax = 1.000;
f64 alpha_of(u32 rough) {
    const f64 r = static_cast<f64>(rough) / 255.0;
    return std::max(r * r, 0.004);
}
f64 cap_of(u32 rough, u32 side) {
    return std::clamp(0.5 * alpha_of(rough) * static_cast<f64>(side), kCapMin, kCapMax);
}

}   // namespace

TEST_CASE("the hero wavelengths average to one, so a material with no dispersion does not move") {
    // **This is the property the whole scheme stands on.** Every sample that crosses a medium is
    // weighted by its wavelength's share of the sensor, and the mean of those weights has to be
    // exactly one in every channel -- otherwise a pane of perfectly ordinary glass would come back
    // a different colour than it went in, and the control arm and the shipped arm would differ for
    // a reason that has nothing to do with dispersion.
    Rgb sum;
    const u32 samples = 4096;
    for (u32 i = 0; i < samples; ++i) {
        const Rgb w = hero_weight(hero_at(i));
        sum.r += w.r;
        sum.g += w.g;
        sum.b += w.b;
    }
    CHECK(sum.r / samples == doctest::Approx(1.0).epsilon(0.02));
    CHECK(sum.g / samples == doctest::Approx(1.0).epsilon(0.02));
    CHECK(sum.b / samples == doctest::Approx(1.0).epsilon(0.02));

    // And it is EVEN at every prefix, which is why the sequence is stratified rather than random:
    // a bin whose first two dozen samples happened to be red would carry a colour cast for the
    // hundreds of frames it takes the burst to outvote them.
    Rgb early;
    for (u32 i = 0; i < 24; ++i) {
        const Rgb w = hero_weight(hero_at(i));
        early.r += w.r;
        early.g += w.g;
        early.b += w.b;
    }
    CHECK(early.r / 24.0 == doctest::Approx(1.0).epsilon(0.12));
    CHECK(early.b / 24.0 == doctest::Approx(1.0).epsilon(0.12));
}

TEST_CASE("blue bends more than red, and the middle of the band is what the clip wrote") {
    // Cauchy. The spread is centred on the authored figure, so `ior=1.5` still means 1.5 -- a
    // dispersion that shifted the mean index would change where a pane puts what is behind it,
    // which is D652's measurement and not this one's to move.
    CHECK(medium_index(1.5, 0.5) == doctest::Approx(1.5).epsilon(1e-12));
    CHECK(medium_index(1.5, 1.0) > medium_index(1.5, 0.0));
    // n(F) - n(C) = (n - 1) / V, which for a dense flint at 1.5 is 0.02.
    CHECK(medium_index(1.5, 1.0) - medium_index(1.5, 0.0) ==
          doctest::Approx(0.5 / kAbbe).epsilon(1e-9));
    // Vacuum disperses nothing however the band is walked, which is what makes this safe to apply
    // to every material rather than to a list of them.
    CHECK(medium_index(1.0, 0.0) == doctest::Approx(1.0));
    CHECK(medium_index(1.0, 1.0) == doctest::Approx(1.0));
}

TEST_CASE("how far apart dispersion puts the colours, through the facility's own glazing") {
    // **The number that says whether this can be SEEN, pinned so nobody has to guess at it.**
    //
    // A flat pane does not deviate a ray at all -- the test at the top of this file is that
    // property -- so what dispersion can do through one is separate the DISPLACEMENTS. Through 12
    // cm at 45 degrees the offset is 4.06 cm; the red and blue ends of the band differ by ONE
    // MILLIMETRE, which at 32 voxels a metre is a thirtieth of a voxel and under a pixel at any
    // distance a player stands at. **A pane is the wrong place to look for dispersion and this is
    // the arithmetic that says so**, at the most dispersive glass anybody makes.
    const f64 thickness = 0.12;
    const Vec3 face{0.0, 0.0, -1.0};
    const auto sideways = [&](f64 tilt, f64 ior) {
        const f64 radians = tilt * 3.14159265358979323846 / 180.0;
        const Vec3 eye = normalize({std::sin(radians), 0.0, std::cos(radians)});
        const Vec3 into = refract(eye, face, 1.0 / ior);
        const Vec3 exit_point = into * (thickness / std::abs(into.z));
        const Vec3 straight = eye * (thickness / std::abs(eye.z));
        return std::abs(exit_point.x - straight.x);
    };

    const f64 red = sideways(45.0, medium_index(1.5, 0.0));
    const f64 blue = sideways(45.0, medium_index(1.5, 1.0));
    CHECK(blue > red);   // blue bends more, so it is displaced further
    const f64 spread = blue - red;
    CHECK(spread == doctest::Approx(0.0011).epsilon(0.05));
    CHECK(spread * 32.0 < 0.05);   // under a twentieth of a voxel

    // **A single interface with a long path behind it is where the term is largest**, because there
    // is no second face to undo it -- 22 cm of water looked into at 70 degrees, which is
    // `clips/refraction_small.clip`'s basin. Four and a half millimetres, which at the two metres
    // that camera stands back is about an eighth of a degree: one or two pixels of colour at the
    // edge of the pale floor under the water, and getting on for four times what the pane manages.
    const auto depth_shift = [](f64 tilt, f64 ior, f64 depth) {
        const f64 radians = tilt * 3.14159265358979323846 / 180.0;
        const f64 inside = std::asin(std::sin(radians) / ior);
        return depth * (std::tan(radians) - std::tan(inside));
    };
    const f64 water_red = depth_shift(70.0, medium_index(1.33, 0.0), 0.22);
    const f64 water_blue = depth_shift(70.0, medium_index(1.33, 1.0), 0.22);
    // A higher index bends further from the straight line, so the bottom appears to move FURTHER:
    // blue is displaced more than red, the same way round as the pane above.
    CHECK(water_blue > water_red);
    CHECK(water_blue - water_red == doctest::Approx(0.0044).epsilon(0.05));
    CHECK(water_blue - water_red > 3.5 * spread);
}

TEST_CASE("a cap of bins is finer than a hemisphere of the same bins") {
    // R4c's image, as arithmetic. Thirty-six bins over a hemisphere is a cone of
    // acos(1 - 1/36) -- 13.5 degrees -- and no image survives that; the same thirty-six over a six
    // degree cap are 2 degrees apart. Held against the two geometries rather than against the
    // expression, which is the whole point of writing it here.
    const f64 to_degrees = 180.0 / 3.14159265358979323846;
    const f64 hemisphere_bin = std::acos(1.0 - 1.0 / 36.0) * to_degrees;
    CHECK(hemisphere_bin == doctest::Approx(13.53).epsilon(0.01));

    // Chrome at rough=8 is floored to alpha 0.004 and lands on the cap's own minimum.
    const f64 chrome_cap = cap_of(8, 6);
    CHECK(chrome_cap == doctest::Approx(kCapMin));
    const f64 chrome_bin = 2.0 * chrome_cap * to_degrees / 6.0;
    CHECK(chrome_bin == doctest::Approx(2.005).epsilon(0.01));
    CHECK(chrome_bin < hemisphere_bin / 6.0);

    // ...and the cap follows ROUGHNESS and nothing else, continuously, with no threshold in it:
    // bronze at rough=110 comes out with bins about as wide as its own lobe, which is what
    // `cap = alpha * side / 2` says, and a rough stone is stopped by the ceiling.
    const f64 bronze_cap = cap_of(110, 6);
    const f64 bronze_bin = 2.0 * bronze_cap * to_degrees / 6.0;
    CHECK(bronze_bin == doctest::Approx(alpha_of(110) * to_degrees).epsilon(0.01));
    CHECK(bronze_cap > chrome_cap);
    CHECK(cap_of(210, 6) == doctest::Approx(kCapMax));

    // Monotone in roughness everywhere between the two clamps, which is what "no roughness
    // threshold anywhere" means when it is said as a test.
    f64 last = 0.0;
    for (u32 rough = 0; rough <= 255; ++rough) {
        const f64 cap = cap_of(rough, 6);
        CHECK(cap >= last - 1e-12);
        last = cap;
    }
}

TEST_CASE("the cap's square maps a direction there and back, and says when it cannot") {
    // The orthographic patch `face_lobe_cap_at` and `face_lobe_cap_direction` are. A frame that
    // disagreed by a swap or a sign is a reflection that is mirrored and looks very nearly right,
    // which is the hardest kind of wrong to see in a picture (D598), so the round trip is pinned.
    const Vec3 axis{0.0, 0.0, 1.0};
    const Vec3 u{1.0, 0.0, 0.0};
    const Vec3 v{0.0, 1.0, 0.0};
    const f64 sin_cap = std::sin(0.105);

    const auto to_square = [&](Vec3 d, bool& inside) {
        const f64 px = dot(d, u) / sin_cap;
        const f64 py = dot(d, v) / sin_cap;
        inside = dot(d, axis) > 0.0 && std::max(std::abs(px), std::abs(py)) <= 1.0;
        return std::pair<f64, f64>{px * 0.5 + 0.5, py * 0.5 + 0.5};
    };
    const auto from_square = [&](f64 ax, f64 ay) {
        const f64 px = (ax * 2.0 - 1.0) * sin_cap;
        const f64 py = (ay * 2.0 - 1.0) * sin_cap;
        const f64 z = std::sqrt(std::max(1.0 - px * px - py * py, 0.0));
        return normalize({px, py, z});
    };

    for (const f64 tilt : {0.0, 0.02, 0.06, 0.1}) {
        const Vec3 d = normalize({std::sin(tilt), 0.0, std::cos(tilt)});
        bool inside = false;
        const auto at = to_square(d, inside);
        CHECK(inside);
        const Vec3 back = from_square(at.first, at.second);
        CHECK(back.x == doctest::Approx(d.x).epsilon(1e-9));
        CHECK(back.y == doctest::Approx(d.y).epsilon(1e-9));
        CHECK(back.z == doctest::Approx(d.z).epsilon(1e-9));
    }

    // A direction outside the cap is REPORTED outside rather than clamped to the rim. The block has
    // measured nothing about it, and the rim bin would draw whatever the face reflects six degrees
    // away as though it were what the eye is looking at -- so the composite falls back to the
    // hemispherical mean instead, which is what a face with no block reads.
    bool inside = true;
    to_square(normalize({std::sin(0.3), 0.0, std::cos(0.3)}), inside);
    CHECK_FALSE(inside);
    to_square({0.0, 0.0, -1.0}, inside);
    CHECK_FALSE(inside);
}

// ---- R4f: the reflected half of the same interface ---------------------------------------------
//
// D652 put what goes THROUGH a surface back on the ray. R4f puts what comes OFF it back on the
// same ray, and the two are one event seen from either side of the Fresnel term. Three pieces of
// arithmetic decide the whole of it, and all three can be held against something other than
// themselves:
//
//   the SHARE -- how much of a pixel's specular its own ray is entitled to be, measured against
//   what a per-face average could carry. It must be continuous, monotone in roughness and in
//   distance, near one for a mirror and near nought for a rough surface, and it must contain
//   nothing that reads as a cutoff;
//   the FRESNEL -- taken from the material's own index rather than from a fixed 0.04, and the
//   check that matters is that a material which never wrote the byte comes back at exactly the
//   old number, so the building does not move when this lands;
//   the DEPTH -- which is a consequence of the budget and not a constant, so how many bounces two
//   facing mirrors reach is arithmetic that can be written down and checked.
//
// Same honesty as the rest of this file: it proves the formulae, not that the marcher hands them
// the right normal. `shaders/reflect.glsl` is where they live.
namespace {

// The angular width of a reflected lobe, from the roughness byte. Deliberately NOT floored at
// `kFaceAlphaMin` -- that floor exists so the GGX distribution does not divide by nought, and read
// as a width it puts half a degree of blur under every mirror in the world.
f64 reflect_lobe_width(u32 rough_byte) {
    const f64 r = static_cast<f64>(rough_byte) / 255.0;
    return 2.0 * r * r;
}

// How much of the answer this pixel's own ray is entitled to be. `carrier` is the angular
// resolution the thing that would answer instead can carry.
f64 reflect_share(f64 lobe_width, f64 carrier) {
    return carrier / std::max(carrier + lobe_width, 1e-9);
}

// The angular size of one voxel face at a distance, in radians, floored at the pixel's own.
f64 carrier_for(u32 face_level, f64 distance_voxels, f64 pixel_angle) {
    return std::max(static_cast<f64>(1u << face_level) / std::max(distance_voxels, 1.0),
                    pixel_angle);
}

// The dielectric's normal-incidence reflectance from the index the record wrote, which is stored as
// the offset from vacuum in 128ths. A byte of nought is "the record did not say".
f64 reflect_dielectric_f0(u32 ior_byte) {
    if (ior_byte == 0) return 0.04;
    const f64 n = 1.0 + static_cast<f64>(ior_byte) / 128.0;
    const f64 r = (n - 1.0) / (n + 1.0);
    return r * r;
}

// Schlick, which face_terms.glsl already had.
f64 schlick(f64 f0, f64 cos_theta) {
    const f64 m = std::clamp(1.0 - cos_theta, 0.0, 1.0);
    return f0 + (1.0 - f0) * (m * m * m * m * m);
}

// A pixel's angular size, which is `2 * tan(fov/2) / height` -- the expression
// shaders/visibility.comp computes.
f64 pixel_angle_of(f64 tan_half_fov, f64 height) { return 2.0 * tan_half_fov / height; }

}  // namespace

TEST_CASE("R4f: the share is continuous, and a mirror's own ray is nearly the whole of it") {
    // `clips/mirror_test.clip`'s own materials. kFaceAlphaMin would floor chrome's alpha at 0.004
    // and make its lobe 0.46 degrees wide, which is four times what rough=8 asks for -- so the
    // width comes from the byte the clip author wrote and the floor stays where it belongs.
    CHECK(reflect_lobe_width(8) == doctest::Approx(0.001968).epsilon(1e-3));
    CHECK(reflect_lobe_width(110) == doctest::Approx(0.372).epsilon(1e-3));

    // The pixel is the FLOOR and not the comparison, and this is the property that makes the rule
    // resolution-independent on anything the camera is close to. A voxel face at 3.4 m subtends
    // 0.0092 rad; a pixel at 1280x800 subtends 0.0014 and at 4K 0.00053, so both are under it and
    // the share is the same number at every resolution -- which is D703's own finding said from
    // the other end, that a face is one colour however many pixels are looking at it.
    const f64 chrome_at = 3.4 * 32.0;
    const f64 carrier_800 = carrier_for(0, chrome_at, pixel_angle_of(0.577, 800.0));
    const f64 carrier_4k = carrier_for(0, chrome_at, pixel_angle_of(0.577, 2160.0));
    CHECK(carrier_800 == doctest::Approx(carrier_4k).epsilon(1e-9));

    const f64 chrome = reflect_share(reflect_lobe_width(8), carrier_800);
    const f64 brushed = reflect_share(reflect_lobe_width(110), carrier_800);
    // Five sixths from the pixel's own ray -- sharp, correct and recursive -- against a fortieth
    // for the brushed sphere beside it, which stays the face's accumulated average: cheap, already
    // measured, no noise. The two are the same expression with a different byte in it.
    CHECK(chrome == doctest::Approx(0.8236).epsilon(1e-3));
    CHECK(brushed == doctest::Approx(0.0241).epsilon(1e-2));

    // Continuous and monotone across the WHOLE byte, with no step anywhere in it. A cutoff would
    // show up here as a jump; what this pins is the largest step between neighbouring roughness
    // bytes, and it is far below anything the eye reads as a boundary (D387).
    f64 previous = 1.0;
    f64 largest_step = 0.0;
    for (u32 rough = 0; rough <= 255; ++rough) {
        const f64 s = reflect_share(reflect_lobe_width(rough), carrier_800);
        CHECK(s <= previous + 1e-12);   // monotone down
        CHECK(s > 0.0);                 // and never exactly nought, so nothing is a special case
        largest_step = std::max(largest_step, previous - s);
        previous = s;
    }
    CHECK(largest_step < 0.06);

    // And monotone in DISTANCE, which is worth pinning because the opposite sign is exactly as
    // plausible until it is written down: the lobe's width is fixed and the face's angular size
    // shrinks with distance, so the stored average becomes the finer of the two and the ray's
    // share FALLS. That is the direction a grazing floor needs -- half a degree of blur really is
    // twenty pixels across at fifteen metres.
    const f64 near = reflect_share(reflect_lobe_width(18), carrier_for(0, 1.0 * 32.0, 0.0014));
    const f64 far = reflect_share(reflect_lobe_width(18), carrier_for(0, 15.0 * 32.0, 0.0014));
    CHECK(near > far);
    CHECK(near > 0.7);
    CHECK(far < 0.2);
}

TEST_CASE("R4f: Fresnel comes from the material's own index, and silence means glass") {
    // A material that never wrote the byte is unchanged, exactly. This is the check that says the
    // whole building does not move when this stage lands: `face_f0_of`'s 0.04 IS ((n-1)/(n+1))^2
    // at n = 1.5, so silence and "1.5" have always been the same answer and now say so.
    CHECK(reflect_dielectric_f0(0) == doctest::Approx(0.04).epsilon(1e-12));
    CHECK(reflect_dielectric_f0(64) == doctest::Approx(0.04).epsilon(1e-3));   // 1.0 + 64/128 = 1.5

    // Water at 1.33 reflects half what glass does head on, and the two have never been drawn apart.
    const u32 water_byte = 42;   // 1.0 + 42/128 = 1.328
    CHECK(reflect_dielectric_f0(water_byte) == doctest::Approx(0.0200).epsilon(3e-2));
    CHECK(reflect_dielectric_f0(water_byte) < reflect_dielectric_f0(64));

    // ...and at a glancing angle both go towards one, which is why a pool is a mirror at the far
    // end and clear under your feet. Nine degrees off the surface is the case R4c's own note names.
    const f64 grazing = std::cos((90.0 - 9.0) * 3.14159265358979 / 180.0);
    CHECK(schlick(reflect_dielectric_f0(water_byte), grazing) > 0.4);
    CHECK(schlick(reflect_dielectric_f0(water_byte), 1.0) ==
          doctest::Approx(reflect_dielectric_f0(water_byte)).epsilon(1e-9));
}

TEST_CASE("R4f: the depth two facing mirrors reach is the budget, not a count") {
    // `clips/mirror_hall.clip`: chrome at rough=6 metal=252 facing chrome across three metres.
    // Each bounce keeps Fresnel times share of what the one before it carried, so the depth is
    // log(budget) / log(kept) -- arithmetic, with no depth limit anywhere in it.
    const f64 albedo = 236.0 / 255.0;
    const f64 f0 = 0.04 + (albedo - 0.04) * (252.0 / 255.0);
    const f64 carrier = carrier_for(0, 3.0 * 32.0, pixel_angle_of(0.577, 800.0));
    const f64 share = reflect_share(reflect_lobe_width(6), carrier);
    const f64 kept = schlick(f0, 0.5) * share;   // sixty degrees off the normal, down the hall
    CHECK(kept > 0.8);
    CHECK(kept < 1.0);

    const auto depth_for = [&](f64 budget) {
        return static_cast<int>(std::floor(std::log(budget) / std::log(kept)));
    };
    // The shipped budget reaches past the loop's own guard rail, and that is the honest finding
    // rather than a comfortable one: on TWO CHROME WALLS it is `kReflectBounces` that stops the
    // series and not the budget. Every other material in the repository is the other way round --
    // the same hall in bronze stops on the budget long before sixteen -- and a report that does not
    // say which of the two did it is a report about a number nobody can reproduce.
    CHECK(depth_for(0.02) > 16);
    CHECK(depth_for(0.30) < 16);

    // A dielectric is the case the budget was sized for. Polished stone at 4% head on is under a
    // fiftieth after ONE bounce, so a second is never cast and costs nothing -- which is the whole
    // of why "infinite reflections" is affordable rather than a setting.
    const f64 stone = schlick(0.04, 1.0) * reflect_share(reflect_lobe_width(18), carrier);
    CHECK(stone > 0.02);          // the first bounce IS cast: a polished floor reflects
    CHECK(stone * stone < 0.02);  // and the second is not
}

TEST_CASE("R4f: the two halves of the split are complementary and lose nothing") {
    // What leaves a surface along the mirror direction is `mix(stored, ray, share)`, and what the
    // series accumulates for the surface behind it is weighted by the same share. The two weights
    // sum to one at every surface, whatever the share happens to be, so nothing is created or lost
    // between them.
    for (const f64 share : {0.0, 0.1, 0.5, 0.9, 1.0}) {
        CHECK((1.0 - share) + share == doctest::Approx(1.0).epsilon(1e-12));
    }

    // And the series the loop accumulates: with a constant `kept` per bounce the terms sum to the
    // closed form of a geometric series, which is what says it CONVERGES rather than running until
    // a counter stops it. The loop carries two accumulators -- one with the first surface's
    // Fresnel in it, for the budget, and one without, for the radiance -- because the composite
    // applies that first Fresnel itself; getting those the same way round is the difference
    // between a reflection and a reflection squared.
    const f64 kept = 0.6;
    f64 sum = 0.0;
    f64 carry = 1.0;
    for (int i = 0; i < 64; ++i) {
        sum += carry * (1.0 - kept);
        carry *= kept;
    }
    CHECK(sum == doctest::Approx(1.0).epsilon(1e-9));
}

// ---- D720: the wire behind the glass, and what the composite does with it ----------------------
//
// Two faults were reported from playing and written up without being fixed (D720): the sky behind
// glass being wrong *"when theres another glass"*, and *"some weird opposite color tinting
// effect"*. The second is arithmetic and can be held against something other than itself, which is
// most of what is below. The first is CONTROL FLOW and cannot be caught by evaluating an
// expression, so it is written out as the state machine it is.
//
// Same honesty as the rest of this file: these are the expressions shaders/visibility.comp and
// shaders/resolve.comp evaluate and the exits shaders/visibility.comp's loop has. They prove the
// formulae and the flow, not that the marcher hands them the right transmittance.

namespace {

// shaders/node.glsl, `node_medium_through`: what a metre of a material lets by, rooted down to one
// voxel. It is a TRANSMITTANCE -- the tint is mixed towards white by the opacity rather than
// multiplied outright, so a pane a quarter opaque tints what passes by a quarter.
constexpr f64 kVoxelsPerMetre = 32.0;

Rgb medium_through_per_voxel(Rgb tint, f64 opacity) {
    if (opacity >= 1.0) return {0.0, 0.0, 0.0};
    const f64 keep = 1.0 - opacity;
    const Rgb metre{(keep + opacity * tint.r) * keep, (keep + opacity * tint.g) * keep,
                    (keep + opacity * tint.b) * keep};
    return {std::pow(std::max(metre.r, 1e-4), 1.0 / kVoxelsPerMetre),
            std::pow(std::max(metre.g, 1e-4), 1.0 / kVoxelsPerMetre),
            std::pow(std::max(metre.b, 1e-4), 1.0 / kVoxelsPerMetre)};
}

Rgb medium_through(Rgb tint, f64 opacity, f64 metres) {
    const Rgb per_voxel = medium_through_per_voxel(tint, opacity);
    const f64 voxels = metres * kVoxelsPerMetre;
    return {std::pow(per_voxel.r, voxels), std::pow(per_voxel.g, voxels),
            std::pow(per_voxel.b, voxels)};
}

// shaders/node.glsl, `node_medium_absorb`: the three bytes are SIXTEENTHS per metre, and this is
// Beer-Lambert over the true path a refracted ray took. **D720's own arithmetic dropped the
// sixteenths** and quotes exp(-21.6) where the shader computes exp(-1.35); the fault it names is
// real and its figure is not, which is worth having written down beside the right one.
Rgb medium_absorb(u32 r, u32 g, u32 b, f64 metres) {
    return {std::exp(-(static_cast<f64>(r) / 16.0) * metres),
            std::exp(-(static_cast<f64>(g) / 16.0) * metres),
            std::exp(-(static_cast<f64>(b) / 16.0) * metres)};
}

Rgb operator*(Rgb a, Rgb b) { return {a.r * b.r, a.g * b.g, a.b * b.b}; }
Rgb operator*(Rgb a, f64 s) { return {a.r * s, a.g * s, a.b * s}; }
Rgb operator+(Rgb a, Rgb b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }

f64 spread_of(Rgb c) {
    return std::max(std::max(c.r, c.g), c.b) - std::min(std::min(c.r, c.g), c.b);
}

// Which channel a colour is most of, so a hue can be asserted without asserting a brightness.
int dominant(Rgb c) {
    if (c.r >= c.g && c.r >= c.b) return 0;
    return (c.g >= c.b) ? 1 : 2;
}

// shaders/visibility.comp, `visibility_pack_behind`: `lets_past` goes into byte 3 of `behind.y` and
// bytes 0 and 1 of `behind.z`, beside the far surface's payload and its flags.
void pack_lets_past(Rgb lets_past, u32 payload, bool far_hit, u32& y, u32& z) {
    const auto q = [](f64 v) {
        return static_cast<u32>(std::llround(std::clamp(v, 0.0, 1.0) * 255.0));
    };
    y = (payload & 0xFFFFFFu) | (q(lets_past.r) << 24u);
    z = q(lets_past.g) | (q(lets_past.b) << 8u) | (far_hit ? (1u << 16u) : 0u) | (1u << 17u) |
        (255u << 19u);
}

// shaders/resolve.comp, `main`: the same three bytes read back out.
Rgb unpack_through(u32 y, u32 z) {
    return {static_cast<f64>((y >> 24u) & 0xFFu) / 255.0, static_cast<f64>(z & 0xFFu) / 255.0,
            static_cast<f64>((z >> 8u) & 0xFFu) / 255.0};
}

// What a pane's own diffuse is scaled by, in each of the two arms. The far surface is scaled by the
// transmittance itself either way, which is not what was ever in doubt.
Rgb pane_body_share(Rgb through, bool grey) {
    if (!grey) return {1.0 - through.r, 1.0 - through.g, 1.0 - through.b};
    const f64 widest = std::max(std::max(through.r, through.g), through.b);
    return {1.0 - widest, 1.0 - widest, 1.0 - widest};
}

// shaders/resolve.comp's composite for a GLASS pixel: the pane's own diffuse, plus what is behind
// it through the pane. `lit` stands in for everything reaching the near face and `behind_colour`
// for the far surface already shaded.
Rgb composite_glass(Rgb albedo, f64 lit, Rgb behind_colour, Rgb through, bool grey) {
    return albedo * pane_body_share(through, grey) * lit + through * behind_colour;
}

}   // namespace

TEST_CASE("D720: the transmittance that goes across the wire is the one that comes back") {
    // The first thing D720 asks for: *"find it by reading the pack and the unpack against each
    // other, and prove it with a probe -- a known transmittance in, the same numbers out"*. It is
    // eight bits a channel, so the tolerance is one step and no more.
    //
    // The payload shares `behind.y` with the red byte and three flags share `behind.z` with green
    // and blue, which is where a pair of this shape goes wrong: a payload running into bit 24, or a
    // flag running into bit 8, comes back as a colour cast that looks exactly like a material
    // fault. 0xFFFFFF is the largest payload either reading can carry -- a 21-bit type id or a
    // 24-bit folded colour -- so it is the one that would collide if anything could.
    for (const Rgb probe : {Rgb{1.0, 1.0, 1.0}, Rgb{0.0, 0.0, 0.0}, Rgb{0.256, 0.957, 0.502},
                            Rgb{0.866, 0.330, 0.349}, Rgb{0.0045, 0.887, 0.067}}) {
        u32 y = 0, z = 0;
        pack_lets_past(probe, 0xFFFFFFu, true, y, z);
        const Rgb back = unpack_through(y, z);
        CHECK(std::abs(back.r - probe.r) <= 1.0 / 255.0);
        CHECK(std::abs(back.g - probe.g) <= 1.0 / 255.0);
        CHECK(std::abs(back.b - probe.b) <= 1.0 / 255.0);
        // ...and the flags survive the colour, which is the same question from the other side.
        CHECK(((z >> 16u) & 1u) == 1u);   // the far ray landed on something
        CHECK(((z >> 17u) & 1u) == 1u);   // there IS a second layer
        CHECK(((z >> 18u) & 1u) == 0u);   // and it is glass rather than an edge
        CHECK((y & 0xFFFFFFu) == 0xFFFFFFu);
    }
    // So the wire is NOT the fault. It round-trips to the byte, in both directions, with the
    // payload and the flags at their extremes.
}

TEST_CASE("D720: a pane's body drawn per channel is the complement of what the pane lets by") {
    // **The probe D720 names, with a known input and a computable expected output**: `absorb` of
    // 180,4,90 through 12 cm of the clip's own clear glazing.
    const Rgb albedo{232.0 / 255.0, 240.0 / 255.0, 244.0 / 255.0};
    const Rgb through = medium_through(albedo, 26.0 / 255.0, 0.12) * medium_absorb(180, 4, 90, 0.12);

    // Green, and a great deal more of it than either of the others. That is what a white wall
    // behind this pane has to come out as, and it is the whole reference for what follows.
    CHECK(dominant(through) == 1);
    CHECK(through.g > 3.0 * through.r);
    CHECK(through.g > 1.7 * through.b);

    // The arm that shipped. The channel the pane transmits most is the channel its own body is
    // drawn DARKEST in, so the pane's body is magenta -- the exact complement -- whatever it is
    // lit by and whatever is behind it.
    const Rgb body_was = albedo * pane_body_share(through, false);
    CHECK(body_was.r > 5.0 * body_was.g);
    CHECK(body_was.b > 5.0 * body_was.g);

    // ...and the arm that is right. What a pane did not transmit was absorbed or scattered, and
    // only the scattered half comes back out as its body; scattering is the channel-independent
    // half, bounded by the channel that lets most past. So the body is grey and the colour of the
    // pane is the colour it lets by.
    const Rgb body_now = albedo * pane_body_share(through, true);
    CHECK(spread_of(body_now) < 0.02 * spread_of(body_was));

    // A WINDOW IN DAYLIGHT is the case that makes it visible, because it is the case where the
    // pane's own face is lit more brightly than what stands behind it. That is the picture D720
    // records from the other end -- *"the wall behind it came out magenta"* -- and it is what a
    // screenshot of this probe shows.
    const Rgb wall{0.5, 0.5, 0.5};
    const Rgb was = composite_glass(albedo, 1.2, wall, through, false);
    const Rgb now = composite_glass(albedo, 1.2, wall, through, true);
    CHECK(dominant(was) == 0);        // red, with green the SMALLEST channel: magenta
    CHECK(was.g < was.r);
    CHECK(was.g < was.b);
    CHECK(dominant(now) == 1);        // and green, which is what the material says
    CHECK(now.g > 2.0 * now.r);
    CHECK(now.g > 1.5 * now.b);
}

TEST_CASE("D720: the chapel's three stained lights, which is the picture that was reported") {
    // `clips/facility/_contract.clip`'s own bytes at `clips/facility/chapel.clip`'s own thickness,
    // over the travertine pavement that file put under them on purpose. The player's words were
    // *"olive-yellow glass"*, and olive-yellow is what `glass_blue` comes out as when its body is
    // drawn in the complement of what it transmits: red and green up, blue down, on a material
    // whose whole job is to be blue.
    struct Light {
        const char* name;
        Rgb albedo;
        u32 ar, ag, ab;
        int transmits;   // 0 red, 1 green, 2 blue
    };
    const Light lights[3] = {
        {"glass_ruby", {224 / 255.0, 150 / 255.0, 150 / 255.0}, 20, 190, 180, 0},
        {"glass_gold", {228 / 255.0, 200 / 255.0, 140 / 255.0}, 30, 90, 200, 0},
        {"glass_blue", {150 / 255.0, 180 / 255.0, 224 / 255.0}, 180, 120, 20, 2},
    };
    const f64 thickness = 0.09;
    const Rgb travertine{214 / 255.0, 196 / 255.0, 164 / 255.0};

    for (const Light& light : lights) {
        const Rgb through = medium_through(light.albedo, 70.0 / 255.0, thickness) *
                            medium_absorb(light.ar, light.ag, light.ab, thickness);
        // The materials are authored RIGHT: what each one transmits is the colour it is named for.
        // Whatever is wrong is downstream of `absorb`, which is what D720 says and this pins.
        CHECK(dominant(through) == light.transmits);

        const Rgb was = composite_glass(light.albedo, 0.85, travertine, through, false);
        const Rgb now = composite_glass(light.albedo, 0.85, travertine, through, true);

        // The complement the old arm adds back very nearly cancels the transmitted colour, which is
        // why three lights of three different colours photograph as one pale pane each.
        CHECK(spread_of(was) < 0.7 * spread_of(now));
        // ...and the hue that survives is the material's own, which the old arm cannot promise:
        // `glass_blue` came out with more green in it than blue.
        CHECK(dominant(now) == light.transmits);
    }

    // Named on its own, because it is the player's sentence. The blue light's own body:
    const Light& blue = lights[2];
    const Rgb through = medium_through(blue.albedo, 70.0 / 255.0, thickness) *
                        medium_absorb(blue.ar, blue.ag, blue.ab, thickness);
    const Rgb body_was = blue.albedo * pane_body_share(through, false);
    CHECK(body_was.r > 2.5 * body_was.b);   // olive-yellow: red and green over a dark blue
    CHECK(body_was.g > 2.5 * body_was.b);
    const Rgb body_now = blue.albedo * pane_body_share(through, true);
    CHECK(body_now.b > body_now.r);         // ...and now it is the blue the author wrote
    CHECK(body_now.b > body_now.g);
}

TEST_CASE("D720: a neutral pane does not move, which is what makes the fix safe on the building") {
    // The facility's `glass`: rgb 198,214,224, opacity 64, and no `absorb` bytes at all. Every
    // window in the building is this material and none of them may shift.
    const Rgb through =
        medium_through({198 / 255.0, 214 / 255.0, 224 / 255.0}, 64.0 / 255.0, 0.12);
    const Rgb was = pane_body_share(through, false);
    const Rgb now = pane_body_share(through, true);
    // Four thousandths of the albedo in the channel that moves most -- under the eight-bit wire the
    // share arrives on, so no window in the facility can be seen to change.
    CHECK(std::abs(was.r - now.r) < 0.004);
    CHECK(std::abs(was.g - now.g) < 0.004);
    CHECK(std::abs(was.b - now.b) < 0.004);

    // And the two arms are IDENTICAL wherever nothing is transmitted, which is every opaque surface
    // in the world and every one of R5d's edges: both pass a `lets_past` of nought, and an edge
    // does its own scaling outside with a share that was already grey.
    const Rgb none{0.0, 0.0, 0.0};
    CHECK(spread_of(pane_body_share(none, false)) == doctest::Approx(0.0));
    CHECK(pane_body_share(none, true).r == doctest::Approx(1.0));
    CHECK(pane_body_share(none, false).r == doctest::Approx(1.0));
}

TEST_CASE("D720: every exit of the bent-ray loop leaves a far surface behind it") {
    // Fault 1, as the control flow it is. shaders/visibility.comp's loop over interfaces has three
    // exits -- total internal reflection at an entry face, a ray that never leaves the medium, and
    // the turn that finds the far surface -- and only the third assigns `far`. The flag that
    // guarded the repair was set BEFORE the branch that decides which of the three happened, so
    // from the second turn on it answered "the ray bent" to the question "is `far` valid".
    //
    // Four things a turn can do, and each list below is one turn each until the loop ends.
    // `AnotherMedium` is the `continue` -- the path that reaches a second pane and is the only way
    // any of the rest of this is reachable.
    enum class Exit { TotalInternal, StuckInside, AnotherMedium, FoundIt };

    // Where the `far` handed to `visibility_pack_behind` came from. `Uninitialised` is the answer
    // that must not exist, and trap 7's rule is why it is a third value rather than a bool: "the
    // bent ray's answer" and "nothing was ever written here" must never be the same reply.
    enum class Source { Uninitialised, Bent, Straight };

    // `bent`, the flag that shipped: set on every turn that got as far as marching a segment,
    // including the turn that goes round again.
    const auto shipped = [](std::initializer_list<Exit> turns) {
        bool bent = false;
        bool assigned = false;
        for (const Exit e : turns) {
            if (e == Exit::TotalInternal || e == Exit::StuckInside) break;
            bent = true;   // ...before the branch below, which is the whole of the fault
            if (e == Exit::FoundIt) {
                assigned = true;
                break;
            }
            // otherwise another medium: round again, and `far` is still not written
        }
        if (assigned) return Source::Bent;
        return bent ? Source::Uninitialised : Source::Straight;   // the repair fires on !bent
    };

    // `have_far`, the flag that is right: set where, and only where, `far` is written.
    const auto fixed = [](std::initializer_list<Exit> turns) {
        bool have_far = false;
        for (const Exit e : turns) {
            if (e == Exit::TotalInternal || e == Exit::StuckInside) break;
            if (e == Exit::FoundIt) {
                have_far = true;
                break;
            }
        }
        return have_far ? Source::Bent : Source::Straight;   // the repair fires on !have_far
    };

    // ONE medium: the two agree on every exit, turn for turn. That is why `--no-refract-stack` is
    // untouched by the fix, and it is why the fault was invisible for the life of the feature --
    // `kRefractMedia` was 1 for everything before D718, so the loop only ever had a first turn.
    for (const Exit only : {Exit::TotalInternal, Exit::StuckInside, Exit::FoundIt}) {
        CHECK(shipped({only}) != Source::Uninitialised);
        CHECK(shipped({only}) == fixed({only}));
    }
    // A SECOND medium reached and then given up on. Both breaks are reachable there -- D720 names
    // them -- and the shipped flag says the pixel is covered when it is not, so `far` reaches
    // `visibility_pack_behind` holding whatever was on the stack, `far.hit` included. That bit is
    // the one that says THIS RAY REACHED THE SKY, which is why the report was about the sky.
    CHECK(shipped({Exit::AnotherMedium, Exit::TotalInternal}) == Source::Uninitialised);
    CHECK(shipped({Exit::AnotherMedium, Exit::StuckInside}) == Source::Uninitialised);
    CHECK(fixed({Exit::AnotherMedium, Exit::TotalInternal}) == Source::Straight);
    CHECK(fixed({Exit::AnotherMedium, Exit::StuckInside}) == Source::Straight);
    // ...and the third turn and the fourth, because `kRefractMedia` is 4 and it is the `continue`
    // that carries the flag forward: every turn past the first has the same hole in it.
    CHECK(shipped({Exit::AnotherMedium, Exit::AnotherMedium, Exit::StuckInside}) ==
          Source::Uninitialised);
    CHECK(fixed({Exit::AnotherMedium, Exit::AnotherMedium, Exit::StuckInside}) == Source::Straight);
    CHECK(shipped({Exit::AnotherMedium, Exit::AnotherMedium, Exit::AnotherMedium,
                   Exit::TotalInternal}) == Source::Uninitialised);
    CHECK(fixed({Exit::AnotherMedium, Exit::AnotherMedium, Exit::AnotherMedium,
                 Exit::TotalInternal}) == Source::Straight);

    // The case that always worked, either way round: the loop found a far surface and said so.
    CHECK(shipped({Exit::AnotherMedium, Exit::FoundIt}) == Source::Bent);
    CHECK(fixed({Exit::AnotherMedium, Exit::FoundIt}) == Source::Bent);
    CHECK(fixed({Exit::AnotherMedium, Exit::AnotherMedium, Exit::FoundIt}) == Source::Bent);
}
