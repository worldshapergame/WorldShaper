#include "gpu/post_pass.hpp"

#include <algorithm>

#include "core/log.hpp"
#include "gpu/render_params.hpp"
#include "gpu/swapchain.hpp"

namespace ws {
namespace {

// Must match the push constant blocks in shaders/bloom_down.comp, shaders/bloom_up.comp and
// shaders/post.comp. A shader and a header disagreeing about a field does not fail, it reads the
// next one along -- which is the fault shaders/params.glsl opens by warning about.
struct DownPush {
    i32 source_size[2];
    i32 destination_size[2];
    u32 mode;
    u32 pad[3];
};
static_assert(sizeof(DownPush) == 32, "shaders/bloom_down.comp declares this block");

struct UpPush {
    i32 coarse_size[2];
    i32 fine_size[2];
};
static_assert(sizeof(UpPush) == 16, "shaders/bloom_up.comp declares this block");

struct PostPush {
    u32 pass;
    f32 bloom_scale;
    u32 pad[2];
};
static_assert(sizeof(PostPush) == 16, "shaders/post.comp declares this block");

constexpr u32 kPassShutter = 0u;
constexpr u32 kPassCombine = 1u;

// Ten per cent of what got past the threshold, and it is `kBloomIntensity` in
// shaders/pt_post.glsl. Divided by the number of levels before it reaches the shader, because
// level 0 of the chain holds every level SUMMED -- so a flat expanse over the threshold comes back
// as exactly this fraction of itself however many levels the frame's size asked for.
constexpr f32 kBloomIntensity = 0.10f;

// How many halvings a frame of this size gets.
//
// Tied to log2 of the short side, so the glare's reach stays the same FRACTION of the picture at
// every resolution -- which is how `kBloomTailRadius` was written when the gather did this, and it
// is the property that stops a 4K frame having a glare a quarter the size of the same scene at
// 1280x800. The minus four leaves the coarsest level about a sixtieth of the frame.
u32 levels_for(u32 width, u32 height) {
    const u32 shortest = std::max(1u, std::min(width, height));
    u32 log2 = 0;
    while ((2u << log2) <= shortest) ++log2;
    const i32 wanted = static_cast<i32>(log2) - 4;
    return static_cast<u32>(std::clamp(wanted, 1, static_cast<i32>(PostPass::kMaxLevels)));
}

void barrier_between_dispatches(VkCommandBuffer cmd) {
    VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    memory.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    memory.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    memory.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    memory.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &memory;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

}  // namespace

PostPass::~PostPass() { destroy(); }

bool PostPass::build_layouts() {
    const VkDevice device = device_->handle();

    // The halving step: the composite, the level above, the level being written.
    {
        VkDescriptorSetLayoutBinding bindings[3]{};
        for (u32 i = 0; i < 3; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = 3;
        info.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &info, nullptr, &down_layout_) != VK_SUCCESS) {
            return false;
        }
    }
    // The upsample: one level in, one level added into.
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
        if (vkCreateDescriptorSetLayout(device, &info, nullptr, &up_layout_) != VK_SUCCESS) {
            return false;
        }
    }
    // Post: colour in, colour out, the chain's head, the visibility buffer, the parameter block.
    {
        VkDescriptorSetLayoutBinding bindings[5]{};
        for (u32 i = 0; i < 5; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = (i < 4) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                 : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = 5;
        info.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &info, nullptr, &post_layout_) != VK_SUCCESS) {
            return false;
        }
    }

    // Sized for kMaxLevels whatever the window is, so a resize rewrites descriptors and never
    // allocates them. A pool that has to grow mid-session is a pool that fails on the one machine
    // with the big monitor.
    const VkDescriptorPoolSize sizes[]{
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 * (kMaxLevels + 1) + 2 * kMaxLevels + 4 * 3},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3},
    };
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = (kMaxLevels + 1) + kMaxLevels + 3;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device, &pool, nullptr, &pool_) != VK_SUCCESS) return false;

    const auto allocate = [&](VkDescriptorSetLayout layout, VkDescriptorSet* out) {
        VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        info.descriptorPool = pool_;
        info.descriptorSetCount = 1;
        info.pSetLayouts = &layout;
        return vkAllocateDescriptorSets(device, &info, out) == VK_SUCCESS;
    };
    for (u32 i = 0; i <= kMaxLevels; ++i) {
        if (!allocate(down_layout_, &down_sets_[i])) return false;
    }
    for (u32 i = 0; i < kMaxLevels; ++i) {
        if (!allocate(up_layout_, &up_sets_[i])) return false;
    }
    for (u32 i = 0; i < 3; ++i) {
        if (!allocate(post_layout_, &post_sets_[i])) return false;
    }
    return true;
}

