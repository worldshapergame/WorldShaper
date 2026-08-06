#include "world/gpu_brick.hpp"

#include <algorithm>
#include <cstring>

#include "core/assert.hpp"
#include "core/hash.hpp"

namespace ws {
namespace {

constexpr u32 kVoxels = static_cast<u32>(kBrickVoxels);

u32 read_u32(const u8* data, u32 byte_offset) {
    u32 value = 0;
    std::memcpy(&value, data + byte_offset, sizeof(u32));
    return value;
}

}  // namespace

void canonical_palette(const Brick& brick, std::vector<VoxelTypeId>& palette,
                       std::vector<u32>& slots) {
    palette.clear();

    // A uniform brick is the common case by a wide margin — solid rock, open air, the
    // inside of anything — and scanning 512 voxels to discover that is pure waste. This
    // early-out was worth two orders of magnitude on the streaming path.
    if (brick.uniform()) {
        palette.push_back(brick.uniform_value());
        slots.clear();
        return;
    }

    palette.reserve(16);
    slots.resize(kVoxels);

    for (u32 voxel = 0; voxel < kVoxels; ++voxel) {
        const VoxelTypeId type = brick.get(voxel);
        const auto at = std::find(palette.begin(), palette.end(), type);
        if (at == palette.end()) {
            slots[voxel] = static_cast<u32>(palette.size());
            palette.push_back(type);
        } else {
            slots[voxel] = static_cast<u32>(at - palette.begin());
        }
    }
}

u32 index_bits_for(usize palette_size) {
    if (palette_size <= 1) return 0;
    if (palette_size <= 2) return 1;
    if (palette_size <= 4) return 2;
    if (palette_size <= 16) return 4;
    if (palette_size <= 256) return 8;
    return 32;
}

// The GPU layout is deliberately identical to the brick's own packed layout, so encoding
// is a copy rather than a re-encode.
//
// This matters more than it looks. Re-deriving a canonical palette per brick measured
// 22 ms on a frame that streamed four chunks, against a 0.8 ms budget, because it walks
// all 512 voxels and scans the palette for each one. Copying the existing buffers is a
// few hundred bytes of memcpy.
//
// The cost of the shortcut is that the GPU palette is in the brick's insertion order
// rather than first-seen voxel order, and may contain entries no voxel uses any more.
// Neither matters: nothing compares GPU bytes to anything, only decoded voxels. The save
// format keeps its canonical form, which is where byte-identity actually has to hold.

u32 gpu_payload_size(const Brick& brick) {
    const u32 bits = brick.index_bits();
    if (bits == 0) return 0;
    if (bits == 32) return kVoxels * 4;
    return static_cast<u32>(brick.palette_data().size()) * 4 + (kVoxels * bits) / 8;
}

u32 filtered_colour(const Brick& brick, const VoxelTypeTable& table) {
    // Uniform bricks are most of a world, and walking 512 identical voxels to average
    // them is the same mistake that cost 22 ms a frame in the encoder. One lookup.
    if (brick.uniform()) {
        const VoxelTypeId type = brick.uniform_value();
        if (type == kAir) return 0;
        const VisualRecord& visual = table.visual_of(type);
        return visual.red | (static_cast<u32>(visual.green) << 8) |
               (static_cast<u32>(visual.blue) << 16) | (255u << 24);
    }

    // Coverage has to be exact — it decides how a sub-pixel node blends — and popcount
    // gives it for free. The colour is an average of an average, so a stride sample of
    // one voxel in eight is indistinguishable and eight times cheaper.
    u64 occupancy[kBrickWords];
    brick.occupancy(occupancy);
    u32 solid = 0;
    for (u64 word : occupancy) solid += static_cast<u32>(std::popcount(word));
    if (solid == 0) return 0;

    u32 red = 0;
    u32 green = 0;
    u32 blue = 0;
    u32 sampled = 0;
    for (u32 voxel = 0; voxel < kVoxels; voxel += 8) {
        const VoxelTypeId type = brick.get(voxel);
        if (type == kAir) continue;
        const VisualRecord& visual = table.visual_of(type);
        red += visual.red;
        green += visual.green;
        blue += visual.blue;
        ++sampled;
    }
    if (sampled == 0) {
        // The stride happened to miss every solid voxel. Rare, and one exact lookup fixes
        // it rather than leaving the node black.
        for (u32 voxel = 0; voxel < kVoxels; ++voxel) {
            const VoxelTypeId type = brick.get(voxel);
            if (type == kAir) continue;
            const VisualRecord& visual = table.visual_of(type);
            red = visual.red;
            green = visual.green;
            blue = visual.blue;
            sampled = 1;
            break;
        }
        if (sampled == 0) return 0;
    }

    const u32 coverage = (solid * 255) / kVoxels;
    return (red / sampled) | ((green / sampled) << 8) | ((blue / sampled) << 16) |
           (coverage << 24);
}

namespace {

// Occupancy filtered down inside the brick: a cell is set when anything in it is set.
//
// Done by folding bits rather than by visiting voxels. Each occupancy word is one z-slice
// of 8x8 voxels, so ORing slices collapses z, shifting and ORing rows collapses y, and one
// masked shift collapses x — about thirty operations for a whole brick against up to 512
// iterations. This runs once per streamed brick and was the largest single cost in the
// streaming budget.
void build_mips(const u64 occupancy[kBrickWords], u64& mip_cell2, u32& mip_cell4) {
    mip_cell2 = 0;
    mip_cell4 = 0;

    for (u32 cz = 0; cz < 4; ++cz) {
        // Two z-slices together: a 2-voxel-deep layer, as 8x8 bits laid out x + y*8.
        const u64 layer = occupancy[cz * 2] | occupancy[cz * 2 + 1];
        if (layer == 0) continue;

        for (u32 cy = 0; cy < 4; ++cy) {
            const u64 two_rows = (layer >> (cy * 16)) & 0xFFFFull;
            const u32 row = static_cast<u32>((two_rows | (two_rows >> 8)) & 0xFFull);
            if (row == 0) continue;

            // Fold neighbouring x pairs, then gather the four results into four bits.
            const u32 pairs = (row | (row >> 1)) & 0x55u;
            const u32 cells = (pairs & 1u) | ((pairs >> 1) & 2u) | ((pairs >> 2) & 4u) |
                              ((pairs >> 3) & 8u);
            mip_cell2 |= static_cast<u64>(cells) << (cz * 16 + cy * 4);
        }
    }

    // The 2x2x2 grid of 4-voxel cells is the same fold once more, off the 4x4x4.
    for (u32 cell = 0; cell < 8; ++cell) {
        const u32 x = cell & 1u;
        const u32 y = (cell >> 1) & 1u;
        const u32 z = (cell >> 2) & 1u;
        bool any = false;
        for (u32 child = 0; child < 8 && !any; ++child) {
            const u32 bit = (x * 2 + (child & 1u)) | ((y * 2 + ((child >> 1) & 1u)) << 2) |
                            ((z * 2 + ((child >> 2) & 1u)) << 4);
            any = ((mip_cell2 >> bit) & 1u) != 0;
        }
        if (any) mip_cell4 |= (1u << cell);
    }
}

}  // namespace

void encode_brick(const Brick& brick, const VoxelTypeTable& table, GpuBrickHeader& header,
                  std::vector<u8>& payload, u64 occupancy[kBrickWords]) {
    brick.occupancy(occupancy);

    const u32 bits = brick.index_bits();
    header = GpuBrickHeader{};
    header.index_bits = static_cast<u8>(bits);
    header.payload_offset = kNoOffset;
    header.average_colour = filtered_colour(brick, table);
    build_mips(occupancy, header.mip_cell2, header.mip_cell4);

    if (bits == 0) {
        header.uniform_type = brick.uniform_value();
        header.palette_count = 0;
        payload.clear();
        return;
    }

    if (bits == 32) {
        header.palette_count = 0;
        const std::vector<VoxelTypeId>& direct = brick.direct_data();
        payload.resize(kVoxels * 4);
        std::memcpy(payload.data(), direct.data(), kVoxels * sizeof(VoxelTypeId));
        return;
    }

    const std::vector<VoxelTypeId>& palette = brick.palette_data();
    const std::vector<u8>& indices = brick.index_data();

    header.palette_count = static_cast<u32>(palette.size());
    const u32 palette_bytes = header.palette_count * 4;
    payload.resize(palette_bytes + indices.size());
    std::memcpy(payload.data(), palette.data(), palette_bytes);
    std::memcpy(payload.data() + palette_bytes, indices.data(), indices.size());
}

VoxelTypeId decode_voxel(const GpuBrickHeader& header, const u8* payload, u32 voxel) {
    WS_ASSERT(voxel < kVoxels, "voxel index out of range");

    if (header.index_bits == 0) return header.uniform_type;
    WS_ASSERT(payload != nullptr, "a non-uniform brick needs its payload");

    if (header.index_bits == 32) return read_u32(payload, voxel * 4);

    const u32 bits = header.index_bits;
    const u32 palette_bytes = header.palette_count * 4;
    const u32 per_byte = 8 / bits;
    const u32 mask = (1u << bits) - 1;

    const u8 packed = payload[palette_bytes + voxel / per_byte];
    const u32 slot = (packed >> ((voxel % per_byte) * bits)) & mask;
    return read_u32(payload, slot * 4);
}

u64 decode_brick_hash(const GpuBrickHeader& header, const u8* payload) {
    // Must match Brick::content_hash exactly, or the mirror check compares apples to
    // oranges and silently passes.
    u64 h = 0x1E35A7BDull;
    for (u32 voxel = 0; voxel < kVoxels; ++voxel) {
        h = hash_combine(h, decode_voxel(header, payload, voxel));
    }
    return h;
}

}  // namespace ws
