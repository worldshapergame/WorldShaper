#include "gpu/profiler.hpp"

namespace ws {

bool GpuProfiler::create(Device& device) {
    device_ = &device;
    for (FrameQueries& frame : frames_) {
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = kMaxPasses * 2;
        WS_VK(vkCreateQueryPool(device_->handle(), &info, nullptr, &frame.pool));
        vkResetQueryPool(device_->handle(), frame.pool, 0, kMaxPasses * 2);
    }
    return true;
}

void GpuProfiler::destroy() {
    if (device_ == nullptr) return;
    for (FrameQueries& frame : frames_) {
        if (frame.pool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(device_->handle(), frame.pool, nullptr);
            frame.pool = VK_NULL_HANDLE;
        }
    }
    device_ = nullptr;
}

void GpuProfiler::begin_frame(VkCommandBuffer cmd, u32 frame_index) {
    current_ = frame_index % kFramesInFlight;
    FrameQueries& frame = frames_[current_];

    // This slot's previous results are now guaranteed complete, because the swapchain
    // waited on its timeline value before handing us the command buffer.
    if (frame.pending) resolve(frame);

    vkCmdResetQueryPool(cmd, frame.pool, 0, kMaxPasses * 2);
    frame.count = 0;
    frame.bytes.fill(0);
    open_pass_ = -1;
}

// The marker a finished pass leaves behind. A driver reports every checkpoint the queue
// reached, so a begin marker with no matching finish after it is the pass that was still
// running when the device died — which is the pass that killed it.
//
// The begin marker is the pass name pointer itself. Every call site passes a string literal,
// which outlives the queue; a pointer into a temporary would be read back after the fault,
// long after it stopped being valid.
namespace {
const char* const kPassFinished = "-- finished";
}

void GpuProfiler::begin_pass(VkCommandBuffer cmd, const char* name, f64 budget_ms) {
    FrameQueries& frame = frames_[current_];
    if (frame.count >= kMaxPasses) return;
    open_pass_ = static_cast<i32>(frame.count);
    frame.names[frame.count] = name;
    frame.budgets[frame.count] = budget_ms;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, frame.pool,
                         frame.count * 2);
    if (device_->has_checkpoints() && vkCmdSetCheckpointNV) {
        vkCmdSetCheckpointNV(cmd, const_cast<char*>(name));
    }
}

void GpuProfiler::end_pass(VkCommandBuffer cmd) {
    FrameQueries& frame = frames_[current_];
    if (open_pass_ < 0) return;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, frame.pool,
                         static_cast<u32>(open_pass_) * 2 + 1);
    if (device_->has_checkpoints() && vkCmdSetCheckpointNV) {
        vkCmdSetCheckpointNV(cmd, const_cast<char*>(kPassFinished));
    }
    ++frame.count;
    open_pass_ = -1;
}

void GpuProfiler::end_frame(VkCommandBuffer /*cmd*/) {
    frames_[current_].pending = frames_[current_].count > 0;
}

void GpuProfiler::add_bytes(u64 bytes) {
    FrameQueries& frame = frames_[current_];
    if (open_pass_ < 0) return;
    frame.bytes[static_cast<usize>(open_pass_)] += bytes;
}

void GpuProfiler::resolve(FrameQueries& frame) {
    std::array<u64, kMaxPasses * 2> stamps{};
    const VkResult result = vkGetQueryPoolResults(
        device_->handle(), frame.pool, 0, frame.count * 2,
        sizeof(u64) * frame.count * 2, stamps.data(), sizeof(u64),
        VK_QUERY_RESULT_64_BIT);
    frame.pending = false;
    if (result != VK_SUCCESS) return;  // not ready yet; skip this frame's numbers

    const f64 period_ns = static_cast<f64>(device_->caps().timestamp_period_ns);

    results_.clear();
    total_gpu_ms_ = 0.0;
    total_bytes_ = 0;
    for (u32 i = 0; i < frame.count; ++i) {
        const u64 begin = stamps[i * 2];
        const u64 end = stamps[i * 2 + 1];
        const f64 ms = (end > begin) ? static_cast<f64>(end - begin) * period_ns * 1e-6 : 0.0;
        results_.push_back(PassTiming{frame.names[i], ms, frame.bytes[i], frame.budgets[i]});
        total_gpu_ms_ += ms;
        total_bytes_ += frame.bytes[i];
    }
}

}  // namespace ws
