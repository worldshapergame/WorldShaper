#pragma once
// The GPU side of the node pool: five device buffers and the staging that feeds them.
//
// `NodePool` decides what should be resident and writes it into CPU arrays that are already laid
// out exactly as the shader reads them (world/node_pool.hpp). This class does nothing but copy
// them, which is why almost all of the traversal logic is testable headless and only this thin
// layer needs a GPU.
//
// # Whole prefixes, not ranges, and why that is the right first version
//
// It copies the used prefix of each array whenever the tree changed, rather than tracking which
// slots moved. That is more traffic than necessary and it is deliberate for now:
//
//   the tree converges and then goes quiet. Once every node a view wants is built, nothing
//   changes and nothing is copied — the cost is a transient, not a per-frame charge;
//
//   the prefix is the high-water mark rather than the capacity, so an empty world copies nothing
//   and the facility copies a couple of megabytes rather than the pool's whole 32 MB;
//
//   and range tracking is an optimisation, which means it wants a measurement to justify it. The
//   old streaming path grew its range machinery before anything measured it and spent three
//   decisions (D128, D129, D130) discovering the budget was counting the wrong quantity.
//
// If R1d shows this on the frame graph, `NodePool` grows a dirty-range list and this becomes a
// scatter copy. Until then the simple thing is the honest thing.

#include <string>

#include "gpu/buffer.hpp"
#include "gpu/field_gpu.hpp"
#include "gpu/swapchain.hpp"
#include "world/node_pool.hpp"

namespace ws {

// R12c: what one dispatch of the marcher derived, read back a frame late.
//
// A ring of these, one per frame in flight, so the slot the host reads is the one the swapchain has
// already waited on. A single counter read while the card is writing it is a number that depends on
// scheduling, which is the fault the frame statistics block documents at length.
struct DeriveStats {
    u32 derived = 0;      // cells the marcher evaluated the field for
    u32 capped = 0;       // cells that wanted a derivation and got R2d's stand-in instead
    u32 evaluations = 0;  // field_eval calls, which is the quantity D687 prices
    // ...and the FIELD nodes those walked, in units of 1024. The second budget, and the one that
    // bounds a frame: a cap on cells is not a cap on work, because D688 measured one cell at
    // 1,073,935 nodes and 372 ms against a typical 5,195 and 1.8.
    u32 visits_k = 0;
};

struct NodeBufferStats {
    u64 uploaded_this_frame = 0;
    u64 total_uploaded = 0;
    u32 uploads = 0;
    u64 device_bytes = 0;
    // A frame whose dirty ranges did not fit in the staging ring. The ranges stay marked and go
    // next frame, so this costs latency rather than correctness -- but it is the difference
    // between "streaming is behind" and "streaming is broken", so it is counted rather than
    // inferred.
    bool staging_exhausted = false;
};

class NodeBuffers {
public:
    bool create(Device& device, const NodePoolBudget& budget);
    void destroy();

    // Copies whatever the pool holds, when it holds something new. Must be recorded inside a
    // command buffer before anything reads the buffers.
    // Takes the pool by reference rather than by const reference because a successful upload
    // clears what it sent. Nothing else about the pool is touched.
    //
    // The batch is no longer an argument. It describes what the pool decided to do; what has to
    // be copied is what actually changed, and only the pool's dirty sets know that.
    //
    // `frame_index` picks which region of the staging ring this frame writes through, and it is
    // not optional: the host writes those bytes while recording and the card reads them when it
    // reaches the copy, so a frame sharing a region with one still in flight overwrites its
    // source. See the note over the allocation in create().
    void upload(VkCommandBuffer cmd, NodePool& pool, u32 frame_index);

    // Decodes what the card actually holds and compares it against the pool, byte for byte.
    //
    // The successor to the mirror check the streaming audit has run since Stage 2, and it exists
    // for the reason that header gives: a renderer reading a structure nobody compares against
    // the world is a renderer debugging a mirage. The node pool shipped with the CPU half of that
    // - `NodePool::mirror_voxel`, asserted against the world - and without this half, and the
    // consequence was a picture difference that took an afternoon of diffing photographs to
    // narrow to two candidates when it should have been one failing assertion with an index on it.
    //
    // Stalls the device. It is for `--node-audit` and for tests, never for a frame anybody plays.
    bool audit(const NodePool& pool);

    VkBuffer entries() const { return entries_.buffer; }
    VkBuffer nodes() const { return nodes_.buffer; }
    VkBuffer leaves() const { return leaves_.buffer; }
    VkBuffer occupancy() const { return occupancy_.buffer; }
    VkBuffer payload() const { return payload_.buffer; }

    u32 entry_capacity() const { return entry_capacity_; }
    const NodeBufferStats& stats() const { return stats_; }

