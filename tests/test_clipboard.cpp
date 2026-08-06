#include <doctest/doctest.h>

#include <cmath>

#include "game/clipboard.hpp"
#include "world/ledger.hpp"
#include "world/world.hpp"

using namespace ws;

namespace {

constexpr VoxelTypeId kRock = 9;

const f64 kEye[3] = {0.5, 0.5, 0.5};
const f64 kForward[3] = {1.0, 0.0, 0.0};   // looking down +x

bool step(Clipboard& board, const World& world, ClipboardInput input, std::vector<Op>& out,
          const f64 direction[3] = kForward) {
    return board.update(world, input, kEye, direction, 1.0 / 60.0, 1, 1, out);
}

// Drives the selection drag to completion. The selection is the chisel, so it wants the
// same sequence: set a distance, press, aim somewhere else, release. Aiming elsewhere is
// what makes the box a box rather than a single voxel.
void select_box(Clipboard& board, const World& world, f64 distance) {
    std::vector<Op> out;
    ClipboardInput reach;
    reach.adjust_distance = true;
    reach.wheel = static_cast<f32>(distance);
    step(board, world, reach, out);

    ClipboardInput drag;
    drag.left = true;
    step(board, world, drag, out);

    // Same distance, different direction: the far corner lands well away on all three axes.
    const f64 diagonal[3] = {0.577, 0.577, 0.577};
    step(board, world, drag, out, diagonal);
    drag.left = false;
    step(board, world, drag, out, diagonal);
}

World filled_world() {
    World world;
    for (i64 z = -2; z <= 2; ++z) {
        for (i64 y = -2; y <= 2; ++y) {
            for (i64 x = 0; x <= 40; ++x) world.set(x, y, z, kRock);
        }
    }
    return world;
}

}  // namespace

TEST_CASE("a selection drag captures a clip and stops selecting") {
    World world = filled_world();
    Clipboard board;
    CHECK_FALSE(board.holding());

    select_box(board, world, 8.0);
    CHECK(board.holding());
    CHECK(board.clip().cell_count() > 0);
    CHECK(board.preview().holding == false);   // the preview is built on the next frame

    std::vector<Op> out;
    ClipboardInput idle;
    step(board, world, idle, out);
    CHECK(board.preview().holding);
    CHECK(board.preview().instances == 1);
}

TEST_CASE("the wheel slides the ghost along the axis you are facing") {
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);

    std::vector<Op> out;
    ClipboardInput move;
    move.wheel = 5.0f;
    step(board, world, move, out);
    CHECK(board.offset()[0] == 5);
    CHECK(board.offset()[1] == 0);
    CHECK(board.offset()[2] == 0);

    // Facing the other way sends it the other way, without any key changing.
    const f64 backwards[3] = {-1.0, 0.0, 0.0};
    step(board, world, move, out, backwards);
    CHECK(board.offset()[0] == 0);

    // And looking up moves it up.
    const f64 upwards[3] = {0.1, 1.0, 0.0};
    step(board, world, move, out, upwards);
    CHECK(board.offset()[1] == 5);
}

TEST_CASE("shift moves the ghost a whole clip length at a time") {
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);
    const i32 length = board.clip().size[0];

    std::vector<Op> out;
    ClipboardInput move;
    move.wheel = 1.0f;
    move.big_step = true;
    step(board, world, move, out);
    CHECK(board.offset()[0] == length);
}

TEST_CASE("copies are spread evenly and the last one lands on the ghost") {
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);

    std::vector<Op> out;
    ClipboardInput move;
    move.wheel = 12.0f;
    step(board, world, move, out);
    REQUIRE(board.offset()[0] == 12);

    ClipboardInput more;
    more.increase = 3;   // 1 -> 4
    step(board, world, more, out);
    REQUIRE(board.copies() == 4);

    // Quarters of the way along, with the fourth landing exactly on the ghost. Measured as
    // gaps from the first copy, so the test does not need to know where the source was.
    i64 at[4][3];
    for (u32 n = 0; n < 4; ++n) board.instance_origin(n, at[n]);
    CHECK(at[1][0] - at[0][0] == 3);
    CHECK(at[2][0] - at[1][0] == 3);
    CHECK(at[3][0] - at[2][0] == 3);
    CHECK(at[3][0] - at[0][0] == 9);   // and the last is 12 from the source, not 9
}

