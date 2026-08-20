#pragma once
// The typeface, as the shell measures it.
//
// # Why this table exists when shaders/ui.glsl already has one
//
// The card DRAWS the words and the shell MEASURES them, and those are different jobs on
// different processors. Where a word ends decides how wide its button is, which decides what a
// click at a given pixel landed on — so a layout worked out from advances the card does not share
// is a layout whose hit boxes sit somewhere other than its drawings. Uploading the shader's table
// back to the host would make the host wait on the card to lay out a menu; carrying a second
// hand-written table would make the two disagree the first time a letter was redrawn.
//
// So both are GENERATED from assets/font/00-basic-latin.txt by tools/font-table.ps1, and
// tests/test_font.cpp checks BOTH against that source, glyph by glyph and advance by advance
// ("the shader's font is the font", "the shell's font is the font"). The third copy that would
// have to exist to settle an argument between them never has to be written.
//
// # The packing
//
// A five by six cell: row 0 at the cap height, row 5 below the baseline, five bits a row, bit 4 of
// a row being its leftmost column — so the number reads the way the drawing was written down.
// Thirty bits, which is why a glyph is one u32. `advance` is the cells the pen steps, before
// tracking. See src/game/font.hpp for why three by five is the floor and which five letters are
// allowed past it.

#include "core/types.hpp"

