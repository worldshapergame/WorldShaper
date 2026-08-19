#pragma once
// The screen the game shows before there is a world to show.
//
// # Why this owns everything it needs
//
// It has to draw at a point in startup when almost nothing exists: the device and the swapchain
// are up and that is all — no render target, no world buffers, no pipelines, no HUD. So it carries
// its own image, its own descriptor set and its own pipeline, and depends on nothing that comes
// after it. That independence is the whole design constraint, and it is what lets the bar cover
// the expensive part of startup rather than appearing after it.
//
// It is also deliberately small. One compute shader with one binding compiles in a few
// milliseconds, which matters when the thing it exists to cover is the wait.
//
// # Why it draws into its own image and blits
//
// Swapchain images are created COLOR_ATTACHMENT | TRANSFER_DST, not STORAGE, so a compute shader
// cannot write to one directly. The real frame already renders to an owned image and blits, so
// this does the same thing, and for the same reason.

#include <filesystem>
#include <string>

#include "gpu/image.hpp"
#include "gpu/shader.hpp"

namespace ws {

class Swapchain;

// Everything the screen needs to know for one frame. Assembled by the caller from a
// LoadProgress::Snapshot, so this file does not need to know what a load stage is.
struct LoadingFrame {
    f32 fraction = 0.0f;         // 0 to 1, of the whole load
    f32 seconds = 0.0f;          // since the load began, for the animation
    u32 stage = 0;               // which mark to draw
    std::string stage_text;      // what is happening, in words
    std::string count_text;      // the same thing as a number the player can watch move
    std::string left_text;       // how much longer, or empty when it is too early to say
    f32 accent[3] = {0.42f, 0.62f, 1.0f};

    // The way in, before the load has finished. 0 is no button at all, which is every frame of
    // every load until the build says there is something to enter; 1 is offered; 2 is under the
    // pointer; 3 is pressed, or taken, and the screen is on its way out.
    u32 button = 0;
    std::string button_text;     // at most eight characters -- see the push block
};

class LoadingScreen {
public:
    ~LoadingScreen();

    LoadingScreen(const LoadingScreen&) = delete;
    LoadingScreen& operator=(const LoadingScreen&) = delete;
    LoadingScreen() = default;

    bool create(Device& device, const std::filesystem::path& shader_source,
                const std::filesystem::path& shader_spirv);
    void destroy();

    bool valid() const { return pipeline_.pipeline() != VK_NULL_HANDLE; }

    // Draws one frame and presents it. Acquires and submits on its own, because at the point this
    // runs there is no frame loop yet to fit into.
    //
    // Returns false when the swapchain could not be acquired, which the caller should treat as
    // "skip this frame", not as an error.
    bool present(Swapchain& swapchain, const LoadingFrame& frame);

    // The image the last frame was drawn into, so `--loading-shot` can photograph it.
    //
    // There is a `--title-shot` and there was nothing for this screen, which meant the one piece
    // of interface a player looks at for ten seconds was the one piece nothing could look at
    // without a person watching. It is the screen's own target rather than the swapchain's, for
    // the same reason the real frame's screenshot path uses the render target: a presented image
    // is owned by the presentation engine by the time anybody wants to read it.
    const GpuImage& image() const { return target_; }

private:
    bool ensure_target(u32 width, u32 height);

    Device* device_ = nullptr;
    ComputePipeline pipeline_;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    GpuImage target_;
    bool target_written_ = false;
};

}  // namespace ws
