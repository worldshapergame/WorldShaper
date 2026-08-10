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
    //
    // `seen` is the face SEEN buffer and `frame` the frame that has just been rendered, and they
    // are here because the number of live faces the frame is actually LIGHTING is the figure every
    // cost in this pass has to be read against -- the same argument as the ambient and lamp lines
    // below, which exist because a face that has finished and a face that is still paying look
    // identical in every picture. Pass VK_NULL_HANDLE to skip it.
    bool audit(const FaceStore& store, VkBuffer seen = VK_NULL_HANDLE, u32 frame = 0,
               u32 seen_window = 0);

    VkBuffer faces() const { return faces_.buffer; }
    VkBuffer entries() const { return entries_.buffer; }
    VkBuffer provisional() const { return provisional_.buffer; }
    u32 entry_capacity() const { return entry_capacity_; }

    // Where the card's own faces start, and how many there are. See create() for why they live in
    // the tail of the same buffer the store mirrors into.
    u32 provisional_base() const { return provisional_base_; }
    static constexpr u32 provisional_count() { return kProvisionalFaces; }

    const FaceBufferStats& stats() const { return stats_; }

private:
    bool stage_at(VkCommandBuffer cmd, const void* source, u64 bytes, u64 destination_offset,
                  GpuBuffer& destination);
    void stage_regions(VkCommandBuffer cmd, const FaceStore& store);

    // How many faces the card may claim for itself, and it is a small number on purpose.
    //
    // These are STAND-INS, not the fine faces: one of them covers five hundred and twelve voxel
    // faces (kFaceAncestorStep), so a screen holds a few thousand rather than the 477,622 faces the
    // close camera claims per voxel. Sized well above that so a claim never fails on a reveal,
    // which is the one frame it exists for; a power of two, so the bucket index is a mask.
    static constexpr u32 kProvisionalFaces = 1u << 15;   // 32,768 — 1 MB of records

    Device* device_ = nullptr;
    GpuBuffer faces_;
    GpuBuffer entries_;
    GpuBuffer provisional_;
    u32 provisional_base_ = 0;
    GpuBuffer staging_;
    u64 staging_capacity_ = 0;
    u64 staging_cursor_ = 0;
    u32 entry_capacity_ = 0;
    // Kept between frames so a few hundred regions a frame is not a few hundred allocations.
    std::vector<VkBufferCopy> regions_;
    FaceBufferStats stats_;
};

}  // namespace ws