bool PostPass::create(Device& device, const std::filesystem::path& source_dir,
                      const std::filesystem::path& spirv_dir) {
    device_ = &device;
    if (!build_layouts()) {
        WS_LOG_ERROR("post", "could not build the post stage's descriptor layouts");
        return false;
    }

    // Not fatal if any of them will not compile, and deliberately so: a game that will not start
    // because its glare would not build has traded the thing for the report on the thing. The
    // caller checks `valid()` and simply presents what the composite wrote.
    if (!down_.create(device, source_dir / "bloom_down.comp", spirv_dir / "bloom_down.comp.spv",
                      down_layout_, sizeof(DownPush))) {
        WS_LOG_ERROR("post", "no glare this run: {}", down_.last_error());
        return false;
    }
    if (!up_.create(device, source_dir / "bloom_up.comp", spirv_dir / "bloom_up.comp.spv",
                    up_layout_, sizeof(UpPush))) {
        WS_LOG_ERROR("post", "no glare this run: {}", up_.last_error());
        return false;
    }
    if (!post_.create(device, source_dir / "post.comp", spirv_dir / "post.comp.spv", post_layout_,
                      sizeof(PostPush))) {
        WS_LOG_ERROR("post", "no post stage this run: {}", post_.last_error());
        return false;
    }
    return true;
}

void PostPass::release_images() {
    if (device_ == nullptr) return;
    for (GpuImage& image : chain_) destroy_image(*device_, image);
    chain_.clear();
    destroy_image(*device_, scratch_);
    bound_colour_ = VK_NULL_HANDLE;
    bound_visibility_ = VK_NULL_HANDLE;
    bound_params_ = VK_NULL_HANDLE;
}

void PostPass::destroy() {
    if (device_ == nullptr) return;
    // The last frame may still be executing against these. Freeing a descriptor the card is
    // reading shows up as a device lost on some later frame, nowhere near the code that caused it.
    vkDeviceWaitIdle(device_->handle());

    down_.destroy();
    up_.destroy();
    post_.destroy();
    release_images();

    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_->handle(), pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
    }
    for (VkDescriptorSetLayout* layout : {&down_layout_, &up_layout_, &post_layout_}) {
        if (*layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_->handle(), *layout, nullptr);
            *layout = VK_NULL_HANDLE;
        }
    }
    device_ = nullptr;
}

void PostPass::reload_if_changed() {
    down_.reload_if_changed();
    up_.reload_if_changed();
    post_.reload_if_changed();
}

void PostPass::force_reload() {
    down_.force_reload();
    up_.force_reload();
    post_.force_reload();
}

