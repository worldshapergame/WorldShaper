#pragma once
// The summary octree: what the world looks like at every scale above a voxel.
//
// documentation/03-voxel-data-model.md §Node has always called for "a sparse octree above
// chunks, for distant rendering". This is it. Stage 4 built its *occupancy* — enough to skip
// empty space — and never its colour, which is what actually draws something.
//
// A node is a Thumbnail: 8x8x8 cells, each a filtered colour and how full it is. Level 0
// covers one chunk, so a cell is a cubic metre; every level up doubles the span, so level k
// covers 2^k chunks and its cells are 2^k metres. Two kilobytes a node at any level.
//
// **A node is built from its eight children, never from the world.** That distinction is the
// whole reason this file exists.
//
// The obvious alternative — summarise a block by sampling bricks inside it — was built,
// measured and thrown away. A fixed sample count per cell makes every level cost the same,
// which is the only way a block of 65,536 chunks is affordable at all. But sampling misses
// thin structure, and thin structure is most of a world: a floor one brick thick, sampled at
// stride four, is simply not there. The tier drew empty sky over ground that was plainly
// visible. It is the same failure as air winning a majority vote when a clip is shrunk, one
// level up — and it cannot be patched with a floor value, because the sample never sees the
// matter in the first place.
//
// Aggregating children instead is exact. Every voxel contributes to exactly one cell at
// every level, so a single railing thins out as it should and never disappears: coverage
// halves as the cells double, and floors at "present" rather than at zero.
//
// The cost is a node per 4 chunks per level rather than nothing, which for a flat world is
// about 1.33 nodes per chunk across all levels — under 3 KB a chunk. The saving is that
// building the top of the tree is O(chunks) once, not O(chunks) per node.

#include <unordered_map>
#include <unordered_set>

#include "core/types.hpp"
#include "world/thumbnail.hpp"
#include "world/world.hpp"

namespace ws {

class VoxelTypeTable;

// Level 0 is a chunk (8 m); level 15 is 32,768 chunks, which is 262 km across. Well past
// anything a 64-bit voxel world will be asked to draw at once, and the tree is sparse so
// unused levels cost nothing.
inline constexpr u32 kMaxSummaryLevels = 16;

// Chunks per side of a level's block.
constexpr i64 summary_span(u32 level) { return static_cast<i64>(1) << level; }

struct SummaryKey {
    u32 level = 0;
    ChunkCoord block{};
    bool operator==(const SummaryKey& other) const {
        return level == other.level && block == other.block;
    }
};

struct SummaryKeyHash {
    usize operator()(const SummaryKey& key) const {
        const usize base = ChunkCoordHash{}(key.block);
        return base * 0x9E3779B97F4A7C15ull + key.level;
    }
};

class SummaryTree {
public:
    void create(const VoxelTypeTable& types);

    // The summary of one block, built on demand along with any of its descendants that do
    // not exist yet. Null when that block holds nothing at all.
    //
    // The first call for a large block is expensive — it walks everything underneath — and
    // every call after it is a hash lookup. Callers build a bounded number per frame.
    const Thumbnail* get(const World& world, u32 level, const ChunkCoord& block);

    // Already built, without building anything. For a caller that wants to know whether a
    // frame's work has been done rather than to do it.
    const Thumbnail* peek(u32 level, const ChunkCoord& block) const;

    // A chunk changed: drop its node and every ancestor, since each of them was derived
    // from it. Costs one erase per level, not a rebuild.
    void invalidate(const ChunkCoord& chunk);

    // Which blocks contain anything at all, at every level. Without it, building a high level
    // recurses into eight children unconditionally — 8^9 is 134 million nodes for a world of
    // a few hundred chunks, and the first version of this simply never returned. With it, the
    // recursion only ever descends where there is something to find, so building the top of
    // the tree costs one node per occupied block rather than one per possible block.
    //
    // Rebuilt from the world's chunk list, which is O(chunks x levels) and runs when the set
    // of chunks changes — not when their contents change.
    void index_world(const World& world);

    void clear();
    usize node_count() const { return nodes_.size(); }

    // Which block at `level` contains a chunk.
    static ChunkCoord block_of(const ChunkCoord& chunk, u32 level);

private:
    // Nothing there. Stored rather than absent so an empty region is not walked again every
    // time something asks about it — which for open sky above a world is most queries.
    struct Node {
        Thumbnail summary;
        bool empty = true;
    };

    const Node* build(const World& world, u32 level, const ChunkCoord& block);

    const VoxelTypeTable* types_ = nullptr;
    std::unordered_map<SummaryKey, Node, SummaryKeyHash> nodes_;
    std::unordered_set<SummaryKey, SummaryKeyHash> occupied_;
    bool indexed_ = false;
};

}  // namespace ws
