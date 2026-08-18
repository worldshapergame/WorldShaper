#pragma once
// R12 — the field on the card: a node of the render tree filled with voxels by a compute shader
// instead of by a worker thread.
//
// # Why this exists, in one measurement
//
// The ladder that fills the world in while somebody is playing asks `forge::sample` for one node at
// a time. A batch is about 65,536 voxels in about 500 ms — **7.6 µs a voxel** — and that is field
// evaluation: a walk of a several-thousand-node expression tree, per voxel, against hundreds of
// paint rules, on half the machine's threads. Every scheduling fault around it has already been
// found: D622 took three waits out and was worth 4.8x, D619 took a starvation out and was worth
// another 3x. What is left is not waiting. **D674 and D675 reached the same conclusion from
// opposite ends — a hundredfold is R12 and nothing short of it.**
//
// # What it does and does not own
//
// It owns the SHAPE and the PAINT of a node: the same per-voxel question `forge::sample`'s descent
// is an optimisation of. It does not own `apply_variation` or `forge::despeckle`, which are
// judgements over a finished clip rather than over a voxel and stay on the host — see the header of
// `shaders/sample_field.comp`.
//
// # Threading, and the one rule
//
// **Submit and poll from the main thread only.** A `VkQueue` is not thread-safe and the frame is
// submitted from the main thread, so this shares that thread rather than taking a lock nobody else
// takes. That is not a limitation in practice: the whole point is that the work is not on a CPU
// thread at all, and a submission is a few hundred microseconds of recording against tens of
// milliseconds of dispatch. The ladder therefore drives it from `pump_refinement`, which already
// runs once a frame on the main thread.
//
// It is asynchronous and fence-polled rather than blocking: `submit` records and returns, `ready`
// answers when the card is done, and nothing waits inside a frame.

#include <filesystem>
#include <vector>

#include "forge/sample.hpp"
#include "gpu/buffer.hpp"
#include "gpu/device.hpp"
#include "gpu/shader.hpp"
#include "world/voxel_type.hpp"

namespace ws {

// One node of the field as the card reads it. Scalars only, so std430 lays it out with a stride of
// four and this matches `struct FieldNode` in shaders/field_types.glsl member for member. A `vec3`
// would align to sixteen and put a hole in the middle of every record.
struct GpuFieldNode {
    u32 op;
    u32 children;
    u32 child[4];
    f32 a[8];
    f32 lo[3];
    f32 hi[3];
};
static_assert(sizeof(GpuFieldNode) == 80, "must match struct FieldNode in field_types.glsl");

struct GpuPaintRuleRecord {
    u32 test;
    u32 type;
    u32 facing_axis;
    f32 facing_min;
    f32 low;
    f32 high;
    f32 slack;
    u32 piece_from;
    u32 piece_to;
    u32 pad;
    f32 lo[3];
    f32 hi[3];
};
static_assert(sizeof(GpuPaintRuleRecord) == 64, "must match struct GpuPaintRule in field_types.glsl");

struct GpuBoxRecord {
    f32 lo[3];
    f32 hi[3];
};
static_assert(sizeof(GpuBoxRecord) == 24, "must match struct GpuBox in field_types.glsl");

// One node of the render tree to fill: where its first voxel sits on the sample grid, and how big a
// voxel is there. Always `forge::kNodeVoxels` cubed, which is what makes the dispatch a flat index.
struct GpuSampleBoxRecord {
    i32 lo[3];
    f32 voxel;
};
static_assert(sizeof(GpuSampleBoxRecord) == 16, "must match struct GpuSampleBox in field_types.glsl");

class FieldSampler {
public:
    FieldSampler() = default;
    ~FieldSampler();

    FieldSampler(const FieldSampler&) = delete;
    FieldSampler& operator=(const FieldSampler&) = delete;

    // The pipeline, the descriptor set and the buffers that do not depend on a clip.
    bool create(Device& device, const std::filesystem::path& source_dir,
                const std::filesystem::path& spirv_dir);
    void destroy();
    bool valid() const { return pipeline_.pipeline() != VK_NULL_HANDLE; }

