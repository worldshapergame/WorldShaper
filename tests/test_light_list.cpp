// The list of emitters the tracer aims at. If this is wrong, rays are sent at lamps that are
// not there and none at the ones that are — and the symptom is noise, which is the hardest
// thing to trace back to its cause.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <vector>

#include "world/emitter_store.hpp"
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

std::filesystem::path scratch(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "worldshaper-tests";
    std::filesystem::create_directories(dir);
    return dir / name;
}

// More fittings than the cap, spread over eighteen neighbourhoods of world space, each one voxel
// and 256 voxels from its neighbours — far enough apart that no two share a cluster cell and none
// of them merge, so the fittings that come out are exactly the cells that went in.
//
// Brightness varies with position and not with distance from anywhere, which is what makes the two
// rules below tell different stories: dealing the cap round the world keeps the brightest of each
// neighbourhood, and ranking it from the camera keeps whatever happens to be near the camera.
constexpr i64 kFieldPitch = 256;
constexpr i64 kFieldX = 12, kFieldY = 12, kFieldZ = 8;   // 1,152 fittings against a cap of 1,024

std::vector<EmissiveCell> lamp_field() {
    std::vector<EmissiveCell> cells;
    cells.reserve(static_cast<usize>(kFieldX * kFieldY * kFieldZ));
    for (i64 i = 0; i < kFieldX; ++i) {
        for (i64 j = 0; j < kFieldY; ++j) {
            for (i64 k = 0; k < kFieldZ; ++k) {
                const i64 x = i * kFieldPitch, y = j * kFieldPitch, z = k * kFieldPitch;
                const f32 bright = 1.0f + static_cast<f32>((i * 7 + j * 5 + k * 3) % 11) * 0.1f;
                EmissiveCell cell;
                cell.key_x = x / kLightClusterVoxels;
                cell.key_y = y / kLightClusterVoxels;
                cell.key_z = z / kLightClusterVoxels;
                cell.min_x = cell.max_x = x;
                cell.min_y = cell.max_y = y;
                cell.min_z = cell.max_z = z;
                cell.red = cell.green = cell.blue = bright;
                cell.voxels = 1;
                cells.push_back(cell);
            }
        }
    }
    return cells;
}

// The cap as it was before R9g's second half: every fitting ranked by what it would deliver at the
// camera, and the list cut where the buffer ends. Written out here rather than reached for in the
// builder, because it is the behaviour being replaced and a test that shared the code could not
// show the two apart.
std::vector<LightSource> nearest_first(const std::vector<EmissiveCell>& cells, i64 cx, i64 cy,
                                       i64 cz) {
    std::vector<LightSource> lights;
    lights.reserve(cells.size());
    for (const EmissiveCell& cell : cells) {
        LightSource light;
        light.x = static_cast<i32>(cell.min_x);
        light.y = static_cast<i32>(cell.min_y);
        light.z = static_cast<i32>(cell.min_z);
        light.red = cell.red / static_cast<f32>(cell.voxels);
        light.green = cell.green / static_cast<f32>(cell.voxels);
        light.blue = cell.blue / static_cast<f32>(cell.voxels);
        light.voxels = cell.voxels;
        lights.push_back(light);
    }
    const auto score = [&](const LightSource& light) {
        const f64 dx = static_cast<f64>(light.x) - static_cast<f64>(cx);
        const f64 dy = static_cast<f64>(light.y) - static_cast<f64>(cy);
        const f64 dz = static_cast<f64>(light.z) - static_cast<f64>(cz);
        const f64 radius = shader_radius(light);
        const f64 luminance = 0.2126 * light.red + 0.7152 * light.green + 0.0722 * light.blue;
        return luminance * radius * radius / std::max(dx * dx + dy * dy + dz * dz, 1.0);
    };
    std::sort(lights.begin(), lights.end(),
              [&](const LightSource& a, const LightSource& b) { return score(a) > score(b); });
    if (lights.size() > kMaxLights) lights.resize(kMaxLights);
    return lights;
}

