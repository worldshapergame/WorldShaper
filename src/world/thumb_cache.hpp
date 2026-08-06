#pragma once
// Which chunks have a thumbnail on the GPU, and where.
//
// This is a second residency tier, sitting under the full-detail one in world/residency.hpp,
// and it is deliberately built the other way round.
//
// Full residency is *pull*: the renderer reports chunks it wanted and could not find, and
// streaming serves them. That is right for detail — it follows the view, so a chunk behind a
// wall costs nothing — but it can only ever ask for what a ray reached, and it took a stall
// that froze the world to make the weakness obvious.
//
// Thumbnails are *push*: the CPU knows the whole world and where the camera is, so it simply
// keeps thumbnails for the nearest chunks within a radius, closest first. No feedback, no
// round trip, nothing to deadlock. That is affordable precisely because a thumbnail is two
// kilobytes: being approximate about which ones to hold costs almost nothing, whereas being
// approximate about full chunks would cost hundreds of megabytes.
//
// The grid is wrapped rather than camera-anchored, so moving does not rebuild it — a cell
// carries the chunk it holds and a lookup that disagrees reads as "no thumbnail", which is
// safe. Unlike the world-occupancy grid this cannot invent anything: the worst a collision
// does here is decline to draw something, never ask for something that is not there.

#include <unordered_map>
#include <vector>

#include "core/types.hpp"
#include "world/thumbnail.hpp"
#include "world/world.hpp"

namespace ws {

class VoxelTypeTable;

inline constexpr u32 kNoThumb = 0xFFFFFFFFu;

struct ThumbnailBudget {
    u32 max_thumbs = 32768;      // 2 KB each, so 64 MB

    // Wrapped grid of slot indices. 256 x 64 x 256 chunks reaches ±1 km horizontally and
    // ±256 m vertically before a cell can alias, which is past any radius the slot budget
    // can fill.
    u32 grid_width = 256;
    u32 grid_height = 64;
    u32 grid_depth = 256;

    i64 radius_chunks = 160;     // 1.28 km

    // Building one visits every brick of a chunk, so this is a real per-frame cost and is
    // capped like every other streaming budget. Thumbnails are held until the chunk changes,
    // so this is a fill rate, not a per-frame load.
    u32 max_builds_per_frame = 6;

    // How far the camera may drift before the work list is rebuilt. The list is in distance
    // order, and the order only matters to within a few chunks.
    i64 rescan_margin = 8;
};

// Per slot, so a wrapped grid cell can be checked against the chunk it was asked about.
struct GpuThumbRecord {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
    u32 used = 0;   // 0 when the slot holds nothing
};
static_assert(sizeof(GpuThumbRecord) == 16, "GpuThumbRecord must stay 16 bytes");

struct ThumbnailBatch {
    std::vector<u32> slots;        // thumbnail slots whose cells and record changed
    std::vector<u32> grid_cells;   // grid entries that changed
    u32 built = 0;
    u32 evicted = 0;
    u32 wanted = 0;                // in radius and either missing or stale
    void clear();
};

class ThumbnailCache {
public:
    void create(const ThumbnailBudget& budget, const VoxelTypeTable& types);

    // Tops up thumbnails around `centre`, nearest first, and reports what to copy.
    const ThumbnailBatch& update(const World& world, const ChunkCoord& centre, u64 frame);

    // Drops a chunk's thumbnail so it is rebuilt. Edits go through here; a thumbnail is a
    // summary of contents, so contents changing makes it wrong.
    void invalidate(const ChunkCoord& coord);

    bool resident(const ChunkCoord& coord) const;
    u32 resident_count() const { return static_cast<u32>(slot_of_.size()); }

    const std::vector<u32>& grid() const { return grid_; }
    const std::vector<GpuThumbRecord>& records() const { return records_; }
    const std::vector<u32>& cells() const { return cells_; }
    const ThumbnailBudget& budget() const { return budget_; }

    u32 grid_index(i64 chunk_x, i64 chunk_y, i64 chunk_z) const;

    // Reads a cell back the way the shader will: wrapped grid, record check, cell fetch.
    // Returns 0 when there is no thumbnail for that chunk, which is also what an empty cell
    // reads as — the shader cannot tell the difference and does not need to.
    u32 sample(const ChunkCoord& coord, u32 x, u32 y, u32 z) const;

    bool validate() const;

private:
    struct Held {
        u32 slot = kNoThumb;
        u64 revision = 0;
    };

    void rescan(const World& world, const ChunkCoord& centre);
    u32 acquire_slot(i64 distance_sq, const ChunkCoord& centre);
    void release(const ChunkCoord& coord);

    ThumbnailBudget budget_;
    const VoxelTypeTable* types_ = nullptr;

    std::vector<u32> grid_;
    std::vector<GpuThumbRecord> records_;
    std::vector<u32> cells_;          // kThumbCells per slot
    std::vector<u32> free_slots_;
    u32 slot_cursor_ = 0;

    std::unordered_map<ChunkCoord, Held, ChunkCoordHash> slot_of_;

    // The work list: chunks in radius, nearest first. Rebuilt when the camera drifts.
    std::vector<std::pair<i64, ChunkCoord>> wanted_;
    ChunkCoord scanned_at_{};
    bool scanned_ = false;

    ThumbnailBatch batch_;
};

}  // namespace ws
