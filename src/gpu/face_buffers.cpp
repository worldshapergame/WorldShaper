#include "gpu/face_buffers.hpp"

#include <cstring>

#include "core/log.hpp"

namespace ws {

bool FaceBuffers::create(Device& device, const FaceStoreBudget& budget) {
    device_ = &device;

    const u64 face_bytes = static_cast<u64>(budget.max_faces) * sizeof(GpuFace);

    // The store's own rule, mirrored here so the shader can be told the capacity as a push
    // constant rather than having to guess it -- and so the two cannot disagree about the mask a
    // bucket index is taken with, which is the sort of divergence that produces a table where
    // half the lookups miss and nothing is obviously wrong.
    entry_capacity_ = 1;
    const u32 wanted = (budget.max_faces * 2 > 1024) ? budget.max_faces * 2 : 1024;
    while (entry_capacity_ < wanted) entry_capacity_ <<= 1;
    const u64 entry_bytes = static_cast<u64>(entry_capacity_) * sizeof(u32);

    constexpr VkBufferUsageFlags kStorage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    faces_ = create_device_buffer(device, face_bytes, kStorage, "faces");
    entries_ = create_device_buffer(device, entry_bytes, kStorage, "face entries");

    // Big enough for both arrays at once, which is what the audit reads back and what a first
    // upload of a full store would send. Sized once and never grown: a reallocation mid-play is
    // the hitch streaming exists to avoid.
    staging_capacity_ = face_bytes + entry_bytes;
    staging_ = create_staging_buffer(device, staging_capacity_, "face staging");

    stats_ = FaceBufferStats{};
    stats_.device_bytes = face_bytes + entry_bytes;
    return faces_.valid() && entries_.valid() && staging_.valid();
}

void FaceBuffers::destroy() {
    if (device_ == nullptr) return;
    destroy_buffer(*device_, faces_);
    destroy_buffer(*device_, entries_);
    destroy_buffer(*device_, staging_);
    device_ = nullptr;
}

bool FaceBuffers::stage_at(VkCommandBuffer cmd, const void* source, u64 bytes,
                           u64 destination_offset, GpuBuffer& destination) {
    if (bytes == 0) return true;
    if (staging_cursor_ + bytes > staging_capacity_) {
        // Out of staging this frame. Saying so matters: the ranges stay marked, so what did not
        // fit goes next frame rather than being silently skipped and left stale on the card.
        stats_.staging_exhausted = true;
        return false;
    }

    std::memcpy(static_cast<u8*>(staging_.mapped) + staging_cursor_, source, bytes);

    VkBufferCopy copy{};
    copy.srcOffset = staging_cursor_;
    copy.dstOffset = destination_offset;
    copy.size = bytes;
    vkCmdCopyBuffer(cmd, staging_.buffer, destination.buffer, 1, &copy);

    staging_cursor_ += bytes;
    stats_.uploaded_this_frame += bytes;
    stats_.total_uploaded += bytes;
    return true;
}

void FaceBuffers::upload(VkCommandBuffer cmd, FaceStore& store) {
    stats_.uploaded_this_frame = 0;
    stats_.staging_exhausted = false;
    staging_cursor_ = 0;

    if (store.nothing_dirty()) return;

    // The gap is in elements, and coalescing is worth it because a copy has a fixed cost of its
    // own: sending a few spare records inside one is cheaper than issuing a second.
    const u64 kFaceGap = 64;    // 2 KB of GpuFace
    const u64 kEntryGap = 256;  // 1 KB of u32

    for (const auto& run : store.dirty_entries().runs(kEntryGap)) {
        stage_at(cmd, store.entries().data() + run.first, run.second * sizeof(u32),
                 run.first * sizeof(u32), entries_);
    }
    for (const auto& run : store.dirty_faces().runs(kFaceGap)) {
        stage_at(cmd, store.faces().data() + run.first, run.second * sizeof(GpuFace),
                 run.first * sizeof(GpuFace), faces_);
    }

    // Only when all of it fitted. Clearing after a partial upload marks clean what the card has
    // not been sent, which is the stale byte this whole arrangement exists to avoid.
    if (!stats_.staging_exhausted) store.clear_dirty();
    ++stats_.uploads;

    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

bool FaceBuffers::audit(const FaceStore& store) {
    if (device_ == nullptr) return false;

    // Wait for the frame BEFORE touching the staging ring, not only after submitting.
    //
    // This reads the device buffers back into the same ring `upload` writes through, and an
    // upload recorded into the frame's command buffer has not necessarily executed yet. Copying
    // over those bytes hands the pending upload whatever the audit just read -- an audit that
    // causes the fault it exists to report, intermittently. The node pool's did exactly that
    // (D265) and it took a per-node eviction pass to make it show up at all.
    device_->wait_idle();

    struct Range {
        const char* name;
        VkBuffer source;
        u64 bytes;
        const void* expected;
        u64 offset;
    };

    // The bucket table only, byte for byte -- and the faces compared on IDENTITY alone below.
    //
    // Ownership is split here and it has to be, because the whole point of the stage is that the
    // card writes the light. The CPU owns which face a slot is: the key, the level, the direction,
    // the flags. The GPU owns what arrives there: irradiance, photons, the sample counters. So a
    // byte-for-byte comparison of the record would report a mismatch on every frame that shaded
    // anything, which is an audit that cries wolf until somebody turns it off -- and this check
    // has already caught three real stale-byte bugs that a photograph never would have.
    Range ranges[1]{
        {"face entries", entries_.buffer, store.entries().size() * sizeof(u32),
         store.entries().data(), 0},
    };
    u64 cursor = 0;
    for (Range& range : ranges) {
        range.offset = cursor;
        cursor += range.bytes;
    }
    if (cursor > staging_capacity_) {
        WS_LOG_ERROR("faces", "audit needs {} bytes of staging, have {}", cursor, staging_capacity_);
        return false;
    }

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = device_->graphics_family();
    VkCommandPool command_pool = VK_NULL_HANDLE;
    WS_VK(vkCreateCommandPool(device_->handle(), &pool_info, nullptr, &command_pool));

    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = command_pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    WS_VK(vkAllocateCommandBuffers(device_->handle(), &alloc, &cmd));

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    WS_VK(vkBeginCommandBuffer(cmd, &begin));
    for (const Range& range : ranges) {
        if (range.bytes == 0) continue;
        VkBufferCopy copy{};
        copy.srcOffset = 0;
        copy.dstOffset = range.offset;
        copy.size = range.bytes;
        vkCmdCopyBuffer(cmd, range.source, staging_.buffer, 1, &copy);
    }
    WS_VK(vkEndCommandBuffer(cmd));

    VkCommandBufferSubmitInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmd_info.commandBuffer = cmd;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;
    WS_VK(vkQueueSubmit2(device_->graphics_queue(), 1, &submit, VK_NULL_HANDLE));
    device_->wait_idle();

    bool matched = true;
    for (const Range& range : ranges) {
        if (range.bytes == 0) continue;
        const u8* card = static_cast<const u8*>(staging_.mapped) + range.offset;
        const u8* host = static_cast<const u8*>(range.expected);
        if (std::memcmp(card, host, static_cast<usize>(range.bytes)) == 0) continue;
        matched = false;
        // Named to the byte. A mismatch reported as "the picture is wrong somewhere" costs a day;
        // reported as an offset and two values it costs a reading of whatever writes that field.
        for (u64 i = 0; i < range.bytes; ++i) {
            if (card[i] == host[i]) continue;
            WS_LOG_ERROR("faces",
                         "GPU mirror mismatch in {}: first at byte {} of {} (card 0x{:02x}, "
                         "host 0x{:02x})", range.name, i, range.bytes, card[i], host[i]);
            break;
        }
    }
    // And the faces, on identity only. A slot the CPU thinks is face A while the card thinks it is
    // face B is the fault that matters: the light would be written to the wrong surface and look
    // like a shading bug for ever. What is IN the face is the card's business.
    const u32 watermark = store.watermark();
    if (matched && watermark > 0) {
        const u64 face_bytes = static_cast<u64>(watermark) * sizeof(GpuFace);
        if (face_bytes <= staging_capacity_) {
            VkCommandPoolCreateInfo face_pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            face_pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            face_pool_info.queueFamilyIndex = device_->graphics_family();
            VkCommandPool face_pool = VK_NULL_HANDLE;
            WS_VK(vkCreateCommandPool(device_->handle(), &face_pool_info, nullptr, &face_pool));
            VkCommandBufferAllocateInfo face_alloc{
                VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            face_alloc.commandPool = face_pool;
            face_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            face_alloc.commandBufferCount = 1;
            VkCommandBuffer face_cmd = VK_NULL_HANDLE;
            WS_VK(vkAllocateCommandBuffers(device_->handle(), &face_alloc, &face_cmd));
            VkCommandBufferBeginInfo face_begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            face_begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            WS_VK(vkBeginCommandBuffer(face_cmd, &face_begin));
            VkBufferCopy face_copy{};
            face_copy.size = face_bytes;
            vkCmdCopyBuffer(face_cmd, faces_.buffer, staging_.buffer, 1, &face_copy);
            WS_VK(vkEndCommandBuffer(face_cmd));
            VkCommandBufferSubmitInfo face_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
            face_info.commandBuffer = face_cmd;
            VkSubmitInfo2 face_submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
            face_submit.commandBufferInfoCount = 1;
            face_submit.pCommandBufferInfos = &face_info;
            WS_VK(vkQueueSubmit2(device_->graphics_queue(), 1, &face_submit, VK_NULL_HANDLE));
            device_->wait_idle();

            const GpuFace* card = static_cast<const GpuFace*>(staging_.mapped);
            for (u32 slot = 0; slot < watermark; ++slot) {
                const GpuFace& host = store.faces()[slot];
                if (card[slot].x == host.x && card[slot].y == host.y && card[slot].z == host.z &&
                    card[slot].packed == host.packed) {
                    continue;
                }
                matched = false;
                WS_LOG_ERROR("faces",
                             "GPU mirror mismatch: slot {} is ({},{},{}) packed 0x{:08x} on the "
                             "card and ({},{},{}) packed 0x{:08x} here", slot, card[slot].x,
                             card[slot].y, card[slot].z, card[slot].packed, host.x, host.y,
                             host.z, host.packed);
                break;
            }
            vkDestroyCommandPool(device_->handle(), face_pool, nullptr);
        }
    }

    if (matched) {
        WS_LOG_INFO("faces", "GPU mirror matches: {} faces by identity, {} buckets exactly",
                    watermark, store.entries().size());
    }

    vkDestroyCommandPool(device_->handle(), command_pool, nullptr);
    return matched;
}

}  // namespace ws
