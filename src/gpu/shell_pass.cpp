#include "gpu/shell_pass.hpp"

#include <algorithm>
#include <cstring>

#include "core/log.hpp"
#include "ui/ink.hpp"

namespace ws {
namespace {

// What one frame of interface is allowed to be. Generous by an order of magnitude against what a
// docked window actually produces — a settings panel is about two hundred marks — and fixed,
// because a buffer that grows mid-frame is a buffer the card is reading while it moves.
constexpr u32 kMaxCommands = 8192;
constexpr u32 kMaxCharacters = 65536;
constexpr u32 kMaxTileIndices = 262144;
// Enough tiles for 4K at sixteen pixels: 240 by 135, rounded up and doubled for a window somebody
// dragged onto a taller monitor.
constexpr u32 kMaxTiles = 512 * 288;

struct MarksPush {
    f32 resolution[2];
    f32 origin[2];
    u32 tile_counts[2];
    f32 seconds;
    f32 pad0;
    f32 accent[3];
    f32 pad1;
    // The three hues the interface is allowed, from `ui::tint_of`. Sent rather than rotated in the
    // shader so that a wire and the word it is named after cannot come out different colours; see
    // src/ui/ink.hpp for why there is exactly one implementation of the rotation.
    f32 tints[3][4];
};
static_assert(sizeof(MarksPush) == 96, "shaders/shell.comp declares this block");

struct BlurPush {
    i32 source_size[2];
    i32 destination_size[2];
    i32 mode;
    i32 pad[3];
};
static_assert(sizeof(BlurPush) == 32, "shaders/shell_blur.comp declares this block");

struct RoomPush {
    f32 resolution[2];
    f32 seconds;
    f32 pad0;
};

}  // namespace

ShellPass::~ShellPass() { destroy(); }

bool ShellPass::build_layouts() {
    const VkDevice device = device_->handle();

    // The marks: two images and four buffers.
    {
        VkDescriptorSetLayoutBinding bindings[6]{};
        for (u32 i = 0; i < 6; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = (i < 2) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                 : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = 6;
        info.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &info, nullptr, &marks_layout_) != VK_SUCCESS) {
            return false;
        }
    }
    // The blur: one image in, one out.
    {
        VkDescriptorSetLayoutBinding bindings[2]{};
        for (u32 i = 0; i < 2; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = 2;
        info.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &info, nullptr, &blur_layout_) != VK_SUCCESS) {
            return false;
        }
    }
    // The room: one image out.
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = 1;
        info.pBindings = &binding;
        if (vkCreateDescriptorSetLayout(device, &info, nullptr, &room_layout_) != VK_SUCCESS) {
            return false;
        }
    }

    const VkDescriptorPoolSize sizes[]{
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * kFramesInFlight},
    };
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = kFramesInFlight + 4;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device, &pool, nullptr, &pool_) != VK_SUCCESS) return false;

    for (u32 i = 0; i < kFramesInFlight; ++i) {
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = pool_;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &marks_layout_;
        if (vkAllocateDescriptorSets(device, &allocate, &slots_[i].set) != VK_SUCCESS) return false;
    }
    for (u32 i = 0; i < 3; ++i) {
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = pool_;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &blur_layout_;
        if (vkAllocateDescriptorSets(device, &allocate, &blur_sets_[i]) != VK_SUCCESS) return false;
    }
    {
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = pool_;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &room_layout_;
        if (vkAllocateDescriptorSets(device, &allocate, &room_set_) != VK_SUCCESS) return false;
    }
    return true;
}

