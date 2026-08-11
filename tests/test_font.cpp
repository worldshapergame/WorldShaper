// The typeface, and the two properties that matter about it: a letter is matter, and it is as
// small as a letter can be.
//
// Everything here checks something a reader would notice. A glyph that is one pixel too wide, a
// baseline that moves between letters, a descender that sits on the line, two characters that came
// out as the same drawing — none of those show up as an error, they show up as a sign that looks
// wrong, and by then it is a sign somebody built.

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "game/font.hpp"
#include "ui/glyphs.hpp"
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

// Whether a page has ink anywhere along a row.
bool row_has_ink(const InkPage& page, i32 row) {
    for (i32 x = 0; x < page.width; ++x) {
        if (page.at(x, row) != 0) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("the font reads, and says so when it cannot") {
    Font font = shipped();
    for (const std::string& problem : font.problems()) {
        INFO(problem);
        CHECK(false);
    }
    CHECK(font.problems().empty());
    // Ninety-five printable ASCII characters, the marks a document needs, and the emoji block.
    CHECK(font.size() >= 95);
}

TEST_CASE("every printable ASCII character has a drawing") {
    const Font font = shipped();
    for (u32 code = 0x20; code <= 0x7E; ++code) {
        INFO("missing U+" << std::hex << code << " (" << static_cast<char>(code) << ")");
        REQUIRE(font.find(code) != nullptr);
    }
}

TEST_CASE("the cell is three by five, and exactly five letters are allowed past it") {
    const Font font = shipped();

    // Three strokes do not fit in three columns, so M, W, m and w are five wide and N is four.
    // The point of the test is that the list cannot grow by accident: a letter that quietly took a
    // fourth column would cost every sign in the world a column it did not ask for.
    const std::set<u32> wide = {'M', 'N', 'W', 'm', 'w'};
    for (u32 code = 0x20; code <= 0x7E; ++code) {
        const Glyph* glyph = font.find(code);
        REQUIRE(glyph != nullptr);
        INFO("U+" << std::hex << code << " (" << static_cast<char>(code) << ") is "
                  << int(glyph->width) << " wide and " << int(glyph->height) << " tall");
        CHECK(glyph->height <= kCapHeight + kDescent);
        if (wide.count(code) != 0) {
            CHECK(glyph->width <= 5);
            CHECK(glyph->width > kNominalWidth);
        } else {
            CHECK(glyph->width <= kNominalWidth);
        }
    }
}

TEST_CASE("no two characters are the same drawing") {
    // The whole argument for three by five is that the shapes stay DISTINCT at that size. This is
    // that claim, as a test: if a redrawn `S` ever becomes the `5`, or an `l` the `1`, it fails
    // here rather than in a word somebody carved into a wall.
    const Font font = shipped();
    for (u32 a = 0x20; a <= 0x7E; ++a) {
        const Glyph* first = font.find(a);
        REQUIRE(first != nullptr);
        for (u32 b = a + 1; b <= 0x7E; ++b) {
            const Glyph* second = font.find(b);
            REQUIRE(second != nullptr);
            const bool identical = first->pixels == second->pixels && first->top == second->top &&
                                   first->width == second->width &&
                                   first->advance == second->advance;
            INFO("U+" << std::hex << a << " and U+" << b << " are the same drawing");
            CHECK_FALSE(identical);
        }
    }
}

TEST_CASE("letters share a baseline, and descenders hang below it") {
    const Font font = shipped();

    // A capital and an ascender reach the same height.
    CHECK(font.find('A')->top == kCapHeight);
    CHECK(font.find('b')->top == kCapHeight);
    CHECK(font.find('d')->top == kCapHeight);

    // A plain lowercase letter is three tall and sits on the line.
    const Glyph* o = font.find('o');
    REQUIRE(o != nullptr);
    CHECK(o->top == kXHeight);
    CHECK(o->height == kXHeight);

    // And a descender is the same three plus one below it — which is what `p` and `o` sharing a
    // top and differing at the bottom means arithmetically.
    const Glyph* p = font.find('p');
    REQUIRE(p != nullptr);
    CHECK(p->top == kXHeight);
    CHECK(p->height == kXHeight + kDescent);

    // A capital may descend too: Q's tail is what stops it being an O.
    const Glyph* q = font.find('Q');
    REQUIRE(q != nullptr);
    CHECK(q->top == kCapHeight);
    CHECK(q->height == kCapHeight + kDescent);
}

TEST_CASE("a narrow letter takes narrow room") {
    // The whole discipline of the typeface in one check: `i` is one wide and must not be set as
    // though it were three, or every word with an i in it has a hole in it.
    const Font font = shipped();
    CHECK(font.find('i')->advance < font.find('m')->advance);
    CHECK(font.find('i')->advance == 1);
    CHECK(font.find(' ')->advance <= 3);
}

TEST_CASE("measuring a string agrees with setting it") {
    const Font font = shipped();
    VoxelTypeTable types;

    const TextMetrics m = font.measure("Hello", 1);
    CHECK(m.width > 0);
    CHECK(m.above == kCapHeight);   // the capital H
    CHECK(m.below == 0);            // nothing in "Hello" descends

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
    CHECK(with_tail.above == kCapHeight);
    CHECK(with_tail.below == kDescent);
}

TEST_CASE("a style is a property of the drawing, not a second typeface") {
    const Font font = shipped();

    const TextMetrics plain = font.measure("Hello", 1);
    const TextMetrics bold = font.measure_styled("Hello", kBold, 1);
    const TextMetrics italic = font.measure_styled("Hello", kItalic, 1);

    // Bold is the drawing smeared one pixel sideways, so every letter is one wider. There is no
    // second weight to draw at three pixels across, and this is what that means arithmetically.
    CHECK(bold.width == plain.width + 5);
    CHECK(italic.width == plain.width + 5);
    CHECK(bold.above == plain.above);

    // The underline hangs one row BELOW the descender, so it never touches a `p`.
    const TextMetrics underlined = font.measure_styled("Hello", kUnderline, 1);
    CHECK(underlined.below == kDescent + 1);
    const InkPage page = set_text(font, "Hello", kUnderline, 1);
    REQUIRE(page.height == kCapHeight + kDescent + 1);
    CHECK(row_has_ink(page, page.height - 1));
    CHECK_FALSE(row_has_ink(page, page.baseline));   // no descender in "Hello", so its row is bare

    // A strike runs along the middle of the x-height, which is where the hyphen already is.
    const InkPage struck = set_text(font, "oo", kStrike, 1);
    for (i32 x = 0; x < struck.width; ++x) {
        INFO("column " << x << " of a struck run");
        CHECK(struck.at(x, struck.baseline - 2) != 0);
    }

    // Monospaced, for code: every glyph steps the same, whatever it is, so the second letter of a
    // pair starts at the same place whether the first was an `i` or an `m`.
    const TextMetrics narrow = font.measure_styled("il", kMono, 1);
    const TextMetrics wide = font.measure_styled("mw", kMono, 1);
    CHECK(narrow.width == kMonoAdvance + font.find('l')->width);
    CHECK(wide.width == kMonoAdvance + font.find('w')->width);
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

TEST_CASE("the marks a document needs are glyphs like any other") {
    const Font font = shipped();
    for (u32 mark : {0x2022u, 0x2610u, 0x2611u}) {   // bullet, empty box, done box
        const Glyph* glyph = font.find(mark);
        INFO("U+" << std::hex << mark);
        REQUIRE(glyph != nullptr);
        CHECK(glyph->width <= kNominalWidth);
        CHECK(glyph->height <= kCapHeight);
    }
    // An empty box and a done box have to be different silhouettes and not one pixel apart.
    CHECK(font.find(0x2610)->pixels != font.find(0x2611)->pixels);
}

TEST_CASE("the shader's font is the font") {
    // The interface draws its text on the card, so shaders/ui.glsl carries a packed copy of the
    // same drawings. Two copies is one place for them to disagree — and a letter that is wrong
    // only on screen is the kind of fault that survives a whole release. So the table is generated
    // (tools/font-table.ps1) and read back here, glyph by glyph, against the source it came from.
    const Font font = shipped();

    const std::string path = std::string(WS_SHADER_SOURCE_DIR) + "/ui.glsl";
    std::ifstream file(path, std::ios::binary);
    REQUIRE_MESSAGE(file.good(), "could not open " << path);
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();

    const usize begin = text.find("kUiGlyphs[95]");
    REQUIRE(begin != std::string::npos);
    const usize end = text.find("--- end generated ---", begin);
    REQUIRE(end != std::string::npos);

    u32 code = 0x20;
    usize at = begin;
    u32 counted = 0;
    while (true) {
        at = text.find("uvec2(0x", at);
        if (at == std::string::npos || at > end) break;
        at += 8;
        const u32 bits = static_cast<u32>(std::strtoul(text.c_str() + at, nullptr, 16));
        const usize comma = text.find(',', at);
        REQUIRE(comma != std::string::npos);
        const u32 advance = static_cast<u32>(std::strtoul(text.c_str() + comma + 1, nullptr, 10));

        const Glyph* glyph = font.find(code);
        REQUIRE(glyph != nullptr);

        // The packing: five bits a row, row 0 at the cap height, bit 4 of a row its leftmost
        // column — so the number reads the way the drawing was written down.
        u32 expected = 0;
        for (u32 y = 0; y < glyph->height; ++y) {
            const i32 row = kCapHeight - (glyph->top - static_cast<i32>(y));
            REQUIRE(row >= 0);
            REQUIRE(row < kCapHeight + kDescent);
            for (u32 x = 0; x < glyph->width; ++x) {
                REQUIRE(x < 5u);
                if (glyph->at(x, y) != 0) {
                    expected |= 1u << static_cast<u32>(row * 5 + (4 - static_cast<i32>(x)));
                }
            }
        }

        INFO("U+" << std::hex << code << " (" << static_cast<char>(code) << ")");
        CHECK(bits == expected);
        CHECK(advance == glyph->advance);

        ++counted;
        ++code;
        at = comma;
    }
    CHECK(counted == 95);   // every printable ASCII character, in order, and nothing else
}

TEST_CASE("the shell's font is the font") {
    // There is a third copy, and it exists because the card DRAWS the words while the shell
    // MEASURES them: where a word ends decides how wide its button is, which decides what a click
    // at a given pixel landed on. Uploading the shader's table back to the host would make the
    // host wait on the card to lay out a menu; hand-writing a second one would make the two
    // disagree the first time a letter was redrawn. So both are generated from this same source by
    // tools/font-table.ps1 and both are checked against it here.
    const Font font = shipped();

    for (u32 code = 0x20; code <= 0x7E; ++code) {
        const Glyph* glyph = font.find(code);
        REQUIRE(glyph != nullptr);
        const ui::Glyph& shell = ui::kGlyphs[code - 0x20];

        u32 expected = 0;
        for (u32 y = 0; y < glyph->height; ++y) {
            const i32 row = kCapHeight - (glyph->top - static_cast<i32>(y));
            for (u32 x = 0; x < glyph->width; ++x) {
                if (glyph->at(x, y) != 0) {
                    expected |= 1u << static_cast<u32>(row * 5 + (4 - static_cast<i32>(x)));
                }
            }
        }
        INFO("U+" << std::hex << code << " (" << static_cast<char>(code) << ")");
        CHECK(shell.bits == expected);
        CHECK(shell.advance == glyph->advance);
    }

    // And the arithmetic on top of the table is the same arithmetic: the advance a run steps is
    // the glyph plus one of tracking, and a flat six in a code span. `ui_step` in shaders/ui.glsl
    // is those same two lines.
    const ui::Glyph& em = ui::glyph_of('m');
    CHECK(ui::glyph_step(em, ui::kPlain) == doctest::Approx(static_cast<f32>(em.advance) + 1.0f));
    CHECK(ui::glyph_step(ui::glyph_of('i'), ui::kMono) == doctest::Approx(ui::kGlyphMonoStep));
    // An emphasis costs no width ON A PANEL, because there it is a device pixel of smear and a
    // shear rather than a wider drawing — so a bold word does not move the rest of its line as it
    // is typed. In the WORLD it still does, because a voxel has no fraction to smear into and the
    // only heavier `e` available there is a wider one.
    CHECK(ui::glyph_step(em, ui::kBold) == doctest::Approx(ui::glyph_step(em, ui::kPlain)));
    CHECK(ui::glyph_step(em, ui::kItalic) == doctest::Approx(ui::glyph_step(em, ui::kPlain)));
    CHECK(font.step(font.find('m'), kBold, 1) > font.step(font.find('m'), kPlain, 1));
}
