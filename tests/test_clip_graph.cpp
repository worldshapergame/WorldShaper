// The document read as a graph, which is what the editor's visual view draws.
//
// documentation/23-shell-and-libraries.md §5c, decisions D452–D454. Three rules are what these
// check, and each of them is a failure somebody would only find by losing work:
//
//   **nothing is dropped** — what the reader cannot read comes back whole, as source (D454);
//   **nothing is reformatted** — an edit changes the bytes of one number and no others;
//   **it never fails** — half of every word being typed is a syntax error (D453).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/time.hpp"
#include "doctest/doctest.h"
#include "game/clip_graph.hpp"

using namespace ws;

namespace {

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> lines;
    std::string line;
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(line);
            line.clear();
        } else {
            line += c;
        }
    }
    lines.push_back(line);
    return lines;
}

std::string text_of(const std::vector<std::string>& lines) {
    std::string text;
    for (usize i = 0; i < lines.size(); ++i) {
        text += lines[i];
        if (i + 1 < lines.size()) text += "\n";
    }
    return text;
}

const ClipNode* named(const ClipGraph& graph, const std::string& name) {
    for (const ClipNode& node : graph.nodes) {
        if (node.name == name) return &node;
    }
    return nullptr;
}

// A whole clip, small enough to reason about and holding one of everything the reader has a rule
// for: a setting, a parameter, a material, a leaf, a wire, a nested call, a block and a coat.
constexpr const char* kClip =
    "# a comment, which is part of the document and not decoration around it\n"
    "metre 32\n"
    "bounds -6 0 -3   6 4 3\n"
    "param wall 0.12\n"
    "\n"
    "material stone rgb=124,120,112 rough=210\n"
    "\n"
    "let plinth = box -6 0 -3   6 0.3 3 round=0.04\n"
    "let grain  = fbm size=0.10 octaves=4 seed=3\n"
    "let slab   = displace { box -2 2 -0.4  2 3 0.4  grain } amount=0.04\n"
    "let hut    = shell { box 3 0.3 -2.6  5.4 1.9 -1 } thickness=wall\n"
    "let all    = union { plinth slab hut }\n"
    "\n"
    "paint stone\n"
    "paint stone where=grain above=0.55\n"
    "\n"
    "solid all\n";

}  // namespace

TEST_CASE("a clip reads as a graph, and a let names its expression rather than boxing it") {
    const ClipGraph graph = read_clip_graph(lines_of(kClip));
    CHECK(graph.opaque_lines == 0);

    // `let plinth = box ...` is ONE node called plinth, not a `let` box wired to a box box. A view
    // with a box for every keyword is a view of the grammar rather than of the clip.
    const ClipNode* plinth = named(graph, "plinth");
    REQUIRE(plinth != nullptr);
    CHECK(plinth->head == "box");
    CHECK(plinth->statement);
    CHECK(plinth->carries == ClipCarries::Shape);
    CHECK(plinth->inputs.empty());

    const ClipNode* grain = named(graph, "grain");
    REQUIRE(grain != nullptr);
    CHECK(grain->head == "fbm");
    CHECK(grain->carries == ClipCarries::Value);

    const ClipNode* stone = named(graph, "stone");
    REQUIRE(stone != nullptr);
    CHECK(stone->head == "material");
    CHECK(stone->carries == ClipCarries::Material);
}

TEST_CASE("a bare name is a wire and not a second copy of the thing") {
    const ClipGraph graph = read_clip_graph(lines_of(kClip));
    const ClipNode* all = named(graph, "all");
    REQUIRE(all != nullptr);
    CHECK(all->head == "union");
    REQUIRE(all->inputs.size() == 3);
    CHECK(graph.nodes[all->inputs[0]].name == "plinth");
    CHECK(graph.nodes[all->inputs[1]].name == "slab");
    CHECK(graph.nodes[all->inputs[2]].name == "hut");
    // And it is the SAME node, so moving the plinth moves what the union is made of.
    CHECK(&graph.nodes[all->inputs[0]] == named(graph, "plinth"));
}

TEST_CASE("a leaf inside a block does not swallow the name after it") {
    // `displace { box -2 2 -0.4  2 3 0.4  grain }` is a displace of TWO things. A reader that let
    // the box take `grain` as a child would draw a two-input node with one input, and the picture
    // would disagree with what the clip builds. This is the whole reason the two head tables are
    // copied into the reader.
    const ClipGraph graph = read_clip_graph(lines_of(kClip));
    const ClipNode* slab = named(graph, "slab");
    REQUIRE(slab != nullptr);
    CHECK(slab->head == "displace");
    REQUIRE(slab->inputs.size() == 2);
    CHECK(graph.nodes[slab->inputs[0]].head == "box");
    CHECK(graph.nodes[slab->inputs[0]].name.empty());   // a sub-expression nobody named
    CHECK(graph.nodes[slab->inputs[0]].numbers.size() == 6);
    CHECK(graph.nodes[slab->inputs[1]].name == "grain");
}

TEST_CASE("a value written as a parameter is a wire from the param, not a number of its own") {
    // `20-clip-forge.md` §3: numbers meant to move are SLOTS, and a node refers to one. Drawing
    // `thickness=wall` as a slider would offer to change one wall's thickness without the other's,
    // which is exactly what naming it was for.
    const ClipGraph graph = read_clip_graph(lines_of(kClip));
    const ClipNode* hut = named(graph, "hut");
    REQUIRE(hut != nullptr);
    CHECK(hut->head == "shell");
    const ClipWord* thickness = hut->word("thickness");
    REQUIRE(thickness != nullptr);
    CHECK(thickness->text == "wall");
    CHECK(thickness->names_a_part);
    CHECK(hut->number("thickness") == nullptr);
    bool wired_to_the_param = false;
    for (u32 input : hut->inputs) {
        if (graph.nodes[input].head == "param" && graph.nodes[input].name == "wall") {
            wired_to_the_param = true;
        }
    }
    CHECK(wired_to_the_param);
}

TEST_CASE("a paint rule is wired to its material and to what it tests") {
    const ClipGraph graph = read_clip_graph(lines_of(kClip));
    u32 coats = 0;
    for (const ClipNode& node : graph.nodes) {
        if (node.head == "paint") ++coats;
    }
    CHECK(coats == 2);
    const ClipNode* tested = nullptr;
    for (const ClipNode& node : graph.nodes) {
        if (node.head == "paint" && node.word("where") != nullptr) tested = &node;
    }
    REQUIRE(tested != nullptr);
    CHECK(tested->carries == ClipCarries::Material);
    CHECK(tested->word("where")->text == "grain");
    bool from_the_grain = false;
    for (u32 input : tested->inputs) {
        if (graph.nodes[input].name == "grain") from_the_grain = true;
    }
    CHECK(from_the_grain);
}

TEST_CASE("the graph is laid out left to right, and a name is further right than what it is made of") {
    const ClipGraph graph = read_clip_graph(lines_of(kClip));
    const ClipNode* plinth = named(graph, "plinth");
    const ClipNode* all = named(graph, "all");
    REQUIRE(plinth != nullptr);
    REQUIRE(all != nullptr);
    CHECK(plinth->depth == 0);
    CHECK(all->depth > plinth->depth);
    // And `solid all` is further right still, because it is what the whole document answers with.
    const ClipNode* solid = nullptr;
    for (const ClipNode& node : graph.nodes) {
        if (node.head == "solid") solid = &node;
    }
    REQUIRE(solid != nullptr);
    CHECK(solid->depth > all->depth);
}

TEST_CASE("an edit changes the bytes of one number and no others") {
    std::vector<std::string> lines = lines_of(kClip);
    const std::string before = text_of(lines);
    const ClipGraph graph = read_clip_graph(lines);
    const ClipNode* plinth = named(graph, "plinth");
    REQUIRE(plinth != nullptr);
    const ClipNumber* round = plinth->number("round");
    REQUIRE(round != nullptr);
    CHECK(round->text == "0.04");
    CHECK(round->value == doctest::Approx(0.04));

    REQUIRE(write_clip_number(lines, *round, spell_clip_number(0.09, round->text)));
    const std::string after = text_of(lines);
    CHECK(after != before);
    // Everything but that one span. The comment, the blank lines, the author's own column of
    // aligned `=` signs and the two spaces they left after `-3` are all still there, because a
    // player who cannot trust the text view after one visual edit will not use the visual view.
    std::string expected = before;
    const usize at = expected.find("round=0.04");
    REQUIRE(at != std::string::npos);
    expected.replace(at, std::string("round=0.04").size(), "round=0.09");
    CHECK(after == expected);
}

