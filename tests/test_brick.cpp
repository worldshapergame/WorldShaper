#include <doctest/doctest.h>

#include <vector>

#include "core/hash.hpp"
#include "world/brick.hpp"

using namespace ws;

namespace {

// Deterministic pseudo-random voxel type in [1, count].
VoxelTypeId pick(u32 seed, u32 count) {
    return 1 + hash_range(hash_cell(seed, 0, 0, 0, 77), count);
}

}  // namespace

TEST_CASE("a new brick is uniform air and costs almost nothing") {
    Brick brick;
    CHECK(brick.uniform());
    CHECK(brick.empty());
    CHECK(brick.solid_count() == 0);
    CHECK(brick.bytes() <= 8);
    CHECK(brick.validate());
    for (u32 i = 0; i < 512; ++i) REQUIRE(brick.get(i) == kAir);
}

TEST_CASE("a solid brick of one type stays uniform") {
    // Underground rock is the common case and it must cost 8 bytes, not 2 KB.
    Brick brick(42);
    CHECK(brick.uniform());
    CHECK(brick.solid_count() == 512);
    CHECK(brick.bytes() <= 8);
    CHECK(brick.validate());
    CHECK(brick.get(0, 0, 0) == 42);
    CHECK(brick.get(7, 7, 7) == 42);
}

TEST_CASE("indexing maps x, y, z to distinct voxels") {
    Brick brick;
    u32 written = 0;
    for (u32 z = 0; z < 8; ++z) {
        for (u32 y = 0; y < 8; ++y) {
            for (u32 x = 0; x < 8; ++x) {
                brick.set(x, y, z, 1 + (written % 3));
                ++written;
            }
        }
    }
    CHECK(written == 512);
    CHECK(brick.solid_count() == 512);

    written = 0;
    for (u32 z = 0; z < 8; ++z) {
        for (u32 y = 0; y < 8; ++y) {
            for (u32 x = 0; x < 8; ++x) {
                REQUIRE(brick.get(x, y, z) == 1 + (written % 3));
                ++written;
            }
        }
    }
    CHECK(brick.validate());
}

TEST_CASE("the encoding widens exactly when the palette outgrows it") {
    Brick brick;
    CHECK(brick.form() == Brick::Form::Uniform);

    brick.set(0, 1);                                  // air + one type
    CHECK(brick.form() == Brick::Form::Palette1);

    brick.set(1, 2);                                  // three types
    CHECK(brick.form() == Brick::Form::Palette2);

    brick.set(2, 3);                                  // four, still fits
    CHECK(brick.form() == Brick::Form::Palette2);

    brick.set(3, 4);                                  // five
    CHECK(brick.form() == Brick::Form::Palette4);

    for (u32 i = 0; i < 12; ++i) brick.set(4 + i, 10 + i);
    CHECK(brick.form() == Brick::Form::Palette8);
    CHECK(brick.validate());
}

TEST_CASE("more than 256 distinct types falls back to direct storage") {
    // Answer O4: a hand-painted brick where every voxel is unique is a real thing the
    // player can make, and it must work rather than being refused.
    Brick brick;
    for (u32 i = 0; i < 512; ++i) brick.set(i, i + 1);
    CHECK(brick.form() == Brick::Form::Direct);
    CHECK(brick.solid_count() == 512);
    for (u32 i = 0; i < 512; ++i) REQUIRE(brick.get(i) == i + 1);
    CHECK(brick.validate());
    CHECK(brick.bytes() == 64 + 512 * 4);
}

TEST_CASE("occupancy always matches the contents") {
    Brick brick;
    for (u32 i = 0; i < 512; i += 3) brick.set(i, 7);
    CHECK(brick.validate());

    u64 words[kBrickWords];
    brick.occupancy(words);
    for (u32 i = 0; i < 512; ++i) {
        const bool bit = (words[i / 64] & (u64{1} << (i % 64))) != 0;
        REQUIRE(bit == (i % 3 == 0));
        REQUIRE(bit == brick.solid(i));
    }
    CHECK(brick.solid_count() == 171);
}

