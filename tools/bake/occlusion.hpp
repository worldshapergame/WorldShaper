// Ambient occlusion, baked to a surface atlas — one texel per exposed voxel face.
//
// # What this is for, and what it is NOT
//
// The viewer already has two things that get called ambient occlusion and neither of them is it:
//
//   corner occlusion   four two-bit values on every quad, from the eight voxels round each corner.
//                      The classic Minecraft vertex darkening. It is ONE VOXEL WIDE and knows
//                      nothing outside its own cell.
//   sky visibility     the light grid: how much of the sky reaches a lattice point 0.4 m from its
//                      neighbours, read trilinearly. It is the ROOM a surface stands in.
//
// Between one voxel and forty centimetres is the entire middle scale, and it is the scale this
// building is made of: 120 coffers in the dome, twenty-four flutes on every shaft, the dentils
// under the cornice, the niches, the reveals of every window, the joint where a wall meets a
// floor. `clips/facility/rotunda.clip` says it outright — a coffer's whole appearance is "a soft
// gradient from a bright lip to a dark pan", and "if ambient occlusion is wrong, a coffer reads as
// a flat dark square". It did. That is what this bakes.
//
// So: a hemisphere of rays about each face's own normal, out to 0.45 m, weighted by distance,
// traced against the clip's own voxels at the resolution it was sampled at.
//
// # Why an atlas and not a volume
//
// The obvious answer, and the one the light grid already uses, is a volume at a finer cell. It was
// measured and rejected, on three counts:
//
//   MEMORY. A volume is n^3 and a surface is n^2. The rotunda fragment is a 12.6 x 11.6 x 12.6 m
//   box: a 0.1 m volume of it is 1.84 M cells, and its whole exposed surface at the 16/m it is
//   sampled at is about a quarter of that. The atlas is both smaller AND sharper — 6.25 cm against
//   10 cm — and the gap widens with every clip that is more air than stone, which is every clip.
//
//   A LATTICE POINT HAS NO NORMAL. Hemisphere-sampled against the surface normal is what ambient
//   occlusion IS; a point in space can only carry sphere openness, which is a blunter quantity
//   that says the same thing about the floor and the ceiling of a 0.3 m recess. Every quad here is
//   axis-aligned and knows exactly which of six directions it faces, so the hemisphere is free.
//
//   A VOLUME LEAKS THROUGH WALLS. A trilinear fetch near a 0.15 m wall blends the open room in
//   front with the stone behind, which is the fault the light grid needed its "half, twice"
//   neighbour fill to survive (see bake_light). At a 0.1 m cell the leak is worse, not better,
//   because the fetch reaches further in voxels. An atlas is ON the surface and has no inside.
//
// What the atlas costs, said plainly: one byte per exposed voxel face, and the run of texels a
// quad owns has to be found. That second part is free — the runs are allocated in the order the
// quads are written to the file, so the viewer prefix-sums `w * h` over the quads it already has
// and needs no per-quad offset in the file at all. The total is written down and checked, so the
// two derivations either agree or one of them says so.
//
// # It does not touch corner occlusion
//
// Corner occlusion stays exactly as it is: it is a different term at a different scale, other work
// depends on it, and the two multiply. One is the voxel's own shape, one is the recess it sits in.

#ifndef WS_TOOLS_BAKE_OCCLUSION_HPP
#define WS_TOOLS_BAKE_OCCLUSION_HPP

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/jobs.hpp"
#include "core/types.hpp"

