#include <doctest/doctest.h>

#include "game/chisel.hpp"
#include "world/ledger.hpp"
#include "world/world.hpp"

using namespace ws;

namespace {

constexpr VoxelTypeId kRock = 9;
constexpr VoxelTypeId kBrickType = 11;

const f64 kEye[3] = {0.5, 0.5, 0.5};
const f64 kForward[3] = {1.0, 0.0, 0.0};

// One frame of the tool. Returns whether an edit came out of it.
bool step(Chisel& chisel, const World& world, ChiselInput input, Op& out,
          const f64 origin[3] = kEye, const f64 direction[3] = kForward) {
    return chisel.update(world, input, origin, direction, 1, 1, out);
}

}  // namespace

TEST_CASE("the idle preview sits where the tool would actually act") {
    // O decides whether the tool acts on the voxel under the crosshair or on the empty one against
    // its face. The preview has to agree with it: marking one voxel while the edit lands in the
    // other means the one thing the preview exists to show is the one thing it gets wrong.
    World world;
    world.set(10, 0, 0, kRock);
    Chisel chisel;
    REQUIRE(chisel.snapping());
    REQUIRE_FALSE(chisel.places_against_face());   // off by default: act where you point

    Op op;
    ChiselInput idle;
    CHECK_FALSE(step(chisel, world, idle, op));
    REQUIRE(chisel.preview().active);
    CHECK(chisel.preview().min[0] == 10);   // the voxel being aimed at
    CHECK(chisel.preview().max[0] == 10);
    REQUIRE(chisel.preview().has_cursor);
    CHECK(chisel.preview().cursor[0] == 10);

    // Turn it on and everything moves onto the empty neighbour together.
    ChiselInput toggle;
    toggle.toggle_anchor = true;
    CHECK_FALSE(step(chisel, world, toggle, op));
    REQUIRE(chisel.preview().active);
    CHECK(chisel.places_against_face());
    CHECK(chisel.preview().min[0] == 9);
    CHECK(chisel.preview().max[0] == 9);
    CHECK(chisel.preview().cursor[0] == 9);
}

TEST_CASE("snapped carving picks the aimed voxel") {
    World world;
    world.set(10, 0, 0, kRock);
    Chisel chisel;
    REQUIRE(chisel.snapping());

    Op op;
    ChiselInput input;
    input.left = true;
    CHECK_FALSE(step(chisel, world, input, op));   // press: anchor only
    CHECK(chisel.preview().active);
    CHECK(chisel.preview().dragging);

    input.left = false;
    REQUIRE(step(chisel, world, input, op));       // release: the edit
    CHECK(op.type == kAir);
    CHECK(op.reason == MatterReason::PlayerBreak);
    CHECK(op.x0 == 10);
    CHECK(op.x1 == 10);
    CHECK(op.volume() == 1);
}

TEST_CASE("O moves where the tool acts, and it moves BOTH modes") {
    // It used to move placing only, with carving always taking the aimed voxel. A setting that
    // says where the tool acts and then applies to half of it is one nobody can predict from, and
    // it made the two modes disagree about which voxel the crosshair means.
    World world;
    world.set(10, 0, 0, kRock);

    SUBCASE("off, the default: both act on the voxel under the crosshair") {
        Chisel chisel;
        chisel.set_material(kBrickType);
        REQUIRE_FALSE(chisel.places_against_face());

        Op op;
        ChiselInput place;
        place.right = true;
        step(chisel, world, place, op);
        place.right = false;
        REQUIRE(step(chisel, world, place, op));
        CHECK(op.type == kBrickType);
        CHECK(op.x0 == 10);   // replaces what was aimed at
        CHECK(op.x1 == 10);

        Chisel carver;
        Op cut;
        ChiselInput carve;
        carve.left = true;
        step(carver, world, carve, cut);
        carve.left = false;
        REQUIRE(step(carver, world, carve, cut));
        CHECK(cut.type == kAir);
        CHECK(cut.x0 == 10);
        CHECK(cut.x1 == 10);
    }

    SUBCASE("on: both act on the empty voxel against the face") {
        Chisel chisel;
        chisel.set_material(kBrickType);
        Op op;
        ChiselInput on;
        on.toggle_anchor = true;
        step(chisel, world, on, op);
        REQUIRE(chisel.places_against_face());

        ChiselInput place;
        place.right = true;
        step(chisel, world, place, op);
        place.right = false;
        REQUIRE(step(chisel, world, place, op));
        CHECK(op.x0 == 9);   // the empty voxel in front of the rock
        CHECK(op.x1 == 9);

        Chisel carver;
        Op cut;
        ChiselInput carve_on;
        carve_on.toggle_anchor = true;
        step(carver, world, carve_on, cut);
        ChiselInput carve;
        carve.left = true;
        step(carver, world, carve, cut);
        carve.left = false;
        REQUIRE(step(carver, world, carve, cut));
        CHECK(cut.type == kAir);
        CHECK(cut.x0 == 9);   // the same voxel a placement would have used
        CHECK(cut.x1 == 9);
    }
}

