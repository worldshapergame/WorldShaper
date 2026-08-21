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

    u32 line = 0;         // where the STATEMENT starts, 1-based
    u32 last_line = 0;    // and the last line it covers
    u32 column = 0;       // where it starts on that line
    // And where its own WORD is, which for `let a = box ...` is the `box` rather than the `let`.
    //
    // The two differ on exactly the statements that bind a name, and they are the two different
    // questions anything that rewrites a document has to ask: *where does this statement begin*,
    // which is what a delete cuts from, and *where is the word that says what this is*, which is
    // what a type swap replaces and what a wrapper is inserted in front of. Answering the second
    // with the first put `cone` over `let` and wrapped a name in a `shell { }`.
    u32 word_line = 0;
    u32 word_column = 0;
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

    // Where the box that stands for the FILE was put, if anybody has moved it. A line of its own —
    // `#@doc x y` — because it belongs to no statement, and in the file rather than in the game
    // because §4 forbids the game keeping state about a file that the file does not carry (D445).
    bool doc_placed = false;
    f32 doc_x = 0.0f;
    f32 doc_y = 0.0f;

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
    // Which fold it lives under. A dozen rows in one list is a list a player reads rather than
    // uses, and the settings window has had the answer to that since D485: sections that fold.
    std::string group;
};

// What a word of the language is CALLED on the screen, where those differ.
//
// They differ in exactly one place so far and it is a real distinction rather than a synonym.
// `material` in the language sets the look of a VOXEL — its colour, its roughness, how it takes
// light — and that is the simple thing every clip has always used. What the word "material" is
// coming to mean in this game is the richer thing that is a graph of its own and can be painted
// with. Two things cannot share one word on the screen while one of them is being built, so the
// simple one is called what it is: **a voxel type**. The file keeps saying `material`, because a
// keyword is a promise to every clip ever written.
std::string clip_head_shown(const std::string& head);

// One line saying what a word of the language DOES, and the other words somebody might look for it
// under.
//
// *I can't find the coat node.* It is called `paint`, and *coat* is what every document in this
// repository calls it in prose — so the word a player has read is not the word the palette lists.
// Rather than rename a keyword, the palette is searchable and a word answers to more than its own
// spelling: `paint` answers to *coat*, `fbm` to *noise*, `union` to *add* and *join*.
struct ClipWordHelp {
    std::string about;                  // one line, for the tooltip
    std::vector<std::string> also;      // other things to look for it under
};
const ClipWordHelp& clip_head_help(const std::string& head);

// Whether this word is what somebody typing `text` is looking for. Matches the word, what it is
// called on screen, the other names it answers to, and its own description — in that order of
// obviousness, and anywhere in each rather than only at the start (the library's own rule).
bool clip_head_matches(const std::string& head, const std::string& text);

// Empty for a head that has no such table — which is most of them, and means "list what is
// written", exactly as before.
const std::vector<ClipProperty>& clip_properties_of(const std::string& head);

// --- SOCKETS: every place a wire can land, by name ------------------------------------------------
//
// **This is the rewrite.** Asked for directly: *rewrite the whole way in which nodes connect, make
// it work exactly like on the old version of the game.*
//
// What the old one did, and what this now does: **a node is a list of named, typed input sockets
// down its left side and one output on its right.** Every socket says what it is called and what
// kind of thing it takes; a socket with no wire shows the value it holds and can be typed into; a
// wire is dragged from an output dot and dropped on the input dot you mean. Nothing is guessed.
//
// What was there before guessed. A wire attached to a NODE and a heuristic decided which of its
// inputs the player had meant — so `where=` and `by=` and a child of a `{ }` block were all *a
// wire into that box*, and the only way to find out where one had gone was to read the script.
// That is the whole of *this entire thing is uncomprehensible*.
//
// The mapping onto the clip language is exact rather than invented, which is why it can be done at
// all without giving up the document:
//
//   a positional number      one socket, named by what that position means (`x0`, `r`, `sides`)
//   a `key=value`            one socket named `key`; `rgb=` is three, named `rgb red` and so on
//   a child of a `{ }` block one socket per child, plus an empty one at the end to drop into
//   a bare name             one socket, for the thing a statement names (`solid X`, `paint TYPE`)
//
// So a socket is a place in the TEXT as much as a place on the screen, and connecting one writes
// exactly the bytes a person would have typed.
struct ClipSocket {
    std::string name;               // what it is called on the node
    ClipCarries takes = ClipCarries::Value;

    // Where it lives in the document, which is what `connect_clip_socket` writes to.
    enum class Where : u8 {
        Number,   // a positional number or one of a key's numbers
        Key,      // a `key=` that is not there yet
        Child,    // one of a `{ }` block's children, or the empty one at the end
        Word,     // the bare name a statement takes: `solid X`, `paint TYPE`
    };
    Where where = Where::Number;
    std::string key;                // for Key and for a keyed Number
    u32 index = 0;                  // which number of that key, or which child

