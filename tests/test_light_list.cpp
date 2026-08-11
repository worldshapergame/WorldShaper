// The list of emitters the tracer aims at. If this is wrong, rays are sent at lamps that are
// not there and none at the ones that are — and the symptom is noise, which is the hardest
// thing to trace back to its cause.

#include <doctest/doctest.h>

#include <cmath>

#include "world/light_list.hpp"
#include "world/ledger.hpp"
#include "world/op.hpp"
#include "world/world.hpp"

using namespace ws;

namespace {

VoxelTypeId make_lamp(VoxelTypeTable& types, u8 strength) {
    VisualRecord visual;
    visual.red = 255;
    visual.green = 240;
    visual.blue = 200;
    visual.emissive = strength;
    visual.emissive_tint = 0xFFFF;
    return types.intern(visual, BehaviourRecord{});
}

VoxelTypeId make_stone(VoxelTypeTable& types) {
    VisualRecord visual;
    visual.red = 120;
    visual.green = 120;
    visual.blue = 120;
    return types.intern(visual, BehaviourRecord{});
}

void fill(World& world, i64 x0, i64 y0, i64 z0, i64 x1, i64 y1, i64 z1, VoxelTypeId type) {
    MatterLedger ledger;
    apply_op(world, Op::fill_box(1, 1, x0, y0, z0, x1, y1, z1, type, MatterReason::Generation),
             ledger);
}

// The sphere the shader will draw round a light, from the only field it has to go on. Written
// out here rather than shared with the builder on purpose: if the two formulas ever drift the
// tests below should notice, and they cannot notice a constant they were handed.
f64 shader_radius(const LightSource& light) {
    return 0.87 * std::cbrt(static_cast<f64>(light.voxels));
}

// How far the furthest corner of a box of these dimensions is from its middle.
f64 half_diagonal(f64 nx, f64 ny, f64 nz) {
    return 0.5 * std::sqrt(nx * nx + ny * ny + nz * nz);
}

}  // namespace

TEST_CASE("a world with no emitters has no lights") {
    VoxelTypeTable types;
    World world;
    fill(world, 0, 0, 0, 8, 8, 8, make_stone(types));
    CHECK(build_light_list(world, types, 0, 0, 0).empty());
}

TEST_CASE("an emissive voxel becomes a light where it stands") {
    VoxelTypeTable types;
    World world;
    fill(world, 10, 20, 30, 10, 20, 30, make_lamp(types, 200));

    const std::vector<LightSource> lights = build_light_list(world, types, 0, 0, 0);
    REQUIRE(lights.size() == 1);
    CHECK(lights[0].x == 10);
    CHECK(lights[0].y == 20);
    CHECK(lights[0].z == 30);
    CHECK(lights[0].voxels == 1);
    CHECK(lights[0].red > 0.0f);
}

TEST_CASE("a lamp of many voxels is one light, not many") {
    // The whole reason for clustering: a fitting is several voxels, and sampling each of them
    // separately would spend the entire ray budget lighting one lamp.
    VoxelTypeTable types;
    World world;
    fill(world, 0, 0, 0, 3, 3, 3, make_lamp(types, 200));   // 64 voxels

    const std::vector<LightSource> lights = build_light_list(world, types, 0, 0, 0);
    CHECK(lights.size() == 1);
    CHECK(lights[0].voxels == 64);
    // Its position is the middle of the fitting rather than a corner of it.
    CHECK(lights[0].x >= 0);
    CHECK(lights[0].x <= 3);
}

TEST_CASE("lamps far apart stay separate lights") {
    VoxelTypeTable types;
    World world;
    const VoxelTypeId lamp = make_lamp(types, 200);
    fill(world, 0, 0, 0, 0, 0, 0, lamp);
    fill(world, 64, 0, 0, 64, 0, 0, lamp);
    fill(world, 0, 64, 0, 0, 64, 0, lamp);

    CHECK(build_light_list(world, types, 0, 0, 0).size() == 3);
}

