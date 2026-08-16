// Baked reflection probes for the clip viewer.
//
// Nothing in the viewer reflected anything. The clips were built to be reflective — `salon.clip`
// and `ballroom.clip` face walls of `mirror` at each other, `pavilion.clip` stands in a basin of
// still water, and the facility's contract declares three golds a stop apart precisely so that
// metal can be judged — and every one of them came out flat diffuse, because a rasteriser has no
// opinion about what a surface can see except the analytic sky.
//
// So: a sparse lattice of probes through the clip's open space, each one a small octahedral map of
// what it can see, ray cast here against the sampled clip, pre-filtered into a chain of roughness
// levels, and read at run time by `web/js/features/probes.js`.
//
// # Octahedral, not a cubemap
//
// One 2D texture, one atlas, one fetch. A cubemap on WebGL2 would be six faces per probe with six
// sets of seams, no way to pack many probes into one binding without an array texture, and a
// filtered mip chain that bleeds across faces unless every face is padded anyway. The octahedral
// map has exactly one square per probe and its edges fold onto themselves by a rule a border of one
// texel can satisfy — which is the same border an atlas needs regardless, so the seam costs nothing
// that was not already being paid.
//
// # Where the probes go, and why not everywhere
//
// A lattice point becomes a probe only if it is in air AND it can see matter: fourteen short rays,
// and at least three of them have to hit something within six metres. That is what puts probes in
// rooms and above floors and keeps them out of the open sky above a building, where a probe would
// hold nothing the shader cannot work out for itself — and where its only effect would be to pull
// the blend towards a sky the analytic term already draws better.
//
// The spacing is a budget and not a number: it starts at two metres and doubles until the atlas
// fits `kProbeByteBudget`. A clip with one room gets probes two metres apart; the whole facility
// gets them four or eight apart and leans on the parallax correction, which is what the correction
// is for.
//
// # Pre-filtering, and the one thing a 32x32 probe cannot do
//
// Level 0 is the raw cast: as sharp as the map's own resolution, which is about three degrees a
// texel at 32x32. Levels 1..4 are integrated from it with a lobe whose width is the LARGER of what
// the roughness asks for and what one texel of that level subtends, so a level is never sharper
// than the texels it is stored in and the chain is monotone. Roughness maps to a level by
// `pow(roughness, 1/1.5) * (levels - 1)`, which spreads the clips' own materials sensibly: the
// three golds (40, 48, 64) land at 1.17, 1.31 and 1.59, and `mirror` (6) lands at 0.33.
//
// Said plainly: a 32x32 probe is not a mirror. Three degrees a texel is a soft reflection however
// low the roughness goes. That is the deliberate division of labour with the screen-space pass —
// SSR is sharp and sees only what is on screen; a probe is soft and sees everything, including what
// is behind the camera, which is most of what a mirror facing you shows.
//
// # It reuses the light grid's machinery
//
// The rays are cast against the same two things the light grid is cast against: the conservative
// occupancy grid, walked as a two-level DDA so an empty region is skipped a coarse cell at a time,
// and the full-resolution voxels inside a cell that is occupied. What a hit is SHADED with is the
// light grid itself — the sun and sky visibility already baked at that point — so a probe agrees
// with the surface shading of the wall it is looking at rather than being a second opinion about it.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/jobs.hpp"
#include "core/time.hpp"
#include "core/types.hpp"
#include "game/clip.hpp"
#include "world/voxel_type.hpp"

