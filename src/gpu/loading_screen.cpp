#include "gpu/loading_screen.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/log.hpp"
#include "gpu/swapchain.hpp"

namespace ws {
namespace {

// The push constant block, laid out to match shaders/loading.comp exactly.
//
// Vulkan guarantees at least 128 bytes and this is 120: forty-eight of numbers and seventy-two of
// text, three lines of twenty-four characters packed four to a uint. That leaves eight bytes of
// headroom, so a fourth line of text would not fit and would have to become a uniform buffer —
// worth knowing before somebody tries.
struct Push {
    f32 resolution[2];
    f32 time;
    f32 fraction;

    f32 scale;
    u32 stage;
    u32 pad0 = 0;
    u32 pad1 = 0;

    f32 accent[3];
    f32 pad2 = 0.0f;

    u32 text[18]{};
};
static_assert(sizeof(Push) == 120, "the shader's push block is laid out to match this");

constexpr u32 kSlotUints = 6;    // twenty-four characters, four to a uint
constexpr u32 kSlotChars = kSlotUints * 4;

// Packs a string into a slot, four characters to a uint, zero-terminated.
//
// It used to upper-case as it went, because the font in ui.glsl was capitals only and a caller
// writing a label should not have to know that. The face carries both cases now, so a label is
// packed as it was written — and a stage called `Sampling` reads as a word rather than as a
// shout. A character the font does not have still comes out blank rather than as a wrong letter,
// which is the failure a reader can actually diagnose.
void pack(u32* text, u32 slot, const std::string& value) {
    const usize count = std::min<usize>(value.size(), kSlotChars - 1);
    for (usize i = 0; i < count; ++i) {
        const char c = value[i];
        const u32 index = slot * kSlotUints + static_cast<u32>(i / 4);
        text[index] |= static_cast<u32>(static_cast<u8>(c)) << ((i % 4) * 8);
    }
}

}  // namespace

LoadingScreen::~LoadingScreen() { destroy(); }

bool LoadingScreen::create(Device& device, const std::filesystem::path& shader_source,
                           const std::filesystem::path& shader_spirv) {
    device_ = &device;

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layout_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.bindingCount = 1;
    layout_info.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device.handle(), &layout_info, nullptr, &set_layout_) !=
        VK_SUCCESS) {
        WS_LOG_ERROR("loading", "could not create the loading screen's set layout");
        return false;
    }

    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &size;
    if (vkCreateDescriptorPool(device.handle(), &pool_info, nullptr, &pool_) != VK_SUCCESS) {
        WS_LOG_ERROR("loading", "could not create the loading screen's descriptor pool");
        return false;
    }

    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &set_layout_;
    if (vkAllocateDescriptorSets(device.handle(), &allocate, &set_) != VK_SUCCESS) {
        WS_LOG_ERROR("loading", "could not allocate the loading screen's descriptor set");
        return false;
    }

    if (!pipeline_.create(device, shader_source, shader_spirv, set_layout_, sizeof(Push))) {
        // Not fatal, and deliberately so: a game that will not start because its progress bar
        // would not compile has traded the thing for the report on the thing.
        WS_LOG_ERROR("loading", "no loading screen this run: {}", pipeline_.last_error());
        return false;
    }
    return true;
}

void LoadingScreen::destroy() {
    if (device_ == nullptr) return;

    // The last loading frame was presented and not waited for, so the card may still be reading
    // this image and running this pipeline. Freeing them here without waiting hands the very next
    // submit a descriptor pointing at memory that has been given back — which shows up as a device
    // lost on frame zero, in the frame loop, nowhere near the code that caused it.
    vkDeviceWaitIdle(device_->handle());

    pipeline_.destroy();
    destroy_image(*device_, target_);
    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_->handle(), pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
        set_ = VK_NULL_HANDLE;
    }
    if (set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_->handle(), set_layout_, nullptr);
        set_layout_ = VK_NULL_HANDLE;
    }
    device_ = nullptr;
}

bool LoadingScreen::ensure_target(u32 width, u32 height) {
    if (target_.valid() && target_.extent.width == width && target_.extent.height == height) {
        return true;
    }
    if (target_.valid()) {
        vkDeviceWaitIdle(device_->handle());
        destroy_image(*device_, target_);
    }
    target_ = create_storage_image(*device_, width, height, VK_FORMAT_R16G16B16A16_SFLOAT,
                                   "loading screen");
    target_written_ = false;
    if (!target_.valid()) return false;

    VkDescriptorImageInfo image{};
    image.imageView = target_.view;
    image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(device_->handle(), 1, &write, 0, nullptr);
    return true;
}

bool LoadingScreen::present(Swapchain& swapchain, const LoadingFrame& frame) {
    if (!valid() || device_ == nullptr) return false;

    const VkExtent2D extent = swapchain.extent();
    if (extent.width == 0 || extent.height == 0) return false;
    if (!ensure_target(extent.width, extent.height)) return false;
    if (!swapchain.begin_frame()) return false;

    const VkCommandBuffer cmd = swapchain.cmd();

    // The first use of the image is UNDEFINED; every use after it comes from the previous frame's
    // blit, which left it in TRANSFER_SRC. Getting this wrong is silent on one driver and a
    // validation error on the next.
    image_barrier(cmd, target_.image,
                  target_written_ ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                  : VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                  VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_WRITE_BIT);

    Push push{};
    push.resolution[0] = static_cast<f32>(extent.width);
    push.resolution[1] = static_cast<f32>(extent.height);
    push.time = frame.seconds;
    push.fraction = std::clamp(frame.fraction, 0.0f, 1.0f);
    push.stage = frame.stage;
    for (u32 i = 0; i < 3; ++i) push.accent[i] = frame.accent[i];

    // Interface pixels are sized from the window's SHORT side, so the layout keeps its proportions
    // on a wide monitor instead of growing until the column runs off the sides.
    const f32 across = static_cast<f32>(std::min(extent.width, extent.height));
    push.scale = std::max(1.0f, std::floor(across / 420.0f));

    pack(push.text, 0, frame.stage_text);
    pack(push.text, 1, frame.count_text);
    pack(push.text, 2, frame.left_text);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.pipeline());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.layout(), 0, 1, &set_,
                            0, nullptr);
    vkCmdPushConstants(cmd, pipeline_.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push),
                       &push);
    vkCmdDispatch(cmd, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);

    image_barrier(cmd, target_.image, VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                  VK_ACCESS_2_TRANSFER_READ_BIT);
    image_barrier(cmd, swapchain.current_image(), VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                  VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkImageBlit2 region{VK_STRUCTURE_TYPE_IMAGE_BLIT_2};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.layerCount = 1;
    region.dstSubresource = region.srcSubresource;
    region.srcOffsets[1] = {static_cast<i32>(extent.width), static_cast<i32>(extent.height), 1};
    region.dstOffsets[1] = region.srcOffsets[1];

    VkBlitImageInfo2 blit{VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2};
    blit.srcImage = target_.image;
    blit.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    blit.dstImage = swapchain.current_image();
    blit.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    blit.regionCount = 1;
    blit.pRegions = &region;
    blit.filter = VK_FILTER_NEAREST;
    vkCmdBlitImage2(cmd, &blit);
    target_written_ = true;

    image_barrier(cmd, swapchain.current_image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_BLIT_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);

    swapchain.end_frame();
    return true;
}

}  // namespace ws
