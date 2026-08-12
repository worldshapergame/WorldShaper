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

#include <algorithm>
#include <vector>

#include "core/block_pool.hpp"
#include "core/dirty_set.hpp"
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

// Thirty-two bits, over the coordinate truncated exactly as the record stores it, so the shader
// computes the identical bucket from the identical numbers. See hash_lattice32 for why this is
// not the 64-bit hash the rest of the engine uses.
//
// A false match needs two faces 2^32 apart at one level, and the probe compares the stored
// coordinate anyway.
struct FaceKeyHash {
    usize operator()(const FaceKey& k) const noexcept {
        return static_cast<usize>(hash_lattice32(static_cast<i32>(k.x), static_cast<i32>(k.y),
                                                 static_cast<i32>(k.z),
                                                 (k.level << 3) | k.face));
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

    // samples | lit << 16
    //
    // A COUNT of shadow rays that reached the sun, out of a count of rays cast. The visibility is
    // their ratio, worked out where it is read, and the penumbra resolves over the samples --
    // which is what moves shadowing off the pixel.
    //
    // It used to hold the ratio itself, as eight bits, updated as a running mean. That cannot
    // converge and the failure is silent: once the mean is within half a count of its target the
    // update rounds back to where it was and stays there for ever. Measured, on a face lit by
    // every one of 494 rays: 245 of 255, parked, with nothing in the picture to say why. A face
    // that always sees the sun must read as fully lit, and two counters give that exactly, at the
    // same eight bytes.
    //
    // Both halve when the sample count fills, which keeps the ratio exact while letting a face
    // notice that the light has moved. Variance -- for prioritising, and for how far a denoiser
    // reaches -- was sharing this word and now has to live elsewhere when R3c needs it.
    u32 counters = 0;

    // The directional payload, or kNoOffset. Null for a matte face, which is most of them.
    u32 bins = 0;
};
static_assert(sizeof(GpuFace) == 32, "GpuFace must stay two to a cache line");

inline constexpr u32 kFaceLeaf = 1u << 0;         // the face of a leaf rather than a coarse node
inline constexpr u32 kFaceTransmissive = 1u << 1; // light comes through it: glass, water

// Which class a face belongs to -- a light ray landed on it, or a pixel did -- lives in a HOST-ONLY
// array beside the records and emphatically not in a flag of `packed`. See `FaceStore::is_secondary`
// for what putting it in the record cost.

// Set on every face this store claims, and the reason it exists is a collision on zero.
//
// A slot was called empty when its packed word was nought. That worked for exactly as long as the
// finest face was a brick, because a level is at least three and `pack_face` puts the level in the
// bottom byte. The moment faces became single voxels, `pack_face(0, 0, 0)` -- level 0, direction
// +x, no flags -- IS nought, so one live face in six read as a free slot: never shaded, never
// settled, and the composite fell back to full sun on it for ever.
//
// A marker bit costs nothing and makes the question answerable rather than inferred.
inline constexpr u32 kFaceLive = 1u << 7;

// Bit 30 of `photons`: this face's ambient term has finished and is casting no more rays. Written
// only by shaders/shade_faces.comp, where kFaceAmbientDone records why it lives in a word the host
// never writes and the face pass has already read. The host reads it for one purpose -- the audit
// line that says how much of the store is still costing rays -- and must never set or clear it.
inline constexpr u32 kFaceAmbientDone = 1u << 30;

// Bit 29: nothing to do on a frame the sun is not due, which is the weaker of the two claims. See
// kFaceAmbientIdle in shaders/node.glsl for why both exist and what conflating them cost.
inline constexpr u32 kFaceAmbientIdle = 1u << 29;

// Bit 28: this face's LAMP term has finished and casts no more rays at the emitter list either.
// Written only by shaders/shade_faces.comp, read here only by the audit line that says how much of
// the store is still costing rays. Must match kFaceLampIdle in shaders/node.glsl.
inline constexpr u32 kFaceLampIdle = 1u << 28;
constexpr bool face_live(const GpuFace& f) { return (f.packed & (kFaceLive << 16)) != 0; }

constexpr u32 pack_face(u32 level, u32 face, u32 flags) {
    return (level & 0xFFu) | ((face & 0xFFu) << 8) | ((flags & 0xFFu) << 16);
}
constexpr u32 face_level(const GpuFace& f) { return f.packed & 0xFFu; }
constexpr u32 face_direction(const GpuFace& f) { return (f.packed >> 8) & 0xFFu; }
constexpr u32 face_flags(const GpuFace& f) { return (f.packed >> 16) & 0xFFu; }

// How many samples a face must have before its light is worth reading. Below this the composite
// uses its own lighting, which is full sun. Must match kFaceSettled in shaders/node.glsl.
//
// One, and it was four. Four meant three extra frames in which a newly claimed face was lit by the
// fallback, and the fallback is not a cautious answer — indoors it is the worst answer available.
// A single sample is a coin toss, so a face may read fully lit when the truth is 0.05; that is
// one face in twenty wrong for one frame, against every face wrong for four.
inline constexpr u32 kFaceSettled = 1;

// How many it needs before it is worth REFRESHING rather than finishing, which is the other
// question the number four was answering. A face below this is never held back by the shading
// stride, and the audit counts it as evidence only above it: a face with one sample says nothing
// about whether shadow rays work, which is what that log line exists to answer.
inline constexpr u32 kFaceEager = 4;

// Where the counts halve. A power of two so halving is exact, and low enough that a face notices
// the sun moving within a second or so at sixty frames.
inline constexpr u32 kFaceWindow = 256;

// How far above a face its STAND-IN sits: the coarse face the composite reads while the fine one
// is still being discovered. Must match kFaceAncestorStep in shaders/node.glsl, which is where the
// stand-in is read; it is claimed in src/app/main.cpp, from the fine request rather than from one
// of its own, because an ancestor key is a shift of its descendant's.
//
// Three levels is five hundred and twelve fine faces to one stand-in, and about sixty-four times
// the screen area — which is the point. A fine face waits for the one-in-sixty-four request lattice
// to land on its own pixel; a stand-in is claimed by the first of five hundred and twelve pixels to
// ask, so it cannot be missed by that lattice and is settled while its children are still being
// found. One level up would be four to one and would wait nearly as long as what it stands in for.
inline constexpr u32 kFaceAncestorStep = 3;

constexpr u32 face_samples(const GpuFace& f) { return f.counters & 0xFFFFu; }
constexpr u32 face_lit(const GpuFace& f) { return (f.counters >> 16) & 0xFFFFu; }
constexpr f32 face_visibility(const GpuFace& f) {
    const u32 samples = face_samples(f);
    return samples == 0 ? 1.0f : static_cast<f32>(face_lit(f)) / static_cast<f32>(samples);
}
constexpr u32 pack_counters(u32 samples, u32 lit) {
    return (samples & 0xFFFFu) | ((lit & 0xFFFFu) << 16);
}

// Where it starts is a measured trade and not a round number, and both directions cost something
// real. Starting at half free removes every refusal and costs the FACES PASS: 1.918 -> 8.086 ms
// flying at the default budget, because a shorter window also gives up faces whose coverage is under
// a pixel — which the lattice samples rarely however plainly they are on screen — and every one of
// them pays its whole ambient burst again when it comes back. Starting at an eighth keeps the store
// large, so the re-burst is small, and still stops the table reaching the point where a claim is
// refused. That is the point of the rule: a refusal is a surface with NO face, and there is no
// window short enough to make one of those come out right.
//
// The proper answer is not on this dial at all: `last_read_` is stamped by the lattice's CLAIM,
// while the card already stamps `face_seen` for every face every pixel resolves to, every frame.
// Residency wants that signal — the same correction D427 made for nodes and D430 for light rays —
// and with it the window could be short without giving up anything anybody is looking at. See the
// handover; that is the next change here, and this constant is the holding position until it lands.
inline constexpr u32 kFacePressureFrom = 8;   // shifts begin once free space is under 1/8

struct FaceStoreBudget {
    // Faces the table can hold. The premise this whole stage rests on was measured before any of
    // it was built (D205): distinct visible faces went 118,826 to 141,110 while pixels went up
    // 8.1x, and the worst case anywhere was 654k. A million is generous against that.
    u32 max_faces = 1u << 20;
    // Directional bins, for the faces that are not matte. R4 fills this; it is sized here so the
    // record's shape does not change when it does.
    u64 bin_bytes = 64ull * 1024 * 1024;
    // A face nothing has asked for in this many frames is given up, the same rule and the same
    // reason as the node pool's. It is the RELAXED window: what the store keeps while it has room
    // to keep it. See `claim_period` and `pressure` for what happens when it does not.
    u32 cold_frames = 600;

