// A library is a file manager over the real folder, so this is a test over a real folder.
//
// documentation/23-shell-and-libraries.md §4, decisions D445–D447. The rule everything here checks
// is the one the whole design rests on: **nothing in the game may keep state about a file that the
// file does not carry**, because the file can move, be renamed or be deleted without the game
// watching.

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "doctest/doctest.h"
#include "ui/library.hpp"

using namespace ws;
using namespace ws::ui;

namespace {

// A root of our own, thrown away afterwards. Never the player's: a test that deletes things has to
// be somewhere it cannot delete the wrong thing.
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

const Kind& clips_kind() {
    for (const Kind& kind : shipped_kinds()) {
        if (kind.folder == "clips") return kind;
    }
    return shipped_kinds().front();
}

const Entry* find(const Library& library, const std::string& shown) {
    for (const Entry& entry : library.entries()) {
        if (entry.shown == shown) return &entry;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("the shelves are folders, and they are made if they are missing") {
    Scratch scratch("shelves");
    Library library(scratch.root);
    library.ensure_folders();
    for (const Kind& kind : shipped_kinds()) {
        CHECK(std::filesystem::is_directory(scratch.root / kind.folder));
    }
    CHECK(std::filesystem::is_directory(scratch.root / "trash"));
}

TEST_CASE("a file the game makes carries who made it, and a copy carries it too") {
    // D447. The mechanism is that the tag is IN the file, which is why duplicating one takes no
    // code at all: a copy of the file is a copy of the tag.
    Scratch scratch("author");
    Library library(scratch.root);
    library.ensure_folders();
    library.open(clips_kind());

    CHECK(library.create("arch", "ada").empty());
    library.refresh(true);
    const Entry* made = find(library, "arch");
    REQUIRE(made != nullptr);
    CHECK(made->author == "ada");

    CHECK(library.duplicate({*made}).empty());
    library.refresh(true);
    const Entry* copy = find(library, "arch 2");
    REQUIRE(copy != nullptr);
    CHECK(copy->author == "ada");

    // And it is still a text file that a clip parser will read: the tag is a comment.
    std::ifstream file(copy->path);
    std::string first;
    std::getline(file, first);
    CHECK(first.rfind("#", 0) == 0);
}

TEST_CASE("who made a file is not part of what the file builds") {
    // A built world is cached under a key hashed from the source that produced it. Stamping a copy
    // with its author changes that text — so without this, every world put on the shelf misses its
    // own cache the first time it is opened and rebuilds from cold. On a large one that is minutes
    // of resampling, and what it looks like from inside is a world that arrives in coarse blocks
    // and slowly resolves, which reads as the streaming being broken rather than as one comment
    // line in the wrong place.
    const std::string stamped =
        "# WSauthor: ada\n"
        "metre 32\n"
        "bounds -4 0 -4  4 4 4\n";
    const std::string plain =
        "metre 32\n"
        "bounds -4 0 -4  4 4 4\n";
    CHECK(without_author(stamped) == plain);

    // Every author line, wherever it is — a clip spliced out of twenty fragments can carry one per
    // fragment, and one surviving is one key that still does not match.
    const std::string spliced =
        "metre 32\n"
        "# WSauthor: ada\n"
        "include done\n"
        "-- WSauthor: zoe\n"
        "bounds -1 0 -1  1 1 1\n";
    CHECK(without_author(spliced) == "metre 32\ninclude done\nbounds -1 0 -1  1 1 1\n");

    // And text with no tag in it comes back untouched, byte for byte: every world cached before
    // this existed has to keep matching its own key.
    CHECK(without_author(plain) == plain);
    CHECK(without_author({}).empty());
}

TEST_CASE("a file from outside carries no author and is not given one") {
    // A clip dragged in from Explorer appears — that is the whole point of the folder being a
    // folder — and the library does not invent a name for it. Guessing that an untagged file is
    // yours is how somebody else's work ends up with your name on it.
    Scratch scratch("foreign");
    Library library(scratch.root);
    library.ensure_folders();
    library.open(clips_kind());
    {
        std::ofstream file(scratch.root / "clips" / "dropped.wsclip");
        file << "metre 32\n";
    }
    library.refresh(true);
    const Entry* dropped = find(library, "dropped");
    REQUIRE(dropped != nullptr);
    CHECK(dropped->author.empty());
}

TEST_CASE("only this shelf's own kind is listed") {
    Scratch scratch("kinds");
    Library library(scratch.root);
    library.ensure_folders();
    library.open(clips_kind());
    {
        std::ofstream a(scratch.root / "clips" / "notes.txt");
        a << "not a clip";
        std::ofstream b(scratch.root / "clips" / "real.wsclip");
        b << "metre 32";
    }
    library.refresh(true);
    CHECK(find(library, "real") != nullptr);
    CHECK(find(library, "notes") == nullptr);
}

TEST_CASE("folders go to any depth, and a folder sorts before a file") {
    Scratch scratch("folders");
    Library library(scratch.root);
    library.ensure_folders();
    library.open(clips_kind());

    CHECK(library.create("zebra", "ada").empty());
    CHECK(library.make_folder("houses").empty());
    library.refresh(true);
    REQUIRE(library.entries().size() == 2);
    CHECK(library.entries().front().folder);

    const Entry* houses = find(library, "houses");
    REQUIRE(houses != nullptr);
    library.enter(*houses);
    CHECK(!library.at_top());
    CHECK(library.create("cottage", "ada").empty());
    CHECK(library.make_folder("gables").empty());
    library.refresh(true);
    CHECK(library.entries().size() == 2);

    library.up();
    CHECK(library.at_top());
}

TEST_CASE("a delete goes to the trash, not away") {
    // A delete a player cannot undo is not the thing they pressed, and this is a library of work
    // somebody made.
    Scratch scratch("trash");
    Library library(scratch.root);
    library.ensure_folders();
    library.open(clips_kind());
    CHECK(library.create("spire", "ada").empty());
    library.refresh(true);
    const Entry* spire = find(library, "spire");
    REQUIRE(spire != nullptr);
    const std::filesystem::path was = spire->path;

    CHECK(library.erase({*spire}).empty());
    library.refresh(true);
    CHECK(find(library, "spire") == nullptr);
    CHECK(!std::filesystem::exists(was));
    CHECK(std::filesystem::exists(library.trash() / "spire.wsclip"));
}

TEST_CASE("two deletes of the same name are two files in the trash") {
    Scratch scratch("trash-names");
    Library library(scratch.root);
    library.ensure_folders();
    library.open(clips_kind());

    for (u32 round = 0; round < 2; ++round) {
        CHECK(library.create("tower", "ada").empty());
        library.refresh(true);
        const Entry* tower = find(library, "tower");
        REQUIRE(tower != nullptr);
        CHECK(library.erase({*tower}).empty());
    }
    CHECK(std::filesystem::exists(library.trash() / "tower.wsclip"));
    CHECK(std::filesystem::exists(library.trash() / "tower 2.wsclip"));
}

TEST_CASE("rename keeps the extension, and refuses a name a file cannot have") {
    Scratch scratch("rename");
    Library library(scratch.root);
    library.ensure_folders();
    library.open(clips_kind());
    CHECK(library.create("shed", "ada").empty());
    library.refresh(true);
    const Entry* shed = find(library, "shed");
    REQUIRE(shed != nullptr);

    CHECK(library.rename(*shed, "barn").empty());
    library.refresh(true);
    CHECK(find(library, "barn") != nullptr);
    CHECK(std::filesystem::exists(scratch.root / "clips" / "barn.wsclip"));

    const Entry* barn = find(library, "barn");
    REQUIRE(barn != nullptr);
    // A separator in a name is a file that lands somewhere else, so it is refused with a line
    // rather than accepted and silently put in another folder.
    CHECK(!library.rename(*barn, "sub/barn").empty());
    CHECK(!library.rename(*barn, "").empty());
}

TEST_CASE("moving a folder into itself is refused") {
    Scratch scratch("move");
    Library library(scratch.root);
    library.ensure_folders();
    library.open(clips_kind());
    CHECK(library.make_folder("estate").empty());
    library.refresh(true);
    const Entry* estate = find(library, "estate");
    REQUIRE(estate != nullptr);
    CHECK(!library.move({*estate}, estate->path).empty());
    CHECK(std::filesystem::is_directory(estate->path));
}

TEST_CASE("sorting has one order, and it puts folders first whichever it is") {
    Scratch scratch("sort");
    Library library(scratch.root);
    library.ensure_folders();
    library.open(clips_kind());
    CHECK(library.create("beta", "zoe").empty());
    CHECK(library.create("alpha", "ada").empty());
    CHECK(library.make_folder("things").empty());
    library.refresh(true);

    library.set_sort(Sort::Name, false);
    REQUIRE(library.entries().size() == 3);
    CHECK(library.entries()[0].folder);
    CHECK(library.entries()[1].shown == "alpha");

    library.set_sort(Sort::Name, true);
    CHECK(library.entries()[0].folder);
    CHECK(library.entries()[1].shown == "beta");

    library.set_sort(Sort::Author, false);
    CHECK(library.entries()[1].author == "ada");
    CHECK(library.entries()[2].author == "zoe");
}

TEST_CASE("a listing is re-read when the folder changes underneath it") {
    // A player who drags a file in from Explorer expects to see it, and nothing in the game told
    // it to look.
    Scratch scratch("watch");
    Library library(scratch.root);
    library.ensure_folders();
    library.open(clips_kind());
    CHECK(library.entries().empty());
    {
        std::ofstream file(scratch.root / "clips" / "dropped.wsclip");
        file << "metre 32\n";
    }
    library.refresh(true);
    CHECK(library.entries().size() == 1);
}
