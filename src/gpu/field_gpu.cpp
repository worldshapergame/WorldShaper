#include "gpu/field_gpu.hpp"

#include <cstring>

#include "core/log.hpp"
#include "core/time.hpp"

namespace ws {
namespace {

// The push block, byte for byte `layout(push_constant) uniform FieldPush` in field_types.glsl.
struct FieldPush {
    u32 root;
    u32 bounds;
    u32 has_bounds;
    u32 rule_count;
    u32 box_count;
    u32 first_type;
    f32 half_cell;
    u32 flags;
};
static_assert(sizeof(FieldPush) == 32, "must match FieldPush in field_types.glsl");

constexpr u32 kFlagNoRescue = 1u;

// Half a cell's diagonal, in voxels: the furthest a surface can be from a cell's centre and still
// pass through that cell, and therefore the reach of the thin-feature rescue.
//
// Handed to the shader rather than written in it, because this is the same constant
// `kHalfCellDiagonal` in src/forge/sample.cpp and the two disagreeing is exactly D613 — the rescue's
// own test carried the number and the box test did not allow for it, so a box was declared empty
// over cells the per-voxel rule would have kept, and the same node came out differently sampled
// alone and sampled inside the building.
constexpr f32 kHalfCellDiagonal = 0.8660254f;

f32 narrow(f64 v) { return static_cast<f32>(v); }

void write_box(f32* lo, f32* hi, const forge::Field::Aabb& box) {
    lo[0] = narrow(box.low.x);
    lo[1] = narrow(box.low.y);
    lo[2] = narrow(box.low.z);
    hi[0] = narrow(box.high.x);
    hi[1] = narrow(box.high.y);
    hi[2] = narrow(box.high.z);
}

}  // namespace

FieldSampler::~FieldSampler() { destroy(); }

bool FieldSampler::build_layout() {
    const VkDevice device = device_->handle();

    VkDescriptorSetLayoutBinding bindings[7]{};
    for (u32 i = 0; i < 7; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = 7;
    info.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device, &info, nullptr, &set_layout_) != VK_SUCCESS) {
        return false;
    }

    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    if (vkCreateDescriptorPool(device, &pool, nullptr, &pool_) != VK_SUCCESS) return false;

    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &set_layout_;
    return vkAllocateDescriptorSets(device, &allocate, &set_) == VK_SUCCESS;
}

