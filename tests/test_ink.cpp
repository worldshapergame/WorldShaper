// The ink rule, and the four properties documentation/14-ui-style.md claims for it.
//
// Every one of those claims is a claim about a function of one variable, and every one of them was
// a bug once. A shader cannot be swept; src/ui/ink.cpp is the same arithmetic in C++ and can be, so
// this is where the claims are actually held.

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "doctest/doctest.h"
#include "ui/ink.hpp"

using namespace ws;
using namespace ws::ui;

namespace {

Colour grey(f32 value) { return Colour{value, value, value}; }
const Colour kGreenAccent{0.0f, 1.0f, 0.0f};

}  // namespace

TEST_CASE("the floor is met wherever an inversion can meet it") {
    // A plain inversion of a grey at `h` lands at `1 - h`, so it is already 0.3 apart whenever
    // `h` is outside 0.35 to 0.65. Those are the backdrops the floor is a floor for, and there it
    // is held exactly.
    for (i32 step = 0; step <= 1000; ++step) {
        const f32 here = static_cast<f32>(step) / 1000.0f;
        if (std::abs(1.0f - here - here) < kInkFloor) continue;
        const Colour mark = ink(grey(here), kGreenAccent);
        const f32 apart = std::abs(luma(mark) - here);
        INFO("backdrop " << here << " gave ink at " << luma(mark));
        CHECK(apart >= kInkFloor - 0.01f);
    }
}

TEST_CASE("closer in, the push tops the inversion up and never cuts it down") {
    // Between those and mid grey the inversion falls short and cannot be topped all the way up —
    // see the impossibility below. What the push does there is add to it, smoothly, by more the
    // shorter it was: at a swing of 0.2 the separation comes out at 0.267 rather than 0.2, and at
    // 0.1 it comes out at 0.167 rather than 0.1.
    //
    // This is the property to hold, and it is not "the floor is met everywhere outside the blind
    // band": the style document's *brightness alone carries the ink* there is a statement about
    // which quantity is doing the work, not a promise of 0.3.
    f32 previous = 1.0f;
    for (i32 step = 0; step <= 500; ++step) {
        const f32 here = static_cast<f32>(step) / 1000.0f;   // 0 up to 0.5
        const f32 plain = std::abs((1.0f - here) - here);
        const f32 pushed = std::abs(ink_target(here) - here);
        INFO("backdrop " << here << ": inversion " << plain << ", pushed to " << pushed);
        CHECK(pushed >= plain - 1e-4f);
        // And it falls away monotonically towards mid grey, which is what makes it continuous
        // rather than a threshold that snaps.
        CHECK(pushed <= previous + 1e-4f);
        previous = pushed;
    }
    CHECK(std::abs(ink_target(0.4f) - 0.4f) == doctest::Approx(0.2667f).epsilon(0.01));
    CHECK(std::abs(ink_target(0.45f) - 0.45f) == doctest::Approx(0.1667f).epsilon(0.01));
}

TEST_CASE("a hard floor everywhere is impossible, and the GLASS is what gives") {
    // The proof in the style document, checked rather than asserted: if ink brightness were
    // continuous and always 0.3 from the backdrop, it could not cross zero, which demands 1.3 at a
    // white backdrop and -0.3 at a black one. So SOMETHING has to give in the middle.
    const f32 middle = luma(ink(grey(0.5f), kGreenAccent));
    CHECK(std::abs(middle - 0.5f) < kInkFloor);

    // What gives is the BACKDROP, not the letters. It used to be the letters — the ink crossfaded
    // to the player's accent in that band — and it was reported from playing: *make it so that text
    // doesnt get the accent color*. A word that changes colour as the world drifts behind it reads
    // as a different kind of word. So the ink is now the inverse and nothing else, at every
    // backdrop, and it carries no colour of its own anywhere.
    const Colour mark = ink(grey(0.5f), kGreenAccent);
    CHECK(mark.r == doctest::Approx(mark.g));
    CHECK(mark.g == doctest::Approx(mark.b));

    // And a panel over mid grey is pushed out of the band before anything is written on it, which
    // is what buys the contrast back. Down only, so there is no direction to get wrong.
    const Colour behind = glass(grey(0.5f));
    CHECK(luma(behind) < 0.5f - 0.1f);
    CHECK(std::abs(luma(ink(behind, kGreenAccent)) - luma(behind)) > kInkFloor - 0.02f);

    // Away from the band it does nothing at all: a bright sky behind a panel is still a bright sky.
    CHECK(luma(glass(grey(0.05f))) == doctest::Approx(0.05f).epsilon(0.001));
    CHECK(luma(glass(grey(0.95f))) == doctest::Approx(0.95f).epsilon(0.001));
}

