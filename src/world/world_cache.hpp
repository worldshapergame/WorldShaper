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
// # Why a half-built world is kept too
//
// A clip is not built in one pass. It is sampled coarse, then re-sampled box by box at full
// detail, nearest and most visible first — and a box nobody can see is skipped for as long as the
// camera stands where it does. So a run from a fixed camera stops with boxes still coarse and
// never reaches "finished". Keeping only finished worlds therefore kept nothing at all: the
// facility was rebuilt from scratch on every single launch, two minutes at a time, for ever.
//
// The file records which boxes have been sharpened, so a world can be written the moment
// refinement runs out of things to do and picked up from there. A later run standing somewhere
// else sharpens what it can see and writes again, and the world converges across runs instead of
// being thrown away at the end of each one.
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

// One box of a clip that is sharpened on its own, and whether it has been.
//
// A clip is built coarse and then re-sampled box by box at full detail, nearest and most visible
// first. A box behind a wall is skipped for as long as the camera stands where it does, so a run
// from one camera reaches a fixed point with boxes still coarse — and that fixed point is worth
// keeping, provided the file says *which* boxes it is. Without that a partially sharpened world
// would be indistinguishable from a finished one, every later launch would load the blocky
// version and find nothing left to do, and the building would never come good again.
//
// The boxes are in the clip's own metres and are checked against the grid the reading run plans
// for itself. They are not the authority on where the boxes are; they only say which of them
// somebody has already paid for.
struct CachedRegion {
    f64 low[3]{};
    f64 high[3]{};
    bool done = false;
};

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
    // Empty means "this world was not built through the coarse-then-sharpen ladder", which is
    // what a clip built at its authored detail in one pass looks like. It is not the same as a
    // ladder with nothing done, which is a list of boxes all reading false.
    std::vector<CachedRegion> regions;
};

// A number that changes whenever anything that would change the world changes: the source text,
// the resolution it was sampled at, the format version — and the build that produced it.
//
// The build matters and it is easy to forget. A cache keyed only on the clip is right until the
// sampler changes, and then it is silently wrong: the file still matches its key, so it loads,
// and the change that was just made to how clips are built has no visible effect. That is a
// particularly nasty failure because it looks like the change did nothing. `build_stamp` is the
// executable's own modification time, which changes exactly when the code does.
u64 world_cache_key(const std::string& source_text, i32 voxels_per_metre, u64 build_stamp);

// Writes to a temporary beside the target and renames, so a run interrupted mid-write leaves the
// old cache intact rather than a truncated one that looks valid.
bool write_world_cache(const std::string& path, u64 key, const WorldCache& cache);

// Returns false — quietly, and without touching anything — when the file is missing, is from
// another version, or was built from different source. A cache miss is not an error.
// Whether the cached world on disk was built for this key, read from its header alone.
bool world_cache_matches(const std::string& path, u64 key);

bool read_world_cache(const std::string& path, u64 key, WorldCache& cache, JobSystem* jobs);

}  // namespace ws
