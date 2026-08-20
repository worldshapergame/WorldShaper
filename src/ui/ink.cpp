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

Colour tint_of(const Colour& accent, u32 which) {
    const f32 r = std::clamp(accent.r, 0.0f, 1.0f);
    const f32 g = std::clamp(accent.g, 0.0f, 1.0f);
    const f32 b = std::clamp(accent.b, 0.0f, 1.0f);
    const f32 high = std::max(r, std::max(g, b));
    const f32 low = std::min(r, std::min(g, b));
    const f32 chroma = high - low;

    f32 hue = 0.0f;
    if (chroma > 1.0e-5f) {
        if (high == r) {
            hue = std::fmod((g - b) / chroma + 6.0f, 6.0f);
        } else if (high == g) {
            hue = (b - r) / chroma + 2.0f;
        } else {
            hue = (r - g) / chroma + 4.0f;
        }
        hue /= 6.0f;
    }
    // Saturation is kept and only the hue turns, which is what makes "an interface whose ink is
    // grey has grey wires" true rather than promised: a grey accent has no chroma, so all three
    // rotations of it are the same grey.
    const f32 saturation = (high > 1.0e-5f) ? (chroma / high) : 0.0f;
    // Stated rather than inverted, so it needs a brightness of its own. A floor rather than the
    // accent's own value: a dark wire over dark glass is a wire that is not there.
    const f32 value = std::max(high, 0.85f);

    hue = std::fmod(hue + static_cast<f32>(which % kTints) / static_cast<f32>(kTints), 1.0f);
    const f32 sector = hue * 6.0f;
    const i32 face = static_cast<i32>(std::floor(sector)) % 6;
    const f32 f = sector - std::floor(sector);
    const f32 p = value * (1.0f - saturation);
    const f32 q = value * (1.0f - saturation * f);
    const f32 t = value * (1.0f - saturation * (1.0f - f));
    switch (face) {
        case 0: return Colour{value, t, p};
        case 1: return Colour{q, value, p};
        case 2: return Colour{p, value, t};
        case 3: return Colour{p, q, value};
        case 4: return Colour{t, p, value};
        default: return Colour{value, p, q};
    }
}

u32 tint_rgb(const Colour& accent, u32 which) {
    const Colour c = tint_of(accent, which);
    const auto byte = [](f32 v) {
        return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return (byte(c.r) << 16) | (byte(c.g) << 8) | byte(c.b);
}

}  // namespace ws::ui
