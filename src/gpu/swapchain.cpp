#include "gpu/swapchain.hpp"

#include <algorithm>

namespace ws {

void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
                   VkImageLayout new_layout, VkPipelineStageFlags2 src_stage,
                   VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                   VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

Swapchain::~Swapchain() { destroy(); }

bool Swapchain::create(Device& device, u32 width, u32 height, bool vsync) {
    device_ = &device;
    vsync_ = vsync;

    VkSemaphoreTypeCreateInfo timeline_type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timeline_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timeline_type.initialValue = 0;
    VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semaphore_info.pNext = &timeline_type;
    WS_VK(vkCreateSemaphore(device_->handle(), &semaphore_info, nullptr, &timeline_));

    for (u32 i = 0; i < kFramesInFlight; ++i) {
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool_info.queueFamilyIndex = device_->graphics_family();
        WS_VK(vkCreateCommandPool(device_->handle(), &pool_info, nullptr, &frames_[i].pool));

        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = frames_[i].pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        WS_VK(vkAllocateCommandBuffers(device_->handle(), &alloc, &frames_[i].cmd));

        VkSemaphoreCreateInfo binary{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        WS_VK(vkCreateSemaphore(device_->handle(), &binary, nullptr,
                                &frames_[i].image_available));
    }

    return build(width, height);
}

bool Swapchain::build(u32 width, u32 height) {
    VkSurfaceCapabilitiesKHR caps{};
    WS_VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_->physical(), device_->surface(),
                                                    &caps));

    extent_ = caps.currentExtent;
    if (extent_.width == 0xFFFFFFFFu) {
        extent_.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent_.height =
            std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent_.width == 0 || extent_.height == 0) return false;  // minimised

    u32 format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device_->physical(), device_->surface(),
                                         &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(device_->physical(), device_->surface(),
                                         &format_count, formats.data());

    VkSurfaceFormatKHR chosen = formats[0];
    for (const VkSurfaceFormatKHR& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }
    format_ = chosen.format;

    u32 mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device_->physical(), device_->surface(),
                                              &mode_count, nullptr);
    std::vector<VkPresentModeKHR> modes(mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(device_->physical(), device_->surface(),
                                              &mode_count, modes.data());

    // FIFO is always available and is the right default: it is the only mode that gives
    // a genuinely locked framerate, which is what the Steam Deck target needs.
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync_) {
        for (VkPresentModeKHR m : modes) {
            if (m == VK_PRESENT_MODE_MAILBOX_KHR) { present_mode = m; break; }
            if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) present_mode = m;
        }
    }

    u32 image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    VkSwapchainKHR old = swapchain_;
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = device_->surface();
    info.minImageCount = image_count;
    info.imageFormat = format_;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = extent_;
    info.imageArrayLayers = 1;
    // TRANSFER_DST so a compute pass can blit its result straight in; COLOR_ATTACHMENT so
    // the HUD can be drawn on top with dynamic rendering.
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = present_mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = old;

    WS_VK(vkCreateSwapchainKHR(device_->handle(), &info, nullptr, &swapchain_));
    if (old != VK_NULL_HANDLE) vkDestroySwapchainKHR(device_->handle(), old, nullptr);

    u32 actual = 0;
    vkGetSwapchainImagesKHR(device_->handle(), swapchain_, &actual, nullptr);
    images_.resize(actual);
    vkGetSwapchainImagesKHR(device_->handle(), swapchain_, &actual, images_.data());

    for (VkImageView view : views_) vkDestroyImageView(device_->handle(), view, nullptr);
    views_.clear();
    views_.reserve(actual);
    for (VkImage image : images_) {
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format_;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        VkImageView view = VK_NULL_HANDLE;
        WS_VK(vkCreateImageView(device_->handle(), &view_info, nullptr, &view));
        views_.push_back(view);
    }

    for (VkSemaphore s : render_finished_) vkDestroySemaphore(device_->handle(), s, nullptr);
    render_finished_.clear();
    render_finished_.reserve(actual);
    for (u32 i = 0; i < actual; ++i) {
        VkSemaphoreCreateInfo binary{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s = VK_NULL_HANDLE;
        WS_VK(vkCreateSemaphore(device_->handle(), &binary, nullptr, &s));
        render_finished_.push_back(s);
    }

    needs_recreate_ = false;
    WS_LOG_INFO("gpu", "swapchain {}x{}, {} images, {}", extent_.width, extent_.height,
                actual, vsync_ ? "vsync" : "uncapped");
    return true;
}

