#include "gpu/screenshot.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_ASSERT(x) ((void)0)
#include <stb_image_write.h>

#include <vector>

namespace ws {

bool save_image_png(Device& device, const GpuImage& image, const std::string& path) {
    if (!image.valid()) return false;

    const u64 bytes = static_cast<u64>(image.extent.width) * image.extent.height * 4;
    GpuBuffer readback = create_staging_buffer(device, bytes, "screenshot readback");

    // One-shot command buffer. Screenshots are rare and off the hot path, so the simple
    // thing is the right thing here.
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = device.graphics_family();
    VkCommandPool pool = VK_NULL_HANDLE;
    WS_VK(vkCreateCommandPool(device.handle(), &pool_info, nullptr, &pool));

    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    WS_VK(vkAllocateCommandBuffers(device.handle(), &alloc, &cmd));

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    WS_VK(vkBeginCommandBuffer(cmd, &begin));

    VkImageMemoryBarrier2 to_src{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    to_src.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    to_src.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    to_src.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_src.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    to_src.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = image.image;
    to_src.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_src.subresourceRange.levelCount = 1;
    to_src.subresourceRange.layerCount = 1;

    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &to_src;
    vkCmdPipelineBarrier2(cmd, &dependency);

    VkBufferImageCopy2 region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {image.extent.width, image.extent.height, 1};

    VkCopyImageToBufferInfo2 copy{VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2};
    copy.srcImage = image.image;
    copy.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    copy.dstBuffer = readback.buffer;
    copy.regionCount = 1;
    copy.pRegions = &region;
    vkCmdCopyImageToBuffer2(cmd, &copy);

    // Put the image back the way we found it: the next frame's barriers expect GENERAL,
    // and a screenshot must not change how the renderer behaves afterwards.
    VkImageMemoryBarrier2 restore = to_src;
    restore.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    restore.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    restore.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    restore.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    restore.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    restore.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    dependency.pImageMemoryBarriers = &restore;
    vkCmdPipelineBarrier2(cmd, &dependency);

    WS_VK(vkEndCommandBuffer(cmd));

    VkCommandBufferSubmitInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmd_info.commandBuffer = cmd;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;
    WS_VK(vkQueueSubmit2(device.graphics_queue(), 1, &submit, VK_NULL_HANDLE));
    device.wait_idle();

    const int written = stbi_write_png(path.c_str(), static_cast<int>(image.extent.width),
                                       static_cast<int>(image.extent.height), 4,
                                       readback.mapped,
                                       static_cast<int>(image.extent.width) * 4);

    vkDestroyCommandPool(device.handle(), pool, nullptr);
    destroy_buffer(device, readback);

    if (written == 0) {
        WS_LOG_ERROR("gpu", "could not write {}", path);
        return false;
    }
    WS_LOG_INFO("gpu", "wrote {} ({}x{})", path, image.extent.width, image.extent.height);
    return true;
}

}  // namespace ws
