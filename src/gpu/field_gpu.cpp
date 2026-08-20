#include "gpu/field_gpu.hpp"

#include <cstdlib>
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
constexpr u32 kFlagCountVisits = 2u;
constexpr u32 kFlagNoAccel = 4u;   // WS_FIELD_FLAG_NO_ACCEL, --no-field-accel

// R12d — the specialisation constants the shader declares, one per op plus the instrument.
//
// `constant_id` IS the op's own number, which is why the shader's guard on a case can be written
// `WS_USES(WS_OP_TORUS)` and nothing anywhere writes 8 twice. `kSpecDivergence` is one past the
// last op, and `WS_OP_COUNT` is what the shader calls that number.
constexpr u32 kSpecOps = static_cast<u32>(forge::Op::Power) + 1u;
constexpr u32 kSpecDivergence = kSpecOps;
constexpr u32 kSpecCount = kSpecOps + 1u;

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

// R12 — the accelerator's word for one node: the child count, and what is already known about the
// boxes that were going to be uploaded anyway.
//
// **It states nothing that is not already in `bounds_`.** The temptation here is to work out a
// bound for a node the CPU could not bound — a quarter of the estate's tree carries no box (D675) —
// and it must be resisted: a card that can reject a subtree the CPU cannot is a card building a
// different world, which is what `--gpu-sample-check` exists to catch, and D646 measured the sound
// version of that idea at 45x for a byte-identical building. Every bit below is a `bounds_of` call
// the walk would otherwise make a hundred million times.
//
// The threshold is the same one the shader reads on the narrowed box (`<= -1e29` or `>= 1e29`), and
// narrowing f64 to f32 cannot move a value across either — 1e30 stays 1e30 and a real building's
// metres stay metres — so the two agree by construction rather than by arithmetic luck. What is NOT
// the same is `Aabb::infinite()`, which this used to call; see `says_nothing` directly below.

// A box that says NOTHING — unbounded on every axis, so no point can ever be outside it.
//
// **This is not `Aabb::infinite()`, and using that here was the host half of the same fault
// `ws_box_away_sq` carried.** `infinite()` asks about the X AXIS ONLY (field.hpp:757), which is the
// question D722 found `away_from` short-cutting on and removed: a ground plane is `y <= 0` and
// nothing else, and it is a perfectly good bound on y. Reading it as "no box" made the walk hand
// the shader a cleared `kNodeChild0` bit, and `ws_child_away_sq` then answers nought for that child
// without looking — so the cull could not fire even once the shader's own arithmetic was fixed.
//
// The bit has to mean exactly "`ws_box_away_sq` might answer more than nought", which is "bounded
// on at least one axis". Then the fast path and the slow path agree by construction rather than by
// luck, which is what the bit is for.
bool says_nothing(const forge::Field::Aabb& box) {
    const bool x = box.low.x <= -1e29 || box.high.x >= 1e29;
    const bool y = box.low.y <= -1e29 || box.high.y >= 1e29;
    const bool z = box.low.z <= -1e29 || box.high.z >= 1e29;
    return x && y && z;
}

u32 pack_cull_word(const forge::Field& field, u32 at) {
    const forge::Node& n = field.node(at);
    const u32 children = n.children;
    u32 word = children & kNodeCountMask;
    if (!says_nothing(field.bounds_of(at))) word |= kNodeBounded;

    u32 bounded_children = 0;
    const u32 slots = (children < 4u) ? children : 4u;
    for (u32 c = 0; c < slots; ++c) {
        const u32 child = n.child[c];
        if (child >= field.size()) continue;
        if (says_nothing(field.bounds_of(child))) continue;
        word |= kNodeChild0 << c;
        ++bounded_children;
    }
    // More than one child and something to sort BY. With no bounded child every key is nought, a
    // stable sort of equal keys is the identity, and the cull's `away > 0` can never fire — so
    // skipping the sort there is the same order and the same decisions, provably, rather than a
    // shortcut round `Field::eval` sorting unconditionally.
    if (children > 1u && bounded_children > 0) word |= kNodeCullable;
    return word;
}

