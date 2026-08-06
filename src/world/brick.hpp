#pragma once
// A brick: 8×8×8 = 512 voxels, the unit of storage, compression, simulation dispatch and
// sleep/wake (documentation/03-voxel-data-model.md §3).
//
// The encoding is chosen automatically on every write and is the single biggest lever on
// how much world fits in memory:
//
//   Uniform    all 512 the same          8 bytes         underground rock, open air
//   Palette1   2 distinct types          64 + 64 bytes   a wall against air
//   Palette2   3–4 types                 64 + 128 bytes  typical terrain surface
//   Palette4   5–16 types                64 + 256 bytes
//   Palette8   17–256 types              64 + 512 bytes  detailed player building
//   Direct     more than 256 types       64 + 2048 bytes hand-painted, every voxel unique
//
// The 64-byte occupancy bitmask on every non-uniform brick is the hottest structure in
// the engine: one cache line answers "is there anything in these 512 voxels", which is
// what lets the ray marcher skip empty space and the simulation test neighbourhoods with
// bit operations. On a bandwidth-bound Steam Deck that mask is the difference between
// fitting in 88 GB/s and not.

#include <vector>

#include "core/types.hpp"
#include "world/voxel_type.hpp"
#include "world/write_mask.hpp"

namespace ws {

inline constexpr u32 kBrickWords = 8;   // 512 bits of occupancy

constexpr u32 brick_index(u32 x, u32 y, u32 z) {
    return x + y * static_cast<u32>(kBrickEdge) +
           z * static_cast<u32>(kBrickEdge * kBrickEdge);
}

class Brick {
public:
    enum class Form : u8 { Uniform, Palette1, Palette2, Palette4, Palette8, Direct };

    Brick() = default;
    explicit Brick(VoxelTypeId fill);

    VoxelTypeId get(u32 index) const;
    VoxelTypeId get(u32 x, u32 y, u32 z) const { return get(brick_index(x, y, z)); }

    // Returns true when the voxel actually changed.
    bool set(u32 index, VoxelTypeId type);
    bool set(u32 x, u32 y, u32 z, VoxelTypeId type) {
        return set(brick_index(x, y, z), type);
    }

    void fill(VoxelTypeId type);

    struct TypeCount {
        VoxelTypeId type;
        u32 count;
    };

    // How many voxels of each distinct type this brick holds. Bulk edits use it to charge
    // the matter ledger once per type instead of once per voxel: a brick the chisel covers
    // completely can be overwritten outright, and the only thing the per-voxel loop was
    // ever needed for was knowing what used to be there.
    void type_histogram(std::vector<TypeCount>& out) const;

    // Overwrites a sub-box of the brick in one pass, reporting what it displaced. Same
    // reasoning as type_histogram, for the bricks a chisel box only partly covers: the
    // palette lookup, the width promotion and the encoding decision are made once for the
    // whole range instead of once per voxel. Coordinates are inclusive, 0-7 on each axis.
    // Returns the number of voxels that actually changed.
    u32 fill_range(u32 x0, u32 y0, u32 z0, u32 x1, u32 y1, u32 z1, VoxelTypeId type,
                   WriteMask mask, std::vector<TypeCount>& displaced);

    bool solid(u32 index) const;
    bool empty() const;                  // every voxel is air
    bool uniform() const { return form_ == Form::Uniform; }
    VoxelTypeId uniform_value() const { return uniform_; }

    // 512 bits, one per voxel, set where the voxel is not air. Always valid, including
    // for uniform bricks (where it is synthesised on demand).
    // Every voxel, in index order. The form is decided once for the whole brick rather than
    // re-tested per voxel, which is the difference that matters when something needs all
    // 512 — undo capture reads a brick this way rather than through get().
    void decode(VoxelTypeId out[kBrickVoxels]) const;

    void occupancy(u64 out[kBrickWords]) const;
    u32 solid_count() const;

    Form form() const { return form_; }
    u32 palette_size() const;
    usize bytes() const;

    // Raw access to the packed representation. The GPU layout is deliberately the same
    // one, so uploading a brick is a memcpy rather than a per-voxel re-encode — see
    // world/gpu_brick.cpp. Only the streaming path should use these.
    u32 index_bits() const;
    const std::vector<VoxelTypeId>& palette_data() const { return palette_; }
    const std::vector<u8>& index_data() const { return indices_; }
    const std::vector<VoxelTypeId>& direct_data() const { return direct_; }

    // Drops palette entries nothing references and re-picks the smallest encoding.
    // Called after bulk edits; a single set() already keeps the form minimal upward but
    // deliberately does not shrink, because carving one voxel out and putting it back
    // would otherwise re-encode the brick twice.
    void compact();

    // Debug/CI invariant: the occupancy mask exactly matches the stored indices, the
    // palette has no duplicates, and the form matches the palette size.
    bool validate() const;

    u64 content_hash() const;
    bool operator==(const Brick& other) const;

private:
    static u32 bits_for(Form form);
    u32 read_index(u32 voxel) const;
    void write_index(u32 voxel, u32 palette_slot);
    void set_form(Form form);
    void promote_to(Form form);
    void materialise_from_uniform();
    u32 palette_slot_for(VoxelTypeId type);   // adds when missing
    void refresh_occupancy();

    Form form_ = Form::Uniform;
    VoxelTypeId uniform_ = kAir;
    std::vector<VoxelTypeId> palette_;
    std::vector<u8> indices_;              // bit-packed, bits_for(form_) per voxel
    std::vector<VoxelTypeId> direct_;      // Form::Direct only
    u64 occupancy_[kBrickWords]{};
};

}  // namespace ws