TEST_CASE("a rewrite refuses a span that no longer holds what the graph said") {
    std::vector<std::string> lines = lines_of(kClip);
    const ClipGraph graph = read_clip_graph(lines);
    const ClipNode* plinth = named(graph, "plinth");
    REQUIRE(plinth != nullptr);
    const ClipNumber* round = plinth->number("round");
    REQUIRE(round != nullptr);

    ClipNumber stale = *round;
    stale.line += 1;   // the same span, one line down, where something else is written
    CHECK_FALSE(write_clip_number(lines, stale, "0.09"));
    ClipNumber gone = *round;
    gone.line = 9999;
    CHECK_FALSE(write_clip_number(lines, gone, "0.09"));
    CHECK(text_of(lines) == std::string(kClip).substr(0, text_of(lines).size()));
}

TEST_CASE("a number is written back in the spelling the author used") {
    // The document's own precision, which is also what the slider's step is made of.
    CHECK(clip_number_decimals("0.18") == 2);
    CHECK(clip_number_decimals("6") == 0);
    CHECK(clip_number_decimals("-2.5") == 1);

    CHECK(spell_clip_number(0.23, "0.18") == "0.23");
    CHECK(spell_clip_number(7.0, "6") == "7");
    CHECK(spell_clip_number(-3.5, "-2.5") == "-3.5");
    // A value finer than the spelling is not rounded away — that is what TYPING one is for, and a
    // field that quietly gave back what the row already said would read as a field that ignores you.
    CHECK(spell_clip_number(0.045, "0.04") == "0.045");
    // And a negative nought is a nought.
    CHECK(spell_clip_number(-0.0, "0") == "0");
}

TEST_CASE("what the reader cannot read comes back whole, as source") {
    // D454. This is a Lua mod on the mods shelf: not a clip in any sense, and the visual view has
    // to be able to open it without deleting it.
    const char* lua =
        "-- a mod, which is not a clip\n"
        "local function tick(world)\n"
        "  world:set(0, 0, 0, \"stone\")\n"
        "end\n";
    const ClipGraph graph = read_clip_graph(lines_of(lua));
    CHECK(graph.nodes.size() >= 1);
    std::string carried;
    for (const ClipNode& node : graph.nodes) {
        CHECK(node.opaque);
        if (!carried.empty()) carried += "\n";
        carried += node.source;
    }
    // Every line of it, in order, byte for byte.
    for (const std::string& line : lines_of(lua)) {
        if (line.empty()) continue;
        CHECK(carried.find(line) != std::string::npos);
    }
    CHECK(graph.opaque_lines >= 4);
}

TEST_CASE("a document being typed into is read rather than refused") {
    // D453: a script that does not parse is not an error. Half of every word being typed is a
    // syntax error, so what matters is that the reader comes back at all and keeps what it had.
    const char* halfway =
        "metre 32\n"
        "let a = box 0 0 0 1 1 1\n"
        "let b = uni\n"
        "let c = union { a\n";
    const ClipGraph graph = read_clip_graph(lines_of(halfway));
    CHECK(named(graph, "a") != nullptr);
    CHECK(named(graph, "b") != nullptr);
    CHECK(named(graph, "c") != nullptr);
    // And the one that was half typed still names what it could see.
    const ClipNode* c = named(graph, "c");
    REQUIRE(c != nullptr);
    REQUIRE(c->inputs.size() == 1);
    CHECK(graph.nodes[c->inputs[0]].name == "a");
}

TEST_CASE("unbalanced braces do not run the reader off the end") {
    // `Parser::block` carries the same bound for the same reason: a file whose braces have
    // desynchronised nests one level for every `{` left in it, and this one is asked on every
    // keystroke by an editor.
    std::string wild = "metre 32\nlet a = box 0 0 0 1 1 1\nlet b = union ";
    for (u32 i = 0; i < 400; ++i) wild += "{ ";
    wild += "a\n";
    const ClipGraph graph = read_clip_graph(lines_of(wild));
    CHECK(named(graph, "a") != nullptr);
    CHECK(graph.nodes.size() < 500);
}

TEST_CASE("an include is a node saying which file, without its quotes") {
    const ClipGraph graph = read_clip_graph(lines_of(
        "# a world is often one line\n"
        "include \"facility.clip\"\n"));
    const ClipNode* include = nullptr;
    for (const ClipNode& node : graph.nodes) {
        if (node.head == "include") include = &node;
    }
    REQUIRE(include != nullptr);
    CHECK(include->name == "facility.clip");
    CHECK(graph.opaque_lines == 0);
    REQUIRE(include->words.size() == 1);
    CHECK(include->words[0].text == "facility.clip");
}

TEST_CASE("a transform carries whatever its child carried") {
    const ClipGraph graph = read_clip_graph(lines_of(
        "let stone = box 0 0 0 1 1 1\n"
        "let grain = fbm size=0.1\n"
        "let moved = translate { stone } 1 0 0\n"
        "let shifted = translate { grain } 1 0 0\n"));
    REQUIRE(named(graph, "moved") != nullptr);
    REQUIRE(named(graph, "shifted") != nullptr);
    CHECK(named(graph, "moved")->carries == ClipCarries::Shape);
    CHECK(named(graph, "shifted")->carries == ClipCarries::Value);
}

TEST_CASE("a node keeps its name through an edit on its own line") {
    // What a selection is held by. A key that moved when a slider moved would deselect the node
    // being dragged on the first frame of the drag.
    std::vector<std::string> lines = lines_of(kClip);
    const ClipGraph before = read_clip_graph(lines);
    const ClipNode* plinth = named(before, "plinth");
    REQUIRE(plinth != nullptr);
    const std::string key = ClipGraph::key_of(*plinth);
    const ClipNumber* round = plinth->number("round");
    REQUIRE(round != nullptr);
    REQUIRE(write_clip_number(lines, *round, "0.5"));

    const ClipGraph after = read_clip_graph(lines);
    const u32 found = after.find(key);
    REQUIRE(found != ClipGraph::kNone);
    CHECK(after.nodes[found].name == "plinth");
    CHECK(after.nodes[found].number("round")->value == doctest::Approx(0.5));

    // And a node with no name is held by where its head is written, which the edit did not move.
    const ClipNode* slab = named(before, "slab");
    REQUIRE(slab != nullptr);
    REQUIRE(!slab->inputs.empty());
    const std::string box_key = ClipGraph::key_of(before.nodes[slab->inputs[0]]);
    CHECK(box_key.front() == '@');
    CHECK(after.find(box_key) != ClipGraph::kNone);
}

TEST_CASE("a re-bound name is two nodes, and they are told apart") {
    // `20-clip-forge.md` §2 calls this *the form most authoring actually takes*, and
    // `clips/sampler.clip` uses it five times. Both nodes are called `slab`; a key that was the
    // name alone would find the first one for both, so choosing the displace in the visual view
    // would open the BOX's numbers — and a box's corners and a displace's amount are both plausible
    // in each other's panel, which is what makes it a fault nobody would see.
    const ClipGraph graph = read_clip_graph(lines_of(
        "let grain = fbm size=0.1\n"
        "let slab  = box 0 0 0 1 1 1\n"
        "let slab  = displace { slab grain } amount=0.04\n"));
    u32 called_slab = 0;
    for (const ClipNode& node : graph.nodes) {
        if (node.name == "slab") ++called_slab;
    }
    CHECK(called_slab == 2);

    std::vector<std::string> keys;
    for (const ClipNode& node : graph.nodes) keys.push_back(ClipGraph::key_of(node));
    for (usize i = 0; i < keys.size(); ++i) {
        for (usize j = i + 1; j < keys.size(); ++j) CHECK(keys[i] != keys[j]);
        CHECK(graph.find(keys[i]) == i);
    }
}

