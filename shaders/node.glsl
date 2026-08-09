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
layout(push_constant) uniform NodeConstants {
    uint entry_capacity;
    uint entry_probes;
    uint pad_a;
    uint pad_b;
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
void node_flush_face(bool enabled, ivec3 voxel, int level, ivec3 normal) {
    if (!enabled) return;
    uint face;
    if (normal.x != 0) face = (normal.x > 0) ? 0u : 1u;
    else if (normal.y != 0) face = (normal.y > 0) ? 2u : 3u;
    else face = (normal.z > 0) ? 4u : 5u;

    uint index = atomicAdd(feedback.count, 1u);
    if (index < push.resolution.w) {
        feedback.entries[index].coord =
            ivec4(voxel.x >> level, voxel.y >> level, voxel.z >> level,
                  (level & 0xFF) | int(face << 8) | kFeedbackFace);
    }
}

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
    float t;
    ivec3 normal;
    uint type_id;    // valid when level == kLeafLevel and the voxel resolved
    uint colour;     // rgba8, valid above the leaf
    uint coverage;   // how much of the cell is matter AS SEEN ALONG the face that was hit
    int level;
    uint steps;
};

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

const uint kNodeMaxSteps = 512u;

NodeHit node_march(vec3 origin, vec3 dir, float pixel_angle, float dither, bool report,
                   bool report_used, bool report_face) {
    NodeHit result;
    result.hit = false;
    result.t = push.lens.y;
    result.normal = ivec3(0, 1, 0);
    result.type_id = 0u;
    result.colour = 0u;
    result.coverage = 255u;
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
                node_flush_face(report_face, voxel, outer_level, last_normal);
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
                    node_flush_face(report_face, voxel, outer_level, inner_normal);
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
                bool foldable = (node_flags_of(nodes.items[found.slot].packed) & kNodeLeaf) != 0 ||
                                nodes.items[found.slot].children != kNoNode;
                if (found.level - 1 == outer_level && found.slot != kNoNode && foldable &&
                    (nodes.items[found.slot].colour >> 24) != 0u) {
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
                    node_flush_face(report_face, voxel, outer_level, stand_normal);
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


