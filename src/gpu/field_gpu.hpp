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
#include <string>
#include <unordered_map>
#include <vector>

#include "forge/sample.hpp"
#include "gpu/buffer.hpp"
#include "gpu/device.hpp"
#include "gpu/shader.hpp"
#include "world/voxel_type.hpp"

namespace ws {

// R12 — the accelerator's word, packed into the high bits of `GpuFieldNode::children`.
//
// **THESE FOUR NUMBERS ARE A SECOND COPY of the WS_NODE_* defines in shaders/field_types.glsl**,
// which is the shape of fault `test_field_gpu.cpp` exists for: a GLSL `#define` cannot be a C++
// constant, nothing about getting one of them wrong looks like getting it wrong, and the symptom
// would be a card that culls a different set of subtrees from the CPU — a quietly different
// building rather than an error. The test reads both files and holds them together. Move one and
// the suite says so.
//
// What they mean is written where the walk reads them, in field_types.glsl. In short: the low byte
// is the child count and everything above it is what the host worked out about the BOXES ALREADY
// BEING UPLOADED — never a new or a sounder bound, which D646 measured and refused at 45x.
inline constexpr u32 kNodeCountMask = 0xFFu;
inline constexpr u32 kNodeBounded = 0x100u;
inline constexpr u32 kNodeChild0 = 0x200u;
inline constexpr u32 kNodeCullable = 0x2000u;

// One node of the field as the card reads it. Scalars only, so std430 lays it out with a stride of
// four and this matches `struct FieldNode` in shaders/field_types.glsl member for member. A `vec3`
// would align to sixteen and put a hole in the middle of every record.
struct GpuFieldNode {
    u32 op;
    u32 children;   // the count in the low byte, the accelerator's word above it. See kNode*.
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

    // ...and how many the ladder is allowed to hand over at once, which is a different number and a
    // safety limit rather than a capacity.
    //
    // A batch is ONE `vkCmdDispatch`, and a node of the estate is tens of milliseconds of it.
    // Windows resets a device that has not come back inside about two seconds; `--refine-batch 2048`
    // duly returned `VK_ERROR_DEVICE_LOST` on its first dispatch (D677). 256 nodes measured at
    // 117 ms on the estate's worst camera, which is a sixteenth of the watchdog and leaves room for
    // a card slower than this one and a clip heavier than this one. A lost device is not a slow
    // frame — it is the game gone mid-session, with whatever the player was building.
    // **32, not 256, and the number came down after a measurement rather than up.** The estate's
    // ENCLOSED camera — the game's own default, standing inside the building — costs 883 ms for 128
    // nodes where the outdoor camera costs 55. At that rate 256 nodes is 1.8 s, which is not under
    // the watchdog at all; 32 is about 220 ms of the worst camera measured. Batch size was separately
    // measured not to affect throughput (64 and 256 both give 0.457 ms a node), so this costs
    // nothing and is pure headroom.
    //
    // **IT IS STILL 32, and both halves of the paragraph above are now measured rather than
    // reasoned about — one of them was right for the wrong reason and the other was wrong.**
    //
    // *"32 is about 220 ms of the worst camera measured"* is optimistic by three times. `--gpu-
    // visits`' duty line reports the WORST dispatch of a run, and on `clips/facility.clip`, cold,
    // the ladder's own nodes: **579, 655 and 658 ms at 32 boxes** over three runs. So the margin
    // against a two-second watchdog is about 3x, not 16x, and that is on the card this was
    // developed on. The sweep, same command, same clip:
    //
    //   | boxes | ms of GPU a dispatch | us a cell | WORST dispatch |
    //   |    32 |  46.8 | 2.857 |   579 ms |
    //   |    64 |  92.4 | 2.819 |   711 ms |
    //   |   128 | 189.0 | 2.884 |  1040 ms |
    //   |   256 | 423.6 | 3.233 |  1950 ms |
    //
    // *"batch size does not affect throughput"* is confirmed and is now a much stronger statement
    // than the run that produced it could make: **an eightfold bigger dispatch — 16,384
    // invocations to 131,022, which is 512 warps against 4,096 on a card with 1,728 warp slots —
    // moves the cost of a cell by nothing at all.** So the card is already at whatever ceiling it
    // has at 32 boxes, there is no idle machine to fill, and every box past that is watchdog
    // margin spent for nothing. 256 came within 50 ms of losing the device.
    //
    // The other half of the same finding is in `duty_gpu_ms`: the card is COMPUTING 93% of the
    // wall clock at 32 boxes. Neither the cap nor the round trip is what makes this slow.
    static constexpr u32 kSafeBoxes = 32;

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

