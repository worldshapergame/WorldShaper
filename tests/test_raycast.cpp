#include <doctest/doctest.h>

#include <cmath>

#include "world/raycast.hpp"
#include "world/world.hpp"

using namespace ws;

namespace {

constexpr VoxelTypeId kRock = 7;

void fill(World& world, i64 x0, i64 y0, i64 z0, i64 x1, i64 y1, i64 z1, VoxelTypeId type) {
    for (i64 z = z0; z <= z1; ++z) {
        for (i64 y = y0; y <= y1; ++y) {
            for (i64 x = x0; x <= x1; ++x) world.set(x, y, z, type);
        }
    }
}

}  // namespace

TEST_CASE("raycast finds the first solid voxel along an axis") {
    World world;
    world.set(10, 0, 0, kRock);

    const RayHit hit = raycast(world, 0.5, 0.5, 0.5, 1, 0, 0, 100.0);
    REQUIRE(hit.hit);
    CHECK(hit.x == 10);
    CHECK(hit.y == 0);
    CHECK(hit.z == 0);
    CHECK(hit.type == kRock);
    // Entered through the -x face, so the normal points back at the camera.
    CHECK(hit.nx == -1);
    CHECK(hit.ny == 0);
    CHECK(hit.nz == 0);
    CHECK(hit.distance == doctest::Approx(9.5));
    // The placement voxel is the empty one in front of that face.
    CHECK(hit.place_x() == 9);
}

TEST_CASE("raycast misses an empty world") {
    World world;
    const RayHit hit = raycast(world, 0.5, 0.5, 0.5, 1, 0, 0, 10000.0);
    CHECK_FALSE(hit.hit);
}

TEST_CASE("raycast respects the maximum distance") {
    World world;
    world.set(100, 0, 0, kRock);

    CHECK_FALSE(raycast(world, 0.5, 0.5, 0.5, 1, 0, 0, 50.0).hit);
    CHECK(raycast(world, 0.5, 0.5, 0.5, 1, 0, 0, 200.0).hit);
}

TEST_CASE("raycast works in negative coordinates") {
    World world;
    world.set(-40, -300, -5000, kRock);

    // Centres of the voxels: -40 + 0.5 is -39.5, not -40.5, which floors into -41.
    const RayHit hit = raycast(world, -39.5, -299.5, -6000.5, 0, 0, 1, 4000.0);
    REQUIRE(hit.hit);
    CHECK(hit.x == -40);
    CHECK(hit.y == -300);
    CHECK(hit.z == -5000);
    CHECK(hit.nz == -1);
}

TEST_CASE("raycast reports every face direction") {
    World world;
    world.set(0, 0, 0, kRock);

    struct Case {
        f64 ox, oy, oz, dx, dy, dz;
        i32 nx, ny, nz;
    };
    const Case cases[] = {
        {-10.0, 0.5, 0.5, 1, 0, 0, -1, 0, 0},  {10.5, 0.5, 0.5, -1, 0, 0, 1, 0, 0},
        {0.5, -10.0, 0.5, 0, 1, 0, 0, -1, 0},  {0.5, 10.5, 0.5, 0, -1, 0, 0, 1, 0},
        {0.5, 0.5, -10.0, 0, 0, 1, 0, 0, -1},  {0.5, 0.5, 10.5, 0, 0, -1, 0, 0, 1},
    };
    for (const Case& c : cases) {
        const RayHit hit = raycast(world, c.ox, c.oy, c.oz, c.dx, c.dy, c.dz, 100.0);
        REQUIRE(hit.hit);
        CHECK(hit.x == 0);
        CHECK(hit.y == 0);
        CHECK(hit.z == 0);
        CHECK(hit.nx == c.nx);
        CHECK(hit.ny == c.ny);
        CHECK(hit.nz == c.nz);
    }
}

TEST_CASE("raycast starting inside a solid voxel hits it immediately") {
    World world;
    world.set(3, 3, 3, kRock);

    const RayHit hit = raycast(world, 3.5, 3.5, 3.5, 1, 0, 0, 100.0);
    REQUIRE(hit.hit);
    CHECK(hit.x == 3);
    CHECK(hit.distance == doctest::Approx(0.0));
    // No entry face exists, so it faces the camera rather than reporting nothing.
    CHECK(hit.nx == -1);
}

TEST_CASE("skipping empty chunks and bricks does not skip past geometry") {
    // A wall far enough away that the ray crosses many empty chunks to reach it. This is
    // the case the box-skipping is for, and getting the epsilon wrong shows up as either a
    // miss or a hit one voxel late.
    World world;
    fill(world, 2000, -8, -8, 2000, 8, 8, kRock);

    for (i64 offset = -8; offset <= 8; ++offset) {
        const f64 target_y = static_cast<f64>(offset) + 0.5;
        const RayHit hit = raycast(world, 0.5, 0.5, 0.5, 1999.5, target_y - 0.5, 0.0, 4000.0);
        REQUIRE(hit.hit);
        CHECK(hit.x == 2000);
        CHECK(hit.nx == -1);
    }
}

TEST_CASE("a diagonal ray hits the same voxel a fine step would") {
    // Independent check on the DDA: march the same ray by hand in small steps and compare.
    World world;
    fill(world, -40, -40, -40, 40, 40, 40, kRock);
    fill(world, -39, -39, -39, 39, 39, 39, kAir);   // a hollow shell, so entry face matters

    const f64 dir[3] = {0.37, 0.81, -0.45};
    const f64 length = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    const RayHit hit = raycast(world, 0.25, 0.5, 0.75, dir[0], dir[1], dir[2], 400.0);
    REQUIRE(hit.hit);

    bool found = false;
    for (f64 t = 0.0; t < 400.0 && !found; t += 0.002) {
        const i64 x = static_cast<i64>(std::floor(0.25 + dir[0] / length * t));
        const i64 y = static_cast<i64>(std::floor(0.5 + dir[1] / length * t));
        const i64 z = static_cast<i64>(std::floor(0.75 + dir[2] / length * t));
        if (world.get(x, y, z) != kAir) {
            CHECK(hit.x == x);
            CHECK(hit.y == y);
            CHECK(hit.z == z);
            found = true;
        }
    }
    CHECK(found);
}