    // How many frames the request lattice takes to come back to a given pixel.
    //
    // This is the floor under everything else here, and getting it wrong is what filled the table.
    // `last_read_` is stamped by a CLAIM and by nothing else, and a claim comes from the one pixel
    // in stride^2 that the moving lattice is asking with this frame -- so "nobody has asked for this
    // face in N frames" says nothing at all until N is past stride^2. Below that, "cold" means "the
    // lattice has not got round to it", and evicting on it throws away the wall you are looking at.
    //
    // Set every frame from the render resolution, because the shader derives the stride from it
    // (visibility.comp: stride doubles until pixels/stride^2 is under sixty thousand). 64 at
    // 1280x800 and 1440p, 256 at 4K.
    u32 claim_period = 64;

    // Shorten the window as the table fills, instead of waiting for it to be full.
    //
    // False restores the old policy and is the control arm (`--no-face-pressure`). See
    // `FaceStore::cold_now`.
    bool pressure = true;

    // How little free space starts the squeeze, as a divisor: 8 means "under an eighth free".
    //
    // A run-time figure and not a constant, because it is a TRADE and a trade nobody can sweep at
    // run time is a trade somebody guesses at (D430's rule, applied to this one). See
    // kFacePressureFrom for what each end of it measured.
    u32 pressure_from = kFacePressureFrom;