TEST_CASE("no `if`: the ink is continuous as the backdrop drifts") {
    // A conditional on a continuous quantity is a cliff, and the cliff is what a player sees as a
    // label flipping from black to white one pixel at a time as a window drifts over a gradient.
    // Swept in steps of 0.002, which is what the style document swept.
    f32 worst = 0.0f;
    f64 total = 0.0;
    u32 count = 0;
    f32 previous = luma(ink(grey(0.0f), kGreenAccent));
    for (i32 step = 1; step <= 500; ++step) {
        const f32 here = static_cast<f32>(step) * 0.002f;
        const f32 now = luma(ink(grey(here), kGreenAccent));
        const f32 jump = std::abs(now - previous);
        worst = std::max(worst, jump);
        total += jump;
        ++count;
        previous = now;
    }
    const f64 average = total / count;
    INFO("worst step " << worst << " against an average of " << average);
    // The document reports 3.4x the average for this form and 0.83 for the one with the `if`
    // (which is smaller only because that version is flat on one side of its threshold and then
    // jumps once, enormously). What is being tested here is that there is no jump at all: an
    // actual cliff in this sweep is tens of times the average.
    CHECK(worst < average * 12.0);
}

TEST_CASE("no direction decision: the push is odd in the swing") {
    // `push` being odd is what removes "which way has more room" from the formula. Without it a
    // window drifting over the world flips its whole ink in one frame, and a shape can be part
    // accent because the direction was per shape while the swing was per pixel.
    for (i32 step = 0; step <= 50; ++step) {
        const f32 here = static_cast<f32>(step) / 100.0f;   // 0 to 0.5
        const f32 low = ink_target(here) - here;
        const f32 high = ink_target(1.0f - here) - (1.0f - here);
        INFO("backdrops " << here << " and " << (1.0f - here));
        CHECK(std::abs(low + high) < 1e-4f);
    }
}

TEST_CASE("brightness, not per channel") {
    // Per channel puts three changeovers at three different pixels, because a backdrop's red
    // crosses the middle before its blue does — which is how an interface with no colour in it
    // grows a red-and-yellow seam down the middle. The test: a saturated backdrop whose channels
    // straddle the middle must still produce ONE decision, so the ink is a scaled inverse rather
    // than a per-channel one.
    const Colour backdrop{0.9f, 0.5f, 0.1f};
    const Colour mark = ink(backdrop, kGreenAccent);
    const Colour opposite{1.0f - backdrop.r, 1.0f - backdrop.g, 1.0f - backdrop.b};
    // Same direction as the plain inverse, only rescaled: the ratios between channels survive.
    const f32 ratio_rg = mark.r / std::max(mark.g, 1e-4f);
    const f32 want_rg = opposite.r / std::max(opposite.g, 1e-4f);
    CHECK(std::abs(ratio_rg - want_rg) < 0.02f);
}

TEST_CASE("luma is the linear luma expressed in encoded units") {
    // A plain weighted average of encoded channels agrees for greys and is wrong by a fifth of the
    // promised contrast for a saturated colour, which is the reason this is a quadratic mean.
    CHECK(luma(grey(0.5f)) == doctest::Approx(0.5f).epsilon(0.001));
    const Colour blue{0.0f, 0.0f, 1.0f};
    const f32 plain_average = 0.0722f;   // what a weighted average of the encoded channels gives
    CHECK(luma(blue) > plain_average * 2.0f);
}

TEST_CASE("the shader's ink rule is this ink rule") {
    // Two copies of one formula, exactly as with the typeface. The constants are what drift, so
    // they are read back out of the shader and checked — a floor that is 0.30 here and 0.35 there
    // is a difference nobody sees until a label goes grey on somebody else's machine.
    const std::string path = std::string(WS_SHADER_SOURCE_DIR) + "/ui.glsl";
    std::ifstream file(path, std::ios::binary);
    REQUIRE_MESSAGE(file.good(), "could not open " << path);
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();

    const usize floor_at = text.find("kInkFloor = ");
    REQUIRE(floor_at != std::string::npos);
    const f32 shader_floor = std::stof(text.substr(floor_at + 12, 8));
    CHECK(shader_floor == doctest::Approx(kInkFloor));

    // And the blind band's half width, which is the second argument of the smoothstep.
    const usize band_at = text.find("smoothstep(0.0, ");
    REQUIRE(band_at != std::string::npos);
    const f32 shader_band = std::stof(text.substr(band_at + 16, 8));
    CHECK(shader_band == doctest::Approx(kBlindBand));
}
