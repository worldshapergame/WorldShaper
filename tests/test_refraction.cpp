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
