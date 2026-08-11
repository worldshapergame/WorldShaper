#include "ui/ink.hpp"

#include <algorithm>
#include <cmath>

namespace ws::ui {
namespace {

// smoothstep, spelled out rather than pulled in, because the shader's is the definition this has
// to agree with and GLSL's is this one.
f32 smoothstep(f32 edge0, f32 edge1, f32 x) {
    const f32 t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

Colour to_brightness(const Colour& colour, f32 want) {
    const f32 have = std::max(luma(colour), 1e-4f);
    const f32 gain = want / have;
    return Colour{std::clamp(colour.r * gain, 0.0f, 1.0f), std::clamp(colour.g * gain, 0.0f, 1.0f),
                  std::clamp(colour.b * gain, 0.0f, 1.0f)};
}

}  // namespace

f32 luma(const Colour& encoded) {
    const f32 linear = encoded.r * encoded.r * 0.2126f + encoded.g * encoded.g * 0.7152f +
                       encoded.b * encoded.b * 0.0722f;
    return std::sqrt(std::max(linear, 0.0f));
}

f32 ink_target(f32 here) {
    // A backdrop of brightness `here` inverts to a brightness of `1 - here` in encoded units for a
    // grey, so the swing is what the inversion buys and the push is what the floor tops it up by.
    const f32 swing = (1.0f - here) - here;
    const f32 shortfall = std::max(0.0f, kInkFloor - std::abs(swing));
    const f32 push = shortfall * std::clamp(swing / kInkFloor, -1.0f, 1.0f);
    return std::clamp(here + swing + push, 0.0f, 1.0f);
}

Colour ink(const Colour& backdrop, const Colour& accent) {
    const f32 here = luma(backdrop);
    const Colour opposite{1.0f - backdrop.r, 1.0f - backdrop.g, 1.0f - backdrop.b};
    const f32 swing = luma(opposite) - here;

    // No `if`. A conditional on a continuous quantity is a cliff: as the backdrop drifts past the
    // threshold, every pixel of a label jumps from "the exact opposite of what is behind it" to
    // "flat light", one after another as the gradient crosses. And no direction decision: `push`
    // is odd in `swing`, so there is no "which way has more room" to get wrong per shape.
    const f32 shortfall = std::max(0.0f, kInkFloor - std::abs(swing));
    const f32 push = shortfall * std::clamp(swing / kInkFloor, -1.0f, 1.0f);
    const f32 want = std::clamp(here + swing + push, 0.0f, 1.0f);

    // Both candidates taken to the same brightness FIRST, then mixed. Blending the finished
    // colours instead lands, halfway across, on the average of "too little contrast" and "enough",
    // which measures a fifth short of legible.
    const Colour inverted = to_brightness(opposite, want);
    const Colour tinted = to_brightness(accent, want);

    const f32 blind = 1.0f - smoothstep(0.0f, kBlindBand, std::abs(here - 0.5f));
    return Colour{std::clamp(inverted.r + (tinted.r - inverted.r) * blind, 0.0f, 1.0f),
                  std::clamp(inverted.g + (tinted.g - inverted.g) * blind, 0.0f, 1.0f),
                  std::clamp(inverted.b + (tinted.b - inverted.b) * blind, 0.0f, 1.0f)};
}

}  // namespace ws::ui