TEST_CASE("winding the copy count down past one carries on the other side of the ghost") {
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);
    REQUIRE(board.copies() == 1);
    REQUIRE(board.instance_count() == 1);

    std::vector<Op> out;
    ClipboardInput fewer;
    fewer.decrease = 1;
    step(board, world, fewer, out);
    // Zero copies is what dropping the clip is for, so the count steps straight over it.
    CHECK(board.copies() == -1);
    CHECK(board.instance_count() == 2);   // the ghost, plus one past it

    fewer.decrease = 2;
    step(board, world, fewer, out);
    CHECK(board.copies() == -3);
    CHECK(board.instance_count() == 4);

    // And back up through zero the same way.
    ClipboardInput more;
    more.increase = 3;
    step(board, world, more, out);
    CHECK(board.copies() == 1);
}

TEST_CASE("copies past the ghost repeat the whole step, and reach further as you add them") {
    // The offset is a *stride*, not a destination. Each copy takes it again from where the
    // last one ended, so adding one extends the row rather than packing another into the
    // same span.
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);

    std::vector<Op> out;
    ClipboardInput move;
    move.wheel = 40.0f;
    step(board, world, move, out);
    REQUIRE(board.offset()[0] == 40);

    ClipboardInput fewer;
    fewer.decrease = 4;   // 1 -> -4: five in the row
    step(board, world, fewer, out);
    REQUIRE(board.instance_count() == 5);

    i64 at[5][3];
    for (u32 n = 0; n < 5; ++n) board.instance_origin(n, at[n]);
    for (u32 n = 0; n < 5; ++n) {
        CHECK(at[n][0] - board.anchor()[0] == static_cast<i64>(n + 1) * 40);
        CHECK(at[n][1] == board.anchor()[1]);
        CHECK(at[n][2] == board.anchor()[2]);
    }

    // One more copy: the row gets longer, and nothing already in it moves.
    const i64 was_the_end = at[4][0];
    ClipboardInput one_more;
    one_more.decrease = 1;
    step(board, world, one_more, out);
    REQUIRE(board.instance_count() == 6);

    i64 now[6][3];
    for (u32 n = 0; n < 6; ++n) board.instance_origin(n, now[n]);
    for (u32 n = 0; n < 5; ++n) CHECK(now[n][0] == at[n][0]);
    CHECK(now[5][0] == was_the_end + 40);
}

TEST_CASE("extra copies bend round when the ghost is turned as well as moved") {
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);

    std::vector<Op> out;
    ClipboardInput move;
    move.wheel = 20.0f;   // straight down +x
    step(board, world, move, out);

    // A quarter turn about y on the ghost: each further step is turned by another quarter,
    // so four steps close a square rather than running off in a straight line.
    ClipboardInput turn;
    turn.turn_right = 1;
    for (i32 i = 0; i < 12; ++i) step(board, world, turn, out);
    REQUIRE(board.angles_degrees()[1] == doctest::Approx(90.0));

    ClipboardInput fewer;
    fewer.decrease = 4;
    step(board, world, fewer, out);
    REQUIRE(board.instance_count() == 5);

    i64 at[5][3];
    for (u32 n = 0; n < 5; ++n) board.instance_origin(n, at[n]);

    // Each leg is the same stride, turned by one more quarter than the last: +x, -z, -x, +z.
    // Four of them close a square, which is the bend at its most obvious.
    const i64 legs[4][3] = {{0, 0, -20}, {-20, 0, 0}, {0, 0, 20}, {20, 0, 0}};
    for (u32 n = 1; n < 5; ++n) {
        for (u32 a = 0; a < 3; ++a) CHECK(at[n][a] - at[n - 1][a] == legs[n - 1][a]);
    }
    // The fourth step brings it back to where the original sits, and the fifth sets off
    // round again — the row is a loop, not a line, because the step turns.
    CHECK(at[3][0] == board.anchor()[0]);
    CHECK(at[3][2] == board.anchor()[2]);
    CHECK(at[4][0] == at[0][0]);
    CHECK(at[4][2] == at[0][2]);
}