    // The most of the table the OFF-SCREEN set may hold, as a divisor: 4 means a quarter.
    //
    // R9b, and it is the reason R9a can be landed at all. A face claimed by a light ray is claimed
    // because some other face needs to read it, and the set of those is bounded by nothing the
    // screen knows about -- §6's whole argument for why faces grow sub-linearly with resolution says
    // nothing about a set that includes surfaces no pixel is looking at. One shared budget lets that
    // set fill the table, and a full table is the state D502 measured: refused claims, no face for a
    // visible surface, and the card's provisional stand-in re-measuring every frame, which is what
    // blocky flickering light IS.
    //
    // A cap per class means a class that overruns degrades its own refresh rate and nothing else's.
    // Declining a secondary claim costs a bounce sample; refusing a primary one costs a picture.
    u32 secondary_share = 4;
};

// How many frames one full eviction sweep is spread over. Eight slices costs eight frames of
// latency on an eviction against a six-hundred-frame window, and saves seven eighths of the scan.
// The same figure and the same reasoning as the node pool's kErodeSlices (D273).
inline constexpr u32 kFaceEvictSlices = 8;

// Retired, and kept written down because the reasoning behind it is the fault.
//
// It was 32 — "far above the handful of frames a face needs to settle, so a store under pressure
// never recycles what it is still converging" — and every word of that is true and about the wrong
// quantity. What bounds an eviction window is not how long a face takes to converge. It is how long
// the request lattice takes to come back and SAY the face is still wanted, because `last_read_` is
// stamped by a claim and a claim comes from one pixel in stride². That is
// `FaceStoreBudget::claim_period`: 64 frames at 1440p, 256 at 4K — two to eight times this number.
//
// So the emergency sweep, halving its window down through 300, 150, 75, 37 to 18, was evicting
// faces the camera was pointed at, which came back the moment the lattice reached them again as
// empty records with no light in them. `FaceStore::min_cold` is `2 * claim_period` and nothing else.
inline constexpr u32 kFaceMinCold = 32;   // unused; see above before reaching for it again

// Where the store starts giving up history rather than waiting to be full.
//
// `cold_frames` is ten seconds. What a screen NEEDS is the faces it can see -- about half a million
// on this building -- and what ten seconds of walking about ACCUMULATES is everything seen along the
// way, which measured 995,684 against a table of 1,048,576 after ten seconds of flight. So the table
// fills with history while the visible set fits inside it several times over, and a full table is
// not a graceful state: a refused claim leaves a pixel with no face of its own and no host stand-in
// either, so it falls to the card's provisional coarse face -- which by construction re-claims
// itself and takes ONE fresh sample every frame. One ray per 8^3-voxel block per frame is a blocky
// picture that changes completely every frame, and that is exactly what was reported.
//
// Halving the window per doubling of pressure spends the history first, which is the thing worth
// spending: a face you looked away from keeps its answer for as long as there is room to keep it,
// and gives it up before a face anybody is looking at is refused. Never below `min_cold`.

// How rarely the full-table emergency sweep may run. It scans everything several times over, so a
// store whose faces are all genuinely hot must not pay it every frame.
inline constexpr u32 kFaceEmergencyGap = 60;

struct FaceStoreStats {
    u32 faces = 0;
    u64 face_bytes = 0;
    u64 bin_bytes_in_use = 0;
    u64 total_bytes = 0;
    u64 claims = 0;
    u64 hits = 0;
    u64 evictions = 0;
    // Claims the table had no room for, ever, and the window the pressure rule is currently
    // holding. Counted rather than deduced from `out_of_room()`, which is a state and answers
    // "is it full now" -- a store that fills and recovers sixty times a second reads as empty
    // from that flag at every moment anybody asks. This is the harm itself: a refused claim is a
    // surface that got no light of its own on this frame. Trap 16.
    u64 refusals = 0;
    u32 cold_window = 0;

