// Walking the node pool: one sparse octree, at every scale, with one answer.
//
// This replaces the four addressing schemes shaders/world.glsl walks end to end â€” a wrapped chunk
// grid, a per-chunk brick mask with popcount prefixes, a per-chunk brick-mask pyramid, and a
// separate summary tier with its own grids. See src/world/node_pool.hpp for why they went.
//
// # The shape of a step
//
// One hash to enter the tree, and pure arithmetic below it: a node holds the base index of its
// eight contiguous children, so descending is `children + octant`. The old walk paid two
// *dependent* loads per chunk entered â€” the wrapped grid cell, then that record's own coordinate,
// because the grid wraps and a cell may be held by a chunk from somewhere else â€” and did it up to
// thirty-two times along each axis a ray crossed, plus a coarse-grid fetch per skip. A dependent
// load is the one thing a marcher cannot hide, and on a machine whose binding constraint is memory
// bandwidth that difference is the point of the exercise.
//
// # Three answers, not two
//
// A descent stops for one of three reasons and they drive three different behaviours:
//
//   HERE      a node covers this point at the level asked for. Draw it.
//   EMPTY     the child cell one level down holds nothing. Jump the width of it â€” the coarse skip
//             falls out of the descent instead of needing five occupancy grids to carry it.
//   WANTED    the world has something here and the pool does not. Report it; that is the only
//             reason anything is ever streamed.
//
// Conflating EMPTY and WANTED is the fault this structure exists to make unrepresentable. An
// unstreamed building that reads as open sky is never reported, so it is never streamed, so it
// stays open sky â€” which is decision D133 and again D147, found twice by looking at photographs.
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
const int kEntryLevel = 14;     // 16,384 voxels â€” 512 m. Must match src/world/node_pool.hpp.

const uint kNodeLeaf = 1u;
const uint kNodeUniform = 2u;

// Thirty-two bytes, declared as scalars rather than vectors on purpose. std430 aligns a uvec3 to
// sixteen bytes, so declaring the coordinate the obvious way silently reads every node after the
// first from the wrong offset â€” the same trap the light list documents in pathtrace.comp.
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
// the CPU â€” a probe that walked past one would find a root that a later insertion had displaced.
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

// Two words a face, and they are two different quantities that must not be averaged into one.
//
//   [0] the FAR field -- sky visibility. Unbounded. Rays cast and rays that escaped, packed as
//       GpuFace::counters is, so face_accumulate serves it. It multiplies sky radiance, and it is
//       the physically missing term in the ambient integral.
//   [1] the NEAR field -- contact. The same ray's FIRST HIT DISTANCE through a falloff over about
//       a metre, summed in fixed point at 255 a ray.
//
// One ray answers both: a hit at 0.3 m says "not sky" and "contact"; a ray that escapes says "sky"
// and "no contact". They are separated because indoors the far field saturates -- every ray hits
// something, so sky visibility is nought on every surface in the room and carries no shape at all.
// Measured: 1,619 of 1,671 surface pixels in the enclosed view fell in the lowest tenth. The near
// field is what varies there, and it is what reads as ambient occlusion.
//   [2] the near field's GRADIENT along the face's first axis, and [3] along its second.
//
// Those two are what make occlusion vary UNDER a voxel. Kept as first moments of the same samples
// rather than fitted afterwards: the jitter is uniform over the sampled span, so the Legendre basis
// on [-1, 1] is already orthogonal under it -- <P_i P_j> = d_ij/(2i+1) -- and every coefficient is
// therefore an independent running mean of the sample weighted by a polynomial. No normal
// equations, no least squares, no second pass, and no extra rays: the face pass already chooses a
// point on the face and was throwing its position away.
//
// A plane fits to a constant, a corner to a gradient. Nothing in the code has to know which it is
// looking at, because what is being fitted is the real visibility field.
const uint kFaceLightWords = 4u;
// How far away geometry stops darkening, in METRES rather than voxels, so a coarse face at 200 m
// and a level-0 face at arm's length darken over the same physical distance and a dolly-in shows
// no transition.
const float kContactMetres = 1.0;
const float kVoxelsPerMetreF = 32.0;

const uint kNoFace = 0xFFFFFFFFu;
const uint kFaceTombstone = 0xFFFFFFFEu;

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