void FieldSampler::write_descriptors() {
    const GpuBuffer* buffers[7] = {&nodes_, &params_, &rules_, &boxes_, &out_, &out_, &pieces_};
    // Binding 5 is the second half of the output block rather than a buffer of its own: one
    // allocation, two ranges, so a batch is one copy back and not two.
    VkDescriptorBufferInfo infos[7]{};
    VkWriteDescriptorSet writes[7]{};
    const u64 half = out_.size / 2;
    for (u32 i = 0; i < 7; ++i) {
        infos[i].buffer = buffers[i]->buffer;
        infos[i].offset = (i == 5) ? half : 0;
        infos[i].range = (i == 4 || i == 5) ? half : buffers[i]->size;
        writes[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = set_;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device_->handle(), 7, writes, 0, nullptr);
}

bool FieldSampler::create(Device& device, const std::filesystem::path& source_dir,
                          const std::filesystem::path& spirv_dir) {
    device_ = &device;

    if (!build_layout()) {
        why_not_ = "the descriptor layout would not build";
        return false;
    }

    nodes_ = create_device_buffer(device, kMaxNodes * sizeof(GpuFieldNode),
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "field nodes");
    params_ = create_device_buffer(device, kMaxParams * sizeof(f32),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "field parameters");
    rules_ = create_device_buffer(device, kMaxRules * sizeof(GpuPaintRuleRecord),
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "field paint rules");
    pieces_ = create_device_buffer(device, kMaxPieces * sizeof(GpuBoxRecord),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "field rule pieces");
    boxes_ = create_staging_buffer(device, kMaxBoxes * sizeof(GpuSampleBoxRecord), "field boxes",
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    // Two ranges in one allocation: the types, then the mask, one word each per cell.
    const u64 cells = static_cast<u64>(kMaxBoxes) * kNodeCells;
    out_ = create_device_buffer(device, cells * sizeof(u32) * 2,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                "field sample out");
    readback_ = create_staging_buffer(device, out_.size, "field sample readback");

    if (!nodes_.valid() || !params_.valid() || !rules_.valid() || !pieces_.valid() ||
        !boxes_.valid() || !out_.valid() || !readback_.valid()) {
        why_not_ = "a buffer would not allocate";
        return false;
    }
    write_descriptors();

    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = device.graphics_family();
    if (vkCreateCommandPool(device.handle(), &pool, nullptr, &commands_) != VK_SUCCESS) {
        why_not_ = "no command pool";
        return false;
    }
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = commands_;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device.handle(), &allocate, &cmd_) != VK_SUCCESS) {
        why_not_ = "no command buffer";
        return false;
    }
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(device.handle(), &fence, nullptr, &fence_) != VK_SUCCESS) {
        why_not_ = "no fence";
        return false;
    }

    // Two timestamps round the dispatch, so what the card spent is measured on the card. A wall
    // clock round a submission measures the queue as well, and the queue is the frame.
    VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    query.queryType = VK_QUERY_TYPE_TIMESTAMP;
    query.queryCount = 2;
    if (vkCreateQueryPool(device.handle(), &query, nullptr, &timestamps_) != VK_SUCCESS) {
        timestamps_ = VK_NULL_HANDLE;   // not fatal; the host clock still reports
    }

    if (!pipeline_.create(device, source_dir / "sample_field.comp",
                          spirv_dir / "sample_field.comp.spv", set_layout_, sizeof(FieldPush))) {
        why_not_ = "sample_field.comp would not build: " + pipeline_.last_error();
        return false;
    }
    return true;
}

void FieldSampler::destroy() {
    if (device_ == nullptr) return;
    const VkDevice device = device_->handle();
    if (in_flight_ > 0) vkWaitForFences(device, 1, &fence_, VK_TRUE, ~0ull);
    pipeline_.destroy();
    if (timestamps_ != VK_NULL_HANDLE) vkDestroyQueryPool(device, timestamps_, nullptr);
    if (fence_ != VK_NULL_HANDLE) vkDestroyFence(device, fence_, nullptr);
    if (commands_ != VK_NULL_HANDLE) vkDestroyCommandPool(device, commands_, nullptr);
    if (pool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, pool_, nullptr);
    if (set_layout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, set_layout_, nullptr);
    destroy_buffer(*device_, nodes_);
    destroy_buffer(*device_, params_);
    destroy_buffer(*device_, rules_);
    destroy_buffer(*device_, pieces_);
    destroy_buffer(*device_, boxes_);
    destroy_buffer(*device_, out_);
    destroy_buffer(*device_, readback_);
    timestamps_ = VK_NULL_HANDLE;
    fence_ = VK_NULL_HANDLE;
    commands_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    set_ = VK_NULL_HANDLE;
    node_count_ = 0;
    in_flight_ = 0;
    device_ = nullptr;
}

// One blocking staging copy. Called once per clip and never in a frame, so the wait is not a stall
// anybody can see — and doing it any other way would mean a second fence and a state machine for
// something that happens when a world is opened.
bool FieldSampler::upload_device_buffer(GpuBuffer& target, const void* data, u64 bytes,
                                        const char* name) {
    if (bytes == 0) return true;
    if (bytes > target.size) {
        why_not_ = std::string("the ") + name + " do not fit the buffer they were given";
        return false;
    }
    GpuBuffer staging = create_staging_buffer(*device_, bytes, "field upload staging");
    if (!staging.valid()) {
        why_not_ = "no staging buffer";
        return false;
    }
    std::memcpy(staging.mapped, data, static_cast<size_t>(bytes));

    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &begin);
    VkBufferCopy copy{};
    copy.size = bytes;
    vkCmdCopyBuffer(cmd_, staging.buffer, target.buffer, 1, &copy);
    vkEndCommandBuffer(cmd_);

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd_;
    vkResetFences(device_->handle(), 1, &fence_);
    WS_VK(vkQueueSubmit(device_->graphics_queue(), 1, &submit, fence_));
    vkWaitForFences(device_->handle(), 1, &fence_, VK_TRUE, ~0ull);
    destroy_buffer(*device_, staging);
    return true;
}

