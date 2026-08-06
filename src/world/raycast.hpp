#pragma once
// Picking: which voxel is the player looking at.
//
// Deliberately against the World rather than the GPU mirror. The world is the truth; the
// mirror is a copy of whatever has been streamed, and a tool that edited what the GPU
// happens to hold rather than what the world holds would carve holes in the wrong place
// the moment streaming fell behind.
//
// Costs one traversal per frame, so it is written for clarity with the obvious skips
// (chunk and brick lookups are cached until the ray crosses out of them) rather than for
// the last cycle.

#include "core/types.hpp"
#include "world/voxel_type.hpp"

namespace ws {

class World;

struct RayHit {
    bool hit = false;
    i64 x = 0;
    i64 y = 0;
    i64 z = 0;
    i32 nx = 0;          // face normal, pointing back along the ray
    i32 ny = 0;
    i32 nz = 0;
    f64 distance = 0.0;  // in voxels
    VoxelTypeId type = kAir;

    // The empty voxel just outside the face that was hit — where a placement goes.
    i64 place_x() const { return x + nx; }
    i64 place_y() const { return y + ny; }
    i64 place_z() const { return z + nz; }
};

// Origin and direction are in voxel units. Direction need not be normalised; distance is
// reported along the normalised direction.
RayHit raycast(const World& world, f64 ox, f64 oy, f64 oz, f64 dx, f64 dy, f64 dz,
               f64 max_distance);

}  // namespace ws
