#pragma once
// The node pool: one sparse octree, at every scale, replacing the chunk system entirely.
//
// # What it replaces, and why all of it at once
//
// Geometry is currently addressed by four separate schemes glued end to end — a wrapped chunk
// grid, a per-chunk brick mask with popcount prefixes, a per-chunk brick-mask pyramid, and a
// separate summary-octree tier with its own wrapped grids and its own residency policy. Every
// seam between them has produced a bug with its own decision-log entry (D133, D137, D146, D147,
// D148, D151, D155), and they are all the same bug: two structures that answer the same question
// and are allowed to disagree.
//
// They exist because a chunk is a fixed 8 m box in a renderer whose entire premise is that
// nothing has a fixed size.
//
// So: one structure, at every scale, with one answer.
//
// # What a node is
//
//   level 3   a brick: 8 voxels, 25 cm. The finest node, and the leaf.
//   level L   2^L voxels. Level 8 is what used to be a chunk; nothing treats it specially.
//
// A node's coordinate is its voxel coordinate shifted right by its level, so a node at level L
// covers voxels [c << L, (c+1) << L).
//
// Below level 3 nothing changes: a brick keeps its palette encoding, its 64-byte occupancy and
// its two occupancy mips, and a ray marches inside it at single-voxel resolution exactly as it
// does now (D132 — the detail level chooses the colour, never the shape). Bricks are the
// storage and compression unit and they work; chunks are the fixed-size box and they do not.
//
// # Why the children are contiguous
//
// A node holds ONE index — the base of its eight children — rather than eight. Descending is
// then `children + octant`, a direct index with no hash and no search, and the eight siblings a
// descent chooses between share a cache line. Only the *entry* into the tree costs a hash
// lookup; everything below it is pointer arithmetic.
//
// Today a ray pays two dependent loads per chunk it enters (the wrapped grid cell, then that
// record's own coordinate, because the grid wraps and a cell may be held by a chunk from
// somewhere else) and does it up to thirty-two times along each axis it crosses, plus a separate
// coarse-grid fetch per skip. On a machine whose binding constraint is memory bandwidth
// (documentation/09 §1) that difference is the point of the exercise.
//
// # Why streaming cannot deadlock any more
//
// A ray reaches a node only through its parent, and a parent's `child_mask` says whether the
// child exists in the *world* — not whether it is resident. So a ray can never fail to report
// something that exists, and can never report something that does not. That is the whole of
// D133 and D147, and it is now unrepresentable rather than fixed: there is no second structure
// to disagree with the first.
//
// # What is deliberately NOT here
//
// Chunks still exist on disk and on the wire. `06-multiplayer.md` reconciles per chunk,
// `05-simulation.md` sleeps and wakes bricks, authority regions are 64 m cells, and `.wsworld`
// writes chunk-sized records. None of that is rendering and none of it benefits from being torn
// up. Chunks leave the *renderer*; they remain a storage grouping the renderer never sees.

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/block_pool.hpp"
#include "core/types.hpp"
#include "world/gpu_brick.hpp"
#include "world/world.hpp"

