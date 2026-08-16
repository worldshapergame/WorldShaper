// R5b's confidence ramp on the sun, in numbers, and the one constant that ties it to the rest.
//
// # Why this is worth a test when the arithmetic is four lines
//
// The four lines are not where this can go wrong. What can go wrong is the FIGURE: `kFaceSunBelieve`
// is four because `kFaceEager` is four, and the two live in different files — the shader's constant
// cannot be spelled as `kFaceEager` because node.glsl includes face_terms.glsl and not the other way
// about, and `src/world/face_store.hpp` carries a third copy for the audit. Three copies of one
// number, each with a comment naming the others, is exactly the arrangement that rots in silence:
// somebody widens the ramp to sixteen for a smoother transient, every picture still looks plausible,
// and three things that were true stop being true at once —
//
//   - a face is only exempt from the shading stride below `kFaceEager`, so past four the stand-in
//     this blends towards is refreshed one frame in `face_stride` instead of every frame, and the
//     ramp advances in visible steps rather than smoothly;
//   - an edit reseeds a face to `kFaceEditSeed` = 8 samples at the ratio it held (D373), so any
//     figure above eight blends every face inside an edit box towards a stand-in the same edit has
//     just reseeded;
//   - **a settled shadow edge stops being untouched.** The sun is deliberately not filtered by
//     R5a's a-trous pass because a shadow edge is a real high-frequency feature, and a ramp that
//     reached past the eager phase would become that filter by another route.
//
// So the test reads the shader's own constant out of the file and holds it against the store's, and
// then pins what the ramp does at each sample count. As with `test_translucency.cpp` and
// `test_refraction.cpp`, this pins the ARITHMETIC and not the picture: whether the composite reads
// the right word for the right face needs a graphics card and an eye.

#include <doctest/doctest.h>

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

#include "core/types.hpp"
#include "world/face_store.hpp"

using namespace ws;

