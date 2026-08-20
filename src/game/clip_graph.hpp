#pragma once
// A clip document, read as a graph — which is what the visual view of the editor draws.
//
// documentation/23-shell-and-libraries.md §5c is the specification: **the visual editor and the
// script editor are two views of one document, and editing either updates the other, live**
// (D452). This is the reader that makes the second view possible, and the three rules it exists
// to keep are D453, D454 and the round-trip rule beside them:
//
//   **A script that does not parse is not an error.** Half of every word being typed is a syntax
//   error, so this reader never fails. It reads what it can and hands back the rest.
//
//   **Anything the view cannot draw is an opaque TEXT NODE carrying its own source, never
//   something dropped** (D454). A view that silently discards what it cannot represent deletes a
//   player's work the first time they open it, and that is the single failure the arrangement
//   exists to make impossible. So a run of lines this reader cannot read becomes one node with
//   those lines inside it, and the document is still the document.
//
//   **A round trip does not reformat what you wrote.** Comments, blank lines, the order things
//   were declared in and the author's own spacing are part of the document rather than decoration
//   around it. So every number here carries the LINE and COLUMN it was written at and the text it
//   was written as, and a visual edit rewrites those bytes and nothing else.
//
// # Why this is a reader of the DOCUMENT and not of `forge::Field`
//
// `20-clip-forge.md` §8 says *the node array is the real representation; text and wires are two
// views of it*, and that is true of what a clip MEANS. It is not what an editor can edit. By the
// time a file has become a `Field`, one `let` has become a dozen nodes, every name has gone, the
// numbers have been folded together, and the comments never existed — so a visual edit made
// against it could not be written back without rewriting the whole file, which is exactly what the
// round-trip rule forbids. What an editor edits is the document: the statements the author wrote,
// the names they bound, and the numbers as they spelled them. Those lower to field nodes; the
// field nodes do not lift back.
//
// # What it knows about the language, and what it works out
//
// Almost all of this is syntax and nothing else: a statement is a head and everything up to the
// next line that starts one, `key=value` is a key, `{ }` holds children, and a bare word that
// names something the document already bound is a wire from it.
//
// Two small tables are the exception and both are copied from `src/forge/clip_script.cpp`'s own
// `call()`: which heads take a `{ }` of many children, and which take one child with the braces
// optional. They have to be here, because `union { a b } smooth=0.02` and
// `box -2 2 -0.4  2 3 0.4 grain` differ only in whether the head takes children — in the second,
// `grain` belongs to whatever encloses the box. A head this reader has never heard of is a leaf
// that takes numbers and keys, which is what every solid and every pattern is, so a new one added
// to the language draws correctly here without this file being touched. The one thing it loses is
// its wire's colour, which falls back to the value hue.
//
// ws_game rather than ws_forge, for the reason `markup.hpp` is here: it is a reader of a text
// format, ws_ui links ws_game and draws what it produces, and ws_ui does not link ws_forge.

#include <string>
#include <vector>

#include "core/types.hpp"

