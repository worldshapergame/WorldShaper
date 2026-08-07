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

TEST_CASE("the idle preview sits where the block would actually go") {
    // O decides whether a placement lands against the face you are aiming at or replaces the
    // voxel itself. The preview has to agree with it: marking the solid voxel under the
    // crosshair while the block appears in the empty one next to it means the one thing the
    // preview exists to show — where matter is about to be — is the one thing it gets wrong.
    World world;
    world.set(10, 0, 0, kRock);
    Chisel chisel;
    REQUIRE(chisel.snapping());

    Op op;
    ChiselInput idle;
    CHECK_FALSE(step(chisel, world, idle, op));
    REQUIRE(chisel.preview().active);
    CHECK(chisel.preview().min[0] == 9);   // against the face, on the near side
    CHECK(chisel.preview().max[0] == 9);

    // Turn it off and the preview moves onto the voxel itself, which is what will be replaced.
    ChiselInput toggle;
    toggle.toggle_anchor = true;
    CHECK_FALSE(step(chisel, world, toggle, op));
    REQUIRE(chisel.preview().active);
    CHECK(chisel.preview().min[0] == 10);
    CHECK(chisel.preview().max[0] == 10);
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

TEST_CASE("snapped placing lands against the face, not inside it") {
    World world;
    world.set(10, 0, 0, kRock);
    Chisel chisel;
    chisel.set_material(kBrickType);

    Op op;
    ChiselInput input;
    input.right = true;
    step(chisel, world, input, op);
    input.right = false;
    REQUIRE(step(chisel, world, input, op));
    CHECK(op.type == kBrickType);
    CHECK(op.reason == MatterReason::PlayerPlace);
    CHECK(op.x0 == 9);   // the empty voxel in front of the rock
    CHECK(op.x1 == 9);
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
