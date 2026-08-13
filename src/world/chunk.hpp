#pragma once
// A chunk: 32×32×32 bricks = 256³ voxels = an 8 metre cube. The unit of streaming,
// saving and network reconciliation (documentation/03-voxel-data-model.md §1).
//
// Inside, bricks hang off a five-level sparse octree. Sparse is the whole point: a chunk
// of open sky costs one node, and a chunk with a single wall in it costs the handful of
// bricks that wall touches plus the path down to them. A flat 32³ array of brick handles
// would cost 128 KB per chunk whether it held anything or not, which at infinite world
// size is not a rounding error — it is the difference between the design working and not.
//
// The octree is also the bottom five levels of the hierarchy the renderer descends
// per pixel (documentation/04-rendering.md §1), so it is not a storage-only structure.

#include <vector>

#include "core/types.hpp"
#include "world/brick.hpp"

namespace ws {

inline constexpr u32 kChunkDepth = 5;                   // 2^5 = 32 bricks per axis
inline constexpr u32 kNoChild = 0xFFFFFFFFu;

class Chunk {
public:
    Chunk();

    // Voxel coordinates local to the chunk, 0–255 on each axis.
    VoxelTypeId get(u32 x, u32 y, u32 z) const;
    bool set(u32 x, u32 y, u32 z, VoxelTypeId type);

    // Brick coordinates, 0–31 on each axis. Null means "entirely air"; there is no
    // allocated brick and nothing to read.
    const Brick* brick(u32 bx, u32 by, u32 bz) const;

    // Allocates the brick and the path down to it if they do not exist yet.
    Brick& brick_for_write(u32 bx, u32 by, u32 bz);

    bool empty() const { return brick_count_ == 0; }
    u32 brick_count() const { return brick_count_; }

    // How many allocated bricks hold nothing. This should be nought at every moment, and it is
    // an instrument rather than a statistic: an empty brick left allocated is `world_has` — and
    // therefore every child mask in the render tree — claiming matter the world does not have,
    // which the marcher draws as a filled cube it can never build (D620). A count above nought
    // names how many lumps are standing.
    u32 empty_bricks() const;
    u32 node_count() const;
    u64 solid_voxels() const;
    usize bytes() const;

    // Frees bricks that have become entirely air, then frees the nodes left with no
    // children. A sweep of the whole chunk: for bulk writers that fill or assign a brick
    // outright, and for re-encoding after an edit. Nothing on an edit path should need it
    // any more — see drop_brick_if_empty.
    void compact();

    // Frees one brick if its last voxel has gone, and the nodes above it that lose their
    // last child with it. One descent, no scan: O(1) in the size of the chunk.
    //
    // This is not an optimisation. `NodePool::world_has` asks whether a brick is
    // ALLOCATED, and every child mask in the render tree is derived from that answer, so a
    // brick left behind empty tells the renderer the world still holds matter there — the
    // shadow of a deleted wall outlives the wall, and the region fades back in black
    // (D348). Making the reader test emptiness instead is correct and costs a scan past
    // every emptied brick, measured at 726 ms of CPU a frame after a large deletion;
    // freeing it at the moment it empties costs nothing and asks nothing of the reader.
    //
    // set() calls this itself. A bulk writer that goes through brick_for_write() and
    // fills or assigns the whole brick has to say so, because the chunk never sees the
    // individual writes. Returns true when the brick was freed.
    bool drop_brick_if_empty(u32 bx, u32 by, u32 bz);

    // Encoding-independent: two chunks holding the same voxels hash the same however
    // they were built. This is what network reconciliation compares
    // (documentation/06-multiplayer.md §4).
    //
    // It walks every voxel, so it is for reconciliation and tests — never for per-frame
    // change detection. Use revision() for that.
    u64 content_hash() const;

    // Bumped on every mutation. Streaming compares this to decide whether a resident
    // chunk is stale, which has to be O(1): calling content_hash() once per visible chunk
    // per frame measured 116 ms a frame on the test scene, against a 0.8 ms budget.
    u64 revision() const { return revision_; }

    // For bulk writers that go through brick_for_write() and edit a whole brick at once
    // rather than voxel by voxel. Forgetting to call this leaves streaming convinced the
    // chunk is unchanged, so the edit is invisible until something else touches it.
    void mark_modified() { ++revision_; }

    // CI invariant: every referenced brick and node index is valid, nothing is referenced
    // twice, and the free lists are disjoint from the live set.
    bool validate() const;

private:
    struct Node {
        u32 child[8];
    };

    static u32 child_index(u32 bx, u32 by, u32 bz, u32 depth);
    u32 find_brick(u32 bx, u32 by, u32 bz) const;   // kNoChild when absent
    // The same descent, keeping the node it passed through at each depth. Unlinking a
    // brick needs its ancestors, and finding them a second time is the same walk twice.
    u32 find_brick_path(u32 bx, u32 by, u32 bz, u32 path[kChunkDepth]) const;
    void unlink_brick(u32 bx, u32 by, u32 bz, const u32 path[kChunkDepth], u32 index);
    u32 allocate_node();
    u32 allocate_brick();
    void free_brick(u32 index);
    void free_node(u32 index);
    bool prune(u32 node_index, u32 depth);          // true when the node became empty

    std::vector<Node> nodes_;
    std::vector<Brick> bricks_;
    std::vector<u32> free_nodes_;
    std::vector<u32> free_bricks_;
    u32 brick_count_ = 0;
    u64 revision_ = 0;
};

}  // namespace ws