bool ShellPass::create(Device& device, const std::filesystem::path& source_dir,
                       const std::filesystem::path& spirv_dir) {
    device_ = &device;
    if (!build_layouts()) {
        WS_LOG_ERROR("shell", "could not build the interface's descriptor layouts");
        return false;
    }

    for (u32 i = 0; i < kFramesInFlight; ++i) {
        slots_[i].commands = create_staging_buffer(device, kMaxCommands * sizeof(ui::Command),
                                                   "shell commands",
                                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        slots_[i].characters = create_staging_buffer(device, kMaxCharacters * sizeof(u32),
                                                     "shell characters",
                                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        slots_[i].tiles = create_staging_buffer(device, kMaxTiles * 2 * sizeof(u32), "shell tiles",
                                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        slots_[i].indices = create_staging_buffer(device, kMaxTileIndices * sizeof(u32),
                                                  "shell tile indices",
                                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (!slots_[i].commands.valid() || !slots_[i].tiles.valid()) return false;
    }

    // Not fatal if any of them will not compile, and deliberately so: a game that will not start
    // because its menu's blur would not build has traded the thing for the report on the thing.
    // The caller checks `valid()` and falls back to opening the world directly.
    if (!marks_.create(device, source_dir / "shell.comp", spirv_dir / "shell.comp.spv",
                       marks_layout_, sizeof(MarksPush))) {
        WS_LOG_ERROR("shell", "no interface this run: {}", marks_.last_error());
        return false;
    }
    if (!blur_.create(device, source_dir / "shell_blur.comp", spirv_dir / "shell_blur.comp.spv",
                      blur_layout_, sizeof(BlurPush))) {
        WS_LOG_ERROR("shell", "no glass this run: {}", blur_.last_error());
        return false;
    }
    if (!room_.create(device, source_dir / "title_room.comp", spirv_dir / "title_room.comp.spv",
                      room_layout_, sizeof(RoomPush))) {
        WS_LOG_ERROR("shell", "no title room this run: {}", room_.last_error());
        return false;
    }
    return true;
}

void ShellPass::destroy() {
    if (device_ == nullptr) return;
    // The last frame may still be executing against these. Same trap the loading screen documents:
    // freeing a descriptor that the card is reading shows up as a device lost on some later frame,
    // nowhere near the code that caused it.
    vkDeviceWaitIdle(device_->handle());

    marks_.destroy();
    blur_.destroy();
    room_.destroy();
    destroy_image(*device_, surface_);
    destroy_image(*device_, half_);
    destroy_image(*device_, quarter_);
    for (Slot& slot : slots_) {
        destroy_buffer(*device_, slot.commands);
        destroy_buffer(*device_, slot.characters);
        destroy_buffer(*device_, slot.tiles);
        destroy_buffer(*device_, slot.indices);
    }
    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_->handle(), pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
    }
    for (VkDescriptorSetLayout* layout : {&marks_layout_, &blur_layout_, &room_layout_}) {
        if (*layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_->handle(), *layout, nullptr);
            *layout = VK_NULL_HANDLE;
        }
    }
    device_ = nullptr;
}

bool ShellPass::ensure(u32 width, u32 height) {
    if (width == 0 || height == 0) return false;
    if (surface_.valid() && surface_.extent.width == width && surface_.extent.height == height) {
        return true;
    }
    vkDeviceWaitIdle(device_->handle());
    destroy_image(*device_, surface_);
    destroy_image(*device_, half_);
    destroy_image(*device_, quarter_);

    surface_ = create_storage_image(*device_, width, height, VK_FORMAT_R16G16B16A16_SFLOAT,
                                    "shell surface");
    const u32 quarter_w = std::max(1u, (width + 3) / 4);
    const u32 quarter_h = std::max(1u, (height + 3) / 4);
    half_ = create_storage_image(*device_, quarter_w, quarter_h, VK_FORMAT_R16G16B16A16_SFLOAT,
                                 "shell glass");
    quarter_ = create_storage_image(*device_, quarter_w, quarter_h,
                                    VK_FORMAT_R16G16B16A16_SFLOAT, "shell glass, halfway");
    surface_written_ = false;
    if (!surface_.valid() || !half_.valid() || !quarter_.valid()) return false;

    // The three blur steps, and the room, never change what they point at once the images exist.
    const auto write_pair = [&](VkDescriptorSet set, const GpuImage& from, const GpuImage& to) {
        VkDescriptorImageInfo images[2]{};
        images[0].imageView = from.view;
        images[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        images[1].imageView = to.view;
        images[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet writes[2]{};
        for (u32 i = 0; i < 2; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(device_->handle(), 2, writes, 0, nullptr);
    };
    write_pair(blur_sets_[0], surface_, half_);
    write_pair(blur_sets_[1], half_, quarter_);
    write_pair(blur_sets_[2], quarter_, half_);

    {
        VkDescriptorImageInfo image{};
        image.imageView = surface_.view;
        image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = room_set_;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(device_->handle(), 1, &write, 0, nullptr);
    }

    for (u32 i = 0; i < kFramesInFlight; ++i) write_descriptors(i);
    return true;
}

void ShellPass::write_descriptors(u32 slot) {
    Slot& own = slots_[slot];
    VkDescriptorImageInfo images[2]{};
    images[0].imageView = surface_.view;
    images[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    images[1].imageView = half_.view;
    images[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    const VkBuffer buffers[4]{own.commands.buffer, own.characters.buffer, own.tiles.buffer,
                              own.indices.buffer};
    VkDescriptorBufferInfo infos[4]{};
    VkWriteDescriptorSet writes[6]{};
    for (u32 i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = own.set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &images[i];
    }
    for (u32 i = 0; i < 4; ++i) {
        infos[i].buffer = buffers[i];
        infos[i].offset = 0;
        infos[i].range = VK_WHOLE_SIZE;
        writes[2 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2 + i].dstSet = own.set;
        writes[2 + i].dstBinding = 2 + i;
        writes[2 + i].descriptorCount = 1;
        writes[2 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2 + i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device_->handle(), 6, writes, 0, nullptr);
}

void ShellPass::upload(const ui::DrawList& list, u32 slot) {
    Slot& own = slots_[slot % kFramesInFlight];
    own.command_count = 0;
    own.bounds = list.bounds();
    own.tiles_across = list.tiles_across();
    own.tiles_down = list.tiles_down();
    if (list.empty()) return;

    // A frame that overflows is DROPPED rather than truncated, and it says so. A truncated
    // interface is one where half a window is missing and nothing anywhere says why — and at eight
    // thousand marks against the two hundred a panel produces, reaching this means something is
    // adding marks in a loop, which is a fault worth being loud about.
    const u32 commands = static_cast<u32>(list.commands().size());
    const u32 characters = static_cast<u32>(list.characters().size());
    const u32 indices = static_cast<u32>(list.tile_indices().size());
    const u32 tiles = static_cast<u32>(list.tiles().size() / 2);
    if (commands > kMaxCommands || characters > kMaxCharacters || indices > kMaxTileIndices ||
        tiles > kMaxTiles) {
        WS_LOG_WARN("shell", "a frame of interface did not fit: {} marks, {} characters, {} tiles",
                    commands, characters, tiles);
        return;
    }

    std::memcpy(own.commands.mapped, list.commands().data(), commands * sizeof(ui::Command));
    if (characters > 0) {
        std::memcpy(own.characters.mapped, list.characters().data(), characters * sizeof(u32));
    }
    std::memcpy(own.tiles.mapped, list.tiles().data(), tiles * 2 * sizeof(u32));
    if (indices > 0) {
        std::memcpy(own.indices.mapped, list.tile_indices().data(), indices * sizeof(u32));
    }
    own.command_count = commands;
}

void ShellPass::barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
    image_barrier(cmd, image, from, to, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                  VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                  VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
}

void ShellPass::draw_room(VkCommandBuffer cmd, f32 seconds) {
    if (!surface_.valid() || room_.pipeline() == VK_NULL_HANDLE) return;
    barrier(cmd, surface_.image,
            surface_written_ ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL);
    surface_written_ = true;

    RoomPush push{};
    push.resolution[0] = static_cast<f32>(surface_.extent.width);
    push.resolution[1] = static_cast<f32>(surface_.extent.height);
    push.seconds = seconds;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, room_.pipeline());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, room_.layout(), 0, 1, &room_set_,
                            0, nullptr);
    vkCmdPushConstants(cmd, room_.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (surface_.extent.width + 7) / 8, (surface_.extent.height + 7) / 8, 1);
}

void ShellPass::blit_in(VkCommandBuffer cmd, VkImage source, VkExtent2D source_extent) {
    if (!surface_.valid()) return;
    barrier(cmd, surface_.image,
            surface_written_ ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageBlit2 region{VK_STRUCTURE_TYPE_IMAGE_BLIT_2};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.layerCount = 1;
    region.dstSubresource = region.srcSubresource;
    region.srcOffsets[1] = {static_cast<i32>(source_extent.width),
                            static_cast<i32>(source_extent.height), 1};
    region.dstOffsets[1] = {static_cast<i32>(surface_.extent.width),
                            static_cast<i32>(surface_.extent.height), 1};

    VkBlitImageInfo2 blit{VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2};
    blit.srcImage = source;
    blit.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    blit.dstImage = surface_.image;
    blit.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    blit.regionCount = 1;
    blit.pRegions = &region;
    // Linear, because this is also where a scaled render is put back to the window's size.
    blit.filter = VK_FILTER_LINEAR;
    vkCmdBlitImage2(cmd, &blit);
    surface_written_ = true;

    barrier(cmd, surface_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
}

void ShellPass::draw(VkCommandBuffer cmd, const ui::DrawList& list, u32 slot, const f32 accent[3],
                     f32 seconds) {
    const Slot& own = slots_[slot % kFramesInFlight];
    if (own.command_count == 0 || !surface_.valid()) return;
    if (marks_.pipeline() == VK_NULL_HANDLE || blur_.pipeline() == VK_NULL_HANDLE) return;

    // --- the glass ---------------------------------------------------------------------------
    //
    // Three dispatches over a quarter-resolution image, and it is the ONLY full-surface work the
    // interface does. It reads what is behind the panels, so it has to happen before the marks and
    // it has to read the whole surface: a panel that is dragged reveals what was under it.
    const u32 quarter_w = half_.extent.width;
    const u32 quarter_h = half_.extent.height;
    barrier(cmd, half_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    barrier(cmd, quarter_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blur_.pipeline());
    const i32 sizes[3][4]{
        {static_cast<i32>(surface_.extent.width), static_cast<i32>(surface_.extent.height),
         static_cast<i32>(quarter_w), static_cast<i32>(quarter_h)},
        {static_cast<i32>(quarter_w), static_cast<i32>(quarter_h), static_cast<i32>(quarter_w),
         static_cast<i32>(quarter_h)},
        {static_cast<i32>(quarter_w), static_cast<i32>(quarter_h), static_cast<i32>(quarter_w),
         static_cast<i32>(quarter_h)},
    };
    for (u32 step = 0; step < 3; ++step) {
        BlurPush push{};
        push.source_size[0] = sizes[step][0];
        push.source_size[1] = sizes[step][1];
        push.destination_size[0] = sizes[step][2];
        push.destination_size[1] = sizes[step][3];
        push.mode = static_cast<i32>(step);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blur_.layout(), 0, 1,
                                &blur_sets_[step], 0, nullptr);
        vkCmdPushConstants(cmd, blur_.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                           &push);
        vkCmdDispatch(cmd, (quarter_w + 7) / 8, (quarter_h + 7) / 8, 1);

        VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        memory.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        memory.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        memory.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        memory.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &memory;
        vkCmdPipelineBarrier2(cmd, &dependency);
    }

    // --- the marks ----------------------------------------------------------------------------
    //
    // Over the tiles that have something in them and no further. This is the rule the style
    // document states as *run the ink only over panel rectangles, never the full screen*, and it
    // is why the world in the middle costs nothing at all.
    const ui::Rect& bounds = own.bounds;
    const u32 x0 = static_cast<u32>(std::max(0.0f, bounds.x0));
    const u32 y0 = static_cast<u32>(std::max(0.0f, bounds.y0));
    const u32 wide = static_cast<u32>(std::max(0.0f, bounds.x1 - bounds.x0));
    const u32 tall = static_cast<u32>(std::max(0.0f, bounds.y1 - bounds.y0));
    if (wide == 0 || tall == 0) return;

    MarksPush push{};
    push.resolution[0] = static_cast<f32>(surface_.extent.width);
    push.resolution[1] = static_cast<f32>(surface_.extent.height);
    push.origin[0] = static_cast<f32>(x0);
    push.origin[1] = static_cast<f32>(y0);
    push.tile_counts[0] = own.tiles_across;
    push.tile_counts[1] = own.tiles_down;
    push.seconds = seconds;
    for (u32 i = 0; i < 3; ++i) push.accent[i] = accent[i];
    for (u32 which = 0; which < ui::kTints; ++which) {
        const ui::Colour tint =
            ui::tint_of(ui::Colour{accent[0], accent[1], accent[2]}, which);
        push.tints[which][0] = tint.r;
        push.tints[which][1] = tint.g;
        push.tints[which][2] = tint.b;
        push.tints[which][3] = 1.0f;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, marks_.pipeline());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, marks_.layout(), 0, 1, &own.set,
                            0, nullptr);
    vkCmdPushConstants(cmd, marks_.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (wide + 7) / 8, (tall + 7) / 8, 1);
    (void)list;
}

void ShellPass::blit_out(VkCommandBuffer cmd, VkImage destination, VkExtent2D extent) {
    if (!surface_.valid()) return;
    barrier(cmd, surface_.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    image_barrier(cmd, destination, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                  VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkImageBlit2 region{VK_STRUCTURE_TYPE_IMAGE_BLIT_2};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.layerCount = 1;
    region.dstSubresource = region.srcSubresource;
    region.srcOffsets[1] = {static_cast<i32>(surface_.extent.width),
                            static_cast<i32>(surface_.extent.height), 1};
    region.dstOffsets[1] = {static_cast<i32>(extent.width), static_cast<i32>(extent.height), 1};

    VkBlitImageInfo2 blit{VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2};
    blit.srcImage = surface_.image;
    blit.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    blit.dstImage = destination;
    blit.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    blit.regionCount = 1;
    blit.pRegions = &region;
    // Nearest, because this one is size for size and the marks are made of whole pixels. Anything
    // else would soften a three-pixel-tall letter for nothing.
    blit.filter = (surface_.extent.width == extent.width && surface_.extent.height == extent.height)
                      ? VK_FILTER_NEAREST
                      : VK_FILTER_LINEAR;
    vkCmdBlitImage2(cmd, &blit);

    barrier(cmd, surface_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
}

bool ShellPass::present(Swapchain& swapchain, const ui::DrawList& list, const f32 accent[3],
                        f32 seconds) {
    if (!valid()) return false;
    const VkExtent2D extent = swapchain.extent();
    if (extent.width == 0 || extent.height == 0) return false;
    if (!ensure(extent.width, extent.height)) return false;

    const u32 slot = swapchain.frame_index();
    upload(list, slot);
    if (!swapchain.begin_frame()) return false;

    const VkCommandBuffer cmd = swapchain.cmd();
    draw_room(cmd, seconds);

    VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    memory.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    memory.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    memory.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    memory.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &memory;
    vkCmdPipelineBarrier2(cmd, &dependency);

    draw(cmd, list, slot, accent, seconds);
    blit_out(cmd, swapchain.current_image(), extent);

    image_barrier(cmd, swapchain.current_image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_BLIT_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
    swapchain.end_frame();
    ++frame_;
    return true;
}

void ShellPass::reload_if_changed() {
    marks_.reload_if_changed();
    blur_.reload_if_changed();
    room_.reload_if_changed();
}

}  // namespace ws