TEST_CASE("every clip the game ships reads as a graph with nothing left over") {
    // The gate that matters, and the one an argument cannot settle: the reader is not judged by a
    // clip written for it, it is judged by the ones that already exist. A statement head added to
    // the language and not to this reader turns a whole fragment into a text node — which is
    // honest (D454) and is still a picture nobody can edit, so it has to be findable here rather
    // than by opening the editor on the estate and seeing one grey box.
    const std::filesystem::path clips = std::filesystem::path(WS_ASSET_SOURCE_DIR) / ".." / "clips";
    std::error_code error;
    if (!std::filesystem::is_directory(clips, error)) return;   // a build from a zip has none

    u32 files = 0;
    u32 nodes = 0;
    std::string unread;
    for (const std::filesystem::directory_entry& item :
         std::filesystem::recursive_directory_iterator(clips, error)) {
        if (!item.is_regular_file(error) || item.path().extension() != ".clip") continue;
        std::ifstream in(item.path(), std::ios::binary);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
        const ClipGraph graph = read_clip_graph(lines);
        ++files;
        nodes += static_cast<u32>(graph.nodes.size());
        if (graph.opaque_lines != 0) {
            unread += "\n  " + item.path().filename().string() + ": " +
                      std::to_string(graph.opaque_lines) + " lines";
        }
    }
    CHECK(files > 0);
    CHECK(nodes > 0);
    INFO("clip files read: " << files << ", nodes: " << nodes);
    CHECK_MESSAGE(unread.empty(), "lines the graph could not read:" << unread);
}

TEST_CASE("reading the largest clip in the repository costs less than a frame") {
    // The editor re-reads the document on every keystroke, so this is a budget rather than a
    // curiosity: a read that costs a frame is a text view that stutters as it is typed into, and
    // `09-performance-budgets.md`'s rule is that exceeding a budget is a bug rather than a
    // trade-off. The bound is a whole frame at 60, which is far above what this should ever be —
    // what it is guarding against is an accidental quadratic, not a few microseconds.
    const std::filesystem::path clips = std::filesystem::path(WS_ASSET_SOURCE_DIR) / ".." / "clips";
    std::error_code error;
    if (!std::filesystem::is_directory(clips, error)) return;

    std::vector<std::string> biggest;
    std::string which;
    for (const std::filesystem::directory_entry& item :
         std::filesystem::recursive_directory_iterator(clips, error)) {
        if (!item.is_regular_file(error) || item.path().extension() != ".clip") continue;
        std::ifstream in(item.path(), std::ios::binary);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
        if (lines.size() > biggest.size()) {
            biggest = std::move(lines);
            which = item.path().filename().string();
        }
    }
    REQUIRE(!biggest.empty());

    const u64 began = now_ns();
    u32 nodes = 0;
    for (u32 round = 0; round < 10; ++round) {
        nodes = static_cast<u32>(read_clip_graph(biggest).nodes.size());
    }
    const f64 each_ms = static_cast<f64>(now_ns() - began) / 1.0e6 / 10.0;
    std::printf("\n--- the graph of the largest clip -------------------------------------------\n"
                "  %s: %zu lines, %u nodes, %.3f ms a read\n\n",
                which.c_str(), biggest.size(), nodes, each_ms);
    CHECK(each_ms < 16.6);
}


// --- the document as something you can CHANGE from the other view ------------------------------
//
// Every one of these is text surgery on the author's own file, so every one of them checks the
// bytes that did NOT move as carefully as the ones that did. A visual editor that reformats a
// document is one nobody uses twice.

namespace {

// A small document with one of each thing a wire can be attached to.
constexpr const char* kWirable =
    "# a document with room in it\n"
    "material stone rgb=124,120,112 rough=210\n"
    "let grain  = fbm size=0.10 octaves=4 seed=3\n"
    "let plinth = box -6 0 -3   6 0.3 3 round=0.04\n"
    "let post   = cylinder 0 1 0 r=0.2 h=2\n"
    "let all    = union { plinth }\n"
    "let thin   = shell post 0.1\n"
    "paint stone\n"
    "solid all\n";

usize count_of(const std::string& text, const std::string& what) {
    usize seen = 0;
    usize at = text.find(what);
    while (at != std::string::npos) {
        ++seen;
        at = text.find(what, at + 1);
    }
    return seen;
}

}  // namespace

TEST_CASE("a colour is given to every byte of a line, and grammar takes none") {
    const std::vector<ClipSpan> spans =
        colour_clip_line("let plinth = box -6 0 -3 round=0.04  # a comment");
    REQUIRE(!spans.empty());
    // In order, and never overlapping: a run that started before the one before it ended would draw
    // one word in two colours.
    for (usize i = 1; i < spans.size(); ++i) {
        CHECK(spans[i].column >= spans[i - 1].column + spans[i - 1].length);
    }
    const std::string line = "let plinth = box -6 0 -3 round=0.04  # a comment";
    const auto part_of = [&](const std::string& word) {
        const usize at = line.find(word);
        for (const ClipSpan& span : spans) {
            if (span.column == at && span.length == word.size()) return span.part;
        }
        return ClipPart::Grammar;
    };
    CHECK(part_of("let") == ClipPart::Name);        // it binds anything, so it is not a kind
    CHECK(part_of("plinth") == ClipPart::Name);
    CHECK(part_of("box") == ClipPart::Shape);
    CHECK(part_of("-6") == ClipPart::Value);
    CHECK(part_of("round") == ClipPart::Name);      // a key names WHICH argument, not a kind
    CHECK(part_of("0.04") == ClipPart::Value);
    CHECK(part_of("=") == ClipPart::Grammar);
    // And the comment runs to the end of the line, whatever is in it.
    CHECK(spans.back().part == ClipPart::Comment);
    CHECK(spans.back().column + spans.back().length == line.size());
}

TEST_CASE("a name that is not the language's is a name, whatever it is spelled like") {
    const std::vector<ClipSpan> spans = colour_clip_line("solid all");
    REQUIRE(spans.size() == 2);
    CHECK(spans[0].part == ClipPart::Shape);
    CHECK(spans[1].part == ClipPart::Name);
    CHECK(clip_head_known("union"));
    CHECK(clip_head_known("fbm"));
    CHECK(clip_head_known("paint"));
    CHECK_FALSE(clip_head_known("plinth"));
}

TEST_CASE("the script and the wires are coloured by the same three things") {
    // The decision that turns two colour schemes into one: a shape is one colour in the script and
    // the same colour on the wire out of it, so the legend over the graph explains both views.
    CHECK(clip_part_tint(ClipPart::Shape) == clip_carries_tint(ClipCarries::Shape));
    CHECK(clip_part_tint(ClipPart::Value) == clip_carries_tint(ClipCarries::Value));
    CHECK(clip_part_tint(ClipPart::Material) == clip_carries_tint(ClipCarries::Material));
    // And the three that are not a kind of thing take no colour at all.
    CHECK(clip_part_tint(ClipPart::Name) == kClipNoTint);
    CHECK(clip_part_tint(ClipPart::Grammar) == kClipNoTint);
    CHECK(clip_part_tint(ClipPart::Comment) == kClipNoTint);

    // A material line reads as a material, a pattern as a value, a solid as a shape.
    const std::vector<ClipSpan> material = colour_clip_line("material stone rgb=124,120,112");
    REQUIRE(material.size() > 2);
    CHECK(material[0].part == ClipPart::Material);
    CHECK(material[1].part == ClipPart::Name);       // the name it is called
    const std::vector<ClipSpan> grain = colour_clip_line("let g = fbm size=0.1");
    bool found_a_value_word = false;
    for (const ClipSpan& span : grain) {
        if (span.length == 3 && grain.size() > 2) {
            const std::string word = std::string("let g = fbm size=0.1").substr(span.column, 3);
            if (word == "fbm") {
                found_a_value_word = span.part == ClipPart::Value;
            }
        }
    }
    CHECK(found_a_value_word);
}

TEST_CASE("joining two things writes one name into the other's braces") {
    std::vector<std::string> lines = lines_of(kWirable);
    const ClipGraph before = read_clip_graph(lines);
    const ClipNode* all = named(before, "all");
    const ClipNode* post = named(before, "post");
    REQUIRE(all != nullptr);
    REQUIRE(post != nullptr);

    const std::string why =
        connect_clip_nodes(lines, before, before.find(ClipGraph::key_of(*post)),
                           before.find(ClipGraph::key_of(*all)));
    CHECK(why.empty());

    const ClipGraph after = read_clip_graph(lines);
    const ClipNode* joined = named(after, "all");
    REQUIRE(joined != nullptr);
    REQUIRE(joined->inputs.size() == 2);
    CHECK(after.nodes[joined->inputs[0]].name == "plinth");
    CHECK(after.nodes[joined->inputs[1]].name == "post");
    // And every other line is what it was.
    const std::vector<std::string> was = lines_of(kWirable);
    for (usize i = 0; i < was.size(); ++i) {
        if (was[i].find("union") != std::string::npos) continue;
        CHECK(lines[i] == was[i]);
    }
}

