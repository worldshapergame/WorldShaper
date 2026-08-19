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

// What a face gives off, and what its lobe is shaped like. Declared here rather than only by the
// two shaders that read a face's record, because this file owns the BUFFERS a lobe lives in and the
// sizes of the arithmetic are what the buffer is laid out from -- so the file that says how many
// bins there are and the file that reserves room for them have to be the same pair every time.
// Guarded, so a shader that includes both in either order gets one copy.
#include "face_terms.glsl"

// ---- R12c: the field evaluator, so a ray can answer for a cell nobody has built -----------------
//
// The three files R12b built, included unchanged. They are somebody else's files and this one does
// not get to edit them, which is the whole reason for the four lines of preprocessor below rather
// than a copy of the evaluator -- and a second evaluator is D200k's fault in its worst form.
//
// **The remap, and it is the only obscure thing in this change.** `field_types.glsl` declares its
// seven buffers at `set = 0, binding = 0..6` and its constants as a PUSH CONSTANT, and this pass
// has neither of those free: bindings 0 and 1 of set 0 are the visibility pass's output images,
// 2..25 are the node pool's own, and a stage is allowed exactly one push-constant block, which
// `NodeConstants` below already is. So the field is moved onto descriptor SET 1 without touching
// the file that declares it:
//
//   `binding` expands to `set = 1, binding`, so `layout(std430, set = 0, binding = 3)` becomes
//   `layout(std430, set = 0, set = 1, binding = 3)`. A repeated layout-qualifier-name takes its
//   LAST occurrence (GLSL 4.60 §4.4), so the set becomes 1 and the binding is untouched. The
//   injected qualifier has to be a COMPLETE one -- `set = 1` and not `binding = 27 +` -- because
//   the `= 3` in the source still belongs to whatever token the macro ends on.
//
//   `push_constant` expands to `std140, binding = 7`, which the line above then turns into
//   `std140, set = 1, binding = 7`: the same block, as a uniform buffer at set 1 binding 7.
//
// Both are `#undef`ed immediately, so every binding this file declares for itself is its own.
// Verified against glslc rather than reasoned about: set 1 bindings 0..7 out, set 0 untouched.
#define binding set = 1, binding
#define push_constant std140, binding = 7
#include "field_types.glsl"
#include "field_leaf.glsl"
#include "field_walk.glsl"
#undef binding
#undef push_constant

// What one dispatch derived, counted where it happens. Mirrors `DeriveStats` in
// src/gpu/node_buffers.hpp, one per frame in flight; `push.derive.z` says which is this frame's.
//
// It is the cap as well as the instrument: D687 priced this stage at 1,239 ms a frame at the
// estate's field cost, so a marcher that derives without a ceiling is a frame that never ends. The
// ticket `derived` hands out IS the budget.
struct DeriveStatsSlot {
    uint derived;
    uint capped;
    uint evaluations;
    // ...and the nodes of the FIELD those evaluations walked, in units of 1024 so a dispatch cannot
    // overflow the word. This is the second budget and it is the one that actually bounds a frame:
    // a cap on CELLS is not a cap on WORK. D688 measured a cell under the podium at 1,073,935 nodes
    // and 372 ms where a typical one walks 5,195 and takes 1.8, so twenty thousand cells is
    // anywhere between forty milliseconds and two hours. Measured here: a cap of 20,000 cells and
    // no visit budget LOST THE DEVICE on a 150-second run, and 300,000 lost it inside ten seconds.
    uint visits_k;
};
layout(std430, set = 1, binding = 8) buffer DeriveStats { DeriveStatsSlot slots[]; } derive_stats;

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
    // How often a face a PIXEL read may say so down the feedback buffer, in frames, as a power of
    // two. 0 is off and restores residency hearing only from the request lattice.
    //
    // A period rather than a switch because it is the whole trade: shorter is a residency clock that
    // tracks the screen more closely and more feedback entries, and the entries are the constrained
    // thing. See node_face_read_due.
    uint face_read_period;
    // How often a face may name ONE face a light ray of its own landed on, in frames, as a power of
    // two. 0 is off and is R9a's control arm.
    //
    // A period rather than a switch because it is the whole trade this rule makes: shorter is an
    // off-screen set that fills in faster and more feedback entries, and the entries are the
    // constrained thing -- D431 is the standing measurement of what happens when a per-ray report
    // goes down this buffer unthrottled (1,538,219 reports against a capacity of 131,072, and the
    // 1,407,147 that were dropped took the streaming reports with them).
    uint secondary_period;
    // 1: a gathering ray reads the surface it lands on and the composite adds what it found. 0 is
    // R9's bounce control arm — and it is NOT "the renderer as it was", because the ambient floor
    // that used to stand in for this light is gone rather than switched off. With the bounce off, a
    // surface that receives nothing is black, which is the point.
    uint bounce;
    // The least far samples a face takes before its bounce may stop, or 0 for kBounceMin. The trade
    // is unbounded rays against a per-face mottle in every interior; see kBounceMin.
    uint bounce_min;
    // How many samples the bounce remembers, or 0 for the cumulative mean this replaced, which is
    // the control arm. See kBounceMemory.
    //
    // It sits in the four bytes that used to be `pad_before_sun_colour`, and that is deliberate
    // rather than thrifty: a vec4 here is aligned to sixteen bytes and its counterpart in the host
    // struct is aligned to four, so a padding word has to be declared or the two blocks are the same
    // fields at different offsets (`--validation` caught that once and nothing else would have).
    // Spending the pad on a u32 keeps the alignment true BY CONSTRUCTION -- a field of the same size
    // cannot move what follows it -- rather than by a second thing to remember. See NodePush in
    // src/app/main.cpp.
    uint bounce_memory;
    // The sun's colour at the reference hour, which is the ONE thing pt_sky.glsl needs that the
    // marcher did not already carry. With it and `sun` above, the face pass evaluates exactly the
    // sky the composite draws rather than a second plausible one -- which matters because a
    // gathering ray that escapes reads the sky, and if this pass and the composite disagreed about
    // what the sky is worth, indirect light would be lit by one sky and the picture by another.
    //
    // Sixteen bytes, taking this block to 116 of the 128 a Vulkan implementation is required to
    // offer. Anything else that wants space here has to earn it.
    vec4 sun_colour;
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

// When a face was last REPORTED to the host as read by a pixel. One word a slot, card-owned, and
// exactly `node_seen` doing for the face store what it already does for the node pool.
//
// # Why residency needed a second signal at all
//
// `FaceStore::last_read_` is stamped by a CLAIM, and a claim comes from the one pixel in stride^2
// the request lattice is asking with this frame. That is the store's whole idea of "somebody is
// looking at this", and it is wrong in the direction that costs: a face whose coverage is under a
// pixel is plainly on screen and is sampled by that lattice rarely, so the only safe cold window is
// a very long one — which is why the store kept ten seconds of everything the camera walked past,
// filled, and started refusing faces (D502). `face_seen` above is the exact signal, written for
// every face every pixel resolves to, every frame; the host simply had no way to hear it.
//
// # Why this is a second array and not a read of that one
//
// They answer different questions. `face_seen` must say "was this read RECENTLY", so it is stamped
// every frame; this must say "has this been REPORTED lately", which is a much longer clock. One word
// cannot hold both, and packing them into halves would put a sixteen-bit frame counter in the middle
// of a comparison that already has to be right about wrap-around.
//
// # Why the phase gate here is not the mistake D432 records
//
// D432 took out a slot-hash pre-gate in front of `node_seen`, because the thing doing the reporting
// there is a LIGHT ray and a converged face casts none — so a node was reported only if a ray
// happened to read it on the one frame it was eligible, which for most nodes is never. The reporter
// here is the PRIMARY ray, which runs for every visible pixel on every frame without exception, so a
// face on screen is guaranteed to be visited on its eligible frame. Same shape, opposite guarantee,
// and the phase is what keeps the first frames of a run from reporting half a million faces at once
// into a buffer that holds 131,072.
layout(std430, binding = 18) buffer FaceRead { uint frames[]; } face_read;

// When a GATHERING ray last read this face, one word a slot, card-owned exactly as face_seen is.
//
// # Why the off-screen set needed this before it was worth anything
//
// R9a claims a face for the surface a bounce ray landed on, so the store holds geometry no pixel is
// looking at. It then held it and did nothing with it: `may_cast` is `node_face_recently_seen`, and
// that stamp is written by the VISIBILITY pass, which only ever runs on pixels. Measured on the
// close camera, settled: **229,413 off-screen faces of a cap of 262,144, every one of them at nought
// sun samples and nought with a finished ambient term** -- a quarter of the table holding empty
// records. The gathering rays that asked for them then read those empty records back: 12.4% of every
// gathering ray in the frame landed on a face that is in the store and has measured nothing.
//
// So the set existed and the light did not, and the next thing the plan called for -- R9c, claiming
// a margin of faces just off the edge of the screen -- would have moved rays from "no face at all"
// (21.2%) into "a face with nothing measured yet" and changed no pixel anywhere.
//
// # Why a second array rather than stamping face_seen
//
// Because the two must be COUNTED apart, not merely told apart. `face_seen`'s population is what the
// sun's ray budget is divided by, and D527 and D557 are the same fault twice: anything that adds
// faces to that denominator refreshes every face on screen less often, the pass gets CHEAPER, and
// the cost lands in a convergence number no pass table carries. An off-screen face must get its own
// rate out of its own budget and take nothing from the screen's, which is R9b's whole sentence, and
// a shared stamp cannot express it.
//
// It is also the exact signal, in the same way `face_seen` is for the pixel and `node_seen` is for
// the light's geometry (D414, D431): a face is worth a ray when something is INTEGRATING it, and the
// thing that integrates an off-screen face is the gathering ray that landed on it. A face nothing
// gathers from is stamped by nobody and casts nothing, so the ten seconds of history the store keeps
// behind a moving camera stays as dark and as free as D414 made it.
//
// Card-owned, and it must stay that way for D528's reason: the host writes whole `GpuFace` records
// for dirty slots, so a class kept in the record would send the host's zeroed counters over the light
// the card accumulated.
layout(std430, binding = 20) buffer FaceGathered { uint frames[]; } face_gathered;

// ---- what a face is MADE of (R4a) --------------------------------------------------------------
//
// One word a face slot, card-owned exactly as the three stamps above it are, and for the same
// reason: the host sends whole `GpuFace` records for the slots the store marks dirty, so anything
// the card works out for itself and keeps in that record is sent the host's copy over the top of
// it. That is D295 and D528, and `GpuFace::bins` -- the field the plan reserved for this -- stays
// nought precisely because it is on the host's side of that line.
//
// # Why the card has to work this out rather than be told
//
// A face is (node, level, direction) and nothing else. The store has never known what the surface
// under it is made of and neither has the light pass: `bounce_face_light` reads the folded average
// colour the MARCHER carries, and the comment there says why -- this pass has no binding for the
// interned tables. An albedo is all a Lambertian face ever needed.
//
// A lobe needs more. How wide a reflection is, and whether a surface has one at all, is roughness
// and metalness, and those live in the visual record and nowhere else. So the two tables are bound
// here now (22 and 23) and a face resolves its own material ONCE, on a frame it was already doing
// something, and keeps the answer. What that costs is one descent per face per lifetime, down the
// same tree the ray it is about to cast is going to walk anyway.
//
// # The word
//
//   0-7    roughness, as the visual record stores it
//   8-15   metalness, likewise
//   16-23  VisualFlags
//   30     NONE -- this face has looked and there is no one material to be had
//   31     KNOWN -- this face has looked and bytes 0-2 are it
//
// Three states and not two, and the third is the one trap 7 and trap 10 are both about. Nought is
// "has not looked", which a face over a brick the pool has not built must keep answering until the
// brick arrives. KNOWN is a material. NONE is a COARSE face -- a node standing over as many as 512
// voxels which need not agree about anything, so it has a folded colour and no roughness, and
// inventing one for it is exactly the fold R9f has not built.
//
// Encoding NONE as "known, roughness nought" was the first shape of this and it is wrong in the way
// that costs: roughness nought is a mirror, so every coarse face in the store would read as polished
// chrome in the census and in the debug view. Two different answers sharing one colour is what
// produced a wrong number nobody questioned in D310.
layout(std430, binding = 21) buffer FaceMaterial { uint words[]; } face_material;

// The two interned tables, which until R4 no pass on this set had any reason to read. The composite
// has had them at its own bindings 2 and 3 since before the face store existed; these are the same
// two buffers, and there is exactly one copy of each on the card.
layout(std430, binding = 22) readonly buffer Types { uvec2 items[]; } types;
layout(std430, binding = 23) readonly buffer Visuals { uvec4 items[]; } visuals;

const uint kFaceMaterialKnown = 1u << 31;
const uint kFaceMaterialNone = 1u << 30;

uint face_material_rough(uint word) { return word & 0xFFu; }
uint face_material_metal(uint word) { return (word >> 8u) & 0xFFu; }
uint face_material_flags(uint word) { return (word >> 16u) & 0xFFu; }

// ---- the outgoing bins, and the pool they come out of (R4c) ------------------------------------
//
// What a face reflects along each of sixteen directions, which is the payload §4 of the plan
// reserved and R4a's first half could not fill. The arithmetic is in `shaders/face_terms.glsl`
// because the pass that WRITES a bin and the pass that READS one are different shaders; what is
// here is where it lives.
//
// # Why this is a pool and not a word a slot, which everything else on a face is
//
// Every other card-owned array here is one word for every slot the face pass may write: 1,081,344
// of them, 4.2 MB apiece. A lobe is 34 words, so the same arrangement would be **147 MB** — more
// than the whole renderer — to hold a number that is nought on nine faces in ten. The plan says so
// in its own words: *a matte stone wall allocates no payload at all*. Measured on this building,
// 35,950 faces of 801,175 carry any metal and 22,158 of the 416,143 a pixel is reading do.
//
// # The shape, which is a cache and not an allocator
//
// A free list needs a free, and a face has no destructor: the store hands a slot to a different
// face and nothing tells the card. So blocks are not owned, they are HELD — each records the slot
// holding it and the frame that slot last touched it, and a face wanting one takes any way of its
// set that is empty, cold, or held by a face worth less than it is. There is nothing to leak,
// because a block whose holder has stopped being shaded goes cold and is taken, exactly as the
// face store gives up a slot nothing has read for `cold_frames`.
//
// The headers are a contiguous array of their own rather than the first words of each block, so a
// four-way probe reads four adjacent pairs -- one cache line -- instead of four words 136 bytes
// apart. The reader does this per PIXEL on a metal surface, which is the one place in R4c where
// the cost is a function of the window.
//
//   words [0, 2 * kLobeBlocks)          two a block: the slot holding it, then the frame it was
//                                       last touched in the low sixteen bits and how much its
//                                       holder is worth, as a byte, in bits 16-23
//   words [2 * kLobeBlocks, ...)        kLobeBlockWords a block: kLobeBins packed rgb9e5 means,
//                                       then kLobeBins float weight sums behind them
//
// The means come first as a run so that the composite's four bilinear taps are four words inside
// 64 bytes; the weights are only read by the pass that writes them.
layout(std430, binding = 24) buffer FaceLobe { uint words[]; } face_lobe;