namespace ws {

// What a node carries, which is what colours the wire out of it.
//
// `14-ui-style.md` grants node-graph wires one of the five permitted colours: *three hues because
// the script language has three value types; a fourth hue would have to mean a fourth kind of
// value.* These are the three.
enum class ClipCarries : u8 {
    Shape = 0,      // a field read as a signed distance: every solid, and everything that combines
                    // or moves one
    Value = 1,      // a field read as an amount: every pattern, and the arithmetic over them
    Material = 2,   // a declared material, and the rules that paint with one
};

// One number in the document, and exactly where it is written.
//
// `text` is what the author typed, byte for byte, and it is what decides how a new value is
// spelled: somebody who wrote `0.18` gets two decimals back, and somebody who wrote `4` gets none.
// A slider that turned `4` into `4.000000` would have reformatted the document.
struct ClipNumber {
    std::string key;     // "round", "rgb", ... empty for a positional argument
    u32 index = 0;       // which of `rgb=124,120,112` this one is
    std::string text;
    f64 value = 0.0;
    u32 line = 0;        // 1-based, into the lines this was read from
    u32 column = 0;      // 0-based, in bytes, into that line
};

// A word that is not a number: `axis=y`, `where=grain`, an included file's name.
struct ClipWord {
    std::string key;      // empty for a positional word
    std::string text;
    u32 line = 0;
    u32 column = 0;
    bool names_a_part = false;   // it names something the document bound, so it is a wire
};

struct ClipNode {
    std::string head;     // "box", "union", "paint", "material", "include", ...
    // What the document called it: a `let` name, a material's name, the kind of a `weather`. Empty
    // for a sub-expression nobody named, which is most of them.
    std::string name;
    ClipCarries carries = ClipCarries::Value;
    // It is a whole statement rather than a sub-expression inside one. Statements are what a
    // document is a list of; the rest are the pieces those are built out of.
    bool statement = false;
    // D454: this is source the reader could not read, carried whole so that nothing is lost.
    bool opaque = false;
    std::string source;   // the lines themselves, for an opaque node

    // A bare name this document never bound and the language has no head for — which is what every
    // part of a world is, because a world is a manifest and its parts are declared in the files it
    // includes. It is not an error and it is not a mistake: it is a name from somewhere else, and
    // the only thing the document itself can say about it is who uses it.
    bool unresolved = false;

    // For an `include`, the file it names as it was written: `facility/_contract.clip`. `name` is
    // the last part of that, because a column of boxes all beginning `facility/` says the folder
    // eleven times and the file none.
    std::string target;

    std::vector<ClipNumber> numbers;
    std::vector<ClipWord> words;
    // Indices into `ClipGraph::nodes`, in the order they were written.
    std::vector<u32> inputs;

    u32 line = 0;         // where its head is, 1-based
    u32 last_line = 0;    // and the last line it covers
    u32 column = 0;       // where its head starts on that line

    // Worked out by the reader, once, so that every drawing of the graph agrees about the shape of
    // it: one further right than the furthest of its inputs.
    u32 depth = 0;

    // A key this node has, or nothing. For the few places a drawing wants one by name.
    const ClipWord* word(const std::string& key) const;
    const ClipNumber* number(const std::string& key) const;
};

struct ClipGraph {
    std::vector<ClipNode> nodes;
    // How many of the nodes are statements, which is the length of the document as a list.
    u32 statements = 0;
    // How many lines the reader could not read, which is what an opaque node is made of. Zero is
    // the normal case for a clip; a Lua mod on the mods shelf is one node and every line.
    u32 opaque_lines = 0;

    // A stable name for a node, so a selection survives the document being re-read on the next
    // keystroke. The `let` name where there is one, and "line:column" where there is not — neither
    // of which moves when a number on the line changes.
    static std::string key_of(const ClipNode& node);
    // Which node has that key, or `kNone`.
    u32 find(const std::string& key) const;
    static constexpr u32 kNone = 0xFFFFFFFFu;
};

// Read a document. Never fails: see the header above.
ClipGraph read_clip_graph(const std::vector<std::string>& lines);

// How many decimals the author wrote, capped at six. It is what a slider's STEP is made of: a
// `sides=6` steps by one and a `round=0.04` steps by a hundredth, and neither had to be told which
// — the document already said.
u32 clip_number_decimals(const std::string& as_written);

// How a value should be written back, in the spelling the author used: the same number of decimals
// they wrote, widened only where that spelling would lose the value, which is what a TYPED number
// does and a dragged one never does.
std::string spell_clip_number(f64 value, const std::string& as_written);

// Put `text` where `at` says the old one is. Returns false when the line no longer holds what the
// graph said it did, which is what a stale graph looks like and is never worth writing through.
//
// It is the ONLY way the visual view changes a document, and that is the whole of the round-trip
// rule: one span of one line is replaced and every other byte of the file is untouched.
bool write_clip_number(std::vector<std::string>& lines, const ClipNumber& at,
                       const std::string& text);

}  // namespace ws
