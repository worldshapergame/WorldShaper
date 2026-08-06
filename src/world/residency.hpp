#pragma once
// GPU residency: what is currently uploaded, what should be, and what to throw away —
// laid out so a shader can walk it without a CPU in the loop.
//
// documentation/03-voxel-data-model.md §8: streaming is demand-driven from a feedback
// buffer. The renderer reports which chunks it wanted, the streamer serves them, and LRU
// evicts the rest. Memory is bounded by what is on screen, not by world size.
//
// The GPU-side addressing scheme, which is what the ray marcher walks:
//
//   chunk grid    A wrapped 3D array of record ids, indexed by chunk coordinate modulo
//                 the grid size. One fetch turns a world position into a chunk record,
//                 with no hashing and no search. Records carry their own coordinate, so
//                 an aliased cell reads as empty rather than as the wrong chunk.
//   chunk record  Where this chunk's mask, prefix and brick slots live.
//   brick mask    32,768 bits, one per brick position in the chunk. The marcher's
//                 empty-space test: one 64-bit word skips 64 brick positions.
//   prefix        Running popcount per mask word, so a set bit converts to a brick's
//                 rank in O(1) — popcount of the partial word plus the stored prefix.
//   brick slots   A chunk's bricks occupy a *contiguous run* of slots, so rank is the
//                 offset from the run's base. Contiguity also keeps a chunk's occupancy
//                 words next to each other in memory, which is what the marcher reads
//                 most.
//
// Everything here is pure logic over a CPU-side mirror of the GPU buffers, so residency,
// eviction, packing and the consistency check are all testable headless before any of it
// touches Vulkan.

#include <unordered_map>
#include <vector>

#include "core/block_pool.hpp"
#include "core/types.hpp"
#include "world/gpu_brick.hpp"
#include "world/world.hpp"