// ---- and what a face lets THROUGH, which nothing on the card has ever known (R4d) --------------
//
// One word a face slot, card-owned, filled beside the material and for the same reason: a face is
// (node, level, direction), and the two numbers that say a surface is glass rather than stone live
// in the visual record and nowhere else.
//
// R4a brought roughness and metalness across and that was enough for a lobe. It is not enough for
// **transmission**, which is the last thing on this building that is drawn plainly wrong: the
// window glass is `opacity=64 ior=1.5` and the two basins are `opacity=110 ior=1.33`, and the
// renderer paints both as opaque Lambertian slabs. No amount of reflection makes that read as glass
// or as water, which is what D596 measured and could not fix.
//
// # The word
//
//   0-7    opacity, as the visual record stores it. 255 is opaque and is nine faces in ten
//   8-15   the index of refraction, as the record's offset from vacuum: 1.0 + ior/128
//   16-23  translucency, which is the other way light gets through a surface -- scattered rather
//          than refracted, which is what marble and a leaf do
//   31     KNOWN, the same third state `face_material` carries and for the same reason: nought is
//          "has not looked" and must never become sticky (trap 7)
//
// It is a SECOND word rather than six spare bits of the first because opacity and the index need
// sixteen between them and the first word has six left. A word a slot is 4.2 MB, which is what
// every other card-owned array here costs, and the alternative is repacking a field that four
// shaders already agree about.
layout(std430, binding = 25) buffer FaceMedium { uint words[]; } face_medium;

const uint kFaceMediumKnown = 1u << 31;
uint face_medium_opacity(uint word) { return word & 0xFFu; }
uint face_medium_ior(uint word) { return (word >> 8u) & 0xFFu; }
uint face_medium_translucency(uint word) { return (word >> 16u) & 0xFFu; }

// How deep light gets into this material before it is spent, in VOXELS. R4e.
//
// The byte the author writes is a depth and not a fraction, which is what makes a row of panels at
// five thicknesses read as five different things out of one number. Four voxels at the top of the
// range: `clips/translucency_test.clip` says marble "stops light in about a voxel" and writes 110
// for it, which lands at 1.7 -- so a four-voxel lip passes about a tenth and the solid body behind
// it passes nothing, which is the sentence that clip exists to make true. Alabaster at 220 reaches
// 3.5 voxels and its four panels, from 1.3 to 12.8 voxels thick, come out at roughly 69%, 53%, 23%
// and 2.5%. A material that never wrote the byte gets nought and no ray at all.
const float kTranslucentDepthVoxels = 4.0;
float face_medium_depth(uint word) {
    return float(face_medium_translucency(word)) * (1.0 / 255.0) * kTranslucentDepthVoxels;
}
// How much of what arrives goes THROUGH, in [0, 1]. The one number the composite needs before it
// can stop drawing a pane of glass as a wall.
float face_medium_transmits(uint word) {
    if ((word & kFaceMediumKnown) == 0u) return 0.0;
    return 1.0 - float(face_medium_opacity(word)) * (1.0 / 255.0);
}

// ---- what a gathering ray found, counted ------------------------------------------------------
//
// The light pass has three audit lines about what it has FINISHED (`ambient on the card`, `lamps on
// the card`, `faces sun on the card`) and none at all about what its rays are LANDING on. So the
// one question R9 is about -- how often a ray that gathers light finds a surface with no light on
// it -- had no number anywhere, and the only way to ask it was to look at a picture and guess.
//
// One word a question, over the whole dispatch, cleared by the host every frame and read back at the
// screenshot audit. It is not per face and not per slot: the quantity wanted is a RATE over the
// frame, and a rate is exactly what a per-face record cannot hold (trap 17 -- one number covering
// three things, and here it is the reverse: three hundred thousand records covering no number).
//
// # Ownership, which this buffer has two of and must not confuse
//
// Word 0 is the HOST's: the dials, written with vkCmdUpdateBuffer every frame and never touched
// here. Words 1 and up are the CARD's: counters, cleared by the host's fill and then written only
// by atomicAdd from this pass. The two are written by separate commands rather than by one fill, so
// "the host owns this word" is a property of the code rather than a rule somebody has to remember
// -- which is D528's whole lesson, arriving in an instrument for once instead of in a record.
layout(std430, binding = 19) buffer LightProbe { uint words[]; } light_probe;

// The word map. Twenty-four words, and it lives here because the pass that writes it and the audit
// that prints it are in different languages and only one of them can be the authority.
//
//   [0]  the DIALS, host-written. See kProbeOn below.
//   [1]  gathering rays cast: the unbounded ambient ray, on the frames it is cast and asks what it
//        found. The denominator for everything under it.
//   [2]  ...that reached open sky, which is the one case that was always answerable.
//   [3]  ...that landed on a surface whose OWN face had light to give.
//   [4]  ...that landed on a surface with no face in the store at all.
//   [5]  ...that landed on a face that is in the store and has measured nothing yet.
//   [6]  ...of [4] and [5], how many the coarse face standing over them could answer. This is the
//        number R9f is worth, measurable before R9f exists, which is why it is here.
//   [7]  ...that were stopped by a cell the world claims and the pool has not built.
//   [8]  faces the OFF-SCREEN class shaded this frame -- R9b's budget, counted where it is spent
//        rather than predicted from a population and a stride on the host. It took this word over
//        from a question that turned out to be unanswerable from here ("of [7], how many a coarse
//        face could answer": an ignorance stop carries NO face key, because `node_face_hit` runs at
//        the leaf hit and nowhere else, which is the gate on any rule that would read light where
//        the pool has not built).
//   [9..23] where [7] happened, by LEVEL. R10's open item -- 6% of surface held short of
//        convergence by unbuilt geometry -- has never had this and cannot be reasoned about
//        without it: a ray stopped by an unbuilt brick has lost 25 cm of sky and one stopped by a
//        shed 512 m subtree has lost a quarter of the county.
//   [24] R4c: faces that asked the lobe pool for a block and are holding one.
//   [25] ...that asked and were turned away, because every way of their set was held by a face the
//        pool thinks is worth more or is not cold yet. A DECLINE is not a fault and not a black
//        surface: the face falls back to the hemispherical mean it already stores, which is a lobe
//        with one bin in it. The two are counted apart for D502's reason, one class along.
//   [26] ...and how many took a block over from another face this frame, which is the number that
//        says whether the pool is thrashing. A pool that is churning and a pool that is full look
//        identical in the count above.
//   [27] R4b: rays the LOBES cast for themselves this frame, which are unbounded and are the
//        expensive kind. They are counted in word [1] as well, because they gather in exactly the
//        sense that word means -- what is here is how many of that total were a lobe's rather than
//        the ambient term's, so neither rate has to be inferred from the other.
//   [28] ...and how many faces were still bursting for their lobe, which is the convergence figure
//        that has to be printed beside any timing of this pass (trap 20).
//   [29] R4b: how many of the faces holding a block are holding the EXPENSIVE class, which is four
//        blocks and a hundred and forty-four bins. Read against [24] -- what it says is how much of
//        the pool the sharp surfaces are taking.
//   [30] R5b: faces under `kFaceSunBelieve` samples that were handed a stand-in to be believed
//        against this frame -- the ramp's own convergence figure.
//   [31] ...and how many of them found NOTHING up the tree worth taking, so were left believing
//        their own one or two rays outright. **Two words and not one**, and the reason is that this
//        change was built, measured, and read as doing nothing at all: every differing pixel
//        between the two arms was a full flip between black and white, which is what the ramp
//        NEVER produces and is exactly what a re-rolled coin toss looks like. With one counter,
//        "no face wanted a stand-in" and "every face wanted one and `kSunSeedMin` refused it" print
//        the same nought -- trap 7, and trap 16's rule that a suspect signal is settled by counting
//        the events that produced it rather than by measuring the signal again.
//   [32] the OFF-SCREEN SET's stride, host-written, in frames. See kProbeSecondaryStride.
//   [33] R9c's HALO MARGIN, host-written, in pixels each side. 0 is off and is the whole control
//        arm -- the dispatch is then exactly the screen and this stage does not exist.
//   [34] R9c's halo STRIDE: one halo sample in this many pixels each way. See kProbeHaloStride.
//   [35] R5b's SUN SEED, host-written: how many samples of the coarse face above it a newly
//        claimed face starts its sun term from. 0 is the control arm and is every build before it.
//        Here rather than in the push block because that block is exactly the 128 bytes Vulkan
//        guarantees, and the three words above it are the standing precedent for a host-written
//        number that will not fit -- D553's warning is against giving an existing field a second
//        meaning, not against this.
const uint kLightProbeWords = 36u;
const uint kLightProbeLevels = 9u;    // where the by-level histogram starts
const uint kProbeLobeHeld = 24u;
const uint kProbeLobeDeclined = 25u;
const uint kProbeLobeTaken = 26u;
const uint kProbeLobeRays = 27u;
const uint kProbeLobeBursting = 28u;
const uint kProbeLobeBig = 29u;

// The dials, in word 0. A bit each, because the push block is exactly full (128 bytes, and the
// static_assert on NodePush says so) and a lever that cannot fit in it must live somewhere -- and
// somewhere is not "change what an existing field means", which is D553's standing warning.
const uint kProbeOn = 1u << 0;          // count at all
const uint kProbeCoarseBounce = 1u << 1;  // a gathering ray may read the coarse face over its hit
// R5a. Off, `face_denoise` writes this face's own answer into the filtered words instead of its
// neighbourhood's -- so the composite reads the same words in both arms and the A/B measures the
// FILTER rather than a branch in the reader. That is deliberate and it is the reason the control arm
// still pays the write: what is being priced is the eight neighbour lookups, which is the only part
// of this that costs anything.
const uint kProbeDenoise = 1u << 2;
// R4a. Off, no face ever asks what it is made of -- so the descent, the two table reads and the
// scattered load that finds out are all gone, and the arm is the renderer exactly as it was before
// this stage. It exists because that cost is the only cost R4a has, it is a load per working face
// per visit of the kind D406 measured at 0.38 ms, and a stage whose whole content is a fact reaching
// a buffer has otherwise no way at all to be priced: the picture is identical in both arms by
// construction, so a timing is the only evidence there is and a timing needs two arms of ONE build
// (D407).
const uint kProbeMaterial = 1u << 3;
// R9f's fold. Off, a coarse face measures itself with its own rays at its own scale and nothing
// reads its children -- which is what it has always done and is the state every figure before this
// was taken in. See `face_fold` in shade_faces.comp.
const uint kProbeFold = 1u << 4;
// R4c. Off, no face keeps outgoing bins and no pixel reads one -- so a metal is drawn by the
// diffuse arithmetic alone with its lobe standing in as the hemispherical mean, which is the
// picture this renderer has drawn since it existed. It is the control arm for the whole of R4c's
// storage half: the sun's highlight is arithmetic and stays either way, so the two arms differ by
// exactly the bins and by nothing else.
const uint kProbeLobe = 1u << 5;
// R4b's ray. Off, a lobe is filled only by the far ray that was being cast anyway -- so the bins
// hold what a cosine-weighted distribution can tell them, which is the state D592 measured and
// found could not carry a reflection. It is the control arm for the RAY on its own: the bins, the
// pool and the energy split are all identical in both arms, so what an A/B prices is the march.
const uint kProbeLobeRay = 1u << 6;
// R4b's coverage rule. Off, every lobe is the cheap thirty-six-bin class however polished the
// surface and however much of the screen it fills -- which is what R4b landed with, and is the arm
// that prices the expensive class on its own.
const uint kProbeLobeCoverage = 1u << 7;
// R4d's first half: a LIGHT ray carries on through transmissive matter instead of stopping dead on
// it. Off, a window blocks the sun, the sky and every lamp exactly as a wall does, which is what
// this renderer has always done and is the state every figure before this was taken in.
// `--no-see-through` clears it.
const uint kProbeSeeThrough = 1u << 8;
// R4d's second half: the PRIMARY ray bends where it crosses into glass or water and bends back
// where it comes out, instead of carrying straight on. Off, what is behind a pane is drawn exactly
// where it would be with no pane there, which is what D604 built and is a window with no glass in
// it. `--no-refraction` clears it.
const uint kProbeRefract = 1u << 9;
// EVERY medium on the way out, and not just the first one. Off, a ray bends at the pane it lands
// on, takes that pane's Beer-Lambert colour over that pane's path, and then goes STRAIGHT through
// everything behind it picking up each voxel's opacity tint and nothing else -- so a second sheet
// of green glass added no green, and a stack seen at an angle displaced what was behind it exactly
// as much as one sheet did. Off is what this renderer drew until now and is bit-identical to it by
// construction, because the loop's last turn is the march the old code cast. `--no-refract-stack`
// clears it.
const uint kProbeRefractStack = 1u << 14;
// R4e: a face whose material is translucent and whose sun is BEHIND it casts the sun ray it would
// otherwise have skipped, through the matter behind it, and is lit by what gets through. Off, a
// marble panel with the sun on the far side is as dark as granite -- which is what this renderer has
// always drawn. `--no-translucency` clears it.
const uint kProbeTranslucent = 1u << 10;
// R5d: a primary ray that stops on a COARSE node only part of which is matter marches on past it,
// and the composite draws the two surfaces in proportion. Off, every hit is fully opaque and a node
// larger than a voxel is a solid block whatever is really inside it -- which is what this renderer
// has always done, and is why a distant railing is a stair-stepped bar that crawls as the camera
// moves. `--no-edge-aa` clears it.
const uint kProbeEdgeAA = 1u << 11;
// R5c's first half: a hit blends its folded colour towards the folded colour of the level the
// ordered dither did NOT pick for this pixel, by how far between the two the pixel's footprint sits.
// Off, a hit draws the colour of the cell it stopped on outright, so two neighbours a level apart
// differ by the whole step between two folded averages in a fixed 4x4 pattern -- which is what this
// renderer has drawn since the marcher existed and is the state every figure before this was taken
// in. `--no-level-blend` clears it. The GEOMETRY is identical in both arms by construction: the
// dither still picks the cell, so the coverage byte, the face key and the depth do not move and the
// two arms differ by colour alone.
const uint kProbeLevelBlend = 1u << 12;
// R5b: a face that has cast fewer than `kFaceSunBelieve` shadow rays is drawn part way towards the
// coarse face standing over it, in proportion to how many it has. Off, its own ratio is believed the
// moment it has one sample — which is every build up to this one, and is what a player sees as
// speckle over everything the camera has just revealed. `--no-sun-confidence` clears it.
//
// The dial is read where the stand-in is WRITTEN and never where it is read, which is the same
// arrangement kProbeDenoise sets out and is not a preference: the composite has no binding for this
// buffer, so a control arm spelled in the reader is not available to it at all. Off, the shading
// pass leaves word `kFaceSunStandIn` at nought, both readers take the branch that returns the face's
// own ratio unchanged, and the two arms differ by the ramp rather than by a branch in the reader.
const uint kProbeSunConfidence = 1u << 13;