    // What is in it now. A socket holds a wire or a value, never both.
    bool linked = false;
    u32 from = 0;                   // the node the wire comes from, when `linked`
    bool implied = false;           // the language means it; the document does not say it (D796)

    // The constant, when there is one. Both are indices into the node's own arrays so that a
    // caller can write the bytes back — a pointer into a graph that is re-read every keystroke is
    // a pointer that is stale by the time it is used.
    bool has_number = false;
    u32 number_at = 0;              // into `node.numbers`
    bool has_word = false;
    u32 word_at = 0;                // into `node.words`
};

// Every socket a node has, in the order they are drawn: its bare name first, then its positional
// numbers, then its keys, then its children.
std::vector<ClipSocket> clip_sockets_of(const ClipGraph& graph, u32 node);

// Join `source`'s output to one socket of `target`. Empty on success, one line otherwise.
std::string connect_clip_socket(std::vector<std::string>& lines, const ClipGraph& graph, u32 target,
                                u32 socket, u32 source);
// And take whatever is in that socket back out.
std::string disconnect_clip_socket(std::vector<std::string>& lines, const ClipGraph& graph,
                                   u32 target, u32 socket);

// What a positional number is called on a node. `box`'s six are two corners; a `sphere`'s three are
// a place. Named rather than numbered because *the fourth number of a box* is not something anybody
// can hold in their head.
std::string clip_number_name(const ClipNode& node, u32 index, u32 count);

// --- what may be wired into what ----------------------------------------------------------------
//
// **A wire had no type at all, and that is a bug rather than a looseness.** Reported: *if i take a
// voxel type node and connect it to an union it doesnt connect but creates another node that is a
// clip with the same name and at the top says unknown shape or pattern.* Nothing was created — the
// name was written into `union { }`, the union read it as a shape it had never heard of, and the
// graph drew that unresolved name as a box of its own. Every one of those three surprises is the
// same missing check.
//
// So a wire is refused unless the thing on the end of it is a kind the target can read, and the
// refusal says what WOULD work. `why` is empty when it is allowed; otherwise it is one line for the
// player. `key` comes back holding the key the wire should be written under, which is how a coat
// reads a pattern through `where=` and a voxel type through neither.
std::string clip_may_join(const ClipNode& target, const ClipNode& source, std::string& key);

// --- and what to BUILD when the two do not fit but the intent is obvious --------------------------
//
// *Instead of when i connect a voxel type to a pattern it rejects it, make it understand that what i
// want is for that voxel type to be applied to that pattern.* Asked for directly, and it is right:
// a refusal is the correct answer to a wire that means nothing and the wrong one to a wire that
// means something the language spells with a third word.
//
// A voxel type and a pattern joined together mean *this type, where that pattern says* — which is a
// COAT, and a coat is a statement rather than a wire. So the join makes one. The same reasoning
// gives four more:
//
//   voxel type -> pattern, or pattern -> voxel type   a coat, `paint TYPE where=PATTERN`
//   voxel type -> shape, or shape -> voxel type       a coat of that type, and the shape is what
//                                                     the world is made of if nothing else is
//   pattern -> shape                                  a displacement, `displace { SHAPE } by=P`
//   shape -> shape                                    a union of the two
//   pattern -> pattern                                a blend of the two
//
// Every one of them writes what a person would have typed, so the script view is the explanation.
// `made` comes back holding the name of whatever was written, so the interface can choose it.
enum class ClipJoinKind : u8 { None, Coat, Displace, Union, Blend };

// What joining these two would MAKE, or `None` when nothing sensible would come of it. Asked before
// anything is written, so the interface can say what a drop is going to do before the hand lets go.
ClipJoinKind clip_join_makes(const ClipNode& a, const ClipNode& b);

// And does it. `from` and `to` are in the order the wire was dragged; the writer works out which is
// which. Empty on success, one line otherwise.
std::string join_clip_nodes(std::vector<std::string>& lines, const ClipGraph& graph, u32 from,
                            u32 to, f32 x, f32 y, std::string& made);

// --- a shape that is hollow, and a shape that is stretched --------------------------------------
//
// *Add a hollow parameter to the settings of shape nodes and parameters to stretch them.* Asked for
// directly, and the language already has both: `shell { ... } 0.05` makes a solid into a skin of
// that thickness, and `scale { ... } 2 1 2` stretches one. What it did not have was a way to ask
// for either from a node's own settings, which is where somebody who has just made a box looks.
//
// So they are OPERATIONS the interface offers as properties. A row that goes from nought wraps the
// statement's expression in the word the language uses; a row that goes back to nought takes the
// wrapper off again. The document ends up saying exactly what a person would have typed, which is
// the whole of why it is done this way rather than by inventing a `hollow=` key that only the
// editor understands.

