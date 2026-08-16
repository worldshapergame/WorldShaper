// The clip viewer's baker: a clip in, something a phone can draw out.
//
// documentation/24-clip-viewer.md is what this is for and why it exists. In one paragraph: the
// person this project is for cannot run a path tracer on a phone, and the clips are being changed
// by other hands faster than anybody can open the game to look at them. So every clip in the
// repository is sampled here, meshed, lit, and written as one small file the viewer streams.
//
// # It is the game's own sampler, and that is the whole point
//
// Nothing in this file re-reads a clip file. `forge::load_clip_script` parses it, `forge::sample`
// turns the field into voxels, `forge::despeckle` cleans it exactly as the game does. What the
// viewer shows is therefore what the clip IS, and a disagreement between the website and the game
// can only come from the shading — never from a second reading of the language, which is the
// failure D204 is named for and the one thing a viewer like this is most likely to introduce.
//
// # What comes out
//
// One `.wsc` file per clip, holding four things:
//
//   materials    the VisualRecord of every material used, verbatim, 16 bytes each. Colour,
//                opacity, roughness, metallic, index of refraction, emission, absorption,
//                translucency, clearcoat and sheen all reach the browser unquantised, because
//                "rasterised" is a statement about the light transport and not about the matter.
//   quads        the surface, greedy-meshed, 16 bytes a quad, grouped by which way they face so
//                the viewer draws six ranges rather than sorting anything.
//   a light grid a small 3D lattice of (sun visibility, sky visibility), ray cast here so the
//                browser never casts anything. This is what makes an interior dark and a
//                courtyard bright, and it costs one texture fetch.
//   occupancy    one bit per collision cell, which is what the viewer walks on.
//
// # Why the light is baked into a lattice and not into the vertices
//
// The obvious place for baked light is the vertex, and it does not survive greedy meshing: two
// faces may only merge when everything about them agrees, so a smooth gradient of sky visibility
// across a wall makes every voxel face its own quad and the mesh stops being a mesh. Corner
// ambient occlusion is two bits and takes four values, so it merges; sky visibility is a gradient
// and does not. Splitting them by that property — the coarse, smooth term into a volume texture
// and the sharp, quantised term into the quad — is what lets a wall stay one quad and still get
// darker as it goes into a room.
//
// # Resolution is chosen by a budget, not written down
//
// A clip says how finely it wants to be sampled and the answer is usually 32 voxels to the metre,
// which for the facility is 582 million cells and minutes of sampling. So the baker halves the
// authored resolution until the cell count fits a budget. That keeps a small clip at full detail,
// keeps a building at a detail a phone can hold, and — the part that matters — needs no list of
// per-clip resolutions that somebody has to remember to edit when a fragment grows.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/jobs.hpp"
#include "core/log.hpp"
#include "core/time.hpp"
#include "core/types.hpp"
#include "forge/clip_script.hpp"
#include "forge/field.hpp"
#include "forge/measure.hpp"
#include "forge/sample.hpp"
#include "game/clip.hpp"
#include "world/tags.hpp"
#include "world/voxel_type.hpp"

namespace fs = std::filesystem;

namespace {

using ws::f32;
using ws::f64;
using ws::i32;
using ws::i64;
using ws::u16;
using ws::u32;
using ws::u64;
using ws::u8;
using ws::usize;

// --------------------------------------------------------------------------------------
// Options
// --------------------------------------------------------------------------------------

struct Options {
    fs::path clips = "clips";
    fs::path out = "web/data";
    // Cells the sampled box may hold before the baker asks for a coarser one. Six million is
    // about a second and a half of sampling for a simple clip and about forty for the whole
    // facility, and it is the number that keeps a full bake inside a CI job.
    i64 budget = 6'000'000;
    i32 max_metre = 32;
    std::string only;   // bake just the clip whose id matches, for working on one
    bool verbose = false;
};

// The direction the sun is in for every bake, and it is a decision rather than a default.
//
// The facility faces south (-z, `_contract.clip`), so a sun in the south-east at about fifty
// degrees puts light on the elevation the building is judged from, throws the portico's columns
// across the wall behind them, and still reaches down the oculus. A sun overhead lights nothing
// interesting and a sun in the north lights the back of the building.
constexpr f64 kSunDir[3] = {0.42, 0.80, -0.43};

// --------------------------------------------------------------------------------------
// Small helpers
// --------------------------------------------------------------------------------------

u64 fnv1a(const u8* data, usize count, u64 seed = 0xcbf29ce484222325ull) {
    u64 hash = seed;
    for (usize i = 0; i < count; ++i) {
        hash ^= static_cast<u64>(data[i]);
        hash *= 0x100000001b3ull;
    }
    return hash;
}

void put_u32(std::vector<u8>& out, usize at, u32 value) {
    out[at + 0] = static_cast<u8>(value & 0xFFu);
    out[at + 1] = static_cast<u8>((value >> 8) & 0xFFu);
    out[at + 2] = static_cast<u8>((value >> 16) & 0xFFu);
    out[at + 3] = static_cast<u8>((value >> 24) & 0xFFu);
}

void put_i32(std::vector<u8>& out, usize at, i32 value) {
    put_u32(out, at, static_cast<u32>(value));
}

void put_f32(std::vector<u8>& out, usize at, f64 value) {
    const f32 narrowed = static_cast<f32>(value);
    u32 bits = 0;
    std::memcpy(&bits, &narrowed, sizeof(bits));
    put_u32(out, at, bits);
}

std::string hex64(u64 value) {
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
    return std::string(buffer);
}

// A path under clips/ turned into one word: `facility/dome.clip` becomes `facility-dome`. It is
// the file name, the URL fragment and the key the viewer remembers a camera against, so it has to
// survive being all three.
std::string identifier(const fs::path& relative) {
    std::string id = relative.generic_string();
    const std::string suffix = ".clip";
    if (id.size() > suffix.size() && id.compare(id.size() - suffix.size(), suffix.size(), suffix) == 0) {
        id.resize(id.size() - suffix.size());
    }
    for (char& c : id) {
        if (c == '/' || c == '\\' || c == ' ' || c == '.') c = '-';
    }
    return id;
}

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// --------------------------------------------------------------------------------------
// The occupancy grids
//
// Two of them, at two resolutions, for two questions that want opposite answers:
//
//   collision  a cell is solid if ANY voxel in it is. Conservative, so a player can never walk
//              through a wall that is thinner than a collision cell.
//   light      a cell is solid if ENOUGH of it is. A window mullion or a balustrade should not
//              turn a whole half-metre cell opaque and put the room behind it in the dark.
// --------------------------------------------------------------------------------------

struct BitGrid {
    i32 dims[3]{0, 0, 0};
    i32 voxels_per_metre = 1;
    std::vector<u8> bits;

