// The document read as a graph, which is what the editor's visual view draws.
//
// documentation/23-shell-and-libraries.md §5c, decisions D452–D454. Three rules are what these
// check, and each of them is a failure somebody would only find by losing work:
//
//   **nothing is dropped** — what the reader cannot read comes back whole, as source (D454);
//   **nothing is reformatted** — an edit changes the bytes of one number and no others;
//   **it never fails** — half of every word being typed is a syntax error (D453).

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
