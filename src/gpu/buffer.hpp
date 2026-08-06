#pragma once
// GPU buffer helper.
//
// Same rule as images (gpu/image.hpp): every buffer is created once at startup from a
// budget and never resized during play. Running out is an eviction, never a stall.

#include "gpu/device.hpp"

namespace ws {

struct GpuBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    void* mapped = nullptr;      // non-null only for host-visible buffers
    u64 size = 0;

    bool valid() const { return buffer != VK_NULL_HANDLE; }
};

// Device-local storage the shaders read. Written only through staging copies.
inline GpuBuffer create_device_buffer(const Device& device, u64 size, VkBufferUsageFlags usage,
                                      const char* debug_name) {
    GpuBuffer out;
    out.size = size;

    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    WS_VK(vmaCreateBuffer(device.allocator(), &info, &alloc, &out.buffer, &out.allocation,
                          nullptr));

    VkBufferDeviceAddressInfo address_info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    address_info.buffer = out.buffer;
    out.address = vkGetBufferDeviceAddress(device.handle(), &address_info);

    device.set_name(reinterpret_cast<u64>(out.buffer), VK_OBJECT_TYPE_BUFFER, debug_name);
    return out;
}

// Host-visible, persistently mapped. Used for staging; written by the CPU every frame.
inline GpuBuffer create_staging_buffer(const Device& device, u64 size, const char* debug_name,
                                       VkBufferUsageFlags extra_usage = 0) {
    GpuBuffer out;
    out.size = size;

    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    // Both directions: staging uploads read from it, screenshot readback writes into it.
    // `extra_usage` covers host-visible buffers the shaders read directly, such as the
    // per-frame parameter block.
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 extra_usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                  VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                  VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo alloc_info{};
    WS_VK(vmaCreateBuffer(device.allocator(), &info, &alloc, &out.buffer, &out.allocation,
                          &alloc_info));
    out.mapped = alloc_info.pMappedData;

    device.set_name(reinterpret_cast<u64>(out.buffer), VK_OBJECT_TYPE_BUFFER, debug_name);
    return out;
}

inline void destroy_buffer(const Device& device, GpuBuffer& buffer) {
    if (buffer.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(device.allocator(), buffer.buffer, buffer.allocation);
        buffer.buffer = VK_NULL_HANDLE;
        buffer.allocation = VK_NULL_HANDLE;
        buffer.mapped = nullptr;
        buffer.size = 0;
    }
}

}  // namespace ws
