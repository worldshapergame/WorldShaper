#include <doctest/doctest.h>

#include <vector>

#include "core/hash.hpp"
#include "world/op.hpp"
#include "world/world.hpp"

using namespace ws;

TEST_CASE("a single-voxel op changes one voxel") {
    World world;
    MatterLedger ledger;
    const Op op = Op::set_voxel(1, 7, 10, 20, 30, 5, MatterReason::PlayerPlace);

    const OpResult result = apply_op(world, op, ledger);
    CHECK(result.voxels_changed == 1);
    CHECK(result.voxels_visited == 1);
    CHECK(world.get(10, 20, 30) == 5);
    CHECK(ledger.total(5) == 1);
}

TEST_CASE("applying the same op twice changes nothing the second time") {
    World world;
    MatterLedger ledger;
    const Op op = Op::set_voxel(1, 0, 0, 0, 0, 3, MatterReason::PlayerPlace);

    CHECK(apply_op(world, op, ledger).voxels_changed == 1);
    CHECK(apply_op(world, op, ledger).voxels_changed == 0);
    CHECK(ledger.total(3) == 1);
}

TEST_CASE("the chisel fills the inclusive box between its two corners") {
    World world;
    MatterLedger ledger;
    const Op op = Op::fill_box(1, 0, 0, 0, 0, 3, 4, 5, 9, MatterReason::PlayerPlace);
    CHECK(op.volume() == 4 * 5 * 6);

    const OpResult result = apply_op(world, op, ledger);
    CHECK(result.voxels_changed == 4 * 5 * 6);
    CHECK(world.get(0, 0, 0) == 9);
    CHECK(world.get(3, 4, 5) == 9);
    CHECK(world.get(4, 4, 5) == kAir);
    CHECK(ledger.total(9) == 4 * 5 * 6);
}

TEST_CASE("corner order does not matter, so two peers describing the same box agree") {
    World a;
    World b;
    MatterLedger la;
    MatterLedger lb;

    apply_op(a, Op::fill_box(1, 0, -2, -3, -4, 5, 6, 7, 2, MatterReason::PlayerPlace), la);
    apply_op(b, Op::fill_box(1, 0, 5, 6, 7, -2, -3, -4, 2, MatterReason::PlayerPlace), lb);

    CHECK(a.content_hash() == b.content_hash());

    const Op forward = Op::fill_box(1, 0, -2, -3, -4, 5, 6, 7, 2, MatterReason::PlayerPlace);
    const Op backward = Op::fill_box(1, 0, 5, 6, 7, -2, -3, -4, 2, MatterReason::PlayerPlace);
    CHECK(forward == backward);
    CHECK(forward.content_hash() == backward.content_hash());
}

TEST_CASE("a box that spans chunk boundaries is still one op") {
    World world;
    MatterLedger ledger;
    const Op op = Op::fill_box(1, 0, 250, 250, 250, 261, 261, 261, 4,
                               MatterReason::PlayerPlace);
    const OpResult result = apply_op(world, op, ledger);
    CHECK(result.voxels_changed == 12 * 12 * 12);
    CHECK(world.chunk_count() == 8);   // the box straddles all eight neighbouring chunks
    CHECK(world.get(250, 250, 250) == 4);
    CHECK(world.get(261, 261, 261) == 4);
    CHECK(world.validate());
}

TEST_CASE("carving is an op like any other and is accounted for") {
    World world;
    MatterLedger ledger;
    apply_op(world, Op::fill_box(1, 1, 0, 0, 0, 7, 7, 7, 6, MatterReason::PlayerPlace),
             ledger);
    CHECK(ledger.total(6) == 512);

    apply_op(world, Op::fill_box(2, 1, 2, 2, 2, 5, 5, 5, kAir, MatterReason::PlayerBreak),
             ledger);
    CHECK(ledger.total(6) == 512 - 64);
    CHECK(ledger.audit(world));
}

TEST_CASE("replaying the same log onto the same start gives the same world") {
    // This is the entire multiplayer design in one test: peers exchange intent, not
    // voxels, and arrive at identical state.
    OpLog log;
    for (u32 i = 0; i < 500; ++i) {
        const u64 h = hash_cell(i, 0, 0, i, 1);
        const i64 x = static_cast<i64>(hash_range(h, 400)) - 200;
        const i64 y = static_cast<i64>(hash_range(hash_mix(h + 1), 400)) - 200;
        const i64 z = static_cast<i64>(hash_range(hash_mix(h + 2), 400)) - 200;
        const u32 size = hash_range(hash_mix(h + 3), 4);
        const VoxelTypeId type =
            (hash_range(hash_mix(h + 4), 100) < 25) ? kAir : (1 + hash_range(h, 8));
        log.append(Op::fill_box(i, i % 4, x, y, z, x + size, y + size, z + size, type,
                                type == kAir ? MatterReason::PlayerBreak
                                             : MatterReason::PlayerPlace));
    }

    World a;
    MatterLedger la;
    apply_ops(a, log.ops(), la);

    World b;
    MatterLedger lb;
    apply_ops(b, log.ops(), lb);

    CHECK(a.content_hash() == b.content_hash());
    CHECK(la.totals() == lb.totals());
    CHECK(la.change_count() == lb.change_count());
}

