#pragma once
// The ink rule, on the host.
//
// documentation/14-ui-style.md is the specification and shaders/ui.glsl is what actually colours a
// pixel. This is the same arithmetic in C++, and it exists for two reasons that are not "drawing":
//
//   **It is what can be tested.** Every property the style document claims for this formula is a
//   claim about a function of one variable — that it is continuous as the backdrop drifts, that it
//   never picks a direction per shape, that it decides from brightness and not per channel, that
//   it holds a floor everywhere except the band where a floor is provably impossible. A shader
//   cannot be swept in a unit test; this can, and tests/test_ink.cpp sweeps it.
//
//   **It is what the shell asks when it needs a colour on the CPU** — the window title in the task
//   bar, a screenshot comparison, anything that has to know what a mark will come out as without
//   asking the card.
//
// The two copies are held together the way the two copies of the typeface are: the test reads
// `kInkFloor` and the blind band's width back out of shaders/ui.glsl and fails if they have drifted
// apart. A constant that is 0.30 in one file and 0.35 in the other is a difference nobody sees
// until a label goes grey on one machine.

#include "core/types.hpp"

namespace ws::ui {

// A colour in encoded (perceptual) units, which is what both the render target and this rule work
// in throughout. Never linear: the whole formula is about what a reader can tell apart, and that
// is a perceptual quantity.
struct Colour {
    f32 r = 0.0f;
    f32 g = 0.0f;
    f32 b = 0.0f;
};

// How far an ink must sit from its backdrop in encoded units before it stops being legible in a
// chair rather than in a screenshot. Chosen by measurement: a panel over a grey sky put its labels
// nineteen levels from their background without it, and about seventy-five with it.
inline constexpr f32 kInkFloor = 0.30f;

// The half-width of the band where inversion has nothing to say, either side of mid grey. Outside
// roughly 0.42 to 0.58 luma the brightness carries the ink on its own; inside it the contrast is
// spent on hue instead. A sixth of the range, centred exactly where inversion is blind.
inline constexpr f32 kBlindBand = 0.08f;

// The weighted quadratic mean of the encoded channels, which is the linear luma expressed in
// encoded units. A plain weighted average of encoded channels agrees for greys and is wrong by a
// fifth of the promised contrast for a saturated colour.
f32 luma(const Colour& encoded);

// The colour a mark has to be to read against what is behind it.
//
// `accent` is the player's own colour, and it is used ONLY inside the blind band. Everywhere else
// this returns the inverse of the backdrop pushed to the floor, which has no palette in it at all.
Colour ink(const Colour& backdrop, const Colour& accent);

// The brightness the ink is aiming at, on its own, so a test can sweep the thing the formula is
// actually about without a colour getting in the way.
f32 ink_target(f32 here);

}  // namespace ws::ui
