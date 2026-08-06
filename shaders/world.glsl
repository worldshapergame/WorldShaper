// Shared world traversal: the buffers the marcher walks, and the march itself.
//
// Included by every pass that has to ask what is at a point in the world - primary
// visibility and the path tracer both. One copy, because two copies of a DDA drift, and the
// one that drifts is whichever is not being looked at that week.
//
// Declares bindings 2 upward. Each including shader owns bindings 0 and 1 for its own images,
// and its own local_size and main().

//
// documentation/04-rendering.md Ã‚Â§1 and documentation/11-reality-check.md Ã‚Â§1.
//
// The ray marches at whatever granularity the *pixel* needs, and coarsens as it gets
// further away. A pixel covers an angle; at distance t it covers t*angle voxels; so the
// cell the ray should be stepping is that size, and anything finer is work whose result
// cannot be seen. Cell sizes are powers of two from one voxel up to 128:
//
//   level 0-2   1, 2, 4 voxels   inside a brick, using the brick's occupancy mips
//   level 3     8 voxels         a brick, using the chunk's brick mask
//   level 4-7   16 - 128 voxels  the chunk's brick-mask pyramid
//
// Consequences, which are the whole point:
//   - there is no render distance. A mountain 40 km away costs the same handful of steps
//     as a rock 4 m away covering the same pixels.
//   - cost is bounded by screen resolution, not by world size.
//   - detail is never reduced above one pixel. A voxel is drawn exactly when it can be
//     seen, which is the physical maximum.
//
// The level is a *continuous* number, and the fractional part picks between the two
// neighbouring levels with an ordered dither. So there is no discrete transition anywhere
// in the maths Ã¢â‚¬â€ not one small enough to hide, but none at all (answer N2).



struct ChunkRecord {
    ivec3 coord;
    uint mask_offset;      // in 64-bit words
    uint prefix_offset;    // in 32-bit entries
    uint slot_base;
    uint brick_count;
    uint average_colour;
};

struct BrickHeader {
    uint payload_offset;
    uint palette_count;
    uint packed;           // index_bits | flags << 8
    uint uniform_type;
    uvec2 mip_cell2;       // 4x4x4 occupancy, 64 bits
    uint mip_cell4;        // 2x2x2 occupancy, low 8 bits
    uint average_colour;
};

layout(std430, binding = 2) readonly buffer Grid { uint cells[]; } grid;
layout(std430, binding = 3) readonly buffer Records { ChunkRecord items[]; } records;
layout(std430, binding = 4) readonly buffer Masks { uint words[]; } masks;
layout(std430, binding = 5) readonly buffer Prefixes { uint values[]; } prefixes;
layout(std430, binding = 6) readonly buffer Headers { BrickHeader items[]; } headers;
layout(std430, binding = 7) readonly buffer Occupancy { uint words[]; } occupancy;
layout(std430, binding = 8) readonly buffer Payload { uint words[]; } payload;
layout(std430, binding = 9) readonly buffer Coarse { uint flags[]; } coarse;

// The second residency tier: one thumbnail per chunk, 8x8x8 cells of a cubic metre each,
// holding a filtered colour and how full the cell is. Two kilobytes against a chunk's
// megabytes, which is what lets the world carry on past the range full detail can reach.
// See world/thumbnail.hpp and world/thumb_cache.hpp.
layout(std430, binding = 10) readonly buffer ThumbGrid { uint slots[]; } thumb_grid;
layout(std430, binding = 11) readonly buffer Thumbs { uint words[]; } thumbs;

struct FeedbackEntry {
    ivec4 coord;   // xyz chunk coordinate, w the detail level the ray was at
};

layout(std430, binding = 12) buffer Feedback {
    uint count;
    uint pad0;
    uint pad1;
    uint pad2;
    FeedbackEntry entries[];
} feedback;

// std140. See gpu/render_params.hpp for why this is not push constants.
layout(std140, binding = 13) uniform Params {
#include "params.glsl"
} push;

