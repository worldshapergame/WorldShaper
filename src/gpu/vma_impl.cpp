// The single translation unit that compiles VulkanMemoryAllocator.
// Warnings are disabled for this file in CMakeLists.txt: it is third-party code and is
// not held to our own -Werror bar.

#include <volk.h>

#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1

#include <vk_mem_alloc.h>
