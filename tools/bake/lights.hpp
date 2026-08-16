#pragma once
// Emissive geometry, turned into a list of LIGHTS the viewer can shade with.
//
// documentation/24-clip-viewer.md §6 is what this is for. In one paragraph: the viewer treats an
// emitter as bright paint and nothing else, so `clips/many_lamps.clip` — a sealed hall with no sky
// and no sun in it, lit by thirty-seven fittings — draws thirty-seven white rectangles on a wall
// that is otherwise the colour of the ambient constant. Everything the clip is FOR is invisible.
// `clips/facility/fittings.clip` says the same thing in its own header: the two halls have no
// window at all and everything past the first bay arrives from a sconce or from nothing.
//
// So the emissive voxels are gathered here, once, into a list of real lights: where each one is,
// how big it is, what colour, how strong, and — per light — a cheap baked visibility so that a
// sconce in one hall does not light the hall next door.
//
// # What a light is derived from, and why the SURFACE AREA is in it
//
// `fittings.clip` is explicit that a sconce bowl is a hemisphere of radius 0.1125 — about
// 0.08 m2 — in a hall whose surface is 143 m2, and that the bowls are deliberately small: "a big
// soft area light makes a room easy and tests nothing". A light list that reduced every fitting to
// a point with one brightness number would throw that away and make the small bright bowl and a
// large dim panel the same object. So a cluster carries the area it actually emits from, and the
// intensity is derived from it:
//
//   L (radiance)   = tint * (emissive/255) * kEmissive  -- see below: the VIEWER's own conversion
//   A (area)       = the emissive voxel faces that touch air, times the voxel's own area
//   I (intensity)  = L * A / 4                          -- an isotropic sphere of total power
//                                                          pi*A*L has intensity phi/(4 pi) = L*A/4
//   E (irradiance) = I * cos(theta) / d^2               -- what the shader does with it
//
// # Which emission curve, and it is NOT the game's
//
// The game's path tracer reads a `VisualRecord` as `tint * (emissive/255)^2 * 64` —
// `material_from_record` in shaders/pt_material.glsl, and `emitted_radiance` in
// src/world/light_list.cpp agrees with it. The obvious thing is to be a third reader of the same
// bytes in the same way, and it was written that way first and it is WRONG HERE.
//
// The viewer paints an emissive surface with `glow * emissive * 6.0` — linear, and 6.0 rather than
// 64 — which for a lamp at emit=200 is a radiance of 4.7 against the game's 39.4. Deriving the
// LIGHT from the game's curve and leaving the PAINT on the viewer's makes a lamp eight times
// brighter than the thing it is coming out of: many_lamps came out with every wall clipped to white
// and the sconces on it visibly DARKER than the pools they cast. A source dimmer than what it
// lights is the one lighting error nobody has to be told to see.
//
// So the light list uses the viewer's own constant, and the two are one number rather than two.
// Whoever changes `colour += glow * emissive * 6.0` in web/js/gl.js has to change this with it, and
// reconciling the viewer with the game's 64 is that person's change and not this one's.
//
// A cluster also carries a RADIUS and a HALF-EXTENT rather than only a position, for the reason the
// task names: a sconce bowl is a hemisphere and a chandelier hoop is a ring, and a point is not
// always honest. The radius is what the shader clamps d^2 against — so standing with your nose in a
// lamp does not divide by nothing — and what widens the specular lobe, which is the whole visible
// difference between a point light and a small area one.
//
// # Clusters, and why a big one is cut up
//
// Emissive voxels are flood filled, six-connected, so a fitting made of two hundred voxels is one
// light rather than two hundred. Then anything whose box is longer than `kSplitMetres` on any axis
// is cut into a lattice of pieces at that pitch. That is not thrift, it is honesty: the corona lucis
// in `fittings.clip` is a hoop 3.60 m across carrying sixteen lamps, and one sphere at the centre of
// a hoop is a light in the one place the fitting has no matter at all. Cut up, it is a ring of
// lights, which is what it is. The same rule keeps a glowing strip a strip.
//
// # Shadowing: a cube of baked distances per light, in one atlas
//
// No rays at run time, and an unshadowed point light leaks through walls so obviously that it is
// worse than no light at all — many_lamps is built out of exactly that case, four quarters walled
// off from each other with lamps bolted to both sides of every partition.
//
// The alternative considered first was a per-lattice-point visibility mask on the light grid the
// baker already casts. It was rejected on size: the mask costs one bit per light per lattice point,
// so it grows with the VOLUME of the clip, and the facility's lattice is half a million points —
// eight shadowed lights would be half a megabyte before a single wall had been looked at, and
// thirty-two would be two.
//
// A shadow cube costs the same per light whatever size the building is. Six faces of
// `kShadowTile` texels, one byte a texel holding the distance to the first blocker along that
// direction as a fraction of `range`. All of them go in one 2D atlas, `kTilesPerRow` tiles across,
// which the viewer uploads as a single R8 texture and reads with one bilinear fetch per light per
// pixel. At 48 texels a face that is 1.9 degrees a texel — about 13 cm at four metres, which is
// about the clip's own voxel, so the shadow is as sharp as the matter that cast it and no sharper.
//
// Four things about that ray cast are not obvious:
//
//   THE LAMP IS INSIDE ITS OWN MATTER. A ray from the centre of a fitting starts in solid stone,
//   and every one of them comes back blocked at the first step: the whole atlas is black and every
//   light goes out. So a ray ignores blockers until it has left the fitting's own box, grown by a
//   voxel.
//   A CLIP IS NOT A CLOSED ROOM. A ray that walks out of the sampled box has hit nothing, and
//   saying "blocked" there would put a black shadow on the far side of every exterior lamp.
//   THE RAYS WALK THE CLIP'S OWN VOXELS. A coarse occupancy grid is cheaper and it is the wrong
//   tool: at half a metre a cell a whole sconce fits inside one cell, and the distance such a grid
//   reports is a cell out — which forces a shadow bias bigger than the walls this exists to stop
//   light passing through.
//   THE BYTE IS ROUNDED UP. Short of the real blocker puts the surface AT the blocker inside its own
//   shadow, which is acne on every wall a lamp is bolted to. Long by up to one quantum leaks light
//   that far behind an occluder instead, and one quantum is 12 cm on the largest clip here.
//
// # What this file does NOT do
//
// The DIFFUSE INDIRECT contribution of these emitters — a lamp filling a room by bounce — belongs
// to the irradiance volume and is somebody else's. This is the direct term and the highlight, and
// the highlight is the half of it that no baked irradiance volume can ever provide: a volume knows
// how much light arrives at a point and not from where, so it cannot put a bright reflection of a
// sconce on a bronze arm. That reflection is why the light list exists.

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