bool Swapchain::recreate(u32 width, u32 height) {
    device_->wait_idle();
    return build(width, height);
}

bool Swapchain::begin_frame() {
    if (needs_recreate_ || swapchain_ == VK_NULL_HANDLE) return false;

    FrameContext& frame = frames_[frame_index_];

    // Wait until this slot's previous submission has finished on the GPU.
    if (frame.timeline_value > 0) {
        VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wait.semaphoreCount = 1;
        wait.pSemaphores = &timeline_;
        wait.pValues = &frame.timeline_value;
        WS_VK(vkWaitSemaphores(device_->handle(), &wait, UINT64_MAX));
    }

    const VkResult acquired =
        vkAcquireNextImageKHR(device_->handle(), swapchain_, UINT64_MAX,
                              frame.image_available, VK_NULL_HANDLE, &image_index_);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        needs_recreate_ = true;
        return false;
    }
    if (acquired == VK_SUBOPTIMAL_KHR) {
        needs_recreate_ = true;  // present this frame anyway, rebuild next
    } else if (acquired != VK_SUCCESS) {
        WS_LOG_ERROR("gpu", "vkAcquireNextImageKHR: {}", vk_result_name(acquired));
        needs_recreate_ = true;
        return false;
    }

    WS_VK(vkResetCommandPool(device_->handle(), frame.pool, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    WS_VK(vkBeginCommandBuffer(frame.cmd, &begin));
    return true;
}

void Swapchain::end_frame() {
    FrameContext& frame = frames_[frame_index_];
    WS_VK(vkEndCommandBuffer(frame.cmd));

    ++timeline_counter_;
    frame.timeline_value = timeline_counter_;

    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = frame.image_available;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signals[2]{};
    signals[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signals[0].semaphore = render_finished_[image_index_];
    signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signals[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signals[1].semaphore = timeline_;
    signals[1].value = timeline_counter_;
    signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmd_info.commandBuffer = frame.cmd;

    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;
    submit.signalSemaphoreInfoCount = 2;
    submit.pSignalSemaphoreInfos = signals;
    WS_VK(vkQueueSubmit2(device_->graphics_queue(), 1, &submit, VK_NULL_HANDLE));

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render_finished_[image_index_];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &image_index_;

    const VkResult presented = vkQueuePresentKHR(device_->graphics_queue(), &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        needs_recreate_ = true;
    } else if (presented != VK_SUCCESS) {
        WS_LOG_ERROR("gpu", "vkQueuePresentKHR: {}", vk_result_name(presented));
        needs_recreate_ = true;
    }

    frame_index_ = (frame_index_ + 1) % kFramesInFlight;
    ++frame_number_;
}

void Swapchain::destroy_swapchain_objects() {
    if (device_ == nullptr) return;
    for (VkSemaphore s : render_finished_) vkDestroySemaphore(device_->handle(), s, nullptr);
    render_finished_.clear();
    for (VkImageView view : views_) vkDestroyImageView(device_->handle(), view, nullptr);
    views_.clear();
    images_.clear();
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_->handle(), swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

void Swapchain::destroy() {
    if (device_ == nullptr) return;
    device_->wait_idle();
    destroy_swapchain_objects();

    for (u32 i = 0; i < kFramesInFlight; ++i) {
        if (frames_[i].image_available != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_->handle(), frames_[i].image_available, nullptr);
            frames_[i].image_available = VK_NULL_HANDLE;
        }
        if (frames_[i].pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_->handle(), frames_[i].pool, nullptr);
            frames_[i].pool = VK_NULL_HANDLE;
        }
    }
    if (timeline_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_->handle(), timeline_, nullptr);
        timeline_ = VK_NULL_HANDLE;
    }
    device_ = nullptr;
}

}  // namespace ws
