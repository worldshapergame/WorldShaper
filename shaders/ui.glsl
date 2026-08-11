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
// Three by five for a capital, three by three for an x-height, one row below the baseline for a
// descender: the same typeface the world is lettered in, and for the same reason — see
// src/game/font.hpp for why that is the floor and which five letters are allowed past it.
//
// Carried in the shader rather than uploaded as an atlas because it is a few hundred bytes, and an
// atlas is a descriptor, a staging copy and a sampler to keep in step with it. The cell is five by
// six, which is thirty bits, so a glyph is one uint — five bits a row, bit 4 of a row being its
// leftmost column, row 0 at the cap height and row 5 below the baseline. The second component is
// the advance, before tracking.
//
// **This table is generated.** `tools/font-table.ps1` prints it from
// assets/font/00-basic-latin.txt, and tests/test_font.cpp reads it back out of this file and
// checks every glyph against that source. Two copies of a drawing is one place for them to
// disagree, and the disagreement is a failing test rather than a letter that is wrong only on
// screen.
//
// --- generated: tools/font-table.ps1 ---
const uvec2 kUiGlyphs[95] = uvec2[95](
    uvec2(0x00000000u, 2u),   // U+0020 space
    uvec2(0x01004210u, 1u),   // U+0021 exclamation mark
    uvec2(0x00000294u, 3u),   // U+0022 quotation mark
    uvec2(0x014E5394u, 3u),   // U+0023 number sign
    uvec2(0x0186230Cu, 3u),   // U+0024 dollar sign
    uvec2(0x01482094u, 3u),   // U+0025 percent sign
    uvec2(0x00CA3288u, 3u),   // U+0026 ampersand
    uvec2(0x00000210u, 1u),   // U+0027 apostrophe
    uvec2(0x00884208u, 2u),   // U+0028 left parenthesis
    uvec2(0x01042110u, 2u),   // U+0029 right parenthesis
    uvec2(0x00005114u, 3u),   // U+002A asterisk
    uvec2(0x00047100u, 3u),   // U+002B plus sign
    uvec2(0x20800000u, 2u),   // U+002C comma
    uvec2(0x000E0000u, 3u),   // U+002D hyphen-minus
    uvec2(0x01000000u, 1u),   // U+002E full stop
    uvec2(0x01082084u, 3u),   // U+002F solidus
    uvec2(0x008A7288u, 3u),   // U+0030 digit zero
    uvec2(0x01C42308u, 3u),   // U+0031 digit one
    uvec2(0x01C41288u, 3u),   // U+0032 digit two
    uvec2(0x01822098u, 3u),   // U+0033 digit three
    uvec2(0x00427294u, 3u),   // U+0034 digit four
    uvec2(0x0182621Cu, 3u),   // U+0035 digit five
    uvec2(0x008A6208u, 3u),   // U+0036 digit six
    uvec2(0x0084209Cu, 3u),   // U+0037 digit seven
    uvec2(0x008A2288u, 3u),   // U+0038 digit eight
    uvec2(0x00823288u, 3u),   // U+0039 digit nine
    uvec2(0x01004000u, 1u),   // U+003A colon
    uvec2(0x20802000u, 2u),   // U+003B semicolon
    uvec2(0x00444104u, 3u),   // U+003C less-than sign
    uvec2(0x01806000u, 2u),   // U+003D equals sign
    uvec2(0x01041110u, 3u),   // U+003E greater-than sign
    uvec2(0x00802098u, 3u),   // U+003F question mark
    uvec2(0x00C87288u, 3u),   // U+0040 commercial at
    uvec2(0x014A7288u, 3u),   // U+0041 capital A
    uvec2(0x018A6298u, 3u),   // U+0042 capital B
    uvec2(0x00C8420Cu, 3u),   // U+0043 capital C
    uvec2(0x018A5298u, 3u),   // U+0044 capital D
    uvec2(0x01C8621Cu, 3u),   // U+0045 capital E
    uvec2(0x0108621Cu, 3u),   // U+0046 capital F
    uvec2(0x00CA520Cu, 3u),   // U+0047 capital G
    uvec2(0x014A7294u, 3u),   // U+0048 capital H
    uvec2(0x01C4211Cu, 3u),   // U+0049 capital I
    uvec2(0x008A1084u, 3u),   // U+004A capital J
    uvec2(0x014C4314u, 3u),   // U+004B capital K
    uvec2(0x01C84210u, 3u),   // U+004C capital L
    uvec2(0x0118D771u, 5u),   // U+004D capital M
    uvec2(0x01295B52u, 4u),   // U+004E capital N
    uvec2(0x008A5288u, 3u),   // U+004F capital O
    uvec2(0x01086298u, 3u),   // U+0050 capital P
    uvec2(0x088A5288u, 3u),   // U+0051 capital Q
    uvec2(0x014A6298u, 3u),   // U+0052 capital R
    uvec2(0x0182220Cu, 3u),   // U+0053 capital S
    uvec2(0x0084211Cu, 3u),   // U+0054 capital T
    uvec2(0x01CA5294u, 3u),   // U+0055 capital U
    uvec2(0x008A5294u, 3u),   // U+0056 capital V
    uvec2(0x00AAD631u, 5u),   // U+0057 capital W
    uvec2(0x014A2294u, 3u),   // U+0058 capital X
    uvec2(0x00842294u, 3u),   // U+0059 capital Y
    uvec2(0x01C8209Cu, 3u),   // U+005A capital Z
    uvec2(0x01884218u, 2u),   // U+005B left square bracket
    uvec2(0x00422210u, 3u),   // U+005C reverse solidus
    uvec2(0x01842118u, 2u),   // U+005D right square bracket
    uvec2(0x00000288u, 3u),   // U+005E circumflex accent
    uvec2(0x38000000u, 3u),   // U+005F low line
    uvec2(0x00000110u, 2u),   // U+0060 grave accent
    uvec2(0x00CA6000u, 3u),   // U+0061 small a
    uvec2(0x018A6210u, 3u),   // U+0062 small b
    uvec2(0x00C83000u, 3u),   // U+0063 small c
    uvec2(0x00CA3084u, 3u),   // U+0064 small d
    uvec2(0x00CE2000u, 3u),   // U+0065 small e
    uvec2(0x0084710Cu, 3u),   // U+0066 small f
    uvec2(0x30CA3000u, 3u),   // U+0067 small g
    uvec2(0x014A6210u, 3u),   // U+0068 small h
    uvec2(0x01084010u, 1u),   // U+0069 small i
    uvec2(0x20842008u, 2u),   // U+006A small j
    uvec2(0x014C5210u, 3u),   // U+006B small k
    uvec2(0x00884210u, 2u),   // U+006C small l
    uvec2(0x015AF800u, 5u),   // U+006D small m
    uvec2(0x014A6000u, 3u),   // U+006E small n
    uvec2(0x008A2000u, 3u),   // U+006F small o
    uvec2(0x218A6000u, 3u),   // U+0070 small p
    uvec2(0x08CA3000u, 3u),   // U+0071 small q
    uvec2(0x01086000u, 3u),   // U+0072 small r
    uvec2(0x01843000u, 3u),   // U+0073 small s
    uvec2(0x00C47108u, 3u),   // U+0074 small t
    uvec2(0x01CA5000u, 3u),   // U+0075 small u
    uvec2(0x008A5000u, 3u),   // U+0076 small v
    uvec2(0x00AAC400u, 5u),   // U+0077 small w
    uvec2(0x01445000u, 3u),   // U+0078 small x
    uvec2(0x30CA5000u, 3u),   // U+0079 small y
    uvec2(0x01C47000u, 3u),   // U+007A small z
    uvec2(0x00C4410Cu, 3u),   // U+007B left curly bracket
    uvec2(0x21084210u, 1u),   // U+007C vertical line
    uvec2(0x01841118u, 3u),   // U+007D right curly bracket
    uvec2(0x000C3000u, 3u)   // U+007E tilde
);
// --- end generated ---