// How many of a list fell in each neighbourhood the cap is dealt round.
std::vector<usize> per_neighbourhood(const std::vector<LightSource>& lights) {
    std::unordered_map<i64, usize> count;
    for (const LightSource& light : lights) {
        const i64 key = (light.x / kLightCapCellVoxels) * 1000000 +
                        (light.y / kLightCapCellVoxels) * 1000 + (light.z / kLightCapCellVoxels);
        ++count[key];
    }
    std::vector<usize> out;
    out.reserve(count.size());
    for (const auto& [key, n] : count) out.push_back(n);
    std::sort(out.begin(), out.end());
    return out;
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

TEST_CASE("a scene past the cap is cut to exactly the cap") {
    VoxelTypeTable types;
    World world;
    const VoxelTypeId lamp = make_lamp(types, 200);
    // Eight voxels apart, so no two share a cluster cell and none of them touch: 1331 separate
    // fittings, which no amount of merging can bring under a cap of 1024.
    for (i64 z = 0; z < 11; ++z)
        for (i64 y = 0; y < 11; ++y)
            for (i64 x = 0; x < 11; ++x) fill(world, x * 8, y * 8, z * 8, x * 8, y * 8, z * 8, lamp);

    const std::vector<LightSource> lights = build_light_list(world, types, 0, 0, 0);
    // Exactly the cap, because that is all the buffer the card has. Everything past it is lit by
    // nobody until a bounce happens to land on it. See kMaxLights.
    REQUIRE(lights.size() == kMaxLights);

    // What this used to check was that the lamp over the camera survived and the one in the far
    // corner did not, and that is R9g's fault written down as a requirement: it makes which lamps
    // EXIST a fact about where somebody is standing. The same fittings capped from the far corner
    // now come out as the same set — see the two cases below, which is where that is tested with
    // enough of the world to tell the two rules apart. This grid is eighty voxels across, which is
    // one neighbourhood, so there is nothing here for a deal to spread.
    CHECK(light_list_hash(lights) == light_list_hash(build_light_list(world, types, 80, 80, 80)));
}

// ---- R9g: the cap is spent on the world, not on the observer ----------------------------------
//
// A lamp that goes out because the player walked is the same fault as a lamp that does not exist
// because its region is not loaded: in both, the light is defined by where the camera is rather
// than by where the light is. The gate is identity — the same world capped from two places has to
// produce the same set of fittings, hash for hash.

TEST_CASE("the cap is dealt round the world, so walking cannot put a lamp out") {
    const std::vector<EmissiveCell> cells = lamp_field();
    REQUIRE(cells.size() > kMaxLights);

    const std::vector<LightSource> here = merge_light_list(cells, 0, 0, 0);
    const std::vector<LightSource> away = merge_light_list(
        cells, (kFieldX - 1) * kFieldPitch, (kFieldY - 1) * kFieldPitch,
        (kFieldZ - 1) * kFieldPitch);
    REQUIRE(here.size() == kMaxLights);
    REQUIRE(away.size() == kMaxLights);
    CHECK(light_list_hash(here) == light_list_hash(away));

    // And the rule it replaces, over the same fittings, so the difference is shown rather than
    // asserted: two places to stand, two different sets of lamps.
    CHECK(light_list_hash(nearest_first(cells, 0, 0, 0)) !=
          light_list_hash(nearest_first(cells, (kFieldX - 1) * kFieldPitch,
                                        (kFieldY - 1) * kFieldPitch,
                                        (kFieldZ - 1) * kFieldPitch)));
}

TEST_CASE("every neighbourhood keeps its lamps, and the brightest of them") {
    const std::vector<EmissiveCell> cells = lamp_field();
    const std::vector<LightSource> dealt = merge_light_list(cells, 0, 0, 0);

    // Eighteen neighbourhoods of sixty-four fittings and a cap of 1,024: fifty-six each with
    // sixteen left over, so no neighbourhood is starved and none is favoured.
    const std::vector<usize> spread = per_neighbourhood(dealt);
    REQUIRE(spread.size() == 18);
    CHECK(spread.front() == 56);
    CHECK(spread.back() == 57);

    // The camera's ranking spends the cap on wherever it is standing: the neighbourhood the camera
    // is in keeps every lamp it has, which the deal can never do, and somewhere else pays for it.
    const std::vector<usize> ranked = per_neighbourhood(nearest_first(cells, 0, 0, 0));
    CHECK(ranked.back() == 64);

    // And inside a neighbourhood it is the dimmest that go, so what survives is brighter on
    // average than the field it was drawn from.
    f64 kept_mean = 0.0;
    for (const LightSource& light : dealt) kept_mean += static_cast<f64>(light.green);
    kept_mean /= static_cast<f64>(dealt.size());
    f64 field_mean = 0.0;
    for (const EmissiveCell& cell : cells) field_mean += static_cast<f64>(cell.green);
    field_mean /= static_cast<f64>(cells.size());
    CHECK(kept_mean > field_mean);
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

// R9g. The expensive half of finding the lamps is per chunk and only changes when that chunk
// does, so the application keeps each chunk's cells and rescans only what an edit touched. That
// is worth 14.15 ms an edit on the facility — and it is only worth anything if the answer does
// not move, because a list that is nearly right is a room lit by nearly the right lamps.
//
// The gate is therefore identity against the whole-world scan, not plausibility: same fittings,
// same order, same hash. A fitting that STRADDLES a chunk boundary is the case that would break a
// naive split, so the world here is built to contain one.
TEST_CASE("scanning chunk by chunk gives the same lights as scanning the world") {
    VoxelTypeTable types;
    World world;
    const VoxelTypeId lamp = make_lamp(types, 200);

    // One fitting inside a chunk, one lying across the boundary at x = 256, and one far away so
    // the ranking has something to order.
    fill(world, 10, 10, 10, 13, 13, 13, lamp);
    fill(world, 254, 40, 40, 258, 43, 43, lamp);
    fill(world, 700, 12, 12, 703, 15, 15, lamp);
    fill(world, -260, 5, 5, -256, 8, 8, lamp);

    const std::vector<LightSource> whole = build_light_list(world, types, 0, 0, 0);
    REQUIRE(whole.size() >= 3);

    std::vector<EmissiveCell> cells;
    world.for_each_chunk([&](const ChunkCoord& coord, const Chunk& chunk) {
        std::vector<EmissiveCell> mine =
            scan_chunk_emitters(chunk, coord.x * static_cast<i64>(kChunkEdge),
                                coord.y * static_cast<i64>(kChunkEdge),
                                coord.z * static_cast<i64>(kChunkEdge), types);
        cells.insert(cells.end(), mine.begin(), mine.end());
    });
    const std::vector<LightSource> piecewise = merge_light_list(cells, 0, 0, 0);

    REQUIRE(piecewise.size() == whole.size());
    for (usize i = 0; i < whole.size(); ++i) {
        CHECK(piecewise[i].x == whole[i].x);
        CHECK(piecewise[i].y == whole[i].y);
        CHECK(piecewise[i].z == whole[i].z);
        CHECK(piecewise[i].voxels == whole[i].voxels);
        CHECK(piecewise[i].red == doctest::Approx(whole[i].red));
    }
    // And the identity the renderer actually reads, which is what decides whether every face in
    // the store throws its lamp light away and measures again.
    CHECK(light_list_hash(piecewise) == light_list_hash(whole));
}

// The half of the same claim that the test above cannot see: a chunk that was NOT rescanned must
// contribute exactly what it contributed before. This is the cache, played out by hand — scan
// everything, drop one chunk's cells, rescan only that one, and the answer must not move.
TEST_CASE("rescanning one chunk and keeping the rest changes nothing") {
    VoxelTypeTable types;
    World world;
    const VoxelTypeId lamp = make_lamp(types, 180);
    fill(world, 20, 20, 20, 23, 23, 23, lamp);
    fill(world, 300, 30, 30, 303, 33, 33, lamp);
    fill(world, 600, 60, 60, 603, 63, 63, lamp);

    std::unordered_map<ChunkCoord, std::vector<EmissiveCell>, ChunkCoordHash> cache;
    auto rebuild = [&]() {
        std::vector<EmissiveCell> cells;
        world.for_each_chunk([&](const ChunkCoord& coord, const Chunk& chunk) {
            auto found = cache.find(coord);
            if (found == cache.end()) {
                found = cache.emplace(coord,
                                      scan_chunk_emitters(chunk,
                                                          coord.x * static_cast<i64>(kChunkEdge),
                                                          coord.y * static_cast<i64>(kChunkEdge),
                                                          coord.z * static_cast<i64>(kChunkEdge),
                                                          types))
                            .first;
            }
            cells.insert(cells.end(), found->second.begin(), found->second.end());
        });
        return merge_light_list(cells, 0, 0, 0);
    };

    const std::vector<LightSource> first = rebuild();
    CHECK(light_list_hash(rebuild()) == light_list_hash(first));   // nothing dropped: cache only

    // Now place a fourth lamp and drop only the chunk it landed in, which is what
    // announce_world_change does with the edited box.
    fill(world, 310, 40, 40, 313, 43, 43, lamp);
    cache.erase(ChunkCoord{1, 0, 0});
    const std::vector<LightSource> after = rebuild();
    CHECK(after.size() == first.size() + 1);
    CHECK(light_list_hash(after) == light_list_hash(build_light_list(world, types, 0, 0, 0)));
}

// ---- R9g: the fittings persist, the voxels need not -------------------------------------------
//
// `build_light_list` reads the chunks the `World` is holding, which makes a lamp's existence a fact
// about what is loaded. `EmitterStore` keeps the fittings instead: a chunk's emissive cells are a
// few dozen records where the chunk is megabytes, so they are remembered, written beside the world
// and merged from whether or not the voxels came back.
//
// The gate is identity and not plausibility, exactly as it was for the scan cache (D587): a list
// that is nearly right is a room lit by nearly the right lamps.

TEST_CASE("a fitting outlives the voxels it was found in") {
    VoxelTypeTable types;
    World world;
    const VoxelTypeId lamp = make_lamp(types, 200);
    // One fitting inside a chunk, one lying across the boundary at x = 256 — the case a per-chunk
    // store would get wrong if the merge were cached with the scan — and one far away.
    fill(world, 10, 10, 10, 13, 13, 13, lamp);
    fill(world, 254, 40, 40, 258, 43, 43, lamp);
    fill(world, 700, 12, 12, 703, 15, 15, lamp);

    EmitterStore store;
    const EmitterScan first = store.refresh(world, types);
    CHECK(first.scanned == static_cast<u32>(world.chunk_count()));
    CHECK(first.reused == 0);
    CHECK(first.absent == 0);

    const std::vector<LightSource> scanned = build_light_list(world, types, 0, 0, 0);
    REQUIRE(scanned.size() == 3);
    CHECK(light_list_hash(build_light_list_from_store(store, 0, 0, 0)) ==
          light_list_hash(scanned));

    // Asking again reads nothing: what is known is kept, which is D587's half of the stage.
    const EmitterScan again = store.refresh(world, types);
    CHECK(again.scanned == 0);
    CHECK(again.reused == static_cast<u32>(world.chunk_count()));

    // And now the voxels are gone with nothing announcing it, which is what a region that is not
    // loaded is. The lamps are still where they were, so the list is the same list.
    World unloaded;
    const EmitterScan without = store.refresh(unloaded, types, EmitterResidency::kKeep);
    CHECK(without.scanned == 0);
    CHECK(without.absent == first.scanned);
    CHECK(light_list_hash(build_light_list_from_store(store, 0, 0, 0)) ==
          light_list_hash(scanned));

    // The control arm is the behaviour being replaced: a fitting exists while its chunk is resident
    // and not otherwise, so an unloaded world is an unlit one.
    EmitterStore control;
    control.refresh(world, types);
    const EmitterScan dropped = control.refresh(unloaded, types, EmitterResidency::kDrop);
    CHECK(dropped.dropped == first.scanned);
    CHECK(dropped.absent == 0);
    CHECK(build_light_list_from_store(control, 0, 0, 0).empty());
}

TEST_CASE("a chunk nobody has looked at is not a chunk with no lamps") {
    // Trap 7, in the one place here where the wrong answer is a building that loads dark.
    EmitterStore store;
    const ChunkCoord coord{5, 0, 0};
    CHECK_FALSE(store.known(coord));
    CHECK(store.cells(coord) == nullptr);

    store.remember(coord, {});
    CHECK(store.known(coord));
    REQUIRE(store.cells(coord) != nullptr);
    CHECK(store.cells(coord)->empty());
    CHECK(store.chunks() == 1);
    CHECK(store.cell_count() == 0);
}

TEST_CASE("a lamp somebody deleted is forgotten, and a lamp nobody loaded is not") {
    // The two ways a chunk leaves the world have opposite meanings and this is what tells them
    // apart: a deletion is announced and drops the chunk from the store; an unload announces
    // nothing and leaves it standing.
    VoxelTypeTable types;
    World world;
    const VoxelTypeId lamp = make_lamp(types, 200);
    fill(world, 10, 10, 10, 13, 13, 13, lamp);
    fill(world, 700, 12, 12, 703, 15, 15, lamp);

    EmitterStore store;
    store.refresh(world, types);
    REQUIRE(build_light_list_from_store(store, 0, 0, 0).size() == 2);

    fill(world, 700, 12, 12, 703, 15, 15, kAir);
    const i64 lo[3] = {700, 12, 12};
    const i64 hi[3] = {703, 15, 15};
    CHECK(store.forget_box(lo, hi) == 1);
    store.refresh(world, types);

    const std::vector<LightSource> left = build_light_list_from_store(store, 0, 0, 0);
    REQUIRE(left.size() == 1);
    CHECK(left[0].x == 11);
    CHECK(light_list_hash(left) == light_list_hash(build_light_list(world, types, 0, 0, 0)));
}

TEST_CASE("the lamps come back from the sidecar without the world") {
    VoxelTypeTable types;
    World world;
    const VoxelTypeId lamp = make_lamp(types, 200);
    fill(world, 10, 10, 10, 13, 13, 13, lamp);
    fill(world, 254, 40, 40, 258, 43, 43, lamp);
    fill(world, 700, 12, 12, 703, 15, 15, lamp);
    // A chunk with matter and no emitter in it, because "looked at, nothing there" is a record the
    // file has to carry: without it every load rescans a chunk to learn nothing.
    fill(world, 1200, 0, 0, 1203, 3, 3, make_stone(types));

    EmitterStore store;
    store.refresh(world, types);

    const std::string path = scratch("emitters.lamps").string();
    REQUIRE(write_emitter_store(path, 0xFEEDBEEFull, store));

    EmitterStore back;
    REQUIRE(read_emitter_store(path, 0xFEEDBEEFull, back));
    CHECK(back.chunks() == store.chunks());
    CHECK(emitter_store_hash(back) == emitter_store_hash(store));
    CHECK(light_list_hash(build_light_list_from_store(back, 0, 0, 0)) ==
          light_list_hash(build_light_list(world, types, 0, 0, 0)));

    // Nothing in it needs the voxels: a world with none of them produces the same list.
    World unloaded;
    const EmitterScan without = back.refresh(unloaded, types, EmitterResidency::kKeep);
    CHECK(without.scanned == 0);
    CHECK(without.absent == static_cast<u32>(store.chunks()));
    CHECK(light_list_hash(build_light_list_from_store(back, 0, 0, 0)) ==
          light_list_hash(build_light_list(world, types, 0, 0, 0)));

    // Written twice, byte for byte the same file. A file whose bytes depend on a map's iteration
    // order cannot be compared between runs, and comparing them is how this is checked at all.
    const std::string second = scratch("emitters-again.lamps").string();
    REQUIRE(write_emitter_store(second, 0xFEEDBEEFull, store));
    const auto read_all = [](const std::string& at) {
        std::ifstream file(at, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    };
    CHECK(read_all(path) == read_all(second));
}

TEST_CASE("a sidecar that cannot be trusted is refused rather than believed") {
    VoxelTypeTable types;
    World world;
    fill(world, 10, 10, 10, 13, 13, 13, make_lamp(types, 200));
    EmitterStore store;
    store.refresh(world, types);

    const std::string path = scratch("emitters-suspect.lamps").string();
    REQUIRE(write_emitter_store(path, 0x1111ull, store));

    // Written for another world. Reading it would light this one with somebody else's lamps.
    EmitterStore other;
    CHECK_FALSE(read_emitter_store(path, 0x2222ull, other));
    CHECK(other.chunks() == 0);

    std::string raw;
    {
        std::ifstream file(path, std::ios::binary);
        raw.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }
    REQUIRE(raw.size() > 40);

    // A file that stops halfway. A half-loaded set of lamps is a building lit in patches, which
    // reads as a rendering fault rather than as a bad file.
    const std::string cut = scratch("emitters-cut.lamps").string();
    {
        std::ofstream file(cut, std::ios::binary | std::ios::trunc);
        file.write(raw.data(), static_cast<std::streamsize>(raw.size() - 16));
    }
    EmitterStore truncated;
    CHECK_FALSE(read_emitter_store(cut, 0x1111ull, truncated));
    CHECK(truncated.chunks() == 0);

    // One byte of the payload turned over.
    const std::string bent = scratch("emitters-bent.lamps").string();
    {
        std::string copy = raw;
        copy[copy.size() - 4] = static_cast<char>(copy[copy.size() - 4] ^ 0x40);
        std::ofstream file(bent, std::ios::binary | std::ios::trunc);
        file.write(copy.data(), static_cast<std::streamsize>(copy.size()));
    }
    EmitterStore corrupt;
    CHECK_FALSE(read_emitter_store(bent, 0x1111ull, corrupt));
    CHECK(corrupt.chunks() == 0);

    // And a file that is not there at all, which is every world written before this existed. It has
    // to mean "nobody wrote any", so the store stays empty and the next refresh scans.
    EmitterStore missing;
    CHECK_FALSE(read_emitter_store(scratch("emitters-absent.lamps").string(), 0x1111ull, missing));
    CHECK(missing.chunks() == 0);
    CHECK(missing.refresh(world, types).scanned == static_cast<u32>(world.chunk_count()));
}

TEST_CASE("what the world says beats what the disk remembers") {
    // The store is filled from a scan of the world in front of us and then a stale sidecar is read
    // over it. The scan wins, because the disk is a memory of a world and the world is the world.
    VoxelTypeTable types;
    World world;
    const VoxelTypeId lamp = make_lamp(types, 200);
    fill(world, 10, 10, 10, 13, 13, 13, lamp);
    fill(world, 20, 10, 10, 23, 13, 13, make_stone(types));   // the chunk stays, the lamp goes

    EmitterStore old;
    old.refresh(world, types);
    const std::string path = scratch("emitters-stale.lamps").string();
    REQUIRE(write_emitter_store(path, 0x3333ull, old));
    REQUIRE(build_light_list_from_store(old, 0, 0, 0).size() == 1);

    // Somebody carves the lamp out and the sidecar has not been rewritten.
    fill(world, 10, 10, 10, 13, 13, 13, kAir);
    EmitterStore fresh;
    fresh.refresh(world, types);
    REQUIRE(read_emitter_store(path, 0x3333ull, fresh));
    CHECK(build_light_list_from_store(fresh, 0, 0, 0).empty());
}
