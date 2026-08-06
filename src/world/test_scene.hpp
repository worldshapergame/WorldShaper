#pragma once
// The scripted test scene (answer O16).
//
// Stages 3 to 5 need something to look at before terrain exists, and a flat plane is a
// poor test: it has no interiors, no overhangs, no thin geometry and no enclosed volumes,
// which are exactly the cases that break a ray marcher, a collision capsule and a lighting
// cache. So the scene is built from a script instead — rooms, towers, tunnels, arches,
// a stepped pit and a lattice.
//
// Because it is deterministic and built from ops, it doubles as the regression benchmark:
// the same seed produces the same world on every machine, so a frame-time or memory
// measurement taken today is comparable with one taken in Stage 20.

#include "core/types.hpp"
#include "world/voxel_type.hpp"   // VoxelTypeId

namespace ws {

class World;
class MatterLedger;

struct TestScenePalette {
    VoxelTypeId stone = 0;
    VoxelTypeId stone_dark = 0;
    VoxelTypeId stone_light = 0;
    VoxelTypeId wood = 0;
    VoxelTypeId metal = 0;
    VoxelTypeId glass = 0;
    VoxelTypeId lamp = 0;
};

// Registers the scene's materials. Safe to call once per world.
TestScenePalette create_test_palette(VoxelTypeTable& types, const TagRegistry& tags);

// Builds the scene centred on the origin. `radius` is in voxels; 1024 (32 m) is a good
// default and roughly fills the 512 x 512 x 128 m brief in the Stage 2 exit criteria when
// scaled up.
void build_test_scene(World& world, const TestScenePalette& palette, i64 radius,
                      MatterLedger& ledger);

}  // namespace ws