// The styles a run can be drawn in, matching Style in src/game/font.hpp value for value, because a
// label that reads `**Fast**` on a sign and plain on a panel is two answers to one question.
const uint kUiPlain = 0u;
const uint kUiBold = 1u;
const uint kUiItalic = 2u;
const uint kUiUnderline = 4u;
const uint kUiStrike = 8u;
const uint kUiMono = 16u;

const int kUiCap = 5;          // rows above the baseline a capital fills
const int kUiRows = 6;         // and the whole cell, the descender's row included
const float kUiMonoStep = 6.0; // what every glyph steps in a code span: `m` is five wide

uvec2 ui_glyph(uint c) {
    // Anything the font does not have comes out blank and still takes room, which is the failure a
    // reader can diagnose — a gap that silently closes moves everything after it.
    if (c < 32u || c > 126u) return uvec2(0u, 3u);
    return kUiGlyphs[c - 32u];
}

// How far the pen moves for one glyph. Proportional, because an `i` set three wide puts a hole
// either side of every one.
//
// Neither emphasis adds to it any more, and that is the same change as the one in `ui_ink_at`
// below: bold is a device pixel and italic is a shear, so neither costs a whole cell of width.
// A bold run and a plain run of the same words now set to the same width, which is also what makes
// `**this**` in a sentence not shift the rest of the line.
float ui_step(uvec2 glyph, uint style) {
    if ((style & kUiMono) != 0u) return kUiMonoStep;
    return float(glyph.y) + 1.0;                             // the glyph, and one of tracking
}

// Whether the drawing has ink at a cell, row 0 being the cap height.
bool ui_cell_on(uint bits, ivec2 cell) {
    if (cell.x < 0 || cell.x > 4 || cell.y < 0 || cell.y >= kUiRows) return false;
    // Bit 4 is the leftmost column, so the pattern reads the way it was written down.
    return ((bits >> uint(cell.y * 5 + (4 - cell.x))) & 1u) != 0u;
}

