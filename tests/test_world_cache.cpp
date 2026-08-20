#include <doctest/doctest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
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

// ==========================================================================================
// R11j — a save after a save writes only what changed
// ==========================================================================================

namespace {

// A ladder of `count` leaves, each one different from its neighbours, so a change to one of them
// can be told from a change to any other. `sharp` many of them are done.
std::vector<CachedRegion> many_regions(u32 count, u32 sharp) {
    std::vector<CachedRegion> out;
    out.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        CachedRegion region;
        region.key[0] = static_cast<i64>(i) - 100;
        region.key[1] = static_cast<i64>(i % 7);
        region.key[2] = -static_cast<i64>(i);
        region.level = i % 9;
        region.low[0] = static_cast<f64>(i) * 0.25;
        region.low[1] = 1.0 / (static_cast<f64>(i) + 1.0);
        region.low[2] = -static_cast<f64>(i);
        region.high[0] = region.low[0] + 8.0;
        region.high[1] = region.low[1] + 8.0;
        region.high[2] = region.low[2] + 8.0;
        region.applied_per_metre = (i % 3 == 0) ? 32 : 8;
        region.done = i < sharp;
        out.push_back(region);
    }
    return out;
}

bool same_regions(const std::vector<CachedRegion>& a, const std::vector<CachedRegion>& b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        for (int axis = 0; axis < 3; ++axis) {
            if (a[i].key[axis] != b[i].key[axis]) return false;
            if (a[i].low[axis] != b[i].low[axis]) return false;
            if (a[i].high[axis] != b[i].high[axis]) return false;
        }
        if (a[i].level != b[i].level) return false;
        if (a[i].applied_per_metre != b[i].applied_per_metre) return false;
        if (a[i].done != b[i].done) return false;
    }
    return true;
}

u64 file_bytes(const std::string& path) {
    std::error_code error;
    const auto bytes = std::filesystem::file_size(path, error);
    return error ? 0 : static_cast<u64>(bytes);
}

}  // namespace