TEST_CASE("turning the clip keeps every voxel") {
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);
    const u64 solid = board.clip().solid_count();

    std::vector<Op> out;
    for (i32 i = 0; i < 12; ++i) {
        ClipboardInput turn;
        turn.turn_right = 1;
        step(board, world, turn, out);
        REQUIRE(board.clip().solid_count() == solid);
    }
    // Twelve steps of 7.5 degrees is a quarter turn, and it is baked from the original
    // every time rather than from the previous result, so the shape has not degraded.
    CHECK(board.angles_degrees()[1] == doctest::Approx(90.0));
}

TEST_CASE("the grid snap rounds the offset to whole clip lengths") {
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);
    const i64 length = board.clip().size[0];
    REQUIRE(length > 1);

    std::vector<Op> out;
    ClipboardInput move;
    move.wheel = static_cast<f32>(length + 1);   // deliberately not a whole length
    step(board, world, move, out);
    CHECK(board.offset()[0] == length + 1);

    ClipboardInput snap;
    snap.toggle_snap = true;
    step(board, world, snap, out);
    CHECK(board.offset()[0] % length == 0);
}

TEST_CASE("the paste mode cycles through all three") {
    World world = filled_world();
    Clipboard board;
    std::vector<Op> out;
    const PasteMode start = board.paste_mode();

    ClipboardInput cycle;
    cycle.toggle_paste_mode = true;
    for (u32 i = 0; i < static_cast<u32>(PasteMode::Count); ++i) {
        step(board, world, cycle, out);
    }
    CHECK(board.paste_mode() == start);
}

TEST_CASE("clicking stamps the clip and every copy lands") {
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);
    const Clip captured = board.clip();
    REQUIRE(captured.solid_count() > 0);

    std::vector<Op> out;
    ClipboardInput move;
    move.wheel = 30.0f;
    step(board, world, move, out);

    ClipboardInput more;
    more.increase = 2;   // three copies
    step(board, world, more, out);
    REQUIRE(board.copies() == 3);

    ClipboardInput click;
    click.left = true;
    REQUIRE(step(board, world, click, out));
    CHECK(!out.empty());

    World target;
    MatterLedger ledger;
    apply_ops(target, out, ledger);
    CHECK(target.stats().solid_voxels == captured.solid_count() * 3);
    // And it is still held, so it can be stamped again.
    CHECK(board.holding());
}

TEST_CASE("dropping the clip goes back to selecting") {
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);
    REQUIRE(board.holding());

    std::vector<Op> out;
    ClipboardInput clear;
    clear.clear_points = true;
    step(board, world, clear, out);
    CHECK_FALSE(board.holding());
    CHECK(board.copies() == 1);
}

TEST_CASE("cancelling drops the clip without stamping") {
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);

    std::vector<Op> out;
    ClipboardInput cancel;
    cancel.cancel = true;
    CHECK_FALSE(step(board, world, cancel, out));
    CHECK_FALSE(board.holding());
}

TEST_CASE("the right button abandons the ghost") {
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);
    REQUIRE(board.holding());

    std::vector<Op> out;
    ClipboardInput click;
    click.right = true;
    CHECK_FALSE(step(board, world, click, out));
    CHECK_FALSE(board.holding());
}

TEST_CASE("the right button abandons a selection in progress") {
    World world = filled_world();
    Clipboard board;

    std::vector<Op> out;
    ClipboardInput drag;
    drag.left = true;
    drag.adjust_distance = true;
    drag.wheel = 8.0f;
    step(board, world, drag, out);

    ClipboardInput abandon;
    abandon.left = true;
    abandon.right = true;
    step(board, world, abandon, out);

    // Letting go now must not stamp a box, because the drag was called off.
    ClipboardInput release;
    CHECK_FALSE(step(board, world, release, out));
    CHECK_FALSE(board.holding());
}

