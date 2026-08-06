#pragma once
// Minimal GPU image wrapper over VMA.
//
// Every allocation the engine makes goes through a pool sized at startup
// (documentation/03-voxel-data-model.md §8). This helper exists so that render targets —
// which are recreated on resize and nothing else — have one obvious place to live.

#include "gpu/device.hpp"

namespace ws {

struct GpuImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    u64 bytes = 0;

    bool valid() const { return image != VK_NULL_HANDLE; }
};

inline GpuImage create_storage_image(const Device& device, u32 width, u32 height,
                                     VkFormat format, const char* debug_name) {
    GpuImage result;
    result.extent = {width, height};
    result.format = format;

    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    alloc.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    VmaAllocationInfo alloc_info{};
    WS_VK(vmaCreateImage(device.allocator(), &info, &alloc, &result.image,
                         &result.allocation, &alloc_info));
    result.bytes = alloc_info.size;

    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = result.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    WS_VK(vkCreateImageView(device.handle(), &view_info, nullptr, &result.view));

    device.set_name(reinterpret_cast<u64>(result.image), VK_OBJECT_TYPE_IMAGE, debug_name);
    return result;
}

inline void destroy_image(const Device& device, GpuImage& image) {
    if (image.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device.handle(), image.view, nullptr);
        image.view = VK_NULL_HANDLE;
    }
    if (image.image != VK_NULL_HANDLE) {
        vmaDestroyImage(device.allocator(), image.image, image.allocation);
        image.image = VK_NULL_HANDLE;
        image.allocation = VK_NULL_HANDLE;
    }
}

}  // namespace ws
