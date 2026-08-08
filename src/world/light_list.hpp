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
// shaders/pathtrace.comp, and the two have to agree: the shader has no separate flag for a
// truncated list, it infers one from `count == kMaxLights` and turns direct light sampling off
// entirely when it sees that. So a list of exactly this length means "do not trust me", and a
// shorter one means "these are all of them".
//
// Raising it is a shader edit as well as this one, and costs 28 bytes of host-visible buffer
// per light (1024 lights is 28 KB today).
inline constexpr usize kMaxLights = 1024;

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

}  // namespace ws
