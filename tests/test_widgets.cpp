// The controls, driven by a fabricated hand.
//
// documentation/23-shell-and-libraries.md §3 is the specification: every numeric value is a slider,
// a double-click types into it, and a typed value is not capped (D444).
//
// # Why this file exists at all
//
// Because a slider was reported as *resetting itself to the value it had before* and, underneath
// that, as the whole settings window having no effect — and both were one fault with one line in it.
// `Ui::number` writes into the caller's variable and returns whether it wrote; the caller copies the
// result into wherever the setting lives. It used to return true only on the frame the button came
// UP, and on that frame the drag arithmetic no longer ran, so what it handed back was the value it
// had been given. Every drag of every slider in the game was discarded, and the handle springing
// back was the interface honestly reporting that nothing had changed.
//
// A screenshot cannot see that. A test can, so the contract is held here: **if the value moved, the
// caller is told, on that frame.**

#include <cmath>
#include <string>

#include "doctest/doctest.h"
#include "ui/widgets.hpp"

using namespace ws;
using namespace ws::ui;

namespace {

// One hand, one row, and a clock that ticks at a sixtieth like the real one.
struct Bench {
    Ui ui;
    InputState input;
    f64 clock = 0.0;
    Rect row{100.0f, 100.0f, 300.0f, 130.0f};
    bool wrote = false;

    // The pointer is placed BEFORE the frame begins, because that is the order the real thing runs
    // in: the platform fills an InputState, then `begin` takes a copy of it. A test that moves the
    // hand after `begin` is testing last frame's hand, which is how this bench was wrong first.
    void hand(f32 x, f32 y, bool down, bool pressed = false, bool released = false) {
        input.mouse_dx = x - input.mouse_x;
        input.mouse_dy = y - input.mouse_y;
        input.mouse_x = x;
        input.mouse_y = y;
        input.mouse_left = down;
        input.mouse_left_pressed = pressed;
        input.mouse_left_released = released;
        input.click_count = pressed ? 1u : 0u;
    }

    // One frame of the shape every caller in the settings window has: a copy of the setting handed
    // in, and copied back if and only if the control says it wrote to it.
    f64 run(u64 id, const Number& about, f64 setting, f32 x, f32 y, bool down, bool pressed = false,
            bool released = false) {
        hand(x, y, down, pressed, released);
        clock += 1.0 / 60.0;
        ui.begin(input, 800, 600, clock, 2.0f);
        f64 value = setting;
        wrote = ui.number(id, row, about, value);
        if (wrote) setting = value;
        ui.end();
        return setting;
    }
};

Number plain(std::string_view label = "a value") {
    Number about;
    about.label = label;
    about.low = 0.0;
    about.high = 1.0;
    about.decimals = 2;
    return about;
}

}  // namespace

TEST_CASE("a slider reports every frame of a drag, and the value it reports is where the hand is") {
    Bench bench;
    const u64 id = id_of("test.slider");
    const Number about = plain();
    f64 setting = 0.25;

    // A press alone changes nothing.
    setting = bench.run(id, about, setting, bench.row.mid_x(), bench.row.mid_y(), true, true);
    CHECK(!bench.wrote);
    CHECK(setting == doctest::Approx(0.25));

    // Drag right by a quarter of the row. A quarter of a range of one is 0.25, so the value arrives
    // at 0.5 — and it arrives DURING the drag, with the button still down.
    setting = bench.run(id, about, setting, bench.row.mid_x() + bench.row.width() * 0.25f,
                        bench.row.mid_y(), true);
    CHECK(bench.wrote);
    CHECK(setting == doctest::Approx(0.5));

    // And the release keeps it there rather than handing back what it started as, which is exactly
    // what the reported bug did.
    setting = bench.run(id, about, setting, bench.row.mid_x() + bench.row.width() * 0.25f,
                        bench.row.mid_y(), false, false, true);
    CHECK(setting == doctest::Approx(0.5));

    // A frame with nothing happening does not move it either.
    setting = bench.run(id, about, setting, bench.row.mid_x(), bench.row.mid_y(), false);
    CHECK(!bench.wrote);
    CHECK(setting == doctest::Approx(0.5));
}

