#include <doctest/doctest.h>

#include <vector>

#include "core/hash.hpp"
#include "world/op.hpp"
#include "world/serialize.hpp"
#include "world/voxel_type.hpp"
#include "world/world.hpp"

using namespace ws;

namespace {

struct Fixture {
    TagRegistry tags;
    PropertyRegistry properties;
    VoxelTypeTable types;
    World world;

    WorldSave save() { return WorldSave{&tags, &properties, &types, &world}; }
};

VisualRecord colour(u8 r, u8 g, u8 b) {
    VisualRecord v{};
    v.red = r;
    v.green = g;
    v.blue = b;
    v.opacity = 255;
    return v;
}

// Builds a world with a few materials, some carving, and coordinates on both sides of the
// origin — the shapes most likely to expose an encoding bug.
void populate(Fixture& fixture) {
    BehaviourRecord stone{};
    stone.material = 1;
    stone.tags.add(fixture.tags.find("stone"));
    stone.properties.set(props::kDensity, PropertyValue::from_uint(2600));

    BehaviourRecord wood{};
    wood.material = 2;
    wood.tags.add(fixture.tags.find("wood"));
    wood.tags.add(fixture.tags.find("flammable"));
    wood.tags.add(400);   // deliberately past the fast-bit range

    const VoxelTypeId grey = fixture.types.intern(colour(120, 120, 120), stone);
    const VoxelTypeId brown = fixture.types.intern(colour(90, 60, 30), wood);
    const VoxelTypeId pale = fixture.types.intern(colour(200, 190, 170), stone);

    MatterLedger ledger;
    apply_op(fixture.world,
             Op::fill_box(1, 1, -20, -20, -20, 20, 20, 20, grey, MatterReason::PlayerPlace),
             ledger);
    apply_op(fixture.world,
             Op::fill_box(2, 1, -5, -5, -5, 5, 5, 5, kAir, MatterReason::PlayerBreak),
             ledger);
    apply_op(fixture.world,
             Op::fill_box(3, 1, 250, 250, 250, 270, 270, 270, brown,
                          MatterReason::PlayerPlace),
             ledger);
    apply_op(fixture.world,
             Op::set_voxel(4, 1, -100000, 40000, 7, pale, MatterReason::PlayerPlace),
             ledger);
}

}  // namespace

TEST_CASE("byte writer and reader round-trip every scalar") {
    ByteWriter out;
    out.u8v(0xAB);
    out.u16v(0xBEEF);
    out.u32v(0xDEADBEEF);
    out.u64v(0x0123456789ABCDEFull);
    out.i64v(-1234567890123LL);
    out.text("hello world");

    ByteReader in(out.data());
    CHECK(in.u8v() == 0xAB);
    CHECK(in.u16v() == 0xBEEF);
    CHECK(in.u32v() == 0xDEADBEEF);
    CHECK(in.u64v() == 0x0123456789ABCDEFull);
    CHECK(in.i64v() == -1234567890123LL);
    CHECK(in.text() == "hello world");
    CHECK(in.ok());
    CHECK(in.exhausted());
}

TEST_CASE("a truncated stream is reported, never read past") {
    ByteWriter out;
    out.u32v(7);
    ByteReader in(out.data());
    CHECK(in.u32v() == 7);
    CHECK(in.ok());
    in.u64v();            // past the end
    CHECK_FALSE(in.ok());
}

TEST_CASE("a brick round-trips exactly") {
    Brick brick;
    for (u32 i = 0; i < 512; ++i) {
        brick.set(i, (i % 7 == 0) ? kAir : (1 + (i % 5)));
    }

    ByteWriter out;
    write_brick(out, brick);

    Brick restored;
    ByteReader in(out.data());
    REQUIRE(read_brick(in, restored));
    CHECK(in.exhausted());
    CHECK(restored == brick);
    CHECK(restored.content_hash() == brick.content_hash());
    CHECK(restored.validate());
}

TEST_CASE("a uniform brick costs almost nothing on disk") {
    Brick brick(9);
    ByteWriter out;
    write_brick(out, brick);
    // One byte of width, four of palette length, four of the single entry.
    CHECK(out.size() == 9);

    Brick restored;
    ByteReader in(out.data());
    REQUIRE(read_brick(in, restored));
    CHECK(restored.uniform());
    CHECK(restored.get(0) == 9);
}

TEST_CASE("a fully unique brick round-trips too") {
    Brick brick;
    for (u32 i = 0; i < 512; ++i) brick.set(i, i + 1);
    REQUIRE(brick.form() == Brick::Form::Direct);

    ByteWriter out;
    write_brick(out, brick);
    Brick restored;
    ByteReader in(out.data());
    REQUIRE(read_brick(in, restored));
    CHECK(restored == brick);
    CHECK(restored.validate());
}