TEST_CASE("stone standing next to a lamp is not a light") {
    VoxelTypeTable types;
    World world;
    fill(world, 0, 0, 0, 7, 7, 7, make_stone(types));
    fill(world, 3, 3, 3, 3, 3, 3, make_lamp(types, 180));

    const std::vector<LightSource> lights = build_light_list(world, types, 0, 0, 0);
    REQUIRE(lights.size() == 1);
    CHECK(lights[0].voxels == 1);
}

TEST_CASE("a brighter lamp carries more radiance") {
    VoxelTypeTable dim_types;
    World dim;
    fill(dim, 0, 0, 0, 0, 0, 0, make_lamp(dim_types, 60));

    VoxelTypeTable bright_types;
    World bright;
    fill(bright, 0, 0, 0, 0, 0, 0, make_lamp(bright_types, 240));

    const auto dim_lights = build_light_list(dim, dim_types, 0, 0, 0);
    const auto bright_lights = build_light_list(bright, bright_types, 0, 0, 0);
    REQUIRE(dim_lights.size() == 1);
    REQUIRE(bright_lights.size() == 1);
    CHECK(bright_lights[0].red > dim_lights[0].red * 2.0f);
}

TEST_CASE("the nearest lamps come first, so a capped list keeps the ones that matter") {
    VoxelTypeTable types;
    World world;
    const VoxelTypeId lamp = make_lamp(types, 200);
    fill(world, 500, 0, 0, 500, 0, 0, lamp);
    fill(world, 100, 0, 0, 100, 0, 0, lamp);
    fill(world, 300, 0, 0, 300, 0, 0, lamp);

    const std::vector<LightSource> lights = build_light_list(world, types, 0, 0, 0);
    REQUIRE(lights.size() == 3);
    CHECK(lights[0].x == 100);
    CHECK(lights[1].x == 300);
    CHECK(lights[2].x == 500);

    // And from somewhere else, the order follows the camera rather than the world.
    const std::vector<LightSource> from_far = build_light_list(world, types, 600, 0, 0);
    CHECK(from_far[0].x == 500);
}

TEST_CASE("a brick with no emitter in it is rejected without reading its voxels") {
    // The palette check is what makes rebuilding this affordable when something is placed.
    // Without it the scan reads every voxel of every brick in the world, which for the test
    // scene is tens of millions, on every edit.
    VoxelTypeTable types;
    World world;
    const VoxelTypeId stone = make_stone(types);
    // A slab big enough that reading it voxel by voxel would be obvious in the timing.
    fill(world, 0, 0, 0, 255, 15, 255, stone);
    fill(world, 128, 8, 128, 128, 8, 128, make_lamp(types, 200));

    const std::vector<LightSource> lights = build_light_list(world, types, 0, 0, 0);
    REQUIRE(lights.size() == 1);
    CHECK(lights[0].x == 128);
}

TEST_CASE("the same world twice gives the same list") {
    // A measurement taken twice has to be the same measurement, and an unordered_map's
    // iteration order is not something to build a render on.
    VoxelTypeTable types;
    World world;
    const VoxelTypeId lamp = make_lamp(types, 200);
    for (i64 i = 0; i < 12; ++i) fill(world, i * 32, 0, 0, i * 32, 0, 0, lamp);

    const std::vector<LightSource> a = build_light_list(world, types, 0, 0, 0);
    const std::vector<LightSource> b = build_light_list(world, types, 0, 0, 0);
    REQUIRE(a.size() == b.size());
    for (usize i = 0; i < a.size(); ++i) {
        CHECK(a[i].x == b[i].x);
        CHECK(a[i].y == b[i].y);
        CHECK(a[i].z == b[i].z);
    }
}

