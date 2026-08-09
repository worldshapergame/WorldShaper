#pragma once
// The face store, on the card.
//
// Shaped exactly like gpu/node_buffers.*, deliberately: two device buffers mirroring two CPU
// arrays, a staging ring, dirty ranges rather than whole prefixes, and an audit that reads the
// real buffers back and names the first byte that disagrees.
//
// That audit is not belt and braces. On the node pool it caught three separate stale-byte bugs in
// one sitting -- an entry table whose empty value is all ones against a device buffer that starts
// at zero, a `refine` that MOVES a record and changes both ends, and a release that zeroes on the
// way to the free list -- and each was named to the byte rather than deduced from a photograph.
// A face store written by one invocation per face has exactly the same failure mode.

#include <vector>

#include "gpu/buffer.hpp"
#include "gpu/swapchain.hpp"
#include "world/face_store.hpp"

namespace ws {

struct FaceBufferStats {
    u64 uploaded_this_frame = 0;
    u64 total_uploaded = 0;
    u32 uploads = 0;
    u64 device_bytes = 0;
    bool staging_exhausted = false;
};

class FaceBuffers {
public:
    bool create(Device& device, const FaceStoreBudget& budget);
    void destroy();

    // Takes the store by reference rather than by const reference because a successful upload
    // clears what it sent. Nothing else about it is touched.
    void upload(VkCommandBuffer cmd, FaceStore& store);

    // Decodes what the card holds and compares it against the store, byte for byte.
    bool audit(const FaceStore& store);

    VkBuffer faces() const { return faces_.buffer; }
    VkBuffer entries() const { return entries_.buffer; }
    u32 entry_capacity() const { return entry_capacity_; }
    const FaceBufferStats& stats() const { return stats_; }

private:
    bool stage_at(VkCommandBuffer cmd, const void* source, u64 bytes, u64 destination_offset,
                  GpuBuffer& destination);
    void stage_regions(VkCommandBuffer cmd, const FaceStore& store);

    Device* device_ = nullptr;
    GpuBuffer faces_;
    GpuBuffer entries_;
    GpuBuffer staging_;
    u64 staging_capacity_ = 0;
    u64 staging_cursor_ = 0;
    u32 entry_capacity_ = 0;
    // Kept between frames so a few hundred regions a frame is not a few hundred allocations.
    std::vector<VkBufferCopy> regions_;
    FaceBufferStats stats_;
};

}  // namespace ws
