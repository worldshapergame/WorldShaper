#pragma once
// Swapchain and the per-frame submission ring.
//
// Frames in flight are capped at two: more would add input latency, which matters more
// here than throughput. Synchronisation uses timeline semaphores plus per-image binary
// semaphores, because presentation engines still require binary ones.

#include <vector>

#include "gpu/device.hpp"

namespace ws {

inline constexpr u32 kFramesInFlight = 2;

struct FrameContext {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore image_available = VK_NULL_HANDLE;
    u64 timeline_value = 0;
};

class Swapchain {
public:
    Swapchain() = default;
    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    bool create(Device& device, u32 width, u32 height, bool vsync);
    void destroy();
    bool recreate(u32 width, u32 height);

    // Waits for the frame slot to be free and begins its command buffer.
    // Returns false when the swapchain is out of date and the caller should skip the
    // frame (it will be recreated on the next call).
    bool begin_frame();

    // Ends the command buffer, submits, and presents.
    void end_frame();

    VkCommandBuffer cmd() const { return frames_[frame_index_].cmd; }
    VkImage current_image() const { return images_[image_index_]; }
    VkImageView current_view() const { return views_[image_index_]; }
    VkFormat format() const { return format_; }
    VkExtent2D extent() const { return extent_; }
    u32 image_count() const { return static_cast<u32>(images_.size()); }
    u32 frame_index() const { return frame_index_; }
    u64 frame_number() const { return frame_number_; }
    bool vsync() const { return vsync_; }
    void set_vsync(bool enabled) { vsync_ = enabled; needs_recreate_ = true; }
    bool needs_recreate() const { return needs_recreate_; }

private:
    bool build(u32 width, u32 height);
    void destroy_swapchain_objects();

    Device* device_ = nullptr;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkSemaphore> render_finished_;   // one per swapchain image
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    bool vsync_ = true;
    bool needs_recreate_ = false;

    FrameContext frames_[kFramesInFlight]{};
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    u64 timeline_counter_ = 0;
    u32 frame_index_ = 0;
    u32 image_index_ = 0;
    u64 frame_number_ = 0;
};

// Small helper used all over the renderer; pipeline barriers are error-prone enough that
// they deserve one tested place.
void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
                   VkImageLayout new_layout, VkPipelineStageFlags2 src_stage,
                   VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                   VkAccessFlags2 dst_access);

}  // namespace ws