TEST_CASE("joining refuses a ring, and refuses a thing with no room for a wire") {
    std::vector<std::string> lines = lines_of(kWirable);
    const ClipGraph graph = read_clip_graph(lines);
    const u32 all = graph.find(ClipGraph::key_of(*named(graph, "all")));
    const u32 plinth = graph.find(ClipGraph::key_of(*named(graph, "plinth")));

    // `all` is already made of `plinth`, so putting `all` into `plinth` would close a ring — and a
    // ring is a document that cannot be built and a reader that would walk it for ever.
    const std::string ring = connect_clip_nodes(lines, graph, all, plinth);
    CHECK_FALSE(ring.empty());
    // A box is not made of anything, so there is nowhere for a wire to go.
    const u32 post = graph.find(ClipGraph::key_of(*named(graph, "post")));
    CHECK_FALSE(connect_clip_nodes(lines, graph, post, plinth).empty());
    // And nothing was written while it was being refused.
    CHECK(text_of(lines) == std::string(kWirable).substr(0, text_of(lines).size()));
}

TEST_CASE("a braceless one-child form gains its braces when a second thing is joined to it") {
    // `let thin = shell post 0.1` has a child and no braces, which is a form the language allows
    // and a form a wire cannot be inserted into. Wrapping it is what makes the room.
    std::vector<std::string> lines = lines_of(kWirable);
    const ClipGraph graph = read_clip_graph(lines);
    const u32 thin = graph.find(ClipGraph::key_of(*named(graph, "thin")));
    const u32 plinth = graph.find(ClipGraph::key_of(*named(graph, "plinth")));
    CHECK(connect_clip_nodes(lines, graph, plinth, thin).empty());

    const ClipGraph after = read_clip_graph(lines);
    const ClipNode* joined = named(after, "thin");
    REQUIRE(joined != nullptr);
    REQUIRE(joined->inputs.size() == 2);
    CHECK(after.nodes[joined->inputs[0]].name == "post");
    CHECK(after.nodes[joined->inputs[1]].name == "plinth");
    CHECK(joined->has_block);
}

TEST_CASE("a coat is joined through the key that reads a pattern") {
    std::vector<std::string> lines = lines_of(kWirable);
    const ClipGraph graph = read_clip_graph(lines);
    const u32 grain = graph.find(ClipGraph::key_of(*named(graph, "grain")));
    u32 coat = ClipGraph::kNone;
    for (usize i = 0; i < graph.nodes.size(); ++i) {
        if (graph.nodes[i].head == "paint") coat = static_cast<u32>(i);
    }
    REQUIRE(coat != ClipGraph::kNone);
    CHECK(connect_clip_nodes(lines, graph, grain, coat).empty());

    const ClipGraph after = read_clip_graph(lines);
    for (const ClipNode& node : after.nodes) {
        if (node.head != "paint") continue;
        REQUIRE(node.word("where") != nullptr);
        CHECK(node.word("where")->text == "grain");
    }
}

TEST_CASE("cutting a wire takes the name out and the space with it") {
    std::vector<std::string> lines = lines_of(kWirable);
    ClipGraph graph = read_clip_graph(lines);
    const u32 all = graph.find(ClipGraph::key_of(*named(graph, "all")));
    REQUIRE(connect_clip_nodes(lines, graph, graph.find(ClipGraph::key_of(*named(graph, "post"))),
                               all)
                .empty());
    graph = read_clip_graph(lines);
    const ClipNode* joined = named(graph, "all");
    REQUIRE(joined->links.size() == 2);

    CHECK(disconnect_clip_node(lines, graph, graph.find(ClipGraph::key_of(*joined)), 0).empty());
    const ClipGraph after = read_clip_graph(lines);
    const ClipNode* cut = named(after, "all");
    REQUIRE(cut != nullptr);
    REQUIRE(cut->inputs.size() == 1);
    CHECK(after.nodes[cut->inputs[0]].name == "post");
    // No double spaces left where the name was.
    for (const std::string& line : lines) CHECK(line.find("{  ") == std::string::npos);
}

TEST_CASE("a new node is written where its name is bound before anything reads it") {
    std::vector<std::string> lines = lines_of(kWirable);
    const ClipGraph graph = read_clip_graph(lines);
    std::string made;
    CHECK(add_clip_node(lines, graph, "sphere", 3.0f, 2.0f, made).empty());
    CHECK(made == "sphere_1");

    const ClipGraph after = read_clip_graph(lines);
    const ClipNode* fresh = named(after, "sphere_1");
    REQUIRE(fresh != nullptr);
    CHECK(fresh->head == "sphere");
    CHECK(fresh->placed);
    CHECK(fresh->at_x == doctest::Approx(3.0));
    CHECK(fresh->at_y == doctest::Approx(2.0));
    // Before every statement that could read it, which is the whole of why it is not simply
    // appended: a `let` after the `solid` that names it builds to nothing.
    const ClipNode* solid = nullptr;
    for (const ClipNode& node : after.nodes) {
        if (node.head == "solid") solid = &node;
    }
    REQUIRE(solid != nullptr);
    CHECK(fresh->line < solid->line);
    // And a second one gets a name of its own rather than the same one.
    std::string second;
    CHECK(add_clip_node(lines, after, "sphere", 0.0f, 0.0f, second).empty());
    CHECK(second == "sphere_2");
}

TEST_CASE("a node made from the palette is a shape somebody can see") {
    // A palette that makes invisible things is a palette a player presses once. Every entry has a
    // template, and every template names its head first.
    for (const ClipPaletteGroup& group : clip_palette()) {
        CHECK(!group.name.empty());
        CHECK(!group.heads.empty());
        for (const std::string& head : group.heads) {
            // Every entry has SOMETHING to write. What it writes is checked by the case below,
            // which puts each one into a document and reads it back — a stronger question than
            // "does the text begin with the word", and one that does not have to know which of them
            // are expressions and which are statements assembled by `add_clip_node`.
            CHECK_MESSAGE(!clip_node_template(head).empty(), "no template for " << head);
        }
    }
    CHECK(clip_node_template("nothing of that name").empty());
}

TEST_CASE("taking a node out is refused while something is still made of it") {
    std::vector<std::string> lines = lines_of(kWirable);
    const ClipGraph graph = read_clip_graph(lines);
    const u32 plinth = graph.find(ClipGraph::key_of(*named(graph, "plinth")));
    const std::string why = delete_clip_node(lines, graph, plinth);
    CHECK_FALSE(why.empty());
    CHECK(text_of(lines) == std::string(kWirable).substr(0, text_of(lines).size()));

    // `grain` is read by nothing, so it goes.
    const u32 grain = graph.find(ClipGraph::key_of(*named(graph, "grain")));
    CHECK(delete_clip_node(lines, graph, grain).empty());
    const ClipGraph after = read_clip_graph(lines);
    CHECK(named(after, "grain") == nullptr);
    CHECK(named(after, "plinth") != nullptr);
    CHECK(count_of(text_of(lines), "fbm") == 0);
}

TEST_CASE("where a node was dragged is written into the document and read back") {
    // `23-shell-and-libraries.md` §4: nothing in the game may keep state about a file that the file
    // does not carry. So a layout lives in the file, as a comment — which means a clip sent to
    // somebody else opens laid out the way it was left.
    std::vector<std::string> lines = lines_of(kWirable);
    ClipGraph graph = read_clip_graph(lines);
    const ClipNode* plinth = named(graph, "plinth");
    REQUIRE(plinth != nullptr);
    CHECK_FALSE(plinth->placed);
    CHECK(place_clip_node(lines, *plinth, 4.5f, -2.0f));

    graph = read_clip_graph(lines);
    const ClipNode* moved = named(graph, "plinth");
    REQUIRE(moved != nullptr);
    CHECK(moved->placed);
    CHECK(moved->at_x == doctest::Approx(4.5));
    CHECK(moved->at_y == doctest::Approx(-2.0));
    // It is a COMMENT, so the shape is untouched: the numbers are all still there and in order.
    CHECK(moved->numbers.size() == 7);
    CHECK(moved->number("round")->value == doctest::Approx(0.04));

    // Moved again, the marker is replaced rather than added to.
    CHECK(place_clip_node(lines, *moved, 1.0f, 1.0f));
    CHECK(count_of(lines[moved->line - 1], "#@") == 1);
}