using ws::f32;
using ws::f64;
using ws::i32;
using ws::i64;
using ws::u32;
using ws::u8;
using ws::usize;

// ---------------------------------------------------------------------------------------------
// The numbers, all in one place, because every one of them is a cost somebody has to be able to
// look up.
// ---------------------------------------------------------------------------------------------

// A fitting longer than this on any axis is cut into pieces at this pitch. A metre is a chandelier
// — the same figure `src/world/light_list.hpp` settled on for the game's own list, and for the same
// reason: past it the sphere standing for the fitting starts to contain the surfaces it is lighting.
inline constexpr f64 kSplitMetres = 1.0;

// How many lights one clip may carry. Past it the weakest are dropped and the baker says so.
inline constexpr usize kMaxLights = 256;

// How many of them get a shadow cube. The rest are lit unshadowed, and the baker prints which.
// Thirty-two cubes at 48 texels is 331 kB, which is the number this is really trading.
inline constexpr usize kMaxShadowed = 32;

// One face of a shadow cube, in texels. 48 is 1.9 degrees a texel, which at four metres is 13 cm —
// the pitch of the occupancy grid the rays are cast against. Finer would be sharper than the
// evidence.
inline constexpr i32 kShadowTile = 48;

// Tiles across the atlas. Six faces a light, so a row holds two and two thirds lights; the number
// that matters is that 16 * 48 = 768 is a width every phone will allocate.
inline constexpr i32 kTilesPerRow = 16;

