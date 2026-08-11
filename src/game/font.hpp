#pragma once
// The game's own typeface, and the rule that a letter is voxels like everything else.
//
// # Why a font at all
//
// A player will want to write on things — a sign over a door, a number on a locker, a name on a
// crate. In a voxel world that is not a text feature, it is a *modelling* feature: the letters
// have to become matter, castable, carvable, and lit by the same renderer as the wall they are
// on. So this produces a clip, and everything that is true of a clip is then true of a word.
//
// # The philosophy: the fewest pixels that still read
//
// This is a pixel typeface and its whole discipline is utilitarian. A glyph is as small as it can
// be and still be unmistakable at one voxel per pixel, because every pixel is a voxel and a word
// on a sign is a thing somebody has to build.
//
// **Three by five for a capital, three by three for an x-height, one row below the baseline for a
// descender.** Six rows and three columns is the whole cell. That is the floor for an alphabet
// carrying both cases, and the floor is where the shapes stop being distinct rather than where
// they get harder to read: at three by four the capitals lose the row that holds B's, E's and S's
// middle bar, and at two wide there is no interior left — no bowl, no counter, nothing to tell an
// `o` from an `n`. Five letters break the three-column rule and each is argued in the font
// source: `M`, `W`, `m` and `w` are five wide because three strokes need two gaps, and `N` is four
// because a diagonal with one column to fall through is a horizontal bar.
//
// What it buys: a capital is fifteen cells against the thirty-five the five-by-seven cell this
// file used to describe cost, and an `o` is nine. A sign is a thing somebody builds voxel by
// voxel, so that is not a saving on a texture — it is less than half the wall.
//
// # Markdown, because the alternative is a second way to say the same thing
//
// Text in this game is written by players — on signs, in the tooltip that explains a setting, in
// whatever a mod puts on a panel — and the moment there is more than one sentence somebody wants
// a heading, a list, or one word made to stand out. Markdown is what people already type when
// they want that, so it is what this reads: `**bold**`, `*italic*`, `` `code` ``, `~~struck~~`,
// `[label](target)`, `# headings`, `- bullets`, `1. numbers`, `> quotes`, fenced code and `---`.
// See `markup.hpp`; the emphases are drawn here because they are properties of a glyph:
//
//   - **bold** is the glyph smeared one pixel sideways, and one wider. At three pixels across
//     there is no second weight to draw — a bold `e` cannot have thicker strokes than an `e`, it
//     can only have more of them.
//   - *italic* leans the rows above the x-height one pixel right. One pixel is the entire lean
//     available; two turns a three-wide letter into a diagonal.
//   - underline hangs one row below the descender, so it never touches a `p`.
//   - a strike-through runs along the middle of the x-height, which is where the hyphen already is.
//   - `code` is set monospaced. A box around a word costs more pixels than the word does at this
//     size, so the thing that says "this is literal" is that the letters stop fitting each other.
//
// # Why a text file and not a binary
//
// A glyph is a picture and a picture wants to be looked at. The source is a plain text file where
// a glyph is drawn with hashes and dots, so adding a character is drawing it and reviewing one is
// reading it. A diff of a font change shows the letter that changed, in the shape it changed to.
//
// That matters more here than usual: "supports a ton of characters" is not a thing one person
// does in one sitting, it is a thing many people add to over years, and the format has to make an
// addition cheap and a mistake obvious.

#include <string>
#include <unordered_map>
#include <vector>

#include "core/types.hpp"
#include "game/clip.hpp"

namespace ws {

class VoxelTypeTable;

// The cell, in pixels. Everything that lays text out reads these rather than a literal, because
// the whole point of the size is that it is a decision and not an accident.
constexpr i32 kCapHeight = 5;      // rows above the baseline a capital or an ascender fills
constexpr i32 kXHeight = 3;        // and a plain lowercase letter
constexpr i32 kDescent = 1;        // rows below it a `p` hangs into
constexpr i32 kNominalWidth = 3;   // what a letter is wide, and what a missing one takes
constexpr i32 kMonoAdvance = 6;    // the step every glyph takes in a code span, tracking included

// How a run of text is drawn. These are properties of the glyph, so they live with the font; what
// PUTS them on a run is the markdown reader in markup.hpp.
enum Style : u8 {
    kPlain = 0,
    kBold = 1u << 0,
    kItalic = 1u << 1,
    kUnderline = 1u << 2,
    kStrike = 1u << 3,
    kMono = 1u << 4,
};

// One character, drawn.
//
// `pixels` is one byte per cell, row-major from the top: 0 is empty, and anything else is an
// index into `palette`. A plain letter has a one-entry palette and reads as a bitmap; an emoji
// has up to fifteen colours and reads as a tiny picture. One representation for both, because a
// letter that happened to be red should not need a second code path.
struct Glyph {
    u32 codepoint = 0;
    u8 width = 0;
    u8 height = 0;
    i8 top = 0;        // rows above the baseline the first row sits at; 5 for a capital
    u8 advance = 0;    // cells to step before the next glyph, before tracking
    std::vector<u8> pixels;
    std::vector<u32> palette;   // 0xRRGGBB, entry 0 unused so an index of 0 can mean empty