TEST_CASE("an ordinary comment is not mistaken for a placement") {
    std::vector<std::string> lines =
        lines_of("let a = box 0 0 0 1 1 1   # see #@ the note below\nsolid a\n");
    const ClipGraph graph = read_clip_graph(lines);
    const ClipNode* a = named(graph, "a");
    REQUIRE(a != nullptr);
    CHECK_FALSE(a->placed);
}

TEST_CASE("where a box sits is not part of what the file builds") {
    // D462's rule, one class along: a built world is cached under a key hashed from its source, so
    // anything in the file that is not what the file BUILDS has to come out of that key. Without
    // this, dragging one box in the editor throws away a built world and the next launch re-samples
    // it region by region over minutes.
    std::vector<std::string> lines = lines_of(kWirable);
    const std::string plain = text_of(lines);
    ClipGraph graph = read_clip_graph(lines);
    REQUIRE(place_clip_node(lines, *named(graph, "plinth"), 3.0f, 4.0f));
    REQUIRE(place_clip_node(lines, *named(graph, "post"), -1.5f, 0.0f));
    CHECK(text_of(lines) != plain);
    CHECK(clip_without_layout(text_of(lines)) == plain);
    // And a document with no markers in it comes back byte for byte, without being rebuilt.
    CHECK(clip_without_layout(plain) == plain);
}

TEST_CASE("copying a set keeps the wiring among it and leaves the rest pointing where it did") {
    // The whole reason this is not a copy of some lines. Duplicating `plinth` and `all = union {
    // plinth }` has to give a second union made of the SECOND plinth — otherwise what comes back is
    // one new shape and one new name for the old one, and moving it moves the original.
    std::vector<std::string> lines = lines_of(kWirable);
    ClipGraph graph = read_clip_graph(lines);
    const u32 plinth = graph.find(ClipGraph::key_of(*named(graph, "plinth")));
    const u32 all = graph.find(ClipGraph::key_of(*named(graph, "all")));

    std::vector<std::string> made;
    CHECK(duplicate_clip_nodes(lines, graph, {plinth, all}, made).empty());
    REQUIRE(made.size() == 2);
    CHECK(made[0] == "plinth_2");
    CHECK(made[1] == "all_2");

    const ClipGraph after = read_clip_graph(lines);
    const ClipNode* copy = named(after, "all_2");
    REQUIRE(copy != nullptr);
    CHECK(copy->head == "union");
    REQUIRE(copy->inputs.size() == 1);
    CHECK(after.nodes[copy->inputs[0]].name == "plinth_2");
    // And the original is untouched — still made of the original.
    const ClipNode* was = named(after, "all");
    REQUIRE(was != nullptr);
    REQUIRE(was->inputs.size() == 1);
    CHECK(after.nodes[was->inputs[0]].name == "plinth");
    // The copies are placed where they can be seen rather than exactly under what they came from.
    CHECK(copy->placed);
}

TEST_CASE("a copy that points outside the set keeps pointing at the original") {
    std::vector<std::string> lines = lines_of(kWirable);
    const ClipGraph graph = read_clip_graph(lines);
    const u32 all = graph.find(ClipGraph::key_of(*named(graph, "all")));

    std::vector<std::string> made;
    CHECK(duplicate_clip_nodes(lines, graph, {all}, made).empty());
    const ClipGraph after = read_clip_graph(lines);
    const ClipNode* copy = named(after, "all_2");
    REQUIRE(copy != nullptr);
    REQUIRE(copy->inputs.size() == 1);
    CHECK(after.nodes[copy->inputs[0]].name == "plinth");   // the one that was already there
}

TEST_CASE("a coat is copied with the key that reads its pattern, not with half of it") {
    std::vector<std::string> lines = lines_of(
        "material stone rgb=1,2,3\n"
        "let grain = fbm size=0.1\n"
        "paint stone where=grain above=0.5\n"
        "let a = box 0 0 0 1 1 1\n"
        "solid a\n");
    const ClipGraph graph = read_clip_graph(lines);
    u32 coat = ClipGraph::kNone;
    u32 grain = ClipGraph::kNone;
    for (usize i = 0; i < graph.nodes.size(); ++i) {
        if (graph.nodes[i].head == "paint") coat = static_cast<u32>(i);
        if (graph.nodes[i].name == "grain") grain = static_cast<u32>(i);
    }
    REQUIRE(coat != ClipGraph::kNone);
    REQUIRE(grain != ClipGraph::kNone);

    std::vector<std::string> made;
    // The coat has no name of its own to rebind, so it is the GRAIN that is copied and the coat
    // that comes with it -- and the copied coat has to read the copied grain, key and all.
    CHECK(duplicate_clip_nodes(lines, graph, {grain}, made).empty());
    const ClipGraph after = read_clip_graph(lines);
    CHECK(named(after, "grain_2") != nullptr);
    // Nothing lost the `where=` it had.
    for (const ClipNode& node : after.nodes) {
        if (node.head != "paint") continue;
        REQUIRE(node.word("where") != nullptr);
    }
}

TEST_CASE("taking a whole group out is not the same question asked several times") {
    // `all` is made of `plinth`, so asking for `plinth` alone is refused. Asking for both is not:
    // the only thing that reads `plinth` is going with it.
    std::vector<std::string> lines = lines_of(kWirable);
    const ClipGraph graph = read_clip_graph(lines);
    const u32 plinth = graph.find(ClipGraph::key_of(*named(graph, "plinth")));
    const u32 all = graph.find(ClipGraph::key_of(*named(graph, "all")));
    const u32 solid_at = [&] {
        for (usize i = 0; i < graph.nodes.size(); ++i) {
            if (graph.nodes[i].head == "solid") return static_cast<u32>(i);
        }
        return ClipGraph::kNone;
    }();
    REQUIRE(solid_at != ClipGraph::kNone);

    CHECK_FALSE(delete_clip_nodes(lines, graph, {plinth}).empty());
    CHECK_FALSE(delete_clip_nodes(lines, graph, {plinth, all}).empty());   // `solid` still reads it
    CHECK(delete_clip_nodes(lines, graph, {plinth, all, solid_at}).empty());

    const ClipGraph after = read_clip_graph(lines);
    CHECK(named(after, "plinth") == nullptr);
    CHECK(named(after, "all") == nullptr);
    CHECK(named(after, "post") != nullptr);   // and nothing else went with them
}

TEST_CASE("everything in the palette can be made, and every group's heads are the language's") {
    // The palette offered a third of the vocabulary and the rest was reachable only by typing,
    // which teaches a player the language is smaller than it is.
    u32 heads = 0;
    for (const ClipPaletteGroup& group : clip_palette()) {
        for (const std::string& head : group.heads) {
            ++heads;
            CHECK_MESSAGE(!clip_node_template(head).empty(), "no template for " << head);
        }
    }
    CHECK(heads >= 75);

    // And every one of them written into an empty document reads as the node it says it is.
    for (const ClipPaletteGroup& group : clip_palette()) {
        for (const std::string& head : group.heads) {
            std::vector<std::string> lines = lines_of(
                "material stone rgb=1,2,3\nlet a = box 0 0 0 1 1 1\nsolid a\n");
            const ClipGraph graph = read_clip_graph(lines);
            std::string made;
            const std::string why = add_clip_node(lines, graph, head, 0.0f, 0.0f, made);
            CHECK_MESSAGE(why.empty(), head << ": " << why);
            const ClipGraph after = read_clip_graph(lines);
            CHECK_MESSAGE(after.opaque_lines == 0, head << " came back as text");
            bool found = false;
            for (const ClipNode& node : after.nodes) {
                if (node.head == head) found = true;
            }
            CHECK_MESSAGE(found, head << " is not in the document it was added to");
        }
    }
}

TEST_CASE("an explicit empty pair of braces is not a braceless child") {
    // `rotate { } x=0 y=0 z=0` -- which is what the palette writes for every one-child node it
    // makes. The one-child form falls back to `shell walls 0.1` when the block is empty, and
    // deciding that from "the block came back with nothing" took the fallback here too: `x` was
    // read as the child, the key was swallowed, and what was left of the line ran into the next
    // statement. Nothing in `clips/` writes an empty pair, which is why nothing found it until
    // something started writing them.
    const ClipGraph graph = read_clip_graph(lines_of(
        "let a = box 0 0 0 1 1 1\n"
        "let turned = rotate { } x=0 y=0.25 z=0\n"
        "solid a\n"));
    CHECK(graph.opaque_lines == 0);
    const ClipNode* turned = named(graph, "turned");
    REQUIRE(turned != nullptr);
    CHECK(turned->head == "rotate");
    CHECK(turned->inputs.empty());
    CHECK(turned->has_block);
    REQUIRE(turned->number("y") != nullptr);
    CHECK(turned->number("y")->value == doctest::Approx(0.25));
    // And the statement after it is still its own statement.
    bool has_solid = false;
    for (const ClipNode& node : graph.nodes) {
        if (node.head == "solid") has_solid = true;
    }
    CHECK(has_solid);
}

