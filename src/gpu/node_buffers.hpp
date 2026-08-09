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

#include "gpu/buffer.hpp"
#include "gpu/swapchain.hpp"
#include "world/node_pool.hpp"

namespace ws {

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
    void upload(VkCommandBuffer cmd, NodePool& pool);

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

private:
    // One staging copy per array, all through one ring. Sized for the largest single upload the
    // pool can produce rather than grown on demand: a reallocation mid-play is a hitch, and a
    // hitch is the one thing streaming is never allowed to cause.
    bool stage(VkCommandBuffer cmd, const void* source, u64 bytes, GpuBuffer& destination);
    bool stage_at(VkCommandBuffer cmd, const void* source, u64 bytes, u64 destination_offset,
                  GpuBuffer& destination);

    Device* device_ = nullptr;
    GpuBuffer entries_;
    GpuBuffer nodes_;
    GpuBuffer leaves_;
    GpuBuffer occupancy_;
    GpuBuffer payload_;
    GpuBuffer staging_;

    u32 entry_capacity_ = 0;
    u64 staging_capacity_ = 0;
    u64 staging_cursor_ = 0;
    NodeBufferStats stats_;
};

}  // namespace ws