// THE GATE. A world written whole and the same world reached by appending have to BE the same
// world when they are read back, or none of the rest of this is worth having.
//
// Both arms end at the same place by two different routes: one file is written once, from the
// finished world; the other is written three times, and the third write only appends what the
// second did not already say. Chunk for chunk, voxel for voxel, and by the world's own content
// hash -- which is what every measurement in this repository is keyed on and the one number a
// world cannot fake agreement on.
TEST_CASE("a world reached by appending is the world written whole") {
    Scratch appended("ws_test_cache_appended.world");
    Scratch at_once("ws_test_cache_at_once.world");
    const u64 key = world_cache_key("a clip", 32, 1234);

    Side wrote;
    const std::vector<VoxelTypeId> materials = populate(wrote);
    const VoxelTypeId grey = materials[0];
    const VoxelTypeId brown = materials[1];

    WorldCache out = wrote.handle();
    out.materials = materials;
    out.regions = many_regions(2500, 900);
    out.stipple_taken = true;
    out.stipple.push_back(CachedStipple{grey, true});

    // Save one: the world as it stands.
    WorldCacheWritten first;
    REQUIRE(write_world_cache(appended.path, key, out, &first));
    CHECK_FALSE(first.incremental);
    const u64 whole_bytes = file_bytes(appended.path);

    // Somebody carries on building. A chunk changes, a chunk appears a long way off, and the
    // ladder sharpens two more leaves.
    apply_op(wrote.world,
             Op::fill_box(4, 1, 60, 60, 60, 90, 90, 90, brown, MatterReason::PlayerPlace),
             wrote.ledger);
    apply_op(wrote.world,
             Op::fill_box(5, 1, 5000, 5000, 5000, 5040, 5040, 5040, grey,
                          MatterReason::PlayerPlace),
             wrote.ledger);
    out.regions = many_regions(2500, 902);
    out.materials = materials;

    WorldCacheWritten second;
    REQUIRE(write_world_cache(appended.path, key, out, &second));
    CHECK(second.incremental);
    CHECK(second.chunks_written > 0);
    CHECK(second.chunks_left_alone > 0);
    CHECK(second.bytes_written < whole_bytes);

    // And again, with one voxel moved: the smallest change there is.
    REQUIRE(wrote.world.set(5001, 5001, 5001, kAir));
    out.regions = many_regions(2600, 902);
    WorldCacheWritten third;
    REQUIRE(write_world_cache(appended.path, key, out, &third));
    CHECK(third.incremental);
    CHECK(third.chunks_written == 1);
    CHECK(third.bytes_written < whole_bytes);

    // The same world, written once, with nothing appended to it.
    REQUIRE(write_world_cache(at_once.path, key, out));

    Side from_journal;
    WorldCache journal = from_journal.handle();
    REQUIRE(read_world_cache(appended.path, key, journal, nullptr));

    Side from_whole;
    WorldCache once = from_whole.handle();
    REQUIRE(read_world_cache(at_once.path, key, once, nullptr));

    CHECK(from_journal.world.content_hash() == wrote.world.content_hash());
    CHECK(from_journal.world.content_hash() == from_whole.world.content_hash());
    CHECK(from_journal.world.chunk_count() == from_whole.world.chunk_count());
    CHECK(from_journal.world.stats().solid_voxels == from_whole.world.stats().solid_voxels);
    CHECK(from_journal.world.stats().solid_voxels == wrote.world.stats().solid_voxels);
    CHECK(from_journal.world.stats().empty_bricks == 0);
    CHECK(from_journal.world.stats().empty_chunks == 0);

    // Everything beside the voxels, which an increment restates by block rather than whole.
    CHECK(same_regions(journal.regions, out.regions));
    CHECK(same_regions(journal.regions, once.regions));
    CHECK(journal.materials == materials);
    CHECK(journal.stipple_taken);
    REQUIRE(journal.stipple.size() == 1);
    CHECK(journal.stipple[0].type == grey);

    // Voxel for voxel across all three of the boxes above, including the one carved out.
    for (i64 z = -18; z <= 90; z += 5) {
        for (i64 y = -18; y <= 90; y += 5) {
            for (i64 x = -18; x <= 90; x += 5) {
                CHECK(from_journal.world.get(x, y, z) == wrote.world.get(x, y, z));
            }
        }
    }
    for (i64 z = 4998; z <= 5042; z += 3) {
        for (i64 y = 4998; y <= 5042; y += 3) {
            for (i64 x = 4998; x <= 5042; x += 3) {
                CHECK(from_journal.world.get(x, y, z) == wrote.world.get(x, y, z));
            }
        }
    }
}

// D701's fourth field, in a format that no longer renames a temporary into place.
//
// Two ways an append is cut off, and both have to be refused rather than read as a whole world. A
// file that says `running` is one a writer started and never finished; a file with bytes past the
// end of its journal is one whose segment landed and whose header never did. Reading either of
// them gives a world with a piece missing that claims to be complete, which is precisely what the
// `running`/`done` distinction was added to prevent.
TEST_CASE("an interrupted append is refused, not read as a whole world") {
    const u64 key = world_cache_key("a clip", 32, 1234);

    SUBCASE("a file still marked as being written") {
        Scratch file("ws_test_cache_running.world");
        Side wrote;
        populate(wrote);
        WorldCache out = wrote.handle();
        out.regions = two_regions(true, false);
        REQUIRE(write_world_cache(file.path, key, out));

        // It reads before the header is touched, which is what makes the check below about the
        // state byte rather than about the file being broken some other way.
        {
            Side read;
            WorldCache in = read.handle();
            CHECK(read_world_cache(file.path, key, in, nullptr));
        }

        // Byte 17 of the header is `done`. A writer that got as far as marking the file and no
        // further leaves it at `running`.
        {
            std::fstream stream(file.path, std::ios::binary | std::ios::in | std::ios::out);
            REQUIRE(stream.good());
            stream.seekp(17);
            const char running = 0;
            stream.write(&running, 1);
        }

        CHECK_FALSE(world_cache_matches(file.path, key));
        WorldCacheMode mode = WorldCacheMode::EditOnly;
        CHECK_FALSE(world_cache_mode_of(file.path, mode));
        Side read;
        WorldCache in = read.handle();
        CHECK_FALSE(read_world_cache(file.path, key, in, nullptr));
    }

    SUBCASE("a segment that landed with no header behind it") {
        Scratch file("ws_test_cache_torn.world");
        Side wrote;
        const std::vector<VoxelTypeId> materials = populate(wrote);
        WorldCache out = wrote.handle();
        out.materials = materials;
        out.regions = two_regions(true, false);
        REQUIRE(write_world_cache(file.path, key, out));

        // What a half-finished append leaves on disk: bytes past the end of the journal the
        // header knows about.
        {
            std::ofstream stream(file.path, std::ios::binary | std::ios::app);
            REQUIRE(stream.good());
            const std::vector<char> half(4096, 0x5A);
            stream.write(half.data(), static_cast<std::streamsize>(half.size()));
        }

        Side read;
        WorldCache in = read.handle();
        CHECK_FALSE(read_world_cache(file.path, key, in, nullptr));

        // And the next save does not append to it. It cannot: the file it would be appending to
        // is not a file this build would read, so it writes the world out whole and the cache is
        // good again -- which is the whole reason a refusal is safe.
        WorldCacheWritten again;
        REQUIRE(write_world_cache(file.path, key, out, &again));
        CHECK_FALSE(again.incremental);
        Side after;
        WorldCache back = after.handle();
        REQUIRE(read_world_cache(file.path, key, back, nullptr));
        CHECK(after.world.content_hash() == wrote.world.content_hash());
    }
}