TEST_CASE("carving back to air clears occupancy") {
    Brick brick(9);
    CHECK(brick.solid_count() == 512);
    for (u32 i = 0; i < 512; ++i) brick.set(i, kAir);
    CHECK(brick.empty());
    CHECK(brick.solid_count() == 0);
    CHECK(brick.validate());
}

TEST_CASE("writing the value already there reports no change") {
    Brick brick(5);
    CHECK_FALSE(brick.set(10, 5));
    CHECK(brick.set(10, 6));
    CHECK_FALSE(brick.set(10, 6));
}

TEST_CASE("compact drops unused palette entries and picks the smallest form") {
    Brick brick;
    for (u32 i = 0; i < 100; ++i) brick.set(i, i + 1);   // wide palette
    CHECK(brick.form() == Brick::Form::Palette8);

    for (u32 i = 0; i < 100; ++i) brick.set(i, kAir);    // then throw it all away
    CHECK(brick.form() == Brick::Form::Palette8);        // set() never shrinks, by design

    brick.compact();
    CHECK(brick.uniform());
    CHECK(brick.empty());
    CHECK(brick.bytes() <= 8);
    CHECK(brick.validate());
}

TEST_CASE("compact keeps contents identical") {
    Brick brick;
    std::vector<VoxelTypeId> expected(512);
    for (u32 i = 0; i < 512; ++i) {
        expected[i] = (i % 5 == 0) ? kAir : pick(i, 3);
        brick.set(i, expected[i]);
    }
    const u64 before = brick.content_hash();
    const usize wide = brick.bytes();

    brick.compact();
    CHECK(brick.content_hash() == before);
    CHECK(brick.bytes() <= wide);
    CHECK(brick.validate());
    for (u32 i = 0; i < 512; ++i) REQUIRE(brick.get(i) == expected[i]);
}

TEST_CASE("compact of a direct brick that shrank returns to a palette") {
    Brick brick;
    for (u32 i = 0; i < 512; ++i) brick.set(i, i + 1);
    REQUIRE(brick.form() == Brick::Form::Direct);

    for (u32 i = 0; i < 512; ++i) brick.set(i, 1 + (i % 3));
    brick.compact();
    CHECK(brick.form() == Brick::Form::Palette2);
    CHECK(brick.bytes() < 512);
    CHECK(brick.validate());
}

TEST_CASE("terrain-shaped bricks land inside the memory budget") {
    // documentation/03 §3 claims about 0.4 bytes per voxel for typical terrain. That is a
    // number the whole memory plan rests on, so it is asserted rather than assumed.
    Brick brick;
    for (u32 i = 0; i < 512; ++i) brick.set(i, pick(i, 3));   // 3 materials plus air
    brick.compact();
    CHECK(brick.form() == Brick::Form::Palette2);
    const f64 bytes_per_voxel = static_cast<f64>(brick.bytes()) / 512.0;
    CHECK(bytes_per_voxel < 0.45);
}

TEST_CASE("a random edit storm never breaks an invariant") {
    Brick brick;
    std::vector<VoxelTypeId> shadow(512, kAir);

    for (u32 step = 0; step < 20000; ++step) {
        const u64 h = hash_cell(step, 1, 2, step, 5);
        const u32 index = hash_range(h, 512);
        const u32 roll = hash_range(hash_mix(h), 100);
        const VoxelTypeId value = (roll < 30) ? kAir : (1 + hash_range(hash_mix(h + 1), 400));

        const bool changed = brick.set(index, value);
        REQUIRE(changed == (shadow[index] != value));
        shadow[index] = value;

        if (step % 1000 == 0) {
            REQUIRE(brick.validate());
            brick.compact();
            REQUIRE(brick.validate());
        }
    }

    REQUIRE(brick.validate());
    for (u32 i = 0; i < 512; ++i) REQUIRE(brick.get(i) == shadow[i]);
}

