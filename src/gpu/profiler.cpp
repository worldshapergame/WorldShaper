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
    frame.ledger.reset();
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
    const i32 slot = frame.ledger.open(name, budget_ms);
    if (slot < 0) return;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, frame.pool,
                         static_cast<u32>(slot) * 2);
    if (device_->has_checkpoints() && vkCmdSetCheckpointNV) {
        vkCmdSetCheckpointNV(cmd, const_cast<char*>(name));
    }
}

void GpuProfiler::end_pass(VkCommandBuffer cmd) {
    FrameQueries& frame = frames_[current_];
    const i32 slot = frame.ledger.close();
    if (slot < 0) return;   // nothing open, or the matching open was refused a slot
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, frame.pool,
                         static_cast<u32>(slot) * 2 + 1);
    if (device_->has_checkpoints() && vkCmdSetCheckpointNV) {
        vkCmdSetCheckpointNV(cmd, const_cast<char*>(kPassFinished));
    }
}

void GpuProfiler::end_frame(VkCommandBuffer /*cmd*/) {
    FrameQueries& frame = frames_[current_];
    // A pass opened and never closed writes no end timestamp, and the pass simply vanishes from
    // the frame with everything else still looking right — which is exactly how the tracer went
    // unmeasured for as long as it did. Loud in development, ignored in release: a mis-measured
    // frame is not a reason to take the game away from somebody.
    WS_ASSERT(frame.ledger.balanced(), "a GPU pass was opened and not closed");
    frame.pending = frame.ledger.count() > 0;
}

void GpuProfiler::add_bytes(u64 bytes) { frames_[current_].ledger.add_bytes(bytes); }

void GpuProfiler::reset_averages() {
    averages_.clear();
    averaged_frames_ = 0;
    total_sum_ms_ = 0.0;
    total_worst_ms_ = 0.0;
}

// Matched by name rather than by index, because the pass list is not the same in every frame:
// the tracer's frame and the real-time frame have different passes in them, and a pass can be
// skipped for a frame when there is nothing for it to do.
void GpuProfiler::accumulate(const PassTiming& pass) {
    for (PassAverage& entry : averages_) {
        if (entry.name != pass.name) continue;
        entry.mean_ms += (pass.gpu_ms - entry.mean_ms) / static_cast<f64>(entry.samples + 1);
        entry.mean_bytes = (entry.mean_bytes * entry.samples + pass.bytes) / (entry.samples + 1);
        entry.worst_ms = (pass.gpu_ms > entry.worst_ms) ? pass.gpu_ms : entry.worst_ms;
        ++entry.samples;
        return;
    }
    averages_.push_back(PassAverage{pass.name, pass.gpu_ms, pass.gpu_ms, pass.bytes,
                                    pass.budget_ms, pass.depth, 1});
}

void GpuProfiler::resolve(FrameQueries& frame) {
    const u32 passes = frame.ledger.count();
    std::array<u64, kMaxPasses * 2> stamps{};
    const VkResult result = vkGetQueryPoolResults(
        device_->handle(), frame.pool, 0, passes * 2,
        sizeof(u64) * passes * 2, stamps.data(), sizeof(u64),
        VK_QUERY_RESULT_64_BIT);
    frame.pending = false;
    if (result != VK_SUCCESS) return;  // not ready yet; skip this frame's numbers

    const f64 period_ns = static_cast<f64>(device_->caps().timestamp_period_ns);

    results_.clear();
    total_gpu_ms_ = 0.0;
    total_bytes_ = 0;
    for (u32 i = 0; i < passes; ++i) {
        const u64 begin = stamps[i * 2];
        const u64 end = stamps[i * 2 + 1];
        const f64 ms = (end > begin) ? static_cast<f64>(end - begin) * period_ns * 1e-6 : 0.0;
        results_.push_back(PassTiming{frame.ledger.name(i), ms, frame.ledger.bytes(i),
                                      frame.ledger.budget(i), frame.ledger.pass_depth(i)});
        // Only the outermost passes count towards the frame, or a pass containing another
        // contributes its child's time twice and the total exceeds the frame it describes.
        if (frame.ledger.pass_depth(i) == 0) {
            total_gpu_ms_ += ms;
            total_bytes_ += frame.ledger.bytes(i);
        }
        accumulate(results_.back());
    }
    ++averaged_frames_;
    total_sum_ms_ += total_gpu_ms_;
    if (total_gpu_ms_ > total_worst_ms_) total_worst_ms_ = total_gpu_ms_;
}

}  // namespace ws