const uint kNoRecord = 0xFFFFFFFFu;
const uint kNoOffset = 0xFFFFFFFFu;
const int kChunkEdge = 256;
const uint kMaxSteps = 512u;
const int kMaxLevel = 7;    // 128-voxel cells

// Word offsets of each level of the chunk's brick-mask pyramid. Must match
// kChunkMipOffset in world/residency.hpp.
const uint kMipOffset[5] = uint[5](0u, 512u, 576u, 584u, 585u);

// World occupancy, from a single chunk up to blocks of 256. Level 0 is per chunk and
// answers "does the world have anything here"; the rest are for skipping empty space in
// bigger jumps. Must match kCoarseFactor and the grid dimensions in world/residency.hpp.
const int kCoarseLevels = 5;
const int kCoarseFactor[5] = int[5](1, 4, 16, 64, 256);
const ivec3 kCoarseDims = ivec3(64, 64, 64);
const uint kCoarseCellsPerLevel = 64u * 64u * 64u;

int floor_div(int value, int divisor) {
    return (value >= 0) ? (value / divisor) : -(((-value) + divisor - 1) / divisor);
}

int floor_mod(int value, int modulus) {
    int result = value % modulus;
    return (result < 0) ? result + modulus : result;
}

uint find_record(ivec3 chunk_coord) {
    ivec3 g = ivec3(floor_mod(chunk_coord.x, int(push.grid_dims.x)),
                    floor_mod(chunk_coord.y, int(push.grid_dims.y)),
                    floor_mod(chunk_coord.z, int(push.grid_dims.z)));
    uint cell = uint((g.z * int(push.grid_dims.y) + g.y) * int(push.grid_dims.x) + g.x);
    uint record = grid.cells[cell];
    if (record == kNoRecord) return kNoRecord;
    if (records.items[record].coord != chunk_coord) return kNoRecord;
    return record;
}

// Is anything at all resident in the block of chunks containing this one? Answering that
// in one fetch is what lets a ray cross empty sky in 32 m or 128 m jumps rather than 8 m
// at a time.
// Thumbnail lookup. Wrapped grid, then the slot's own record has to agree that it holds the
// chunk being asked about Ã¢â‚¬â€ a cell that has been claimed by some far-off chunk reads as "no
// thumbnail", which costs a coarser picture and never a wrong one.
const int kThumbAxis = 8;
const int kThumbRecordWords = 4;
const int kThumbSlotWords = kThumbRecordWords + kThumbAxis * kThumbAxis * kThumbAxis;
const uint kNoThumb = 0xFFFFFFFFu;

// `block` is the block at `level` containing the chunk. Every level shares one grid buffer and
// one slot buffer at its own base offset, so this is one binding however many levels there are.
uint find_summary(ivec3 block, int level) {
    ivec3 g = ivec3(floor_mod(block.x, push.thumb_dims.x),
                    floor_mod(block.y, push.thumb_dims.y),
                    floor_mod(block.z, push.thumb_dims.z));
    uint cell = uint(push.thumb_tiers[level].x) +
                uint((g.z * push.thumb_dims.y + g.y) * push.thumb_dims.x + g.x);
    uint slot = thumb_grid.slots[cell];
    if (slot == kNoThumb) return kNoThumb;

    uint base = (uint(push.thumb_tiers[level].y) + slot) * uint(kThumbSlotWords);
    if (thumbs.words[base + 3u] == 0u) return kNoThumb;           // slot holds nothing
    if (int(thumbs.words[base + 0u]) != block.x) return kNoThumb;
    if (int(thumbs.words[base + 1u]) != block.y) return kNoThumb;
    if (int(thumbs.words[base + 2u]) != block.z) return kNoThumb;
    return base + uint(kThumbRecordWords);
}

uint thumb_cell(uint cells, ivec3 c) {
    return thumbs.words[cells + uint(c.x | (c.y << 3) | (c.z << 6))];
}