TEST_CASE("a whole document can be put inside another one") {
    // `include` is how a document is made of other documents, and every world the game ships is
    // one. The graph could not say it: reported as there being no node for adding a clip.
    std::vector<std::string> lines = lines_of(kWirable);
    ClipGraph graph = read_clip_graph(lines);
    CHECK(add_clip_include(lines, graph, "porch.clip", 2.0f, 1.0f).empty());

    graph = read_clip_graph(lines);
    const ClipNode* part = nullptr;
    for (const ClipNode& node : graph.nodes) {
        if (node.head == "include") part = &node;
    }
    REQUIRE(part != nullptr);
    CHECK(part->target == "porch.clip");
    CHECK(part->placed);
    CHECK(part->at_x == doctest::Approx(2.0));
    CHECK(part->at_y == doctest::Approx(1.0));
    // At the top, because an include brings names with it and a name has to be bound before
    // anything reads it.
    for (const ClipNode& node : graph.nodes) {
        if (node.head == "solid") CHECK(part->line < node.line);
    }

    // Twice is refused rather than written: two of the same include is the same file spliced in
    // twice, which is every name in it declared twice.
    CHECK_FALSE(add_clip_include(lines, graph, "porch.clip", 4.0f, 0.0f).empty());
    // And a quotation mark in the name would end the string the whole language then has to survive.
    CHECK_FALSE(add_clip_include(lines, graph, "por\"ch.clip", 4.0f, 0.0f).empty());
    CHECK_FALSE(add_clip_include(lines, graph, "", 4.0f, 0.0f).empty());
}

TEST_CASE("a material offers every property it can take, and one of them can be written in") {
    // Reported directly: *materials shouldn't just be material properties which btw it lacks a ton
    // of them, it should be more like IOR, metallic, emissive.* Every one of those was in the
    // language already; a panel that lists what is WRITTEN is a panel that says a material has as
    // many properties as the author happened to type.
    const std::vector<ClipProperty>& offers = clip_properties_of("material");
    CHECK(offers.size() > 8);
    const auto has = [&](const char* key) {
        for (const ClipProperty& offer : offers) {
            if (offer.key == key) return true;
        }
        return false;
    };
    CHECK(has("rgb"));
    CHECK(has("rough"));
    CHECK(has("metal"));
    CHECK(has("emit"));
    CHECK(has("ior"));
    CHECK(has("opacity"));
    CHECK(has("absorb"));
    // A shape has the numbers it has and no others, so it offers nothing and the panel lists what
    // is written, exactly as it always did.
    CHECK(clip_properties_of("box").empty());

    std::vector<std::string> lines =
        lines_of("material stone rgb=124,120,112 rough=210   #@ 1.0 2.0\nsolid nothing\n");
    ClipGraph graph = read_clip_graph(lines);
    const ClipNode* stone = named(graph, "stone");
    REQUIRE(stone != nullptr);

    // A key that is not there goes in at the end of the STATEMENT, before the comment that carries
    // the layout -- a key written after a `#` is a key written into a comment.
    REQUIRE(write_clip_key(lines, *stone, "metal", "180"));
    CHECK(lines[0].find("metal=180") < lines[0].find("#@"));
    graph = read_clip_graph(lines);
    stone = named(graph, "stone");
    REQUIRE(stone != nullptr);
    CHECK(stone->placed);
    CHECK(stone->at_x == doctest::Approx(1.0));
    REQUIRE(stone->number("metal") != nullptr);
    CHECK(stone->number("metal")->value == doctest::Approx(180.0));

    // A key that IS there is replaced, and nothing else on the line moves.
    REQUIRE(write_clip_key(lines, *stone, "rough", "40"));
    CHECK(lines[0].find("rough=40") != std::string::npos);
    CHECK(lines[0].find("rough=210") == std::string::npos);
    CHECK(lines[0].find("rgb=124,120,112") != std::string::npos);
    CHECK(count_of(lines[0], "#@") == 1);

    // And a key of several parts is written whole, because `rgb=124` is not a colour.
    REQUIRE(write_clip_key(lines, *stone, "absorb", "1,2,3"));
    graph = read_clip_graph(lines);
    stone = named(graph, "stone");
    REQUIRE(stone != nullptr);
    u32 parts = 0;
    f64 last = 0.0;
    for (const ClipNumber& number : stone->numbers) {
        if (number.key != "absorb") continue;
        ++parts;
        last = number.value;
    }
    CHECK(parts == 3);
    CHECK(last == doctest::Approx(3.0));
}

// --- the clipboard, for boxes ---------------------------------------------------------------------

TEST_CASE("nodes copy out as text and paste back in, renamed where they collide") {
    std::vector<std::string> lines = lines_of(kWirable);
    ClipGraph graph = read_clip_graph(lines);
    const u32 plinth = graph.find(ClipGraph::key_of(*named(graph, "plinth")));
    const u32 all = graph.find(ClipGraph::key_of(*named(graph, "all")));

    // What goes on the clipboard is the statements THEMSELVES, so it is something a person can
    // read, paste into a text editor, and paste back.
    const std::string text = copy_clip_nodes(lines, graph, {plinth, all});
    CHECK(text.find("plinth") != std::string::npos);
    CHECK(text.find("union") != std::string::npos);

    std::vector<std::string> made;
    CHECK(paste_clip_nodes(lines, graph, text, 3.0f, 1.0f, made).empty());
    REQUIRE(made.size() == 2);
    CHECK(made[0] == "plinth_2");
    CHECK(made[1] == "all_2");

    graph = read_clip_graph(lines);
    const ClipNode* copy = named(graph, "all_2");
    REQUIRE(copy != nullptr);
    // The copy is made of the COPIED plinth, not the original -- otherwise pasting gives one new
    // shape and one new name for the old one, and moving it moves what you copied from.
    REQUIRE(copy->inputs.size() == 1);
    CHECK(graph.nodes[copy->inputs.front()].name == "plinth_2");
    CHECK(copy->placed);

    // Into a document that has none of those names, the names come back exactly as they were.
    std::vector<std::string> empty = lines_of("metre 32\n");
    const ClipGraph blank = read_clip_graph(empty);
    std::vector<std::string> fresh;
    CHECK(paste_clip_nodes(empty, blank, text, 0.0f, 0.0f, fresh).empty());
    REQUIRE(fresh.size() == 2);
    CHECK(fresh[0] == "plinth");
    CHECK(fresh[1] == "all");

    // And nonsense on the clipboard is refused rather than written.
    std::vector<std::string> unchanged = lines_of(kWirable);
    const ClipGraph before = read_clip_graph(unchanged);
    std::vector<std::string> none;
    CHECK_FALSE(paste_clip_nodes(unchanged, before, "", 0.0f, 0.0f, none).empty());
    CHECK(text_of(unchanged) == std::string(kWirable));
}

TEST_CASE("where the document's own box sits is in the file and out of the cache key") {
    // It belongs to no statement, so it is a line of its own -- and a line of its own has to come
    // out of `clip_without_layout` WHOLE, or the document the world was built from and the document
    // on the disk stop hashing the same (D462).
    std::vector<std::string> lines = lines_of(kWirable);
    const std::string plain = text_of(lines);

    CHECK(place_clip_document(lines, 6.0f, 2.0f));
    ClipGraph graph = read_clip_graph(lines);
    CHECK(graph.doc_placed);
    CHECK(graph.doc_x == doctest::Approx(6.0));
    CHECK(graph.doc_y == doctest::Approx(2.0));
    CHECK(clip_without_layout(text_of(lines)) == plain);

    // Moved again, there is still one of them.
    CHECK(place_clip_document(lines, 1.0f, 1.0f));
    graph = read_clip_graph(lines);
    CHECK(graph.doc_x == doctest::Approx(1.0));
    CHECK(count_of(text_of(lines), "#@doc") == 1);
    CHECK(clip_without_layout(text_of(lines)) == plain);

    // A document that says nothing about it says nothing about it.
    const ClipGraph quiet = read_clip_graph(lines_of(kWirable));
    CHECK_FALSE(quiet.doc_placed);
}

