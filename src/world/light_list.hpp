#pragma once
// Where the emitters are, so a surface can ask a lamp for light instead of hoping to find it.
//
// The path tracer finds the sun by aiming at it — next-event estimation — and that is why
// sunlight is clean. Emissive voxels had no equivalent: a lamp was only ever found when a
// diffuse bounce happened to land on it. A lamp is a few voxels across and a hemisphere is
// large, so indoors, where lamps are the only light there is, almost every sample came back
// black and the few that hit came back very bright. That is the interior noise.
//
// This is the list those rays need to aim at. Emissive voxels are merged into clusters,
// because a lamp is made of many voxels and sampling each of them separately would spend the
// whole budget on one fitting.

#include <vector>

#include "core/types.hpp"
#include "world/voxel_type.hpp"

namespace ws {

class World;

// One emitter, in absolute world voxels, with the radiance it gives off.
struct LightSource {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
    f32 red = 0.0f;
    f32 green = 0.0f;
    f32 blue = 0.0f;
    // How many voxels were merged into this one. A bigger cluster is a brighter fitting, and
    // the shader scales by it rather than pretending every lamp is one voxel.
    u32 voxels = 0;
};
static_assert(sizeof(LightSource) == 28, "LightSource is copied to the GPU as-is");

// Voxels this far apart or closer are treated as the same fitting. Four voxels is 12 cm: two
// lamps that close are one light as far as a shadow ray is concerned, and merging them costs
// nothing anybody can see.
inline constexpr i64 kLightClusterVoxels = 4;

// More than this and the furthest are dropped. A thousand fittings is far more than a room
// has, and the cost of sampling is per light in the list rather than per light in the world.
inline constexpr usize kMaxLights = 1024;

// Collect every emissive voxel in the world, merged into clusters, nearest to `centre` first.
//
// Nearest first because the list is capped: when a build has more lamps than the cap, the ones
// worth sampling are the ones near enough to light what is on screen.
std::vector<LightSource> build_light_list(const World& world, const VoxelTypeTable& types,
                                          i64 centre_x, i64 centre_y, i64 centre_z);

}  // namespace ws
