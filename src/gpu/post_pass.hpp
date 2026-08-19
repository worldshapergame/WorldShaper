#pragma once
// R6a and R6b: post, as a pass of its own.
//
// # Why it is a pass and not more of the composite
//
// The composite is per pixel and knows nothing outside its own invocation. Glare is the opposite
// shape of problem: it is a neighbourhood, and a neighbourhood is either a gather -- which is what
// `shaders/pt_post.glsl` used to do, at up to 437 taps a pixel at 1440p, straight out of the
// accumulation image -- or a chain of halvings. A chain cannot live inside a per-pixel shader,
// because every level has to be finished before the next one reads it.
//
// So: three shaders and one full-resolution scratch image.
//
//   `bloom_down.comp`  the composite -> half -> quarter -> ... , thresholded on the first step
//   `bloom_up.comp`    back up, each level adding itself into the one above it
//   `post.comp`        the shutter (R6b), then the combine that adds the glare and presents
//
// The reach comes from the number of levels rather than from a radius, so the taps per pixel are
// the same at 1280x800 and at 4K and the whole stage is a small multiple of one full-resolution
// read. `documentation/21-renderer-rewrite.md` puts the gate at **post <= 1.0 ms at 4K**.
//
// # What the scratch image is for
//
// The shutter reads its neighbours. A dispatch that read and wrote one image would have each
// invocation reading whichever of its neighbours happened to have run already -- which does not
// fail, it produces a picture that is part blurred and part not, differently on every card. So the
// shutter writes elsewhere and the combine reads what it wrote. When the shutter does not run the
// combine touches only its own pixel, and then it is in place and the scratch is not read at all.
//
// # The control arms
//
// `--no-bloom` and `--no-motion-blur` each restore the old picture by NOT RECORDING the work,
// rather than by a flag inside a shader that could drift from the path beside it. With both off
// this pass issues no command at all and the frame is the one the composite wrote, to the bit.

#include <filesystem>
#include <vector>

#include "gpu/device.hpp"
#include "gpu/image.hpp"
#include "gpu/profiler.hpp"
#include "gpu/shader.hpp"

namespace ws {

// What one frame of post is asked for. Both default to on; the two flags turn them off.
struct PostSettings {
    bool bloom = true;
    bool motion_blur = true;
};

class PostPass {
public:
    // The most levels the chain is ever built with. Eight halvings of a 4K frame reaches a level
    // eight pixels tall, which is the point at which another one stops adding reach and starts
    // adding a dispatch that covers one workgroup.
    static constexpr u32 kMaxLevels = 8;

    PostPass() = default;
    ~PostPass();
    PostPass(const PostPass&) = delete;
    PostPass& operator=(const PostPass&) = delete;

    bool create(Device& device, const std::filesystem::path& source_dir,
                const std::filesystem::path& spirv_dir);
    void destroy();
    bool valid() const { return post_.pipeline() != VK_NULL_HANDLE; }

    // Same hot reload as every other pass: glare is tuned by looking at it.
    void reload_if_changed();
    void force_reload();

    // The whole stage, into a command buffer that has already recorded the composite.
    //
    // `colour` is the render target: in GENERAL, read and written. `visibility` is what the
    // primary ray hit, which is the only thing the shutter needs that a colour image cannot tell
    // it. `params` is the shared per-frame block, bound with the same dynamic offset the composite
    // used. Records nothing when neither half is asked for.
    void record(VkCommandBuffer cmd, GpuProfiler& profiler, const GpuImage& colour,
                const GpuImage& visibility, VkBuffer params, u32 params_offset,
                const PostSettings& settings);

    u32 levels() const { return static_cast<u32>(chain_.size()); }

private:
    bool build_layouts();
    // Sizes the chain to the frame. Cheap and does nothing when neither the extent nor the images
    // it points at have moved.
    bool ensure(const GpuImage& colour, const GpuImage& visibility, VkBuffer params);
    void release_images();

    Device* device_ = nullptr;
    ComputePipeline down_;
    ComputePipeline up_;
    ComputePipeline post_;

    VkDescriptorSetLayout down_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout up_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout post_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;

    std::vector<GpuImage> chain_;   // [0] is half resolution, each below it half again
    GpuImage scratch_;              // full resolution, what the shutter writes

    // [0] composite -> chain[0]; [1] scratch -> chain[0]; [2 + i] chain[i] -> chain[i + 1].
    VkDescriptorSet down_sets_[kMaxLevels + 1]{};
    VkDescriptorSet up_sets_[kMaxLevels]{};
    // 0 the shutter (composite -> scratch), 1 the combine after it (scratch -> composite),
    // 2 the combine with no shutter (composite -> composite).
    VkDescriptorSet post_sets_[3]{};

    VkImageView bound_colour_ = VK_NULL_HANDLE;
    VkImageView bound_visibility_ = VK_NULL_HANDLE;
    VkBuffer bound_params_ = VK_NULL_HANDLE;
};

}  // namespace ws