TEST_CASE("a fitting wider than one cluster cell is still one light") {
    // This is the whole reason a building fits under the cap. A sconce is a few hundred voxels
    // and a cluster cell is four across, so grouping by cell alone gave a dozen lights per
    // sconce and a hall of forty of them overflowed a list of a thousand on fittings alone.
    VoxelTypeTable types;
    World world;
    fill(world, 0, 0, 0, 9, 5, 4, make_lamp(types, 200));   // 10 x 6 x 5, twelve cells

    const std::vector<LightSource> lights = build_light_list(world, types, 0, 0, 0);
    REQUIRE(lights.size() == 1);
    // The middle of the fitting, not the corner the scan happened to start from.
    CHECK(lights[0].x == 4);
    CHECK(lights[0].y == 2);
    CHECK(lights[0].z == 2);
}

TEST_CASE("a merged fitting's sphere still covers every voxel it stands for") {
    // Direct sampling owns emitters outright: light outside the cone the shader draws round a
    // light is owned by nobody and goes out. So a merged entry has to claim a sphere big enough
    // for the box it replaced, which for anything longer than it is wide means claiming more
    // voxels than are really there.
    VoxelTypeTable types;
    World world;
    fill(world, 0, 0, 0, 9, 5, 4, make_lamp(types, 200));   // 300 voxels in a 10 x 6 x 5 box

    const std::vector<LightSource> lights = build_light_list(world, types, 0, 0, 0);
    REQUIRE(lights.size() == 1);
    CHECK(shader_radius(lights[0]) >= half_diagonal(10.0, 6.0, 5.0));
    CHECK(lights[0].voxels > 300);
}

TEST_CASE("a lamp that really is a cube claims exactly its own voxels") {
    // The raising above must not creep in where it is not needed: 0.87 * cbrt(n) is the sphere
    // round a solid cube of n voxels, so a solid cube is already covered and reports its count.
    VoxelTypeTable types;
    World world;
    fill(world, 0, 0, 0, 3, 3, 3, make_lamp(types, 200));

    const std::vector<LightSource> lights = build_light_list(world, types, 0, 0, 0);
    REQUIRE(lights.size() == 1);
    CHECK(lights[0].voxels == 64);
    CHECK(shader_radius(lights[0]) >= half_diagonal(4.0, 4.0, 4.0));
}

TEST_CASE("a run one voxel wide is left in pieces rather than merged into a balloon") {
    // Merging is only free for a blob. A glowing strip merged end to end would be a handful of
    // voxels inside a sphere standing for hundreds, and the tracer would aim nearly every ray
    // at empty air — unbiased, and far noisier than leaving the strip alone.
    VoxelTypeTable types;
    World world;
    fill(world, 0, 0, 0, 199, 0, 0, make_lamp(types, 200));

    const std::vector<LightSource> lights = build_light_list(world, types, 0, 0, 0);
    CHECK(lights.size() == 50);   // one per cluster cell, none of them joined
    for (const LightSource& light : lights) {
        // Each still covers its own four voxels, and nothing like the length of the run.
        CHECK(shader_radius(light) >= half_diagonal(4.0, 1.0, 1.0));
        CHECK(shader_radius(light) < 4.0);
    }
}

TEST_CASE("the cap is spent on what delivers most light, not on what is nearest") {
    // A dim indicator close by is worth less than a bright lamp across the room, and ordering
    // by distance alone would have handed the slot to the indicator.
    VoxelTypeTable types;
    World world;
    fill(world, 20, 0, 0, 20, 0, 0, make_lamp(types, 40));
    fill(world, 60, 0, 0, 60, 0, 0, make_lamp(types, 200));

    const std::vector<LightSource> lights = build_light_list(world, types, 0, 0, 0);
    REQUIRE(lights.size() == 2);
    CHECK(lights[0].x == 60);
    CHECK(lights[1].x == 20);
}