namespace ws {

inline constexpr u32 kNoSlot = 0xFFFFFFFFu;
inline constexpr u32 kNoRecord = 0xFFFFFFFFu;

// The chunk's brick-occupancy pyramid, stored as one contiguous run of 64-bit words per
// chunk. Level 0 is the 32x32x32 brick grid; each level above halves the resolution and
// covers twice the distance, which is what lets a ray step in bigger jumps the further it
// gets from the camera (documentation/04-rendering.md §1).
//
//   level 0: 32^3 bricks   (8 m cells / 32,768 bits) = 512 words
//   level 1: 16^3          (16 m ... wait, 2-brick cells = 0.5 m) = 64 words
//   level 2: 8^3                                                  = 8 words
//   level 3: 4^3                                                  = 1 word
//   level 4: 2^3                                                  = 1 word
//
// Cell size in voxels at level k is 8 << k, so level 4 cells are 128 voxels and a whole
// chunk is two cells across.
inline constexpr u32 kChunkMipLevels = 5;
inline constexpr u32 kChunkMipWords[kChunkMipLevels] = {512, 64, 8, 1, 1};
inline constexpr u32 kChunkMipOffset[kChunkMipLevels] = {0, 512, 576, 584, 585};
inline constexpr u32 kChunkMaskWords = 586;

// Bricks per axis at a given mip level.
constexpr u32 chunk_mip_axis(u32 level) { return 32u >> level; }

// Above chunk level: coarse occupancy grids saying "is there anything at all in this block
// of chunks". Without them, a ray crossing empty sky steps one 8 m chunk at a time, which
// measured 4.1 ms of a 1440p frame spent finding nothing — the cost is in the stepping,
// not in the geometry.
//
// Five levels covering blocks of 1, 4, 16, 64 and 256 chunks: 8 m up to 2 km. Each level
// has its own wrapped grid of the same size rather than the fine grid divided down, so
// the coarser the level the further it reaches — level 4 covers ±64 km before a cell can
// alias. This is the upward octree carried forward from Stage 1, in the form the marcher
// actually needs.
//
// Level 0 is the important one for streaming: at factor 1 it answers "does the world have
// a chunk here at all", which is what lets the marcher tell *empty space* from *something
// you have not streamed yet*. Without it, a ray descending into an occupied block reports
// whichever empty-sky chunk it happened to enter first, the CPU discards it as
// non-existent, and the same useless report is made every frame forever.
inline constexpr u32 kCoarseLevels = 5;
inline constexpr u32 kCoarseFactor[kCoarseLevels] = {1, 4, 16, 64, 256};
inline constexpr u32 kCoarseDimX = 64;
inline constexpr u32 kCoarseDimY = 16;
inline constexpr u32 kCoarseDimZ = 64;
inline constexpr u32 kCoarseCellsPerLevel = kCoarseDimX * kCoarseDimY * kCoarseDimZ;

// Each coarse cell stores the block coordinate it holds plus a state, four u32 in all.
// The coordinate is not optional: a wrapped grid without it cannot tell whether the cell
// describes the block being asked about or one far away, so every query has to assume
// "occupied" and no skipping happens at all. That mistake measured 10.2 ms on a frame of
// open sky against 4.1 ms with no coarse grid whatsoever.
inline constexpr u32 kCoarseEmpty = 0;
inline constexpr u32 kCoarseOccupied = 1;
inline constexpr u32 kCoarseConflicted = 2;

// Brick position within a chunk, x fastest. Mask bit order and slot order are both this,
// so a brick's rank is a popcount away.
constexpr u32 brick_linear_index(u32 bx, u32 by, u32 bz) {
    return bx | (by << 5) | (bz << 10);
}

struct GpuChunkRecord {
    // Chunk coordinates, truncated to 32 bits. The shader works entirely in 32-bit
    // integers — no 64-bit maths in the inner loop — and these exist only to verify that
    // a wrapped grid cell really holds the chunk we asked for. A false match would need
    // two resident chunks exactly 2^32 chunks apart, which is 34 billion kilometres.
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
    u32 mask_offset = 0;     // in u64 words, into the mask buffer
    u32 prefix_offset = 0;   // in u32 entries, into the prefix buffer
    u32 slot_base = 0;       // first brick slot of this chunk's contiguous run
    u32 brick_count = 0;
    u32 average_colour = 0;  // rgba8, alpha is coverage — used when the chunk is tiny
};
static_assert(sizeof(GpuChunkRecord) == 32, "GpuChunkRecord must stay 32 bytes");

struct ResidencyBudget {
    u64 payload_bytes = 256ull * 1024 * 1024;
    u32 max_bricks = 262144;   // brick slots: headers and occupancy
    u32 max_chunks = 4096;     // chunk records, and their masks and prefixes

    // The wrapped chunk grid. 64 x 16 x 64 covers ±256 m horizontally and ±64 m
    // vertically before a cell can alias, which is far beyond any streaming radius.
    u32 grid_width = 64;
    u32 grid_height = 16;
    u32 grid_depth = 64;

    // Per-frame work caps. Streaming is allowed to fall behind — a chunk that does not
    // arrive this frame arrives next, and the renderer draws it at a coarser level in the
    // meantime. What it is never allowed to do is blow the frame budget
    // (documentation/09-performance-budgets.md §2: 0.8 ms on a Steam Deck). Without these
    // caps, one fast camera turn queues every visible chunk into a single frame.
    u32 max_chunk_uploads_per_frame = 4;
    u64 max_upload_bytes_per_frame = 8ull * 1024 * 1024;