namespace ws::web {

// --------------------------------------------------------------------------------------
// The numbers, all in one place
// --------------------------------------------------------------------------------------

// How many bytes of probe the atlas may take. A megabyte is three times a small clip's whole file
// and a twentieth of the facility's, and it is the number the spacing is chosen against.
constexpr usize kProbeByteBudget = 1024u * 1024u;
// A probe map is 32x32 unless the clip is small enough that 64x64 also fits the budget, which is
// worth four times the texels only where there are few enough probes for it to be free.
constexpr i32 kProbeBaseSize = 32;
constexpr i32 kProbeLargeSize = 64;
constexpr i32 kProbeLevels = 5;
constexpr i32 kProbeBorder = 1;
// Radiance is stored as sqrt(value / range) in 8 bits, so the darks get the precision and the sun
// still fits. Decoded in the shader as `c * c * range`.
constexpr f64 kProbeRange = 8.0;
// How far a probe ray looks before it calls the answer sky.
constexpr f64 kProbeReach = 64.0;
// The placement test: fourteen rays this far, and at least this many have to find matter.
constexpr f64 kProbeNearReach = 6.0;
constexpr i32 kProbeNearHits = 3;
// roughness = (level / (levels - 1)) ^ curve, inverted at run time. See the header note.
constexpr f64 kProbeRoughnessCurve = 1.5;
// WebGL2 guarantees 2048, and nothing here needs more.
constexpr i32 kProbeAtlasMax = 2048;

// The sky the viewer draws, so that what a probe stores and what stands behind the geometry are the
// same sky. These MUST match web/js/gl.js: `Renderer.sunColour`, `.skyUp`, `.skyDown` and the
// background's own `SKY_FRAGMENT`, which is the one a reflection actually sees.
constexpr f64 kProbeSunColour[3] = {3.30, 3.10, 2.78};
constexpr f64 kProbeSkyUp[3] = {0.30, 0.47, 0.92};
constexpr f64 kProbeSkyDown[3] = {0.62, 0.67, 0.74};

// --------------------------------------------------------------------------------------
// What the baker hands over
// --------------------------------------------------------------------------------------

// The conservative occupancy grid — a cell is set if ANY voxel in it is matter. The same grid the
// walker collides with, and it is conservative for the same reason: a ray must not pass through a
// wall one voxel thick, and a mirror in these clips is exactly that.
struct ProbeOccupancy {
    const u8* bits = nullptr;
    i32 dims[3]{0, 0, 0};
    i32 voxels_per_metre = 1;
};

// The light grid, already cast. Two bytes a point: sun visibility, sky visibility.
struct ProbeLightGrid {
    const u8* texels = nullptr;
    i32 dims[3]{0, 0, 0};
    f64 cell = 0.4;
};

struct ProbeInput {
    const ws::Clip* clip = nullptr;
    const ws::VoxelTypeTable* types = nullptr;
    ProbeOccupancy occupancy;
    ProbeLightGrid light;
    f64 origin[3]{0, 0, 0};        // metres at voxel (0,0,0), and the light grid's origin too
    f64 size_metres[3]{0, 0, 0};   // the sampled box
    i32 metre = 32;                // voxels per metre of `clip`
    f64 sun[3]{0, 1, 0};           // normalised
};

struct ProbeSet {
    u32 count = 0;
    i32 base = kProbeBaseSize;
    i32 levels = kProbeLevels;
    i32 border = kProbeBorder;
    f64 spacing = 0.0;
    i32 dims[3]{0, 0, 0};
    f64 grid_origin[3]{0, 0, 0};   // world position of index cell (0,0,0)
    i32 atlas_w = 0;
    i32 atlas_h = 0;
    i32 tile_w = 0;
    i32 tile_h = 0;
    i32 per_row = 0;
    u64 rays = 0;
    f64 seconds = 0.0;
    std::vector<u8> index;   // dims product * 2, little-endian probe id, 0xFFFF = no probe
    std::vector<u8> atlas;   // atlas_w * atlas_h * 4, RGBA8

