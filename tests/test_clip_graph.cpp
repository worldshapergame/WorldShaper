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
            const std::string body = clip_node_template(head);
            CHECK_MESSAGE(!body.empty(), "no template for " << head);
            // A `let` names its head first. `material`, `param` and `paint` are statements rather
            // than expressions and carry only what goes AFTER their own name, which is why they
            // are assembled by `add_clip_node` and not written out whole here.
            if (group.name != "document") CHECK(body.compare(0, head.size(), head) == 0);
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