namespace ws::bake {

using ws::f32;
using ws::f64;
using ws::i32;
using ws::u32;
using ws::u8;
using ws::usize;

// One merged face, as the mesher made it and in the order the file writes it. `face` is the
// baker's own numbering: 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z.
struct AoQuad {
    i32 x = 0, y = 0, z = 0;   // the voxel the face belongs to
    i32 w = 1, h = 1;          // extent along the face's two in-plane axes
    i32 face = 0;
};

struct Occlusion {
    u32 atlas_width = 0;    // texels across; a run may wrap, and the reader unwraps by index
    u32 texel_count = 0;    // exactly the number of exposed voxel faces
    f32 radius = 0.0f;      // metres the hemisphere reached
    u32 rays = 0;
    std::vector<u8> texels;   // atlas_width * rows, one byte a texel, 255 is open
};

// The face tables, and they are the same six the mesher uses. Written out again rather than shared
// because this header has to agree with the FILE's face order, which is what the viewer draws by --
// if the two ever disagree the atlas is applied to the wrong faces and nothing says so.
inline constexpr i32 kAoFaceAxis[6] = {0, 0, 1, 1, 2, 2};
inline constexpr i32 kAoFaceU[6] = {1, 1, 2, 2, 0, 0};
inline constexpr i32 kAoFaceV[6] = {2, 2, 0, 0, 1, 1};
inline constexpr i32 kAoFaceSign[6] = {1, -1, 1, -1, 1, -1};

// A cosine-weighted hemisphere about +Z, by the Fibonacci construction, and the SAME set for every
// texel.
//
// Fixed directions rather than jittered ones on purpose. A different random set per texel turns a
// smooth gradient into noise at one texel per voxel face, and there is no denoiser downstream of
// this and never will be; the same set everywhere makes the result a smooth function of position,
// which is what a coffer's gradient has to be. Cosine weighting is in the directions themselves,
// so every ray counts the same and the sum needs no weights.
inline std::vector<f64> hemisphere_directions(i32 count) {
    std::vector<f64> out;
    out.reserve(static_cast<usize>(count) * 3);
    const f64 golden = 3.14159265358979323846 * (3.0 - std::sqrt(5.0));
    for (i32 i = 0; i < count; ++i) {
        // z = sqrt(1 - u) with u uniform is the cosine-weighted hemisphere; the stratified u keeps
        // the set even instead of clumped.
        const f64 u = (static_cast<f64>(i) + 0.5) / static_cast<f64>(count);
        const f64 z = std::sqrt(1.0 - u);
        const f64 r = std::sqrt(std::max(0.0, u));
        const f64 theta = golden * static_cast<f64>(i);
        out.push_back(std::cos(theta) * r);
        out.push_back(std::sin(theta) * r);
        out.push_back(z);
    }
    return out;
}

// How much of the hemisphere over one face is open.
//
// `solid(x, y, z)` is the clip's own voxels at the resolution it was sampled at, out of bounds
// being air -- the same rule the mesher uses, so a face on the edge of the box is open to the sky
// rather than walled in by the box.
//
// The falloff is `1 - (t/R)^3` rather than `1 - t/R`, and that cube is not decoration. A coffer at
// the top ring of the dome is 0.309 m across and 0.225 m deep: everything that occludes its pan is
// at two thirds of the radius, where a linear falloff has already thrown away three quarters of
// it, and the pan comes back at 0.8 -- which is to say, still a flat square. The cube keeps a
// blocker at two thirds of the radius worth 0.7 of a near one and still tapers to nothing at R, so
// nothing pops when a wall crosses the radius.
template <typename Solid>
inline f64 face_openness(const Solid& solid, const i32 cell[3], i32 face, i32 du, i32 dv,
                         const std::vector<f64>& directions, f64 voxel, f64 radius, i32 steps) {
    const i32 axis = kAoFaceAxis[face];
    const i32 u = kAoFaceU[face];
    const i32 v = kAoFaceV[face];
    const i32 sign = kAoFaceSign[face];

    // The face's own frame: n out of the surface, and the two in-plane axes.
    f64 n[3] = {0, 0, 0};
    n[axis] = static_cast<f64>(sign);
    f64 eu[3] = {0, 0, 0};
    eu[u] = 1.0;
    f64 ev[3] = {0, 0, 0};
    ev[v] = 1.0;

    // The centre of this texel's face, in voxels, lifted a quarter of a voxel clear of the plane.
    // Grazing rays would otherwise sample points that land in the surface's own cell by rounding
    // and report a flat wall as half occluded.
    const f64 from[3] = {
        static_cast<f64>(cell[0]) + 0.5 + n[0] * 0.75 + eu[0] * static_cast<f64>(du) +
            ev[0] * static_cast<f64>(dv),
        static_cast<f64>(cell[1]) + 0.5 + n[1] * 0.75 + eu[1] * static_cast<f64>(du) +
            ev[1] * static_cast<f64>(dv),
        static_cast<f64>(cell[2]) + 0.5 + n[2] * 0.75 + eu[2] * static_cast<f64>(du) +
            ev[2] * static_cast<f64>(dv),
    };

    const usize rays = directions.size() / 3;
    const f64 step = radius / static_cast<f64>(steps);
    f64 occluded = 0.0;
    for (usize r = 0; r < rays; ++r) {
        const f64 a = directions[r * 3 + 0];
        const f64 b = directions[r * 3 + 1];
        const f64 c = directions[r * 3 + 2];
        const f64 dir[3] = {eu[0] * a + ev[0] * b + n[0] * c, eu[1] * a + ev[1] * b + n[1] * c,
                            eu[2] * a + ev[2] * b + n[2] * c};
        for (i32 s = 1; s <= steps; ++s) {
            const f64 t = static_cast<f64>(s) * step;
            const f64 along = t / voxel;
            const i32 gx = static_cast<i32>(std::floor(from[0] + dir[0] * along));
            const i32 gy = static_cast<i32>(std::floor(from[1] + dir[1] * along));
            const i32 gz = static_cast<i32>(std::floor(from[2] + dir[2] * along));
            if (!solid(gx, gy, gz)) continue;
            const f64 fraction = t / radius;
            occluded += 1.0 - fraction * fraction * fraction;
            break;
        }
    }
    return 1.0 - occluded / static_cast<f64>(rays);
}

// The whole atlas. `quads` must be in the order the file writes them -- every opaque face group in
// face order, then every transparent one -- because that order IS the allocation, and it is what
// the viewer re-derives by prefix-summing `w * h`.
template <typename Solid>
inline Occlusion bake_occlusion(const Solid& solid, const std::vector<AoQuad>& quads,
                                i32 voxels_per_metre, f64 radius_metres, i32 ray_count,
                                ws::JobSystem& jobs) {
    Occlusion out;
    out.radius = static_cast<f32>(radius_metres);
    out.rays = static_cast<u32>(ray_count);

    // Where every quad's run starts. The viewer computes exactly this from the quads it already
    // has, so it is never written to the file -- four bytes a quad is 1.6 MB on the facility, for
    // a number that is a prefix sum of two fields sitting next to it.
    std::vector<usize> base(quads.size(), 0);
    usize running = 0;
    for (usize i = 0; i < quads.size(); ++i) {
        base[i] = running;
        running += static_cast<usize>(std::max(1, quads[i].w)) *
                   static_cast<usize>(std::max(1, quads[i].h));
    }
    out.texel_count = static_cast<u32>(running);
    if (running == 0) {
        out.atlas_width = 1;
        out.texels.assign(1, 255);
        return out;
    }

    // 2048 across, which every WebGL2 implementation can hold and which keeps the row count at
    // four figures for the whole building. A run may wrap a row; the reader indexes by texel and
    // unwraps, so nothing has to be packed and not one texel is wasted on padding.
    const u32 width = static_cast<u32>(std::min<usize>(2048, running));
    const usize rows = (running + width - 1) / width;
    out.atlas_width = width;
    out.texels.assign(static_cast<usize>(width) * rows, 255);

    const std::vector<f64> directions = hemisphere_directions(ray_count);
    const f64 voxel = 1.0 / static_cast<f64>(std::max(1, voxels_per_metre));
    // Half a voxel a step, so nothing thinner than a voxel is stepped over, and at least eight
    // steps so a coarse clip still has a gradient rather than three bands.
    const i32 steps = std::max(8, static_cast<i32>(std::ceil(radius_metres / (voxel * 0.5))));

    jobs.parallel_for(quads.size(), 16, [&](usize begin, usize end) {
        for (usize q = begin; q < end; ++q) {
            const AoQuad& quad = quads[q];
            // The quad's corner voxel. `du` and `dv` walk it along the face's own two in-plane
            // axes inside face_openness, so nothing here has to know which axes those are.
            const i32 cell[3] = {quad.x, quad.y, quad.z};
            for (i32 dv = 0; dv < quad.h; ++dv) {
                for (i32 du = 0; du < quad.w; ++du) {
                    const f64 open =
                        face_openness(solid, cell, quad.face, du, dv, directions, voxel,
                                      radius_metres, steps);
                    const usize at = base[q] + static_cast<usize>(dv) * static_cast<usize>(quad.w) +
                                     static_cast<usize>(du);
                    out.texels[at] = static_cast<u8>(
                        std::lround(255.0 * std::min(1.0, std::max(0.0, open))));
                }
            }
        }
    });

    return out;
}

}  // namespace ws::bake

#endif   // WS_TOOLS_BAKE_OCCLUSION_HPP