// D721's own waste, as a number. A resumed run reaches the fixed point again with
// `refine_saved_regions_` back at nought, decides the world is worth keeping, and rewrites the
// whole file to say exactly what it already said. The file is the authority on whether anything
// changed, and it can answer without a byte being written.
TEST_CASE("saving a world the file already holds writes nothing") {
    Scratch file("ws_test_cache_nothing.world");
    const u64 key = world_cache_key("a clip", 32, 1234);

    Side wrote;
    const std::vector<VoxelTypeId> materials = populate(wrote);
    WorldCache out = wrote.handle();
    out.materials = materials;
    out.regions = many_regions(1500, 700);
    out.stipple_taken = true;
    out.stipple.push_back(CachedStipple{materials[0], false});

    REQUIRE(write_world_cache(file.path, key, out));
    const u64 was = file_bytes(file.path);

    WorldCacheWritten again;
    REQUIRE(write_world_cache(file.path, key, out, &again));
    CHECK(again.unchanged);
    CHECK(again.bytes_written == 0);
    CHECK(file_bytes(file.path) == was);

    // Twice more, to make sure "nothing changed" is not a one-off that leaves the file in a state
    // the next save has to rebuild.
    REQUIRE(write_world_cache(file.path, key, out, &again));
    CHECK(again.unchanged);
    REQUIRE(write_world_cache(file.path, key, out, &again));
    CHECK(again.unchanged);
    CHECK(file_bytes(file.path) == was);

    Side read;
    WorldCache in = read.handle();
    REQUIRE(read_world_cache(file.path, key, in, nullptr));
    CHECK(read.world.content_hash() == wrote.world.content_hash());
    CHECK(same_regions(in.regions, out.regions));
}

// The ladder is the part of a world that changes on every single save -- a save happens BECAUSE
// nodes were sharpened -- so a format that restated it whole would leave the bank exactly as
// expensive as it was. It is written in blocks, and one sharpened leaf moves one of them.
TEST_CASE("sharpening one leaf rewrites one block of the ladder") {
    Scratch file("ws_test_cache_blocks.world");
    const u64 key = world_cache_key("a clip", 32, 1234);

    Side wrote;
    populate(wrote);
    WorldCache out = wrote.handle();
    out.regions = many_regions(4000, 1000);   // four blocks of a thousand
    WorldCacheWritten first;
    REQUIRE(write_world_cache(file.path, key, out, &first));
    CHECK(first.region_blocks_total == 4);
    CHECK(first.region_blocks_written == 4);

    // One leaf, in the middle of the second block, comes good.
    out.regions[1500].done = true;
    out.regions[1500].applied_per_metre = 32;
    WorldCacheWritten second;
    REQUIRE(write_world_cache(file.path, key, out, &second));
    CHECK(second.incremental);
    CHECK(second.region_blocks_total == 4);
    CHECK(second.region_blocks_written == 1);
    CHECK(second.chunks_written == 0);

    Side read;
    WorldCache in = read.handle();
    REQUIRE(read_world_cache(file.path, key, in, nullptr));
    REQUIRE(in.regions.size() == 4000);
    CHECK(same_regions(in.regions, out.regions));
    CHECK(read.world.content_hash() == wrote.world.content_hash());
}

