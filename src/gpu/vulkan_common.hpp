#pragma once
// Shared Vulkan includes and error handling.
//
// volk resolves every entry point at runtime, so the game never links vulkan-1.lib and
// starts cleanly on a machine with no Vulkan runtime (it reports the problem instead of
// failing to launch).

#include <volk.h>

#include <vk_mem_alloc.h>

#include "core/assert.hpp"
#include "core/log.hpp"
#include "core/types.hpp"

namespace ws {

const char* vk_result_name(VkResult result);

}  // namespace ws

// Any Vulkan call that must succeed. A failure here means the GPU state is unknown, so
// continuing would only produce a more confusing crash later.
#define WS_VK(expr)                                                                    \
    do {                                                                               \
        const VkResult ws_vk_result_ = (expr);                                         \
        if (ws_vk_result_ != VK_SUCCESS) [[unlikely]] {                                \
            ::ws::panic(__FILE__, __LINE__, #expr,                                     \
                        std::format("Vulkan call failed: {}",                          \
                                    ::ws::vk_result_name(ws_vk_result_)));             \
        }                                                                              \
    } while (false)
