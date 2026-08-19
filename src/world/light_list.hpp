#pragma once
// Where the emitters are, so a surface can ask a lamp for light instead of hoping to find it.
//
// The path tracer finds the sun by aiming at it — next-event estimation — and that is why
// sunlight is clean. Emissive voxels had no equivalent: a lamp was only ever found when a
// diffuse bounce happened to land on it. A lamp is a few voxels across and a hemisphere is
// large, so indoors, where lamps are the only light there is, almost every sample came back
// black and the few that hit came back very bright. That is the interior noise.
//
// This is the list those rays need to aim at. Emissive voxels are merged into whole fittings,
// because a lamp is made of many voxels and sampling each of them separately would spend the
// whole budget on one fitting.

#include <vector>

#include "core/types.hpp"
#include "world/voxel_type.hpp"

namespace ws {

class World;
class Chunk;

// One emitter, in absolute world voxels, with the radiance it gives off.
struct LightSource {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
    f32 red = 0.0f;
    f32 green = 0.0f;
    f32 blue = 0.0f;
    // How big the fitting is, in the only unit the shader has for saying so. It reads this as
    // `radius = 0.87 * cbrt(voxels)` — the sphere that circumscribes a solid cube of that many
    // voxels — and that sphere has to *contain* the emitters merged into this entry, because
    // direct sampling owns them outright and anything outside the cone is owned by nobody and
    // goes out. So this is the count only when the fitting really is a cube; for anything
    // longer than it is wide it is raised until the sphere covers the box the fitting occupies.
    // Erring large costs noise, erring small costs light that never arrives.
    u32 voxels = 0;
};
static_assert(sizeof(LightSource) == 28, "LightSource is copied to the GPU as-is");

// Voxels this far apart or closer are treated as the same fitting. Four voxels is 12 cm: two
// lamps that close are one light as far as a shadow ray is concerned, and merging them costs
// nothing anybody can see.
inline constexpr i64 kLightClusterVoxels = 4;

// How far one fitting is allowed to grow while touching cells are joined up. Thirty-two voxels
// is a metre, and a metre is a chandelier — bigger than any sconce and bigger than the eye
// reads as one object.
//
// The limit is not thrift, it is the ownership guarantee. The shader covers a fitting with a
// cone drawn round its sphere, and that cone only contains the sphere while the shaded surface
// is outside it. A merged blob a metre across has a sphere 87 cm in radius, which is already
// close enough to the wall a sconce is bolted to; growing without limit would put wall surfaces
// inside the sphere, where part of the fitting falls outside the cone and stops being lit by
// anybody. So a long emissive run is cut into metre pieces rather than becoming one huge sphere.
inline constexpr i64 kLightFittingVoxels = 32;

// How much emptiness a merge is allowed to enclose: the sphere may stand for at most this many
// times the emissive voxels actually inside it.
//
// Merging is only free when the thing being merged is roughly a blob. A sconce is, and joining
// its cells costs nothing — the sphere ends up about a third larger than the count. A glowing
// strip one voxel wide is not: two of its cells merged give eight voxels inside a sphere that
// stands for a hundred, and the shader would then aim nine rays in ten at empty air and take
// the miss honestly. That is still unbiased and much noisier than leaving the strip in pieces,
// so past this ratio the merge is refused and the pieces stay pieces.
inline constexpr u32 kLightMergeSlack = 8;

// More than this and the weakest are dropped. Matched by `const uint kMaxLights` in
// shaders/node.glsl, and the two have to agree: the shader has no separate flag for a
// truncated list, it infers one from `count == kMaxLights` and turns direct light sampling off
// entirely when it sees that. So a list of exactly this length means "do not trust me", and a
// shorter one means "these are all of them".
//
// Raising it is a shader edit as well as this one, and costs 28 bytes of host-visible buffer
// per light (1024 lights is 28 KB today).
inline constexpr usize kMaxLights = 1024;

// ---- the two halves of finding the lamps, split so only one of them has to be redone ----------
//
// Finding the emitters is a walk of every brick of every chunk, and it was run again from scratch
// on every announced change to the world -- every chisel stroke and every region the clip ladder
// pastes. Measured on the facility: **14.15 ms on average and 14.99 ms at worst, against the edit
// that provoked it costing 0.19 ms**, to rediscover the same twenty-one fittings. That is the shape
// of the two largest costs this rewrite has already deleted (D515, D522) and it had never been
// printed.
//
// The split is where the expense is. The SCAN is per chunk and only changes when that chunk does.
// The MERGE -- cells joined into fittings, ranked, capped -- runs over a few hundred cells however
// large the world is, and it has to stay global because a fitting may straddle a chunk boundary.
//
// A cluster cell is four voxels and a chunk is 256, so cells never straddle a chunk: 4 divides 256
// exactly, and the cells of two chunks therefore have disjoint keys. That is what makes the cached
// halves simply concatenate, with no key merging and no double counting.

// One cluster cell of emissive voxels: the key it was gathered under, the box it occupies and the
// radiance summed over it. Everything `Cluster` carries inside the merge, in a form the host can
// keep between frames.
struct EmissiveCell {
    i64 key_x = 0, key_y = 0, key_z = 0;
    i64 min_x = 0, min_y = 0, min_z = 0;
    i64 max_x = 0, max_y = 0, max_z = 0;
    f32 red = 0.0f, green = 0.0f, blue = 0.0f;
    u32 voxels = 0;
};

// The emissive cells of one chunk. `base` is that chunk's origin in absolute voxels.
//
// A brick whose palette holds no emitter is rejected in a handful of comparisons, which is what
// makes this affordable per chunk; what was never affordable was doing it for every chunk in the
// world on every edit.
std::vector<EmissiveCell> scan_chunk_emitters(const Chunk& chunk, i64 base_x, i64 base_y, i64 base_z,
                                              const VoxelTypeTable& types);

// Cells into fittings, ranked by what each delivers at `centre`, capped. The half that must see
// every cell at once, and the half that costs nothing because there are only ever a few hundred.
std::vector<LightSource> merge_light_list(const std::vector<EmissiveCell>& cells, i64 centre_x,
                                          i64 centre_y, i64 centre_z);

// Collect every emissive voxel in the world, merged into whole fittings, strongest first.
//
// Two things decide what survives the cap. Merging comes first and is worth far more: a wall
// sconce is a few hundred voxels spanning a dozen cluster cells, so joining its cells up turns
// a dozen entries into one, and a hall of forty sconces goes from five hundred entries to
// forty. Ranking is the fallback — by the light each fitting would deliver at `centre`, its
// radiance over the square of its distance, so the cap is spent on the lamp over the player's
// head rather than the one behind them in another room.
std::vector<LightSource> build_light_list(const World& world, const VoxelTypeTable& types,
                                          i64 centre_x, i64 centre_y, i64 centre_z);

// The identity of a list, so that "the lamps changed" is a fact rather than an inference.
//
// The face pass accumulates each face's lamp light over hundreds of frames and then stops casting
// rays at all, so a lamp placed, deleted, moved or dimmed after that would never be noticed: a
// silent face has nothing left that could discover it. The host therefore has to SAY so, on the
// exact frame it happens, which is D373's lesson generalised — an accumulator that infers "the
// world changed" from its own samples cannot see a change that arrives before the last one
// finished.
//
// Over the whole record and not over the count, because the interesting edits do not change the
// length: dimming a sconce, retinting it, or carving one voxel off a fitting so its centre moves
// all leave a list of exactly the same size. Over the bytes rather than field by field because
// `LightSource` is a packed 28-byte POD with no padding — the static_assert above is what makes
// that safe, and it is why the assert is worth keeping.
//
// Ordering does NOT count, and the version of this that said it did was wrong about what the
// re-measure costs. The list is sorted by contribution at the camera, so it is reordered by the
// camera moving with no lamp having changed at all — and this hash is the gate on `light_reset`,
// which reopens the lamp term of every face in the store, not one face. Measured: nine chisel
// strokes from a static camera bumped the version once and the store stayed converged; the same
// nine while flying bumped it nine times and left `lamps on the card: 0 of 997,296 live faces cast
// no more rays at all`, which is a room made of per-face squares that flicker. So the hash runs
// over a canonically ordered copy. What ordering was said to buy is not lost: a permutation of the
// same records is the same set of lamps, and rank only decides which survive the `kMaxLights` cap,
// which changes the set. D500.
u64 light_list_hash(const std::vector<LightSource>& lights);

}  // namespace ws