bool coarse_occupied(int level, ivec3 chunk_coord) {
    int factor = kCoarseFactor[level];
    ivec3 block = ivec3(floor_div(chunk_coord.x, factor), floor_div(chunk_coord.y, factor),
                        floor_div(chunk_coord.z, factor));
    ivec3 g = ivec3(floor_mod(block.x, kCoarseDims.x), floor_mod(block.y, kCoarseDims.y),
                    floor_mod(block.z, kCoarseDims.z));

    uint base = (uint(level) * kCoarseCellsPerLevel +
                 uint((g.z * kCoarseDims.y + g.y) * kCoarseDims.x + g.x)) * 4u;

    uint state = coarse.flags[base + 3u];
    if (state == 0u) return false;                       // known empty
    if (state == 2u) return true;                        // aliased: assume occupied
    ivec3 stored = ivec3(int(coarse.flags[base + 0u]), int(coarse.flags[base + 1u]),
                         int(coarse.flags[base + 2u]));
    // A cell describing a different block means nothing was ever recorded for ours, and
    // anything that had been would have marked the cell aliased. So: empty.
    return stored == block;
}

bool mask_bit(uint record, uint level, ivec3 cell) {
    uint axis = 32u >> level;
    uint bit = uint(cell.x) + uint(cell.y) * axis + uint(cell.z) * axis * axis;
    uint word = records.items[record].mask_offset + kMipOffset[level] + (bit >> 6u);
    uint half_word = masks.words[word * 2u + ((bit & 32u) != 0u ? 1u : 0u)];
    return ((half_word >> (bit & 31u)) & 1u) != 0u;
}

// Rank of a brick within its chunk, from the level-0 mask and its prefix.
uint brick_slot(uint record, ivec3 brick_cell) {
    uint linear = uint(brick_cell.x) | (uint(brick_cell.y) << 5u) | (uint(brick_cell.z) << 10u);
    uint word = linear >> 6u;
    uint bit = linear & 63u;
    uint base = records.items[record].mask_offset + word;
    uint lo = masks.words[base * 2u];
    uint hi = masks.words[base * 2u + 1u];

    uint below_lo = (bit == 0u) ? 0u : (lo & ((bit >= 32u) ? 0xFFFFFFFFu : ((1u << bit) - 1u)));
    uint below_hi = (bit <= 32u) ? 0u : (hi & ((1u << (bit - 32u)) - 1u));
    return records.items[record].slot_base +
           prefixes.values[records.items[record].prefix_offset + word] +
           bitCount(below_lo) + bitCount(below_hi);
}

// A coarse cell has no single colour, so walk down the pyramid taking the first occupied
// child at each level until a real brick is reached, and use that brick's filtered
// colour. Deterministic, so the image is stable frame to frame.
uint representative_colour(uint record, uint level, ivec3 cell) {
    ivec3 current = cell;
    for (uint l = level; l > 0u; --l) {
        bool found = false;
        for (int child = 0; child < 8 && !found; ++child) {
            ivec3 candidate = current * 2 + ivec3(child & 1, (child >> 1) & 1, (child >> 2) & 1);
            if (mask_bit(record, l - 1u, candidate)) {
                current = candidate;
                found = true;
            }
        }
        if (!found) return records.items[record].average_colour;
    }
    return headers.items[brick_slot(record, current)].average_colour;
}

bool voxel_solid(uint slot, ivec3 local) {
    uint index = uint(local.x + local.y * 8 + local.z * 64);
    return ((occupancy.words[slot * 16u + (index >> 5u)] >> (index & 31u)) & 1u) != 0u;
}

