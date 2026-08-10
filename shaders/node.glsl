// Walking the node pool: one sparse octree, at every scale, with one answer.
//
// This replaces the four addressing schemes shaders/world.glsl walks end to end Ã¢â‚¬â€ a wrapped chunk
// grid, a per-chunk brick mask with popcount prefixes, a per-chunk brick-mask pyramid, and a
// separate summary tier with its own grids. See src/world/node_pool.hpp for why they went.
//
// # The shape of a step
//
// One hash to enter the tree, and pure arithmetic below it: a node holds the base index of its
// eight contiguous children, so descending is `children + octant`. The old walk paid two
// *dependent* loads per chunk entered Ã¢â‚¬â€ the wrapped grid cell, then that record's own coordinate,
// because the grid wraps and a cell may be held by a chunk from somewhere else Ã¢â‚¬â€ and did it up to
// thirty-two times along each axis a ray crossed, plus a coarse-grid fetch per skip. A dependent
// load is the one thing a marcher cannot hide, and on a machine whose binding constraint is memory
// bandwidth that difference is the point of the exercise.
//
// # Three answers, not two
//
// A descent stops for one of three reasons and they drive three different behaviours:
//
//   HERE      a node covers this point at the level asked for. Draw it.
//   EMPTY     the child cell one level down holds nothing. Jump the width of it Ã¢â‚¬â€ the coarse skip
//             falls out of the descent instead of needing five occupancy grids to carry it.
//   WANTED    the world has something here and the pool does not. Report it; that is the only
//             reason anything is ever streamed.
//
// Conflating EMPTY and WANTED is the fault this structure exists to make unrepresentable. An
// unstreamed building that reads as open sky is never reported, so it is never streamed, so it
// stays open sky Ã¢â‚¬â€ which is decision D133 and again D147, found twice by looking at photographs.
//
// # The stack, and why it costs nothing to keep
//
// A ray moves a little at a time, so nearly every ancestor of the point it was at is still an
// ancestor of the point it moved to. The walk therefore keeps the slots it descended through and
// re-uses whatever survives. Which levels survive needs no memory at all: the node at level L
// containing p is p >> L by definition, so two points share every ancestor above the highest bit
// in which they differ. One XOR and one findMSB, and the descent restarts from there.

const uint kNoNode = 0xFFFFFFFFu;
const int kLeafLevel = 3;       // a brick: 8 voxels
const int kEntryLevel = 14;     // 16,384 voxels Ã¢â‚¬â€ 512 m. Must match src/world/node_pool.hpp.

const uint kNodeLeaf = 1u;
const uint kNodeUniform = 2u;

// Thirty-two bytes, declared as scalars rather than vectors on purpose. std430 aligns a uvec3 to
// sixteen bytes, so declaring the coordinate the obvious way silently reads every node after the
// first from the wrong offset Ã¢â‚¬â€ the same trap the light list documents in pathtrace.comp.
struct Node {
    int x;
    int y;
    int z;
    uint packed;        // level | flags << 8 | child_mask << 16
    uint children;      // interior: base slot of eight children. leaf: a leaf id.
    uint colour;        // rgba8; alpha is coverage and never rounds to nothing
    uint coverage_xy;   // +x -x +y -y, one byte each
    uint coverage_z;    // +z -z
};

// The brick layout the renderer has used since Stage 2, unchanged. Bricks work; chunks did not.
struct LeafHeader {
    uint payload_offset;
    uint palette_count;
    uint packed;          // index_bits | flags << 8
    uint uniform_type;
    uvec2 mip_cell2;      // 4x4x4 occupancy, 64 bits
    uint mip_cell4;       // 2x2x2 occupancy, low 8 bits
    uint average_colour;
};

layout(std430, binding = 2) readonly buffer NodeEntries { uint slots[]; } node_entries;
layout(std430, binding = 3) readonly buffer Nodes { Node items[]; } nodes;
layout(std430, binding = 4) readonly buffer Leaves { LeafHeader items[]; } leaves;
layout(std430, binding = 5) readonly buffer Occupancy { uint words[]; } occupancy;
layout(std430, binding = 6) readonly buffer Payload { uint words[]; } payload;

struct FeedbackEntry {
    ivec4 coord;   // xyz the node coordinate at its own level, w the level
};

layout(std430, binding = 7) buffer Feedback {
    uint count;
    uint pad0;
    uint pad1;
    uint pad2;
    FeedbackEntry entries[];
} feedback;

layout(std140, binding = 8) uniform Params {
#include "params.glsl"
} push;

// How many buckets the entry table has, and how far a probe may run. A power of two, so the
// modulo is a mask.
// One push-constant block for every pipeline that includes this file, because a stage is allowed
// exactly one and the face shader needs the marcher.
//
// The visibility pass writes only the first two fields and ignores the rest; the face shader
// writes all of them. Both pipelines declare the same range so neither reads past what its layout
// allows -- a shader declaring more than its pipeline layout reserves is a validation error, and
// one declaring less than the other pushes is a silent misread of the fields after the gap, which
// is exactly the std140 fault D168 records in params.glsl.
layout(push_constant) uniform NodeConstants {
    uint entry_capacity;
    uint entry_probes;
    uint face_count;    // face shader: how many face slots are in use
    uint frame;
    vec4 sun;           // face shader: xyz towards the sun
    uint face_capacity; // buckets in the face table, a power of two
    uint face_probes;
    uint face_stride;   // face shader: shade one face in this many each frame, round robin
    uint provisional_base;  // where the card's own faces start in the same array. 0 disables them
    uint face_first;    // face shader: the first slot this dispatch owns
    // The lamps. See "the emitter list" below for why a version and a reset flag are both here.
    uint light_count;   // how many entries of `lights` are live. 0 means the scene has no lamps
    uint light_version; // bumped whenever the list's CONTENTS change, not merely its length
    uint light_reset;   // 1 on the one frame the list changed, which is what reopens an idle face
    uint from_worklist; // shading: take the slot from face_work.slots rather than from the index
    uint seen_window;   // how many frames a face keeps being lit after the last pixel that read it
    uint prolong;       // 1: a subdivided face inherits its parent's fit. 0: the old stand-in seed
    // 1: a ray reports the bricks it passes THROUGH as read, not only the one it stops on.
    // A run-time lever rather than a build, because D407 and D420 are both about what happens
    // when the two arms of an A/B are two binaries. Off restores the behaviour D426 measured.
    uint report_crossings;
    // How often a light ray may stamp a node it reads, in frames, as a power of two. 0 is off.
    // A period rather than a switch because it is the whole trade in this rule -- see node_light_due.
    uint light_read_period;
    // How many samples a face keeps when the host announces that the world under it changed.
    // 0 is the old wipe and is the control arm for that change. See kFaceEditSeed.
    uint edit_seed;
    // 1: an edit reopens a face's LAMP term only where it can stand between that face and a
    // fitting. 0 reopens the whole sixteen-metre box, which is the control arm and is what made a
    // room full of lamps flicker for as long as somebody was building in it.
    uint lamp_edit_scope;
} node_push;

uint node_level_of(uint packed) { return packed & 0xFFu; }
uint node_flags_of(uint packed) { return (packed >> 8) & 0xFFu; }
uint node_child_mask_of(uint packed) { return (packed >> 16) & 0xFFu; }

