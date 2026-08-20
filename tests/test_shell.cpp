// The editor: opening a document, and the two views of it.
//
// documentation/23-shell-and-libraries.md §5c, decisions D452–D456. `Shell` needs no device — it
// produces a list of marks and somebody else draws it — so the whole of this runs headless, which
// is the only reason the visual view has a gate at all: it is a screen, and a screen is otherwise
// something only a person looking at it can check.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "doctest/doctest.h"
#include "ui/shell.hpp"

using namespace ws;
using namespace ws::ui;

namespace {

struct Scratch {
    std::filesystem::path root;
    explicit Scratch(const char* name) {
        root = std::filesystem::temp_directory_path() / "worldshaper-tests" / name;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
    }
    ~Scratch() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

std::filesystem::path write_file(const std::filesystem::path& at, const std::string& text) {
    std::filesystem::create_directories(at.parent_path());
    std::ofstream out(at, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return at;
}

constexpr const char* kClip =
    "# a clip with one of everything the editor draws\n"
    "metre 32\n"
    "param wall 0.12\n"
    "material stone rgb=124,120,112 rough=210\n"
    "let plinth = box -6 0 -3   6 0.3 3 round=0.04\n"
    "let grain  = fbm size=0.10 octaves=4 seed=3\n"
    "let all    = union { plinth }\n"
    "paint stone where=grain above=0.55\n"
    "solid all\n";

// One frame of the shell with nobody touching anything. Enough to run every layout and hit test in
// the editor over a real document, which is what a crash in the visual view would look like.
Verdict quiet_frame(Shell& shell, f64 seconds) {
    InputState input{};
    input.mouse_x = -1000.0f;
    input.mouse_y = -1000.0f;
    return shell.frame(input, 1280, 800, seconds);
}

}  // namespace

TEST_CASE("the editor opens a document, and both views draw it") {
    Scratch scratch("shell-editor");
    const std::filesystem::path clip = write_file(scratch.root / "clips" / "one.wsclip", kClip);

    Shell shell;
    shell.load(scratch.root, scratch.root / "game");
    shell.open_editor(clip);
    CHECK(shell.editing() == clip);

    // A node the document bound, found by the name the author gave it. This is the whole of the
    // graph plumbing — read the text, lay it out, key every node — asked as one question.
    CHECK(shell.choose_node("plinth"));
    CHECK(shell.choose_node("grain"));
    CHECK(shell.choose_node("all"));
    CHECK_FALSE(shell.choose_node("nothing of that name"));

    CHECK(shell.open_editor_view("script"));
    CHECK_FALSE(quiet_frame(shell, 0.0).quit);
    CHECK(shell.open_editor_view("visual"));
    CHECK_FALSE(quiet_frame(shell, 0.1).quit);
    CHECK_FALSE(shell.open_editor_view("wires please"));

    // And with a node chosen, which is what puts its parameters on the LEFT — a second panel, a
    // second layout, and the one that reads the numbers out of the document.
    CHECK(shell.choose_node("plinth"));
    CHECK_FALSE(quiet_frame(shell, 0.2).quit);
    CHECK_FALSE(shell.ui().draw().empty());
}

TEST_CASE("a document that is not a clip opens rather than being refused") {
    // D453 and D454 together, at the level a player meets them: the mods shelf holds Lua, the graph
    // cannot read a line of it, and opening it must give them their file rather than an empty view.
    Scratch scratch("shell-editor-lua");
    const std::filesystem::path lua = write_file(
        scratch.root / "mods" / "thing.wslua",
        "-- a mod\nlocal function tick(world)\n  world:set(0, 0, 0, \"stone\")\nend\n");

    Shell shell;
    shell.load(scratch.root, scratch.root / "game");
    shell.open_editor(lua);
    CHECK(shell.editing() == lua);
    CHECK(shell.open_editor_view("visual"));
    CHECK_FALSE(quiet_frame(shell, 0.0).quit);
    CHECK_FALSE(shell.ui().draw().empty());
}

TEST_CASE("a world of nothing but includes opens, and the editor draws it") {
    // The case the editor could not reach at all: *open* on the worlds shelf enters the world, so
    // the one file kind the editor never saw was the one the game itself makes when a player
    // presses *new* — and that file is `include "facility.clip"`.
    Scratch scratch("shell-editor-world");
    write_file(scratch.root / "worlds" / "parts" / "wall.clip",
               "let wall = box 0 0 0 1 2 0.2\n");
    const std::filesystem::path world =
        write_file(scratch.root / "worlds" / "a.wsworld",
                   "# a world is a manifest\ninclude \"parts/wall.clip\"\nsolid wall\n");

    Shell shell;
    shell.load(scratch.root, scratch.root / "game");
    shell.open_editor(world);
    CHECK(shell.editing() == world);
    CHECK(shell.open_editor_view("visual"));
    CHECK_FALSE(quiet_frame(shell, 0.0).quit);
    CHECK_FALSE(shell.ui().draw().empty());
}

TEST_CASE("the editor is empty until something is opened, and says so rather than drawing nothing") {
    Scratch scratch("shell-editor-empty");
    Shell shell;
    shell.load(scratch.root, scratch.root / "game");
    CHECK(shell.editing().empty());
    shell.open_window("worlds", true);
    CHECK_FALSE(quiet_frame(shell, 0.0).quit);
    CHECK_FALSE(shell.ui().draw().empty());
}

TEST_CASE("the visual view changes the document, and the file says so") {
    // The whole of D757 asked as one question: a node added from the palette is a line in the
    // author's file, the document is dirty, and everything else in it is untouched.
    Scratch scratch("shell-editor-add");
    const std::filesystem::path clip = write_file(scratch.root / "clips" / "grow.wsclip", kClip);

    Shell shell;
    shell.load(scratch.root, scratch.root / "game");
    shell.open_editor(clip);
    CHECK(shell.open_editor_view("visual"));
    CHECK_FALSE(quiet_frame(shell, 0.0).quit);

    CHECK(shell.add_node("sphere").empty());
    CHECK(shell.choose_node("sphere_1"));
    CHECK_FALSE(quiet_frame(shell, 0.1).quit);

    // It is not on disk until it is saved, which is what the star beside the name means.
    std::ifstream on_disk(clip, std::ios::binary);
    const std::string still((std::istreambuf_iterator<char>(on_disk)),
                            std::istreambuf_iterator<char>());
    CHECK(still.find("sphere_1") == std::string::npos);

    // A head the palette does not have is refused rather than written.
    CHECK_FALSE(shell.add_node("nothing of that name").empty());
}

TEST_CASE("a node the palette cannot place says so instead of writing a line that cannot parse") {
    // `paint` has to paint with something. A document with no material in it has nothing for it to
    // use, and a coat naming a material that does not exist is a document that stops building.
    Scratch scratch("shell-editor-paint");
    const std::filesystem::path clip = write_file(scratch.root / "clips" / "bare.wsclip",
                                                  "let a = box 0 0 0 1 1 1\nsolid a\n");
    Shell shell;
    shell.load(scratch.root, scratch.root / "game");
    shell.open_editor(clip);
    CHECK_FALSE(shell.add_node("paint").empty());
    CHECK(shell.add_node("material").empty());
    CHECK(shell.add_node("paint").empty());
    CHECK_FALSE(quiet_frame(shell, 0.0).quit);
}

// --- what is chosen ---------------------------------------------------------------------------
//
// The script view had no selection at all: no double-click, no drag, no ctrl-A, and nothing for
// ctrl-C to take. Reported directly. The gate is end to end — put a selection in with keys, replace
// it by typing, save, and read the file back off the disk — because the pieces in between are
// private and a test of them would be a test of this file's own arithmetic.
namespace {

InputState quiet_input() {
    InputState input{};
    input.mouse_x = -1000.0f;
    input.mouse_y = -1000.0f;
    return input;
}

InputState with_ctrl(Key key) {
    InputState input = quiet_input();
    input.down[static_cast<usize>(Key::Ctrl)] = true;
    input.down[static_cast<usize>(key)] = true;
    input.pressed[static_cast<usize>(key)] = true;
    return input;
}

std::string file_text(const std::filesystem::path& path) {
    std::ifstream back(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(back)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("ctrl-A chooses the document, and the next key replaces it") {
    Scratch scratch("shell-choose-all");
    const std::filesystem::path clip = write_file(scratch.root / "clips" / "pick.wsclip", kClip);

    Shell shell;
    shell.load(scratch.root, scratch.root / "game");
    shell.open_editor(clip);
    REQUIRE(shell.open_editor_view("script"));
    quiet_frame(shell, 0.0);

    shell.frame(with_ctrl(Key::A), 1280, 800, 0.1);

    InputState typed = quiet_input();
    typed.typed = "x";
    shell.frame(typed, 1280, 800, 0.2);

    shell.frame(with_ctrl(Key::S), 1280, 800, 0.3);
    // The newline is the file's, not the editor's: a document that ended with one still does.
    CHECK(file_text(clip) == "x\n");
}

TEST_CASE("shift and an arrow drag a selection out, and it is what gets replaced") {
    Scratch scratch("shell-choose-run");
    const std::filesystem::path clip = write_file(scratch.root / "clips" / "run.wsclip", kClip);

    Shell shell;
    shell.load(scratch.root, scratch.root / "game");
    shell.open_editor(clip);
    REQUIRE(shell.open_editor_view("script"));
    quiet_frame(shell, 0.0);

    // The caret opens at the top left. Three held-shift steps take the first three characters of
    // the first line with it, and one typed character stands in for all three.
    f64 at = 0.1;
    for (u32 step = 0; step < 3; ++step) {
        InputState right = quiet_input();
        right.down[static_cast<usize>(Key::Shift)] = true;
        right.down[static_cast<usize>(Key::Right)] = true;
        right.pressed[static_cast<usize>(Key::Right)] = true;
        shell.frame(right, 1280, 800, at);
        at += 0.1;
    }
    InputState typed = quiet_input();
    typed.typed = "@";
    shell.frame(typed, 1280, 800, at);
    at += 0.1;
    shell.frame(with_ctrl(Key::S), 1280, 800, at);

    const std::string text = file_text(clip);
    const std::string was(kClip);
    CHECK(text.compare(0, 1, "@") == 0);
    CHECK(text == "@" + was.substr(3));
}

TEST_CASE("opening a document and saving it back changes not one byte") {
    Scratch scratch("shell-round-trip");
    const std::filesystem::path clip = write_file(scratch.root / "clips" / "same.wsclip", kClip);

    Shell shell;
    shell.load(scratch.root, scratch.root / "game");
    shell.open_editor(clip);
    REQUIRE(shell.open_editor_view("script"));
    quiet_frame(shell, 0.0);

    // Type a character and take it straight back out, so the file is dirty and nothing about it has
    // actually changed. `getline` cannot tell a trailing newline from none, and the save was
    // dropping it — every clip in this repository ends with one.
    InputState typed = quiet_input();
    typed.typed = "q";
    shell.frame(typed, 1280, 800, 0.1);
    InputState back = quiet_input();
    back.down[static_cast<usize>(Key::Backspace)] = true;
    back.pressed[static_cast<usize>(Key::Backspace)] = true;
    shell.frame(back, 1280, 800, 0.2);
    shell.frame(with_ctrl(Key::S), 1280, 800, 0.3);

    CHECK(file_text(clip) == std::string(kClip));
}

TEST_CASE("who made a file is not edited from in here") {
    Scratch scratch("shell-author");
    const std::string with_author = std::string("# WSauthor: somebody else\n") + kClip;
    const std::filesystem::path clip =
        write_file(scratch.root / "clips" / "theirs.wsclip", with_author);

    Shell shell;
    shell.load(scratch.root, scratch.root / "game");
    shell.open_editor(clip);
    REQUIRE(shell.open_editor_view("script"));
    quiet_frame(shell, 0.0);

    // The caret opens ON the author line, so typing there is refused; and choosing the whole
    // document and typing over it is the same edit by a longer route, so that is refused too.
    InputState typed = quiet_input();
    typed.typed = "me";
    shell.frame(typed, 1280, 800, 0.1);
    shell.frame(with_ctrl(Key::A), 1280, 800, 0.2);
    shell.frame(typed, 1280, 800, 0.3);
    shell.frame(with_ctrl(Key::S), 1280, 800, 0.4);

    CHECK(file_text(clip) == with_author);
}

// --- no two boxes in one place ----------------------------------------------------------------
//
// The graph's own promise, and the half of *never overlap* that a test can hold. The other half —
// a wire never crossing a box — is structural rather than checkable from out here: a box fills the
// top-left of its cell, so the strip down the right of every column and the strip along the bottom
// of every row have no box in them at any layout, and a wire only ever turns in those.

TEST_CASE("two boxes may not stand in the same place, however the file asks") {
    Scratch scratch("shell-no-overlap");

    // Three statements, and a hand-written layout comment putting all three on one cell. Nothing
    // stops a file saying this: it is a comment, it can be typed, and a document merged from two
    // branches can say it by accident.
    const std::filesystem::path clip = write_file(
        scratch.root / "clips" / "piled.wsclip",
        "metre 32\n"
        "let a = box 0 0 0  1 1 1   #@ 2 1\n"
        "let b = box 2 0 0  3 1 1   #@ 2 1\n"
        "let c = box 4 0 0  5 1 1   #@ 2 1\n"
        "let all = union { a b c }\n"
        "solid all\n");

    Shell shell;
    shell.load(scratch.root, scratch.root / "game");
    shell.open_editor(clip);
    REQUIRE(shell.open_editor_view("visual"));
    quiet_frame(shell, 0.0);
    CHECK(shell.boxes_overlapping() == 0);

    // And inside a box, where what is shown is that box and its parts rather than the answers --
    // which for this document is the union and all three piled-up boxes at once.
    REQUIRE(shell.choose_node("a"));
    quiet_frame(shell, 0.1);
    CHECK(shell.boxes_overlapping() == 0);
}

TEST_CASE("every document in the repository lays out with nothing on top of anything") {
    // The layout is worked out from the document, so this is a real sweep rather than a smoke test:
    // whatever the shipped clips do to it, no cell ends up with two boxes on it.
    const std::filesystem::path shipped = std::filesystem::path(WS_ASSET_SOURCE_DIR) / ".." / "clips";
    if (!std::filesystem::exists(shipped)) return;

    Scratch scratch("shell-no-overlap-all");
    Shell shell;
    shell.load(scratch.root, scratch.root / "game");

    u32 looked_at = 0;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::recursive_directory_iterator(shipped)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".clip") continue;
        if (++looked_at > 24) break;   // enough to cover every shape of document in there
        shell.open_editor(entry.path());
        REQUIRE(shell.open_editor_view("visual"));
        quiet_frame(shell, static_cast<f64>(looked_at) * 0.1);
        INFO(entry.path().filename().string());
        CHECK(shell.boxes_overlapping() == 0);
    }
    CHECK(looked_at > 0);
}