bool FieldSampler::upload(const forge::SamplePlan& plan, u32 bounds_node, bool has_bounds) {
    node_count_ = 0;
    if (!valid() || !plan.ok()) {
        why_not_ = "no pipeline, or no plan";
        return false;
    }
    const forge::Field& field = *plan.field;
    const usize count = field.size();
    if (count > kMaxNodes) {
        why_not_ = "the field has more nodes than the card's buffer holds";
        return false;
    }

    std::vector<GpuFieldNode> nodes(count);
    for (usize i = 0; i < count; ++i) {
        const forge::Node& from = field.node(static_cast<u32>(i));
        GpuFieldNode& to = nodes[i];
        to.op = static_cast<u32>(from.op);
        to.children = from.children;
        for (u32 c = 0; c < 4; ++c) to.child[c] = from.child[c];
        for (u32 a = 0; a < 8; ++a) to.a[a] = narrow(from.a[a]);
        write_box(to.lo, to.hi, field.bounds_of(static_cast<u32>(i)));
    }

    std::vector<f32> params(field.parameter_count());
    for (usize i = 0; i < params.size(); ++i) params[i] = narrow(field.parameter_value(i));
    if (params.size() > kMaxParams) {
        why_not_ = "more parameters than the card's buffer holds";
        return false;
    }

    // The WIDENED rules, because those are what the descent tests against: a band grown by what a
    // displacement can move the surface it is keyed on.
    const usize rules = plan.widened.size();
    if (rules > kMaxRules) {
        why_not_ = "more paint rules than the card's buffer holds";
        return false;
    }
    std::vector<GpuPaintRuleRecord> rule_records(rules);
    std::vector<GpuBoxRecord> pieces;
    for (usize i = 0; i < rules; ++i) {
        const forge::PaintRule& from = plan.widened[i];
        GpuPaintRuleRecord& to = rule_records[i];
        to.test = from.test;
        to.type = from.type;
        to.facing_axis = from.facing_axis;
        to.facing_min = narrow(from.facing_min);
        to.low = narrow(from.low);
        to.high = narrow(from.high);
        to.slack = (i < plan.rule_slack.size()) ? narrow(plan.rule_slack[i]) : 0.0f;
        to.pad = 0;
        to.piece_from = static_cast<u32>(pieces.size());
        if (!plan.rule_piece_at.empty() && i + 1 < plan.rule_piece_at.size()) {
            for (u32 p = plan.rule_piece_at[i]; p < plan.rule_piece_at[i + 1]; ++p) {
                GpuBoxRecord piece{};
                write_box(piece.lo, piece.hi, plan.rule_piece[p]);
                pieces.push_back(piece);
            }
        }
        to.piece_to = static_cast<u32>(pieces.size());
        if (i < plan.rule_box.size()) {
            write_box(to.lo, to.hi, plan.rule_box[i]);
        } else {
            const forge::Field::Aabb everywhere;
            write_box(to.lo, to.hi, everywhere);
        }
    }
    if (pieces.size() > kMaxPieces) {
        why_not_ = "more rule pieces than the card's buffer holds";
        return false;
    }

    if (!upload_device_buffer(nodes_, nodes.data(), nodes.size() * sizeof(GpuFieldNode), "nodes") ||
        !upload_device_buffer(params_, params.data(), params.size() * sizeof(f32), "parameters") ||
        !upload_device_buffer(rules_, rule_records.data(),
                              rule_records.size() * sizeof(GpuPaintRuleRecord), "paint rules") ||
        !upload_device_buffer(pieces_, pieces.data(), pieces.size() * sizeof(GpuBoxRecord),
                              "rule pieces")) {
        return false;
    }

    root_ = plan.root;
    bounds_ = bounds_node;
    has_bounds_ = has_bounds ? 1u : 0u;
    rule_count_ = static_cast<u32>(rules);
    first_type_ = rules > 0 ? plan.widened.front().type : 0u;
    node_count_ = static_cast<u32>(count);
    why_not_.clear();
    WS_LOG_INFO("clip",
                "the field is on the card: {} nodes ({} KB), {} parameters, {} paint rules, {} "
                "zone pieces",
                node_count_, (node_count_ * sizeof(GpuFieldNode)) / 1024, params.size(),
                rule_count_, pieces.size());
    return true;
}

