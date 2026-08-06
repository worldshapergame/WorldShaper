#pragma once
// A clip: a box of voxels lifted out of the world, and the transforms you can apply to it.
//
// # Why rotation is the hard part
//
// Rotating a voxel volume by an angle that is not a multiple of 90° has an obvious
// implementation — for every destination voxel, rotate backwards, round, and read whatever
// is there — and that implementation is wrong for this game. Nearest-neighbour resampling
// reads some source voxels twice and never reads others, so a wall of 1,000 bricks comes
// out as 987 or 1,013. In a game whose central invariant is that matter is conserved
// (documentation/05 §3), a tool that quietly manufactures stone is not acceptable.
//
// So rotation is done as three shears instead — the Paeth decomposition:
//
//     R(θ) = shear_x(-tan(θ/2)) ∘ shear_y(sin θ) ∘ shear_x(-tan(θ/2))
//
// Each shear moves whole rows (or columns) by a whole number of voxels. A shift of a row is
// a bijection on that row, so each shear is a bijection on the lattice, and so is their
// composition. Every source voxel lands on exactly one destination voxel and every
// destination voxel comes from exactly one source voxel. The count is preserved by
// construction, not by rounding luck — and a test asserts it over a sweep of angles.
//
// Two details make it work in practice:
//
//  - **Whole quarter turns are done separately**, by permuting and flipping axes, which is
//    exact and free. Only the remainder, always within ±45°, goes through the shears —
//    tan(θ/2) blows up as θ approaches 180°, and a shear by a huge factor is a smear.
//  - **Rotation always starts from the original clip.** Turning a clip 7.5° twelve times by
//    re-rotating the previous result would visibly chew the shape; the clipboard keeps the
//    captured clip and the accumulated angle, and bakes from the original every time.
//
// # Why a clip carries an `inside` mask
//
// A rotated clip is a rotated box sitting in an axis-aligned array, so the array has
// corners that were never part of the clip. Those are not air — air is a material decision
// the clip makes and can stamp — they are *outside the clip*, and stamping must not touch
// them. One byte per voxel says which is which.

#include <vector>

#include "core/types.hpp"
#include "world/op.hpp"
#include "world/voxel_type.hpp"

namespace ws {

class World;

// What a stamp does to what is already there.
enum class PasteMode : u8 {
    Replace = 0,   // the clip lands whole, its empty parts clearing what they cover
    SolidOnly,     // only the clip's matter is written; its empty parts leave things alone
    IntoAir,       // written only where the destination is empty — for trim and moulding
    Count,
};

const char* paste_mode_name(PasteMode mode);

struct Clip {
    struct TypeTally {
        VoxelTypeId type;
        u32 count;
    };

    i32 size[3]{0, 0, 0};
    std::vector<VoxelTypeId> voxels;
    std::vector<u8> inside;   // 1 where this cell is part of the clip

    // One byte per 8×8×8 block, non-zero when that block holds any matter. The ghost march
    // steps over empty blocks instead of walking them a voxel at a time — a selection is
    // mostly air, so this is the difference between a ghost costing what its surface costs
    // and costing what its bounding box costs.
    i32 coarse_size[3]{0, 0, 0};
    std::vector<u8> coarse;

    usize coarse_index(i32 x, i32 y, i32 z) const {
        return static_cast<usize>(x) + static_cast<usize>(y) * coarse_size[0] +
               static_cast<usize>(z) * coarse_size[0] * coarse_size[1];
    }
    u64 coarse_count() const {
        return static_cast<u64>(coarse_size[0]) * static_cast<u64>(coarse_size[1]) *
               static_cast<u64>(coarse_size[2]);
    }
    // Rebuilt from `voxels`. Every function here that returns a Clip calls it; anything
    // that edits one in place has to.
    void build_coarse();

    bool empty() const { return size[0] <= 0 || size[1] <= 0 || size[2] <= 0; }
    u64 cell_count() const {
        return static_cast<u64>(size[0]) * static_cast<u64>(size[1]) *
               static_cast<u64>(size[2]);
    }
    usize index(i32 x, i32 y, i32 z) const {
        return static_cast<usize>(x) + static_cast<usize>(y) * size[0] +
               static_cast<usize>(z) * size[0] * size[1];
    }
    VoxelTypeId at(i32 x, i32 y, i32 z) const { return voxels[index(x, y, z)]; }
    bool covered(i32 x, i32 y, i32 z) const { return inside[index(x, y, z)] != 0; }

    u64 solid_count() const;
    u64 covered_count() const;
    u64 content_hash() const;
};

// Lifts the current contents of a box out of the world. Nothing is removed; a clip is a
// copy until something stamps it.
Clip capture_clip(const World& world, i64 x0, i64 y0, i64 z0, i64 x1, i64 y1, i64 z1);

// Rotates about a world axis (0 = x, 1 = y, 2 = z). The voxel count is preserved exactly.
Clip rotate_clip(const Clip& clip, u32 axis, f64 radians);

// Rotations about all three axes, applied x then y then z, from the original clip.
Clip rotate_clip(const Clip& clip, const f64 radians[3]);

Clip mirror_clip(const Clip& clip, u32 axis);

// Resizing, at any ratio, per axis.
//
// Rotation can promise that not one voxel is lost or invented, because a rotation is a
// bijection. Resizing cannot, and no implementation could: making a thousand-brick wall
// twice as big means eight thousand bricks, and there is nowhere for that to come from
// except creating it. What this does promise is that **the destination is never torn**:
//
//  - Every destination voxel is decided by the source region that maps onto it, so there
//    are no holes and no stripes — the failure mode of a naive forward resample, where
//    source voxels are scattered into the destination and miss.
//  - Where that region covers less than one voxel (growing), the answer is that voxel:
//    growing by a whole number reproduces each voxel as a solid block, exactly.
//  - Where it covers many (shrinking), the answer is whichever type there is most of, with
//    ties going to the lowest type id so that two machines agree.
//
// Matter therefore scales with volume, and the ledger records the difference as a source or
// a sink like any other creative edit.
// No upper limit. The only floor is that an axis cannot go below a single voxel, because a
// clip with a zero-width side is not a smaller clip, it is nothing.
Clip scale_clip(const Clip& clip, const f64 factor[3]);

// The ops that stamp `clip` with its (0,0,0) cell at the given world voxel.
u64 clip_to_ops(const Clip& clip, i64 ox, i64 oy, i64 oz, PasteMode mode, u64 tick, u32 player,
                std::vector<Op>& out);

}  // namespace ws