bool PostPass::ensure(const GpuImage& colour, const GpuImage& visibility, VkBuffer params) {
    if (!colour.valid() || !visibility.valid()) return false;
    if (bound_colour_ == colour.view && bound_visibility_ == visibility.view &&
        bound_params_ == params && !chain_.empty() &&
        scratch_.extent.width == colour.extent.width &&
        scratch_.extent.height == colour.extent.height) {
        return true;
    }

    // The one caller that changes the render target's size already waits for the device before it
    // frees the old one, so this is normally a wait on an idle queue. It is here anyway because
    // "the caller happens to do it" is exactly the kind of invariant that stops being true.
    vkDeviceWaitIdle(device_->handle());
    release_images();

    const u32 width = colour.extent.width;
    const u32 height = colour.extent.height;
    if (width == 0 || height == 0) return false;

    scratch_ = create_storage_image(*device_, width, height, VK_FORMAT_R8G8B8A8_UNORM,
                                    "post scratch");
    if (!scratch_.valid()) return false;

    const u32 count = levels_for(width, height);
    chain_.reserve(count);
    u32 level_width = width;
    u32 level_height = height;
    for (u32 i = 0; i < count; ++i) {
        level_width = std::max(1u, (level_width + 1) / 2);
        level_height = std::max(1u, (level_height + 1) / 2);
        // R16G16B16A16_SFLOAT rather than a packed 11-11-10, which would halve the traffic: the
        // packed format is not one Vulkan requires an implementation to support as a storage
        // image, and a glare that works on the development card and not on the Deck is worse than
        // a glare that costs a little more on both.
        chain_.push_back(create_storage_image(*device_, level_width, level_height,
                                              VK_FORMAT_R16G16B16A16_SFLOAT, "bloom level"));
        if (!chain_.back().valid()) return false;
    }

    const auto image_info = [](const GpuImage& image) {
        VkDescriptorImageInfo info{};
        info.imageView = image.view;
        info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        return info;
    };
    const auto write_images = [&](VkDescriptorSet set, const VkDescriptorImageInfo* infos,
                                  u32 how_many) {
        VkWriteDescriptorSet writes[5]{};
        for (u32 i = 0; i < how_many; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device_->handle(), how_many, writes, 0, nullptr);
    };

    // The first step twice: once reading the composite, once reading what the shutter left.
    {
        const VkDescriptorImageInfo from_colour[3]{image_info(colour), image_info(chain_[0]),
                                                   image_info(chain_[0])};
        write_images(down_sets_[0], from_colour, 3);
        const VkDescriptorImageInfo from_scratch[3]{image_info(scratch_), image_info(chain_[0]),
                                                    image_info(chain_[0])};
        write_images(down_sets_[1], from_scratch, 3);
    }
    // ...and every step below it, which reads the level above and never the composite. Binding 0
    // is still written -- a set with an unwritten binding is undefined behaviour the moment it is
    // bound, whether or not the shader's branch reaches it.
    for (u32 i = 0; i + 1 < count; ++i) {
        const VkDescriptorImageInfo infos[3]{image_info(colour), image_info(chain_[i]),
                                             image_info(chain_[i + 1])};
        write_images(down_sets_[2 + i], infos, 3);
    }
    for (u32 i = 0; i + 1 < count; ++i) {
        const VkDescriptorImageInfo infos[2]{image_info(chain_[i + 1]), image_info(chain_[i])};
        write_images(up_sets_[i], infos, 2);
    }

    {
        VkDescriptorBufferInfo params_info{};
        params_info.buffer = params;
        params_info.offset = 0;
        // The block's own size and not VK_WHOLE_SIZE. A dynamic uniform buffer bound whole may
        // only ever be given an offset of zero, and this one is bound with the frame's slot -- so
        // the whole-size version does not read the wrong frame, it is refused outright by the
        // validation layer and reads whatever slot 0 holds without it.
        params_info.range = sizeof(RenderParams);

        const GpuImage* pairs[3][2]{
            {&colour, &scratch_},   // the shutter
            {&scratch_, &colour},   // the combine after it
            {&colour, &colour},     // the combine with no shutter, in place
        };
        for (u32 i = 0; i < 3; ++i) {
            const VkDescriptorImageInfo infos[4]{image_info(*pairs[i][0]), image_info(*pairs[i][1]),
                                                 image_info(chain_[0]), image_info(visibility)};
            write_images(post_sets_[i], infos, 4);

            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = post_sets_[i];
            write.dstBinding = 4;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            write.pBufferInfo = &params_info;
            vkUpdateDescriptorSets(device_->handle(), 1, &write, 0, nullptr);
        }
    }

    bound_colour_ = colour.view;
    bound_visibility_ = visibility.view;
    bound_params_ = params;
    WS_LOG_INFO("post", "the glare chain: {} levels from {}x{} down to {}x{}", count, width, height,
                chain_.back().extent.width, chain_.back().extent.height);
    return true;
}

