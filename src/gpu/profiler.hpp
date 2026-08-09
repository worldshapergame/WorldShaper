#pragma once
// GPU timing and bandwidth accounting.
//
// documentation/09-performance-budgets.md gives every pass a time budget *and* a
// bandwidth budget, because the minimum spec (Steam Deck) is bandwidth-bound rather than
// compute-bound. Both numbers are measured here and shown on the HUD, so a regression is
// noticed the moment it appears rather than three stages later.
//
// Timestamps are read back one frame late, which costs nothing and avoids a stall.

#include <array>
#include <string>
#include <vector>

// The slot bookkeeping, which lives in core because it has nothing to do with a GPU and because
// that is what lets it be unit tested — see the note at the top of that file for the fault it
// exists to prevent.
#include "core/pass_ledger.hpp"
#include "gpu/device.hpp"
#include "gpu/swapchain.hpp"

namespace ws {

struct PassTiming {
    std::string name;
    f64 gpu_ms = 0.0;
    u64 bytes = 0;      // reported by the pass itself, not measured by the driver
    f64 budget_ms = 0.0;  // 0 means "no budget set yet"
    // How deeply this pass is nested inside another. Only depth 0 counts towards the frame
    // total, or a pass that contains another would have its child's time in the sum twice.
    u32 depth = 0;
};

// One pass, averaged over however many frames have been measured since the last reset.
//
// A single frame's GPU time is not a measurement, it is a sample: the same camera on the same
// build moves several per cent frame to frame from clock and scheduling alone. Every number in
// documentation/ that a later change has to beat therefore has to be a mean over a window, and
// the window has to start after warm-up — shaders are still compiling and chunks are still
// arriving for the first dozens of frames, and timing those measures the loading screen.
struct PassAverage {
    std::string name;
    f64 mean_ms = 0.0;
    f64 worst_ms = 0.0;
    u64 mean_bytes = 0;
    f64 budget_ms = 0.0;
    u32 depth = 0;
    u32 samples = 0;
};

class GpuProfiler {
public:
    static constexpr u32 kMaxPasses = PassLedger::kMaxPasses;

    bool create(Device& device);
    void destroy();

    // Call once per frame before recording any passes.
    void begin_frame(VkCommandBuffer cmd, u32 frame_index);
    // Passes may nest. A pass opened inside another gets its own timestamps and its own row,
    // and the enclosing pass still measures the whole of itself.
    //
    // They did not nest before, and the failure was silent rather than loud: `begin_pass` kept
    // one open index and did not claim its slot until `end_pass`, so an inner pass overwrote
    // the outer one's name and slot, the inner `end_pass` cleared the open index, and the
    // outer `end_pass` then found nothing open and wrote no end timestamp at all. The work
    // after the inner pass — which in the path-traced frame is the entire tracer — was
    // measured by nothing. It read as a frame with 0.253 ms of GPU work in it against 37.7 ms
    // of wall clock, and every path-traced figure recorded in documentation/19-auto-quality.md
    // predates the nesting and cannot be reproduced.
    void begin_pass(VkCommandBuffer cmd, const char* name, f64 budget_ms = 0.0);
    void end_pass(VkCommandBuffer cmd);
    void end_frame(VkCommandBuffer cmd);

    // Adds bytes moved to the pass currently open. Systems report their own traffic;
    // there is no portable driver counter for this.
    void add_bytes(u64 bytes);

    const std::vector<PassTiming>& results() const { return results_; }
    f64 total_gpu_ms() const { return total_gpu_ms_; }
    u64 total_bytes() const { return total_bytes_; }

    // The window a baseline is read from. Averages accumulate from the last reset; a harness
    // resets after warm-up and reads them at the end.
    const std::vector<PassAverage>& averages() const { return averages_; }
    void reset_averages();
    u32 averaged_frames() const { return averaged_frames_; }
    f64 mean_total_gpu_ms() const {
        return (averaged_frames_ > 0) ? total_sum_ms_ / static_cast<f64>(averaged_frames_) : 0.0;
    }
    f64 worst_total_gpu_ms() const { return total_worst_ms_; }

private:
    struct FrameQueries {
        VkQueryPool pool = VK_NULL_HANDLE;
        PassLedger ledger;
        bool pending = false;
    };

    void resolve(FrameQueries& frame);
    void accumulate(const PassTiming& pass);

    Device* device_ = nullptr;
    FrameQueries frames_[kFramesInFlight];
    u32 current_ = 0;
    std::vector<PassTiming> results_;
    f64 total_gpu_ms_ = 0.0;
    u64 total_bytes_ = 0;

    std::vector<PassAverage> averages_;
    u32 averaged_frames_ = 0;
    f64 total_sum_ms_ = 0.0;
    f64 total_worst_ms_ = 0.0;
};

}  // namespace ws