namespace {

// shaders/face_terms.glsl, `kFaceSunBelieve`. Mirrored here rather than read from the file for the
// arithmetic below, and the very first test is what keeps the mirror honest.
constexpr u32 kFaceSunBelieve = 4;

// `face_sun_confidence`: how far a face of `samples` shadow rays is its own answer.
f64 confidence(u32 samples) {
    const f64 raw = static_cast<f64>(samples) / static_cast<f64>(kFaceSunBelieve);
    return raw < 0.0 ? 0.0 : (raw > 1.0 ? 1.0 : raw);
}

// `face_sun_believed`, with the stand-in known. `mix(stand_in, own, confidence)`.
f64 believed(f64 own, u32 samples, f64 stand_in) {
    const f64 a = confidence(samples);
    return stand_in * (1.0 - a) + own * a;
}

// `face_sun_pack` and `face_sun_unpack`: sixteen bits and a known bit at 31.
constexpr u32 kFaceSunKnown = 1u << 31;
u32 pack(f64 visible) {
    const f64 clamped = visible < 0.0 ? 0.0 : (visible > 1.0 ? 1.0 : visible);
    return static_cast<u32>(clamped * 65535.0 + 0.5) | kFaceSunKnown;
}
f64 unpack(u32 word) { return static_cast<f64>(word & 0xFFFFu) / 65535.0; }

std::string shader_text(const char* name) {
    const std::string path = std::string(WS_SHADER_SOURCE_DIR) + "/" + name;
    std::ifstream file(path, std::ios::binary);
    REQUIRE_MESSAGE(file.good(), "could not open " << path);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// The value of `const uint NAME = <digits>u;` in a shader, or 0 if it is not there in that form.
u32 shader_uint(const std::string& text, const char* name) {
    const std::string want = std::string("const uint ") + name + " = ";
    const usize at = text.find(want);
    if (at == std::string::npos) return 0;
    return static_cast<u32>(std::strtoul(text.c_str() + at + want.size(), nullptr, 10));
}

}   // namespace

TEST_CASE("the ramp ends where a face stops being eager, in all three files that say so") {
    // The shader's own text, so that changing the constant and not this test fails here rather than
    // in a picture nobody is looking at.
    const u32 in_shader = shader_uint(shader_text("face_terms.glsl"), "kFaceSunBelieve");
    CHECK(in_shader == kFaceSunBelieve);
    CHECK(in_shader == kFaceEager);
    CHECK(shader_uint(shader_text("node.glsl"), "kFaceEager") == kFaceEager);

    // ...and the floor the READ path takes an ancestor at, which is the same four and is not the
    // same constant as the one-time claim seed's. It was `kSunSeedMin` = 16 for one landing and the
    // audit measured 64.4% of the population being refused a stand-in because of it (D662), so what
    // this pins is that the two have not been re-merged by somebody tidying up two names for one
    // number. They are two numbers for two questions and only one of them is four.
    const std::string node = shader_text("node.glsl");
    CHECK(shader_uint(node, "kSunStandInMin") == kFaceSunBelieve);
    CHECK(shader_uint(node, "kSunSeedMin") > kFaceSunBelieve);

    // ...and the edit reseed, which is the figure the ramp must stay under. See D373: an edit keeps
    // a face's answer and drops its confidence to eight samples, so a ramp that reached past eight
    // would pull every face in an edited room towards a stand-in the same edit has just reseeded.
    CHECK(kFaceSunBelieve <= 8u);

    // And the ramp must start above the count at which the composite may read a face at all, or
    // there would be sample counts that are drawn and not ramped.
    CHECK(kFaceSettled < kFaceSunBelieve);
}

TEST_CASE("one shadow ray is a coin toss and is believed as a quarter of one") {
    // The case this whole change is about. Two neighbouring faces have each cast exactly one ray at
    // the sun in a room whose true answer is nearly nought; one of them happened to reach it. Raw,
    // they disagree by the whole range, which is what a player sees as speckle.
    const f64 stand_in = 0.05;   // what the coarse face over both of them has measured
    const f64 lit = believed(1.0, 1, stand_in);
    const f64 dark = believed(0.0, 1, stand_in);

    CHECK(lit - dark == doctest::Approx(0.25));
    // ...and each of them sits a quarter of the way from the stand-in towards its own coin.
    CHECK(lit == doctest::Approx(0.05 * 0.75 + 1.0 * 0.25));
    CHECK(dark == doctest::Approx(0.05 * 0.75));

    // The step between them is a quarter of what it was, so the VARIANCE it carries is a sixteenth.
    // That is the whole claim, and it is arithmetic rather than a measurement: nothing here says how
    // much of the picture is made of such faces, which is what the card-free shot is for.
    CHECK((lit - dark) * (lit - dark) == doctest::Approx(1.0 / 16.0));
}

TEST_CASE("the ramp is a straight line from one sample to four and stops there") {
    CHECK(confidence(0) == doctest::Approx(0.0));
    CHECK(confidence(1) == doctest::Approx(0.25));
    CHECK(confidence(2) == doctest::Approx(0.5));
    CHECK(confidence(3) == doctest::Approx(0.75));
    CHECK(confidence(4) == doctest::Approx(1.0));
    // Above the threshold it stays at one rather than running past it, which a clamp is for and a
    // division alone is not.
    CHECK(confidence(5) == doctest::Approx(1.0));
    CHECK(confidence(256) == doctest::Approx(1.0));

    // It rises smoothly, which is D387's standing rule: a hard per-face decision is a new
    // discontinuity per face, and neighbours either side of a threshold then disagree by more than
    // the noise ever did. So no two adjacent counts may differ by more than one step of the ramp.
    for (u32 n = 1; n <= 8; ++n) {
        CHECK(confidence(n) - confidence(n - 1) <= doctest::Approx(1.0 / kFaceSunBelieve));
    }
}

TEST_CASE("a settled face is untouched, and that is what keeps a shadow edge sharp") {
    // Every face at or past the threshold reads exactly what it measured, whatever the stand-in
    // says. This is the property that keeps the ramp from becoming the cross-face filter R5a
    // deliberately refuses the sun -- a penumbral face reaches four samples in four frames, because
    // a face under kFaceEager casts every frame, and from there this is the identity.
    for (const f64 own : {0.0, 0.05, 0.5, 0.9375, 1.0}) {
        for (const u32 samples : {4u, 5u, 16u, 64u, 256u}) {
            CHECK(believed(own, samples, 0.5) == own);       // exactly, not approximately
            CHECK(believed(own, samples, 0.0) == own);
            CHECK(believed(own, samples, 1.0) == own);
        }
    }

    // A face at half a penumbra and a face fully lit beside it keep the whole step between them.
    CHECK(believed(1.0, 32, 0.2) - believed(0.5, 32, 0.2) == doctest::Approx(0.5));
}

TEST_CASE("an edit keeps its answer rather than being pulled back towards the stand-in") {
    // `kFaceEditSeed` is eight: an announced edit drops a face to eight samples at the ratio it
    // held, so that the composite goes on reading it and no pixel falls back to full sun (D373).
    // Eight is past the ramp, so the answer the reseed preserved is the answer that is drawn.
    const u32 after_an_edit = 8;
    CHECK(after_an_edit >= kFaceSunBelieve);
    CHECK(believed(0.0, after_an_edit, 0.9) == 0.0);
    CHECK(believed(1.0, after_an_edit, 0.1) == 1.0);
}

TEST_CASE("the stored stand-in is finer than the estimator it stands for") {
    // Sixteen bits of a fraction whose own estimator holds at most kFaceWindow = 256 samples, so
    // one step of the storage is far under one step of the thing being stored. If this ever stops
    // being true the quantisation becomes visible as banding on exactly the faces this is trying to
    // smooth.
    const f64 finest_the_counters_can_say = 1.0 / static_cast<f64>(kFaceWindow);
    for (u32 lit = 0; lit <= kFaceWindow; ++lit) {
        const f64 exact = static_cast<f64>(lit) / static_cast<f64>(kFaceWindow);
        const u32 word = pack(exact);
        CHECK((word & kFaceSunKnown) != 0u);
        CHECK(std::fabs(unpack(word) - exact) < finest_the_counters_can_say / 16.0);
    }
}

TEST_CASE("nought is a legal stand-in and is not the same word as no stand-in at all") {
    // Trap 7 in sixteen bits. A sealed room's true answer is nought, so the packed value of a fully
    // shadowed ancestor is nought in its low bits -- and a slot whose light words have just been
    // zeroed for a new face is nought in ALL of them. Only the known bit tells the two apart, and
    // without it every newly claimed face would blend towards black.
    const u32 fully_shadowed = pack(0.0);
    const u32 never_written = 0u;
    CHECK(fully_shadowed != never_written);
    CHECK((fully_shadowed & kFaceSunKnown) != 0u);
    CHECK((never_written & kFaceSunKnown) == 0u);
    CHECK(unpack(fully_shadowed) == doctest::Approx(0.0));
}