// Which of the sixty-seven ops this clip can actually reach, walked from every root the shader is
// ever handed.
//
// **The roots are the plural and that is the whole trap.** `sample_field.comp` asks about
// `field_push.root`, about `field_push.bounds` — the shape saying which cells are part of the clip
// — and about `paint_rules.items[r].test` for every rule that survives its box, which on the
// facility is six hundred and twenty-eight separate little expressions. A set collected from the
// solid alone would be missing every op the weathering is written in, and a shader specialised
// against it would answer a rule's test with whatever a missing case falls through to. That is not
// a slow shader, it is a different building.
std::vector<bool> reachable_ops(const forge::Field& field, const std::vector<u32>& roots) {
    std::vector<bool> ops(static_cast<usize>(forge::Op::Power) + 1u, false);
    const u32 count = static_cast<u32>(field.size());
    std::vector<bool> seen(count, false);
    std::vector<u32> stack;
    for (const u32 root : roots) {
        if (root >= count || seen[root]) continue;
        stack.push_back(root);
        seen[root] = true;
        while (!stack.empty()) {
            const u32 at = stack.back();
            stack.pop_back();
            const forge::Node& n = field.node(at);
            const usize op = static_cast<usize>(n.op);
            if (op < ops.size()) ops[op] = true;
            const u32 slots = (n.children < 4u) ? n.children : 4u;
            for (u32 c = 0; c < slots; ++c) {
                const u32 child = n.child[c];
                if (child >= count || seen[child]) continue;
                seen[child] = true;
                stack.push_back(child);
            }
        }
    }
    return ops;
}

// Which bucket a warp's mean distinct-op count falls in. Bucketed rather than averaged because the
// decision this measurement is for is a threshold — "two or three" is one build and "ten or more"
// is a different one — and a mean over a bimodal population would land between the two answers and
// name neither.
u32 divergence_bucket(f64 mean) {
    if (mean < 1.5) return 0;
    if (mean < 2.5) return 1;
    if (mean < 3.5) return 2;
    if (mean < 6.0) return 3;
    if (mean < 10.0) return 4;
    if (mean < 16.0) return 5;
    return 6;
}

}  // namespace

FieldSampler::~FieldSampler() { destroy(); }

void FieldSampler::drop_specialised() {
    if (device_ == nullptr) return;
    for (auto& entry : specialised_) {
        if (entry.second != VK_NULL_HANDLE) vkDestroyPipeline(device_->handle(), entry.second, nullptr);
    }
    specialised_.clear();
    active_ = VK_NULL_HANDLE;
}