// How often a face nobody is looking at may cast, in frames. One frame in this many, phased on the
// slot so the off-screen set does not all come due together -- D431's fault, in the pass rather than
// in the feedback buffer.
//
// A value and not a bit, so it needs a word of its own, and it is host-written every frame for the
// reason the dials are: it is derived from how many off-screen faces there ARE, so that the class
// costs a bounded number of shaded faces a frame rather than a number that grows with the set. R9b:
// a class that overruns degrades its own refresh rate and nothing else's. 0 is the control arm and
// restores the state every figure taken before this measured -- an off-screen face casting nothing.
//
// It lives at the far END of this buffer, past the card's counters, so the host's two writes and the
// host's one fill cover three DISJOINT ranges. Transfer commands in a command buffer have no implicit
// ordering between them, so a host word inside the zeroed span would be a race whose loser is
// whichever the driver ran second -- and a stride that reads nought half the time is a class that
// silently stops casting.
const uint kProbeSecondaryStride = 32u;

// ---- R9c, the halo: how far past the screen the primary pass claims ---------------------------
//
// In PIXELS each side, and worked out on the host from how fast the camera is turning, so standing
// still is nought and the dispatch is exactly the screen. That is not an optimisation, it is what
// makes the stage free in the case the grid measures: a settled camera reveals nothing, so there is
// nothing to claim ahead of.
//
// # What this is for, measured before it was built (D585)
//
// It is NOT that a pan reveals geometry with no face -- the full-sun fallback is nought pixels of
// 605,945 at every band, panning or still, because R3e claims a stand-in in the pass that discovers
// it and R9d reads the coarse face three levels up. It is that the face it arrives with has barely
// measured anything: the leading edge of a pan carries **112 ambient samples against 707** for the
// identical pixels arrived at from the other side, and the deficit reaches the outer third of the
// frame.
//
// # Why this does not add work, which D566's tail argument would suggest it does
//
// The faces a halo lights are exactly the faces that were about to be lit anyway. It does not create
// rays, it moves them EARLIER -- so over a pan the total is unchanged and what changes is how many
// faces are mid-burst at any instant. The honest risk is therefore the peak rather than the total,
// and the honest measurement is the faces pass while panning, beside the convergence gate.
const uint kProbeHaloMargin = 33u;
const uint kProbeHaloStride = 34u;
// R5b's sun seed, host-written. See the word map above, and kSunSeedSamples for the figure.
const uint kProbeSunSeed = 35u;
// R5b's ramp, counted where it is decided: how many faces wanted a stand-in this frame and how many
// of them were refused one. See the word map above for why it is two words rather than one.
const uint kProbeSunRamped = 30u;
const uint kProbeSunNoStandIn = 31u;

uint probe_halo_margin() { return light_probe.words[kProbeHaloMargin]; }
uint probe_halo_stride() { return max(light_probe.words[kProbeHaloStride], 1u); }

void probe_add(uint at) {
    if ((light_probe.words[0] & kProbeOn) == 0u) return;
    atomicAdd(light_probe.words[at], 1u);
}
bool probe_coarse_bounce() { return (light_probe.words[0] & kProbeCoarseBounce) != 0u; }
bool probe_denoise() { return (light_probe.words[0] & kProbeDenoise) != 0u; }
bool probe_material() { return (light_probe.words[0] & kProbeMaterial) != 0u; }
bool probe_fold() { return (light_probe.words[0] & kProbeFold) != 0u; }
bool probe_lobe() { return (light_probe.words[0] & kProbeLobe) != 0u; }
bool probe_lobe_ray() { return (light_probe.words[0] & kProbeLobeRay) != 0u; }
bool probe_lobe_big() { return (light_probe.words[0] & kProbeLobeCoverage) != 0u; }
bool probe_see_through() { return (light_probe.words[0] & kProbeSeeThrough) != 0u; }
bool probe_refract() { return (light_probe.words[0] & kProbeRefract) != 0u; }
bool probe_refract_stack() { return (light_probe.words[0] & kProbeRefractStack) != 0u; }
bool probe_translucent() { return (light_probe.words[0] & kProbeTranslucent) != 0u; }
bool probe_edge_aa() { return (light_probe.words[0] & kProbeEdgeAA) != 0u; }
bool probe_level_blend() { return (light_probe.words[0] & kProbeLevelBlend) != 0u; }
bool probe_sun_confidence() { return (light_probe.words[0] & kProbeSunConfidence) != 0u; }
uint probe_secondary_stride() { return light_probe.words[kProbeSecondaryStride]; }
uint probe_sun_seed() { return light_probe.words[kProbeSunSeed]; }

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
//   [9][10][11] the BOUNCE -- the running MEAN of the radiance the far rays found when they landed
//       on a surface rather than reaching sky, one float a channel, with a memory of
//       `kBounceMemory` samples. A float and not fixed point for the same reason the lamps are:
//       this is radiance, it has no bound in [0,1] to scale itself against.
//
//       A MEAN and not a sum, and the memory is the whole of why. Everything else in this record
//       measures something that does not move -- geometry, or a sun that has not -- so a cumulative
//       mean is the right estimator for it and the count may run away as far as it likes. The
//       bounce measures the OTHER FACES, and their light climbs from black as they take their own
//       samples: it is a progressive radiosity solve, and a cumulative mean of a solve in progress
//       converges to the average of the path rather than to where the path is going. See
//       kBounceMemory.
//
// The bounce shares the FAR field's count on purpose. It is the same ray: one unbounded,
// cosine-weighted sample of the hemisphere, whose answer is the sky when it escapes and a surface's
// outgoing radiance when it does not. Splitting the count would allow the two to disagree about how
// many samples one ray produced, which is the shape of fault D316's provisional table exists to make
// unrepresentable. What the composite reads is `sky_radiance(normal) * open_sky + bounce`, and those
// two terms are the two halves of one integral rather than two effects added together -- the count
// divides the first and bounds the memory of the second, which is one count answering one question
// about one ray in the two ways the two halves need it.
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
// Declared in shaders/face_terms.glsl, which the composite includes too. Two copies of
// this number is what drew the enclosed camera as salt and pepper; see that file.
#include "face_terms.glsl"
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

// Say that a GATHERING ray read this face on this frame. R9b's half of `node_face_seen`, and the
// same three lines for the same three reasons: conditional so a face many rays land on writes once,
// on a record the caller has already fetched, and two invocations racing to write one value is not a
// race worth ordering.
//
// Stamped where the light is READ and not where the face is claimed. A claim happens once and the
// store's cold window then keeps the record for six hundred frames whether or not anything is still
// integrating it; this has to mean "somebody is reading this NOW", because it is what decides
// whether the face is worth a ray. That is D554 and D508 in one sentence -- a clock stamped by an
// event that happens once is a clock about the past.
void node_face_gathered(uint slot) {
    if (slot == kNoFace || slot == kFaceTombstone) return;
    if (slot >= face_gathered.frames.length()) return;
    if (face_gathered.frames[slot] != node_push.frame) face_gathered.frames[slot] = node_push.frame;
}

