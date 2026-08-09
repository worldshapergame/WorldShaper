#pragma once
// The face store: one entry per voxel face, and the place light lives.
//
// # Why this exists
//
// Light is currently worked out per screen pixel. That is why the tracer costs more the larger
// the window is, and why it is slightly *super*-linear in pixels rather than merely linear: in an
// enclosed room every refining pixel pays for a secondary ray that never escapes to sky. The cost
// is per pixel times how enclosed you are, and both terms are unbounded (documentation/21 §6).
//
// A face does not care how many pixels are looking at it. Work light out once per face, let the
// pixels read the answer, and the lighting cost stops being a function of resolution at all.
//
// # What replaces what
//
// The tracer's existing cache holds up to FOUR entries for one face — irradiance at the pixel's
// level, six ancestor levels above it, a shadow entry at level 0, a shadow parent at level 6, and
// up to ninety-six radiance bins — all as 32-byte records in one table that is measurably
// refusing slots. It is a table fighting itself.
//
// One face is one entry here, with a variable payload from a size-classed pool, which is exactly
// the arrangement gpu_brick.hpp already uses for brick payloads and for the same reason: most
// faces are cheap and a few are not. A matte stone wall — which is most of a world — allocates no
// payload at all.
//
// # The key is the one the marcher already has
//
// A face is (the node the ray stopped on, which of six directions it was hit from). The marcher
// descends to that node anyway and knows its slot, so nothing is recomputed and the two passes
// cannot disagree about which face they are talking about — which is the fault D133 and D147
// describe, arriving through a different door.
//
// # Written by exactly one invocation per face
//
// D191. Everything the current tracer does to survive a thousand pixels writing one face in the
// same dispatch — the halving compare-and-swap, the read-the-sum-twice-and-take-the-minimum, the
// empty-then-publish key ordering, the eight-probe coldest-slot eviction — is a consequence of
// the wrong thing owning the write, not a set of bugs to be fixed. One writer removes all of it.

#include <vector>

#include "core/block_pool.hpp"
#include "core/hash.hpp"
#include "core/types.hpp"

namespace ws {

inline constexpr u32 kNoFace = 0xFFFFFFFFu;

// A bucket whose face was evicted, as against one that was never used.
//
// The difference is the whole of why an open-addressed table can be deleted from safely. Emptying
// a bucket in the middle of a probe run cuts every face behind it out of its own sequence: they
// are still in the table and can no longer be found, so the pass that would refresh them never
// sees them and nothing anywhere says so. A tombstone ends nothing and is reusable on the way
// past, so a run stays whole and the slot comes back.
inline constexpr u32 kFaceTombstone = 0xFFFFFFFEu;

// Which of a cube's six faces. The order matches node_face_coverage in shaders/node.glsl and the
// visibility buffer's face field, so a direction never has to be translated between them.
inline constexpr u32 kFaceCount = 6;

// A face, identified the way the marcher already identifies it.
struct FaceKey {
    i64 x = 0;
    i64 y = 0;
    i64 z = 0;
    u32 level = 0;
    u32 face = 0;   // 0..5, +x -x +y -y +z -z
    bool operator==(const FaceKey& other) const {
        return x == other.x && y == other.y && z == other.z && level == other.level &&
               face == other.face;
    }
};

struct FaceKeyHash {
    usize operator()(const FaceKey& k) const noexcept {
        return static_cast<usize>(hash_cell(k.x, k.y, k.z, (k.level << 3) | k.face, 0x46414345ull));
    }
};

// What the GPU sees. Thirty-two bytes, one cache line per two faces, exactly as GpuNode is.
//
// Declared as scalars rather than vectors on purpose: std430 aligns a uvec3 to sixteen bytes, so
// declaring the coordinate the obvious way silently reads every entry after the first from the
// wrong offset. That trap has already cost this project two debugging sessions in two different
// files, so it is written down at every place it could recur.
struct GpuFace {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
    u32 packed = 0;        // level | face << 8 | flags << 16

    u32 irradiance = 0;    // rgb9e5: what arrives over the hemisphere. All a diffuse face needs.
    u32 photons = 0;       // rgb9e5: caustic energy, deposited by the photon pass

    // samples | visibility << 16 | variance << 24
    //
    // Visibility is the sun as a fraction with its penumbra resolved over the samples, which is
    // what moves shadowing off the pixel. Variance is for prioritising and for how far the
    // denoiser reaches, so eight bits of it is plenty and it shares this word rather than
    // widening the record past thirty-two bytes.
    u32 counters = 0;