// Occupancy inside a brick at 2- or 4-voxel granularity.
bool brick_cell_solid(uint slot, ivec3 cell, int cell_size) {
    if (cell_size == 1) return voxel_solid(slot, cell);
    if (cell_size == 2) {
        uint bit = uint(cell.x) | (uint(cell.y) << 2u) | (uint(cell.z) << 4u);
        uvec2 mip = headers.items[slot].mip_cell2;
        return ((bit < 32u ? mip.x >> bit : mip.y >> (bit - 32u)) & 1u) != 0u;
    }
    uint bit = uint(cell.x) | (uint(cell.y) << 1u) | (uint(cell.z) << 2u);
    return ((headers.items[slot].mip_cell4 >> bit) & 1u) != 0u;
}

uint read_payload_byte(uint byte_offset) {
    uint word = payload.words[byte_offset >> 2u];
    return (word >> ((byte_offset & 3u) * 8u)) & 0xFFu;
}

uint voxel_type(uint slot, ivec3 local) {
    BrickHeader header = headers.items[slot];
    uint bits = header.packed & 0xFFu;
    if (bits == 0u) return header.uniform_type;

    uint index = uint(local.x + local.y * 8 + local.z * 64);
    uint base = header.payload_offset;
    if (bits == 32u) return payload.words[(base >> 2u) + index];

    uint palette_bytes = header.palette_count * 4u;
    uint per_byte = 8u / bits;
    uint packed = read_payload_byte(base + palette_bytes + index / per_byte);
    uint slot_index = (packed >> ((index % per_byte) * bits)) & ((1u << bits) - 1u);
    return payload.words[(base >> 2u) + slot_index];
}

