#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "world/ledger.hpp"
#include "world/op.hpp"
#include "world/property.hpp"
#include "world/tags.hpp"
#include "world/voxel_type.hpp"
#include "world/world.hpp"
#include "world/world_cache.hpp"

using namespace ws;

namespace {

// A world, its type table and everything else the cache needs to be handed. Two of these are made
// per test — one written from, one read into — because reading into the registries that wrote is
// how a round trip passes without ever having encoded anything.
struct Side {
    TagRegistry tags;
    PropertyRegistry properties;
    VoxelTypeTable types;
    World world;
    MatterLedger ledger;

    WorldCache handle() { return WorldCache{&tags, &properties, &types, &world, &ledger, {}, {}}; }
};

VisualRecord colour(u8 r, u8 g, u8 b) {
    VisualRecord v{};
    v.red = r;
    v.green = g;
    v.blue = b;
    v.opacity = 255;
    return v;
}

// Small, but with the shapes that break encoders: a uniform brick, a carved cavity that leaves
// bricks with palettes, and coordinates either side of the origin.
std::vector<VoxelTypeId> populate(Side& side) {
    BehaviourRecord stone{};
    stone.material = 1;
    stone.tags.add(side.tags.find("stone"));
    stone.properties.set(props::kDensity, PropertyValue::from_uint(2600));

    BehaviourRecord wood{};
    wood.material = 2;
    wood.tags.add(side.tags.find("wood"));

    const VoxelTypeId grey = side.types.intern(colour(120, 120, 120), stone);
    const VoxelTypeId brown = side.types.intern(colour(90, 60, 30), wood);

    apply_op(side.world,
             Op::fill_box(1, 1, -18, -18, -18, 17, 17, 17, grey, MatterReason::PlayerPlace),
             side.ledger);
    apply_op(side.world, Op::fill_box(2, 1, -3, -3, -3, 3, 3, 3, kAir, MatterReason::PlayerBreak),
             side.ledger);
    apply_op(side.world, Op::fill_box(3, 1, 40, 40, 40, 52, 52, 52, brown, MatterReason::PlayerPlace),
             side.ledger);
    return {grey, brown};
}

// A path in the system temporary directory that is removed when the test leaves, whichever way it
// leaves. A cache file is not small and a test that leaks one leaks it every run.
struct Scratch {
    std::string path;

    explicit Scratch(const char* name) {
        path = (std::filesystem::temp_directory_path() / name).string();
    }
    ~Scratch() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path + ".part", ignored);
    }
};

// Two neighbouring leaves of the ladder, as the sharpening run would hand them over: a key each,
// because that is what the reading run rebuilds its tree from, and the detail each was sampled at.
// The two differ in level as well as in position, because the ladder's leaves are of every size at
// once and a list that only ever held one size would not catch a format that drops the level.
std::vector<CachedRegion> two_regions(bool first_done, bool second_done) {
    CachedRegion a;
    a.key[0] = -3;
    a.key[1] = 0;
    a.key[2] = 5;
    a.level = 4;
    a.low[0] = -16.0;
    a.low[1] = 0.0;
    a.low[2] = -16.0;
    a.high[0] = 0.0;
    a.high[1] = 12.0;
    a.high[2] = 16.0;
    a.applied_per_metre = 32;
    a.done = first_done;

    CachedRegion b = a;
    b.key[0] = 11;
    b.level = 7;
    b.low[0] = 0.0;
    b.high[0] = 16.0;
    b.applied_per_metre = 8;
    b.done = second_done;
    return {a, b};
}

}  // namespace

TEST_CASE("a cached world comes back as it went in") {
    Scratch file("ws_test_cache_roundtrip.world");
    Side wrote;
    const std::vector<VoxelTypeId> materials = populate(wrote);

    WorldCache out = wrote.handle();
    out.materials = materials;
    const u64 key = world_cache_key("a clip", 32, 1234);
    REQUIRE(write_world_cache(file.path, key, out));

    Side read;
    WorldCache in = read.handle();
    REQUIRE(read_world_cache(file.path, key, in, nullptr));

    CHECK(read.world.stats().solid_voxels == wrote.world.stats().solid_voxels);
    CHECK(read.world.chunk_count() == wrote.world.chunk_count());
    CHECK(in.materials == materials);
    // Every voxel of a few probes, rather than a count that two different worlds could share.
    for (i64 z = -18; z <= 52; z += 7) {
        for (i64 y = -18; y <= 52; y += 7) {
            for (i64 x = -18; x <= 52; x += 7) {
                CHECK(read.world.get(x, y, z) == wrote.world.get(x, y, z));
            }
        }
    }
}