    // The directional payload, or kNoOffset. Null for a matte face, which is most of them.
    u32 bins = 0;
};
static_assert(sizeof(GpuFace) == 32, "GpuFace must stay two to a cache line");

inline constexpr u32 kFaceLeaf = 1u << 0;         // the face of a leaf rather than a coarse node
inline constexpr u32 kFaceTransmissive = 1u << 1; // light comes through it: glass, water

constexpr u32 pack_face(u32 level, u32 face, u32 flags) {
    return (level & 0xFFu) | ((face & 0xFFu) << 8) | ((flags & 0xFFu) << 16);
}
constexpr u32 face_level(const GpuFace& f) { return f.packed & 0xFFu; }
constexpr u32 face_direction(const GpuFace& f) { return (f.packed >> 8) & 0xFFu; }
constexpr u32 face_flags(const GpuFace& f) { return (f.packed >> 16) & 0xFFu; }

constexpr u32 face_samples(const GpuFace& f) { return f.counters & 0xFFFFu; }
constexpr u32 face_visibility(const GpuFace& f) { return (f.counters >> 16) & 0xFFu; }
constexpr u32 face_variance(const GpuFace& f) { return (f.counters >> 24) & 0xFFu; }
constexpr u32 pack_counters(u32 samples, u32 visibility, u32 variance) {
    return (samples & 0xFFFFu) | ((visibility & 0xFFu) << 16) | ((variance & 0xFFu) << 24);
}

struct FaceStoreBudget {
    // Faces the table can hold. The premise this whole stage rests on was measured before any of
    // it was built (D205): distinct visible faces went 118,826 to 141,110 while pixels went up
    // 8.1x, and the worst case anywhere was 654k. A million is generous against that.
    u32 max_faces = 1u << 20;
    // Directional bins, for the faces that are not matte. R4 fills this; it is sized here so the
    // record's shape does not change when it does.
    u64 bin_bytes = 64ull * 1024 * 1024;
    // A face nothing has asked for in this many frames is given up, the same rule and the same
    // reason as the node pool's.
    u32 cold_frames = 600;
};

struct FaceStoreStats {
    u32 faces = 0;
    u64 face_bytes = 0;
    u64 bin_bytes_in_use = 0;
    u64 total_bytes = 0;
    u64 claims = 0;
    u64 hits = 0;
    u64 evictions = 0;
};

// The store itself.
//
// Built and tested headless before the renderer touches it, which is how residency was built in
// Stage 2 and the node pool in R1a, and for the reason R1a's four bugs demonstrated: a structure
// the renderer walks and nobody compares against a known answer is a renderer debugging a mirage.
class FaceStore {
public:
    void create(const FaceStoreBudget& budget);

    // The slot for a face, claiming one if it is not there. kNoFace only when the table is full,
    // which is a fact about this store and never about the world -- the same distinction
    // `out_of_room_` draws in the node pool, and for the same reason.
    u32 claim(const FaceKey& key, u64 frame);

    // The slot for a face, or kNoFace. Never claims. This is what the composite does.
    u32 find(const FaceKey& key) const;

    void touch(u32 slot, u64 frame);
    void evict_cold(u64 frame);

    // What a shading pass writes. One invocation per face per frame owns this (D191), so it is a
    // plain store rather than an atomic.
    void write(u32 slot, u32 irradiance, u32 visibility, u32 samples, u32 variance);

    const std::vector<GpuFace>& faces() const { return faces_; }
    const std::vector<u32>& entries() const { return entries_; }
    u32 watermark() const { return next_free_; }
    bool out_of_room() const { return out_of_room_; }

    FaceStoreStats stats() const;
    bool validate() const;

private:
    usize bucket_of(const FaceKey& key) const;

    FaceStoreBudget budget_;
    std::vector<GpuFace> faces_;
    std::vector<u32> entries_;        // open-addressed, slot per bucket, kNoFace when empty
    std::vector<u32> last_read_;      // CPU-side, parallel to faces_, as the node pool's is
    std::vector<u32> free_faces_;
    u32 next_free_ = 0;
    bool out_of_room_ = false;

    u64 claims_ = 0;
    u64 hits_ = 0;
    u64 evictions_ = 0;
};

}  // namespace ws
