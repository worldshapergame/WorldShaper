// Docking, and the four failures it exists to rule out.
//
// documentation/23-shell-and-libraries.md §2 (D443). Each of the four is a property of the
// rectangles this produces, so each is a test rather than a promise: nothing floats, a window never
// covers what it is about, two on one edge split it, and a size is remembered per edge.

#include <filesystem>
#include <fstream>

#include "doctest/doctest.h"
#include "ui/dock.hpp"

using namespace ws;
using namespace ws::ui;

namespace {

// A frame of nothing happening, which is all these tests need: no press, no pointer anywhere near
// a handle, so the layout is whatever the state says it is.
struct Bench {
    Ui ui;
    InputState input;
    Rect screen{0.0f, 0.0f, 1600.0f, 900.0f};
    f64 clock = 0.0;

    void step(Dock& dock) {
        input.mouse_x = -100.0f;   // off the window entirely
        input.mouse_y = -100.0f;
        clock += 1.0 / 60.0;
        ui.begin(input, 1600, 900, clock, 2.0f);
        dock.layout(ui, screen);
        ui.end();
    }
};

std::filesystem::path scratch(const char* name) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "worldshaper-tests";
    std::filesystem::create_directories(dir);
    return dir / name;
}

}  // namespace

TEST_CASE("a window opens on the side its family means") {
    // Parameters on the left, libraries on the right. The side is not decoration: changing a
    // number and choosing a thing are different tasks, and a consistent side means the eye knows
    // where to go without reading a title bar.
    Dock dock;
    const u32 settings = dock.add("settings", Family::Parameters);
    const u32 worlds = dock.add("worlds", Family::Library);
    CHECK(dock.edge_of(settings) == Edge::Left);
    CHECK(dock.edge_of(worlds) == Edge::Right);
}

TEST_CASE("nothing floats, and nothing covers the middle") {
    Dock dock;
    const u32 settings = dock.add("settings", Family::Parameters);
    const u32 worlds = dock.add("worlds", Family::Library);
    dock.set_open(settings, true);
    dock.set_open(worlds, true);

    Bench bench;
    bench.step(dock);

    // Every open window touches the edge it is docked to, so none of them is floating in the
    // middle of the screen where it could be lost behind another.
    CHECK(dock.rect(settings).x0 == doctest::Approx(bench.screen.x0));
    CHECK(dock.rect(worlds).x1 == doctest::Approx(bench.screen.x1));

    // And the world in the middle is not under either of them. A parameters window editing a
    // tool's reach is worth nothing if it is over the thing being reached at.
    const Rect& centre = dock.centre();
    CHECK(centre.x0 >= dock.rect(settings).x1 - 0.001f);
    CHECK(centre.x1 <= dock.rect(worlds).x0 + 0.001f);
    CHECK(centre.width() > 0.0f);
}

TEST_CASE("two windows on one edge share it rather than stacking") {
    Dock dock;
    const u32 first = dock.add("settings", Family::Parameters);
    const u32 second = dock.add("worlds", Family::Library);
    dock.set_open(first, true);
    dock.set_open(second, true);
    dock.dock_to(second, Edge::Left);

    Bench bench;
    bench.step(dock);

    const Rect& a = dock.rect(first);
    const Rect& b = dock.rect(second);
    // The same band, split along it, with no overlap: one above the other and both full width.
    CHECK(a.x0 == doctest::Approx(b.x0));
    CHECK(a.x1 == doctest::Approx(b.x1));
    CHECK(a.y1 <= b.y0 + 0.001f);
    CHECK(a.height() > 0.0f);
    CHECK(b.height() > 0.0f);
    // And the right-hand edge is now empty, so the middle got that room back.
    CHECK(dock.centre().x1 == doctest::Approx(bench.screen.x1));
}

TEST_CASE("a window remembers its size per edge") {
    // The useful width of a library docked left is not its useful height docked top, so the two
    // are different numbers and moving between them does not overwrite either.
    Dock dock;
    const u32 worlds = dock.add("worlds", Family::Library);
    dock.set_open(worlds, true);

    Bench bench;
    bench.step(dock);
    const f32 as_right = dock.rect(worlds).width() / bench.screen.width();

    dock.dock_to(worlds, Edge::Top);
    bench.step(dock);
    const f32 as_top = dock.rect(worlds).height() / bench.screen.height();

    dock.dock_to(worlds, Edge::Right);
    bench.step(dock);
    CHECK(dock.rect(worlds).width() / bench.screen.width() == doctest::Approx(as_right));
    CHECK(as_top != doctest::Approx(as_right));   // the defaults differ, and they are kept apart
}

TEST_CASE("a layout survives a round trip, and a damaged one costs the defaults") {
    const std::filesystem::path path = scratch("layout.txt");

    {
        Dock dock;
        const u32 settings = dock.add("settings", Family::Parameters);
        const u32 worlds = dock.add("worlds", Family::Library);
        dock.set_open(settings, true);
        dock.dock_to(worlds, Edge::Bottom);
        REQUIRE(dock.write(path));
    }
    {
        Dock dock;
        const u32 settings = dock.add("settings", Family::Parameters);
        const u32 worlds = dock.add("worlds", Family::Library);
        REQUIRE(dock.read(path));
        CHECK(dock.is_open(settings));
        CHECK(dock.edge_of(worlds) == Edge::Bottom);
    }

    // Half a line, a key nobody has heard of, and a number well out of range.
    {
        std::ofstream broken(path, std::ios::trunc);
        broken << "split 2\nwindow worlds 99 1 5 5 5\nnonsense here\n";
    }
    {
        Dock dock;
        const u32 worlds = dock.add("worlds", Family::Library);
        CHECK(dock.read(path));           // reads, rather than failing
        CHECK(dock.edge_of(worlds) == Edge::Right);   // and the defaults survived it
        Bench bench;
        bench.step(dock);
        CHECK(dock.centre().width() > 0.0f);
    }
    std::filesystem::remove(path);
}