    usize bytes() const { return index.size() + atlas.size(); }
};

// --------------------------------------------------------------------------------------
// The octahedral mapping, written once
//
// Edge-aligned: texel 0 sits at uv = -1 and texel size-1 at uv = +1, so the square's boundary lands
// exactly on texel centres and the fold across it is an exact integer reflection rather than a
// half-texel guess. `web/js/features/probes.js` maps a direction back with the same convention, and
// the two are the only places it is written down.
// --------------------------------------------------------------------------------------

inline void probe_oct_decode(i32 i, i32 j, i32 size, f64 out[3]) {
    const f64 span = static_cast<f64>(std::max(1, size - 1));
    const f64 u = 2.0 * static_cast<f64>(i) / span - 1.0;
    const f64 v = 2.0 * static_cast<f64>(j) / span - 1.0;
    const f64 y = 1.0 - std::fabs(u) - std::fabs(v);
    f64 x = u;
    f64 z = v;
    if (y < 0.0) {
        x = (1.0 - std::fabs(v)) * (u >= 0.0 ? 1.0 : -1.0);
        z = (1.0 - std::fabs(u)) * (v >= 0.0 ? 1.0 : -1.0);
    }
    const f64 length = std::sqrt(x * x + y * y + z * z);
    out[0] = x / length;
    out[1] = y / length;
    out[2] = z / length;
}

// How much of the sphere one texel of the map covers, up to a constant. The octahedral map is not
// equal-area — the corners carry about three times the solid angle of the centre — so an average
// taken over its texels without this leans towards whatever is straight up.
inline f64 probe_oct_solid_angle(i32 i, i32 j, i32 size) {
    const f64 span = static_cast<f64>(std::max(1, size - 1));
    const f64 u = 2.0 * static_cast<f64>(i) / span - 1.0;
    const f64 v = 2.0 * static_cast<f64>(j) / span - 1.0;
    const f64 y = 1.0 - std::fabs(u) - std::fabs(v);
    f64 x = u;
    f64 z = v;
    if (y < 0.0) {
        x = (1.0 - std::fabs(v)) * (u >= 0.0 ? 1.0 : -1.0);
        z = (1.0 - std::fabs(u)) * (v >= 0.0 ? 1.0 : -1.0);
    }
    const f64 length = std::sqrt(x * x + y * y + z * z);
    return 1.0 / (length * length * length);
}

// A texel outside the interior is a texel on the other side of the fold. Reflecting the index is
// exact because the mapping is edge-aligned; the corners fold twice and land on the -Y pole, which
// is what they are.
inline void probe_oct_fold(i32 size, i32& i, i32& j) {
    if (i < 0) {
        i = 1;
        j = size - 1 - j;
    } else if (i >= size) {
        i = size - 2;
        j = size - 1 - j;
    }
    if (j < 0) {
        j = 1;
        i = size - 1 - i;
    } else if (j >= size) {
        j = size - 2;
        i = size - 1 - i;
    }
    i = std::max(0, std::min(size - 1, i));
    j = std::max(0, std::min(size - 1, j));
}

inline i32 probe_level_size(i32 base, i32 level) {
    i32 size = base >> level;
    return std::max(2, size);
}

inline i32 probe_tile_width(i32 base, i32 levels, i32 border) {
    i32 width = 0;
    for (i32 level = 0; level < levels; ++level) {
        width += probe_level_size(base, level) + border * 2;
    }
    return width;
}

// --------------------------------------------------------------------------------------
// Casting one ray
// --------------------------------------------------------------------------------------

namespace detail {

inline bool occupancy_at(const ProbeOccupancy& grid, i32 x, i32 y, i32 z) {
    if (x < 0 || y < 0 || z < 0 || x >= grid.dims[0] || y >= grid.dims[1] || z >= grid.dims[2]) {
        return false;
    }
    const usize at = static_cast<usize>(x) + static_cast<usize>(y) * static_cast<usize>(grid.dims[0]) +
                     static_cast<usize>(z) * static_cast<usize>(grid.dims[0]) *
                         static_cast<usize>(grid.dims[1]);
    return (grid.bits[at >> 3] & static_cast<u8>(1u << (at & 7u))) != 0;
}

inline bool voxel_solid(const ws::Clip& clip, i32 x, i32 y, i32 z) {
    if (x < 0 || y < 0 || z < 0 || x >= clip.size[0] || y >= clip.size[1] || z >= clip.size[2]) {
        return false;
    }
    const usize at = static_cast<usize>(x) + static_cast<usize>(y) * static_cast<usize>(clip.size[0]) +
                     static_cast<usize>(z) * static_cast<usize>(clip.size[0]) *
                         static_cast<usize>(clip.size[1]);
    return clip.inside[at] != 0 && clip.voxels[at] != ws::kAir;
}

// How far along the ray until it leaves the cell of size `cell` it is standing in, and across which
// axis. `local` is the point relative to the clip's own origin, so a cell boundary is a multiple of
// `cell`.
inline f64 exit_of_cell(const f64 local[3], const f64 dir[3], f64 cell, i32& axis) {
    f64 best = 1e30;
    axis = 0;
    for (i32 a = 0; a < 3; ++a) {
        if (std::fabs(dir[a]) < 1e-9) continue;
        const f64 index = std::floor(local[a] / cell);
        const f64 boundary = (dir[a] > 0.0) ? (index + 1.0) * cell : index * cell;
        f64 t = (boundary - local[a]) / dir[a];
        if (t <= 1e-9) t = cell / std::fabs(dir[a]);
        if (t < best) {
            best = t;
            axis = a;
        }
    }
    return best;
}

struct ProbeRayHit {
    bool hit = false;
    i32 voxel[3]{0, 0, 0};
    i32 axis = 1;
    f64 normal = 1.0;   // +1 or -1 along `axis`
    f64 distance = 0.0;
};

// Two levels: skip a coarse cell at a time while the conservative grid says there is nothing in it,
// and step voxel by voxel once it says there might be. Nothing thinner than a voxel is missed,
// which matters here because a `mirror` wall in these clips is one voxel of paint on stone.
inline ProbeRayHit probe_cast(const ProbeInput& in, const f64 from[3], const f64 dir[3], f64 reach) {
    ProbeRayHit out;
    const f64 coarse = 1.0 / static_cast<f64>(in.occupancy.voxels_per_metre);
    const f64 fine = 1.0 / static_cast<f64>(in.metre);
    f64 t = 0.0;
    i32 axis = 1;
    for (i32 step = 0; step < 8192 && t < reach; ++step) {
        f64 local[3];
        bool outside = false;
        for (i32 a = 0; a < 3; ++a) {
            local[a] = from[a] - in.origin[a] + dir[a] * t;
            if (local[a] < -1e-6 || local[a] > in.size_metres[a] + 1e-6) outside = true;
        }
        // Everything past the sampled box is open air, exactly as the light grid treats it.
        if (outside && t > 0.0) return out;

        const i32 cx = static_cast<i32>(std::floor(local[0] / coarse));
        const i32 cy = static_cast<i32>(std::floor(local[1] / coarse));
        const i32 cz = static_cast<i32>(std::floor(local[2] / coarse));
        if (!occupancy_at(in.occupancy, cx, cy, cz)) {
            i32 crossed = 0;
            t += exit_of_cell(local, dir, coarse, crossed) + 1e-6;
            axis = crossed;
            continue;
        }

        const i32 vx = static_cast<i32>(std::floor(local[0] / fine));
        const i32 vy = static_cast<i32>(std::floor(local[1] / fine));
        const i32 vz = static_cast<i32>(std::floor(local[2] / fine));
        if (voxel_solid(*in.clip, vx, vy, vz)) {
            out.hit = true;
            out.voxel[0] = vx;
            out.voxel[1] = vy;
            out.voxel[2] = vz;
            out.axis = axis;
            out.normal = (dir[axis] > 0.0) ? -1.0 : 1.0;
            out.distance = t;
            return out;
        }
        i32 crossed = 0;
        t += exit_of_cell(local, dir, fine, crossed) + 1e-6;
        axis = crossed;
    }
    return out;
}

// The light grid, trilinear, exactly where the shader reads it: point index (x,y,z) is at
// `origin + (x,y,z) * cell`.
inline void sample_light(const ProbeLightGrid& light, const f64 origin[3], const f64 p[3],
                         f64& sun, f64& sky) {
    sun = 0.0;
    sky = 0.0;
    f64 g[3];
    for (i32 a = 0; a < 3; ++a) {
        g[a] = (p[a] - origin[a]) / light.cell;
        g[a] = std::max(0.0, std::min(static_cast<f64>(light.dims[a] - 1), g[a]));
    }
    const i32 x0 = static_cast<i32>(g[0]);
    const i32 y0 = static_cast<i32>(g[1]);
    const i32 z0 = static_cast<i32>(g[2]);
    const f64 fx = g[0] - static_cast<f64>(x0);
    const f64 fy = g[1] - static_cast<f64>(y0);
    const f64 fz = g[2] - static_cast<f64>(z0);
    for (i32 corner = 0; corner < 8; ++corner) {
        const i32 dx = corner & 1;
        const i32 dy = (corner >> 1) & 1;
        const i32 dz = (corner >> 2) & 1;
        const i32 x = std::min(light.dims[0] - 1, x0 + dx);
        const i32 y = std::min(light.dims[1] - 1, y0 + dy);
        const i32 z = std::min(light.dims[2] - 1, z0 + dz);
        const f64 w = (dx ? fx : 1.0 - fx) * (dy ? fy : 1.0 - fy) * (dz ? fz : 1.0 - fz);
        if (w <= 0.0) continue;
        const usize at = static_cast<usize>(x) + static_cast<usize>(y) * static_cast<usize>(light.dims[0]) +
                         static_cast<usize>(z) * static_cast<usize>(light.dims[0]) *
                             static_cast<usize>(light.dims[1]);
        sun += w * static_cast<f64>(light.texels[at * 2 + 0]) / 255.0;
        sky += w * static_cast<f64>(light.texels[at * 2 + 1]) / 255.0;
    }
}

inline void probe_sky(const f64 dir[3], const f64 sun[3], f64 out[3]) {
    const f64 up = std::max(0.0, std::min(1.0, dir[1] * 0.5 + 0.5));
    const f64 towards = std::max(0.0, dir[0] * sun[0] + dir[1] * sun[1] + dir[2] * sun[2]);
    // The background's own numbers, not the surface shader's slightly different pair: what a
    // reflection shows has to be the sky that is actually standing behind the geometry.
    const f64 disc = std::pow(towards, 900.0) * 4.0;
    const f64 glow = std::pow(towards, 8.0) * 0.05;
    for (i32 c = 0; c < 3; ++c) {
        out[c] = kProbeSkyDown[c] + (kProbeSkyUp[c] - kProbeSkyDown[c]) * up +
                 kProbeSunColour[c] * (disc + glow);
    }
}

// What one ray comes back with, in linear radiance, pre-tonemap. The shading matches the surface
// shader term for term where it can: the same albedo curve, the same ambient, the same light grid,
// the same emission scale. What it leaves out is the corner occlusion (a probe has no quad to read
// it from), the sun's own specular highlight (three degrees a texel would alias it into a
// flickering dot) and translucency.
inline void probe_radiance(const ProbeInput& in, const f64 from[3], const f64 dir[3], f64 out[3],
                           f64& distance) {
    const ProbeRayHit hit = probe_cast(in, from, dir, kProbeReach);
    if (!hit.hit) {
        probe_sky(dir, in.sun, out);
        distance = kProbeReach;
        return;
    }
    distance = hit.distance;

    const f64 fine = 1.0 / static_cast<f64>(in.metre);
    f64 normal[3]{0.0, 0.0, 0.0};
    normal[hit.axis] = hit.normal;

    const usize at = static_cast<usize>(hit.voxel[0]) +
                     static_cast<usize>(hit.voxel[1]) * static_cast<usize>(in.clip->size[0]) +
                     static_cast<usize>(hit.voxel[2]) * static_cast<usize>(in.clip->size[0]) *
                         static_cast<usize>(in.clip->size[1]);
    const ws::VisualRecord& record = in.types->visual_of(in.clip->voxels[at]);

    f64 surface[3];
    for (i32 a = 0; a < 3; ++a) {
        surface[a] = in.origin[a] + (static_cast<f64>(hit.voxel[a]) + 0.5) * fine +
                     normal[a] * (fine * 0.5 + in.light.cell);
    }
    f64 sun_visible = 0.0;
    f64 sky_visible = 0.0;
    sample_light(in.light, in.origin, surface, sun_visible, sky_visible);

    const f64 albedo[3] = {
        (static_cast<f64>(record.red) / 255.0) * (static_cast<f64>(record.red) / 255.0),
        (static_cast<f64>(record.green) / 255.0) * (static_cast<f64>(record.green) / 255.0),
        (static_cast<f64>(record.blue) / 255.0) * (static_cast<f64>(record.blue) / 255.0),
    };
    const f64 metal = static_cast<f64>(record.metallic) / 255.0;
    const f64 ndl = std::max(0.0, normal[0] * in.sun[0] + normal[1] * in.sun[1] + normal[2] * in.sun[2]);
    const f64 up = std::max(0.0, std::min(1.0, normal[1] * 0.5 + 0.5));
    const f64 occluded = 0.25 + 0.75 * sky_visible;

    f64 reflected[3];
    {
        const f64 dot_n = dir[0] * normal[0] + dir[1] * normal[1] + dir[2] * normal[2];
        const f64 bounce[3] = {dir[0] - 2.0 * dot_n * normal[0], dir[1] - 2.0 * dot_n * normal[1],
                               dir[2] - 2.0 * dot_n * normal[2]};
        probe_sky(bounce, in.sun, reflected);
    }

    for (i32 c = 0; c < 3; ++c) {
        const f64 ambient = (kProbeSkyDown[c] + (kProbeSkyUp[c] - kProbeSkyDown[c]) * up) * 0.5;
        const f64 direct = kProbeSunColour[c] * ndl * sun_visible;
        out[c] = albedo[c] * (1.0 - metal) * (direct + ambient * occluded);
        // One bounce of sky off a metal, and no further. A probe that recursed would be a path
        // tracer, which is the thing this exists instead of.
        out[c] += albedo[c] * metal * reflected[c] * (0.25 + 0.75 * sky_visible) * 0.6;
    }

    if (record.emissive > 0) {
        const u16 tint = record.emissive_tint;
        const f64 glow[3] = {static_cast<f64>((tint >> 11) & 31) / 31.0,
                             static_cast<f64>((tint >> 5) & 63) / 63.0,
                             static_cast<f64>(tint & 31) / 31.0};
        const f64 scale = static_cast<f64>(record.emissive) / 255.0 * 6.0;
        for (i32 c = 0; c < 3; ++c) out[c] += glow[c] * scale;
    }
}

}  // namespace detail

// --------------------------------------------------------------------------------------
// Placing them
// --------------------------------------------------------------------------------------

namespace detail {

// The fourteen directions the placement test uses: six axes and eight corners. Even coverage
// matters more than count — a probe over an open floor should find it with the downward five, and a
// probe in a room should find walls with the sideways six.
inline const f64 (&probe_probe_dirs())[14][3] {
    static const f64 dirs[14][3] = {
        {1, 0, 0},  {-1, 0, 0}, {0, 1, 0},  {0, -1, 0}, {0, 0, 1},  {0, 0, -1},
        {0.5774, 0.5774, 0.5774},    {-0.5774, 0.5774, 0.5774},
        {0.5774, -0.5774, 0.5774},   {-0.5774, -0.5774, 0.5774},
        {0.5774, 0.5774, -0.5774},   {-0.5774, 0.5774, -0.5774},
        {0.5774, -0.5774, -0.5774},  {-0.5774, -0.5774, -0.5774},
    };
    return dirs;
}

struct ProbeSite {
    i32 cell[3]{0, 0, 0};
    f64 position[3]{0, 0, 0};
};

inline std::vector<ProbeSite> place_probes(const ProbeInput& in, f64 spacing, i32 dims[3],
                                           f64 grid_origin[3]) {
    for (i32 a = 0; a < 3; ++a) {
        dims[a] = std::max(1, static_cast<i32>(std::floor(in.size_metres[a] / spacing)) + 1);
        grid_origin[a] = in.origin[a] + spacing * 0.5;
    }
    std::vector<ProbeSite> sites;
    const f64 coarse = 1.0 / static_cast<f64>(in.occupancy.voxels_per_metre);
    for (i32 z = 0; z < dims[2]; ++z) {
        for (i32 y = 0; y < dims[1]; ++y) {
            for (i32 x = 0; x < dims[0]; ++x) {
                ProbeSite site;
                site.cell[0] = x;
                site.cell[1] = y;
                site.cell[2] = z;
                const i32 cell[3] = {x, y, z};
                bool inside_box = true;
                for (i32 a = 0; a < 3; ++a) {
                    site.position[a] = grid_origin[a] + static_cast<f64>(cell[a]) * spacing;
                    const f64 local = site.position[a] - in.origin[a];
                    if (local < 0.0 || local > in.size_metres[a]) inside_box = false;
                }
                if (!inside_box) continue;

                // In matter is not a place to stand. The occupancy grid is conservative, so this
                // also keeps a probe out of the half-cell either side of a wall.
                const i32 ox = static_cast<i32>(std::floor((site.position[0] - in.origin[0]) / coarse));
                const i32 oy = static_cast<i32>(std::floor((site.position[1] - in.origin[1]) / coarse));
                const i32 oz = static_cast<i32>(std::floor((site.position[2] - in.origin[2]) / coarse));
                if (occupancy_at(in.occupancy, ox, oy, oz)) continue;

                i32 found = 0;
                for (const auto& dir : probe_probe_dirs()) {
                    if (probe_cast(in, site.position, dir, kProbeNearReach).hit) ++found;
                    if (found >= kProbeNearHits) break;
                }
                if (found < kProbeNearHits) continue;
                sites.push_back(site);
            }
        }
    }
    return sites;
}

}  // namespace detail

// --------------------------------------------------------------------------------------
// Baking the lot
// --------------------------------------------------------------------------------------

inline ProbeSet bake_probes(const ProbeInput& in, ws::JobSystem& jobs) {
    ProbeSet set;
    if (in.clip == nullptr || in.types == nullptr || in.occupancy.bits == nullptr ||
        in.light.texels == nullptr) {
        return set;
    }
    const u64 began = ws::now_ns();

    // The spacing is chosen against the byte budget, not written down: start at two metres and
    // double until the atlas fits. A one-room clip gets two, a building gets four or eight.
    f64 spacing = 2.0;
    std::vector<detail::ProbeSite> sites;
    i32 dims[3]{0, 0, 0};
    f64 grid_origin[3]{0, 0, 0};
    i32 base = kProbeBaseSize;
    for (i32 attempt = 0; attempt < 8; ++attempt) {
        sites = detail::place_probes(in, spacing, dims, grid_origin);
        if (sites.empty()) break;
        const i32 large_tile = probe_tile_width(kProbeLargeSize, kProbeLevels, kProbeBorder) *
                               (kProbeLargeSize + kProbeBorder * 2);
        const i32 small_tile = probe_tile_width(kProbeBaseSize, kProbeLevels, kProbeBorder) *
                               (kProbeBaseSize + kProbeBorder * 2);
        const usize large_bytes = sites.size() * static_cast<usize>(large_tile) * 4u;
        const usize small_bytes = sites.size() * static_cast<usize>(small_tile) * 4u;
        if (large_bytes <= kProbeByteBudget) {
            base = kProbeLargeSize;
            break;
        }
        if (small_bytes <= kProbeByteBudget) {
            base = kProbeBaseSize;
            break;
        }
        spacing *= 2.0;
    }
    if (sites.empty()) {
        set.seconds = static_cast<f64>(ws::now_ns() - began) / 1e9;
        return set;
    }

    set.count = static_cast<u32>(sites.size());
    set.base = base;
    set.levels = kProbeLevels;
    set.border = kProbeBorder;
    set.spacing = spacing;
    for (i32 a = 0; a < 3; ++a) {
        set.dims[a] = dims[a];
        set.grid_origin[a] = grid_origin[a];
    }
    set.tile_w = probe_tile_width(base, kProbeLevels, kProbeBorder);
    set.tile_h = base + kProbeBorder * 2;
    // Roughly square, so the last row wastes at most one row of tiles rather than most of one. An
    // atlas laid out as wide as it will go and two rows deep is half empty at twenty probes, and
    // that half is real bytes in the file.
    const i32 wide = std::max(1, kProbeAtlasMax / set.tile_w);
    set.per_row = std::max(1, std::min(wide, static_cast<i32>(std::ceil(std::sqrt(
                                                 static_cast<f64>(set.count))))));
    const i32 rows = (static_cast<i32>(set.count) + set.per_row - 1) / set.per_row;
    if (rows * set.tile_h > kProbeAtlasMax) {
        // More probes than one 2048-square atlas holds. The budget above makes this unreachable for
        // anything in this repository; if it ever is reached, the clip is better served by a
        // coarser spacing than by an atlas the card will refuse.
        set.count = static_cast<u32>(std::min<usize>(
            sites.size(), static_cast<usize>(set.per_row) *
                              static_cast<usize>(std::max(1, kProbeAtlasMax / set.tile_h))));
        sites.resize(set.count);
    }
    set.atlas_w = set.per_row * set.tile_w;
    set.atlas_h = ((static_cast<i32>(set.count) + set.per_row - 1) / set.per_row) * set.tile_h;
    set.atlas.assign(static_cast<usize>(set.atlas_w) * static_cast<usize>(set.atlas_h) * 4u, 0);

    // The index volume: which probe stands at each lattice point, or 0xFFFF for none. It is what
    // lets the shader find its eight neighbours with a texelFetch and no search, and it is why the
    // probe positions are not stored — a probe IS its lattice point, said once.
    const usize cells = static_cast<usize>(dims[0]) * static_cast<usize>(dims[1]) *
                        static_cast<usize>(dims[2]);
    set.index.assign(cells * 2, 0xFF);
    for (usize i = 0; i < sites.size(); ++i) {
        const detail::ProbeSite& site = sites[i];
        const usize at = static_cast<usize>(site.cell[0]) +
                         static_cast<usize>(site.cell[1]) * static_cast<usize>(dims[0]) +
                         static_cast<usize>(site.cell[2]) * static_cast<usize>(dims[0]) *
                             static_cast<usize>(dims[1]);
        set.index[at * 2 + 0] = static_cast<u8>(i & 0xFFu);
        set.index[at * 2 + 1] = static_cast<u8>((i >> 8) & 0xFFu);
    }

    // Level offsets within a tile, once.
    std::vector<i32> level_x(static_cast<usize>(kProbeLevels), 0);
    std::vector<i32> level_size(static_cast<usize>(kProbeLevels), 0);
    {
        i32 running = 0;
        for (i32 level = 0; level < kProbeLevels; ++level) {
            level_size[static_cast<usize>(level)] = probe_level_size(base, level);
            level_x[static_cast<usize>(level)] = running;
            running += level_size[static_cast<usize>(level)] + kProbeBorder * 2;
        }
    }

    // The source set the blurry levels are integrated from: level 0, subsampled to at most 32x32 so
    // that a 64x64 probe costs the same to filter as a 32x32 one. Bounding it here is what keeps
    // the prefilter cheap next to the ray casting rather than dominating it.
    const i32 source_size = std::min(base, kProbeBaseSize);
    const i32 source_step = base / source_size;

    const usize rays_per_probe = static_cast<usize>(base) * static_cast<usize>(base);
    set.rays = static_cast<u64>(rays_per_probe) * static_cast<u64>(set.count);

    jobs.parallel_for(sites.size(), 1, [&](usize begin, usize end) {
        std::vector<f64> level0(rays_per_probe * 3, 0.0);
        std::vector<f64> source(static_cast<usize>(source_size) * static_cast<usize>(source_size) * 3,
                                0.0);
        std::vector<f64> source_dir(
            static_cast<usize>(source_size) * static_cast<usize>(source_size) * 3, 0.0);
        std::vector<f64> source_weight(
            static_cast<usize>(source_size) * static_cast<usize>(source_size), 0.0);

        for (usize which = begin; which < end; ++which) {
            const detail::ProbeSite& site = sites[which];
            const i32 tile_x = (static_cast<i32>(which) % set.per_row) * set.tile_w;
            const i32 tile_y = (static_cast<i32>(which) / set.per_row) * set.tile_h;

            // --- level 0: one ray a texel -----------------------------------------------------
            for (i32 j = 0; j < base; ++j) {
                for (i32 i = 0; i < base; ++i) {
                    f64 dir[3];
                    probe_oct_decode(i, j, base, dir);
                    f64 radiance[3];
                    f64 distance = 0.0;
                    detail::probe_radiance(in, site.position, dir, radiance, distance);
                    const usize at = (static_cast<usize>(j) * static_cast<usize>(base) +
                                      static_cast<usize>(i)) * 3u;
                    level0[at + 0] = radiance[0];
                    level0[at + 1] = radiance[1];
                    level0[at + 2] = radiance[2];
                }
            }

            // --- the source set the blurry levels read ----------------------------------------
            for (i32 j = 0; j < source_size; ++j) {
                for (i32 i = 0; i < source_size; ++i) {
                    const usize at = static_cast<usize>(j) * static_cast<usize>(source_size) +
                                     static_cast<usize>(i);
                    f64 sum[3]{0.0, 0.0, 0.0};
                    i32 taken = 0;
                    for (i32 dj = 0; dj < source_step; ++dj) {
                        for (i32 di = 0; di < source_step; ++di) {
                            const i32 sx = std::min(base - 1, i * source_step + di);
                            const i32 sy = std::min(base - 1, j * source_step + dj);
                            const usize from = (static_cast<usize>(sy) * static_cast<usize>(base) +
                                                static_cast<usize>(sx)) * 3u;
                            sum[0] += level0[from + 0];
                            sum[1] += level0[from + 1];
                            sum[2] += level0[from + 2];
                            ++taken;
                        }
                    }
                    for (i32 c = 0; c < 3; ++c) source[at * 3 + static_cast<usize>(c)] = sum[c] / taken;
                    f64 dir[3];
                    probe_oct_decode(i, j, source_size, dir);
                    source_dir[at * 3 + 0] = dir[0];
                    source_dir[at * 3 + 1] = dir[1];
                    source_dir[at * 3 + 2] = dir[2];
                    source_weight[at] = probe_oct_solid_angle(i, j, source_size);
                }
            }

            // --- every level into the atlas ---------------------------------------------------
            for (i32 level = 0; level < kProbeLevels; ++level) {
                const i32 size = level_size[static_cast<usize>(level)];
                std::vector<f64> plane(static_cast<usize>(size) * static_cast<usize>(size) * 3, 0.0);

                if (level == 0 && size == base) {
                    plane = level0;
                } else {
                    const f64 roughness =
                        std::pow(static_cast<f64>(level) / static_cast<f64>(kProbeLevels - 1),
                                 kProbeRoughnessCurve);
                    const f64 alpha = roughness * roughness;
                    // The lobe is never narrower than one texel of the level it is stored in: a
                    // level sharper than its own resolution is aliasing with a physical excuse.
                    const f64 texel_angle = 1.5707963267948966 /
                                            static_cast<f64>(std::max(1, size - 1));
                    const f64 angle = std::max(std::atan(alpha), texel_angle);
                    const f64 cosine = std::max(1e-4, std::cos(std::min(angle, 1.5533)));
                    const f64 power = std::max(0.5, std::min(4096.0, std::log(0.5) / std::log(cosine)));

                    for (i32 j = 0; j < size; ++j) {
                        for (i32 i = 0; i < size; ++i) {
                            f64 dir[3];
                            probe_oct_decode(i, j, size, dir);
                            f64 sum[3]{0.0, 0.0, 0.0};
                            f64 total = 0.0;
                            const usize samples = static_cast<usize>(source_size) *
                                                  static_cast<usize>(source_size);
                            for (usize s = 0; s < samples; ++s) {
                                const f64 d = dir[0] * source_dir[s * 3 + 0] +
                                              dir[1] * source_dir[s * 3 + 1] +
                                              dir[2] * source_dir[s * 3 + 2];
                                if (d <= 0.0) continue;
                                const f64 w = std::pow(d, power) * source_weight[s];
                                if (w <= 1e-9) continue;
                                sum[0] += source[s * 3 + 0] * w;
                                sum[1] += source[s * 3 + 1] * w;
                                sum[2] += source[s * 3 + 2] * w;
                                total += w;
                            }
                            const usize at = (static_cast<usize>(j) * static_cast<usize>(size) +
                                              static_cast<usize>(i)) * 3u;
                            if (total > 0.0) {
                                for (i32 c = 0; c < 3; ++c) {
                                    plane[at + static_cast<usize>(c)] = sum[c] / total;
                                }
                            }
                        }
                    }
                }

                // Into the atlas, sqrt-encoded, with the border filled by the fold so that a
                // bilinear fetch at the edge of the map reads the direction that is actually next
                // to it rather than the one the square happens to store beside it.
                const i32 ox = tile_x + level_x[static_cast<usize>(level)];
                for (i32 j = -kProbeBorder; j < size + kProbeBorder; ++j) {
                    for (i32 i = -kProbeBorder; i < size + kProbeBorder; ++i) {
                        i32 si = i;
                        i32 sj = j;
                        probe_oct_fold(size, si, sj);
                        const usize from = (static_cast<usize>(sj) * static_cast<usize>(size) +
                                            static_cast<usize>(si)) * 3u;
                        const i32 ax = ox + kProbeBorder + i;
                        const i32 ay = tile_y + kProbeBorder + j;
                        const usize to = (static_cast<usize>(ay) * static_cast<usize>(set.atlas_w) +
                                          static_cast<usize>(ax)) * 4u;
                        for (i32 c = 0; c < 3; ++c) {
                            const f64 value = std::max(0.0, plane[from + static_cast<usize>(c)]);
                            const f64 encoded = std::sqrt(std::min(1.0, value / kProbeRange));
                            set.atlas[to + static_cast<usize>(c)] =
                                static_cast<u8>(std::lround(encoded * 255.0));
                        }
                        set.atlas[to + 3] = 255;
                    }
                }
            }
        }
    });

    set.seconds = static_cast<f64>(ws::now_ns() - began) / 1e9;
    return set;
}

// --------------------------------------------------------------------------------------
// The `RPRB` chunk
//
// 80 bytes of header, then the index volume, then the atlas. Every offset is relative to the start
// of the chunk, so the chunk directory can put it anywhere in the file. Matched, field for field,
// by `readProbes` in web/js/features/probes.js.
//
//    0  u32 probeCount        16  u32 tileHeight        32  u32 border      64  f32 range
//    4  u32 atlasWidth        20  u32 probesPerRow      36  f32 spacing     68  f32 roughnessCurve
//    8  u32 atlasHeight       24  u32 baseSize          40  i32 dims[3]     72  u32 indexOffset
//   12  u32 tileWidth         28  u32 levels            52  f32 gridOrigin  76  u32 atlasOffset
// --------------------------------------------------------------------------------------

constexpr usize kProbeChunkHeader = 80;

inline std::vector<u8> probe_chunk(const ProbeSet& set) {
    std::vector<u8> out;
    if (set.count == 0) return out;
    out.assign(kProbeChunkHeader, 0);

    const auto put_u32 = [&out](usize at, u32 value) {
        out[at + 0] = static_cast<u8>(value & 0xFFu);
        out[at + 1] = static_cast<u8>((value >> 8) & 0xFFu);
        out[at + 2] = static_cast<u8>((value >> 16) & 0xFFu);
        out[at + 3] = static_cast<u8>((value >> 24) & 0xFFu);
    };
    const auto put_f32 = [&put_u32](usize at, f64 value) {
        const f32 narrowed = static_cast<f32>(value);
        u32 bits = 0;
        std::memcpy(&bits, &narrowed, sizeof(bits));
        put_u32(at, bits);
    };

    put_u32(0, set.count);
    put_u32(4, static_cast<u32>(set.atlas_w));
    put_u32(8, static_cast<u32>(set.atlas_h));
    put_u32(12, static_cast<u32>(set.tile_w));
    put_u32(16, static_cast<u32>(set.tile_h));
    put_u32(20, static_cast<u32>(set.per_row));
    put_u32(24, static_cast<u32>(set.base));
    put_u32(28, static_cast<u32>(set.levels));
    put_u32(32, static_cast<u32>(set.border));
    put_f32(36, set.spacing);
    for (i32 a = 0; a < 3; ++a) {
        put_u32(40 + static_cast<usize>(a) * 4, static_cast<u32>(set.dims[a]));
        put_f32(52 + static_cast<usize>(a) * 4, set.grid_origin[a]);
    }
    put_f32(64, kProbeRange);
    put_f32(68, kProbeRoughnessCurve);
    put_u32(72, static_cast<u32>(kProbeChunkHeader));
    put_u32(76, static_cast<u32>(kProbeChunkHeader + set.index.size()));

    out.insert(out.end(), set.index.begin(), set.index.end());
    out.insert(out.end(), set.atlas.begin(), set.atlas.end());
    return out;
}

}  // namespace ws::web
