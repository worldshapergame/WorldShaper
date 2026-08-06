#pragma once
// Vulkan instance, physical device selection, logical device and allocator.
//
// Feature floor is Vulkan 1.3 (documentation/08-tech-stack-and-licensing.md §3), chosen
// because the minimum spec is a Steam Deck rather than an old integrated GPU. That buys
// dynamic rendering, synchronization2, timeline semaphores, bindless descriptor indexing
// and 64-bit atomics without any optional-path branching.

#include <string>
#include <vector>

#include "gpu/vulkan_common.hpp"
#include "platform/window.hpp"

namespace ws {

struct DeviceCapabilities {
    std::string name;
    u32 api_version = 0;
    u32 driver_version = 0;
    u32 vendor_id = 0;
    bool discrete = false;
    u64 device_local_bytes = 0;         // total VRAM (or shared pool on a Deck-like part)
    u32 max_workgroup_invocations = 0;
    u32 subgroup_size = 0;
    bool has_ray_query = false;         // present on RDNA2; used only if measured faster
    bool has_int64_atomics = false;
    f32 timestamp_period_ns = 0.0f;
    u64 min_uniform_offset = 256;   // alignment for dynamic uniform buffer offsets
};

class Device {
public:
    Device() = default;
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    // window may be null for headless runs, which skips surface and swapchain support.
    bool create(Window* window, bool enable_validation);
    void destroy();

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physical() const { return physical_; }
    VkDevice handle() const { return device_; }
    VkSurfaceKHR surface() const { return surface_; }
    VmaAllocator allocator() const { return allocator_; }

    VkQueue graphics_queue() const { return graphics_queue_; }
    u32 graphics_family() const { return graphics_family_; }
    VkQueue transfer_queue() const { return transfer_queue_; }
    u32 transfer_family() const { return transfer_family_; }

    const DeviceCapabilities& caps() const { return caps_; }

    void wait_idle() const;

    // Attaches a human-readable name to an object so validation messages and captures in
    // RenderDoc name the thing that actually went wrong.
    void set_name(u64 object, VkObjectType type, const char* name) const;

private:
    bool create_instance(bool enable_validation);
    bool pick_physical_device();
    bool create_logical_device();
    bool create_allocator();
    void query_capabilities();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;

    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    VkQueue transfer_queue_ = VK_NULL_HANDLE;
    u32 graphics_family_ = ~0u;
    u32 transfer_family_ = ~0u;

    Window* window_ = nullptr;
    bool validation_ = false;
    bool debug_utils_ = false;
    DeviceCapabilities caps_;
};

}  // namespace ws