namespace ws {

class VoxelTypeTable;

inline constexpr u32 kNoNode = 0xFFFFFFFFu;

// A brick is the leaf. Eight voxels a side, which is level 3.
inline constexpr u32 kLeafLevel = 3;

// The coarsest node the pool will build. Level 24 is 16.7 million voxels a side — 524 km — which
// is past anything a 64-bit voxel world is asked to draw at once, and the tree is sparse so the
// levels nobody reaches cost nothing.
inline constexpr u32 kMaxNodeLevel = 24;

// Where a ray enters the tree. Above this, lookup is a hash; below it, pointer arithmetic.
//
// Level 14 is 16,384 voxels — 512 m. Coarse enough that a ray crossing a scene enters through a
// handful of them and pays a handful of hashes; fine enough that the table stays small and a
// probe stays in cache. This is a performance knob and nothing depends on its value being right,
// which is the property to preserve when tuning it.
inline constexpr u32 kEntryLevel = 14;

struct NodeKey {
    i64 x = 0;
    i64 y = 0;
    i64 z = 0;
    u32 level = 0;
    bool operator==(const NodeKey& other) const {
        return x == other.x && y == other.y && z == other.z && level == other.level;
    }
};

struct NodeKeyHash {
    usize operator()(const NodeKey& k) const noexcept {
        return static_cast<usize>(hash_cell(k.x, k.y, k.z, k.level, 0x4E4F4445ull));
    }
};

// Which node at `level` contains a voxel. Arithmetic shift, not division: a voxel at -1 belongs
// to node -1, not node 0. Getting this wrong puts every negative coordinate one node out, which
// is the same trap `chunk_of` documents.
constexpr NodeKey node_key_of(i64 x, i64 y, i64 z, u32 level) {
    return NodeKey{x >> level, y >> level, z >> level, level};
}

// Which of its parent's eight children a node is. x fastest, matching brick and cell order
// everywhere else in the engine, so a shader can walk it with no translation table.
constexpr u32 octant_of(i64 x, i64 y, i64 z) {
    return static_cast<u32>((x & 1) | ((y & 1) << 1) | ((z & 1) << 2));
}

// The bucket a root lands in.
//
// Deliberately a 32-bit mix rather than the 64-bit `hash_cell` the CPU map uses, and mixed one
// axis at a time rather than by XORing three products together. Two reasons, and both have cost
// somebody a day in this file's neighbourhood already:
//
//   the shader has to compute the identical value, and a 32-bit mix is short enough that the two
//   implementations can be compared by eye rather than trusted;
//
//   voxel coordinates are a dense lattice, and XOR-of-products over a lattice leaves whole planes
//   landing in the same few buckets — which is exactly the fault the face cache's key hit, where
//   faces that failed to find a slot rendered as scattered black voxels.
//
// Must match `node_entry_hash` in shaders/node.glsl.
constexpr u32 node_hash_mix(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

constexpr u32 entry_hash32(i32 x, i32 y, i32 z, u32 level) {
    u32 h = 0x811C9DC5u;
    h = node_hash_mix(h ^ static_cast<u32>(x));
    h = node_hash_mix(h ^ static_cast<u32>(y));
    h = node_hash_mix(h ^ static_cast<u32>(z));
    return node_hash_mix(h ^ level);
}

// What the GPU sees. Thirty-two bytes, one cache line per two nodes.
struct GpuNode {
    // The node's own coordinate at its own level, truncated to 32 bits. The shader works in
    // 32-bit integers throughout; these exist so a hash probe can confirm it found the node it
    // asked for. A false match needs two nodes 2^32 apart at the same level, which at level 14
    // is 137 billion kilometres.
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;

    // level | flags << 8 | child_mask << 16
    //
    // The child mask is the empty-space test and the reason a descent costs no memory traffic
    // for the space it skips: one byte says which of the eight children exist, so seven of them
    // can be dismissed without ever being fetched.
    u32 packed = 0;

    // Two meanings, told apart by kNodeLeaf, because a node has only ever one of them and
    // thirty-two bytes is two nodes to a cache line.
    //
    //   interior   the base slot of this node's eight children, contiguous
    //   leaf       an index into the leaf array, where this brick's header and occupancy live
    //
    // A leaf has no children by definition, so nothing is lost and nothing has to be kept in
    // step. What it does need is a guard on every descent: walking into a leaf's `children`
    // would treat a payload offset as a slot index.
    u32 children = kNoNode;

    // rgba8, filtered over the subtree. Alpha is coverage.
    //
    // Coverage never rounds to nothing (D139): a node holding one solid voxel reports "present,
    // however faintly". Without that floor a railing or a wire does not look thin at a distance,
    // it vanishes — at exactly the range where you can no longer see well enough to notice.
    u32 colour = 0;

    // How much of this node is matter as seen ALONG each of the six face directions, one byte
    // each, packed +x -x +y -y in the first word and +z -z in the low half of the second.
    //
    // This is the quantity Stage 4 needed and did not have. It tried volumetric fill fraction as
    // an alpha and distant window frames dithered into the sky, because a brick on the surface of
    // the ground is about an eighth full and completely opaque when you look at it. Fill fraction
    // and screen coverage coincide only for a node seen edge-on. Folded per direction at build
    // time, this is exact, and it is what lets the composite anti-alias an edge analytically
    // rather than with a temporal filter.
    u32 coverage_xy = 0;
    // +z and -z, one byte each, and nothing else.
    //
    // The first version stashed the brick's encoding in the top half of this word to avoid a
    // second array. It collided with the -z byte, so `index_bits` read back as a coverage value
    // and decode_voxel divided by it — a crash rather than a wrong picture, which was luck. A
    // leaf now carries a real GpuBrickHeader in its own array: the layout the renderer has used
    // since Stage 2, already tested against the world.
    u32 coverage_z = 0;
};
static_assert(sizeof(GpuNode) == 32, "GpuNode must stay 32 bytes");

inline constexpr u32 kNodeLeaf = 1u << 0;        // a brick; `payload` and occupancy are valid
inline constexpr u32 kNodeUniform = 1u << 1;     // the whole subtree is one voxel type
inline constexpr u32 kNodeEmissive = 1u << 2;    // something under here emits light
inline constexpr u32 kNodeTransmissive = 1u << 3;   // something under here lets light through

constexpr u32 node_level(const GpuNode& n) { return n.packed & 0xFFu; }
constexpr u32 node_flags(const GpuNode& n) { return (n.packed >> 8) & 0xFFu; }
constexpr u32 node_child_mask(const GpuNode& n) { return (n.packed >> 16) & 0xFFu; }
constexpr u32 pack_node(u32 level, u32 flags, u32 child_mask) {
    return (level & 0xFFu) | ((flags & 0xFFu) << 8) | ((child_mask & 0xFFu) << 16);
}

// Where a descent stopped, and why.
//
// Three answers, not two, and the difference between them is what streaming is made of:
//
//   empty_below   the child cell one level down holds nothing. A ray jumps the width of it, and
//                 that jump is the coarse skip — it falls out of the descent instead of needing
//                 five separate occupancy grids to carry it.
//   wanted        the world has something here and the pool does not. A ray reports it, which is
//                 the only reason anything is ever streamed.
//   otherwise     `slot` is the finest node covering the point, at `level`.
//
// Conflating the first two is the fault this whole structure exists to make unrepresentable: an
// unstreamed building that reads as open sky is never requested, so it stays open sky.
struct NodeFind {
    u32 slot = kNoNode;
    u32 level = 0;
    bool empty_below = false;
    bool wanted = false;
};

struct NodePoolBudget {
    u32 max_nodes = 1u << 20;                       // 1M nodes, 32 MB
    u64 payload_bytes = 512ull * 1024 * 1024;
    u32 max_occupancy_leaves = 1u << 18;            // 64 B each, 16 MB

    // Per-frame work caps. Streaming is allowed to fall behind — a node that does not arrive
    // this frame arrives next, and the renderer draws its parent in the meantime. What it is
    // never allowed to do is blow the frame budget (documentation/09 §2: 0.8 ms on a Deck).
    // Raised from 2048 once a request meant a whole path rather than one level of one. A frame
    // that builds more converges sooner, and the cap bounds a spike rather than rationing
    // steady-state work - which is nil, because a converged tree builds nothing at all.
    u32 max_builds_per_frame = 16384;
    u64 max_upload_bytes_per_frame = 8ull * 1024 * 1024;

    // How long a node may go unwanted before its slot can be taken, in frames. Several seconds
    // at any frame rate: a node just off screen has to survive turning round and back.
    u32 cold_frames = 600;

    // Held resident regardless of visibility, at full detail, because collision, physics and
    // editing have to touch what is behind you and under your feet (D199). Twenty metres.
    i64 proximity_voxels = 20 * 32;
};

// Which elements of one array changed this frame.
//
// A bitmap rather than a list of indices, because a single build touches the same node several
// times over -- once when it is allocated, again when its own children are folded into it, again
// when its parent folds it in -- and a list would send it once per touch. It also has to be
// cheap to mark, since a busy frame marks tens of thousands of times.
//
// Runs are coalesced with a gap tolerance on the way out: two dirty blocks with one clean block
// between them go as a single copy, because a copy has a fixed cost of its own and sending four
// spare kilobytes is cheaper than issuing a second one.
class DirtySet {
public:
    void create(u64 count) {
        count_ = count;
        words_.assign(static_cast<usize>((count + 63) / 64), 0ull);
        marked_ = 0;
    }
    void clear() {
        if (marked_ != 0) std::fill(words_.begin(), words_.end(), 0ull);
        marked_ = 0;
    }
    void mark(u64 index) {
        if (index >= count_) return;
        const u64 word = index >> 6;
        const u64 bit = 1ull << (index & 63);
        if ((words_[static_cast<usize>(word)] & bit) == 0) {
            words_[static_cast<usize>(word)] |= bit;
            ++marked_;
        }
    }
    void mark_range(u64 first, u64 count) {
        for (u64 i = 0; i < count; ++i) mark(first + i);
    }
    bool empty() const { return marked_ == 0; }
    u64 marked() const { return marked_; }

    // Coalesced runs of dirty elements, as (first, count). `gap` is how many clean elements may
    // sit inside a run before it is worth splitting.
    std::vector<std::pair<u64, u64>> runs(u64 gap) const {
        std::vector<std::pair<u64, u64>> out;
        u64 i = 0;
        while (i < count_) {
            if (!test(i)) { ++i; continue; }
            u64 first = i;
            u64 last = i;
            ++i;
            while (i < count_) {
                if (test(i)) { last = i; ++i; continue; }
                // Look ahead over the gap rather than ending the run immediately.
                u64 probe = i;
                bool found = false;
                while (probe < count_ && probe - last <= gap) {
                    if (test(probe)) { found = true; break; }
                    ++probe;
                }
                if (!found) break;
                last = probe;
                i = probe + 1;
            }
            out.emplace_back(first, last - first + 1);
        }
        return out;
    }

private:
    bool test(u64 index) const {
        return (words_[static_cast<usize>(index >> 6)] >> (index & 63)) & 1ull;
    }
    std::vector<u64> words_;
    u64 count_ = 0;
    u64 marked_ = 0;
};

// What changed this frame and must be copied to the GPU.
struct NodeUploadBatch {
    std::vector<u32> nodes;        // node slots whose record changed
    std::vector<u32> leaves;       // leaf slots whose occupancy changed
    std::vector<u32> payload_from; // payload byte ranges that changed
    std::vector<u32> payload_size;
    u64 payload_bytes = 0;
    u32 built = 0;
    u32 evicted = 0;
    u32 deferred = 0;              // wanted, but the frame's budget ran out
    bool out_of_memory = false;
    void clear();
};

struct NodePoolStats {
    u32 nodes = 0;
    u32 leaves = 0;
    u64 payload_in_use = 0;
    u64 payload_capacity = 0;
    u64 node_bytes = 0;
    u64 occupancy_bytes = 0;
    u64 total_bytes = 0;
    u64 builds = 0;
    u64 evictions = 0;
    u64 requests = 0;
    u64 hits = 0;
    f64 hit_rate() const {
        return (requests > 0) ? static_cast<f64>(hits) / static_cast<f64>(requests) : 0.0;
    }
};

class NodePool {
public:
    void create(const NodePoolBudget& budget, const VoxelTypeTable& types);

    // "A ray wanted this node and could not find it." The only reason anything is ever built.
    //
    // A node smaller than a pixel is never requested, because the descent stops at the pixel
    // footprint — so it is never fetched, never uploaded, and does not exist (D190).
    void request(const NodeKey& key);

    // A ray READ this node, so it is wanted whether or not anything is missing under it.
    //
    // Without this, "wanted" only ever meant "missing", and a finished tree stopped being wanted
    // by everything at once. See D247 and the note over the eviction loop.
    void touch(const NodeKey& key);

    // The same, for a node the marcher can name outright.
    //
    // A ray knows the SLOT it stopped on -- it descended to it -- so it reports that rather than a
    // key, and this is a store instead of a descent. Per node rather than per root, because
    // eviction at the root can only drop the scene or nothing (D260).
    void touch_slot(u32 slot);

    // "The copy you are holding is out of date." Pushed by whatever edited the world, and
    // remembered until it is served.
    //
    // This cannot be pulled. Feedback reports nodes the renderer wanted and *could not find*; a
    // node that is resident but stale is found, so it is never reported, so it is never
    // refreshed. Only the code that changed it knows. A one-frame request is not enough either:
    // the frame budget serves a bounded number and a large edit touches far more, so all but the
    // first few would be stranded, drawing pre-edit contents until something unrelated evicted
    // them (D131).
    void invalidate(i64 x, i64 y, i64 z);

    // Serves this frame's requests, holds the proximity radius, evicts what has gone cold, and
    // returns what the GPU layer must copy.
    const NodeUploadBatch& update(const World& world, const f64 camera_voxel[3], u64 frame);

    // The slot holding a node, or kNoNode. Walks the same path the shader walks: hash the entry
    // ancestor, then descend by octant.
    u32 find(const NodeKey& key) const;
    bool resident(const NodeKey& key) const { return find(key) != kNoNode; }

    // Where a descent stopped, and why — the three-way answer a ray needs.
    NodeFind locate(const NodeKey& key) const;

    // Reads one voxel the way the shader will — entry hash, descent, child mask, leaf payload —
    // so that "the GPU holds what the CPU holds" is a test rather than a hope. This is the
    // successor to mirror_voxel_world and is held to the same standard.
    VoxelTypeId mirror_voxel(i64 x, i64 y, i64 z) const;

    // How much of each array is in use, so the GPU copies a prefix rather than a capacity. An
    // empty world uploads nothing and the facility uploads a couple of megabytes, where copying
    // the whole pool would be 32 MB whatever it held.
    u32 node_watermark() const { return next_free_; }
    u32 leaf_watermark() const { return next_leaf_; }
    u64 payload_watermark() const { return payload_high_; }

    const std::vector<GpuNode>& nodes() const { return nodes_; }
    const std::vector<u64>& occupancy() const { return occupancy_; }
    const std::vector<GpuBrickHeader>& leaves() const { return leaves_; }
    const std::vector<u8>& payload() const { return payload_; }
    // The entry-level hash table, as the shader reads it: slot per bucket, kNoNode when empty.
    const std::vector<u32>& entries() const { return entries_; }

    // What changed since the last upload, so the GPU is sent that rather than every used byte.
    //
    // The prefixes above are what the first version copied, every frame anything changed at all.
    // That is fine while the tree is converged and quiet, and it is 10 MB a frame while somebody
    // is walking -- measured at 2.725 ms mean and 8.915 ms worst against a 0.80 ms budget, the
    // largest single cost in the frame and the reason the pool read as laggy while marching
    // faster than what it replaced.
    const DirtySet& dirty_nodes() const { return dirty_nodes_; }
    const DirtySet& dirty_leaves() const { return dirty_leaves_; }
    const DirtySet& dirty_entries() const { return dirty_entries_; }
    // Payload is bytes rather than records, and a block allocation is contiguous, so ranges are
    // recorded where they are written instead of through a bitmap over half a gigabyte.
    const std::vector<std::pair<u64, u64>>& dirty_payload() const { return dirty_payload_; }

    bool nothing_dirty() const {
        return dirty_nodes_.empty() && dirty_leaves_.empty() && dirty_entries_.empty() &&
               dirty_payload_.empty();
    }

    // Cleared by whoever uploads, and only once the copy is actually recorded. Held otherwise,
    // so a frame that ran out of staging retries rather than leaving the card a stale byte.
    void clear_dirty() {
        dirty_nodes_.clear();
        dirty_leaves_.clear();
        dirty_entries_.clear();
        dirty_payload_.clear();
    }

    NodePoolStats stats() const;
    bool validate() const;

private:
    struct Resident {
        u32 slot = kNoNode;
        u64 last_wanted = 0;
        u64 revision = 0;      // the chunk revision this copy was folded from
    };

    // Builds a node and everything under it, bounded by the frame's budget. Returns the slot, or
    // kNoNode — and kNoNode means one of two completely different things, which is why
    // `out_of_room_` exists beside it. "Nothing is here" and "I could not fit it" have to be told
    // apart: treating the second as the first is how a tree stops building and reports open sky,
    // and it is the same silence D133 and D147 are both about.
    // A node on its own: coordinates, level, and a child mask taken from the world. No children
    // and no colour — a shell gains its colour when it is folded from children that arrive later,
    // which is the only exact way to get one (D152).
    u32 build_shell(const World& world, const NodeKey& key, u32& budget);
    u32 build_leaf(const World& world, const NodeKey& key, u32& budget);

    // Walks from a root down to the level that was asked for, creating what is missing and
    // folding the chain back up. Bounded by that level, which is what makes the pool follow the
    // pixels instead of building everything: a node smaller than the pixel that asked for it is
    // never created (D190).
    u32 refine(const World& world, const NodeKey& key, u32 root_slot, u32& budget);
    void fold_children(u32 slot);
    // Frees what a node owns; the caller decides what happens to its own slot, because a
    // node inside a run is not separately allocated and must not be freed as though it were.
    void release_contents(u32 slot);
    void release(u32 slot);
    u32 allocate_node();
    u32 allocate_children();

    // Does the world hold anything at all under this node?
    //
    // Without it, building a node at the entry level descends into eight children
    // unconditionally, and 8^11 is 8.6 billion nodes for a world holding one brick. The first
    // version of the summary octree hit exactly this and never returned (D153); this one hit it
    // and was rescued by an allocator that started failing, which was worse, because failing
    // looked like empty.
    bool world_has(const World& world, const NodeKey& key) const;
    void index_world(const World& world);

    NodePoolBudget budget_;
    const VoxelTypeTable* types_ = nullptr;
    // The same segregated-fit pool residency uses for brick payloads, for the same
    // reason: a brick's encoded size varies from eight bytes to two kilobytes, and a
    // bump allocator that never reclaims cannot survive a camera that moves.
    BlockPool payload_pool_;

    std::vector<GpuNode> nodes_;
    std::vector<u64> occupancy_;     // kBrickWords per leaf
    std::vector<u8> payload_;
    std::vector<u32> entries_;       // open-addressed table over entry-level nodes

    // Leaves, in their own array indexed by a leaf id rather than by a node slot.
    //
    // Two things fall out of the separation and both matter. A leaf's header is the brick layout
    // the renderer has used since Stage 2 — payload offset, palette, index bits, the two
    // occupancy mips — so nothing about brick decoding has to be reinvented, and the ugly attempt
    // to fold it into spare coverage bits goes away. And a leaf keeps its id when its node is
    // moved into a parent's contiguous run, so moving a node copies thirty-two bytes rather than
    // its sixty-four bytes of occupancy as well.
    std::vector<GpuBrickHeader> leaves_;
    std::vector<u32> leaf_payload_size_;   // CPU only: what to hand back to the pool on eviction

    // Marked at every write into the arrays above. A missed mark is a stale byte on the GPU and
    // a wrong picture, which is exactly what NodeBuffers::audit exists to catch: it walks the
    // real buffers against these arrays and names the first byte that disagrees.
    u64 touch_frame_ = 0;   // the frame `touch` stamps, set by update()
    // When each node was last read by a ray. CPU-side and parallel to `nodes_`, rather than a
    // field in GpuNode: the record is thirty-two bytes, which is two nodes to a cache line, and
    // the GPU never reads this. Four bytes a node, so four megabytes at the default budget.
    std::vector<u32> node_last_read_;
    // Kept between frames so it is not reallocated every one. Cleared at the top of update().
    std::unordered_set<NodeKey, NodeKeyHash> seen_requests_;
    DirtySet dirty_nodes_;
    DirtySet dirty_leaves_;
    DirtySet dirty_entries_;
    std::vector<std::pair<u64, u64>> dirty_payload_;
    std::vector<u32> free_leaves_;
    u32 next_leaf_ = 0;
    // The highest payload byte in use, so the upload copies a prefix rather than the
    // whole pool. It only rises: a released block is reused before this moves, and
    // tracking it downward would mean walking the free lists to find the new maximum.
    u64 payload_high_ = 0;

    std::unordered_map<NodeKey, Resident, NodeKeyHash> live_;
    std::vector<NodeKey> requested_;
    std::vector<NodeKey> dirty_;

    // A bump pointer plus two free lists, and never a search for a contiguous run.
    //
    // The first version carved runs off the tail of a single free list and checked that the
    // eight it found happened to be consecutive. They are, exactly until something is freed —
    // after which the check fails, allocation returns "nothing", and the caller reads that as an
    // empty region. A tree that stops building because it ran out of memory must not look like a
    // tree that stopped because the world is empty.
    u32 next_free_ = 0;
    std::vector<u32> free_singles_;
    std::vector<u32> free_runs_;
    bool out_of_room_ = false;

    // Which blocks hold anything, at every level from the chunk up. Rebuilt when the set of
    // chunks changes — not when their contents change, which is what `dirty_` is for.
    std::unordered_set<NodeKey, NodeKeyHash> occupied_;
    u64 indexed_chunks_ = 0;

    NodeUploadBatch batch_;
    u64 builds_ = 0;
    u64 evictions_ = 0;
    u64 requests_ = 0;
    u64 hits_ = 0;
};

}  // namespace ws