void PostPass::record(VkCommandBuffer cmd, GpuProfiler& profiler, const GpuImage& colour,
                      const GpuImage& visibility, VkBuffer params, u32 params_offset,
                      const PostSettings& settings) {
    if (!valid()) return;
    // Neither half asked for: no command at all, so the frame is the one the composite wrote, to
    // the bit. This is what makes `--no-bloom --no-motion-blur` a control arm rather than a second
    // code path that happens to agree.
    if (!settings.bloom && !settings.motion_blur) return;
    if (!ensure(colour, visibility, params)) return;

    const u32 width = colour.extent.width;
    const u32 height = colour.extent.height;
    const u32 count = static_cast<u32>(chain_.size());

    profiler.begin_pass(cmd, "post", 1.0);

    // The composite wrote the image this reads. Everything after this is ordered dispatch by
    // dispatch by `barrier_between_dispatches`.
    barrier_between_dispatches(cmd);

    // Every image this stage owns into GENERAL, here, before the first dispatch and whether or not
    // the half that uses it was asked for.
    //
    // From UNDEFINED rather than from GENERAL, because none of them carries anything between
    // frames -- each is rewritten in full every time -- and discarding is the cheaper transition.
    //
    // Whether or not it is used, because a descriptor SET names all of its images and the layout a
    // submission expects is checked against every one of them, not against the ones the shader's
    // branch happens to reach. The shutter's set names the chain's head it never reads, and doing
    // this per half left that one image in UNDEFINED at submit: the picture was right and the
    // validation layer was the only thing that said otherwise, which is trap 1 exactly.
    image_barrier(cmd, scratch_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_NONE,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    for (const GpuImage& level : chain_) {
        image_barrier(cmd, level.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_NONE,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    }

    u64 bytes = 0;
    const auto dispatch = [&](const ComputePipeline& pipeline, VkDescriptorSet set,
                              const void* push, u32 push_bytes, u32 w, u32 h, bool dynamic) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout(), 0, 1, &set,
                                dynamic ? 1 : 0, dynamic ? &params_offset : nullptr);
        vkCmdPushConstants(cmd, pipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes,
                           push);
        vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
    };

    // --- the shutter (R6b) -----------------------------------------------------------------
    //
    // Into the scratch image, so that the taps read a composite nothing is writing.
    if (settings.motion_blur) {
        PostPush push{};
        push.pass = kPassShutter;
        push.bloom_scale = 0.0f;
        dispatch(post_, post_sets_[0], &push, sizeof(push), width, height, true);
        bytes += static_cast<u64>(width) * height * 8;
        barrier_between_dispatches(cmd);
    }

    // --- the glare chain (R6a) -------------------------------------------------------------
    f32 bloom_scale = 0.0f;
    if (settings.bloom) {
        u32 source_width = width;
        u32 source_height = height;
        for (u32 i = 0; i < count; ++i) {
            DownPush push{};
            push.source_size[0] = static_cast<i32>(source_width);
            push.source_size[1] = static_cast<i32>(source_height);
            push.destination_size[0] = static_cast<i32>(chain_[i].extent.width);
            push.destination_size[1] = static_cast<i32>(chain_[i].extent.height);
            push.mode = (i == 0) ? 0u : 1u;
            const VkDescriptorSet set =
                (i == 0) ? (settings.motion_blur ? down_sets_[1] : down_sets_[0])
                         : down_sets_[2 + (i - 1)];
            dispatch(down_, set, &push, sizeof(push), chain_[i].extent.width,
                     chain_[i].extent.height, false);
            bytes += static_cast<u64>(source_width) * source_height * (i == 0 ? 4 : 8);
            bytes += static_cast<u64>(chain_[i].extent.width) * chain_[i].extent.height * 8;
            barrier_between_dispatches(cmd);
            source_width = chain_[i].extent.width;
            source_height = chain_[i].extent.height;
        }

        // Coarsest first, each adding itself into the level above it.
        for (u32 i = count - 1; i-- > 0;) {
            UpPush push{};
            push.coarse_size[0] = static_cast<i32>(chain_[i + 1].extent.width);
            push.coarse_size[1] = static_cast<i32>(chain_[i + 1].extent.height);
            push.fine_size[0] = static_cast<i32>(chain_[i].extent.width);
            push.fine_size[1] = static_cast<i32>(chain_[i].extent.height);
            dispatch(up_, up_sets_[i], &push, sizeof(push), chain_[i].extent.width,
                     chain_[i].extent.height, false);
            bytes += static_cast<u64>(chain_[i].extent.width) * chain_[i].extent.height * 24;
            barrier_between_dispatches(cmd);
        }

        bloom_scale = kBloomIntensity / static_cast<f32>(count);
    }

    // --- the combine -----------------------------------------------------------------------
    {
        PostPush push{};
        push.pass = kPassCombine;
        push.bloom_scale = bloom_scale;
        dispatch(post_, settings.motion_blur ? post_sets_[1] : post_sets_[2], &push, sizeof(push),
                 width, height, true);
        bytes += static_cast<u64>(width) * height * 8;
    }

    profiler.add_bytes(bytes);
    profiler.end_pass(cmd);
}

}  // namespace ws
