#pragma once
// Where the lamps are, kept apart from the voxels they were found in. R9g's second half.
//
// # The fault this closes
//
// `build_light_list` walks the chunks the `World` happens to be holding. That makes a lamp's
// existence a fact about what is LOADED rather than about where the lamp is: a fitting in a region
// whose voxels are not in memory does not light anything, and it comes back on when the voxels do.
// It is the face set's fault (R9a) one system along — the light is defined by where the camera is
// standing rather than by where the light is.
//
// The fix is to persist the FITTINGS and not to load the voxels. A chunk's emissive cells are a few
// dozen records of 88 bytes; the chunk they were read out of is megabytes. So they are kept here,
// written to a sidecar beside the world, and read back with the index — and the light list is
// merged from THIS rather than from whatever is resident.
//
// # What "known" means, and it is trap 7
//
// A chunk that is absent from this store means *nobody has looked*, never *there are no lamps
// there*. The two have the same shape — an empty answer — and opposite meanings: read the first as
// the second and a building loads with its lights off. So `known()` is a separate question from
// `cells()`, an unknown chunk is scanned the moment its voxels are in front of us, and a file that
// carries nothing leaves every chunk unknown rather than empty.
//
// # Why an unloaded chunk and a DELETED chunk are not the same thing
//
// Both end with a chunk that is not in the `World`, and only one of them means the lamps are gone.
// The difference is that a deletion is announced: `announce_world_change` names the box that
// changed, `forget_box` drops exactly those chunks, and the next refresh finds them unknown and
// rescans what is left. An unload announces nothing, so what is remembered stays remembered. That
// is the whole safety argument for keeping a fitting whose voxels are absent, and it is why
// `forget_box` is not optional at the call site.

#include <string>
#include <unordered_map>
#include <vector>

#include "core/types.hpp"
#include "world/light_list.hpp"
#include "world/world.hpp"

namespace ws {

// R9g's control arm, compile-time because the flag it wants lives in `main.cpp` and this is not
// that file. `false` restores exactly what was there before: a fitting exists only while the chunk
// it was found in is resident, so the list is a fact about residency again.
//
// The arm this switches is one line of `refresh` — what happens to a remembered chunk the world no
// longer holds — so both arms run the same code and differ by what is in a map, which is what D407
// asks of an A/B and what `--no-emitter-cache` already does for the scan.
inline constexpr bool kEmitterPersist = true;

// What a refresh does with a chunk it remembers and the world does not have.
enum class EmitterResidency : u8 {
    kKeep,   // the fitting outlives its voxels — R9g
    kDrop,   // only what is loaded exists — what this did before
};

inline constexpr EmitterResidency kEmitterResidency =
    kEmitterPersist ? EmitterResidency::kKeep : EmitterResidency::kDrop;

// What one refresh had to do, so that "the lamps came back without their voxels" is a printed
// number rather than a hope. `absent` is the count this stage exists for: chunks whose fittings are
// in the list and whose voxels are not in memory.
struct EmitterScan {
    u32 scanned = 0;   // resident, not known, read brick by brick
    u32 reused = 0;    // resident and already known
    u32 absent = 0;    // known, not resident, and kept anyway
    u32 dropped = 0;   // known, not resident, and forgotten (the control arm)
};

// The emissive cells of the world, keyed by chunk, whether or not that chunk is loaded.
//
// Per chunk rather than per region, following D588: it is the granularity an edit invalidates at,
// and the one where a cluster cell cannot straddle a boundary — a cell is 4 voxels, a chunk is 256,
// and 4 divides 256 exactly, so two chunks' cells have disjoint keys and simply concatenate.
class EmitterStore {
public:
    bool known(const ChunkCoord& coord) const { return by_chunk_.find(coord) != by_chunk_.end(); }

    // Null for a chunk nobody has looked at. An empty vector for a chunk that was looked at and
    // holds no emitter — the two are different answers and this is where they stay different.
    const std::vector<EmissiveCell>* cells(const ChunkCoord& coord) const;

    void remember(const ChunkCoord& coord, std::vector<EmissiveCell> cells);

    bool forget(const ChunkCoord& coord);

    // Every chunk the inclusive voxel box touches. This is what an announced change calls, and it
    // is what keeps a deleted lamp from being remembered for ever.
    usize forget_box(const i64 lo[3], const i64 hi[3]);

    void clear() { by_chunk_.clear(); }

    usize chunks() const { return by_chunk_.size(); }
    usize cell_count() const;
    std::vector<ChunkCoord> sorted_coords() const;

    // Read whatever is resident and not yet known; keep everything already known. Returns what it
    // had to do. `residency` is the control arm and defaults to what the build is set to.
    EmitterScan refresh(const World& world, const VoxelTypeTable& types,
                        EmitterResidency residency = kEmitterResidency);

    // Every known chunk's cells, resident or not, in chunk order. The half that makes a lamp exist
    // because of where the LAMP is: `merge_light_list` sees the same set whatever is loaded.
    //
    // Ordered because the merge grows fittings from whichever cell it reaches first, and an
    // unordered_map's iteration order is not something to build a render on.
    std::vector<EmissiveCell> cells_for_merge() const;

private:
    std::unordered_map<ChunkCoord, std::vector<EmissiveCell>, ChunkCoordHash> by_chunk_;
};

// The list the tracer aims at, from the fittings rather than from the voxels. Ranked and capped by
// `merge_light_list` exactly as before, so a world whose chunks are all resident produces the
// identical list — which is the gate, and it is identity rather than plausibility.
std::vector<LightSource> build_light_list_from_store(const EmitterStore& store, i64 centre_x,
                                                     i64 centre_y, i64 centre_z);

// The identity of a store, so "it came back off the disk unchanged" is a fact. Over the chunks in
// sorted order and the cells as bytes, which is safe because `EmissiveCell` is a packed POD — the
// static_assert in the .cpp is what makes that true rather than assumed.
u64 emitter_store_hash(const EmitterStore& store);

// ---- the sidecar ------------------------------------------------------------------------------
//
// Beside the world cache rather than inside it, and that is the point of it: this file is kilobytes
// for a whole world where one chunk of voxels is megabytes, so it can be read on its own, at load,
// without opening the world at all. `key` is the world's own cache key — a file written for one
// world read against another is refused rather than believed.

// The name of the sidecar for a world cache file: the same path with `.lamps` on the end.
std::string emitter_sidecar_path(const std::string& world_cache_path);

bool write_emitter_store(const std::string& path, u64 key, const EmitterStore& store);

// Fills in what the store does not already know, and never overwrites what it does. What is in
// memory was read from the world in front of us; what is on disk is a memory of one. When the two
// disagree the world wins.
//
// False for a file that is missing, short, of the wrong version, written for another world, or
// whose payload does not hash to what its header says. Nothing is changed on a refusal, so a
// corrupt sidecar costs a scan and never a wrong answer.
bool read_emitter_store(const std::string& path, u64 key, EmitterStore& store);

}  // namespace ws