// R12d — one pipeline per distinct op set, compiled from the SAME SPIR-V the general pipeline uses.
//
// There is no runtime shader compiler here and no generated GLSL. `sample_field.comp.spv` declares
// sixty-seven `layout(constant_id = N) const bool` guards defaulting to true; this hands the driver
// a `VkSpecializationInfo` saying which are false, and the driver folds them and deletes the cases.
// The cost is one `vkCreateComputePipelines` per clip, and it is measured rather than assumed --
// see the line it logs.
//
// **Every failure path returns VK_NULL_HANDLE and the caller falls back to the general pipeline.**
// A clip that could not be specialised must still be sampled; a clip that is sampled by the wrong
// shader must not exist.
VkPipeline FieldSampler::pipeline_for(const std::vector<bool>& ops, bool measure) {
    if (!valid() || ops.size() != kSpecOps) return VK_NULL_HANDLE;
    if (!specialise_ && !measure) return VK_NULL_HANDLE;
    ++pipelines_asked_;

    // **The divergence run must measure the shader that SHIPS.** With specialisation off — which is
    // the default — a pipeline is still needed for the instrument, and it has to be the general
    // sixty-seven-op one; measuring divergence in a narrowed switch would be a number about a
    // shader nobody runs, and it is the number the whole decision rests on.
    const std::vector<bool> use = specialise_ ? ops : std::vector<bool>(kSpecOps, true);

    // The set itself as the key. Not a hash: a hash collision here is one clip running another
    // clip's shader, which is a different building and no error anywhere.
    std::string key(kSpecCount, '0');
    for (usize i = 0; i < use.size(); ++i) key[i] = use[i] ? '1' : '0';
    key[kSpecDivergence] = measure ? '1' : '0';
    const auto found = specialised_.find(key);
    if (found != specialised_.end()) return found->second;

    // Read the SPIR-V afresh rather than keeping a copy: this runs once per clip, and a copy kept
    // across a hot reload is the stale-`.spv` fault (trap 11) with a longer fuse.
    const std::vector<u32> spirv = read_spirv(spirv_path_);
    if (spirv.empty()) return VK_NULL_HANDLE;
    const VkDevice device = device_->handle();
    VkShaderModule module = create_shader_module(device, spirv);
    if (module == VK_NULL_HANDLE) return VK_NULL_HANDLE;

    std::vector<VkBool32> values(kSpecCount, VK_FALSE);
    std::vector<VkSpecializationMapEntry> entries(kSpecCount);
    for (u32 i = 0; i < kSpecCount; ++i) {
        values[i] = (i < kSpecOps) ? (use[i] ? VK_TRUE : VK_FALSE)
                                   : (measure ? VK_TRUE : VK_FALSE);
        entries[i].constantID = i;
        entries[i].offset = i * static_cast<u32>(sizeof(VkBool32));
        entries[i].size = sizeof(VkBool32);
    }
    VkSpecializationInfo spec{};
    spec.mapEntryCount = kSpecCount;
    spec.pMapEntries = entries.data();
    spec.dataSize = values.size() * sizeof(VkBool32);
    spec.pData = values.data();

    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";
    stage.pSpecializationInfo = &spec;

    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = stage;
    info.layout = pipeline_.layout();

    const u64 began = now_ns();
    VkPipeline built = VK_NULL_HANDLE;
    const VkResult result =
        vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &built);
    last_compile_ms_ = ns_to_ms(now_ns() - began);
    vkDestroyShaderModule(device, module, nullptr);
    if (result != VK_SUCCESS || built == VK_NULL_HANDLE) {
        WS_LOG_WARN("clip",
                    "the clip's own pipeline would not build ({}); the card samples with the "
                    "general shader instead",
                    static_cast<i32>(result));
        return VK_NULL_HANDLE;
    }
    ++pipelines_built_;
    specialised_.emplace(key, built);
    return built;
}

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

    // The divergence instrument, SAID out loud when it is on. A run measuring divergence builds a
    // world of nonsense and is several times slower than one that is not, and a mode nobody
    // announced is a mode somebody will take a timing beside.
    const char* diverge = std::getenv("WS_GPU_DIVERGE");
    diverge_ = diverge != nullptr && diverge[0] != '\0' && diverge[0] != '0';
    // R12d SHIPS OFF, and the reason is the measurement rather than caution.
    //
    // Specialised against `clips/facility.clip`, over the SAME 1,089 dispatches and 17.8 M cells in
    // both arms: **3.503, 3.450 and 3.507 µs a cell against a control built with the change absent
    // from the tree at 3.471, 3.497 and 3.462. 1.00x.** It is not slower and it is not faster; the
    // eighteen cases it deletes are cases no lane was standing on, and the ballot says why — a warp
    // holds 31.1 lanes over 4.03 distinct ops, so it already runs about four op bodies a turn and a
    // narrower switch cannot make it run fewer.
    //
    // What it does cost is a `vkCreateComputePipelines` at world open: **721 ms on the facility the
    // first time on a machine and 1 ms every time after**, out of the driver's own on-disk cache.
    // That is small against a ninety-second load and it is 17% of `mirror_hall.clip`'s whole run,
    // and a cost with no measured gain against it is not a trade-off (09-performance-budgets.md).
    //
    // So it is here, gated, with its instrument, for the next person who has a clip where the switch
    // really is most of the shader — `mirror_hall.clip` reaches 5 of the 67 ops — and it is off
    // until somebody measures that clip and finds a number.
    const char* wanted = std::getenv("WS_GPU_SPECIALISE");
    specialise_ = wanted != nullptr && wanted[0] != '\0' && wanted[0] != '0';
    if (specialise_) {
        WS_LOG_WARN("clip",
                    "WS_GPU_SPECIALISE is set: each clip gets a pipeline compiled for its own op "
                    "set. Measured at 1.00x on clips/facility.clip, so this is an experiment and "
                    "not the shipped path");
    }
    if (diverge_) {
        WS_LOG_WARN("clip",
                    "WS_GPU_DIVERGE is set: the card counts how many DISTINCT ops the lanes of a "
                    "warp are spread over and builds NO WORLD. Pair it with --refine-batch 1; the "
                    "ballot loop makes a dispatch several times dearer and the watchdog is 2 s.");
    }

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

    spirv_path_ = spirv_dir / "sample_field.comp.spv";
    if (!pipeline_.create(device, source_dir / "sample_field.comp", spirv_path_, set_layout_,
                          sizeof(FieldPush))) {
        why_not_ = "sample_field.comp would not build: " + pipeline_.last_error();
        return false;
    }
    reload_seen_ = pipeline_.reload_count();
    return true;
}