bool FieldSampler::submit(const std::vector<GpuSampleBoxRecord>& boxes) {
    if (!valid() || node_count_ == 0 || in_flight_ > 0) return false;
    if (boxes.empty() || boxes.size() > kMaxBoxes) return false;

    submit_began_ns_ = now_ns();
    std::memcpy(boxes_.mapped, boxes.data(), boxes.size() * sizeof(GpuSampleBoxRecord));

    const u32 count = static_cast<u32>(boxes.size());
    const u32 cells = count * kNodeCells;

    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &begin);

    if (timestamps_ != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(cmd_, timestamps_, 0, 2);
        vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps_, 0);
    }

    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.pipeline());
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.layout(), 0, 1, &set_,
                            0, nullptr);
    FieldPush push{};
    push.root = root_;
    push.bounds = bounds_;
    push.has_bounds = has_bounds_;
    push.rule_count = rule_count_;
    push.box_count = count;
    push.first_type = first_type_;
    push.half_cell = kHalfCellDiagonal;
    push.flags = rescue_ ? 0u : kFlagNoRescue;
    vkCmdPushConstants(cmd_, pipeline_.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                       &push);
    vkCmdDispatch(cmd_, (cells + 63) / 64, 1, 1);

    if (timestamps_ != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps_, 1);
    }

    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);

    // Only the cells this batch actually wrote, from both halves of the block. Copying the whole
    // eight megabytes when a batch is a hundred and twenty-eight nodes would put the readback's
    // cost on the batch SIZE rather than on the batch.
    const u64 half = out_.size / 2;
    VkBufferCopy copies[2]{};
    copies[0].srcOffset = 0;
    copies[0].dstOffset = 0;
    copies[0].size = static_cast<u64>(cells) * sizeof(u32);
    copies[1].srcOffset = half;
    copies[1].dstOffset = half;
    copies[1].size = copies[0].size;
    vkCmdCopyBuffer(cmd_, out_.buffer, readback_.buffer, 2, copies);

    vkEndCommandBuffer(cmd_);

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd_;
    vkResetFences(device_->handle(), 1, &fence_);
    WS_VK(vkQueueSubmit(device_->graphics_queue(), 1, &submit, fence_));
    in_flight_ = count;
    return true;
}

bool FieldSampler::ready() {
    if (in_flight_ == 0) return false;
    if (vkGetFenceStatus(device_->handle(), fence_) != VK_SUCCESS) return false;

    const u32 count = in_flight_;
    const u32 cells = count * kNodeCells;
    types_.resize(cells);
    inside_.resize(cells);

    const u8* base = static_cast<const u8*>(readback_.mapped);
    const u32* out_type = reinterpret_cast<const u32*>(base);
    const u32* out_inside = reinterpret_cast<const u32*>(base + readback_.size / 2);
    for (u32 i = 0; i < cells; ++i) {
        types_[i] = static_cast<VoxelTypeId>(out_type[i]);
        inside_[i] = static_cast<u8>(out_inside[i] != 0);
    }

    last_gpu_ms_ = 0.0;
    if (timestamps_ != VK_NULL_HANDLE) {
        u64 stamps[2]{};
        if (vkGetQueryPoolResults(device_->handle(), timestamps_, 0, 2, sizeof(stamps), stamps,
                                  sizeof(u64), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const f64 period = static_cast<f64>(device_->caps().timestamp_period_ns);
            last_gpu_ms_ = static_cast<f64>(stamps[1] - stamps[0]) * period * 1.0e-6;
        }
    }
    last_host_ms_ = ns_to_ms(now_ns() - submit_began_ns_);
    delivered_ = count;
    in_flight_ = 0;
    return true;
}

}  // namespace ws
