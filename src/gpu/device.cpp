#include "gpu/device.hpp"

#include <algorithm>
#include <cstring>

namespace ws {

const char* vk_result_name(VkResult result) {
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
        default: return "VK_ERROR_<unmapped>";
    }
}

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*user*/) {
    if (data == nullptr || data->pMessage == nullptr) return VK_FALSE;

    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        WS_LOG_ERROR("vulkan", "{}", data->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        WS_LOG_WARN("vulkan", "{}", data->pMessage);
    } else {
        WS_LOG_DEBUG("vulkan", "{}", data->pMessage);
    }
    return VK_FALSE;
}

bool has_extension(const std::vector<VkExtensionProperties>& list, const char* name) {
    return std::any_of(list.begin(), list.end(), [name](const VkExtensionProperties& e) {
        return std::strcmp(e.extensionName, name) == 0;
    });
}

bool has_layer(const std::vector<VkLayerProperties>& list, const char* name) {
    return std::any_of(list.begin(), list.end(), [name](const VkLayerProperties& l) {
        return std::strcmp(l.layerName, name) == 0;
    });
}

}  // namespace

// The one device, so that WS_VK can ask it what happened without every call site holding a
// pointer to it. There is exactly one for the life of the process.
namespace {
const Device* g_device_for_lost_report = nullptr;
}

void report_device_lost() {
    if (g_device_for_lost_report != nullptr) g_device_for_lost_report->log_device_lost();
}

Device::~Device() { destroy(); }

bool Device::create(Window* window, bool enable_validation) {
    window_ = window;

    if (volkInitialize() != VK_SUCCESS) {
        WS_LOG_FATAL("gpu",
                     "no Vulkan loader found. Install up-to-date graphics drivers.");
        return false;
    }

    if (!create_instance(enable_validation)) return false;

    if (window_ != nullptr) {
        void* raw_surface = nullptr;
        if (!window_->create_vulkan_surface(instance_, &raw_surface)) return false;
        surface_ = static_cast<VkSurfaceKHR>(raw_surface);
    }

    if (!pick_physical_device()) return false;
    if (!create_logical_device()) return false;
    if (!create_allocator()) return false;

    query_capabilities();
    WS_LOG_INFO("gpu", "{} | Vulkan {}.{}.{} | {} MB device-local | subgroup {}",
                caps_.name, VK_API_VERSION_MAJOR(caps_.api_version),
                VK_API_VERSION_MINOR(caps_.api_version),
                VK_API_VERSION_PATCH(caps_.api_version),
                caps_.device_local_bytes / (1024 * 1024), caps_.subgroup_size);
    return true;
}

bool Device::create_instance(bool enable_validation) {
    u32 available_layer_count = 0;
    vkEnumerateInstanceLayerProperties(&available_layer_count, nullptr);
    std::vector<VkLayerProperties> available_layers(available_layer_count);
    vkEnumerateInstanceLayerProperties(&available_layer_count, available_layers.data());

    u32 available_extension_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &available_extension_count, nullptr);
    std::vector<VkExtensionProperties> available_extensions(available_extension_count);
    vkEnumerateInstanceExtensionProperties(nullptr, &available_extension_count,
                                           available_extensions.data());

    std::vector<const char*> extensions;
    if (window_ != nullptr) {
        u32 sdl_count = 0;
        const char* const* sdl_extensions = Window::required_vulkan_extensions(sdl_count);
        for (u32 i = 0; i < sdl_count; ++i) extensions.push_back(sdl_extensions[i]);
    }

    debug_utils_ = has_extension(available_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (debug_utils_) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    std::vector<const char*> layers;
    validation_ = enable_validation && has_layer(available_layers, "VK_LAYER_KHRONOS_validation");
    if (validation_) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
    } else if (enable_validation) {
        WS_LOG_WARN("gpu", "validation layers requested but not installed; continuing without");
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "WorldShaper";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "WorldShaper";
    app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<u32>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.enabledLayerCount = static_cast<u32>(layers.size());
    info.ppEnabledLayerNames = layers.data();

    VkDebugUtilsMessengerCreateInfoEXT debug{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug.pfnUserCallback = debug_callback;
    if (validation_) info.pNext = &debug;

    const VkResult result = vkCreateInstance(&info, nullptr, &instance_);
    if (result != VK_SUCCESS) {
        WS_LOG_FATAL("gpu", "vkCreateInstance failed: {}. A Vulkan 1.3 driver is required.",
                     vk_result_name(result));
        return false;
    }

    volkLoadInstanceOnly(instance_);

    if (validation_ && debug_utils_) {
        WS_VK(vkCreateDebugUtilsMessengerEXT(instance_, &debug, nullptr, &messenger_));
    }
    return true;
}

bool Device::pick_physical_device() {
    u32 count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) {
        WS_LOG_FATAL("gpu", "no Vulkan-capable GPU found");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    i64 best_score = -1;
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(candidate, &props);

        if (props.apiVersion < VK_API_VERSION_1_3) {
            WS_LOG_DEBUG("gpu", "skipping {}: reports Vulkan {}.{}, need 1.3",
                         props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
                         VK_API_VERSION_MINOR(props.apiVersion));
            continue;
        }

        // Must have one queue family that can do graphics, compute and present. Every
        // target GPU has this; splitting them would complicate every submission path.
        u32 family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());

        bool usable = false;
        for (u32 i = 0; i < family_count; ++i) {
            const bool gfx = (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            const bool comp = (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
            bool present = (surface_ == VK_NULL_HANDLE);
            if (surface_ != VK_NULL_HANDLE) {
                VkBool32 supported = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface_, &supported);
                present = (supported == VK_TRUE);
            }
            if (gfx && comp && present) {
                usable = true;
                break;
            }
        }
        if (!usable) continue;

        i64 score = 0;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 100000;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 10000;
        score += props.limits.maxComputeSharedMemorySize / 1024;

        if (score > best_score) {
            best_score = score;
            physical_ = candidate;
        }
    }

    if (physical_ == VK_NULL_HANDLE) {
        WS_LOG_FATAL("gpu",
                     "no GPU supports Vulkan 1.3 with graphics+compute+present. Update "
                     "your graphics drivers.");
        return false;
    }
    return true;
}