TEST_CASE("a drag sweeps a box between where the button went down and came up") {
    World world;
    world.set(10, 0, 0, kRock);
    world.set(10, 8, 0, kRock);
    Chisel chisel;

    Op op;
    ChiselInput input;
    input.left = true;
    step(chisel, world, input, op);   // anchor at (10,0,0)

    // Look higher: the second corner resolves to the other block.
    const f64 up_ray[3] = {10.0, 8.0, 0.0};
    step(chisel, world, input, op, kEye, up_ray);
    CHECK(chisel.preview().volume == 9);   // 1 x 9 x 1

    input.left = false;
    REQUIRE(step(chisel, world, input, op, kEye, up_ray));
    CHECK(op.y0 == 0);
    CHECK(op.y1 == 8);
    CHECK(op.volume() == 9);
}

TEST_CASE("the wheel sets a working distance and leaves snapping behind") {
    World world;
    Chisel chisel;

    Op op;
    ChiselInput input;
    input.adjust_distance = true;
    input.wheel = 5.0f;
    step(chisel, world, input, op);
    CHECK(chisel.distance() == doctest::Approx(5.0));
    CHECK_FALSE(chisel.snapping());

    // The step grows with distance so crossing a hundred metres does not take forever.
    input.wheel = 100.0f;
    step(chisel, world, input, op);
    CHECK(chisel.distance() > 100.0);

    // And it cannot go below zero or past the reach.
    input.wheel = -100000.0f;
    step(chisel, world, input, op);
    CHECK(chisel.distance() == doctest::Approx(0.0));
    CHECK(chisel.snapping());

    input.wheel = 1000000.0f;
    step(chisel, world, input, op);
    CHECK(chisel.distance() == doctest::Approx(chisel.reach()));
}

TEST_CASE("the wheel is ignored unless the modifier is held") {
    World world;
    Chisel chisel;
    Op op;
    ChiselInput input;
    input.wheel = 5.0f;
    input.adjust_distance = false;
    step(chisel, world, input, op);
    CHECK(chisel.distance() == doctest::Approx(0.0));
}

TEST_CASE("at a distance the tool works in open air with nothing to aim at") {
    World world;   // completely empty
    Chisel chisel;

    Op op;
    ChiselInput input;
    input.adjust_distance = true;
    input.wheel = 32.0f;
    step(chisel, world, input, op);
    REQUIRE(chisel.distance() == doctest::Approx(32.0));

    input.wheel = 0.0f;
    input.adjust_distance = false;
    input.right = true;
    step(chisel, world, input, op);
    CHECK(chisel.preview().active);

    input.right = false;
    REQUIRE(step(chisel, world, input, op));
    CHECK(op.x0 == 32);   // 0.5 + 32 along +x
}

TEST_CASE("snapping at the sky produces no preview and no edit") {
    World world;
    Chisel chisel;

    Op op;
    ChiselInput input;
    input.left = true;
    CHECK_FALSE(step(chisel, world, input, op));
    CHECK_FALSE(chisel.preview().active);
    input.left = false;
    CHECK_FALSE(step(chisel, world, input, op));
}

TEST_CASE("constraint points pull the box out to reach them") {
    World world;
    world.set(10, 0, 0, kRock);
    world.set(10, 0, 40, kRock);
    Chisel chisel;

    Op op;
    ChiselInput mark;
    mark.add_point = true;
    const f64 far_ray[3] = {10.0, 0.0, 40.0};
    step(chisel, world, mark, op, kEye, far_ray);
    REQUIRE(chisel.constraints().size() == 1);

    ChiselInput input;
    input.left = true;
    step(chisel, world, input, op);
    input.left = false;
    REQUIRE(step(chisel, world, input, op));
    // The drag alone was one voxel; the constraint stretches the box to include it.
    CHECK(op.z0 == 0);
    CHECK(op.z1 == 40);
    CHECK(chisel.constraints().empty());   // consumed by the edit
}

TEST_CASE("constraint points can be cleared without editing") {
    World world;
    world.set(10, 0, 0, kRock);
    Chisel chisel;

    Op op;
    ChiselInput mark;
    mark.add_point = true;
    step(chisel, world, mark, op);
    CHECK(chisel.constraints().size() == 1);

    ChiselInput clear;
    clear.clear_points = true;
    step(chisel, world, clear, op);
    CHECK(chisel.constraints().empty());
}

TEST_CASE("cancelling a drag makes no edit") {
    World world;
    world.set(10, 0, 0, kRock);
    Chisel chisel;

    Op op;
    ChiselInput input;
    input.left = true;
    step(chisel, world, input, op);
    REQUIRE(chisel.preview().dragging);

    input.cancel = true;
    step(chisel, world, input, op);
    CHECK_FALSE(chisel.preview().dragging);

    input.cancel = false;
    input.left = false;
    CHECK_FALSE(step(chisel, world, input, op));
}