TEST_CASE("scrolling the same way gathers speed and reversing gives it up") {
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);

    std::vector<Op> out;
    ClipboardInput tick;
    tick.wheel = 1.0f;

    // The first click of a run is always exactly one voxel; acceleration you cannot escape
    // would make lining a ghost up to the voxel impossible.
    step(board, world, tick, out);
    CHECK(board.offset()[0] == 1);

    i64 previous = board.offset()[0];
    i64 last_gap = 1;
    for (i32 i = 0; i < 30; ++i) {
        step(board, world, tick, out);
        const i64 gap = board.offset()[0] - previous;
        CHECK(gap >= last_gap);   // never slows down while the run continues
        previous = board.offset()[0];
        last_gap = gap;
    }
    CHECK(last_gap > 4);   // and it did get meaningfully faster

    // Reversing starts over at one voxel a click.
    ClipboardInput back;
    back.wheel = -1.0f;
    const i64 before = board.offset()[0];
    step(board, world, back, out);
    CHECK(board.offset()[0] == before - 1);
}

TEST_CASE("not scrolling lets the speed cool off") {
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);

    std::vector<Op> out;
    ClipboardInput tick;
    tick.wheel = 1.0f;
    for (i32 i = 0; i < 30; ++i) step(board, world, tick, out);

    // The run survives the gaps between notches — that is what a fast scroll looks like —
    // but a second and a half of nothing puts it back to one voxel a click.
    ClipboardInput idle;
    for (i32 i = 0; i < 90; ++i) step(board, world, idle, out);

    const i64 before = board.offset()[0];
    step(board, world, tick, out);
    CHECK(board.offset()[0] == before + 1);
}

TEST_CASE("the run survives the idle frames between wheel notches") {
    // The bug this replaced: the speed decayed on every frame the wheel was quiet, and a
    // wheel is quiet between notches, so it never sped up at all.
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);

    std::vector<Op> out;
    ClipboardInput tick;
    tick.wheel = 1.0f;
    ClipboardInput idle;

    i64 previous = board.offset()[0];
    i64 gap = 0;
    for (i32 notch = 0; notch < 40; ++notch) {
        step(board, world, tick, out);
        gap = board.offset()[0] - previous;
        previous = board.offset()[0];
        // Three quiet frames between notches, which is what scrolling hard actually gives.
        for (i32 i = 0; i < 3; ++i) step(board, world, idle, out);
    }
    CHECK(gap > 4);
}

TEST_CASE("middle click sends the ghost to what you are looking at") {
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);

    std::vector<Op> out;
    ClipboardInput idle;
    step(board, world, idle, out);
    const i64 before[3] = {board.preview().min[0][0], board.preview().min[0][1],
                           board.preview().min[0][2]};

    // Aim down +x at the slab, which starts at x = 0 and runs to x = 40.
    ClipboardInput teleport;
    teleport.middle = true;
    step(board, world, teleport, out);

    const i64 after = board.preview().min[0][0];
    CHECK(after != before[0]);
    // It landed against the face it was aimed at, not inside the wall.
    CHECK(after <= 0);
}

TEST_CASE("copies share the transform, arriving at the full amount on the last") {
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);

    std::vector<Op> out;
    ClipboardInput more;
    more.increase = 3;   // four copies
    step(board, world, more, out);
    REQUIRE(board.copies() == 4);

    // A quarter turn on the last copy means an eighth, a quarter, three eighths, a half of
    // it on the way there — four distinct shapes, not four of the same one.
    for (i32 i = 0; i < 12; ++i) {
        ClipboardInput turn;
        turn.turn_right = 1;
        step(board, world, turn, out);
    }
    CHECK(board.angles_degrees()[1] == doctest::Approx(90.0));
    CHECK(board.shapes().size() == 4);

    // Every one of them still holds exactly the voxels it started with.
    const u64 solid = board.shapes().front().solid_count();
    for (const Clip& shape : board.shapes()) CHECK(shape.solid_count() == solid);
}

