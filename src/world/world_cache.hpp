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
//
// # Why a world can be written as a clip plus its edits — R11f
//
// Everything above is about not paying twice for the same answer. What it does not answer is why
// the answer has to be *written down* at all. A world built from a clip and never touched is
// recoverable from the clip: the same source at the same resolution over the same leaf set gives
// the same voxels back, and the file is then several hundred megabytes of something already on
// disk in two hundred lines of script.
//
// What is NOT recoverable is the handful of things a clip does not say. Which boxes the ladder
// sharpened and how far (`CachedRegion`), what the despeckler was allowed to touch
// (`CachedStipple`), where the lamps are (`CachedEmitters`) — and, the one that matters most,
// every voxel somebody changed. A clip has no idea a player cut a doorway through its wall.
//
// So a file may be written in `WorldCacheMode::EditOnly`, where the voxel payload is not the world
// but the DIFFERENCE between the world and the one its clip builds. The reader is handed that
// clip-built world — the `baseline` — and lays the difference over it. A brick nobody touched is
// not in the file at all; a brick somebody carved is in the file whole.
//
// **This is the one thing here that can lose somebody's building, and every decision below is made
// that way round.** A world that comes back slow is a nuisance; a world that comes back missing a
// carving is the reason people stop trusting a save. Concretely that means: the mode is a byte in
// the header and never inferred; an edit-only file read with no baseline is refused outright
// rather than applied over nothing; the baseline is checked chunk by chunk against a fingerprint
// written beside the difference; a brick that is *air* where the clip puts matter is written down
// as an explicit clearing rather than left to be noticed; and a writer that knows which boxes were
// edited may name them, so a brick that agrees with the clip today is still written if somebody's
// hands were on it. When the two readings differ, the file carries the larger one.

#include <string>
#include <vector>

#include "core/types.hpp"
#include "world/light_list.hpp"
#include "world/op.hpp"
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

// Which of the two things a file is: a whole world, or a world's difference from its clip.
//
// It is a byte in the header and it is never inferred, because the two have the same shape when
// they are empty and opposite meanings — trap 7 in the one place here where it costs a building.
// An edit-only file over a world nobody has touched holds NO voxels, and a whole-world file of an
// empty world holds no voxels either. Read the first as the second and the building vanishes; read
// the second as the first and whatever baseline happens to be in memory becomes the world.
enum class WorldCacheMode : u8 {
    Whole = 0,      // every voxel, as this file has always been written
    EditOnly = 1,   // the difference from what the clip builds; needs a baseline to be read
};

// A box of voxels the writer states somebody edited — whether or not it still differs from what
// the clip builds.
//
// The difference on its own is nearly enough, and the gap it leaves is the one that loses a
// building. A brick is left out of an edit-only file when it matches the baseline, and a player
// who carves a niche and fills it back in, or who places by hand the same stone the clip would
// have placed, owns a brick that matches. Nothing is lost while the baseline stays the same one —
// and the moment the clip or the sampler moves under the file, those bricks come back as whatever
// the clip now says, with no record that a person ever chose them.
//
// So a writer that KNOWS what was edited — there is an op log, and it knows exactly — may say so.
// Every brick a named box touches is written in full even where it agrees with the clip, including
// written as an explicit clearing where it is now air: that is the carve that takes away matter
// the clip would otherwise put back, and it is the case the difference alone cannot see.
//
// Voxel coordinates, inclusive of both corners.
struct CachedEditBox {
    i64 low[3]{};
    i64 high[3]{};
};

// How many boxes a file will carry before the writer stops taking them one at a time.
//
// A box is 48 bytes, so this is three megabytes at the ceiling and it is never reached by a
// person: a chisel stroke is one op, and a very long evening is thousands. It exists because the
// op log is also where a script's `fill` ops land, and a clip driven from the console can append
// as many as it likes -- and a list that grows without a bound turns "what was edited" from a
// safeguard into the largest thing in the file.
inline constexpr usize kMaxEditBoxes = 65536;

