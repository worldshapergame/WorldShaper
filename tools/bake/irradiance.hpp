#pragma once
// The colour irradiance volume — the light that has BOUNCED, with the colour it bounced off.
//
// documentation/24-clip-viewer.md §2a is what this is and why. In one paragraph: the viewer's
// light grid holds two bytes a point, how much of the sun reaches it and how much of the sky. That
// is a VISIBILITY term. It cannot carry colour, so nothing in the viewer bounces light off a red
// floor onto a white vault — which is the single most visible thing a path tracer does, and the
// thing `clips/facility/rotunda.clip` was built to put under load ("porphyry, lapis and verde
// floor under a white plaster dome... if the vault comes back neutral white, the bounce is
// carrying luminance and not spectrum").
//
// So this bakes a second lattice holding INDIRECT radiance in colour. The direct sun and the sky
// stay exactly where they are, in the two visibility bytes, because a dozen other things read
// them; this is added on top and holds only light that has bounced at least once. Nothing is
// counted twice: a ray from a lattice point that reaches the sky contributes NOTHING here, because
// the sky's own arrival is already the sky byte.
//
// # An ambient cube, not spherical harmonics, and the reason is this renderer specifically
//
// A single ambient number per point makes every face of every object the same brightness and is
// why baked-ambient renderers look dead — a floor and a ceiling in one room must not agree. So the
// stored value has to be directional. The two honest choices are second-order spherical harmonics
// (9 x RGB a point) and the Half-Life 2 ambient cube (6 x RGB a point, one per axis direction).
//
// **This is an ambient cube, and it is exact here rather than approximate.** Every surface the
// rasteriser draws is a merged VOXEL FACE, and a voxel face's normal is exactly one of the six
// world axes — the shader's `u_normal` is a uniform, one of +X -X +Y -Y +Z -Z, set per draw call.
// So an ambient cube evaluated on it reads one stored value with no basis reconstruction at all:
// no ringing, no negative lobes, no dot products, one texture fetch. SH2 would cost half again the
// storage, nine fetches or a fatter texture, and would introduce ringing that shows up as dark
// haloes on exactly the surfaces this exists for. The cube is also what lets the viewer bind ONE
// 3D texture per face pass instead of sampling six.
//
// Its known weakness, said plainly: a cube cannot represent a sharp directional lobe, so a very
// strong bounce arriving from one direction is spread over the whole positive hemisphere of the
// three axes it has a component along. That is the correct trade for a term that is low-frequency
// by construction, and it is the same trade Half-Life 2 made for the same reason.
//
// # Two bounces, by iterating the same gather
//
//   1. every surface cell of a COARSE copy of the clip gets its direct lighting out of the light
//      grid the baker already cast, and a radiosity B = albedo * E_direct + emission;
//   2. every lattice point gathers B over the sphere -> volume 1, which is one bounce;
//   3. every surface cell re-reads volume 1 along its own normal and gets a new
//      B = albedo * (E_direct + E_indirect) + emission;
//   4. every lattice point gathers again -> volume 2, which is what is written.
//
// So a wall lit by a floor lit by the sun is lit, which is the whole test. Emissive voxels
// (`lamp`, `sconce`, `taper`, `candle` — the only light in the facility's halls) contribute their
// own radiance as sources in step 1, with the same 6x scale `web/js/gl.js` draws them at, so a
// sconce lights the wall behind it by exactly the amount it is seen to glow.
//
// # Storage, which matters more than accuracy here
//
// Six RGB values a point at 8 bits is EIGHTEEN bytes, against the light grid's two. On the
// facility's rotunda the light lattice is 34 x 32 x 34 = 36,992 points, so a full-resolution
// ambient cube would be 666 KB against a whole baked clip of 1.1 MB. That is not a trade worth
// making for a term that is low-frequency by construction.
//
// **So the indirect lattice is HALF the light lattice: 0.8 m rather than 0.4 m.** An eighth of the
// points, 18 bytes each, is 2.25 bytes per light-grid point — about what the visibility grid
// itself costs, for six colours instead of two greys. A coarser grid for indirect light is the
// standard trade and it is the correct one: what it loses is a sharpness the term does not have.
//
// Values are quantised as `v = 255 * sqrt(L / range)` and read back as `L = (v/255)^2 * range`,
// with range 4.0. A square is one multiply in the shader and it puts the quantisation steps where
// the eye is — a linear 8-bit encoding of a 0..4 range has a step of 0.0157, which is a third of
// the whole indirect term in a dark interior and visibly bands.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/jobs.hpp"
#include "core/types.hpp"
#include "game/clip.hpp"
#include "world/voxel_type.hpp"