    // THE DUTY CYCLE, and it is the number this class was missing.
    //
    // `last_gpu_ms()` says what a dispatch cost and says nothing at all about what the card did
    // for the rest of the second. Those are different questions and only the second one explains a
    // throughput: a sampler whose dispatches are 40 ms each and which submits one of them every
    // 120 ms is running at a third of the card whatever the shader does, and no per-dispatch
    // figure anywhere in this file would say so.
    //
    // Three spans, all in nanoseconds and all measured on the host clock round the same events:
    //
    //   `span`  — first submit to last collection. The wall clock the card was available for.
    //   `busy`  — submit to the moment `ready()` first saw the fence signalled, summed. That is
    //             an OVER-statement of what the card was executing, because the poll is once a
    //             frame; the timestamp sum below is the under-statement, and the truth is between.
    //   `gpu`   — the timestamps round the dispatch, summed. What the card actually executed.
    //
    // `gpu / span` is the share of the run the card was computing. Trap 17: a cost that tracks
    // nothing about its own output is a wait, and the question to ask of a wait is who else is
    // running.
    f64 duty_gpu_ms() const { return gpu_ms_total_; }
    f64 duty_busy_ms() const { return ns_to_ms_(busy_ns_total_); }
    f64 duty_span_ms() const {
        return (last_ready_ns_ > first_submit_ns_) ? ns_to_ms_(last_ready_ns_ - first_submit_ns_)
                                                   : 0.0;
    }
    u64 dispatches() const { return dispatches_; }
    // The WORST dispatch of the run, which is the only figure the watchdog argument may be made
    // from. A mean says nothing about a device reset: the reset is caused by the one submission
    // that ran long, and on this clip the enclosed camera is fifteen times the outdoor one.
    f64 worst_gpu_ms() const { return worst_gpu_ms_; }
    // ...and how long the host spent recording and reading back, which is the part that is NOT
    // free and the part a hundredfold can hide behind.
    f64 last_host_ms() const { return last_host_ms_; }

    // The thin-feature rescue off, as a control arm. Every other flag in this repository that turns
    // a rule off is one of these; see documentation/22-rewrite-handover.md §7.
    void set_rescue(bool on) { rescue_ = on; }

    // R12's first speed step off, as a control arm: `--no-field-accel`.
    //
    // ON is the shipped state whenever `--gpu-sample` is on, and off restores the walk exactly as
    // it stood before — a union's children asked in the order the author wrote them, and every one
    // of a difference's carves asked. One build, two arms, D407.
    //
    // It is a push-constant bit rather than a second upload, so both arms read the same buffers and
    // the same boxes and cannot differ by anything but the branch.
    void set_accelerate(bool on) { accelerate_ = on; }
    bool accelerating() const { return accelerate_; }

    // How many of the field's nodes carry a finite box, out of how many there are.
    //
    // The accelerator's whole reach, in one number, and it is not decoration: D675 counted 923 of
    // the estate's 18,250 nodes with NO box, an ancestor of an unbounded node cannot be bounded
    // either, and a cull can never touch any of them. A clip where this number falls is a clip
    // where the walk got longer for a reason nothing else reports.
    u32 bounded_nodes() const { return bounded_nodes_; }
    u32 sortable_unions() const { return sortable_unions_; }

    // Count the nodes each cell walks instead of building a world. An instrument: what a dispatch
    // costs is (nodes walked) x (what a step costs), and no clock can tell those two apart. See
    // `ws_field_visits` in shaders/field_types.glsl.
    void set_count_visits(bool on) { count_visits_ = on; }
    bool counting_visits() const { return count_visits_; }

