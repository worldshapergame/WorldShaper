// R11f — a world written as its clip plus what somebody changed.
//
// Every case here is about the same question: can a person's building come back out of a file that
// does not contain it? The difference is what the clip does not say, and the whole of this file is
// the list of ways "the clip does not say" can be got wrong.
//
// The one that matters most is the carve. Somebody cuts a doorway through a wall the clip builds:
// the saved world holds AIR there, the clip holds stone, and "the file does not mention it" already
// means "leave the clip's answer alone". A doorway is therefore the one edit that is invisible
// unless the file goes out of its way to say so, and a format that forgets it comes back with the
// wall healed and no error anywhere.

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/jobs.hpp"
#include "core/time.hpp"
#include "forge/clip_script.hpp"
#include "forge/sample.hpp"
#include "game/clip.hpp"
#include "world/history.hpp"
#include "world/ledger.hpp"
#include "world/op.hpp"
#include "world/property.hpp"
#include "world/tags.hpp"
#include "world/voxel_type.hpp"
#include "world/world.hpp"
#include "world/world_cache.hpp"

using namespace ws;

namespace {

VisualRecord colour(u8 r, u8 g, u8 b) {
    VisualRecord v{};
    v.red = r;
    v.green = g;
    v.blue = b;
    v.opacity = 255;
    return v;
}

// One end of a round trip: the registries, the world somebody has, and the world their clip
// builds. Two of these per test, because reading back into the registries that wrote is how a
// round trip passes without anything ever having been encoded.
//
// `clip_world` is what R11f calls the baseline. It is built by `build_clip_world` below, which
// stands in for the sampler: a pure function of nothing, so both ends of the trip can build the
// same one and the reading end genuinely re-derives what the writing end left out.
struct Side {
    TagRegistry tags;
    PropertyRegistry properties;
    VoxelTypeTable types;
    World clip_world;
    MatterLedger clip_ledger;
    World world;
    MatterLedger ledger;
    VoxelTypeId stone = 0;
    VoxelTypeId wood = 0;
    VoxelTypeId marble = 0;

    // The same three materials, and `reordered` interns them the other way round.
    //
    // A type id is the order things were interned in, nothing more. Two runs of one clip that met
    // its materials in a different order -- a different camera, a different box sampled first --
    // hand back the same building with its ids permuted. That used to be an assumption here
    // ("this is an assumption, not a guarantee") and it is now a case: see "a baseline interned in
    // another order comes back in its own materials" below.
    void make_types(bool reordered = false) {
        BehaviourRecord rock{};
        rock.material = 1;
        rock.tags.add(tags.find("stone"));
        rock.properties.set(props::kDensity, PropertyValue::from_uint(2600));
        BehaviourRecord timber{};
        timber.material = 2;
        timber.tags.add(tags.find("wood"));
        BehaviourRecord cut{};
        cut.material = 3;

        if (reordered) {
            marble = types.intern(colour(240, 240, 230), cut);
            wood = types.intern(colour(90, 60, 30), timber);
            stone = types.intern(colour(120, 120, 120), rock);
            return;
        }
        stone = types.intern(colour(120, 120, 120), rock);
        wood = types.intern(colour(90, 60, 30), timber);
        marble = types.intern(colour(240, 240, 230), cut);
    }

    WorldCache handle() {
        WorldCache out;
        out.tags = &tags;
        out.properties = &properties;
        out.types = &types;
        out.world = &world;
        out.ledger = &ledger;
        return out;
    }
};

// What the clip builds. Deterministic, so both ends of a round trip build the same thing, and
// deliberately more than one chunk: a wall either side of the origin, a separate outbuilding
// eight chunks away, and a lintel that crosses a chunk boundary.
//
// `flavour` is the clip CHANGING under a file that was written against it -- the sampler moved,
// somebody edited the script -- which is the case the baseline fingerprint exists to catch.
void build_clip_world(World& world, MatterLedger& ledger, VoxelTypeId stone, VoxelTypeId wood,
                      bool flavour = false) {
    apply_op(world, Op::fill_box(1, 1, -40, -40, -40, 39, 39, 39, stone, MatterReason::Generation),
             ledger);
    apply_op(world,
             Op::fill_box(2, 1, 250, 250, 250, 290, 280, 280, wood, MatterReason::Generation),
             ledger);
    apply_op(world, Op::fill_box(3, 1, 100, 0, 0, 140, 20, 20, stone, MatterReason::Generation),
             ledger);
    if (flavour) {
        // A different clip, agreeing with the first everywhere except one brick of the wall.
        apply_op(world, Op::fill_box(4, 1, 16, 16, 16, 23, 23, 23, wood, MatterReason::Generation),
                 ledger);
    }
}

// Both worlds, voxel for voxel, over every box either of them can hold anything in. Not a hash and
// not a count: a hash says two worlds differ and a probe grid says nothing at all about the voxel
// between two probes, and the thing being tested here is precisely whether one voxel came back.
void must_be_identical(const World& got, const World& want) {
    CHECK(got.content_hash() == want.content_hash());
    CHECK(got.stats().solid_voxels == want.stats().solid_voxels);
    CHECK(got.chunk_count() == want.chunk_count());
    // The wall, exhaustively -- 512,000 voxels, and it is where every carve in this file lands.
    u64 differing = 0;
    for (i64 z = -41; z <= 40; ++z) {
        for (i64 y = -41; y <= 40; ++y) {
            for (i64 x = -41; x <= 40; ++x) {
                if (got.get(x, y, z) != want.get(x, y, z)) ++differing;
            }
        }
    }
    CHECK(differing == 0);
    // And the two outlying pieces, which is where a lost chunk would show.
    u64 outlying = 0;
    for (i64 z = 249; z <= 291; ++z) {
        for (i64 y = 249; y <= 281; ++y) {
            for (i64 x = 249; x <= 291; ++x) {
                if (got.get(x, y, z) != want.get(x, y, z)) ++outlying;
            }
        }
    }
    for (i64 z = -1; z <= 21; ++z) {
        for (i64 y = -1; y <= 21; ++y) {
            for (i64 x = 99; x <= 141; ++x) {
                if (got.get(x, y, z) != want.get(x, y, z)) ++outlying;
            }
        }
    }
    CHECK(outlying == 0);
}

// A path in the system temporary directory, removed however the test leaves.
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
    u64 bytes() const {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        return error ? 0u : static_cast<u64>(size);
    }
};

CachedEditBox box_of(i64 x0, i64 y0, i64 z0, i64 x1, i64 y1, i64 z1) {
    CachedEditBox box;
    box.low[0] = x0;
    box.low[1] = y0;
    box.low[2] = z0;
    box.high[0] = x1;
    box.high[1] = y1;
    box.high[2] = z1;
    return box;
}

}  // namespace

// ============================================================================================
// The round trip. This is the deliverable.
// ============================================================================================

