// The list the card is handed: what is in it, what is left out, and which tiles each mark reaches.
//
// This is the part of the interface that can be wrong without looking wrong — a mark binned into
// the wrong tile draws in the wrong place only when a window happens to be there, and a mark
// binned into no tile at all simply is not drawn and nothing says so.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "doctest/doctest.h"
#include "ui/draw.hpp"

using namespace ws;
using namespace ws::ui;

TEST_CASE("measuring a string is what the card will actually set") {
    // The advance table is the same table shaders/ui.glsl carries, and tests/test_font.cpp checks
    // both against the font's own source. What is checked here is the arithmetic on top of it.
    DrawList list;
    list.begin(800, 600);

    // `i` is one wide and `m` is five, so a proportional face measures them differently. A face
    // that did not would put a hole either side of every `i`.
    CHECK(DrawList::measure("i", 1.0f) < DrawList::measure("m", 1.0f));
    // Tracking after the last letter is not part of the word.
    CHECK(DrawList::measure("ii", 1.0f) == doctest::Approx(DrawList::measure("i", 1.0f) * 2.0f + 1.0f));
    // Scale multiplies.
    CHECK(DrawList::measure("hello", 2.0f) == doctest::Approx(DrawList::measure("hello", 1.0f) * 2.0f));
    // A code span is monospaced, so every glyph steps the same.
    CHECK(DrawList::measure("il", 1.0f, kMono) == doctest::Approx(DrawList::measure("mm", 1.0f, kMono)));
    // And an emphasis costs no width at all. On a panel bold is one device pixel of smear at
    // partial ink and italic is a shear, so a word does not get wider when it is emphasised —
    // which is what stops `**this**` from shifting the rest of a line while it is being typed.
    CHECK(DrawList::measure("abc", 1.0f, kBold) == doctest::Approx(DrawList::measure("abc", 1.0f)));
    CHECK(DrawList::measure("abc", 1.0f, kItalic) == doctest::Approx(DrawList::measure("abc", 1.0f)));
}

TEST_CASE("how much of a name fits is measured, not guessed") {
    const f32 whole = DrawList::measure("facility", 2.0f);
    CHECK(DrawList::fits("facility", whole + 1.0f, 2.0f) == 8);
    CHECK(DrawList::fits("facility", 0.0f, 2.0f) == 0);
    const usize some = DrawList::fits("facility", whole * 0.5f, 2.0f);
    CHECK(some > 0);
    CHECK(some < 8);
}

TEST_CASE("a run is centred on its capitals, not on its line box") {
    DrawList list;
    list.begin(400, 100);
    // Two strings at the same size, one with a descender and one without, asked for the same top.
    // A capital is the same box for every string at a given size, so both start on the same row —
    // which is what keeps a column of labels on one baseline whatever letters are in them.
    list.text(10.0f, 20.0f, "Fast", 2.0f);
    list.text(10.0f, 20.0f, "Happy", 2.0f);
    REQUIRE(list.commands().size() == 2);
    CHECK(list.commands()[0].f2 == list.commands()[1].f2);
}

TEST_CASE("centring a run puts its middle where it was asked for") {
    DrawList list;
    list.begin(400, 100);
    const f32 width = DrawList::measure("worlds", 2.0f);
    list.text(200.0f, 10.0f, "worlds", 2.0f, kPlain, Align::Centre);
    REQUIRE(list.commands().size() == 1);
    // Snapped to a whole device pixel, because the face is a pixel font and a run set at a
    // fractional offset has its columns land on different sides of a boundary along the string.
    CHECK(list.commands()[0].f1 == doctest::Approx(std::floor(200.0f - width * 0.5f + 0.5f)));
}

TEST_CASE("a mark entirely outside its window is not drawn and not binned") {
    DrawList list;
    list.begin(256, 256);
    list.push_clip(Rect{0.0f, 0.0f, 100.0f, 100.0f});
    list.ink(Rect{200.0f, 200.0f, 240.0f, 240.0f});
    list.pop_clip();
    CHECK(list.commands().empty());
    list.bin();
    CHECK(list.tile_indices().empty());
}

TEST_CASE("a clip narrows what a mark reaches, so a list scrolls under its own header") {
    DrawList list;
    list.begin(256, 256);
    list.push_clip(Rect{0.0f, 64.0f, 256.0f, 128.0f});
    list.ink(Rect{0.0f, 0.0f, 256.0f, 256.0f});
    list.pop_clip();
    list.bin();

    // Rows 0 to 3 are above the clip and rows 8 up are below it; only the four tile rows the clip
    // covers may hold the mark.
    const u32 across = list.tiles_across();
    for (u32 row = 0; row < list.tiles_down(); ++row) {
        const u32 count = list.tiles()[(static_cast<usize>(row) * across) * 2 + 1];
        const bool inside = (row >= 4 && row < 8);
        INFO("tile row " << row);
        CHECK((count > 0) == inside);
    }
}

TEST_CASE("a tile's list is the marks that reach it, in the order they were added") {
    DrawList list;
    list.begin(64, 64);
    list.glass(Rect{0.0f, 0.0f, 32.0f, 32.0f});     // command 0: tiles (0,0) and (1,0)-ish
    list.ink(Rect{0.0f, 0.0f, 16.0f, 16.0f});       // command 1: tile (0,0) only
    list.ink(Rect{48.0f, 48.0f, 64.0f, 64.0f});     // command 2: the far corner
    list.bin();

    const u32 across = list.tiles_across();
    REQUIRE(across == 4);
    const auto tile_list = [&](u32 x, u32 y) {
        const usize at = (static_cast<usize>(y) * across + x) * 2;
        const u32 first = list.tiles()[at];
        const u32 count = list.tiles()[at + 1];
        return std::vector<u32>(list.tile_indices().begin() + first,
                                list.tile_indices().begin() + first + count);
    };

    // The compositing order is the order a pixel walks its own tile's list, which is why it has to
    // be the order the marks were added: a label added after the glass it sits on must read the
    // glass as its backdrop.
    CHECK(tile_list(0, 0) == std::vector<u32>{0u, 1u});
    CHECK(tile_list(1, 1) == std::vector<u32>{0u});
    CHECK(tile_list(3, 3) == std::vector<u32>{2u});
    CHECK(tile_list(2, 0).empty());
}

TEST_CASE("the bounds are whole tiles, because that is what a dispatch can cover") {
    DrawList list;
    list.begin(256, 256);
    list.ink(Rect{20.0f, 20.0f, 40.0f, 40.0f});
    list.bin();
    const Rect& bounds = list.bounds();
    CHECK(bounds.x0 == doctest::Approx(16.0f));
    CHECK(bounds.y0 == doctest::Approx(16.0f));
    CHECK(bounds.x1 == doctest::Approx(48.0f));
    CHECK(bounds.y1 == doctest::Approx(48.0f));
}

TEST_CASE("nothing drawn is nothing dispatched") {
    // The world in the middle costs exactly nothing when no window is open, which is the whole
    // reason the marks are binned rather than run over the screen.
    DrawList list;
    list.begin(1920, 1080);
    list.bin();
    CHECK(list.empty());
    CHECK(list.bounds().empty());
}

TEST_CASE("a character the face does not have takes room and comes out blank") {
    // A gap that silently closes moves everything after it, which is the failure a reader cannot
    // diagnose. A blank that takes its own width is one they can.
    const f32 known = DrawList::measure("A", 1.0f);
    const f32 unknown = DrawList::measure("\xE2\x98\x85", 1.0f);   // U+2605, a star: not in the face
    CHECK(unknown > 0.0f);
    CHECK(known > 0.0f);
}
