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

// Where one of a node's inputs is written, so that a wire can be taken out again.
//
// A wire in the visual view is a NAME in the document — `union { plinth slab }` is two of them —
// and cutting one means erasing the name that made it. Which bytes to erase is not derivable from
// the node it points at, because the same node can be named from six places, so the span is
// recorded where the reference was read.
struct ClipLink {
    u32 from = 0;          // the node it comes from
    // It is written as a NAME. A sub-expression written inline — `displace { box 0 0 0 1 1 1 g }` —
    // is an input with nothing to erase, and it is cut by deleting the shape rather than the wire.
    bool named = false;
    std::string key;       // "where", "on", "thickness"; empty for a child of a `{ }` block
    u32 line = 0;          // where the whole reference starts: the KEY when there is one
    u32 column = 0;
    u32 length = 0;        // through to the end of the name
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

    // Where each input is written, in step with `inputs`. See ClipLink.
    std::vector<ClipLink> links;

    u32 line = 0;         // where its head is, 1-based
    u32 last_line = 0;    // and the last line it covers
    u32 column = 0;       // where its head starts on that line
    // And where the name it is BOUND to is written — the word after `let`, or after `material` or
    // `param`. A copy has to be given a name of its own, and that means knowing which bytes the old
    // one occupies rather than searching the line for a word that may appear in it twice.
    u32 name_line = 0;
    u32 name_column = 0;
    u32 name_length = 0;
    // And where it ENDS: one past its last token. What a hoist cuts and a delete removes.
    u32 end_line = 0;
    u32 end_column = 0;

    // Its `{ }`, when it has one — which is where a new wire is written. `has_block` is false for
    // a leaf and for the braceless form `shell walls 0.1`, and those are wired differently.
    bool has_block = false;
    u32 block_line = 0;      // the `{`
    u32 block_column = 0;
    u32 close_line = 0;      // and the `}`
    u32 close_column = 0;

