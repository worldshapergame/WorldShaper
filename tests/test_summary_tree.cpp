#include <doctest/doctest.h>

#include "world/op.hpp"
#include "world/summary_tree.hpp"
#include "world/world.hpp"

using namespace ws;

namespace {
VoxelTypeTable types;
}  // namespace

TEST_CASE("a summary of nothing is nothing, at every level") {
    World world;
    SummaryTree tree;
    tree.create(types);
    for (u32 level = 0; level < 8; ++level) {
        CHECK(tree.get(world, level, ChunkCoord{0, 0, 0}) == nullptr);
    }
}

TEST_CASE("one chunk of matter is visible from every level above it") {
    // The test that matters, and the one the sampling approach failed. A floor one voxel
    // thick is 0.003% of a cubic metre; by level 8 that cell covers 256 m and holds the same
    // sliver of matter. Coverage halves at every level and reaches zero almost immediately —
    // and zero means "empty", so a naive average makes the world disappear as you climb the
    // tree, at whatever distance that level happens to be used.
    World world;
    MatterLedger ledger;
    apply_op(world, Op::fill_box(0, 0, 0, 0, 0, 255, 0, 255, 1, MatterReason::Generation),
             ledger);

    SummaryTree tree;
    tree.create(types);

    for (u32 level = 0; level < 10; ++level) {
        const ChunkCoord block = SummaryTree::block_of(ChunkCoord{0, 0, 0}, level);
        const Thumbnail* summary = tree.get(world, level, block);
        REQUIRE_MESSAGE(summary != nullptr, "level " << level << " lost the world");
        REQUIRE_FALSE(summary->empty());

        u32 solid = 0;
        for (u32 cell : summary->cells) {
            if (cell != 0) ++solid;
        }
        CHECK_MESSAGE(solid > 0, "level " << level << " summarised matter as air");
    }
}

TEST_CASE("a node is exactly its eight children folded together") {
    World world;
    MatterLedger ledger;
    // Two chunks of solid matter, diagonally apart, so the fold has to put each in the right
    // octant rather than merely somewhere.
    apply_op(world, Op::fill_box(0, 0, 0, 0, 0, 255, 255, 255, 1, MatterReason::Generation),
             ledger);
    apply_op(world, Op::fill_box(1, 0, 256, 256, 256, 511, 511, 511, 1, MatterReason::Generation),
             ledger);

    SummaryTree tree;
    tree.create(types);

    // Level 1 covers chunks (0,0,0) to (1,1,1), so the two solid chunks are opposite corners.
    const Thumbnail* level1 = tree.get(world, 1, ChunkCoord{0, 0, 0});
    REQUIRE(level1 != nullptr);

    // The low octant and the high octant are full; the other six are empty.
    CHECK(level1->at(0, 0, 0) != 0u);
    CHECK(level1->at(3, 3, 3) != 0u);
    CHECK(level1->at(4, 4, 4) != 0u);
    CHECK(level1->at(7, 7, 7) != 0u);
    CHECK(level1->at(0, 0, 7) == 0u);   // an octant with nothing in it
    CHECK(level1->at(7, 0, 0) == 0u);
    CHECK(level1->at(0, 7, 0) == 0u);
}

TEST_CASE("coverage halves as the cells double") {
    // A solid chunk is completely full. One level up it occupies one eighth of the block, so
    // its cells are one eighth full — and so on. Getting this wrong makes distant terrain
    // either transparent or inflated.
    World world;
    MatterLedger ledger;
    apply_op(world, Op::fill_box(0, 0, 0, 0, 0, 255, 255, 255, 1, MatterReason::Generation),
             ledger);

    SummaryTree tree;
    tree.create(types);

    const u32 full = tree.get(world, 0, ChunkCoord{0, 0, 0})->at(4, 4, 4) >> 24;
    CHECK(full == 255u);

    // At level 1 the solid chunk fills one octant, and a cell inside that octant is still
    // entirely matter — the cell is smaller than the chunk. Coverage stays high.
    const u32 inside = tree.get(world, 1, ChunkCoord{0, 0, 0})->at(1, 1, 1) >> 24;
    CHECK(inside == 255u);

    // A cell in an empty octant is empty.
    CHECK(tree.get(world, 1, ChunkCoord{0, 0, 0})->at(6, 6, 6) == 0u);
}

TEST_CASE("editing a chunk drops it and every ancestor") {
    World world;
    MatterLedger ledger;
    apply_op(world, Op::fill_box(0, 0, 0, 0, 0, 255, 255, 255, 1, MatterReason::Generation),
             ledger);

    SummaryTree tree;
    tree.create(types);
    REQUIRE(tree.get(world, 6, SummaryTree::block_of(ChunkCoord{0, 0, 0}, 6)) != nullptr);
    const usize built = tree.node_count();
    REQUIRE(built >= 7);

    tree.invalidate(ChunkCoord{0, 0, 0});
    CHECK(tree.node_count() < built);

    // Nothing above it survives, so a stale summary cannot be handed out.
    for (u32 level = 0; level <= 6; ++level) {
        CHECK(tree.peek(level, SummaryTree::block_of(ChunkCoord{0, 0, 0}, level)) == nullptr);
    }
}

TEST_CASE("an empty region is remembered as empty rather than walked again") {
    World world;
    MatterLedger ledger;
    apply_op(world, Op::fill_box(0, 0, 0, 0, 0, 255, 255, 255, 1, MatterReason::Generation),
             ledger);

    SummaryTree tree;
    tree.create(types);
    tree.get(world, 4, ChunkCoord{100, 100, 100});   // far from anything
    const usize after_first = tree.node_count();
    REQUIRE(after_first > 0);

    tree.get(world, 4, ChunkCoord{100, 100, 100});
    CHECK(tree.node_count() == after_first);   // answered from the map, nothing rebuilt
}

TEST_CASE("a thin floor survives all the way up a large world") {
    // The real shape of the problem: a wide, one-voxel-thick floor, which is what a built
    // platform looks like from above and precisely what sampling could not see.
    World world;
    MatterLedger ledger;
    apply_op(world, Op::fill_box(0, 0, -2048, 0, -2048, 2047, 0, 2047, 1,
                                 MatterReason::Generation),
             ledger);

    SummaryTree tree;
    tree.create(types);

    for (u32 level = 0; level < 6; ++level) {
        const ChunkCoord block = SummaryTree::block_of(ChunkCoord{4, 0, 4}, level);
        const Thumbnail* summary = tree.get(world, level, block);
        REQUIRE_MESSAGE(summary != nullptr, "the floor vanished at level " << level);
    }
}
