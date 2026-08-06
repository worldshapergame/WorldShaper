#include <doctest/doctest.h>

#include "core/hash.hpp"
#include "world/history.hpp"
#include "world/ledger.hpp"
#include "world/region.hpp"
#include "world/world.hpp"

using namespace ws;

namespace {

constexpr VoxelTypeId kRock = 3;
constexpr VoxelTypeId kWood = 4;
constexpr VoxelTypeId kGlass = 5;

Op box(i64 x0, i64 y0, i64 z0, i64 x1, i64 y1, i64 z1, VoxelTypeId type) {
    return Op::fill_box(1, 1, x0, y0, z0, x1, y1, z1, type,
                        (type == kAir) ? MatterReason::PlayerBreak : MatterReason::PlayerPlace);
}

// Deterministic scrambling, so a test failure is reproducible.
void scatter(World& world, i64 x0, i64 y0, i64 z0, i64 x1, i64 y1, i64 z1) {
    for (i64 z = z0; z <= z1; ++z) {
        for (i64 y = y0; y <= y1; ++y) {
            for (i64 x = x0; x <= x1; ++x) {
                const u64 h = hash_cell(x, y, z, 0, 0xABCDEFull);
                const VoxelTypeId choice[4] = {kAir, kRock, kWood, kGlass};
                world.set(x, y, z, choice[h & 3]);
            }
        }
    }
}

}  // namespace

TEST_CASE("decompose_region rebuilds a region exactly") {
    World source;
    scatter(source, -20, -20, -20, 20, 20, 20);
    source.set(300, 4, 4, kRock);   // outside the region: must not be touched

    std::vector<Op> ops;
    const u64 count = decompose_region(source, -20, -20, -20, 20, 20, 20, 1, 1, ops);
    CHECK(count == ops.size());
    CHECK(count > 0);

    World rebuilt;
    MatterLedger ledger;
    apply_ops(rebuilt, ops, ledger);

    for (i64 z = -20; z <= 20; ++z) {
        for (i64 y = -20; y <= 20; ++y) {
            for (i64 x = -20; x <= 20; ++x) {
                REQUIRE(rebuilt.get(x, y, z) == source.get(x, y, z));
            }
        }
    }
}

TEST_CASE("a uniform region costs one op to describe however large it is") {
    World world;
    MatterLedger ledger;
    apply_op(world, box(0, 0, 0, 255, 255, 255, kRock), ledger);

    std::vector<Op> ops;
    // Brick-aligned and entirely one material: the whole 16.7 million voxels collapse.
    decompose_region(world, 0, 0, 0, 255, 255, 255, 1, 1, ops);
    CHECK(ops.size() == 1);
    CHECK(ops[0].type == kRock);
    CHECK(ops[0].volume() == 256ull * 256ull * 256ull);
}

TEST_CASE("undo and redo restore the world exactly") {
    World world;
    MatterLedger ledger;
    OpLog log;
    EditHistory history;

    apply_op(world, box(0, 0, 0, 63, 63, 63, kRock), ledger);
    scatter(world, 10, 10, 10, 30, 30, 30);
    const u64 before = world.content_hash();

    history.apply(world, ledger, log, box(8, 8, 8, 40, 40, 40, kAir));
    const u64 after_carve = world.content_hash();
    CHECK(after_carve != before);
    CHECK(history.undo_depth(1) == 1);

    REQUIRE(history.undo(world, ledger, log, 2, 1));
    CHECK(world.content_hash() == before);
    CHECK(history.undo_depth(1) == 0);
    CHECK(history.redo_depth(1) == 1);

    REQUIRE(history.redo(world, ledger, log, 3, 1));
    CHECK(world.content_hash() == after_carve);
}

TEST_CASE("undo depth is unlimited and every step is reversible") {
    World world;
    MatterLedger ledger;
    OpLog log;
    EditHistory history;

    apply_op(world, box(-64, -64, -64, 64, 64, 64, kRock), ledger);

    std::vector<u64> hashes;
    hashes.push_back(world.content_hash());

    // Two hundred overlapping edits. The point is that nothing is discarded: the two
    // hundredth undo has to land on the state before the first edit.
    // Driven by the hash rather than by a modular sequence: a repeating pattern of boxes
    // eventually re-applies an edit that changes nothing, which is deliberately not an undo
    // step, and the depth would then be short of 200 for reasons unrelated to the history.
    for (i32 i = 0; i < 200; ++i) {
        const u64 h = hash_mix(static_cast<u64>(i) + 1);
        const i64 x = static_cast<i64>(h % 48) - 24;
        const i64 y = static_cast<i64>((h >> 8) % 48) - 24;
        const i64 z = static_cast<i64>((h >> 16) % 48) - 24;
        const VoxelTypeId choice[3] = {kAir, kWood, kGlass};
        const VoxelTypeId type = choice[(h >> 24) % 3];
        history.apply(world, ledger, log, box(x, y, z, x + 9, y + 5, z + 7, type));
        REQUIRE(world.content_hash() != hashes.back());
        hashes.push_back(world.content_hash());
    }
    CHECK(history.undo_depth(1) == 200);

    for (i32 i = 199; i >= 0; --i) {
        REQUIRE(history.undo(world, ledger, log, 1000 + i, 1));
        REQUIRE(world.content_hash() == hashes[static_cast<usize>(i)]);
    }
    CHECK(history.undo_depth(1) == 0);
    CHECK(history.redo_depth(1) == 200);

    for (i32 i = 0; i < 200; ++i) {
        REQUIRE(history.redo(world, ledger, log, 2000 + i, 1));
        REQUIRE(world.content_hash() == hashes[static_cast<usize>(i) + 1]);
    }
}