    usize index(i32 x, i32 y, i32 z) const {
        return static_cast<usize>(x) + static_cast<usize>(y) * static_cast<usize>(dims[0]) +
               static_cast<usize>(z) * static_cast<usize>(dims[0]) * static_cast<usize>(dims[1]);
    }
    bool at(i32 x, i32 y, i32 z) const {
        if (x < 0 || y < 0 || z < 0 || x >= dims[0] || y >= dims[1] || z >= dims[2]) return false;
        const usize i = index(x, y, z);
        return (bits[i >> 3] & static_cast<u8>(1u << (i & 7u))) != 0;
    }
    void set(i32 x, i32 y, i32 z) {
        const usize i = index(x, y, z);
        bits[i >> 3] |= static_cast<u8>(1u << (i & 7u));
    }
};

// --------------------------------------------------------------------------------------
// One baked clip
// --------------------------------------------------------------------------------------

struct Quad {
    u16 x = 0, y = 0, z = 0;   // the voxel the face belongs to
    u16 w = 0, h = 0;          // extent along the face's two in-plane axes
    u16 material = 0;
    u8 ao = 0xFF;              // four corners, two bits each
};

struct Baked {
    std::string id;
    std::string source;   // path under clips/, as written
    std::string group;
    i32 dims[3]{0, 0, 0};
    i32 metre = 32;
    i32 authored_metre = 32;
    f64 origin[3]{0, 0, 0};
    f64 matter_low[3]{0, 0, 0};
    f64 matter_high[3]{0, 0, 0};
    u64 solid = 0;
    u32 quads = 0;
    u64 hash = 0;
    usize bytes = 0;
    std::vector<std::string> materials;   // names, for the viewer's material list
};

// The six faces, in the order the file stores them.
//
//   0 +X   1 -X   2 +Y   3 -Y   4 +Z   5 -Z
//
// Each has an axis it is normal to and two in-plane axes, chosen so that u cross v is the
// positive normal. That is what lets the vertex shader wind a quad correctly by looking at one
// bit rather than at a table.
constexpr i32 kFaceAxis[6] = {0, 0, 1, 1, 2, 2};
constexpr i32 kFaceU[6] = {1, 1, 2, 2, 0, 0};
constexpr i32 kFaceV[6] = {2, 2, 0, 0, 1, 1};
constexpr i32 kFaceSign[6] = {1, -1, 1, -1, 1, -1};

class ClipMesher {
public:
    ClipMesher(const ws::Clip& clip, const ws::VoxelTypeTable& types) : clip_(clip), types_(types) {
        dims_[0] = clip.size[0];
        dims_[1] = clip.size[1];
        dims_[2] = clip.size[2];
    }

    bool solid(i32 x, i32 y, i32 z) const {
        if (x < 0 || y < 0 || z < 0 || x >= dims_[0] || y >= dims_[1] || z >= dims_[2]) return false;
        const usize i = static_cast<usize>(x) + static_cast<usize>(y) * static_cast<usize>(dims_[0]) +
                        static_cast<usize>(z) * static_cast<usize>(dims_[0]) * static_cast<usize>(dims_[1]);
        return clip_.inside[i] != 0 && clip_.voxels[i] != ws::kAir;
    }

    ws::VoxelTypeId type_at(i32 x, i32 y, i32 z) const {
        const usize i = static_cast<usize>(x) + static_cast<usize>(y) * static_cast<usize>(dims_[0]) +
                        static_cast<usize>(z) * static_cast<usize>(dims_[0]) * static_cast<usize>(dims_[1]);
        return clip_.voxels[i];
    }

    // The palette, interned by what a material LOOKS like rather than by its type id. Two types
    // that differ only in their tags shade identically and there is no reason to send both.
    u16 material_of(ws::VoxelTypeId type) {
        const auto found = by_type_.find(type);
        if (found != by_type_.end()) return found->second;
        const ws::VisualRecord& record = types_.visual_of(type);
        const u64 key = record.content_hash();
        const auto seen = by_visual_.find(key);
        u16 index = 0;
        if (seen != by_visual_.end()) {
            index = seen->second;
        } else {
            index = static_cast<u16>(palette_.size());
            palette_.push_back(record);
            by_visual_.emplace(key, index);
        }
        by_type_.emplace(type, index);
        return index;
    }

    // Ambient occlusion at one corner of one face, the way every voxel renderer does it: how many
    // of the three cells that share that corner, on the air side of the face, are solid.
    u8 corner_ao(i32 cx, i32 cy, i32 cz, i32 face, i32 du, i32 dv) const {
        const i32 axis = kFaceAxis[face];
        const i32 u = kFaceU[face];
        const i32 v = kFaceV[face];
        i32 air[3]{cx, cy, cz};
        air[axis] += kFaceSign[face];

        const i32 su = du * 2 - 1;
        const i32 sv = dv * 2 - 1;

        i32 a[3]{air[0], air[1], air[2]};
        a[u] += su;
        i32 b[3]{air[0], air[1], air[2]};
        b[v] += sv;
        i32 c[3]{air[0], air[1], air[2]};
        c[u] += su;
        c[v] += sv;

        const bool side1 = solid(a[0], a[1], a[2]);
        const bool side2 = solid(b[0], b[1], b[2]);
        // Two sides closed means the corner is in a crease whatever the diagonal does, and asking
        // about the diagonal there is what produces the light seam along an inside corner.
        if (side1 && side2) return 0;
        const bool corner = solid(c[0], c[1], c[2]);
        return static_cast<u8>(3 - (static_cast<i32>(side1) + static_cast<i32>(side2) +
                                    static_cast<i32>(corner)));
    }

