#pragma once
// Markdown, and why a voxel game reads it.
//
// # Why markdown at all
//
// Text in this game is written by players: on a sign over a door, in the one sentence a tooltip
// gets, in whatever a mod puts on a panel. The moment there is more than one sentence, somebody
// wants a heading, a list, or one word made to stand out — and what people already type when they
// want that is `**bold**` and `- a bullet`. Reading it costs a few hundred lines; inventing a
// second notation costs everybody who ever writes a sign the trouble of learning it, and it would
// be markdown with the corners filed off anyway.
//
// It also earns its place *because* the typeface is tiny. Three pixels across leaves no room to
// say "this is a heading" by choosing a different face, so structure has to be said with size,
// weight and marks — which is exactly the vocabulary markdown already carries.
//
// # What is read, and what is deliberately not
//
// Read: `#` to `######` headings, `- `/`* `/`+ ` bullets with nesting by indent, `1.`/`1)`
// numbered items, `- [ ]`/`- [x]` task boxes, `>` quotations (nested), fenced code blocks,
// `---`/`***`/`___` rules, `**bold**`, `__bold__`, `*italic*`, `_italic_`, `` `code` ``,
// `~~struck~~`, `[label](target)`, and `\` to escape any of them.
//
// Not read, and each is a decision rather than a gap:
//
//   - **Tables.** A table needs columns to line up, and at three pixels a column rule is a third
//     of a letter. A list says the same thing in the space that exists.
//   - **Images.** `![alt](src)` sets its alt text as a link. There is nowhere to fetch a picture
//     from inside a world, and the emoji block already covers the one case that matters.
//   - **Raw HTML.** There is no browser here and never will be one.
//   - **Setext headings** (underlining with `===`). Two ways to write a heading is one more than a
//     player needs, and `---` is already a rule.
//   - **Reference links** (`[a][b]` with a definition elsewhere). They are a document feature;
//     nothing here is a document long enough to want one.
//
// `_` inside a word is not emphasis, so `snake_case_names` survive being written down. That single
// rule is most of what CommonMark's flanking machinery buys, at a fraction of the size.

#include <string>
#include <vector>

#include "core/types.hpp"
#include "game/clip.hpp"
#include "game/font.hpp"

namespace ws {

class VoxelTypeTable;

// What one line of a document is. A quotation is not one of these: it is a depth carried by any
// of them, because `> - a bullet` is a bullet that happens to be quoted.
enum class MarkupBlock : u8 {
    paragraph,
    heading,
    bullet,
    ordered,
    code,
    rule,
    blank,
};

// A stretch of text drawn in one style.
struct Run {
    std::string text;
    u8 style = kPlain;
};

struct MarkupLine {
    MarkupBlock kind = MarkupBlock::paragraph;
    u8 level = 0;      // heading depth, 1 to 6
    u8 indent = 0;     // list nesting, in steps of two written spaces
    u8 quote = 0;      // how many `>` deep
    i8 box = -1;       // -1 for no task box, 0 for an empty one, 1 for a ticked one
    u32 ordinal = 0;   // the number a numbered item was actually written with
    std::vector<Run> runs;
};

struct Markup {
    std::vector<MarkupLine> lines;
};

// The reader. Never fails: anything it does not recognise is text, which is the property that lets
// somebody write a sign without knowing they are writing markdown.
Markup parse_markup(const std::string& markdown);

// How a document becomes matter.
struct MarkupRequest : MatterRequest {
    std::string markdown;
    i32 tracking = 1;   // pixels between glyphs
    i32 leading = 2;    // pixels between one line's lowest row and the next line's capitals
    i32 wrap = 0;       // wrap at this many pixels of width; 0 sets every line as written
};

// The document, drawn. One page, so the caller can put it on a wall, in a clip, or later into a
// texture without any of those knowing what a bullet is.
InkPage set_markup(const Font& font, const MarkupRequest& request);

// And as matter.
Clip markup_to_clip(const Font& font, const MarkupRequest& request, VoxelTypeTable& types);

}  // namespace ws