TEST_CASE("a scene past the cap keeps the strongest and drops the rest") {
    VoxelTypeTable types;
    World world;
    const VoxelTypeId lamp = make_lamp(types, 200);
    // Eight voxels apart, so no two share a cluster cell and none of them touch: 1331 separate
    // fittings, which no amount of merging can bring under a cap of 1024.
    for (i64 z = 0; z < 11; ++z)
        for (i64 y = 0; y < 11; ++y)
            for (i64 x = 0; x < 11; ++x) fill(world, x * 8, y * 8, z * 8, x * 8, y * 8, z * 8, lamp);

    const std::vector<LightSource> lights = build_light_list(world, types, 0, 0, 0);
    // Exactly the cap, and that is deliberate: it is the only signal the shader has for "this
    // list is not all of them". A list one short would look complete, and every dropped lamp
    // would then be owned by nobody and go out. See kMaxLights.
    REQUIRE(lights.size() == kMaxLights);

    // The lamp over the camera survives; the one in the far corner does not.
    bool kept_nearest = false;
    bool kept_furthest = false;
    for (const LightSource& light : lights) {
        if (light.x == 0 && light.y == 0 && light.z == 0) kept_nearest = true;
        if (light.x == 80 && light.y == 80 && light.z == 80) kept_furthest = true;
    }
    CHECK(kept_nearest);
    CHECK_FALSE(kept_furthest);
}

// ---- the list's identity, which is how a face knows a lamp changed ---------------------------
//
// The face pass accumulates lamp light per voxel face over hundreds of frames and then stops
// casting rays at it altogether. A face that has gone silent cannot discover anything, so a lamp
// placed, deleted, moved or dimmed after that would never arrive — unless the host says so, on the
// frame it happens. `light_list_hash` is what decides whether it has to.
//
// These are the gate on "responsive". Every one of them is a case a player produces in a second of
// play, and each has to come out with a different number from the list before it.

TEST_CASE("an unchanged world produces an unchanged list identity") {
    VoxelTypeTable types;
    World world;
    fill(world, 0, 0, 0, 3, 3, 3, make_lamp(types, 200));

    const u64 first = light_list_hash(build_light_list(world, types, 0, 0, 0));
    const u64 again = light_list_hash(build_light_list(world, types, 0, 0, 0));
    CHECK(first == again);
    // And it is not the trivially-equal answer a broken hash would also give.
    CHECK(first != light_list_hash(std::vector<LightSource>{}));
}

TEST_CASE("walking to the other lamp does not change the list identity") {
    // The gate on D500, and it is the case that was costing a player the picture.
    //
    // The list is RANKED by what each fitting delivers at the camera, so two lamps come back in one
    // order from beside the first and in the other order from beside the second. Nothing about the
    // lamps has changed. A hash over the rank order says otherwise, and the host answers a changed
    // identity by reopening the lamp term of EVERY face in the store — so a player who moved between
    // two edits relit the whole room on the second one, every face dropped to eight samples, and
    // none of them ever reached `kLampConverged`. Measured in the game: nine chisel strokes from a
    // static camera bumped the version once; the same nine while flying bumped it nine times and
    // left `lamps on the card: 0 of 997,296 live faces cast no more rays at all`.
    VoxelTypeTable types;
    World world;
    const VoxelTypeId lamp = make_lamp(types, 200);
    fill(world, 0, 0, 0, 3, 3, 3, lamp);
    fill(world, 1000, 0, 0, 1003, 3, 3, lamp);

    const std::vector<LightSource> near_first = build_light_list(world, types, 0, 0, 0);
    const std::vector<LightSource> far_first = build_light_list(world, types, 1000, 0, 0);
    REQUIRE(near_first.size() == 2);
    REQUIRE(far_first.size() == 2);
    // The ranking really did change, or this test would pass on a build where the camera makes no
    // difference at all and would be evidence about nothing. Trap 15: a measurement that never ran
    // and a clean one look identical.
    REQUIRE(near_first[0].x != far_first[0].x);
    CHECK(light_list_hash(near_first) == light_list_hash(far_first));
}

TEST_CASE("placing a lamp changes the list identity") {
    VoxelTypeTable types;
    World world;
    const VoxelTypeId lamp = make_lamp(types, 200);
    fill(world, 0, 0, 0, 3, 3, 3, lamp);
    const u64 before = light_list_hash(build_light_list(world, types, 0, 0, 0));

    fill(world, 40, 0, 0, 43, 3, 3, lamp);
    CHECK(light_list_hash(build_light_list(world, types, 0, 0, 0)) != before);
}

