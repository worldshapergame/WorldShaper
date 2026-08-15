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

#include <cmath>
#include <initializer_list>

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
