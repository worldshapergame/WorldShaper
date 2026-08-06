#include "gpu/feedback.hpp"

#include <cstring>

namespace ws {

bool FeedbackBuffer::create(Device& device) {
    device_ = &device;
    device_buffer_ = create_device_buffer(device, kSlotBytes,
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                          "streaming feedback");
    readback_ = create_staging_buffer(device, kSlotBytes * kFramesInFlight,
                                      "feedback readback");
    entries_.reserve(kFeedbackCapacity);
    return true;
}

void FeedbackBuffer::destroy() {
    if (device_ == nullptr) return;
    destroy_buffer(*device_, readback_);
    destroy_buffer(*device_, device_buffer_);
    device_ = nullptr;
}

void FeedbackBuffer::begin_frame(VkCommandBuffer cmd) {
    vkCmdFillBuffer(cmd, device_buffer_.buffer, 0, kHeaderBytes, 0);

    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;

    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

void FeedbackBuffer::end_frame(VkCommandBuffer cmd, u32 frame_index) {
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;

    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);

    VkBufferCopy copy{};
    copy.srcOffset = 0;
    copy.dstOffset = static_cast<u64>(frame_index % kFramesInFlight) * kSlotBytes;
    copy.size = kSlotBytes;
    vkCmdCopyBuffer(cmd, device_buffer_.buffer, readback_.buffer, 1, &copy);
}

const std::vector<FeedbackEntry>& FeedbackBuffer::read(u32 frame_index) {
    entries_.clear();
    last_reported_ = 0;
    last_truncated_ = 0;
    if (readback_.mapped == nullptr) return entries_;

    const u8* slot = static_cast<const u8*>(readback_.mapped) +
                     static_cast<u64>(frame_index % kFramesInFlight) * kSlotBytes;

    u32 reported = 0;
    std::memcpy(&reported, slot, sizeof(u32));
    last_reported_ = reported;

    // The counter keeps climbing past the capacity so the overflow is visible rather than
    // silent; only what actually fits was written.
    const u32 stored = (reported > kFeedbackCapacity) ? kFeedbackCapacity : reported;
    last_truncated_ = reported - stored;

    entries_.resize(stored);
    if (stored > 0) {
        std::memcpy(entries_.data(), slot + kHeaderBytes,
                    static_cast<usize>(stored) * sizeof(FeedbackEntry));
    }
    return entries_;
}

}  // namespace ws