    // THE DIVERGENCE INSTRUMENT, and the reason it is an environment variable rather than a flag.
    //
    // D727 named specialising the shader per clip as the lever, on the reasoning that every lane
    // pays for every other lane's opcode in a switch over sixty-seven ops. **That reasoning has a
    // hole**: taking ops out of the switch shrinks the instruction stream and does nothing about
    // divergence among the ops that remain. If a warp's lanes are spread over fifteen ops at a
    // turn, the warp runs fifteen branches whichever shader it is running.
    //
    // So this counts, at every turn of the walk, how many DISTINCT ops the active lanes of the warp
    // are standing on — with a subgroup ballot, on the ladder's own nodes, on a real clip. See the
    // block above `ws_distinct_ops` in shaders/field_types.glsl.
    //
    // `WS_GPU_DIVERGE=1` in the environment, and not a command-line flag, because the flag would
    // have to be parsed in `src/app/main.cpp` and this instrument is not the ladder's business:
    // the run it belongs to builds a world of nonsense and exists to answer one question about the
    // shader. **Pair it with `--refine-batch 1`.** The ballot loop makes a dispatch several times
    // dearer, the worst dispatch on the facility is already 510 ms of a two-second watchdog, and a
    // lost device in an instrument run is the same lost device as anywhere else.
    bool diverging() const { return diverge_; }

    // R12d — the pipeline this clip's op set was compiled for, and what it cost.
    //
    // **OFF unless `WS_GPU_SPECIALISE=1`, and the reason is in `create`.** It was built, gated and
    // measured at 1.00x on the clip this engine ships, against a control built with the change
    // absent from the tree. The two arms live in one build and read the same buffers and the same
    // boxes, so they cannot differ by anything but which pipeline was bound (D407) — and D683's
    // trap is why the arm that decided it was the tree without the change rather than the flag
    // turned off, because both flag arms are inside the diff.
    bool specialised() const { return active_ != VK_NULL_HANDLE; }
    u32 clip_ops() const { return clip_ops_; }
    // Milliseconds `vkCreateComputePipelines` took for the last specialised pipeline built, and how
    // many were built against how many times one was asked for. Opening the same clip twice must
    // compile once, and this is the number that says whether it did — 721 ms on the facility the
    // first time a machine sees that op set and 1 ms after, and `built` stays at 1 for `asked` 1
    // because `main.cpp`'s `field_card_tried_` uploads a field once per world.
    f64 last_compile_ms() const { return last_compile_ms_; }
    u32 pipelines_built() const { return pipelines_built_; }
    u32 pipelines_asked() const { return pipelines_asked_; }
    // Cells the card could not answer for at all. Nought is the only acceptable number: a refusal
    // is not a wrong voxel, it is a voxel nobody computed, and it must never read as air.
    u64 refused() const { return refused_; }
    // The op the first refusal was standing on, or 128 for depth. ~0 when nothing has refused.
    u32 refused_op() const { return refused_op_; }
    u64 visits() const { return visits_; }
    // Field evaluations a cell asked for, over the same cells `visits()` counted. The outer factor
    // of the cost model: (evaluations) x (visits an evaluation) x (what a step costs), and until
    // this existed the first of the three was a guess of "about two" nobody had counted. D722 is
    // exactly that fault on the CPU side, where both published factors turned out wrong.
    u64 evals() const { return evals_; }
    u64 visited_cells() const { return visited_cells_; }
    // Every cell the card was ever asked for, refusals included. The denominator `refused()` needs:
    // "nought refusals" is only a statement about anything if something says how many were asked,
    // and a run that asked for none reports nought either way. D676 is exactly that fault — the
    // mirror refused every point of the estate and reported nought sign changes over nought points,
    // which reads precisely like perfect agreement.
    u64 answered_cells() const { return answered_cells_; }

    void reload_if_changed() { pipeline_.reload_if_changed(); }

private:
    bool build_layout();
    void write_descriptors();
    bool upload_device_buffer(GpuBuffer& target, const void* data, u64 bytes, const char* name);
    // R12d — the pipeline for one op set, out of the cache or freshly compiled. VK_NULL_HANDLE
    // means "use the general one", which is what every failure here falls back to: a clip that
    // could not be specialised must still be sampled.
    VkPipeline pipeline_for(const std::vector<bool>& ops, bool measure);
    void drop_specialised();

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

