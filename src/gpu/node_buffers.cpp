#include "gpu/node_buffers.hpp"

#include <cstring>

#include "core/log.hpp"

namespace ws {

bool NodeBuffers::create(Device& device, const NodePoolBudget& budget) {
    device_ = &device;

    const u64 node_bytes = static_cast<u64>(budget.max_nodes) * sizeof(GpuNode);
    const u64 leaf_bytes = static_cast<u64>(budget.max_occupancy_leaves) * sizeof(GpuBrickHeader);
    const u64 occupancy_bytes =
        static_cast<u64>(budget.max_occupancy_leaves) * kBrickWords * sizeof(u64);

    // The entry table's size is the pool's own rule, mirrored here so the shader can be told the
    // capacity as a push constant rather than having to guess it.
    entry_capacity_ = 1;
    const u32 wanted = (budget.max_nodes / 4 > 1024) ? budget.max_nodes / 4 : 1024;
    while (entry_capacity_ < wanted) entry_capacity_ <<= 1;
    const u64 entry_bytes = static_cast<u64>(entry_capacity_) * sizeof(u32);

    // TRANSFER_SRC as well as storage, because the audit reads these back.
    //
    // Without it every mirror check has been an illegal copy: `vkCmdCopyBuffer` from a buffer that
    // was never declared as a copy source is undefined behaviour that happens to work on this
    // driver, and it went unseen because nothing ran with --validation after the audit was
    // written. The check that has found three stale-byte bugs was itself unsound, which is the
    // handover's first trap almost exactly -- ask whether the tool is connected before trusting
    // what it says.
    constexpr VkBufferUsageFlags kStorage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    entries_ = create_device_buffer(device, entry_bytes, kStorage, "node entries");
    nodes_ = create_device_buffer(device, node_bytes, kStorage, "nodes");
    leaves_ = create_device_buffer(device, leaf_bytes, kStorage, "node leaves");
    occupancy_ = create_device_buffer(device, occupancy_bytes, kStorage, "node occupancy");
    payload_ = create_device_buffer(device, budget.payload_bytes, kStorage, "node payload");

    // Big enough for the largest single frame's worth: every array's used prefix at once, in the
    // worst case where the whole pool has just been built. Sized once, never grown — a
    // reallocation mid-play is the hitch streaming exists to avoid.
    staging_capacity_ = node_bytes + leaf_bytes + occupancy_bytes + entry_bytes;
    if (staging_capacity_ > budget.payload_bytes) {
        staging_capacity_ += budget.payload_bytes;
    } else {
        staging_capacity_ += budget.payload_bytes;
    }
    staging_ = create_staging_buffer(device, staging_capacity_, "node staging");

    stats_ = NodeBufferStats{};
    stats_.device_bytes =
        node_bytes + leaf_bytes + occupancy_bytes + entry_bytes + budget.payload_bytes;
    return entries_.valid() && nodes_.valid() && leaves_.valid() && occupancy_.valid() &&
           payload_.valid() && staging_.valid();
}

void NodeBuffers::destroy() {
    if (device_ == nullptr) return;
    destroy_buffer(*device_, entries_);
    destroy_buffer(*device_, nodes_);
    destroy_buffer(*device_, leaves_);
    destroy_buffer(*device_, occupancy_);
    destroy_buffer(*device_, payload_);
    destroy_buffer(*device_, staging_);
    device_ = nullptr;
}

bool NodeBuffers::stage(VkCommandBuffer cmd, const void* source, u64 bytes,
                        GpuBuffer& destination) {
    return stage_at(cmd, source, bytes, 0, destination);
}

// The same copy, landing somewhere other than the start of the destination -- which is what a
// dirty range needs and a whole-prefix copy never did.
bool NodeBuffers::stage_at(VkCommandBuffer cmd, const void* source, u64 bytes, u64 destination_offset,
                           GpuBuffer& destination) {
    if (bytes == 0) return true;
    if (staging_cursor_ + bytes > staging_capacity_) {
        // Out of staging for this frame. Saying so matters: the arrays stay marked, so the run
        // that did not fit is retried next frame rather than silently skipped and left stale.
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

void NodeBuffers::upload(VkCommandBuffer cmd, NodePool& pool) {
    stats_.uploaded_this_frame = 0;
    stats_.staging_exhausted = false;
    staging_cursor_ = 0;

    // Nothing changed, so nothing is copied. This is the steady state: the tree converges and
    // then goes quiet, exactly as feedback-driven streaming does once it has served every request
    // — which is the property that makes copying whole prefixes affordable at all.
    // `nodes` as well as the counters, and that is not belt and braces. The pool seeds a root
    // for every entry block the world occupies without counting them as builds — they are not
    // work anybody asked for — so a condition on the counters alone skipped the one upload the
    // marcher cannot start without. The symptom was an empty entry table on the GPU: every ray
    // found no root, read that as empty space, reported nothing, and marched to the far plane.
    // Asked of the dirty sets rather than of the counters. The counters describe what the pool
    // decided to do; the dirty sets describe what actually changed, and only the second can
    // answer "is there anything to send". The old condition also had to name `nodes` explicitly
    // because seeding a root is not counted as a build - a distinction that stops mattering once
    // the question is asked of the right thing.
    if (pool.nothing_dirty()) return;

    // What changed, not what exists.
    //
    // This used to copy every array's whole used prefix on any frame that touched anything. That
    // is free while the tree is converged and quiet, and it is 10 MB a frame while somebody walks
    // across the building - measured on a flight at 2.725 ms mean and 8.915 ms worst against a
    // 0.80 ms budget, the largest single cost in the frame, and the reason the pool read as laggy
    // while marching faster than the thing it replaced.
    //
    // A missed dirty mark is a stale byte on the GPU and a wrong picture. That is precisely what
    // NodeBuffers::audit checks -- it reads the real buffers back and names the first byte that
    // disagrees with the pool -- so this is verifiable rather than hoped for.
    //
    // The gap is in elements and is why the runs are worth coalescing at all: a copy has a fixed
    // cost, so sending a few spare records inside one is cheaper than issuing a second.
    const u64 kNodeGap = 64;     // 2 KB of GpuNode
    const u64 kLeafGap = 64;
    const u64 kEntryGap = 256;   // 1 KB of u32

    for (const auto& run : pool.dirty_entries().runs(kEntryGap)) {
        stage_at(cmd, pool.entries().data() + run.first, run.second * sizeof(u32),
                 run.first * sizeof(u32), entries_);
    }
    for (const auto& run : pool.dirty_nodes().runs(kNodeGap)) {
        stage_at(cmd, pool.nodes().data() + run.first, run.second * sizeof(GpuNode),
                 run.first * sizeof(GpuNode), nodes_);
    }
    for (const auto& run : pool.dirty_leaves().runs(kLeafGap)) {
        stage_at(cmd, pool.leaves().data() + run.first, run.second * sizeof(GpuBrickHeader),
                 run.first * sizeof(GpuBrickHeader), leaves_);
        // Occupancy is kBrickWords per leaf and is written with its header, so one run covers
        // both and the two arrays can never disagree about which leaf is current.
        stage_at(cmd, pool.occupancy().data() + run.first * kBrickWords,
                 run.second * kBrickWords * sizeof(u64),
                 run.first * kBrickWords * sizeof(u64), occupancy_);
    }
    // Payload ranges are recorded where the bytes are written, and a block allocation is
    // contiguous by construction, so these need no coalescing.
    for (const auto& range : pool.dirty_payload()) {
        stage_at(cmd, pool.payload().data() + range.first, range.second, range.first, payload_);
    }

    // Only when all of it fitted. Clearing after a partial upload would mark clean what the
    // card has not been sent -- the stale-byte failure this whole change has to avoid.
    if (!stats_.staging_exhausted) pool.clear_dirty();
    ++stats_.uploads;

    // One barrier for the lot. The copies are independent of each other and every one of them has
    // to land before the marcher reads any of them, so there is nothing to gain by separating.
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

// ---- the mirror check --------------------------------------------------------------------------

namespace {

// Where each array lands in the readback, so one copy-back covers the lot.
struct AuditRange {
    const char* name;
    VkBuffer source;
    u64 bytes;
    const void* expected;
    u64 offset;
};

}  // namespace

bool NodeBuffers::audit(const NodePool& pool) {
    if (device_ == nullptr) return false;

    // Wait for the frame BEFORE touching the staging ring, not only after submitting.
    //
    // This reads the device buffers back into `staging_`, which is the same ring `upload` writes
    // its dirty ranges through -- and an upload recorded into the frame's command buffer has not
    // necessarily executed yet. Copying over those bytes hands the pending upload whatever the
    // audit just read, so the card receives the wrong thing and then disagrees with the pool: an
    // audit that CAUSES the fault it exists to report, intermittently, depending on whether an
    // upload happened to be in flight.
    //
    // Seen once at 640x400 after per-node eviction (D262) started making uploads happen on most
    // frames rather than a few, and it did not reproduce, which is exactly how a race presents.
    // The audit is off the hot path by construction -- it stalls the device anyway -- so waiting
    // twice costs nothing worth having.
    device_->wait_idle();

    const u32 node_count = pool.node_watermark();
    const u32 leaf_count = pool.leaf_watermark();

    AuditRange ranges[5]{
        {"entries", entries_.buffer, pool.entries().size() * sizeof(u32), pool.entries().data(), 0},
        {"nodes", nodes_.buffer, static_cast<u64>(node_count) * sizeof(GpuNode),
         pool.nodes().data(), 0},
        {"leaves", leaves_.buffer, static_cast<u64>(leaf_count) * sizeof(GpuBrickHeader),
         pool.leaves().data(), 0},
        {"occupancy", occupancy_.buffer,
         static_cast<u64>(leaf_count) * kBrickWords * sizeof(u64), pool.occupancy().data(), 0},
        {"payload", payload_.buffer, pool.payload_watermark(), pool.payload().data(), 0},
    };

    u64 cursor = 0;
    for (AuditRange& range : ranges) {
        range.offset = cursor;
        cursor += range.bytes;
    }
    if (cursor > staging_capacity_) {
        WS_LOG_ERROR("nodes", "audit needs {} bytes of staging, have {}", cursor,
                     staging_capacity_);
        return false;
    }

    // One-shot command buffer, exactly as the screenshot path does it. This is off the hot path
    // by construction, so the simple thing is the right thing.
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
    for (const AuditRange& range : ranges) {
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
    for (const AuditRange& range : ranges) {
        if (range.bytes == 0) continue;
        const u8* card = static_cast<const u8*>(staging_.mapped) + range.offset;
        const u8* host = static_cast<const u8*>(range.expected);
        if (std::memcmp(card, host, static_cast<usize>(range.bytes)) == 0) continue;

        // The first differing byte, not merely "they differ". An index is the difference between
        // a bug you can look at and an afternoon of narrowing.
        u64 at = 0;
        while (at < range.bytes && card[at] == host[at]) ++at;
        WS_LOG_ERROR("nodes",
                     "GPU mirror mismatch in {}: first at byte {} of {} (card {:#04x}, "
                     "host {:#04x})",
                     range.name, at, range.bytes, card[at], host[at]);
        matched = false;
    }

    vkDestroyCommandPool(device_->handle(), command_pool, nullptr);
    if (matched) {
        WS_LOG_INFO("nodes", "GPU mirror matches: {} nodes, {} leaves, {} payload bytes",
                    node_count, leaf_count, pool.payload_watermark());
    }
    return matched;
}

}  // namespace ws
