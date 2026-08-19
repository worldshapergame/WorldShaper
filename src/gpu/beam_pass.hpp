#pragma once
// R7a and R7b -- the beam pre-pass, on the host side.
//
// One coarse ray per 8x8 tile corner marches the octree and records how far it got before it could
// have hit anything; every full-resolution ray in the tile then starts from that distance instead
// of from the camera. `documentation/04-rendering.md` §1 has asked for this since Stage 3.
//
// What is here is deliberately only the plumbing: an image a sixty-fourth of the screen, a pipeline
// that shares the marcher's descriptor set, and one dispatch. All of the arithmetic -- the cone
// width, the relief, where a corner is -- lives in `shaders/beam.glsl`, because the primary ray
// reads the same numbers and two copies of them would eventually disagree.
//
// It shares `node_layout_` rather than owning a set, for the reason main.cpp already gives about
// the face shader: a beam ray marches exactly the geometry the primary ray marches, and a second
// set would be a second place for the same twenty-two buffers to be bound.

#include <filesystem>

#include "gpu/device.hpp"
#include "gpu/image.hpp"
#include "gpu/shader.hpp"

namespace ws {

// Pixels a tile, each way. Must match kBeamTile in shaders/beam.glsl.
inline constexpr u32 kBeamTile = 8;

// Where the corner grid binds on the marcher's set. The set runs 0..26 already; this is the next.
inline constexpr u32 kBeamBinding = 27;

class BeamPass {
public:
    BeamPass() = default;
    ~BeamPass();

    BeamPass(const BeamPass&) = delete;
    BeamPass& operator=(const BeamPass&) = delete;

    // The pipeline. `set_layout` is the marcher's, and `push_constant_bytes` is the marcher's
    // block: every pipeline that includes node.glsl declares the same one, and a stage gets one.
    bool create(Device& device, const std::filesystem::path& source_path,
                const std::filesystem::path& spirv_path, VkDescriptorSetLayout set_layout,
                u32 push_constant_bytes);
    void destroy();
    bool valid() const { return pipeline_.pipeline() != VK_NULL_HANDLE; }
    const std::string& last_error() const { return pipeline_.last_error(); }

    // The corner grid, sized for a render target of `width` x `height`, and bound into `set`.
    //
    // One more corner than there are tiles each way, so the last tile has a right edge and a bottom
    // edge to read. Idempotent: called again with the same size it does nothing, which is what lets
    // both the render-target path and the descriptor-set path call it without either having to know
    // whether the other has run yet.
    void ensure(Device& device, VkDescriptorSet set, u32 width, u32 height);

    // The dispatch. Leaves the corner image written and a barrier behind it, so the primary pass
    // that follows reads what this wrote.
    //
    // `extent` is the render extent, not the image's: the corner grid is allocated for the largest
    // the target has been and only the part covering this frame is filled, exactly as the marcher
    // dispatches over the render extent rather than over the image.
    void record(VkCommandBuffer cmd, VkDescriptorSet set, u32 params_offset, const void* push,
                u32 push_bytes, VkExtent2D extent);

    // Corners each way for a render extent, which is also what the dispatch covers.
    static u32 corners_across(u32 pixels) { return (pixels + kBeamTile - 1) / kBeamTile + 1; }

    const GpuImage& image() const { return image_; }
    // What one frame of it moves, for the profiler's bandwidth column: the corner grid written,
    // plus the previous frame's depth read once per pixel by R7b's block minimum.
    static u64 bytes(u32 width, u32 height) {
        return static_cast<u64>(corners_across(width)) * corners_across(height) * 4ull +
               static_cast<u64>(width) * height * 4ull;
    }

    bool reload_if_changed() { return pipeline_.reload_if_changed(); }
    bool force_reload() { return pipeline_.force_reload(); }

private:
    Device* device_ = nullptr;
    ComputePipeline pipeline_;
    GpuImage image_;
    bool transitioned_ = false;
};

}  // namespace ws