    // Bricks encoded per frame, which is what cost actually scales with: a chunk can hold
    // anywhere from one brick to 32,768.
    //
    // This is a hard cap, not a threshold: a chunk too big to finish in one frame is
    // encoded across several and only becomes visible when it is complete. Before that,
    // the cap was checked before *starting* a chunk, so the real worst case was this plus
    // one whole chunk — 2.0 ms on the test scene against a 0.8 ms budget, and unbounded
    // in general.
    u32 max_bricks_per_frame = 1024;
};

// A contiguous run of a mirror buffer that changed and needs copying to the GPU.
struct UploadRange {
    u32 offset = 0;
    u32 size = 0;
};

// What changed this frame. Held by the manager and refilled in place, so the streaming
// path allocates nothing per frame.
struct UploadBatch {
    std::vector<u32> slots;              // brick slots whose header and occupancy changed
    std::vector<u32> records;            // chunk records (and their mask/prefix) that changed
    std::vector<u32> grid_cells;         // chunk-grid cells that changed
    std::vector<u32> coarse_cells;       // coarse-grid entries that changed (flat index)
    std::vector<UploadRange> payload;    // payload byte ranges that changed
    u64 payload_bytes = 0;
    u32 chunks_added = 0;
    u32 chunks_refreshed = 0;
    u32 chunks_evicted = 0;
    u32 bricks_added = 0;
    u32 chunks_deferred = 0;             // wanted, but the frame's upload budget ran out
    bool out_of_memory = false;          // could not fit even after evicting everything

    void clear();
};

struct ResidencyStats {
    u32 resident_chunks = 0;
    u32 resident_bricks = 0;
    u64 payload_in_use = 0;
    u64 payload_capacity = 0;
    u64 slot_run_in_use = 0;      // brick slots held, including run rounding
    u64 header_bytes = 0;
    u64 occupancy_bytes = 0;
    u64 index_bytes = 0;          // records, masks, prefixes, grid
    u64 total_bytes = 0;
    u64 uploads = 0;
    u64 evictions = 0;
    u64 requests = 0;
    u64 hits = 0;
    f64 hit_rate() const {
        return (requests > 0) ? static_cast<f64>(hits) / static_cast<f64>(requests) : 0.0;
    }
};

class VoxelTypeTable;

class ResidencyManager {
public:
    // The type table is needed when packing: filtered colours for distant nodes come from
    // it. It must outlive the manager.
    void create(const ResidencyBudget& budget, const VoxelTypeTable& types);

    // Called by the renderer's feedback pass: "I wanted this chunk this frame."
    void request(const ChunkCoord& coord);

    // Resolves the frame's requests: uploads what is missing or stale, evicts the least
    // recently used chunks to make room, and returns what the GPU layer must copy.
    const UploadBatch& update(const World& world, u64 frame);

    bool resident(const ChunkCoord& coord) const;
    u64 resident_chunk_count() const { return chunks_.size(); }

    // Decodes the mirror back to voxels and hashes it exactly as Chunk::content_hash
    // does. Equal hashes mean the GPU holds precisely what the CPU holds.
    u64 mirror_hash(const ChunkCoord& coord) const;

    // Reads one voxel out of the mirror the way the shader will: grid lookup, record,
    // mask bit, rank, occupancy, payload. If this is wrong, the picture is wrong.
    VoxelTypeId mirror_voxel(const ChunkCoord& coord, u32 x, u32 y, u32 z) const;

    // Same walk, but starting from a world voxel coordinate and going through the wrapped
    // grid — exactly what the ray marcher does.
    VoxelTypeId mirror_voxel_world(i64 x, i64 y, i64 z) const;

    const std::vector<GpuBrickHeader>& headers() const { return headers_; }
    const std::vector<u64>& occupancy() const { return occupancy_; }
    const std::vector<u8>& payload() const { return payload_; }
    const std::vector<GpuChunkRecord>& records() const { return records_; }
    const std::vector<u64>& masks() const { return masks_; }
    const std::vector<u32>& prefixes() const { return prefixes_; }
    const std::vector<u32>& grid() const { return grid_; }
    // Both coarse levels, concatenated: level 0 first, then level 1.
    const std::vector<u32>& coarse() const { return coarse_flags_; }
    const ResidencyBudget& budget() const { return budget_; }

    // Bounding box of everything currently resident, in chunk coordinates, inclusive.
    // The marcher clips rays to it: a ray that leaves the box cannot hit anything, and
    // without the clip it keeps stepping through empty blocks looking for something that
    // is not there. Returns false when nothing is resident.
    bool resident_bounds(ChunkCoord& min, ChunkCoord& max) const;

    u32 grid_index(i64 chunk_x, i64 chunk_y, i64 chunk_z) const;
    u32 coarse_index(u32 level, i64 chunk_x, i64 chunk_y, i64 chunk_z) const;
    u32 coarse_offset(u32 level) const;