    // The clip: its field, its parameters and the plan's widened rules with their boxes. Once per
    // clip, and again whenever the plan is rebuilt.
    //
    // Returns false — and leaves the sampler unusable, so the ladder falls back to the CPU — when
    // the field does not fit the buffers it was given. A field that silently sampled its first
    // n nodes would be a different building with no error, which is the shape of fault trap 7 is
    // about.
    bool upload(const forge::SamplePlan& plan, u32 bounds_node, bool has_bounds);
    bool loaded() const { return node_count_ > 0; }
    const std::string& why_not() const { return why_not_; }

    // How many nodes one submission may carry. A node is 512 cells, so this is the dispatch width
    // divided by 512.
    static constexpr u32 kMaxBoxes = 2048;

    // Record and submit. `boxes` must be at most kMaxBoxes. Returns false if a batch is already in
    // flight, which the caller treats as "come back next frame".
    bool submit(const std::vector<GpuSampleBoxRecord>& boxes);
    bool busy() const { return in_flight_ > 0; }

    // Has the card finished? Non-blocking. When it answers true the results are in `types()` and
    // `inside()`, 512 entries per box in the order they were submitted, and the sampler is idle.
    bool ready();

    const std::vector<VoxelTypeId>& types() const { return types_; }
    const std::vector<u8>& inside() const { return inside_; }
    u32 delivered() const { return delivered_; }

    // Milliseconds the card spent on the last batch, from the timestamps around the dispatch. The
    // ladder reports this where it used to report the worker's own clock, so the two arms of
    // `--cpu-sample` are the same number measured the same way.
    f64 last_gpu_ms() const { return last_gpu_ms_; }
    // ...and how long the host spent recording and reading back, which is the part that is NOT
    // free and the part a hundredfold can hide behind.
    f64 last_host_ms() const { return last_host_ms_; }

    // The thin-feature rescue off, as a control arm. Every other flag in this repository that turns
    // a rule off is one of these; see documentation/22-rewrite-handover.md §7.
    void set_rescue(bool on) { rescue_ = on; }

    // Count the nodes each cell walks instead of building a world. An instrument: what a dispatch
    // costs is (nodes walked) x (what a step costs), and no clock can tell those two apart. See
    // `ws_field_visits` in shaders/field_types.glsl.
    void set_count_visits(bool on) { count_visits_ = on; }
    bool counting_visits() const { return count_visits_; }
    u64 visits() const { return visits_; }
    u64 visited_cells() const { return visited_cells_; }

    void reload_if_changed() { pipeline_.reload_if_changed(); }

private:
    bool build_layout();
    void write_descriptors();
    bool upload_device_buffer(GpuBuffer& target, const void* data, u64 bytes, const char* name);

    Device* device_ = nullptr;
    ComputePipeline pipeline_;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;

    VkCommandPool commands_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkQueryPool timestamps_ = VK_NULL_HANDLE;

    GpuBuffer nodes_;
    GpuBuffer params_;
    GpuBuffer rules_;
    GpuBuffer pieces_;
    GpuBuffer boxes_;        // host-visible; the batch is written straight into it
    GpuBuffer out_;          // device-local, what the shader writes
    GpuBuffer readback_;     // host-visible, copied from `out_` in the same submission

    std::vector<VoxelTypeId> types_;
    std::vector<u8> inside_;
    std::string why_not_;

    u32 node_count_ = 0;
    u32 rule_count_ = 0;
    u32 root_ = 0;
    u32 bounds_ = 0;
    u32 has_bounds_ = 0;
    u32 first_type_ = 0;
    u32 in_flight_ = 0;      // boxes in the batch the card is working on
    u32 delivered_ = 0;
    bool rescue_ = true;
    bool count_visits_ = false;
    u64 visits_ = 0;
    u64 visited_cells_ = 0;
    f64 last_gpu_ms_ = 0.0;
    f64 last_host_ms_ = 0.0;
    u64 submit_began_ns_ = 0;

    static constexpr u32 kNodeCells = 512;      // forge::kNodeVoxels cubed
    static constexpr u64 kMaxNodes = 262144;    // the estate's field is 3,744; this is room to grow
    static constexpr u64 kMaxRules = 8192;
    static constexpr u64 kMaxPieces = 65536;
    static constexpr u64 kMaxParams = 4096;
};

}  // namespace ws
