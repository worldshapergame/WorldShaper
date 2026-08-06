#pragma once
// The GPU-side brick layout.
//
// Three parallel pools rather than one interleaved structure, because the ray marcher's
// access pattern is not uniform (documentation/04-rendering.md §1):
//
//   occupancy   64 bytes per resident brick, in one contiguous array. This is the hot
//               data — one cache line answers "is anything in these 512 voxels", and on a
//               bandwidth-bound Steam Deck that is the difference between fitting in
//               88 GB/s and not. A ray touches many bricks' occupancy and almost none of
//               their contents.
//   headers     16 bytes per brick: where its payload is and how it is encoded. Read only
//               once a ray has decided it actually hit something.
//   payload     variable, from a segregated-fit pool: the palette followed by packed
//               indices. Read last and least.
//
// The encoding is canonical — derived from the brick's contents, not from whichever form
// it happens to be in on the CPU — so the same brick always produces the same bytes. That
// is what lets a GPU-side hash be compared against a CPU-side one.

#include <vector>

#include "core/block_pool.hpp"   // kNoOffset: a brick with no payload
#include "core/types.hpp"
#include "world/brick.hpp"

namespace ws {

struct GpuBrickHeader {
    u32 payload_offset = 0xFFFFFFFFu;   // byte offset into the payload pool
    u32 palette_count = 0;
    u8 index_bits = 0;                  // 0 = uniform, then 1, 2, 4, 8, or 32 for direct
    u8 flags = 0;
    u16 reserved = 0;
    u32 uniform_type = kAir;            // meaningful when index_bits == 0

    // Continuous detail (documentation/04-rendering.md §1). Occupancy filtered down twice
    // inside the brick, so a ray whose pixel footprint is bigger than a voxel can stop
    // early instead of stepping 3 cm cells it cannot resolve.
    u64 mip_cell2 = 0;                  // 4x4x4 grid of 2-voxel cells, 64 bits
    u32 mip_cell4 = 0;                  // 2x2x2 grid of 4-voxel cells, low 8 bits
    u32 average_colour = 0;             // rgba8, filtered over the brick's solid voxels

    bool uniform() const { return index_bits == 0; }
    bool operator==(const GpuBrickHeader&) const = default;
};
static_assert(sizeof(GpuBrickHeader) == 32, "GpuBrickHeader must stay 32 bytes");

// The canonical palette: distinct types in first-seen voxel order, plus the slot each
// voxel uses. Shared with serialisation so the two encodings can never drift apart.
void canonical_palette(const Brick& brick, std::vector<VoxelTypeId>& palette,
                       std::vector<u32>& slots);

// Bits per index for a palette of this size: 0 (uniform), 1, 2, 4, 8, or 32 (direct).
u32 index_bits_for(usize palette_size);

// Bytes the payload will occupy, before size-class rounding. Zero for a uniform brick.
u32 gpu_payload_size(const Brick& brick);

// Fills header, payload and occupancy. `payload` is resized to exactly gpu_payload_size.
// The type table is needed for the filtered average colour the renderer uses when a
// brick is too small on screen to resolve individual voxels.
void encode_brick(const Brick& brick, const VoxelTypeTable& table, GpuBrickHeader& header,
                  std::vector<u8>& payload, u64 occupancy[kBrickWords]);

// Filtered colour of a brick's solid voxels, as rgba8. Alpha carries coverage: what
// fraction of the brick is matter, which is what a partially-filled node needs when it
// covers less than a pixel.
u32 filtered_colour(const Brick& brick, const VoxelTypeTable& table);

// Reads one voxel back out of the GPU representation. This is the CPU model of exactly
// what the shader will do, and the two are held to the same test.
VoxelTypeId decode_voxel(const GpuBrickHeader& header, const u8* payload, u32 voxel);

// Hashes the decoded contents with the same rule Brick::content_hash uses, so a mirror
// hash and a CPU hash are directly comparable.
u64 decode_brick_hash(const GpuBrickHeader& header, const u8* payload);

}  // namespace ws
