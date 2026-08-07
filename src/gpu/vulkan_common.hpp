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

// Asks the device what it was doing when it died, and logs it.
//
// A lost device is reported at whatever call happens next, which is almost never the work
// that caused it: both crashes so far named vkQueueSubmit2 in the swapchain, which is only
// the first call after the GPU had already gone. Left at that, the report says the device
// was lost and nothing about why. This dumps the last executed pass markers and, where the
// driver supports it, the fault addresses — into the log, and so into the crash report.
//
// Installed by Device once it knows which diagnostic extensions exist; a no-op before that.
void report_device_lost();

}  // namespace ws

// Any Vulkan call that must succeed. A failure here means the GPU state is unknown, so
// continuing would only produce a more confusing crash later.
#define WS_VK(expr)                                                                    \
    do {                                                                               \
        const VkResult ws_vk_result_ = (expr);                                         \
        if (ws_vk_result_ != VK_SUCCESS) [[unlikely]] {                                \
            if (ws_vk_result_ == VK_ERROR_DEVICE_LOST) ::ws::report_device_lost();     \
            ::ws::panic(__FILE__, __LINE__, #expr,                                     \
                        std::format("Vulkan call failed: {}",                          \
                                    ::ws::vk_result_name(ws_vk_result_)));             \
        }                                                                              \
    } while (false)