TEST_CASE("a drag that leaves the row is still the same drag") {
    // A hand that slides off the row while holding is still holding. Recomputing from where the
    // pointer IS rather than accumulating per frame is what makes that true, and it is also what
    // makes a drag independent of the frame rate.
    Bench bench;
    const u64 id = id_of("test.slider");
    const Number about = plain();
    f64 setting = 0.5;

    setting = bench.run(id, about, setting, bench.row.mid_x(), bench.row.mid_y(), true, true);

    // Far to the right and well below the row: the value goes to the end of its travel and stays.
    for (i32 step = 0; step < 3; ++step) {
        setting = bench.run(id, about, setting, bench.row.x1 + 500.0f, bench.row.y1 + 200.0f, true);
    }
    CHECK(setting == doctest::Approx(1.0));

    // Back inside, a quarter of the row left of where it was grabbed: it tracks the hand again.
    setting = bench.run(id, about, setting, bench.row.mid_x() - bench.row.width() * 0.25f,
                        bench.row.mid_y(), true);
    CHECK(setting == doctest::Approx(0.25));
}

TEST_CASE("a slider with a step lands on its notches") {
    Bench bench;
    const u64 id = id_of("test.stepped");
    Number about = plain("interface size");
    about.low = 0.0;
    about.high = 6.0;
    about.step = 1.0;
    about.decimals = 0;
    f64 setting = 0.0;

    setting = bench.run(id, about, setting, bench.row.x0 + 1.0f, bench.row.mid_y(), true, true);
    // Half way along a range of six is three, and three is a notch.
    setting = bench.run(id, about, setting, bench.row.mid_x(), bench.row.mid_y(), true);
    CHECK(setting == doctest::Approx(3.0));
    // Not a fraction: a stepped value that comes back as 2.97 spells itself "3" and saves as
    // something else.
    CHECK(setting == std::round(setting));
}

TEST_CASE("a refused value is put back, and the caller is told so it can put it back too") {
    // The second half of D444: a value that would break the game is refused, and the refusal says in
    // one line what it would have done. The caller has to hear about the refusal, or the setting
    // keeps a number the control has already rejected.
    Bench bench;
    const u64 id = id_of("test.guarded");
    Number about = plain("render scale");
    about.guard = [](f64 value) -> std::string {
        return (value < 0.2) ? std::string("a scale under a fifth has nothing left to draw")
                             : std::string();
    };
    f64 setting = 1.0;

    setting = bench.run(id, about, setting, bench.row.x1 - 1.0f, bench.row.mid_y(), true, true);
    setting = bench.run(id, about, setting, bench.row.x0 - 200.0f, bench.row.mid_y(), true);
    CHECK(setting == doctest::Approx(0.0));   // the drag itself is not stopped by the guard

    setting = bench.run(id, about, setting, bench.row.x0 - 200.0f, bench.row.mid_y(), false, false,
                        true);
    // Back to what it was when the hand went down, and the caller was told, so the setting followed.
    CHECK(bench.wrote);
    CHECK(setting == doctest::Approx(1.0));
}

TEST_CASE("a switch answers on the release, like everything else") {
    Bench bench;
    const u64 id = id_of("test.toggle");
    bool value = false;

    bench.hand(bench.row.mid_x(), bench.row.mid_y(), true, true);
    bench.clock += 1.0 / 60.0;
    bench.ui.begin(bench.input, 800, 600, bench.clock, 2.0f);
    CHECK(!bench.ui.toggle(id, bench.row, "sound", value));
    bench.ui.end();
    CHECK(!value);

    bench.hand(bench.row.mid_x(), bench.row.mid_y(), false, false, true);
    bench.clock += 1.0 / 60.0;
    bench.ui.begin(bench.input, 800, 600, bench.clock, 2.0f);
    CHECK(bench.ui.toggle(id, bench.row, "sound", value));
    bench.ui.end();
    CHECK(value);
}