    // More than one INK. Entry 0 is the empty cell and never a colour, so a plain letter has a
    // palette of two — nothing, and the one ink it is drawn in — and is not coloured.
    bool coloured() const { return palette.size() > 2; }
    u8 at(u32 x, u32 y) const {
        return (x < width && y < height) ? pixels[static_cast<usize>(y) * width + x] : u8{0};
    }
};

// How a string is laid out, in pixels.
struct TextMetrics {
    i32 width = 0;
    i32 above = 0;   // pixels above the baseline
    i32 below = 0;   // and below it, the underline's row included when there is one
};

class Font {
public:
    // Reads the plain-text source. Several files may be read into one font, which is how the
    // character set grows without any one file becoming unreadable: one for Latin, one for
    // punctuation, one per emoji group.
    bool read(const std::string& path);
    bool read_directory(const std::string& path);

    const Glyph* find(u32 codepoint) const;
    usize size() const { return glyphs_.size(); }

    // What is missing, so a caller can say so rather than silently dropping it.
    std::vector<u32> missing(const std::string& utf8) const;

    TextMetrics measure(const std::string& utf8, i32 tracking = 1) const;
    TextMetrics measure_styled(const std::string& utf8, u8 style, i32 tracking = 1) const;

    // How far the pen moves for one glyph, style and tracking included. A missing glyph still
    // takes room, because a gap that silently closes changes the spacing of everything after it.
    i32 step(const Glyph* glyph, u8 style, i32 tracking) const;

    const std::vector<std::string>& problems() const { return problems_; }

private:
    std::unordered_map<u32, Glyph> glyphs_;
    std::vector<std::string> problems_;
};

// UTF-8 in, codepoints out. Shared with the markdown reader, which has to walk the same text.
// Malformed input yields U+FFFD rather than stopping, because a font is asked to draw whatever
// somebody typed and "your text is invalid" is not a useful answer to somebody who wants a sign.
std::vector<u32> decode_utf8(const std::string& text);

// A drawing, before it is anything else.
//
// Both a line of text and a whole marked-up page land here first, and only then become matter.
// The intermediate exists because the two paths agree about everything except how they place
// glyphs: one clip conversion, one set of colour rules, one place a decoration can be got wrong.
// It is also what a screen-space UI will upload later, without going through voxels at all.
struct InkPage {
    i32 width = 0;
    i32 height = 0;
    i32 baseline = 0;   // rows from the top to the first line's baseline
    // 0 is empty, 1 is the caller's own ink, and 2 upwards indexes `colours`.
    std::vector<u8> pixels;
    std::vector<u32> colours;   // 0xRRGGBB for index 2 and up; entries 0 and 1 are unused

    u8 at(i32 x, i32 y) const {
        return (x >= 0 && y >= 0 && x < width && y < height)
                   ? pixels[static_cast<usize>(y) * width + x]
                   : u8{0};
    }
    void set(i32 x, i32 y, u8 ink) {
        if (x >= 0 && y >= 0 && x < width && y < height) {
            pixels[static_cast<usize>(y) * width + x] = ink;
        }
    }
    // The colour index for an RGB, interned so a word in one emoji costs one entry per colour in
    // it rather than one per pixel.
    u8 intern(u32 rgb);
};

// How a drawing becomes matter.
struct MatterRequest {
    i32 depth = 1;            // how many voxels deep the letters stand
    i32 scale = 1;            // voxels per glyph pixel
    bool sunk = false;        // cut into a face rather than standing proud of it

    // What a letter is made of when the glyph has no colour of its own. A coloured glyph brings
    // its own materials and this is ignored for those pixels.
    VoxelTypeId material = 1;
};

// How a word becomes matter.
struct TextRequest : MatterRequest {
    std::string utf8;
    u8 style = kPlain;
    i32 tracking = 1;         // pixels between glyphs, on top of each glyph's own advance
};

// One line of text, drawn. The page is exactly the size the metrics said it would be, so a caller
// can lay two lines out against each other without setting them first.
InkPage set_text(const Font& font, const std::string& utf8, u8 style = kPlain, i32 tracking = 1);

// A solid block of the caller's own ink: an underline, a strike-through, a rule between sections,
// the bar down the side of a quotation. Every straight line a document draws is one of these, and
// they are all the same thing so that none of them can be off by a pixel on its own.
void paint_rule(InkPage& page, i32 x, i32 width, i32 y, i32 thickness);

// One glyph, with its style, painted into a page.
//
// `base` is the row that sits just BELOW the baseline, so a pixel at height h lands at
// `base - h * scale`: a capital's top row (h = 5) is five bands up, a descender (h = 0) is the base
// row itself, and an underline (h = -1) is one band below it. Measuring from the baseline rather
// than from the top of the cell is what lets a `p` and an `o` share a line.
void paint_glyph(InkPage& page, const Glyph& glyph, i32 pen, i32 base, u8 style, i32 scale);

// How wide the ink of one glyph actually is, which is not its advance: bold is the same drawing
// smeared a pixel sideways and italic leans its upper rows a pixel right, so both put ink one
// column past the cell. The distinction matters for an underline, which has to end where the ink
// does and not where the next letter starts. A missing glyph has no ink and takes none.
i32 ink_width(const Glyph* glyph, u8 style);

// A drawing, as a clip: one voxel per ink pixel per scale step, `depth` deep.
//
// The clip's own origin is the left end of the baseline, so two lines set from the same point
// line up the way a reader expects rather than the way an array happens to be indexed.
Clip page_to_clip(const InkPage& page, const MatterRequest& request, VoxelTypeTable& types);

// A word, as a clip.
Clip text_to_clip(const Font& font, const TextRequest& request, VoxelTypeTable& types);

}  // namespace ws
