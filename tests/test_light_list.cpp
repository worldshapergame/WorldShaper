// The list of emitters the tracer aims at. If this is wrong, rays are sent at lamps that are
// not there and none at the ones that are — and the symptom is noise, which is the hardest
// thing to trace back to its cause.

#include <doctest/doctest.h>

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
