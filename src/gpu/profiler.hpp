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

#include "gpu/device.hpp"
#include "gpu/swapchain.hpp"

namespace ws {

struct PassTiming {
    std::string name;
    f64 gpu_ms = 0.0;
    u64 bytes = 0;      // reported by the pass itself, not measured by the driver
    f64 budget_ms = 0.0;  // 0 means "no budget set yet"
};

class GpuProfiler {
public:
    static constexpr u32 kMaxPasses = 32;

    bool create(Device& device);
    void destroy();

    // Call once per frame before recording any passes.
    void begin_frame(VkCommandBuffer cmd, u32 frame_index);
    void begin_pass(VkCommandBuffer cmd, const char* name, f64 budget_ms = 0.0);
    void end_pass(VkCommandBuffer cmd);
    void end_frame(VkCommandBuffer cmd);

    // Adds bytes moved to the pass currently open. Systems report their own traffic;
    // there is no portable driver counter for this.
    void add_bytes(u64 bytes);

    const std::vector<PassTiming>& results() const { return results_; }
    f64 total_gpu_ms() const { return total_gpu_ms_; }
    u64 total_bytes() const { return total_bytes_; }

private:
    struct FrameQueries {
        VkQueryPool pool = VK_NULL_HANDLE;
        u32 count = 0;
        bool pending = false;
        std::array<std::string, kMaxPasses> names;
        std::array<f64, kMaxPasses> budgets{};
        std::array<u64, kMaxPasses> bytes{};
    };

    void resolve(FrameQueries& frame);

    Device* device_ = nullptr;
    FrameQueries frames_[kFramesInFlight];
    u32 current_ = 0;
    i32 open_pass_ = -1;
    std::vector<PassTiming> results_;
    f64 total_gpu_ms_ = 0.0;
    u64 total_bytes_ = 0;
};

}  // namespace ws