// How much ink the smeared half of a bold glyph carries, and how far it reaches.
//
// Both numbers were arrived at by looking. A whole cell of smear at full ink is what the rule
// started as and it fills every counter the face has: `o`, `e`, `a`, `g` and `y` come out as
// blocks. One device pixel at half ink is the other end of the mistake — a quarter of a cell at
// 0.55 is about a seventh of a stroke's worth of extra ink, which is not a weight, it is nothing.
//
// HALF a cell at nearly full ink is what reads. The stroke gains half its own width again, so the
// word is plainly heavier; the counter loses half of itself and keeps the other half, so it is
// still a hole. The two numbers are doing different jobs and both are needed: the reach is what
// makes it bold, and the ink being under one is what keeps the added half legible AS an addition
// rather than as a second column of the letter.
const float kUiBoldInk = 0.72;
const float kUiBoldReach = 0.5;   // in cells

// The ink of one glyph at a point inside its own cell, with the style applied.
//
// # Why bold is half a cell and not a whole one
//
// The rule this started as — *bold is the drawing smeared one pixel sideways, one wider* — is
// right about the world, where a glyph pixel IS a voxel and there is nothing finer to smear into.
// It is wrong about a panel. At three columns across, a counter is one cell: smear a whole cell
// and `o`, `e`, `a`, `g` and `y` all fill in solid, so a bold word is not heavier, it is a row of
// blocks. It was legible in the specification and unreadable on the screen, which is the only
// place that counts.
//
// On a panel a glyph pixel is SEVERAL device pixels — four, at the size a row is set at — so a
// fraction of a cell exists to smear into, and half of one is what bold takes. See kUiBoldInk
// above for why it is a half rather than a quarter, and why the added half is drawn at less than
// full ink. It degrades honestly: at one device pixel per glyph pixel there is no fraction to be
// had and the smear is a whole cell again, which is exactly the case the world's own letters are
// always in.
//
// Italic leans by the same reasoning. The rule says *the rows above the x-height leaned one pixel
// right*, and one pixel is the whole lean available; here that same one pixel is spread
// continuously over the cap height instead of stepping at the x-height, which is the same total
// lean without the notch a step puts in the side of every `l`.
//
// The underline hangs a row below the descender so it never touches a `p`, and the strike runs
// along the middle of the x-height, where the hyphen already is.
//
// `smear` is one device pixel measured in cells — 1.0 / (device pixels per glyph pixel).
float ui_ink_at(uvec2 glyph, vec2 inside, uint style, float smear) {
    if (inside.x < 0.0 || inside.y < 0.0 || inside.y >= float(kUiRows + 1)) return 0.0;

    // The lean, in cells, at this height: one cell at the cap line falling to nothing at the
    // baseline. Rows below the baseline lean back the other way by the same slope, so a `p`'s bowl
    // and its descender stay on one stem.
    float lean = 0.0;
    if ((style & kUiItalic) != 0u) {
        lean = (float(kUiCap) - inside.y) / float(kUiCap);
    }
    vec2 at = vec2(inside.x - lean, inside.y);

    ivec2 cell = ivec2(floor(at));
    if ((style & kUiUnderline) != 0u && cell.y == kUiRows) return 1.0;
    if ((style & kUiStrike) != 0u && cell.y == 3) return 1.0;
    if (cell.y >= kUiRows) return 0.0;

    if (ui_cell_on(glyph.x, cell)) return 1.0;
    if ((style & kUiBold) != 0u) {
        ivec2 behind = ivec2(floor(vec2(at.x - smear, at.y)));
        if (ui_cell_on(glyph.x, behind)) return kUiBoldInk;
    }
    return 0.0;
}

// How wide a string sets, so a caller can centre it without drawing it first.
float ui_width(uint chars[24], uint count, uint style) {
    float pen = 0.0;
    for (uint i = 0u; i < count; ++i) pen += ui_step(ui_glyph(chars[i]), style);
    return max(pen - 1.0, 0.0);   // the tracking after the last letter is not part of the word
}

// How much ink a string puts at a point, where `at` is measured in glyph pixels from the string's
// left edge and its capitals' top.
//
// Text is centred on its CAPITALS, not on its line box: a line box carries descender room whether
// the string has descenders or not, which puts a word visibly high in its button with an empty
// band beneath. A capital is the same box for every string at a given size, so that is what gets
// centred.
float ui_text(vec2 at, uint chars[24], uint count, uint style, float smear) {
    if (at.y < 0.0 || at.y >= float(kUiRows + 1)) return 0.0;
    float pen = 0.0;
    for (uint i = 0u; i < count; ++i) {
        uvec2 glyph = ui_glyph(chars[i]);
        float step = ui_step(glyph, style);
        // The smear and the lean both reach a little past a glyph's own cell, so a run is walked
        // with a margin either side rather than by which cell a point falls in — otherwise the
        // weight bold adds to a letter is clipped off by the letter after it.
        if (at.x >= pen - 1.0 && at.x < pen + step + 1.0) {
            float ink = ui_ink_at(glyph, vec2(at.x - pen, at.y), style, smear);
            if (ink > 0.0) return ink;
        }
        pen += step;
    }
    return 0.0;
}