void FieldSampler::destroy() {
    if (device_ == nullptr) return;
    // The refusal count, SAID rather than left to be inferred from the absence of a warning.
    //
    // A refusal is not a wrong voxel, it is a voxel nobody computed, and the whole danger of it is
    // that it reads as agreement: D676's mirror refused every point of the estate and reported
    // nought sign changes over nought points, and only counting refusals separately caught it. The
    // warning beside `refused()` fires the moment one happens and says nothing at all when none do,
    // which is the same silence a run that never asked would make — so this line carries the
    // denominator and prints whatever the answer is, including the good one.
    if (answered_cells_ > 0) {
        WS_LOG_INFO("clip",
                    "the card answered {} cells over this run and REFUSED {} of them; the field "
                    "accelerator was {}",
                    answered_cells_, refused_, accelerate_ ? "ON" : "OFF (--no-field-accel)");
    }
    // ...and the DUTY CYCLE, which is the number that says whether the shader is the problem.
    //
    // Every figure this class has ever printed is per dispatch, and a per-dispatch figure cannot
    // tell a card that is slow from a card that is idle. The three spans here can: `gpu` over
    // `span` is the share of the run the card was executing, and everything between `gpu` and
    // `busy` is the host — recording, the fence poll landing on a frame boundary, and the readback.
    if (dispatches_ > 0) {
        const f64 span = duty_span_ms();
        WS_LOG_INFO("clip",
                    "the card's DUTY: {} dispatches, {} cells, {:.0f} ms of GPU and {:.0f} ms "
                    "submit-to-collect over a {:.0f} ms span — the card was computing {:.1f}% of "
                    "it, and {:.3f} ms of GPU a dispatch over {} cells each ({:.3f} us a cell); "
                    "the WORST dispatch was {:.1f} ms against a ~2000 ms watchdog",
                    dispatches_, cells_dispatched_, gpu_ms_total_, duty_busy_ms(), span,
                    (span > 0.0) ? 100.0 * gpu_ms_total_ / span : 0.0,
                    gpu_ms_total_ / static_cast<f64>(dispatches_),
                    cells_dispatched_ / dispatches_,
                    (cells_dispatched_ > 0)
                        ? 1000.0 * gpu_ms_total_ / static_cast<f64>(cells_dispatched_)
                        : 0.0,
                    worst_gpu_ms_);
    }
    // THE DIVERGENCE, which is the number that decides whether specialising the shader per clip is
    // worth building. See the block above `ws_distinct_ops` in shaders/field_types.glsl.
    if (diverge_ && div_turns_ > 0) {
        const f64 turns = static_cast<f64>(div_turns_);
        const f64 distinct = div_ops_ / turns;
        const f64 nodes = div_nodes_ / turns;
        // The active-lane count, taken the way D727 takes it rather than from a second ballot: an
        // aligned run of 32 entries is a warp, the maximum of the run is the warp's turns, and the
        // sum of the run is the lane-turns in them. Their ratio is how full the warp was.
        const f64 lanes = (div_warp_turns_ > 0)
                              ? turns / static_cast<f64>(div_warp_turns_)
                              : 0.0;
        WS_LOG_INFO("clip",
                    "the card's DIVERGENCE: {} lane-turns in {} warp-turns over {} warps -- a warp "
                    "carries {:.1f} of 32 lanes at a turn and they stand on {:.2f} DISTINCT ops "
                    "over {:.2f} DISTINCT nodes, so {:.2f} lanes share each branch and {:.2f} "
                    "share each 80-byte record; the clip can reach {} of {} ops at all",
                    div_turns_, div_warp_turns_, div_warps_, lanes, distinct, nodes,
                    (distinct > 0.0) ? lanes / distinct : 0.0,
                    (nodes > 0.0) ? lanes / nodes : 0.0, clip_ops_,
                    static_cast<u32>(forge::Op::Power) + 1u);
        WS_LOG_INFO("clip",
                    "...and the warps by their mean distinct ops: [1] {}, [2] {}, [3] {}, [4-5] "
                    "{}, [6-9] {}, [10-15] {}, [16+] {} — of {} warps",
                    div_hist_[0], div_hist_[1], div_hist_[2], div_hist_[3], div_hist_[4],
                    div_hist_[5], div_hist_[6], div_warps_);
    }
    // The cost model's three factors, together, on the run that asked for them. Printed here rather
    // than at the settle line in main.cpp, because a run cut short by `--max-seconds` never reaches
    // that line — which is every measured run there has ever been.
    if (count_visits_ && visited_cells_ > 0) {
        const f64 cells = static_cast<f64>(visited_cells_);
        WS_LOG_INFO("clip",
                    "the card's WALK: {} cells asked the field {} times ({:.2f} evaluations a "
                    "cell) and walked {} nodes — {:.0f} a cell, {:.0f} an evaluation, against a "
                    "field of {} nodes",
                    visited_cells_, evals_, static_cast<f64>(evals_) / cells, visits_,
                    static_cast<f64>(visits_) / cells,
                    (evals_ > 0) ? static_cast<f64>(visits_) / static_cast<f64>(evals_) : 0.0,
                    node_count_);
        // ...and what the warp actually spent, which is the only one of these numbers that is a
        // cost. See `lane_slots_`: a warp runs until its slowest lane is done, so the card's bill
        // is thirty-two times the WORST cell of every thirty-two, and the share of that which is
        // real work is the utilisation. D681 called this divergence and could only infer it from
        // two cameras costing 23x for 2x the visits; this counts it.
        // The one comparison that is like for like in its POINTS: one evaluation of the clip's own
        // root, at the ladder's own cell centres, on the card and on the CPU. Everything else in
        // this file divides one arm's number by the other arm's points.
        //
        // **AND THE RATIO IS NOT EXTRA WORK, which is the half worth saying out loud.** The two
        // counters count different things and neither is wrong: `Field::eval` increments once per
        // CALL, so a node is one; this shader increments once per TURN of the stack machine, and a
        // node takes as many turns as it has answers to receive — a union of four children is five,
        // and a one-child op was two until the six pure point transforms were fused. So the ratio
        // is TURNS A NODE, a property of writing a recursion as a loop, and reading it as "the card
        // walks 1.75x the tree" is the mistake D681 made three times with the refusal stamp: an
        // instrument that names a suspect is worse than one that says nothing.
        //
        // What it is good for is watching that ratio MOVE. Anything that makes the machine take
        // fewer turns for the same nodes shows up here and nowhere else.
        if (mirror_cells_ > 0) {
            WS_LOG_INFO("clip",
                        "the card AGAINST the CPU on the SAME points: one evaluation of the root "
                        "is {:.0f} TURNS of the card's machine over {:.0f} nodes in `Field::eval` "
                        "— {:.2f} turns a node, which is the machine's shape and not extra work",
                        static_cast<f64>(root_visits_) / static_cast<f64>(visited_cells_),
                        static_cast<f64>(mirror_visits_) / static_cast<f64>(mirror_cells_),
                        (mirror_visits_ > 0)
                            ? (static_cast<f64>(root_visits_) / static_cast<f64>(visited_cells_)) /
                                  (static_cast<f64>(mirror_visits_) /
                                   static_cast<f64>(mirror_cells_))
                            : 0.0);
        }
        WS_LOG_INFO("clip",
                    "the card's WARPS: {} lane-slots spent to do {} visits — the lanes were "
                    "USEFUL {:.1f}% of the time, and the worst cell of a warp walks {:.1f}x what "
                    "the mean one does",
                    lane_slots_, visits_,
                    (lane_slots_ > 0) ? 100.0 * static_cast<f64>(visits_) /
                                            static_cast<f64>(lane_slots_)
                                      : 0.0,
                    (visits_ > 0) ? static_cast<f64>(lane_slots_) / static_cast<f64>(visits_)
                                  : 0.0);
    }
    const VkDevice device = device_->handle();
    if (in_flight_ > 0) vkWaitForFences(device, 1, &fence_, VK_TRUE, ~0ull);
    drop_specialised();
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
    clip_ops_used_.clear();
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
    // Back to the general shader until this clip's own set has been worked out. A pipeline left
    // over from the PREVIOUS clip is the worst outcome available here: it runs, it is fast, and it
    // is missing whichever ops the new clip added.
    active_ = VK_NULL_HANDLE;
    clip_ops_used_.clear();
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

    bounded_nodes_ = 0;
    sortable_unions_ = 0;
    std::vector<GpuFieldNode> nodes(count);
    for (usize i = 0; i < count; ++i) {
        const forge::Node& from = field.node(static_cast<u32>(i));
        GpuFieldNode& to = nodes[i];
        to.op = static_cast<u32>(from.op);
        to.children = pack_cull_word(field, static_cast<u32>(i));
        if ((to.children & kNodeBounded) != 0) ++bounded_nodes_;
        if ((to.children & kNodeCullable) != 0 && from.op == forge::Op::Union) ++sortable_unions_;
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

    plan_ = &plan;   // only ever read by `mirror_the_walk`, which only runs under --gpu-visits
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
    // R12's accelerator, and what it can actually reach on THIS clip. Said out loud because it is
    // the one thing about the cull that varies from clip to clip and nothing else reports it: a
    // node with no box can never be rejected, an ancestor of one cannot be bounded either, and
    // this build measures 2,811 of the estate's 18,250 in that state, and D675 counted 923 of the
    // nodes a cell WALKS — a quarter of the walk, which is the denominator that decides the cost.
    // A clip where this share falls is a clip whose walk got longer, and the log is the only place
    // that would show it.
    // WHICH OPS THIS CLIP CAN REACH, from every root the shader is ever handed — the solid, the
    // bounds shape, and every paint rule's test. The set a specialised pipeline is keyed on, and
    // worth printing whether or not one is built: it is the size of the switch the walk could be,
    // against the sixty-seven it is, and nothing else in the log says it.
    {
        std::vector<u32> roots;
        roots.push_back(plan.root);
        if (has_bounds) roots.push_back(bounds_node);
        for (const forge::PaintRule& rule : plan.widened) roots.push_back(rule.test);
        clip_ops_used_ = reachable_ops(field, roots);
        clip_ops_ = 0;
        clip_op_names_.clear();
        for (usize op = 0; op < clip_ops_used_.size(); ++op) {
            if (!clip_ops_used_[op]) continue;
            ++clip_ops_;
            if (!clip_op_names_.empty()) clip_op_names_ += ' ';
            clip_op_names_ += forge::op_name(static_cast<forge::Op>(op));
        }
        WS_LOG_INFO("clip", "the clip reaches {} of {} ops from {} roots: {}", clip_ops_, kSpecOps,
                    roots.size(), clip_op_names_);

        // ...and the pipeline compiled for exactly that set. Said out loud with its cost, because
        // "opening the same clip twice compiles once" is a claim and `pipelines_built_` against
        // `pipelines_asked_` is the only thing that can support it.
        const u32 built_before = pipelines_built_;
        active_ = pipeline_for(clip_ops_used_, diverge_);
        if (active_ != VK_NULL_HANDLE && specialise_) {
            const bool fresh = pipelines_built_ > built_before;
            WS_LOG_INFO("clip",
                        "the card's shader is SPECIALISED to this clip: {} of {} ops kept, {} "
                        "deleted, {} in {:.0f} ms; {} pipelines built for {} asked",
                        clip_ops_, kSpecOps, kSpecOps - clip_ops_,
                        fresh ? "COMPILED" : "out of the cache",
                        fresh ? last_compile_ms_ : 0.0, pipelines_built_, pipelines_asked_);
        }
    }
    WS_LOG_INFO("clip",
                "field accelerator: {} of {} nodes carry a box ({:.1f}%), {} unions worth sorting",
                bounded_nodes_, node_count_,
                (node_count_ > 0)
                    ? 100.0 * static_cast<f64>(bounded_nodes_) / static_cast<f64>(node_count_)
                    : 0.0,
                sortable_unions_);
    return true;
}

// The SAME question, asked of `Field::eval`, at the SAME points — which is the only way to compare
// two walks.
//
// `--box-probe` already reports what one CPU evaluation walks and it reports it over the wrong
// points: its evaluations are the descent's box centres across a whole ancestor, most of them
// metres from any surface where a union's cull rejects nearly everything. The ladder's cells are
// all within one voxel of a surface, where it rejects far less. D722 wrote that trap down about
// `--sample-cost`'s 5.1x — *a penalty measured over the wrong nodes is not a penalty* — and it is
// just as true of a walk length.
//
// So this walks the first sixteen boxes of the run, cell for cell, at the very coordinates
// `sample_field.comp` computes, and counts what `Field::eval` visits. One evaluation of the root
// per cell in both arms, so the two numbers are over the same points — see the line that prints
// them for what the RATIO is and is not.
void FieldSampler::mirror_the_walk(const GpuSampleBoxRecord& box) {
    if (plan_ == nullptr || !plan_->ok() || plan_->field == nullptr) return;
    const forge::Field& field = *plan_->field;
    forge::reset_field_visits();
    for (u32 cell = 0; cell < kNodeCells; ++cell) {
        const i32 within[3] = {static_cast<i32>(cell & 7u), static_cast<i32>((cell >> 3u) & 7u),
                               static_cast<i32>((cell >> 6u) & 7u)};
        const forge::Vec3 p{(static_cast<f64>(box.lo[0] + within[0]) + 0.5) * box.voxel,
                            (static_cast<f64>(box.lo[1] + within[1]) + 0.5) * box.voxel,
                            (static_cast<f64>(box.lo[2] + within[2]) + 0.5) * box.voxel};
        (void)field.eval(root_, p);
    }
    mirror_visits_ += forge::field_visits();
    mirror_cells_ += kNodeCells;
    // Let the borrowed plan go the moment it has been asked enough. `SamplePlan::field` points into
    // the refinement script, which `stop_refine_worker` can pull out from under it, and a pointer
    // kept for a whole run to be used in its first second is a pointer waiting to dangle.
    if (mirror_cells_ >= 16ull * kNodeCells) plan_ = nullptr;
}

bool FieldSampler::submit(const std::vector<GpuSampleBoxRecord>& boxes) {
    if (!valid() || node_count_ == 0 || in_flight_ > 0) return false;
    if (boxes.empty() || boxes.size() > kMaxBoxes) return false;

    // Sixteen nodes of the run, and only under `--gpu-visits`, which is not building a world.
    if (count_visits_ && mirror_cells_ < 16ull * kNodeCells) mirror_the_walk(boxes.front());

    submit_began_ns_ = now_ns();
    if (first_submit_ns_ == 0) first_submit_ns_ = submit_began_ns_;
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

    // R12d — the pipeline compiled for THIS clip's op set, or the general one when there is none.
    // Rebuilt rather than reused when the source has been hot-reloaded under it: the specialised
    // pipeline came out of a `.spv` that is no longer the one on disk, and a shader edit that
    // silently did not take is trap 11 wearing a pipeline cache.
    if (pipeline_.reload_count() != reload_seen_) {
        drop_specialised();
        reload_seen_ = pipeline_.reload_count();
        active_ = clip_ops_used_.empty() ? VK_NULL_HANDLE
                                         : pipeline_for(clip_ops_used_, diverge_);
    }
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE,
                      (active_ != VK_NULL_HANDLE) ? active_ : pipeline_.pipeline());
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
    push.flags = (rescue_ ? 0u : kFlagNoRescue) | (count_visits_ ? kFlagCountVisits : 0u) |
                 (accelerate_ ? 0u : kFlagNoAccel);
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
        // AIR under `--gpu-visits`, and it has to be said rather than left to the cast.
        //
        // In that mode the type word is not a type, it is two counters (see `ws_type_or_evals`),
        // and `VoxelTypeId` is narrower than the word — so the cast handed the paste a material id
        // out of the top of the range, which indexes a palette that does not have one. It is an
        // ACCESS VIOLATION about two thirds of the way into a run, in a mode that is not building a
        // world and whose voxels nobody looks at. Trap 21's shape: an instrument's bookkeeping
        // riding in a field somebody else owns.
        types_[i] = (count_visits_ || diverge_) ? VoxelTypeId{0}
                                                : static_cast<VoxelTypeId>(out_type[i]);
        // The top bit is a REFUSAL and not part of the mask: the walk ran out of stack or out of
        // turns for that cell, and the shader has no way to assert. Counted rather than dropped --
        // a refusal that reads as "air" is a hole in a wall that nothing anywhere reports.
        const u32 word = out_inside[i];
        if (diverge_) {
            // Three numbers a lane: its own turn count in this word, and the two averages packed
            // into the type word as ten-bit fixed point. Multiplying each average back by that
            // lane's turns recovers the SUM over its turns, so the totals below are over
            // lane-turns and every turn is weighted by how many lanes were in it.
            const u32 turns = word;
            const u32 packed = out_type[i];
            const f64 ops = static_cast<f64>(packed & 0xFFFFu) / 1024.0 * static_cast<f64>(turns);
            const f64 nodes = static_cast<f64>(packed >> 16u) / 1024.0 * static_cast<f64>(turns);
            div_ops_ += ops;
            div_nodes_ += nodes;
            div_turns_ += turns;
            div_warp_ops_ += ops;
            div_warp_lane_turns_ += turns;
            if (turns > div_warp_peak_) div_warp_peak_ = turns;
            // An aligned run of thirty-two entries IS a warp — `local_size_x` is 64 and `gid` is
            // the flat invocation index — so the maximum of the run is how many turns the warp
            // took and the sum is how many lane-turns were in them.
            if ((i % 32u) == 31u || i + 1 == cells) {
                div_warp_turns_ += div_warp_peak_;
                if (div_warp_lane_turns_ > 0) {
                    ++div_warps_;
                    ++div_hist_[divergence_bucket(div_warp_ops_ /
                                                  static_cast<f64>(div_warp_lane_turns_))];
                }
                div_warp_peak_ = 0;
                div_warp_ops_ = 0.0;
                div_warp_lane_turns_ = 0;
            }
        }
        if ((word & 0x80000000u) != 0 && !count_visits_ && !diverge_) {
            ++refused_;
            // WHICH op the walk was standing on, or 128 for running out of stack. Kept as the
            // FIRST one seen rather than a histogram: a refusal is a fault to be fixed, not a
            // statistic, and one op number is what turns a hunt into a line number.
            if (refused_op_ == 0xFFFFFFFFu) refused_op_ = (word >> 8u) & 0xFFu;
        }
        inside_[i] = static_cast<u8>((word & 1u) != 0);
        if (count_visits_) {
            visits_ += word;
            // ...and how many EVALUATIONS those visits were spread over, which is the factor that
            // turns a visit count into a cost model. The type word carries it under
            // `--gpu-visits`; see `ws_field_evals` in sample_field.comp.
            // Two counts in the one spare word: the root walk in the low twenty-one bits and the
            // number of asks in the top eleven. See `ws_type_or_evals`.
            root_visits_ += out_type[i] & 0x1FFFFFu;
            evals_ += out_type[i] >> 21u;
            // THE LANE SLOTS, and this is the number every figure in this file was missing.
            //
            // `ws_field_visits` is incremented by each LANE that is still walking, so it counts
            // work done and is blind to work not done. A warp of thirty-two lanes runs one loop
            // for all of them, and it runs until the LAST one finishes — so a warp holding one
            // cell that walks 40,000 nodes and thirty-one that walk 400 costs the card 40,000
            // turns and reports 52,400 visits. The counter says the card was 100% busy and it was:
            // busy holding thirty-one idle lanes against a barrier that is the warp itself.
            //
            // The mapping is exact and needs no shader change to read. `local_size_x` is 64 and
            // `gid` is the flat invocation index, so an aligned run of thirty-two consecutive
            // entries of this buffer IS a warp on any card whose subgroup is 32 — which is every
            // NVIDIA card. The maximum of that run is how many turns the warp took; thirty-two
            // times it is how many lane-slots the card spent on them.
            warp_peak_ = (word > warp_peak_) ? word : warp_peak_;
            if ((i % 32u) == 31u || i + 1 == cells) {
                lane_slots_ += static_cast<u64>(warp_peak_) * 32ull;
                warp_peak_ = 0;
            }
        }
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
    const u64 ready_at = now_ns();
    last_host_ms_ = ns_to_ms(ready_at - submit_began_ns_);
    // The duty cycle's three spans. See `duty_gpu_ms` in the header for why a per-dispatch time
    // cannot answer the question this does.
    ++dispatches_;
    cells_dispatched_ += cells;
    gpu_ms_total_ += last_gpu_ms_;
    if (last_gpu_ms_ > worst_gpu_ms_) worst_gpu_ms_ = last_gpu_ms_;
    busy_ns_total_ += ready_at - submit_began_ns_;
    last_ready_ns_ = ready_at;
    answered_cells_ += cells;
    if (count_visits_) visited_cells_ += cells;
    delivered_ = count;
    in_flight_ = 0;
    return true;
}

}  // namespace ws
