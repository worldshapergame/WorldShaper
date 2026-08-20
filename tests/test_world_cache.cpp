#include <doctest/doctest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "core/jobs.hpp"
#include "core/time.hpp"
#include "forge/clip_script.hpp"
#include "forge/sample.hpp"
#include "game/clip.hpp"
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

// The whole file, and a prefix of a file. Between them they are how a crash is written down: the
// bytes the writer really produced, cut off at the moment a machine stopped.
std::vector<u8> read_all(const std::string& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) return {};
    const std::streamsize size = stream.tellg();
    stream.seekg(0);
    std::vector<u8> out(static_cast<usize>(size));
    if (size > 0 && !stream.read(reinterpret_cast<char*>(out.data()), size)) return {};
    return out;
}

void write_all(const std::string& path, const u8* data, usize size) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
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

// THE ONE THAT DECIDES WHETHER ANY OF THIS IS SAFE TO SHIP.
//
// The whole-world pass banks every two minutes for what may be an hour, and the thing banking
// exists to survive is a machine that stops. So a machine that stops DURING a save has to leave
// the world the last save left -- not a refused file, and not a world with a piece missing.
//
// The header is the commit record and it is written last, so an append writes its new segments
// past everything the header claims and only then moves `journal_bytes` over them. That makes
// three moments, and each of them is a real file here rather than an argument:
//
//   1. before any of the tail landed;
//   2. part way through the tail;
//   3. after the whole tail, before the header.
//
// Every one of them is built out of two real writes -- the file after save A, and the file after
// the append that turns it into B -- so the bytes are the bytes the writer actually produces. The
// first assertion is the one the arrangement stands on: **the append does not touch a single byte
// the old header committed to**, which is what makes moment 1 the untouched file and moments 2
// and 3 a good file with an ignorable tail on the end.
TEST_CASE("a machine that stops during an append still has the world it had before") {
    const u64 key = world_cache_key("a clip", 32, 1234);

    // World A, saved. These are the bytes and the hash every case below has to come back to.
    Scratch source("ws_test_cache_crash_source.world");
    Side wrote;
    const std::vector<VoxelTypeId> materials = populate(wrote);
    WorldCache out = wrote.handle();
    out.materials = materials;
    out.regions = many_regions(2000, 700);
    REQUIRE(write_world_cache(source.path, key, out));
    const u64 hash_a = wrote.world.content_hash();
    const u64 chunks_a = wrote.world.chunk_count();
    const u64 voxels_a = wrote.world.stats().solid_voxels;
    const std::vector<u8> after_a = read_all(source.path);
    REQUIRE(after_a.size() > 64);

    // ...and then somebody builds for two minutes, and the next bank appends.
    apply_op(wrote.world,
             Op::fill_box(9, 1, 4000, 4000, 4000, 4060, 4060, 4060, materials[1],
                          MatterReason::PlayerPlace),
             wrote.ledger);
    out.regions = many_regions(2100, 780);
    WorldCacheWritten appended;
    REQUIRE(write_world_cache(source.path, key, out, &appended));
    REQUIRE(appended.incremental);
    const std::vector<u8> after_b = read_all(source.path);
    REQUIRE(after_b.size() > after_a.size());

    // THE PROPERTY THE WHOLE THING RESTS ON: the append touched the sixty-four bytes of header and
    // NOTHING ELSE that world A committed to. Every byte of the journal A committed to is still
    // exactly where it was, so the file at each moment below is a file the writer really produces
    // rather than one this test invented.
    //
    // The header is the one thing that does move, and it moves LAST and all at once -- that is
    // what makes it the commit. If a writer marked the file on the way in, or wrote a segment over
    // the top of a committed one, it would show here.
    for (usize at = 64; at < after_a.size(); ++at) {
        REQUIRE(after_b[at] == after_a[at]);
    }
    // And it really did change, or the case above is comparing a file with itself.
    REQUIRE(std::memcmp(after_b.data(), after_a.data(), 64) != 0);

    const usize tail_from = after_a.size();
    const usize tail_bytes = after_b.size() - tail_from;

    // The file at any instant during the append: the OLD header, because the new one is the last
    // thing written and by definition has not gone down yet, in front of however much of the new
    // tail had landed. `at` bytes in total.
    const auto file_at_that_instant = [&](usize at) {
        std::vector<u8> bytes(after_b.begin(), after_b.begin() + static_cast<isize>(at));
        std::copy(after_a.begin(), after_a.begin() + 64, bytes.begin());
        return bytes;
    };

    SUBCASE("stopped before any of the new segments landed") {
        Scratch file("ws_test_cache_crash_1.world");
        const std::vector<u8> bytes = file_at_that_instant(tail_from);
        // Which is the file exactly as save A left it, byte for byte -- nothing was touched.
        REQUIRE(bytes == after_a);
        write_all(file.path, bytes.data(), bytes.size());
        Side read;
        WorldCache in = read.handle();
        REQUIRE(read_world_cache(file.path, key, in, nullptr));
        CHECK(read.world.content_hash() == hash_a);
        CHECK(read.world.chunk_count() == chunks_a);
        CHECK(read.world.stats().solid_voxels == voxels_a);
    }

    SUBCASE("stopped part way through the new segments") {
        Scratch file("ws_test_cache_crash_2.world");
        const std::vector<u8> bytes = file_at_that_instant(tail_from + tail_bytes / 2);
        REQUIRE(bytes.size() > after_a.size());
        write_all(file.path, bytes.data(), bytes.size());
        Side read;
        WorldCache in = read.handle();
        REQUIRE(read_world_cache(file.path, key, in, nullptr));
        CHECK(read.world.content_hash() == hash_a);
        CHECK(read.world.chunk_count() == chunks_a);
        CHECK(read.world.stats().solid_voxels == voxels_a);
    }

    SUBCASE("stopped after the new segments and before the header") {
        Scratch file("ws_test_cache_crash_3.world");
        // Every byte of the append on disk and the old header still in front of it, which is what
        // the file is at the instant before the last sixty-four bytes go down.
        const std::vector<u8> bytes = file_at_that_instant(after_b.size());
        write_all(file.path, bytes.data(), bytes.size());

        Side read;
        WorldCache in = read.handle();
        REQUIRE(read_world_cache(file.path, key, in, nullptr));
        CHECK(read.world.content_hash() == hash_a);
        CHECK(read.world.chunk_count() == chunks_a);
        CHECK(read.world.stats().solid_voxels == voxels_a);
        CHECK(same_regions(in.regions, many_regions(2000, 700)));

        // And the cache is not merely readable, it is still a cache: the next save appends to it
        // as normal, over the top of the tail nobody committed to, and trims what is left.
        WorldCacheWritten again;
        REQUIRE(write_world_cache(file.path, key, out, &again));
        CHECK(again.incremental);
        CHECK(file_bytes(file.path) == again.file_bytes);
        Side after;
        WorldCache back = after.handle();
        REQUIRE(read_world_cache(file.path, key, back, nullptr));
        CHECK(after.world.content_hash() == wrote.world.content_hash());
        CHECK(same_regions(back.regions, out.regions));
    }
}