TEST_CASE("a world built from other source is not loaded") {
    Scratch file("ws_test_cache_key.world");
    Side wrote;
    populate(wrote);
    WorldCache out = wrote.handle();
    REQUIRE(write_world_cache(file.path, world_cache_key("a clip", 32, 1234), out));

    CHECK(world_cache_matches(file.path, world_cache_key("a clip", 32, 1234)));
    CHECK_FALSE(world_cache_matches(file.path, world_cache_key("another clip", 32, 1234)));
    CHECK_FALSE(world_cache_matches(file.path, world_cache_key("a clip", 16, 1234)));
    CHECK_FALSE(world_cache_matches(file.path, world_cache_key("a clip", 32, 5678)));

    Side read;
    WorldCache in = read.handle();
    CHECK_FALSE(read_world_cache(file.path, world_cache_key("a clip", 16, 1234), in, nullptr));
}

// The point of the region list: a half-sharpened world has to say so, or the next run loads the
// blocky one, finds nothing left to do, and the building never comes good again.
TEST_CASE("a half-sharpened world says which regions it is missing") {
    Scratch file("ws_test_cache_regions.world");
    Side wrote;
    populate(wrote);

    WorldCache out = wrote.handle();
    out.regions = two_regions(true, false);
    const u64 key = world_cache_key("a clip", 32, 1234);
    REQUIRE(write_world_cache(file.path, key, out));

    Side read;
    WorldCache in = read.handle();
    REQUIRE(read_world_cache(file.path, key, in, nullptr));

    REQUIRE(in.regions.size() == 2);
    CHECK(in.regions[0].done);
    CHECK_FALSE(in.regions[1].done);
    CHECK(in.regions[0].low[0] == doctest::Approx(-16.0));
    CHECK(in.regions[0].high[1] == doctest::Approx(12.0));
    CHECK(in.regions[1].low[0] == doctest::Approx(0.0));
    CHECK(in.regions[1].high[0] == doctest::Approx(16.0));

    // The key is the leaf's identity and the reading run rebuilds its whole tree from it, so a
    // format that wrote the corners but lost the key would still pass every check above and leave
    // the resuming run with nothing to rebuild from. Negative coordinates on purpose: the keys are
    // signed and a narrowing to unsigned would put this leaf on the other side of the world.
    CHECK(in.regions[0].key[0] == -3);
    CHECK(in.regions[0].key[1] == 0);
    CHECK(in.regions[0].key[2] == 5);
    CHECK(in.regions[0].level == 4u);
    CHECK(in.regions[1].key[0] == 11);
    CHECK(in.regions[1].level == 7u);

    // And the detail each was sampled at. Losing this is not a lost optimisation but lost quality:
    // a sample is pasted as a REPLACE over the whole leaf, so a run that reads 32 back as the
    // coarse rung will let a coarser answer land on top of sharper geometry.
    CHECK(in.regions[0].applied_per_metre == 32);
    CHECK(in.regions[1].applied_per_metre == 8);
}

// The corners survive exactly, not nearly. They are compared corner for corner against the grid a
// later run plans for itself, and a rounded double would throw away every flag in the file.
TEST_CASE("region corners round trip bit for bit") {
    Scratch file("ws_test_cache_corners.world");
    Side wrote;
    populate(wrote);

    WorldCache out = wrote.handle();
    CachedRegion odd;
    odd.low[0] = -17.0 + 34.0 * 1 / 3;   // the arithmetic the region planner actually does
    odd.low[1] = 0.1;
    odd.low[2] = -1.0 / 3.0;
    odd.high[0] = -17.0 + 34.0 * 2 / 3;
    odd.high[1] = 1e-9;
    odd.high[2] = 1.0 / 7.0;
    odd.done = true;
    out.regions.push_back(odd);
    const u64 key = world_cache_key("a clip", 32, 1234);
    REQUIRE(write_world_cache(file.path, key, out));

    Side read;
    WorldCache in = read.handle();
    REQUIRE(read_world_cache(file.path, key, in, nullptr));
    REQUIRE(in.regions.size() == 1);
    for (int i = 0; i < 3; ++i) {
        CHECK(in.regions[0].low[i] == odd.low[i]);
        CHECK(in.regions[0].high[i] == odd.high[i]);
    }
}

// A world with no ladder behind it writes no regions, and that has to stay distinguishable from a
// ladder with nothing done — the first needs no sharpening, the second needs all of it.
TEST_CASE("a world built in one pass carries no regions") {
    Scratch file("ws_test_cache_noregions.world");
    Side wrote;
    populate(wrote);
    WorldCache out = wrote.handle();
    const u64 key = world_cache_key("a clip", 32, 1234);
    REQUIRE(write_world_cache(file.path, key, out));

    Side read;
    WorldCache in = read.handle();
    in.regions = two_regions(true, true);   // rubbish the reader must clear
    REQUIRE(read_world_cache(file.path, key, in, nullptr));
    CHECK(in.regions.empty());
}