    ResidencyStats stats() const;
    bool validate() const;

private:
    struct ResidentBrick {
        u32 linear = 0;            // brick_linear_index within the chunk
        u32 payload_offset = kNoOffset;
        u32 payload_size = 0;      // as requested, for release
    };

    struct ResidentChunk {
        std::vector<ResidentBrick> bricks;   // sorted by linear, so index == rank
        u32 record = kNoRecord;
        u32 slot_base = 0;
        u32 slot_run = 0;          // entries requested; released with the same value
        u64 last_used = 0;
        u64 revision = 0;          // the chunk revision this copy was packed from
    };

    // A chunk part-way through being encoded. It holds its slot run and record from the
    // start — so nothing else can take them — but is not published to the chunk grid, and
    // so is invisible to the marcher, until every brick is in place. A half-uploaded chunk
    // that the marcher could see would read whatever was in those slots before.
    struct PendingUpload {
        ChunkCoord coord;
        u32 record = kNoRecord;
        u32 slot_base = 0;
        u32 brick_count = 0;
        u32 next_linear = 0;   // resume point, as a brick linear index
        u64 revision = 0;
        std::vector<ResidentBrick> bricks;
        bool active = false;
    };

    enum class UploadStep { Complete, Partial, Failed };

    UploadStep advance_upload(const World& world, u64 frame, u32 brick_budget);
    void abandon_pending();
    void finish_pending(u64 frame);

    void coarse_add(const ChunkCoord& coord, i32 delta);

public:
    // Rebuilds the coarse occupancy grids from the *world*, not from what is resident.
    //
    // This distinction is the whole feedback loop. If the coarse grids describe residency,
    // then a region that has never been streamed reads as empty, the marcher skips it, and
    // it is therefore never requested — streaming deadlocks and nothing past the initial
    // seed is ever loaded. Describing the world instead means "there is something here,
    // and you do not have it", which is exactly the question feedback needs answered.
    //
    // Rebuilt wholesale for now. Incremental maintenance arrives with terrain, when chunks
    // start appearing and disappearing during play.
    void rebuild_coarse(const World& world);

private:
    // Begins encoding a chunk. Returns Complete if it fitted in the budget, Partial if it
    // will continue next frame, Failed if there was no room even after evicting.
    UploadStep begin_upload(const World& world, const ChunkCoord& coord, u64 frame,
                            u32 brick_budget);
    void evict_chunk(const ChunkCoord& coord);
    bool evict_one_lru(u64 frame);
    u32 allocate_record();
    void release_record(u32 record);
    const ResidentBrick* find_brick(const ResidentChunk& chunk, u32 linear) const;
    u32 brick_rank(const ResidentChunk& chunk, u32 linear) const;   // kNoSlot when absent

    ResidencyBudget budget_;
    const VoxelTypeTable* types_ = nullptr;
    BlockPool payload_pool_;
    BlockPool slot_pool_;      // brick slot runs, measured in entries

    // The CPU mirror of the GPU buffers.
    std::vector<GpuBrickHeader> headers_;
    std::vector<u64> occupancy_;
    std::vector<u8> payload_;
    std::vector<GpuChunkRecord> records_;
    std::vector<u64> masks_;
    std::vector<u32> prefixes_;
    std::vector<u32> grid_;
    std::vector<u32> coarse_counts_;   // resident chunks per coarse cell, both levels
    std::vector<u32> coarse_flags_;    // 1 where the count is non-zero; this is the GPU copy

    bool coarse_dirty_ = false;
    std::vector<u32> free_records_;
    u32 record_cursor_ = 0;

    std::unordered_map<ChunkCoord, ResidentChunk, ChunkCoordHash> chunks_;
    PendingUpload pending_;
    std::vector<ChunkCoord> requested_;
    UploadBatch batch_;

    std::vector<u8> scratch_payload_;

    u64 uploads_ = 0;
    u64 evictions_ = 0;
    u64 requests_ = 0;
    u64 hits_ = 0;
};

}  // namespace ws