    // ---- R12c: the field, mirrored onto the marcher's own descriptor set -----------------------
    //
    // The same records `FieldSampler::upload` writes, in a second copy. A second copy rather than a
    // shared one because the two live on different descriptor sets and `FieldSampler` hands out no
    // buffer handles; the estate's field is 1,425 KB, which is a rounding error against the pool's
    // own 32 MB and is not worth an interface change on a file another hand is in.
    //
    // Returns false and stays unloaded when the field does not fit, which the marcher reads as
    // "derive nothing". A marcher that derived from the first n nodes of a field would be drawing a
    // different building with no error anywhere.
    bool upload_field(const forge::SamplePlan& plan, u32 bounds_node, bool has_bounds);
    bool field_loaded() const { return field_node_count_ > 0; }
    const std::string& field_why_not() const { return field_why_not_; }
    u32 field_node_count() const { return field_node_count_; }
    u32 field_rule_count() const { return field_rule_count_; }

    VkBuffer field_nodes() const { return field_nodes_.buffer; }
    VkBuffer field_parameters() const { return field_params_.buffer; }
    VkBuffer field_rules() const { return field_rules_.buffer; }
    VkBuffer field_pieces() const { return field_pieces_.buffer; }
    VkBuffer field_push() const { return field_push_.buffer; }
    VkBuffer derive_stats() const { return derive_stats_.buffer; }

    // Zero this frame's slot of the derivation counters, and take last time round's reading off it
    // first. Recorded before anything marches, unconditionally -- a counter that is only cleared on
    // the frames a feature is on reads as that feature's cost on the frame it was turned off.
    void begin_derive_frame(VkCommandBuffer cmd, u32 frame_index);
    const DeriveStats& last_derive() const { return last_derive_; }

private:
    // One staging copy per array, all through one ring -- which holds kFramesInFlight regions,
    // one per frame that may be in flight, because the host writes it at record time and the card
    // reads it at execute time. Sized once rather than grown on demand: a reallocation mid-play is
    // a hitch, and a hitch is the one thing streaming is never allowed to cause.
    bool stage(VkCommandBuffer cmd, const void* source, u64 bytes, GpuBuffer& destination);
    bool stage_at(VkCommandBuffer cmd, const void* source, u64 bytes, u64 destination_offset,
                  GpuBuffer& destination);
    // A blocking one-shot copy on its own command buffer, for the field. It happens once per clip
    // at load time and never inside a frame, so it is allowed to wait.
    bool upload_once(GpuBuffer& target, const void* data, u64 bytes, const char* what);

    Device* device_ = nullptr;
    GpuBuffer entries_;
    GpuBuffer nodes_;
    GpuBuffer leaves_;
    GpuBuffer occupancy_;
    GpuBuffer payload_;
    GpuBuffer staging_;

    u32 entry_capacity_ = 0;
    // What ONE frame in flight may stage. The buffer is kFramesInFlight of these end to end, so
    // `staging_.size` is the whole ring and this is a frame's share of it.
    u64 staging_capacity_ = 0;
    // Offset within the frame's region, never an absolute offset into the buffer.
    u64 staging_cursor_ = 0;
    // Where this frame's region starts. Set at the top of every upload from the frame index.
    u64 staging_frame_base_ = 0;
    NodeBufferStats stats_;

    // ---- R12c ---------------------------------------------------------------------------------
    GpuBuffer field_nodes_;
    GpuBuffer field_params_;
    GpuBuffer field_rules_;
    GpuBuffer field_pieces_;
    GpuBuffer field_push_;      // the uniform block field_types.glsl declares as a push constant
    GpuBuffer derive_stats_;    // host-visible; kFramesInFlight x DeriveStats
    VkCommandPool field_commands_ = VK_NULL_HANDLE;
    VkCommandBuffer field_cmd_ = VK_NULL_HANDLE;
    VkFence field_fence_ = VK_NULL_HANDLE;
    std::string field_why_not_;
    u32 field_node_count_ = 0;
    u32 field_rule_count_ = 0;
    DeriveStats last_derive_;

    // The estate's field is 18,250 nodes, 21 parameters, 628 rules and 18 zone pieces. These are a
    // few times that rather than `FieldSampler`'s own ceilings: this is a SECOND copy of the same
    // field and it is allocated on every run whether anything derives or not, so 5 MB of headroom
    // is the right trade where 21 MB is not. A clip that outgrows them derives nothing and says so.
    static constexpr u64 kMaxFieldNodes = 65536;
    static constexpr u64 kMaxFieldRules = 4096;
    static constexpr u64 kMaxFieldPieces = 16384;
    static constexpr u64 kMaxFieldParams = 4096;
};

}  // namespace ws