// Which node in a statement's own chain has this head, or `kNone`. A chain is a node, the single
// thing it is made of, the single thing THAT is made of, and so on — which is what a stack of
// wrappers round one shape is.
u32 clip_wrapper_of(const ClipGraph& graph, u32 node, const std::string& head);

// Wraps a statement's whole expression in `head { ... } args`. The statement keeps its name, its
// comment and its `#@` marker: only the expression between them is enclosed.
bool wrap_clip_node(std::vector<std::string>& lines, const ClipGraph& graph, u32 node,
                    const std::string& head, const std::string& args);

// And takes one off: the wrapper named by `wrapper` goes, and what it was made of stays.
bool unwrap_clip_node(std::vector<std::string>& lines, const ClipGraph& graph, u32 wrapper);

// --- swapping one word of the language for another ----------------------------------------------
//
// *Make it so that nodes that should allow for it you can easily change their type, like switch a
// cube node for a cone node because its just a shape one, or a clip one for a different clip, by
// using a dropdown to list what they are instead of plain text.* Asked for directly.
//
// What a node CAN become is its own palette group and nothing else: a `box` can be a `cone` because
// both are shapes and take a place and a size, and a `box` cannot be a `union` because a union is
// made of things and a box is not. The group is the answer the palette already gives, so this is
// the palette read backwards rather than a second table to keep in step.
//
// Empty for a head that is in no group — `metre`, `solid`, `include` — which is how the interface
// knows not to offer the choice at all.
const std::vector<std::string>& clip_heads_like(const std::string& head);

// Writes a different head word over this node's. The numbers and keys after it are left exactly
// where they are: a `cone` reads `r=` and `h=` where a `cylinder` reads the same two, and where the
// new word does not want a key the reader ignores it rather than failing — which is the behaviour
// that makes this safe to offer at all.
bool write_clip_head(std::vector<std::string>& lines, const ClipNode& node,
                     const std::string& head);

// And the file an `include` names, in place, between the quotation marks that are already there.
bool write_clip_target(std::vector<std::string>& lines, const ClipNode& node,
                       const std::string& file);

// Takes a `key=...` back OUT of a statement, so it goes back to whatever the reader uses when the
// document is silent. That is what *put this back* means for a property whose default is a silence.
bool erase_clip_key(std::vector<std::string>& lines, const ClipNode& node, const std::string& key);

// What a word of the language is made WITH: the number in its palette template, which is the value
// a node has the moment it is added.
//
// This is where *put this back* gets its answer for a positional number. A `box` has six of them
// and none of them has a name, so there is nothing in the language that says what a box's default
// third number is — but the palette has to make a box that somebody can see, and the number it
// makes it with is exactly that answer. `has` is false for a head with no template, or a key the
// template does not carry.
bool clip_default_number(const std::string& head, const std::string& key, u32 index, f64& value);

// Writes `key=value` into a statement, replacing whatever that key held or adding it at the end of
// the statement's first line. Returns true when the document changed.
//
// Narrower than it looks, and deliberately: it touches one key and leaves every other byte of the
// line — the comments, the `#@` marker, the author's own spacing — exactly where it was, which is
// the same promise `write_clip_number` makes about one number (D746).
bool write_clip_key(std::vector<std::string>& lines, const ClipNode& node, const std::string& key,
                    const std::string& value);

// --- the clipboard, for boxes rather than for letters -------------------------------------------
//
// *Make ctrl-C, X and V also work on the visual node editor for selected nodes.* Asked for directly,
// and the answer is that a node on a clipboard is TEXT — the statements themselves, exactly as they
// are written, layout markers and all. That is what makes a node you copied out of one clip pasteable
// into another, into a text editor, and back; a private in-memory node format would be a clipboard
// only this window could read, which is not a clipboard (the same argument as `platform/window.hpp`'s).
std::string copy_clip_nodes(const std::vector<std::string>& lines, const ClipGraph& graph,
                            const std::vector<u32>& nodes);

// And back in. Names that collide with this document are renamed, and every reference AMONG the
// pasted statements follows the rename — a reference out of them is left pointing where it pointed,
// which reads as unresolved and is honest. `x`/`y` is where the top-left of what was copied lands;
// the rest keep their offsets from it, so a shape and the coat that paints it arrive in formation.
std::string paste_clip_nodes(std::vector<std::string>& lines, const ClipGraph& graph,
                             const std::string& text, f32 x, f32 y,
                             std::vector<std::string>& made);

// Where the document's own box sits, written as a line of its own at the top of the file. Replaces
// the line if one is already there, so moving it twice leaves one.
bool place_clip_document(std::vector<std::string>& lines, f32 x, f32 y);

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