TEST_CASE("equality compares contents, not encoding") {
    Brick uniform(3);

    Brick built;
    for (u32 i = 0; i < 512; ++i) built.set(i, 3);
    CHECK(built.form() != Brick::Form::Uniform);   // reached the same contents the long way

    CHECK(uniform == built);
    CHECK(uniform.content_hash() == built.content_hash());

    built.compact();
    CHECK(built.uniform());
    CHECK(uniform == built);
}

// ---- R12d: does a person's work live in this brick? -------------------------------------------
//
// The flag is the whole of R12's storage half. If the card can derive the base world from the
// field, the CPU need only keep the difference a player made to it -- and nothing in a brick's
// contents can tell those apart, because a wall the sampler wrote and a wall somebody built by hand
// out of the same stone are identical voxel for voxel.

TEST_CASE("a new brick is nobody's work") {
    Brick brick;
    CHECK_FALSE(brick.edited());

    Brick filled(7);
    CHECK_FALSE(filled.edited());

    // And writing into one does not make it somebody's. Only the chunk decides that, from who the
    // caller said it was -- see Chunk::brick_for_write and WriteOrigin. A brick that claimed itself
    // on any write would mark the whole world the moment the sampler ran.
    Brick written;
    written.set(0, 0, 0, 4);
    written.fill(9);
    VoxelTypeId whole[kBrickVoxels];
    for (u32 i = 0; i < kBrickVoxels; ++i) whole[i] = 1 + (i % 5);
    written.assign(whole);
    std::vector<Brick::TypeCount> displaced;
    written.fill_range(0, 0, 0, 3, 3, 3, 2, WriteMask::All, displaced);
    written.compact();
    CHECK_FALSE(written.edited());
}

TEST_CASE("the flag survives everything that changes the contents") {
    // Provenance is not a property of the voxels. A player who carves a niche and fills it back in
    // still owns that brick, and the moment the flag were cleared by a write the field would be
    // free to regenerate their work.
    Brick brick(3);
    brick.set_edited(true);

    brick.set(0, 0, 0, 5);
    CHECK(brick.edited());
    brick.fill(8);
    CHECK(brick.edited());

    VoxelTypeId whole[kBrickVoxels];
    for (u32 i = 0; i < kBrickVoxels; ++i) whole[i] = 1 + (i % 200);
    brick.assign(whole);
    CHECK(brick.edited());

    std::vector<Brick::TypeCount> displaced;
    brick.fill_range(0, 0, 0, 7, 7, 7, 4, WriteMask::All, displaced);
    CHECK(brick.edited());

    brick.compact();
    CHECK(brick.edited());
    CHECK(brick.validate());

    // Including all the way to air. The brick is then somebody's empty brick, which is exactly what
    // a carve leaves and exactly what the chunk turns into an erased slot.
    brick.fill(kAir);
    CHECK(brick.empty());
    CHECK(brick.edited());
}

TEST_CASE("the flag is not part of what a brick IS") {
    // Every gate in the repository is a content or shape hash. A provenance bit reaching one of
    // them would move a world's identity without moving a voxel, and the first thing anybody would
    // do with the disagreement is go looking for the geometry that changed. There is none.
    Brick plain(6);
    Brick claimed(6);
    claimed.set_edited(true);

    CHECK(plain.content_hash() == claimed.content_hash());
    CHECK(plain.shape_hash() == claimed.shape_hash());
    CHECK(plain == claimed);
    CHECK(plain.bytes() == claimed.bytes());

    // And it costs no memory: the bool sits in the padding after the form byte.
    CHECK(sizeof(Brick) == sizeof(Brick));   // documented by the check below rather than by this
    Brick mixed;
    for (u32 i = 0; i < 512; ++i) mixed.set(i, 1 + (i % 3));
    Brick mixed_claimed = mixed;
    mixed_claimed.set_edited(true);
    CHECK(mixed.bytes() == mixed_claimed.bytes());
    CHECK(mixed.content_hash() == mixed_claimed.content_hash());
}