    // Where the author DRAGGED it, if they did, in node widths and heights from the top left of
    // the graph. It lives in the document as a `#@ x y` comment on the node's own line, because
    // `23-shell-and-libraries.md` §4's rule is that nothing in the game may keep state about a file
    // that the file does not carry — so a clip you send somebody opens laid out the way you left
    // it, and there is no sidecar to lose.
    bool placed = false;
    f32 at_x = 0.0f;
    f32 at_y = 0.0f;

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

// --- one line, read for COLOUR ----------------------------------------------------------------
//
// `14-ui-style.md`'s fourth permitted colour: *command-line parts, using the same three rotations,
// for the same reason: several unlike things on one line, and telling them apart is the whole
// task.* A clip script is that argument at length, so a line of it is read into runs and each run
// is drawn in one of the three.
//
// **And the three are the SAME three the wires are**, which is a decision rather than a
// coincidence. The style document grants the two separately and only requires that both be
// rotations of the player's own ink; nothing forced them to agree about what each rotation *means*.
// Making them agree is what turns two colour schemes a player has to learn into one: green is a
// shape in the script and a shape on the wire, and the legend over the graph explains both.
//
// Everything that is not one of the three takes no hue at all — the braces and the equals signs
// because they are grammar rather than a kind of thing, the comments because they are not the
// document's meaning, and a NAME because it is the one thing on the line the author chose, and the
// ordinary ink is the strongest thing this interface has.
enum class ClipPart : u8 {
    Grammar = 0,    // `{ } = ,`: joining words, which take no hue
    Comment = 1,    // to the end of the line
    Name = 2,       // what something is called, and the keyword that binds it
    Shape = 3,      // a word that makes or moves matter: box, union, translate, solid
    Value = 4,      // a word that makes an amount, and every number
    Material = 5,   // material, paint, weather
};

// Which of `ui::tint_of`'s three a part is drawn in, or `kClipNoTint` for the ordinary ink.
inline constexpr u32 kClipNoTint = 0xFFFFFFFFu;
inline constexpr u32 clip_part_tint(ClipPart part) {
    switch (part) {
        case ClipPart::Shape: return 0;
        case ClipPart::Value: return 1;
        case ClipPart::Material: return 2;
        default: return kClipNoTint;
    }
}

// And what a wire carries, in the same three. One table, so a wire and the word it is named after
// cannot come out different colours.
inline constexpr u32 clip_carries_tint(ClipCarries carries) {
    switch (carries) {
        case ClipCarries::Shape: return 0;
        case ClipCarries::Value: return 1;
        default: return 2;
    }
}

struct ClipSpan {
    u32 column = 0;
    u32 length = 0;
    ClipPart part = ClipPart::Grammar;
};

// The runs of one line, in order, covering every byte of it. Cheap: one pass, no allocation beyond
// the vector, and no knowledge of any line but this one — which is what lets the script view colour
// only the lines it is about to draw.
std::vector<ClipSpan> colour_clip_line(const std::string& line);

// Whether the language knows this word. The vocabulary of `20-clip-forge.md` §2, and the one thing
// the colouring needs that the syntax cannot tell it: `box` is a verb and `plinth` is a name, and
// nothing about the two words says which.
bool clip_head_known(const std::string& word);

// --- changing the document --------------------------------------------------------------------
//
// Every one of these returns an empty string when it worked and ONE LINE saying why not when it
// did not, which is `Library`'s convention and for the same reason: that line is what the interface
// has room to say, and a refusal that does not explain itself is indistinguishable from a bug.
//
// They all edit `lines` in place and touch nothing they were not asked to. The graph is re-read
// afterwards by the caller — none of them try to keep it in step, because a half-updated graph is
// a graph that disagrees with the document it came from.

// Where the author put it. Written as a `#@ x y` comment on the node's own line.
bool place_clip_node(std::vector<std::string>& lines, const ClipNode& node, f32 x, f32 y);

// The same text with every layout marker taken out of it.
//
// This exists for exactly one caller and it is worth saying why, because `ui::without_author` was
// written for the same reason and the reason is D462. **A built world is cached under a key hashed
// from the source that produced it**, so anything written into a file that is not part of what the
// file BUILDS throws away a cache that is still perfectly good — and D462 is the record of what
// that costs: every world on the shelf rebuilt from cold, coarse first, then re-sampled region by
// region over minutes, which from inside is a world made of blocks slowly resolving and looks
// exactly like the streaming being broken.
//
// Where a box sits is not part of what a file builds. Neither is who made it.
std::string clip_without_layout(const std::string& text);

// Wire `from` into `to`. The name of `from` is written into `to`'s `{ }`, or into the key that
// takes one, or over the expression a `solid` names.
std::string connect_clip_nodes(std::vector<std::string>& lines, const ClipGraph& graph, u32 from,
                               u32 to);

// Cut one of `to`'s wires: `which` indexes `to`'s `links`.
std::string disconnect_clip_node(std::vector<std::string>& lines, const ClipGraph& graph, u32 to,
                                 u32 which);

// A new statement, from the palette. `made` comes back holding the name it was given.
// --- what a statement can be given, whether or not the document gives it ------------------------
//
// A node's panel showed the numbers that are WRITTEN, which is right for a shape — a `box` has six
// and there are no others — and wrong for a material, where the document writes three of a dozen
// and the rest take their usual value silently. Reported directly: *materials shouldn't just be
// material properties which btw it lacks a ton of them, it should be more like IOR, metallic,
// emissive.* They were all there in the language and none of them were on the screen.
//
// So a head can declare what it offers, and the panel lists all of it: a property the document
// writes is the number the document wrote, and one it does not is its usual value with a slider on
// it that writes the key in the first time it is moved.
struct ClipProperty {
    std::string key;
    u32 parts = 1;        // `rgb` is three numbers under one key
    f64 fallback = 0.0;   // what the reader uses when the document is silent
    f64 low = 0.0;
    f64 high = 1.0;
    u32 decimals = 0;
    std::string about;    // one line, for the tooltip
};

// Empty for a head that has no such table — which is most of them, and means "list what is
// written", exactly as before.
const std::vector<ClipProperty>& clip_properties_of(const std::string& head);

// Writes `key=value` into a statement, replacing whatever that key held or adding it at the end of
// the statement's first line. Returns true when the document changed.
//
// Narrower than it looks, and deliberately: it touches one key and leaves every other byte of the
// line — the comments, the `#@` marker, the author's own spacing — exactly where it was, which is
// the same promise `write_clip_number` makes about one number (D746).
bool write_clip_key(std::vector<std::string>& lines, const ClipNode& node, const std::string& key,
                    const std::string& value);

// `include "some.clip"`, written in at the top with everything else the document reads from
// elsewhere — and with the same `#@` marker, so a part dropped on the canvas stays where it was
// dropped.
//
// Separate from `add_clip_node` because an include is not a word out of the palette: it names a
// FILE, and which file is a question only the interface can answer. Reported directly — *there is
// no node for adding a clip into the editor either an empty one or one from your clip library.*
std::string add_clip_include(std::vector<std::string>& lines, const ClipGraph& graph,
                             const std::string& file, f32 x, f32 y);

std::string add_clip_node(std::vector<std::string>& lines, const ClipGraph& graph,
                          const std::string& head, f32 x, f32 y, std::string& made);

// Take a statement out. Refused while anything is still made of it, because a document with a
// dangling name in it is one the player has to repair by hand.
std::string delete_clip_node(std::vector<std::string>& lines, const ClipGraph& graph, u32 node);

// Take a SET out, which is not the same question asked several times: a thing used only by others
// in the same set is going with them, so it is not "still made of" anything that will survive.
// Refused only when something OUTSIDE the set still reads one of them.
std::string delete_clip_nodes(std::vector<std::string>& lines, const ClipGraph& graph,
                              const std::vector<u32>& nodes);

// Copy a set of statements, keeping the wiring AMONG them.
//
// That is the whole of why this is not a copy of some lines. Duplicating `let a` and `let b = union
// { a }` has to give a second `b` made of the second `a` — otherwise what comes back is one new
// shape and one new name for the old one, and moving it moves the original. Anything referenced
// from outside the set keeps pointing at the original, which is the other half of the same rule.
//
// `made` comes back holding the new names, in the order they were written.
std::string duplicate_clip_nodes(std::vector<std::string>& lines, const ClipGraph& graph,
                                 const std::vector<u32>& nodes, std::vector<std::string>& made);

// What a new node of this head is written as, before its name. Empty for a head with no template,
// which is what the palette is built from.
std::string clip_node_template(const std::string& head);

// The palette the visual view offers, grouped. One list, so the menu and the templates cannot
// disagree about what can be made.
struct ClipPaletteGroup {
    std::string name;
    std::vector<std::string> heads;
};
const std::vector<ClipPaletteGroup>& clip_palette();

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
