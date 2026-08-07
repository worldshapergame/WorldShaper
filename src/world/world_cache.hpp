#pragma once
// A built world, kept on disk so it does not have to be built twice.
//
// # Why
//
// A clip is a description, and turning a description into voxels is real work: the facility asks
// the field two hundred million questions, gives sixty million voxels their own material, and
// stamps the result into the world. Done well that is tens of seconds, and no amount of tuning
// turns tens of seconds into one — a tree-walking evaluator cannot answer two hundred million
// questions in a second on any CPU, and it should not have to answer them twice for the same
// answer.
//
// So the answer is kept. The first run builds and writes; every run after that reads. The file is
// keyed on what produced it — the source text and the resolution — so editing the clip invalidates
// it automatically and no stale world is ever loaded. Delete the file and the next run rebuilds it.
//
// # Why not the save format
//
// documentation/03 §9 requires saves to be byte-identical across a round trip, which means bricks
// are written in a canonical palette form and read back through a re-encode. That is exactly
// right for a world a player might lose, and exactly wrong here: this file is a cache, it is
// thrown away whenever anything about it is in doubt, and the only thing it is optimised for is
// the speed of getting back into memory. So bricks are written the way they are held and read
// straight back, and the type table is restored as an array rather than re-interned one record at
// a time.
//
// The two formats do not compete. A save is what a world *is*; this is what a world *was*, when
// re-deriving it would be slower than remembering it.

#include <string>
#include <vector>

#include "core/types.hpp"
#include "world/voxel_type.hpp"

namespace ws {

class JobSystem;
class MatterLedger;
class PropertyRegistry;
class TagRegistry;
class World;

// Everything a cached world needs to come back complete. The materials a clip declared are part
// of it, because they are what the chisel is loaded with and there is nowhere else to get them
// once the script is no longer being read.
struct WorldCache {
    TagRegistry* tags = nullptr;
    PropertyRegistry* properties = nullptr;
    VoxelTypeTable* types = nullptr;
    World* world = nullptr;
    MatterLedger* ledger = nullptr;
    std::vector<VoxelTypeId> materials;
};

// A number that changes whenever anything that would change the world changes: the source text,
// the resolution it was sampled at, and the format version.
u64 world_cache_key(const std::string& source_text, i32 voxels_per_metre);

// Writes to a temporary beside the target and renames, so a run interrupted mid-write leaves the
// old cache intact rather than a truncated one that looks valid.
bool write_world_cache(const std::string& path, u64 key, const WorldCache& cache);

// Returns false — quietly, and without touching anything — when the file is missing, is from
// another version, or was built from different source. A cache miss is not an error.
bool read_world_cache(const std::string& path, u64 key, WorldCache& cache, JobSystem* jobs);

}  // namespace ws
