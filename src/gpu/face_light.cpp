#include "gpu/face_light.hpp"

#include "core/log.hpp"

namespace ws {

bool FaceLight::create(Device& device, u32 slots, const char* name) {
    device_ = &device;
    bytes_ = static_cast<u64>(slots) * sizeof(u32);

    // TRANSFER_DST because it has to start at nought, and nought is a real answer here rather than
    // a spare value: an entry with no samples reads as fully open sky, which is what every surface
    // was assumed to have before this stage existed. Starting from anything else would darken the
    // world by whatever the allocator happened to leave behind.
    // TRANSFER_SRC as well, because the seen stamps are read back at the screenshot audit -- the
    // count of faces the frame is actually lighting is the number every cost figure in this pass
    // has to be read against, and an undeclared copy source is the unsound-instrument trap the
    // face buffers already record.
    buffer_ = create_device_buffer(device, bytes_,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   name);
    if (buffer_.buffer == VK_NULL_HANDLE) {
        WS_LOG_ERROR("faces", "could not allocate {} bytes of {}", bytes_, name);
        return false;
    }

    // Zeroed once, here, on a transient command buffer of its own. Doing it in the frame would
    // mean a clear the size of the store on the first frame that happens to notice, and doing it
    // lazily in the shader would mean a flag per slot to say whether it had been done.
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
    vkCmdFillBuffer(cmd, buffer_.buffer, 0, bytes_, 0u);
    WS_VK(vkEndCommandBuffer(cmd));

    VkCommandBufferSubmitInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmd_info.commandBuffer = cmd;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;
    WS_VK(vkQueueSubmit2(device.graphics_queue(), 1, &submit, VK_NULL_HANDLE));
    device.wait_idle();
    vkDestroyCommandPool(device.handle(), pool, nullptr);

    WS_LOG_INFO("faces", "{} {} KB for {} slots", name, bytes_ / 1024, slots);
    return true;
}

void FaceLight::destroy() {
    if (device_ == nullptr) return;
    destroy_buffer(*device_, buffer_);
    device_ = nullptr;
}

}  // namespace ws