TEST_CASE("the encoding a brick happens to be in does not change its bytes") {
    // The whole point of a canonical form: two bricks with the same contents must produce
    // the same file, whether one was built directly and the other carved into shape.
    Brick direct(4);

    Brick carved;
    for (u32 i = 0; i < 512; ++i) carved.set(i, 1 + (i % 200));   // wide palette
    for (u32 i = 0; i < 512; ++i) carved.set(i, 4);               // then all the same
    REQUIRE(carved.form() != Brick::Form::Uniform);

    ByteWriter a;
    ByteWriter b;
    write_brick(a, direct);
    write_brick(b, carved);
    CHECK(a.data() == b.data());
}

TEST_CASE("a world round-trips and keeps its hash") {
    Fixture original;
    populate(original);
    const u64 hash_before = original.world.content_hash();

    ByteWriter out;
    write_world(out, original.world);

    World restored;
    ByteReader in(out.data());
    REQUIRE(read_world(in, restored));
    CHECK(in.exhausted());
    CHECK(restored.content_hash() == hash_before);
    CHECK(restored.validate());
    CHECK(restored.stats().solid_voxels == original.world.stats().solid_voxels);
}

TEST_CASE("save, load, save produces identical bytes") {
    // documentation/03 §9 invariant 6. Without this, every load-and-save cycle drifts and
    // nobody notices until a world will not open.
    Fixture original;
    populate(original);

    ByteWriter first;
    WorldSave save = original.save();
    write_save(first, save);

    Fixture loaded;
    WorldSave target = loaded.save();
    ByteReader in(first.data());
    REQUIRE(read_save(in, target));
    CHECK(in.exhausted());

    ByteWriter second;
    WorldSave resaved = loaded.save();
    write_save(second, resaved);

    CHECK(first.data().size() == second.data().size());
    CHECK(first.data() == second.data());
}

TEST_CASE("everything survives the round trip, not just the voxels") {
    Fixture original;
    populate(original);

    ByteWriter out;
    WorldSave save = original.save();
    write_save(out, save);

    Fixture loaded;
    WorldSave target = loaded.save();
    ByteReader in(out.data());
    REQUIRE(read_save(in, target));

    CHECK(loaded.world.content_hash() == original.world.content_hash());
    CHECK(loaded.types.type_count() == original.types.type_count());
    CHECK(loaded.types.visual_count() == original.types.visual_count());
    CHECK(loaded.types.behaviour_count() == original.types.behaviour_count());
    CHECK(loaded.tags.count() == original.tags.count());
    CHECK(loaded.properties.count() == original.properties.count());

    // Spot-check that a voxel still means the same thing after loading.
    for (VoxelTypeId id = 0; id < original.types.type_count(); ++id) {
        REQUIRE(loaded.types.visual_of(id) == original.types.visual_of(id));
        REQUIRE(loaded.types.behaviour_of(id).material ==
                original.types.behaviour_of(id).material);
        REQUIRE(loaded.types.behaviour_of(id).tags ==
                original.types.behaviour_of(id).tags);
    }
    // Including the tag that lives past the fast-bit range.
    CHECK(loaded.types.behaviour_of(2).tags.has(400));
}

TEST_CASE("the ledger recount matches after loading") {
    Fixture original;
    populate(original);

    ByteWriter out;
    WorldSave save = original.save();
    write_save(out, save);

    Fixture loaded;
    WorldSave target = loaded.save();
    ByteReader in(out.data());
    REQUIRE(read_save(in, target));

    CHECK(MatterLedger::recount(loaded.world) == MatterLedger::recount(original.world));
}

TEST_CASE("a corrupt or foreign file is refused rather than half-loaded") {
    Fixture fixture;
    ByteWriter out;
    out.u32v(0x12345678);   // wrong magic
    out.u32v(1);

    WorldSave target = fixture.save();
    ByteReader in(out.data());
    CHECK_FALSE(read_save(in, target));
    CHECK(fixture.world.chunk_count() == 0);
}

TEST_CASE("a wrong version is refused with a clear failure") {
    Fixture fixture;
    ByteWriter out;
    out.u32v(0x31575357u);
    out.u32v(999);

    WorldSave target = fixture.save();
    ByteReader in(out.data());
    CHECK_FALSE(read_save(in, target));
}

TEST_CASE("truncating a save anywhere fails cleanly instead of crashing") {
    Fixture original;
    populate(original);
    ByteWriter out;
    WorldSave save = original.save();
    write_save(out, save);

    const std::vector<u8>& full = out.data();
    for (usize cut = 1; cut < full.size(); cut += 97) {
        Fixture fixture;
        WorldSave target = fixture.save();
        ByteReader in(full.data(), cut);
        const bool loaded = read_save(in, target);
        // Loading a truncated file may fail; what it must never do is read out of bounds
        // or report success on a short read.
        if (loaded) REQUIRE(in.ok());
    }
}