// --- changing what a node IS ----------------------------------------------------------------------

TEST_CASE("a shape can be swapped for another shape, and a clip for another clip") {
    // *Nodes that should allow for it you can easily change their type, like switch a cube node for
    // a cone node because its just a shape one.* What a node can BECOME is its own palette group,
    // which is the answer the palette already gives to *which of these words are alike*.
    CHECK(clip_heads_like("box").size() > 4);
    CHECK(clip_heads_like("cone").size() > 4);
    // A group whose words are not interchangeable offers nothing: `param` beside `add` is not a
    // choice, it is a different statement.
    CHECK(clip_heads_like("param").empty());
    CHECK(clip_heads_like("solid").empty());
    CHECK(clip_heads_like("metre").empty());

    std::vector<std::string> lines =
        lines_of("let a = box 0 0 0  1 1 1 round=0.04   #@ 2.0 1.0\ninclude \"porch.clip\"\n");
    ClipGraph graph = read_clip_graph(lines);
    const ClipNode* a = named(graph, "a");
    REQUIRE(a != nullptr);

    // The numbers and keys after the word stay exactly where they are: a cone reads the same place
    // a box does, and a key the new word does not want is ignored rather than fatal.
    CHECK(write_clip_head(lines, *a, "cone"));
    CHECK(lines[0].find("cone 0 0 0  1 1 1 round=0.04") != std::string::npos);
    CHECK(count_of(lines[0], "#@") == 1);
    graph = read_clip_graph(lines);
    a = named(graph, "a");
    REQUIRE(a != nullptr);
    CHECK(a->head == "cone");
    CHECK(a->placed);

    // A graph read before an edit and used after one must not overwrite the wrong bytes: the span
    // is checked against what is actually there.
    ClipNode stale = *a;
    stale.word_column = 200;
    CHECK_FALSE(write_clip_head(lines, stale, "sphere"));
    stale = *a;
    stale.head = "not what is there";
    CHECK_FALSE(write_clip_head(lines, stale, "sphere"));

    // And the file an include names, in place.
    const ClipNode* door = nullptr;
    for (const ClipNode& node : graph.nodes) {
        if (node.head == "include") door = &node;
    }
    REQUIRE(door != nullptr);
    CHECK(write_clip_target(lines, *door, "terrace.wsclip"));
    CHECK(lines[1] == "include \"terrace.wsclip\"");
    CHECK_FALSE(write_clip_target(lines, *door, "has\"quote.clip"));
}

TEST_CASE("hollow and stretch are the words the language already has") {
    // *Add a hollow parameter to the settings of shape nodes and parameters to stretch them.* Both
    // exist as OPERATIONS -- `shell { } thickness=` and `scale { } x= y= z=` -- so a row that goes
    // off nought wraps the statement in the word a person would have typed, and a row that comes
    // back takes it off again. The document never learns a key only the editor understands.
    std::vector<std::string> lines = lines_of("let a = box 0 0 0  1 1 1   #@ 2.0 1.0\nsolid a\n");
    ClipGraph graph = read_clip_graph(lines);
    u32 a = graph.find(ClipGraph::key_of(*named(graph, "a")));

    CHECK(clip_wrapper_of(graph, a, "shell") == ClipGraph::kNone);
    REQUIRE(wrap_clip_node(lines, graph, a, "shell", "thickness=0.05"));
    CHECK(lines[0].find("shell { box 0 0 0  1 1 1 } thickness=0.05") != std::string::npos);
    // The name, the comment and the marker are outside the wrapper: only the expression is enclosed.
    CHECK(lines[0].compare(0, 8, "let a = ") == 0);
    CHECK(count_of(lines[0], "#@") == 1);

    graph = read_clip_graph(lines);
    a = graph.find(ClipGraph::key_of(*named(graph, "a")));
    const u32 skin = clip_wrapper_of(graph, a, "shell");
    REQUIRE(skin != ClipGraph::kNone);
    CHECK(graph.nodes[skin].head == "shell");
    REQUIRE(graph.nodes[skin].number("thickness") != nullptr);
    CHECK(graph.nodes[skin].number("thickness")->value == doctest::Approx(0.05));
    // And the shape is still in there, still a box, still where it was put.
    REQUIRE(graph.nodes[skin].inputs.size() == 1);
    CHECK(graph.nodes[graph.nodes[skin].inputs.front()].head == "box");
    CHECK(graph.nodes[skin].placed);

    // Off again, and the document is what it was.
    REQUIRE(unwrap_clip_node(lines, graph, skin));
    REQUIRE(named(read_clip_graph(lines), "a") != nullptr);
    CHECK(lines[0].find("shell") == std::string::npos);
    CHECK(lines[0].find("let a = box 0 0 0  1 1 1") != std::string::npos);
    CHECK(count_of(lines[0], "#@") == 1);
    graph = read_clip_graph(lines);
    CHECK(named(graph, "a")->head == "box");

    // Stretch is the same machinery with a different word.
    a = graph.find(ClipGraph::key_of(*named(graph, "a")));
    REQUIRE(wrap_clip_node(lines, graph, a, "scale", "x=2.00"));
    graph = read_clip_graph(lines);
    a = graph.find(ClipGraph::key_of(*named(graph, "a")));
    const u32 stretched = clip_wrapper_of(graph, a, "scale");
    REQUIRE(stretched != ClipGraph::kNone);
    REQUIRE(graph.nodes[stretched].number("x") != nullptr);
    CHECK(graph.nodes[stretched].number("x")->value == doctest::Approx(2.0));
}

TEST_CASE("a key can be taken back out of a statement") {
    std::vector<std::string> lines =
        lines_of("material stone rgb=124,120,112 rough=210 metal=40   #@ 1.0 2.0\n");
    ClipGraph graph = read_clip_graph(lines);
    const ClipNode* stone = named(graph, "stone");
    REQUIRE(stone != nullptr);

    // *Put this back* for a property whose default is a SILENCE means taking the key out.
    CHECK(erase_clip_key(lines, *stone, "rough"));
    CHECK(lines[0].find("rough=") == std::string::npos);
    // The space in front goes with it, so a line does not keep a gap where a key was.
    CHECK(lines[0].find("rgb=124,120,112 metal=40") != std::string::npos);
    CHECK(count_of(lines[0], "#@") == 1);
    // One that is not there is not an edit.
    CHECK_FALSE(erase_clip_key(lines, *stone, "rough"));
    CHECK_FALSE(erase_clip_key(lines, *stone, "sheen"));
}

TEST_CASE("what a new one is made with is what put-it-back puts back") {
    // Nothing in the language says what a box's third number ought to be. The palette has to make a
    // box somebody can SEE, and the number it makes it with is exactly that answer.
    f64 value = 0.0;
    CHECK(clip_default_number("box", "", 4, value));
    CHECK(value == doctest::Approx(1.0));
    CHECK(clip_default_number("sphere", "r", 0, value));
    CHECK(value == doctest::Approx(0.5));
    CHECK_FALSE(clip_default_number("box", "nonsense", 0, value));
    CHECK_FALSE(clip_default_number("not a word", "", 0, value));
}

// --- what may be wired into what ------------------------------------------------------------------

