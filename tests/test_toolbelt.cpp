#include <doctest/doctest.h>

#include "game/repeat.hpp"
#include "game/toolbelt.hpp"

using namespace ws;

TEST_CASE("the chisel is on slot one and the clipboard on slot two") {
    Toolbelt belt;
    CHECK(belt.active_slot() == 0);
    CHECK(belt.active() == ToolKind::Chisel);
    CHECK(belt.slot_size(0) == 1);
    CHECK(belt.slot_size(1) == 1);
    CHECK(belt.select_slot(1));
    CHECK(belt.active() == ToolKind::Clipboard);
    for (u32 slot = 2; slot < kToolSlots; ++slot) CHECK(belt.slot_empty(slot));
}

TEST_CASE("cycling walks the tools on the active slot and wraps both ways") {
    Toolbelt belt;
    // Both on one slot, which is what stacking a slot is for.
    belt.assign(0, ToolKind::Clipboard);
    const u32 count = belt.slot_size(0);
    REQUIRE(count >= 2);

    const ToolKind first = belt.active();
    CHECK(belt.cycle(1));
    CHECK(belt.active() != first);

    // All the way round comes back to where it started.
    for (u32 i = 1; i < count; ++i) belt.cycle(1);
    CHECK(belt.active() == first);

    // And backwards past the front lands on the last.
    CHECK(belt.cycle(-1));
    CHECK(belt.active() == static_cast<ToolKind>(count - 1));
}

TEST_CASE("cycling a slot with one tool on it changes nothing") {
    Toolbelt belt;
    belt.clear(0);
    belt.assign(0, ToolKind::Chisel);
    CHECK_FALSE(belt.cycle(1));
    CHECK(belt.active() == ToolKind::Chisel);
}

TEST_CASE("selecting an empty slot is not a selection") {
    Toolbelt belt;
    CHECK_FALSE(belt.select_slot(4));
    CHECK(belt.active_slot() == 0);   // still on the slot that has something on it

    belt.assign(4, ToolKind::Clipboard);
    CHECK(belt.select_slot(4));
    CHECK(belt.active_slot() == 4);
    CHECK(belt.active() == ToolKind::Clipboard);
}

TEST_CASE("assigning the same tool twice does not duplicate it") {
    Toolbelt belt;
    belt.clear(2);
    belt.assign(2, ToolKind::Chisel);
    belt.assign(2, ToolKind::Chisel);
    CHECK(belt.slot_size(2) == 1);
}

TEST_CASE("selecting the slot already active reports no change") {
    Toolbelt belt;
    CHECK_FALSE(belt.select_slot(0));
}

TEST_CASE("key repeat fires once on press then at a steady rate") {
    KeyRepeat repeat;
    repeat.configure(0.30, 10.0);   // 300 ms delay, then ten a second

    CHECK(repeat.poll(true, 0.0) == 1);       // the press itself
    CHECK(repeat.poll(true, 0.1) == 0);       // still inside the delay
    CHECK(repeat.poll(true, 0.1) == 0);
    CHECK(repeat.poll(true, 0.15) == 1);      // delay elapsed

    // Over the next second it fires at the configured rate. Checked as a total rather than
    // per poll, because the timer carries its overshoot forward on purpose — that is what
    // keeps the average rate exact instead of drifting slower with every fire.
    u32 fired = 0;
    for (i32 i = 0; i < 100; ++i) fired += repeat.poll(true, 0.01);
    CHECK(fired == 10);

    // A long frame owes several, rather than silently dropping to one per frame.
    CHECK(repeat.poll(true, 0.35) >= 3);

    // Releasing resets, so the next press fires immediately again.
    CHECK(repeat.poll(false, 0.1) == 0);
    CHECK(repeat.poll(true, 0.0) == 1);
}

TEST_CASE("key repeat does not bank up an unbounded backlog") {
    // One long stall must not spin a clip through half a turn when the frame recovers.
    KeyRepeat repeat;
    repeat.configure(0.0, 60.0);
    repeat.poll(true, 0.0);
    const u32 fired = repeat.poll(true, 10.0);
    CHECK(fired <= 8);
    CHECK(repeat.poll(true, 0.0) == 0);   // and the excess was dropped, not stored
}