// The same, for the ambient term, and it is eight times longer for a reason that does not apply to
// the sun: **ambient occlusion depends only on geometry**. The sun moves across the sky and a face
// has to be able to forget where it used to be, which is what a 256-sample window buys. Geometry
// moves only when somebody edits it, and since an edit now tells the faces inside it to start again
// from nothing rather than leaving them to average their way back (cebf015), the ambient window is
// not a forgetting mechanism at all -- it exists to stop the counters overflowing.
//
// So it can be as long as the counters allow, and length is variance: the error of a fraction over
// N samples falls as one over the square root of N, and the window IS N. At 256 a contact fraction
// near a half has a standard deviation of about 8 of 255, which is exactly the face-to-face
// roughness measured on a flat wall -- the cap was the noise floor. At 2,048 it is under 3.
//
// The counters hold it: samples and the lit count are sixteen bits each and 2,048 is well inside
// them, and the contact sum and its two gradients are full words at 255 a sample, which is 522,240
// at the cap against four thousand million.
const uint kSkyWindow = 2048u;

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

// What fraction of this face's rays reached the sun. One before any have been cast, so a face
// nobody has shaded yet is lit rather than black -- the composite gates on the sample count and
// never sees this, but a caller that forgets to should fail towards the picture it already had.
float face_visibility_of(uint counters) {
    uint samples = face_samples_of(counters);
    return samples == 0u ? 1.0 : float(face_lit_of(counters)) / float(samples);
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
// The first version kept the whole descent â€” a twelve-element array indexed by level, so a step
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
// A single root, held in two scalars, costs registers and stays in them. It saves the hash â€” which
// is the one genuinely expensive part, since a ray crosses few 512 m blocks â€” and the descent
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

uint g_node_root = kNoNode;
ivec3 g_node_block = ivec3(0x7FFFFFFF);
uint g_node_mid = kNoNode;
ivec3 g_node_mid_block = ivec3(0x7FFFFFFF);
uint g_node_fine = kNoNode;
ivec3 g_node_fine_block = ivec3(0x7FFFFFFF);

void node_walk_reset() {
    g_node_root = kNoNode;
    g_node_block = ivec3(0x7FFFFFFF);
    g_node_mid = kNoNode;
    g_node_mid_block = ivec3(0x7FFFFFFF);
    g_node_fine = kNoNode;
    g_node_fine_block = ivec3(0x7FFFFFFF);
}

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
        // every frame while every counter reads calm â€” measured at 3.7 million requests a run
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
// streaming converges front to back â€” which is also the order a player notices.

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
NodeHit node_march(vec3 origin, vec3 dir, float pixel_angle, float dither, bool report,
                   bool report_used, bool report_face, bool stand_in, bool occlude_unknown) {
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

    node_walk_reset();

    vec3 inv_dir = 1.0 / max(abs(dir), vec3(1e-9));
    ivec3 step_dir = ivec3(sign(dir));
    ivec3 origin_voxel = push.camera_chunk.xyz * 256;

    float t = 0.0;

    // Clipped to the extent of the world, and this is not slack.
    //
    // Nothing outside it can be hit, so stepping through it looking is pure loss — and the loss is
    // not small, because a ray that will never hit anything keeps stepping until its budget runs
    // out. D148 measured exactly this on the old marcher: widening the clip took a frame from
    // 0.78 ms to 23.7 ms. This marcher shipped without the clip at all and cost 12.0 ms against
    // the old one's 1.6, with the tell being that the cost scaled linearly with the step budget —
    // 12.0 ms at 512 steps, 3.3 at 128, 1.0 at 32. A marcher whose cost is proportional to its
    // budget is a marcher that never terminates.
    float limit = push.lens.y;
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
            // Re-seed the DDA at the new granularity. A handful of times per ray â€” once per level
            // boundary crossed â€” not once per step.
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
                node_flush_read(report_used, found.slot);
                node_face_hit(result, report_face, voxel, outer_level, last_normal);
                return result;
            }

            // Inside the brick, always at single voxels â€” deliberately, even when the pixel is
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
                    node_flush_read(report_used, found.slot);
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
                    // arithmetic is written against (§6: a voxel covers a whole pixel at 22.5 m,
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
        } else {
            if (found.state == kFoundWanted) {
                // The world has something here and the pool does not. Report the node at the
                // level the descent stopped at, which is the coarsest thing that is missing â€” so
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
            // empty cell is exactly one marching cell â€” and taking the analytic path for that
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