// What an op log says somebody's hands were on, as boxes the world cache can be handed.
//
// **This is the piece R11f was missing and the reason its first two data-loss cases were open.**
// The format has always been able to carry named boxes; nothing produced any. Without them the
// file holds only the DIFFERENCE from the clip, and a difference cannot see two things a person
// did:
//
//   - a brick they carved and refilled with the material the clip would have used, which agrees
//     with the clip today and comes back as the clip's the moment the clip moves;
//   - a swing through open air the clip agrees about, which leaves no difference at all and is
//     filled in the day the clip grows a buttress there.
//
// Both are recoverable from the op log, which knows exactly which boxes were written to, and from
// nothing else. So this turns the log into that list.
//
// EVERY op counts, including one that changed nothing. `apply_op` reports `voxels_changed`, and a
// swing that met only air reports nought -- which is precisely the case above. "Somebody's hands
// were here" is a fact about the op, not about its result.
//
// Undo counts too, and it has to: an undo is an ordinary op through the same log (see
// EditHistory), so a carve and its undo are two boxes over the same voxels. That writes a brick
// the person put back exactly as the clip has it, which is the intended cost -- the alternative is
// deciding from outside which of a person's actions were "real", and there is no such thing.
//
// Boxes are normalised, exact duplicates are dropped, and a box wholly inside a box already in the
// list is dropped. Nothing else is merged: a bounding box over two carves at opposite ends of a
// building would name every brick between them and put the whole building in the file, which is
// the failure this is trying to avoid, upside down.
std::vector<CachedEditBox> edit_boxes_from_ops(const std::vector<Op>& ops);

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

    // ---- R11f: a world as a clip plus its edits -------------------------------------------
    //
    // ON THE WAY OUT: the world this file's voxels are a difference FROM, and the switch that puts
    // the write into edit-only mode. Null writes every voxel, exactly as before, and every file
    // written before R11f is that. It must not be `world` itself: a world differenced against
    // itself is empty by construction, so the file would say "this world is exactly what the clip
    // builds" over a world somebody has spent an evening carving, and the writer refuses it.
    //
    // ON THE WAY IN: the world the difference is applied TO. It has to be the world the clip
    // builds, resumed to the same leaf set the file carries, and the reader checks that chunk by
    // chunk before it applies anything. Point it at `world` and the difference is laid down in
    // place, which is the ordinary path and costs nothing; point it at a separate world and that
    // world's chunks are copied across first.
    //
    // A null baseline against an edit-only file is refused, loudly. Applying a difference over
    // nothing produces a world that is only the carvings — a building reduced to the holes cut in
    // it — and that failure looks enough like a partly-loaded world to be argued about for an hour.
    const World* baseline = nullptr;

    // What was edited, as boxes. See CachedEditBox for why the difference alone is not enough.
    //
    // `edits_named` is separate from an empty list, and it is `stipple_taken`'s reason exactly:
    // "the op log is empty, nobody has touched this world" and "nobody told the writer what was
    // edited" are both an empty vector and mean opposite things. The first says the difference is
    // the whole truth. The second says the difference is merely all anybody knows.
    bool edits_named = false;
    std::vector<CachedEditBox> edited;

    // Filled in by the reader; ignored on the way out.
    WorldCacheMode mode = WorldCacheMode::Whole;
    // False when the baseline handed to the reader is not the one the file was written against.
    //
    // The edits are applied anyway and the read still succeeds. Every brick the file names is
    // written in full, so nothing the file KNOWS about is lost by going ahead — what changes is
    // that everything the file left to the clip comes back as the clip now builds it, which is not
    // necessarily how it was saved. Refusing instead would hand the caller nothing at all, and a
    // caller with nothing at all rebuilds from the clip, which loses the carvings outright. So the
    // reader says what happened and lets the caller weigh it; it will not throw a building away
    // over a disagreement it can survive.
    bool baseline_agreed = true;
    u32 baseline_chunks_differing = 0;
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

// What a write actually put in the file, brick by brick.
//
// It is the answer to R11f's second gate clause — "no derived node is in the file" — and a gate
// wants a NUMBER rather than a size. A file can be small for the wrong reason (a world that failed
// to build is small too), so the claim is not "the file shrank": it is that a world nobody has
// touched writes nought bricks and nought clearings, and every brick it holds is one the clip does
// not build.
struct WorldCacheWritten {
    u32 bricks_written = 0;             // bricks the file carries in full
    u32 bricks_cleared = 0;             // bricks said to be air against a clip that fills them
    u32 bricks_left_to_the_clip = 0;    // derived, and therefore not in the file at all
    u32 chunks_fingerprinted = 0;
};

// Writes to a temporary beside the target and renames, so a run interrupted mid-write leaves the
// old cache intact rather than a truncated one that looks valid.
//
// Writes every voxel when `cache.baseline` is null, and the difference from that baseline when it
// is not. See WorldCache::baseline.
bool write_world_cache(const std::string& path, u64 key, const WorldCache& cache,
                       WorldCacheWritten* written = nullptr);

// Returns false — quietly, and without touching anything — when the file is missing, is from
// another version, or was built from different source. A cache miss is not an error.
// Whether the cached world on disk was built for this key, read from its header alone.
bool world_cache_matches(const std::string& path, u64 key);

// Which mode the file on disk is in, from its header alone, and false if there is no readable
// header of this version there.
//
// A caller needs this BEFORE it reads: an edit-only file cannot be opened without first building
// the world its clip describes, and there is no way to discover that halfway through except by
// failing after the expensive part. Seventeen bytes, so it costs nothing to ask first.
bool world_cache_mode_of(const std::string& path, WorldCacheMode& out);

// Reads it back. An edit-only file needs `cache.baseline` set to the world its clip builds, and is
// refused without one; a whole-world file ignores the baseline entirely.
bool read_world_cache(const std::string& path, u64 key, WorldCache& cache, JobSystem* jobs);

}  // namespace ws