    // The two classes, and what the cap turned away. A DECLINE is not a refusal and the two must not
    // be added together: a refusal is a surface with no light of its own, which is a picture fault; a
    // decline is one bounce sample that reads its coarse stand-in instead, which is the cap doing
    // exactly what it is for. Trap 7 -- "nothing here" and "I could not fit it" -- one level along.
    u32 secondary = 0;
    u32 secondary_cap = 0;
    u64 secondary_declined = 0;
    u64 promotions = 0;
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
    //
    // `was_new` says whether this call is what put the face here, which the caller needs to avoid
    // doing follow-on work for a face it has already done it for: the overwhelming majority of
    // claims are repeats -- the same surfaces reported by the same lattice frame after frame -- and
    // the stand-in claim hanging off this one is only worth anything the first time. Asking the
    // store instead of asking `find` first is the point: `claim` has already done that probe.
    // `secondary` says a LIGHT ray asked for this face rather than a pixel (R9a). Such a claim is
    // subject to `secondary_cap` and is DECLINED rather than refused when the cap is reached, which
    // is a different answer with a different consequence -- see FaceStoreStats::secondary_declined.
    // A secondary claim for a face that is already here never demotes it: the class only ever moves
    // towards primary, because a pixel wanting a face outranks a ray wanting it.
    u32 claim(const FaceKey& key, u64 frame, bool* was_new = nullptr, bool secondary = false);

    // A pixel read this face, so it belongs to the on-screen set whatever put it here.
    //
    // Called from the `kFeedbackFaceRead` report, which is the exact signal: it is sent by the
    // visibility pass for every face a pixel resolves to. Without it a face claimed by a bounce ray
    // and then walked up to would stay inside the off-screen cap for the rest of its life, and the
    // cap would be holding down faces the player is looking at.
    void promote(u32 slot);

    // Which class a slot is in. HOST-ONLY, in an array beside the records rather than in a flag of
    // `packed`, and the reason is D295 with a new door into it.
    //
    // `packed` is mirrored to the card, and `FaceBuffers::upload` sends whole records for the slots
    // the store marks dirty. The record has two owners -- the host owns the key and the flags, the
    // card owns `irradiance`, `photons` and `counters` -- so marking a LIVE face dirty sends the
    // host's zeroed counters over the light the card has accumulated into it. That is exactly the
    // fault D295 found and the reason uploads go by exact region rather than by coalesced range.
    //
    // Class was a flag of `packed` for one afternoon, and promotion therefore wiped the light of
    // every face a bounce ray had found and a pixel then walked up to: 29,882 of them in one flight,
    // each starting its whole ambient burst again, and nothing anywhere would have said so -- the
    // picture is right, the mirror agrees, and only the cost moves. An instrument's own bookkeeping
    // must not be written into a field the card is writing to.
    bool is_secondary(u32 slot) const {
        return slot < secondary_.size() && secondary_[slot] != 0;
    }

