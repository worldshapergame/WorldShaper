#pragma once
// The interface, on the card.
//
// One surface, three shaders and four buffers. It has two jobs, and they are the same job:
//
//   **The title**, where there is no world at all — no pipelines, no residency, no bricks. It
//   draws its own room (shaders/title_room.comp), blurs it for the glass, draws the marks over it
//   and presents. This is what the game opens on, and it is up in the time it takes to open a
//   window because nothing above the device and the swapchain exists yet (D441).
//
//   **In the world**, where the renderer has already produced a frame. The frame is blitted into
//   this surface at the window's own resolution — which is also where a scaled render is put back
//   to full size — the same blur runs, the same marks are drawn, and the result goes to the
//   swapchain. Drawing into the render target instead would put the interface through the upscale
//   with it, and a three-pixel-wide letter does not survive being stretched.
//
// The cost is the reason it is shaped this way. `documentation/09-performance-budgets.md` gives
// the whole interface 0.6 ms on a T0 machine, and the marks pass only ever dispatches over the
// tiles the host says have something in them — so an interface with nothing open costs one blur it
// does not run and one dispatch it does not make.

#include <filesystem>

#include "gpu/buffer.hpp"
#include "gpu/device.hpp"
#include "gpu/image.hpp"
#include "gpu/shader.hpp"
#include "gpu/swapchain.hpp"
#include "ui/draw.hpp"

namespace ws {

class ShellPass {
public:
    ~ShellPass();

    ShellPass() = default;
    ShellPass(const ShellPass&) = delete;
    ShellPass& operator=(const ShellPass&) = delete;

    bool create(Device& device, const std::filesystem::path& source_dir,
                const std::filesystem::path& spirv_dir);
    void destroy();
    bool valid() const { return marks_.pipeline() != VK_NULL_HANDLE; }

    // The surface everything lands on. Sized to the window, not to the render scale.
    bool ensure(u32 width, u32 height);
    const GpuImage& surface() const { return surface_; }

    // Writes this frame's marks into the slot the swapchain is about to record. Call before
    // recording; `slot` must be the frame index, so that writing next frame's list cannot disturb
    // the one the card is still reading.
    void upload(const ui::DrawList& list, u32 slot);

    // --- recording ------------------------------------------------------------------------------
    //
    // Each of these leaves the surface in GENERAL and expects it there, except `blit_in`, which
    // takes a frame from elsewhere.

    // The title's own room, when there is no world to be the backdrop.
    void draw_room(VkCommandBuffer cmd, f32 seconds);
    // A rendered frame, scaled into the surface. `source` must be in TRANSFER_SRC.
    void blit_in(VkCommandBuffer cmd, VkImage source, VkExtent2D source_extent);
    // The glass, then the marks. Does nothing at all when the list is empty.
    void draw(VkCommandBuffer cmd, const ui::DrawList& list, u32 slot, const f32 accent[3],
              f32 seconds);
    // The surface to the swapchain image, one to one.
    void blit_out(VkCommandBuffer cmd, VkImage destination, VkExtent2D extent);

    // The whole of a title frame, for the loop that has nothing else to record: acquire, room,
    // marks, present. Returns false when the swapchain would not give an image, which the caller
    // treats as "try again next frame".
    bool present(Swapchain& swapchain, const ui::DrawList& list, const f32 accent[3], f32 seconds);

    // Whether the shaders changed on disk. Same hot reload as every other pass: the interface is
    // tuned by looking at it, which is exactly the argument gpu/shader.hpp makes.
    void reload_if_changed();

private:
    bool build_layouts();
    void write_descriptors(u32 slot);
    void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to);

    struct Slot {
        GpuBuffer commands;
        GpuBuffer characters;
        GpuBuffer tiles;
        GpuBuffer indices;
        VkDescriptorSet set = VK_NULL_HANDLE;
        u32 tiles_across = 0;
        u32 tiles_down = 0;
        u32 command_count = 0;
        ui::Rect bounds{};
    };

    Device* device_ = nullptr;
    ComputePipeline marks_;
    ComputePipeline blur_;
    ComputePipeline room_;

    VkDescriptorSetLayout marks_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout blur_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout room_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;

    GpuImage surface_;         // the window-sized composite, in encoded units
    GpuImage half_;            // quarter resolution, the downsample and the second blur pass
    GpuImage quarter_;         // and the one between them
    bool surface_written_ = false;

    VkDescriptorSet room_set_ = VK_NULL_HANDLE;
    VkDescriptorSet blur_sets_[3]{};   // surface->half, half->quarter, quarter->half

    Slot slots_[kFramesInFlight];
    u32 frame_ = 0;
};

}  // namespace ws