    // The plan the field on the card came from, kept ONLY so that `--gpu-visits` can ask the CPU
    // the same question at the same points. Never read on a run that is building a world.
    //
    // It is a borrowed pointer and that is safe for exactly one reason: `upload` is handed
    // `Application::refine_plan_`, which outlives the sampler, and this is used only between a
    // `submit` and its `ready` on the same frame.
    const forge::SamplePlan* plan_ = nullptr;
    void mirror_the_walk(const GpuSampleBoxRecord& box);
    u64 mirror_cells_ = 0;
    u64 mirror_visits_ = 0;

    u32 node_count_ = 0;
    u32 rule_count_ = 0;
    u32 root_ = 0;
    u32 bounds_ = 0;
    u32 has_bounds_ = 0;
    u32 first_type_ = 0;
    u32 in_flight_ = 0;      // boxes in the batch the card is working on
    u32 delivered_ = 0;
    bool rescue_ = true;
    bool accelerate_ = true;
    bool count_visits_ = false;
    bool diverge_ = false;
    // The divergence run's totals. `div_ops_` and `div_active_` are sums over LANE-turns, so each
    // turn is weighted by how many lanes were in it — which is the weighting that prices a warp,
    // since a turn with two lanes left in it is not where the card spends anything. `div_warp_turns_`
    // is the unweighted count of warp-turns, taken as the maximum of each aligned run of 32 lanes'
    // turn counts: D727's mapping, and the same one `lane_slots_` above uses.
    f64 div_ops_ = 0.0;
    f64 div_nodes_ = 0.0;
    u64 div_turns_ = 0;
    u64 div_warp_turns_ = 0;
    u64 div_warps_ = 0;
    u64 div_hist_[7]{};
    f64 div_warp_ops_ = 0.0;
    u64 div_warp_lane_turns_ = 0;
    u32 div_warp_peak_ = 0;
    // How many of the sixty-seven ops this clip's field can reach, from every root the shader is
    // handed — the solid, the bounds shape and every paint rule's test.
    u32 clip_ops_ = 0;
    std::string clip_op_names_;

    // R12d — the specialised pipelines, one per distinct op set, keyed on the set itself.
    //
    // **Keyed on the SET and not on the clip**, which is what makes opening the same clip twice
    // compile once and also makes two clips with the same ops share a pipeline. The key is the
    // 67-bit set written out as characters, because that is a key nobody can get subtly wrong: a
    // hash would collide silently, and a silent collision here is a clip running another clip's
    // shader, which is a different building rather than an error.
    bool specialise_ = true;
    std::filesystem::path spirv_path_;
    std::unordered_map<std::string, VkPipeline> specialised_;
    VkPipeline active_ = VK_NULL_HANDLE;
    std::vector<bool> clip_ops_used_;
    // What the base pipeline's reload counter stood at when the cache was filled. A hot reload
    // recompiles the .spv, and a specialised pipeline built from the old one would then be the
    // shader the file no longer says — the fault trap 11 is about, one level along.
    u64 reload_seen_ = 0;
    f64 last_compile_ms_ = 0.0;
    u32 pipelines_built_ = 0;
    u32 pipelines_asked_ = 0;
    u32 bounded_nodes_ = 0;
    u32 sortable_unions_ = 0;
    u64 visits_ = 0;
    u64 evals_ = 0;
    u64 root_visits_ = 0;
    u64 lane_slots_ = 0;
    u32 warp_peak_ = 0;
    u64 refused_ = 0;
    u64 answered_cells_ = 0;
    u32 refused_op_ = 0xFFFFFFFFu;
    u64 visited_cells_ = 0;
    f64 last_gpu_ms_ = 0.0;
    f64 last_host_ms_ = 0.0;
    u64 submit_began_ns_ = 0;

    static f64 ns_to_ms_(u64 ns) { return static_cast<f64>(ns) * 1.0e-6; }
    u64 dispatches_ = 0;
    u64 cells_dispatched_ = 0;
    f64 gpu_ms_total_ = 0.0;
    f64 worst_gpu_ms_ = 0.0;
    u64 busy_ns_total_ = 0;
    u64 first_submit_ns_ = 0;
    u64 last_ready_ns_ = 0;

    static constexpr u32 kNodeCells = 512;      // forge::kNodeVoxels cubed
    static constexpr u64 kMaxNodes = 262144;    // the estate's field is 3,744; this is room to grow
    static constexpr u64 kMaxRules = 8192;
    static constexpr u64 kMaxPieces = 65536;
    static constexpr u64 kMaxParams = 4096;
};

}  // namespace ws
