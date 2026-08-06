#pragma once
// The renderer's feedback buffer.
//
// documentation/03-voxel-data-model.md §8: streaming is demand-driven from a feedback
// buffer — the renderer reports what it wanted and could not find, the streamer serves
// it, LRU evicts the rest. Memory is then bounded by what is on screen rather than by
// world size, which is the mechanism that makes an infinite world fit on a handheld.
//
// The alternative, and what this replaces, is requesting a radius around the camera. That
// is the wrong rule for a renderer with continuous detail: it decides residency from
// where you *are* rather than from what you can *see*, so a chunk 300 m away is never
// streamed however large it looks on screen.
//
// Results come back two frames late. That is fine — a chunk that arrives two frames after
// it was first wanted is drawn at a coarser level in the meantime, which is exactly what
// the detail hierarchy is for.

#include <vector>

#include "gpu/buffer.hpp"
#include "gpu/render_params.hpp"
#include "gpu/swapchain.hpp"

namespace ws {

class FeedbackBuffer {
public:
    bool create(Device& device);
    void destroy();

    VkBuffer buffer() const { return device_buffer_.buffer; }

    // Zeroes the counter. Must be recorded before anything writes feedback.
    void begin_frame(VkCommandBuffer cmd);

    // Copies this frame's report into the readback slot for `frame_index`.
    void end_frame(VkCommandBuffer cmd, u32 frame_index);

    // Reads the report written into this slot the last time it was used, which the
    // swapchain has already waited on. Returns an empty span on the first frames.
    const std::vector<FeedbackEntry>& read(u32 frame_index);

    u32 last_reported() const { return last_reported_; }
    u32 last_truncated() const { return last_truncated_; }

private:
    Device* device_ = nullptr;
    GpuBuffer device_buffer_;
    GpuBuffer readback_;
    std::vector<FeedbackEntry> entries_;
    u32 last_reported_ = 0;
    u32 last_truncated_ = 0;

    static constexpr u64 kHeaderBytes = 16;   // counter plus padding to 16
    static constexpr u64 kSlotBytes =
        kHeaderBytes + static_cast<u64>(kFeedbackCapacity) * sizeof(FeedbackEntry);
};

}  // namespace ws