// The record the file carries, and the viewer's `web/js/features/lights.js` reads the same 48
// bytes in the same order.
inline constexpr usize kLightBytes = 48;

// ---------------------------------------------------------------------------------------------
// One light
// ---------------------------------------------------------------------------------------------

struct Light {
    f32 position[3]{0, 0, 0};   // the cluster's centroid, in metres
    f32 radius = 0.0f;          // the sphere that stands for it: clamps 1/d^2 and widens the lobe
    f32 rgb[3]{0, 0, 0};        // intensity, per channel: E = rgb * ndl / max(d^2, radius^2)
    f32 area = 0.0f;            // the emitting surface, m2 — reported, and it is where rgb came from
    f32 half[3]{0, 0, 0};       // the box half-extent, so a long fitting is not pretending to be round
    i32 shadow = -1;            // which cube in the atlas, or -1 for "this one is not shadowed"

    f64 power() const {         // what it delivers, for ranking. Luminance, not the sum.
        return 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
    }
};

struct LightBake {
    std::vector<Light> lights;
    std::vector<u8> atlas;      // atlas_w * atlas_h bytes, R8
    i32 atlas_w = 0;
    i32 atlas_h = 0;
    f32 range = 0.0f;           // metres the byte 0..255 spans
    usize clusters = 0;         // before the cap
    usize shadowed = 0;
    usize dropped = 0;          // clusters past kMaxLights
    usize dark = 0;             // clusters with no face touching air: they emit nowhere
    f64 seconds = 0.0;
};

