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

    // DOES A PERSON'S WORK LIVE IN THESE 512 VOXELS? — R12d.
    //
    // Nothing about the contents. A brick the sampler wrote and a brick a player built by hand out
    // of the same material are identical voxel for voxel, hash the same both ways and encode the
    // same; the only thing that separates them is who wrote them, and that is a fact that has to
    // be recorded at the write or lost for ever. It is what makes the base world derivable: a
    // brick reading false is one the clip can regenerate and therefore one storage need not keep,
    // and a brick reading true is somebody's building.
    //
    // Deliberately NOT part of content_hash, shape_hash or operator==. Those three answer "is this
    // the same world", which is a question about voxels, and every gate in the repository is built
    // on them — a provenance bit leaking into one would move the world's identity without moving
    // one voxel. It is not part of `bytes()` either, and that is honest rather than a rounding:
    // the bool sits in the padding after `form_`, so `sizeof(Brick)` does not move.
    //
    // The contents changing does not clear it and must not: `fill`, `assign`, `compact` and
    // `set` all leave it alone. A player who carves a niche and fills it back in still owns that
    // brick. What clears it is the brick being freed and handed back out, which is `Chunk`'s job.
    bool edited() const { return edited_; }

    // GO THROUGH THE CHUNK, not through here, on anything a chunk owns.
    //
    // `Chunk` keeps a count of how many of its bricks read true, so that a chunk with none can be
    // answered whole without touching 32,768 slots. Setting the flag behind the chunk's back
    // leaves that count wrong, which is a redundant pair of indexes disagreeing — trap 13, the one
    // that took a session in D345/D358. `Chunk::brick_for_write(bx, by, bz, WriteOrigin::Edit)`
    // is the way in; `Chunk::validate` re-derives the count and fails if anything took this door.
    void set_edited(bool value) { edited_ = value; }

    // 512 bits, one per voxel, set where the voxel is not air. Always valid, including
    // for uniform bricks (where it is synthesised on demand).
    // Every voxel, in index order. The form is decided once for the whole brick rather than
    // re-tested per voxel, which is the difference that matters when something needs all
    // 512 — undo capture reads a brick this way rather than through get().
    void decode(VoxelTypeId out[kBrickVoxels]) const;

    // The reverse: replaces all 512 voxels in one pass and picks the smallest encoding that
    // holds them. This is what a bulk writer wants, and the difference is not small — set()
    // per voxel re-tests the form, re-searches the palette and may re-pack the whole brick
    // 512 times over, where this decides once.
    //
    // It exists because of the rule that no two voxels in a clip share properties. Under that
    // rule a brick routinely holds 512 distinct types, which is the exact case every
    // incremental path is worst at: the palette search alone is quadratic in the distinct
    // count, so a brick that used to cost a handful of comparisons costs a hundred thousand.
    void assign(const VoxelTypeId in[kBrickVoxels]);

    void occupancy(u64 out[kBrickWords]) const;

    // WHICH CELLS HOLD MATTER, and nothing about what the matter is.
    //
    // `content_hash` hashes the `VoxelTypeId` of every cell, and a type id is only meaningful
    // against the table it was interned into: two worlds that look identical hash differently if
    // their tables were built in a different order. That is exactly what a resumed run does —
    // it re-samples every leaf that was not already at the clip's own detail (see
    // `resume_refinement`) and interns the variation records afresh — so `content_hash` drifts on
    // every launch while the building does not change at all.
    //
    // This is the half that CAN be compared across runs: solid or not, cell by cell. If two worlds
    // agree here and differ on `content_hash`, the difference is in the naming and not in the
    // shape, and that is a very different report from "the world changed".
    u64 shape_hash() const;
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
    // Beside `form_` deliberately: both are one byte and `uniform_` is four, so this sits in
    // padding that was already there and `sizeof(Brick)` does not move. R12d costs no memory.
    bool edited_ = false;
    VoxelTypeId uniform_ = kAir;
    std::vector<VoxelTypeId> palette_;
    std::vector<u8> indices_;              // bit-packed, bits_for(form_) per voxel
    std::vector<VoxelTypeId> direct_;      // Form::Direct only
    u64 occupancy_[kBrickWords]{};
};

}  // namespace ws