TEST_CASE("with no transform every copy shares one shape") {
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);

    std::vector<Op> out;
    ClipboardInput more;
    more.increase = 7;
    step(board, world, more, out);
    REQUIRE(board.copies() == 8);
    CHECK(board.shapes().size() == 1);   // one bake, one upload
}

TEST_CASE("the adjust mode cycles between copies and size") {
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);

    std::vector<Op> out;
    CHECK(board.adjust_mode() == AdjustMode::Copies);

    ClipboardInput cycle;
    cycle.cycle_adjust = true;
    step(board, world, cycle, out);
    CHECK(board.adjust_mode() == AdjustMode::Resize);

    // In resize, "." changes the size rather than adding copies.
    ClipboardInput grow;
    grow.increase = 4;
    step(board, world, grow, out);
    CHECK(board.copies() == 1);
    CHECK(board.scale()[0] == doctest::Approx(std::pow(Clipboard::kResizeStep, 4.0)));
    CHECK(board.scale()[1] == doctest::Approx(board.scale()[0]));
    CHECK(board.scale()[2] == doctest::Approx(board.scale()[0]));

    step(board, world, cycle, out);
    CHECK(board.adjust_mode() == AdjustMode::Copies);
}

TEST_CASE("resizing is fine-grained enough to land on an awkward size") {
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);

    std::vector<Op> out;
    ClipboardInput cycle;
    cycle.cycle_adjust = true;
    step(board, world, cycle, out);
    REQUIRE(board.adjust_mode() == AdjustMode::Resize);

    // 4.5x is not a whole number and has to be reachable; the step is small enough that
    // some number of taps lands within a percent of it.
    ClipboardInput grow;
    grow.increase = 1;
    for (i32 i = 0; i < 200 && board.scale()[0] < 4.5; ++i) step(board, world, grow, out);
    CHECK(board.scale()[0] > 4.4);
    CHECK(board.scale()[0] < 4.6);
}

TEST_CASE("in resize the up and down arrows stretch the axis you are facing") {
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);

    std::vector<Op> out;
    ClipboardInput cycle;
    cycle.cycle_adjust = true;
    step(board, world, cycle, out);
    REQUIRE(board.adjust_mode() == AdjustMode::Resize);

    // Facing +x: up stretches x and leaves the other two alone.
    ClipboardInput up;
    up.turn_up = 3;
    step(board, world, up, out);
    CHECK(board.scale()[0] > 1.0);
    CHECK(board.scale()[1] == doctest::Approx(1.0));
    CHECK(board.scale()[2] == doctest::Approx(1.0));
    // And it did not turn the clip, which is what those keys do in the other mode.
    CHECK(board.angles_degrees()[0] == doctest::Approx(0.0));

    // Facing up, the same keys stretch y instead.
    const f64 upwards[3] = {0.1, 1.0, 0.0};
    step(board, world, up, out, upwards);
    CHECK(board.scale()[1] > 1.0);
}

TEST_CASE("stretching follows the world axis you face, even on a turned clip") {
    // The order the transforms are applied in decides this. Resizing *before* rotating
    // stretches the clip's own axes, which after a quarter turn point somewhere else — so
    // looking down one axis and pressing up made the clip grow along a different one.
    // Turning first and resizing second means the stretch is in world axes, which is what
    // "the direction you are looking" means.
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);

    std::vector<Op> out;
    ClipboardInput cycle;
    cycle.cycle_adjust = true;

    // A quarter turn about y first: twelve presses of 7.5 degrees.
    ClipboardInput turn;
    turn.turn_right = 1;
    for (i32 i = 0; i < 12; ++i) step(board, world, turn, out);

    step(board, world, cycle, out);
    REQUIRE(board.adjust_mode() == AdjustMode::Resize);

    const i32 before[3] = {board.clip().size[0], board.clip().size[1], board.clip().size[2]};
    // Facing +x, so up must make the clip wider in x and leave y and z alone.
    ClipboardInput up;
    up.turn_up = 30;
    step(board, world, up, out);
    CHECK(board.clip().size[0] > before[0]);
    CHECK(board.clip().size[1] == before[1]);
    CHECK(board.clip().size[2] == before[2]);

    // Facing +z instead, the same key grows z.
    const f64 forward_z[3] = {0.0, 0.0, 1.0};
    const i32 middle[3] = {board.clip().size[0], board.clip().size[1], board.clip().size[2]};
    step(board, world, up, out, forward_z);
    CHECK(board.clip().size[2] > middle[2]);
    CHECK(board.clip().size[0] == middle[0]);
}

