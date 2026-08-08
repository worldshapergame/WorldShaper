// The interface, and the rule that it introduces no colour of its own.
//
// documentation/14-ui-style.md is the specification; this is its implementation, and the first
// piece of it. Everything here is written to be used by whatever comes after, which is why the
// ink rule is a function taking a backdrop rather than something the loading screen keeps to
// itself.
//
// # The ink rule
//
// A panel's background is the world behind it, blurred. Its text is the per-pixel INVERSE of what
// is behind each letter. Nothing has a colour of its own, because an interface with its own
// palette fights whatever the player is looking at - a sunset, a cave, snow - and the usual fix
// is to darken the game behind the panel until the palette wins, which trades away the thing the
// player came for.
//
// Three properties of the formula below matter and each was a bug once:
//
//   No `if`. A conditional on a continuous quantity is a cliff: as the backdrop drifts past the
//   threshold, every pixel of a label jumps from "the exact opposite of what is behind it" to
//   "flat light", one after another as the gradient crosses.
//
//   No direction decision. `push` is odd in `swing`, so there is no "which way has more room" to
//   decide. A per-shape direction made a window drifting over the world flip its whole ink in a
//   single frame.
//
//   Brightness, not per channel. Per channel puts three changeovers at three different pixels,
//   because a backdrop's red crosses the middle before its blue does - which is how an interface
//   with no colour in it grows a red-and-yellow seam down the middle.
//
// Over a mid-grey backdrop an inversion has nothing to say, and a hard floor everywhere is
// provably impossible. So in that one band the contrast is spent on HUE instead: the ink
// crossfades to the player's accent. Crossfade the hue, never the contrast - blending the
// finished colours lands, halfway across, on the average of "too little" and "enough".

const float kInkFloor = 0.30;

// Weighted quadratic mean of the encoded channels, which is the linear luma expressed in encoded
// units. A plain weighted average of encoded channels agrees for greys and is wrong by a fifth of
// the promised contrast for a saturated colour.
float ui_luma(vec3 encoded) {
    vec3 linear = encoded * encoded;
    return sqrt(dot(linear, vec3(0.2126, 0.7152, 0.0722)));
}

// The colour a mark has to be to read against what is behind it.
vec3 ui_ink(vec3 backdrop, vec3 accent) {
    float here = ui_luma(backdrop);
    vec3 opposite = vec3(1.0) - backdrop;
    float swing = ui_luma(opposite) - here;

    float shortfall = max(0.0, kInkFloor - abs(swing));
    float push = shortfall * clamp(swing / kInkFloor, -1.0, 1.0);
    float want = clamp(here + swing + push, 0.0, 1.0);

    // Both candidates taken to the same brightness FIRST, then mixed.
    vec3 inverted = opposite * (want / max(ui_luma(opposite), 1e-4));
    vec3 tinted = accent * (want / max(ui_luma(accent), 1e-4));

    // Brightness carries the ink everywhere outside roughly 0.42 to 0.58 - a sixth of the range,
    // centred exactly where inversion is blind.
    float blind = 1.0 - smoothstep(0.0, 0.08, abs(here - 0.5));
    return clamp(mix(inverted, tinted, blind), 0.0, 1.0);
}