// A journal that only ever added would bring a demolished outbuilding back on the next load: the
// chunk is in an early segment, nothing later mentions it, and "the file does not say" reads as
// "leave it as it was". So a chunk the world has lost is named.
TEST_CASE("a chunk the world has lost is dropped from the journal") {
    Scratch file("ws_test_cache_dropped.world");
    const u64 key = world_cache_key("a clip", 32, 1234);

    Side wrote;
    const std::vector<VoxelTypeId> materials = populate(wrote);
    apply_op(wrote.world,
             Op::fill_box(6, 1, 5000, 5000, 5000, 5100, 5100, 5100, materials[0],
                          MatterReason::PlayerPlace),
             wrote.ledger);
    WorldCache out = wrote.handle();
    out.materials = materials;
    out.regions = two_regions(true, false);
    REQUIRE(write_world_cache(file.path, key, out));
    const u64 with_it = wrote.world.chunk_count();
    REQUIRE(with_it > 1);

    // Demolished, every voxel of it, so the chunk itself leaves the world.
    apply_op(wrote.world,
             Op::fill_box(7, 1, 5000, 5000, 5000, 5100, 5100, 5100, kAir,
                          MatterReason::PlayerBreak),
             wrote.ledger);
    CHECK(wrote.world.chunk_count() < with_it);

    WorldCacheWritten second;
    REQUIRE(write_world_cache(file.path, key, out, &second));
    CHECK(second.incremental);
    CHECK(second.chunks_dropped > 0);

    Side read;
    WorldCache in = read.handle();
    REQUIRE(read_world_cache(file.path, key, in, nullptr));
    CHECK(read.world.content_hash() == wrote.world.content_hash());
    CHECK(read.world.chunk_count() == wrote.world.chunk_count());
    CHECK(read.world.get(5050, 5050, 5050) == kAir);
    CHECK(read.world.stats().empty_chunks == 0);
}

// Without a bound the file creeps: a chunk rewritten fifty times is in the file fifty times, and
// the dead copies are never read but are always carried. So the journal is rewritten whole once it
// has doubled, and the world that comes back out is still the world that went in.
TEST_CASE("the journal is rewritten before it can creep") {
    Scratch file("ws_test_cache_creep.world");
    const u64 key = world_cache_key("a clip", 32, 1234);

    Side wrote;
    const std::vector<VoxelTypeId> materials = populate(wrote);
    WorldCache out = wrote.handle();
    out.materials = materials;
    out.regions = two_regions(true, false);
    REQUIRE(write_world_cache(file.path, key, out));
    const u64 whole = file_bytes(file.path);
    REQUIRE(whole > 0);

    u32 rewrites = 0;
    u64 largest = whole;
    for (i64 i = 0; i < 40; ++i) {
        // A voxel of change each time, somewhere nothing has been written yet, so every save has
        // something to append and no two saves append the same thing.
        REQUIRE(wrote.world.set(1000 + i * 9, 3, 4, materials[1]));
        WorldCacheWritten step;
        REQUIRE(write_world_cache(file.path, key, out, &step));
        if (!step.incremental) ++rewrites;
        largest = std::max(largest, file_bytes(file.path));
    }
    CHECK(rewrites > 0);
    CHECK(largest < whole * 4);

    Side read;
    WorldCache in = read.handle();
    REQUIRE(read_world_cache(file.path, key, in, nullptr));
    CHECK(read.world.content_hash() == wrote.world.content_hash());
    CHECK(read.world.stats().solid_voxels == wrote.world.stats().solid_voxels);
}