    // Greedy meshing, one face direction and one slab at a time.
    //
    // Two faces merge only when their material AND all four of their ambient occlusion corners
    // agree, which is what keeps the shading of a merged quad exactly the shading of the faces it
    // replaced. On a flat wall every face agrees and the whole wall is one quad; within a voxel of
    // an edge they disagree and the edge keeps its own strip.
    void build() {
        for (i32 face = 0; face < 6; ++face) {
            const i32 axis = kFaceAxis[face];
            const i32 u = kFaceU[face];
            const i32 v = kFaceV[face];
            const i32 su = dims_[u];
            const i32 sv = dims_[v];
            std::vector<u32> mask(static_cast<usize>(su) * static_cast<usize>(sv), 0);
            std::vector<u16> mask_material(mask.size(), 0);

            for (i32 slab = 0; slab < dims_[axis]; ++slab) {
                std::fill(mask.begin(), mask.end(), 0u);
                for (i32 j = 0; j < sv; ++j) {
                    for (i32 i = 0; i < su; ++i) {
                        i32 cell[3]{0, 0, 0};
                        cell[axis] = slab;
                        cell[u] = i;
                        cell[v] = j;
                        if (!solid(cell[0], cell[1], cell[2])) continue;
                        i32 air[3]{cell[0], cell[1], cell[2]};
                        air[axis] += kFaceSign[face];
                        if (solid(air[0], air[1], air[2])) continue;

                        u8 ao = 0;
                        for (i32 corner = 0; corner < 4; ++corner) {
                            const i32 du = corner & 1;
                            const i32 dv = (corner >> 1) & 1;
                            ao = static_cast<u8>(
                                ao | static_cast<u8>(corner_ao(cell[0], cell[1], cell[2], face, du, dv)
                                                     << (corner * 2)));
                        }
                        const u16 material = material_of(type_at(cell[0], cell[1], cell[2]));
                        const usize at = static_cast<usize>(i) + static_cast<usize>(j) * static_cast<usize>(su);
                        // Bit 31 marks the cell as carrying a face at all, so that a legitimate
                        // material 0 with ambient occlusion 0 is not read as empty.
                        mask[at] = 0x80000000u | (static_cast<u32>(ao) << 16) | static_cast<u32>(material);
                        mask_material[at] = material;
                    }
                }

                for (i32 j = 0; j < sv; ++j) {
                    for (i32 i = 0; i < su;) {
                        const usize at = static_cast<usize>(i) + static_cast<usize>(j) * static_cast<usize>(su);
                        const u32 here = mask[at];
                        if (here == 0) {
                            ++i;
                            continue;
                        }
                        i32 width = 1;
                        while (i + width < su && mask[at + static_cast<usize>(width)] == here) ++width;

                        i32 height = 1;
                        bool growing = true;
                        while (growing && j + height < sv) {
                            const usize row = at + static_cast<usize>(height) * static_cast<usize>(su);
                            for (i32 k = 0; k < width; ++k) {
                                if (mask[row + static_cast<usize>(k)] != here) {
                                    growing = false;
                                    break;
                                }
                            }
                            if (growing) ++height;
                        }

                        for (i32 hh = 0; hh < height; ++hh) {
                            const usize row = at + static_cast<usize>(hh) * static_cast<usize>(su);
                            for (i32 ww = 0; ww < width; ++ww) mask[row + static_cast<usize>(ww)] = 0;
                        }

                        Quad quad;
                        i32 cell[3]{0, 0, 0};
                        cell[axis] = slab;
                        cell[u] = i;
                        cell[v] = j;
                        quad.x = static_cast<u16>(cell[0]);
                        quad.y = static_cast<u16>(cell[1]);
                        quad.z = static_cast<u16>(cell[2]);
                        quad.w = static_cast<u16>(width);
                        quad.h = static_cast<u16>(height);
                        quad.material = static_cast<u16>(here & 0xFFFFu);
                        quad.ao = static_cast<u8>((here >> 16) & 0xFFu);

                        // Glass is drawn after everything else and blended, so it travels in its
                        // own list. Anything that lets light through at all counts: a wall that is
                        // 250/255 opaque still has to be blended or the pane behind it vanishes.
                        const bool clear = palette_[quad.material].opacity < 255;
                        (clear ? transparent_ : opaque_)[static_cast<usize>(face)].push_back(quad);

                        i += width;
                    }
                }
            }
        }
    }