// The stipple verdict is the one thing about a world that cannot be re-derived from a corner of
// it: it is taken over the whole clip in a single sample, and a run that resumes from this file has
// only the world and a list of boxes. Left out of the file, the resuming run starts with an empty
// verdict — which forge::despeckle reads as "leave every speck alone, everywhere" — and sharpens
// the whole building with the despeckler silently off. Measured before it was fixed: 512 voxels
// repainted building the facility cold, 0 loading the same world from cache.
TEST_CASE("a cached world brings its stipple verdict back") {
    Scratch file("ws_test_cache_stipple.world");
    Side wrote;
    populate(wrote);

    WorldCache out = wrote.handle();
    out.regions = two_regions(true, false);
    out.stipple_taken = true;
    out.stipple.push_back(CachedStipple{27, false});    // a deliberate dither: leave it alone
    out.stipple.push_back(CachedStipple{131, true});    // a sampling accident: repaint it
    out.stipple.push_back(CachedStipple{554, false});
    const u64 key = world_cache_key("a clip", 32, 1234);
    REQUIRE(write_world_cache(file.path, key, out));

    Side read;
    WorldCache in = read.handle();
    REQUIRE(read_world_cache(file.path, key, in, nullptr));

    CHECK(in.stipple_taken);
    REQUIRE(in.stipple.size() == 3);
    CHECK(in.stipple[0].type == 27);
    CHECK_FALSE(in.stipple[0].may_despeckle);
    CHECK(in.stipple[1].type == 131);
    CHECK(in.stipple[1].may_despeckle);
    CHECK(in.stipple[2].type == 554);
    CHECK_FALSE(in.stipple[2].may_despeckle);
}

// Trap 7, in the place it does the most damage: "nobody asked" and "asked, and no material clears
// the floor" are both an empty list, and they mean opposite things. A run that inherits the first
// has to say out loud that it cannot despeckle; a run that inherits the second is correct to leave
// everything standing. One flag, written separately from the list, is what tells them apart.
TEST_CASE("an empty verdict is not the same as no verdict") {
    const u64 key = world_cache_key("a clip", 32, 1234);
    {
        Scratch file("ws_test_cache_stipple_empty.world");
        Side wrote;
        populate(wrote);
        WorldCache out = wrote.handle();
        out.regions = two_regions(true, false);
        out.stipple_taken = true;   // asked, and nothing came back
        REQUIRE(write_world_cache(file.path, key, out));

        Side read;
        WorldCache in = read.handle();
        REQUIRE(read_world_cache(file.path, key, in, nullptr));
        CHECK(in.stipple_taken);
        CHECK(in.stipple.empty());
    }
    {
        Scratch file("ws_test_cache_stipple_none.world");
        Side wrote;
        populate(wrote);
        WorldCache out = wrote.handle();
        out.regions = two_regions(true, false);   // never asked: --no-despeckle built this one
        REQUIRE(write_world_cache(file.path, key, out));

        Side read;
        WorldCache in = read.handle();
        in.stipple_taken = true;                              // rubbish the reader must clear
        in.stipple.push_back(CachedStipple{1, true});         // ditto
        REQUIRE(read_world_cache(file.path, key, in, nullptr));
        CHECK_FALSE(in.stipple_taken);
        CHECK(in.stipple.empty());
    }
}

// A file from the version before the region list is rejected rather than read up to the point
// where the format diverges. That is what the version in the header is for, and the reader gets it
// from the same three fields world_cache_matches reads.
TEST_CASE("a cache from an older format is refused") {
    Scratch file("ws_test_cache_oldversion.world");
    {
        std::ofstream stream(file.path, std::ios::binary | std::ios::trunc);
        const u32 magic = 0x57534357u;
        const u32 version = 1u;
        const u64 key = world_cache_key("a clip", 32, 1234);
        stream.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
        stream.write(reinterpret_cast<const char*>(&version), sizeof(version));
        stream.write(reinterpret_cast<const char*>(&key), sizeof(key));
    }
    const u64 key = world_cache_key("a clip", 32, 1234);
    CHECK_FALSE(world_cache_matches(file.path, key));

    Side read;
    WorldCache in = read.handle();
    CHECK_FALSE(read_world_cache(file.path, key, in, nullptr));
}

// A truncated file is a cache miss and not a crash. It is what an interrupted write leaves, and
// the writer's rename-into-place is the other half of the same guarantee.
TEST_CASE("a truncated cache is a miss") {
    Scratch file("ws_test_cache_truncated.world");
    Side wrote;
    populate(wrote);
    WorldCache out = wrote.handle();
    out.regions = two_regions(true, false);
    const u64 key = world_cache_key("a clip", 32, 1234);
    REQUIRE(write_world_cache(file.path, key, out));

    const auto full = std::filesystem::file_size(file.path);
    REQUIRE(full > 64);
    std::filesystem::resize_file(file.path, full / 2);

    Side read;
    WorldCache in = read.handle();
    CHECK_FALSE(read_world_cache(file.path, key, in, nullptr));
}