TEST_CASE("the matter ledger survives undo and redo") {
    World world;
    MatterLedger ledger;
    OpLog log;
    EditHistory history;

    apply_op(world, box(0, 0, 0, 40, 40, 40, kRock), ledger);
    scatter(world, 5, 5, 5, 20, 20, 20);
    ledger.clear();
    for (const auto& [type, count] : MatterLedger::recount(world)) {
        ledger.record_bulk(kAir, type, static_cast<u64>(count), MatterReason::Load);
    }
    REQUIRE(ledger.audit(world));

    history.apply(world, ledger, log, box(3, 3, 3, 30, 30, 30, kGlass));
    CHECK(ledger.audit(world));
    REQUIRE(history.undo(world, ledger, log, 5, 1));
    CHECK(ledger.audit(world));
    REQUIRE(history.redo(world, ledger, log, 6, 1));
    CHECK(ledger.audit(world));
}

TEST_CASE("a new edit discards the redo branch") {
    World world;
    MatterLedger ledger;
    OpLog log;
    EditHistory history;

    history.apply(world, ledger, log, box(0, 0, 0, 7, 7, 7, kRock));
    history.apply(world, ledger, log, box(4, 4, 4, 11, 11, 11, kWood));
    REQUIRE(history.undo(world, ledger, log, 3, 1));
    CHECK(history.redo_depth(1) == 1);

    history.apply(world, ledger, log, box(20, 20, 20, 27, 27, 27, kGlass));
    CHECK(history.redo_depth(1) == 0);
    CHECK_FALSE(history.redo(world, ledger, log, 4, 1));
}

TEST_CASE("an edit that changes nothing is not an undo step") {
    World world;
    MatterLedger ledger;
    OpLog log;
    EditHistory history;

    history.apply(world, ledger, log, box(0, 0, 0, 7, 7, 7, kRock));
    CHECK(history.undo_depth(1) == 1);
    history.apply(world, ledger, log, box(0, 0, 0, 7, 7, 7, kRock));
    CHECK(history.undo_depth(1) == 1);
}

TEST_CASE("undo is per player") {
    World world;
    MatterLedger ledger;
    OpLog log;
    EditHistory history;

    Op mine = box(0, 0, 0, 7, 7, 7, kRock);
    mine.player = 1;
    Op theirs = box(64, 0, 0, 71, 7, 7, kWood);
    theirs.player = 2;

    history.apply(world, ledger, log, mine);
    history.apply(world, ledger, log, theirs);

    CHECK(history.undo_depth(1) == 1);
    CHECK(history.undo_depth(2) == 1);

    REQUIRE(history.undo(world, ledger, log, 3, 1));
    CHECK(world.get(0, 0, 0) == kAir);
    CHECK(world.get(64, 0, 0) == kWood);   // the other player's edit stands
}

TEST_CASE("every op the history produces is replayable") {
    // The identity the whole architecture rests on: a world is the ordered op log applied
    // to an empty one. Undo must not be an exception to it.
    World world;
    MatterLedger ledger;
    OpLog log;
    EditHistory history;

    history.apply(world, ledger, log, box(0, 0, 0, 31, 31, 31, kRock));
    history.apply(world, ledger, log, box(8, 8, 8, 20, 20, 20, kAir));
    history.apply(world, ledger, log, box(10, 10, 10, 12, 40, 12, kGlass));
    REQUIRE(history.undo(world, ledger, log, 10, 1));
    history.apply(world, ledger, log, box(0, 40, 0, 5, 45, 5, kWood));
    REQUIRE(history.undo(world, ledger, log, 11, 1));
    REQUIRE(history.redo(world, ledger, log, 12, 1));

    World replayed;
    MatterLedger replay_ledger;
    apply_ops(replayed, log.ops(), replay_ledger);
    CHECK(replayed.content_hash() == world.content_hash());
}