TEST_CASE("a huge edit is carried out rather than refused") {
    // There is no size limit. There was one, and it also capped what the clipboard could
    // select, because selecting is this same drag — so a limit meant for a carve was
    // quietly refusing to copy a large building.
    World world;
    Chisel chisel;

    Op op;
    ChiselInput input;
    input.adjust_distance = true;
    input.wheel = 100000.0f;   // out to the reach
    step(chisel, world, input, op);

    input.wheel = 0.0f;
    input.adjust_distance = false;
    input.left = true;
    step(chisel, world, input, op);   // anchor far away on +x

    // Away on two more axes, so the box spans thousands of voxels on all three.
    const f64 other[3] = {0.0, 1.0, 1.0};
    input.left = false;
    REQUIRE(step(chisel, world, input, op, kEye, other));
    CHECK(op.volume() > 1000000000ull);
}

TEST_CASE("holding both buttons does not start two drags") {
    World world;
    world.set(10, 0, 0, kRock);
    Chisel chisel;

    Op op;
    ChiselInput input;
    input.left = true;
    input.right = true;
    step(chisel, world, input, op);
    CHECK(chisel.preview().mode == ChiselMode::Carve);   // left wins, and it stays carve

    input.right = false;
    step(chisel, world, input, op);
    CHECK(chisel.preview().mode == ChiselMode::Carve);
    input.left = false;
    REQUIRE(step(chisel, world, input, op));
    CHECK(op.type == kAir);
}

// ---- R11e and R11h ---------------------------------------------------------------------------

TEST_CASE("an edit's bounds are the union of every op in the group") {
    // What has to be sampled before a cut is the volume the WHOLE group touches. `hollow_box`
    // turns one carve into six slabs and the clipboard produces a run of them, so asking each op
    // on its own would sample the middle of a hollow shell that nothing is going to write to.
    std::vector<Op> ops;
    i64 low[3]{};
    i64 high[3]{};
    CHECK_FALSE(edit_bounds(ops, low, high));   // empty is not a box of nought volume

    ops.push_back(Op::fill_box(1, 1, 10, 10, 10, 12, 12, 12, kRock, MatterReason::PlayerPlace));
    REQUIRE(edit_bounds(ops, low, high));
    CHECK(low[0] == 10);
    CHECK(high[2] == 12);

    // Corners the other way round: `edit_bounds` normalises, because an op is allowed to carry
    // them in either order and two peers describing one box differently must agree here.
    ops.push_back(Op::fill_box(2, 1, 40, -5, 3, 20, -9, 1, kAir, MatterReason::PlayerBreak));
    REQUIRE(edit_bounds(ops, low, high));
    CHECK(low[0] == 10);
    CHECK(low[1] == -9);
    CHECK(low[2] == 1);
    CHECK(high[0] == 40);
    CHECK(high[1] == 12);
    CHECK(high[2] == 12);
}

TEST_CASE("the proximity radius decides whether anything guaranteed the edit's detail") {
    // Twenty metres is 640 voxels (D199, R2c). Inside it the ladder has already been obliged to
    // hold the volume; outside it nothing has, which is the whole of R11h's far case.
    const f64 radius = kProximityMetres * 32.0;

    const i64 low[3] = {1000, 0, 0};
    const i64 high[3] = {1007, 7, 7};

    // Standing on it.
    const f64 close[3] = {1004.0, 4.0, 4.0};
    CHECK_FALSE(edit_beyond_proximity(low, high, close, radius));

    // Six hundred voxels away on one axis: inside, just.
    const f64 near_edge[3] = {400.0, 4.0, 4.0};
    CHECK_FALSE(edit_beyond_proximity(low, high, near_edge, radius));

    // Sixty metres out, which is the arm that has never been run.
    const f64 far_off[3] = {1000.0 - 60.0 * 32.0, 4.0, 4.0};
    CHECK(edit_beyond_proximity(low, high, far_off, radius));

    // Diagonal: the distance is to the BOX and not along any one axis, so three components each
    // inside the radius can still be outside it together.
    const f64 corner[3] = {1000.0 - 500.0, -500.0, -500.0};
    CHECK(edit_beyond_proximity(low, high, corner, radius));

    // A radius of nought is the guarantee switched off, and off has to cover nothing -- otherwise
    // every edit would read as "inside" and skip the pre-sample.
    CHECK(edit_beyond_proximity(low, high, close, 0.0));
}

// R11e's own two -- `SampleGate` and `feedback_ray_class` -- are NOT tested here, and it is worth
// saying why rather than leaving the omission to be noticed. They live in `src/gpu/feedback.hpp`,
// which reaches Vulkan through `gpu/buffer.hpp`, and this suite links no Vulkan (see the note at
// the top of `tests/test_field_gpu.cpp`, which makes the same choice for the same reason). A copy
// of the classification written here to have something to assert against would be the trap the
// handover names: three checks agreeing because all three read the same wrong source. What stands
// in for it is the counted run, which is what R11e's gate asks for in the first place -- the
// `light paths:` line beside the settle line, on `clips/sealed_dark.clip`, in both arms of
// `--no-light-sampling-guard`.
