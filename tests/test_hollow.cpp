#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <vector>

#include "game/chisel.hpp"
#include "game/clip.hpp"
#include "world/op.hpp"
#include "world/world.hpp"

using namespace ws;

namespace {

u64 total_volume(const std::vector<Op>& ops) {
    u64 sum = 0;
    for (const Op& op : ops) sum += op.volume();
    return sum;
}

// Every voxel the ops touch, so overlap is detectable rather than assumed away.
usize distinct_voxels(const std::vector<Op>& ops) {
    std::vector<std::array<i64, 3>> seen;
    for (const Op& raw : ops) {
        Op op = raw;
        op.normalise();
        for (i64 z = op.z0; z <= op.z1; ++z) {
            for (i64 y = op.y0; y <= op.y1; ++y) {
                for (i64 x = op.x0; x <= op.x1; ++x) seen.push_back({x, y, z});
            }
        }
    }
    std::sort(seen.begin(), seen.end());
    seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
    return seen.size();
}

}  // namespace

TEST_CASE("a hollow box is a shell with the inside left alone") {
    u64 id = 1;
    std::vector<Op> ops;
    const Op box = Op::fill_box(0, 0, 0, 0, 0, 9, 9, 9, 1, MatterReason::PlayerPlace);
    hollow_box(box, 1, id, ops);

    CHECK(ops.size() == 6);

    // 10^3 solid, minus the 8^3 that should be untouched.
    CHECK(total_volume(ops) == 1000 - 512);
}

TEST_CASE("the slabs of a shell never share a voxel") {
    // Two ops writing the same voxel count it twice in the matter ledger, and the ledger is
    // audited against a full recount — so an overlap here fails an invariant somewhere else
    // entirely, which is a miserable thing to debug.
    u64 id = 1;
    std::vector<Op> ops;
    hollow_box(Op::fill_box(0, 0, -3, -3, -3, 6, 5, 4, 1, MatterReason::PlayerPlace), 2, id, ops);

    CHECK(distinct_voxels(ops) == total_volume(ops));
}

TEST_CASE("a box with no inside is emitted whole") {
    u64 id = 1;
    std::vector<Op> ops;
    // Five voxels across with a shell of two leaves a single voxel; three across leaves none.
    hollow_box(Op::fill_box(0, 0, 0, 0, 0, 2, 2, 2, 1, MatterReason::PlayerPlace), 2, id, ops);
    CHECK(ops.size() == 1);
    CHECK(total_volume(ops) == 27);
}

TEST_CASE("thickness zero is solid") {
    u64 id = 1;
    std::vector<Op> ops;
    hollow_box(Op::fill_box(0, 0, 0, 0, 0, 9, 9, 9, 1, MatterReason::PlayerPlace), 0, id, ops);
    CHECK(ops.size() == 1);
    CHECK(total_volume(ops) == 1000);
}

TEST_CASE("every slab of a shell gets its own id") {
    // They go into the op log, which is replayed and hashed. Two ops sharing an id is a
    // divergence waiting for a multiplayer session.
    u64 id = 7;
    std::vector<Op> ops;
    hollow_box(Op::fill_box(0, 0, 0, 0, 0, 9, 9, 9, 1, MatterReason::PlayerPlace), 1, id, ops);

    std::vector<u64> ids;
    for (const Op& op : ops) ids.push_back(op.tick);
    std::sort(ids.begin(), ids.end());
    const bool all_distinct = std::adjacent_find(ids.begin(), ids.end()) == ids.end();
    CHECK(all_distinct);
    CHECK(id == 7 + ops.size());
}

TEST_CASE("hollowing a clip leaves a shell of its own shape") {
    World world;
    MatterLedger ledger;
    apply_op(world, Op::fill_box(0, 0, 0, 0, 0, 9, 9, 9, 1, MatterReason::Generation), ledger);

    const Clip solid = capture_clip(world, 0, 0, 0, 9, 9, 9);
    REQUIRE(solid.solid_count() == 1000);

    const Clip shell = hollow_clip(solid, 1);
    CHECK(shell.solid_count() == 1000 - 512);

    // The rim survives and the middle does not.
    CHECK(shell.at(0, 0, 0) != kAir);
    CHECK(shell.at(5, 0, 5) != kAir);
    CHECK(shell.at(5, 5, 5) == kAir);
}

TEST_CASE("a clip thinner than the shell keeps everything") {
    World world;
    MatterLedger ledger;
    apply_op(world, Op::fill_box(0, 0, 0, 0, 0, 2, 2, 2, 1, MatterReason::Generation), ledger);

    const Clip solid = capture_clip(world, 0, 0, 0, 2, 2, 2);
    const Clip shell = hollow_clip(solid, 3);
    CHECK(shell.solid_count() == solid.solid_count());
}

TEST_CASE("hollowing follows the shape, not its bounding box") {
    // The point of doing this as an erosion rather than as six slabs. An L needs a shell that
    // turns the corner; a box shell would wall off the notch as though it were solid.
    World world;
    MatterLedger ledger;
    apply_op(world, Op::fill_box(0, 0, 0, 0, 0, 9, 9, 3, 1, MatterReason::Generation), ledger);
    apply_op(world, Op::fill_box(1, 0, 0, 0, 4, 3, 9, 9, 1, MatterReason::Generation), ledger);

    const Clip solid = capture_clip(world, 0, 0, 0, 9, 9, 9);
    const Clip shell = hollow_clip(solid, 1);

    CHECK(shell.solid_count() > 0);
    CHECK(shell.solid_count() < solid.solid_count());

    // A cell deep inside the thick part of the L is removed; the inner corner, which is one
    // step from the notch, is kept.
    CHECK(shell.at(5, 5, 1) == kAir);
    CHECK(shell.at(5, 5, 3) != kAir);
}