    u32 secondary_cap() const;

    // The slot for a face, or kNoFace. Never claims. This is what the composite does.
    u32 find(const FaceKey& key) const;

    void touch(u32 slot, u64 frame);

    // How long the request lattice takes to come back to a pixel, which is the floor under every
    // eviction decision. Set from the render resolution every frame, because it doubles with it.
    void set_claim_period(u32 frames) { budget_.claim_period = std::max(1u, frames); }

    // The window eviction is actually using this frame, after pressure and the floor. Exposed
    // because a policy nobody can read is a policy nobody can tell has gone wrong.
    u32 cold_now() const;
    u32 min_cold() const;
    // How hard the table is being squeezed: 0 with room to spare, one more per halving of the free
    // space past half. It sets both the window and how much of the table is looked at per frame.
    u32 pressure_shift() const;

    // Give up faces nobody has asked for. Must be called every frame: without it the store only
    // ever grows, reaches its cap, and then refuses every new face for the rest of the run.
    void evict_cold(u64 frame);

    // What a shading pass writes. One invocation per face per frame owns this (D191), so it is a
    // plain store rather than an atomic.
    void write(u32 slot, u32 irradiance, u32 samples, u32 lit);

    const std::vector<GpuFace>& faces() const { return faces_; }
    const std::vector<u32>& entries() const { return entries_; }
    u32 watermark() const { return next_free_; }
    bool out_of_room() const { return out_of_room_; }

    // What changed since the last upload, so the card is sent that rather than every used byte.
    // The same arrangement as the node pool's, and for the reason D235 measured there: copying
    // whole prefixes is free while a structure is quiet and ten megabytes a frame while it is not.
    const DirtySet& dirty_faces() const { return dirty_faces_; }
    const DirtySet& dirty_entries() const { return dirty_entries_; }
    bool nothing_dirty() const { return dirty_faces_.empty() && dirty_entries_.empty(); }
    void clear_dirty() {
        dirty_faces_.clear();
        dirty_entries_.clear();
    }
    // Mark clean exactly the span that was just sent, and nothing else.
    //
    // For an upload that ran out of staging part way through. Clearing the whole set there would
    // mark clean what the card was never sent, which is the stale byte this arrangement exists to
    // avoid; clearing NOTHING is what it used to do, and that restages the same prefix next frame,
    // runs out in the same place, and leaves the card holding records the store gave up long ago.
    // Clearing per staged run is the only one of the three that drains.
    void clear_dirty_faces(u64 first, u64 count) { dirty_faces_.clear_range(first, count); }
    void clear_dirty_entries(u64 first, u64 count) { dirty_entries_.clear_range(first, count); }

    FaceStoreStats stats() const;
    bool validate() const;

private:
    usize bucket_of(const FaceKey& key) const;
    void sweep(u32 now, u32 cold, u32 first, u32 last);

    FaceStoreBudget budget_;
    std::vector<GpuFace> faces_;
    std::vector<u32> entries_;        // open-addressed, slot per bucket, kNoFace when empty
    std::vector<u32> last_read_;      // CPU-side, parallel to faces_, as the node pool's is
    std::vector<u8> secondary_;       // CPU-side too, and see `is_secondary` for why it must be
    std::vector<u32> free_faces_;
    u32 next_free_ = 0;
    u32 evict_cursor_ = 0;     // where the rolling eviction sweep is
    u32 last_emergency_ = 0;   // when the full-table sweep last ran
    bool out_of_room_ = false;

    // Marked at every write into the two arrays above. A missed mark is a stale byte on the card
    // and a wrong picture, which is what the audit in gpu/face_buffers.cpp exists to name.
    DirtySet dirty_faces_;
    DirtySet dirty_entries_;

    u64 claims_ = 0;
    u64 hits_ = 0;
    u64 evictions_ = 0;
    u64 refusals_ = 0;
    u32 secondary_live_ = 0;
    u64 secondary_declined_ = 0;
    u64 promotions_ = 0;
};

}  // namespace ws
