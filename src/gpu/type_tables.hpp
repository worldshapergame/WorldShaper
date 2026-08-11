#pragma once
// The two interned tables a voxel is turned into a colour with, on the card.
//
// This is what is left of `gpu/world_buffers.*` after R1e. That class held the chunk renderer's
// whole world: brick headers, brick occupancy, brick payload, chunk records, chunk brick masks,
// popcount prefixes, the wrapped chunk grid, five coarse occupancy grids and two thumbnail
// buffers, streamed every frame out of `world/residency.*` — about 650 MB of device memory and
// twelve milliseconds of CPU a frame, feeding a descriptor set no shader declared any more. The
// node pool holds the geometry now (`gpu/node_buffers.*`), and what it does NOT hold is the type
// and visual tables, because those are interned per world rather than per node.
//
// So: two device buffers and the staging ring that feeds them. They only ever grow, which is why
// the upload is a tail copy and why there is no residency policy here at all.

#include <vector>

#include "gpu/buffer.hpp"
#include "gpu/swapchain.hpp"
#include "world/voxel_type.hpp"

namespace ws {

class TypeTables {
public:
    bool create(Device& device, u64 staging_bytes);
    void destroy();

    // Voxel type and visual tables. They only ever grow, so this copies the new tail when
    // the counts change and does nothing otherwise. Must run inside the same command
    // buffer, before anything shades a voxel.
    //
    // $frame_index picks which slice of the staging ring to write, so filling next frame's
    // copy cannot overwrite bytes the GPU is still reading from this frame's.
    void upload_tables(VkCommandBuffer cmd, const VoxelTypeTable& types, u32 frame_index);

    VkBuffer types() const { return types_.buffer; }
    VkBuffer visuals() const { return visuals_.buffer; }

    // What went to the card this frame, for the profiler's byte counter.
    u64 staged_bytes() const { return staged_bytes_; }
    u64 device_bytes() const { return types_.size + visuals_.size; }

private:
    Device* device_ = nullptr;
    GpuBuffer types_;
    GpuBuffer visuals_;
    GpuBuffer staging_;

    u32 uploaded_types_ = 0;
    u32 uploaded_visuals_ = 0;
    // How many voxel types and visual records the GPU will hold.
    //
    // A quarter of a million was ample while a world used a palette of a few dozen materials.
    // It stopped being ample the moment clips started giving every voxel its own version of its
    // material — a surface with no two square centimetres alike is what makes a voxel wall stop
    // reading as a voxel wall, and it costs one record per voxel to do properly. A single
    // weathered block of a million voxels asked for six hundred thousand records and took the
    // renderer down with an assertion.
    //
    // Two million, then: 32 MB of visuals and 16 MB of types, and enough for a two-million-voxel
    // clip to be entirely unique. Past that the variation pass reuses records rather than making
    // new ones, so the limit is a quality ceiling and never a crash.
    static constexpr u32 kMaxTables = 2097152;

    u64 staging_capacity_per_frame_ = 0;
    u64 staged_bytes_ = 0;

    // Appends `size` bytes from `source` to this frame's slice of the staging ring and records
    // the copy. False when it does not fit, in which case NOTHING is uploaded and the counts are
    // left alone, so the same tail is offered again next frame.
    bool stage(const void* source, u64 size, u64 base, std::vector<VkBufferCopy>& into);
};

}  // namespace ws
