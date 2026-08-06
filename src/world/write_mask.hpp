#pragma once
// Which voxels an edit is allowed to touch.
//
// Its own header because both ends of the engine need it: the op layer, where it is part of
// the record a peer replays, and the brick layer, where it decides whether a bulk write can
// take the fast path. Neither should have to include the other.

#include "core/types.hpp"
#include "world/voxel_type.hpp"

namespace ws {

enum class WriteMask : u8 {
    All = 0,     // every voxel in range
    IntoAir,     // only where the destination is empty; existing matter is never disturbed
    OntoSolid,   // only where the destination already holds something
};

constexpr bool mask_allows(WriteMask mask, VoxelTypeId destination) {
    switch (mask) {
        case WriteMask::IntoAir:   return destination == kAir;
        case WriteMask::OntoSolid: return destination != kAir;
        default:                   return true;
    }
}

}  // namespace ws