TEST_CASE("copies stretch along the ramp the way they turn along it") {
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);

    std::vector<Op> out;
    ClipboardInput more;
    more.increase = 3;   // four copies
    step(board, world, more, out);
    REQUIRE(board.copies() == 4);

    ClipboardInput cycle;
    cycle.cycle_adjust = true;
    step(board, world, cycle, out);

    ClipboardInput grow;
    grow.increase = 40;
    step(board, world, grow, out);

    // Four distinct shapes, each bigger than the one before, with the last carrying the
    // full amount — the same ramp rotation gets.
    REQUIRE(board.shapes().size() == 4);
    for (usize i = 1; i < board.shapes().size(); ++i) {
        REQUIRE(board.shapes()[i].cell_count() > board.shapes()[i - 1].cell_count());
    }
    CHECK(board.shapes().back().size[0] > board.shapes().front().size[0]);
}


TEST_CASE("there is no cap on how many copies there can be") {
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);

    std::vector<Op> out;
    ClipboardInput more;
    more.increase = 500;
    step(board, world, more, out);
    CHECK(board.copies() == 501);
    CHECK(board.instance_count() == 501);

    // Only the drawable ones are baked and held; the rest are baked at stamp time and let go
    // again, so five hundred copies do not mean five hundred clips sitting in memory.
    CHECK(board.preview().total == 501);
    CHECK(board.preview().instances <= kMaxPreviewInstances);
    CHECK(board.shapes().size() <= kMaxPreviewInstances);
}

TEST_CASE("the copy being steered is always one of the ones drawn") {
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);

    std::vector<Op> out;
    ClipboardInput move;
    move.wheel = 60.0f;
    step(board, world, move, out);
    ClipboardInput more;
    more.increase = 200;
    step(board, world, more, out);
    REQUIRE(board.instance_count() > kMaxPreviewInstances);

    // The last ghost drawn has to be the last copy, or the tool has no cursor.
    i64 last[3];
    board.instance_origin(board.instance_count() - 1, last);
    const u32 slot = board.preview().instances - 1;
    for (u32 a = 0; a < 3; ++a) CHECK(board.preview().min[slot][a] == last[a]);
}

TEST_CASE("every copy is stamped, drawn or not") {
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);
    const u64 solid = board.clip().solid_count();
    REQUIRE(solid > 0);

    std::vector<Op> out;
    ClipboardInput move;
    move.wheel = 800.0f;   // far enough apart that the copies cannot overlap each other
    step(board, world, move, out);

    ClipboardInput more;
    more.increase = 39;   // forty copies, well past what is drawn
    step(board, world, more, out);
    REQUIRE(board.instance_count() == 40);
    REQUIRE(board.clip().size[0] < 20);   // spacing is 20, so nothing overlaps

    ClipboardInput click;
    click.left = true;
    REQUIRE(step(board, world, click, out));

    World target;
    MatterLedger ledger;
    apply_ops(target, out, ledger);
    CHECK(target.stats().solid_voxels == solid * 40);
}

TEST_CASE("there is no cap on how far a clip can be resized") {
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);
    const i32 before = board.clip().size[0];

    std::vector<Op> out;
    ClipboardInput cycle;
    cycle.cycle_adjust = true;
    step(board, world, cycle, out);

    ClipboardInput grow;
    grow.increase = 120;   // well past what the old ceiling allowed
    step(board, world, grow, out);
    CHECK(board.scale()[0] > 10.0);
    CHECK(board.clip().size[0] > before * 10);
    CHECK_FALSE(board.baking_truncated());
}

