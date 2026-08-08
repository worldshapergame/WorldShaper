#pragma once
// The GPU side of streaming: three device-local buffers and the staging ring that feeds
// them.
//
// ResidencyManager decides *what* should be resident and packs it into a CPU mirror
// (world/residency.hpp). This class does nothing but copy the ranges it reports, which is
// why almost all of the streaming logic is testable headless and only this thin layer
// needs a GPU to exercise.
//
// Budget: documentation/09-performance-budgets.md §2 allows 0.8 ms and 120 MB of traffic
// per frame for streaming on a Steam Deck. Both are measured by the profiler scope the
// caller wraps around upload().

#include <vector>

#include "gpu/buffer.hpp"
#include "gpu/swapchain.hpp"
#include "world/residency.hpp"
#include "world/thumb_cache.hpp"

namespace ws {

struct WorldBufferStats {
    u64 staging_capacity = 0;
    u64 staged_bytes = 0;       // this frame
    u32 copy_regions = 0;       // after coalescing
    u32 raw_regions = 0;        // before coalescing
    u64 deferred_bytes = 0;
    // True when the world occupancy grid did not fit this frame. Residency has to be told,
    // or it clears its dirty flag over an update that never arrived.
    bool coarse_incomplete = false;     // did not fit this frame; retried next
    // Brick data that did not fit. When this is set the chunk records and grid cells are HELD BACK
    // rather than sent, because a grid cell is a pointer and sending it without what it points at
    // is what draws a chunk as somebody else's geometry.
    bool chunks_incomplete = false;
    u64 total_uploaded = 0;
};

class WorldBuffers {
public:
    // $thumb_slots and $thumb_grid_cells are totals across every summary level: they all
    // share one slot buffer and one grid buffer, each level at its own base offset.
    bool create(Device& device, const ResidencyBudget& budget, u32 thumb_slots,
                u32 thumb_grid_cells, u64 staging_bytes);
    void destroy();

    // Copies whatever the thumbnail cache says changed. Separate from upload() because the
    // two tiers are filled by different mechanisms — one follows the view, the other follows
    // the camera — but it shares the same staging ring, so it must be called after upload()
    // within a frame and takes whatever room is left.
    // Returns false when it did not all fit, in which case the caller must offer the same
    // batch again — a copy that is dropped and forgotten is how a chunk ends up drawing
    // something that is not there.
    bool upload_thumbnails(VkCommandBuffer cmd, const ThumbnailCache& cache,
                           const ThumbnailBatch& batch);

    // Records the copies for everything the batch says changed. Must be called inside a
    // command buffer, before anything reads the buffers.
    //
    // If the staging ring cannot hold the whole batch, the remainder is deferred to the
    // next frame rather than growing the ring — a frame that uploads slightly less is a
    // frame that still hits its budget, and a reallocation mid-play is a hitch.
    void upload(VkCommandBuffer cmd, const ResidencyManager& residency,
                const UploadBatch& batch, u32 frame_index);

    // Voxel type and visual tables. They only ever grow, so this copies the new tail when
    // the counts change and does nothing otherwise. Must run inside the same command
    // buffer, before anything shades a voxel.
    void upload_tables(VkCommandBuffer cmd, const VoxelTypeTable& types);

    VkBuffer headers() const { return headers_.buffer; }
    VkBuffer occupancy() const { return occupancy_.buffer; }
    VkBuffer payload() const { return payload_.buffer; }
    VkBuffer records() const { return records_.buffer; }
    VkBuffer masks() const { return masks_.buffer; }
    VkBuffer prefixes() const { return prefixes_.buffer; }
    VkBuffer grid() const { return grid_.buffer; }
    VkBuffer coarse() const { return coarse_.buffer; }
    VkBuffer thumb_grid() const { return thumb_grid_.buffer; }
    VkBuffer thumbs() const { return thumbs_.buffer; }
    VkBuffer types() const { return types_.buffer; }
    VkBuffer visuals() const { return visuals_.buffer; }

    const WorldBufferStats& stats() const { return stats_; }
    u64 device_bytes() const;

private:
    struct Region {
        u64 src = 0;   // offset in the staging ring
        u64 dst = 0;   // offset in the destination buffer
        u64 size = 0;
    };

    // Appends `size` bytes from `source` to the staging ring, returning false when the
    // ring is full for this frame.
    bool stage(const void* source, u64 size, u64 destination, std::vector<Region>& into);

    // Records and grid cells whose brick data did not fit in an earlier frame. Carried forward and
    // sent once it has, so the two never separate.
    std::vector<u32> held_records_;
    std::vector<u32> held_cells_;
    void flush(VkCommandBuffer cmd, VkBuffer destination, const std::vector<Region>& regions);

    Device* device_ = nullptr;
    GpuBuffer headers_;
    GpuBuffer occupancy_;
    GpuBuffer payload_;
    GpuBuffer records_;
    GpuBuffer masks_;
    GpuBuffer prefixes_;
    GpuBuffer grid_;
    GpuBuffer coarse_;
    // One buffer for both halves of a thumbnail, interleaved: per slot, four u32 of record
    // (the chunk it holds) then 512 cells. One binding instead of two, and the record a
    // lookup needs to check sits in the same cache line as the cells it guards.
    GpuBuffer thumb_grid_;
    GpuBuffer thumbs_;
    GpuBuffer types_;
    GpuBuffer visuals_;
    GpuBuffer staging_;

    u32 uploaded_types_ = 0;
    u32 uploaded_visuals_ = 0;
    // How many voxel types and visual records the GPU will hold.
    //
    // A quarter of a million was ample while a world used a palette of a few dozen materials.
    // It stopped being ample the moment clips started giving every voxel its own version of its
    // material — a surface with no two square centimetres alike is what makes a voxel wall stop
    // reading as a voxel wall, and it costs one record per voxel to do properly. A single
    // weathered block of a million voxels asked for six hundred thousand records and took the
    // renderer down with an assertion.
    //
    // Two million, then: 32 MB of visuals and 16 MB of types, which is small beside the
    // 460 MB payload buffer next to it and enough for a two-million-voxel clip to be entirely
    // unique. Past that the variation pass reuses records rather than making new ones, so the
    // limit is a quality ceiling and never a crash.
    static constexpr u32 kMaxTables = 2097152;

    u64 staging_capacity_per_frame_ = 0;
    u64 staging_cursor_ = 0;
    u64 staging_frame_base_ = 0;

    std::vector<Region> header_regions_;
    std::vector<Region> occupancy_regions_;
    std::vector<Region> payload_regions_;
    std::vector<Region> record_regions_;
    std::vector<Region> mask_regions_;
    std::vector<Region> prefix_regions_;
    std::vector<Region> grid_regions_;
    std::vector<Region> coarse_regions_;
    std::vector<Region> thumb_grid_regions_;
    std::vector<Region> thumb_regions_;
    std::vector<VkBufferCopy> copies_;
    std::vector<u32> sorted_slots_;

    WorldBufferStats stats_;
};

}  // namespace ws