TEST_CASE("the op log's rolling hash detects a divergent history") {
    OpLog a;
    OpLog b;
    for (u32 i = 0; i < 100; ++i) {
        const Op op = Op::set_voxel(i, 0, i, 0, 0, 1, MatterReason::PlayerPlace);
        a.append(op);
        b.append(op);
    }
    CHECK(a.rolling_hash() == b.rolling_hash());

    b.append(Op::set_voxel(100, 0, 0, 0, 0, 2, MatterReason::PlayerPlace));
    CHECK(a.rolling_hash() != b.rolling_hash());
}

TEST_CASE("op order matters and the hash says so") {
    OpLog forward;
    OpLog reversed;
    const Op first = Op::set_voxel(1, 0, 0, 0, 0, 1, MatterReason::PlayerPlace);
    const Op second = Op::set_voxel(2, 0, 0, 0, 0, 2, MatterReason::PlayerPlace);

    forward.append(first);
    forward.append(second);
    reversed.append(second);
    reversed.append(first);
    CHECK(forward.rolling_hash() != reversed.rolling_hash());
}

TEST_CASE("the ledger matches a full recount after a long random session") {
    // The conservation audit. If anything ever writes voxels without going through an op,
    // or the ledger miscounts a substitution, this fails.
    World world;
    MatterLedger ledger;

    for (u32 step = 0; step < 3000; ++step) {
        const u64 h = hash_cell(step, 2, 4, step, 9);
        const i64 x = static_cast<i64>(hash_range(h, 300)) - 150;
        const i64 y = static_cast<i64>(hash_range(hash_mix(h + 1), 300)) - 150;
        const i64 z = static_cast<i64>(hash_range(hash_mix(h + 2), 300)) - 150;
        const u32 size = hash_range(hash_mix(h + 3), 3);
        const u32 roll = hash_range(hash_mix(h + 4), 100);
        const VoxelTypeId type = (roll < 30) ? kAir : (1 + hash_range(hash_mix(h + 5), 6));

        apply_op(world,
                 Op::fill_box(step, step % 3, x, y, z, x + size, y + size, z + size, type,
                              type == kAir ? MatterReason::PlayerBreak
                                           : MatterReason::PlayerPlace),
                 ledger);

        if (step % 500 == 0) REQUIRE(ledger.audit_failures(world).empty());
    }

    CHECK(ledger.audit(world));
    world.compact();
    CHECK(ledger.audit(world));   // compaction must not lose a single voxel
}

TEST_CASE("per-player accounting is recorded, ready for survival") {
    // Decision D53: build the hooks now, because retrofitting them later means auditing
    // every system that touches the world.
    World world;
    MatterLedger ledger;

    apply_op(world, Op::fill_box(1, 42, 0, 0, 0, 3, 3, 3, 5, MatterReason::PlayerPlace),
             ledger);
    CHECK(ledger.player(42).placed == 64);
    CHECK(ledger.player(42).removed == 0);

    apply_op(world, Op::fill_box(2, 42, 0, 0, 0, 1, 1, 1, kAir, MatterReason::PlayerBreak),
             ledger);
    CHECK(ledger.player(42).removed == 8);
    CHECK(ledger.player(7).placed == 0);   // a player who has done nothing owes nothing

    // Simulation and generation move matter without crediting anybody.
    apply_op(world, Op::set_voxel(3, 42, 100, 100, 100, 9, MatterReason::Generation),
             ledger);
    CHECK(ledger.player(42).placed == 64);
}

TEST_CASE("substituting one material for another moves the totals, not the count") {
    World world;
    MatterLedger ledger;
    apply_op(world, Op::fill_box(1, 0, 0, 0, 0, 3, 3, 3, 1, MatterReason::PlayerPlace),
             ledger);
    CHECK(ledger.total(1) == 64);

    apply_op(world, Op::fill_box(2, 0, 0, 0, 0, 3, 3, 3, 2, MatterReason::PlayerPlace),
             ledger);
    CHECK(ledger.total(1) == 0);
    CHECK(ledger.total(2) == 64);
    CHECK(ledger.audit(world));
}