// --- the pixel font ---------------------------------------------------------------------------
//
// Five by seven. Carried in the shader rather than uploaded as an atlas because it is a few
// hundred bytes, and an atlas is a descriptor, a staging copy and a sampler to keep in step with
// it. Seven rows of five bits is thirty-five, which does not fit a uint, so the top six rows are
// in .x and the last is in .y.
//
// Only the characters an interface with as little text as possible actually needs.
uvec2 ui_glyph(uint c) {
    if (c == 48u) return uvec2(0x239ACE2Eu, 0x0Eu);   // 0
    if (c == 49u) return uvec2(0x08421184u, 0x0Eu);   // 1
    if (c == 50u) return uvec2(0x1041062Eu, 0x1Fu);   // 2
    if (c == 51u) return uvec2(0x2211105Fu, 0x0Eu);   // 3
    if (c == 52u) return uvec2(0x05F928C2u, 0x02u);   // 4
    if (c == 53u) return uvec2(0x2210FA1Fu, 0x0Eu);   // 5
    if (c == 54u) return uvec2(0x231F4106u, 0x0Eu);   // 6
    if (c == 55u) return uvec2(0x1082083Fu, 0x08u);   // 7
    if (c == 56u) return uvec2(0x2317462Eu, 0x0Eu);   // 8
    if (c == 57u) return uvec2(0x0417C62Eu, 0x0Cu);   // 9
    if (c == 65u) return uvec2(0x231FC62Eu, 0x11u);   // A
    if (c == 66u) return uvec2(0x231F463Eu, 0x1Eu);   // B
    if (c == 67u) return uvec2(0x2308422Eu, 0x0Eu);   // C
    if (c == 68u) return uvec2(0x2518C65Cu, 0x1Cu);   // D
    if (c == 69u) return uvec2(0x210F421Fu, 0x1Fu);   // E
    if (c == 70u) return uvec2(0x210F421Fu, 0x10u);   // F
    if (c == 71u) return uvec2(0x231BC22Eu, 0x0Fu);   // G
    if (c == 72u) return uvec2(0x231FC631u, 0x11u);   // H
    if (c == 73u) return uvec2(0x0842108Eu, 0x0Eu);   // I
    if (c == 75u) return uvec2(0x254C5251u, 0x11u);   // K
    if (c == 76u) return uvec2(0x21084210u, 0x1Fu);   // L
    if (c == 77u) return uvec2(0x231AD771u, 0x11u);   // M
    if (c == 78u) return uvec2(0x2319D731u, 0x11u);   // N
    if (c == 79u) return uvec2(0x2318C62Eu, 0x0Eu);   // O
    if (c == 80u) return uvec2(0x210F463Eu, 0x10u);   // P
    if (c == 82u) return uvec2(0x254F463Eu, 0x11u);   // R
    if (c == 83u) return uvec2(0x0217420Fu, 0x1Eu);   // S
    if (c == 84u) return uvec2(0x0842109Fu, 0x04u);   // T
    if (c == 85u) return uvec2(0x2318C631u, 0x0Eu);   // U
    if (c == 86u) return uvec2(0x1518C631u, 0x04u);   // V
    if (c == 87u) return uvec2(0x375AC631u, 0x11u);   // W
    if (c == 88u) return uvec2(0x22A22A31u, 0x11u);   // X
    if (c == 89u) return uvec2(0x08422A31u, 0x04u);   // Y
    if (c == 90u) return uvec2(0x2082083Fu, 0x1Fu);   // Z
    if (c == 37u) return uvec2(0x16820B59u, 0x13u);   // %
    if (c == 46u) return uvec2(0x18000000u, 0x0Cu);   // .
    if (c == 45u) return uvec2(0x000F8000u, 0x00u);   // -
    return uvec2(0u, 0u);
}

// Whether a glyph has ink at a cell, with the row counted from the top.
bool ui_glyph_on(uvec2 bits, ivec2 cell) {
    if (cell.x < 0 || cell.x > 4 || cell.y < 0 || cell.y > 6) return false;
    uint row = (cell.y < 6) ? ((bits.x >> uint(cell.y * 5)) & 31u) : (bits.y & 31u);
    // Bit 4 is the leftmost column, so the pattern reads the way it was written down.
    return ((row >> uint(4 - cell.x)) & 1u) != 0u;
}

// How much ink a string puts at a point, where `at` is measured in glyph cells from the string's
// left edge and its capitals' top.
//
// Text is centred on its CAPITALS, not on its line box: a line box carries descender room whether
// the string has descenders or not, which puts a word visibly high in its button with an empty
// band beneath. A capital is the same box for every string at a given size, so that is what gets
// centred.
float ui_text(vec2 at, uint chars[24], uint count) {
    if (at.y < 0.0 || at.y >= 7.0) return 0.0;
    float advance = 6.0;                       // five wide, one of space
    int index = int(floor(at.x / advance));
    if (index < 0 || index >= int(count)) return 0.0;
    vec2 inside = vec2(at.x - float(index) * advance, at.y);
    if (inside.x >= 5.0) return 0.0;
    return ui_glyph_on(ui_glyph(chars[index]), ivec2(inside)) ? 1.0 : 0.0;
}
