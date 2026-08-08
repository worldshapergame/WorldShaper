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
// on a sign is a thing somebody has to build. Five by seven for capitals is the smallest size at
// which the Latin alphabet is unambiguous — four wide loses the difference between B and 8, and
// six wide buys nothing a reader can use.
//
// The cell is five by nine: seven above the baseline for capitals, two below for descenders. A
// glyph declares only the rows it uses, so an `x` is five by five and costs what it looks like.
//
// Emoji are the exception and they earn it. A face at five by seven is a smudge; the smallest
// size at which a rounded mouth reads as a mouth is about eleven across. They also carry colour,
// because a yellow circle and a red one are different emoji and a monochrome one is a shrug. So
// an emoji is up to sixteen square with a palette of its own — still tiny, still hand-placed,
// still every pixel argued for.
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
    i8 top = 0;        // rows above the baseline the first row sits at; 7 for a capital
    u8 advance = 0;    // cells to step before the next glyph, including the gap
    std::vector<u8> pixels;
    std::vector<u32> palette;   // 0xRRGGBB, entry 0 unused so an index of 0 can mean empty

    // More than one INK. Entry 0 is the empty cell and never a colour, so a plain letter has a
    // palette of two — nothing, and the one ink it is drawn in — and is not coloured.
    bool coloured() const { return palette.size() > 2; }
    u8 at(u32 x, u32 y) const {
        return (x < width && y < height) ? pixels[static_cast<usize>(y) * width + x] : u8{0};
    }
};

// How a string is laid out, in cells.
struct TextMetrics {
    i32 width = 0;
    i32 above = 0;   // cells above the baseline
    i32 below = 0;   // and below it
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

    const std::vector<std::string>& problems() const { return problems_; }

private:
    std::unordered_map<u32, Glyph> glyphs_;
    std::vector<std::string> problems_;
};

// How a word becomes matter.
struct TextRequest {
    std::string utf8;
    i32 tracking = 1;         // cells between glyphs, on top of each glyph's own advance
    i32 depth = 1;            // how many voxels deep the letters stand
    i32 scale = 1;            // voxels per glyph pixel
    bool sunk = false;        // cut into a face rather than standing proud of it

    // What a letter is made of when the glyph has no colour of its own. A coloured glyph brings
    // its own materials and this is ignored for those pixels.
    VoxelTypeId material = 1;
};

// A word, as a clip: one voxel per glyph pixel per scale step, `depth` deep.
//
// The clip's own origin is the left end of the baseline, so two lines set from the same point
// line up the way a reader expects rather than the way an array happens to be indexed.
Clip text_to_clip(const Font& font, const TextRequest& request, VoxelTypeTable& types);

}  // namespace ws