// Is a gathering ray reading this face recently enough to be worth a ray of its own?
//
// False when it cannot tell, which is the opposite of `node_face_recently_seen` above and is
// deliberate: that one errs towards lighting a face the composite may be about to draw, and this one
// errs towards NOT paying for a face nobody can see. The two are read together in `face_work_of`, so
// an unanswerable slot is already covered by the first.
//
// The window is the same `seen_window` the pixel's stamp uses. It is the same question -- how long
// after the last reader a face goes on measuring -- and giving the off-screen set a second window
// would be a second number to get wrong for no measured reason. What differs between the classes is
// the RATE, which is `probe_secondary_stride`, and that is the one this class is budgeted by.
bool node_face_recently_gathered(uint slot) {
    if (slot >= face_gathered.frames.length()) return false;
    return (node_push.frame - face_gathered.frames[slot]) <= node_push.seen_window;
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
//
// R5b's confidence ramp is written against this same four and calls it `kFaceSunBelieve`
// (shaders/face_terms.glsl), because "may be READ at one sample" and "is BELIEVED at one sample"
// turned out to be the two halves of one sentence and only the first was ever decided. The figure
// cannot be spelled as this constant there, since node.glsl includes face_terms.glsl and not the
// other way about; the comment on kFaceSunBelieve carries the whole argument for why the two are one
// number rather than two that happen to agree.
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

// ...and the least far samples a face must take before its BOUNCE is worth stopping on, which is a
// different question with a much larger answer.
//
// The rule above stops the far field when its own error falls under `kSkyFarEps`, and for a sealed
// room that is **thirty-two rays**: every one of them hits something, p is nought, and the variance
// of a proportion at nought is nought. That is exactly right for the question it was asked — *how
// much sky can this face see* — and useless for the question the same ray now also answers, which is
// *what colour is everything it hit*. A colour has no such collapse: thirty-two samples of a room is
// thirty-two samples of a room however unanimous the sky part was, and the estimate that comes out
// is a per-face mottle, which is the failure mode this whole term exists to avoid.
//
// So the sky's own test may not end the ray while the bounce is still short. What it costs is
// UNBOUNDED rays, which are the expensive ray in this pass — one a visit on the sun's schedule until
// this is reached, against thirty-two before. It is the one number in R9 that buys picture with
// frames, and `--bounce-min N` sweeps it rather than leaving it to be guessed at.
//
// It was 128 and it is four memories now, because of what a face STOPPING means once the mean below
// has a memory: whatever it holds at that moment is what it holds for the life of the face. Three
// turnovers leave 5% of the fill-up in it ((1 - 1/N)^3N = 0.05) and the fourth is the memory it
// spends filling before it starts forgetting at all.
//
// **Nothing about the RATE changes.** This is the same one unbounded ray every `face_stride` frames
// the face was already casting; what moves is when it stops. So no frame casts more rays than it did
// -- a static camera simply takes longer to fall silent, and that is the whole cost of this number.
//
// It buys the sky's own term as well, which was not the reason for it and is the larger half of what
// it measured. `open_sky` is `far_open / far_n` and four times the samples is half the noise in it:
// on the close camera at full convergence, speckle **47.63 before against 45.53** -- so the picture
// this leaves is quieter than the one it replaces, not merely less biased.
const uint kBounceMin = 512u;

// How many samples the bounce remembers, which is what makes it a measurement of the room rather
// than of the camera's history. **This is the fix for a fault a player reported in exactly these
// words: "when i stay still for a while and then move back the voxel faces where i stood still are
// way betterly rendered than the rest and are often brighter".**
//
// # Why a cumulative mean cannot be right here and is right everywhere else in this record
//
// The sun, the near field and the far field measure things that do not move, so every sample they
// take is a sample of the same quantity and averaging all of them is exactly correct. The bounce
// does not: `bounce_radiance` reads what the face a ray landed on is giving off AT THAT MOMENT, and
// that face is itself climbing from black towards its own answer. The room fills with light over
// seconds -- a progressive radiosity solve, iterated one ray at a time -- and a cumulative mean of a
// signal that is still climbing converges to the AVERAGE OF THE CLIMB, not to the top of it.
//
// Two things follow and both are what the player saw:
//
//   - the time constant of a cumulative mean grows with the sample count, so how bright a face is
//     depends on how long the camera has been pointed at it. Measured at the close camera, mean
//     pixel over the whole frame: 131.30 at 150 frames, 131.80 at 300, 132.61 at 900, 132.71 at
//     2,700. A patch the camera dwelt on sits at the end of that curve and everything around it sits
//     where its own dwell left it, with a hard seam between them;
//   - a face that reaches kBounceMin sets kFaceAmbientDone and is silent for the rest of its life,
//     so where it happened to be on that curve is where it stays, for ever.
//
// # Why this costs nothing, in noise or in rays
//
// It is the same one ray a visit, the same record, the same three words:
// `mean += (sample - mean) / min(far_n, N)`. What changes is which samples it is a mean OF -- the
// most recent ones rather than all of them. A mean with a memory of N has the variance of a simple
// mean of 2N - 1 samples, so the noise is a number that can be chosen rather than one that has to be
// accepted, and it was chosen against kBounceMin: 128 is a 255-sample mean, which is what the old
// estimator held after 255 samples of dwelling.
//
// # The sweep, because both halves of this are a trade and neither was guessed at
//
// Close camera, 1280x800, `--settle`, frame 2,700, every face converged and silent in every arm.
// The mean pixel is the whole frame's and the unbiased value is what the shortest memory converges
// to, since a memory of 32 has no fill-up left in it at all:
//
//   memory   min   speckle   mean pixel   short of unbiased
//   -------------------------------------------------------
//   (none)   128    47.63      132.708      -0.85     <- what this replaces
//   32       256    54.00      133.553       0.00
//   64       256    49.42      133.440      -0.11
//   96       384    47.08      133.460      -0.09
//   128      512    45.53      133.504      -0.05     <- this
//
// So the dwell bias goes from 0.85 of 255 to 0.05 and the picture gets QUIETER by 4.4%. The noise
// does not come from the memory: it comes from `kBounceMin` beside it, because the far ray answers
// the sky as well and four times the samples is half the noise in `open_sky`. Reading either number
// on its own gets the trade backwards -- memory 32 at min 256 is the noisiest arm in the table.
//
// `--bounce-memory N` sweeps it, 0 means this constant, and the CONTROL ARM is a memory larger than
// `far_n` can ever reach, which is the cumulative mean exactly rather than near it:
// `--bounce-memory 4096 --bounce-min 128` is the row at the top.
const uint kBounceMemory = 128u;


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

// ---- R5b: what a face's SUN term starts from, which until now was nothing at all ---------------
//
// Every other term on a new face is seeded from the coarse face standing over it — the near field
// and its gradients, the far field, the bounce and the lamps, all in `face_light_seed`, each with a
// comment saying that starting from nought is a flash a player sees. **The sun was the one term
// with no prior**, and it is the term `kFaceSettled` lets the composite read after ONE sample.
//
// So a face claimed this frame shows a coin toss: one shadow ray, believed. In a room where the
// true answer is 0.05 that is one face in twenty reading fully lit, scattered over every surface
// the camera has just revealed — and the close camera has **23% of its faces still measuring**,
// which is where R5b's speckle actually comes from. `kFaceSettled`'s own comment says the variance
// it accepts is "what R5's denoise exists to take"; this is the other half of that sentence, and it
// takes the variance out at the source rather than filtering it afterwards.
//
// **Three samples, and the number is chosen against `kFaceEager` rather than against the noise.**
// A face under kFaceEager = 4 samples is exempt from the shading stride — it gets a ray EVERY
// frame until it has four of its own. A prior of four or more would put a newly claimed face
// straight onto one frame in `face_stride`, so it would inherit its parent's answer and then take
// a hundred frames to correct it. At three it starts from a prior, keeps its every-frame ray, and
// its own samples outvote the parent within a handful of frames — the same shape as
// kFaceEditSeed and kLampSeedSamples, with the stride's threshold deciding the figure.
//
// The prior is also self-correcting where it is wrong: `face_accumulate`'s unanimity rule fires on
// a sample that contradicts a unanimous history, so a child of a fully-lit parent that finds itself
// in shadow drops to two samples on its first ray and is measuring its own answer immediately.
const uint kSunSeedSamples = 3u;
// And the least the ancestor must have measured before its ratio is a prior rather than noise one
// level coarser. Sixteen rather than kSkySeedMin's 64 because the sun's window is 256 where the
// ambient term's is 2,048: the same fraction of the same estimator.
const uint kSunSeedMin = 16u;

// ...and the same question asked at READ time, which turns out to have a different answer — this is
// FOUR, and the sixteen above was inherited into it once and measured as being most of why R5b's
// ramp did nothing.
//
// **The measurement, card-free, three frames after a camera cut through 180 degrees** (D662): of
// 4,665 faces short of `kFaceSunBelieve` samples, **3,006 — 64.4% — found no ancestor holding
// sixteen rays of its own**, so two thirds of the population the ramp exists for went on believing
// one shadow ray outright. Nothing in any picture said so, and it could not have been read out of
// one: every pixel that differed between the two arms was a full flip between black and white,
// which is what a re-rolled coin toss looks like and is the one thing the ramp never produces.
// `kProbeSunRamped` and `kProbeSunNoStandIn` are what answered it, and they are two counters rather
// than one for exactly that reason.
//
// **Why the two thresholds differ is the whole of it, and it is not a relaxation.** `kSunSeedMin`
// guards a prior written ONCE, into a face's own counters, at claim: it will be believed for the
// rest of that face's life against every ray the face takes afterwards, so a thin ancestor there is
// noise that has to be measured back out, and refusing costs nothing — the face simply starts from
// nothing, which is what it did before D660. This guards a blend re-decided EVERY FRAME and thrown
// away the moment the face has four rays of its own, so refusing is not free: it is the feature not
// happening, on the faces it was built for. And the case that makes the two diverge is the one that
// matters most — a camera turning into geometry it has never faced, where the coarse face is exactly
// as new as the fine one under it and takes forty frames to reach sixteen samples, because it is
// eager for four and then on `face_stride` for the rest.
//
// Four, and it is `kFaceSunBelieve` again rather than a third number: the honest test is whether the
// ancestor is better measured than the face reading it, and the face is under four by construction
// here. So the ancestor has to be worth at least what this renderer calls its own answer. Below
// that the two are both coin tosses and there is nothing to be gained by mixing them; at four and
// above the ancestor is drawing on five hundred and twelve faces' worth of geometry at four times
// the confidence of the face asking.
const uint kSunStandInMin = 4u;   // must match kFaceSunBelieve in face_terms.glsl

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
    bool off_screen;         // ...and is it casting for a gathering ray rather than for a pixel
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
    w.off_screen = false;
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
    const bool on_screen = provisional_face || node_face_recently_seen(slot);

    // ...and the off-screen set, which is worth a ray for a different reason and at a different rate.
    //
    // R9b. A pixel reading a face is one reason to measure it; a GATHERING RAY reading it is the
    // other, and until now only the first bought anything. The store held 229,413 faces claimed by
    // light rays with nought samples in every one of them, and 12.4% of the frame's gathering rays
    // read those empty records straight back out (see `face_gathered`).
    //
    // The rate is the whole of the budget and it is set by the host from how many of these there are,
    // so a class that grows refreshes itself more slowly and takes nothing from the screen. Phased on
    // the slot, or half a million faces come due on one frame and the pass spikes -- D431's shape in
    // the dispatch rather than in the feedback buffer.
    //
    // A face that is BOTH is on-screen: `on_screen` is tested first and the stride never applies to
    // it, because a face a pixel is reading must not be slowed down by a class it is only incidentally
    // in. That is also what keeps this out of the sun's denominator, which counts the on-screen set on
    // the host and is the number D527 and D557 were both lost in.
    // The arithmetic before the LOAD, and this is not the pre-gate D432 forbids.
    //
    // The four tests are a conjunction and reordering them cannot change which faces are eligible on
    // which frame -- the predicate is identical, and only where the memory traffic happens moves.
    // D432's gate was different in kind: it decided whether a node was ever REPORTED, and a report
    // could only happen on a frame a ray happened to read it, so making the two coincide threw most
    // of them away. Nothing here is thrown away.
    //
    // It is worth stating what it cost to have this the other way round, because the shape recurs:
    // `face_work_of` runs for every live slot in BOTH passes, so a load in front of the stride was
    // three quarters of a million scattered four-byte reads a frame, twice -- and the tell was that
    // the faces pass barely responded to the budget at all. Eight times fewer off-screen faces shaded
    // moved it 3.58 ms to 3.14; the load was most of what had been added, and it is charged whether
    // or not a single face casts.
    const uint secondary_stride = probe_secondary_stride();
    w.off_screen = !on_screen && secondary_stride != 0u &&
                   ((node_push.frame + slot) % secondary_stride) == 0u &&
                   node_face_recently_gathered(slot);
    w.may_cast = on_screen || w.off_screen;

    // Nothing to do at all: nobody is looking at it and the host has announced nothing.
    if (!w.may_cast && !w.edit_reset && node_push.light_reset == 0u) return w;

    // An announcement lowers a face's confidence; it does not erase its answer. See kFaceEditSeed:
    // zeroing this is what made every edit turn the room into flashing blocks.
    w.counters = w.edit_reset ? face_reseed(face.counters, node_push.edit_seed) : face.counters;
    w.samples = face_samples_of(w.counters);
    // The sun's stride is the ON-SCREEN class's budget and must not be applied on top of the
    // off-screen one. A face that got here because a gathering ray is reading it has already been
    // thinned by `secondary_stride`, and the two thinnings do not compose: unless one stride divides
    // the other, `(slot + frame)` is nought modulo both on almost no frame at all, and the off-screen
    // set would be visited and then given no sun ray -- lit by sky and lamps only, for ever, with
    // every audit line reading exactly as it does when the class is working. Trap 7 in the budget.
    w.sun_due = w.off_screen ||
                !(node_push.face_stride > 1u && w.samples >= kFaceEager && !w.near_edit &&
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

// ---- what one voxel is made of, asked away from a march (R4a) ---------------------------------
//
// The marcher hands back a type id for the voxel a ray STOPPED on, and that is the only way this
// pass has ever had of asking. A face has to ask about itself, on a frame no ray of its own is
// necessarily being cast, so it descends to its own brick and reads the type out of it: the same
// two functions the inner walk uses, without the walk.
//
// Nought for "the pool cannot answer", which covers three different things on purpose -- the brick
// is not built, the node at the leaf level is a shell, or the voxel is not solid. All three mean
// *ask again later*, and none of them means matte. Air is type nought too and is the same answer
// for the same reason: a face over nothing has nothing to be made of.
uint node_voxel_type_at(ivec3 voxel) {
    Found found = node_locate(voxel, kLeafLevel);
    if (found.state != kFoundHere) return 0u;
    if ((node_flags_of(nodes.items[found.slot].packed) & kNodeLeaf) == 0u) return 0u;
    uint leaf = nodes.items[found.slot].children;
    if (leaf == kNoNode) return 0u;
    // Arithmetic shift, so this is the positive remainder either side of the origin -- the same
    // convention node_locate itself uses one line above.
    ivec3 inner = voxel - ((voxel >> 3) << 3);
    if (!leaf_voxel_solid(leaf, inner)) return 0u;
    return leaf_voxel_type(leaf, inner);
}

// What THIS face is made of, resolved once and kept. See the FaceMaterial binding for the word.
//
// The read is the common case by a wide margin -- a face looks once and every visit after that is
// one load of a word the shading pass is about to touch anyway -- so the branch is arranged for it.
uint node_face_material(uint slot, ivec3 node, uint level) {
    if (!probe_material()) return 0u;
    if (slot >= face_material.words.length()) return 0u;
    const uint held = face_material.words[slot];
    if ((held & (kFaceMaterialKnown | kFaceMaterialNone)) != 0u) return held;

    // A coarse face has looked and there is nothing to find. See the binding for why that is its
    // own answer rather than a roughness of nought.
    uint word = kFaceMaterialNone;
    if (level == 0u) {
        const uint type_id = node_voxel_type_at(node);
        // Not known, and it must not be recorded as known: the brick arrives later and this face
        // has to look again. Trap 7 -- "nothing here" and "I could not find out" are different
        // answers and the second one is not allowed to be sticky.
        if (type_id == 0u) return 0u;
        // Clamped, because an out-of-range index into an SSBO is not a wrong number, it is whatever
        // the driver decides out of bounds means -- and this renderer has already lost a device to
        // exactly that (see material_of in shaders/pt_material.glsl).
        const uint type_at = min(type_id, uint(types.items.length()) - 1u);
        const uint visual_at = min(types.items[type_at].x, uint(visuals.items.length()) - 1u);
        const uvec4 visual = visuals.items[visual_at];
        word = kFaceMaterialKnown;
        // VisualRecord's second word is roughness, metallic, ior, emissive, one byte each, so the
        // low half of it IS this word's low half. See src/world/voxel_type.hpp, which is the
        // authority, and pt_material.glsl, which unpacks the same bytes for the deleted tracer.
        word |= visual.y & 0xFFFFu;
        word |= ((visual.w >> 16u) & 0xFFu) << 16u;

        // ...and what it lets through, into the second word (R4d). The same descent, the same two
        // table reads, the same visit: opacity is byte 3 of the first word, the index of refraction
        // is byte 2 of the second and translucency is byte 3 of the third.
        if (slot < face_medium.words.length()) {
            face_medium.words[slot] = kFaceMediumKnown | ((visual.x >> 24u) & 0xFFu) |
                                      (((visual.y >> 16u) & 0xFFu) << 8u) |
                                      (((visual.z >> 24u) & 0xFFu) << 16u);
        }
    }
    face_material.words[slot] = word;
    return word;
}

// A slot the store has just handed out, or a face the world has changed under, has to look again.
//
// The array is never uploaded, so a claim cannot clear it the way it clears `counters` -- which is
// the same sentence `face_light`'s reset block already carries, and this is called from beside it.
void node_face_material_forget(uint slot) {
    if (slot >= face_material.words.length()) return;
    face_material.words[slot] = 0u;
    // ...and what it lets through, which was worked out on the same visit and is stale for exactly
    // the same reasons (R4d).
    if (slot < face_medium.words.length()) face_medium.words[slot] = 0u;
}

// ---- R4c: the block of outgoing bins this face is holding, if it can get one -------------------
//
// Called once a visit, from the shading pass, immediately after the face has worked out what it is
// made of. It does three things and they are one walk over four adjacent headers:
//
//   - if this face already holds one of them, stamp it with this frame and hand it back. That is
//     the common case by a wide margin and it costs two loads and one store;
//   - if a way is free or its holder has gone cold, take it and zero the bins;
//   - if every way is held by a face that is warm and worth more, hand back kNoLobe. That is a
//     DECLINE and not a failure: the composite then reads the hemispherical mean this face already
//     stores, which is the same lobe with one bin in it (see face_terms.glsl).
//
// # The compare-and-swap, which is the only atomic in this pass
//
// D191's *one invocation owns each face* is what removed every atomic from the face shader, and
// this does not put them back for a face's own record: a block's BINS are written by its holder
// alone, exactly as a face's light is. What needs an atomic is the take-over, because two faces in
// the same dispatch can want the same way -- and the loser has to find out, or two faces write one
// block's bins for the rest of the run and the picture disagrees with itself frame to frame.
//
// The loser finds the winner's slot in the word it wanted, exactly as `node_face_claim` above is
// arranged, so the race resolves by reading rather than by retrying.
uint node_face_lobe(uint slot, uint material, uint frame) {
    if (!probe_lobe()) return kNoLobe;
    if ((material & kFaceMaterialKnown) == 0u) return kNoLobe;
    const float worth = face_lobe_worth(face_material_rough(material),
                                        face_material_metal(material));
    // Negative is the sentinel for "the shader's own figure", exactly as `--denoise-edge` beside it
    // is: a dial whose control arm is spelled one greater than it means is a wrong measurement
    // waiting to happen, and here nought is a legitimate setting that means EVERY face asks.
    const float floor_worth = push.tone.z >= 0.0 ? push.tone.z : kLobeWorthFloor;
    if (worth < floor_worth) return kNoLobe;
    if (slot >= kLobeSlotMask) return kNoLobe;

    const uint base = face_lobe_set(slot);
    const uint mine = face_lobe_holder(slot, worth);

    // R4b: whether this face earns the expensive class. Its material has to be sharper than
    // thirty-six bins can hold AND it has to be filling enough of the screen to be worth four
    // blocks -- see kLobeBigPixels, and the note above it about why coverage is the affordability
    // rather than the demand.
    // **The gate is the MATERIAL and not the coverage, and that is a reversal of D186 with a
    // measurement behind it.** Gating on how many pixels the face covers was built first and it
    // speckled the great door: coverage varies smoothly across one surface, so the class flipped
    // face to face along the middle of it, and a lobe cut into 144 directions and one cut into 36
    // share no bin for the cross-face blend to pair. **A hard per-face decision is a new
    // discontinuity per face** -- D387's standing measurement, arriving in a size class.
    //
    // Roughness is a material property, so neighbouring faces of one material agree by
    // construction and the blend has like to pair with like. See kLobeBigSide for what coverage
    // was supposed to buy and why it does not.
    const bool wants_big = probe_lobe_big() &&
                           face_lobe_bins_wanted(face_material_rough(material)) > float(kLobeBins);

    // Already holding one, which is what nearly every visit after the first is. A follower way
    // steps back to the head of its run, because that is the block the whole lobe is addressed
    // from.
    for (uint way = 0u; way < kLobeWays; ++way) {
        const uint header = face_lobe_header(base + way);
        if (header + 1u >= face_lobe.words.length()) return kNoLobe;
        const uint held = face_lobe.words[header];
        if (face_lobe_is_held(held) && face_lobe_holder_slot(held) == slot) {
            const uint state = face_lobe.words[face_lobe_state_at(base + way)];
            const uint head = ((state & (kLobeBig | kLobeFollower)) != 0u)
                                  ? base + (way & ~(kLobeBigWays - 1u))
                                  : base + way;
            face_lobe.words[face_lobe_header(head) + 1u] = frame;
            probe_add(kProbeLobeHeld);
            if ((face_lobe.words[face_lobe_state_at(head)] & kLobeBig) != 0u) {
                probe_add(kProbeLobeBig);
            }
            return head;
        }
    }

    // ---- the expensive class, which is four consecutive blocks of one set --------------------
    //
    // Tried first and given up on quietly: a face that cannot get four falls through to asking for
    // one, which is a coarser lobe rather than none. A decline here must never become a decline
    // altogether, or the sharpest surfaces in the building would be the only ones with no
    // reflection at all -- trap 7, arriving in a size class.
    if (wants_big) {
        for (uint run = 0u; run + kLobeBigWays <= kLobeWays; run += kLobeBigWays) {
            bool free_run = true;
            for (uint way = run; way < run + kLobeBigWays; ++way) {
                const uint header = face_lobe_header(base + way);
                const uint held = face_lobe.words[header];
                if (!face_lobe_is_held(held)) continue;
                const uint since = frame - face_lobe.words[header + 1u];
                // A run is taken whole, so every block in it has to be free or cold. Worth is not
                // enough here: taking four warm blocks off four faces to serve one is a trade this
                // has no way to price, and the cheap class is what most of the picture is.
                if (since <= kLobeCold) { free_run = false; break; }
            }
            if (!free_run) continue;

            const uint head = base + run;
            const uint header = face_lobe_header(head);
            const uint was = face_lobe.words[header];
            if (atomicCompSwap(face_lobe.words[header], was, mine) != was) continue;
            for (uint way = 1u; way < kLobeBigWays; ++way) {
                face_lobe.words[face_lobe_header(head + way)] = mine;
                face_lobe.words[face_lobe_header(head + way) + 1u] = frame;
                face_lobe.words[face_lobe_state_at(head + way)] = kLobeFollower;
            }
            face_lobe.words[header + 1u] = frame;
            face_lobe.words[face_lobe_state_at(head)] = kLobeBig;
            face_lobe.words[face_lobe_count_at(head)] = 0u;
            for (uint bin = 0u; bin < kLobeBigBins; ++bin) {
                const uint at = face_lobe_bin_at(head, bin);
                if (at >= face_lobe.words.length()) return kNoLobe;
                face_lobe.words[at] = 0u;
                face_lobe.words[face_lobe_weight_at(head, bin, kLobeBigBins)] = 0u;
            }
            if (face_lobe_is_held(was)) probe_add(kProbeLobeTaken);
            probe_add(kProbeLobeBig);
            probe_add(kProbeLobeHeld);
            return head;
        }
    }

    // Nobody's, or somebody's who has gone cold, or somebody's who is worth less. Weakest first, so
    // a full set gives up the block it will miss least -- and a face that finds no candidate at all
    // takes nothing rather than the last one it looked at.
    uint best = kLobeWays;
    float best_worth = worth;
    for (uint way = 0u; way < kLobeWays; ++way) {
        const uint header = face_lobe_header(base + way);
        const uint held = face_lobe.words[header];
        if (!face_lobe_is_held(held)) {
            best = way;
            best_worth = -1.0;
            continue;
        }
        // Cold is a stronger claim than worth: a block whose holder has stopped being shaded is
        // holding an answer nobody is going to read, however valuable that answer would have been.
        const uint since = frame - face_lobe.words[header + 1u];
        // ...and a WARM block is only taken by a face worth substantially more, which is hysteresis
        // and not caution.
        //
        // **Taking a block resets its sample count, so its holder starts its burst again.** Without
        // a margin, two faces of nearly equal worth sharing a set trade the same block for ever and
        // neither ever converges: measured, a settled camera sat at 417 blocks changing hands and
        // 883 faces still bursting, and the faces pass was 2.2 ms over where it should have been.
        // That is the state the third counter on the audit line exists to separate from a pool that
        // is merely full, and it is the only reason it is printed.
        const float against =
            since > kLobeCold ? -1.0 : face_lobe_holder_worth(held) * kLobeTakeMargin;
        if (against < best_worth) {
            best = way;
            best_worth = against;
        }
    }
    if (best == kLobeWays) {
        probe_add(kProbeLobeDeclined);
        return kNoLobe;
    }

    const uint block = base + best;
    const uint header = face_lobe_header(block);
    const uint was = face_lobe.words[header];
    if (atomicCompSwap(face_lobe.words[header], was, mine) != was) {
        // Somebody else took it between the look and the swap. One frame without a lobe, and the
        // next visit sorts it out -- retrying here would be a loop whose length is decided by how
        // many faces happen to share a set.
        probe_add(kProbeLobeDeclined);
        return kNoLobe;
    }
    face_lobe.words[header + 1u] = frame;
    // ...and the count of rays this lobe has taken, which is what decides whether it still bursts.
    // Zeroing it is what makes a taken-over block a new lobe rather than a converged one holding
    // somebody else's answer.
    face_lobe.words[face_lobe_count_at(block)] = 0u;
    // ...and the class, because this block may have been the head of a big run a moment ago. A
    // stale kLobeBig here would have the next reader index thirty-six bins as a hundred and
    // forty-four and walk into three blocks it does not hold.
    face_lobe.words[face_lobe_state_at(block)] = 0u;
    // Whatever the last holder measured is about a different surface pointing a different way, and
    // it must not be averaged into this one. Zeroed rather than seeded: there is no ancestor to
    // inherit a lobe from -- a coarse face has no material and so never holds a block -- and a
    // weight of nought is exactly what tells every reader to fall back to the hemispherical mean.
    for (uint bin = 0u; bin < kLobeBins; ++bin) {
        const uint at = face_lobe_bin_at(block, bin);
        if (at >= face_lobe.words.length()) return kNoLobe;
        face_lobe.words[at] = 0u;
        face_lobe.words[face_lobe_weight_at(block, bin, kLobeBins)] = 0u;
    }
    // Counted only where it was somebody ELSE's. A first claim on an empty pool is not churn, and
    // counting the two together would make an empty pool and a thrashing one read the same -- which
    // is the whole reason this counter is beside the other two rather than folded into them.
    if (face_lobe_is_held(was)) probe_add(kProbeLobeTaken);
    probe_add(kProbeLobeHeld);
    return block;
}

// A slot the store has handed to a different face gives its block up on the spot, rather than
// waiting to go cold.
//
// Called from beside `node_face_material_forget` and for the same reason: this array is never
// uploaded, so a claim cannot clear it. Without it a re-used slot would find its own number in a
// header, decide the block was its own, and read the previous occupant's reflection.
// Which block a slot is holding, without claiming one. The same probe `node_face_lobe` opens with
// and the same one `resolve.comp` runs per metal pixel -- three copies of a walk over eight
// adjacent headers, and it is three because each of them is in a shader with a different descriptor
// set. The arithmetic they share is in face_terms.glsl, which is where a stride belongs (D332).
uint node_face_lobe_find(uint slot) {
    if (slot >= kLobeSlotMask) return kNoLobe;
    const uint base = face_lobe_set(slot);
    for (uint way = 0u; way < kLobeWays; ++way) {
        const uint header = face_lobe_header(base + way);
        if (header + 1u >= face_lobe.words.length()) return kNoLobe;
        const uint held = face_lobe.words[header];
        if (face_lobe_is_held(held) && face_lobe_holder_slot(held) == slot) {
            const uint state = face_lobe.words[face_lobe_state_at(base + way)];
            return ((state & (kLobeBig | kLobeFollower)) != 0u)
                       ? base + (way & ~(kLobeBigWays - 1u))
                       : base + way;
        }
    }
    return kNoLobe;
}

void node_face_lobe_forget(uint slot) {
    if (slot >= kLobeSlotMask) return;
    const uint base = face_lobe_set(slot);
    for (uint way = 0u; way < kLobeWays; ++way) {
        const uint header = face_lobe_header(base + way);
        if (header + 1u >= face_lobe.words.length()) return;
        const uint held = face_lobe.words[header];
        if (face_lobe_is_held(held) && face_lobe_holder_slot(held) == slot) {
            // Every block of the run, not just the one that was found: three quarters of a released
            // big block still marked as held is three blocks nobody can ever take again.
            face_lobe.words[header] = 0u;
            face_lobe.words[face_lobe_state_at(base + way)] = 0u;
            continue;
        }
    }
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

// A FACE a pixel READ, named by slot in x, exactly as kFeedbackRead names a node slot.
//
// The distinction against kFeedbackFace is the whole point and the two must not be conflated: that
// one is the request lattice saying *claim this face, I have not seen it before*, and it arrives for
// one pixel in stride² so it cannot be the residency clock. This one says *this face, which you
// already have, is on the screen right now* — one entry per face per `face_read_period` frames
// however many pixels are on it. The consumer's answer is `FaceStore::touch` and nothing else: it
// claims nothing, builds nothing and asks for nothing. D508.
const int kFeedbackFaceRead = 0x100000;

// A face a LIGHT ray landed on, rather than one a pixel landed on. R9a.
//
// The store holds what a primary ray stopped on, so it holds exactly what the camera can see and the
// light in it is a screen-space set in world-space clothing. Everything the eye can see behaves;
// everything the eye cannot see, but which the eye's surfaces depend on, does not exist. That is
// what a mirror facing a wall behind the camera reflects, what a red wall out of frame bounces, and
// what has to be rediscovered every time the camera turns.
//
// This is deliberately the OPPOSITE of the rule a shadow ray follows (D292: a light path may not
// drag streaming towards whatever it crosses), and the distinction is the whole of why it is
// affordable: a ray names the one face it LANDED on, never the geometry along the way. It is also
// not a streaming request of any kind -- the host's answer is a face claim, which builds nothing,
// asks the world for nothing and cannot move one node into the pool. R9h's rule survives intact.
//
// The class rides on the tag rather than in a second buffer because it is one bit about one entry,
// and the host needs it for exactly one decision: whether this claim is subject to the off-screen
// cap. See kFaceSecondary in src/world/face_store.hpp.
const int kFeedbackSecondary = 0x200000;

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

// A face a PIXEL read, named by slot, so the store's residency clock hears what the screen is
// actually looking at instead of what the request lattice got round to asking about.
//
// See `face_read` above for why this exists and why the phase gate here is not D432's mistake. Two
// gates, and both are needed:
//
//   the PHASE  -- a slot is eligible one frame in `period`, so the reports of a store of half a
//                 million faces are spread evenly rather than all arriving on the first frame that
//                 anything is due. Without it a cold store reports every visible face at once into a
//                 buffer of 131,072 entries and the overflow takes the streaming reports with it,
//                 which is D431 exactly;
//   the STAMP  -- the array, so the thousands of pixels that share one face on its eligible frame
//                 produce one entry between them and not thousands.
//
// The stamp is a plain store rather than an atomic. Two pixels racing on the same face on the same
// frame both see the stale value and both report, which costs one duplicate entry out of thousands
// and cannot cost correctness -- the host's answer to two reports of one slot is to stamp it twice
// with the same frame. An atomicCompSwap here would be a read-modify-write on every visible face
// every eligible frame to save an entry nobody misses.
void node_face_report_read(uint slot) {
    const uint period = node_push.face_read_period;
    if (period == 0u || slot == kNoFace || slot == kFaceTombstone) return;
    // The card's own provisional faces live above the store's capacity and are not the host's to
    // keep: they are re-claimed every frame by whichever pixel gets there first, and the host has no
    // slot of that number. Reporting one would stamp an unrelated face in the store.
    if (node_push.provisional_base != 0u && slot >= node_push.provisional_base) return;
    if (slot >= face_read.frames.length()) return;
    if (((node_push.frame + slot) & (period - 1u)) != 0u) return;
    if ((node_push.frame - face_read.frames[slot]) < period) return;
    face_read.frames[slot] = node_push.frame;

    uint index = atomicAdd(feedback.count, 1u);
    if (index < push.resolution.w) {
        feedback.entries[index].coord = ivec4(int(slot), 0, 0, kFeedbackFaceRead);
    }
}

// Ask the host to claim the face a LIGHT ray landed on. R9a, and the tag above is the whole of the
// argument for why this is allowed at all.
//
// Called from the face pass after a march rather than from inside `node_march`, and that is not
// tidiness. The march already RECORDS the face key on every hit -- recording is unconditional and
// reporting is not, which is `node_face_hit`'s own split -- so the caller has the three numbers in
// hand and the marcher's hot path is not touched by this stage at all. A parameter threaded through
// `node_march` would put a branch in the inner walk of the pass that is 63% of a moving frame.
//
// The THROTTLE is the caller's and it has to be, because there is no slot to stamp: the whole point
// is that this face is not in the store yet, so `node_seen`'s trick (D431 -- one entry per node per
// window, however many rays hit it) has nothing to key on. What bounds this instead is that it is
// asked on one ray in `secondary_period` frames per gathering face; see shaders/shade_faces.comp.
void node_face_request(ivec3 node, uint level, uint dir) {
    uint index = atomicAdd(feedback.count, 1u);
    if (index < push.resolution.w) {
        feedback.entries[index].coord =
            ivec4(node, (int(level) & 0xFF) | int(dir << 8) | kFeedbackFace | kFeedbackSecondary);
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
    // R4d: what fraction of a ray survived the transmissive matter it crossed on the way here.
    //
    // One for a ray that crossed nothing, which is every ray on nine tenths of this building, and
    // the product of what each pane let through otherwise. It is a COLOUR because glass is coloured:
    // a green pane turns white sun green on its way in, and the tint is the medium's own albedo.
    vec3 through;

    // R4d's refraction: the interface where the ray came OUT of transmissive matter, if it did.
    //
    // `left_medium` is the whole reason this is three fields rather than one: a march asked to stop
    // at the far side of the glass comes back with `hit == false`, and so does a ray that reached
    // open sky. Those are not the same fact and the caller has to be able to tell them apart, which
    // is trap 7 in the shape it always takes here. `exit_normal` points back the way the ray came,
    // which is the convention GLSL's own `refract` wants.
    bool left_medium;
    float exit_t;
    ivec3 exit_normal;

    // R4e: how far the ray travelled THROUGH matter, in voxels. Filled by `kThroughSolid` and
    // nought for every other mode. A ray that came back `hit == false` with a small `crossed` got
    // to the other side through that much stone, which is the whole question a translucent surface
    // asks.
    float crossed;

    // R5d: where the ray leaves the cell whose `coverage` this is, so a caller that wants to know
    // what is BEHIND a partly covered node can start a second march past it rather than into it.
    //
    // Nought unless the coverage above describes exactly the cell that was drawn, and that gate is
    // the whole reason this is a distance rather than a bool. Three places set `coverage`; only the
    // coarse-node branch reads it off the very node whose box the pixel landed in. The other two
    // read it off a node COARSER than the cell they draw -- the in-brick march takes the brick's
    // byte for a pixel resolving two voxels, and the stand-in takes the parent's for a cell one
    // level under it -- and blending a pixel by a fraction measured over eight times its footprint
    // is trap 6 again at a different scale. So those two leave this at nought and get no edge AA,
    // which is exactly what they had before this existed.
    float coverage_exit_t;
};

// ---- R4d: the three things a ray can do about matter it can see through -------------------------
//
// A bool held two of these and the third is what refraction needs. A light ray must carry ON to the
// far side, because what a shadow ray wants to know is whether the sun reaches; a refracted primary
// ray must STOP at the far face of the pane, because that is the interface it bends at. Neither is
// a shade of the other.
const uint kThroughStop = 0u;   // transmissive matter stops the ray dead, as it always did
const uint kThroughPass = 1u;   // the ray carries on, multiplying `through` by what each voxel let by
const uint kThroughExit = 2u;   // ...and stops where it LEAVES the medium, for the caller to bend

// How many media one primary ray will bend its way out of before it gives up and slides through the
// rest. Four, because that is a window, its double glazing, and the water in the basin behind them,
// and because each one costs two marches: the crossing and the segment after it. The turn that hits
// the cap marches `kThroughPass`, so exceeding it degrades to exactly the picture this renderer
// drew before the loop existed rather than to a hole.
const uint kRefractMedia = 4u;
// R4e, and it is about OPAQUE matter rather than transmissive: the ray crosses solid voxels of any
// material, adding up how far it went through them, and gives up once it has crossed
// `kCrossedMax`. Nothing is attenuated and nothing is bent -- what comes back is a distance.
//
// This is the measurement `clips/translucency_test.clip` was written around, in its author's own
// words: *"the renderer has to find it out by counting the voxels a scattered ray crosses before it
// is extinguished, which is the one measurement a voxel world can always make."* No clip says which
// parts of a building are thin. A ray does.
const uint kThroughSolid = 3u;

// How far a ray in `kThroughSolid` may travel through matter before it counts as stopped, in
// voxels. A metre, which is far past the point where any translucent material this engine can
// describe has anything left: `absorb`-free marble at 110 extinguishes in under two voxels and the
// most translucent thing describable, 255, in four. It is a bound on the WALK rather than a
// physical constant -- the extinction is the caller's arithmetic, and this only stops a ray that
// would otherwise walk the length of a building through solid stone.
const float kCrossedMax = 32.0;

const uint kNoFaceLevel = 0xFFFFFFFFu;

// How far a ray gets through one voxel of a transmissive material, as a tint.
//
// `opacity` is what the clip author wrote and what `face_medium` carries; the colour is the voxel's
// own, because a pane of green glass makes what passes through it green.
//
// **PER METRE, and that is not a detail -- it is the difference between a window and a wall.**
// The first version of this applied the material's opacity once per VOXEL, and a voxel is 3 cm: a
// four-voxel pane of `opacity=64` glass then transmitted 0.75^4 = 0.32 and the daylit hall went
// dark again, with the light meter pinned at its ceiling. What the clip author writes is a property
// of the MATERIAL and not of the sampling grid -- `absorb` in the same record is documented "per
// metre" for exactly this reason -- so the figure is taken over a metre and rooted down to the
// voxel. A pane is then nearly clear however finely it is sampled, which is what a pane is, and
// three voxels of water dim more than one, which is what water does.
//
// It is not Beer-Lambert over the exact path yet: this is a product over the voxels crossed, which
// is the same thing for a ray along an axis and drifts from it on a diagonal. The `absorb` bytes
// and the exact distance are R4d's own sub-step.
vec3 node_medium_through(uint type_id) {
    const uint type_at = min(type_id, uint(types.items.length()) - 1u);
    const uint visual_at = min(types.items[type_at].x, uint(visuals.items.length()) - 1u);
    const uvec4 visual = visuals.items[visual_at];
    const float opacity = float((visual.x >> 24u) & 0xFFu) * (1.0 / 255.0);
    if (opacity >= 1.0) return vec3(0.0);
    const vec3 tint = vec3(float(visual.x & 0xFFu), float((visual.x >> 8u) & 0xFFu),
                           float((visual.x >> 16u) & 0xFFu)) * (1.0 / 255.0);
    // Mixed towards white rather than multiplied outright: a pane a quarter opaque should tint what
    // passes by a quarter, not paint it its own colour. At opacity nought it is clear glass.
    const vec3 metre = mix(vec3(1.0), tint, opacity) * (1.0 - opacity);
    return pow(max(metre, vec3(1e-4)), vec3(1.0 / kVoxelsPerMetreF));
}

// The index of refraction the author wrote, as a ratio to vacuum.
//
// `VisualRecord::ior` is stored as the offset from vacuum in 128ths -- 0 is vacuum and 64 is 1.5 --
// so a material that never mentions it comes back as 1.0 and bends nothing, which is what every
// opaque surface in the building wants and is why this needs no flag of its own to be safe.
float node_medium_ior(uint type_id) {
    const uint type_at = min(type_id, uint(types.items.length()) - 1u);
    const uint visual_at = min(types.items[type_at].x, uint(visuals.items.length()) - 1u);
    const uint word = visuals.items[visual_at].y;
    return 1.0 + float((word >> 16u) & 0xFFu) * (1.0 / 128.0);
}

// Beer-Lambert absorption, PER METRE, which is how `VisualRecord` documents the three bytes.
//
// This is the other quantity `node_medium_through` above could not use. That one is a product over
// the voxels a ray crossed, which is exact along an axis and drifts on a diagonal, and it is all a
// straight ray could ever have -- a ray that does not know where it entered the medium cannot know
// how far it went through it. A refracted ray does know both, so the absorption can be taken over
// the true distance, which is what the plan asks for and what makes a deep look through water
// darker than a glancing one.
//
// The bytes are an amount per metre in the same units the author writes: `absorb=6,1,4` on the
// green glass means six units of red taken out per metre. A sixteenth of a byte per metre puts a
// useful range in a byte -- 255 is a metre of near-black and 1 is a pane you can barely tell from
// clear.
vec3 node_medium_absorb(uint type_id) {
    const uint type_at = min(type_id, uint(types.items.length()) - 1u);
    const uint visual_at = min(types.items[type_at].x, uint(visuals.items.length()) - 1u);
    const uint word = visuals.items[visual_at].z;
    return vec3(float(word & 0xFFu), float((word >> 8u) & 0xFFu),
                float((word >> 16u) & 0xFFu)) * (1.0 / 16.0);
}

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
//
// One caution about "exact", which R5d is what it matters to: it is exact at the LEAF and a maximum
// above it. `NodePool::fold_children` takes the largest of a node's eight children rather than
// projecting them, which errs towards *present* on the same argument as the floor of one that stops
// a single voxel rounding away. So a coarse node reads as more solid than it is, the edge
// anti-aliasing it gets is conservative rather than excessive, and a silhouette across a node that
// is half solid wall gets none at all. `tests/test_node_pool.cpp` pins both halves of that.
uint node_face_coverage(uint slot, ivec3 normal) {
    uint xy = nodes.items[slot].coverage_xy;
    uint z = nodes.items[slot].coverage_z;
    if (normal.x != 0) return (normal.x > 0) ? (xy & 0xFFu) : ((xy >> 8u) & 0xFFu);
    if (normal.y != 0) return (normal.y > 0) ? ((xy >> 16u) & 0xFFu) : ((xy >> 24u) & 0xFFu);
    return (normal.z > 0) ? (z & 0xFFu) : ((z >> 8u) & 0xFFu);
}

// R5d: at or above this, a coarse node is treated as solid and no second march is cast for it.
//
// What it trades is the last sixty-fourth of an edge against a march. The byte is
// `covered * 255 / 64` over an 8x8 projection, so the only values it can take are multiples of
// 255/64 and 251 is "sixty-three of the sixty-four columns have matter in them". Blending that
// pixel 1.6% towards what is behind it is under the quantisation of the byte itself and well under
// the quantisation of the `through` byte the answer is packed into -- so the picture is unchanged
// and the march is saved.
//
// The saving that matters is not this threshold, though, and that is worth saying because the
// threshold looks like the cost control and is not. A flat wall seen face on has EVERY column of
// its projection covered along the direction the ray struck it, so it reports 255 and pays nothing
// at all. What pays is silhouettes and open structure, which is the set that needed anti-aliasing
// in the first place -- the cost of this stage is proportional to the number of edge pixels rather
// than to the number of coarse pixels, by construction rather than by a dial.
const uint kEdgeCoverageFull = 251u;

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

// ---- R5c: what the dither decides, and what it stops deciding ---------------------------------
//
// The dither above is not a mistake to be deleted. `log2(footprint)` is a continuous number and a
// level is an integer, so SOMETHING has to choose between the two levels a pixel sits between, and
// choosing per pixel from a fixed table is what lets this marcher work with no temporal
// accumulation behind it. What is wrong with it is not the choice but its consequence: the two
// levels are drawn with their own folded colours, whole, so a surface halfway between them comes out
// as two flat tones in a 4x4 pattern that swims as the camera walks.
//
// So the dither goes on deciding the SHAPE -- which cell the ray stops on, and therefore the
// coverage byte, the face key, the depth and everything the composite keys off them -- and the
// COLOUR becomes a continuous function of the same fraction the dither is quantising. Both arms of
// the pair aim at the same number:
//
//     base = floor(log2(footprint)),  frac = log2(footprint) - base
//     drawn = (1 - frac) * colour(base) + frac * colour(base + 1)
//
// A pixel that took `base` blends towards its PARENT by `frac`; a pixel that took `base + 1` blends
// towards the child the ray entered by `1 - frac`. Where both fetches land, the two pixels draw the
// same colour and the pattern is gone; where one cannot, that pixel keeps exactly what it draws
// today and the pair is left closer than it was rather than further. That is why this is a blend and
// not a deletion -- the comment in `node_march` records what deleting the dither costs, and it is
// the shape rather than the colour: which pixels report full coverage and which report the node's
// filtered fraction, 43% of pixels different from the marcher this replaces.
//
// The blend is only meaningful because the pool folds a parent EXACTLY from its children --
// `NodePool::fold_children` averages the children's colour weighted by their own coverage -- so a
// point between the two is a point on a line whose ends are the same picture at two scales, not a
// guess. D149's refusal (summarising a region by sampling it) is the thing this is not.
//
// # What this deliberately does not touch
//
// A level-0 hit. The composite reads a TYPE ID at level 0 and a folded colour above it (one payload
// field, two mutually exclusive readings -- see visibility.comp), so the level-0 arm of the 0/1 pair
// cannot be moved from here at all: it would mean changing what the visibility buffer packs. The
// level-1 arm still blends towards the voxel's own colour, which leaves the pair `frac` of the step
// apart instead of the whole of it -- better everywhere and exact nowhere. Moving the other half is
// the composite's business and belongs with R5c's second half.
//
// A stand-in. That branch already draws a cell COARSER than the pixel asked for, because the pool
// has not built the one it wants; blending it with its own parent would be smoothing a picture that
// is about to be replaced wholesale when the node arrives, and paying a descent per unbuilt pixel to
// do it.
//
// And levels 1 to 3 blended against each other. The pool has no node between a voxel and a brick, so
// `colour(1)`, `colour(2)` and `colour(3)` are all the same brick average -- which is what the
// marcher already reports at those levels. A pixel whose footprint is between two and eight voxels
// therefore shows no dither today, and there is nothing there for this to remove. Worth knowing
// before hunting for a change in that band: at 320x200 and a 90 degree lens that is everything
// between six and twenty-five metres.
const float kNodeBlendDeadband = 1.0 / 16.0;

// The pair a pixel sits between, resolved once at the hit.
//
// `want` is the level of the OTHER member -- one above when this pixel took the finer of the two,
// one below when it took the coarser -- and `w` is how much of that member's colour to take.
struct NodeBlend {
    bool on;
    int want;
    float w;
};

NodeBlend node_level_blend(float detail, int level, bool drawn) {
    NodeBlend blend;
    blend.on = false;
    blend.want = level;
    blend.w = 0.0;
    if (!drawn || !probe_level_blend()) return blend;

    const int base = int(floor(detail));
    const float frac = detail - float(base);
    // Both arms of the dither clamp to the same cell up here, so there is no pair and nothing to
    // blend -- the same clamp `level` is taken through.
    if (base + 1 > kNodeMaxDetail) return blend;

    if (level == base) {
        blend.want = level + 1;
        blend.w = frac;
    } else if (level == base + 1) {
        blend.want = level - 1;
        blend.w = 1.0 - frac;
    } else {
        // Unreachable as the clamp above `level` is written -- the footprint floors at one voxel so
        // the low end never bites, and the high end is the test two lines up. It is here so that
        // moving either end of that clamp cannot silently blend a pixel against a level it never sat
        // between, which is the one way this could go wrong without looking wrong.
        return blend;
    }

    // The deadband, and it is the ordered dither's own step rather than a tuning constant. The
    // table holds sixteenths, so below a sixteenth of the way into a pair NO pixel of the 4x4 tile
    // picked the other level and the tile is already one flat tone; above fifteen sixteenths the
    // one pixel that did not is the only one with a correction left worth making, and it makes it,
    // because this weight is the pixel's OWN and not the tile's. What it trades is a descent per
    // hit against a residual of at most a sixteenth of the step between two folded averages -- four
    // levels of 255 on the worst pair in the building, which is under what the byte it is quantised
    // into can hold apart.
    blend.on = blend.w >= kNodeBlendDeadband;
    return blend;
}

// Two packed rgba8 colours mixed, keeping the first one's alpha.
//
// The alpha is a volumetric fill fraction that halves per level and nothing downstream of the
// marcher reads it -- the composite takes its coverage from the node's per-direction bytes (see
// node_face_coverage) -- so it is left exactly as it was rather than interpolated into a third
// meaning. Rounded rather than truncated: a truncation is a half-level bias downwards on every
// blended pixel and a whole surface drifting a fraction of a per cent darker is precisely the class
// of shift the dither comment warns about.
uint node_colour_mix(uint from, uint towards, float w) {
    const vec3 a = vec3(float(from & 0xFFu), float((from >> 8u) & 0xFFu),
                        float((from >> 16u) & 0xFFu));
    const vec3 b = vec3(float(towards & 0xFFu), float((towards >> 8u) & 0xFFu),
                        float((towards >> 16u) & 0xFFu));
    const uvec3 mixed = uvec3(round(clamp(mix(a, b, w), vec3(0.0), vec3(255.0))));
    return (from & 0xFF000000u) | mixed.x | (mixed.y << 8u) | (mixed.z << 16u);
}

// A node's folded colour, or false where there is nothing worth blending with.
//
// The refusals are the ones the stand-in branch of `node_march` already makes, for the same reasons
// and in the same order: a descent that did not reach the level asked for is answering about a
// differently sized cell, a shell has never been folded from anything so its colour is nought, and a
// node the fold gave up on carries no coverage. "I could not find out" must not become a colour
// (trap 7) -- here it means this pixel keeps exactly the colour it draws today, which is the arm
// this change is measured against.
bool node_folded_colour(Found found, int want, out uint colour) {
    colour = 0u;
    if (found.state != kFoundHere || found.slot == kNoNode || found.level != want) return false;
    const uint packed = nodes.items[found.slot].packed;
    if ((node_flags_of(packed) & kNodeLeaf) == 0 && nodes.items[found.slot].children == kNoNode) {
        return false;
    }
    colour = nodes.items[found.slot].colour;
    return (colour >> 24u) != 0u;
}

// One voxel's own colour, which is what `colour(0)` means where the folded ladder runs out.
//
// The same two clamped table reads as node_medium_through beside it, and the same reason for the
// clamps: an out-of-range index into an SSBO is not a wrong colour, it is whatever the driver
// decides out of bounds means. The low three bytes of the first visual word are the red, green and
// blue `filtered_colour` averages to build every folded colour above this, so the two ends of the
// blend are the same quantity at two scales rather than two conventions.
uint node_type_colour(uint type_id) {
    const uint type_at = min(type_id, uint(types.items.length()) - 1u);
    const uint visual_at = min(types.items[type_at].x, uint(visuals.items.length()) - 1u);
    return visuals.items[visual_at].x & 0xFFFFFFu;
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

// `through_mode` is R4d: what the ray does when it meets matter it can see through, out of the
// three states above.
//
// `kThroughStop` for the primary ray and `kThroughPass` for the light rays, and that division is
// the original design. The primary ray has to stop on the glass, because a face is claimed where a
// pixel lands and the window's own surface -- its sun, its sky, its lamps and its reflection --
// exists only if it is claimed. The light rays have no such need: what a shadow ray wants to know
// is whether the sun reaches, and a window does not stop the sun.
//
// `kThroughExit` is the refraction segment: it enters the glass, attenuates through it, and stops
// at the face where it comes out so the caller can bend it there and march on.
//
// `start_t` is where along the ray the walk begins, in voxels from `origin`, and it is R5d's. A
// caller that wants to know what is BEHIND something could always have moved the origin forward
// instead -- and that is what R4d's refracted segments do -- but moving the origin also RESTARTS THE
// DETAIL CLOCK, because `footprint` is `t * pixel_angle` and `t` is measured from wherever the ray
// began. A second march launched from a silhouette a hundred metres out would then resolve the
// landscape behind it as though the eye were standing on the silhouette: bricks where the pixel
// resolves half a metre, which is eight times finer than anything can be seen and, far worse, eight
// times finer than what the miss reports would then ask the pool to BUILD. Streaming the world
// behind every edge at a resolution nobody can see is exactly the unbounded growth this rewrite
// exists to stop.
//
// Keeping the eye as the origin and skipping forward in `t` keeps every distance in this function
// eye-relative -- the footprint, the world clip, the D156 nudge and the `t` that comes back -- so
// the caller has nothing to add on afterwards and nothing to get wrong.
// ---- R12c: a ray answers for a cell the pool has not built --------------------------------------
//
// The descent reaches a node with a level of nought, the feedback report goes out exactly as it
// always did -- the CPU still has to build it properly -- and instead of drawing the parent's
// folded colour (R2d, D237) the ray asks the FIELD what is there and draws that.
//
// **What it asks is one question per cell, at the cell's centre.** The marching cell is one node of
// the render tree at `outer_level`, and a node is the size of the pixel's own footprint by
// construction, so a single evaluation answers at exactly the resolution the pixel resolves. That
// is also the arithmetic D687 priced the stage with: misses a frame times the cost of one
// evaluation.
//
// **The thin-feature rescue is deliberately NOT here.** `sample_field.comp` runs it because its
// cells are voxels and a mullion half a voxel wide would be deleted outright; a marching cell is a
// whole node, and rescuing at that size would fatten every silhouette by a pixel. What this draws
// for one frame is the surface at the pixel's own resolution, and what the CPU builds behind it is
// unchanged.
//
// **It is bounded, and the bound is a ticket rather than a rule of thumb.** D687: 368,876 nodes
// wanted at the peak on the enclosed camera, four pixels each, 0.84 us an evaluation -- 1,239 ms of
// card in one frame, before a single march step. `--derive-cap` is how many cells one dispatch may
// derive; past it a ray falls back to R2d's stand-in, which is the behaviour this replaces and
// therefore the only safe thing to fall back TO.

const uint kDeriveRefused = 0xFFFFFFFFu;

// What a derivation is CHARGED before it runs, in units of 1024 field nodes, refunded down to what
// it actually cost when it finishes.
//
// **The charge is taken up front and reconciled afterwards, and that is the whole mechanism rather
// than a nicety.** The first version of this budget read what the dispatch had already spent and
// stopped when that passed the ceiling, which sounds equivalent and is not: at 1280x800 the whole
// screen is in flight within a few waves, every one of them reads nought, and every one of them
// proceeds. It measured a **2,279 ms frame at a 64 M-node ceiling and LOST THE DEVICE** -- 70x over
// a budget that should have been 32 ms, because a retrospective budget shared by a million lanes
// bounds nothing at all. Charging first bounds how many derivations may be OUTSTANDING at
// ceiling/charge, whatever they each turn out to cost.
//
// **And the charge is the WORST case rather than the mean, which is the second thing measured
// rather than reasoned.** It was 8 -- D681's measured mean of 8,231 nodes for a cell of the enclosed
// camera -- and the outdoor camera still settled a 2,045 ms frame on a 16 M ceiling and lost the
// device. 16 M over a charge of 8 is 2,048 outstanding derivations, and the ones left to derive
// once a world is mostly built are D688's deep-interior cells at about a million nodes each: two
// thousand of those IS two seconds. At 64 the same ceiling allows 256 outstanding, which is 256 ms
// of the worst cell this clip has, and a cheap cell refunds itself back to about 8 the moment it
// finishes -- so the budget still passes thousands of ordinary cells through a frame.
const uint kDeriveChargeK = 64u;
const uint kDeriveRefund = 0u - kDeriveChargeK;   // wraps, which is what an unsigned atomic wants

const uint kDerivePhaseBounds = 0u;
const uint kDerivePhaseSolid = 1u;
const uint kDerivePhaseRule = 2u;
const uint kDerivePhaseNormal = 3u;
const uint kDerivePhaseDone = 4u;

// Once this invocation has been refused a ticket it stops asking for one. A ray that kept asking
// would spend the rest of its march on atomics that can only fail.
bool g_derive_spent = false;

// One of the six central-difference offsets, as `sub` counts 0..5. `ws_offset` in sample_field.comp.
vec3 node_derive_offset(uint sub, float h) {
    return ws_with_axis(vec3(0.0), sub >> 1u, ((sub & 1u) == 0u) ? h : -h);
}

// How far a point is from a paint rule's box, squared, and from one piece of its zone. Nought
// inside, and nought for the infinite box a rule whose place could not be bounded carries. The same
// two functions sample_field.comp has, because a zone that is the great steps and the cornice wash
// has a bounding box of the whole building and rejects nothing without the pieces.
float node_rule_box_away_sq(uint rule, vec3 p) {
    const float lx = paint_rules.items[rule].lo[0];
    if (lx <= -1.0e29 || paint_rules.items[rule].hi[0] >= 1.0e29) return 0.0;
    const vec3 lo = vec3(lx, paint_rules.items[rule].lo[1], paint_rules.items[rule].lo[2]);
    const vec3 hi = vec3(paint_rules.items[rule].hi[0], paint_rules.items[rule].hi[1],
                         paint_rules.items[rule].hi[2]);
    const vec3 d = max(max(lo - p, p - hi), vec3(0.0));
    return dot(d, d);
}

float node_piece_away_sq(uint piece, vec3 p) {
    const float lx = rule_pieces.items[piece].lo[0];
    if (lx <= -1.0e29 || rule_pieces.items[piece].hi[0] >= 1.0e29) return 0.0;
    const vec3 lo = vec3(lx, rule_pieces.items[piece].lo[1], rule_pieces.items[piece].lo[2]);
    const vec3 hi = vec3(rule_pieces.items[piece].hi[0], rule_pieces.items[piece].hi[1],
                         rule_pieces.items[piece].hi[2]);
    const vec3 d = max(max(lo - p, p - hi), vec3(0.0));
    return dot(d, d);
}

bool node_rule_could_apply(uint rule, vec3 p) {
    if (node_rule_box_away_sq(rule, p) > 0.0) return false;
    const uint from = paint_rules.items[rule].piece_from;
    const uint to = paint_rules.items[rule].piece_to;
    if (from == to) return true;
    for (uint piece = from; piece < to; ++piece) {
        if (node_piece_away_sq(piece, p) <= 0.0) return true;
    }
    return false;
}

// What the field says is at `p` metres, with `h` the cell's own size in metres.
//
// A LITTLE MACHINE AND NOT STRAIGHT-LINE CODE, for the reason sample_field.comp's header gives at
// length: GLSL has no out-of-line call, the build compiles with `-O`, and a helper containing a
// call is a private COPY of everything it calls -- including `field_eval`'s 128-frame stack. There
// is exactly one call to `field_eval` in this function and it must stay that way (D677).
//
// `solid` and the returned type are separate answers: a clip with no paint rules at all has matter
// and type nought, and a cell of air has neither. Conflating them is D133's shape in a new place.
uint node_derive_type(vec3 p, float h, out uint asks, out bool solid) {
    asks = 0u;
    solid = false;
    ws_field_visits = 0u;
    ws_field_refused = 0u;
    ws_field_refused_op = 0u;

    uint phase = (field_push.has_bounds != 0u) ? kDerivePhaseBounds : kDerivePhaseSolid;
    uint sub = 0u;
    uint rule = 0u;
    uint type = 0u;
    vec3 hold = vec3(0.0);
    vec3 away = vec3(0.0);
    bool have_normal = false;

    // 1 for the bounds, 1 for the shape, one per rule, six for a normal, and slack. The same shape
    // of guard sample_field.comp uses, and for the same reason: a card is the wrong place to
    // discover a loop that does not end.
    const uint guard = 16u + 2u * field_push.rule_count;
    for (uint turn = 0u; turn < guard && phase != kDerivePhaseDone; ++turn) {
        // ---- and ONE cell is bounded too, which is the third thing this had to be taught ------
        //
        // The guard above bounds the number of QUESTIONS at 16 + 2 x 628, and every one of those is
        // a full `field_eval` that may itself walk WS_FIELD_TURNS. So a single derivation was
        // bounded at about 1,272 evaluations of a million nodes, and the two budgets outside this
        // function -- cells, then field nodes charged before the walk -- both went on losing the
        // device because neither of them bounds one cell. Measured: 256 outstanding derivations on
        // a 16 M ceiling still made a **2,158 ms frame** on the settled outdoor camera, which is
        // 8 ms a derivation.
        //
        // A cell that has walked its own charge stops asking and answers with what it has. The
        // shape is what matters and it is the FIRST question; what is given up is some of the
        // weathering, on a picture that lasts one frame and is replaced by the CPU's own.
        if (ws_field_visits > kDeriveChargeK << 10u) {
            // ...unless the shape itself is not known yet, in which case there is no answer to
            // give and the stand-in is the honest one.
            if (phase == kDerivePhaseBounds || phase == kDerivePhaseSolid) return kDeriveRefused;
            break;
        }
        uint ask_node = field_push.root;
        vec3 ask_at = p;
        if (phase == kDerivePhaseBounds) {
            ask_node = field_push.bounds;
        } else if (phase == kDerivePhaseNormal) {
            ask_at = p + node_derive_offset(sub, h);
        } else if (phase == kDerivePhaseRule) {
            // Whatever the boxes already answer for, without asking the field.
            while (rule < field_push.rule_count && !node_rule_could_apply(rule, p)) ++rule;
            if (rule >= field_push.rule_count) break;
            ask_node = paint_rules.items[rule].test;
        }

        // ---- THE ONE CALL --------------------------------------------------------------------
        const float v = field_eval(ask_node, ask_at);
        ++asks;
        // A refusal is not a value and must not read as air: the walk ran out of stack or out of
        // turns and nobody computed this cell. The caller falls back to the stand-in, which is a
        // coarse answer rather than no answer. D676, from the other side.
        if (ws_field_refused != 0u) return kDeriveRefused;

        if (phase == kDerivePhaseBounds) {
            if (v > 0.0) return 0u;   // outside the clip's own shape: none of its business
            phase = kDerivePhaseSolid;
        } else if (phase == kDerivePhaseSolid) {
            if (v > 0.0) return 0u;   // air at this cell's centre
            solid = true;
            phase = kDerivePhaseRule;
        } else if (phase == kDerivePhaseRule) {
            const float low = paint_rules.items[rule].low;
            const float high = paint_rules.items[rule].high;
            if (v < low || v > high) {
                ++rule;
            } else if (paint_rules.items[rule].slack >= 1.0e30 &&
                       paint_rules.items[rule].facing_axis < 3u && !have_normal) {
                // Only a rule the descent could settle for NOTHING asks which way a surface faces
                // -- see GpuPaintRule::slack. Six more evaluations, once a cell at most.
                phase = kDerivePhaseNormal;
                sub = 0u;
            } else {
                if (paint_rules.items[rule].slack >= 1.0e30 &&
                    paint_rules.items[rule].facing_axis < 3u) {
                    const float component = ws_axis_of(away, paint_rules.items[rule].facing_axis);
                    const float want = paint_rules.items[rule].facing_min;
                    const bool passes = (want >= 0.0) ? (component >= want) : (component <= want);
                    if (!passes) { ++rule; continue; }
                }
                type = paint_rules.items[rule].type;
                ++rule;
            }
            if (phase == kDerivePhaseRule && rule >= field_push.rule_count) {
                phase = kDerivePhaseDone;
            }
        } else if (phase == kDerivePhaseNormal) {
            const uint axis = sub >> 1u;
            if ((sub & 1u) == 0u) {
                hold = ws_with_axis(hold, axis, v);
            } else {
                away = ws_with_axis(away, axis, ws_axis_of(hold, axis) - v);
            }
            ++sub;
            if (sub >= 6u) {
                // `forge::normalise`'s own answer for a zero vector, which is {0,1,0}: a rule keyed
                // on "up" then matches, which is what the CPU does.
                const float len = length(away);
                away = (len > 0.0) ? (away / len) : vec3(0.0, 1.0, 0.0);
                have_normal = true;
                phase = kDerivePhaseRule;   // back to the rule that asked, with `rule` unchanged
            }
        }
    }

    // Matter with no rule that matched is still matter, and a hole in a wall is harder to notice
    // than a wrong colour. sample_field.comp's last two lines.
    if (solid && type == 0u && field_push.rule_count > 0u) type = field_push.first_type;
    return type;
}

// The dispatch's budget, handed out one cell at a time. Returns false once it is gone, and says so
// in the counters rather than silently drawing something cheaper.

bool node_derive_take() {
    if (g_derive_spent) return false;
    const uint slot = push.derive.z;
    // The WORK budget first, because it is the one that keeps a frame under the driver's watchdog.
    if (push.derive.w != 0u) {
        const uint spent = atomicAdd(derive_stats.slots[slot].visits_k, kDeriveChargeK);
        if (spent >= push.derive.w) {
            atomicAdd(derive_stats.slots[slot].visits_k, kDeriveRefund);
            g_derive_spent = true;
            atomicAdd(derive_stats.slots[slot].capped, 1u);
            return false;
        }
    }
    const uint before = atomicAdd(derive_stats.slots[slot].derived, 1u);
    if (before < push.derive.y) return true;
    g_derive_spent = true;
    atomicAdd(derive_stats.slots[slot].capped, 1u);
    return false;
}

NodeHit node_march(vec3 origin, vec3 dir, float pixel_angle, float dither, bool report,
                   bool report_used, bool report_face, bool stand_in, bool occlude_unknown,
                   uint through_mode, float max_t, float start_t) {
    NodeHit result;
    result.hit = false;
    result.unknown = false;
    result.t = push.lens.y;
    result.normal = ivec3(0, 1, 0);
    result.type_id = 0u;
    result.colour = 0u;
    result.coverage = 255u;
    result.through = vec3(1.0);
    result.left_medium = false;
    result.exit_t = 0.0;
    result.exit_normal = ivec3(0, 1, 0);
    result.crossed = 0.0;
    result.coverage_exit_t = 0.0;
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

    // Whether the ray is currently INSIDE transmissive matter, which has to outlive the brick it
    // entered in: a pane is four voxels thick and a brick is eight, so a ray crossing one at an
    // angle enters in one brick and comes out in the next often enough to matter.
    bool in_medium = false;

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

    // R5d's near bound, applied after the clip so a start past the world is a miss rather than a
    // walk. A ray asked to begin beyond `limit` runs no steps at all and comes back as it would
    // from open sky, which is what it is: everything this ray was allowed to look at is behind it.
    t = max(t, start_t);

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
        //
        // R5c keeps every word of that and takes the COLOUR off the choice: `detail` is the
        // continuous level, the dither still picks which cell the ray stops on, and the hit blends
        // its folded colour towards the other member of the pair by the fraction between them. See
        // node_level_blend, and note that `detail` is named here rather than recomputed at the hit
        // so the two cannot drift apart -- the pair the colour blends between has to be the pair the
        // level was chosen from.
        float footprint = max(t * pixel_angle * push.lens.z, 1.0);
        float detail = log2(footprint);
        int level = clamp(int(floor(detail + dither)), 0, kNodeMaxDetail);
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
                // R5d: and where the ray comes out the other side of this same cell, which costs
                // two compares because the DDA has already worked it out. `t_max` holds the t of
                // the next boundary on each axis for the cell being tested, so the smallest of the
                // three is where the ray leaves it -- and it is the cell the coverage was read off,
                // because a node that is not a leaf can only be reached here at exactly the level
                // the walk asked for (node_descend breaks above `target` only on a LEAF, which the
                // line above has already excluded).
                //
                // Computed unconditionally rather than under `coverage < 255`, because a branch on
                // a value this late costs more than the two compares it would save on a path that
                // runs at most once per ray.
                result.coverage_exit_t = min(min(t_max.x, t_max.y), t_max.z);
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

                // ---- R5c: and towards the colour of the level this pixel did NOT take ------
                //
                // Last, after everything that has to be reported, and that is not tidiness. The
                // locate below can re-seed this invocation's entry-block cache, and `g_node_block`
                // is what `node_flush_used` sends to residency -- so a blend placed above it would
                // change which root a sampled pixel says it is using, and the two arms of the A/B
                // would differ by a residency report as well as by a colour.
                //
                // `outer_level` is `level` here -- a hit on a node that is not a leaf is above the
                // brick level, where the two are equal by construction -- so the pair the dither
                // chose from is the pair this blends between.
                //
                // The two directions cost very different things and that is why they are written
                // out rather than folded into one lookup. The parent is a fresh `node_locate` from
                // whatever ancestor this invocation has cached, which is the price this change
                // actually has, and it is why the deadband exists. The CHILD is one more turn of
                // the descent that has already been made, entered at the node in hand -- two loads
                // and a mask test -- so the arm that carries the greater weight is the cheap one.
                //
                // The child is taken at the point the ray ENTERED the cell rather than at its
                // corner, clamped back into the cell so a nudge at ninety thousand voxels cannot
                // leave it (D156 is the same arithmetic on the skip step). It is not the child a
                // finer marcher would have stopped on -- that one is only knowable by marching
                // finer, which is the whole cost this level exists to avoid -- but it is a child of
                // this cell, and the fold means the parent is the average of exactly these.
                const NodeBlend blend = node_level_blend(detail, level, stand_in);
                if (blend.on) {
                    uint other;
                    bool have;
                    if (blend.want > level) {
                        have = node_folded_colour(node_locate(voxel, blend.want), blend.want, other);
                    } else {
                        const vec3 inside = origin + dir * (t + max(1e-3, t * 1e-5));
                        const ivec3 at = clamp(origin_voxel + ivec3(floor(inside)), voxel,
                                               voxel + (1 << outer_level) - 1);
                        have = node_folded_colour(node_descend(at, blend.want, found.slot, level),
                                                  blend.want, other);
                    }
                    if (have) result.colour = node_colour_mix(result.colour, other, blend.w);
                }
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
                const bool solid = leaf_voxel_solid(leaf, inner);
                // ---- R4d: the face where the ray comes OUT of the glass -----------------------
                //
                // Read one cell at a time like everything else here, and detected on the cell AFTER
                // the medium rather than on the last cell of it, because that is the only way to
                // know the medium has ended: a transmissive voxel with another behind it is the
                // middle of a pane and a transmissive voxel with air behind it is its far face.
                //
                // `inner_normal` at this point is the boundary just crossed and points back against
                // the ray, into the medium -- which is the sign `refract` wants at an exit.
                if (in_medium && !solid) {
                    in_medium = false;
                    result.left_medium = true;
                    result.exit_t = max(t_inner, 0.0);
                    result.exit_normal = inner_normal;
                    if (through_mode == kThroughExit) {
                        node_flush(report, has_pending, pending);
                        return result;
                    }
                }
                if (solid) {
                    // ---- R4d: matter a ray can see through -------------------------------
                    //
                    // Asked at the LEAF and only at the leaf, because a coarse node stands over as
                    // many as 512 voxels which need not agree about being glass -- the same reason
                    // `face_material` answers NONE above level 0. And asked only when the caller
                    // wants it, so the primary ray's inner loop is byte for byte what it was.
                    //
                    // What it costs when it fires is one table lookup and one more step of a DDA
                    // that was already running. What it costs when it does not fire is a branch on
                    // a uniform.
                    // ---- R4e: matter a ray can scatter through ---------------------------
                    //
                    // The same step, over voxels that are not transmissive at all. What separates
                    // it from the branch below is what it accumulates: a distance rather than a
                    // tint, and the ray gives up on a budget of MATTER rather than on one of
                    // brightness. Everything else -- the leaf gate, the DDA, the cost when it does
                    // not fire -- is the same.
                    if (through_mode == kThroughSolid && level == 0) {
                        const float was = t_inner;
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
                        result.crossed += max(t_inner - was, 0.0);
                        // Out of matter to give: past this the ray is extinguished, and an
                        // extinguished ray is a stopped one. Reported as a hit rather than as a
                        // miss, because "I was swallowed by a metre of stone" and "I reached the
                        // sky" are the two answers this mode exists to tell apart (trap 7).
                        if (result.crossed >= kCrossedMax) {
                            result.hit = true;
                            result.t = max(t_inner, 0.0);
                            result.normal = inner_normal;
                            result.level = level;
                            node_flush(report, has_pending, pending);
                            return result;
                        }
                        if (t_inner > limit) break;
                        continue;
                    }
                    if (through_mode != kThroughStop && level == 0) {
                        const vec3 past = node_medium_through(leaf_voxel_type(leaf, inner));
                        if (past.r + past.g + past.b > 0.0) {
                            result.through *= past;
                            // Inside it now, so the next cell that is not glass is the far face.
                            in_medium = true;
                            // Bounded, or a ray down the length of a pane would multiply a hundred
                            // times and a ray inside a solid block of glass would never stop at all.
                            // Below this the medium has taken everything and the ray has its answer.
                            if (result.through.r + result.through.g + result.through.b > 0.02) {
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
                                continue;
                            }
                        }
                    }
                    result.hit = true;
                    result.t = max(t_inner, 0.0);
                    result.normal = inner_normal;
                    result.level = level;
                    if (level == 0) {
                        // A single voxel is all of itself.
                        result.type_id = leaf_voxel_type(leaf, inner);
                        result.coverage = 255u;
                        // ...and the brick's average colour beside it, which the primary ray does
                        // not use and a GATHERING ray does. A bounce needs an albedo for the
                        // surface it landed on and cannot reach the interned tables from the face
                        // pass, which does not have them bound. Twenty-five centimetres of average
                        // is the right resolution for light arriving from a surface rather than for
                        // the surface itself, and it costs nothing: `leaf_voxel_type` on the line
                        // above has already fetched this LeafHeader, and `average_colour` is a
                        // field of the same struct.
                        result.colour = leaves.items[leaf].average_colour;
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

                    // ---- R5c, inside the brick, where the ladder has two rungs and no more ----
                    //
                    // After the reports, for the reason the coarse hit above records: the locate
                    // can re-seed the entry-block cache `node_flush_used` has just sent.
                    //
                    // The pool holds a voxel and it holds a brick and it holds nothing in between,
                    // so of the pairs a footprint under eight voxels can sit in, only two have two
                    // different colours in them at all: (0, 1), whose fine end is the voxel's own
                    // colour, and (3, 4), whose coarse end is the first folded node above the
                    // brick. Between them `colour(1)`, `colour(2)` and `colour(3)` are one number
                    // -- this brick's average -- so a blend there would fetch a colour in order to
                    // mix a colour with itself. Skipped by naming the two levels rather than by
                    // comparing the colours afterwards, because the fetch is the whole cost.
                    //
                    // The voxel is the one the ray STOPPED on, which is the voxel a level-0 pixel
                    // of the same tile stopped on: the inner walk marches single voxels whatever
                    // the level, so the two arms of the (0, 1) pair differ in colour and in
                    // coverage and never in which voxel they found (D132). That is what makes this
                    // end of the blend exact rather than representative.
                    //
                    // A level-0 hit is left alone and cannot be anything else from here -- the
                    // composite reads a type id at that level and this word is not what it draws.
                    if (level > 0) {
                        const NodeBlend near = node_level_blend(detail, level, stand_in);
                        uint other;
                        if (near.on && near.want == 0) {
                            other = node_type_colour(leaf_voxel_type(leaf, inner));
                            result.colour = node_colour_mix(result.colour, other, near.w);
                        } else if (near.on && near.want > kLeafLevel &&
                                   node_folded_colour(node_locate(voxel, near.want), near.want,
                                                      other)) {
                            result.colour = node_colour_mix(result.colour, other, near.w);
                        }
                    }
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

                // ---- R12c: derive it rather than stand in ------------------------------------
                //
                // Everything above this line is untouched: the feedback report has already gone
                // out, because the CPU still has to build this node properly and a ray that drew
                // its own answer and said nothing would be a building that never streams.
                //
                // Gated on `stand_in` AND on the ray not seeing through matter, and both halves of
                // that are about register budget rather than taste. `node_march` is inlined at
                // every call site -- ten of them across the two passes that include this file --
                // and each copy carries its own 128-frame field stack (D677, seventeen stacks and
                // 1.05 MB of SPIR-V). `stand_in` is a compile-time FALSE at all four of the face
                // pass's call sites and `through_mode == kThroughStop` at three of the six in the
                // visibility pass, so the two together leave three copies instead of ten. It is
                // also the right rule on its own terms: the stand-in is what a primary ray draws,
                // and a segment inside glass has R4d's own answer for a cell it cannot see.
                bool derived_empty = false;
                if (stand_in && through_mode == kThroughStop && push.derive.x != 0u &&
                    node_derive_take()) {
                    // The cell's centre, in metres. `voxel` is the absolute voxel coordinate of the
                    // cell's low corner and the world is kVoxelsPerMetreF voxels to the metre, which
                    // is the same grid `forge::sample` turns a box in metres into -- so the two
                    // agree to the integer rather than to a rounding.
                    const float span = float(1 << outer_level);
                    const vec3 at_m = (vec3(voxel) + 0.5 * span) * (1.0 / kVoxelsPerMetreF);
                    uint asks = 0u;
                    bool derived_solid = false;
                    const uint derived_type =
                        node_derive_type(at_m, span * (1.0 / kVoxelsPerMetreF), asks,
                                         derived_solid);
                    atomicAdd(derive_stats.slots[push.derive.z].evaluations, asks);
                    // `ws_field_visits` accumulates across every call `node_derive_type` made,
                    // because it is zeroed once at the top of it and by nothing after. The charge
                    // taken before the walk is handed back in the same add, so what the counter
                    // holds is what was actually walked and the budget corrects itself upward the
                    // moment a cell turns out to be one of D688's expensive ones.
                    atomicAdd(derive_stats.slots[push.derive.z].visits_k,
                              (ws_field_visits >> 10u) + kDeriveRefund);
                    if (derived_type != kDeriveRefused && derived_solid) {
                        ivec3 derived_normal = last_normal;
                        if (derived_normal == ivec3(0)) {
                            // First step, camera inside the cell: no face was crossed to get here,
                            // so face the ray, exactly as the stand-in below does.
                            vec3 a = abs(dir);
                            int dominant = (a.x > a.y && a.x > a.z) ? 0 : ((a.y > a.z) ? 1 : 2);
                            derived_normal = ivec3(0);
                            derived_normal[dominant] = -step_dir[dominant];
                        }
                        result.hit = true;
                        result.t = t;
                        result.normal = derived_normal;
                        result.type_id = derived_type;
                        result.colour = node_type_colour(derived_type);
                        result.coverage = 255u;
                        result.level = min(outer_level, kNodeMaxDetail);
                        node_flush(report, has_pending, pending);
                        node_flush_used(report_used, g_node_block);
                        node_flush_read(report_used && found.slot != kNoNode, found.slot);
                        node_face_hit(result, report_face, voxel, outer_level, derived_normal);
                        return result;
                    }
                    // Air, and that is an ANSWER rather than an absence: the ray carries on and
                    // may derive the next cell, which is what makes a derived first frame a
                    // building against sky instead of R2d's blob against sky. A refusal is not
                    // air and falls through to the stand-in below.
                    derived_empty = derived_type != kDeriveRefused;
                }

                bool foldable = (node_flags_of(nodes.items[found.slot].packed) & kNodeLeaf) != 0 ||
                                nodes.items[found.slot].children != kNoNode;
                if (!derived_empty && stand_in && found.level - 1 == outer_level &&
                    found.slot != kNoNode &&
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

            // R4d, and it is the other half of the exit test in the leaf loop: the medium can also
            // end where the BRICK does, when the cell on the far side of the pane holds nothing at
            // all and so has no leaf to step through. The interface is then the brick boundary the
            // walk last crossed, which is a coarser normal than the voxel one and is the same face
            // in every case that matters -- a pane's far side is flat and axis-aligned.
            if (in_medium) {
                in_medium = false;
                result.left_medium = true;
                result.exit_t = max(t, 0.0);
                result.exit_normal = (last_normal == ivec3(0)) ? ivec3(0, 1, 0) : last_normal;
                if (through_mode == kThroughExit) {
                    node_flush(report, has_pending, pending);
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