// Ordered dither. Deterministic per pixel, so the level a pixel picks does not flicker
// between frames Ã¢â‚¬â€ which is what lets this work without temporal accumulation.
float bayer(ivec2 pixel) {
    const int table[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
    return float(table[(pixel.y & 3) * 4 + (pixel.x & 3)]) / 16.0;
}

struct Hit {
    bool hit;
    float t;
    ivec3 normal;
    uint type_id;      // valid when level == 0
    uint colour;       // rgba8, valid when level > 0
    int level;
    uint steps;
};

// Report one chunk per ray: the *first* one it wanted, that the world actually has, and
// that is not resident.
//
// "That the world actually has" is what level 0 of the occupancy grid is for. Without it
// the first miss is almost always empty sky the world has no chunk for, the CPU discards
// it, and the same useless report repeats every frame Ã¢â‚¬â€ measured at 22,600 reports a
// frame with none of them real.
//
// First rather than deepest, now that reports are real: a ray reports the nearest thing
// it is missing, that arrives, and next frame it reports the next one along. Streaming
// therefore converges front to back, which is also the order the player notices.
void note_missing(inout bool has_pending, inout ivec4 pending, ivec3 chunk_coord, int level) {
    if (has_pending) return;
    has_pending = true;
    pending = ivec4(chunk_coord, level);
}

void flush_missing(bool enabled, bool has_pending, ivec4 pending) {
    if (!enabled || !has_pending) return;
    uint index = atomicAdd(feedback.count, 1u);
    if (index < push.resolution.w) {
        feedback.entries[index].coord = pending;
    }
}

Hit march(vec3 origin, vec3 dir, float pixel_angle, float dither, bool report) {
    Hit result;
    result.hit = false;
    result.t = push.lens.y;
    result.normal = ivec3(0, 1, 0);
    result.type_id = 0u;
    result.colour = 0u;
    result.level = 0;
    result.steps = 0u;

    vec3 inv_dir = 1.0 / max(abs(dir), vec3(1e-9));
    ivec3 step_dir = ivec3(sign(dir));

    // Clip to the resident bounding box. Anything outside it is empty by definition, and
    // stepping through it looking for geometry that cannot be there was costing more than
    // the geometry itself.
    float limit = push.lens.y;
    {
        vec3 box_min = vec3(push.bounds_min.xyz) * float(kChunkEdge);
        vec3 box_max = (vec3(push.bounds_max.xyz) + 1.0) * float(kChunkEdge);
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
        limit = min(limit, exit);
    }

    float t = 0.0;
    bool has_pending = false;
    ivec4 pending = ivec4(0);
    ivec3 last_normal = ivec3(0);
    int outer_level = -1;

    ivec3 cell = ivec3(0);
    vec3 t_max = vec3(1e30);
    vec3 t_delta = vec3(1e30);

    for (uint step = 0u; step < kMaxSteps && t < limit; ++step) {
        result.steps = step;

        // Detail the pixel can actually resolve at this distance, as a continuous level.
        float footprint = max(t * pixel_angle * push.lens.z, 1.0);
        float wanted = log2(footprint);
        int level = clamp(int(floor(wanted + dither)), 0, kMaxLevel);
        int march_level = max(level, 3);   // the outer walk never goes finer than a brick

        if (march_level != outer_level) {
            // Re-seed the DDA at the new granularity. This happens a handful of times per
            // ray Ã¢â‚¬â€ once per level boundary crossed Ã¢â‚¬â€ not per step.
            outer_level = march_level;
            int size = 1 << outer_level;
            vec3 p = origin + dir * t;
            cell = ivec3(floor(p / float(size)));
            for (int axis = 0; axis < 3; ++axis) {
                if (abs(dir[axis]) < 1e-9) {
                    t_max[axis] = 1e30;
                    t_delta[axis] = 1e30;
                } else {
                    float boundary = (float(cell[axis]) + (step_dir[axis] > 0 ? 1.0 : 0.0)) *
                                     float(size);
                    t_max[axis] = (boundary - origin[axis]) / dir[axis];
                    t_delta[axis] = float(size) * inv_dir[axis];
                }
            }
        }

        int size = 1 << outer_level;
        int mip = outer_level - 3;          // 0 = bricks, up to 4 = 128-voxel cells
        ivec3 voxel_min = cell * size;
        ivec3 chunk_offset = ivec3(floor_div(voxel_min.x, kChunkEdge),
                                   floor_div(voxel_min.y, kChunkEdge),
                                   floor_div(voxel_min.z, kChunkEdge));
        uint record = find_record(push.camera_chunk.xyz + chunk_offset);

        if (record == kNoRecord) {
            // No full-detail chunk here. Before writing this off as empty space, see whether
            // there is a thumbnail Ã¢â‚¬â€ this is the branch that used to draw sky where the world
            // is, because full detail is bounded by memory and past that bound there was
            // nothing else to draw with.
            ivec3 thumb_chunk = push.camera_chunk.xyz + chunk_offset;

            // Report it missing *before* drawing the thumbnail, not after.
            //
            // A thumbnail hit returns from this function, and the report was assembled
            // further down Ã¢â‚¬â€ so the moment a chunk had a thumbnail it stopped being reported
            // as missing, streaming never fetched it, and it drew as one-metre blocks
            // forever. The tier meant to stand in for detail was suppressing it.
            if (coarse_occupied(0, thumb_chunk)) {
                note_missing(has_pending, pending, thumb_chunk, outer_level);
            }

            // Exactly the level whose cells this pixel can resolve Ã¢â‚¬â€ never a coarser one as a
            // fallback, and never a finer one.
            //
            // A level's cells are 32 * 2^level voxels, so this picks the one about a pixel
            // across. Falling back to a coarser level when this one has nothing is the obvious
            // thing and is badly wrong: a near sky chunk has no summary, the ray drops a level,
            // and a two-kilometre block holding ground a mile away reads as occupied Ã¢â‚¬â€ drawing
            // a 256 m blob a few metres from the camera. Testing every level at every step
            // instead measured 57 ms a frame. A level whose cells are bigger than the pixel is
            // not a worse picture of the same thing, it is a different piece of world.
            // Rounded, not floored. Flooring picks the level below the one the pixel needs,
            // which looks the same and costs coverage: each level is held for a fixed radius
            // *in blocks*, so using a level finer than intended halves how far it reaches.
            // At 3 km that put the ground 94 blocks away against a 96-block radius Ã¢â‚¬â€ right on
            // the edge, so rays straight down found a summary and rays at any angle did not,
            // and the screen was empty but for a smudge at the centre.
            int level = clamp(int(floor(log2(max(footprint, 32.0) / 32.0) + 0.5)), 0,
                              push.thumb_dims.w - 1);
            int span = push.thumb_tiers[level].z;
            ivec3 block = ivec3(floor_div(thumb_chunk.x, span), floor_div(thumb_chunk.y, span),
                                floor_div(thumb_chunk.z, span));

            uint cells = find_summary(block, level);
            if (cells != kNoThumb) {
                // Walk the block's 8x8x8 cells. Same DDA as everywhere else, just coarser: a
                // cell is `span` metres, below a pixel from about span*515 m, and each level
                // reaches twice as far as the one below Ã¢â‚¬â€ so where one starts to look blocky
                // the next has already taken over.
                int kCellVoxels = span * (kChunkEdge / kThumbAxis);
                ivec3 chunk_min = (block * span - push.camera_chunk.xyz) * kChunkEdge;

                float t_in = max(t, 0.0);
                vec3 p = origin + dir * (t_in + 1e-4);
                ivec3 c = ivec3(floor((p - vec3(chunk_min)) / float(kCellVoxels)));
                c = clamp(c, ivec3(0), ivec3(kThumbAxis - 1));

                vec3 c_max;
                vec3 c_delta;
                for (int axis = 0; axis < 3; ++axis) {
                    if (abs(dir[axis]) < 1e-9) {
                        c_max[axis] = 1e30;
                        c_delta[axis] = 1e30;
                    } else {
                        float boundary = float(chunk_min[axis] + (c[axis] + (step_dir[axis] > 0 ? 1 : 0)) *
                                                                     kCellVoxels);
                        c_max[axis] = (boundary - origin[axis]) / dir[axis];
                        c_delta[axis] = float(kCellVoxels) * inv_dir[axis];
                    }
                }

                ivec3 c_normal = last_normal;
                if (c_normal == ivec3(0)) {
                    vec3 a = abs(dir);
                    int dominant = (a.x > a.y && a.x > a.z) ? 0 : ((a.y > a.z) ? 1 : 2);
                    c_normal[dominant] = -step_dir[dominant];
                }

                for (int cell_step = 0; cell_step < 3 * kThumbAxis; ++cell_step) {
                    if (any(lessThan(c, ivec3(0))) || any(greaterThanEqual(c, ivec3(kThumbAxis)))) {
                        break;
                    }
                    uint packed = thumb_cell(cells, c);
                    if (packed != 0u) {
                        result.hit = true;
                        result.t = max(t_in, 0.0);
                        result.normal = c_normal;
                        // Alpha is coverage, not opacity Ã¢â‚¬â€ a cubic metre with a railing in it
                        // is barely covered and completely opaque when you look at it. The
                        // renderer wants a colour, so the coverage is dropped here rather than
                        // used to fade the cell into the sky, which is the mistake that made
                        // distant window frames dither away in Stage 4.
                        result.colour = (packed & 0x00FFFFFFu) | 0xFF000000u;
                        result.level = 5;   // a metre, so: coarse, and not a voxel type
                        flush_missing(report, has_pending, pending);
                        return result;
                    }
                    if (c_max.x < c_max.y && c_max.x < c_max.z) {
                        t_in = c_max.x; c.x += step_dir.x; c_max.x += c_delta.x;
                        c_normal = ivec3(-step_dir.x, 0, 0);
                    } else if (c_max.y < c_max.z) {
                        t_in = c_max.y; c.y += step_dir.y; c_max.y += c_delta.y;
                        c_normal = ivec3(0, -step_dir.y, 0);
                    } else {
                        t_in = c_max.z; c.z += step_dir.z; c_max.z += c_delta.z;
                        c_normal = ivec3(0, 0, -step_dir.z);
                    }
                    if (t_in > limit) break;
                }
            }

            // Skip empty space analytically, at the coarsest granularity that is still
            // entirely empty. Testing the biggest block first means a ray heading into
            // open sky leaves in a handful of steps rather than one per chunk.
            // Finest level first, stopping as soon as one is occupied. Next to geometry
            // that is a single fetch and an 8 m step; out in open sky it walks up to the
            // 2 km level. Testing coarsest-first instead costs four fetches per step
            // everywhere near anything, which measured slower than having no coarse grid.
            int skip_chunks = 1;
            ivec3 skip_origin = chunk_offset;
            ivec3 absolute = push.camera_chunk.xyz + chunk_offset;

            // Level 0 is per chunk: does the world have anything here at all?
            int chosen = -1;
            if (coarse_occupied(0, absolute)) {
                // Already reported above, before the thumbnail had a chance to return.
            } else {
                // Genuinely empty. Find the coarsest block that is also empty and jump
                // over the lot.
                for (int level = 1; level < kCoarseLevels; ++level) {
                    if (coarse_occupied(level, absolute)) break;
                    chosen = level;
                }
            }
            if (chosen >= 0) {
                int level = chosen;
                skip_chunks = kCoarseFactor[level];
                // Block boundaries are in absolute chunk space. Rounding down in
                // camera-relative space instead gives a box that does not line up with
                // the block being skipped, so the ray can exit "behind" where it already
                // is and creep forward 1e-3 at a time until it runs out of steps.
                ivec3 block = ivec3(floor_div(absolute.x, skip_chunks),
                                    floor_div(absolute.y, skip_chunks),
                                    floor_div(absolute.z, skip_chunks)) * skip_chunks;
                skip_origin = block - push.camera_chunk.xyz;
            }

            vec3 chunk_min = vec3(skip_origin) * float(kChunkEdge);
            vec3 chunk_max = chunk_min + float(kChunkEdge * skip_chunks);
            float exit_t = 1e30;
            int exit_axis = 1;
            for (int axis = 0; axis < 3; ++axis) {
                if (abs(dir[axis]) < 1e-9) continue;
                float far_boundary = (dir[axis] > 0.0) ? chunk_max[axis] : chunk_min[axis];
                float axis_t = (far_boundary - origin[axis]) / dir[axis];
                if (axis_t < exit_t) { exit_t = axis_t; exit_axis = axis; }
            }
            // The face the ray leaves through is the face it enters the next chunk
            // through. Without carrying it, geometry hit immediately after a skip has no
            // normal at all.
            last_normal = ivec3(0);
            last_normal[exit_axis] = -step_dir[exit_axis];

            // The nudge past the boundary has to grow with distance, or it stops being a nudge
            // at all. A 32-bit float's own step at t = 96,000 voxels is 0.0156, so `t + 1e-3`
            // rounds straight back to t: the ray stops advancing and spins on the same
            // boundary until its step budget runs out. Looking down from 3 km that was every
            // pixel on the screen Ã¢â‚¬â€ the step-count view came back saturated, and raising the
            // budget eightfold changed nothing, because the ray was not stepping slowly, it
            // was not stepping.
            //
            // Scaled, it stays about a hundred-thousandth of the distance travelled:
            // comfortably above the float's resolution and far below a voxel, so it cannot
            // skip anything.
            t = max(exit_t, t) + max(1e-3, t * 1e-5);
            outer_level = -1;   // force a DDA re-seed at the new position

            // There used to be a `if (++empty_run > 24) break;` here, described as a guard
            // against a pathological ray rather than a distance limit. It was both, and once
            // thumbnails existed it was the thing stopping them being seen.
            //
            // Each pass through here advances t to at least the next block boundary, so a ray
            // cannot fail to progress and does not need a separate guard Ã¢â‚¬â€ kMaxSteps already
            // bounds the loop. What the run *did* bound was distance: twenty-four consecutive
            // single-chunk steps is 192 m, and near geometry the coarse levels have nothing
            // large to skip, so steps are single chunks. Looking down at a world from 700 m,
            // only the rays going almost straight down reached the ground within the run;
            // everything at an angle crossed more chunks and gave up. That drew as one small
            // square of world in the middle of an empty screen.
            continue;
        }


        ivec3 local_cell = cell - chunk_offset * (kChunkEdge / size);
        if (mask_bit(record, uint(mip), local_cell)) {
            if (mip > 0) {
                // Coarser than a brick: this cell is the finest thing the pixel can see.
                result.hit = true;
                result.t = t;
                result.normal = last_normal;
                result.colour = representative_colour(record, uint(mip), local_cell);
                result.level = outer_level;
                flush_missing(report, has_pending, pending);
                return result;
            }

            uint slot = brick_slot(record, local_cell);
            if (level >= 3) {
                result.hit = true;
                result.t = t;
                result.normal = last_normal;
                result.colour = headers.items[slot].average_colour;
                result.level = 3;
                flush_missing(report, has_pending, pending);
                return result;
            }

            // Inside the brick, always at single voxels Ã¢â‚¬â€ and deliberately so, even when the
            // pixel is too small to resolve one.
            //
            // The level still decides the *colour*: a pixel spanning several voxels gets the
            // brick's filtered average rather than whichever voxel happened to be under the
            // ray, which is the whole point of the scheme. What it must not decide is the
            // shape, and it used to: at level 1 the walk stepped in 2-voxel cells, and a cell
            // counts as solid when *any* of its eight voxels is. A cell straddling a surface
            // therefore stands half a cell proud of it, so a grazing ray clips the cell's
            // side and the pixel is shaded as a wall instead of a floor.
            //
            // The geometric error is a voxel Ã¢â‚¬â€ 3 cm, and by construction below what the pixel
            // can resolve. The shading error is lit-top against dark-side, which is not
            // subtle at all: on flat ground it measured 680 wrong pixels in 63,000 at level 1
            // against 2 in 270,000 at level 0, and it read on screen as faint dotted lines
            // scattered over the surface (the ordered dither is what scatters them, by giving
            // neighbouring pixels different levels).
            //
            // Marching a brick at voxel resolution costs at most 24 inner steps instead of
            // 12, inside data already fetched. That is a cheap price for a correct face.
            const int inner_size = 1;
            const int inner_axis = 8;
            float t_inner = max(t, 0.0);
            vec3 p = origin + dir * (t_inner + 1e-4);
            ivec3 inner = ivec3(floor(p / float(inner_size))) - cell * inner_axis;
            inner = clamp(inner, ivec3(0), ivec3(inner_axis - 1));

            vec3 i_max;
            vec3 i_delta;
            for (int axis = 0; axis < 3; ++axis) {
                if (abs(dir[axis]) < 1e-9) {
                    i_max[axis] = 1e30;
                    i_delta[axis] = 1e30;
                } else {
                    float boundary = float((cell[axis] * inner_axis + inner[axis]) * inner_size) +
                                     (step_dir[axis] > 0 ? float(inner_size) : 0.0);
                    i_max[axis] = (boundary - origin[axis]) / dir[axis];
                    i_delta[axis] = float(inner_size) * inv_dir[axis];
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
                if (any(lessThan(inner, ivec3(0))) ||
                    any(greaterThanEqual(inner, ivec3(inner_axis)))) {
                    break;
                }
                if (brick_cell_solid(slot, inner, inner_size)) {
                    result.hit = true;
                    result.t = max(t_inner, 0.0);
                    result.normal = inner_normal;
                    result.level = level;
                    if (level == 0) {
                        result.type_id = voxel_type(slot, inner);
                    } else {
                        result.colour = headers.items[slot].average_colour;
                    }
                    flush_missing(report, has_pending, pending);
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

    flush_missing(report, has_pending, pending);
    return result;
}