// The other half, and it is a different question: a file with a piece missing from the MIDDLE of
// what it claims. There is nothing to fall back to there -- the header committed to bytes that are
// not on the disk -- so it is refused, and the caller rebuilds.
TEST_CASE("a file shorter than it claims, or corrupt in its header, is refused") {
    const u64 key = world_cache_key("a clip", 32, 1234);

    SUBCASE("a committed journal that is not all there") {
        Scratch file("ws_test_cache_short.world");
        Side wrote;
        populate(wrote);
        WorldCache out = wrote.handle();
        out.regions = two_regions(true, false);
        REQUIRE(write_world_cache(file.path, key, out));

        const auto full = std::filesystem::file_size(file.path);
        REQUIRE(full > 128);
        std::filesystem::resize_file(file.path, full - 64);

        CHECK_FALSE(world_cache_matches(file.path, key));
        Side read;
        WorldCache in = read.handle();
        CHECK_FALSE(read_world_cache(file.path, key, in, nullptr));
    }

    SUBCASE("a header that was itself torn") {
        Scratch file("ws_test_cache_tornhead.world");
        Side wrote;
        populate(wrote);
        WorldCache out = wrote.handle();
        out.regions = two_regions(true, false);
        REQUIRE(write_world_cache(file.path, key, out));

        // One byte of the journal length, which the check hash covers. This is the only moment of
        // an append that costs the cache, and it has to be caught rather than believed.
        {
            std::fstream stream(file.path, std::ios::binary | std::ios::in | std::ios::out);
            REQUIRE(stream.good());
            stream.seekp(24);
            const char rubbish = 0x7F;
            stream.write(&rubbish, 1);
        }

        CHECK_FALSE(world_cache_matches(file.path, key));
        WorldCacheMode mode = WorldCacheMode::EditOnly;
        CHECK_FALSE(world_cache_mode_of(file.path, mode));
        Side read;
        WorldCache in = read.handle();
        CHECK_FALSE(read_world_cache(file.path, key, in, nullptr));
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
    out.regions = many_regions(4000, 1000);   // sixty-three blocks of sixty-four
    WorldCacheWritten first;
    REQUIRE(write_world_cache(file.path, key, out, &first));
    CHECK(first.region_blocks_total == 63);
    CHECK(first.region_blocks_written == 63);

    // One leaf, a long way into the list, comes good.
    out.regions[1500].done = true;
    out.regions[1500].applied_per_metre = 32;
    WorldCacheWritten second;
    REQUIRE(write_world_cache(file.path, key, out, &second));
    CHECK(second.incremental);
    CHECK(second.region_blocks_total == 63);
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

// ============================================================================================
// The measurement, on a real world. Skipped by default -- it samples a real clip.
//
//   ws_tests.exe --no-skip --test-case="banking the facility, three saves"
//
// WS_FACILITY_PER_METRE sets the grain (8 by default, 32 for the world the game builds) and
// WS_LADDER_LEAVES how many leaves the ladder is carrying, because the leaf set is a real part of
// what the file costs and it changes on every save by construction.
// ============================================================================================

TEST_CASE("banking the facility, three saves" * doctest::skip()) {
    std::string named = "clips/facility.clip";
    if (const char* asked = std::getenv("WS_GATE_CLIP")) named = asked;
    const std::string prefixes[] = {"", "../", "../../", "../../../"};
    std::string clip_path;
    for (const std::string& prefix : prefixes) {
        if (std::filesystem::exists(prefix + named)) {
            clip_path = prefix + named;
            break;
        }
    }
    REQUIRE_MESSAGE(!clip_path.empty(), "the clip named is not beside the test binary");

    i32 per_metre = 8;
    if (const char* asked = std::getenv("WS_FACILITY_PER_METRE")) per_metre = std::atoi(asked);
    u32 leaves = 40000;
    if (const char* asked = std::getenv("WS_LADDER_LEAVES")) {
        leaves = static_cast<u32>(std::atoi(asked));
    }

    JobSystem jobs;
    TagRegistry tags;
    PropertyRegistry properties;
    VoxelTypeTable types;
    forge::Script script = forge::load_clip_script(clip_path, types, tags);
    script.settings.voxels_per_metre = per_metre;

    const u64 sample_began = now_ns();
    const forge::SampleResult built =
        forge::sample(script.field, script.solid, script.paint, script.settings, &jobs);
    const f64 sample_ms = ns_to_ms(now_ns() - sample_began);
    REQUIRE_FALSE(built.clip.empty());

    World world;
    MatterLedger ledger;
    paste_clip(world, ledger, built.clip, built.origin_voxel[0], built.origin_voxel[1],
               built.origin_voxel[2], PasteMode::Replace, MatterReason::Generation, 1, &jobs,
               types.type_count());
    const WorldStats stats = world.stats();

    Scratch file("ws_bank_facility.world");
    const u64 key = world_cache_key("facility", per_metre, 1);
    WorldCache out;
    out.tags = &tags;
    out.properties = &properties;
    out.types = &types;
    out.world = &world;
    out.ledger = &ledger;
    out.regions = many_regions(leaves, leaves / 2);
    out.stipple_taken = true;

    std::printf("\nfacility      %s at %d voxels a metre, sampled in %.0f ms\n", clip_path.c_str(),
                per_metre, sample_ms);
    std::printf("world         %llu chunks, %llu bricks, %llu solid voxels, %u ladder leaves\n",
                static_cast<unsigned long long>(stats.chunks),
                static_cast<unsigned long long>(stats.bricks),
                static_cast<unsigned long long>(stats.solid_voxels), leaves);

    // One: the first bank of a run. There is nothing on disk, so this is the whole world.
    WorldCacheWritten first;
    const u64 began_first = now_ns();
    REQUIRE(write_world_cache(file.path, key, out, &first));
    const f64 first_ms = ns_to_ms(now_ns() - began_first);
    CHECK_FALSE(first.incremental);

    // Two: a bank where nothing has moved since. This is the resumed run that used to rewrite the
    // whole file to say what it already said.
    WorldCacheWritten second;
    const u64 began_second = now_ns();
    REQUIRE(write_world_cache(file.path, key, out, &second));
    const f64 second_ms = ns_to_ms(now_ns() - began_second);
    CHECK(second.unchanged);
    CHECK(second.bytes_written == 0);

    // Three: two minutes of building later. One node of the ladder came good and one voxel of one
    // chunk moved with it, which is the smallest real bank there is.
    const std::vector<ChunkCoord> coords = world.sorted_chunk_coords();
    REQUIRE_FALSE(coords.empty());
    const ChunkCoord touched = coords[coords.size() / 2];
    bool moved = false;
    for (i64 lz = 0; lz < 256 && !moved; ++lz) {
        for (i64 ly = 0; ly < 256 && !moved; ++ly) {
            for (i64 lx = 0; lx < 256 && !moved; ++lx) {
                const i64 x = touched.x * 256 + lx;
                const i64 y = touched.y * 256 + ly;
                const i64 z = touched.z * 256 + lz;
                if (world.get(x, y, z) != kAir) moved = world.set(x, y, z, kAir);
            }
        }
    }
    REQUIRE(moved);
    out.regions[leaves / 2].done = true;
    out.regions[leaves / 2].applied_per_metre = per_metre;

    WorldCacheWritten third;
    const u64 began_third = now_ns();
    REQUIRE(write_world_cache(file.path, key, out, &third));
    const f64 third_ms = ns_to_ms(now_ns() - began_third);
    CHECK(third.incremental);
    CHECK(third.chunks_written == 1);

    std::printf("\n                       bytes written        ms   chunks written / left alone\n");
    std::printf("1 whole            %16llu  %8.0f   %u / %u\n",
                static_cast<unsigned long long>(first.bytes_written), first_ms,
                first.chunks_written, first.chunks_left_alone);
    std::printf("2 nothing changed  %16llu  %8.0f   %u / %u\n",
                static_cast<unsigned long long>(second.bytes_written), second_ms,
                second.chunks_written, second.chunks_left_alone);
    std::printf("3 one node, one voxel %13llu  %8.0f   %u / %u   (%u of %u leaf blocks)\n",
                static_cast<unsigned long long>(third.bytes_written), third_ms,
                third.chunks_written, third.chunks_left_alone, third.region_blocks_written,
                third.region_blocks_total);
    std::printf("the file is %llu bytes\n\n",
                static_cast<unsigned long long>(third.file_bytes));

    // And it is still the world. Read back from a journal of three segments plus its directories.
    World back;
    MatterLedger back_ledger;
    TagRegistry back_tags;
    PropertyRegistry back_properties;
    VoxelTypeTable back_types;
    WorldCache in;
    in.tags = &back_tags;
    in.properties = &back_properties;
    in.types = &back_types;
    in.world = &back;
    in.ledger = &back_ledger;
    const u64 read_began = now_ns();
    REQUIRE(read_world_cache(file.path, key, in, &jobs));
    std::printf("read back in %.0f ms\n", ns_to_ms(now_ns() - read_began));
    CHECK(back.content_hash() == world.content_hash());
    CHECK(back.stats().solid_voxels == world.stats().solid_voxels);
    CHECK(back.chunk_count() == world.chunk_count());
    CHECK(same_regions(in.regions, out.regions));
}
