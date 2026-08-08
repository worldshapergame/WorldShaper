// The typeface, and the one property that matters about it: a letter is matter.
//
// Everything here checks something a reader would notice. A glyph that is one pixel too wide, a
// baseline that moves between letters, a descender that sits on the line — none of those show up
// as an error, they show up as a sign that looks wrong, and by then it is a sign somebody built.

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "game/font.hpp"
#include "world/voxel_type.hpp"

using namespace ws;

namespace {

// The font as it ships, found by a path the build recorded rather than by guessing from the
// working directory. A test that walks up from wherever it was launched passes from the build
// folder and fails from the repository root, which makes it a test of where somebody was
// standing.
std::string font_directory() {
    return std::string(WS_ASSET_SOURCE_DIR) + "/font";
}

Font shipped() {
    Font font;
    font.read_directory(font_directory());
    return font;
}

}  // namespace

TEST_CASE("the font reads, and says so when it cannot") {
    Font font = shipped();
    for (const std::string& problem : font.problems()) {
        INFO(problem);
        CHECK(false);
    }
    CHECK(font.problems().empty());
    // Ninety-five printable ASCII characters, and the emoji block on top of them.
    CHECK(font.size() >= 95);
}

TEST_CASE("every printable ASCII character has a drawing") {
    const Font font = shipped();
    for (u32 code = 0x20; code <= 0x7E; ++code) {
        INFO("missing U+" << std::hex << code << " (" << static_cast<char>(code) << ")");
        REQUIRE(font.find(code) != nullptr);
    }
}

TEST_CASE("nothing is wider than the cell it was designed in") {
    // Five wide for a letter, and the emoji block is allowed its own larger cell. A glyph that
    // quietly grew a column would overlap its neighbour on every sign in the world.
    const Font font = shipped();
    for (u32 code = 0x20; code <= 0x7E; ++code) {
        const Glyph* glyph = font.find(code);
        REQUIRE(glyph != nullptr);
        INFO("U+" << std::hex << code << " is " << int(glyph->width) << " wide");
        CHECK(glyph->width <= 5);
        CHECK(glyph->height <= 9);
    }
}

TEST_CASE("letters share a baseline, and descenders hang below it") {
    const Font font = shipped();

    // A capital and an ascender reach the same height.
    CHECK(font.find('A')->top == 7);
    CHECK(font.find('b')->top == 7);
    CHECK(font.find('d')->top == 7);

    // A plain lowercase letter is five tall and sits on the line.
    const Glyph* o = font.find('o');
    REQUIRE(o != nullptr);
    CHECK(o->top == 5);
    CHECK(o->height == 5);

    // And a descender is the same five plus two below it — which is what `p` and `o` sharing a
    // top and differing at the bottom means arithmetically.
    const Glyph* p = font.find('p');
    REQUIRE(p != nullptr);
    CHECK(p->top == 5);
    CHECK(p->height == 7);
}

TEST_CASE("a narrow letter takes narrow room") {
    // The whole discipline of the typeface in one check: `i` is two wide and must not be set as
    // though it were five, or every word with an i in it has a hole in it.
    const Font font = shipped();
    CHECK(font.find('i')->advance < font.find('m')->advance);
    CHECK(font.find(' ')->advance <= 3);
}

TEST_CASE("measuring a string agrees with setting it") {
    const Font font = shipped();
    VoxelTypeTable types;

    const TextMetrics m = font.measure("Hello", 1);
    CHECK(m.width > 0);
    CHECK(m.above == 7);      // the capital H
    CHECK(m.below == 0);      // nothing in "Hello" descends

    TextRequest request;
    request.utf8 = "Hello";
    request.material = 1;
    const Clip clip = text_to_clip(font, request, types);
    CHECK(clip.size[0] == m.width);
    CHECK(clip.size[1] == m.above + m.below);
    CHECK(clip.solid_count() > 0);

    // A descender makes the clip taller without moving the baseline, which is the property that
    // lets two words set from the same point line up.
    const TextMetrics with_tail = font.measure("Happy", 1);
    CHECK(with_tail.above == 7);
    CHECK(with_tail.below == 2);
}

TEST_CASE("a word becomes matter, and a sunk word becomes its absence") {
    const Font font = shipped();
    VoxelTypeTable types;

    TextRequest raised;
    raised.utf8 = "AB";
    raised.material = 1;
    const Clip proud = text_to_clip(font, raised, types);

    TextRequest cut = raised;
    cut.sunk = true;
    const Clip sunk = text_to_clip(font, cut, types);

    REQUIRE(proud.cell_count() == sunk.cell_count());
    CHECK(proud.solid_count() > 0);
    CHECK(sunk.solid_count() > 0);
    // Every cell is matter in exactly one of them: the letters, or everything that is not.
    CHECK(proud.solid_count() + sunk.solid_count() == static_cast<u64>(proud.cell_count()));
}

TEST_CASE("scale and depth multiply the matter, not the drawing") {
    const Font font = shipped();
    VoxelTypeTable types;

    TextRequest one;
    one.utf8 = "1";
    one.material = 1;
    const u64 single = text_to_clip(font, one, types).solid_count();

    TextRequest bigger = one;
    bigger.scale = 3;
    bigger.depth = 2;
    const u64 many = text_to_clip(font, bigger, types).solid_count();
    CHECK(many == single * 3 * 3 * 2);
}

TEST_CASE("an emoji brings its own colours and a letter does not") {
    const Font font = shipped();

    const Glyph* letter = font.find('A');
    REQUIRE(letter != nullptr);
    CHECK_FALSE(letter->coloured());

    const Glyph* heart = font.find(0x2764);
    REQUIRE(heart != nullptr);
    CHECK(heart->coloured());
    CHECK(heart->width > 5);   // an emoji is allowed the room a face needs

    // And setting one interns a record per colour rather than per pixel.
    VoxelTypeTable types;
    const u32 before = types.type_count();
    TextRequest request;
    request.utf8 = "\xE2\x9D\xA4";   // U+2764
    request.material = 1;
    const Clip clip = text_to_clip(font, request, types);
    CHECK(clip.solid_count() > 0);
    CHECK(types.type_count() - before <= heart->palette.size());
}

TEST_CASE("text that is not in the font is reported rather than dropped") {
    const Font font = shipped();
    // A character nobody has drawn yet: it must be nameable, so a caller can say which one.
    const std::vector<u32> gaps = font.missing("A\xE2\x98\x83");   // U+2603 snowman
    REQUIRE(gaps.size() == 1);
    CHECK(gaps[0] == 0x2603);

    // And it still takes room, so the spacing of everything after it does not silently close up.
    CHECK(font.measure("A\xE2\x98\x83""B", 1).width > font.measure("AB", 1).width);
}