namespace ws::ui {

struct Glyph {
    u32 bits = 0;
    u32 advance = 3;
};

// The styles a run can be drawn in. The same values as Style in src/game/font.hpp and as the
// kUi* constants in shaders/ui.glsl, because a label that reads `**Fast**` on a sign and plain on
// a panel is two answers to one question.
enum GlyphStyle : u32 {
    kPlain = 0,
    kBold = 1u << 0,
    kItalic = 1u << 1,
    kUnderline = 1u << 2,
    kStrike = 1u << 3,
    kMono = 1u << 4,
};

// Which of the three permitted hues a run is STATED in, in the two bits above the emphases.
//
// Nought is the ordinary ink and is what almost everything is: the per-pixel inverse of whatever is
// behind it, with no colour in it at all. One, two and three are `ui::tint_of`'s three rotations of
// the player's own accent, and they exist for the fourth of `14-ui-style.md`'s five permitted
// colours — *command-line parts, verb, subject and value, because several unlike things are on one
// line and telling them apart is the whole task.* A script is that argument at length.
//
// Packed into the STYLE word rather than into a field of its own because a `Command` is sixty-four
// bytes and every one of them is spoken for. Both the host and the shader test the emphases with
// `& kBold` and friends, so bits above them are ignored by everything that does not look for them.
constexpr u32 kTintShift = 5;
constexpr u32 kTintBits = 3u << kTintShift;

// `style` with a hue on it. `which` is nought for the ordinary ink and 1..3 for the three.
inline constexpr u32 tinted(u32 style, u32 which) {
    return (style & ~kTintBits) | ((which & 3u) << kTintShift);
}

constexpr i32 kGlyphCap = 5;         // rows above the baseline a capital fills
constexpr i32 kGlyphRows = 6;        // the whole cell, the descender's row included
constexpr f32 kGlyphMonoStep = 6.0f; // what every glyph steps in a code span: `m` is five wide

// --- generated: tools/font-table.ps1 -Format cpp ---
constexpr Glyph kGlyphs[95] = {
    {0x00000000u, 2u},   // U+0020 space
    {0x01004210u, 1u},   // U+0021 exclamation mark
    {0x00000294u, 3u},   // U+0022 quotation mark
    {0x014E5394u, 3u},   // U+0023 number sign
    {0x0186230Cu, 3u},   // U+0024 dollar sign
    {0x01482094u, 3u},   // U+0025 percent sign
    {0x00CA3288u, 3u},   // U+0026 ampersand
    {0x00000210u, 1u},   // U+0027 apostrophe
    {0x00884208u, 2u},   // U+0028 left parenthesis
    {0x01042110u, 2u},   // U+0029 right parenthesis
    {0x00005114u, 3u},   // U+002A asterisk
    {0x00047100u, 3u},   // U+002B plus sign
    {0x20800000u, 2u},   // U+002C comma
    {0x000E0000u, 3u},   // U+002D hyphen-minus
    {0x01000000u, 1u},   // U+002E full stop
    {0x01082084u, 3u},   // U+002F solidus
    {0x008A7288u, 3u},   // U+0030 digit zero
    {0x01C42308u, 3u},   // U+0031 digit one
    {0x01C41288u, 3u},   // U+0032 digit two
    {0x01822098u, 3u},   // U+0033 digit three
    {0x00427294u, 3u},   // U+0034 digit four
    {0x0182621Cu, 3u},   // U+0035 digit five
    {0x008A6208u, 3u},   // U+0036 digit six
    {0x0084209Cu, 3u},   // U+0037 digit seven
    {0x008A2288u, 3u},   // U+0038 digit eight
    {0x00823288u, 3u},   // U+0039 digit nine
    {0x01004000u, 1u},   // U+003A colon
    {0x20802000u, 2u},   // U+003B semicolon
    {0x00444104u, 3u},   // U+003C less-than sign
    {0x01806000u, 2u},   // U+003D equals sign
    {0x01041110u, 3u},   // U+003E greater-than sign
    {0x00802098u, 3u},   // U+003F question mark
    {0x00C87288u, 3u},   // U+0040 commercial at
    {0x014A7288u, 3u},   // U+0041 capital A
    {0x018A6298u, 3u},   // U+0042 capital B
    {0x00C8420Cu, 3u},   // U+0043 capital C
    {0x018A5298u, 3u},   // U+0044 capital D
    {0x01C8621Cu, 3u},   // U+0045 capital E
    {0x0108621Cu, 3u},   // U+0046 capital F
    {0x00CA520Cu, 3u},   // U+0047 capital G
    {0x014A7294u, 3u},   // U+0048 capital H
    {0x01C4211Cu, 3u},   // U+0049 capital I
    {0x008A1084u, 3u},   // U+004A capital J
    {0x014C4314u, 3u},   // U+004B capital K
    {0x01C84210u, 3u},   // U+004C capital L
    {0x0118D771u, 5u},   // U+004D capital M
    {0x01295B52u, 4u},   // U+004E capital N
    {0x008A5288u, 3u},   // U+004F capital O
    {0x01086298u, 3u},   // U+0050 capital P
    {0x088A5288u, 3u},   // U+0051 capital Q
    {0x014A6298u, 3u},   // U+0052 capital R
    {0x0182220Cu, 3u},   // U+0053 capital S
    {0x0084211Cu, 3u},   // U+0054 capital T
    {0x01CA5294u, 3u},   // U+0055 capital U
    {0x008A5294u, 3u},   // U+0056 capital V
    {0x00AAD631u, 5u},   // U+0057 capital W
    {0x014A2294u, 3u},   // U+0058 capital X
    {0x00842294u, 3u},   // U+0059 capital Y
    {0x01C8209Cu, 3u},   // U+005A capital Z
    {0x01884218u, 2u},   // U+005B left square bracket
    {0x00422210u, 3u},   // U+005C reverse solidus
    {0x01842118u, 2u},   // U+005D right square bracket
    {0x00000288u, 3u},   // U+005E circumflex accent
    {0x38000000u, 3u},   // U+005F low line
    {0x00000110u, 2u},   // U+0060 grave accent
    {0x00CA6000u, 3u},   // U+0061 small a
    {0x018A6210u, 3u},   // U+0062 small b
    {0x00C83000u, 3u},   // U+0063 small c
    {0x00CA3084u, 3u},   // U+0064 small d
    {0x00CE2000u, 3u},   // U+0065 small e
    {0x0084710Cu, 3u},   // U+0066 small f
    {0x30CA3000u, 3u},   // U+0067 small g
    {0x014A6210u, 3u},   // U+0068 small h
    {0x01084010u, 1u},   // U+0069 small i
    {0x20842008u, 2u},   // U+006A small j
    {0x014C5210u, 3u},   // U+006B small k
    {0x00884210u, 2u},   // U+006C small l
    {0x015AF800u, 5u},   // U+006D small m
    {0x014A6000u, 3u},   // U+006E small n
    {0x008A2000u, 3u},   // U+006F small o
    {0x218A6000u, 3u},   // U+0070 small p
    {0x08CA3000u, 3u},   // U+0071 small q
    {0x01086000u, 3u},   // U+0072 small r
    {0x01843000u, 3u},   // U+0073 small s
    {0x00C47108u, 3u},   // U+0074 small t
    {0x01CA5000u, 3u},   // U+0075 small u
    {0x008A5000u, 3u},   // U+0076 small v
    {0x00AAC400u, 5u},   // U+0077 small w
    {0x01445000u, 3u},   // U+0078 small x
    {0x30CA5000u, 3u},   // U+0079 small y
    {0x01C47000u, 3u},   // U+007A small z
    {0x00C4410Cu, 3u},   // U+007B left curly bracket
    {0x21084210u, 1u},   // U+007C vertical line
    {0x01841118u, 3u},   // U+007D right curly bracket
    {0x000C3000u, 3u},   // U+007E tilde
};
// --- end generated ---

// Anything the font does not have comes out blank and still takes room, which is the failure a
// reader can diagnose — a gap that silently closes moves everything after it.
inline Glyph glyph_of(u32 codepoint) {
    if (codepoint < 32u || codepoint > 126u) return Glyph{0u, 3u};
    return kGlyphs[codepoint - 32u];
}

// How far the pen moves for one glyph, in glyph pixels. Proportional, because an `i` set three
// wide puts a hole either side of every one. The arithmetic is the same as `ui_step` in
// shaders/ui.glsl and has to stay that way: this is the copy that decides where a button ends.
//
// No emphasis adds to it. On a panel, bold is one DEVICE pixel of smear and italic is a shear of
// one glyph pixel spread over the cap height — neither costs a cell, so neither changes where a
// word ends. That is also what stops `**this**` in the middle of a sentence from moving the rest
// of the line as it is typed. The world's own letters are a different case and game/font.hpp keeps
// the width there: a voxel has no fraction to smear into.
inline f32 glyph_step(const Glyph& glyph, u32 style) {
    if ((style & kMono) != 0u) return kGlyphMonoStep;
    return static_cast<f32>(glyph.advance) + 1.0f;   // the glyph, and one of tracking
}

// Whether the drawing has ink at a cell, row 0 being the cap height. Only the tests and the
// screenshot comparisons walk this; the card draws from its own copy of the same numbers.
inline bool glyph_cell_on(u32 bits, i32 x, i32 y) {
    if (x < 0 || x > 4 || y < 0 || y >= kGlyphRows) return false;
    return ((bits >> static_cast<u32>(y * 5 + (4 - x))) & 1u) != 0u;
}

}  // namespace ws::ui