namespace detail {

// What the VIEWER paints an emissive surface with: `colour += glow * emissive * 6.0` in the surface
// shader of web/js/gl.js. It is not the game's curve — see the header for why the two must be one
// number here even though the game's is squared and sixty-four times larger.
inline constexpr f64 kEmissive = 6.0;

inline void radiance_of(const ws::VisualRecord& visual, f64 out[3]) {
    const f64 scale = static_cast<f64>(visual.emissive) / 255.0;
    const f64 strength = scale * kEmissive;
    const u32 tint = visual.emissive_tint;
    out[0] = static_cast<f64>((tint >> 11) & 0x1F) / 31.0 * strength;
    out[1] = static_cast<f64>((tint >> 5) & 0x3F) / 63.0 * strength;
    out[2] = static_cast<f64>(tint & 0x1F) / 31.0 * strength;
}

// A cluster while it is being grown. Everything a Light needs, in voxels rather than metres.
struct Cluster {
    i64 sum[3]{0, 0, 0};       // for the centroid, over EMITTING voxels
    i32 low[3]{0, 0, 0};
    i32 high[3]{0, 0, 0};
    f64 radiance[3]{0, 0, 0};   // summed over emitting faces, so a two-material fitting is weighted
    i64 faces = 0;              // emissive voxel faces that touch air
    i64 voxels = 0;
    bool any = false;
};

// The six-connected fill, on a stack rather than by recursion: a solid emissive slab in the
// facility is thousands of voxels and a recursive fill on that is a stack overflow, not a bug you
// find by reading.
inline void grow(const ws::Clip& clip, const std::vector<u8>& emissive, std::vector<i32>& label,
                 i32 mark, i32 sx, i32 sy, i32 sz, std::vector<i32>& stack) {
    const i32 dx = clip.size[0], dy = clip.size[1], dz = clip.size[2];
    const auto at = [&](i32 x, i32 y, i32 z) {
        return static_cast<usize>(x) + static_cast<usize>(y) * static_cast<usize>(dx) +
               static_cast<usize>(z) * static_cast<usize>(dx) * static_cast<usize>(dy);
    };
    stack.clear();
    stack.push_back(sx);
    stack.push_back(sy);
    stack.push_back(sz);
    label[at(sx, sy, sz)] = mark;
    while (!stack.empty()) {
        const i32 z = stack.back(); stack.pop_back();
        const i32 y = stack.back(); stack.pop_back();
        const i32 x = stack.back(); stack.pop_back();
        const i32 offsets[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (const auto& o : offsets) {
            const i32 nx = x + o[0], ny = y + o[1], nz = z + o[2];
            if (nx < 0 || ny < 0 || nz < 0 || nx >= dx || ny >= dy || nz >= dz) continue;
            const usize n = at(nx, ny, nz);
            if (!emissive[n] || label[n] >= 0) continue;
            label[n] = mark;
            stack.push_back(nx);
            stack.push_back(ny);
            stack.push_back(nz);
        }
    }
}

// Does anything stand between the light and `range` metres along `dir`?
//
// Against the clip's OWN VOXELS, at 0.4 of a voxel a step, and not against a coarser occupancy
// grid. A coarse grid was written first and it is the wrong tool twice over: half a metre a cell is
// bigger than a whole sconce, so every ray began and ended in the same cell; and the distance it
// reports is a cell out, which forces a shadow bias larger than the walls this is meant to stop
// light passing through. At the clip's own pitch the bias is two voxels and a 0.45 m wall is opaque.
//
// Blockers inside the fitting's own box are ignored: a lamp is made of matter and a ray that starts
// inside it reports the lamp shadowing itself, which put every light in the clip out.
template <typename Solid>
inline f64 first_blocker(const Solid& solid, f64 voxel, const f64 from[3], const f64 dir[3],
                         const f64 skip_low[3], const f64 skip_high[3], const f64 origin[3],
                         f64 range) {
    const f64 step = voxel * 0.4;   // under half a voxel, so a one-voxel sheet is never stepped over
    const i32 steps = static_cast<i32>(range / step) + 1;
    for (i32 s = 1; s <= steps; ++s) {
        const f64 t = static_cast<f64>(s) * step;
        if (t > range) break;
        const f64 p[3] = {from[0] + dir[0] * t, from[1] + dir[1] * t, from[2] + dir[2] * t};
        if (p[0] >= skip_low[0] && p[0] <= skip_high[0] && p[1] >= skip_low[1] &&
            p[1] <= skip_high[1] && p[2] >= skip_low[2] && p[2] <= skip_high[2]) {
            continue;   // still inside the fitting itself
        }
        // Out of the clip is open air. A clip is a box and everything past it is sky, which is what
        // keeps a lamp on an outside wall from being shadowed by the edge of its own sampled box.
        const i32 g[3] = {static_cast<i32>(std::floor((p[0] - origin[0]) / voxel)),
                          static_cast<i32>(std::floor((p[1] - origin[1]) / voxel)),
                          static_cast<i32>(std::floor((p[2] - origin[2]) / voxel))};
        if (solid(g[0], g[1], g[2])) return t;
    }
    return range;
}

// The direction one texel of one cube face looks in. The face order is the one every cube map uses
// and the one `lights.js` decodes back: +X -X +Y -Y +Z -Z.
inline void cube_direction(i32 face, f64 u, f64 v, f64 out[3]) {
    switch (face) {
        case 0: out[0] = 1;  out[1] = -v; out[2] = -u; break;
        case 1: out[0] = -1; out[1] = -v; out[2] = u;  break;
        case 2: out[0] = u;  out[1] = 1;  out[2] = v;  break;
        case 3: out[0] = u;  out[1] = -1; out[2] = -v; break;
        case 4: out[0] = u;  out[1] = -v; out[2] = 1;  break;
        default: out[0] = -u; out[1] = -v; out[2] = -1; break;
    }
    const f64 length = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    out[0] /= length;
    out[1] /= length;
    out[2] /= length;
}

}  // namespace detail

// ---------------------------------------------------------------------------------------------
// The whole of it: a sampled clip in, a light list and a shadow atlas out.
//
//   `origin`   the world metres of voxel (0, 0, 0)
//   `metre`    voxels to the metre this clip was sampled at
// ---------------------------------------------------------------------------------------------

inline LightBake bake_lights(const ws::Clip& clip, const ws::VoxelTypeTable& types,
                             const f64 origin[3], i32 metre, ws::JobSystem& jobs) {
    LightBake out;
    const i32 dx = clip.size[0], dy = clip.size[1], dz = clip.size[2];
    if (dx <= 0 || dy <= 0 || dz <= 0) return out;
    const usize cells = static_cast<usize>(dx) * static_cast<usize>(dy) * static_cast<usize>(dz);
    const f64 voxel = 1.0 / static_cast<f64>(metre);

    const auto at = [&](i32 x, i32 y, i32 z) {
        return static_cast<usize>(x) + static_cast<usize>(y) * static_cast<usize>(dx) +
               static_cast<usize>(z) * static_cast<usize>(dx) * static_cast<usize>(dy);
    };
    const auto solid = [&](i32 x, i32 y, i32 z) {
        if (x < 0 || y < 0 || z < 0 || x >= dx || y >= dy || z >= dz) return false;
        const usize i = at(x, y, z);
        return clip.inside[i] != 0 && clip.voxels[i] != ws::kAir;
    };

    // ---- which voxels emit ------------------------------------------------------------------
    std::vector<u8> emissive(cells, 0);
    bool any = false;
    for (i32 z = 0; z < dz; ++z) {
        for (i32 y = 0; y < dy; ++y) {
            for (i32 x = 0; x < dx; ++x) {
                if (!solid(x, y, z)) continue;
                const usize i = at(x, y, z);
                if (types.visual_of(clip.voxels[i]).emissive == 0) continue;
                emissive[i] = 1;
                any = true;
            }
        }
    }
    if (!any) return out;

    // ---- flood fill them into fittings --------------------------------------------------------
    std::vector<i32> label(cells, -1);
    std::vector<i32> stack;
    i32 marks = 0;
    for (i32 z = 0; z < dz; ++z) {
        for (i32 y = 0; y < dy; ++y) {
            for (i32 x = 0; x < dx; ++x) {
                const usize i = at(x, y, z);
                if (!emissive[i] || label[i] >= 0) continue;
                detail::grow(clip, emissive, label, marks, x, y, z, stack);
                ++marks;
            }
        }
    }

    // ---- and then cut any fitting that is too long to stand for a sphere ----------------------
    //
    // The corona lucis is a hoop 3.60 m across. One light at the centre of a hoop is a light in the
    // one place the fitting has no matter, so a cluster whose box is longer than kSplitMetres is
    // re-keyed by which kSplitMetres cell each of its voxels falls in.
    const i32 split = std::max(1, static_cast<i32>(std::lround(kSplitMetres * metre)));
    std::vector<i32> box_low(static_cast<usize>(marks) * 3, 0);
    std::vector<i32> box_high(static_cast<usize>(marks) * 3, 0);
    std::vector<u8> seen(static_cast<usize>(marks), 0);
    for (i32 z = 0; z < dz; ++z) {
        for (i32 y = 0; y < dy; ++y) {
            for (i32 x = 0; x < dx; ++x) {
                const i32 mark = label[at(x, y, z)];
                if (mark < 0) continue;
                const usize m = static_cast<usize>(mark);
                const i32 p[3] = {x, y, z};
                if (!seen[m]) {
                    seen[m] = 1;
                    for (i32 a = 0; a < 3; ++a) { box_low[m * 3 + a] = p[a]; box_high[m * 3 + a] = p[a]; }
                } else {
                    for (i32 a = 0; a < 3; ++a) {
                        box_low[m * 3 + a] = std::min(box_low[m * 3 + a], p[a]);
                        box_high[m * 3 + a] = std::max(box_high[m * 3 + a], p[a]);
                    }
                }
            }
        }
    }

    // A cluster that has to be cut gets a base index and a stride in each axis, so a voxel's final
    // key is base + piece, computed in one pass with no map lookups.
    std::vector<i32> piece_base(static_cast<usize>(marks), -1);
    std::vector<i32> piece_span(static_cast<usize>(marks) * 3, 1);
    i32 total = 0;
    for (i32 m = 0; m < marks; ++m) {
        const usize u = static_cast<usize>(m);
        i32 spans[3] = {1, 1, 1};
        for (i32 a = 0; a < 3; ++a) {
            const i32 extent = box_high[u * 3 + a] - box_low[u * 3 + a] + 1;
            spans[a] = std::max(1, (extent + split - 1) / split);
            piece_span[u * 3 + a] = spans[a];
        }
        piece_base[u] = total;
        total += spans[0] * spans[1] * spans[2];
        if (total > static_cast<i32>(kMaxLights) * 64) break;   // a pathological clip stops here
    }
    if (total <= 0) return out;

    std::vector<detail::Cluster> pieces(static_cast<usize>(total));

    for (i32 z = 0; z < dz; ++z) {
        for (i32 y = 0; y < dy; ++y) {
            for (i32 x = 0; x < dx; ++x) {
                const usize i = at(x, y, z);
                const i32 mark = label[i];
                if (mark < 0) continue;
                const usize m = static_cast<usize>(mark);
                if (piece_base[m] < 0) continue;
                const i32 key = piece_base[m] +
                                ((x - box_low[m * 3 + 0]) / split) +
                                ((y - box_low[m * 3 + 1]) / split) * piece_span[m * 3 + 0] +
                                ((z - box_low[m * 3 + 2]) / split) * piece_span[m * 3 + 0] *
                                    piece_span[m * 3 + 1];
                if (key < 0 || key >= total) continue;
                detail::Cluster& c = pieces[static_cast<usize>(key)];

                // How much of this voxel actually radiates: the faces that touch air. A lamp buried
                // in its own fixture emits nowhere, and it is the area — not the voxel count — that
                // the intensity comes from.
                i32 open = 0;
                if (!solid(x + 1, y, z)) ++open;
                if (!solid(x - 1, y, z)) ++open;
                if (!solid(x, y + 1, z)) ++open;
                if (!solid(x, y - 1, z)) ++open;
                if (!solid(x, y, z + 1)) ++open;
                if (!solid(x, y, z - 1)) ++open;

                const i32 p[3] = {x, y, z};
                if (!c.any) {
                    c.any = true;
                    for (i32 a = 0; a < 3; ++a) { c.low[a] = p[a]; c.high[a] = p[a]; }
                } else {
                    for (i32 a = 0; a < 3; ++a) {
                        c.low[a] = std::min(c.low[a], p[a]);
                        c.high[a] = std::max(c.high[a], p[a]);
                    }
                }
                c.voxels += 1;
                if (open == 0) continue;
                f64 radiance[3];
                detail::radiance_of(types.visual_of(clip.voxels[i]), radiance);
                for (i32 a = 0; a < 3; ++a) c.radiance[a] += radiance[a] * static_cast<f64>(open);
                c.faces += open;
                // Weighted by how much of the voxel is open, so the centroid is the middle of the
                // EMITTING SURFACE rather than of the matter: the middle of a hemispherical bowl's
                // shell is not the middle of the solid it was cut from, and the shell is what
                // radiates.
                for (i32 a = 0; a < 3; ++a) c.sum[a] += static_cast<i64>(p[a]) * open;
            }
        }
    }

    // ---- pieces into lights ------------------------------------------------------------------
    const f64 face_area = voxel * voxel;
    std::vector<Light> found;
    found.reserve(static_cast<usize>(total));
    for (const detail::Cluster& c : pieces) {
        if (!c.any) continue;
        out.clusters += 1;
        if (c.faces == 0) {
            out.dark += 1;
            continue;
        }
        Light light;
        const f64 area = static_cast<f64>(c.faces) * face_area;
        light.area = static_cast<f32>(area);
        // The centroid, over the voxels that carry an emitting face, weighted by how many faces
        // each of them opens onto air. +0.5 puts it in the middle of a voxel and not on its corner.
        for (i32 a = 0; a < 3; ++a) {
            const f64 mean = static_cast<f64>(c.sum[a]) / static_cast<f64>(c.faces);
            light.position[a] = static_cast<f32>(origin[a] + (mean + 0.5) * voxel);
            light.half[a] = static_cast<f32>(
                (static_cast<f64>(c.high[a] - c.low[a]) + 1.0) * 0.5 * voxel);
        }
        // The sphere that stands for the fitting. Half the longest axis, never less than half a
        // voxel: it clamps 1/d^2 close up and it is what widens the specular lobe, so a bowl three
        // voxels across has a highlight the size of a bowl rather than the size of a pinprick.
        const f64 radius = std::max({static_cast<f64>(light.half[0]), static_cast<f64>(light.half[1]),
                                     static_cast<f64>(light.half[2]), voxel * 0.5});
        light.radius = static_cast<f32>(radius);
        // Mean radiance over the emitting faces, times the area, over four. See the header.
        for (i32 a = 0; a < 3; ++a) {
            const f64 mean = c.radiance[a] / static_cast<f64>(c.faces);
            light.rgb[a] = static_cast<f32>(mean * area * 0.25);
        }
        if (light.power() <= 0.0) continue;
        found.push_back(light);
    }

    // Strongest first, so the cap and the shadow budget are both spent on the lamps that matter.
    // Ties broken by position so two identical sconces do not swap places between two bakes and
    // change the file's hash for no reason.
    std::stable_sort(found.begin(), found.end(), [](const Light& a, const Light& b) {
        if (a.power() != b.power()) return a.power() > b.power();
        if (a.position[0] != b.position[0]) return a.position[0] < b.position[0];
        if (a.position[1] != b.position[1]) return a.position[1] < b.position[1];
        return a.position[2] < b.position[2];
    });
    if (found.size() > kMaxLights) {
        out.dropped = found.size() - kMaxLights;
        found.resize(kMaxLights);
    }
    out.lights = std::move(found);
    if (out.lights.empty()) return out;

    // ---- the shadow atlas ---------------------------------------------------------------------
    const u64 began = ws::now_ns();
    const usize shadowed = std::min(out.lights.size(), kMaxShadowed);
    out.shadowed = shadowed;
    const i32 tiles = static_cast<i32>(shadowed) * 6;
    out.atlas_w = kTilesPerRow * kShadowTile;
    out.atlas_h = ((tiles + kTilesPerRow - 1) / kTilesPerRow) * kShadowTile;
    out.atlas.assign(static_cast<usize>(out.atlas_w) * static_cast<usize>(out.atlas_h), 255);

    // One range for every light, so the shader needs one uniform rather than a per-light one. The
    // clip's own diagonal, capped: past 32 m a byte is 12 cm and the quantisation starts to show as
    // a step in a shadow that should be straight.
    const f64 span[3] = {static_cast<f64>(dx) * voxel, static_cast<f64>(dy) * voxel,
                         static_cast<f64>(dz) * voxel};
    const f64 range = std::min(32.0, std::sqrt(span[0] * span[0] + span[1] * span[1] +
                                               span[2] * span[2]) + 1.0);
    out.range = static_cast<f32>(range);

    jobs.parallel_for(shadowed, 1, [&](usize begin, usize end) {
        for (usize index = begin; index < end; ++index) {
            Light& light = out.lights[index];
            light.shadow = static_cast<i32>(index);
            const f64 from[3] = {light.position[0], light.position[1], light.position[2]};
            // The fitting's own matter, grown by a voxel, is what a ray ignores on its way out.
            const f64 grow = voxel;
            const f64 skip_low[3] = {from[0] - light.half[0] - grow, from[1] - light.half[1] - grow,
                                     from[2] - light.half[2] - grow};
            const f64 skip_high[3] = {from[0] + light.half[0] + grow, from[1] + light.half[1] + grow,
                                      from[2] + light.half[2] + grow};
            for (i32 face = 0; face < 6; ++face) {
                const i32 tile = static_cast<i32>(index) * 6 + face;
                const i32 tx = (tile % kTilesPerRow) * kShadowTile;
                const i32 ty = (tile / kTilesPerRow) * kShadowTile;
                for (i32 j = 0; j < kShadowTile; ++j) {
                    for (i32 i = 0; i < kShadowTile; ++i) {
                        const f64 u = (static_cast<f64>(i) + 0.5) / static_cast<f64>(kShadowTile) * 2.0 - 1.0;
                        const f64 v = (static_cast<f64>(j) + 0.5) / static_cast<f64>(kShadowTile) * 2.0 - 1.0;
                        f64 dir[3];
                        detail::cube_direction(face, u, v, dir);
                        const f64 hit = detail::first_blocker(solid, voxel, from, dir, skip_low,
                                                              skip_high, origin, range);
                        // Rounded UP, never down. The byte is the distance the viewer compares
                        // against, so a value short of the real blocker puts the surface AT the
                        // blocker in its own shadow -- acne on every wall a lamp is bolted to. Long
                        // by up to one quantum instead leaks light that far behind an occluder,
                        // which at 12 cm is well inside the thinnest wall in these clips.
                        const i32 byte = static_cast<i32>(std::ceil(hit / range * 255.0));
                        out.atlas[static_cast<usize>(ty + j) * static_cast<usize>(out.atlas_w) +
                                  static_cast<usize>(tx + i)] =
                            static_cast<u8>(std::clamp(byte, 0, 255));
                    }
                }
            }
        }
    });
    out.seconds = static_cast<f64>(ws::now_ns() - began) / 1e9;
    return out;
}

// The bytes that go in the `LGTS` chunk. Header, then `kLightBytes` a light, then the atlas.
// `web/js/features/lights.js` reads exactly this and the two are only correct together.
inline std::vector<u8> write_lights(const LightBake& bake) {
    std::vector<u8> out;
    if (bake.lights.empty()) return out;
    const auto push_u32 = [&out](u32 bits) {
        out.push_back(static_cast<u8>(bits & 0xFFu));
        out.push_back(static_cast<u8>((bits >> 8) & 0xFFu));
        out.push_back(static_cast<u8>((bits >> 16) & 0xFFu));
        out.push_back(static_cast<u8>((bits >> 24) & 0xFFu));
    };
    const auto push_f32 = [&push_u32](f32 value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        push_u32(bits);
    };

    push_u32(1);                                          //  0 the chunk's own version
    push_u32(static_cast<u32>(bake.lights.size()));       //  4
    push_u32(static_cast<u32>(bake.shadowed));            //  8
    push_u32(static_cast<u32>(kShadowTile));              // 12
    push_u32(static_cast<u32>(bake.atlas_w));             // 16
    push_u32(static_cast<u32>(bake.atlas_h));             // 20
    push_f32(bake.range);                                 // 24
    push_u32(static_cast<u32>(kTilesPerRow));             // 28

    for (const Light& light : bake.lights) {
        for (i32 a = 0; a < 3; ++a) push_f32(light.position[a]);
        push_f32(light.radius);
        for (i32 a = 0; a < 3; ++a) push_f32(light.rgb[a]);
        push_f32(light.area);
        for (i32 a = 0; a < 3; ++a) push_f32(light.half[a]);
        push_u32(static_cast<u32>(light.shadow));
    }
    out.insert(out.end(), bake.atlas.begin(), bake.atlas.end());
    return out;
}

}  // namespace ws::web
