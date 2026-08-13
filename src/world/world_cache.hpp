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
#include "world/light_list.hpp"
#include "world/voxel_type.hpp"

namespace ws {

class JobSystem;
class MatterLedger;
class PropertyRegistry;
class TagRegistry;
class World;

// One leaf of the ladder that sharpens a clip: which node it is, and how sharp it got.
//
// A clip is built coarse and then re-sampled box by box at full detail, nearest and most visible
// first. A box behind a wall is skipped for as long as the camera stands where it does, so a run
// from one camera reaches a fixed point with boxes still coarse — and that fixed point is worth
// keeping, provided the file says *which* boxes it is. Without that a partially sharpened world
// would be indistinguishable from a finished one, every later launch would load the blocky
// version and find nothing left to do, and the building would never come good again.
//
// The list is the ladder's WHOLE leaf set, coarse leaves included, and not just the ones somebody
// has paid for. That is the difference between resuming a run and guessing at one. The ladder is
// an octree that splits where the camera wants detail, so its leaves are of every size at once,
// and the only thing that describes such a set is the set itself. It used to hold the sharp boxes
// alone and the reading run tried to recognise them by containment, which since R11c could not
// work at all: a sharp box is the SMALLEST node in the tree, and a smaller box contains nothing.
//
// `key`/`level` are the node's own key in the octree the renderer marches, so the reading run
// rebuilds the tree exactly. The corners are written beside them for a reader that wants to know
// where a leaf is without knowing the clip's bounds; the run that resumes recomputes them from the
// key instead, because a box out of a file is a claim and the bounds are the authority.
struct CachedRegion {
    i64 key[3]{};
    u32 level = 0;
    f64 low[3]{};
    f64 high[3]{};
    // Voxels per metre this leaf was last sampled at. Without it a resuming run has to assume the
    // coarsest, which does not merely waste the work of sharpening it again -- a sample is pasted
    // as a REPLACE over its whole box, so assuming coarse lets a sixteen-per-metre answer land on
    // top of a thirty-two-per-metre one and the building gets blockier the more it is loaded.
    i32 applied_per_metre = 0;
    bool done = false;
};

// One material's answer to "may a lone voxel of this be repainted, or is it a deliberate dither?"
//
// The forge takes that judgement once, over the whole clip, and hands it to every box the ladder
// sharpens afterwards -- a box's own five hundred cells cannot tell a weathering coat from a
// sampling accident, and the seams between per-box answers are visible. It is therefore something
// a world KNOWS, not something it can work out again from a corner of itself, and a cached world
// that comes back without it comes back unable to despeckle anything at all: forge::despeckle
// reads an empty verdict as "leave every speck alone, everywhere". That is what this list is for.
//
// Kept as a plain pair rather than the forge's own StippleVerdict so that world/ does not have to
// know what a clip is. The ids are indices into the type table written beside it, so they mean the
// same thing on the way back in.
struct CachedStipple {
    VoxelTypeId type = 0;
    bool may_despeckle = false;
};

// One chunk's emissive cells, kept so that finding the lamps does not mean reading the world again.
//
// R9g. Emitters are the one thing about a world that is expensive to find and tiny to store: the
// facility's 21 fittings come out of a scan of every brick of every chunk that cost **14 ms**, and
// the cells they were built from are a few hundred bytes. So they are written beside the world
// rather than rediscovered from it, and a cached world comes back knowing where its lamps are.
//
// Per CHUNK and not per region, because that is the granularity an edit invalidates at and the
// granularity the application already keeps them at -- and because a cluster cell is four voxels
// and a chunk is 256, so no cell straddles a chunk and the pieces simply concatenate.
struct CachedEmitters {
    i64 chunk_x = 0, chunk_y = 0, chunk_z = 0;
    std::vector<EmissiveCell> cells;
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
    // What the despeckler is allowed to touch. See CachedStipple: this is the one thing about a
    // world that is taken from the whole clip at once, so a run that resumes from this file has no
    // way to derive it and must be given it.
    //
    // `stipple_taken` and an empty list are NOT the same thing, and conflating them is trap 7 in
    // its purest form: "no material has any specks" and "nobody ever asked" both come out as an
    // empty map, and the second must not be read as the first. A file written by a --no-despeckle
    // run has no verdict in it, and the run that loads it needs to say so rather than quietly
    // sharpen a building full of stray voxels.
    bool stipple_taken = false;
    std::vector<CachedStipple> stipple;
    // Where the lamps are, per chunk. Empty means "this file predates R9g or was written by a run
    // that had not scanned", and the reader treats that as "scan on demand" rather than as "there
    // are no lamps" -- which is trap 7, and here it would be a building with its lights off.
    std::vector<CachedEmitters> emitters;
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
