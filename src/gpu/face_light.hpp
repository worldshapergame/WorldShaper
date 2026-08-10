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
// Nine words, and the layout is documented once beside the pass that writes it (kFaceLightWords in
// shaders/node.glsl). In outline: the ambient term's two sample counts, its near field and the two
// gradients under the voxel, its far field, then three floats of accumulated LAMP irradiance and a
// word holding the lamp sample count with the emitter-list version those samples were taken under.
//
// Counts rather than stored fractions throughout, for the reason D293 records: an eight-bit running
// mean cannot converge, because once it is within half a count of its target the update rounds back
// to where it was. The lamps go further and keep a float SUM rather than any kind of mean, because
// unlike the sun and the sky their answer is not a fraction in [0,1] — it is radiance over a solid
// angle divided by the density that chose it, and there is no bound to scale it against.

#include "gpu/buffer.hpp"
#include "gpu/device.hpp"
#include "world/face_store.hpp"

namespace ws {

class FaceLight {
public:
    // `slots` is every slot the face pass may write, which is the store's own capacity plus the
    // card's provisional tail -- not the watermark, which grows.
    //
    // `name` is what the allocator and the log call it, because there is now a second array with
    // exactly these properties -- a word a slot, device local, zeroed once, written and read by the
    // card and by nothing else. That is the face SEEN stamp (`face_seen` in shaders/node.glsl), and
    // sharing this class rather than copying it is the point: the guarantee being relied on is "the
    // host cannot write this", and there should be one piece of code that provides it.
    bool create(Device& device, u32 slots, const char* name = "face light");
    void destroy();

    VkBuffer buffer() const { return buffer_.buffer; }
    u64 bytes() const { return bytes_; }

private:
    Device* device_ = nullptr;
    GpuBuffer buffer_;
    u64 bytes_ = 0;
};

}  // namespace ws
