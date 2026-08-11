#include "gpu/screenshot.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_ASSERT(x) ((void)0)
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/log.hpp"

namespace ws {
namespace {

// Half precision back to a float. Written out rather than pulled in, because it is nine lines and
// the alternative is a dependency for nine lines.
f32 from_half(u16 bits) {
    const u32 sign = static_cast<u32>(bits & 0x8000u) << 16;
    const u32 exponent = (bits >> 10) & 0x1Fu;
    const u32 mantissa = bits & 0x3FFu;
    u32 out = 0;
    if (exponent == 0) {
        // Subnormal, or zero. Rare enough in a picture that the simple branch is the right one.
        if (mantissa != 0) {
            f32 value = static_cast<f32>(mantissa) / 1024.0f / 16384.0f;
            u32 packed = 0;
            std::memcpy(&packed, &value, sizeof(packed));
            out = packed;
        }
    } else if (exponent == 31) {
        out = 0x7F800000u | (mantissa << 13);
    } else {
        out = ((exponent + 112) << 23) | (mantissa << 13);
    }
    out |= sign;
    f32 value = 0.0f;
    std::memcpy(&value, &out, sizeof(value));
    return value;
}

// How many bytes a pixel of this format is, and whether it needs converting on the way out.
//
// Two formats reach here and they are both pictures somebody wants to look at: the renderer's
// eight-bit target, and the sixteen-bit float surfaces the loading screen and the interface
// compose into. Reading the second one as though it were the first is not a subtle failure — it
// is a magenta and blue tartan, which is exactly what it looked like the first time the title was
// photographed.
u32 pixel_bytes(VkFormat format) {
    return (format == VK_FORMAT_R16G16B16A16_SFLOAT) ? 8u : 4u;
}

}  // namespace

bool save_image_png(Device& device, const GpuImage& image, const std::string& path) {
    if (!image.valid()) return false;

    const u64 bytes =
        static_cast<u64>(image.extent.width) * image.extent.height * pixel_bytes(image.format);
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

    // A sixteen-bit float surface is already in ENCODED units — the ink rule and the tone curve
    // both work there — so this is a range conversion and not a tone map. Anything else here
    // would make a photograph of the interface disagree with the interface.
    std::vector<u8> converted;
    const void* pixels = readback.mapped;
    if (pixel_bytes(image.format) == 8) {
        const u64 count = static_cast<u64>(image.extent.width) * image.extent.height * 4;
        converted.resize(static_cast<usize>(count));
        const u16* source = static_cast<const u16*>(readback.mapped);
        for (u64 i = 0; i < count; ++i) {
            const f32 value = std::clamp(from_half(source[i]), 0.0f, 1.0f);
            converted[static_cast<usize>(i)] = static_cast<u8>(value * 255.0f + 0.5f);
        }
        pixels = converted.data();
    }

    const int written = stbi_write_png(path.c_str(), static_cast<int>(image.extent.width),
                                       static_cast<int>(image.extent.height), 4, pixels,
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
