#include "gpu/beam_pass.hpp"

#include "core/log.hpp"
#include "gpu/swapchain.hpp"

namespace ws {

BeamPass::~BeamPass() { destroy(); }

bool BeamPass::create(Device& device, const std::filesystem::path& source_path,
                      const std::filesystem::path& spirv_path, VkDescriptorSetLayout set_layout,
                      u32 push_constant_bytes) {
    device_ = &device;
    return pipeline_.create(device, source_path, spirv_path, set_layout, push_constant_bytes);
}

void BeamPass::destroy() {
    pipeline_.destroy();
    if (device_ != nullptr && image_.valid()) destroy_image(*device_, image_);
    transitioned_ = false;
    device_ = nullptr;
}

void BeamPass::ensure(Device& device, VkDescriptorSet set, u32 width, u32 height) {
    device_ = &device;
    const u32 across = corners_across(width);
    const u32 down = corners_across(height);
    if (image_.valid() && image_.extent.width == across && image_.extent.height == down) {
        // Still the right size, but the SET may be a different one -- the render target is created
        // before the descriptor sets exist on the first pass through startup, so the binding has to
        // be written again when they do. Writing a descriptor that already holds this view is free.
        if (set != VK_NULL_HANDLE) {
            VkDescriptorImageInfo info{};
            info.imageView = image_.view;
            info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = set;
            write.dstBinding = kBeamBinding;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            write.pImageInfo = &info;
            vkUpdateDescriptorSets(device.handle(), 1, &write, 0, nullptr);
        }
        return;
    }

    if (image_.valid()) destroy_image(device, image_);
    // R32_SFLOAT, one texel a corner: a distance in voxels from the eye, which is the unit
    // everything in node.glsl is measured in. A sixty-fourth of the screen and change -- 480 x 271
    // texels at 4K, half a megabyte.
    image_ = create_storage_image(device, across, down, VK_FORMAT_R32_SFLOAT, "beam start");
    transitioned_ = false;

    if (set != VK_NULL_HANDLE) {
        VkDescriptorImageInfo info{};
        info.imageView = image_.view;
        info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.dstBinding = kBeamBinding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.pImageInfo = &info;
        vkUpdateDescriptorSets(device.handle(), 1, &write, 0, nullptr);
    }
}

void BeamPass::record(VkCommandBuffer cmd, VkDescriptorSet set, u32 params_offset,
                      const void* push, u32 push_bytes, VkExtent2D extent) {
    if (!valid() || !image_.valid()) return;

    // Every corner the dispatch covers is written by it, so the old contents are of no interest
    // and the transition may discard them -- but only the first time, because a layout of
    // UNDEFINED is a licence to throw away what is there and after the first frame that would be
    // a licence taken every frame for no reason. After that it is GENERAL to GENERAL, carrying
    // the dependency from the previous frame's primary pass, which read this image.
    image_barrier(cmd, image_.image,
                  transitioned_ ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_GENERAL,
                  transitioned_ ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                  transitioned_ ? VK_ACCESS_2_SHADER_READ_BIT : 0,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
    transitioned_ = true;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.pipeline());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.layout(), 0, 1, &set, 1,
                            &params_offset);
    vkCmdPushConstants(cmd, pipeline_.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes, push);

    const u32 across = corners_across(extent.width);
    const u32 down = corners_across(extent.height);
    vkCmdDispatch(cmd, (across + 7) / 8, (down + 7) / 8, 1);

    // Two hazards in one barrier, and the second is the one that is easy to miss.
    //
    //   The corner grid this just wrote is read by the primary pass that follows.
    //   The DEPTH image this just READ is overwritten by that same primary pass -- a write after a
    //   read, which needs the dependency as much as the other way round does. Without it the
    //   temporal term is a race against the frame it is meant to be one behind.
    //
    // A memory barrier rather than two image barriers: neither image changes layout, both are
    // compute to compute, and the two are one dependency.
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

}  // namespace ws