TEST_CASE("deleting the only lamp changes the list identity and empties it") {
    VoxelTypeTable types;
    World world;
    fill(world, 0, 0, 0, 3, 3, 3, make_lamp(types, 200));
    const u64 before = light_list_hash(build_light_list(world, types, 0, 0, 0));

    fill(world, 0, 0, 0, 3, 3, 3, kAir);
    const std::vector<LightSource> after = build_light_list(world, types, 0, 0, 0);
    CHECK(after.empty());
    CHECK(light_list_hash(after) != before);
}

TEST_CASE("dimming a lamp changes the list identity without changing its length") {
    // The case a hash over the COUNT would miss, and the one a player produces by editing a
    // material rather than by placing anything: same fitting, same place, different radiance.
    VoxelTypeTable bright_types;
    World bright;
    fill(bright, 0, 0, 0, 3, 3, 3, make_lamp(bright_types, 200));

    VoxelTypeTable dim_types;
    World dim;
    fill(dim, 0, 0, 0, 3, 3, 3, make_lamp(dim_types, 100));

    const std::vector<LightSource> a = build_light_list(bright, bright_types, 0, 0, 0);
    const std::vector<LightSource> b = build_light_list(dim, dim_types, 0, 0, 0);
    REQUIRE(a.size() == 1);
    REQUIRE(b.size() == a.size());
    CHECK(light_list_hash(a) != light_list_hash(b));
}

TEST_CASE("carving a fitting smaller changes the list identity") {
    // A chisel taken to the lamp itself rather than to anything around it. The box the fitting
    // occupies shrinks, so its centre moves and the sphere the shader draws round it changes —
    // and every face in the room measured its shadow against where the lamp used to be.
    VoxelTypeTable types;
    World world;
    const VoxelTypeId lamp = make_lamp(types, 200);
    fill(world, 0, 0, 0, 3, 3, 3, lamp);
    const u64 before = light_list_hash(build_light_list(world, types, 0, 0, 0));

    fill(world, 3, 0, 0, 3, 3, 3, kAir);   // one face of the cube taken off
    CHECK(light_list_hash(build_light_list(world, types, 0, 0, 0)) != before);
}

TEST_CASE("an edit a fitting is unchanged by leaves the list identity alone") {
    // The other side of the line, and it is drawn where the RECORD is rather than where the voxels
    // are. Taking one corner voxel off a solid cube leaves the bounding box exactly where it was,
    // and the record holds the box's middle, the mean radiance over the voxels present, and the
    // sphere that covers the box — so all three come out identical.
    //
    // That is the right answer and not a missed change: the shader reads those three numbers and
    // nothing else, so nothing it can see has moved, and re-measuring the whole store would spend a
    // second of rays arriving back at the number it already held.
    VoxelTypeTable types;
    World world;
    const VoxelTypeId lamp = make_lamp(types, 200);
    fill(world, 0, 0, 0, 3, 3, 3, lamp);
    const u64 before = light_list_hash(build_light_list(world, types, 0, 0, 0));

    fill(world, 3, 3, 3, 3, 3, 3, kAir);
    CHECK(light_list_hash(build_light_list(world, types, 0, 0, 0)) == before);
}

TEST_CASE("an edit that touches no emitter leaves the list identity alone") {
    // The other half of the gate, and the half that decides what this COSTS. A changed identity
    // makes every face in the store drop its lamp confidence and measure again; an edit twenty
    // metres from the nearest sconce must not pay for that.
    VoxelTypeTable types;
    World world;
    fill(world, 0, 0, 0, 3, 3, 3, make_lamp(types, 200));
    const u64 before = light_list_hash(build_light_list(world, types, 0, 0, 0));

    fill(world, 200, 0, 0, 231, 31, 31, make_stone(types));
    CHECK(light_list_hash(build_light_list(world, types, 0, 0, 0)) == before);
}