TEST_CASE("a carved world comes back carved, with the clip re-derived around it") {
    Scratch file("ws_test_edits_roundtrip.world");
    const u64 key = world_cache_key("a clip", 32, 1234);

    Side wrote;
    wrote.make_types();
    build_clip_world(wrote.clip_world, wrote.clip_ledger, wrote.stone, wrote.wood);
    build_clip_world(wrote.world, wrote.ledger, wrote.stone, wrote.wood);
    REQUIRE(wrote.world.content_hash() == wrote.clip_world.content_hash());

    // THE EDIT THAT CAN BE LOST. A doorway cut clean through the wall: the saved world holds air
    // where the clip holds stone, and a file that only writes what it HAS writes nothing here.
    apply_op(wrote.world,
             Op::fill_box(10, 1, -12, -12, -12, 11, 11, 11, kAir, MatterReason::PlayerBreak),
             wrote.ledger);
    // A second carve that takes a whole chunk out, so the case where a chunk stops existing is
    // exercised as well as the case where one loses a few bricks.
    apply_op(wrote.world,
             Op::fill_box(11, 1, 250, 250, 250, 290, 280, 280, kAir, MatterReason::PlayerBreak),
             wrote.ledger);
    // And matter placed where the clip puts none, plus matter of a material the clip never used
    // laid over matter it did.
    apply_op(wrote.world,
             Op::fill_box(12, 1, 60, 60, 60, 70, 70, 70, wrote.marble, MatterReason::PlayerPlace),
             wrote.ledger);
    apply_op(wrote.world,
             Op::fill_box(13, 1, -30, -30, -30, -25, -25, -25, wrote.marble,
                          MatterReason::PlayerPlace),
             wrote.ledger);
    // And a hut built in an empty field: a chunk the clip has never heard of, so there is nothing
    // to be a difference FROM and every brick of it has to be written.
    apply_op(wrote.world,
             Op::fill_box(14, 1, 5000, 5000, 5000, 5031, 5031, 5031, wrote.marble,
                          MatterReason::PlayerPlace),
             wrote.ledger);

    WorldCache out = wrote.handle();
    out.baseline = &wrote.clip_world;
    out.edits_named = true;
    out.edited = {box_of(-12, -12, -12, 11, 11, 11),     box_of(250, 250, 250, 290, 280, 280),
                  box_of(60, 60, 60, 70, 70, 70),         box_of(-30, -30, -30, -25, -25, -25),
                  box_of(5000, 5000, 5000, 5031, 5031, 5031)};
    REQUIRE(write_world_cache(file.path, key, out));

    // The reading end knows only the clip. It builds what the clip builds and lays the file over
    // it, in place, which is the ordinary path.
    Side read;
    read.make_types();
    build_clip_world(read.world, read.ledger, read.stone, read.wood);
    WorldCache in = read.handle();
    in.baseline = &read.world;
    REQUIRE(read_world_cache(file.path, key, in, nullptr));

    CHECK(in.mode == WorldCacheMode::EditOnly);
    CHECK(in.baseline_agreed);
    CHECK(in.baseline_chunks_differing == 0);
    CHECK(in.edits_named);
    CHECK(in.edited.size() == 5);

    must_be_identical(read.world, wrote.world);

    // Said again as the things a person would look at, so a failure names what is wrong rather
    // than only that something is.
    CHECK(read.world.get(0, 0, 0) == kAir);          // the doorway
    CHECK(read.world.get(-12, -12, -12) == kAir);    // its corner
    CHECK(read.world.get(-13, -13, -13) == read.stone);   // and the wall beside it
    CHECK(read.world.get(12, 12, 12) == read.stone);
    CHECK(read.world.get(270, 270, 270) == kAir);    // the demolished outbuilding
    CHECK(read.world.get(-27, -27, -27) == read.marble);
    CHECK(read.world.get(5015, 5015, 5015) == read.marble);   // the hut in the empty field
    CHECK(read.world.get(120, 10, 10) == read.stone);     // untouched, and not in the file at all

    // Nothing left standing where an edit emptied it. A brick or a chunk kept alive with nothing
    // in it is `world_has` claiming matter the world does not have, which the marcher draws as a
    // cube it can never build and the chisel's own raycast cannot find (D620, D621) -- and the
    // clearing path here is exactly where one would be left behind.
    const WorldStats after = read.world.stats();
    CHECK(after.empty_bricks == 0);
    CHECK(after.empty_chunks == 0);
}

TEST_CASE("the carve survives with a baseline in a world of its own") {
    // The same trip with the baseline kept separate from the world it is laid into, which is the
    // other of the two shapes WorldCache::baseline allows.
    Scratch file("ws_test_edits_separate.world");
    const u64 key = world_cache_key("a clip", 32, 99);

    Side wrote;
    wrote.make_types();
    build_clip_world(wrote.clip_world, wrote.clip_ledger, wrote.stone, wrote.wood);
    build_clip_world(wrote.world, wrote.ledger, wrote.stone, wrote.wood);
    apply_op(wrote.world,
             Op::fill_box(10, 1, -4, -40, -4, 4, 39, 4, kAir, MatterReason::PlayerBreak),
             wrote.ledger);

    WorldCache out = wrote.handle();
    out.baseline = &wrote.clip_world;
    REQUIRE(write_world_cache(file.path, key, out));

    Side read;
    read.make_types();
    build_clip_world(read.clip_world, read.clip_ledger, read.stone, read.wood);
    WorldCache in = read.handle();
    in.baseline = &read.clip_world;   // a different world from `in.world`, which starts empty
    REQUIRE(read_world_cache(file.path, key, in, nullptr));

    CHECK(in.baseline_agreed);
    must_be_identical(read.world, wrote.world);
    CHECK(read.world.get(0, 0, 0) == kAir);
    CHECK(read.world.get(5, 0, 0) == read.stone);
}

// ============================================================================================
// The file says which of the two things it is
// ============================================================================================

TEST_CASE("a whole world and a difference are different files, and the header says which") {
    Scratch whole("ws_test_edits_mode_whole.world");
    Scratch diff("ws_test_edits_mode_diff.world");
    const u64 key = world_cache_key("a clip", 32, 7);

    Side wrote;
    wrote.make_types();
    build_clip_world(wrote.clip_world, wrote.clip_ledger, wrote.stone, wrote.wood);
    build_clip_world(wrote.world, wrote.ledger, wrote.stone, wrote.wood);

    WorldCache full = wrote.handle();
    REQUIRE(write_world_cache(whole.path, key, full));
    WorldCache thin = wrote.handle();
    thin.baseline = &wrote.clip_world;
    REQUIRE(write_world_cache(diff.path, key, thin));

    WorldCacheMode mode = WorldCacheMode::EditOnly;
    REQUIRE(world_cache_mode_of(whole.path, mode));
    CHECK(mode == WorldCacheMode::Whole);
    REQUIRE(world_cache_mode_of(diff.path, mode));
    CHECK(mode == WorldCacheMode::EditOnly);
    CHECK_FALSE(world_cache_mode_of(whole.path + ".missing", mode));

    // And the size, which is the whole point of the exercise: this world has no edits in it at
    // all, so the difference carries no voxels -- 1,540 bytes against 20,991, and nearly all of
    // the 1,540 is the type table and the baseline fingerprint. The ratio is the floor and not the
    // headline: a toy world's fixed header is most of its difference file, and a real one's is
    // rounding. Gated at a fifth, which is far below what was measured, because the number that
    // matters is on the facility and not here.
    CHECK(diff.bytes() * 5 < whole.bytes());

    // An unedited world through the difference is still the same world.
    Side read;
    read.make_types();
    build_clip_world(read.world, read.ledger, read.stone, read.wood);
    WorldCache in = read.handle();
    in.baseline = &read.world;
    REQUIRE(read_world_cache(diff.path, key, in, nullptr));
    must_be_identical(read.world, wrote.world);
}

