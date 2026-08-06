#include "gpu/world_buffers.hpp"

#include <algorithm>
#include <cstring>

#include "gpu/render_params.hpp"

namespace ws {

static_assert(kRenderSummaryTiers == kSummaryTiers,
              "the parameter block and the summary levels must agree");

bool WorldBuffers::create(Device& device, const ResidencyBudget& budget, u32 thumb_slots,
                          u32 thumb_grid_cells, u64 staging_bytes) {
    device_ = &device;

    const u64 header_bytes = static_cast<u64>(budget.max_bricks) * sizeof(GpuBrickHeader);
    const u64 occupancy_bytes =
        static_cast<u64>(budget.max_bricks) * kBrickWords * sizeof(u64);

    const u64 record_bytes = static_cast<u64>(budget.max_chunks) * sizeof(GpuChunkRecord);
    const u64 mask_bytes = static_cast<u64>(budget.max_chunks) * kChunkMaskWords * sizeof(u64);
    const u64 prefix_bytes = static_cast<u64>(budget.max_chunks) * kChunkMaskWords * sizeof(u32);
    const u64 grid_bytes = static_cast<u64>(budget.grid_width) * budget.grid_height *
                           budget.grid_depth * sizeof(u32);

    const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    headers_ = create_device_buffer(device, header_bytes, storage, "brick headers");
    occupancy_ = create_device_buffer(device, occupancy_bytes, storage, "brick occupancy");
    payload_ = create_device_buffer(device, budget.payload_bytes, storage, "brick payload");
    records_ = create_device_buffer(device, record_bytes, storage, "chunk records");
    masks_ = create_device_buffer(device, mask_bytes, storage, "chunk brick masks");
    prefixes_ = create_device_buffer(device, prefix_bytes, storage, "chunk brick prefixes");
    grid_ = create_device_buffer(device, grid_bytes, storage, "chunk grid");
    // Four u32 per cell (block coordinate plus state), every level, sized from the
    // constants rather than a literal — a literal is how this got left at 1 MB when the
    // grids grew to 5, which the GPU reported as a lost device.
    const u64 coarse_bytes =
        static_cast<u64>(kCoarseLevels) * kCoarseCellsPerLevel * 4 * sizeof(u32);
    coarse_ = create_device_buffer(device, coarse_bytes, storage, "world occupancy");

    const u64 thumb_grid_bytes = static_cast<u64>(thumb_grid_cells) * sizeof(u32);
    const u64 thumb_bytes = static_cast<u64>(thumb_slots) * kThumbSlotWords * sizeof(u32);
    thumb_grid_ = create_device_buffer(device, thumb_grid_bytes, storage, "thumbnail grid");
    thumbs_ = create_device_buffer(device, thumb_bytes, storage, "thumbnails");
    types_ = create_device_buffer(device, static_cast<u64>(kMaxTables) * 8, storage,
                                  "voxel types");
    visuals_ = create_device_buffer(device, static_cast<u64>(kMaxTables) * 16, storage,
                                    "visual records");

    // One staging region per frame in flight, so writing next frame's data cannot
    // overwrite bytes the GPU is still reading from this frame's copies.
    staging_capacity_per_frame_ = staging_bytes;
    staging_ = create_staging_buffer(device, staging_bytes * kFramesInFlight, "streaming staging");

    stats_ = WorldBufferStats{};
    stats_.staging_capacity = staging_capacity_per_frame_;

    WS_LOG_INFO("gpu",
                "world buffers: headers {} MB, occupancy {} MB, payload {} MB, "
                "index {} MB, staging {} MB",
                header_bytes / (1024 * 1024), occupancy_bytes / (1024 * 1024),
                budget.payload_bytes / (1024 * 1024),
                (record_bytes + mask_bytes + prefix_bytes + grid_bytes) / (1024 * 1024),
                staging_.size / (1024 * 1024));
    return true;
}

void WorldBuffers::upload_tables(VkCommandBuffer cmd, const VoxelTypeTable& table) {
    const u32 type_count = table.type_count();
    const u32 visual_count = table.visual_count();
    if (type_count == uploaded_types_ && visual_count == uploaded_visuals_) return;

    WS_CHECK(type_count <= kMaxTables && visual_count <= kMaxTables,
             "voxel type table outgrew its GPU buffer ({} types, {} visuals)", type_count,
             visual_count);

    // Small enough to build in a scratch vector and copy in one go; these change only
    // when new materials appear, which is a handful of times per session.
    std::vector<u32> type_pairs(static_cast<usize>(type_count) * 2);
    for (u32 i = 0; i < type_count; ++i) {
        type_pairs[i * 2] = table.type(i).visual;
        type_pairs[i * 2 + 1] = table.type(i).behaviour;
    }
    std::vector<VisualRecord> visual_records(visual_count);
    for (u32 i = 0; i < visual_count; ++i) visual_records[i] = table.visual(i);

    std::vector<Region> type_regions;
    std::vector<Region> visual_regions;
    if (!stage(type_pairs.data(), type_pairs.size() * sizeof(u32), 0, type_regions)) return;
    if (!stage(visual_records.data(), visual_records.size() * sizeof(VisualRecord), 0,
               visual_regions)) {
        return;
    }
    flush(cmd, types_.buffer, type_regions);
    flush(cmd, visuals_.buffer, visual_regions);

    uploaded_types_ = type_count;
    uploaded_visuals_ = visual_count;
    WS_LOG_DEBUG("gpu", "uploaded {} voxel types, {} visual records", type_count,
                 visual_count);
}

void WorldBuffers::destroy() {
    if (device_ == nullptr) return;
    destroy_buffer(*device_, staging_);
    destroy_buffer(*device_, visuals_);
    destroy_buffer(*device_, types_);
    destroy_buffer(*device_, thumbs_);
    destroy_buffer(*device_, thumb_grid_);
    destroy_buffer(*device_, coarse_);
    destroy_buffer(*device_, grid_);
    destroy_buffer(*device_, prefixes_);
    destroy_buffer(*device_, masks_);
    destroy_buffer(*device_, records_);
    destroy_buffer(*device_, payload_);
    destroy_buffer(*device_, occupancy_);
    destroy_buffer(*device_, headers_);
    device_ = nullptr;
}

u64 WorldBuffers::device_bytes() const {
    return headers_.size + occupancy_.size + payload_.size + records_.size + masks_.size +
           prefixes_.size + grid_.size + coarse_.size + thumb_grid_.size + thumbs_.size +
           types_.size + visuals_.size;
}

bool WorldBuffers::stage(const void* source, u64 size, u64 destination,
                         std::vector<Region>& into) {
    if (size == 0) return true;
    if (staging_cursor_ + size > staging_capacity_per_frame_) {
        stats_.deferred_bytes += size;
        return false;
    }

    const u64 offset = staging_frame_base_ + staging_cursor_;
    std::memcpy(static_cast<u8*>(staging_.mapped) + offset, source, size);
    into.push_back(Region{offset, destination, size});
    staging_cursor_ += size;
    stats_.staged_bytes += size;
    return true;
}

void WorldBuffers::flush(VkCommandBuffer cmd, VkBuffer destination,
                         const std::vector<Region>& regions) {
    if (regions.empty()) return;

    copies_.clear();
    copies_.reserve(regions.size());

    // Coalesce runs that are contiguous in both buffers. Slot updates arrive in ascending
    // order after sorting, so a screenful of newly streamed bricks usually collapses into
    // a handful of copies instead of thousands.
    for (const Region& region : regions) {
        if (!copies_.empty()) {
            VkBufferCopy& last = copies_.back();
            if (last.srcOffset + last.size == region.src &&
                last.dstOffset + last.size == region.dst) {
                last.size += region.size;
                continue;
            }
        }
        VkBufferCopy copy{};
        copy.srcOffset = region.src;
        copy.dstOffset = region.dst;
        copy.size = region.size;
        copies_.push_back(copy);
    }

    stats_.copy_regions += static_cast<u32>(copies_.size());
    vkCmdCopyBuffer(cmd, staging_.buffer, destination, static_cast<u32>(copies_.size()),
                    copies_.data());
}

bool WorldBuffers::upload_thumbnails(VkCommandBuffer cmd, const ThumbnailCache& cache,
                                     const ThumbnailBatch& batch) {
    if (batch.slots.empty() && batch.grid_cells.empty()) return true;

    thumb_grid_regions_.clear();
    thumb_regions_.clear();

    // Contents before the grid, and it matters which way round.
    //
    // The grid is what makes a slot findable. Send a grid entry whose slot contents did not
    // fit this frame and the marcher draws whatever that slot held before — some other
    // chunk, somewhere else. Send the contents and lose the grid entry and it draws nothing
    // for a frame. Only one of those is a wrong picture.
    const std::vector<GpuThumbRecord>& records = cache.records();
    const std::vector<u32>& cells = cache.cells();

    // Every level writes into the same two buffers at its own base, so a slot index is local
    // to its level and a grid cell is local to its level's grid.
    const u64 slot_base = cache.budget().slot_base;
    const u64 grid_base = cache.budget().grid_offset;

    bool complete = true;
    for (u32 slot : batch.slots) {
        const u64 base = (slot_base + slot) * kThumbSlotWords * sizeof(u32);
        if (!stage(&records[slot], sizeof(GpuThumbRecord), base, thumb_regions_) ||
            !stage(&cells[static_cast<usize>(slot) * kThumbCells], kThumbCells * sizeof(u32),
                   base + kThumbRecordWords * sizeof(u32), thumb_regions_)) {
            complete = false;
            break;
        }
    }

    // If any contents were left behind, the grid waits too — pointing at half of them is
    // exactly the wrong picture described above. The caller re-offers the whole batch.
    if (complete) {
        const std::vector<u32>& grid = cache.grid();
        for (u32 cell : batch.grid_cells) {
            if (!stage(&grid[cell], sizeof(u32), (grid_base + cell) * sizeof(u32),
                       thumb_grid_regions_)) {
                complete = false;
                break;
            }
        }
    }

    flush(cmd, thumbs_.buffer, thumb_regions_);
    flush(cmd, thumb_grid_.buffer, thumb_grid_regions_);

    // Its own barrier, and it needs one. upload() issues a barrier at the end of its own
    // copies — and skips it entirely when it staged nothing, which is exactly the case here:
    // a camera outside the full-detail window has no chunks to stream and everything on
    // screen is a thumbnail. Copies recorded after that barrier are unsynchronised with the
    // dispatch that reads them, so the marcher reads whatever was in the buffer before.
    if (thumb_regions_.empty() && thumb_grid_regions_.empty()) return complete;

    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
    return complete;
}

void WorldBuffers::upload(VkCommandBuffer cmd, const ResidencyManager& residency,
                          const UploadBatch& batch, u32 frame_index) {
    staging_frame_base_ = static_cast<u64>(frame_index % kFramesInFlight) *
                          staging_capacity_per_frame_;
    staging_cursor_ = 0;
    stats_.staged_bytes = 0;
    stats_.copy_regions = 0;
    stats_.deferred_bytes = 0;
    stats_.raw_regions = static_cast<u32>(batch.slots.size() * 2 + batch.payload.size());

    stats_.coarse_incomplete = false;

    header_regions_.clear();
    occupancy_regions_.clear();
    payload_regions_.clear();
    record_regions_.clear();
    mask_regions_.clear();
    prefix_regions_.clear();
    grid_regions_.clear();
    coarse_regions_.clear();

    // The world occupancy grid goes first, before any brick data.
    //
    // It used to go last, and that was a bug with a very confusing face. Staging silently
    // gives up when the frame's staging buffer is full, so after a large edit the brick data
    // filled it and the occupancy update was dropped — while the flag saying it needed
    // sending had already been cleared. The grid on the GPU then said "no chunk here" for
    // regions that had just been filled, so the marcher never asked for them, so they never
    // streamed, so there was a hole. Editing inside the hole marked the grid dirty again on
    // a quieter frame and the region appeared, which made it look like a rendering glitch
    // rather than a dropped upload.
    //
    // Order fixes the common case: the grid is small and everything else depends on it being
    // right, whereas a brick that misses this frame merely arrives in the next one. The
    // reporting below fixes the rest — if even this does not fit, residency is told to try
    // again rather than assuming it was sent.
    const std::vector<u32>& coarse = residency.coarse();
    std::vector<u32> sorted_coarse = batch.coarse_cells;
    std::sort(sorted_coarse.begin(), sorted_coarse.end());
    sorted_coarse.erase(std::unique(sorted_coarse.begin(), sorted_coarse.end()),
                        sorted_coarse.end());
    for (u32 cell : sorted_coarse) {
        if (!stage(&coarse[static_cast<usize>(cell) * 4], sizeof(u32) * 4,
                   static_cast<u64>(cell) * sizeof(u32) * 4, coarse_regions_)) {
            stats_.coarse_incomplete = true;
            break;
        }
    }

    // Ascending slot order is what makes coalescing work at all.
    sorted_slots_ = batch.slots;
    std::sort(sorted_slots_.begin(), sorted_slots_.end());
    sorted_slots_.erase(std::unique(sorted_slots_.begin(), sorted_slots_.end()),
                        sorted_slots_.end());

    const std::vector<GpuBrickHeader>& headers = residency.headers();
    const std::vector<u64>& occupancy = residency.occupancy();
    const std::vector<u8>& payload = residency.payload();

    for (u32 slot : sorted_slots_) {
        if (!stage(&headers[slot], sizeof(GpuBrickHeader),
                   static_cast<u64>(slot) * sizeof(GpuBrickHeader), header_regions_)) {
            break;
        }
        if (!stage(occupancy.data() + static_cast<usize>(slot) * kBrickWords,
                   kBrickWords * sizeof(u64),
                   static_cast<u64>(slot) * kBrickWords * sizeof(u64), occupancy_regions_)) {
            break;
        }
    }

    // Payload ranges come out of the residency manager in allocation order; sorting them
    // lets neighbouring allocations merge into one copy.
    std::vector<UploadRange> ranges = batch.payload;
    std::sort(ranges.begin(), ranges.end(),
              [](const UploadRange& a, const UploadRange& b) { return a.offset < b.offset; });

    for (const UploadRange& range : ranges) {
        if (!stage(payload.data() + range.offset, range.size, range.offset, payload_regions_)) {
            break;
        }
    }

    // Chunk records, their masks and prefixes, and the grid cells that now point
    // somewhere else. These are small — a handful of chunks per frame — but they are what
    // makes the brick data reachable at all, so they must land in the same batch.
    const std::vector<GpuChunkRecord>& records = residency.records();
    const std::vector<u64>& masks = residency.masks();
    const std::vector<u32>& prefixes = residency.prefixes();
    const std::vector<u32>& grid = residency.grid();

    std::vector<u32> sorted_records = batch.records;
    std::sort(sorted_records.begin(), sorted_records.end());
    for (u32 record : sorted_records) {
        stage(&records[record], sizeof(GpuChunkRecord),
              static_cast<u64>(record) * sizeof(GpuChunkRecord), record_regions_);
        stage(masks.data() + static_cast<usize>(record) * kChunkMaskWords,
              kChunkMaskWords * sizeof(u64),
              static_cast<u64>(record) * kChunkMaskWords * sizeof(u64), mask_regions_);
        stage(prefixes.data() + static_cast<usize>(record) * kChunkMaskWords,
              kChunkMaskWords * sizeof(u32),
              static_cast<u64>(record) * kChunkMaskWords * sizeof(u32), prefix_regions_);
    }

    std::vector<u32> sorted_cells = batch.grid_cells;
    std::sort(sorted_cells.begin(), sorted_cells.end());
    sorted_cells.erase(std::unique(sorted_cells.begin(), sorted_cells.end()),
                       sorted_cells.end());
    for (u32 cell : sorted_cells) {
        stage(&grid[cell], sizeof(u32), static_cast<u64>(cell) * sizeof(u32), grid_regions_);
    }

    flush(cmd, headers_.buffer, header_regions_);
    flush(cmd, occupancy_.buffer, occupancy_regions_);
    flush(cmd, payload_.buffer, payload_regions_);
    flush(cmd, records_.buffer, record_regions_);
    flush(cmd, masks_.buffer, mask_regions_);
    flush(cmd, prefixes_.buffer, prefix_regions_);
    flush(cmd, grid_.buffer, grid_regions_);
    flush(cmd, coarse_.buffer, coarse_regions_);

    stats_.total_uploaded += stats_.staged_bytes;

    if (stats_.staged_bytes == 0) return;

    // One barrier for all three buffers: the copies must land before any shader reads
    // them, and batching the barrier costs less than three separate ones.
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;

    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

}  // namespace ws