    const std::vector<ws::VisualRecord>& palette() const { return palette_; }
    const std::array<std::vector<Quad>, 6>& opaque() const { return opaque_; }
    const std::array<std::vector<Quad>, 6>& transparent() const { return transparent_; }

private:
    const ws::Clip& clip_;
    const ws::VoxelTypeTable& types_;
    i32 dims_[3]{0, 0, 0};
    std::vector<ws::VisualRecord> palette_;
    std::unordered_map<u64, u16> by_visual_;
    std::unordered_map<ws::VoxelTypeId, u16> by_type_;
    std::array<std::vector<Quad>, 6> opaque_;
    std::array<std::vector<Quad>, 6> transparent_;
};

// --------------------------------------------------------------------------------------
// The light grid
//
// A lattice of points about forty centimetres apart, each holding how much of the sun and how
// much of the sky reaches it. Cast here, once, against a coarse copy of the clip; read in the
// browser as one trilinear fetch from a 3D texture.
//
// This is the entire reason an interior looks like an interior in the viewer. Without it a room
// with one door is lit exactly as brightly as the lawn outside, because a rasteriser has no
// opinion about what is between a surface and the sky.
// --------------------------------------------------------------------------------------

// Thirty-two directions spread evenly over the sphere, by the Fibonacci construction. Even
// spacing matters more than count here: eight rays in a bad arrangement give a room banding that
// looks like a bug in the shader.
std::vector<ws::forge::Vec3> sphere_directions(i32 count) {
    std::vector<ws::forge::Vec3> out;
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

// Does anything stand between `from` and `distance` metres along `dir`? A step of one coarse cell
// is exact enough for a visibility term: the grid it walks is half a metre, and a ray that clips
// the corner of a cell it should have missed changes a sky fraction by one part in thirty-two.
bool blocked(const BitGrid& grid, const f64 origin[3], const ws::forge::Vec3& from,
             const ws::forge::Vec3& dir, f64 distance) {
    const f64 cell = 1.0 / static_cast<f64>(grid.voxels_per_metre);
    const i32 steps = static_cast<i32>(distance / cell) + 1;
    for (i32 s = 1; s <= steps; ++s) {
        const f64 t = static_cast<f64>(s) * cell;
        if (t > distance) break;
        const f64 wx = from.x + dir.x * t;
        const f64 wy = from.y + dir.y * t;
        const f64 wz = from.z + dir.z * t;
        const i32 gx = static_cast<i32>(std::floor((wx - origin[0]) / cell));
        const i32 gy = static_cast<i32>(std::floor((wy - origin[1]) / cell));
        const i32 gz = static_cast<i32>(std::floor((wz - origin[2]) / cell));
        // Outside the clip is sky. A clip is a box and everything beyond it is open air, which is
        // what makes the top of a roof fully lit rather than half lit by its own bounding box.
        if (gx < 0 || gy < 0 || gz < 0 || gx >= grid.dims[0] || gy >= grid.dims[1] ||
            gz >= grid.dims[2]) {
            return false;
        }
        if (grid.at(gx, gy, gz)) return true;
    }
    return false;
}

struct LightGrid {
    i32 dims[3]{0, 0, 0};
    f64 cell = 0.4;
    std::vector<u8> texels;   // two bytes a point: sun, sky
};

LightGrid bake_light(const BitGrid& coarse, const f64 origin[3], const f64 size_metres[3],
                     ws::JobSystem& jobs) {
    LightGrid light;
    light.cell = 0.4;
    for (i32 axis = 0; axis < 3; ++axis) {
        light.dims[axis] = static_cast<i32>(size_metres[axis] / light.cell) + 3;
    }
    const usize points = static_cast<usize>(light.dims[0]) * static_cast<usize>(light.dims[1]) *
                         static_cast<usize>(light.dims[2]);
    light.texels.assign(points * 2, 0);
    std::vector<u8> known(points, 0);

    const std::vector<ws::forge::Vec3> sky_rays = sphere_directions(32);
    std::vector<f64> sky_weight(sky_rays.size(), 0.0);
    f64 sky_total = 0.0;
    for (usize i = 0; i < sky_rays.size(); ++i) {
        // Weighted towards straight up, with the horizon still counting for something. A sky is
        // brighter overhead than at the horizon and a point that can only see the horizon is in
        // shadow in a way a uniform weighting refuses to say.
        sky_weight[i] = std::max(0.0, 0.2 + 0.8 * sky_rays[i].y);
        sky_total += sky_weight[i];
    }

    // Eight rays in a small cone, so a shadow edge has eight steps in it instead of one. The cone
    // is about four degrees, which is eight times the real sun and is the right lie: the grid is
    // forty centimetres and a perfectly sharp shadow in it would be forty centimetres of stairs.
    std::vector<ws::forge::Vec3> sun_rays;
    {
        ws::forge::Vec3 sun{kSunDir[0], kSunDir[1], kSunDir[2]};
        const f64 length = std::sqrt(sun.x * sun.x + sun.y * sun.y + sun.z * sun.z);
        sun = {sun.x / length, sun.y / length, sun.z / length};
        ws::forge::Vec3 across{-sun.z, 0.0, sun.x};
        const f64 across_length = std::sqrt(across.x * across.x + across.z * across.z);
        across = {across.x / across_length, 0.0, across.z / across_length};
        const ws::forge::Vec3 up{sun.y * across.z - sun.z * across.y, sun.z * across.x - sun.x * across.z,
                                 sun.x * across.y - sun.y * across.x};
        for (i32 i = 0; i < 8; ++i) {
            const f64 angle = 2.0 * 3.14159265358979323846 * static_cast<f64>(i) / 8.0;
            const f64 spread = 0.07;
            ws::forge::Vec3 d{sun.x + (across.x * std::cos(angle) + up.x * std::sin(angle)) * spread,
                              sun.y + (across.y * std::cos(angle) + up.y * std::sin(angle)) * spread,
                              sun.z + (across.z * std::cos(angle) + up.z * std::sin(angle)) * spread};
            const f64 dl = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            sun_rays.push_back({d.x / dl, d.y / dl, d.z / dl});
        }
    }

    const f64 reach = 24.0;
    const f64 coarse_cell = 1.0 / static_cast<f64>(coarse.voxels_per_metre);

    jobs.parallel_for(static_cast<usize>(light.dims[2]), 1, [&](usize begin, usize end) {
        for (usize slab = begin; slab < end; ++slab) {
            const i32 z = static_cast<i32>(slab);
            for (i32 y = 0; y < light.dims[1]; ++y) {
                for (i32 x = 0; x < light.dims[0]; ++x) {
                    const usize at = static_cast<usize>(x) +
                                     static_cast<usize>(y) * static_cast<usize>(light.dims[0]) +
                                     static_cast<usize>(z) * static_cast<usize>(light.dims[0]) *
                                         static_cast<usize>(light.dims[1]);
                    const ws::forge::Vec3 p{origin[0] + static_cast<f64>(x) * light.cell,
                                            origin[1] + static_cast<f64>(y) * light.cell,
                                            origin[2] + static_cast<f64>(z) * light.cell};
                    const i32 gx = static_cast<i32>(std::floor((p.x - origin[0]) / coarse_cell));
                    const i32 gy = static_cast<i32>(std::floor((p.y - origin[1]) / coarse_cell));
                    const i32 gz = static_cast<i32>(std::floor((p.z - origin[2]) / coarse_cell));
                    // A point buried in stone has no light and no business lending any to the
                    // surface beside it. It is left unknown and filled in from its neighbours
                    // below, which is what stops a wall reading its own inside.
                    if (coarse.at(gx, gy, gz)) continue;

                    f64 sky = 0.0;
                    for (usize r = 0; r < sky_rays.size(); ++r) {
                        if (sky_weight[r] <= 0.0) continue;
                        if (!blocked(coarse, origin, p, sky_rays[r], reach)) sky += sky_weight[r];
                    }
                    f64 sun = 0.0;
                    for (const ws::forge::Vec3& d : sun_rays) {
                        if (!blocked(coarse, origin, p, d, reach)) sun += 1.0;
                    }
                    light.texels[at * 2 + 0] =
                        static_cast<u8>(std::lround(255.0 * sun / static_cast<f64>(sun_rays.size())));
                    light.texels[at * 2 + 1] =
                        static_cast<u8>(std::lround(255.0 * std::min(1.0, sky / sky_total)));
                    known[at] = 1;
                }
            }
        }
    });

    // Fill the points inside matter from the ones outside it. Three passes of "take the brightest
    // neighbour" reaches far enough that a surface never samples a point that knows nothing, and
    // the bias the viewer applies along the normal does the rest.
    for (i32 pass = 0; pass < 2; ++pass) {
        std::vector<u8> filled = known;
        for (i32 z = 0; z < light.dims[2]; ++z) {
            for (i32 y = 0; y < light.dims[1]; ++y) {
                for (i32 x = 0; x < light.dims[0]; ++x) {
                    const usize at = static_cast<usize>(x) +
                                     static_cast<usize>(y) * static_cast<usize>(light.dims[0]) +
                                     static_cast<usize>(z) * static_cast<usize>(light.dims[0]) *
                                         static_cast<usize>(light.dims[1]);
                    if (known[at]) continue;
                    i32 best_sun = -1;
                    i32 best_sky = -1;
                    const i32 offsets[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                               {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                    for (const auto& offset : offsets) {
                        const i32 nx = x + offset[0];
                        const i32 ny = y + offset[1];
                        const i32 nz = z + offset[2];
                        if (nx < 0 || ny < 0 || nz < 0 || nx >= light.dims[0] || ny >= light.dims[1] ||
                            nz >= light.dims[2]) {
                            continue;
                        }
                        const usize nat = static_cast<usize>(nx) +
                                          static_cast<usize>(ny) * static_cast<usize>(light.dims[0]) +
                                          static_cast<usize>(nz) * static_cast<usize>(light.dims[0]) *
                                              static_cast<usize>(light.dims[1]);
                        if (!known[nat]) continue;
                        best_sun = std::max(best_sun, static_cast<i32>(light.texels[nat * 2 + 0]));
                        best_sky = std::max(best_sky, static_cast<i32>(light.texels[nat * 2 + 1]));
                    }
                    if (best_sun < 0) continue;
                    // Halved on the way in, and the fraction is the whole of how much daylight
                    // leaks through a wall in this viewer.
                    //
                    // A point buried in stone is read by the SURFACES either side of it, because a
                    // trilinear fetch near a wall blends the air in front of it with the stone
                    // behind it. Borrow three quarters of the brightest neighbour and a room's
                    // ceiling is lit by the sky above the roof: the facility's halls came out with
                    // pale bands across every soffit. Half, twice, is dark enough that the leak is
                    // below what the eye picks out of the ambient and bright enough that a wall
                    // face is not outlined in black.
                    light.texels[at * 2 + 0] = static_cast<u8>(best_sun / 2);
                    light.texels[at * 2 + 1] = static_cast<u8>(best_sky / 2);
                    filled[at] = 1;
                }
            }
        }
        known.swap(filled);
    }

    return light;
}

// --------------------------------------------------------------------------------------
// Writing the file
// --------------------------------------------------------------------------------------

constexpr usize kHeaderBytes = 192;

void append_quads(std::vector<u8>& out, const std::vector<Quad>& quads) {
    for (const Quad& q : quads) {
        const u16 values[6] = {q.x, q.y, q.z, q.w, q.h, q.material};
        for (const u16 value : values) {
            out.push_back(static_cast<u8>(value & 0xFFu));
            out.push_back(static_cast<u8>((value >> 8) & 0xFFu));
        }
        out.push_back(q.ao);
        out.push_back(0);
        out.push_back(0);
        out.push_back(0);
    }
}

// --------------------------------------------------------------------------------------
// One clip, end to end
// --------------------------------------------------------------------------------------

// A parsed clip file, kept whole. The script's field indexes into nothing else, but its materials
// are ids into the table they were interned in, so the two travel together or the colours are
// somebody else's.
struct Program {
    ws::VoxelTypeTable types;
    ws::TagRegistry tags;
    ws::forge::Script script;
    bool parsed = false;
};

// Everything after the parse: sample the root, mesh it, light it, write it.
//
// `root` is which node is the matter, and it is not always the file's `solid`. A fragment of the
// facility declares `part_<its own name>` and leaves the manifest to say what the building is, so
// a fragment is baked from the MANIFEST'S parse with its own part as the root — which is also the
// only way it can be baked at all, because a fragment uses the Ionic vocabulary out of
// `_order.clip` and does not include it. Parsed on its own, `doors.clip` does not know what a
// dentil is.
bool bake_root(const Options& options, Program& program, u32 root, bool is_part,
               const fs::path& relative, ws::JobSystem& jobs, Baked& baked) {
    ws::forge::Script& script = program.script;
    ws::VoxelTypeTable& types = program.types;

    ws::forge::SampleSettings settings = script.settings;
    baked.authored_metre = settings.voxels_per_metre;

    // A fragment inherits the whole building's `bounds` from the contract it includes, so
    // sampling one on its own would sample thirty-four metres of mostly nothing at the resolution
    // the fragment deserves. The field already knows the box its own shapes reach, so the box is
    // taken from there and given a quarter metre of air to hold the faces at its edge.
    if (is_part) {
        // `origin` moves the finished clip so a chosen point in it lands on the world origin, and
        // it moves the solid and every paint rule together — but not the names the file bound on
        // the way to them, because nothing had ever asked for one of those afterwards. A part
        // taken by name is therefore in the space its author typed while the paint that belongs to
        // it is in the space the clip ended up in, and for the facility those are 3.5 m apart.
        //
        // It shows as a part sampled in a box 3.5 m from where its matter is: the dome came out
        // 197 x 21 x 197 voxels — a twelve metre saucer four fifths of a metre tall, sliced off at
        // the height the box ran out — and every material on it was whatever the paint rules
        // happened to say 3.5 m below where they meant. So the part is moved exactly the way
        // `apply_origin` moves the solid, and then it is in the same space as everything else.
        const f64 shift[3] = {script.origin_shift[0], script.origin_shift[1],
                              script.origin_shift[2]};
        if (shift[0] != 0.0 || shift[1] != 0.0 || shift[2] != 0.0) {
            root = script.field.translate(root, ws::forge::Vec3{shift[0], shift[1], shift[2]});
        }

        // And then cut to what the BUILDING makes of it, which is the whole point of showing a
        // fragment at all.
        //
        // A part as its file binds it is the part before the manifest has finished with it: the
        // walls before the doors and windows are punched through them, the rotunda as the solid
        // drum it is carved out of rather than as the room. `part_rotunda` on its own is a blank
        // white cylinder, and somebody checking what their morning's work looks like learns
        // nothing from it.
        //
        // Intersecting with the clip's own solid gives the part exactly as it stands in the
        // finished building — every void taken out of it, the grain displaced over it — with no
        // rule about which voids apply to which part. That distinction is real and subtle enough to
        // have been got wrong in the manifest twice (a room's air must not eat a lamp, a doorway
        // must), and this needs to know nothing about it: whatever the building kept is what shows.
        if (script.has_solid) {
            root = script.field.intersect({root, script.solid});
        }

        // The new nodes have no box until the boxes are built again, and everything downstream
        // reads them: without this the part is not culled at all and takes minutes.
        script.field.build_bounds();

        const ws::forge::Field::Aabb box = script.field.bounds_of(root);
        if (options.verbose) {
            std::printf("      root %u  aabb %.2f %.2f %.2f .. %.2f %.2f %.2f\n", root, box.low.x,
                        box.low.y, box.low.z, box.high.x, box.high.y, box.high.z);
        }
        if (!box.infinite()) {
            const f64 margin = 0.25;
            settings.low = {std::max(settings.low.x, box.low.x - margin),
                            std::max(settings.low.y, box.low.y - margin),
                            std::max(settings.low.z, box.low.z - margin)};
            settings.high = {std::min(settings.high.x, box.high.x + margin),
                             std::min(settings.high.y, box.high.y + margin),
                             std::min(settings.high.z, box.high.z + margin)};
        }
        // `bounds` narrows which cells belong to the clip, and for the whole building that is the
        // right shape to cut. For one fragment on its own it is the building's outline, and it
        // would cut the fragment to something it is not.
        settings.has_bounds = false;
    }

    const f64 span[3] = {settings.high.x - settings.low.x, settings.high.y - settings.low.y,
                         settings.high.z - settings.low.z};
    if (span[0] <= 0.0 || span[1] <= 0.0 || span[2] <= 0.0) {
        std::printf("  - empty bounds\n");
        return false;
    }

    // Halve until it fits. Halving rather than stepping to any integer keeps the sampled lattice
    // a subset of the authored one, so a coarse bake lands its voxels where the fine one would
    // have and a wall does not move by half a voxel between two resolutions.
    i32 metre = std::min(options.max_metre, settings.voxels_per_metre);
    while (metre > 1) {
        const f64 cells = span[0] * span[1] * span[2] * static_cast<f64>(metre) *
                          static_cast<f64>(metre) * static_cast<f64>(metre);
        if (cells <= static_cast<f64>(options.budget)) break;
        metre /= 2;
    }
    settings.voxels_per_metre = metre;
    settings.count_rule_cost = false;

    const u64 began = ws::now_ns();
    ws::forge::SampleResult built =
        ws::forge::sample(script.field, root, script.paint, settings, &jobs);
    ws::forge::despeckle(built.clip);
    const f64 seconds = static_cast<f64>(ws::now_ns() - began) / 1e9;

    const ws::Clip& clip = built.clip;
    if (clip.size[0] <= 0 || clip.size[1] <= 0 || clip.size[2] <= 0) {
        std::printf("  - sampled to nothing\n");
        return false;
    }
    if (clip.size[0] > 65535 || clip.size[1] > 65535 || clip.size[2] > 65535) {
        std::printf("  - %d x %d x %d is past what a 16-bit quad can address\n", clip.size[0],
                    clip.size[1], clip.size[2]);
        return false;
    }

    ClipMesher mesher(clip, types);
    mesher.build();

    // The two occupancy grids. Collision is capped at eight cells to the metre, which is 12.5 cm
    // — fine enough for the 18 cm risers the building is full of, and small enough that the whole
    // facility's collision is about a megabyte.
    BitGrid collision;
    collision.voxels_per_metre = std::min(metre, 8);
    BitGrid coarse;
    coarse.voxels_per_metre = std::min(metre, 2);
    u64 solid_voxels = 0;
    {
        const i32 collision_step = metre / collision.voxels_per_metre;
        const i32 coarse_step = metre / coarse.voxels_per_metre;
        for (i32 axis = 0; axis < 3; ++axis) {
            collision.dims[axis] = (clip.size[axis] + collision_step - 1) / collision_step;
            coarse.dims[axis] = (clip.size[axis] + coarse_step - 1) / coarse_step;
        }
        const usize collision_cells = static_cast<usize>(collision.dims[0]) *
                                      static_cast<usize>(collision.dims[1]) *
                                      static_cast<usize>(collision.dims[2]);
        const usize coarse_cells = static_cast<usize>(coarse.dims[0]) *
                                   static_cast<usize>(coarse.dims[1]) *
                                   static_cast<usize>(coarse.dims[2]);
        collision.bits.assign((collision_cells + 7) / 8, 0);
        coarse.bits.assign((coarse_cells + 7) / 8, 0);
        std::vector<u32> coarse_fill(coarse_cells, 0);

        for (i32 z = 0; z < clip.size[2]; ++z) {
            for (i32 y = 0; y < clip.size[1]; ++y) {
                for (i32 x = 0; x < clip.size[0]; ++x) {
                    if (!mesher.solid(x, y, z)) continue;
                    ++solid_voxels;
                    collision.set(x / collision_step, y / collision_step, z / collision_step);
                    ++coarse_fill[coarse.index(x / coarse_step, y / coarse_step, z / coarse_step)];
                }
            }
        }
        // The light grid's copy of the clip is solid where a THIRD of it is solid. A balustrade or
        // a window mullion should not close a half-metre cell and put the room behind it out.
        const u32 needed = static_cast<u32>(
            std::max(1, coarse_step * coarse_step * coarse_step / 3));
        for (i32 z = 0; z < coarse.dims[2]; ++z) {
            for (i32 y = 0; y < coarse.dims[1]; ++y) {
                for (i32 x = 0; x < coarse.dims[0]; ++x) {
                    if (coarse_fill[coarse.index(x, y, z)] >= needed) coarse.set(x, y, z);
                }
            }
        }
    }

    const f64 origin[3] = {settings.low.x, settings.low.y, settings.low.z};
    const f64 size_metres[3] = {static_cast<f64>(clip.size[0]) / static_cast<f64>(metre),
                                static_cast<f64>(clip.size[1]) / static_cast<f64>(metre),
                                static_cast<f64>(clip.size[2]) / static_cast<f64>(metre)};
    const LightGrid light = bake_light(coarse, origin, size_metres, jobs);

    // Where the matter actually is, which is what the viewer frames on. The sampled box is nearly
    // always bigger, and framing on it puts a building in the corner of the screen.
    const ws::forge::Measurement measured = ws::forge::measure(clip, metre);
    f64 matter_low[3] = {origin[0], origin[1], origin[2]};
    f64 matter_high[3] = {origin[0] + size_metres[0], origin[1] + size_metres[1],
                          origin[2] + size_metres[2]};
    if (measured.extent.any) {
        for (i32 axis = 0; axis < 3; ++axis) {
            matter_low[axis] =
                origin[axis] + static_cast<f64>(measured.extent.low[axis]) / static_cast<f64>(metre);
            matter_high[axis] = origin[axis] + static_cast<f64>(measured.extent.high[axis] + 1) /
                                                   static_cast<f64>(metre);
        }
    }

    // ---- the file ----------------------------------------------------------------------------
    std::vector<u8> out(kHeaderBytes, 0);
    out[0] = 'W';
    out[1] = 'S';
    out[2] = 'C';
    out[3] = 'V';
    put_u32(out, 4, 1);
    for (i32 axis = 0; axis < 3; ++axis) put_i32(out, 8 + static_cast<usize>(axis) * 4, clip.size[axis]);
    put_i32(out, 20, metre);
    for (i32 axis = 0; axis < 3; ++axis) put_f32(out, 24 + static_cast<usize>(axis) * 4, origin[axis]);
    {
        const f64 length = std::sqrt(kSunDir[0] * kSunDir[0] + kSunDir[1] * kSunDir[1] +
                                     kSunDir[2] * kSunDir[2]);
        for (i32 axis = 0; axis < 3; ++axis) {
            put_f32(out, 36 + static_cast<usize>(axis) * 4, kSunDir[axis] / length);
        }
    }
    put_u32(out, 48, static_cast<u32>(mesher.palette().size()));

    u32 opaque_total = 0;
    u32 transparent_total = 0;
    for (i32 face = 0; face < 6; ++face) {
        opaque_total += static_cast<u32>(mesher.opaque()[static_cast<usize>(face)].size());
        transparent_total += static_cast<u32>(mesher.transparent()[static_cast<usize>(face)].size());
    }
    put_u32(out, 52, opaque_total);
    put_u32(out, 56, transparent_total);

    // Seven offsets rather than six: the last one is the end, so a range is always
    // `start[i] .. start[i + 1]` with no special case at the top.
    {
        u32 running = 0;
        for (i32 face = 0; face < 6; ++face) {
            put_u32(out, 60 + static_cast<usize>(face) * 4, running);
            running += static_cast<u32>(mesher.opaque()[static_cast<usize>(face)].size());
        }
        put_u32(out, 60 + 24, running);
        running = 0;
        for (i32 face = 0; face < 6; ++face) {
            put_u32(out, 88 + static_cast<usize>(face) * 4, running);
            running += static_cast<u32>(mesher.transparent()[static_cast<usize>(face)].size());
        }
        put_u32(out, 88 + 24, running);
    }

    for (i32 axis = 0; axis < 3; ++axis) {
        put_i32(out, 116 + static_cast<usize>(axis) * 4, collision.dims[axis]);
    }
    put_i32(out, 128, collision.voxels_per_metre);
    for (i32 axis = 0; axis < 3; ++axis) {
        put_i32(out, 132 + static_cast<usize>(axis) * 4, light.dims[axis]);
    }
    put_f32(out, 144, light.cell);
    for (i32 axis = 0; axis < 3; ++axis) {
        put_f32(out, 148 + static_cast<usize>(axis) * 4, matter_low[axis]);
        put_f32(out, 160 + static_cast<usize>(axis) * 4, matter_high[axis]);
    }
    put_i32(out, 172, baked.authored_metre);
    put_u32(out, 176, static_cast<u32>(std::min<u64>(solid_voxels, 0xFFFFFFFFull)));

    for (const ws::VisualRecord& record : mesher.palette()) {
        const u8* bytes = reinterpret_cast<const u8*>(&record);
        out.insert(out.end(), bytes, bytes + sizeof(ws::VisualRecord));
    }
    for (i32 face = 0; face < 6; ++face) append_quads(out, mesher.opaque()[static_cast<usize>(face)]);
    for (i32 face = 0; face < 6; ++face) {
        append_quads(out, mesher.transparent()[static_cast<usize>(face)]);
    }
    out.insert(out.end(), light.texels.begin(), light.texels.end());
    out.insert(out.end(), collision.bits.begin(), collision.bits.end());

    baked.id = identifier(relative);
    baked.source = relative.generic_string();
    {
        const fs::path parent = relative.parent_path();
        baked.group = parent.empty() ? std::string("clips") : parent.generic_string();
    }
    for (i32 axis = 0; axis < 3; ++axis) {
        baked.dims[axis] = clip.size[axis];
        baked.origin[axis] = origin[axis];
        baked.matter_low[axis] = matter_low[axis];
        baked.matter_high[axis] = matter_high[axis];
    }
    baked.metre = metre;
    baked.solid = solid_voxels;
    baked.quads = opaque_total + transparent_total;
    baked.hash = fnv1a(out.data(), out.size());
    baked.bytes = out.size();
    baked.materials = script.material_names;

    const fs::path target = options.out / (baked.id + ".wsc");
    std::ofstream stream(target, std::ios::binary | std::ios::trunc);
    if (!stream) {
        std::printf("  ! cannot write %s\n", target.string().c_str());
        return false;
    }
    stream.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    stream.close();

    std::printf("  %d/m  %d x %d x %d  %u quads  %zu materials  %.1f MB  %.1f s\n", metre,
                clip.size[0], clip.size[1], clip.size[2], baked.quads, mesher.palette().size(),
                static_cast<f64>(out.size()) / (1024.0 * 1024.0), seconds);
    if (options.verbose) {
        std::printf("      box  %.2f %.2f %.2f  ..  %.2f %.2f %.2f      matter  %.2f %.2f %.2f  "
                    ".. %.2f %.2f %.2f\n",
                    settings.low.x, settings.low.y, settings.low.z, settings.high.x,
                    settings.high.y, settings.high.z, matter_low[0], matter_low[1], matter_low[2],
                    matter_high[0], matter_high[1], matter_high[2]);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::printf("%s wants a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--clips") {
            options.clips = next("--clips");
        } else if (arg == "--out") {
            options.out = next("--out");
        } else if (arg == "--budget") {
            options.budget = std::stoll(next("--budget"));
        } else if (arg == "--max-metre") {
            options.max_metre = std::stoi(next("--max-metre"));
        } else if (arg == "--only") {
            options.only = next("--only");
        } else if (arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            std::printf(
                "ws_bake_web - bake every clip for the viewer at web/\n\n"
                "  --clips DIR      where the clips are (default clips)\n"
                "  --out DIR        where the .wsc files go (default web/data)\n"
                "  --budget N       cells a sampled box may hold before the resolution halves\n"
                "  --max-metre N    never sample finer than this\n"
                "  --only ID        bake one clip, by its id (facility, facility-dome, ...)\n");
            return 0;
        } else {
            std::printf("unknown argument %s\n", arg.c_str());
            return 2;
        }
    }

    if (!fs::exists(options.clips)) {
        std::printf("no clips at %s\n", options.clips.string().c_str());
        return 1;
    }
    std::error_code code;
    fs::create_directories(options.out, code);

    // Every .clip under clips/, in a stable order, minus the ones whose name begins with an
    // underscore. Those are the contract, the order and the template: they are included BY clips
    // and are not clips, and the convention is already the one the facility's own files use.
    std::vector<fs::path> files;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(options.clips)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".clip") continue;
        if (!entry.path().filename().string().empty() && entry.path().filename().string()[0] == '_') {
            continue;
        }
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());

    ws::JobSystem jobs;
    std::vector<Baked> done;
    i32 failed = 0;
    const u64 began = ws::now_ns();

    // The manifest, parsed once and kept.
    //
    // Every fragment of the facility is baked out of THIS parse rather than out of its own file,
    // and there are two reasons, of which only the second was foreseen. The first: a fragment does
    // not include `_order.clip`, so parsed alone it does not know what a column or a dentil is and
    // fails on its first use of one. The second: a part baked out of the assembled building
    // carries the paint the building gives it, including the weathering coats `surface.clip` adds
    // over everything, so what the viewer shows of a fragment is what that fragment is IN the
    // building rather than what it would be on a table by itself.
    //
    // A fragment the manifest has not been told to include yet still gets its own parse below, so
    // a brand new file is visible before the three lines that add it are written.
    Program manifest;
    const fs::path manifest_path = options.clips / "facility.clip";
    if (fs::exists(manifest_path)) {
        manifest.script = ws::forge::load_clip_script(manifest_path.string(), manifest.types,
                                                      manifest.tags);
        manifest.parsed = manifest.script.ok();
        if (!manifest.parsed) {
            std::printf("facility.clip does not build; its fragments fall back to their own files\n");
            for (const ws::forge::ScriptError& error : manifest.script.errors) {
                std::printf("  ! line %u: %s\n", error.line, error.message.c_str());
            }
        }
    }

    for (const fs::path& file : files) {
        const fs::path relative = fs::relative(file, options.clips);
        const std::string id = identifier(relative);
        if (!options.only.empty() && options.only != id) continue;
        std::printf("%s\n", relative.generic_string().c_str());

        const std::string stem = file.stem().string();
        Baked baked;
        bool built = false;

        u32 root = 0;
        if (manifest.parsed && relative == fs::path("facility.clip")) {
            built = bake_root(options, manifest, manifest.script.solid, false, relative, jobs, baked);
        } else if (manifest.parsed && manifest.script.part("part_" + stem, root)) {
            built = bake_root(options, manifest, root, true, relative, jobs, baked);
        } else {
            Program own;
            own.script = ws::forge::load_clip_script(file.string(), own.types, own.tags);
            bool fatal = false;
            for (const ws::forge::ScriptError& error : own.script.errors) {
                // A fragment says no `solid` by design; the manifest says it for them. That is not
                // an error here, and reporting it as one would hide the errors that are.
                if (error.line == 0 && error.message.find("which shape is the solid") != std::string::npos) {
                    continue;
                }
                std::printf("  ! line %u: %s\n", error.line, error.message.c_str());
                fatal = true;
            }
            if (!fatal) {
                u32 own_root = own.script.solid;
                bool is_part = false;
                if (!own.script.has_solid) {
                    if (own.script.part("part_" + stem, own_root)) {
                        is_part = true;
                    } else {
                        std::printf("  - no `solid` and no `part_%s`; nothing to build\n",
                                    stem.c_str());
                    }
                }
                if (own.script.has_solid || is_part) {
                    built = bake_root(options, own, own_root, is_part, relative, jobs, baked);
                }
            }
        }

        if (built) {
            done.push_back(baked);
        } else {
            ++failed;
        }
    }

    // `--only` deliberately leaves the index alone. It is for working on one clip, and an index
    // rewritten from a run that built one of forty is an index that has just deleted the site.
    if (!options.only.empty()) {
        std::printf("\n%zu clip baked; index.json left as it was (--only)\n", done.size());
        return done.empty() ? 1 : 0;
    }

    // The index. The viewer reads only this to know what exists, and re-reads it every few seconds
    // to find out whether anything has changed — which is what makes the site follow the work
    // going on in the clips rather than showing whatever it was built with.
    u64 combined = 0xcbf29ce484222325ull;
    for (const Baked& baked : done) {
        const std::string key = baked.id + hex64(baked.hash);
        combined = fnv1a(reinterpret_cast<const u8*>(key.data()), key.size(), combined);
    }

    std::string json = "{\n";
    json += "  \"version\": 1,\n";
    {
        const std::time_t now = std::time(nullptr);
        char stamp[32] = {0};
        std::tm utc{};
#if defined(_WIN32)
        gmtime_s(&utc, &now);
#else
        gmtime_r(&now, &utc);
#endif
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
        json += "  \"built\": \"" + std::string(stamp) + "\",\n";
    }
    json += "  \"hash\": \"" + hex64(combined) + "\",\n";
    json += "  \"clips\": [\n";
    for (usize i = 0; i < done.size(); ++i) {
        const Baked& baked = done[i];
        char row[1024];
        std::snprintf(row, sizeof(row),
                      "    {\"id\": \"%s\", \"source\": \"%s\", \"group\": \"%s\", "
                      "\"hash\": \"%s\", \"bytes\": %zu, \"quads\": %u, \"solid\": %llu, "
                      "\"metre\": %d, \"authored\": %d, \"dims\": [%d, %d, %d], "
                      "\"low\": [%.4f, %.4f, %.4f], \"high\": [%.4f, %.4f, %.4f]}%s\n",
                      json_escape(baked.id).c_str(), json_escape(baked.source).c_str(),
                      json_escape(baked.group).c_str(), hex64(baked.hash).c_str(), baked.bytes,
                      baked.quads, static_cast<unsigned long long>(baked.solid), baked.metre,
                      baked.authored_metre, baked.dims[0], baked.dims[1], baked.dims[2],
                      baked.matter_low[0], baked.matter_low[1], baked.matter_low[2],
                      baked.matter_high[0], baked.matter_high[1], baked.matter_high[2],
                      (i + 1 < done.size()) ? "," : "");
        json += row;
    }
    json += "  ]\n}\n";

    const fs::path index = options.out / "index.json";
    std::ofstream stream(index, std::ios::trunc);
    if (!stream) {
        std::printf("cannot write %s\n", index.string().c_str());
        return 1;
    }
    stream << json;
    stream.close();

    const f64 seconds = static_cast<f64>(ws::now_ns() - began) / 1e9;
    std::printf("\n%zu clips baked, %d could not be, in %.1f s -> %s\n", done.size(), failed,
                seconds, options.out.string().c_str());

    // A clip that cannot be built is reported and does not fail the bake. The site exists to show
    // work in progress, and a fragment somebody is halfway through editing is exactly the case it
    // has to survive — failing here would take the other twenty clips off the site with it.
    return done.empty() ? 1 : 0;
}