TEST_CASE("a wire is refused when the thing on the end of it is the wrong kind") {
    // Reported: *if i take a voxel type node and connect it to an union it doesnt connect but
    // creates another node that is a clip with the same name and at the top says unknown shape or
    // pattern.* Nothing was created -- the name went into `union { }`, the union read it as a shape
    // it had never heard of, and the graph drew the unresolved name as a box of its own. Three
    // surprises, one missing check.
    std::vector<std::string> lines = lines_of(
        "metre 32\n"
        "material stone rgb=170,166,158 rough=205\n"
        "let grain = fbm size=0.2 octaves=3 seed=1\n"
        "let a = box 0 0 0  1 1 1\n"
        "let b = box 2 0 0  3 1 1\n"
        "let all = union { a }\n"
        "paint stone\n"
        "solid all\n");
    ClipGraph graph = read_clip_graph(lines);
    const u32 stone = graph.find(ClipGraph::key_of(*named(graph, "stone")));
    const u32 grain = graph.find(ClipGraph::key_of(*named(graph, "grain")));
    const u32 b = graph.find(ClipGraph::key_of(*named(graph, "b")));
    const u32 all = graph.find(ClipGraph::key_of(*named(graph, "all")));
    // `solid` and `paint` bind no name of their own, so they are found by their word.
    u32 solid = ClipGraph::kNone;
    u32 coat = ClipGraph::kNone;
    for (usize i = 0; i < graph.nodes.size(); ++i) {
        if (graph.nodes[i].head == "solid") solid = static_cast<u32>(i);
        if (graph.nodes[i].head == "paint") coat = static_cast<u32>(i);
    }
    REQUIRE(solid != ClipGraph::kNone);
    REQUIRE(coat != ClipGraph::kNone);

    // A voxel type is not a shape, and the refusal says what WOULD work.
    const std::string why = connect_clip_nodes(lines, graph, stone, all);
    CHECK_FALSE(why.empty());
    CHECK(why.find("coat") != std::string::npos);
    CHECK(text_of(lines).find("union { a }") != std::string::npos);   // nothing was written

    // Nor is a pattern.
    CHECK_FALSE(connect_clip_nodes(lines, graph, grain, all).empty());
    CHECK_FALSE(connect_clip_nodes(lines, graph, stone, solid).empty());

    // A shape is.
    CHECK(connect_clip_nodes(lines, graph, b, all).empty());
    CHECK(text_of(lines).find("union { a b }") != std::string::npos);

    // And a coat takes BOTH kinds, each in its own place: the voxel type where the coat's own
    // material name is written, and the pattern under `where=`.
    graph = read_clip_graph(lines);
    CHECK(connect_clip_nodes(lines, graph, grain, coat).empty());
    CHECK(text_of(lines).find("paint stone where=grain") != std::string::npos);
}

TEST_CASE("what may be wired is asked before it is written, so the picture can say so too") {
    // The editor lights the boxes that could take the wire being dragged, which is the whole of
    // what makes a typed graph learnable rather than a thing you find out about by being refused.
    // It asks this, so this has to answer without touching the document.
    std::vector<std::string> lines = lines_of(
        "material stone rgb=1,2,3\n"
        "let grain = fbm size=0.2\n"
        "let a = box 0 0 0  1 1 1\n"
        "let all = union { a }\n"
        "paint stone\n"
        "weather cracks 0.5\n"
        "solid all\n");
    const ClipGraph graph = read_clip_graph(lines);
    const auto find = [&](const char* head) {
        for (const ClipNode& node : graph.nodes) {
            if (node.head == head) return node;
        }
        return ClipNode{};
    };
    const ClipNode shape = *named(graph, "a");
    const ClipNode type = *named(graph, "stone");
    const ClipNode pattern = *named(graph, "grain");
    std::string key;

    CHECK(clip_may_join(find("union"), shape, key).empty());
    CHECK(key.empty());
    CHECK_FALSE(clip_may_join(find("union"), type, key).empty());
    CHECK_FALSE(clip_may_join(find("union"), pattern, key).empty());

    CHECK(clip_may_join(find("paint"), type, key).empty());
    CHECK(key == "*");
    CHECK(clip_may_join(find("paint"), pattern, key).empty());
    CHECK(key == "where");
    CHECK_FALSE(clip_may_join(find("paint"), shape, key).empty());

    CHECK(clip_may_join(find("weather"), pattern, key).empty());
    CHECK(key == "on");
    CHECK_FALSE(clip_may_join(find("weather"), type, key).empty());

    CHECK(clip_may_join(find("solid"), shape, key).empty());
    CHECK_FALSE(clip_may_join(find("solid"), type, key).empty());
}

// --- what two things MEAN together ----------------------------------------------------------------

TEST_CASE("two things that cannot be wired but mean something together make it") {
    // *Instead of when i connect a voxel type to a pattern it rejects it, make it understand that
    // what i want is for that voxel type to be applied to that pattern.* A refusal is the right
    // answer to a wire that means nothing and the wrong one to a wire that means something the
    // language spells with a third word.
    std::vector<std::string> lines = lines_of(
        "metre 32\n"
        "material stone rgb=170,166,158\n"
        "let grain = fbm size=0.2 octaves=3 seed=1\n"
        "let rough = fbm size=0.6 octaves=2 seed=5\n"
        "let a = box 0 0 0  1 1 1\n"
        "let b = box 2 0 0  3 1 1\n"
        "solid a\n");
    ClipGraph graph = read_clip_graph(lines);
    const auto at = [&](const char* name) {
        return graph.find(ClipGraph::key_of(*named(graph, name)));
    };

    // A voxel type and a pattern is a COAT, and it reads the same both ways round.
    CHECK(clip_join_makes(*named(graph, "stone"), *named(graph, "grain")) == ClipJoinKind::Coat);
    CHECK(clip_join_makes(*named(graph, "grain"), *named(graph, "stone")) == ClipJoinKind::Coat);
    CHECK(clip_join_makes(*named(graph, "grain"), *named(graph, "a")) == ClipJoinKind::Displace);
    CHECK(clip_join_makes(*named(graph, "a"), *named(graph, "b")) == ClipJoinKind::Union);
    CHECK(clip_join_makes(*named(graph, "grain"), *named(graph, "rough")) == ClipJoinKind::Blend);

    std::string made;
    CHECK(join_clip_nodes(lines, graph, at("grain"), at("stone"), 2.0f, 1.0f, made).empty());
    CHECK(made == "stone");
    CHECK(text_of(lines).find("paint stone where=grain above=0.5") != std::string::npos);
    // A coat goes AFTER the voxel types and BEFORE the solid, because a name has to be bound before
    // it is read and a solid reads everything above it.
    ClipGraph after = read_clip_graph(lines);
    u32 coat_line = 0;
    u32 solid_line = 0;
    u32 type_line = 0;
    for (const ClipNode& node : after.nodes) {
        if (node.head == "paint") coat_line = node.line;
        if (node.head == "solid") solid_line = node.line;
        if (node.head == "material") type_line = node.line;
    }
    CHECK(type_line < coat_line);
    CHECK(coat_line < solid_line);

    // A pattern and a shape is a displacement, written the way a person would have typed it.
    graph = read_clip_graph(lines);
    CHECK(join_clip_nodes(lines, graph, at("grain"), at("a"), 3.0f, 0.0f, made).empty());
    CHECK(text_of(lines).find("displace { a } by=grain") != std::string::npos);

    // And two shapes are a union of both, not of one twice.
    graph = read_clip_graph(lines);
    CHECK(join_clip_nodes(lines, graph, at("a"), at("b"), 0.0f, 0.0f, made).empty());
    CHECK(text_of(lines).find("union { a b }") != std::string::npos);

    // Everything written parses back into a graph that says what it looks like.
    const ClipGraph again = read_clip_graph(lines);
    CHECK(again.nodes.size() > after.nodes.size());
    for (const ClipNode& node : again.nodes) {
        CHECK_FALSE(node.opaque);
    }
}

TEST_CASE("a word of the language can be found by what it does") {
    // *Add a search bar for the right click list for nodes, i cant find the coat node.* It is
    // called `paint`, and every document in this repository calls it a coat.
    CHECK(clip_head_matches("paint", "coat"));
    CHECK(clip_head_matches("paint", "paint"));
    CHECK(clip_head_matches("paint", "colour"));
    CHECK(clip_head_shown("paint") == "coat");
    CHECK(clip_head_shown("material") == "voxel type");

    // The vocabulary of every other voxel game, which is what a player arrives with.
    CHECK(clip_head_matches("union", "add"));
    CHECK(clip_head_matches("difference", "subtract"));
    CHECK(clip_head_matches("fbm", "noise"));
    CHECK(clip_head_matches("cells", "voronoi"));
    CHECK(clip_head_matches("shell", "hollow"));
    CHECK(clip_head_matches("scale", "stretch"));
    CHECK(clip_head_matches("material", "voxel type"));

    // And what it DOES, when the name is no help at all.
    CHECK(clip_head_matches("occlusion", "enclosed"));
    CHECK(clip_head_matches("plane", "ground"));

    // Case and separators do not matter; nonsense matches nothing; empty matches everything.
    CHECK(clip_head_matches("union", "ADD"));
    CHECK(clip_head_matches("cell_edge", "cell edge"));
    CHECK_FALSE(clip_head_matches("box", "trombone"));
    CHECK(clip_head_matches("box", ""));

    // Every word the palette offers has a line about it, or the search has nothing to read and the
    // tooltip has nothing to say.
    for (const ClipPaletteGroup& group : clip_palette()) {
        for (const std::string& head : group.heads) {
            INFO(head);
            CHECK_FALSE(clip_head_help(head).about.empty());
        }
    }
}
