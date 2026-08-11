#include "gpu/type_tables.hpp"

#include <cstring>

namespace ws {

bool TypeTables::create(Device& device, u64 staging_bytes) {
    device_ = &device;

    const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    types_ = create_device_buffer(device, static_cast<u64>(kMaxTables) * 8, storage, "voxel types");
    visuals_ =
        create_device_buffer(device, static_cast<u64>(kMaxTables) * 16, storage, "visual records");

    // One staging region per frame in flight, so writing next frame's data cannot
    // overwrite bytes the GPU is still reading from this frame's copies.
    staging_capacity_per_frame_ = staging_bytes;
    staging_ = create_staging_buffer(device, staging_bytes * kFramesInFlight, "table staging");

    WS_LOG_INFO("gpu", "type tables: {} MB of types, {} MB of visuals, staging {} MB",
                types_.size / (1024 * 1024), visuals_.size / (1024 * 1024),
                staging_.size / (1024 * 1024));
    return true;
}

void TypeTables::destroy() {
    if (device_ == nullptr) return;
    destroy_buffer(*device_, staging_);
    destroy_buffer(*device_, visuals_);
    destroy_buffer(*device_, types_);
    device_ = nullptr;
}

bool TypeTables::stage(const void* source, u64 size, u64 base,
                       std::vector<VkBufferCopy>& into) {
    if (size == 0) return true;
    if (size > staging_capacity_per_frame_) return false;

    std::memcpy(static_cast<u8*>(staging_.mapped) + base, source, size);
    VkBufferCopy copy{};
    copy.srcOffset = base;
    copy.dstOffset = 0;
    copy.size = size;
    into.push_back(copy);
    staged_bytes_ += size;
    return true;
}

void TypeTables::upload_tables(VkCommandBuffer cmd, const VoxelTypeTable& table, u32 frame_index) {
    staged_bytes_ = 0;

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

    const u64 frame_base =
        static_cast<u64>(frame_index % kFramesInFlight) * staging_capacity_per_frame_;
    const u64 type_bytes = type_pairs.size() * sizeof(u32);
    const u64 visual_bytes = visual_records.size() * sizeof(VisualRecord);

    // Both or neither, and the counts move only when both landed: a visual record the types
    // table points at but which has not arrived is a voxel drawn as whatever the buffer held.
    if (type_bytes + visual_bytes > staging_capacity_per_frame_) {
        WS_LOG_WARN("gpu", "the type tables ({} + {} bytes) do not fit one frame of staging ({})",
                    type_bytes, visual_bytes, staging_capacity_per_frame_);
        return;
    }

    std::vector<VkBufferCopy> type_copies;
    std::vector<VkBufferCopy> visual_copies;
    if (!stage(type_pairs.data(), type_bytes, frame_base, type_copies)) return;
    if (!stage(visual_records.data(), visual_bytes, frame_base + type_bytes, visual_copies)) return;

    if (!type_copies.empty()) {
        vkCmdCopyBuffer(cmd, staging_.buffer, types_.buffer,
                        static_cast<u32>(type_copies.size()), type_copies.data());
    }
    if (!visual_copies.empty()) {
        vkCmdCopyBuffer(cmd, staging_.buffer, visuals_.buffer,
                        static_cast<u32>(visual_copies.size()), visual_copies.data());
    }

    // The copies must land before any shader reads them.
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;

    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);

    uploaded_types_ = type_count;
    uploaded_visuals_ = visual_count;
    WS_LOG_DEBUG("gpu", "uploaded {} voxel types, {} visual records", type_count, visual_count);
}

}  // namespace ws
