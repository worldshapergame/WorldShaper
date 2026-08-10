#pragma once
// Light the card works out and the card alone reads. One entry per face slot.
//
// # Why this is not in GpuFace
//
// `GpuFace` has two owners. The CPU owns which face a slot is -- key, level, direction, flags --
// and the card owns what arrives there, and D295 records what that cost: the uploader coalesced
// dirty runs across clean records and sent the CPU's zeroed bytes over light the card had written,
// so every upload wiped the sun off the faces around each new one. The fix there was to send exact
// regions. The fix here is not to put the byte in a shared record at all.
//
// Ambient occlusion is written by the face pass, read by the composite, and never looked at by the
// host for any purpose. So it lives in a device-local array that is allocated with the store and
// then never uploaded, never mirrored, never audited and never in a dirty range. There is no code
// path by which the host can overwrite it, which is a stronger guarantee than remembering not to.
//
// This also starts paying down R3d's standing debt -- split `GpuFace` so the CPU's half and the
// card's half are never in one copy -- rather than adding to it. The sun's counters move here when
// R3d lands.
//
// # What an entry holds
//
// One word, packed exactly as `GpuFace::counters` is: rays cast in the low sixteen bits, rays that
// escaped in the high sixteen. Same packing means `face_accumulate` and `face_visibility_of` in
// shaders/node.glsl serve both without a second pair of functions to drift apart, and the reason
// it is two counts rather than a stored fraction is D293: an eight-bit running mean cannot
// converge, because once it is within half a count of its target the update rounds back.

#include "gpu/buffer.hpp"
#include "gpu/device.hpp"
#include "world/face_store.hpp"

namespace ws {

class FaceLight {
public:
    // `slots` is every slot the face pass may write, which is the store's own capacity plus the
    // card's provisional tail -- not the watermark, which grows.
    bool create(Device& device, u32 slots);
    void destroy();

    VkBuffer buffer() const { return buffer_.buffer; }
    u64 bytes() const { return bytes_; }

private:
    Device* device_ = nullptr;
    GpuBuffer buffer_;
    u64 bytes_ = 0;
};

}  // namespace ws