TEST_CASE("a difference with no world to lay it over is refused rather than applied") {
    Scratch file("ws_test_edits_nobaseline.world");
    const u64 key = world_cache_key("a clip", 32, 11);

    Side wrote;
    wrote.make_types();
    build_clip_world(wrote.clip_world, wrote.clip_ledger, wrote.stone, wrote.wood);
    build_clip_world(wrote.world, wrote.ledger, wrote.stone, wrote.wood);
    apply_op(wrote.world,
             Op::fill_box(10, 1, -12, -12, -12, 11, 11, 11, kAir, MatterReason::PlayerBreak),
             wrote.ledger);

    WorldCache out = wrote.handle();
    out.baseline = &wrote.clip_world;
    REQUIRE(write_world_cache(file.path, key, out));

    Side read;
    read.make_types();
    WorldCache in = read.handle();   // no baseline
    CHECK_FALSE(read_world_cache(file.path, key, in, nullptr));
    // And nothing was written into the world on the way to refusing. A partly-applied difference
    // is the failure that looks like a world still loading.
    CHECK(read.world.chunk_count() == 0);
}

TEST_CASE("a world written before the mode byte is rejected, never misread") {
    Scratch file("ws_test_edits_version.world");
    const u64 key = world_cache_key("a clip", 32, 13);

    Side wrote;
    wrote.make_types();
    build_clip_world(wrote.world, wrote.ledger, wrote.stone, wrote.wood);
    WorldCache out = wrote.handle();
    REQUIRE(write_world_cache(file.path, key, out));
    REQUIRE(world_cache_matches(file.path, key));

    // Put the previous version number back in the header. Everything after the key is at a
    // different offset in a version 5 file, so a reader that let this through would not fail --
    // it would read a whole world as a difference, or a type table as a fingerprint.
    {
        std::fstream patch(file.path, std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(patch.good());
        patch.seekp(4);
        const u32 five = 5u;
        patch.write(reinterpret_cast<const char*>(&five), sizeof(five));
    }

    CHECK_FALSE(world_cache_matches(file.path, key));
    WorldCacheMode mode = WorldCacheMode::Whole;
    CHECK_FALSE(world_cache_mode_of(file.path, mode));

    Side read;
    read.make_types();
    WorldCache in = read.handle();
    CHECK_FALSE(read_world_cache(file.path, key, in, nullptr));
    CHECK(read.world.chunk_count() == 0);
}

// ============================================================================================
// The baseline, and what happens when it is not the one the file was cut from
// ============================================================================================

TEST_CASE("a difference laid over the wrong world says so, and keeps every edit it names") {
    Scratch file("ws_test_edits_moved.world");
    const u64 key = world_cache_key("a clip", 32, 17);

    Side wrote;
    wrote.make_types();
    build_clip_world(wrote.clip_world, wrote.clip_ledger, wrote.stone, wrote.wood);
    build_clip_world(wrote.world, wrote.ledger, wrote.stone, wrote.wood);
    apply_op(wrote.world,
             Op::fill_box(10, 1, -12, -12, -12, 11, 11, 11, kAir, MatterReason::PlayerBreak),
             wrote.ledger);

    WorldCache out = wrote.handle();
    out.baseline = &wrote.clip_world;
    out.edits_named = true;
    out.edited = {box_of(-12, -12, -12, 11, 11, 11)};
    REQUIRE(write_world_cache(file.path, key, out));

    // The clip has moved under the file: one brick of the wall is wood now.
    Side read;
    read.make_types();
    build_clip_world(read.world, read.ledger, read.stone, read.wood, /*flavour=*/true);
    WorldCache in = read.handle();
    in.baseline = &read.world;
    REQUIRE(read_world_cache(file.path, key, in, nullptr));

    CHECK_FALSE(in.baseline_agreed);
    CHECK(in.baseline_chunks_differing == 1);
    // Read anyway, and the doorway is still cut. Refusing would hand the caller nothing, and a
    // caller with nothing rebuilds from the clip -- which loses the doorway outright.
    CHECK(read.world.get(0, 0, 0) == kAir);
    CHECK(read.world.get(-12, -12, -12) == kAir);
    CHECK(read.world.get(-13, -13, -13) == read.stone);
    // And the clip's own change came through, because that is what "re-derived around it" means.
    CHECK(read.world.get(20, 20, 20) == read.wood);
}

TEST_CASE("matter the baseline has and the file never saw is counted as a disagreement") {
    Scratch file("ws_test_edits_extra.world");
    const u64 key = world_cache_key("a clip", 32, 19);

    Side wrote;
    wrote.make_types();
    build_clip_world(wrote.clip_world, wrote.clip_ledger, wrote.stone, wrote.wood);
    build_clip_world(wrote.world, wrote.ledger, wrote.stone, wrote.wood);
    WorldCache out = wrote.handle();
    out.baseline = &wrote.clip_world;
    REQUIRE(write_world_cache(file.path, key, out));

    Side read;
    read.make_types();
    build_clip_world(read.world, read.ledger, read.stone, read.wood);
    // A whole chunk the file's fingerprint has no entry for. Every named chunk still agrees, so
    // only the other direction of the check can see this one.
    apply_op(read.world,
             Op::fill_box(20, 1, 2000, 2000, 2000, 2050, 2050, 2050, read.stone,
                          MatterReason::Generation),
             read.ledger);
    WorldCache in = read.handle();
    in.baseline = &read.world;
    REQUIRE(read_world_cache(file.path, key, in, nullptr));
    CHECK_FALSE(in.baseline_agreed);
    CHECK(in.baseline_chunks_differing >= 1);
}

// ============================================================================================
// Trap 7: what nobody said, and what somebody said was nothing
// ============================================================================================

TEST_CASE("nobody named the edits and nothing was edited are different files") {
    const u64 key = world_cache_key("a clip", 32, 23);
    Side wrote;
    wrote.make_types();
    build_clip_world(wrote.clip_world, wrote.clip_ledger, wrote.stone, wrote.wood);
    build_clip_world(wrote.world, wrote.ledger, wrote.stone, wrote.wood);

    {
        Scratch file("ws_test_edits_said_none.world");
        WorldCache out = wrote.handle();
        out.baseline = &wrote.clip_world;
        out.edits_named = true;   // the op log was read, and it was empty
        REQUIRE(write_world_cache(file.path, key, out));

        Side read;
        read.make_types();
        build_clip_world(read.world, read.ledger, read.stone, read.wood);
        WorldCache in = read.handle();
        in.baseline = &read.world;
        REQUIRE(read_world_cache(file.path, key, in, nullptr));
        CHECK(in.edits_named);
        CHECK(in.edited.empty());
    }
    {
        Scratch file("ws_test_edits_said_nothing.world");
        WorldCache out = wrote.handle();
        out.baseline = &wrote.clip_world;
        out.edits_named = false;   // nobody asked the op log
        REQUIRE(write_world_cache(file.path, key, out));

        Side read;
        read.make_types();
        build_clip_world(read.world, read.ledger, read.stone, read.wood);
        WorldCache in = read.handle();
        in.baseline = &read.world;
        REQUIRE(read_world_cache(file.path, key, in, nullptr));
        CHECK_FALSE(in.edits_named);
        CHECK(in.edited.empty());
    }
    {
        // And a whole-world file makes no claim about edits at all, which is a third thing again.
        Scratch file("ws_test_edits_whole_says_nothing.world");
        WorldCache out = wrote.handle();
        REQUIRE(write_world_cache(file.path, key, out));

        Side read;
        read.make_types();
        WorldCache in = read.handle();
        in.edits_named = true;   // whatever the caller left in the struct
        in.edited = {box_of(0, 0, 0, 1, 1, 1)};
        REQUIRE(read_world_cache(file.path, key, in, nullptr));
        CHECK_FALSE(in.edits_named);
        CHECK(in.edited.empty());
    }
}

TEST_CASE("a world may not be written as a difference from itself") {
    Scratch file("ws_test_edits_selfdiff.world");
    Side wrote;
    wrote.make_types();
    build_clip_world(wrote.world, wrote.ledger, wrote.stone, wrote.wood);
    WorldCache out = wrote.handle();
    out.baseline = &wrote.world;
    CHECK_FALSE(write_world_cache(file.path, world_cache_key("a clip", 32, 29), out));
}

// ============================================================================================
// The gap the difference alone cannot see, and the mechanism that closes it
// ============================================================================================

TEST_CASE("a hand-placed voxel that agrees with the clip survives, when it was named") {
    // Somebody breaks a brick of the wall and puts the same stone straight back. The world now
    // agrees with the clip there, so the difference has nothing to write -- and when the clip
    // moves under the file, the brick comes back as whatever the clip says now. Naming the box is
    // what keeps it.
    Scratch named_file("ws_test_edits_named_keep.world");
    Scratch bare_file("ws_test_edits_bare_keep.world");
    const u64 key = world_cache_key("a clip", 32, 31);

    Side wrote;
    wrote.make_types();
    build_clip_world(wrote.clip_world, wrote.clip_ledger, wrote.stone, wrote.wood);
    build_clip_world(wrote.world, wrote.ledger, wrote.stone, wrote.wood);
    apply_op(wrote.world,
             Op::fill_box(10, 1, 16, 16, 16, 23, 23, 23, kAir, MatterReason::PlayerBreak),
             wrote.ledger);
    apply_op(wrote.world,
             Op::fill_box(11, 1, 16, 16, 16, 23, 23, 23, wrote.stone, MatterReason::PlayerPlace),
             wrote.ledger);
    REQUIRE(wrote.world.content_hash() == wrote.clip_world.content_hash());

    WorldCache named = wrote.handle();
    named.baseline = &wrote.clip_world;
    named.edits_named = true;
    named.edited = {box_of(16, 16, 16, 23, 23, 23)};
    REQUIRE(write_world_cache(named_file.path, key, named));

    WorldCache bare = wrote.handle();
    bare.baseline = &wrote.clip_world;
    REQUIRE(write_world_cache(bare_file.path, key, bare));
    // The named file is bigger, and that is the whole of what naming buys: one brick of stone
    // written down because a person chose it, not because it differs from anything.
    CHECK(named_file.bytes() > bare_file.bytes());

    {
        Side read;
        read.make_types();
        build_clip_world(read.world, read.ledger, read.stone, read.wood, /*flavour=*/true);
        WorldCache in = read.handle();
        in.baseline = &read.world;
        REQUIRE(read_world_cache(named_file.path, key, in, nullptr));
        CHECK(read.world.get(20, 20, 20) == read.stone);   // the person's stone, kept
    }
    {
        // And the same file without the naming. THIS USED TO BE THE DOCUMENTED LOSS: a brick that
        // agrees with the clip and is not named came back as the clip now builds it, asserted here
        // so that it was a decision somebody made rather than a surprise.
        //
        // **R12d closes it, and from the other end.** The brick itself now records that a person
        // wrote it — `Chunk::brick_for_write` takes a `WriteOrigin` and `apply_op` passes one — so
        // the writer no longer has to be HANDED the boxes to know. Naming still buys something (a
        // box covers slots the person's write never reached, which is the swing through air two
        // cases below), but the commonest form of this loss no longer depends on anybody
        // remembering to name anything.
        Side read;
        read.make_types();
        build_clip_world(read.world, read.ledger, read.stone, read.wood, /*flavour=*/true);
        WorldCache in = read.handle();
        in.baseline = &read.world;
        REQUIRE(read_world_cache(bare_file.path, key, in, nullptr));
        CHECK(read.world.get(20, 20, 20) == read.stone);   // theirs, kept, with nothing named
        CHECK_FALSE(in.baseline_agreed);                   // and the reader still said so
    }
}

TEST_CASE("a carve through air the clip agrees about is still a carve") {
    // The other half of the same gap, and the one that comes back as SOLID rather than as the
    // wrong colour. Somebody swings a chisel through open air beside the wall. The clip puts
    // nothing there today, so there is no difference to write -- and if the clip grows a buttress
    // there tomorrow, the swing has to have been recorded or the buttress fills the space they
    // cleared.
    Scratch file("ws_test_edits_air_carve.world");
    const u64 key = world_cache_key("a clip", 32, 37);

    Side wrote;
    wrote.make_types();
    build_clip_world(wrote.clip_world, wrote.clip_ledger, wrote.stone, wrote.wood);
    build_clip_world(wrote.world, wrote.ledger, wrote.stone, wrote.wood);
    // 200..207 is open air in this clip -- the chisel meets nothing, changes nothing, and leaves
    // the saved world byte-identical to the baseline. The only record that the swing happened is
    // the named box.
    apply_op(wrote.world,
             Op::fill_box(10, 1, 200, 200, 200, 207, 207, 207, kAir, MatterReason::PlayerBreak),
             wrote.ledger);
    REQUIRE(wrote.world.content_hash() == wrote.clip_world.content_hash());

    WorldCache out = wrote.handle();
    out.baseline = &wrote.clip_world;
    out.edits_named = true;
    out.edited = {box_of(200, 200, 200, 207, 207, 207)};
    REQUIRE(write_world_cache(file.path, key, out));

    // A clip that now builds something exactly there.
    Side read;
    read.make_types();
    build_clip_world(read.world, read.ledger, read.stone, read.wood);
    apply_op(read.world,
             Op::fill_box(20, 1, 196, 196, 196, 211, 211, 211, read.stone,
                          MatterReason::Generation),
             read.ledger);
    WorldCache in = read.handle();
    in.baseline = &read.world;
    REQUIRE(read_world_cache(file.path, key, in, nullptr));

    CHECK(read.world.get(203, 203, 203) == kAir);       // the swing, kept
    CHECK(read.world.get(198, 198, 198) == read.stone); // and the clip's new matter beside it
}

// ============================================================================================
// The two data-loss cases that were still open, and the op log that closes the first of them
// ============================================================================================

TEST_CASE("the op log is what names the edits, and the boxes it gives keep them") {
    // DATA-LOSS CASE 1. The format could always carry named boxes; NOTHING PRODUCED ANY. Both of
    // the edits below leave the saved world byte-identical to the clip's, so the difference has
    // nothing at all to write, and the only record that either happened is the op log.
    Scratch file("ws_test_edits_from_oplog.world");
    const u64 key = world_cache_key("a clip", 32, 47);

    Side wrote;
    wrote.make_types();
    build_clip_world(wrote.clip_world, wrote.clip_ledger, wrote.stone, wrote.wood);
    build_clip_world(wrote.world, wrote.ledger, wrote.stone, wrote.wood);

    OpLog log;
    const auto edit = [&](const Op& op) {
        apply_op(wrote.world, op, wrote.ledger);
        log.append(op);
    };
    // A brick broken and put straight back with the clip's own stone.
    edit(Op::fill_box(10, 1, 16, 16, 16, 23, 23, 23, kAir, MatterReason::PlayerBreak));
    edit(Op::fill_box(11, 1, 16, 16, 16, 23, 23, 23, wrote.stone, MatterReason::PlayerPlace));
    // And a swing through open air beside the wall.
    edit(Op::fill_box(12, 1, 200, 200, 200, 207, 207, 207, kAir, MatterReason::PlayerBreak));
    REQUIRE(wrote.world.content_hash() == wrote.clip_world.content_hash());

    const std::vector<CachedEditBox> named = edit_boxes_from_ops(log.ops());
    CHECK(named.size() == 2);   // the refill names the carve's own box again and is dropped

    WorldCache out = wrote.handle();
    out.baseline = &wrote.clip_world;
    out.edits_named = true;
    out.edited = named;
    REQUIRE(write_world_cache(file.path, key, out));

    // The clip has moved under the file: that brick is wood now, and there is matter where the
    // swing was.
    Side read;
    read.make_types();
    build_clip_world(read.world, read.ledger, read.stone, read.wood, /*flavour=*/true);
    apply_op(read.world,
             Op::fill_box(20, 1, 196, 196, 196, 211, 211, 211, read.stone,
                          MatterReason::Generation),
             read.ledger);
    WorldCache in = read.handle();
    in.baseline = &read.world;
    REQUIRE(read_world_cache(file.path, key, in, nullptr));

    CHECK(read.world.get(20, 20, 20) == read.stone);      // the person's stone, kept
    CHECK(read.world.get(203, 203, 203) == kAir);         // the swing, kept
    CHECK(read.world.get(198, 198, 198) == read.stone);   // the clip's new matter beside it
}

TEST_CASE("an op log becomes boxes: normalised, deduplicated, and nothing merged") {
    OpLog log;
    log.append(Op::fill_box(1, 1, 10, 10, 10, 17, 17, 17, kAir, MatterReason::PlayerBreak));
    log.append(Op::fill_box(2, 1, 10, 10, 10, 17, 17, 17, 7, MatterReason::PlayerPlace));
    log.append(Op::fill_box(3, 1, 17, 17, 17, 10, 10, 10, kAir, MatterReason::PlayerBreak));
    log.append(Op::set_voxel(4, 1, -4, -4, -4, 7, MatterReason::PlayerPlace));
    log.append(Op::fill_box(5, 1, 12, 12, 12, 14, 14, 14, 7, MatterReason::PlayerPlace));

    const std::vector<CachedEditBox> boxes = edit_boxes_from_ops(log.ops());
    REQUIRE(boxes.size() == 2);
    CHECK(boxes[0].low[0] == 10);
    CHECK(boxes[0].high[0] == 17);
    // A single voxel is a box of one, and it has to be: a set_voxel is somebody's hands too.
    CHECK(boxes[1].low[0] == -4);
    CHECK(boxes[1].high[0] == -4);

    // And NOTHING is merged. Two carves at opposite ends of a building stay two boxes: a bounding
    // box over them would name every brick between and put the whole building back in the file,
    // which is this feature upside down.
    OpLog far_apart;
    far_apart.append(Op::fill_box(1, 1, 0, 0, 0, 7, 7, 7, kAir, MatterReason::PlayerBreak));
    far_apart.append(
        Op::fill_box(2, 1, 4000, 4000, 4000, 4007, 4007, 4007, kAir, MatterReason::PlayerBreak));
    const std::vector<CachedEditBox> two = edit_boxes_from_ops(far_apart.ops());
    REQUIRE(two.size() == 2);
    CHECK(two[1].low[0] == 4000);

    CHECK(edit_boxes_from_ops({}).empty());
}

TEST_CASE("a swing through air in a chunk neither world has anything in is still a carve") {
    // DATA-LOSS CASE 2 IN THE WRITER, and it is the named-box guarantee failing exactly where it
    // was written for. The walk that writes the difference went over the chunks the saved world
    // and the clip's world have BETWEEN them, so a named box in a chunk neither of them has
    // anything in reached no chunk at all and was written nowhere.
    Scratch file("ws_test_edits_empty_chunk_carve.world");
    const u64 key = world_cache_key("a clip", 32, 43);

    Side wrote;
    wrote.make_types();
    build_clip_world(wrote.clip_world, wrote.clip_ledger, wrote.stone, wrote.wood);
    build_clip_world(wrote.world, wrote.ledger, wrote.stone, wrote.wood);
    // Voxel 5000 is chunk 19 on every axis, and neither world has a chunk there. The chisel meets
    // nothing, changes nothing, and leaves no difference of any kind.
    apply_op(wrote.world,
             Op::fill_box(10, 1, 5000, 5000, 5000, 5007, 5007, 5007, kAir,
                          MatterReason::PlayerBreak),
             wrote.ledger);
    REQUIRE(wrote.world.content_hash() == wrote.clip_world.content_hash());
    REQUIRE_FALSE(wrote.world.has_chunk(ChunkCoord{19, 19, 19}));

    WorldCache out = wrote.handle();
    out.baseline = &wrote.clip_world;
    out.edits_named = true;
    out.edited = {box_of(5000, 5000, 5000, 5007, 5007, 5007)};
    REQUIRE(write_world_cache(file.path, key, out));

    // A clip that has since grown a tower exactly there.
    Side read;
    read.make_types();
    build_clip_world(read.world, read.ledger, read.stone, read.wood);
    apply_op(read.world,
             Op::fill_box(20, 1, 4990, 4990, 4990, 5020, 5020, 5020, read.stone,
                          MatterReason::Generation),
             read.ledger);
    WorldCache in = read.handle();
    in.baseline = &read.world;
    REQUIRE(read_world_cache(file.path, key, in, nullptr));

    CHECK(read.world.get(5003, 5003, 5003) == kAir);         // the swing, kept
    CHECK(read.world.get(4995, 4995, 4995) == read.stone);   // the tower beside it
    const WorldStats after = read.world.stats();
    CHECK(after.empty_bricks == 0);
    CHECK(after.empty_chunks == 0);
}

TEST_CASE("a baseline interned in another order comes back in its own materials") {
    // DATA-LOSS CASE 3, and it is the whole building rather than one brick of it. The file's type
    // table is adopted over whatever the reading run had; the baseline is a world the reading run
    // built out of ITS table, and every voxel in it is an id into that one. Adopt a table interned
    // in another order and nothing fails -- every voxel still has an id, every id still names a
    // record, and the building comes back wearing somebody else's materials.
    Scratch file("ws_test_edits_reordered.world");
    const u64 key = world_cache_key("a clip", 32, 41);

    Side wrote;
    wrote.make_types();
    build_clip_world(wrote.clip_world, wrote.clip_ledger, wrote.stone, wrote.wood);
    build_clip_world(wrote.world, wrote.ledger, wrote.stone, wrote.wood);
    apply_op(wrote.world,
             Op::fill_box(10, 1, -12, -12, -12, 11, 11, 11, kAir, MatterReason::PlayerBreak),
             wrote.ledger);
    apply_op(wrote.world,
             Op::fill_box(11, 1, -30, -30, -30, -25, -25, -25, wrote.marble,
                          MatterReason::PlayerPlace),
             wrote.ledger);

    WorldCache out = wrote.handle();
    out.baseline = &wrote.clip_world;
    REQUIRE(write_world_cache(file.path, key, out));

    // The reading run met its materials in the other order, so its stone is not the file's stone.
    Side read;
    read.make_types(/*reordered=*/true);
    REQUIRE(read.stone != wrote.stone);
    build_clip_world(read.world, read.ledger, read.stone, read.wood);
    WorldCache in = read.handle();
    in.baseline = &read.world;
    REQUIRE(read_world_cache(file.path, key, in, nullptr));

    // The numbering disagreed and the WORLD did not, which is the whole distinction: the baseline
    // is moved onto the file's table before a single chunk hash is compared.
    CHECK(in.baseline_agreed);
    CHECK(in.baseline_chunks_differing == 0);
    must_be_identical(read.world, wrote.world);

    // Said as a person would see it. The wall is stone-coloured and the block somebody placed is
    // marble-coloured, whatever number either of them wears.
    CHECK(read.types.visual_of(read.world.get(-13, -13, -13)).red == 120);
    CHECK(read.types.visual_of(read.world.get(-27, -27, -27)).red == 240);
    CHECK(read.world.get(0, 0, 0) == kAir);   // and the doorway is still cut
}

TEST_CASE("a session of chisel, undo and redo comes back exactly as it was left") {
    // The whole edit path rather than `apply_op` on its own: `EditHistory` is what a player's
    // hands actually reach, an undo is an ordinary op through the same log, and the boxes the file
    // is handed come out of that log and nowhere else.
    Scratch file("ws_test_edits_session.world");
    const u64 key = world_cache_key("a clip", 32, 61);

    Side wrote;
    wrote.make_types();
    build_clip_world(wrote.clip_world, wrote.clip_ledger, wrote.stone, wrote.wood);
    build_clip_world(wrote.world, wrote.ledger, wrote.stone, wrote.wood);

    EditHistory history;
    OpLog log;
    const u32 player = 1;
    history.apply(wrote.world, wrote.ledger, log,
                  Op::fill_box(1, player, -12, -12, -12, 11, 11, 11, kAir,
                               MatterReason::PlayerBreak));
    history.apply(wrote.world, wrote.ledger, log,
                  Op::fill_box(2, player, 60, 60, 60, 70, 70, 70, wrote.marble,
                               MatterReason::PlayerPlace));
    history.apply(wrote.world, wrote.ledger, log,
                  Op::fill_box(3, player, 16, 16, 16, 23, 23, 23, kAir,
                               MatterReason::PlayerBreak));
    std::vector<Op> stepped;
    REQUIRE(history.undo(wrote.world, wrote.ledger, log, 4, player, stepped));
    stepped.clear();
    REQUIRE(history.redo(wrote.world, wrote.ledger, log, 5, player, stepped));
    stepped.clear();
    REQUIRE(history.undo(wrote.world, wrote.ledger, log, 6, player, stepped));
    const u64 as_left = wrote.world.content_hash();

    WorldCache out = wrote.handle();
    out.baseline = &wrote.clip_world;
    out.edits_named = true;
    out.edited = edit_boxes_from_ops(log.ops());
    CHECK(out.edited.size() >= 3);
    REQUIRE(write_world_cache(file.path, key, out));

    Side read;
    read.make_types();
    build_clip_world(read.world, read.ledger, read.stone, read.wood);
    WorldCache in = read.handle();
    in.baseline = &read.world;
    REQUIRE(read_world_cache(file.path, key, in, nullptr));

    CHECK(read.world.content_hash() == as_left);
    must_be_identical(read.world, wrote.world);
    CHECK(read.world.get(0, 0, 0) == kAir);            // the doorway
    CHECK(read.world.get(65, 65, 65) == read.marble);  // the block that was placed
    CHECK(read.world.get(20, 20, 20) == read.stone);   // the stroke that was undone, put back
    const WorldStats after = read.world.stats();
    CHECK(after.empty_bricks == 0);
    CHECK(after.empty_chunks == 0);
}

// ============================================================================================
// The gate's second and fourth clauses, said as numbers
// ============================================================================================

TEST_CASE("a world nobody has touched puts no derived node in the file") {
    // GATE CLAUSE 2. Not "the file is small" -- a world that failed to build is small too -- but
    // that a world identical to what its clip builds writes NOUGHT bricks and NOUGHT clearings,
    // and every brick it holds is one the clip does not.
    Scratch bare("ws_test_edits_gate_bare.world");
    Scratch carved("ws_test_edits_gate_carved.world");
    const u64 key = world_cache_key("a clip", 32, 53);

    Side wrote;
    wrote.make_types();
    build_clip_world(wrote.clip_world, wrote.clip_ledger, wrote.stone, wrote.wood);
    build_clip_world(wrote.world, wrote.ledger, wrote.stone, wrote.wood);

    WorldCacheWritten nothing_written;
    WorldCache nothing = wrote.handle();
    nothing.baseline = &wrote.clip_world;
    REQUIRE(write_world_cache(bare.path, key, nothing, &nothing_written));
    CHECK(nothing_written.bricks_written == 0);
    CHECK(nothing_written.bricks_cleared == 0);
    CHECK(nothing_written.bricks_left_to_the_clip > 0);
    CHECK(nothing_written.chunks_fingerprinted > 0);

    // And one carve, so the count is a count and not a constant.
    apply_op(wrote.world,
             Op::fill_box(10, 1, -12, -12, -12, 11, 11, 11, kAir, MatterReason::PlayerBreak),
             wrote.ledger);
    WorldCacheWritten carved_written;
    WorldCache some = wrote.handle();
    some.baseline = &wrote.clip_world;
    REQUIRE(write_world_cache(carved.path, key, some, &carved_written));
    CHECK(carved_written.bricks_cleared > 0);
    CHECK(carved_written.bricks_left_to_the_clip <
          nothing_written.bricks_left_to_the_clip + carved_written.bricks_cleared + 1);
    CHECK(carved.bytes() > bare.bytes());
}

TEST_CASE("a whole world still loads with the difference reader in the run") {
    // GATE CLAUSE 4. A file written the way every file was written before R11f loads exactly as
    // it did, and a baseline left lying in the struct is ignored rather than laid under it -- the
    // mode is a byte in the header and is never inferred.
    Scratch file("ws_test_edits_old_file.world");
    const u64 key = world_cache_key("a clip", 32, 59);

    Side wrote;
    wrote.make_types();
    build_clip_world(wrote.world, wrote.ledger, wrote.stone, wrote.wood);
    apply_op(wrote.world,
             Op::fill_box(10, 1, -12, -12, -12, 11, 11, 11, kAir, MatterReason::PlayerBreak),
             wrote.ledger);
    WorldCache out = wrote.handle();   // no baseline: the whole world, as always
    REQUIRE(write_world_cache(file.path, key, out));

    Side read;
    read.make_types();
    World stray;   // something in the baseline slot that has nothing to do with this file
    MatterLedger stray_ledger;
    build_clip_world(stray, stray_ledger, read.stone, read.wood, /*flavour=*/true);
    WorldCache in = read.handle();
    in.baseline = &stray;
    REQUIRE(read_world_cache(file.path, key, in, nullptr));

    CHECK(in.mode == WorldCacheMode::Whole);
    must_be_identical(read.world, wrote.world);
    CHECK(read.world.get(0, 0, 0) == kAir);
    CHECK(read.world.get(20, 20, 20) == read.stone);   // the stray world's wood did not get in
}

// ============================================================================================
// The facility, both ways. Skipped by default -- it samples a real clip and writes real files.
// Run it with:  ws_tests.exe --no-skip --test-case="the facility, both ways"
// ============================================================================================

TEST_CASE("the facility, both ways" * doctest::skip()) {
    // Where the clip is, from wherever the test binary happens to be run. `WS_GATE_CLIP` names
    // another one -- `clips/sampler.clip`, or a single estate building -- so the four gate clauses
    // can be taken on more than one real world without a second test.
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

    // The world the clip builds -- the baseline -- and a copy of it for somebody to carve.
    World clip_world;
    MatterLedger clip_ledger;
    paste_clip(clip_world, clip_ledger, built.clip, built.origin_voxel[0], built.origin_voxel[1],
               built.origin_voxel[2], PasteMode::Replace, MatterReason::Generation, 1, &jobs,
               types.type_count());
    World edited;
    MatterLedger edited_ledger;
    paste_clip(edited, edited_ledger, built.clip, built.origin_voxel[0], built.origin_voxel[1],
               built.origin_voxel[2], PasteMode::Replace, MatterReason::Generation, 1, &jobs,
               types.type_count());

    // A chisel through the middle of it: two metres cubed, which is about what one swing takes.
    const i64 reach = per_metre;   // one metre either side of the origin
    apply_op(edited,
             Op::fill_box(1, 1, -reach, -reach, -reach, reach, reach, reach, kAir,
                          MatterReason::PlayerBreak),
             edited_ledger);

    const WorldStats stats = edited.stats();
    std::printf("\nfacility      %s at %d voxels a metre, sampled in %.0f ms\n", clip_path.c_str(),
                per_metre, sample_ms);
    std::printf("world         %llu chunks, %llu solid voxels\n",
                static_cast<unsigned long long>(stats.chunks),
                static_cast<unsigned long long>(stats.solid_voxels));

    Scratch whole("ws_facility_whole.world");
    Scratch diff("ws_facility_edits.world");
    const u64 key = world_cache_key("facility", per_metre, 1);

    WorldCache out;
    out.tags = &tags;
    out.properties = &properties;
    out.types = &types;
    out.world = &edited;
    out.ledger = &edited_ledger;

    WorldCacheWritten whole_written;
    const u64 whole_began = now_ns();
    REQUIRE(write_world_cache(whole.path, key, out, &whole_written));
    const f64 whole_write_ms = ns_to_ms(now_ns() - whole_began);

    out.baseline = &clip_world;
    out.edits_named = true;
    out.edited = {box_of(-reach, -reach, -reach, reach, reach, reach)};
    WorldCacheWritten diff_written;
    const u64 diff_began = now_ns();
    REQUIRE(write_world_cache(diff.path, key, out, &diff_written));
    const f64 diff_write_ms = ns_to_ms(now_ns() - diff_began);

    // A third file: the same world, untouched, written as a difference. That is the FIXED cost of
    // an edit-only file -- the type table, the region list, the lamps and the baseline fingerprint
    // -- with no voxels in it at all, and measuring it is what separates "the difference is small"
    // from "the header is most of the file at this size".
    Scratch bare("ws_facility_bare.world");
    World untouched = clip_world;
    WorldCacheWritten bare_written;
    {
        WorldCache nothing;
        nothing.tags = &tags;
        nothing.properties = &properties;
        nothing.types = &types;
        nothing.world = &untouched;
        nothing.ledger = &clip_ledger;
        nothing.baseline = &clip_world;
        REQUIRE(write_world_cache(bare.path, key, nothing, &bare_written));
    }
    // GATE CLAUSE 2, as a number rather than as a size: a world nobody has touched puts NO derived
    // node in the file. Nought bricks, nought clearings, and every brick of the building left to
    // the clip.
    CHECK(bare_written.bricks_written == 0);
    CHECK(bare_written.bricks_cleared == 0);
    std::printf("gate 2        %u bricks written, %u cleared, %u left to the clip (untouched)\n",
                bare_written.bricks_written, bare_written.bricks_cleared,
                bare_written.bricks_left_to_the_clip);
    std::printf("              carved: %u written, %u cleared, %u left to the clip\n",
                diff_written.bricks_written, diff_written.bricks_cleared,
                diff_written.bricks_left_to_the_clip);
    std::printf("              whole:  %u bricks, every one of them derived\n",
                whole_written.bricks_written);

    std::printf("whole world   %llu bytes (%.2f MB), written in %.0f ms\n",
                static_cast<unsigned long long>(whole.bytes()),
                static_cast<f64>(whole.bytes()) / (1024.0 * 1024.0), whole_write_ms);
    std::printf("clip + edits  %llu bytes (%.2f MB), written in %.0f ms  -- %.1fx smaller\n",
                static_cast<unsigned long long>(diff.bytes()),
                static_cast<f64>(diff.bytes()) / (1024.0 * 1024.0), diff_write_ms,
                static_cast<f64>(whole.bytes()) / static_cast<f64>(diff.bytes()));
    std::printf("  fixed cost  %llu bytes: the table, the leaves and the fingerprint, no voxels\n",
                static_cast<unsigned long long>(bare.bytes()));
    std::printf("  so voxels   %llu bytes whole -> %llu bytes as a difference (%.0fx)\n",
                static_cast<unsigned long long>(whole.bytes() - bare.bytes()),
                static_cast<unsigned long long>(diff.bytes() - bare.bytes()),
                static_cast<f64>(whole.bytes() - bare.bytes()) /
                    static_cast<f64>(diff.bytes() - bare.bytes() + 1));

    // Reading the whole world back, which is what a shipped world costs today.
    World back_whole;
    MatterLedger back_whole_ledger;
    TagRegistry whole_tags;
    PropertyRegistry whole_properties;
    VoxelTypeTable whole_types;
    WorldCache in_whole;
    in_whole.tags = &whole_tags;
    in_whole.properties = &whole_properties;
    in_whole.types = &whole_types;
    in_whole.world = &back_whole;
    in_whole.ledger = &back_whole_ledger;
    const u64 read_whole_began = now_ns();
    REQUIRE(read_world_cache(whole.path, key, in_whole, &jobs));
    const f64 read_whole_ms = ns_to_ms(now_ns() - read_whole_began);

    // And the difference, laid over the world the clip builds. The baseline here is read from the
    // whole-world file rather than re-sampled, because that is how the two compose in the game: a
    // world is shipped once beside its clip, and a person's save is what they changed about it.
    World back_diff;
    MatterLedger back_diff_ledger;
    TagRegistry diff_tags;
    PropertyRegistry diff_properties;
    VoxelTypeTable diff_types;
    WorldCache base_in;
    base_in.tags = &diff_tags;
    base_in.properties = &diff_properties;
    base_in.types = &diff_types;
    base_in.world = &back_diff;
    base_in.ledger = &back_diff_ledger;
    // The shipped world, unedited, so the baseline is what the clip builds.
    Scratch shipped("ws_facility_shipped.world");
    {
        WorldCache ship;
        ship.tags = &tags;
        ship.properties = &properties;
        ship.types = &types;
        ship.world = &clip_world;
        ship.ledger = &clip_ledger;
        REQUIRE(write_world_cache(shipped.path, key, ship));
    }
    const u64 base_began = now_ns();
    REQUIRE(read_world_cache(shipped.path, key, base_in, &jobs));
    const f64 base_ms = ns_to_ms(now_ns() - base_began);

    WorldCache in_diff = base_in;
    in_diff.baseline = &back_diff;
    const u64 read_diff_began = now_ns();
    REQUIRE(read_world_cache(diff.path, key, in_diff, &jobs));
    const f64 read_diff_ms = ns_to_ms(now_ns() - read_diff_began);

    std::printf("read          whole %.0f ms;  shipped world %.0f ms + edits %.0f ms = %.0f ms\n",
                read_whole_ms, base_ms, read_diff_ms, base_ms + read_diff_ms);
    std::printf("sampling it instead would be %.0f ms\n\n", sample_ms);

    CHECK(in_diff.baseline_agreed);
    CHECK(back_whole.content_hash() == edited.content_hash());
    CHECK(back_diff.content_hash() == edited.content_hash());
    CHECK(diff.bytes() < whole.bytes());
}

// ---- R12d: the carve, saved, reloaded, and RE-SAMPLED -----------------------------------------
//
// This is the gate the stage is judged on and it is four steps, none of which is worth anything
// without the ones after it. A player carves a doorway. The world is written to a file and read
// back into a fresh one. The ladder then sharpens that region again — which pastes the clip's own
// answer over the box as a REPLACE — and the doorway has to still be there.
//
// The step that is easy to miss is the last. In the running game the re-fill is repaired a
// different way: `pump_refinement` replays the ENTIRE op log immediately after every paste, so the
// field wins for a moment and the cut is made again. That works while the ops are in memory and
// stops working the moment a world is reloaded, because a resumed run has the carvings and not the
// ops that made them. A brick that knows whose it is needs neither.
namespace {

// The box a person cuts, and it is deliberately a whole brick plus a bite out of its neighbour: one
// slot empties completely (which unlinks the brick, so the flag on it has nowhere to live) and one
// only partly (which keeps its brick, so the flag does). Those are two different mechanisms and a
// carve that only exercised one would pass with the other broken.
constexpr i64 kCarveLow[3] = {0, 0, 0};
constexpr i64 kCarveHigh[3] = {11, 7, 7};

u64 air_in_carve(const World& world) {
    u64 air = 0;
    for (i64 z = kCarveLow[2]; z <= kCarveHigh[2]; ++z) {
        for (i64 y = kCarveLow[1]; y <= kCarveHigh[1]; ++y) {
            for (i64 x = kCarveLow[0]; x <= kCarveHigh[0]; ++x) {
                if (world.get(x, y, z) == kAir) ++air;
            }
        }
    }
    return air;
}

// Every brick in the world that says a person's work is in it, and every slot that says a person
// emptied it. The count is the evidence: "the carve survived" and "everything got marked, so of
// course it did" look identical from the carve alone.
void claims_in(const World& world, u64& bricks, u64& erased, u64& live_bricks) {
    bricks = 0;
    erased = 0;
    live_bricks = 0;
    world.for_each_chunk([&](const ChunkCoord&, const Chunk& chunk) {
        bricks += chunk.edited_bricks();
        erased += chunk.erased_bricks();
        live_bricks += chunk.brick_count();
    });
}

}  // namespace

TEST_CASE("R12d: a carve survives a save, a reload and a re-sample") {
    Scratch file("ws_test_r12d_carve.world");
    const u64 key = world_cache_key("a clip", 32, 71);

    Side wrote;
    wrote.make_types();
    build_clip_world(wrote.clip_world, wrote.clip_ledger, wrote.stone, wrote.wood);
    build_clip_world(wrote.world, wrote.ledger, wrote.stone, wrote.wood);

    // Nothing is claimed by the clip building itself. This is the boundary, and it is checked
    // before the carve rather than after, because a world already fully claimed would pass every
    // assertion below for the wrong reason.
    u64 claimed = 0;
    u64 erased = 0;
    u64 live = 0;
    claims_in(wrote.world, claimed, erased, live);
    REQUIRE(live > 0);
    REQUIRE(claimed == 0);
    REQUIRE(erased == 0);

    apply_op(wrote.world,
             Op::fill_box(9, 1, kCarveLow[0], kCarveLow[1], kCarveLow[2], kCarveHigh[0],
                          kCarveHigh[1], kCarveHigh[2], kAir, MatterReason::PlayerBreak),
             wrote.ledger);

    const u64 carved_air = air_in_carve(wrote.world);
    const u64 carved_shape = wrote.world.shape_hash();
    REQUIRE(carved_air == 12u * 8u * 8u);

    claims_in(wrote.world, claimed, erased, live);
    // One brick per slot the box reached and not one more: 8x8x8 at the origin went entirely and
    // is an erased slot; the half-brick beside it kept its brick and carries the flag.
    CHECK(claimed == 1);
    CHECK(erased == 1);
    CHECK(wrote.world.validate());

    // Written WITHOUT naming a single box, deliberately. R11f's answer to this was the op log, and
    // the op log is what a reloaded world does not have; the point of R12d is that the world can
    // answer for itself.
    WorldCache out = wrote.handle();
    out.baseline = &wrote.clip_world;
    REQUIRE_FALSE(out.edits_named);
    REQUIRE(write_world_cache(file.path, key, out));

    Side read;
    read.make_types();
    build_clip_world(read.clip_world, read.clip_ledger, read.stone, read.wood);
    build_clip_world(read.world, read.ledger, read.stone, read.wood);
    WorldCache in = read.handle();
    in.baseline = &read.world;
    REQUIRE(read_world_cache(file.path, key, in, nullptr));
    CHECK(in.baseline_agreed);

    // SHAPE, not content. A reload re-interns the type table and `content` moves legitimately with
    // it; `shape` is which cells hold matter and survives that (D729).
    CHECK(read.world.shape_hash() == carved_shape);
    CHECK(air_in_carve(read.world) == carved_air);
    CHECK(read.world.validate());

    // And the bookkeeping came back with it, which is the thing a version 7 file could not say.
    u64 back_claimed = 0;
    u64 back_erased = 0;
    u64 back_live = 0;
    claims_in(read.world, back_claimed, back_erased, back_live);
    CHECK(back_claimed == claimed);
    CHECK(back_erased == erased);

    // ---- and now the ladder sharpens that region again ------------------------------------
    //
    // Exactly what `pump_refinement` does: the clip's own answer for the box, stamped as a REPLACE.
    // Taken out of the clip world with `capture_clip`, so it is the field's answer and not a
    // hand-written one.
    const Clip again = capture_clip(read.clip_world, kCarveLow[0] - 8, kCarveLow[1] - 8,
                                    kCarveLow[2] - 8, kCarveHigh[0] + 8, kCarveHigh[1] + 8,
                                    kCarveHigh[2] + 8);
    REQUIRE(again.solid_count() > 0);
    paste_clip(read.world, read.ledger, again, kCarveLow[0] - 8, kCarveLow[1] - 8,
               kCarveLow[2] - 8, PasteMode::Replace, MatterReason::PlayerPlace, 1);

    CHECK(air_in_carve(read.world) == carved_air);      // the doorway is still a doorway
    CHECK(read.world.shape_hash() == carved_shape);     // and nothing else moved either
    CHECK(read.world.validate());

    // The re-sample must not have claimed anything for itself. It wrote real matter into bricks
    // all around the carve and every one of those is still the clip's to rebuild.
    u64 after_claimed = 0;
    u64 after_erased = 0;
    u64 after_live = 0;
    claims_in(read.world, after_claimed, after_erased, after_live);
    CHECK(after_claimed == claimed);
    CHECK(after_erased == erased);
}

TEST_CASE("R12d: a whole-world file carries who wrote each brick") {
    // The other form of the file. An edit-only file says what differs from a clip; a whole-world
    // one says everything, and until version 8 "everything" did not include this.
    Scratch file("ws_test_r12d_whole.world");
    const u64 key = world_cache_key("a clip", 32, 73);

    Side wrote;
    wrote.make_types();
    build_clip_world(wrote.world, wrote.ledger, wrote.stone, wrote.wood);
    apply_op(wrote.world,
             Op::fill_box(9, 1, kCarveLow[0], kCarveLow[1], kCarveLow[2], kCarveHigh[0],
                          kCarveHigh[1], kCarveHigh[2], kAir, MatterReason::PlayerBreak),
             wrote.ledger);
    u64 claimed = 0;
    u64 erased = 0;
    u64 live = 0;
    claims_in(wrote.world, claimed, erased, live);
    REQUIRE(claimed == 1);
    REQUIRE(erased == 1);

    WorldCache out = wrote.handle();   // no baseline: every voxel, as this file has always been
    REQUIRE(write_world_cache(file.path, key, out));

    Side read;
    read.make_types();
    WorldCache in = read.handle();
    REQUIRE(read_world_cache(file.path, key, in, nullptr));
    CHECK(in.mode == WorldCacheMode::Whole);
    CHECK(read.world.shape_hash() == wrote.world.shape_hash());

    u64 back_claimed = 0;
    u64 back_erased = 0;
    u64 back_live = 0;
    claims_in(read.world, back_claimed, back_erased, back_live);
    CHECK(back_claimed == claimed);
    CHECK(back_erased == erased);
    CHECK(back_live == live);
    CHECK(read.world.validate());
}