TEST_CASE("the copy at the far end is the outlined one, and it is the last") {
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);

    std::vector<Op> out;
    ClipboardInput move;
    move.wheel = 40.0f;
    step(board, world, move, out);
    const i64 stride = board.offset()[0];

    ClipboardInput fewer;
    fewer.decrease = 4;   // 1 -> -4: five in the row
    step(board, world, fewer, out);
    REQUIRE(board.instance_count() == 5);

    // The row runs outward, and the last one is furthest — that is the end drawn with an
    // outline, and the end that moves most when the stride changes.
    i64 at[5][3];
    for (u32 n = 0; n < 5; ++n) board.instance_origin(n, at[n]);
    for (u32 n = 1; n < 5; ++n) CHECK(at[n][0] > at[n - 1][0]);
    CHECK(at[4][0] == board.anchor()[0] + stride * 5);
    // And the preview draws them in the same order, so the last slot is the last copy.
    const u32 slot = board.preview().instances - 1;
    for (u32 a = 0; a < 3; ++a) CHECK(board.preview().min[slot][a] == at[4][a]);
}

TEST_CASE("an axis cannot be squashed out of existence") {
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);

    std::vector<Op> out;
    ClipboardInput cycle;
    cycle.cycle_adjust = true;
    step(board, world, cycle, out);

    ClipboardInput shrink;
    shrink.decrease = 8;
    for (i32 i = 0; i < 200; ++i) step(board, world, shrink, out);
    for (u32 a = 0; a < 3; ++a) REQUIRE(board.clip().size[a] >= 1);
}

TEST_CASE("the snap changes the turn step and the spacing, not the kind of rotation") {
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);

    // A twelfth of a quarter turn free, an eighth when snapped. Both divide 90 evenly, so
    // a right angle is exactly reachable either way.
    CHECK(board.turn_step_degrees() == doctest::Approx(7.5));
    std::vector<Op> out;
    ClipboardInput snap;
    snap.toggle_snap = true;
    step(board, world, snap, out);
    REQUIRE(board.grid_snap());
    CHECK(board.turn_step_degrees() == doctest::Approx(11.25));
}

TEST_CASE("twelve presses is exactly a quarter turn, and it keeps going past a full one") {
    // The fold that turns a big angle into quarter turns plus a small remainder used to
    // mask the quarter count before subtracting it, so anything past 315 degrees was handed
    // to the shears whole. On screen that was a row of copies winding smoothly and then
    // collapsing all at once.
    World world = filled_world();
    Clipboard board;
    select_box(board, world, 8.0);
    const u64 solid = board.clip().solid_count();
    const i32 upright[3] = {board.clip().size[0], board.clip().size[1], board.clip().size[2]};

    std::vector<Op> out;
    ClipboardInput turn;
    turn.turn_right = 1;
    for (i32 press = 1; press <= 96; ++press) {   // two full turns, 7.5 degrees at a time
        step(board, world, turn, out);
        REQUIRE(board.clip().solid_count() == solid);
        if (press % 12 == 0) {
            // Every quarter turn is a permutation of the axes, so the extents come back
            // exactly rather than drifting or blowing up.
            const bool swapped = (press % 24) == 12;
            REQUIRE(board.clip().size[1] == upright[1]);
            REQUIRE(board.clip().size[0] == (swapped ? upright[2] : upright[0]));
            REQUIRE(board.clip().size[2] == (swapped ? upright[0] : upright[2]));
        }
    }
    // And the angle kept climbing rather than wrapping, which is what a row of copies needs
    // in order to spiral past one revolution.
    CHECK(board.angles_degrees()[1] == doctest::Approx(720.0));
}

TEST_CASE("a selection past the clip limit is refused rather than captured") {
    World world = filled_world();
    Clipboard board;

    std::vector<Op> out;
    ClipboardInput reach;
    reach.adjust_distance = true;
    reach.wheel = 100000.0f;   // out to the chisel's full reach
    step(board, world, reach, out);

    ClipboardInput drag;
    drag.left = true;
    step(board, world, drag, out);
    const f64 other[3] = {0.0, 1.0, 1.0};
    drag.left = false;
    step(board, world, drag, out, other);
    CHECK_FALSE(board.holding());
}