bool Device::create_logical_device() {
    u32 family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, families.data());

    for (u32 i = 0; i < family_count; ++i) {
        const bool gfx = (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        const bool comp = (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
        bool present = (surface_ == VK_NULL_HANDLE);
        if (surface_ != VK_NULL_HANDLE) {
            VkBool32 supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(physical_, i, surface_, &supported);
            present = (supported == VK_TRUE);
        }
        if (gfx && comp && present && graphics_family_ == ~0u) graphics_family_ = i;

        // A transfer-only family lets streaming uploads overlap rendering instead of
        // serialising behind it (documentation/09 §2, "Streaming upload" budget).
        const bool transfer_only = (families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
                                   !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                                   !(families[i].queueFlags & VK_QUEUE_COMPUTE_BIT);
        if (transfer_only && transfer_family_ == ~0u) transfer_family_ = i;
    }

    WS_CHECK(graphics_family_ != ~0u, "no suitable queue family");

    const f32 priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    VkDeviceQueueCreateInfo gfx_queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    gfx_queue.queueFamilyIndex = graphics_family_;
    gfx_queue.queueCount = 1;
    gfx_queue.pQueuePriorities = &priority;
    queue_infos.push_back(gfx_queue);

    if (transfer_family_ != ~0u) {
        VkDeviceQueueCreateInfo transfer_queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        transfer_queue.queueFamilyIndex = transfer_family_;
        transfer_queue.queueCount = 1;
        transfer_queue.pQueuePriorities = &priority;
        queue_infos.push_back(transfer_queue);
    }

    // Feature floor. Anything here that a device lacks means we refuse to run rather than
    // silently taking a slow path — see documentation/08 §3.
    VkPhysicalDeviceVulkan13Features features13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;
    features13.maintenance4 = VK_TRUE;

    VkPhysicalDeviceVulkan12Features features12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features12.pNext = &features13;
    features12.timelineSemaphore = VK_TRUE;
    features12.bufferDeviceAddress = VK_TRUE;
    features12.descriptorIndexing = VK_TRUE;
    features12.runtimeDescriptorArray = VK_TRUE;
    features12.descriptorBindingPartiallyBound = VK_TRUE;
    features12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    features12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    features12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
    features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    features12.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
    features12.storageBuffer8BitAccess = VK_TRUE;
    features12.uniformAndStorageBuffer8BitAccess = VK_TRUE;
    features12.shaderInt8 = VK_TRUE;
    features12.scalarBlockLayout = VK_TRUE;
    features12.hostQueryReset = VK_TRUE;

    VkPhysicalDeviceVulkan11Features features11{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    features11.pNext = &features12;
    features11.storageBuffer16BitAccess = VK_TRUE;
    features11.uniformAndStorageBuffer16BitAccess = VK_TRUE;

    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features2.pNext = &features11;
    features2.features.shaderInt64 = VK_TRUE;
    features2.features.shaderInt16 = VK_TRUE;
    features2.features.samplerAnisotropy = VK_TRUE;
    features2.features.fillModeNonSolid = VK_TRUE;
    features2.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;

    std::vector<const char*> device_extensions;
    if (surface_ != VK_NULL_HANDLE) {
        device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    // Promoted into Vulkan 1.3, but Dear ImGui's backend still asks for it by name, and
    // enabling a promoted extension is free on any device that reports 1.3.
    device_extensions.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);

    // Diagnostics for a lost device. Both are optional and cost nothing when the GPU is
    // healthy: checkpoints are a marker write per pass, and the fault extension is only ever
    // read after the device has already died.
    //
    // Worth having enabled always rather than behind a flag. A lost device is exactly the
    // failure a player cannot reproduce on demand, so the one time it happens has to be the
    // time it is recorded.
    u32 available_count = 0;
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &available_count, nullptr);
    std::vector<VkExtensionProperties> available(available_count);
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &available_count, available.data());

    checkpoints_ = has_extension(available, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
    if (checkpoints_) {
        device_extensions.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
    }

    VkPhysicalDeviceFaultFeaturesEXT fault_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    device_fault_ = has_extension(available, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
    if (device_fault_) {
        device_extensions.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
        fault_features.deviceFault = VK_TRUE;
        fault_features.pNext = features2.pNext;
        features2.pNext = &fault_features;
    }

    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = &features2;
    info.queueCreateInfoCount = static_cast<u32>(queue_infos.size());
    info.pQueueCreateInfos = queue_infos.data();
    info.enabledExtensionCount = static_cast<u32>(device_extensions.size());
    info.ppEnabledExtensionNames = device_extensions.data();

    const VkResult result = vkCreateDevice(physical_, &info, nullptr, &device_);
    if (result != VK_SUCCESS) {
        WS_LOG_FATAL("gpu",
                     "vkCreateDevice failed: {}. The GPU is missing a required Vulkan 1.3 "
                     "feature.",
                     vk_result_name(result));
        return false;
    }

    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, graphics_family_, 0, &graphics_queue_);
    if (transfer_family_ != ~0u) {
        vkGetDeviceQueue(device_, transfer_family_, 0, &transfer_queue_);
    } else {
        transfer_queue_ = graphics_queue_;
        transfer_family_ = graphics_family_;
    }

    // The extension being advertised is not the same as its entry points being loadable, and
    // the difference is only discovered at the worst possible moment. Settle it here, once,
    // while everything still works.
    checkpoints_ = checkpoints_ && vkCmdSetCheckpointNV != nullptr &&
                   vkGetQueueCheckpointDataNV != nullptr;
    device_fault_ = device_fault_ && vkGetDeviceFaultInfoEXT != nullptr;

    g_device_for_lost_report = this;
    WS_LOG_INFO("gpu", "device-lost diagnostics: checkpoints {}, fault info {}",
                checkpoints_ ? "yes" : "no", device_fault_ ? "yes" : "no");
    return true;
}

void Device::log_device_lost() const {
    WS_LOG_ERROR("gpu", "device lost. Asking the driver what it was doing.");

    // Which passes the GPU had actually reached. Markers are recorded at the top and tail of
    // every pass, so the last TOP_OF_PIPE marker without its matching BOTTOM_OF_PIPE is the
    // pass that was still running — which is the pass that killed it.
    // Every one of these is null-checked, and that is not defensive habit — it is a bug this
    // code already caused. vkGetQueueCheckpointData2NV is the revision-2 entry point and volk
    // leaves it null on a driver that only exposes revision 1, so calling it turned a lost
    // device into "execute at 0x0" with an empty stack. The crash handler destroyed the very
    // report it exists to write. Anything reached only while the GPU is already dead has to
    // assume it is the last code that will run.
    if (checkpoints_ && graphics_queue_ != VK_NULL_HANDLE && vkGetQueueCheckpointDataNV) {
        u32 count = 0;
        vkGetQueueCheckpointDataNV(graphics_queue_, &count, nullptr);
        if (count == 0) {
            WS_LOG_ERROR("gpu", "  no checkpoints reported");
        } else {
            std::vector<VkCheckpointDataNV> data(
                count, VkCheckpointDataNV{VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV});
            vkGetQueueCheckpointDataNV(graphics_queue_, &count, data.data());
            for (const VkCheckpointDataNV& point : data) {
                const char* marker = static_cast<const char*>(point.pCheckpointMarker);
                WS_LOG_ERROR("gpu", "  checkpoint stage 0x{:08X}: {}",
                             static_cast<u32>(point.stage), marker ? marker : "(null)");
            }
        }
    } else {
        WS_LOG_ERROR("gpu", "  no checkpoint support on this driver");
    }

    // Page faults and their addresses, where the driver will say. This distinguishes the two
    // things a lost device usually is: a shader reading out of bounds, or a dispatch that ran
    // long enough for the operating system to reset the GPU underneath it.
    if (device_fault_ && device_ != VK_NULL_HANDLE && vkGetDeviceFaultInfoEXT) {
        VkDeviceFaultCountsEXT counts{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
        if (vkGetDeviceFaultInfoEXT(device_, &counts, nullptr) == VK_SUCCESS) {
            std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
            std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
            std::vector<u8> binary(static_cast<usize>(counts.vendorBinarySize));

            VkDeviceFaultInfoEXT fault{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
            fault.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
            fault.pVendorInfos = vendors.empty() ? nullptr : vendors.data();
            fault.pVendorBinaryData = binary.empty() ? nullptr : binary.data();
            if (vkGetDeviceFaultInfoEXT(device_, &counts, &fault) == VK_SUCCESS) {
                WS_LOG_ERROR("gpu", "  driver says: {}", fault.description);
                for (u32 i = 0; i < counts.addressInfoCount; ++i) {
                    WS_LOG_ERROR("gpu", "  fault type {} at 0x{:016X} (precision {} bytes)",
                                 static_cast<u32>(addresses[i].addressType),
                                 static_cast<u64>(addresses[i].reportedAddress),
                                 static_cast<u64>(addresses[i].addressPrecision));
                }
                for (u32 i = 0; i < counts.vendorInfoCount; ++i) {
                    WS_LOG_ERROR("gpu", "  vendor: {} (code {}, data {})",
                                 vendors[i].description,
                                 static_cast<u64>(vendors[i].vendorFaultCode),
                                 static_cast<u64>(vendors[i].vendorFaultData));
                }
            }
        }
    } else {
        WS_LOG_ERROR("gpu", "  no fault-info support on this driver");
    }
}

bool Device::create_allocator() {
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo info{};
    info.instance = instance_;
    info.physicalDevice = physical_;
    info.device = device_;
    info.vulkanApiVersion = VK_API_VERSION_1_3;
    info.pVulkanFunctions = &functions;
    info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;

    const VkResult result = vmaCreateAllocator(&info, &allocator_);
    if (result != VK_SUCCESS) {
        WS_LOG_FATAL("gpu", "vmaCreateAllocator failed: {}", vk_result_name(result));
        return false;
    }
    return true;
}

void Device::query_capabilities() {
    VkPhysicalDeviceSubgroupProperties subgroup{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &subgroup;
    vkGetPhysicalDeviceProperties2(physical_, &props2);

    caps_.name = props2.properties.deviceName;
    caps_.api_version = props2.properties.apiVersion;
    caps_.driver_version = props2.properties.driverVersion;
    caps_.vendor_id = props2.properties.vendorID;
    caps_.discrete = props2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    caps_.max_workgroup_invocations = props2.properties.limits.maxComputeWorkGroupInvocations;
    caps_.subgroup_size = subgroup.subgroupSize;
    caps_.timestamp_period_ns = props2.properties.limits.timestampPeriod;
    caps_.min_uniform_offset = props2.properties.limits.minUniformBufferOffsetAlignment;

    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &memory);
    for (u32 i = 0; i < memory.memoryHeapCount; ++i) {
        if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            caps_.device_local_bytes += memory.memoryHeaps[i].size;
        }
    }

    u32 extension_count = 0;
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> extensions(extension_count);
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &extension_count,
                                         extensions.data());
    caps_.has_ray_query = has_extension(extensions, VK_KHR_RAY_QUERY_EXTENSION_NAME);
    caps_.has_int64_atomics =
        has_extension(extensions, VK_KHR_SHADER_ATOMIC_INT64_EXTENSION_NAME);
}

void Device::wait_idle() const {
    if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
}

void Device::set_name(u64 object, VkObjectType type, const char* name) const {
    if (!validation_ || !debug_utils_ || device_ == VK_NULL_HANDLE) return;
    VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    info.objectType = type;
    info.objectHandle = object;
    info.pObjectName = name;
    vkSetDebugUtilsObjectNameEXT(device_, &info);
}

void Device::destroy() {
    if (allocator_ != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator_);
        allocator_ = VK_NULL_HANDLE;
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (messenger_ != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
        messenger_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

}  // namespace ws
