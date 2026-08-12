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
    // How many frames ran out of staging before they had sent everything the store had changed.
    //
    // A state rather than an event was all this had, and a state answers "is it happening now" --
    // which for something that happens on a burst of frames while the camera moves reads as false
    // at every moment anybody asks. The same distinction `FaceStoreStats::refusals` draws against
    // `out_of_room()`, and the same trap (16). It still matters after D544: an exhausted frame now
    // clears what it managed to send, so the backlog drains a staging region a frame instead of
    // never, but the card is still behind the store until it has drained, and the face pass shades
    // what the CARD holds. Read it against `the card is N records ahead of the store`.
    u32 exhausted_frames = 0;
};

class FaceBuffers {
public:
    bool create(Device& device, const FaceStoreBudget& budget);
    void destroy();

    // Takes the store by reference rather than by const reference because a successful upload
    // clears what it sent. Nothing else about it is touched.
    //
    // `frame_index` picks this frame's region of the staging ring, and it is not optional: the
    // host writes those bytes while recording and the card reads them when it reaches the copy.
    // See the note over the allocation in create().
    void upload(VkCommandBuffer cmd, FaceStore& store, u32 frame_index);

    // The control arm for D544: go back to clearing the dirty sets only when the WHOLE set fitted,
    // which is what this did before. Two flags of one build, as D407 requires. --whole-set-retry.
    void set_whole_set_retry(bool on) { whole_set_retry_ = on; }

    // Decodes what the card holds and compares it against the store, byte for byte.
    //
    // `seen` is the face SEEN buffer and `frame` the frame that has just been rendered, and they
    // are here because the number of live faces the frame is actually LIGHTING is the figure every
    // cost in this pass has to be read against -- the same argument as the ambient and lamp lines
    // below, which exist because a face that has finished and a face that is still paying look
    // identical in every picture. Pass VK_NULL_HANDLE to skip it.
    // `probe` is the light probe (kLightProbeWords in shaders/node.glsl): the gathering ray's own
    // counters for the frame that has just been drawn. It is read in the FIRST submit here, not the
    // second, because the second is gated on the mirror having matched and on the store fitting in
    // the staging ring -- and a counter about what rays found has nothing to do with either. D529.
    bool audit(const FaceStore& store, VkBuffer seen = VK_NULL_HANDLE, u32 frame = 0,
               u32 seen_window = 0, VkBuffer probe = VK_NULL_HANDLE);

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
    // By reference, not const reference: a run is marked clean the moment it is staged.
    void stage_regions(VkCommandBuffer cmd, FaceStore& store);

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
    // What ONE frame in flight may stage. `staging_.size` is the whole ring, which holds
    // kFramesInFlight of these end to end.
    u64 staging_capacity_ = 0;
    // Offset within the frame's region, never an absolute offset into the buffer.
    u64 staging_cursor_ = 0;
    // Where this frame's region starts. Set at the top of every upload from the frame index.
    u64 staging_frame_base_ = 0;
    u32 entry_capacity_ = 0;
    // Kept between frames so a few hundred regions a frame is not a few hundred allocations.
    std::vector<VkBufferCopy> regions_;
    bool whole_set_retry_ = false;
    FaceBufferStats stats_;
};

}  // namespace ws