namespace ws::bakeweb {

using ws::f32;
using ws::f64;
using ws::i32;
using ws::i64;
using ws::u8;
using ws::usize;

// The six directions, in the order everything else in this format uses: +X -X +Y -Y +Z -Z. It is
// `kFaceAxis`/`kFaceSign` in tools/bake_web.cpp and `FACES` in web/js/gl.js, and a disagreement
// between the three would light the left of a room from the right.
inline constexpr f64 kCubeFace[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                        {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};

// THESE MUST MATCH web/js/gl.js's Renderer constructor, and there is no way to check it from here.
//
// They are the radiance of the sun and of the sky in the viewer's own units, and the bounce has to
// be computed in the units the viewer draws in or the indirect term arrives at the wrong exposure:
// half as bright reads as "the bounce is not working", twice as bright reads as "everything is
// washed out". If somebody changes the sky in gl.js, change it here and rebake.
inline constexpr f64 kSunColour[3] = {3.30, 3.10, 2.78};
inline constexpr f64 kSkyUp[3] = {0.30, 0.47, 0.92};
inline constexpr f64 kSkyDown[3] = {0.62, 0.67, 0.74};
// gl.js: `ambient = mix(skyDown, skyUp, N.y * 0.5 + 0.5) * 0.5`, then `* mix(0.25, 1, skyVisible)`.
inline constexpr f64 kAmbientScale = 0.5;

// The decode range. `L = (v / 255)^2 * kRange`.
inline constexpr f32 kRange = 4.0f;

// What the .wsc's GIRR chunk begins with, so the viewer needs nothing from the 208-byte header.
inline constexpr usize kChunkHeaderBytes = 32;
inline constexpr u32 kChunkVersion = 1;

struct IrradianceVolume {
    i32 dims[3]{0, 0, 0};
    f32 cell = 0.8f;
    f32 range = kRange;
    // Six planes, one per face direction, each dims[0]*dims[1]*dims[2]*3 bytes of RGB8. Planes
    // rather than interleaved, so the viewer uploads each face's 3D texture from one subarray.
    std::vector<u8> planes;

    usize points() const {
        return static_cast<usize>(dims[0]) * static_cast<usize>(dims[1]) *
               static_cast<usize>(dims[2]);
    }
    bool empty() const { return planes.empty(); }

    // The whole chunk, sub-header and all, ready to append to the file.
    std::vector<u8> chunk() const {
        std::vector<u8> out(kChunkHeaderBytes + planes.size(), 0);
        const auto put_u32 = [&out](usize at, u32 value) {
            out[at + 0] = static_cast<u8>(value & 0xFFu);
            out[at + 1] = static_cast<u8>((value >> 8) & 0xFFu);
            out[at + 2] = static_cast<u8>((value >> 16) & 0xFFu);
            out[at + 3] = static_cast<u8>((value >> 24) & 0xFFu);
        };
        const auto put_f32 = [&put_u32](usize at, f32 value) {
            u32 bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            put_u32(at, bits);
        };
        put_u32(0, kChunkVersion);
        put_u32(4, static_cast<u32>(dims[0]));
        put_u32(8, static_cast<u32>(dims[1]));
        put_u32(12, static_cast<u32>(dims[2]));
        put_f32(16, cell);
        put_f32(20, range);
        put_u32(24, 6);   // planes; a later basis would say something else here
        put_u32(28, 0);
        std::memcpy(out.data() + kChunkHeaderBytes, planes.data(), planes.size());
        return out;
    }
};

struct IrradianceSettings {
    f64 origin[3]{0, 0, 0};
    f64 size_metres[3]{0, 0, 0};
    f64 sun[3]{0, 1, 0};   // normalised, pointing TOWARDS the sun
    // Half the light grid's 0.4 m. See the header comment: eighteen bytes a point is only
    // affordable at an eighth of the points, and indirect light does not miss the resolution.
    f64 cell = 0.8;
    i32 rays = 64;
    // How far a gather ray looks before it gives up and calls the answer sky. The light grid uses
    // the same 24 m for the same reason: past that the contribution is below a quantisation step
    // and the march is the whole cost of this.
    f64 reach = 24.0;
};

struct IrradianceStats {
    usize points = 0;
    usize lit_points = 0;      // outside matter, so actually gathered
    usize surface_cells = 0;   // coarse cells that emit anything
    usize emissive_cells = 0;
    f64 seconds = 0.0;
    usize bytes = 0;
};

namespace detail {

// Fibonacci again, the same construction the sky rays use, and for the same reason: with a basis
// this smooth, even spacing matters far more than count. Sixty-four of these into a six-value
// cosine-weighted basis is smooth; sixty-four random ones are not.
inline std::vector<std::array<f64, 3>> sphere(i32 count) {
    std::vector<std::array<f64, 3>> out;
    out.reserve(static_cast<usize>(count));
    const f64 golden = 3.14159265358979323846 * (3.0 - std::sqrt(5.0));
    for (i32 i = 0; i < count; ++i) {
        const f64 y = 1.0 - (static_cast<f64>(i) + 0.5) / static_cast<f64>(count) * 2.0;
        const f64 radius = std::sqrt(std::max(0.0, 1.0 - y * y));
        const f64 theta = golden * static_cast<f64>(i);
        out.push_back({std::cos(theta) * radius, y, std::sin(theta) * radius});
    }
    return out;
}

// A coarse cell that has at least one air neighbour: what it reflects, what it emits, and which
// way it faces. This is the only thing a gather ray can hit, so it is the whole of what indirect
// light in this viewer is made of.
struct Surfel {
    f32 albedo[3]{0, 0, 0};
    f32 emit[3]{0, 0, 0};
    f32 normal[3]{0, 0, 0};
    f32 radiosity[3]{0, 0, 0};
    f32 direct[3]{0, 0, 0};
    u32 samples = 0;
};

// sRGB in the file, linear in the shader, and gl.js does it with one multiply -- `albedo =
// base.rgb * base.rgb`. Matching that exactly rather than using 2.2 is what makes the colour that
// bounces off a floor the colour the viewer draws that floor as.
inline f64 to_linear(u8 channel) {
    const f64 v = static_cast<f64>(channel) / 255.0;
    return v * v;
}

// The same decode gl.js uses for an emissive: an RGB565 tint, a 0..1 scale, and a fixed 6x. A
// sconce therefore lights the wall behind it by exactly the amount it is seen to glow.
inline void emitted_radiance(const ws::VisualRecord& record, f64 out[3]) {
    out[0] = out[1] = out[2] = 0.0;
    if (record.emissive == 0) return;
    const u32 tint = record.emissive_tint;
    const f64 scale = static_cast<f64>(record.emissive) / 255.0 * 6.0;
    out[0] = static_cast<f64>((tint >> 11) & 31u) / 31.0 * scale;
    out[1] = static_cast<f64>((tint >> 5) & 63u) / 63.0 * scale;
    out[2] = static_cast<f64>(tint & 31u) / 31.0 * scale;
}

inline void sky_radiance(f64 up, f64 out[3]) {
    const f64 t = std::clamp(up * 0.5 + 0.5, 0.0, 1.0);
    for (i32 c = 0; c < 3; ++c) {
        out[c] = (kSkyDown[c] + (kSkyUp[c] - kSkyDown[c]) * t) * kAmbientScale;
    }
}

// A 3D DDA (Amanatides and Woo) over the coarse occupancy, returning the first solid cell or -1
// for "reached the sky".
//
// Not the fixed half-cell step the visibility rays use, and the difference is not speed. A ray
// crossing a cell corner diagonally can step straight through a one-cell wall, and for a
// visibility term that costs one part in thirty-two of a sky fraction. Here it costs a room the
// colour of the room next door, which is exactly the fault this term would be blamed for.
template <typename Occupancy>
i64 first_hit(const Occupancy& grid, const f64 origin[3], const f64 from[3], const f64 dir[3],
              f64 reach) {
    const f64 cell = 1.0 / static_cast<f64>(grid.voxels_per_metre);
    f64 local[3];
    i32 at[3];
    for (i32 a = 0; a < 3; ++a) {
        local[a] = (from[a] - origin[a]) / cell;
        at[a] = static_cast<i32>(std::floor(local[a]));
        if (at[a] < 0 || at[a] >= grid.dims[a]) return -1;   // started outside: it is all sky
    }

    i32 step[3];
    f64 next[3];
    f64 delta[3];
    for (i32 a = 0; a < 3; ++a) {
        if (dir[a] > 1e-12) {
            step[a] = 1;
            delta[a] = cell / dir[a];
            next[a] = (static_cast<f64>(at[a] + 1) - local[a]) * cell / dir[a];
        } else if (dir[a] < -1e-12) {
            step[a] = -1;
            delta[a] = cell / -dir[a];
            next[a] = (local[a] - static_cast<f64>(at[a])) * cell / -dir[a];
        } else {
            step[a] = 0;
            delta[a] = 1e30;
            next[a] = 1e30;
        }
    }

    f64 travelled = 0.0;
    while (travelled < reach) {
        i32 axis = 0;
        if (next[1] < next[axis]) axis = 1;
        if (next[2] < next[axis]) axis = 2;
        travelled = next[axis];
        at[axis] += step[axis];
        next[axis] += delta[axis];
        if (at[axis] < 0 || at[axis] >= grid.dims[axis]) return -1;   // out of the clip is sky
        if (grid.at(at[0], at[1], at[2])) {
            return static_cast<i64>(grid.index(at[0], at[1], at[2]));
        }
    }
    return -1;   // nothing within reach: no bounce, which is the honest answer and not black
}

}   // namespace detail

// --------------------------------------------------------------------------------------
// The whole thing
//
// `Occupancy` is whatever grid the light bake already built -- it needs `at(x,y,z)`,
// `index(x,y,z)`, `dims[3]` and `voxels_per_metre`. Templated rather than taking an interface,
// because this is called once per lattice point per ray and a virtual call there is the whole cost.
// --------------------------------------------------------------------------------------

template <typename Occupancy>
IrradianceVolume bake_irradiance(const ws::Clip& clip, const ws::VoxelTypeTable& types,
                                 const Occupancy& coarse, const std::vector<u8>& light,
                                 const i32 light_dims[3], f64 light_cell, i32 metre,
                                 const IrradianceSettings& settings, ws::JobSystem& jobs,
                                 IrradianceStats* stats) {
    using detail::Surfel;

    IrradianceVolume volume;
    volume.cell = static_cast<f32>(settings.cell);
    volume.range = kRange;
    for (i32 a = 0; a < 3; ++a) {
        volume.dims[a] = static_cast<i32>(settings.size_metres[a] / settings.cell) + 3;
    }
    const usize points = volume.points();
    if (points == 0) return volume;

    const f64 coarse_cell = 1.0 / static_cast<f64>(coarse.voxels_per_metre);
    const usize coarse_cells = static_cast<usize>(coarse.dims[0]) *
                               static_cast<usize>(coarse.dims[1]) *
                               static_cast<usize>(coarse.dims[2]);

    // ---- 1. what every coarse cell is made of ------------------------------------------------
    //
    // Only the voxels with air beside them: a voxel four deep in a wall reflects nothing at any
    // wavelength and averaging it into the cell would drag the wall's colour towards whatever the
    // inside of it happens to be made of.
    std::vector<Surfel> surfels(coarse_cells);
    {
        const i32 step = std::max(1, metre / coarse.voxels_per_metre);
        const auto solid = [&clip](i32 x, i32 y, i32 z) {
            if (x < 0 || y < 0 || z < 0 || x >= clip.size[0] || y >= clip.size[1] ||
                z >= clip.size[2]) {
                return false;
            }
            const usize i = static_cast<usize>(x) +
                            static_cast<usize>(y) * static_cast<usize>(clip.size[0]) +
                            static_cast<usize>(z) * static_cast<usize>(clip.size[0]) *
                                static_cast<usize>(clip.size[1]);
            return clip.inside[i] != 0 && clip.voxels[i] != ws::kAir;
        };
        constexpr i32 kNeighbour[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                          {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
        for (i32 z = 0; z < clip.size[2]; ++z) {
            for (i32 y = 0; y < clip.size[1]; ++y) {
                for (i32 x = 0; x < clip.size[0]; ++x) {
                    if (!solid(x, y, z)) continue;
                    f64 face[3] = {0, 0, 0};
                    bool exposed = false;
                    for (const auto& n : kNeighbour) {
                        if (solid(x + n[0], y + n[1], z + n[2])) continue;
                        exposed = true;
                        face[0] += static_cast<f64>(n[0]);
                        face[1] += static_cast<f64>(n[1]);
                        face[2] += static_cast<f64>(n[2]);
                    }
                    if (!exposed) continue;

                    const usize at = static_cast<usize>(x) +
                                     static_cast<usize>(y) * static_cast<usize>(clip.size[0]) +
                                     static_cast<usize>(z) * static_cast<usize>(clip.size[0]) *
                                         static_cast<usize>(clip.size[1]);
                    const ws::VisualRecord& record = types.visual_of(clip.voxels[at]);
                    const f64 diffuse = 1.0 - static_cast<f64>(record.metallic) / 255.0;
                    f64 emit[3];
                    detail::emitted_radiance(record, emit);

                    Surfel& cell = surfels[coarse.index(x / step, y / step, z / step)];
                    cell.albedo[0] += static_cast<f32>(detail::to_linear(record.red) * diffuse);
                    cell.albedo[1] += static_cast<f32>(detail::to_linear(record.green) * diffuse);
                    cell.albedo[2] += static_cast<f32>(detail::to_linear(record.blue) * diffuse);
                    for (i32 c = 0; c < 3; ++c) cell.emit[c] += static_cast<f32>(emit[c]);
                    for (i32 c = 0; c < 3; ++c) cell.normal[c] += static_cast<f32>(face[c]);
                    cell.samples += 1;
                }
            }
        }
    }

    // ---- 2. the direct light on each of them, out of the grid the baker already cast ----------
    usize surface_cells = 0;
    usize emissive_cells = 0;
    {
        for (i32 z = 0; z < coarse.dims[2]; ++z) {
            for (i32 y = 0; y < coarse.dims[1]; ++y) {
                for (i32 x = 0; x < coarse.dims[0]; ++x) {
                    Surfel& cell = surfels[coarse.index(x, y, z)];
                    if (cell.samples == 0) continue;
                    const f32 inverse = 1.0f / static_cast<f32>(cell.samples);
                    for (i32 c = 0; c < 3; ++c) {
                        cell.albedo[c] *= inverse;
                        cell.emit[c] *= inverse;
                    }
                    ++surface_cells;
                    if (cell.emit[0] + cell.emit[1] + cell.emit[2] > 0.0f) ++emissive_cells;

                    f64 n[3] = {cell.normal[0], cell.normal[1], cell.normal[2]};
                    const f64 length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    if (length < 1e-6) {
                        n[0] = 0.0;
                        n[1] = 1.0;
                        n[2] = 0.0;
                    } else {
                        for (i32 c = 0; c < 3; ++c) n[c] /= length;
                    }
                    for (i32 c = 0; c < 3; ++c) cell.normal[c] = static_cast<f32>(n[c]);

                    // A point one coarse cell out along the face's own normal: the air the light
                    // grid actually knows about. Read from inside the cell it would be a buried
                    // point, and every wall in the building would light itself from its own core.
                    const f64 world[3] = {
                        settings.origin[0] + (static_cast<f64>(x) + 0.5) * coarse_cell +
                            n[0] * coarse_cell,
                        settings.origin[1] + (static_cast<f64>(y) + 0.5) * coarse_cell +
                            n[1] * coarse_cell,
                        settings.origin[2] + (static_cast<f64>(z) + 0.5) * coarse_cell +
                            n[2] * coarse_cell};
                    i32 lattice[3];
                    for (i32 a = 0; a < 3; ++a) {
                        lattice[a] = static_cast<i32>(
                            std::lround((world[a] - settings.origin[a]) / light_cell));
                        lattice[a] = std::clamp(lattice[a], 0, light_dims[a] - 1);
                    }
                    const usize at = (static_cast<usize>(lattice[0]) +
                                      static_cast<usize>(lattice[1]) *
                                          static_cast<usize>(light_dims[0]) +
                                      static_cast<usize>(lattice[2]) *
                                          static_cast<usize>(light_dims[0]) *
                                          static_cast<usize>(light_dims[1])) *
                                     2;
                    const f64 sun_visible =
                        (at + 1 < light.size()) ? static_cast<f64>(light[at]) / 255.0 : 0.0;
                    const f64 sky_visible =
                        (at + 1 < light.size()) ? static_cast<f64>(light[at + 1]) / 255.0 : 0.0;

                    const f64 ndl = std::max(0.0, n[0] * settings.sun[0] + n[1] * settings.sun[1] +
                                                      n[2] * settings.sun[2]);
                    f64 sky[3];
                    detail::sky_radiance(n[1], sky);
                    // gl.js: `diffuse = albedo * (direct + ambient * occluded)`, with occluded
                    // carrying mix(0.25, 1, skyVisible). The same shape, so a surface radiates
                    // what the viewer is about to draw it as.
                    for (i32 c = 0; c < 3; ++c) {
                        cell.direct[c] = static_cast<f32>(kSunColour[c] * ndl * sun_visible +
                                                          sky[c] * (0.25 + 0.75 * sky_visible));
                        cell.radiosity[c] = cell.albedo[c] * cell.direct[c] + cell.emit[c];
                    }
                }
            }
        }
    }

    // ---- 3. gather, twice --------------------------------------------------------------------
    const std::vector<std::array<f64, 3>> rays = detail::sphere(settings.rays);
    // Cosine weights per face, normalised so that a point standing in a uniform field of radiance
    // L comes out holding exactly L on every face. That is what makes the stored number a RADIANCE
    // in the viewer's own units: the shader multiplies it by albedo and adds it, with no constant.
    std::vector<f64> weight(rays.size() * 6, 0.0);
    f64 weight_total[6] = {0, 0, 0, 0, 0, 0};
    for (usize r = 0; r < rays.size(); ++r) {
        for (i32 f = 0; f < 6; ++f) {
            const f64 w = std::max(0.0, rays[r][0] * kCubeFace[f][0] + rays[r][1] * kCubeFace[f][1] +
                                            rays[r][2] * kCubeFace[f][2]);
            weight[r * 6 + static_cast<usize>(f)] = w;
            weight_total[f] += w;
        }
    }
    for (i32 f = 0; f < 6; ++f) {
        if (weight_total[f] <= 0.0) weight_total[f] = 1.0;
    }

    std::vector<f32> cube(points * 18, 0.0f);
    std::vector<u8> known(points, 0);
    usize lit_points = 0;

    const auto gather = [&](void) {
        std::fill(cube.begin(), cube.end(), 0.0f);
        std::fill(known.begin(), known.end(), 0);
        jobs.parallel_for(static_cast<usize>(volume.dims[2]), 1, [&](usize begin, usize end) {
            for (usize slab = begin; slab < end; ++slab) {
                const i32 z = static_cast<i32>(slab);
                for (i32 y = 0; y < volume.dims[1]; ++y) {
                    for (i32 x = 0; x < volume.dims[0]; ++x) {
                        const usize at = static_cast<usize>(x) +
                                         static_cast<usize>(y) * static_cast<usize>(volume.dims[0]) +
                                         static_cast<usize>(z) * static_cast<usize>(volume.dims[0]) *
                                             static_cast<usize>(volume.dims[1]);
                        const f64 p[3] = {settings.origin[0] + static_cast<f64>(x) * settings.cell,
                                          settings.origin[1] + static_cast<f64>(y) * settings.cell,
                                          settings.origin[2] + static_cast<f64>(z) * settings.cell};
                        i32 g[3];
                        bool inside = true;
                        for (i32 a = 0; a < 3; ++a) {
                            g[a] = static_cast<i32>(
                                std::floor((p[a] - settings.origin[a]) / coarse_cell));
                            if (g[a] < 0 || g[a] >= coarse.dims[a]) inside = false;
                        }
                        // Buried in stone, exactly as the visibility grid treats it: left unknown
                        // and filled from its neighbours below, so a wall never reads its own core.
                        if (inside && coarse.at(g[0], g[1], g[2])) continue;

                        f64 sum[6][3] = {};
                        for (usize r = 0; r < rays.size(); ++r) {
                            const f64 dir[3] = {rays[r][0], rays[r][1], rays[r][2]};
                            const i64 hit =
                                detail::first_hit(coarse, settings.origin, p, dir, settings.reach);
                            // Sky is NOT added: the sky's own arrival is the sky visibility byte
                            // and adding it again would count it twice. This volume holds bounce.
                            if (hit < 0) continue;
                            const Surfel& cell = surfels[static_cast<usize>(hit)];
                            if (cell.samples == 0) continue;
                            for (i32 f = 0; f < 6; ++f) {
                                const f64 w = weight[r * 6 + static_cast<usize>(f)];
                                if (w <= 0.0) continue;
                                for (i32 c = 0; c < 3; ++c) {
                                    sum[f][c] += static_cast<f64>(cell.radiosity[c]) * w;
                                }
                            }
                        }
                        for (i32 f = 0; f < 6; ++f) {
                            for (i32 c = 0; c < 3; ++c) {
                                cube[at * 18 + static_cast<usize>(f) * 3 + static_cast<usize>(c)] =
                                    static_cast<f32>(sum[f][c] / weight_total[f]);
                            }
                        }
                        known[at] = 1;
                    }
                }
            }
        });
        // Counted here rather than at the end, because the fill below marks the buried points
        // known as it borrows from their neighbours -- so afterwards every reachable point reads
        // as "lit" and the number stops saying anything.
        lit_points = 0;
        for (usize i = 0; i < points; ++i) lit_points += known[i] ? 1u : 0u;

        // The buried points, filled from the brightest neighbour at half strength, twice — the
        // same rule and the same fraction the visibility grid uses, and for the same reason: a
        // trilinear fetch near a wall blends the air in front of it with the stone behind it, so a
        // buried point of zero draws a black outline round every surface and a buried point of
        // three quarters puts a pale band across every soffit.
        for (i32 pass = 0; pass < 2; ++pass) {
            std::vector<u8> filled = known;
            constexpr i32 kNeighbour[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                              {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
            for (i32 z = 0; z < volume.dims[2]; ++z) {
                for (i32 y = 0; y < volume.dims[1]; ++y) {
                    for (i32 x = 0; x < volume.dims[0]; ++x) {
                        const usize at = static_cast<usize>(x) +
                                         static_cast<usize>(y) * static_cast<usize>(volume.dims[0]) +
                                         static_cast<usize>(z) * static_cast<usize>(volume.dims[0]) *
                                             static_cast<usize>(volume.dims[1]);
                        if (known[at]) continue;
                        f64 best = -1.0;
                        usize from = 0;
                        for (const auto& n : kNeighbour) {
                            const i32 nx = x + n[0], ny = y + n[1], nz = z + n[2];
                            if (nx < 0 || ny < 0 || nz < 0 || nx >= volume.dims[0] ||
                                ny >= volume.dims[1] || nz >= volume.dims[2]) {
                                continue;
                            }
                            const usize nat =
                                static_cast<usize>(nx) +
                                static_cast<usize>(ny) * static_cast<usize>(volume.dims[0]) +
                                static_cast<usize>(nz) * static_cast<usize>(volume.dims[0]) *
                                    static_cast<usize>(volume.dims[1]);
                            if (!known[nat]) continue;
                            f64 bright = 0.0;
                            for (usize k = 0; k < 18; ++k) bright += cube[nat * 18 + k];
                            if (bright > best) {
                                best = bright;
                                from = nat;
                            }
                        }
                        if (best < 0.0) continue;
                        for (usize k = 0; k < 18; ++k) cube[at * 18 + k] = cube[from * 18 + k] * 0.5f;
                        filled[at] = 1;
                    }
                }
            }
            known.swap(filled);
        }
    };

    const auto cube_along = [&](const f64 world[3], const f32 n[3], f64 out[3]) {
        i32 lattice[3];
        for (i32 a = 0; a < 3; ++a) {
            lattice[a] =
                static_cast<i32>(std::lround((world[a] - settings.origin[a]) / settings.cell));
            lattice[a] = std::clamp(lattice[a], 0, volume.dims[a] - 1);
        }
        const usize at = static_cast<usize>(lattice[0]) +
                         static_cast<usize>(lattice[1]) * static_cast<usize>(volume.dims[0]) +
                         static_cast<usize>(lattice[2]) * static_cast<usize>(volume.dims[0]) *
                             static_cast<usize>(volume.dims[1]);
        // The cube reconstructed on an arbitrary normal: the squared components pick the three
        // faces the normal points into, and they sum to one. On an axis it is exactly one face,
        // which is every surface the rasteriser will ever ask about.
        const f64 n2[3] = {static_cast<f64>(n[0]) * n[0], static_cast<f64>(n[1]) * n[1],
                           static_cast<f64>(n[2]) * n[2]};
        const i32 face[3] = {n[0] >= 0.0f ? 0 : 1, n[1] >= 0.0f ? 2 : 3, n[2] >= 0.0f ? 4 : 5};
        for (i32 c = 0; c < 3; ++c) {
            out[c] = 0.0;
            for (i32 a = 0; a < 3; ++a) {
                out[c] += n2[a] * static_cast<f64>(
                                      cube[at * 18 + static_cast<usize>(face[a]) * 3 +
                                           static_cast<usize>(c)]);
            }
        }
    };

    // One bounce...
    gather();
    // ...then every surface re-lit by it, and one more. Two gathers is two bounces: a wall lit by
    // a floor lit by the sun, which is the whole reason this is iterated rather than cast once.
    for (i32 z = 0; z < coarse.dims[2]; ++z) {
        for (i32 y = 0; y < coarse.dims[1]; ++y) {
            for (i32 x = 0; x < coarse.dims[0]; ++x) {
                Surfel& cell = surfels[coarse.index(x, y, z)];
                if (cell.samples == 0) continue;
                const f64 world[3] = {
                    settings.origin[0] + (static_cast<f64>(x) + 0.5) * coarse_cell +
                        static_cast<f64>(cell.normal[0]) * coarse_cell,
                    settings.origin[1] + (static_cast<f64>(y) + 0.5) * coarse_cell +
                        static_cast<f64>(cell.normal[1]) * coarse_cell,
                    settings.origin[2] + (static_cast<f64>(z) + 0.5) * coarse_cell +
                        static_cast<f64>(cell.normal[2]) * coarse_cell};
                f64 indirect[3];
                cube_along(world, cell.normal, indirect);
                for (i32 c = 0; c < 3; ++c) {
                    cell.radiosity[c] =
                        cell.albedo[c] * (cell.direct[c] + static_cast<f32>(indirect[c])) +
                        cell.emit[c];
                }
            }
        }
    }
    gather();

    // ---- 4. out, as six planes of RGB8 -------------------------------------------------------
    volume.planes.assign(points * 18, 0);
    for (i32 f = 0; f < 6; ++f) {
        const usize plane = static_cast<usize>(f) * points * 3;
        for (usize i = 0; i < points; ++i) {
            for (i32 c = 0; c < 3; ++c) {
                const f64 value =
                    static_cast<f64>(cube[i * 18 + static_cast<usize>(f) * 3 +
                                          static_cast<usize>(c)]);
                const f64 encoded = std::sqrt(std::clamp(value / kRange, 0.0, 1.0));
                volume.planes[plane + i * 3 + static_cast<usize>(c)] =
                    static_cast<u8>(std::lround(encoded * 255.0));
            }
        }
    }

    if (stats != nullptr) {
        stats->points = points;
        stats->lit_points = lit_points;
        stats->surface_cells = surface_cells;
        stats->emissive_cells = emissive_cells;
        stats->bytes = volume.planes.size() + kChunkHeaderBytes;
    }
    return volume;
}

}   // namespace ws::bakeweb