// Must match node_hash_mix / entry_hash32 in src/world/node_pool.hpp exactly. Mixed one axis at a
// time rather than by XORing three products together, because voxel coordinates are a dense
// lattice and XOR-of-products over a lattice leaves whole planes in the same few buckets.
uint node_hash_mix(uint x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// Must match hash_lattice32 in src/core/hash.hpp. Shared by the node entry table and the face
// table, so there is one mixing function on this side as there is one on the other.
uint node_hash_lattice(ivec3 coord, uint tag) {
    uint h = 0x811C9DC5u;
    h = node_hash_mix(h ^ uint(coord.x));
    h = node_hash_mix(h ^ uint(coord.y));
    h = node_hash_mix(h ^ uint(coord.z));
    return node_hash_mix(h ^ tag);
}

uint node_entry_hash(ivec3 coord, uint level) {
    uint h = 0x811C9DC5u;
    h = node_hash_mix(h ^ uint(coord.x));
    h = node_hash_mix(h ^ uint(coord.y));
    h = node_hash_mix(h ^ uint(coord.z));
    return node_hash_mix(h ^ level);
}

// The root covering this point, or kNoNode. An empty bucket ends the run, exactly as it does on
// the CPU Ã¢â‚¬â€ a probe that walked past one would find a root that a later insertion had displaced.
uint node_entry_lookup(ivec3 block) {
    uint bucket = node_entry_hash(block, uint(kEntryLevel)) & (node_push.entry_capacity - 1u);
    for (uint probe = 0u; probe < node_push.entry_probes; ++probe) {
        uint slot = node_entries.slots[(bucket + probe) & (node_push.entry_capacity - 1u)];
        if (slot == kNoNode) return kNoNode;
        Node n = nodes.items[slot];
        if (node_level_of(n.packed) == uint(kEntryLevel) && n.x == block.x && n.y == block.y &&
            n.z == block.z) {
            return slot;
        }
    }
    return kNoNode;
}

// ---- the face store ------------------------------------------------------------------------
//
// Declared here rather than in the pass that shades it, because three passes need it now: the
// shading pass writes light into it, the visibility pass resolves each pixel to a slot, and the
// composite reads that slot. One declaration means the three cannot disagree about the layout,
// which is the same reason the traversal itself lives in this file.

struct Face {
    int x;
    int y;
    int z;
    uint packed;      // level | face << 8 | flags << 16
    uint irradiance;
    uint photons;
    uint counters;    // samples | visibility << 16 | variance << 24
    uint bins;
};

layout(std430, binding = 9) buffer Faces { Face items[]; } faces;
layout(std430, binding = 10) readonly buffer FaceEntries { uint slots[]; } face_entries;

// The card's own faces: one word a bucket, and the slot is the bucket. See node_face_claim below,
// and src/gpu/face_buffers.hpp for why this table is separate from the one above.
layout(std430, binding = 12) buffer FaceProvisional { uint marks[]; } provisional;

// Light the card owns outright: one word a face slot, packed exactly as GpuFace::counters is, so
// face_accumulate and face_visibility_of serve both. The host never writes it and there is no path
// by which it could -- see src/gpu/face_light.hpp for why that is a stronger guarantee than
// remembering not to.
layout(std430, binding = 13) buffer FaceLight { uint words[]; } face_light;

// ---- what the camera can actually see ---------------------------------------------------------
//
// One word a face slot: the frame a pixel last READ this face. The card's outright, like the face
// light beside it, for the same reason -- there is no host path that could overwrite it.
//
// # Why the light pass needs this at all
//
// "If you cannot see it, it is not processed and does not exist" is the rule the whole rewrite was
// asked for under, and the light pass was the one place still ignoring it. The store keeps a face
// for `cold_frames` after anything last asked for it, which is six hundred frames, and every one of
// those faces was being shaded every frame it owed a ray. Measured on the flight: 763,800 live
// faces, of which the frame in front of you reads a fraction -- and 281,244 of them still bursting
// sixteen ambient rays, eight lamp rays and a sun ray apiece. That is most of a moving frame spent
// on surfaces nobody is looking at.
//
// Residency is NOT what to fix. A face has to stay in the store while it is off screen, or the
// composite has nothing to read when it comes back and the whole R9d stand-in argument starts
// again. What has to stop is the RAYS. So the store keeps its six hundred frames and the light
// stops at this window, and the two questions are asked separately because they are separate.
//
// # Why a stamp rather than the store's own idea of recency
//
// The host knows which faces the request lattice reported, which is one pixel in sixteen to
// sixty-four with a phase that takes stride^2 frames to come round -- so its idea of "seen" is
// stale by up to sixty-four frames and would have to be believed for at least that long. The
// visibility pass, on the other hand, looks a face up for EVERY pixel, so it knows exactly, this
// frame, with no lattice and no round trip. It is already holding the record it would stamp.
layout(std430, binding = 15) buffer FaceSeen { uint frames[]; } face_seen;

// When a node was last REPORTED as read, one word a slot, card-owned exactly as face_seen is.
//
// This is a deduplicator and nothing else. Without it, a light ray keeping the geometry it reads
// (D429) is one feedback entry per RAY, and thousands of rays are stopped by the same brick: at 4K
// while flying and chiselling that measured 1,538,219 reports against a 131,072 capacity, of which
// 1,407,147 were dropped -- and what gets dropped is not only the new stamps but the miss reports
// that stream geometry, so the cure was worse than the fault by a wide margin. With it, a node
// produces at most one entry per `light_read_period` frames however many rays hit it, so the volume
// is a function of how much DISTINCT world the light is touching rather than of how many rays it
// takes to touch it. D430.
//
// The host never writes it, which is what makes it safe to be stale: a slot reused by a different
// node keeps an old stamp and is merely reported a few frames late, and the host stamps a node it
// builds anyway. Two invocations racing both report, which costs one duplicate entry.
layout(std430, binding = 17) buffer NodeSeen { uint frames[]; } node_seen;

// How many frames a face goes on being lit after the last pixel that read it. The DEFAULT; the
// live value is `node_push.seen_window`, so `--face-gate` can widen it to the whole run and give
// an exact same-commit control for this change (D407: two builds of this pass cannot be compared
// from adjacent batches, so the arms have to be one build).
//
// It can be small because the stamp is exact and arrives in the same frame: the visibility pass
// writes it, a barrier follows, and the shading pass reads it. Not one, because a face at a
// grazing angle can drop out of the frame for a frame at a time as the camera moves, and a face
// that stops and restarts its burst converges no faster for the pause.
const uint kFaceSeenWindow = 4u;

// Nine words a face, holding three different quantities that must not be averaged into one.
//
//   [0] the two SAMPLE COUNTS: near-field rays in the low sixteen bits, far-field rays in the high
//       sixteen. They used to be one count because one ray answered both questions. They are not
//       one count any more, and that is the whole of what made the ambient term converge: see
//       "why the near field and the far field stopped sharing a ray" below.
//   [1] the NEAR field -- contact. A ray's FIRST HIT DISTANCE through a falloff over about a metre,
//       summed in fixed point at 255 a ray, over count [0].lo.
//   [2] the near field's GRADIENT along the face's first axis, and [3] along its second.
//   [4] the FAR field -- how many of count [0].hi reached open sky. It multiplies sky radiance, and
//       it is the physically missing term in the ambient integral.
//   [5][6][7] the LAMPS -- the running SUM of the irradiance estimator, one float a channel. Not a
//       running mean and not a fixed-point count: D293 is the standing measurement of what a
//       narrow running mean does to a quantity that has to converge, and unlike the sun and the
//       sky a lamp's contribution is not a fraction in [0,1] -- it is radiance over a solid angle
//       divided by a density, and it has no bound this record could scale itself against.
//   [8] the lamp SAMPLE COUNT in the low sixteen bits, and in the high sixteen the LIGHT-LIST
//       VERSION those samples were taken under. See kLampConverged.
//
// Indoors the far field saturates -- every ray hits something, so sky visibility is nought on every
// surface in the room and carries no shape at all. Measured: 1,619 of 1,671 surface pixels in the
// enclosed view fell in the lowest tenth. The near field is what varies there, and it is what reads
// as ambient occlusion.
//
// # Why the near field and the far field stopped sharing a ray
//
// They are the same integral over the same hemisphere, so one ray answering both looks like the
// obviously right economy, and it was â€” until the question became how fast the term CONVERGES.
// The two halves need wildly different rays:
//
// - the near field is a falloff over `kContactMetres`. Every answer past a metre is the same
//   answer, so its ray can stop at 32 voxels and cost a fraction of a march;
// - the far field asks whether a direction reaches the sky, which is a question about the other
//   end of the world and cannot be bounded at all.
//
// Sharing one ray priced the near field at the far field's cost, and the near field is the term
// that carries every crease in the building. Split, a face can afford `kSkyBurst` near rays in the
// visit where it used to afford one, because a bounded ray is most of an order of magnitude cheaper
// than an unbounded one â€” and the far field keeps exactly the one unbounded ray a frame it always
// had, so nothing about it changed or regressed.
//
// # Those gradients
//
// They are what make occlusion vary UNDER a voxel. Kept as first moments of the same samples rather
// than fitted afterwards: the sample position is uniform over the sampled span, so the Legendre
// basis on [-1, 1] is already orthogonal under it -- <P_i P_j> = d_ij/(2i+1) -- and every
// coefficient is therefore an independent running mean of the sample weighted by a polynomial. No
// normal equations, no least squares, no second pass, and no extra rays.
//
// A plane fits to a constant, a corner to a gradient. Nothing in the code has to know which it is
// looking at, because what is being fitted is the real visibility field.
// Nine words a slot, and NOT padded to sixteen.
//
// Padding was the obvious thing to try at five -- a 20-byte stride is unaligned to everything,
// cannot be loaded as a vector, and puts a slot's words across a cache line three times in four,
// which is the reason `GpuFace` is thirty-two bytes ("two to a cache line", face_store.hpp). It was
// tried and measured and it bought nothing: 2.185 ms against 2.185 on the facade camera and 1.256
// against 1.053 once converged, for 34 MB against 21. This record is touched a handful of times per
// face per frame, not per step, so its stride is not where this pass spends anything. The same
// argument holds at nine, and padding to sixteen would cost 26 MB to buy the same nothing.
const uint kFaceLightWords = 9u;
// How far away geometry stops darkening, in METRES rather than voxels, so a coarse face at 200 m
// and a level-0 face at arm's length darken over the same physical distance and a dolly-in shows
// no transition.
const float kContactMetres = 1.0;
const float kVoxelsPerMetreF = 32.0;
// The same distance in voxels, which is what a near ray is bounded by. Nothing beyond it changes
// `contact`, so nothing beyond it is worth a step.
const float kContactVoxels = kContactMetres * kVoxelsPerMetreF;

// How far outside its own bounds an edit is announced. Must match kEditShadowReach in
// src/app/main.cpp, and it is sized for the SUN: a shadow cast by what you just placed can land
// sixteen metres away, so every face within sixteen metres has to look again.
const int kEditShadowReach = 512;
// How far an edit can move the NEAR field, which is a different and much smaller number: the near
// field is a falloff over kContactMetres and is blind to anything past it. Two metres -- the
// falloff plus as much again -- is generous.
//
// This is the difference between placing one voxel costing a handful of faces their ambient history
// and costing every face within sixteen metres of it. The near field is the expensive half to
// re-converge (kSkyBurst rays a face) and it is the half that is genuinely local, so the two
// reaches are kept apart rather than the larger one being used for both. The FAR field keeps the
// full reach, because opening a hole in a roof really does change how much sky a floor sixteen
// metres away can see -- and the far field costs one ray a visit to put right.
const int kEditAmbientReach = 64;
const int kEditAmbientShrink = kEditShadowReach - kEditAmbientReach;

// This face's ambient term is finished, in bit 30 of `photons` -- which is a GPU-owned word the
// face pass already has in hand, and that is the entire reason it lives there rather than in the
// face light beside the counts it describes.
//
// A converged face must cost NOTHING, and "nothing" has to include the load that finds out. The
// face light is one word a slot in a 21 MB array, so asking it "are you done" is a scattered
// four-byte read per face per frame across half a million faces -- 64 MB of cache lines a frame
// fetched to learn that there is no work. Measured: the pass sat at 2.13 ms against the 1.75 ms of
// the build it replaces, with every visible face converged and casting not one ray. `Face` is
// thirty-two bytes and consecutive invocations read consecutive slots, so the same answer carried
// there is coalesced and free.
//
// The host zeroes `photons` when it claims a slot, which is exactly right: a re-claimed slot is a
// different face and owes the burst again. And if a record is ever re-uploaded over the card's, the
// flag clears, the pass looks again, finds the counts already at the target, and sets it back --
// the failure mode is one wasted lookup rather than a face that never converges.
const uint kFaceAmbientDone = 1u << 30;

// And bit 29: this face has nothing to do on a frame the SUN is not due, which is a weaker claim
// and a different one. The far field takes its one unbounded ray on the sun's schedule, so a face
// can be idle five frames in six while still owing rays on the sixth.
//
// Two bits rather than one because the first version of this used one for both meanings, and an
// audit line built on it reported the store as finished while the pass was plainly still paying for
// something. Trap 7: "nothing to do this frame" and "nothing to do ever" are not one answer, and a
// counter that cannot tell them apart sends you looking in the wrong place -- which it did.
const uint kFaceAmbientIdle = 1u << 29;

// And bit 28: this face's LAMP term has finished and is casting no more rays at it either.
//
// The same argument as kFaceAmbientDone, and the same word for the same reason: a converged face
// must cost nothing, and "nothing" has to include the load that finds out. It sits beside the
// ambient flags rather than in the face light because `Face` is already in hand and coalesced,
// while the face light is a scattered read into a 39 MB array.
//
// Bit 28 is the last free bit of `photons` -- 0-23 are the sun ray's step count, 24-27 the level a
// shadow ray was stopped at, 29-31 the two ambient flags and the ignorance flag. Anything else that
// wants a bit here has to widen the record rather than borrow one.
const uint kFaceLampIdle = 1u << 28;

const uint kNoFace = 0xFFFFFFFFu;
const uint kFaceTombstone = 0xFFFFFFFEu;

// Say that a pixel read this face on this frame. See `face_seen` above for why this exists.
//
// Conditional, and that is the whole of what makes it affordable: a face covers many pixels, so
// after the first one the word already says this frame and the rest are cache hits with no write
// and no dirty line. The record it belongs to has already been fetched by `node_face_lookup` to
// compare the key against, so there is no traffic here the lookup was not paying anyway.
//
// Two pixels racing to write the same value is not a race worth ordering.
void node_face_seen(uint slot) {
    if (slot == kNoFace || slot == kFaceTombstone) return;
    if (slot >= face_seen.frames.length()) return;
    if (face_seen.frames[slot] != node_push.frame) face_seen.frames[slot] = node_push.frame;
}

// Was this face read by a pixel recently enough to be worth a ray?
//
// Unsigned arithmetic on purpose: a slot that has never been stamped reads nought, and
// `frame - 0` is the frame number itself, which is past the window from the fifth frame of the run.
bool node_face_recently_seen(uint slot) {
    if (slot >= face_seen.frames.length()) return true;   // cannot tell, so light it
    return (node_push.frame - face_seen.frames[slot]) <= node_push.seen_window;
}

uint face_level_of(uint packed) { return packed & 0xFFu; }
uint face_dir_of(uint packed) { return (packed >> 8) & 0xFFu; }
// Must match kFaceLive in src/world/face_store.hpp: flags bit 7, so the flags byte at bits 16-23.
const uint kFaceLive = 1u << 7;
uint face_flags_of(uint packed) { return (packed >> 16) & 0xFFu; }
bool face_live_of(uint packed) { return (packed & (1u << 23)) != 0u; }
// Two counts in one word: rays cast, and rays that reached the sun. Must match pack_counters in
// src/world/face_store.hpp. The fraction is worked out where it is read, because a fraction stored
// in eight bits cannot be updated without eventually rounding back to itself.
uint face_samples_of(uint counters) { return counters & 0xFFFFu; }
uint face_lit_of(uint counters) { return (counters >> 16) & 0xFFFFu; }
// Samples before the light is worth reading, and it is ONE.
//
// It was four, and four is three frames of the composite lighting a surface with full sun -- which
// indoors is not a cautious answer, it is the worst answer available. The reasoning for four was
// that a single sample is binary, so a face reads as fully lit or fully shadowed with nothing in
// between; that is true and it is the wrong comparison. One sample is NOISY and unbiased: in a room
// where the true answer is 0.05, nineteen faces in twenty read black immediately and the twentieth
// is wrong for a frame. Full sun is every face wrong, for four frames, in the same direction.
//
// Bias is what a player sees as "the shadows have not arrived yet"; variance is what they see as a
// speckle that is gone before it registers, and it is what R5's denoise exists to take. The face
// keeps accumulating to kFaceWindow either way -- this gates only when the answer may be READ.
const uint kFaceSettled = 1u;
// And how long a face is exempt from the shading stride, which is a different question with a
// different answer: a face that may be read at one sample is still converging at four, so it keeps
// its ray every frame until it has enough of them to be worth refreshing rather than finishing.
const uint kFaceEager = 4u;
const uint kFaceWindow = 256u;   // where both counts halve, so the sun may move

// The same figure for the ambient term, and it is eight times larger for a reason that does not
// apply to the sun: **ambient occlusion depends only on geometry**. The sun moves across the sky and
// a face has to be able to forget where it used to be, which is what a 256-sample window buys.
// Geometry moves only when somebody edits it, and an edit tells the faces inside it to start again
// from nothing rather than leaving them to average their way back (cebf015) -- so this is not a
// forgetting mechanism at all. It is where the answer is good enough to stop.
//
// It can therefore be as large as the counters allow, and size is variance: the error of a fraction
// over N samples falls as one over the square root of N, and the target IS N. At 256 a contact
// fraction near a half has a standard deviation of about 8 of 255, which is exactly the face-to-face
// roughness measured on a flat wall -- the old cap was the noise floor. At 2,048 it is under 3.
//
// The counters hold it: the two sample counts are sixteen bits each and 2,048 is well inside them,
// and the contact sum and its two gradients are full words at 255 a sample, which is 522,240 at the
// target against four thousand million.
const uint kSkyWindow = 2048u;

// ---- how fast the ambient term converges, and why it costs nothing to converge it fast ---------
//
// The ambient term used to take one ray a face a frame, and a settled face is visited one frame in
// `face_stride`, so 2,048 samples was six thousand frames â€” a hundred seconds of a wall visibly
// sharpening, at the enclosed camera, on every load. Three things were true about that and each is
// a lever:
//
// 1. **The ray was priced as a sun ray and is not one.** Bounded at `kContactVoxels`, the near ray
//    stops after a metre because nothing past a metre changes its answer.
// 2. **The face went on paying for ever.** Ambient occlusion is a function of GEOMETRY, and
//    geometry does not move unless somebody edits it â€” at which point the host says so on the exact
//    frame, in `edit_min.w`, and the record starts again from nothing. So a converged face needs no
//    further rays AT ALL, and every one it was casting was re-measuring a constant. That is the
//    budget the burst is spent out of, and it is why this is not a trade.
// 3. **The far field was holding the near field's rate down.** Split, above.
//
// So: a face that has not converged spends its visit on `kSkyBurst` bounded rays instead of one
// unbounded one; a face that has converged spends nothing. Total rays over the life of a face go
// DOWN â€” 2,048 and then silence, against 2,048 and then one every stride for the rest of the run â€”
// while the samples that decide what the player sees all land in the first few frames.
//
// The burst is the sampling rate, not a quality knob: `kSkyWindow` still decides where the answer
// lands, and 32 was measured against 8 and 64 on the grid rather than chosen.
const uint kSkyBurst = 16u;
// Where the near field stops casting rays: the count the window was chosen for, reached and then
// held rather than rolled over. There is no halving any more and there is nothing left for one to
// do -- an edit announces itself and resets the record outright, a re-claimed slot is zeroed, and
// the only other thing that can move the answer is the pool building or shedding what the rays
// crossed, which the ignorance hold in shade_faces.comp is for. A window that never fires is a
// branch that can only be wrong.
const uint kSkyConverged = kSkyWindow;
// And where the FAR field stops if it never makes up its mind, which is four times sooner and is
// not a compromise.
//
// Sky visibility does not reach the picture raw: the composite mixes it over [kIndirectFloor, 1],
// which halves it, and then multiplies it by the near field. So its standard error arrives at a
// quarter strength. At 256 samples a fraction near a half has an error of 0.031, which lands at
// 0.0085 of the ambient term against a renderer whose own run-to-run noise is 0.0098 -- under the
// floor, and going further under it costs UNBOUNDED rays quadratically. 2,048 would be sixteen
// times the rays for a difference nothing can see.
const uint kSkyFarConverged = 256u;

// ...and most faces stop long before it, because **a fraction's error depends on the fraction**,
// and that is what makes the expensive half of this term affordable.
//
// Sky visibility is a binomial proportion, so after N rays its variance is p(1-p)/N. A face in a
// sealed room measures p = 0 and a roof under open sky measures p = 1, and BOTH have an error of
// nought after a handful of rays -- every further unbounded ray they cast re-derives a number
// already in hand. Only faces that genuinely straddle an opening (a window reveal, the lip of a
// soffit, the edge of the terrace) sit near a half, and those are exactly the faces where sky
// visibility is worth spending rays on.
//
// So the far field stops when its own measured error falls under `kSkyFarEps`, floored at
// `kSkyFarMin` samples so the estimate of p is not itself noise, and capped at kSkyFarConverged so
// a face at exactly a half still terminates. Nothing here knows whether it is looking at a room or
// a roof, which is the same property the near field's fit has.
//
// Measured on the facade camera, where this was first got wrong: a plain unanimity test -- all
// thirty-two rays agreeing -- stopped 8,752 faces of 498,179, because an exterior face is rarely
// unanimous and is very often 0.9 and settled. The pass sat at 2.15 ms against the 0.62 it costs
// with the far field silent.
const uint kSkyFarMin = 32u;
// The error the far field is measured to, squared, so the test is a multiply rather than a divide.
//
// 0.02 of a fraction that is mixed over [kIndirectFloor, 1] and then multiplied by the near field
// arrives as 0.0055 of the ambient term, against a renderer whose own run-to-run noise is 0.0098.
// It is under the floor, and being further under it costs unbounded rays quadratically.
const float kSkyFarEpsSq = 0.0004;
// The far samples that are worth taking off the sun's schedule and having at once. `open_sky`
// starts at one -- fully open, which indoors is the worst answer available -- and it is the first
// few dozen rays that move it off that. It is the same number as kSkyFarMin on purpose: a face
// whose answer is unanimous reaches it in thirty-two frames and then never casts an unbounded ray
// again for the rest of its life, which is the whole of why the burst above is affordable.
const uint kSkyFarEager = kSkyFarMin;
// How many samples a face inherits from the coarse face standing over it when it is first shaded.
//
// A face with no samples reads as fully unoccluded, which indoors is the worst answer available â€”
// the same fault D308-D311 fixed for the sun, arriving through the ambient door. The stand-in three
// levels up is already claimed, is already converged, and covers this face: it is the best prior
// available for nothing, and the R9d measurement says a stand-in is about a tenth off and sharpens.
//
// Eight samples' worth, so it is believed at once and is outvoted by the face's own first burst of
// thirty-two. Not more: a prior that survives its own evidence is a bias, and the reason the sun's
// stand-in is read rather than accumulated is precisely that.
const uint kSkySeedSamples = 8u;

// ---- and how much of it is worth INHERITING, which is a different question ---------------------
//
// The one above is a prior for a face that has appeared out of nowhere. This is for the case that
// dominates a moving camera, and it was being treated as if it were the same thing.
//
// **A face is keyed by the level the pixel resolves**, so walking towards a wall does not refine a
// face -- it retires one and claims eight new ones. Each of those then spent `kSkyConverged` = 2,048
// samples measuring occlusion that the face it came out of had already measured, over the same
// geometry, in the same place, to the same 2,048 samples. Measured on the flight: about two thousand
// faces claimed a frame, times 2,048 samples, is **four million ambient rays a frame that re-derive
// something already in hand** -- which is D389's fault ("every ray a converged face was casting
// re-derived a constant it already held") happening at the moment of subdivision instead of after it.
//
// So a child inherits its parent's answer PROLONGED rather than copied, which is the part D392 was
// right to refuse:
//
//   - the mean is the parent's linear fit EVALUATED AT THE CHILD'S CENTRE, not the parent's mean.
//     A child sits at the edge of the parent's sampled span, so the difference is the whole of the
//     variation the gradient was fitted to carry;
//   - the gradient is rescaled by the change of extent -- half per level -- because a coefficient
//     against a coordinate means nothing without the coordinate's range. Copying it unchanged is
//     what D386 measured as 71% of all the roughness in the picture, and this is not that;
//   - both are shrunk by `c^2/(c^2 + noise)` first, exactly as resolve.comp shrinks them on read
//     (D386, D387), so a parent's noise is not prolonged into a child as invented tilt.
//
// The child still measures for itself -- `kSkyConverged` minus this is its own budget -- so the
// detail that is genuinely new at the finer scale is still measured, and the answer converges to
// the same place. What is bought is the part that was never new.
//
// It falls by a factor of four per level of separation, because a coarser ancestor covers a wider
// area and its fit says correspondingly less about any one child: one level up is worth nearly all
// of it, three levels up is the old stand-in and is worth about what the old stand-in was worth.
const uint kSkyProlong = 1536u;
// And the least the stand-in must have seen before it is worth inheriting. Seeding from four rays
// of noise is not a prior, it is the noise arriving one level coarser.
const uint kSkySeedMin = 64u;

// How far above a face its STAND-IN sits: the coarse face the composite reads while the fine one
// is still being discovered. Must match kFaceAncestorStep in src/world/face_store.hpp, which is
// where the stand-in is actually claimed.
//
// Three levels, because the number that matters is how many fine faces share one. Eight times
// coarser on each axis is five hundred and twelve to one, so a stand-in is claimed by the first of
// five hundred and twelve pixels to ask, and covers about sixty-four times the screen area of the
// face under it -- which is the whole mechanism: the fine face waits for a one-in-sixty-four
// sampling lattice to reach its own pixel, and the stand-in over it cannot be missed by that
// lattice at all. One level up would be four to one and would wait nearly as long as the face it
// is standing in for.
//
// Three is also the brick, which is what every face in the store was before D298, so the picture
// has already been seen at this granularity: it read as blocky, not as wrong.
const int kFaceAncestorStep = 3;

// ---- the emitter list, and lamps on the face (R3c) -------------------------------------------
//
// Where the lamps are, so a FACE can ask one for light instead of a pixel doing it. The list is
// built on the host by src/world/light_list.cpp, which merges emissive voxels into whole fittings:
// a sconce is a few hundred voxels and sampling each of them separately would spend the whole
// budget on one lamp.
//
// Declared field by field rather than as vectors on purpose. The buffer is a tightly packed
// 28-byte record and std430 aligns an ivec3 to sixteen bytes -- declaring the coordinate the
// obvious way silently reads every lamp after the first from the wrong offset. That trap has cost
// this project two debugging sessions in two different files, so it is written down at every place
// it could recur.
struct LightSource {
    int x;
    int y;
    int z;
    float red;
    float green;
    float blue;
    uint voxels;
};
layout(std430, binding = 14) readonly buffer Lights { LightSource items[]; } lights;

// Must match kMaxLights in src/world/light_list.hpp. A list exactly this long has been TRUNCATED,
// and a truncated list is one this cannot own: direct sampling owns emitters outright, so a lamp
// past the end would be lit by nobody and simply go out. The host reports the overflow; this is
// only the bound the shader clamps its index against.
const uint kMaxLights = 1024u;

// How many lamps are scored before one is kept. Resampled importance sampling, and it is not a
// refinement -- it is the difference between this helping and hurting. clips/many_lamps.clip holds
// eighty-four fittings and a face in one quarter of that hall is walled off from thirty of them;
// picking one uniformly makes about one sample in eighty-four useful and gives it eighty-four
// times the weight, which is unbiased and far noisier than doing nothing at all.
//
// The same figure and the same estimator the path tracer uses (kNeeCandidates), because the two
// have to agree about how bright a room is or the reference stops being a reference.
const uint kLampCandidates = 8u;

// Rays a face casts at the lamps on a frame it has not finished, and the argument is D394's: what
// this pass spends on an unconverged face is mostly the FACE and not the ray, so the cheapest thing
// to do with a face that has to measure is to let it measure all at once and be finished with it.
// Three ways of metering the ambient burst were built and all three made the transient worse.
const uint kLampBurst = 8u;

// And where it stops. A lamp is not the sun: it does not move, and unlike the ambient term it is
// not a pure function of geometry either -- it is a function of geometry AND of the emitter list.
// Both of those are announced exactly, on the frame they change (`light_reset` above, `edit_min.w`
// for the world), so a converged face has nothing left to discover and every ray it casts after
// this is re-measuring a constant it already holds.
//
// Five hundred and twelve rather than the ambient term's two thousand because the estimator is
// importance sampled rather than uniform: the variance left in it is which fitting was kept and
// whether the ray got there, not the whole hemisphere. At kLampBurst it is sixty-four frames, about
// a second, and the running mean is readable from the first sample.
const uint kLampConverged = 512u;

// What a face inherits when the list changes under it, and this is the whole of "instantly".
//
// A lamp that is placed, deleted or dimmed invalidates every accumulated lamp sample in the store.
// Throwing them away outright is exact and it FLASHES: the composite would read a mean of one or
// two rays on the next frame, which is an unbiased estimate with the variance of a coin toss, on
// every surface at once. So the mean is KEPT and its confidence is dropped to this -- the face's
// own next burst outvotes the prior eight to eight on the very next frame and has buried it within
// three, while the picture moves towards the new answer instead of exploding into noise.
//
// It is the same shape as kSkySeedSamples and for the same reason: a prior is believed only until
// evidence arrives, and evidence arrives at kLampBurst a frame.
const uint kLampSeedSamples = 8u;

// And the same for the lamps, on the same argument: a face that has just been subdivided out of a
// converged one is looking at the same fittings through the same geometry. Three quarters of
// kLampConverged, so a child spends a quarter of the samples its parent did and lands in the same
// place. Quartered per level of separation, as the ambient one is.
const uint kLampProlong = 384u;

// How many fittings an edit may ask about before it gives up and reopens the face anyway.
//
// The one place a face looks at the whole emitter list -- see lamp_path_crosses_edit, which runs on
// the single frame an edit is announced and decides whether that edit can have moved this face's
// lamp light at all. A cap is what keeps the cost of an edit bounded in a scene the size of
// clips/many_lamps.clip: past it the answer is the old conservative one, which is the behaviour
// that was there before and is never wrong, only wasteful.
const uint kLampEditProbe = 64u;

// The least a stand-in must have measured before a fine face is worth seeding from it. Same
// argument as kSkySeedMin: seeding from a handful of rays is not a prior, it is the noise arriving
// one level coarser.
const uint kLampSeedMin = 64u;

// Where a lamp shadow ray stops short of the fitting, in voxels.
//
// The ray is aimed INTO the lamp, and the lamp is made of matter: without this every sample would
// be occluded by the thing it is sampling. `light_list.hpp` guarantees the sphere of radius
// `0.87 * cbrt(voxels)` CONTAINS the fitting, so stopping a voxel short of that sphere can never
// enter it. What it costs is any occluder in the last few centimetres before the lamp, which errs
// bright by a hair and is the standard bargain.
const float kLampSurfaceMargin = 1.0;

// One more ray into a face's two counts. Halving at the window keeps the ratio exact -- both
// counts are integers and the halving is a shift -- while stopping a face that has watched the
// sun for an hour from needing another hour to notice it has set.
//
// # A sample that contradicts a unanimous history is the world having changed
//
// The window alone is far too slow for an EDIT, and the arithmetic says why: a floor indoors sits
// at nought lit of two hundred and fifty-six, so the first ray to reach the sun through a hole the
// player has just carved moves it to one in two hundred and fifty-seven. It needs a hundred and
// twenty-eight more rays to reach a half, and a settled face gets one every `face_stride` frames.
// Measured, carving a hole in the roof over the enclosed camera: the light reached **twelve per
// cent** of its converged value after three hundred frames -- five seconds -- and a player reads
// that as the chisel not casting shadows at all.
//
// The fix is not a shorter window, which would cost every face its penumbra. It is that
// UNANIMITY is what makes a contradiction meaningful. A face that has seen the sun on every one of
// two hundred rays and is now told it cannot is not observing noise; something moved. A face
// already at half has no such claim, and is left alone to converge as it always did.
//
// So a contradicted face keeps its answer and loses its confidence: two samples, the same ratio.
// The next ray puts it at a third or two thirds, and because two is below kFaceEager it is shaded
// every frame instead of one frame in `face_stride` -- so it lands on the new answer in a handful
// of frames rather than a thousand. It never drops below kFaceSettled, so the composite keeps
// reading it throughout and no pixel ever falls back to full sun on account of an edit.
//
// The false positive is a face whose true visibility is a shade under one -- a thin occluder that
// crosses the sun's disc for one sample in hundreds. It demotes, dips for two or three frames, and
// climbs back. That is the cost, it is small, and it is paid by faces that are nearly unanimous
// rather than by the penumbra, which is where a wrong answer would actually show.
uint face_accumulate(uint counters, bool reached_the_sun) {
    uint samples = face_samples_of(counters);
    uint lit = face_lit_of(counters);

    if (samples >= kFaceEager && (lit == samples ? !reached_the_sun : (lit == 0u &&
                                                                      reached_the_sun))) {
        lit = (lit == samples) ? 2u : 0u;
        samples = 2u;
    }

    if (samples >= kFaceWindow) {
        samples >>= 1u;
        lit >>= 1u;
    }
    return ((samples + 1u) & 0xFFFFu) | ((lit + (reached_the_sun ? 1u : 0u)) << 16);
}

// How much confidence a face keeps when the HOST announces that the world under it changed, as
// against how much it keeps when one of its own samples contradicts it (two, above).
//
// Not nought, and that is the whole of this constant. The paragraph above says it in as many words:
// a contradicted face keeps its answer and loses its confidence, "so the composite keeps reading it
// throughout and no pixel ever falls back to full sun on account of an edit". The edit announcement
// -- `edit_min.w == 2`, which arrived later -- then zeroed the counters outright, which is the one
// thing that sentence promises never happens. A face at nought samples is not answerable, so every
// pixel standing on it reads the coarse face three levels up; that stand-in is inside the same
// sixteen-metre box, so it has been zeroed too; so the pixel ends on a provisional face with no
// light at all. What a player sees is the whole room turning to eight-voxel blocks and flashing,
// once per edit, for as long as they keep building -- which is exactly how it was reported.
//
// Eight, the same figure and the same argument as kLampSeedSamples: a burst of new rays outvotes a
// prior of eight within a handful of frames, and the face sits under kFaceEager throughout, so it
// is shaded every frame until it has caught up rather than one frame in `face_stride`. D373's
// measurement is what the wipe was for -- a face mid-transition has no unanimity to contradict and
// averages back down inside a 256-sample window -- and a window of eight is not that window.
//
// The live value is `node_push.edit_seed`, so `--face-edit-seed 0` is the old behaviour exactly and
// the two arms of the A/B are one build (D407).
const uint kFaceEditSeed = 8u;

// A pair of counts scaled down to at most `seed` samples, keeping the ratio. The same arithmetic
// face_light_seed already does to a stand-in's history, applied to a face's own.
uint face_reseed(uint counters, uint seed) {
    const uint had = face_samples_of(counters);
    if (had <= seed) return counters;
    const uint lit = face_lit_of(counters);
    // Rounded rather than truncated, so a face at 255 of 256 does not come back at 7 of 8 and read
    // as a shade darker every time somebody places a voxel across the room.
    const uint kept = (lit * seed + had / 2u) / had;
    return (seed & 0xFFFFu) | ((kept & 0xFFFFu) << 16);
}

// What fraction of this face's rays reached the sun. One before any have been cast, so a face
// nobody has shaded yet is lit rather than black -- the composite gates on the sample count and
// never sees this, but a caller that forgets to should fail towards the picture it already had.
float face_visibility_of(uint counters) {
    uint samples = face_samples_of(counters);
    return samples == 0u ? 1.0 : float(face_lit_of(counters)) / float(samples);
}

// ---- what a face owes this frame, decided in one place ----------------------------------------
//
// Two passes ask this question and they must not answer it differently. `face_worklist.comp` asks
// it to decide whether a slot goes in the compacted dispatch at all; `shade_faces.comp` asks it to
// decide what to do with the slot it was handed. If the two ever disagree in the direction of the
// worklist being the stricter, a face silently stops being lit and nothing anywhere says so -- the
// exact shape of trap 7 and of the five bugs R3c's chain produced.
//
// So it is one function returning everything either caller needs, rather than one predicate copied
// twice. It reads the record, the seen stamp and the push block, and touches nothing else.
struct FaceWork {
    bool owed;               // is this slot worth an invocation at all
    bool may_cast;           // ...and is it worth a RAY, which is the narrower question
    bool near_edit;
    bool near_edit_contact;
    bool edit_reset;
    bool sun_due;
    bool ambient_idle;
    bool lamp_idle;
    uint counters;           // nought when the edit reset it
    uint samples;
};

FaceWork face_work_of(uint slot, Face face, bool provisional_face) {
    FaceWork w;
    w.owed = false;
    w.may_cast = false;
    w.near_edit = false;
    w.near_edit_contact = false;
    w.edit_reset = false;
    w.sun_due = false;
    w.ambient_idle = false;
    w.lamp_idle = false;
    w.counters = 0u;
    w.samples = 0u;
    if (!face_live_of(face.packed)) return w;

    // Is this face inside the box the host says it has just changed, and inside the much smaller
    // box the NEAR field can have been changed by. See kEditAmbientReach.
    if (push.edit_min.w != 0) {
        const float extent = float(1 << face_level_of(face.packed));
        const vec3 low = vec3(ivec3(face.x, face.y, face.z)) * extent -
                         vec3(push.camera_chunk.xyz * 256);
        w.near_edit = all(greaterThanEqual(low + vec3(extent), vec3(push.edit_min.xyz))) &&
                      all(lessThanEqual(low, vec3(push.edit_max.xyz)));
        const vec3 tight_min = vec3(push.edit_min.xyz) + vec3(float(kEditAmbientShrink));
        const vec3 tight_max = vec3(push.edit_max.xyz) - vec3(float(kEditAmbientShrink));
        w.near_edit_contact = w.near_edit &&
                              all(greaterThanEqual(low + vec3(extent), tight_min)) &&
                              all(lessThanEqual(low, tight_max));
    }
    w.edit_reset = (push.edit_min.w == 2) && w.near_edit;
    w.may_cast = provisional_face || node_face_recently_seen(slot);

    // Nothing to do at all: nobody is looking at it and the host has announced nothing.
    if (!w.may_cast && !w.edit_reset && node_push.light_reset == 0u) return w;

    // An announcement lowers a face's confidence; it does not erase its answer. See kFaceEditSeed:
    // zeroing this is what made every edit turn the room into flashing blocks.
    w.counters = w.edit_reset ? face_reseed(face.counters, node_push.edit_seed) : face.counters;
    w.samples = face_samples_of(w.counters);
    w.sun_due = !(node_push.face_stride > 1u && w.samples >= kFaceEager && !w.near_edit &&
                  ((slot + node_push.frame) % node_push.face_stride) != 0u);
    w.ambient_idle = (face.photons & kFaceAmbientIdle) != 0u && !w.edit_reset;
    w.lamp_idle = (face.photons & kFaceLampIdle) != 0u && !w.edit_reset &&
                  node_push.light_reset == 0u;
    w.owed = w.sun_due || !w.ambient_idle || !w.lamp_idle;
    return w;
}

// ---- the compacted dispatch --------------------------------------------------------------------
//
// The slots that owe work, packed, with a dispatch command in front of them.
//
// The shading pass used to be dispatched over every live slot, and by the time light stopped at
// what a pixel had read that was 220,000 faces of work spread over 772,000 invocations -- about
// eighteen busy lanes in every workgroup of sixty-four, and a workgroup runs for as long as its
// slowest lane. The idle five sixths cost nothing in bandwidth and everything in occupancy.
//
// Words 0-2 are a VkDispatchIndirectCommand, so the host never has to know the count; word 3 is
// that count for the shader to bound itself against; the slots follow.
layout(std430, binding = 16) buffer FaceWorkList {
    uint groups_x;
    uint groups_y;
    uint groups_z;
    uint count;
    uint slots[];
} face_work;

// The R_4 low-discrepancy sequence: one point of a four-dimensional stratified series per index.
//
// The ambient sampler needs four numbers a ray â€” where on the face it leaves from, and which way â€”
// and it takes them from ONE sequence rather than from two 2D ones. That is not tidiness. Two R_2
// sequences advanced by the same index are both affine in that index, so their points sit in a
// fixed relative pattern for the life of the face: where a sample stands on the face would predict
// which way it leaves, and the pair â€” which is exactly what the gradient is a fit over â€” would be
// drawn along a line through a 4D space instead of spread over it. The pattern depends on the
// per-face rotation, so it would appear on some faces and not others, which is structured error.
// The eye finds structure and does not find noise.
//
// alpha_j = phi_4^-j, where phi_4 is the real root of x^5 = x + 1 -- the four-dimensional
// generalisation of the plastic constant the 2D version uses.
vec4 node_r4(uint index, vec4 rotation) {
    const vec4 alpha = vec4(0.8566748838545029, 0.7338918566271259,
                            0.6287067210378086, 0.5386328571121754);
    return fract(alpha * float(index) + rotation);
}

// The slot a face lives in, or kNoFace. Never claims -- claiming is the CPU's, from the requests
// this pass puts down the feedback buffer, so a face that has not been claimed yet is simply not
// here for a frame or two and the composite falls back to its own lighting. That is the same
// graceful degradation an unstreamed node already gets.
//
// Mirrors FaceStore::find exactly, including the two rules that made it correct: an empty bucket
// ENDS the run, because a probe that walked past one would miss a face a later insertion
// displaced; a tombstone does NOT, because that is what makes it a tombstone -- and it is not a
// slot number either, so indexing it reads past the end of the table.
uint node_face_lookup(ivec3 node, uint level, uint face) {
    if (node_push.face_capacity == 0u) return kNoFace;
    uint mask = node_push.face_capacity - 1u;
    uint bucket = node_hash_lattice(node, (level << 3) | face) & mask;
    for (uint probe = 0u; probe < node_push.face_probes; ++probe) {
        uint slot = face_entries.slots[(bucket + probe) & mask];
        if (slot == kNoFace) return kNoFace;
        if (slot == kFaceTombstone) continue;
        Face f = faces.items[slot];
        if (f.x == node.x && f.y == node.y && f.z == node.z &&
            face_level_of(f.packed) == level && face_dir_of(f.packed) == face) {
            return slot;
        }
    }
    return kNoFace;
}

// ---- the card's own claim -----------------------------------------------------------------------
//
// A face the CPU has not heard of yet, claimed here, in the pass that discovered it.
//
// # Why this exists
//
// The store is claimed from the feedback buffer, and feedback is two frames old by construction:
// the GPU writes a report on frame N and the host may not read it until frame N+2, because that is
// when frame N's command buffer has retired. So a surface revealed by turning round had NO face for
// two frames however fast everything else was, and the composite lit it with full sun -- measured
// at a hard cut as two full frames of a completely unshadowed room. No arrangement of host code
// shortens that; reading the buffer earlier is a fence wait.
//
// So this table is the card's. It holds STAND-INS only -- the coarse face kFaceAncestorStep above
// the fine one, which five hundred and twelve faces share -- so a screen needs a few thousand of
// them rather than the half million faces it holds per voxel. The store's own claims arrive two
// frames later and take over, and a provisional entry is simply never looked at again.
//
// # Why the slot IS the bucket, and why that is the whole trick
//
// There is no allocator here. A face's slot is the bucket its key hashes to, so a claim is one
// atomicCompSwap and nothing else -- and the pixel that LOSES that race learns the winner's slot
// from the value the atomic hands back, in the same instruction. That matters more than the saving:
// the alternative is allocating a slot and then publishing it, where a pixel that lost has to read
// what the winner wrote, and nothing orders two workgroups. Every other pixel on the face would
// have to wait a frame, which is the frame this exists to remove.
//
// The word holds the frame it was claimed in and a tag from a second hash. An entry stamped with an
// older frame is free, so nothing is ever cleared and there is no sweep: the first claimant of a
// bucket this frame takes it. A face therefore re-claims itself each frame it is needed, and gets
// one fresh sample per frame, which is exactly what kFaceSettled = 1 makes readable.
//
// A false match needs the same bucket AND the same twenty-four-bit tag from an independent hash --
// fifteen bits plus twenty-four, so about one pair in five hundred billion. What it would cost is
// one coarse face reading another's shadow for one frame.
const uint kProvisionalMask = (1u << 15) - 1u;   // must match kProvisionalFaces in face_buffers.hpp

uint node_face_claim(ivec3 node, uint level, uint face, out bool claimed) {
    claimed = false;
    if (node_push.provisional_base == 0u) return kNoFace;

    const uint tag = node_hash_lattice(node.zxy, (level << 3) | face | 0x8000u) & 0xFFFFFFu;
    const uint want = ((node_push.frame & 0xFFu) << 24) | tag;
    uint bucket = node_hash_lattice(node, (level << 3) | face) & kProvisionalMask;

    for (uint probe = 0u; probe < 8u; ++probe) {
        const uint at = (bucket + probe) & kProvisionalMask;
        uint current = provisional.marks[at];
        if (current == want) return node_push.provisional_base + at;   // already ours this frame
        if ((current >> 24) != (node_push.frame & 0xFFu)) {
            // Stale or never used: take it. Whoever wins writes the record; whoever loses reads
            // the winner's mark out of the compare-exchange and needs nothing else.
            const uint previous = atomicCompSwap(provisional.marks[at], current, want);
            if (previous == current) {
                claimed = true;
                return node_push.provisional_base + at;
            }
            if (previous == want) return node_push.provisional_base + at;
        }
    }
    return kNoFace;
}

// ---- the descent, with its stack ---------------------------------------------------------------

const uint kFoundHere = 0u;
const uint kFoundEmpty = 1u;
const uint kFoundWanted = 2u;

struct Found {
    uint slot;
    int level;     // where the descent stopped
    uint state;
};

// The last root a ray entered, and nothing else.
//
// The first version kept the whole descent Ã¢â‚¬â€ a twelve-element array indexed by level, so a step
// could restart from the deepest ancestor that still contained the point. The arithmetic for that
// is free (two points share every ancestor above the highest bit in which they differ, so one XOR
// and one findMSB says where to restart), and it looked like the obviously right thing.
//
// It is not, and the reason has nothing to do with the algorithm. A GPU keeps a local array in
// registers only while every index into it is known at compile time. This one is indexed by a
// level computed at run time, so the whole array goes to scratch memory, and every step then pays
// a round trip to memory to save itself a handful of loads that were going to hit cache anyway.
// Measured: 11.52 ms against the old marcher's 1.60 ms with the array, and see below without it.
//
// A single root, held in two scalars, costs registers and stays in them. It saves the hash Ã¢â‚¬â€ which
// is the one genuinely expensive part, since a ray crosses few 512 m blocks Ã¢â‚¬â€ and the descent
// below it is a handful of dependent loads through nodes whose siblings share a cache line.
//
// # Why the root alone was not enough
//
// It saved the hash and nothing else: every step still walked all eleven levels from 512 m down to
// a brick. Measured on the enclosed room, which is the one camera this marcher lost on: 9.12 steps
// a pixel against the chunk marcher's 31.27, and 1.088 ms against 0.723. Three and a half times
// fewer steps and half again the time, so a step cost about five times what the old one did, and
// the step is this descent.
//
// So two more ancestors are cached, at fixed levels, in named scalars - never an array indexed by a
// level, which is the trap above. A cached ancestor is used when the point being located sits in
// the same cell: two points in one cell at level L share every ancestor above L by definition, so
// they take the same octant at every level down to L and every child-mask test above L gives the
// same answer. The descent from the cache is therefore identical to the descent from the root, and
// the picture cannot move - which is the claim the image diff checks rather than assumes.
//
// The levels are chosen against how far a step actually moves. Near geometry a ray marches bricks,
// so it crosses a 1 m cell every four steps and an 8 m cell every thirty-two: the deep cache pays
// two levels a step, the middle one three levels every fourth step, and the root six every
// thirty-second.
//
// Nothing needs invalidating between steps. The pool is immutable for the whole dispatch, so a
// cached slot is a function of the tree rather than of the path taken to it - a stale entry whose
// cell still contains the point is the same node it would have been found by walking.
const int kMidLevel = 8;    // 256 voxels - 8 m
const int kFineLevel = 5;   // 32 voxels - 1 m

// # The cache belongs to the INVOCATION, not to the ray, and that is worth more than it looks
//
// These are declared with initialisers, and a global initialiser in GLSL runs once per invocation
// before main. So an invocation that marches many rays enters the tree cold ONCE, and there is
// nothing left for a reset to do -- which is why there is no longer a node_walk_reset() to call.
//
// `node_march` used to call one at the top of every march, on the reasonable-sounding grounds that
// a ray should start from a known state. It cost the whole of this cache to every ray after the
// first. The face pass casts about twenty-five rays per unconverged face -- kSkyBurst near rays,
// one far ray, kLampBurst lamp rays and the sun's -- and every one of them paid a hash probe and an
// eleven-level descent from the 512 m root before its first step, on rays that are BOUNDED AT ONE
// METRE and then take a handful of steps. At the flight camera that is about 6.3 M descents a
// frame (D412).
//
// Keeping it is not an approximation. The pool is immutable for the whole dispatch, so a cached
// slot is a function of the tree and not of the path taken to it, and `node_locate` block-checks
// every entry before trusting it: an entry whose cell still contains the point is the same node a
// walk from the root would have found. Rays leaving one face share an origin cell, so rays 2..25
// enter at level 5 and descend two levels instead of eleven -- bit-identically.
uint g_node_root = kNoNode;
ivec3 g_node_block = ivec3(0x7FFFFFFF);
uint g_node_mid = kNoNode;
ivec3 g_node_mid_block = ivec3(0x7FFFFFFF);
uint g_node_fine = kNoNode;
ivec3 g_node_fine_block = ivec3(0x7FFFFFFF);

// The walk itself, from wherever it is entered. Split out from node_locate so that the entry can be
// the root or a cached ancestor without two copies of the loop drifting apart.
Found node_descend(ivec3 p, int target, uint slot, int level) {
    Found result;
    result.slot = kNoNode;
    result.level = level;
    result.state = kFoundWanted;

    while (level > target) {
        uint packed = nodes.items[slot].packed;
        if ((node_flags_of(packed) & kNodeLeaf) != 0u) break;   // as fine as this gets

        int child_level = level - 1;
        uint octant = uint(((p.x >> child_level) & 1) | (((p.y >> child_level) & 1) << 1) |
                           (((p.z >> child_level) & 1) << 2));

        if ((node_child_mask_of(packed) & (1u << octant)) == 0u) {
            // Empty, and empty at a known size: the whole cell at child_level holds nothing.
            result.slot = slot;
            result.level = level;
            result.state = kFoundEmpty;
            return result;
        }

        uint children = nodes.items[slot].children;
        // A shell: the mask says the world has children and the pool has not allocated a run for
        // them. WANTED, and tested before the arithmetic below, because `children` is kNoNode here
        // and `kNoNode + octant` wraps to an address that reads whatever is in the buffer and
        // calls it geometry.
        if (children == kNoNode) {
            result.slot = slot;
            result.level = level;
            result.state = kFoundWanted;
            return result;
        }

        uint child = children + octant;
        if (node_level_of(nodes.items[child].packed) == 0u) {
            // The mask says the world has it; a level of nought says the pool has not built it.
            result.slot = slot;
            result.level = level;
            result.state = kFoundWanted;
            return result;
        }

        slot = child;
        level = child_level;

        // Keep the two ancestors the next step is most likely to enter at. Each test is against a
        // compile-time constant, so these stay named scalars and never become an array indexed by
        // a run-time level - which is the thing that sent the first version to scratch memory.
        if (level == kMidLevel) {
            g_node_mid = slot;
            g_node_mid_block = ivec3(p.x >> kMidLevel, p.y >> kMidLevel, p.z >> kMidLevel);
        } else if (level == kFineLevel) {
            g_node_fine = slot;
            g_node_fine_block = ivec3(p.x >> kFineLevel, p.y >> kFineLevel, p.z >> kFineLevel);
        }
    }

    result.slot = slot;
    result.level = level;
    result.state = kFoundHere;
    return result;
}

Found node_locate(ivec3 p, int target) {
    target = clamp(target, kLeafLevel, kEntryLevel);

    // The deepest cached ancestor that still contains this point and is not already below what was
    // asked for. `>=` rather than `>`: a cache sitting exactly at the target level is the answer,
    // and the loop exits on its first test and returns it.
    ivec3 fine_block = ivec3(p.x >> kFineLevel, p.y >> kFineLevel, p.z >> kFineLevel);
    if (kFineLevel >= target && g_node_fine != kNoNode && fine_block == g_node_fine_block) {
        return node_descend(p, target, g_node_fine, kFineLevel);
    }

    ivec3 mid_block = ivec3(p.x >> kMidLevel, p.y >> kMidLevel, p.z >> kMidLevel);
    if (kMidLevel >= target && g_node_mid != kNoNode && mid_block == g_node_mid_block) {
        return node_descend(p, target, g_node_mid, kMidLevel);
    }

    ivec3 block = ivec3(p.x >> kEntryLevel, p.y >> kEntryLevel, p.z >> kEntryLevel);
    if (block != g_node_block) {
        g_node_block = block;
        g_node_root = node_entry_lookup(block);
    }

    if (g_node_root == kNoNode) {
        // No root means the WORLD is empty here, not that the pool is behind: the pool seeds a
        // root for every entry block the world occupies, whether anything has looked at it or not.
        // See the note in NodePool::update.
        //
        // That invariant is load-bearing, and it was broken once: root eviction used to remove the
        // entry as well, so a block whose root had gone cold read as open sky and a shadow ray flew
        // straight out of a sealed room. A cold root now sheds its children and keeps its entry, so
        // the only thing that clears one is the world not being there (D324).
        //
        // Reporting this as wanted instead is D133 in a new structure. A ray crossing open sky
        // asks for nothing, the CPU has nothing to give it, and the same useless request repeats
        // every frame while every counter reads calm Ã¢â‚¬â€ measured at 3.7 million requests a run
        // against 367,000 hits, with the tree frozen and the scene undrawn.
        Found result;
        result.slot = kNoNode;
        result.level = kEntryLevel;
        result.state = kFoundEmpty;
        return result;
    }

    return node_descend(p, target, g_node_root, kEntryLevel);
}

// ---- inside a leaf -------------------------------------------------------------------------------

bool leaf_voxel_solid(uint leaf, ivec3 local) {
    uint index = uint(local.x + local.y * 8 + local.z * 64);
    return ((occupancy.words[leaf * 16u + (index >> 5u)] >> (index & 31u)) & 1u) != 0u;
}

uint leaf_read_byte(uint byte_offset) {
    uint word = payload.words[byte_offset >> 2u];
    return (word >> ((byte_offset & 3u) * 8u)) & 0xFFu;
}

uint leaf_voxel_type(uint leaf, ivec3 local) {
    LeafHeader header = leaves.items[leaf];
    uint bits = header.packed & 0xFFu;
    if (bits == 0u) return header.uniform_type;

    uint index = uint(local.x + local.y * 8 + local.z * 64);
    uint base = header.payload_offset;
    if (bits == 32u) return payload.words[(base >> 2u) + index];

    uint palette_bytes = header.palette_count * 4u;
    uint per_byte = 8u / bits;
    uint packed = leaf_read_byte(base + palette_bytes + index / per_byte);
    uint slot_index = (packed >> ((index % per_byte) * bits)) & ((1u << bits) - 1u);
    return payload.words[(base >> 2u) + slot_index];
}

// ---- reporting ------------------------------------------------------------------------------------
//
// One node per ray: the first one it wanted and could not find. First rather than deepest, so
// streaming converges front to back Ã¢â‚¬â€ which is also the order a player notices.

void node_note(inout bool has_pending, inout ivec4 pending, ivec3 coord, int level) {
    if (has_pending) return;
    has_pending = true;
    pending = ivec4(coord, level);
}

void node_flush(bool enabled, bool has_pending, ivec4 pending) {
    if (!enabled || !has_pending) return;
    uint index = atomicAdd(feedback.count, 1u);
    if (index < push.resolution.w) feedback.entries[index].coord = pending;
}

// A ray reporting what it USED, which is the other half of residency and was missing.
//
// Feedback has only ever carried misses, and a miss is the one thing that cannot keep a node
// alive: the moment the tree is complete the rays stop missing, nothing is reported, and the
// pool's idea of what is wanted stops advancing for everything at once. It then evicted the
// whole scene on a timer while every ray was reading it (D247).
//
// Reported at the ENTRY level, because that is the granularity eviction works at - a root is
// released with its entire subtree - so one report per visible root per frame is all that is
// needed, and every pixel would otherwise say the same handful of things. A sparse sample
// covers it by a wide margin at a fraction of the buffer.
const int kFeedbackUsed = 0x10000;   // in the level field: used, as against missing
const int kFeedbackRead = 0x20000;   // ...and this one carries a slot in x, not a coordinate

// A FACE the eye can see: the node the ray stopped on and the direction it was hit from.
//
// This is what the face pass shades, and it goes down the buffer that already exists rather than
// a second one, because a face request is the same four integers a node request is -- a
// coordinate and a level -- with the direction packed beside the level. One buffer means one
// capacity, one readback and one barrier to get right instead of two.
const int kFeedbackFace = 0x40000;

// This coordinate is EXACT: do not dilate it to its neighbours.
//
// A miss report is a guess about where geometry might be, so the consumer grows it by one face in
// each direction and streams the neighbours too -- without which only the cells some ray happened
// to land on are ever built and the edges of what has streamed show notches. A shadow ray's report
// is not a guess: it is the one cell that stopped the ray. Dilating it asks for six cells nobody
// has any reason to want, and at about fifty thousand such reports a frame that is a factor of
// seven on the request volume, which is where the node pool's CPU goes (D351).
const int kFeedbackExact = 0x80000;

// A light ray keeps the geometry it is standing on, and asks for nothing new.
//
// D292 forbids a light path from dragging residency towards whatever it crosses, and that rule is
// right: a ray that REQUESTED everything it passed through would ask for the world behind every
// surface. This is not that. It is a stamp on a node that is already built, saying the node is in
// use -- and without it the pool cannot tell a brick nothing wants from a brick that occludes half
// the room, because the only thing a light ray has ever said is "this cell is MISSING".
//
// What that silence costs is a closed loop with a period of exactly `cold_frames`: a light ray is
// stopped by an unbuilt cell and reports it, the pool builds it, no primary ray ever reads it
// because it is an occluder and not a visible surface, six hundred frames later the erosion sweep
// takes it, and the next light ray to reach it reports it again. Measured on the close camera after
// D427 closed the primary-ray half: 28,764 of the 29,077 remaining rebuilds in a settled, static,
// un-edited run, none of them asked for by a primary ray. D429.
//
// Throttled per NODE rather than per ray, which is the difference between a guarantee and a sample:
// a brick occluding anything at all is reported within `light_read_period` frames whatever hits it,
// against a six-hundred-frame cold window. Throttling per ray instead reports a busy brick hundreds
// of times and a quiet one never, which is the wrong way round -- the quiet one is the one at risk.
//
// The period is a push constant and `--light-read-period N` sets it, because it is the whole of the
// trade this rule makes -- feedback entries against how long a silent occluder survives -- and a
// trade nobody can sweep at run time is a trade somebody guesses at.
//
// Unsigned arithmetic on purpose, exactly as node_face_recently_seen does it: a slot never stamped
// reads nought, and `frame - 0` is past any period from the first frames of the run.
bool node_light_due(bool enabled, uint slot) {
    if (!enabled || slot == kNoNode || node_push.light_read_period == 0u) return false;
    const uint period = node_push.light_read_period;
    if (slot >= node_seen.frames.length()) return false;

    // The array IS the throttle, and nothing cheaper may stand in front of it. A slot-hash pre-gate
    // was tried here to keep the load off the memory bus -- eligible one frame in `period`, decided
    // by arithmetic -- and it cost most of the benefit: churn 1,028 against 8,651 on the settled
    // close camera, because a brick is only reported if a light ray happens to read it on the one
    // frame it is eligible, and a converged face casts nothing on most frames. What it was bought
    // for turned out not to exist: the +6% it was chasing on the faces pass was two rounds of
    // noise, and four interleaved rounds put the two arms inside each other's spread. D430.
    //
    // So: reported the first time a ray reads it after `period` frames, whenever that is. Every one
    // of the thousands of rays stopped by the same brick after that reads `frame - frame`, which is
    // nought, and falls out here -- which is the whole job.
    if ((node_push.frame - node_seen.frames[slot]) < period) return false;
    node_seen.frames[slot] = node_push.frame;
    return true;
}

void node_flush_used(bool enabled, ivec3 block) {
    if (!enabled) return;
    uint index = atomicAdd(feedback.count, 1u);
    if (index < push.resolution.w) {
        feedback.entries[index].coord = ivec4(block, kEntryLevel | kFeedbackUsed);
    }
}

// And the node itself, by slot.
//
// The root says a ray entered a 512 m block, which is all eviction could act on while residency
// was tracked per root -- and a scene inside one root can then only be kept whole or dropped
// whole, which is why resident memory could not follow the screen (D260). What actually decides
// that is which NODES were read, and a ray knows the slot it stopped on because it descended to
// it. A slot rather than a key so the CPU can store it instead of walking the tree to find it
// again: sixteen thousand of these a frame is sixteen thousand stores.
//
// The CPU and the GPU agree about slot numbering by construction -- the buffers are a mirror of
// the arrays, and NodeBuffers::audit checks it byte for byte.
// The face a ray stopped on. `voxel` is the absolute coordinate of the cell it stopped in and
// `level` is that cell's level, so the node's own coordinate is one shift -- the same arithmetic
// the descent already did to get there, so the two cannot disagree about which face this is.
void node_flush_read(bool enabled, uint slot) {
    if (!enabled || slot == kNoNode) return;
    uint index = atomicAdd(feedback.count, 1u);
    if (index < push.resolution.w) {
        feedback.entries[index].coord = ivec4(int(slot), 0, 0, kFeedbackRead);
    }
}

// ---- the march ------------------------------------------------------------------------------------

struct NodeHit {
    bool hit;
    // Whether it stopped on a cell the pool has NOT built, rather than on matter it can see. Both
    // are `hit` for occlusion, and telling them apart is the difference between a shadow cast by
    // something and a shadow cast by ignorance.
    bool unknown;
    float t;
    ivec3 normal;
    uint type_id;    // valid when level == kLeafLevel and the voxel resolved
    uint colour;     // rgba8, valid above the leaf
    uint coverage;   // how much of the cell is matter AS SEEN ALONG the face that was hit
    // The face this ray stopped on: the node's own coordinate at `face_level`, and which of six
    // directions it was hit from. The same three numbers the face request carries, produced by the
    // same arithmetic in the same place -- so the pass that asks for a face and the pass that
    // looks it up cannot disagree about which face it is.
    // `face_level` is kNoFaceLevel when the ray stopped on nothing. It used to be nought, which
    // was the same collision on zero that kFaceLive fixes on the other side: a level-0 face is a
    // single voxel and is the ordinary case near the camera, so "level 0" stopped meaning "no
    // face" the moment the marcher started reporting voxels. Every shadow within twenty metres of
    // the camera disappeared, because the composite skipped the lookup for exactly those pixels.
    ivec3 face_node;
    uint face_level;
    uint face_dir;
    int level;
    uint steps;
};

const uint kNoFaceLevel = 0xFFFFFFFFu;

// Coverage along one face direction, out of the six bytes a node carries.
//
// This is the quantity, and the alpha of the filtered colour is not. That alpha is a *volumetric*
// fill fraction folded from the subtree, so it halves at every level and a sparse branch decays to
// nothing - which is fine as a measure of how much matter is in a box and useless as a measure of
// how much of a pixel the box covers. Feeding it to the composite made every coarse hit nearly
// transparent, so the marcher hit geometry on 100% of pixels at close range and drew sky.
//
// Stage 4 hit the same wall from the other side and recorded it: a brick on the surface of the
// ground is about an eighth full and completely opaque when you look at it, and the two quantities
// coincide only for a node seen edge-on. Per direction, folded at build time, this is exact.
uint node_face_coverage(uint slot, ivec3 normal) {
    uint xy = nodes.items[slot].coverage_xy;
    uint z = nodes.items[slot].coverage_z;
    if (normal.x != 0) return (normal.x > 0) ? (xy & 0xFFu) : ((xy >> 8u) & 0xFFu);
    if (normal.y != 0) return (normal.y > 0) ? ((xy >> 16u) & 0xFFu) : ((xy >> 24u) & 0xFFu);
    return (normal.z > 0) ? (z & 0xFFu) : ((z >> 8u) & 0xFFu);
}

// The bounds in the parameter block are in chunks, which the renderer still uses as the unit it
// reports the world's extent in. Nothing else here knows what a chunk is.
const int kNodeChunkEdge = 256;

// The coarsest cell the marcher will step, matching the chunk marcher's kMaxLevel.
//
// Not the same number as kEntryLevel and it must not be: the entry level is where the hash table
// lives, while this is what goes into the visibility buffer's eight-bit level field - which
// resolve divides by seven for the detail view and, more importantly, uses to decide whether a
// pixel reports full coverage or the node's filtered fraction. Writing a node level of up to
// fourteen into a field that means nought-to-seven is one of the two ways these marchers came to
// disagree about every pixel.
const int kNodeMaxDetail = 7;

// Ordered dither, exactly as shaders/world.glsl does it. Deterministic per pixel, so the level a
// pixel picks does not flicker between frames - which is what lets it work with no temporal
// accumulation behind it.
float node_bayer(ivec2 pixel) {
    const int table[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
    return float(table[(pixel.y & 3) * 4 + (pixel.x & 3)]) / 16.0;
}

// Where this frame's sparse-sample lattice sits, for a power-of-two stride.
//
// A sampling grid that never moves is not a sample of the screen; it is a permanent choice of
// which pixels are allowed to speak. Everything that lands between the points is invisible to
// whatever is being fed, and no amount of waiting fixes it because the next frame asks the same
// pixels again. Walking the offset one column a frame, wrapping into the next row, visits every
// phase in stride^2 frames and asks nothing of the caller but the frame number.
uvec2 node_sample_phase(uint frame, uint stride) {
    uint mask = stride - 1u;
    return uvec2(frame & mask, (frame >> uint(findMSB(stride))) & mask);
}

// Records the face this ray stopped on, and reports it when this pixel is one of the ones asking.
//
// Recording is unconditional and reporting is not: EVERY pixel needs the key so the composite can
// look its face up, while only a fraction need to ask for one to be claimed. Splitting those two
// jobs across two pieces of code is how they would come to disagree about which face a pixel is
// on -- and a composite reading a different face from the one being shaded is a lighting bug with
// no visible cause.
void node_face_hit(inout NodeHit result, bool report, ivec3 voxel, int level, ivec3 normal) {
    uint face;
    if (normal.x != 0) face = (normal.x > 0) ? 0u : 1u;
    else if (normal.y != 0) face = (normal.y > 0) ? 2u : 3u;
    else face = (normal.z > 0) ? 4u : 5u;

    result.face_node = ivec3(voxel.x >> level, voxel.y >> level, voxel.z >> level);
    result.face_level = uint(level);
    result.face_dir = face;

    // One entry, still: the stand-in three levels up is a SHIFT of this key, so the CPU derives it
    // from this request rather than being told it twice.
    //
    // Sending it would double the face traffic in a buffer that is already the binding constraint
    // -- 57,600 face samples a frame at 1440p against a capacity of 131,072 shared with the node
    // reports -- and buying nothing with it, because a coordinate that can be computed from another
    // coordinate is not information. See the claim in src/app/main.cpp.
    if (!report) return;
    uint index = atomicAdd(feedback.count, 1u);
    if (index < push.resolution.w) {
        feedback.entries[index].coord =
            ivec4(result.face_node, (level & 0xFF) | int(face << 8) | kFeedbackFace);
    }
}

const uint kNodeMaxSteps = 512u;

// `stand_in` is what a ray does when the pool does not know what is in a cell the world says is
// occupied. A PRIMARY ray draws the parent, because the alternative is sky where a building is
// (R2d). A SHADOW ray must not: "I do not know" is not "opaque", and treating it as opaque means
// every ray leaving a surface is stopped by the first unbuilt cell it meets. The tree is only
// refined where the camera looks, and a shadow ray leaves the surface in whatever direction the
// sun happens to be, so it meets one almost at once -- measured, with this reading as solid:
// 15,038 faces settled, 14,325 of them fully shadowed, and NOT ONE face anywhere in the scene
// lit, on an outdoor camera at midday.
//
// Marching past it means a shadow can be missed while the tree fills in, which is a shadow that
// arrives late. Standing in means a scene that is black and stays black.
// `occlude_unknown` is the OTHER half of the same question, and the half a shadow ray needs.
//
// `stand_in` decides what COLOUR to draw for a cell the pool has not built. It cannot answer for a
// shell -- a node the world says is occupied whose children have never been built -- because a
// shell has never been folded from anything and its colour is nought, and drawing that paints a
// building black. So a shell falls through and the ray carries on as though the cell were empty.
//
// That is right for a ray that is deciding what you can see and wrong for a ray that is deciding
// what you can see THROUGH. For occlusion there is nothing to draw: "the world has matter here and
// the pool has not built it yet" is matter, and a shadow ray must stop. Letting it through is the
// sun shining in through a wall that has not finished streaming, and indoors -- where every
// correct answer is full shadow -- it measured as 47,353 of 93,745 faces leaking about a tenth of
// their rays, with not one pixel of the room above 0.9 for that light to have come from.
// `max_t` is how far this ray is allowed to travel, in voxels, and it is a parameter rather than a
// constant because one caller asks a question that is answered within a metre.
//
// The ambient near field darkens over `kContactMetres` and is flat beyond it, so a ray sent to
// answer it has nothing left to learn after 32 voxels: everything past that returns the same
// number. Marching on is not conservatism, it is spending the whole step budget to re-derive a
// value already in hand â€” and D148 is the standing measurement of what a ray that keeps stepping
// costs (0.78 ms to 23.7 ms from widening a clip). Pass `kNodeUnbounded` for a ray that really does
// need to know what is at the other end of the world, which is the sun ray and the primary ray.
//
// A bounded ray that reaches its limit returns `hit == false`, exactly as a ray that reached open
// sky does â€” and those are NOT the same fact. Trap 7 in the handover is this exact shape: "nothing
// here" and "I stopped looking" must never be one answer. So a bounded ray may only be asked
// questions whose answer is the same for both, which is what the near field is and what sky
// visibility emphatically is not. The far field keeps an unbounded ray of its own for that reason,
// and the two counts are kept apart in the record so neither can be read as the other.
const float kNodeUnbounded = 3.4e38;

NodeHit node_march(vec3 origin, vec3 dir, float pixel_angle, float dither, bool report,
                   bool report_used, bool report_face, bool stand_in, bool occlude_unknown,
                   float max_t) {
    NodeHit result;
    result.hit = false;
    result.unknown = false;
    result.t = push.lens.y;
    result.normal = ivec3(0, 1, 0);
    result.type_id = 0u;
    result.colour = 0u;
    result.coverage = 255u;
    result.face_node = ivec3(0);
    result.face_level = kNoFaceLevel;
    result.face_dir = 0u;
    result.level = kLeafLevel;
    result.steps = 0u;

    // No walk reset here. The descent cache is the invocation's, and it survives from one ray to
    // the next on purpose -- see the globals it lives in for why that is exact rather than close.

    vec3 inv_dir = 1.0 / max(abs(dir), vec3(1e-9));
    ivec3 step_dir = ivec3(sign(dir));
    ivec3 origin_voxel = push.camera_chunk.xyz * 256;

    float t = 0.0;

    // Clipped to the extent of the world, and this is not slack.
    //
    // Nothing outside it can be hit, so stepping through it looking is pure loss â€” and the loss is
    // not small, because a ray that will never hit anything keeps stepping until its budget runs
    // out. D148 measured exactly this on the old marcher: widening the clip took a frame from
    // 0.78 ms to 23.7 ms. This marcher shipped without the clip at all and cost 12.0 ms against
    // the old one's 1.6, with the tell being that the cost scaled linearly with the step budget â€”
    // 12.0 ms at 512 steps, 3.3 at 128, 1.0 at 32. A marcher whose cost is proportional to its
    // budget is a marcher that never terminates.
    float limit = min(push.lens.y, max_t);
    {
        vec3 box_min = vec3(push.bounds_min.xyz) * float(kNodeChunkEdge);
        vec3 box_max = (vec3(push.bounds_max.xyz) + 1.0) * float(kNodeChunkEdge);
        float enter = 0.0;
        float exit = push.lens.y;
        for (int axis = 0; axis < 3; ++axis) {
            if (abs(dir[axis]) < 1e-9) {
                if (origin[axis] < box_min[axis] || origin[axis] > box_max[axis]) {
                    return result;   // parallel to a slab and outside it
                }
                continue;
            }
            float t1 = (box_min[axis] - origin[axis]) / dir[axis];
            float t2 = (box_max[axis] - origin[axis]) / dir[axis];
            enter = max(enter, min(t1, t2));
            exit = min(exit, max(t1, t2));
        }
        if (enter > exit) return result;   // never enters
        t = max(t, enter);
        limit = min(limit, exit);
    }

    // This ray treats an unbuilt cell as an occluder, which is exactly the property that makes it
    // need the geometry to exist -- so it is also the ray that must say it is using it. The two
    // are one fact and are deliberately not two flags: a ray that reads ignorance as matter and
    // does not keep what it reads is the loop D429 measured.
    bool keeps_geometry = occlude_unknown && node_push.light_read_period != 0u;

    bool has_pending = false;
    ivec4 pending = ivec4(0);
    ivec3 last_normal = ivec3(0);
    int outer_level = -1;

    ivec3 cell = ivec3(0);
    vec3 t_max = vec3(1e30);
    vec3 t_delta = vec3(1e30);

    for (uint step = 0u; step < kNodeMaxSteps && t < limit; ++step) {
        result.steps = step;

        // The detail this pixel can resolve here, as a continuous number. Nothing finer than a
        // brick in the outer walk: inside a brick the ray always steps single voxels, because the
        // level decides the colour and never the shape (D132).
        // The fractional part of the level picks between the two neighbouring levels with an
        // ordered dither, so detail is a continuous function of distance with no discrete
        // transition anywhere in the maths (answer N2). Leaving it out does not merely look
        // different - it changes which pixels report full coverage and which report the node's
        // filtered fraction, so a whole surface shifts a few per cent and 43% of pixels differ
        // from the marcher this replaces.
        float footprint = max(t * pixel_angle * push.lens.z, 1.0);
        int level = clamp(int(floor(log2(footprint) + dither)), 0, kNodeMaxDetail);
        int march_level = max(level, kLeafLevel);

        if (march_level != outer_level) {
            // Re-seed the DDA at the new granularity. A handful of times per ray Ã¢â‚¬â€ once per level
            // boundary crossed Ã¢â‚¬â€ not once per step.
            outer_level = march_level;
            float size = float(1 << outer_level);
            vec3 p = origin + dir * t;
            cell = ivec3(floor(p / size));
            for (int axis = 0; axis < 3; ++axis) {
                if (abs(dir[axis]) < 1e-9) {
                    t_max[axis] = 1e30;
                    t_delta[axis] = 1e30;
                } else {
                    float boundary = (float(cell[axis]) + (step_dir[axis] > 0 ? 1.0 : 0.0)) * size;
                    t_max[axis] = (boundary - origin[axis]) / dir[axis];
                    t_delta[axis] = size * inv_dir[axis];
                }
            }
        }

        // Absolute voxel coordinate of this cell, truncated to 32 bits exactly as the CPU
        // truncates it when it writes a node record, so both sides agree about the key.
        ivec3 voxel = origin_voxel + cell * (1 << outer_level);
        Found found = node_locate(voxel, outer_level);

        // A shell reached at the level the pixel asked for is NOT an answer. The world has
        // something here and the pool does not know what it looks like yet: a shell has no
        // children to have been folded from, so its colour is nought, and drawing it paints black
        // geometry over open sky. That is why the two marchers disagreed about 83% of the pixels
        // on a camera pointed straight up, where there is no geometry to disagree about.
        //
        // The descent already handles this on the way DOWN. What was missing is the case where
        // the descent *ends* on one, which is every ray whose pixel is coarse enough to stop at a
        // level the tree has only outlined.
        bool shell = found.state == kFoundHere &&
                     (node_flags_of(nodes.items[found.slot].packed) & kNodeLeaf) == 0 &&
                     nodes.items[found.slot].children == kNoNode;
        if (shell) found.state = kFoundWanted;

        if (found.state == kFoundHere) {
            uint packed = nodes.items[found.slot].packed;
            if ((node_flags_of(packed) & kNodeLeaf) == 0) {
                // A node exists at the size this pixel resolves, and that is the answer.
                result.hit = true;
                result.t = t;
                result.normal = last_normal;
                result.colour = nodes.items[found.slot].colour;
                result.coverage = node_face_coverage(found.slot, last_normal);
                // Clamped into the range the visibility buffer's level field means. A node level
                // and a detail level are the same units - both a power of two in voxels - but the
                // field stops at seven, and a node fourteen levels up is a cell nobody is looking
                // at anyway.
                result.level = min(found.level, kNodeMaxDetail);
                node_flush(report, has_pending, pending);
                node_flush_used(report_used, g_node_block);
                node_flush_read(report_used || node_light_due(keeps_geometry, found.slot),
                                found.slot);
                node_face_hit(result, report_face, voxel, outer_level, last_normal);
                return result;
            }

            // Inside the brick, always at single voxels Ã¢â‚¬â€ deliberately, even when the pixel is
            // too small to resolve one. The level still decides the colour; what it must not
            // decide is the shape, and when it did, a cell straddling a surface stood half a cell
            // proud of it and a grazing ray clipped its side. That read on screen as faint dotted
            // lines scattered over flat ground (D132).
            uint leaf = nodes.items[found.slot].children;
            float t_inner = max(t, 0.0);
            vec3 p = origin + dir * (t_inner + 1e-4);
            ivec3 inner = ivec3(floor(p)) - cell * 8;
            inner = clamp(inner, ivec3(0), ivec3(7));

            vec3 i_max;
            vec3 i_delta;
            for (int axis = 0; axis < 3; ++axis) {
                if (abs(dir[axis]) < 1e-9) {
                    i_max[axis] = 1e30;
                    i_delta[axis] = 1e30;
                } else {
                    float boundary = float(cell[axis] * 8 + inner[axis]) +
                                     (step_dir[axis] > 0 ? 1.0 : 0.0);
                    i_max[axis] = (boundary - origin[axis]) / dir[axis];
                    i_delta[axis] = inv_dir[axis];
                }
            }

            ivec3 inner_normal = last_normal;
            if (inner_normal == ivec3(0)) {
                // Camera inside solid matter: no correct answer, so face the ray.
                vec3 a = abs(dir);
                int dominant = (a.x > a.y && a.x > a.z) ? 0 : ((a.y > a.z) ? 1 : 2);
                inner_normal[dominant] = -step_dir[dominant];
            }

            for (int inner_step = 0; inner_step < 3 * 8; ++inner_step) {
                if (any(lessThan(inner, ivec3(0))) || any(greaterThanEqual(inner, ivec3(8)))) break;
                if (leaf_voxel_solid(leaf, inner)) {
                    result.hit = true;
                    result.t = max(t_inner, 0.0);
                    result.normal = inner_normal;
                    result.level = level;
                    if (level == 0) {
                        // A single voxel is all of itself.
                        result.type_id = leaf_voxel_type(leaf, inner);
                        result.coverage = 255u;
                    } else {
                        result.colour = leaves.items[leaf].average_colour;
                        result.coverage = node_face_coverage(found.slot, inner_normal);
                    }
                    node_flush(report, has_pending, pending);
                    node_flush_used(report_used, g_node_block);
                    node_flush_read(report_used || node_light_due(keeps_geometry, found.slot),
                                    found.slot);
                    // The VOXEL the ray stopped on, at the level the PIXEL resolves -- not the
                    // brick the outer walk was stepping.
                    //
                    // The outer walk never steps finer than a brick, because the level decides
                    // the colour and must not decide the shape (D132). Reporting the face at that
                    // level made every face in the store a brick: 19,196 faces at the close
                    // camera, all of them level 3, so the smallest shadow the renderer could cast
                    // was 25 cm and flat stone came out in blocks. Debug view 11 has been keying
                    // on the voxel at the pixel's level since long before this marcher existed
                    // and counts 609,592 faces on the same view, which is the number the plan's
                    // arithmetic is written against (Â§6: a voxel covers a whole pixel at 22.5 m,
                    // so everything nearer is at level 0).
                    //
                    // `voxel` is brick-aligned and `inner` is under eight, so at level 3 and
                    // above this shifts to exactly what it shifted to before -- the refinement is
                    // in the near field and nothing else moves.
                    node_face_hit(result, report_face, voxel + inner, level, inner_normal);
                    return result;
                }
                if (i_max.x < i_max.y && i_max.x < i_max.z) {
                    t_inner = i_max.x; inner.x += step_dir.x; i_max.x += i_delta.x;
                    inner_normal = ivec3(-step_dir.x, 0, 0);
                } else if (i_max.y < i_max.z) {
                    t_inner = i_max.y; inner.y += step_dir.y; i_max.y += i_delta.y;
                    inner_normal = ivec3(0, -step_dir.y, 0);
                } else {
                    t_inner = i_max.z; inner.z += step_dir.z; i_max.z += i_delta.z;
                    inner_normal = ivec3(0, 0, -step_dir.z);
                }
                if (t_inner > limit) break;
            }

            // The ray crossed this brick and came out the other side. It READ it, and until now
            // nothing anywhere said so.
            //
            // `node_flush_read` above fires only where the march STOPS, so residency only ever
            // heard about the last brick of a path -- and every brick in front of it, which the ray
            // has to walk voxel by voxel to get past, aged out on a six-hundred-frame timer while
            // it was being read every frame. Measured on the close camera at 1280x800: 249,454
            // evictions over a settled, static run, 228,964 of them inside the frustum, 249,414 of
            // them nodes no ray had EVER reported reading, and 37,213 requested again within two
            // seconds of going -- 1,177 of those by a primary ray's own miss, which this line takes
            // to nought and which was the visible half of it. That churn is what a player
            // sees as bricks flickering to plain cubes while standing still: an evicted node reads
            // as unbuilt-but-occupied, so the marcher paints an ancestor's folded colour over the
            // whole cell (D421) and occlusion treats it as opaque, which is why the flicker casts a
            // shadow. D426, D427.
            //
            // It is one report per crossed brick per reporting ray, on the same lattice the stop
            // report already uses, so the traffic follows the same screen area it always did.
            //
            // A LIGHT ray deliberately says nothing here, and that is D292 rather than an omission.
            // The rule it narrows is "a light path may name the one cell that STOPPED it, and
            // nothing it merely crossed"; keeping what it crosses is the other half of the same
            // sentence, and dragging residency along every shadow ray is what that rule exists to
            // prevent. Measured both ways: stamping crossings too takes settled churn from 4,606 to
            // 1,028, and costs the faces pass 7.88 -> 8.25 ms while flying and chiselling, which is
            // outside its spread where stop-only is inside it. Not carried. D430.
            node_flush_read(report_used && node_push.report_crossings != 0u, found.slot);
        } else {
            if (found.state == kFoundWanted) {
                // The world has something here and the pool does not. Report the node at the
                // level the descent stopped at, which is the coarsest thing that is missing Ã¢â‚¬â€ so
                // streaming fills in from near to far rather than from the leaves up.
                // Report the node at the level this pixel actually WANTS, not the coarsest one
                // that happens to be missing. `NodePool::refine` walks the whole path in one
                // call, so asking for the coarsest gap throws away ten of the eleven levels it
                // would have built anyway and makes a path take eleven frames instead of one.
                int missing = max(outer_level, kLeafLevel);
                node_note(has_pending, pending,
                          ivec3(voxel.x >> missing, voxel.y >> missing, voxel.z >> missing),
                          missing);

                // Draw the parent rather than nothing (R2d).
                //
                // Falling through to the skip below treats a region the world HAS as empty
                // space, so the ray flies through a building that has not streamed yet and
                // draws sky. The old renderer never did this: it pushed a coarse thumbnail
                // from the camera, so there was always something to show. This is why the
                // node pool reads as slow to load when it is faster to march.
                //
                // Only when the missing cell is the size this pixel resolves, which is D151
                // and is not a tuning choice. The descent returns WANTED at a level ABOVE the
                // one it was asked for, so the cell standing in is never finer than the pixel
                // and is only ever equal to it here. Allowing coarser is the fault D151
                // records: a two-kilometre block containing ground a mile away has its near
                // face a few metres from the camera, and drawing it there puts a blob in your
                // face. The parent's folded colour and its per-direction coverage are exactly
                // what a coarse hit is supposed to be, and both already exist on the node.
                // ...and only from a node that HAS a folded colour.
                //
                // At WANTED the node reached can itself be a shell - the world says it has
                // children and the pool has not built them - and a shell has never been folded
                // from anything, so its colour is nought. Standing in with that paints the
                // building black, which is precisely what the check twenty lines above refuses
                // to do on the way down and what this managed to reintroduce on the way out.
                // Reported from a screenshot: the facility in silhouette, every column solid
                // black, against a correct sky.
                // Occlusion stops here, with no colour and no conditions. Everything below this
                // is about what to DRAW, and a ray that draws nothing has no use for any of it.
                if (occlude_unknown) {
                    // Report the cell that stopped it, whatever `report` says. This is R9i's
                    // second half and the narrowing D292 has always needed.
                    //
                    // D292 forbids a shadow ray from dragging residency towards whatever it
                    // crosses, and that rule is right: a ray that reported everything it passed
                    // through would ask for the world behind every surface. But it is one node
                    // here, not a path -- the single cell the ray was STOPPED by -- and without
                    // it the pool never learns that anything needs building there, so the cell
                    // stays unbuilt, stays opaque, and casts a shadow for ever.
                    //
                    // Measured before this: on the close camera the same step reads fully lit on
                    // its left and fully shadowed on its right, with nothing between the two to
                    // cast anything. The boundary is the edge of what the camera has caused to be
                    // built. 18,820 faces shadowed by ignorance with no edit at all, and 54,933
                    // after deleting the roof off the building -- which is the reported bug: the
                    // shadow of a thing that is not there any more.
                    //
                    // It costs one entry per shadow ray that stops on an unbuilt cell, and that
                    // number falls to nothing as the pool fills, exactly as a miss report does.
                    if (has_pending) {
                        uint index = atomicAdd(feedback.count, 1u);
                        if (index < push.resolution.w) {
                            feedback.entries[index].coord =
                                ivec4(pending.xyz, pending.w | kFeedbackExact);
                        }
                    }
                    result.hit = true;
                    result.unknown = true;
                    result.t = t;
                    result.normal = last_normal;
                    result.level = min(outer_level, kNodeMaxDetail);
                    result.coverage = 255u;
                    return result;
                }

                bool foldable = (node_flags_of(nodes.items[found.slot].packed) & kNodeLeaf) != 0 ||
                                nodes.items[found.slot].children != kNoNode;
                if (stand_in && found.level - 1 == outer_level && found.slot != kNoNode &&
                    foldable && (nodes.items[found.slot].colour >> 24) != 0u) {
                    ivec3 stand_normal = last_normal;
                    if (stand_normal == ivec3(0)) {
                        // First step, camera inside the cell: no face was crossed to get here,
                        // so face the ray, exactly as the leaf march does.
                        vec3 a = abs(dir);
                        int dominant = (a.x > a.y && a.x > a.z) ? 0 : ((a.y > a.z) ? 1 : 2);
                        stand_normal = ivec3(0);
                        stand_normal[dominant] = -step_dir[dominant];
                    }
                    result.hit = true;
                    result.t = t;
                    result.normal = stand_normal;
                    result.colour = nodes.items[found.slot].colour;
                    result.coverage = node_face_coverage(found.slot, stand_normal);
                    result.level = min(outer_level, kNodeMaxDetail);
                    node_flush(report, has_pending, pending);
                    node_flush_used(report_used, g_node_block);
                    node_flush_read(report_used, found.slot);
                    node_face_hit(result, report_face, voxel, outer_level, stand_normal);
                    return result;
                }
            }

            // Nothing here. How far the ray may jump is what the descent already worked out: the
            // empty cell is one level below where it stopped.
            //
            // The jump is only worth taking when it is BIGGER than the step the DDA was going to
            // take anyway. Near geometry the descent usually reaches the marching level, so the
            // empty cell is exactly one marching cell Ã¢â‚¬â€ and taking the analytic path for that
            // recomputes three boundary intersections, re-seeds the whole DDA and nudges `t`,
            // to move exactly as far as one ordinary step would have. That was most steps of
            // most rays, and it is why this marcher was seven times slower than the one it
            // replaces while doing strictly less work per lookup.
            int skip_level = found.level - 1;
            if (skip_level > outer_level) {
                float skip_size = float(1 << skip_level);
                ivec3 block = ivec3(voxel.x >> skip_level, voxel.y >> skip_level,
                                    voxel.z >> skip_level);
                vec3 block_min = vec3(block * (1 << skip_level) - origin_voxel);
                vec3 block_max = block_min + skip_size;

                float exit_t = 1e30;
                int exit_axis = 1;
                for (int axis = 0; axis < 3; ++axis) {
                    if (abs(dir[axis]) < 1e-9) continue;
                    float far_boundary = (dir[axis] > 0.0) ? block_max[axis] : block_min[axis];
                    float axis_t = (far_boundary - origin[axis]) / dir[axis];
                    if (axis_t < exit_t) { exit_t = axis_t; exit_axis = axis; }
                }
                last_normal = ivec3(0);
                last_normal[exit_axis] = -step_dir[exit_axis];

                // The nudge past the boundary scales with distance, or it stops being a nudge at
                // all. A 32-bit float's own step at t = 96,000 voxels is 0.0156, so `t + 1e-3`
                // rounds straight back to t and the ray spins on the same boundary until its
                // budget runs out (D156).
                t = max(exit_t, t) + max(1e-3, t * 1e-5);
                outer_level = -1;   // force a DDA re-seed at the new position
                continue;
            }
            // Otherwise fall through: one ordinary DDA step is exactly as far, and costs a
            // compare rather than a re-seed.
        }

        if (t_max.x < t_max.y && t_max.x < t_max.z) {
            t = t_max.x; cell.x += step_dir.x; t_max.x += t_delta.x;
            last_normal = ivec3(-step_dir.x, 0, 0);
        } else if (t_max.y < t_max.z) {
            t = t_max.y; cell.y += step_dir.y; t_max.y += t_delta.y;
            last_normal = ivec3(0, -step_dir.y, 0);
        } else {
            t = t_max.z; cell.z += step_dir.z; t_max.z += t_delta.z;
            last_normal = ivec3(0, 0, -step_dir.z);
        }
    }

    node_flush(report, has_pending, pending);
    return result;
}


