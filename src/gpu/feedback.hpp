#pragma once
// The renderer's feedback buffer.
//
// documentation/03-voxel-data-model.md §8: streaming is demand-driven from a feedback
// buffer — the renderer reports what it wanted and could not find, the streamer serves
// it, LRU evicts the rest. Memory is then bounded by what is on screen rather than by
// world size, which is the mechanism that makes an infinite world fit on a handheld.
//
// The alternative, and what this replaces, is requesting a radius around the camera. That
// is the wrong rule for a renderer with continuous detail: it decides residency from
// where you *are* rather than from what you can *see*, so a chunk 300 m away is never
// streamed however large it looks on screen.
//
// Results come back two frames late. That is fine — a chunk that arrives two frames after
// it was first wanted is drawn at a coarser level in the meantime, which is exactly what
// the detail hierarchy is for.

#include <vector>

#include "gpu/buffer.hpp"
#include "gpu/render_params.hpp"
#include "gpu/swapchain.hpp"

namespace ws {

// ---- R11e: what threw the ray, carried on the request itself ---------------------------------
//
// A ray reports what it USED and not only what it missed (D427), and this is the other half of the
// same idea: it reports WHO IT WAS. The rule R11e adds needs that and cannot be built without it —
// **a node request that originates from a light path may not cause a sample job** — because by the
// time a request reaches the thing that would sample it, a coordinate and a level are all that is
// left of where it came from.
//
// The rule matters now and did not before. Until R11d nothing a light ray asked for could cause a
// sample, because everything was sampled up front; after it (D673, on by default) the world holds
// only what a camera has justified, so the obvious next line to write anywhere near `stream()` is
// *"the pool cannot build this because the world has nothing here — go and sample it"*. That line
// would make a dark room sample the whole building: a gathering ray leaves a wall, is stopped by a
// cell nobody has built, asks for it, and the field answers. The load R11d removed comes back
// through the back door, in the one scene that has no pixels to justify any of it.
//
// The classification is by the TAG the shader wrote, which is the only witness that cannot
// disagree with which pass cast the ray:
//
//   kFeedbackExact                     R9i — the one cell that STOPPED a shadow, ambient or lamp
//                                      ray. `occlude_unknown` is true only in shade_faces.comp,
//                                      so this bit is a light path by construction.
//   kFeedbackFace | kFeedbackSecondary R9a — the one face a gathering ray LANDED on.
//   everything else                    a pixel: a primary ray's miss, its used report, its face.
//
// Both of those are D292's two deliberate exceptions (D589 restates the rule to what was built),
// and they are exceptions about **claiming a face** and **naming a cell**. Neither may become a
// sample job, and neither stops working here: the claim and the residency request are untouched:
// only the sampling is refused.
enum class RayClass : u8 {
    kPixel = 0,
    kLight = 1,
};

// Which class the entry's `level` field says threw it. Takes the raw field rather than the entry
// so the shader's own bits are the whole of the argument and a caller cannot pass half of it.
RayClass feedback_ray_class(i32 level_field);

// Who is asking for a node to be sampled, at the point the job is made.
//
// The ladder's own pick is `kCamera`: a node large on screen, ranked, tested for occlusion and
// split to the level its own footprint asks for. `kEdit` is R11h — a chisel about to cut a surface
// that may never have been sampled, which must sample before it cuts or it carves a four-voxel
// approximation into the world for ever (R11f makes carved matter authoritative). `kLight` is the
// class this whole file's rule exists to refuse.
enum class SampleCause : u8 {
    kCamera = 0,
    kEdit = 1,
    kLight = 2,
    kCount = 3,
};

const char* sample_cause_name(SampleCause cause);

// The one door a sample job is made through, so the rule is a refusal in one place rather than an
// absence in several.
//
// COUNTED RATHER THAN REASONED ABOUT, which is the whole of R11e's gate. "A light path cannot
// cause a sample because nothing wires it to the sampler" is a property of today's call graph and
// is worth nothing the first time somebody wires one — and a counter that is nought because the
// question is never asked looks exactly like a counter that is nought because the answer is no. So
// the light classes are OFFERED here, on every request they make, and refused: the offers are the
// evidence that the gate ran, and the causes are the evidence that it held.
class SampleGate {
public:
    // `--no-light-sampling-guard` sets this. It is the arm that shows the rule has teeth rather
    // than a restatement of today: with light allowed through, `clips/sealed_dark.clip` samples the
    // building it is sealed inside.
    void allow_light(bool allowed) { light_allowed_ = allowed; }
    bool light_allowed() const { return light_allowed_; }

    // Asked at the point the job is made. False means no sample job, ever, for this request.
    bool may_sample(SampleCause cause) {
        const u8 at = static_cast<u8>(cause);
        if (at >= static_cast<u8>(SampleCause::kCount)) return false;
        ++offered_[at];
        if (cause == SampleCause::kLight && !light_allowed_) {
            ++refused_;
            return false;
        }
        ++caused_[at];
        return true;
    }

    u64 offered(SampleCause cause) const { return offered_[static_cast<u8>(cause)]; }
    u64 caused(SampleCause cause) const { return caused_[static_cast<u8>(cause)]; }
    u64 refused() const { return refused_; }

private:
    bool light_allowed_ = false;
    u64 offered_[static_cast<u8>(SampleCause::kCount)]{};
    u64 caused_[static_cast<u8>(SampleCause::kCount)]{};
    u64 refused_ = 0;
};

class FeedbackBuffer {
public:
    bool create(Device& device);
    void destroy();

    VkBuffer buffer() const { return device_buffer_.buffer; }

    // Zeroes the counter. Must be recorded before anything writes feedback.
    void begin_frame(VkCommandBuffer cmd);

    // Copies this frame's report into the readback slot for `frame_index`.
    void end_frame(VkCommandBuffer cmd, u32 frame_index);

    // Reads the report written into this slot the last time it was used, which the
    // swapchain has already waited on. Returns an empty span on the first frames.
    const std::vector<FeedbackEntry>& read(u32 frame_index);

    u32 last_reported() const { return last_reported_; }
    u32 last_truncated() const { return last_truncated_; }

private:
    Device* device_ = nullptr;
    GpuBuffer device_buffer_;
    GpuBuffer readback_;
    std::vector<FeedbackEntry> entries_;
    u32 last_reported_ = 0;
    u32 last_truncated_ = 0;

    static constexpr u64 kHeaderBytes = 16;   // counter plus padding to 16
    static constexpr u64 kSlotBytes =
        kHeaderBytes + static_cast<u64>(kFeedbackCapacity) * sizeof(FeedbackEntry);
};

}  // namespace ws
